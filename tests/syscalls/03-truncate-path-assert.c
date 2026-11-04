// Checks: truncate(2) by path (arm64 syscall 45).
// Expected (Linux, truncate(2)): returns 0 and the file has the new size.
// Observed upstream: sys_truncate64 passes a NULL "at" fd to generic_setattrat, and the
// emulator aborts on an assertion in path_normalize.
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
int main(void) {
    int fd = open("/tmp/trunc.txt", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    write(fd, "hello world", 11); close(fd);
    printf("calling truncate...\n"); fflush(stdout);
    int r = truncate("/tmp/trunc.txt", 5);
    struct stat st; stat("/tmp/trunc.txt", &st);
    int ok = r == 0 && st.st_size == 5;
    printf("%s: truncate = %d, size = %lld\n", ok ? "PASS" : "FAIL", r, (long long)st.st_size);
    return !ok;
}
