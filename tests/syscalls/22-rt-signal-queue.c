// Every instance of a blocked real-time signal is queued with its own
// siginfo, in order, up to RLIMIT_SIGPENDING (then EAGAIN); standard signals
// coalesce.
#define _GNU_SOURCE
#include <signal.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static int queue(int sig, int val) {
    siginfo_t si; memset(&si, 0, sizeof(si));
    si.si_signo = sig; si.si_code = SI_QUEUE; si.si_pid = getpid(); si.si_uid = getuid();
    si.si_value.sival_int = val;
    return (int) syscall(SYS_rt_tgsigqueueinfo, getpid(), (pid_t) syscall(SYS_gettid), sig, &si);
}
static volatile int seen[4], nseen;
static void handler(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    if (nseen < 4) seen[nseen] = si->si_value.sival_int;
    nseen++;
}
int main(void) {
    const int RT = 35;
    sigset_t set; sigemptyset(&set); sigaddset(&set, RT); sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    struct timespec zero = {0, 0};
    siginfo_t si;

    // sigwaitinfo dequeues both payloads in order
    CHECK(queue(RT, 1) == 0 && queue(RT, 2) == 0, "queue 35 twice");
    sigset_t rt; sigemptyset(&rt); sigaddset(&rt, RT);
    int s1 = sigwaitinfo(&rt, &si); int v1 = si.si_value.sival_int;
    int s2 = sigtimedwait(&rt, &si, &zero); int v2 = si.si_value.sival_int;
    CHECK(s1 == RT && v1 == 1 && s2 == RT && v2 == 2, "sigwaitinfo x2 -> (%d,%d) (%d,%d), want (35,1) (35,2)", s1, v1, s2, v2);
    sigset_t pend; sigpending(&pend);
    CHECK(!sigismember(&pend, RT), "35 no longer pending after both dequeued");

    // unblocking runs the handler once per instance, in order
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigaction(RT, &sa, NULL);
    queue(RT, 10); queue(RT, 20); queue(RT, 30);
    sigprocmask(SIG_UNBLOCK, &rt, NULL);
    CHECK(nseen == 3 && seen[0] == 10 && seen[1] == 20 && seen[2] == 30,
          "handler saw %d instances: %d %d %d (want 3: 10 20 30)", nseen, seen[0], seen[1], seen[2]);
    sigprocmask(SIG_BLOCK, &rt, NULL);

    // standard signals coalesce
    queue(SIGUSR1, 1); queue(SIGUSR1, 2);
    sigset_t usr; sigemptyset(&usr); sigaddset(&usr, SIGUSR1);
    s1 = sigtimedwait(&usr, &si, &zero);
    s2 = sigtimedwait(&usr, &si, &zero);
    CHECK(s1 == SIGUSR1 && s2 == -1 && errno == EAGAIN, "SIGUSR1 queued twice is delivered once (%d %d)", s1, s2);

    // past RLIMIT_SIGPENDING, sigqueue gives EAGAIN
    struct rlimit rl; getrlimit(RLIMIT_SIGPENDING, &rl);
    int ok = 0, r = 0;
    for (int i = 0; i < 5000; i++) {
        r = queue(RT, i);
        if (r != 0) break;
        ok++;
    }
    CHECK(r == -1 && errno == EAGAIN && ok > 1 && ok <= (int) rl.rlim_cur,
          "queue until full: %d queued (limit %ld), then r=%d errno=%d (want EAGAIN)", ok, (long) rl.rlim_cur, r, errno);
    int drained = 0, inorder = 1;
    while ((s1 = sigtimedwait(&rt, &si, &zero)) == RT) {
        if (si.si_value.sival_int != drained) inorder = 0;
        drained++;
    }
    CHECK(drained == ok && inorder, "drained %d in order=%d", drained, inorder);
    return failures != 0;
}
