#ifndef PSCAL_OPENSSH_RUNTIME_HOOKS_H
#define PSCAL_OPENSSH_RUNTIME_HOOKS_H

#include <setjmp.h>

typedef void (*pscal_openssh_cleanup_fn)(int);

typedef struct pscal_openssh_exit_context {
    sigjmp_buf env;
    int exit_code;
    pscal_openssh_cleanup_fn cleanup;
    struct pscal_openssh_exit_context *prev;
} pscal_openssh_exit_context;

void pscal_openssh_push_exit_context(pscal_openssh_exit_context *ctx);
void pscal_openssh_pop_exit_context(pscal_openssh_exit_context *ctx);
void pscal_openssh_register_cleanup(pscal_openssh_cleanup_fn cleanup);
void pscal_openssh_reset_progress_state(void);
const char *pscal_openssh_hostkey_path(const char *default_path);
const char *pscal_openssh_hostkey_dir(void);
void cleanup_exit(int code);

#ifdef PSCAL_TARGET_IOS
/* Optional runtime logger provided by the host app. */
extern void pscalRuntimeDebugLog(const char *message) __attribute__((weak));
#endif

#endif /* PSCAL_OPENSSH_RUNTIME_HOOKS_H */
