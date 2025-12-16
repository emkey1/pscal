#include "ios/vproc.h"

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h> // Added for fprintf
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

/* -- Compatibility Macros -- */
#ifndef W_EXITCODE
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#endif

#ifndef W_STOPCODE
#define W_STOPCODE(sig) ((sig) << 8 | 0x7f)
#endif

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
static int vprocHostOpenVirtualized(const char *path, int flags, int mode) {
    if (flags & O_CREAT) {
        return pscalPathVirtualized_open(path, flags, mode);
    }
    return pscalPathVirtualized_open(path, flags);
}
#elif defined(PSCAL_TARGET_IOS)
#define vprocHostOpenVirtualized(path, flags, mode) pscalPathVirtualized_open((path), (flags), (mode))
#else
#define vprocHostOpenVirtualized(path, flags, mode) open((path), (flags), (mode))
#endif

/* -- Undefine host macros that might conflict -- */
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

/* -- Data Structures -- */

typedef struct {
    int host_fd;
} VProcFdEntry;

struct VProc {
    pthread_mutex_t mu;     // ADDED: Protects the FD table shared by threads
    VProcFdEntry *entries;
    size_t capacity;
    int next_fd;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int stdout_host_fd;
    int stderr_host_fd;
    VProcWinsize winsize;
    int pid;
};

static __thread VProc *gVProcCurrent = NULL;
static int gNextSyntheticPid = 1000;
static int gShellSelfPid = 0;

typedef struct {
    int pid;
    pthread_t tid;
    int parent_pid;
    int pgid;
    int sid;
    int status;
    int exit_signal;
    bool exited;
    bool stopped;
    int stop_signo;
    bool zombie;
    int job_id;
    char *label;
    char comm[16];
} VProcTaskEntry;

typedef struct {
    VProcTaskEntry *items;
    size_t count;
    size_t capacity;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} VProcTaskTable;

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    VProc *vp;
} VProcThreadStartCtx;

static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid);

static VProcTaskTable gVProcTasks = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

/* -- Helper Functions -- */

static void vprocSetCommLocked(VProcTaskEntry *entry, const char *label) {
    if (!entry) return;
    if (label && *label) {
        strncpy(entry->comm, label, sizeof(entry->comm) - 1);
        entry->comm[sizeof(entry->comm) - 1] = '\0';
    } else {
        memset(entry->comm, 0, sizeof(entry->comm));
    }
}

static void vprocMaybeUpdateThreadNameLocked(VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0 || entry->tid == 0) {
        return;
    }
    if (!pthread_equal(entry->tid, pthread_self())) {
        return;
    }
    const char *base = entry->comm[0] ? entry->comm : "vproc";
    char name[16];
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "-%d", entry->pid);
    size_t suffix_len = strlen(suffix);
    size_t base_cap = (suffix_len + 1 < sizeof(name)) ? (sizeof(name) - suffix_len - 1) : 0;
    if (base_cap > 0) {
        snprintf(name, sizeof(name), "%.*s%s", (int)base_cap, base, suffix);
    } else {
        snprintf(name, sizeof(name), "%s", suffix);
    }
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

// NOTE: Caller must hold vp->mu
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

