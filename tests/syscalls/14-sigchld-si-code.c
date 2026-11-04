// Checks: si_code of the SIGCHLD a parent receives when a child exits.
// Expected (Linux, sigaction(2) "SIGCHLD fills in si_code"): CLD_EXITED (1) with si_status 3.
// Observed upstream: si_code is SI_KERNEL (128) and si_status holds the raw wait status.
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
static volatile int code = -1, status = -1;
static void h(int s, siginfo_t *si, void *u) { (void)s; (void)u; code = si->si_code; status = si->si_status; }
int main(void) {
    struct sigaction sa = { .sa_sigaction = h, .sa_flags = SA_SIGINFO };
    sigaction(SIGCHLD, &sa, NULL);
    pid_t c = fork();
    if (c == 0) _exit(3);
    while (code == -1) pause();
    waitpid(c, NULL, 0);
    int ok = code == CLD_EXITED && status == 3;
    printf("%s: si_code %d si_status %d (want %d 3)\n", ok ? "PASS" : "FAIL", code, status, CLD_EXITED);
    return !ok;
}
