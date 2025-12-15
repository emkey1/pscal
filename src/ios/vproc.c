#include "ios/vproc.h"

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#if defined(PSCAL_TARGET_IOS)
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#undef PATH_VIRTUALIZATION_NO_MACROS
#endif

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
static int vprocHostOpenVirtualized(const char *path, int flags, int mode) {
    if (flags & O_CREAT) {
        return open(path, flags, mode);
    }
    return open(path, flags);
}
#elif defined(PSCAL_TARGET_IOS)
#define vprocHostOpenVirtualized(path, flags, mode) pscalPathVirtualized_open((path), (flags), (mode))
#else
#define vprocHostOpenVirtualized(path, flags, mode) open((path), (flags), (mode))
#endif

#ifdef waitpid
#undef waitpid
#endif
#ifdef kill
#undef kill
#endif
#ifdef getpid
#undef getpid
#endif

#ifndef VPROC_INITIAL_CAPACITY
#define VPROC_INITIAL_CAPACITY 16
#endif

typedef struct {
    int host_fd;
} VProcFdEntry;

struct VProc {
    VProcFdEntry *entries;
    size_t capacity;
    int next_fd;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    VProcWinsize winsize;
    int pid;
};

static __thread VProc *gVProcCurrent = NULL;
static int gNextSyntheticPid = 1000;
static int gShellSelfPid = 0;

typedef struct {
    int pid;
    pthread_t tid;
    int status;
    bool exited;
    bool stopped;
    int stop_signo;
    int job_id;
    char *label;
} VProcTaskEntry;

typedef struct {
    VProcTaskEntry *items;
    size_t count;
    size_t capacity;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} VProcTaskTable;

static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid);

static VProcTaskTable gVProcTasks = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

static int vprocAllocSlot(VProc *vp) {
    if (!vp) {
        return -1;
    }
    for (int i = 0; i < (int)vp->capacity; ++i) {
        int idx = (vp->next_fd + i) % (int)vp->capacity;
        if (vp->entries[idx].host_fd < 0) {
            vp->next_fd = idx + 1;
            return idx;
        }
    }
    size_t new_cap = vp->capacity ? vp->capacity * 2 : VPROC_INITIAL_CAPACITY;
    VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
    if (!resized) {
        return -1;
    }
    for (size_t i = vp->capacity; i < new_cap; ++i) {
        resized[i].host_fd = -1;
    }
    int idx = (int)vp->capacity;
    vp->entries = resized;
    vp->capacity = new_cap;
    vp->next_fd = idx + 1;
    return idx;
}

static int vprocInsert(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        errno = EBADF;
        return -1;
    }
    int slot = vprocAllocSlot(vp);
    if (slot < 0) {
        return -1;
    }
    vp->entries[slot].host_fd = host_fd;
    return slot;
}

static int vprocCloneFd(int source_fd) {
    int duped = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0 && errno == EINVAL) {
        duped = fcntl(source_fd, F_DUPFD, 0);
        if (duped >= 0) {
            fcntl(duped, F_SETFD, FD_CLOEXEC);
        }
    }
    return duped;
}

int vprocReservePid(void) {
    int pid = __sync_fetch_and_add(&gNextSyntheticPid, 1);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        entry->pid = pid;
        entry->tid = 0;
        entry->status = 0;
        entry->exited = false;
        entry->stopped = false;
        entry->stop_signo = 0;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pid;
}

static void vprocMaybeAdvancePidCounter(int pid_hint) {
    if (pid_hint <= 0) {
        return;
    }
    while (true) {
        int current = gNextSyntheticPid;
        if (pid_hint < current) {
            break;
        }
        if (__sync_bool_compare_and_swap(&gNextSyntheticPid, current, pid_hint + 1)) {
            break;
        }
    }
}

static VProcTaskEntry *vprocTaskFindLocked(int pid) {
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        if (gVProcTasks.items[i].pid == pid) {
            return &gVProcTasks.items[i];
        }
    }
    return NULL;
}

