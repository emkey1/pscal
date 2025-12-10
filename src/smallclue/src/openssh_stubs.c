#include <setjmp.h>
#include <signal.h>
#include <stdio.h>

#include "../../third-party/openssh-10.2p1/pscal_runtime_hooks.h"

/* Stub OpenSSH entry points so the standalone build links. */
__attribute__((weak)) int pscal_openssh_ssh_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "ssh: OpenSSH integration unavailable in this build\n");
    return 127;
}

__attribute__((weak)) int pscal_openssh_scp_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "scp: OpenSSH integration unavailable in this build\n");
    return 127;
}

__attribute__((weak)) int pscal_openssh_sftp_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "sftp: OpenSSH integration unavailable in this build\n");
    return 127;
}

__attribute__((weak)) int pscal_openssh_ssh_keygen_main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "ssh-keygen: OpenSSH integration unavailable in this build\n");
    return 127;
}

static pscal_openssh_exit_context *g_current_ctx = NULL;

void pscal_openssh_reset_progress_state(void) {
    /* No-op for the stub build. */
}

void pscal_openssh_push_exit_context(pscal_openssh_exit_context *ctx) {
    if (ctx) {
        ctx->exit_code = 0;
        g_current_ctx = ctx;
    }
}

void pscal_openssh_pop_exit_context(pscal_openssh_exit_context *ctx) {
    (void)ctx;
    g_current_ctx = NULL;
}

void pscal_openssh_request_exit(int code) {
    if (g_current_ctx) {
        g_current_ctx->exit_code = code;
        siglongjmp(g_current_ctx->env, 1);
    }
}
