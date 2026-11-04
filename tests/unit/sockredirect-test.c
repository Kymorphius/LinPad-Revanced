// Native tests for transparent TCP redirection (fs/sockredirect.c): the
// lease and policy decisions, the /proc control, and original-destination
// lookups through real host loopback sockets. No guest or device needed.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include "fs/proc.h"
#include "fs/sockredirect.h"
#include "kernel/errno.h"

static char shown[4096];
void proc_printf(struct proc_data *buf, const char *format, ...) {
    (void) buf;
    size_t used = strlen(shown);
    va_list args;
    va_start(args, format);
    vsnprintf(shown + used, sizeof(shown) - used, format, args);
    va_end(args);
}

#define CHECK(test) do { \
    if (!(test)) { \
        fprintf(stderr, "sockredirect-test:%d: %s\nshown: %s\n", __LINE__, #test, shown); \
        exit(1); \
    } \
} while (0)

static int update(const char *text, unsigned euid) {
    return sockredirect_update(text, strlen(text), euid);
}

static const char *show(void) {
    shown[0] = '\0';
    sockredirect_show(NULL);
    return shown;
}

static struct sockaddr_storage v4(const char *ip, unsigned port) {
    struct sockaddr_storage s = {0};
    struct sockaddr_in *a = (struct sockaddr_in *) &s;
#if defined(__APPLE__)
    a->sin_len = sizeof(*a);
#endif
    a->sin_family = AF_INET;
    a->sin_port = htons(port);
    CHECK(inet_pton(AF_INET, ip, &a->sin_addr) == 1);
    return s;
}

static struct sockaddr_storage v6(const char *ip, unsigned port) {
    struct sockaddr_storage s = {0};
    struct sockaddr_in6 *a = (struct sockaddr_in6 *) &s;
#if defined(__APPLE__)
    a->sin6_len = sizeof(*a);
#endif
    a->sin6_family = AF_INET6;
    a->sin6_port = htons(port);
    CHECK(inet_pton(AF_INET6, ip, &a->sin6_addr) == 1);
    return s;
}

// Whether dst is redirected, and to which "address:port" text.
static bool target_text(struct sockaddr_storage dst, unsigned euid, uint16_t fixed, char *out) {
    struct sockaddr_storage t;
    socklen_t len;
    if (!sockredirect_target((struct sockaddr *) &dst, euid, fixed, &t, &len))
        return false;
    char ip[INET6_ADDRSTRLEN];
    if (t.ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *) &t;
        CHECK(len == sizeof(*a));
        inet_ntop(AF_INET, &a->sin_addr, ip, sizeof(ip));
        sprintf(out, "%s:%u", ip, ntohs(a->sin_port));
    } else {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *) &t;
        CHECK(len == sizeof(*a));
        inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
        sprintf(out, "[%s]:%u", ip, ntohs(a->sin6_port));
    }
    return true;
}

static void test_policy(void) {
    char t[80];
    CHECK(strstr(show(), "active 0\n") && strstr(shown, "exempt_uid none\n"));
    CHECK(strstr(shown, "bypass 0.0.0.0/8 10.0.0.0/8 100.64.0.0/10 127.0.0.0/8"));
    CHECK(!target_text(v4("93.184.216.34", 443), 0, 0, t)); // off by default
    CHECK(update("tcp_port 7890\nlease 30\n", 1000) == _EPERM);
    CHECK(strstr(show(), "tcp_port 0\n"));

    CHECK(update("tcp_port 7890\nexempt_uid 901\nlease 30\n", 0) == 0);
    CHECK(strstr(show(), "active 1\n") && strstr(shown, "exempt_uid 901\n"));
    CHECK(strstr(shown, "lease_remaining 30\n"));
    CHECK(target_text(v4("93.184.216.34", 443), 0, 0, t) && strcmp(t, "127.0.0.1:7890") == 0);
    CHECK(target_text(v6("2001:db8::1", 443), 1000, 0, t) && strcmp(t, "[::1]:7890") == 0);
    // IPv4 through an IPv6 socket stays IPv4 on the proxy side.
    CHECK(target_text(v6("::ffff:93.184.216.34", 80), 0, 0, t) && strcmp(t, "[::ffff:127.0.0.1]:7890") == 0);
    CHECK(!target_text(v4("93.184.216.34", 443), 901, 0, t)); // the proxy's own uid
    CHECK(!target_text(v4("127.0.0.1", 7890), 0, 0, t));
    CHECK(!target_text(v4("0.0.0.0", 80), 0, 0, t));
    CHECK(!target_text(v6("::1", 7890), 0, 0, t));
    CHECK(!target_text(v6("::ffff:127.0.0.1", 22), 0, 0, t));
    CHECK(!target_text(v4("192.168.0.10", 22), 0, 0, t));
    CHECK(!target_text(v4("100.64.0.7", 22), 0, 0, t)); // tailnet CGNAT
    CHECK(!target_text(v6("fe80::1", 22), 0, 0, t));
    CHECK(strstr(show(), "redirected 3\n"));

    // A replaced bypass list; loopback still never redirects.
    CHECK(update("bypass 93.184.216.0/24\n", 0) == 0);
    CHECK(!target_text(v4("93.184.216.34", 443), 0, 0, t));
    CHECK(target_text(v4("192.168.0.10", 22), 0, 0, t));
    CHECK(update("bypass\n", 0) == 0);
    CHECK(strstr(show(), "bypass\n"));
    CHECK(!target_text(v4("127.0.0.1", 80), 0, 0, t));
    CHECK(update("bypass 10.0.0.0/8 192.168.0.0/16 fc00::/7\n", 0) == 0);

    // Invalid input changes nothing, even with valid lines before it.
    CHECK(update("tcp_port 1234\nbypass 300.1.1.1/8\n", 0) == _EINVAL);
    CHECK(update("tcp_port 70000\n", 0) == _EINVAL);
    CHECK(update("lease 3601\n", 0) == _EINVAL);
    CHECK(update("bypass 10.0.0.0/33\n", 0) == _EINVAL);
    CHECK(update("colour blue\n", 0) == _EINVAL);
    CHECK(strstr(show(), "tcp_port 7890\n") && strstr(shown, "bypass 10.0.0.0/8 192.168.0.0/16 fc00::/7\n"));

    // An expired lease means direct connections again, but a retry of an
    // already redirected connect keeps its port.
    CHECK(update("lease 0\n", 0) == 0);
    CHECK(strstr(show(), "active 0\n") && strstr(shown, "lease_remaining 0\n"));
    CHECK(!target_text(v4("93.184.216.34", 443), 0, 0, t));
    CHECK(target_text(v4("93.184.216.34", 443), 0, 7890, t) && strcmp(t, "127.0.0.1:7890") == 0);
    CHECK(update("exempt_uid none\ntcp_port 7890\nlease 60\n", 0) == 0);
    CHECK(strstr(show(), "exempt_uid none\n") && target_text(v4("93.184.216.34", 443), 901, 0, t));
    printf("PASS lease, uid exemption, bypass list, families and atomic control writes\n");
}

