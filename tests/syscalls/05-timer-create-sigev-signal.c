// Checks: a POSIX timer created with SIGEV_SIGNAL delivers its signal.
// Expected (Linux, timer_create(2)): SIGUSR1 arrives with si_code SI_TIMER.
// Observed upstream: timer->thread_pid is set to 0 for SIGEV_SIGNAL, the callback looks up
// pid 0, and the signal is never delivered.
#include <stdio.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t hits, code;
static void h(int s, siginfo_t *si, void *u) { (void)s; (void)u; hits++; code = si->si_code; }
int main(void) {
    struct sigaction sa = { .sa_sigaction = h, .sa_flags = SA_SIGINFO };
    sigaction(SIGUSR1, &sa, NULL);
    struct sigevent sev = { .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGUSR1 };
    timer_t t;
    if (timer_create(CLOCK_MONOTONIC, &sev, &t) != 0) { perror("FAIL: timer_create"); return 1; }
    struct itimerspec its = { .it_value = { 0, 50000000 } };
    timer_settime(t, 0, &its, NULL);
    for (int i = 0; i < 20 && !hits; i++) usleep(50000);
    int ok = hits == 1 && code == SI_TIMER;
    printf("%s: signal delivered %d time(s), si_code %d (want 1, SI_TIMER=%d)\n", ok ? "PASS" : "FAIL", hits, code, SI_TIMER);
    return !ok;
}
