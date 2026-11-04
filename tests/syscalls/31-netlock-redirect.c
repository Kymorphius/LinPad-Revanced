// Checks: the transparent proxy and its netlock (fs/sockredirect.h), through
// /proc/ish/transparent_proxy as root. 192.0.2.1 (TEST-NET-1) stands for the outside.
// Expected: under the netlock, outside TCP without a lease and outside datagrams fail with
// EPERM, except for the exempt uid and loopback; with a lease TCP reaches the proxy, which
// reads the original destination with SO_ORIGINAL_DST; a lapsed lease blocks again.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#ifndef SO_ORIGINAL_DST
#define SO_ORIGINAL_DST 80
#endif
static int failures;
#define CHECK(cond, ...) do { int ok_ = (cond); printf("%s: ", ok_ ? "PASS" : "FAIL"); failures += !ok_; \
    printf(__VA_ARGS__); printf("\n"); } while (0)

static int control(const char *text) {
    int fd = open("/proc/ish/transparent_proxy", O_WRONLY);
    if (fd < 0)
        return -errno;
    int r = write(fd, text, strlen(text)) < 0 ? -errno : 0;
    close(fd);
    return r;
}

static struct sockaddr_in addr(const char *ip, int port) {
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

// connect() on a fresh non-blocking socket; 0 on success or EINPROGRESS.
static int try_connect(int type, struct sockaddr_in to) {
    int fd = socket(AF_INET, type | SOCK_NONBLOCK, 0);
    int r = connect(fd, (struct sockaddr *) &to, sizeof(to));
    int e = r == 0 || errno == EINPROGRESS ? 0 : errno;
    close(fd);
    return e;
}

static int try_sendto(struct sockaddr_in to) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    int r = sendto(fd, "x", 1, 0, (struct sockaddr *) &to, sizeof(to));
    int e = r == 1 ? 0 : errno;
    close(fd);
    return e;
}

