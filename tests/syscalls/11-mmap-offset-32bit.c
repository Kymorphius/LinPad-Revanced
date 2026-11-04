// Checks: mmap(2) with a file offset of 4 GiB on a 64-bit guest.
// Expected (Linux, mmap(2): offset is off_t, 64-bit on arm64): the mapping shows the bytes
// stored at offset 4 GiB.
// Observed upstream: the offset is truncated to 32 bits and the page at offset 0 is mapped.
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
int main(void) {
    int fd = open("/tmp/sparse4g", O_CREAT | O_TRUNC | O_RDWR, 0644);
    pwrite(fd, "LOW!", 4, 0);
    pwrite(fd, "HIGH", 4, 1LL << 32);
    char *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, fd, 1LL << 32);
    if (m == MAP_FAILED) { perror("FAIL: mmap"); return 1; }
    int ok = memcmp(m, "HIGH", 4) == 0;
    printf("%s: mapped content \"%.4s\" (want HIGH)\n", ok ? "PASS" : "FAIL", m);
    unlink("/tmp/sparse4g");
    return !ok;
}