// NOTE: Caller must hold vp->mu, or wrap this
static int vprocInsertLocked(VProc *vp, int host_fd) {
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

static int vprocInsert(VProc *vp, int host_fd) {
    if (!vp) return -1;
    pthread_mutex_lock(&vp->mu);
    int rc = vprocInsertLocked(vp, host_fd);
    pthread_mutex_unlock(&vp->mu);
    return rc;
}

int vprocAdoptHostFd(VProc *vp, int host_fd) {
    return vprocInsert(vp, host_fd);
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
    
    // Initialize mutex before use
    if (pthread_mutex_init(&vp->mu, NULL) != 0) {
        free(vp);
        return NULL;
    }

    vp->capacity = VPROC_INITIAL_CAPACITY;
    vp->entries = calloc(vp->capacity, sizeof(VProcFdEntry));
    if (!vp->entries) {
        pthread_mutex_destroy(&vp->mu);
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
        slot->parent_pid = 0;
        slot->pgid = vp->pid;
        slot->sid = vp->pid;
        slot->status = 0;
        slot->exit_signal = 0;
        slot->exited = false;
        slot->stopped = false;
        slot->stop_signo = 0;
        slot->zombie = false;
        slot->job_id = 0;
        memset(slot->comm, 0, sizeof(slot->comm));
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
    vp->stdout_host_fd = stdout_src;
    vp->stderr_host_fd = stderr_src;
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

    // Lock shouldn't strictly be needed here if refcount is 0, but good practice
    pthread_mutex_lock(&vp->mu);
    /* Close only the vproc-owned fds; do not close the saved host stdio fds. */
    for (size_t i = 0; i < vp->capacity; ++i) {
        if (vp->entries[i].host_fd >= 0 &&
            vp->entries[i].host_fd != vp->stdout_host_fd &&
            vp->entries[i].host_fd != vp->stderr_host_fd) {
            close(vp->entries[i].host_fd);
        }
        vp->entries[i].host_fd = -1;
    }
    if (vp->stdout_host_fd >= 0) {
        close(vp->stdout_host_fd);
    }
    if (vp->stderr_host_fd >= 0) {
        close(vp->stderr_host_fd);
    }
    free(vp->entries);
    vp->entries = NULL;
    pthread_mutex_unlock(&vp->mu);
    
    pthread_mutex_destroy(&vp->mu);
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

static void *vprocThreadTrampoline(void *arg) {
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)arg;
    
    // Detach so resources are freed by iOS when the thread exits.
    // We rely on vprocTaskEntry for logical join/wait.
    pthread_detach(pthread_self());

    VProc *vp = ctx ? ctx->vp : NULL;
    if (vp) {
        vprocActivate(vp);
        vprocRegisterThread(vp, pthread_self());
    }

    void *res = NULL;
    if (ctx && ctx->start_routine) {
        res = ctx->start_routine(ctx->arg);
    }

    if (vp) {
        // FIX: Report exit status to the task table before dying
        // Assuming the thread return value is the exit code (intptr_t cast)
        int exit_code = (int)(intptr_t)res;
        vprocMarkExit(vp, W_EXITCODE(exit_code, 0));
        vprocDeactivate();
    }
    
    free(ctx);
    return res;
}

int vprocPthreadCreateShim(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg) {
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)calloc(1, sizeof(VProcThreadStartCtx));
    if (!ctx) {
        errno = ENOMEM;
        return ENOMEM;
    }
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->vp = vprocCurrent();
    return pthread_create(thread, attr, vprocThreadTrampoline, ctx);
}

int vprocTranslateFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    int host = -1;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity) {
        host = vp->entries[fd].host_fd;
    }
    pthread_mutex_unlock(&vp->mu);
    
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
    // We cannot hold lock here during Clone, but we need it for Translate
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&vp->mu);
    if ((size_t)target >= vp->capacity) {
        size_t new_cap = vp->capacity;
        if (new_cap == 0) new_cap = VPROC_INITIAL_CAPACITY;
        while ((size_t)target >= new_cap) {
            new_cap *= 2;
        }
        VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
        if (!resized) {
            pthread_mutex_unlock(&vp->mu);
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
    // Clone calls fcntl on host, safe to do while holding lock as it doesn't re-enter vproc
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        pthread_mutex_unlock(&vp->mu);
        return -1;
    }
    vp->entries[target].host_fd = cloned;
    pthread_mutex_unlock(&vp->mu);
    return target;
}

