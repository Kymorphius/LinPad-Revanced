// A process-directed timer signal still arrives once the thread that created the timer (the
// group leader) has exited, as long as another thread is running: a worker waits for it.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static void *worker(void *arg) {
    (void) arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    siginfo_t si;
    struct timespec limit = {5, 0};
    int s = sigtimedwait(&set, &si, &limit);
    _exit(s == SIGUSR1 && si.si_code == SI_TIMER ? 0 : 1);
    return NULL;
}

int main(void) {
    pid_t child = fork();
    if (child == 0) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        sigprocmask(SIG_BLOCK, &set, NULL); // inherited by the worker, which waits for it
        struct sigevent sev = {.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1};
        timer_t timer;
        timer_create(CLOCK_MONOTONIC, &sev, &timer);
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        struct itimerspec its = {.it_value = {0, 300000000}};
        timer_settime(timer, 0, &its, NULL);
        pthread_exit(NULL); // the leader goes away; the process lives on in the worker
    }
    int status;
    waitpid(child, &status, 0);
    int ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    printf("%s: timer signal reaches a worker after the leader exited (status %#x)\n", ok ? "PASS" : "FAIL", status);
    return !ok;
}
