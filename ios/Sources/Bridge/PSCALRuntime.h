#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PSCALRuntimeOutputHandler)(const char *utf8, size_t length, void *context);
typedef void (*PSCALRuntimeExitHandler)(int status, void *context);
typedef void (*PSCALRuntimeSessionOutputHandler)(uint64_t session_id,
                                                 const char *utf8,
                                                 size_t length,
                                                 void *context);

/// Configures the callbacks used to stream stdout/stderr data and completion status.
void PSCALRuntimeConfigureHandlers(PSCALRuntimeOutputHandler output_handler,
                                   PSCALRuntimeExitHandler exit_handler,
                                   void *context);
void PSCALRuntimeOutputDidProcess(size_t length);

/// Completes interposer initialization once the app runtime is ready.
void PSCALRuntimeInterposeBootstrap(void);

/// Enables or disables interposed logic after bootstrap.
void PSCALRuntimeInterposeSetFeatureEnabled(int enabled);

/// Launches the PSCAL shell (exsh) with the provided argv vector.
/// Returns -1 if a runtime is already active or the PTY could not be created.
int PSCALRuntimeLaunchExsh(int argc, char* argv[]);

/// Launches exsh on a dedicated pthread with an explicit stack size (in bytes).
/// Useful on iOS where the default libdispatch worker stack is too small for the compilers.
int PSCALRuntimeLaunchExshWithStackSize(int argc, char* argv[], size_t stackSizeBytes);

/// Writes UTF-8 input into the running exsh instance (no-op if not active).
void PSCALRuntimeSendInput(const char *utf8, size_t length);
/// Writes UTF-8 input into the specified session (direct PTY mode).
void PSCALRuntimeSendInputForSession(uint64_t session_id, const char *utf8, size_t length);

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
void PSCALRuntimeLogLine(const char *message);
void PSCALRuntimeSetDebugLogMirroring(int enable);
void PSCALRuntimeBeginScriptCapture(const char *path, int append);
void PSCALRuntimeEndScriptCapture(void);
int PSCALRuntimeScriptCaptureActive(void);

/// Returns a newly allocated C string containing the runtime log for the
/// current session; caller must free() the result. Returns NULL on failure.
char *pscalRuntimeCopySessionLog(void);

/// Clears the in-memory runtime log for the current session.
void pscalRuntimeResetSessionLog(void);

/// Returns a newly allocated C string containing the marketing (short) app
/// version from the bundle. Caller must free(); returns NULL on failure.
char *pscalRuntimeCopyMarketingVersion(void);

/// Delivers a signal to the active runtime thread (no-op if inactive).
/// Useful on iOS when virtual TTY mode prevents kernel-generated job-control
/// signals from being delivered via the terminal.
void PSCALRuntimeSendSignal(int signo);

/// Configures the PATH_TRUNCATE environment variable used by the shell to
/// present sandboxed paths as a shorter root (e.g. "/").
/// Passing NULL or an empty string clears the setting.
void PSCALRuntimeApplyPathTruncation(const char *path);

/// Enable or disable the virtual /dev/location device.
void PSCALRuntimeSetLocationDeviceEnabled(int enabled);

/// Writes a payload into the virtual /dev/location device.
int PSCALRuntimeWriteLocationDevice(const char *utf8, size_t length);

/// Creates/destroys an isolated shell runtime context for embedding scenarios
/// (e.g. multiple iPadOS windows). The caller owns the returned pointer.
void *PSCALRuntimeCreateShellContext(void);
void PSCALRuntimeDestroyShellContext(void *ctx);

/// Makes a shell context current for the next exsh launch. Pass takeOwnership=0
/// when the caller will free the context; non-zero transfers ownership to the
/// runtime (it will free any previously owned context).
void PSCALRuntimeSetShellContext(void *ctx, int takeOwnership);

/// Launches an OpenSSH ssh session backed by a kernel-managed virtual TTY.
/// Returns 0 on success and provides UI read/write fds.
int PSCALRuntimeCreateSshSession(int argc,
                                 char **argv,
                                 uint64_t session_id,
                                 int *out_read_fd,
                                 int *out_write_fd);

/// Launches an exsh shell session backed by a kernel-managed virtual TTY.
/// Returns 0 on success and provides UI read/write fds.
int PSCALRuntimeCreateShellSession(int argc,
                                   char **argv,
                                   uint64_t session_id,
                                   int *out_read_fd,
                                   int *out_write_fd);

/// Registers a per-session output handler (direct PTY mode).
void PSCALRuntimeRegisterSessionOutputHandler(uint64_t session_id,
                                              PSCALRuntimeSessionOutputHandler handler,
                                              void *context);
/// Unregisters a per-session output handler.
void PSCALRuntimeUnregisterSessionOutputHandler(uint64_t session_id);

/// Updates the virtual terminal size for a session-backed PTY.
int PSCALRuntimeSetSessionWinsize(uint64_t session_id, int cols, int rows);

/// Requests the UI to open an additional shell tab (iOS only).
int pscalRuntimeOpenShellTab(void);

#ifdef __cplusplus
}
#endif