static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        return entry;
    }
    if (gVProcTasks.count >= gVProcTasks.capacity) {
        size_t new_cap = gVProcTasks.capacity ? (gVProcTasks.capacity * 2) : 8;
        VProcTaskEntry *resized = realloc(gVProcTasks.items, new_cap * sizeof(VProcTaskEntry));
        if (!resized) {
            return NULL;
        }
        gVProcTasks.items = resized;
        gVProcTasks.capacity = new_cap;
    }
    entry = &gVProcTasks.items[gVProcTasks.count++];
    memset(entry, 0, sizeof(*entry));
    entry->pid = pid;
    entry->status = 0;
    entry->exited = false;
    entry->stopped = false;
    entry->stop_signo = 0;
    entry->job_id = 0;
    entry->label = NULL;
    return entry;
}

VProcOptions vprocDefaultOptions(void) {
    VProcOptions opts;
    opts.stdin_fd = -1;
    opts.stdout_fd = -1;
    opts.stderr_fd = -1;
    opts.winsize_cols = 80;
    opts.winsize_rows = 24;
    opts.pid_hint = -1;
    return opts;
}

VProc *vprocCreate(const VProcOptions *opts) {
    VProcOptions local = opts ? *opts : vprocDefaultOptions();
    VProc *vp = calloc(1, sizeof(VProc));
    if (!vp) {
        return NULL;
    }
    vp->capacity = VPROC_INITIAL_CAPACITY;
    vp->entries = calloc(vp->capacity, sizeof(VProcFdEntry));
    if (!vp->entries) {
        free(vp);
        return NULL;
    }
    for (size_t i = 0; i < vp->capacity; ++i) {
        vp->entries[i].host_fd = -1;
    }
    vp->next_fd = 3;
    vp->winsize.cols = (local.winsize_cols > 0) ? local.winsize_cols : 80;
    vp->winsize.rows = (local.winsize_rows > 0) ? local.winsize_rows : 24;
    if (local.pid_hint > 0) {
        vprocMaybeAdvancePidCounter(local.pid_hint);
        vp->pid = local.pid_hint;
    } else {
        vp->pid = __sync_fetch_and_add(&gNextSyntheticPid, 1);
    }
    /* Ensure a task slot exists for synthetic pid bookkeeping. */
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *slot = vprocTaskEnsureSlotLocked(vp->pid);
    if (slot) {
        slot->pid = vp->pid;
        slot->tid = 0;
        slot->status = 0;
        slot->exited = false;
        slot->stopped = false;
        slot->stop_signo = 0;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);

    int stdin_src = (local.stdin_fd == -2) ? open("/dev/null", O_RDONLY)
                                           : vprocCloneFd((local.stdin_fd >= 0) ? local.stdin_fd : STDIN_FILENO);
    int stdout_src = vprocCloneFd((local.stdout_fd >= 0) ? local.stdout_fd : STDOUT_FILENO);
    int stderr_src = vprocCloneFd((local.stderr_fd >= 0) ? local.stderr_fd : STDERR_FILENO);

    if (stdin_src < 0 || stdout_src < 0 || stderr_src < 0) {
        if (stdin_src >= 0) close(stdin_src);
        if (stdout_src >= 0) close(stdout_src);
        if (stderr_src >= 0) close(stderr_src);
        vprocDestroy(vp);
        return NULL;
    }
    vp->entries[0].host_fd = stdin_src;
    vp->entries[1].host_fd = stdout_src;
    vp->entries[2].host_fd = stderr_src;
    vp->stdin_fd = 0;
    vp->stdout_fd = 1;
    vp->stderr_fd = 2;
    return vp;
}

void vprocDestroy(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vp->pid);
    if (entry && entry->label) {
        free(entry->label);
        entry->label = NULL;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    for (size_t i = 0; i < vp->capacity; ++i) {
        if (vp->entries[i].host_fd >= 0) {
            close(vp->entries[i].host_fd);
            vp->entries[i].host_fd = -1;
        }
    }
    free(vp->entries);
    vp->entries = NULL;
    free(vp);
}

void vprocActivate(VProc *vp) {
    gVProcCurrent = vp;
}

void vprocDeactivate(void) {
    gVProcCurrent = NULL;
}

VProc *vprocCurrent(void) {
    return gVProcCurrent;
}

int vprocTranslateFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    if ((size_t)fd >= vp->capacity) {
        errno = EBADF;
        return -1;
    }
    int host = vp->entries[fd].host_fd;
    if (host < 0) {
        errno = EBADF;
        return -1;
    }
    return host;
}

int vprocDup(VProc *vp, int fd) {
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        return -1;
    }
    return vprocInsert(vp, cloned);
}

