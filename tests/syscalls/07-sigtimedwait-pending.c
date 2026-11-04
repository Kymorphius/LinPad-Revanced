// Checks: sigtimedwait(2) when the signal is already pending.
// Expected (Linux, sigtimedwait(2)): returns SIGUSR1 immediately.
// Observed upstream: sys_rt_sigtimedwait sleeps before looking at the pending set, so it
// waits for the whole timeout (or forever with a NULL timeout) and returns EAGAIN.
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
int main(void) {
    sigset_t s; sigemptyset(&s); sigaddset(&s, SIGUSR1);
    sigprocmask(SIG_BLOCK, &s, NULL);
    raise(SIGUSR1);
    struct timespec to = { 2, 0 };
    int r = sigtimedwait(&s, NULL, &to);
    printf("%s: sigtimedwait = %d errno %d (want %d)\n", r == SIGUSR1 ? "PASS" : "FAIL", r, r < 0 ? errno : 0, SIGUSR1);
    return r != SIGUSR1;
}
