#include "ext_builtins/register.h"

#include <errno.h>
#include <stdlib.h>

__attribute__((weak)) void registerShellFrontendBuiltins(void) {
    /* No-op on iOS tool runner builds */
}

#if defined(PSCAL_TARGET_IOS)
__attribute__((weak)) void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}

__attribute__((weak)) void pscalShowGwinMessage(const char *message) {
    (void)message;
}

__attribute__((weak)) void pscalEditorDump(void) {
}

__attribute__((weak)) void pscalElvisDump(void) {
    pscalEditorDump();
}

__attribute__((weak)) void pscalIOSPromoteSDLWindow(void) {
}

__attribute__((weak)) void pscalIOSPromoteSDLNativeWindow(void *nativeWindow) {
    (void)nativeWindow;
}

__attribute__((weak)) void pscalIOSRestoreTerminalWindowKey(void) {
}

__attribute__((weak)) int pscalIOSSDLModeActive(void) {
    return 0;
}

__attribute__((weak)) void pscalRuntimeSdlDidOpen(void) {
}

__attribute__((weak)) void pscalRuntimeSdlDidClose(void) {
}

__attribute__((weak))
int PSCALRuntimePingHost(const char *host, int count, int timeout_ms, char **out_output) {
    (void)host;
    (void)count;
    (void)timeout_ms;
    if (out_output) {
        *out_output = NULL;
    }
    errno = ENOSYS;
    return 1;
}
#endif
