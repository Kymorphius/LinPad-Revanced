// Process-directed signals stay on the process's queue until one thread
// dequeues them, one instance at a time; standard signals coalesce per
// queue (thread and process each keep one); dequeue order is the lowest
// signal number first, FIFO within one number.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
static sigset_t s40;
static int got_a, got_b, val_a, val_b;
static void *take_one(void *arg) {
    siginfo_t si;
    struct timespec ts = {2, 0};
    int sig = sigtimedwait(&s40, &si, &ts);
    *(int *) arg = sig;
    if (arg == &got_a) val_a = si.si_value.sival_int; else val_b = si.si_value.sival_int;
    return NULL;
}
static int q(int sig, int v) { return sigqueue(getpid(), sig, (union sigval) {.sival_int = v}); }
int main(void) {
    sigset_t all; sigemptyset(&all);
    sigaddset(&all, 40); sigaddset(&all, 41); sigaddset(&all, SIGUSR1);
    sigprocmask(SIG_BLOCK, &all, NULL);
    sigemptyset(&s40); sigaddset(&s40, 40);
    struct timespec zero = {0, 0};
    siginfo_t si;

    // two instances to the process: thread A takes one and exits, B the other
    q(40, 1); q(40, 2);
    pthread_t t;
    pthread_create(&t, NULL, take_one, &got_a); pthread_join(t, NULL);
    pthread_create(&t, NULL, take_one, &got_b); pthread_join(t, NULL);
    CHECK(got_a == 40 && val_a == 1 && got_b == 40 && val_b == 2,
          "each thread dequeues one: A (%d,%d) B (%d,%d), want (40,1) (40,2)", got_a, val_a, got_b, val_b);

    // standard signal: one thread-directed and one process-directed instance
    syscall(SYS_tgkill, getpid(), (pid_t) syscall(SYS_gettid), SIGUSR1);
    kill(getpid(), SIGUSR1);
    sigset_t usr; sigemptyset(&usr); sigaddset(&usr, SIGUSR1);
    int a = sigtimedwait(&usr, &si, &zero), b = sigtimedwait(&usr, &si, &zero);
    CHECK(a == SIGUSR1 && b == SIGUSR1, "tgkill + kill SIGUSR1 are two pending instances (%d %d)", a, b);

    // lowest number first, FIFO within a number
    q(41, 10); q(40, 20); q(41, 11); q(40, 21);
    int order[4], vals[4];
    sigset_t both; sigemptyset(&both); sigaddset(&both, 40); sigaddset(&both, 41);
    for (int i = 0; i < 4; i++) { order[i] = sigtimedwait(&both, &si, &zero); vals[i] = si.si_value.sival_int; }
    CHECK(order[0] == 40 && vals[0] == 20 && order[1] == 40 && vals[1] == 21 &&
          order[2] == 41 && vals[2] == 10 && order[3] == 41 && vals[3] == 11,
          "dequeue order (%d,%d) (%d,%d) (%d,%d) (%d,%d), want 40/20 40/21 41/10 41/11",
          order[0], vals[0], order[1], vals[1], order[2], vals[2], order[3], vals[3]);
    return failures != 0;
}