int vprocDup2(VProc *vp, int fd, int target) {
    if (!vp || target < 0) {
        errno = EBADF;
        return -1;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    if ((size_t)target >= vp->capacity) {
        size_t new_cap = vp->capacity;
        while ((size_t)target >= new_cap) {
            new_cap *= 2;
        }
        VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
        if (!resized) {
            return -1;
        }
        for (size_t i = vp->capacity; i < new_cap; ++i) {
            resized[i].host_fd = -1;
        }
        vp->entries = resized;
        vp->capacity = new_cap;
    }
    if (vp->entries[target].host_fd >= 0) {
        close(vp->entries[target].host_fd);
        vp->entries[target].host_fd = -1;
    }
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        return -1;
    }
    vp->entries[target].host_fd = cloned;
    return target;
}

int vprocClose(VProc *vp, int fd) {
    if (!vp || fd < 0 || (size_t)fd >= vp->capacity) {
        errno = EBADF;
        return -1;
    }
    int host = vp->entries[fd].host_fd;
    if (host < 0) {
        errno = EBADF;
        return -1;
    }
    vp->entries[fd].host_fd = -1;
    return close(host);
}

int vprocPipe(VProc *vp, int pipefd[2]) {
    if (!vp || !pipefd) {
        errno = EINVAL;
        return -1;
    }
    int raw[2];
    if (pipe(raw) != 0) {
        return -1;
    }
    int left = vprocInsert(vp, raw[0]);
    int right = vprocInsert(vp, raw[1]);
    if (left < 0 || right < 0) {
        if (left >= 0) vprocClose(vp, left);
        if (right >= 0) vprocClose(vp, right);
        return -1;
    }
    pipefd[0] = left;
    pipefd[1] = right;
    return 0;
}

int vprocOpenAt(VProc *vp, const char *path, int flags, int mode) {
    if (!vp || !path) {
        errno = EINVAL;
        return -1;
    }
    int host_fd = open(path, flags, mode);
    if (host_fd < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        close(host_fd);
    }
    return slot;
}

int vprocSetWinsize(VProc *vp, int cols, int rows) {
    if (!vp) {
        errno = EINVAL;
        return -1;
    }
    if (cols > 0) vp->winsize.cols = cols;
    if (rows > 0) vp->winsize.rows = rows;
    return 0;
}

int vprocGetWinsize(VProc *vp, VProcWinsize *out) {
    if (!vp || !out) {
        errno = EINVAL;
        return -1;
    }
    *out = vp->winsize;
    return 0;
}

int vprocPid(VProc *vp) {
    return vp ? vp->pid : -1;
}

