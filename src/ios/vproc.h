#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/select.h>
#include <dirent.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clean-room virtual process abstraction for iOS builds. Maintains a private
// fd table per task and translates basic I/O syscalls through the table.

typedef struct VProc VProc;
struct pscal_fd;

typedef struct {
    int stdin_fd;   // host fd to dup for stdin; -1 to dup host STDIN_FILENO; -2 for /dev/null
    int stdout_fd;  // host fd to dup for stdout; -1 to dup host STDOUT_FILENO
    int stderr_fd;  // host fd to dup for stderr; -1 to dup host STDERR_FILENO
    int winsize_cols;
    int winsize_rows;
    int pid_hint;   // optional fixed synthetic pid; -1 for auto
    int job_id;     // optional job id to stamp on creation
} VProcOptions;

typedef struct {
    int cols;
    int rows;
} VProcWinsize;

typedef struct {
    int pid;
    pthread_t tid;
    int parent_pid;
    int pgid;
    int sid;
    bool exited;
    bool stopped;
    bool continued;
    bool zombie;
    int exit_signal;
    int status;
    int stop_signo;
    bool sigchld_pending;
    int rusage_utime;
    int rusage_stime;
    int fg_pgid;
    int job_id;
    char comm[16];
    char command[64];
} VProcSnapshot;

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)

VProcOptions vprocDefaultOptions(void);
VProc *vprocCreate(const VProcOptions *opts);
void vprocDestroy(VProc *vp);
int vprocPid(VProc *vp);
int vprocReservePid(void);

void vprocActivate(VProc *vp);
void vprocDeactivate(void);
VProc *vprocCurrent(void);
bool vprocWaitIfStopped(VProc *vp);

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity);

int vprocTranslateFd(VProc *vp, int fd);
struct pscal_fd *vprocGetPscalFd(VProc *vp, int fd);
int vprocDup(VProc *vp, int fd);
int vprocDup2(VProc *vp, int fd, int target);
/* Sync vproc table entry to a host fd already duplicated onto target_fd. */
int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src);
int vprocClose(VProc *vp, int fd);
int vprocPipe(VProc *vp, int pipefd[2]);
/* Duplicate a host fd onto a target host descriptor, bypassing shim indirection. */
int vprocHostDup2(int host_fd, int target_fd);
/* Duplicate a host descriptor without routing through the shim table. */
int vprocHostDup(int fd);
/* Open a host path without routing through the shim table. */
int vprocHostOpen(const char *path, int flags, ...);
/* Create a host pipe without routing through the shim table. */
int vprocHostPipe(int pipefd[2]);
/* Seek on a host descriptor without routing through the shim table. */
off_t vprocHostLseek(int fd, off_t offset, int whence);
/* Sync a host descriptor without routing through the shim table. */
int vprocHostFsync(int fd);
/* Close a host descriptor without routing through the shim table. */
int vprocHostClose(int fd);
/* Read from a host descriptor without routing through the shim table. */
ssize_t vprocHostRead(int fd, void *buf, size_t count);
/* Write to a host descriptor without routing through the shim table. */
ssize_t vprocHostWrite(int fd, const void *buf, size_t count);
/* Virtual isatty shim for session-backed TTYs. */
int vprocIsattyShim(int fd);
/* Enable/disable the virtual location device (/dev/location). */
void vprocLocationDeviceSetEnabled(bool enabled);
/* Write a payload to the virtual location device. */
ssize_t vprocLocationDeviceWrite(const void *data, size_t len);
/* Read from the shared session input queue (stdin) on iOS. */
ssize_t vprocSessionReadInputShim(void *buf, size_t count);
/* Read from the shared session input queue with nonblocking support. */
ssize_t vprocSessionReadInputShimMode(void *buf, size_t count, bool nonblocking);
/* Spawn a joinable host thread, bypassing vproc's pthread_create shim. */
int vprocHostPthreadCreate(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg);
/* Adopt an existing host fd into the vproc table, returning the vproc-local fd. */
int vprocAdoptHostFd(VProc *vp, int host_fd);
/* Inherit the current vproc into new threads created via pthread_create shim. */
int vprocPthreadCreateShim(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg);
int vprocOpenAt(VProc *vp, const char *path, int flags, int mode);
int vprocSetWinsize(VProc *vp, int cols, int rows);
int vprocGetWinsize(VProc *vp, VProcWinsize *out);

