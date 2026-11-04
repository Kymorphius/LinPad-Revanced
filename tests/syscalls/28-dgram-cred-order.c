// Two threads sending on one datagram or seqpacket socket: each record keeps the credentials
// its own sender gave it, whichever order the sends interleave in.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#define PER_THREAD 2000
static int sv[2];
static void *sender(void *arg) {
    int which = *(int *) arg; // 0: pid 1111, 1: pid 2222
    struct ucred cred = {.pid = which ? 2222 : 1111, .uid = getuid(), .gid = getgid()};
    char payload = which ? 'B' : 'A';
    for (int i = 0; i < PER_THREAD; i++) {
        struct iovec iov = {&payload, 1};
        char control[CMSG_SPACE(sizeof(cred))];
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        struct cmsghdr *c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_CREDENTIALS;
        c->cmsg_len = CMSG_LEN(sizeof(cred));
        memcpy(CMSG_DATA(c), &cred, sizeof(cred));
        while (sendmsg(sv[1], &mh, 0) < 0)
            usleep(100);
    }
    return NULL;
}
static int run(int type, const char *name) {
    socketpair(AF_UNIX, type, 0, sv);
    int one = 1;
    setsockopt(sv[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
    int which[2] = {0, 1};
    pthread_t t[2];
    pthread_create(&t[0], NULL, sender, &which[0]);
    pthread_create(&t[1], NULL, sender, &which[1]);
    int wrong = 0;
    for (int i = 0; i < 2 * PER_THREAD; i++) {
        char payload;
        struct iovec iov = {&payload, 1};
        char control[CMSG_SPACE(sizeof(struct ucred))];
        struct msghdr mh = {.msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control)};
        if (recvmsg(sv[0], &mh, 0) != 1) { wrong = -1; break; }
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c)) {
            if (c->cmsg_type != SCM_CREDENTIALS) continue;
            struct ucred cred;
            memcpy(&cred, CMSG_DATA(c), sizeof(cred));
            if (cred.pid != (payload == 'B' ? 2222 : 1111)) wrong++;
        }
    }
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    printf("%s: %s: %d of %d records carried the wrong sender\n", wrong == 0 ? "PASS" : "FAIL", name, wrong, 2 * PER_THREAD);
    close(sv[0]);
    close(sv[1]);
    return wrong != 0;
}
int main(void) {
    int bad = run(SOCK_DGRAM, "dgram");
    bad += run(SOCK_SEQPACKET, "seqpacket");
    return bad != 0;
}
