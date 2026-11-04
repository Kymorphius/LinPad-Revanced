// Checks: eventfd(2) with EFD_SEMAPHORE.
// Expected (Linux, eventfd(2)): the flag is accepted; each read returns 1 and decrements by 1.
// Observed upstream: eventfd returns -1 EINVAL for EFD_SEMAPHORE.
#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/eventfd.h>
int main(void) {
    int fd = eventfd(3, EFD_SEMAPHORE | EFD_NONBLOCK);
    if (fd < 0) { printf("FAIL: eventfd(3, EFD_SEMAPHORE) = -1 errno %d\n", errno); return 1; }
    uint64_t v1 = 0, v2 = 0; read(fd, &v1, 8); read(fd, &v2, 8);
    int ok = v1 == 1 && v2 == 1;
    printf("%s: reads %llu %llu (want 1 1)\n", ok ? "PASS" : "FAIL", (unsigned long long)v1, (unsigned long long)v2);
    return !ok;
}
