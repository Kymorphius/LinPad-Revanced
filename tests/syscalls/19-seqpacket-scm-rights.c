// SOCK_SEQPACKET made with socket()/connect(): a file descriptor sent with a record
// arrives with that record only, large records survive intact, poll() sees a waiting
// record, and a non-blocking read of an empty socket is EAGAIN. Complements
// 17-seqpacket-boundaries.c, which covers the record boundaries themselves.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int send_with_fd(int sock, const char *data, size_t len, int fd) {
    struct iovec iov = {(void *) data, len};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1};
    if (fd >= 0) {
        mh.msg_control = control;
        mh.msg_controllen = sizeof(control);
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &fd, sizeof(int));
    }
    return (int) sendmsg(sock, &mh, 0);
}

static ssize_t recv_with_fd(int sock, char *buf, size_t len, int *fd_out) {
    struct iovec iov = {buf, len};
    char control[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
    ssize_t r = recvmsg(sock, &mh, 0);
    *fd_out = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c != NULL; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
            memcpy(fd_out, CMSG_DATA(c), sizeof(int));
    return r;
}

int main(void) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, "/tmp/linpad-seq-scm");
    unlink(addr.sun_path);
    int l = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    bind(l, (void *) &addr, sizeof(addr));
    listen(l, 2);
    int c = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    CHECK(connect(c, (void *) &addr, sizeof(addr)) == 0, "connect errno=%d", errno);
    int s = accept(l, NULL, NULL);
    CHECK(s >= 0, "accept errno=%d", errno);

    // non-blocking read of an empty socket
    int fl = fcntl(s, F_GETFL);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    char buf[200000];
    errno = 0;
    ssize_t r = recv(s, buf, sizeof(buf), 0);
    CHECK(r == -1 && errno == EAGAIN, "empty non-blocking recv r=%zd errno=%d", r, errno);
    fcntl(s, F_SETFL, fl);

    // a pipe's read end travels with one record; the next record has none
    int p[2];
    pipe(p);
    write(p[1], "through the pipe", 16);
    CHECK(send_with_fd(c, "with fd", 7, p[0]) == 7, "send with fd");
    CHECK(send_with_fd(c, "no fd", 5, -1) == 5, "send without fd");
    int got = -1;
    r = recv_with_fd(s, buf, sizeof(buf), &got);
    CHECK(r == 7 && memcmp(buf, "with fd", 7) == 0 && got >= 0, "first record r=%zd fd=%d", r, got);
    if (got >= 0) {
        char pb[32] = {0};
        ssize_t pr = read(got, pb, sizeof(pb));
        CHECK(pr == 16 && memcmp(pb, "through the pipe", 16) == 0, "received fd reads %zd bytes", pr);
        close(got);
    }
    r = recv_with_fd(s, buf, sizeof(buf), &got);
    CHECK(r == 5 && got == -1, "second record r=%zd fd=%d (want none)", r, got);

    // a large record arrives whole, and poll() reports it
    static char big[150000], back[150000];
    for (size_t i = 0; i < sizeof(big); i++) big[i] = (char) (i * 31 + 7);
    CHECK(send(c, big, sizeof(big), 0) == (ssize_t) sizeof(big), "send 150000");
    struct pollfd pfd = {.fd = s, .events = POLLIN};
    CHECK(poll(&pfd, 1, 2000) == 1 && (pfd.revents & POLLIN), "poll sees the record");
    r = recv(s, back, sizeof(back), 0);
    CHECK(r == (ssize_t) sizeof(big) && memcmp(big, back, sizeof(big)) == 0, "large record r=%zd", r);

    // a peeked record still counts as readable
    send(c, "peeked", 6, 0);
    r = recv(s, buf, sizeof(buf), MSG_PEEK);
    CHECK(r == 6, "peek r=%zd", r);
    pfd.revents = 0;
    CHECK(poll(&pfd, 1, 500) == 1 && (pfd.revents & POLLIN), "poll after peek");
    r = recv(s, buf, sizeof(buf), 0);
    CHECK(r == 6, "read after peek r=%zd", r);

    // MSG_TRUNC returns the record's real length, but only the buffer's size is written
    send(c, "0123456789", 10, 0);
    char small[8];
    memset(small, 'z', sizeof(small));
    r = recv(s, small, 3, MSG_TRUNC);
    CHECK(r == 10 && memcmp(small, "012", 3) == 0 && small[3] == 'z' && small[7] == 'z',
          "recv MSG_TRUNC writes only 3 bytes: r=%zd, next bytes '%c' '%c'", r, small[3], small[7]);

    // a record larger than the socket buffer is refused, as on Linux
    static char huge[2 << 20];
    r = send(c, huge, sizeof(huge), 0);
    CHECK(r == -1 && errno == EMSGSIZE, "oversized record: r=%zd errno=%d", r, errno);

    close(l);
    unlink(addr.sun_path);
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures != 0;
}
