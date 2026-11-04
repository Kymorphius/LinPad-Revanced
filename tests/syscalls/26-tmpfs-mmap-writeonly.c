// A private mapping of a tmpfs file copies its contents, so a write-only descriptor cannot map it.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
static int failures;
#define CHECK(cond, ...) do { if (cond) printf("PASS: "); else { printf("FAIL: "); failures++; } \
    printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)
int main(void) {
    mkdir("/tmp/mm", 0755);
    if (mount("none", "/tmp/mm", "tmpfs", 0, NULL) != 0) { perror("FAIL: mount"); return 1; }
    int w = open("/tmp/mm/f", O_CREAT | O_WRONLY, 0644);
    write(w, "secret", 6);
    void *p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, w, 0);
    CHECK(p == MAP_FAILED && errno == EACCES, "private map of a write-only fd: %p errno=%d", p, errno);
    int r = open("/tmp/mm/f", O_RDONLY);
    p = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, r, 0);
    CHECK(p != MAP_FAILED && memcmp(p, "secret", 6) == 0, "private map of a read-only fd shows the contents");
    printf(failures ? "RESULT: FAIL (%d)\n" : "RESULT: PASS\n", failures);
    return failures != 0;
}
