// Adversarial fixture for Tests/vm_ext_plugin/run.sh: pscal_ext_register()
// calls abort() (SIGABRT, distinct from crash_segv.c's SIGSEGV) -- covers a
// second crash class through the same fork-isolated probe.
#include <stdlib.h>

int pscal_ext_register(const void *host, unsigned int abi) {
    (void)host;
    (void)abi;
    abort();
    return 0;
}
