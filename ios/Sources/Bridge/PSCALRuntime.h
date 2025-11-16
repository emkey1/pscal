#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSCALRuntimeOutputHandler)(const char *utf8, size_t length, void *context);
typedef void (*PSCALRuntimeExitHandler)(int status, void *context);

/// Configures the callbacks used to stream stdout/stderr data and completion status.
void PSCALRuntimeConfigureHandlers(PSCALRuntimeOutputHandler output_handler,
                                   PSCALRuntimeExitHandler exit_handler,
                                   void *context);

/// Launches the PSCAL shell (exsh) with the provided argv vector.
/// Returns -1 if a runtime is already active or the PTY could not be created.
int PSCALRuntimeLaunchExsh(int argc, char* argv[]);

/// Launches exsh on a dedicated pthread with an explicit stack size (in bytes).
/// Useful on iOS where the default libdispatch worker stack is too small for the compilers.
int PSCALRuntimeLaunchExshWithStackSize(int argc, char* argv[], size_t stackSizeBytes);

/// Writes UTF-8 input into the running exsh instance (no-op if not active).
void PSCALRuntimeSendInput(const char *utf8, size_t length);

/// Returns non-zero while the runtime is active.
int PSCALRuntimeIsRunning(void);

/// Configures the destination file for AddressSanitizer crash reports when available.
void PSCALRuntimeConfigureAsanReportPath(const char *path);

/// Updates the PTY window size (columns/rows). Safe to call before launching;
/// the size will be applied once exsh starts.
void PSCALRuntimeUpdateWindowSize(int columns, int rows);

/// Returns non-zero when the runtime is using the virtual TTY fallback (pipes).
int PSCALRuntimeIsVirtualTTY(void);

void pscalRuntimeDebugLog(const char *message);

#ifdef __cplusplus
}
#endif