int main(void) {
    struct sockaddr_in outside = addr("192.0.2.1", 80), loop = addr("127.0.0.1", 9);
    CHECK(control("netlock on\nexempt_uid 901\ntcp_port 0\nlease 0\nbypass 10.0.0.0/8\n") == 0, "netlock on");
    int e = try_connect(SOCK_STREAM, outside);
    CHECK(e == EPERM, "TCP to the outside without a lease: errno %d", e);
    e = try_sendto(outside);
    CHECK(e == EPERM, "UDP sendto the outside: errno %d", e);
    e = try_connect(SOCK_DGRAM, outside);
    CHECK(e == EPERM, "UDP connect to the outside: errno %d", e);
    e = try_sendto(loop);
    CHECK(e == 0, "UDP to loopback: errno %d", e);
    e = try_connect(SOCK_STREAM, addr("10.1.2.3", 22));
    CHECK(e != EPERM, "TCP to the bypass list is not blocked: errno %d", e);

    CHECK(seteuid(901) == 0, "seteuid 901");
    e = try_connect(SOCK_STREAM, outside);
    CHECK(e != EPERM, "exempt uid TCP is not blocked: errno %d", e);
    e = try_sendto(outside);
    CHECK(e != EPERM, "exempt uid UDP is not blocked: errno %d", e);
    seteuid(0);

    // the proxy
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in proxy = addr("127.0.0.1", 0);
    socklen_t len = sizeof(proxy);
    bind(listener, (struct sockaddr *) &proxy, sizeof(proxy));
    listen(listener, 4);
    getsockname(listener, (struct sockaddr *) &proxy, &len);
    char text[64];
    snprintf(text, sizeof(text), "tcp_port %u\nlease 30\n", ntohs(proxy.sin_port));
    CHECK(control(text) == 0, "lease 30 to port %u", ntohs(proxy.sin_port));
    int client = socket(AF_INET, SOCK_STREAM, 0);
    int r = connect(client, (struct sockaddr *) &outside, sizeof(outside));
    CHECK(r == 0, "TCP to the outside with a lease reaches the proxy: %d errno %d", r, r ? errno : 0);
    int accepted = accept(listener, NULL, NULL);
    struct sockaddr_in original = {0};
    len = sizeof(original);
    r = getsockopt(accepted, SOL_IP, SO_ORIGINAL_DST, &original, &len);
    char ip[INET_ADDRSTRLEN] = "";
    inet_ntop(AF_INET, &original.sin_addr, ip, sizeof(ip));
    CHECK(r == 0 && strcmp(ip, "192.0.2.1") == 0 && ntohs(original.sin_port) == 80,
        "SO_ORIGINAL_DST = %d: %s:%u", r, ip, ntohs(original.sin_port));
    struct sockaddr_in peer = {0};
    len = sizeof(peer);
    getpeername(client, (struct sockaddr *) &peer, &len);
    CHECK(peer.sin_addr.s_addr == outside.sin_addr.s_addr, "the client's peer is still the original");
    e = try_sendto(outside);
    CHECK(e == EPERM, "UDP to the outside stays blocked with a lease: errno %d", e);
    close(accepted);
    close(client);

    // a uid with a proxy of its own
    int listener2 = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in proxy2 = addr("127.0.0.1", 0);
    len = sizeof(proxy2);
    bind(listener2, (struct sockaddr *) &proxy2, sizeof(proxy2));
    listen(listener2, 4);
    getsockname(listener2, (struct sockaddr *) &proxy2, &len);
    snprintf(text, sizeof(text), "uid_port 902 %u\n", ntohs(proxy2.sin_port));
    CHECK(control(text) == 0, "uid 902 gets port %u", ntohs(proxy2.sin_port));
    seteuid(902);
    client = socket(AF_INET, SOCK_STREAM, 0);
    r = connect(client, (struct sockaddr *) &outside, sizeof(outside));
    seteuid(0);
    CHECK(r == 0, "uid 902 reaches its own proxy: %d errno %d", r, r ? errno : 0);
    accepted = accept(listener2, NULL, NULL);
    memset(&original, 0, sizeof(original));
    len = sizeof(original);
    r = getsockopt(accepted, SOL_IP, SO_ORIGINAL_DST, &original, &len);
    inet_ntop(AF_INET, &original.sin_addr, ip, sizeof(ip));
    CHECK(accepted >= 0 && r == 0 && strcmp(ip, "192.0.2.1") == 0, "its SO_ORIGINAL_DST = %d: %s", r, ip);
    close(accepted);
    close(client);
    close(listener2);
    CHECK(control("uid_port 902 0\n") == 0, "uid 902 back to the common port");

    CHECK(control("lease 0\n") == 0, "lease lapses");
    e = try_connect(SOCK_STREAM, outside);
    CHECK(e == EPERM, "TCP to the outside after the lease: errno %d", e);

    // a datagram socket connected before the netlock came on
    CHECK(control("netlock off\n") == 0, "netlock off");
    int udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in far = addr("192.0.2.1", 9);
    r = connect(udp, (struct sockaddr *) &far, sizeof(far));
    CHECK(r == 0, "UDP connect without the netlock: %d", r);
    control("netlock on\n");
    r = send(udp, "x", 1, 0);
    CHECK(r == -1 && errno == EPERM, "its send under the netlock: %d errno %d", r, errno);
    r = write(udp, "x", 1);
    CHECK(r == -1 && errno == EPERM, "its write under the netlock: %d errno %d", r, errno);
    close(udp);

    seteuid(1000);
    r = control("netlock off\n");
    seteuid(0);
    char state[512] = "";
    int fd = open("/proc/ish/transparent_proxy", O_RDONLY);
    read(fd, state, sizeof(state) - 1);
    close(fd);
    CHECK((r == -EACCES || r == -EPERM) && strstr(state, "netlock 1\n") != NULL,
        "only root changes it: %d, still locked", r);
    CHECK(control("netlock off\nlease 0\ntcp_port 0\n") == 0, "netlock off");
    return failures != 0;
}