/* Task lifecycle management for virtual processes (iOS clean-room). */
int vprocRegisterTidHint(int pid, pthread_t tid);
int vprocRegisterThread(VProc *vp, pthread_t tid);
void vprocUnregisterThread(VProc *vp, pthread_t tid);
int vprocSpawnThread(VProc *vp, void *(*start_routine)(void *), void *arg, pthread_t *thread_out);
void vprocMarkExit(VProc *vp, int status);
void vprocSetParent(int pid, int parent_pid);
int vprocSetPgid(int pid, int pgid);
int vprocSetSid(int pid, int sid);
int vprocGetPgid(int pid);
int vprocGetSid(int pid);
void vprocSetStopUnsupported(int pid, bool stop_unsupported);
int vprocSetForegroundPgid(int sid, int fg_pgid);
int vprocGetForegroundPgid(int sid);
int vprocNextJobIdSeed(void);
void vprocMarkGroupExit(int pid, int status);
void vprocSetRusage(int pid, int utime, int stime);
int vprocBlockSignals(int pid, int mask);
int vprocUnblockSignals(int pid, int mask);
int vprocIgnoreSignal(int pid, int mask);
int vprocDefaultSignal(int pid, int mask);
int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old);

// Shimmed syscalls: respect the active vproc on the current thread when set,
// otherwise fall back to real libc/syscall equivalents.
ssize_t vprocReadShim(int fd, void *buf, size_t count);
ssize_t vprocWriteShim(int fd, const void *buf, size_t count);
int vprocDupShim(int fd);
int vprocDup2Shim(int fd, int target);
int vprocCloseShim(int fd);
int vprocFsyncShim(int fd);
int vprocPipeShim(int pipefd[2]);
int vprocFstatShim(int fd, struct stat *st);
int vprocStatShim(const char *path, struct stat *st);
int vprocLstatShim(const char *path, struct stat *st);
int vprocChdirShim(const char *path);
char *vprocGetcwdShim(char *buffer, size_t size);
int vprocAccessShim(const char *path, int mode);
int vprocMkdirShim(const char *path, mode_t mode);
int vprocRmdirShim(const char *path);
int vprocUnlinkShim(const char *path);
int vprocRemoveShim(const char *path);
int vprocRenameShim(const char *oldpath, const char *newpath);
DIR *vprocOpendirShim(const char *name);
int vprocSymlinkShim(const char *target, const char *linkpath);
ssize_t vprocReadlinkShim(const char *path, char *buf, size_t size);
char *vprocRealpathShim(const char *path, char *resolved_path);
int vprocIoctlShim(int fd, unsigned long request, ...);
off_t vprocLseekShim(int fd, off_t offset, int whence);
int vprocOpenShim(const char *path, int flags, ...);
int vprocPollShim(struct pollfd *fds, nfds_t nfds, int timeout);
int vprocSelectShim(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout);
pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options);
int vprocKillShim(pid_t pid, int sig);
pid_t vprocGetPidShim(void);
pid_t vprocGetPpidShim(void);
pid_t vprocGetpgrpShim(void);
pid_t vprocGetpgidShim(pid_t pid);
int vprocSetpgidShim(pid_t pid, pid_t pgid);
pid_t vprocSetsidShim(void);
pid_t vprocGetsidShim(pid_t pid);
pid_t vprocTcgetpgrpShim(int fd);
int vprocTcsetpgrpShim(int fd, pid_t pgid);
void vprocSetShellSelfPid(int pid);
int vprocGetShellSelfPid(void);
void vprocSetShellSelfTid(pthread_t tid);
bool vprocIsShellSelfThread(void);
/* Optional: identify a per-session "kernel" vproc that acts as adoptive parent. */
void vprocSetKernelPid(int pid);
int vprocGetKernelPid(void);
void vprocClearKernelPidGlobal(void);
int vprocGetSessionKernelPid(void);
void vprocSetSessionKernelPid(int pid);
/* Ensure a shared kernel vproc exists and return its pid. */
int vprocEnsureKernelPid(void);

/* Per-session stdio ownership: duplicated host fds that define the
 * controlling stdio for a given shell window/session. */
