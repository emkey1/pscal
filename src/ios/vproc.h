#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>

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

VProcOptions vprocDefaultOptions(void);
VProc *vprocCreate(const VProcOptions *opts);
void vprocDestroy(VProc *vp);
int vprocPid(VProc *vp);
int vprocReservePid(void);

void vprocActivate(VProc *vp);
void vprocDeactivate(void);
VProc *vprocCurrent(void);

typedef struct {
    int pid;
    pthread_t tid;
    bool exited;
    bool stopped;
    int status;
    int stop_signo;
    char command[64];
} VProcSnapshot;

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity);

int vprocTranslateFd(VProc *vp, int fd);
int vprocDup(VProc *vp, int fd);
int vprocDup2(VProc *vp, int fd, int target);
int vprocClose(VProc *vp, int fd);
int vprocPipe(VProc *vp, int pipefd[2]);
/* Adopt an existing host fd into the vproc table, returning the vproc-local fd. */
int vprocAdoptHostFd(VProc *vp, int host_fd);
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

/* Optional synthetic metadata: store/retrieve a stable job id for a vproc pid. */
void vprocSetJobId(int pid, int job_id);
int vprocGetJobId(int pid);
void vprocSetCommandLabel(int pid, const char *label);
bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len);

#ifdef __cplusplus
}
#endif
