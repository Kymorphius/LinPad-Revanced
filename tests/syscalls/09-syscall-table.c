// Checks: three arm64 syscall table entries (119 sched_setscheduler,
// 126 sched_get_priority_min, 240 rt_tgsigqueueinfo).
// Expected (Linux, include/uapi/asm-generic/unistd.h and the man pages):
//   sched_setscheduler(0, SCHED_OTHER, {0}) == 0; sched_get_priority_min(SCHED_FIFO) == 1;
//   rt_tgsigqueueinfo to self delivers the signal.
// Observed upstream: 119 returns its first argument (the result is never written to x0),
// 126 is wired to the priority_max handler, 240 is wired to rt_sigreturn.
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sched.h>
#include <sys/syscall.h>
#include <sys/wait.h>
static volatile int got;
static void h(int s) { (void)s; got = 1; }
int main(void) {
    struct sched_param sp = { 0 };
    long r = syscall(SYS_sched_setscheduler, getpid(), SCHED_OTHER, &sp);
    printf("%s: sched_setscheduler(getpid()=%d, SCHED_OTHER) = %ld (want 0)\n", r == 0 ? "PASS" : "FAIL", getpid(), r);
    long m = syscall(SYS_sched_get_priority_min, SCHED_FIFO);
    printf("%s: sched_get_priority_min(SCHED_FIFO) = %ld (want 1)\n", m == 1 ? "PASS" : "FAIL", m);
    pid_t c = fork();
    if (c == 0) {
        signal(SIGUSR1, h);
        siginfo_t si; memset(&si, 0, sizeof si);
        si.si_signo = SIGUSR1; si.si_code = SI_QUEUE;
        long q = syscall(SYS_rt_tgsigqueueinfo, getpid(), getpid(), SIGUSR1, &si);
        _exit(q == 0 && got ? 0 : 1);
    }
    int st; waitpid(c, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s: rt_tgsigqueueinfo to self (child status 0x%x)\n", ok ? "PASS" : "FAIL", st);
    return !(r == 0 && m == 1 && ok);
}
