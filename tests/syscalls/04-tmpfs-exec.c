// Checks: execve(2) of a static binary stored on tmpfs.
// Expected (Linux, execve(2)): the copy runs and exits 0.
// Observed upstream: the emulator crashes while mapping the ELF segments (load_entry).
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "child") == 0) return 0;
    mkdir("/tmp/ex", 0755);
    if (mount("none", "/tmp/ex", "tmpfs", 0, NULL) != 0) { perror("FAIL: mount"); return 1; }
    int in = open("/proc/self/exe", O_RDONLY), out = open("/tmp/ex/t", O_CREAT | O_WRONLY, 0755);
    char buf[65536]; ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) write(out, buf, n);
    close(in); close(out);
    printf("exec from tmpfs...\n"); fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) { execl("/tmp/ex/t", "t", "child", (char *)0); _exit(127); }
    int st; waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s: child status 0x%x\n", ok ? "PASS" : "FAIL", st);
    return !ok;
}