typedef struct VProcSessionInput {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned char *buf;
    size_t len;
    size_t cap;
    int reader_fd;
    uint64_t reader_generation;
    bool inited;
    bool eof;
    bool reader_active;
    bool interrupt_pending;
    bool stop_requested;
} VProcSessionInput;

typedef struct VProcSessionStdio {
    int stdin_host_fd;
    int stdout_host_fd;
    int stderr_host_fd;
    int kernel_pid;
    int shell_pid;
    VProcSessionInput *input;
    struct pscal_fd *stdin_pscal_fd;
    struct pscal_fd *stdout_pscal_fd;
    struct pscal_fd *stderr_pscal_fd;
    struct pscal_fd *pty_master;
    struct pscal_fd *pty_slave;
    int pty_in_fd;
    int pty_out_fd;
    pthread_t pty_in_thread;
    pthread_t pty_out_thread;
    bool pty_active;
    uint64_t session_id;
} VProcSessionStdio;

typedef void (*VProcSessionOutputHandler)(uint64_t session_id,
                                          const unsigned char *data,
                                          size_t len,
                                          void *context);

/* Obtain the current session stdio (per window/session). */
VProcSessionStdio *vprocSessionStdioCurrent(void);
/* Initialize session stdio from the current host stdio and kernel pid. */
void vprocSessionStdioInit(VProcSessionStdio *stdio_ctx, int kernel_pid);
/* Initialize session stdio from explicit host fds (duplicates are taken). */
void vprocSessionStdioInitWithFds(VProcSessionStdio *stdio_ctx,
                                  int stdin_fd,
                                  int stdout_fd,
                                  int stderr_fd,
                                  int kernel_pid);
int vprocSessionStdioInitWithPty(VProcSessionStdio *stdio_ctx,
                                 struct pscal_fd *pty_slave,
                                 struct pscal_fd *pty_master,
                                 int bridge_in_fd,
                                 int bridge_out_fd,
                                 uint64_t session_id,
                                 int kernel_pid);
int vprocAdoptPscalStdio(VProc *vp,
                         struct pscal_fd *stdin_fd,
                         struct pscal_fd *stdout_fd,
                         struct pscal_fd *stderr_fd);
int vprocAdoptPscalFd(VProc *vp, int target_fd, struct pscal_fd *pscal_fd);
int vprocSetSessionWinsize(uint64_t session_id, int cols, int rows);
bool vprocSessionStdioNeedsRefresh(VProcSessionStdio *stdio_ctx);
void vprocSessionStdioRefresh(VProcSessionStdio *stdio_ctx, int kernel_pid);
void vprocSessionDebugDumpShim(const char *tag);
bool vprocSessionStdioIsDefault(VProcSessionStdio *stdio_ctx);
/* Activate a session stdio context for the calling thread (per window). */
void vprocSessionStdioActivate(VProcSessionStdio *stdio_ctx);
/* Create/destroy a heap-backed session stdio for custom sessions. */
VProcSessionStdio *vprocSessionStdioCreate(void);
void vprocSessionStdioDestroy(VProcSessionStdio *stdio_ctx);
/* Replace the default (main) session stdio snapshot. */
void vprocSessionStdioSetDefault(VProcSessionStdio *stdio_ctx);
/* Register/unregister per-session PTY output callbacks. */
void vprocSessionSetOutputHandler(uint64_t session_id,
                                  VProcSessionOutputHandler handler,
                                  void *context);
void vprocSessionClearOutputHandler(uint64_t session_id);
/* Write input data directly to a session PTY master. */
ssize_t vprocSessionWriteToMaster(uint64_t session_id, const void *buf, size_t len);
/* Ensure session input is initialized for the current session. */
VProcSessionInput *vprocSessionInputEnsureShim(void);
/* Inject input into the current session input queue. */
bool vprocSessionInjectInputShim(const void *data, size_t len);

/* Create per-session pipes for virtual TTY I/O (session in/out + UI in/out). */
int vprocCreateSessionPipes(int *session_stdin,
                            int *session_stdout,
                            int *ui_read,
                            int *ui_write);