int vprocRegisterThread(VProc *vp, pthread_t tid) {
    if (!vp || vp->pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(vp->pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        return -1;
    }
    entry->tid = tid;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return vp->pid;
}

void vprocMarkExit(VProc *vp, int status) {
    if (!vp || vp->pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vp->pid);
    if (entry) {
        entry->status = status;
        entry->exited = true;
        entry->stopped = false;
        entry->stop_signo = 0;
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) {
    size_t count = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) {
            continue;
        }
        if (out && count < capacity) {
            out[count].pid = entry->pid;
            out[count].tid = entry->tid;
            out[count].exited = entry->exited;
            out[count].stopped = entry->stopped;
            out[count].status = entry->status;
            out[count].stop_signo = entry->stop_signo;
            if (entry->label) {
                strncpy(out[count].command, entry->label, sizeof(out[count].command) - 1);
                out[count].command[sizeof(out[count].command) - 1] = '\0';
            } else {
                out[count].command[0] = '\0';
            }
        }
        ++count;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return count;
}

pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options) {
    bool allow_stop = (options & WUNTRACED) != 0;
    bool nohang = (options & WNOHANG) != 0;
    if (pid <= 0) {
        return waitpid(pid, status_out, options);
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked((int)pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return waitpid(pid, status_out, options);
    }
    while (!entry->exited && !(allow_stop && entry->stopped)) {
        if (nohang) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            if (status_out) {
                *status_out = 0;
            }
            return 0;
        }
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }

    int status = 0;
    if (entry->exited) {
        status = W_EXITCODE(entry->status & 0xff, 0);
    } else if (entry->stopped && entry->stop_signo > 0) {
        status = W_STOPCODE(entry->stop_signo & 0xff);
    }
    if (status_out) {
        *status_out = status;
    }

    /* Only clear tracking on exit; stopped tasks remain in the table so they
     * can be continued and waited on again. */
    if (entry->exited) {
        if (entry->label) {
            free(entry->label);
            entry->label = NULL;
        }
        entry->pid = 0;
        entry->tid = 0;
        entry->exited = true;
        entry->stopped = false;
        entry->stop_signo = 0;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pid;
}

int vprocKillShim(pid_t pid, int sig) {
    if (pid == 0) {
        return kill(pid, sig);
    }
    int target = (pid < 0) ? -pid : pid;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(target);
    if (!entry || entry->pid <= 0) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return kill(pid, sig);
    }
    bool stop_signal = (sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU);
    bool cont_signal = (sig == SIGCONT);

    if (entry->tid && !stop_signal && !cont_signal) {
        /* Try to cancel the target thread to stop blocking syscalls (sleep, etc.). */
        pthread_kill(entry->tid, sig);
        pthread_cancel(entry->tid);
    } else if (entry->tid && stop_signal) {
        pthread_kill(entry->tid, sig);
    }

    if (stop_signal) {
        entry->stopped = true;
        entry->exited = false;
        entry->stop_signo = sig;
        entry->status = 128 + sig;
    } else if (cont_signal) {
        entry->stopped = false;
        entry->stop_signo = 0;
    } else {
        /* Simulate signal delivery for synthetic tasks: mark exit immediately and
         * wake any waiters. This avoids terminating the entire process when using
         * pthread_kill with SIGTERM/SIGKILL semantics on a shared process. */
        entry->status = 128 + sig;
        entry->exited = true;
        entry->stopped = false;
        entry->stop_signo = 0;
    }
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

pid_t vprocGetPidShim(void) {
    VProc *vp = vprocCurrent();
    if (vp) {
        return vprocPid(vp);
    }
    return getpid();
}

void vprocSetShellSelfPid(int pid) {
    gShellSelfPid = pid;
}

int vprocGetShellSelfPid(void) {
    return gShellSelfPid;
}

void vprocSetJobId(int pid, int job_id) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->job_id = job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocGetJobId(int pid) {
    int id = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        id = entry->job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return id;
}

void vprocSetCommandLabel(int pid, const char *label) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        free(entry->label);
        entry->label = NULL;
        if (label && *label) {
            entry->label = strdup(label);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return false;
    }
    bool ok = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->label && entry->label[0]) {
        strncpy(buf, entry->label, buf_len - 1);
        buf[buf_len - 1] = '\0';
        ok = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return ok;
}

static VProc *vprocForThread(void) {
    return gVProcCurrent;
}

static int shimTranslate(int fd, int allow_real) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return allow_real ? fd : -1;
    }
    int host = vprocTranslateFd(vp, fd);
    return host;
}

ssize_t vprocReadShim(int fd, void *buf, size_t count) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return read(host, buf, count);
}

ssize_t vprocWriteShim(int fd, const void *buf, size_t count) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return write(host, buf, count);
}

int vprocDupShim(int fd) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return dup(fd);
    }
    int host_fd = shimTranslate(fd, 0);
    if (host_fd < 0) {
        return -1;
    }
    return vprocInsert(vp, vprocCloneFd(host_fd));
}

int vprocDup2Shim(int fd, int target) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return dup2(fd, target);
    }
    return vprocDup2(vp, fd, target);
}

int vprocCloseShim(int fd) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return close(fd);
    }
    return vprocClose(vp, fd);
}

int vprocPipeShim(int pipefd[2]) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return pipe(pipefd);
    }
    return vprocPipe(vp, pipefd);
}

int vprocFstatShim(int fd, struct stat *st) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return fstat(host, st);
}

off_t vprocLseekShim(int fd, off_t offset, int whence) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return lseek(host, offset, whence);
}

int vprocOpenShim(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    VProc *vp = vprocForThread();
    if (!vp) {
        /* Apply path virtualization even when vproc is inactive. */
        return vprocHostOpenVirtualized(path, flags, mode);
    }
    int host_fd = vprocHostOpenVirtualized(path, flags, mode);
    if (host_fd < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        close(host_fd);
    }
    return slot;
}

#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */
