// Checks: capget(2) fills in the header version and the capability sets.
// Expected (Linux, capget(2); kernel/capability.c): root gets full effective/permitted
// sets; a NULL datap probe writes back _LINUX_CAPABILITY_VERSION_3 (0x20080522).
// Observed upstream: sys_capget returns 0 and writes nothing.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
struct hdr { unsigned version; int pid; };
struct data { unsigned eff, perm, inh; };
int main(void) {
    struct hdr h = { 0x20080522, 0 };
    struct data d[2]; memset(d, 0xa5, sizeof d);
    long r = syscall(SYS_capget, &h, d);
    int ok1 = r == 0 && d[0].eff != 0xa5a5a5a5 && d[0].perm != 0xa5a5a5a5;
    printf("%s: capget data eff=%#x perm=%#x\n", ok1 ? "PASS" : "FAIL", d[0].eff, d[0].perm);
    struct hdr p = { 0, 0 };
    syscall(SYS_capget, &p, NULL);
    int ok2 = p.version == 0x20080522;
    printf("%s: probe version=%#x (want 0x20080522)\n", ok2 ? "PASS" : "FAIL", p.version);
    return !(ok1 && ok2);
}