/* Terminate and discard all vprocs in the given session (sid). */
void vprocTerminateSession(int sid);
/* Minimal signal queries/suspension helpers. */
int vprocSigpending(int pid, sigset_t *set);
int vprocSigsuspend(int pid, const sigset_t *mask);
int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset);
int vprocSigwait(int pid, const sigset_t *set, int *sig);
int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig);

/* Optional synthetic metadata: store/retrieve a stable job id for a vproc pid. */
void vprocSetJobId(int pid, int job_id);
int vprocGetJobId(int pid);
void vprocSetCommandLabel(int pid, const char *label);
bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len);
void vprocDiscard(int pid);
bool vprocSigchldPending(int pid);
int vprocSetSigchldBlocked(int pid, bool block);
void vprocClearSigchldPending(int pid);

typedef struct {
    VProc *prev;
    VProc *vp;
    int pid;
} VProcCommandScope;

/* Utility for iOS-hosted tools (smallclue applets, in-process exec, etc):
 * optionally create and activate a child vproc to represent the invoked command,
 * then tear it down while marking it exited. */
bool vprocCommandScopeBegin(VProcCommandScope *scope,
                            const char *label,
                            bool force_new_vproc,
                            bool inherit_parent_pgid);
void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code);

/* Signal API shims: allow vproc_shim.h to virtualize signal dispositions when
 * a vproc is active on the current thread. */
typedef void (*VProcSigHandler)(int);
int vprocSigactionShim(int sig, const struct sigaction *act, struct sigaction *oldact);
int vprocSigprocmaskShim(int how, const sigset_t *set, sigset_t *oldset);
int vprocSigpendingShim(sigset_t *set);
int vprocSigsuspendShim(const sigset_t *mask);
int vprocPthreadSigmaskShim(int how, const sigset_t *set, sigset_t *oldset);
int vprocRaiseShim(int sig);
VProcSigHandler vprocSignalShim(int sig, VProcSigHandler handler);
/* Indicates when it is safe for interposed libc calls to enter vproc shims. */
int vprocInterposeReady(void);
/* True when the thread has been registered as part of a vproc task. */
int vprocThreadIsRegistered(pthread_t tid);
/* Nonblocking variant for interposer gates (returns 0 if registry is busy). */
int vprocThreadIsRegisteredNonblocking(pthread_t tid);
/* True when the calling thread has an active vproc context (TLS). */
int vprocThreadHasActiveVproc(void);
/* True when the thread should bypass interposed libc calls entirely. */
int vprocThreadIsInterposeBypassed(pthread_t tid);
void vprocRegisterInterposeBypassThread(pthread_t tid);
void vprocUnregisterInterposeBypassThread(pthread_t tid);
/* Kernel-managed path truncation (present sandbox as "/"). */
void vprocApplyPathTruncation(const char *prefix);
/* Temporarily disable interposed libc calls while performing host syscalls. */
void vprocInterposeBypassEnter(void);
void vprocInterposeBypassExit(void);
int vprocInterposeBypassActive(void);

static inline void vprocFormatCpuTimes(int utime_cs, int stime_cs, double *utime_s, double *stime_s) {
    if (utime_s) {
        *utime_s = (double)utime_cs / 100.0;
    }
    if (stime_s) {
        *stime_s = (double)stime_cs / 100.0;
    }
}

