// The SIGCHLD for a multithreaded child reports the same status waitpid does, and the child's pid,
// even when the group leader exited first and a worker thread ends the process with exit_group.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
static volatile sig_atomic_t got, code, status, from;
static void handler(int sig, siginfo_t *si, void *uc) {
    (void) sig; (void) uc;
    got = 1; code = si->si_code; status = si->si_status; from = si->si_pid;
}
static void *worker(void *arg) {
    (void) arg;
    usleep(200000); // after the leader has gone
    exit(3);
}
int main(void) {
    struct sigaction sa = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO};
    sigaction(SIGCHLD, &sa, NULL);
    pid_t child = fork();
    if (child == 0) {
        pthread_t t;
        pthread_create(&t, NULL, worker, NULL);
        pthread_exit(NULL);
    }
    int wstatus;
    waitpid(child, &wstatus, 0);
    for (int i = 0; i < 50 && !got; i++) usleep(10000);
    int ok = WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 3 && got && code == CLD_EXITED && status == 3 && from == child;
    printf("%s: waitpid status %#x; SIGCHLD code %d status %d pid %d (child %d)\n", ok ? "PASS" : "FAIL",
           wstatus, (int) code, (int) status, (int) from, child);
    return !ok;
}
