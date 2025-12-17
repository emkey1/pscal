#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

// Clean-room virtual process abstraction for iOS builds. Maintains a private
// fd table per task and translates basic I/O syscalls through the table.

typedef struct VProc VProc;

typedef struct {
    int stdin_fd;   // host fd to dup for stdin; -1 to dup host STDIN_FILENO; -2 for /dev/null
    int stdout_fd;  // host fd to dup for stdout; -1 to dup host STDOUT_FILENO
    int stderr_fd;  // host fd to dup for stderr; -1 to dup host STDERR_FILENO
    int winsize_cols;
    int winsize_rows;
    int pid_hint;   // optional fixed synthetic pid; -1 for auto
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

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity);

int vprocTranslateFd(VProc *vp, int fd);
int vprocDup(VProc *vp, int fd);
int vprocDup2(VProc *vp, int fd, int target);
int vprocClose(VProc *vp, int fd);
int vprocPipe(VProc *vp, int pipefd[2]);
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
int vprocRegisterThread(VProc *vp, pthread_t tid);
void vprocMarkExit(VProc *vp, int status);
void vprocSetParent(int pid, int parent_pid);
int vprocSetPgid(int pid, int pgid);
int vprocSetSid(int pid, int sid);
int vprocGetPgid(int pid);
int vprocGetSid(int pid);
int vprocSetForegroundPgid(int sid, int fg_pgid);
int vprocGetForegroundPgid(int sid);
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
int vprocPipeShim(int pipefd[2]);
int vprocFstatShim(int fd, struct stat *st);
off_t vprocLseekShim(int fd, off_t offset, int whence);
int vprocOpenShim(const char *path, int flags, ...);
pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options);
int vprocKillShim(pid_t pid, int sig);
pid_t vprocGetPidShim(void);
void vprocSetShellSelfPid(int pid);
int vprocGetShellSelfPid(void);
/* Minimal signal queries/suspension helpers. */
int vprocSigpending(int pid, sigset_t *set);
int vprocSigsuspend(int pid, const sigset_t *mask);
int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset);
int vprocSigwait(int pid, const sigset_t *set, int *sig);

/* Optional synthetic metadata: store/retrieve a stable job id for a vproc pid. */
void vprocSetJobId(int pid, int job_id);
int vprocGetJobId(int pid);
void vprocSetCommandLabel(int pid, const char *label);
bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len);
void vprocDiscard(int pid);
bool vprocSigchldPending(int pid);
int vprocSetSigchldBlocked(int pid, bool block);
void vprocClearSigchldPending(int pid);

#else /* desktop stubs */
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdarg.h>
#include <signal.h>
/* Desktop stubs: map to host syscalls or no-ops so non-iOS builds compile. */
static inline VProcOptions vprocDefaultOptions(void) {
    VProcOptions o = {.stdin_fd = -1, .stdout_fd = -1, .stderr_fd = -1, .winsize_cols = 80, .winsize_rows = 24, .pid_hint = -1};
    return o;
}
static inline VProc *vprocCreate(const VProcOptions *opts) { (void)opts; return NULL; }
static inline void vprocDestroy(VProc *vp) { (void)vp; }
static inline int vprocPid(VProc *vp) { (void)vp; return -1; }
static inline int vprocReservePid(void) { return (int)getpid(); }
static inline void vprocActivate(VProc *vp) { (void)vp; }
static inline void vprocDeactivate(void) {}
static inline VProc *vprocCurrent(void) { return NULL; }
static inline size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) { (void)out; (void)capacity; return 0; }
static inline int vprocTranslateFd(VProc *vp, int fd) { (void)vp; (void)fd; return -1; }
static inline int vprocDup(VProc *vp, int fd) { (void)vp; (void)fd; return -1; }
static inline int vprocDup2(VProc *vp, int fd, int target) { (void)vp; (void)fd; (void)target; return -1; }
static inline int vprocClose(VProc *vp, int fd) { (void)vp; (void)fd; return close(fd); }
static inline int vprocPipe(VProc *vp, int pipefd[2]) { (void)vp; return pipe(pipefd); }
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
static inline int vprocPipeShim(int pipefd[2]) { return pipe(pipefd); }
static inline int vprocFstatShim(int fd, struct stat *st) { return fstat(fd, st); }
static inline off_t vprocLseekShim(int fd, off_t offset, int whence) { return lseek(fd, offset, whence); }
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
static inline void vprocSetShellSelfPid(int pid) { (void)pid; }
static inline int vprocGetShellSelfPid(void) { return (int)getpid(); }
static inline int vprocSigpending(int pid, sigset_t *set) { (void)pid; return sigpending(set); }
static inline int vprocSigsuspend(int pid, const sigset_t *mask) { (void)pid; return sigsuspend(mask); }
static inline int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset) {
    (void)pid;
    return sigprocmask(how, set, oldset);
}
static inline int vprocSigwait(int pid, const sigset_t *set, int *sig) { (void)pid; return sigwait(set, sig); }
static inline void vprocSetJobId(int pid, int job_id) { (void)pid; (void)job_id; }
static inline int vprocGetJobId(int pid) { (void)pid; return 0; }
static inline void vprocSetCommandLabel(int pid, const char *label) { (void)pid; (void)label; }
static inline bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len) { (void)pid; (void)buf; (void)buf_len; return false; }
static inline void vprocDiscard(int pid) { (void)pid; }
static inline bool vprocSigchldPending(int pid) { (void)pid; return false; }
static inline int vprocSetSigchldBlocked(int pid, bool block) { (void)pid; (void)block; return 0; }
static inline void vprocClearSigchldPending(int pid) { (void)pid; }
#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */

#ifdef __cplusplus
}
#endif
