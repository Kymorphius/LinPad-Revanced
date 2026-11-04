// Checks: timer_create(SIGEV_THREAD_ID) with a nonexistent tid, then fork().
// Expected (Linux, timer_create(2)): EINVAL, and the process keeps working.
// Observed upstream: the EINVAL path returns with pids_lock still held, so the next
// call that takes pids_lock (here fork) blocks forever.
#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#ifndef sigev_notify_thread_id
#define sigev_notify_thread_id _sigev_un._tid
#endif
int main(void) {
    struct sigevent sev = { .sigev_notify = SIGEV_THREAD_ID, .sigev_signo = SIGUSR1 };
    sev.sigev_notify_thread_id = 99999;
    timer_t t;
    int r = timer_create(CLOCK_MONOTONIC, &sev, &t), e = errno;
    printf("%s: timer_create(bad tid) = %d errno %d (want -1 EINVAL)\n", r == -1 && e == EINVAL ? "PASS" : "FAIL", r, e);
    printf("forking...\n"); fflush(stdout);
    pid_t p = fork();
    if (p == 0) _exit(0);
    int st; waitpid(p, &st, 0);
    printf("PASS: fork after failed timer_create returned\n");
    return 0;
}
