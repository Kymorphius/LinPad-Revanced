// Checks: open(O_RDWR) by a non-owner on a file that grants others read but not write.
// Expected (Linux, path_resolution(7) / open(2)): EACCES; every requested bit must be granted.
// Observed upstream: access_check accepts the open if any one requested bit is granted.
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
int main(void) {
    int fd = open("/tmp/ro-for-others", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    close(fd);
    if (setgid(1000) || setuid(1000)) { perror("FAIL: setuid"); return 1; }
    int r = open("/tmp/ro-for-others", O_RDWR), e = errno;
    int ok = r == -1 && e == EACCES;
    printf("%s: open(O_RDWR) as uid 1000 = %d errno %d (want -1 EACCES)\n", ok ? "PASS" : "FAIL", r, r < 0 ? e : 0);
    return !ok;
}
