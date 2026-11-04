// Checks: read(2) with a count above 4 GiB on a 64-bit guest.
// Expected (Linux, read(2): count is size_t): reads the whole 12 KiB file.
// Observed upstream: the count is truncated to 32 bits (4 GiB + 10 becomes 10). The same
// dword_t size appears in pread, sendfile, copy_file_range and mremap.
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
int main(void) {
    int fd = open("/tmp/rw64", O_CREAT | O_TRUNC | O_RDWR, 0644);
    static char buf[12288]; write(fd, buf, sizeof buf); lseek(fd, 0, SEEK_SET);
    // A large mapping so that a 4 GiB + 10 count is not EFAULT on Linux; only 12 KiB is touched.
    char *m = mmap(NULL, (1ULL << 32) + 4096 * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) { perror("FAIL: mmap"); return 1; }
    ssize_t n = read(fd, m, (1ULL << 32) + 10);
    printf("%s: read(count=4GiB+10) = %zd (want 12288)\n", n == 12288 ? "PASS" : "FAIL", n);
    unlink("/tmp/rw64");
    return n != 12288;
}
