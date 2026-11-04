// jit_probe_executable (jit/probe.h) tells apart memory that runs code from memory that faults.
// Needs an Apple Silicon Mac. The executable region is set up the way the JIT does it: an anonymous
// mapping, a writable alias of it (vm_remap), then the original made read+execute.
#include <stdio.h>
#include <sys/mman.h>
#include <mach/mach.h>
#include <mach/vm_map.h>
#include "jit/probe.h"

static bool make_dual(uint32_t **rw, uint32_t **rx, size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return false;
    vm_address_t alias = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &alias, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t) p, false,
                 &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS)
        return false;
    if (mprotect((void *) alias, size, PROT_READ | PROT_WRITE) != 0 || mprotect(p, size, PROT_READ | PROT_EXEC) != 0)
        return false;
    *rw = (uint32_t *) alias;
    *rx = p;
    return true;
}

int main(void) {
    int failures = 0;

    uint32_t *rw, *rx;
    if (!make_dual(&rw, &rx, 16384)) {
        printf("SKIP: this host cannot make executable memory this way\n");
        return 2;
    }
    bool executable = jit_probe_executable(rw, rx);
    printf("%s: memory that runs code is reported executable\n", executable ? "PASS" : "FAIL");
    failures += !executable;

    // the same region, but the executing view loses its execute permission: what #9 looked like
    mprotect(rx, 16384, PROT_READ);
    bool lost = jit_probe_executable(rw, rx);
    printf("%s: a region that stopped being executable is reported not executable, without crashing\n", lost ? "FAIL" : "PASS");
    failures += lost;

    // the fault handlers it used are gone again
    struct sigaction now;
    sigaction(SIGSEGV, NULL, &now);
    bool restored = now.sa_handler == SIG_DFL;
    printf("%s: the previous SIGSEGV handler is restored\n", restored ? "PASS" : "FAIL");
    failures += !restored;

    // and it can run again afterwards
    mprotect(rx, 16384, PROT_READ | PROT_EXEC);
    bool again = jit_probe_executable(rw, rx);
    printf("%s: it can be used more than once\n", again ? "PASS" : "FAIL");
    failures += !again;
    return failures != 0;
}
