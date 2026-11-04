// sigtimedwait wakes when a signal in its set arrives later, from another thread or process.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void *sender(void *arg) { (void) arg; usleep(150000); kill(getpid(), SIGUSR1); return NULL; }
int main(void) {
    sigset_t set; sigemptyset(&set); sigaddset(&set, SIGUSR1);
    sigprocmask(SIG_BLOCK, &set, NULL);
    struct timespec timeout = {3, 0};
    siginfo_t si;

    pthread_t t;
    pthread_create(&t, NULL, sender, NULL);
    double start = now();
    int r = sigtimedwait(&set, &si, &timeout);
    double took = now() - start;
    CHECK(r == SIGUSR1 && took < 1.5, "signal from a thread: r=%d after %.2fs", r, took);
    pthread_join(t, NULL);

    pid_t child = fork();
    if (child == 0) { usleep(150000); kill(getppid(), SIGUSR1); _exit(0); }
    start = now();
    r = sigtimedwait(&set, &si, &timeout);
    took = now() - start;
    CHECK(r == SIGUSR1 && took < 1.5 && si.si_pid == child, "signal from a process: r=%d pid %d (child %d) after %.2fs", r, si.si_pid, child, took);
    waitpid(child, NULL, 0);

    struct timespec shortwait = {0, 200000000};
    start = now();
    r = sigtimedwait(&set, &si, &shortwait);
    took = now() - start;
    CHECK(r == -1 && took >= 0.15 && took < 1.0, "times out: r=%d after %.2fs", r, took);
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures != 0;
}
