#include "ext_builtins/register.h"

__attribute__((weak)) void registerShellFrontendBuiltins(void) {
    /* No-op on iOS tool runner builds */
}

#if defined(PSCAL_TARGET_IOS)
typedef struct VProcSessionStdio VProcSessionStdio;

__attribute__((weak)) void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}

__attribute__((weak)) void pscalShowGwinMessage(const char *message) {
    (void)message;
}

__attribute__((weak)) void pscalEditorDump(void) {
}

__attribute__((weak)) VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) {
    return NULL;
}
#endif
