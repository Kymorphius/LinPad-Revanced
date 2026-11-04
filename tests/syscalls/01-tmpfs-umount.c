// Checks: umount(2) of a tmpfs mount.
// Expected (Linux, umount(2)): returns 0 and the mount is gone.
// Observed upstream: the emulator process aborts in tmpfs_umount (TODO("tmpfs umount")).
#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
int main(void) {
    mkdir("/tmp/umnt", 0755);
    if (mount("none", "/tmp/umnt", "tmpfs", 0, NULL) != 0) { perror("FAIL: mount"); return 1; }
    printf("mounted, unmounting...\n"); fflush(stdout);
    int r = umount("/tmp/umnt");
    printf("%s: umount = %d\n", r == 0 ? "PASS" : "FAIL", r);
    return r != 0;
}