#else /* desktop stubs */
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <errno.h>
typedef struct VProcSessionStdio {
    int dummy;
} VProcSessionStdio;
/* Desktop stubs: map to host syscalls or no-ops so non-iOS builds compile. */
static inline VProcOptions vprocDefaultOptions(void) {
    VProcOptions o = {.stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .winsize_cols = 80, .winsize_rows = 24, .pid_hint = -1, .job_id = 0};
    return o;
}
static inline VProc *vprocCreate(const VProcOptions *opts) { (void)opts; return NULL; }
static inline void vprocDestroy(VProc *vp) { (void)vp; }
static inline int vprocPid(VProc *vp) { (void)vp; return -1; }
static inline int vprocReservePid(void) { return (int)getpid(); }
static inline void vprocActivate(VProc *vp) { (void)vp; }
static inline void vprocDeactivate(void) {}
static inline VProc *vprocCurrent(void) { return NULL; }
static inline bool vprocWaitIfStopped(VProc *vp) { (void)vp; return false; }
static inline size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) { (void)out; (void)capacity; return 0; }
static inline int vprocTranslateFd(VProc *vp, int fd) { (void)vp; (void)fd; return -1; }
static inline struct pscal_fd *vprocGetPscalFd(VProc *vp, int fd) { (void)vp; (void)fd; return NULL; }
static inline int vprocDup(VProc *vp, int fd) { (void)vp; (void)fd; return -1; }
static inline int vprocDup2(VProc *vp, int fd, int target) { (void)vp; (void)fd; (void)target; return -1; }
static inline int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src) {
    (void)vp;
    (void)target_fd;
    (void)host_src;
    return 0;
}
static inline int vprocClose(VProc *vp, int fd) { (void)vp; (void)fd; return close(fd); }
static inline int vprocPipe(VProc *vp, int pipefd[2]) { (void)vp; return pipe(pipefd); }
static inline int vprocHostDup2(int host_fd, int target_fd) { return dup2(host_fd, target_fd); }
static inline int vprocHostDup(int fd) { return dup(fd); }
static inline int vprocHostOpen(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    return open(path, flags, mode);
}
static inline int vprocHostPipe(int pipefd[2]) { return pipe(pipefd); }
static inline off_t vprocHostLseek(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
static inline int vprocHostFsync(int fd) { return fsync(fd); }
static inline int vprocHostClose(int fd) { return close(fd); }
static inline ssize_t vprocHostRead(int fd, void *buf, size_t count) { return read(fd, buf, count); }
static inline ssize_t vprocHostWrite(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
static inline void vprocLocationDeviceSetEnabled(bool enabled) { (void)enabled; }
static inline ssize_t vprocLocationDeviceWrite(const void *data, size_t len) {
    (void)data;
    (void)len;
    errno = ENODEV;
    return -1;
}
static inline int vprocIoctlShim(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    uintptr_t arg = va_arg(ap, uintptr_t);
    va_end(ap);
    return ioctl(fd, request, arg);
}
static inline int vprocHostPthreadCreate(pthread_t *thread,
                                         const pthread_attr_t *attr,
                                         void *(*start_routine)(void *),
                                         void *arg) {
    return pthread_create(thread, attr, start_routine, arg);
}
static inline int vprocAdoptHostFd(VProc *vp, int host_fd) { (void)vp; return host_fd; }
static inline int vprocPthreadCreateShim(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) { return pthread_create(t, a, fn, arg); }
static inline int vprocOpenAt(VProc *vp, const char *path, int flags, int mode) { (void)vp; return open(path, flags, mode); }
static inline int vprocSetWinsize(VProc *vp, int cols, int rows) { (void)vp; (void)cols; (void)rows; return 0; }
static inline int vprocGetWinsize(VProc *vp, VProcWinsize *out) { if (out) { out->cols = 80; out->rows = 24; } return 0; }
static inline int vprocRegisterThread(VProc *vp, pthread_t tid) { (void)vp; (void)tid; return 0; }
static inline void vprocMarkExit(VProc *vp, int status) { (void)vp; (void)status; }
static inline void vprocSetParent(int pid, int parent_pid) { (void)pid; (void)parent_pid; }
static inline int vprocSetPgid(int pid, int pgid) { (void)pid; (void)pgid; return 0; }
static inline int vprocSetSid(int pid, int sid) { (void)pid; (void)sid; return 0; }
static inline int vprocGetPgid(int pid) { (void)pid; return -1; }
static inline int vprocGetSid(int pid) { (void)pid; return -1; }
static inline int vprocSetForegroundPgid(int sid, int fg) { (void)sid; (void)fg; return 0; }
static inline int vprocGetForegroundPgid(int sid) { (void)sid; return -1; }
static inline void vprocMarkGroupExit(int pid, int status) { (void)pid; (void)status; }
static inline void vprocSetRusage(int pid, int utime, int stime) { (void)pid; (void)utime; (void)stime; }
static inline int vprocBlockSignals(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocUnblockSignals(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocIgnoreSignal(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocDefaultSignal(int pid, int mask) { (void)pid; (void)mask; return 0; }
static inline int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old) {
    (void)pid; (void)sig; (void)act; (void)old; return sigaction(sig, act, old);
}
static inline ssize_t vprocReadShim(int fd, void *buf, size_t count) { return read(fd, buf, count); }
static inline ssize_t vprocWriteShim(int fd, const void *buf, size_t count) { return write(fd, buf, count); }
static inline int vprocDupShim(int fd) { return dup(fd); }
static inline int vprocDup2Shim(int fd, int target) { return dup2(fd, target); }
static inline int vprocCloseShim(int fd) { return close(fd); }
static inline int vprocFsyncShim(int fd) { return fsync(fd); }
static inline int vprocPipeShim(int pipefd[2]) { return pipe(pipefd); }
static inline int vprocFstatShim(int fd, struct stat *st) { return fstat(fd, st); }
static inline int vprocStatShim(const char *path, struct stat *st) { return stat(path, st); }
static inline int vprocLstatShim(const char *path, struct stat *st) { return lstat(path, st); }
static inline int vprocChdirShim(const char *path) { return chdir(path); }
static inline char *vprocGetcwdShim(char *buffer, size_t size) { return getcwd(buffer, size); }
static inline int vprocAccessShim(const char *path, int mode) { return access(path, mode); }
static inline int vprocMkdirShim(const char *path, mode_t mode) { return mkdir(path, mode); }
static inline int vprocRmdirShim(const char *path) { return rmdir(path); }
static inline int vprocUnlinkShim(const char *path) { return unlink(path); }
static inline int vprocRemoveShim(const char *path) { return remove(path); }
static inline int vprocRenameShim(const char *oldpath, const char *newpath) { return rename(oldpath, newpath); }
static inline DIR *vprocOpendirShim(const char *name) { return opendir(name); }
static inline int vprocSymlinkShim(const char *target, const char *linkpath) { return symlink(target, linkpath); }
static inline ssize_t vprocReadlinkShim(const char *path, char *buf, size_t size) { return readlink(path, buf, size); }
static inline char *vprocRealpathShim(const char *path, char *resolved_path) { return realpath(path, resolved_path); }
static inline off_t vprocLseekShim(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
static inline int vprocIsattyShim(int fd) { return isatty(fd); }
static inline int vprocOpenShim(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap);
        return open(path, flags, mode);
    }
    return open(path, flags);
}
static inline pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options) { return waitpid(pid, status_out, options); }
static inline int vprocKillShim(pid_t pid, int sig) { return kill(pid, sig); }
static inline pid_t vprocGetPidShim(void) { return getpid(); }
static inline pid_t vprocGetPpidShim(void) { return getppid(); }
static inline pid_t vprocGetpgrpShim(void) { return getpgrp(); }
static inline pid_t vprocGetpgidShim(pid_t pid) { return getpgid(pid); }
static inline int vprocSetpgidShim(pid_t pid, pid_t pgid) { return setpgid(pid, pgid); }
static inline pid_t vprocSetsidShim(void) { return setsid(); }
static inline pid_t vprocGetsidShim(pid_t pid) { return getsid(pid); }
static inline pid_t vprocTcgetpgrpShim(int fd) { return tcgetpgrp(fd); }
static inline int vprocTcsetpgrpShim(int fd, pid_t pgid) { return tcsetpgrp(fd, pgid); }
static inline int vprocPollShim(struct pollfd *fds, nfds_t nfds, int timeout) { return poll(fds, nfds, timeout); }
static inline int vprocSelectShim(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    return select(nfds, readfds, writefds, exceptfds, timeout);
}
static inline void vprocSetShellSelfPid(int pid) { (void)pid; }
static inline int vprocGetShellSelfPid(void) { return (int)getpid(); }
static inline void vprocSetShellSelfTid(pthread_t tid) { (void)tid; }
static inline bool vprocIsShellSelfThread(void) { return false; }
static inline int vprocInterposeReady(void) { return 0; }
static inline int vprocThreadIsRegistered(pthread_t tid) { (void)tid; return 0; }
static inline int vprocThreadIsRegisteredNonblocking(pthread_t tid) { (void)tid; return 0; }
static inline int vprocThreadHasActiveVproc(void) { return 0; }
static inline int vprocThreadIsInterposeBypassed(pthread_t tid) { (void)tid; return 0; }
static inline void vprocRegisterInterposeBypassThread(pthread_t tid) { (void)tid; }
static inline void vprocUnregisterInterposeBypassThread(pthread_t tid) { (void)tid; }
static inline void vprocApplyPathTruncation(const char *prefix) { (void)prefix; }
static inline void vprocInterposeBypassEnter(void) {}
static inline void vprocInterposeBypassExit(void) {}
static inline int vprocInterposeBypassActive(void) { return 0; }
static inline void vprocSetKernelPid(int pid) { (void)pid; }
static inline int vprocGetKernelPid(void) { return 0; }
static inline void vprocClearKernelPidGlobal(void) {}
static inline bool vprocSessionStdioIsDefault(VProcSessionStdio *stdio_ctx) { (void)stdio_ctx; return true; }
static inline int vprocSessionStdioInitWithPty(VProcSessionStdio *stdio_ctx,
                                               struct pscal_fd *pty_slave,
                                               struct pscal_fd *pty_master,
                                               int bridge_in_fd,
                                               int bridge_out_fd,
                                               uint64_t session_id,
                                               int kernel_pid) {
    (void)stdio_ctx;
    (void)pty_slave;
    (void)pty_master;
    (void)bridge_in_fd;
    (void)bridge_out_fd;
    (void)session_id;
    (void)kernel_pid;
    errno = ENOTSUP;
    return -1;
}
static inline int vprocAdoptPscalStdio(VProc *vp,
                                       struct pscal_fd *stdin_fd,
                                       struct pscal_fd *stdout_fd,
                                       struct pscal_fd *stderr_fd) {
    (void)vp;
    (void)stdin_fd;
    (void)stdout_fd;
    (void)stderr_fd;
    return 0;
}
static inline int vprocAdoptPscalFd(VProc *vp, int target_fd, struct pscal_fd *pscal_fd) {
    (void)vp;
    (void)target_fd;
    (void)pscal_fd;
    return 0;
}
static inline int vprocSetSessionWinsize(uint64_t session_id, int cols, int rows) {
    (void)session_id;
    (void)cols;
    (void)rows;
    errno = ENOTSUP;
    return -1;
}
static inline int vprocSigpending(int pid, sigset_t *set) { (void)pid; return sigpending(set); }
static inline int vprocSigsuspend(int pid, const sigset_t *mask) { (void)pid; return sigsuspend(mask); }
static inline int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset) {
    (void)pid;
    return sigprocmask(how, set, oldset);
}
static inline int vprocSigwait(int pid, const sigset_t *set, int *sig) { (void)pid; return sigwait(set, sig); }
static inline int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig) {
    (void)pid;
#if defined(__APPLE__)
    (void)set;
    (void)timeout;
    (void)sig;
    errno = ENOSYS;
    return -1;
#else
    return sigtimedwait(set, sig, timeout);
#endif
}
static inline void vprocSetJobId(int pid, int job_id) { (void)pid; (void)job_id; }
static inline int vprocGetJobId(int pid) { (void)pid; return 0; }
static inline void vprocSetCommandLabel(int pid, const char *label) { (void)pid; (void)label; }
static inline bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len) { (void)pid; (void)buf; (void)buf_len; return false; }
static inline void vprocDiscard(int pid) { (void)pid; }
static inline bool vprocSigchldPending(int pid) { (void)pid; return false; }
typedef struct {
    VProc *prev;
    VProc *vp;
    int pid;
} VProcCommandScope;
static inline bool vprocCommandScopeBegin(VProcCommandScope *scope,
                                         const char *label,
                                         bool force_new_vproc,
                                         bool inherit_parent_pgid) {
    (void)scope;
    (void)label;
    (void)force_new_vproc;
    (void)inherit_parent_pgid;
    return false;
}
static inline void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code) {
    (void)scope;
    (void)exit_code;
}
static inline int vprocSetSigchldBlocked(int pid, bool block) { (void)pid; (void)block; return 0; }
static inline void vprocClearSigchldPending(int pid) { (void)pid; }
static inline void vprocFormatCpuTimes(int utime_cs, int stime_cs, double *utime_s, double *stime_s) {
    if (utime_s) {
        *utime_s = (double)utime_cs / 100.0;
    }
    if (stime_s) {
        *stime_s = (double)stime_cs / 100.0;
    }
}
#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */

#ifdef __cplusplus
}
#endif