int vprocClose(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd >= vp->capacity) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    int host = vp->entries[fd].host_fd;
    if (host < 0) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    vp->entries[fd].host_fd = -1;
    pthread_mutex_unlock(&vp->mu);
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
    // vprocInsert locks internally, so this is safe
    int left = vprocInsert(vp, raw[0]);
    int right = vprocInsert(vp, raw[1]);
    if (left < 0 || right < 0) {
        if (left >= 0) vprocClose(vp, left);
        else close(raw[0]);
        if (right >= 0) vprocClose(vp, right);
        else close(raw[1]);
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

int vprocSetWinsize(VProc *vp, int cols, int rows) {
    if (!vp) {
        errno = EINVAL;
        return -1;
    }
    // Winsize changes are atomic enough to not strictly need mutex for this simple struct,
    // but better safe if we expanded struct later.
    pthread_mutex_lock(&vp->mu);
    if (cols > 0) vp->winsize.cols = cols;
    if (rows > 0) vp->winsize.rows = rows;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocGetWinsize(VProc *vp, VProcWinsize *out) {
    if (!vp || !out) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    *out = vp->winsize;
    pthread_mutex_unlock(&vp->mu);
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
    vprocMaybeUpdateThreadNameLocked(entry);
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
        entry->exit_signal = 0;
        entry->exited = true;
        entry->stopped = false;
        entry->stop_signo = 0;
        entry->zombie = true;
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetParent(int pid, int parent_pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->parent_pid = parent_pid;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocSetPgid(int pid, int pgid) {
    if (pid <= 0 || pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->pgid = pgid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocSetSid(int pid, int sid) {
    if (pid <= 0 || sid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sid = sid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocGetPgid(int pid) {
    int pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        pgid = entry->pgid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pgid;
}

int vprocGetSid(int pid) {
    int sid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        sid = entry->sid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return sid;
}

static void vprocClearEntryLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    if (entry->label) {
        free(entry->label);
        entry->label = NULL;
    }
    memset(entry->comm, 0, sizeof(entry->comm));
    entry->pid = 0;
    entry->tid = 0;
    entry->parent_pid = 0;
    entry->pgid = 0;
    entry->sid = 0;
    entry->status = 0;
    entry->exit_signal = 0;
    entry->exited = false;
    entry->stopped = false;
    entry->stop_signo = 0;
    entry->zombie = false;
    entry->job_id = 0;
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
            out[count].parent_pid = entry->parent_pid;
            out[count].pgid = entry->pgid;
            out[count].sid = entry->sid;
            out[count].exited = entry->exited;
            out[count].stopped = entry->stopped;
            out[count].zombie = entry->zombie;
            out[count].exit_signal = entry->exit_signal;
            out[count].status = entry->status;
            out[count].stop_signo = entry->stop_signo;
            strncpy(out[count].comm, entry->comm, sizeof(out[count].comm) - 1);
            out[count].comm[sizeof(out[count].comm) - 1] = '\0';
            if (entry->label) {
                strncpy(out[count].command, entry->label, sizeof(out[count].command) - 1);
                out[count].command[sizeof(out[count].command) - 1] = '\0';
            } else if (entry->comm[0]) {
                strncpy(out[count].command, entry->comm, sizeof(out[count].command) - 1);
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
    bool allow_cont = (options & WCONTINUED) != 0;
    bool nohang = (options & WNOHANG) != 0;
    bool nowait = (options & WNOWAIT) != 0;
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    
    pthread_mutex_lock(&gVProcTasks.mu);
    while (true) {
        VProcTaskEntry *ready = NULL;
        bool has_candidate = false;
        
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (!entry || entry->pid <= 0) continue;
            
            bool match = false;
            if (pid > 0 && entry->pid == pid) {
                match = true;
            } else if (pid == -1) {
                match = true;
            } else if (pid == 0) {
                int shell_pgid = vprocGetShellSelfPid();
                match = (shell_pgid > 0) ? (entry->pgid == shell_pgid) : true;
            } else if (pid < -1) {
                match = (entry->pgid == -pid);
            }
            
            if (!match) continue;
            has_candidate = true;
            
            // Check if this task has a state change to report
            if (entry->exited ||
               (allow_stop && entry->stopped && entry->stop_signo > 0) ||
               (allow_cont && !entry->stopped && entry->exit_signal == 0 && entry->zombie)) {
                ready = entry;
                break;
            }
        }

        if (ready) {
            int status = 0;
            // Cache values locally before potentially clearing the entry
            int r_exited = ready->exited;
            int r_stopped = ready->stopped;
            int r_exit_sig = ready->exit_signal;
            int r_stop_sig = ready->stop_signo;
            int r_status = ready->status;
            int waited_pid = ready->pid;

            if (r_exited) {
                if (r_exit_sig > 0) {
                    status = (r_exit_sig & 0x7f);
                } else {
                    status = W_EXITCODE(r_status & 0xff, 0);
                }
            } else if (r_stopped && r_stop_sig > 0) {
                status = W_STOPCODE(r_stop_sig & 0xff);
            }
            
            if (status_out) {
                *status_out = status;
            }
            
            // FIX: Reap the process (clear the slot) here, and only here.
            if (r_exited && !nowait) {
                vprocClearEntryLocked(ready);
            } else if (r_exited) {
                ready->zombie = true; // WNOWAIT requested, leave as zombie
            } else if (r_stopped) {
                ready->stop_signo = 0; // Clear stop signal after reporting
            }

            if (dbg) {
                fprintf(stderr, "[vproc-wait] pid=%d status=%d exited=%d stop=%d\n",
                        waited_pid, status, r_exited, r_stopped);
            }
            pthread_mutex_unlock(&gVProcTasks.mu);
            return waited_pid;
        }

        if (nohang) {
            if (status_out) *status_out = 0;
            pthread_mutex_unlock(&gVProcTasks.mu);
            return 0;
        }

        if (!has_candidate) {
            // No synthetic match found
            pthread_mutex_unlock(&gVProcTasks.mu);
            if (pid > 0) {
                errno = ESRCH;
                return -1;
            }
            return waitpid(pid, status_out, options);
        }

        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
}

int vprocKillShim(pid_t pid, int sig) {
    bool target_group = (pid <= 0);
    bool broadcast_all = (pid == -1);
    int target = target_group ? -pid : pid;
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    
    if (pid == 0) {
        target = vprocGetShellSelfPid();
        if (target <= 0) return kill(pid, sig);
    }
    
    int rc = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    bool delivered = false;
    
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        
        if (broadcast_all) {
            // Don't kill self in broadcast
            if (entry->pid == vprocGetPidShim()) continue;
        } else if (target_group) {
            if (entry->pgid != target) continue;
        } else {
            if (entry->pid != target) continue;
        }
        
        delivered = true;
        
        if (dbg) {
            fprintf(stderr, "[vproc-kill] pid=%d sig=%d target=%d entry_pid=%d tid=%p\n",
                    (int)pid, sig, target, entry->pid, (void *)entry->tid);
        }

        bool stop_signal = (sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU);
        bool cont_signal = (sig == SIGCONT);

        if (entry->tid && !stop_signal && !cont_signal) {
            // Terminate the thread
            pthread_kill(entry->tid, sig);
            pthread_cancel(entry->tid);
        } else if (entry->tid && stop_signal) {
            pthread_kill(entry->tid, sig);
        }

        if (stop_signal) {
            entry->stopped = true;
            entry->exited = false;
            entry->stop_signo = sig;
            entry->exit_signal = 0;
            entry->status = 128 + sig;
            entry->zombie = false;
        } else if (cont_signal) {
            entry->stopped = false;
            entry->stop_signo = 0;
            entry->exit_signal = 0;
            entry->zombie = false;
        } else {
            // TERMINATION
            entry->status = entry->status & 0xff;
            entry->exit_signal = sig;
            entry->exited = true;
            entry->stopped = false;
            entry->stop_signo = 0;
            
            // FIX: Do NOT clear the entry here!
            // Mark it as zombie so waitpid can find it and report status.
            entry->zombie = true;
        }
    }
    
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    if (delivered) return rc;

    if (target_group || broadcast_all) {
        errno = ESRCH;
        return -1;
    }
    
    return kill(pid, sig);
}

/* ... (Remaining functions for getters/setters/shell-self-pid are unchanged logically) ... */
/* INCLUDED HERE FOR COMPLETENESS */

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
        vprocSetCommLocked(entry, label);
        vprocMaybeUpdateThreadNameLocked(entry);
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
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr, "[vwrite] fd=%d -> host=%d count=%zu\n", fd, host, count);
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
    // vprocInsert handles internal locking
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
        if (getenv("PSCALI_TOOL_DEBUG")) {
            fprintf(stderr, "[vproc-open] path=%s flags=%d errno=%d\n", path, flags, errno);
        }
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        close(host_fd);
    }
    return slot;
}

#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */
