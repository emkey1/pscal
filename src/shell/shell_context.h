#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShellContext {
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
} ShellContext;

/// Creates a new shell context with the provided fds. Caller owns the result.
ShellContext *shellContextCreate(int stdin_fd, int stdout_fd, int stderr_fd);

/// Destroys a shell context created by shellContextCreate.
void shellContextDestroy(ShellContext *ctx);

/// Sets the current thread-local shell context, returning the previous one (may be NULL).
ShellContext *shellContextSetCurrent(ShellContext *ctx);

/// Returns the current thread-local shell context (may be NULL).
ShellContext *shellContextCurrent(void);

#ifdef __cplusplus
}
#endif
