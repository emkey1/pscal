#ifndef PSCAL_OPENSSH_RUNTIME_HOOKS_H
#define PSCAL_OPENSSH_RUNTIME_HOOKS_H

#include <setjmp.h>
#include <signal.h>

#ifndef PSCAL_THREAD_LOCAL
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && \
    !defined(__STDC_NO_THREADS__)
#define PSCAL_THREAD_LOCAL _Thread_local
#else
#define PSCAL_THREAD_LOCAL __thread
#endif
#endif

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
void pscal_openssh_set_global_exit_handler(sigjmp_buf *env,
    volatile sig_atomic_t *code_out);
const char *pscal_openssh_hostkey_path(const char *default_path);
const char *pscal_openssh_hostkey_dir(void);
void pscal_clientloop_reset_hostkeys(void);
void pscal_mux_reset_state(void);
void pscal_sshconnect_reset_state(void);
void pscal_sshconnect2_reset_state(void);
void pscal_sshtty_reset_state(void);
void cleanup_exit(int code);

#ifdef PSCAL_TARGET_IOS
/* Optional runtime logger provided by the host app. */
extern void pscalRuntimeDebugLog(const char *message) __attribute__((weak));
#endif

#endif /* PSCAL_OPENSSH_RUNTIME_HOOKS_H */
