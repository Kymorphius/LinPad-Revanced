// More of the signal queue: RLIMIT_SIGPENDING limits sigqueue but not kill, a timer's overrun
// and id reach timer_getoverrun and signalfd, a signal sent to a process that shares its
// handlers (CLONE_SIGHAND without CLONE_THREAD) stays pending for that process, and
// rt_tgsigqueueinfo can queue to another process.
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int sync_pipe[2];

static int clone_child(void *arg) {
    // SIGUSR1 is blocked, inherited from the parent; wait until the parent has sent it to us
    (void) arg;
    char c;
    read(sync_pipe[0], &c, 1);
    sigset_t pending;
    sigpending(&pending);
    return sigismember(&pending, SIGUSR1) ? 0 : 1;
}

int main(void) {
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, 40);
    sigaddset(&block, SIGALRM);
    sigprocmask(SIG_BLOCK, &block, NULL);

    // sigqueue() fails with EAGAIN once the limit is used up; kill() of a standard signal still works
    struct rlimit old, none = {0, 0};
    getrlimit(RLIMIT_SIGPENDING, &old);
    setrlimit(RLIMIT_SIGPENDING, &none);
    int r = sigqueue(getpid(), 40, (union sigval) {.sival_int = 1});
    CHECK(r == -1 && errno == EAGAIN, "sigqueue with a limit of 0: r=%d errno=%d", r, errno);
    r = kill(getpid(), SIGUSR1);
    CHECK(r == 0, "kill with a limit of 0: r=%d", r);
    sigset_t one;
    sigemptyset(&one);
    sigaddset(&one, SIGUSR1);
    struct timespec zero = {0, 0};
    siginfo_t si;
    CHECK(sigtimedwait(&one, &si, &zero) == SIGUSR1, "the kill() signal is pending");
    setrlimit(RLIMIT_SIGPENDING, &old);

    // an interval timer whose signal is blocked while it expires several times
    struct sigevent sev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
    timer_t unused, timer; // the second timer has id 1, so a tid of 0 would be wrong
    timer_create(CLOCK_MONOTONIC, &sev, &unused);
    timer_create(CLOCK_MONOTONIC, &sev, &timer);
    struct itimerspec its = {.it_interval = {0, 20000000}, .it_value = {0, 20000000}};
    timer_settime(timer, 0, &its, NULL);
    usleep(300000);
    struct itimerspec stop = {0};
    timer_settime(timer, 0, &stop, NULL);
    int sfd = signalfd(-1, &one, SFD_NONBLOCK);
    struct signalfd_siginfo ssi;
    ssize_t n = read(sfd, &ssi, sizeof(ssi));
    CHECK(n == sizeof(ssi) && ssi.ssi_signo == SIGUSR1 && ssi.ssi_code == SI_TIMER &&
          ssi.ssi_tid == (uint32_t) (uintptr_t) timer,
          "signalfd reads the timer signal: n=%zd signo %u code %d tid %u overrun %u",
          n, ssi.ssi_signo, ssi.ssi_code, ssi.ssi_tid, ssi.ssi_overrun);
    CHECK(ssi.ssi_overrun >= 3, "expirations while the signal was pending count as overrun: %u", ssi.ssi_overrun);
    int overrun = timer_getoverrun(timer);
    CHECK(overrun >= 3, "timer_getoverrun = %d after taking that signal", overrun);
    timer_delete(timer);
    timer_delete(unused);
    while (read(sfd, &ssi, sizeof(ssi)) == sizeof(ssi))
        ;
    while (sigtimedwait(&one, &si, &zero) == SIGUSR1)
        ;

    // a process that shares its handlers with this one still has its own pending signals
    pipe(sync_pipe);
    size_t stack_size = 1 << 20;
    char *stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    pid_t child = clone(clone_child, stack + stack_size, CLONE_VM | CLONE_SIGHAND | SIGCHLD, NULL);
    if (child > 0) {
        kill(child, SIGUSR1);
        write(sync_pipe[1], "x", 1);
        int status;
        waitpid(child, &status, 0);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "a signal sent to a CLONE_SIGHAND process is pending for it: status %#x", status);
    } else {
        CHECK(0, "clone failed errno=%d", errno);
    }

    // queue a real-time signal to another process, which waits for it
    int fds[2];
    pipe(fds);
    pid_t waiter = fork();
    if (waiter == 0) {
        sigset_t rt;
        sigemptyset(&rt);
        sigaddset(&rt, 40);
        sigprocmask(SIG_BLOCK, &rt, NULL);
        write(fds[1], "r", 1);
        siginfo_t got;
        struct timespec limit = {5, 0};
        int s = sigtimedwait(&rt, &got, &limit);
        _exit(s == 40 && got.si_value.sival_int == 77 ? 0 : 1);
    }
    char c;
    read(fds[0], &c, 1);
    usleep(100000);
    siginfo_t out;
    memset(&out, 0, sizeof(out));
    out.si_signo = 40;
    out.si_code = SI_QUEUE;
    out.si_pid = getpid();
    out.si_uid = getuid();
    out.si_value.sival_int = 77;
    long q = syscall(SYS_rt_tgsigqueueinfo, waiter, waiter, 40, &out);
    int status;
    waitpid(waiter, &status, 0);
    CHECK(q == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "rt_tgsigqueueinfo to another process: q=%ld status %#x", q, status);
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures != 0;
}
