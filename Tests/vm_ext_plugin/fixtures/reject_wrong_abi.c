// Adversarial fixture for Tests/vm_ext_plugin/run.sh: a plugin built
// against an ABI major version the host will never report (999), so its
// own version check deterministically rejects every real host. Exercises
// the real "ABI major mismatch" rejection path described in
// pscal_ext_api.h (a plugin checks PSCAL_EXT_ABI_MAJOR_OF(host_abi) itself
// and returns nonzero) rather than an arbitrary sentinel return value.
#include "backend_ast/pscal_ext_api.h"

int pscal_ext_register(const PscalExtHostApi *host, uint32_t host_abi) {
    (void)host;
    if (PSCAL_EXT_ABI_MAJOR_OF(host_abi) != 999) {
        return 1;
    }
    return 0;
}
