#pragma once

#include <stdbool.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Enables or disables the virtual TTY fallback. When enabled, stdin/stdout/stderr
// are treated as interactive even if the underlying file descriptors are pipes.
void pscalRuntimeSetVirtualTTYEnabled(bool enabled);

bool pscalRuntimeVirtualTTYEnabled(void);

bool pscalRuntimeFdIsInteractive(int fd);
bool pscalRuntimeStdinIsInteractive(void);
bool pscalRuntimeStdoutIsInteractive(void);
bool pscalRuntimeStderrIsInteractive(void);

bool pscalRuntimeFdHasRealTTY(int fd);
bool pscalRuntimeStdinHasRealTTY(void);
bool pscalRuntimeStdoutHasRealTTY(void);
bool pscalRuntimeStderrHasRealTTY(void);

#ifdef __cplusplus
}
#endif

