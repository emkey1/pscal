#include <pthread.h>
#include <stdint.h>

typedef struct VProcSessionStdio VProcSessionStdio;

__attribute__((weak))
VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) {
    return NULL;
}

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
/* Host test stubs for iOS runtime hooks. */

typedef struct ShellRuntimeState ShellRuntimeState;
typedef struct PSCALRuntimeContext PSCALRuntimeContext;

void pscalRuntimeShellSessionExited(uint64_t session_id, int status) {
    (void)session_id;
    (void)status;
}

void pscalRuntimeKernelSessionExited(uint64_t session_id, int status) {
    (void)session_id;
    (void)status;
}

void pscalRuntimeRegisterShellThread(uint64_t session_id, pthread_t tid) {
    (void)session_id;
    (void)tid;
}

ShellRuntimeState *pscalRuntimeShellContextForSession(uint64_t session_id) {
    (void)session_id;
    return NULL;
}

PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void) {
    return NULL;
}

void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx) {
    (void)ctx;
}

__attribute__((weak))
void pscalRuntimeRequestSigint(void) {
}
#endif
