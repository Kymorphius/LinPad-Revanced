// Checks: getcwd(3) inside a directory on tmpfs.
// Expected (Linux, getcwd(3)): returns "/tmp/gp/sub".
// Observed upstream: tmpfs_getpath never advances to the parent dirent, so the call
// spins forever (the test is killed by the timeout).
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mount.h>
#include <sys/stat.h>
int main(void) {
    char buf[256];
    mkdir("/tmp/gp", 0755);
    if (mount("none", "/tmp/gp", "tmpfs", 0, NULL) != 0) { perror("FAIL: mount"); return 1; }
    mkdir("/tmp/gp/sub", 0755);
    if (chdir("/tmp/gp/sub") != 0) { perror("FAIL: chdir"); return 1; }
    printf("calling getcwd...\n"); fflush(stdout);
    char *p = getcwd(buf, sizeof buf);
    int ok = p && strcmp(p, "/tmp/gp/sub") == 0;
    printf("%s: getcwd = %s\n", ok ? "PASS" : "FAIL", p ? p : "(null)");
    return !ok;
}
