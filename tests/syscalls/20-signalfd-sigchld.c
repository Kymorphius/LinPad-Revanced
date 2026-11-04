// A SIGCHLD read from a signalfd names the child and says how it ended:
// ssi_code CLD_EXITED with ssi_status the exit code, or CLD_KILLED with the signal.
#include <signal.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
int main(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_BLOCK, &set, NULL);
    int sfd = signalfd(-1, &set, 0);
    CHECK(sfd >= 0, "signalfd");

    pid_t exited = fork();
    if (exited == 0)
        _exit(3);
    struct signalfd_siginfo si;
    ssize_t r = read(sfd, &si, sizeof(si));
    CHECK(r == sizeof(si) && si.ssi_signo == SIGCHLD && si.ssi_pid == (uint32_t) exited &&
          si.ssi_code == CLD_EXITED && si.ssi_status == 3,
          "exit(3): signo %u pid %u (child %d) code %d status %d", si.ssi_signo, si.ssi_pid, exited, si.ssi_code, si.ssi_status);
    waitpid(exited, NULL, 0);

    pid_t killed = fork();
    if (killed == 0) {
        raise(SIGKILL);
        _exit(0);
    }
    r = read(sfd, &si, sizeof(si));
    CHECK(r == sizeof(si) && si.ssi_signo == SIGCHLD && si.ssi_pid == (uint32_t) killed &&
          si.ssi_code == CLD_KILLED && si.ssi_status == SIGKILL,
          "killed: pid %u (child %d) code %d status %d", si.ssi_pid, killed, si.ssi_code, si.ssi_status);
    waitpid(killed, NULL, 0);
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures != 0;
}
