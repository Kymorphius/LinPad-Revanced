// SO_PEERCRED names the connecting process on an accepted socket (and the
// other end of a socketpair); SO_PASSCRED + SCM_CREDENTIALS name the sender of
// each record, not just the last one to send.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int peer(int s, struct ucred *cr) {
    socklen_t len = sizeof(*cr);
    return getsockopt(s, SOL_SOCKET, SO_PEERCRED, cr, &len);
}

static pid_t recv_cred(int s) {
    char b[8];
    struct iovec iov = {b, sizeof(b)};
    char cbuf[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof(cbuf)};
    if (recvmsg(s, &mh, 0) < 0)
        return -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS) {
            struct ucred cr;
            memcpy(&cr, CMSG_DATA(c), sizeof(cr));
            return cr.pid;
        }
    return -2;
}

int main(void) {
    int types[] = {SOCK_STREAM, SOCK_SEQPACKET};
    const char *names[] = {"stream", "seqpacket"};
    for (int t = 0; t < 2; t++) {
        struct sockaddr_un addr = {.sun_family = AF_UNIX};
        strcpy(addr.sun_path, "/tmp/linpad-peercred");
        unlink(addr.sun_path);
        int l = socket(AF_UNIX, types[t], 0);
        bind(l, (void *) &addr, sizeof(addr));
        listen(l, 4);
        int go[2];
        pipe(go);
        pid_t child = fork();
        if (child == 0) {
            int c = socket(AF_UNIX, types[t], 0);
            connect(c, (void *) &addr, sizeof(addr));
            char b;
            read(go[0], &b, 1); // keep the connection open until the parent checked
            _exit(0);
        }
        int s = accept(l, NULL, NULL);
        struct ucred cr = {0};
        int r = peer(s, &cr);
        CHECK(r == 0 && cr.pid == child && cr.uid == getuid() && cr.gid == getgid(),
                "%s: SO_PEERCRED of accepted socket pid=%d (child %d) uid=%d gid=%d", names[t], cr.pid, child, cr.uid, cr.gid);
        // Linux truncates/accepts other lengths instead of failing
        char big[sizeof(struct ucred) + 8];
        socklen_t blen = sizeof(big);
        r = getsockopt(s, SOL_SOCKET, SO_PEERCRED, big, &blen);
        CHECK(r == 0 && blen == sizeof(struct ucred), "%s: SO_PEERCRED with a larger buffer r=%d len=%u", names[t], r, blen);
        write(go[1], "x", 1);
        waitpid(child, NULL, 0);
        close(s); close(l); close(go[0]); close(go[1]);
        unlink(addr.sun_path);
    }

    int stypes[] = {SOCK_SEQPACKET, SOCK_DGRAM};
    const char *snames[] = {"seqpacket", "dgram"};
    for (int t = 0; t < 2; t++) {
        int sv[2];
        socketpair(AF_UNIX, stypes[t], 0, sv);
        struct ucred cr = {0};
        peer(sv[0], &cr);
        CHECK(cr.pid == getpid(), "%s socketpair: SO_PEERCRED pid=%d", snames[t], cr.pid);
        int one = 1;
        setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
        pid_t child = fork();
        if (child == 0) {
            send(sv[1], "c", 1, 0);
            _exit(0);
        }
        waitpid(child, NULL, 0);
        send(sv[1], "p", 1, 0);
        pid_t first = recv_cred(sv[0]);
        pid_t second = recv_cred(sv[0]);
        CHECK(first == child, "%s: SCM_CREDENTIALS of record 1 = child %d (got %d)", snames[t], child, first);
        CHECK(second == getpid(), "%s: SCM_CREDENTIALS of record 2 = parent %d (got %d)", snames[t], getpid(), second);
        close(sv[0]); close(sv[1]);
    }
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures != 0;
}
