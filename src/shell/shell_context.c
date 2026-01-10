#include "shell_context.h"

#include <stdlib.h>

#if defined(__APPLE__)
#define THREAD_LOCAL __thread
#else
#define THREAD_LOCAL _Thread_local
#endif

static THREAD_LOCAL ShellContext *g_shell_tls = NULL;

ShellContext *shellContextCreate(int stdin_fd, int stdout_fd, int stderr_fd) {
    ShellContext *ctx = (ShellContext *)calloc(1, sizeof(ShellContext));
    if (!ctx) {
        return NULL;
    }
    ctx->stdin_fd = stdin_fd;
    ctx->stdout_fd = stdout_fd;
    ctx->stderr_fd = stderr_fd;
    return ctx;
}

void shellContextDestroy(ShellContext *ctx) {
    if (!ctx) {
        return;
    }
    free(ctx);
}

ShellContext *shellContextSetCurrent(ShellContext *ctx) {
    ShellContext *previous = g_shell_tls;
    g_shell_tls = ctx;
    return previous;
}

ShellContext *shellContextCurrent(void) {
    return g_shell_tls;
}
