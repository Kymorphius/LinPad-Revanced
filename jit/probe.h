// A check that code memory really runs code, for the JIT to make before it relies on it.
//
// On an iOS device the code cache is only executable while the process counts as debugged
// (CS_DEBUGGED), or where TXM is present once the debugger has prepared it. The flags that say
// so can be right while the region still is not executable: mprotect(PROT_EXEC) can succeed and
// the first jump into the region then faults. That killed LinPad a few seconds after every
// launch (issue #9). Running one instruction first, with the fault caught, turns that into
// "the JIT stays off".
#ifndef JIT_PROBE_H
#define JIT_PROBE_H

#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <libkern/OSCacheControl.h>

static sigjmp_buf jit_probe_env;

static void jit_probe_fault(int sig) {
    (void) sig;
    siglongjmp(jit_probe_env, 1);
}

// Writes a `ret` through `rw`, then calls it through `rx` (the same word, as the executing view
// sees it). True if it returned; false if the jump faulted. Nothing else may be running in the
// region, and no other thread may be relying on the SIGSEGV/SIGBUS handlers meanwhile.
static inline bool jit_probe_executable(uint32_t *rw, uint32_t *rx) {
    *rw = 0xd65f03c0; // ret
    sys_icache_invalidate(rx, sizeof(uint32_t));

    struct sigaction sa = {.sa_handler = jit_probe_fault, .sa_flags = SA_NODEFER};
    struct sigaction old_bus, old_segv;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, &old_bus);
    sigaction(SIGSEGV, &sa, &old_segv);
    bool ran = false;
    if (sigsetjmp(jit_probe_env, 1) == 0) {
        // a raw call: the region holds unsigned code, which a pointer-authenticated call
        // (arm64e) would refuse
        __asm__ volatile("blr %0" : : "r"(rx) : "x30", "memory");
        ran = true;
    }
    sigaction(SIGBUS, &old_bus, NULL);
    sigaction(SIGSEGV, &old_segv, NULL);
    return ran;
}

#endif