// A real loopback connection standing in for guest client and proxy.
static void connect_pair(int family, int *client, int *accepted, uint16_t *listen_port) {
    int listener = socket(family, SOCK_STREAM, 0);
    CHECK(listener >= 0);
    struct sockaddr_storage addr = family == AF_INET ? v4("127.0.0.1", 0) : v6("::1", 0);
    socklen_t len = family == AF_INET ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6);
    CHECK(bind(listener, (struct sockaddr *) &addr, len) == 0);
    CHECK(listen(listener, 4) == 0);
    CHECK(getsockname(listener, (struct sockaddr *) &addr, &len) == 0);
    *listen_port = ntohs(family == AF_INET ? ((struct sockaddr_in *) &addr)->sin_port
                                           : ((struct sockaddr_in6 *) &addr)->sin6_port);
    *client = socket(family, SOCK_STREAM, 0);
    CHECK(*client >= 0 && connect(*client, (struct sockaddr *) &addr, len) == 0);
    *accepted = accept(listener, NULL, NULL);
    CHECK(*accepted >= 0);
    close(listener);
}

static void test_original_destination(void) {
    int client, accepted;
    uint16_t lport;
    connect_pair(AF_INET, &client, &accepted, &lport);
    struct sockaddr_storage dst = v4("93.184.216.34", 443), got;
    socklen_t len;
    CHECK(sockredirect_original_dst(accepted, false, &got, &len) == _ENOENT);
    bool is_v6 = true;
    uint16_t port = sockredirect_remember(client, (struct sockaddr *) &dst, &is_v6);
    CHECK(port != 0 && !is_v6);
    CHECK(sockredirect_original_dst(accepted, false, &got, &len) == 0);
    CHECK(len == sizeof(struct sockaddr_in) && memcmp(&got, &dst, len) == 0);
    CHECK(sockredirect_original_dst(accepted, true, &got, &len) == _ENOENT); // SOL_IPV6 for IPv4
    CHECK(sockredirect_lookup(false, port, &got, &len) == 0);
    CHECK(strstr(show(), "open 1\n"));
    sockredirect_forget(false, port);
    CHECK(sockredirect_original_dst(accepted, false, &got, &len) == _ENOENT);
    CHECK(strstr(show(), "open 0\n"));
    close(client);
    close(accepted);

    connect_pair(AF_INET6, &client, &accepted, &lport);
    dst = v6("2001:db8::7", 8443);
    port = sockredirect_remember(client, (struct sockaddr *) &dst, &is_v6);
    CHECK(port != 0 && is_v6);
    CHECK(sockredirect_original_dst(accepted, true, &got, &len) == 0);
    CHECK(len == sizeof(struct sockaddr_in6) && memcmp(&got, &dst, len) == 0);
    CHECK(sockredirect_original_dst(accepted, false, &got, &len) == _ENOENT);
    // An IPv4-mapped destination is stored, and answered, as IPv4.
    struct sockaddr_storage mapped = v6("::ffff:93.184.216.34", 80), plain = v4("93.184.216.34", 80);
    sockredirect_remember(client, (struct sockaddr *) &mapped, &is_v6);
    CHECK(sockredirect_lookup(true, port, &got, &len) == 0);
    CHECK(len == sizeof(struct sockaddr_in) && memcmp(&got, &plain, len) == 0);
    sockredirect_forget(true, port);
    close(client);
    close(accepted);
    CHECK(strstr(show(), "original_dst_lookups 6\n") && strstr(shown, "original_dst_misses 4\n"));
    printf("PASS original destination by the proxy's peer address, per family, until forgotten\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_policy();
    test_original_destination();
    return 0;
}
