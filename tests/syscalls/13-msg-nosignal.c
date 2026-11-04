// Checks: send(2) with MSG_NOSIGNAL to a socket whose peer is closed.
// Expected (Linux, send(2) "MSG_NOSIGNAL"): -1 EPIPE and no SIGPIPE.
// Observed upstream: EPIPE is returned but SIGPIPE is also raised.
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/socket.h>
static volatile int sigpipes;
static void h(int s) { (void)s; sigpipes++; }
int main(void) {
    signal(SIGPIPE, h);
    int sv[2]; socketpair(AF_UNIX, SOCK_STREAM, 0, sv); close(sv[1]);
    int r = send(sv[0], "x", 1, MSG_NOSIGNAL), e = errno;
    int ok = r == -1 && e == EPIPE && sigpipes == 0;
    printf("%s: send = %d errno %d, SIGPIPE count %d (want -1 EPIPE, 0)\n", ok ? "PASS" : "FAIL", r, e, sigpipes);
    return !ok;
}
