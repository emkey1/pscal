#include <pthread.h>
#include <stdint.h>

typedef struct VProcSessionStdio VProcSessionStdio;

__attribute__((weak))
VProcSessionStdio *PSCALRuntimeGetCurrentRuntimeStdio(void) {
    return NULL;
}

__attribute__((weak))
void PSCALRuntimeSetCurrentRuntimeStdio(VProcSessionStdio *stdio_ctx) {
    (void)stdio_ctx;
}

__attribute__((weak))
int PSCALRuntimeSetTabTitle(const char *title) {
    (void)title;
    return -1;
}

__attribute__((weak))
int PSCALRuntimeSetTabStartupCommand(const char *command) {
    (void)command;
    return -1;
}

__attribute__((weak))
void PSCALRuntimeOnProcessGroupEmpty(int pgid) {
    (void)pgid;
}

__attribute__((weak))
void pscalRuntimeRegisterShellThread(uint64_t session_id, pthread_t tid) {
    (void)session_id;
    (void)tid;
}

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
__attribute__((weak))
int pscalRuntimeCurrentForegroundPgid(void) {
    return -1;
}
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
