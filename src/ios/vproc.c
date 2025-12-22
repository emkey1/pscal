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
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <termios.h>
#include <limits.h>
#include <sys/time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#endif
#include "common/runtime_tty.h"

#if defined(PSCAL_TARGET_IOS)
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#undef PATH_VIRTUALIZATION_NO_MACROS
#endif

#if defined(PSCAL_TARGET_IOS)
__attribute__((weak)) void pscalRuntimeRequestSigint(void);
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
#ifdef getppid
#undef getppid
#endif
#ifdef getpgrp
#undef getpgrp
#endif
#ifdef getpgid
#undef getpgid
#endif
#ifdef setpgid
#undef setpgid
#endif
#ifdef getsid
#undef getsid
#endif
#ifdef setsid
#undef setsid
#endif
#ifdef tcgetpgrp
#undef tcgetpgrp
#endif
#ifdef tcsetpgrp
#undef tcsetpgrp
#endif
#ifdef sigaction
#undef sigaction
#endif
#ifdef sigprocmask
#undef sigprocmask
#endif
#ifdef sigpending
#undef sigpending
#endif
#ifdef sigsuspend
#undef sigsuspend
#endif
#ifdef signal
#undef signal
#endif
#ifdef raise
#undef raise
#endif
#ifdef pthread_sigmask
#undef pthread_sigmask
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
    int stdin_host_fd;      /* stable handle for "controlling" stdin */
    int stdout_host_fd;
    int stderr_host_fd;
    VProcWinsize winsize;
    int pid;
};

static __thread VProc *gVProcCurrent = NULL;
static __thread VProc *gVProcStack[16] = {0};
static __thread size_t gVProcStackDepth = 0;
static VProc **gVProcRegistry = NULL;
static size_t gVProcRegistryCount = 0;
static size_t gVProcRegistryCapacity = 0;
static pthread_mutex_t gVProcRegistryMu = PTHREAD_MUTEX_INITIALIZER;
static int gNextSyntheticPid = 0;
static _Thread_local int gShellSelfPid = 0;
static _Thread_local int gKernelPid = 0;
static pthread_t gShellSelfTid;
static bool gShellSelfTidValid = false;
static VProcSessionStdio gSessionStdio = { .stdin_host_fd = -1, .stdout_host_fd = -1, .stderr_host_fd = -1, .kernel_pid = 0 };
static pthread_mutex_t gSessionInputInitMu = PTHREAD_MUTEX_INITIALIZER;

static int vprocNextPidSeed(void) {
    int host = (int)getpid();
    if (host < 2000) {
        host += 2000;
    }
    return host;
}

static void vprocRegistryAdd(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
    }
    if (gVProcRegistryCount >= gVProcRegistryCapacity) {
        size_t new_cap = gVProcRegistryCapacity ? gVProcRegistryCapacity * 2 : 16;
        VProc **resized = (VProc **)realloc(gVProcRegistry, new_cap * sizeof(VProc *));
        if (!resized) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
        gVProcRegistry = resized;
        gVProcRegistryCapacity = new_cap;
    }
    gVProcRegistry[gVProcRegistryCount++] = vp;
    pthread_mutex_unlock(&gVProcRegistryMu);
}

static void vprocRegistryRemove(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            gVProcRegistry[i] = gVProcRegistry[gVProcRegistryCount - 1];
            gVProcRegistryCount--;
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
}

static bool vprocRegistryContains(const VProc *vp) {
    if (!vp) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return found;
}

static void vprocClearThreadState(void) {
    gVProcCurrent = NULL;
    gVProcStackDepth = 0;
    for (size_t i = 0; i < sizeof(gVProcStack) / sizeof(gVProcStack[0]); ++i) {
        gVProcStack[i] = NULL;
    }
}

static VProc *vprocForThread(void);

typedef struct {
    int pid;
    pthread_t tid;
    pthread_t *threads;
    size_t thread_count;
    size_t thread_capacity;
    int parent_pid;
    int pgid;
    int sid;
    bool session_leader;
    int fg_pgid;
    int status;
    int exit_signal;
    bool exited;
    bool stopped;
    bool continued;
    int stop_signo;
    bool zombie;
    bool stop_unsupported;
    int job_id;
    char *label;
    char comm[16];
    int *children;
    size_t child_count;
    size_t child_capacity;
    int sigchld_events;
    bool sigchld_blocked;
    int rusage_utime;
    int rusage_stime;
    bool group_exit;
    int group_exit_code;
    uint32_t blocked_signals;
    uint32_t pending_signals;
    uint32_t ignored_signals;
    int pending_counts[32];
    int fg_override_pgid;
    struct sigaction actions[32];
    uint64_t start_mono_ns;
} VProcTaskEntry;

typedef enum {
    VPROC_SIGCHLD_EVENT_EXIT = 0,
    VPROC_SIGCHLD_EVENT_STOP,
    VPROC_SIGCHLD_EVENT_CONT
} VProcSigchldEvent;

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
    int shell_self_pid;
    int kernel_pid;
    bool detach;
} VProcThreadStartCtx;

static VProcTaskEntry *vprocTaskFindLocked(int pid);
static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid);
static void vprocClearEntryLocked(VProcTaskEntry *entry);
static void vprocCancelListAdd(pthread_t **list, size_t *count, size_t *capacity, pthread_t tid);
static void vprocTaskTableRepairLocked(void);

static VProcTaskTable gVProcTasks = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static VProcTaskEntry *gVProcTasksItemsStable = NULL;
static size_t gVProcTasksCapacityStable = 0;

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

static void vprocNotifyParentSigchldLocked(int parent_pid, VProcSigchldEvent evt);
static struct sigaction vprocGetSigactionLocked(VProcTaskEntry *entry, int sig);
static int vprocDefaultParentPid(void) {
    int cur = vprocGetPidShim();
    if (cur <= 0) {
        cur = vprocGetShellSelfPid();
    }
    if (cur <= 0) {
        cur = vprocGetKernelPid();
    }
    return cur > 0 ? cur : 0;
}

static int vprocAdoptiveParentPidLocked(const VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0) {
        return 0;
    }

    if (entry->session_leader && entry->sid == entry->pid) {
        /* Session leader teardown: let children become reparented to pid 0. */
        return 0;
    }

    /* Prefer reparenting within the same session by adopting to the session
     * leader (sid) when it exists. This supports multiple concurrent sessions
     * (multiple shells/windows) without relying on a global kernel pid. */
    if (entry->sid > 0 && entry->sid != entry->pid) {
        VProcTaskEntry *leader = vprocTaskFindLocked(entry->sid);
        if (leader && leader->pid == entry->sid && leader->session_leader) {
            return entry->sid;
        }
        /* Even if we can't find the leader entry (it may have already been
         * discarded), falling back to the sid still keeps children grouped. */
        return entry->sid;
    }

    int kernel = vprocGetKernelPid();
    if (kernel > 0 && kernel != entry->pid) {
        return kernel;
    }
    int shell = vprocGetShellSelfPid();
    if (shell > 0 && shell != entry->pid) {
        return shell;
    }
    return 0;
}

static inline uint32_t vprocSigMask(int sig) {
    if (sig <= 0 || sig >= 32) return 0;
    return (1u << sig);
}

int vprocNextJobIdSeed(void) {
    static int next_job_id = 1;
    return __sync_fetch_and_add(&next_job_id, 1);
}

static int vprocRuntimeCenti(const VProcTaskEntry *entry, uint64_t now_ns) {
    if (!entry || entry->start_mono_ns == 0) {
        return 0;
    }
    uint64_t start = entry->start_mono_ns;
    uint64_t delta_ns = (now_ns > start) ? (now_ns - start) : 0;
    int centi = (int)(delta_ns / 10000000ull); /* centiseconds */
    return centi < 0 ? 0 : centi;
}

static int vprocCentiFromMicros(int64_t micros) {
    if (micros <= 0) {
        return 0;
    }
    int64_t centi = micros / 10000;
    if (centi > INT_MAX) {
        return INT_MAX;
    }
    return (int)centi;
}

static bool vprocThreadUsageMicros(pthread_t tid, int64_t *user_us, int64_t *system_us) {
#if defined(__APPLE__)
    thread_t thread_port = pthread_mach_thread_np(tid);
    if (thread_port == MACH_PORT_NULL) {
        return false;
    }
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t kr = thread_info(thread_port, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), thread_port);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    if (user_us) {
        *user_us = (int64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds;
    }
    if (system_us) {
        *system_us = (int64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
    }
    return true;
#else
    (void)tid;
    if (user_us) *user_us = 0;
    if (system_us) *system_us = 0;
    return false;
#endif
}

static bool vprocComputeCpuTimesLocked(const VProcTaskEntry *entry, int *utime_cs, int *stime_cs) {
    if (!entry) {
        return false;
    }
    int64_t user_total = 0;
    int64_t system_total = 0;
    bool saw = false;
    if (entry->thread_count == 0) {
        int64_t user_us = 0;
        int64_t system_us = 0;
        if (vprocThreadUsageMicros(entry->tid, &user_us, &system_us)) {
            user_total += user_us;
            system_total += system_us;
            saw = true;
        }
    } else {
        for (size_t i = 0; i < entry->thread_count; ++i) {
            pthread_t tid = entry->threads[i];
            int64_t user_us = 0;
            int64_t system_us = 0;
            if (vprocThreadUsageMicros(tid, &user_us, &system_us)) {
                user_total += user_us;
                system_total += system_us;
                saw = true;
            }
        }
    }
    if (!saw) {
        return false;
    }
    if (utime_cs) {
        *utime_cs = vprocCentiFromMicros(user_total);
    }
    if (stime_cs) {
        *stime_cs = vprocCentiFromMicros(system_total);
    }
    return true;
}

static uint64_t vprocNowMonoNs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

static inline bool vprocSigIndexValid(int sig) {
    return sig > 0 && sig < 32;
}

static inline bool vprocSignalBlockable(int sig) {
    return !(sig == SIGKILL || sig == SIGSTOP);
}

static inline bool vprocSignalIgnorable(int sig) {
    return vprocSignalBlockable(sig);
}

static void vprocInitEntryDefaultsLocked(VProcTaskEntry *entry, int pid, const VProcTaskEntry *parent) {
    const VProcTaskEntry *inherit_parent = NULL;
    VProc *active = vprocCurrent();
    int shell_pid = vprocGetShellSelfPid();
    if (parent) {
        if ((active && vprocPid(active) == parent->pid) ||
            (shell_pid > 0 && parent->pid == shell_pid)) {
            inherit_parent = parent;
        }
    }
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    entry->stop_unsupported = false;
    entry->pid = pid;
    entry->pgid = pid;
    entry->sid = pid;
    entry->session_leader = false;
    entry->fg_pgid = pid;
    entry->sigchld_blocked = false;
    entry->job_id = inherit_parent ? inherit_parent->job_id : 0;
    for (int i = 0; i < 32; ++i) {
        sigemptyset(&entry->actions[i].sa_mask);
        entry->actions[i].sa_handler = SIG_DFL;
        entry->actions[i].sa_flags = 0;
    }
    if (inherit_parent) {
        if (inherit_parent->sid > 0) entry->sid = inherit_parent->sid;
        if (inherit_parent->pgid > 0) entry->pgid = inherit_parent->pgid;
        if (inherit_parent->fg_pgid > 0) entry->fg_pgid = inherit_parent->fg_pgid;
        /* Do NOT inherit blocked signals so job-control signals (TERM/INT) work. */
        entry->blocked_signals = 0;
        entry->ignored_signals = inherit_parent->ignored_signals & ~(vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP));
        entry->sigchld_blocked = inherit_parent->sigchld_blocked;
        memcpy(entry->actions, inherit_parent->actions, sizeof(entry->actions));
        if (shell_pid > 0 && inherit_parent->pid == shell_pid) {
            int job_sigs[] = { SIGTSTP, SIGTTIN, SIGTTOU };
            size_t sig_count = sizeof(job_sigs) / sizeof(job_sigs[0]);
            for (size_t i = 0; i < sig_count; ++i) {
                int sig = job_sigs[i];
                if (!vprocSigIndexValid(sig)) {
                    continue;
                }
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_handler = SIG_DFL;
                entry->actions[sig].sa_flags = 0;
                entry->ignored_signals &= ~vprocSigMask(sig);
            }
        }
    }
    entry->start_mono_ns = vprocNowMonoNs();
}

static bool vprocAddChildLocked(VProcTaskEntry *parent, int child_pid) {
    if (!parent || child_pid <= 0) return false;
    if (parent->child_count >= parent->child_capacity) {
        size_t new_cap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        int *resized = realloc(parent->children, new_cap * sizeof(int));
        if (!resized) return false;
        parent->children = resized;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child_pid;
    return true;
}

static bool vprocSignalBlockedLocked(VProcTaskEntry *entry, int sig) {
    if (!vprocSignalBlockable(sig)) {
        return false;
    }
    uint32_t mask = vprocSigMask(sig);
    return mask != 0 && (entry->blocked_signals & mask);
}

static bool vprocSignalIgnoredLocked(VProcTaskEntry *entry, int sig) {
    if (!vprocSignalIgnorable(sig)) {
        return false;
    }
    if (vprocSigIndexValid(sig) && entry->actions[sig].sa_handler == SIG_IGN) {
        return true;
    }
    uint32_t mask = vprocSigMask(sig);
    return mask != 0 && (entry->ignored_signals & mask);
}

static void vprocMaybeStampRusageLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    int utime = 0;
    int stime = 0;
    if (vprocComputeCpuTimesLocked(entry, &utime, &stime)) {
        if (utime > entry->rusage_utime) {
            entry->rusage_utime = utime;
        }
        if (stime > entry->rusage_stime) {
            entry->rusage_stime = stime;
        }
        return;
    }
    if (entry->rusage_utime == 0 && entry->rusage_stime == 0) {
        uint64_t now = vprocNowMonoNs();
        int centi = vprocRuntimeCenti(entry, now);
        if (centi > 0) {
            entry->rusage_utime = centi;
            entry->rusage_stime = centi / 10; /* crude split fallback */
        }
    }
}

typedef enum {
    VPROC_SIG_IGNORE,
    VPROC_SIG_STOP,
    VPROC_SIG_CONT,
    VPROC_SIG_KILL,
    VPROC_SIG_HANDLER
} VProcSignalAction;

static VProcSignalAction vprocDefaultSignalAction(int sig) {
    switch (sig) {
        case SIGCHLD:
#ifdef SIGWINCH
        case SIGWINCH:
#endif
#ifdef SIGURG
        case SIGURG:
#endif
#ifdef SIGIO
        case SIGIO:
#endif
            return VPROC_SIG_IGNORE;
        case SIGCONT:
            return VPROC_SIG_CONT;
        case SIGTSTP:
        case SIGSTOP:
        case SIGTTIN:
        case SIGTTOU:
            return VPROC_SIG_STOP;
        default:
            return VPROC_SIG_KILL;
    }
}

static VProcSignalAction vprocEffectiveSignalActionLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return vprocDefaultSignalAction(sig);
    }
    struct sigaction sa = entry->actions[sig];
    if (sa.sa_handler == SIG_IGN) {
        return VPROC_SIG_IGNORE;
    }
    if (sa.sa_handler != SIG_DFL) {
        return VPROC_SIG_HANDLER;
    }
    return vprocDefaultSignalAction(sig);
}

static void vprocInvokeHandlerLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return;
    }
    struct sigaction sa = vprocGetSigactionLocked(entry, sig);
    if (sa.sa_handler == SIG_IGN || sa.sa_handler == SIG_DFL) {
        return;
    }
    int saved_blocked = entry->blocked_signals;
    if (!(sa.sa_flags & SA_NODEFER)) {
        entry->blocked_signals |= vprocSigMask(sig);
    }
    /* Apply handler mask. */
    for (int s = 1; s < 32; ++s) {
        if (sigismember(&sa.sa_mask, s)) {
            entry->blocked_signals |= vprocSigMask(s);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (sa.sa_flags & SA_SIGINFO) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code = SI_USER;
        info.si_pid = entry->parent_pid;
        sa.sa_sigaction(sig, &info, NULL);
    } else {
        sa.sa_handler(sig);
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    entry->blocked_signals = saved_blocked;
}

static void vprocQueuePendingSignalLocked(VProcTaskEntry *entry, int sig) {
    uint32_t mask = vprocSigMask(sig);
    if (mask != 0) {
        entry->pending_signals |= mask;
        if (sig > 0 && sig < 32) {
            if (entry->pending_counts[sig] < INT_MAX) {
                entry->pending_counts[sig]++;
            }
        }
    }
}

static void vprocApplySignalLocked(VProcTaskEntry *entry, int sig) {
    VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);

    if (vprocSignalIgnoredLocked(entry, sig) || action == VPROC_SIG_IGNORE) {
        return;
    }
    if (action == VPROC_SIG_HANDLER) {
        struct sigaction sa = vprocGetSigactionLocked(entry, sig);
        if (sa.sa_flags & SA_RESETHAND) {
            entry->actions[sig].sa_handler = SIG_DFL;
            entry->actions[sig].sa_flags = 0;
            sigemptyset(&entry->actions[sig].sa_mask);
            entry->ignored_signals &= ~vprocSigMask(sig);
        }
        entry->continued = false;
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        vprocInvokeHandlerLocked(entry, sig);
        return;
    }
    if (action == VPROC_SIG_STOP) {
        if (entry->stop_unsupported) {
            if (sig == SIGTSTP && pscalRuntimeRequestSigint) {
                pscalRuntimeRequestSigint();
            }
            return;
        }
        entry->stopped = true;
        entry->continued = false;
        entry->exited = false;
        entry->stop_signo = sig;
        entry->exit_signal = 0;
        entry->status = 128 + sig;
        entry->zombie = false;
        vprocNotifyParentSigchldLocked(entry->parent_pid, VPROC_SIGCHLD_EVENT_STOP);
    } else if (action == VPROC_SIG_CONT) {
        entry->stopped = false;
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        entry->continued = true;
        vprocNotifyParentSigchldLocked(entry->parent_pid, VPROC_SIGCHLD_EVENT_CONT);
    } else if (sig > 0) {
        entry->status = entry->status & 0xff;
        entry->exit_signal = sig;
        entry->exited = true;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        entry->zombie = true;
        vprocNotifyParentSigchldLocked(entry->parent_pid, VPROC_SIGCHLD_EVENT_EXIT);
    }
}

static int vprocForegroundPgidLocked(int sid) {
    if (sid <= 0) return -1;
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->sid == sid && entry->session_leader) {
            return entry->fg_pgid;
        }
    }
    return -1;
}

static bool vprocShouldStopForBackgroundTty(VProc *vp, int sig) {
    if (!vp) {
        return false;
    }
    bool stopped = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    if (entry && entry->sid > 0) {
        int fg = vprocForegroundPgidLocked(entry->sid);
        if (fg > 0 && entry->pgid != fg) {
            vprocApplySignalLocked(entry, sig);
            pthread_cond_broadcast(&gVProcTasks.cv);
            stopped = true;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return stopped;
}

static int vprocForegroundPgidForEntryLocked(const VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0) {
        return -1;
    }
    if (entry->sid > 0) {
        int fg = vprocForegroundPgidLocked(entry->sid);
        if (fg > 0) {
            return fg;
        }
    }
    if (entry->pgid > 0) {
        return entry->pgid;
    }
    return entry->pid;
}

bool vprocWaitIfStopped(VProc *vp) {
    if (!vp) {
        return false;
    }
    int pid = vprocPid(vp);
    if (pid <= 0) {
        return false;
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0 && pid == shell_pid) {
        return false;
    }
    bool waited = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->stop_unsupported) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return false;
    }
    while (entry && entry->stopped && !entry->exited) {
        if (entry->stop_unsupported) {
            entry->stopped = false;
            entry->continued = true;
            entry->stop_signo = 0;
            pthread_cond_broadcast(&gVProcTasks.cv);
            break;
        }
        waited = true;
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
        if (entry->pid != pid) {
            entry = vprocTaskFindLocked(pid);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return waited;
}

static void vprocDispatchControlSignal(VProc *vp, int sig) {
    if (!vp) {
        return;
    }
    int pid = vprocPid(vp);
    int shell_pid = vprocGetShellSelfPid();
    if (pid <= 0 || (shell_pid > 0 && pid == shell_pid)) {
        return;
    }
    int fg_pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    fg_pgid = vprocForegroundPgidForEntryLocked(entry);
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (fg_pgid > 0) {
        (void)vprocKillShim(-fg_pgid, sig);
    } else {
        (void)vprocKillShim(pid, sig);
    }
    if (sig == SIGTSTP) {
        (void)vprocWaitIfStopped(vp);
    }
}

static void vprocDeliverPendingSignalsLocked(VProcTaskEntry *entry) {
    uint32_t pending = entry->pending_signals;
    for (int sig = 1; sig < 32; ++sig) {
        uint32_t mask = vprocSigMask(sig);
        if (!(pending & mask)) continue;
        if (vprocSignalBlockedLocked(entry, sig)) continue;
        VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
        if (action == VPROC_SIG_IGNORE || vprocSignalIgnoredLocked(entry, sig)) {
            entry->pending_signals &= ~mask;
            entry->pending_counts[sig] = 0;
            continue;
        }
        vprocApplySignalLocked(entry, sig);
        entry->pending_signals &= ~mask;
        entry->pending_counts[sig] = 0;
    }
}

typedef struct {
    VProcSessionStdio *session;
    int shell_pid;
    int kernel_pid;
} VProcSessionInputCtx;

static VProcSessionInput *vprocSessionInputEnsure(VProcSessionStdio *session, int shell_pid, int kernel_pid);
static ssize_t vprocSessionReadInput(VProcSessionStdio *session, void *buf, size_t count);

static bool vprocShellOwnsForegroundLocked(int shell_pid, int *out_fgid) {
    if (out_fgid) {
        *out_fgid = -1;
    }
    if (shell_pid <= 0) {
        return true;
    }
    VProcTaskEntry *entry = vprocTaskFindLocked(shell_pid);
    if (!entry) {
        return true;
    }
    int shell_pgid = entry->pgid;
    int sid = entry->sid;
    int fg = (sid > 0) ? vprocForegroundPgidLocked(sid) : -1;
    if (out_fgid) {
        *out_fgid = fg;
    }
    if (fg <= 0 || shell_pgid <= 0) {
        return true;
    }
    return fg == shell_pgid;
}

static void vprocDispatchControlSignalToForeground(int shell_pid, int sig) {
    if (shell_pid <= 0) {
        return;
    }
    int target_fgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    bool shell_owns_fg = vprocShellOwnsForegroundLocked(shell_pid, &target_fgid);
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (shell_owns_fg || target_fgid <= 0) {
#if defined(PSCAL_TARGET_IOS)
        if (sig == SIGINT && pscalRuntimeRequestSigint) {
            pscalRuntimeRequestSigint();
        }
#endif
        return;
    }
    (void)vprocKillShim(-target_fgid, sig);
}

static void *vprocSessionInputThread(void *arg) {
    VProcSessionInputCtx *ctx = (VProcSessionInputCtx *)arg;
    if (!ctx || !ctx->session) {
        free(ctx);
        return NULL;
    }
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }

    VProcSessionStdio *session = ctx->session;
    VProcSessionInput *input = session ? session->input : NULL;
    int fd = session ? session->stdin_host_fd : -1;
    unsigned char ch = 0;
    while (fd >= 0) {
        ssize_t r = vprocHostRead(fd, &ch, 1);
        if (r <= 0) {
            if (input) {
                pthread_mutex_lock(&input->mu);
                input->eof = true;
                pthread_cond_broadcast(&input->cv);
                pthread_mutex_unlock(&input->mu);
            }
            break;
        }
        if (ch == 3 || ch == 26) {
            int sig = (ch == 3) ? SIGINT : SIGTSTP;
            vprocDispatchControlSignalToForeground(ctx->shell_pid, sig);
            continue;
        }
        if (!input) {
            continue;
        }
        pthread_mutex_lock(&input->mu);
        if (input->len + 1 > input->cap) {
            size_t new_cap = input->cap ? input->cap * 2 : 256;
            unsigned char *resized = (unsigned char *)realloc(input->buf, new_cap);
            if (resized) {
                input->buf = resized;
                input->cap = new_cap;
            }
        }
        if (input->len < input->cap) {
            input->buf[input->len++] = ch;
            pthread_cond_signal(&input->cv);
        }
        pthread_mutex_unlock(&input->mu);
    }
    free(ctx);
    return NULL;
}

static VProcSessionInput *vprocSessionInputEnsure(VProcSessionStdio *session, int shell_pid, int kernel_pid) {
    if (!session || session->stdin_host_fd < 0) {
        return NULL;
    }
    pthread_mutex_lock(&gSessionInputInitMu);
    if (!session->input) {
        session->input = (VProcSessionInput *)calloc(1, sizeof(VProcSessionInput));
        if (session->input) {
            pthread_mutex_init(&session->input->mu, NULL);
            pthread_cond_init(&session->input->cv, NULL);
            session->input->inited = true;
        }
    }
    VProcSessionInput *input = session->input;
    if (input && !input->reader_active) {
        VProcSessionInputCtx *ctx = (VProcSessionInputCtx *)calloc(1, sizeof(VProcSessionInputCtx));
        if (ctx) {
            ctx->session = session;
            ctx->shell_pid = shell_pid;
            ctx->kernel_pid = kernel_pid;
            pthread_t tid;
            if (vprocHostPthreadCreate(&tid, NULL, vprocSessionInputThread, ctx) == 0) {
                pthread_detach(tid);
                input->reader_active = true;
            } else {
                free(ctx);
            }
        }
    }
    pthread_mutex_unlock(&gSessionInputInitMu);
    return input;
}

static ssize_t vprocSessionReadInput(VProcSessionStdio *session, void *buf, size_t count) {
    if (!session || !session->input || !buf || count == 0) {
        return 0;
    }
    VProcSessionInput *input = session->input;
    pthread_mutex_lock(&input->mu);
    while (input->len == 0 && !input->eof) {
        pthread_cond_wait(&input->cv, &input->mu);
    }
    if (input->len == 0 && input->eof) {
        pthread_mutex_unlock(&input->mu);
        return 0;
    }
    size_t to_copy = (count < input->len) ? count : input->len;
    memcpy(buf, input->buf, to_copy);
    input->len -= to_copy;
    if (input->len > 0) {
        memmove(input->buf, input->buf + to_copy, input->len);
    }
    pthread_mutex_unlock(&input->mu);
    return (ssize_t)to_copy;
}

static void vprocRemoveChildLocked(VProcTaskEntry *parent, int child_pid) {
    if (!parent || !parent->children || parent->child_count == 0) return;
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (parent->children[i] == child_pid) {
            parent->children[i] = parent->children[parent->child_count - 1];
            parent->child_count--;
            break;
        }
    }
}

static void vprocUpdateParentLocked(int child_pid, int new_parent_pid) {
    if (child_pid <= 0) return;
    VProcTaskEntry *child_entry = vprocTaskFindLocked(child_pid);
    if (!child_entry) return;
    int old_parent = child_entry->parent_pid;
    if (old_parent == new_parent_pid) return;
    if (old_parent > 0) {
        VProcTaskEntry *old_parent_entry = vprocTaskFindLocked(old_parent);
        if (old_parent_entry) {
            vprocRemoveChildLocked(old_parent_entry, child_pid);
        }
    }
    child_entry->parent_pid = new_parent_pid;
    if (new_parent_pid > 0) {
        VProcTaskEntry *new_parent_entry = vprocTaskEnsureSlotLocked(new_parent_pid);
        child_entry = vprocTaskFindLocked(child_pid);
        if (!child_entry) return;
        child_entry->parent_pid = new_parent_pid;
        if (new_parent_entry) {
            vprocAddChildLocked(new_parent_entry, child_pid);
        }
    }
}

static void vprocReparentChildrenLocked(int parent_pid, int new_parent_pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(parent_pid);
    if (!entry || entry->child_count == 0 || !entry->children) {
        if (entry) {
            entry->child_count = 0;
        }
        return;
    }
    size_t count = entry->child_count;
    int *children = (int *)calloc(count, sizeof(int));
    if (!children) {
        return;
    }
    memcpy(children, entry->children, count * sizeof(int));
    for (size_t i = 0; i < count; ++i) {
        int child_pid = children[i];
        if (child_pid > 0) {
            vprocUpdateParentLocked(child_pid, new_parent_pid);
        }
    }
    free(children);
    entry = vprocTaskFindLocked(parent_pid);
    if (entry) {
        entry->child_count = 0;
    }
}

static void vprocNotifyParentSigchldLocked(int parent_pid, VProcSigchldEvent evt) {
    if (parent_pid <= 0) return;
    VProcTaskEntry *parent_entry = vprocTaskFindLocked(parent_pid);
    if (!parent_entry) {
        return;
    }
    if (evt == VPROC_SIGCHLD_EVENT_STOP) {
        struct sigaction sa = vprocGetSigactionLocked(parent_entry, SIGCHLD);
        if (sa.sa_flags & SA_NOCLDSTOP) {
            return;
        }
    }
    parent_entry->sigchld_events++;
    vprocQueuePendingSignalLocked(parent_entry, SIGCHLD);
    if (!parent_entry->sigchld_blocked) {
        vprocDeliverPendingSignalsLocked(parent_entry);
    }
}

static int vprocWaiterPid(void) {
    VProc *cur = vprocCurrent();
    if (cur) {
        return vprocPid(cur);
    }
    int shell = vprocGetShellSelfPid();
    if (shell > 0) {
        return shell;
    }
    return (int)getpid();
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

static int vprocSelectHostFd(VProc *inherit_from, int option_fd, int stdno) {
    /* Explicit host fd provided: clone it. */
    if (option_fd >= 0) {
        return vprocCloneFd(option_fd);
    }
    /* Force /dev/null. */
    if (option_fd == -2) {
        int flags = (stdno == STDIN_FILENO) ? O_RDONLY : O_WRONLY;
        return open("/dev/null", flags);
    }
    /* Otherwise inherit from the active vproc's mapping, falling back to host stdno. */
    int source = stdno;
    if (inherit_from) {
        int translated = vprocTranslateFd(inherit_from, stdno);
        if (translated >= 0) {
            source = translated;
        }
    }
    return vprocCloneFd(source);
}

int vprocReservePid(void) {
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    int pid = __sync_fetch_and_add(&gNextSyntheticPid, 1);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        int parent_pid = vprocDefaultParentPid();
        if (parent_pid > 0 && parent_pid != pid) {
            (void)vprocTaskEnsureSlotLocked(parent_pid);
        }
        VProcTaskEntry *parent_entry = vprocTaskFindLocked(parent_pid);
        vprocClearEntryLocked(entry);
        entry = vprocTaskFindLocked(pid);
        if (entry) {
            vprocInitEntryDefaultsLocked(entry, pid, parent_entry);
            entry->parent_pid = parent_pid;
            /* Reserve creates a brand-new process group; do not inherit the shell's
             * pgid/fg_pgid or later kill/pgid lookups will miss the pre-start task. */
            entry->pgid = pid;
            entry->fg_pgid = pid;
            if (parent_pid > 0 && parent_pid != pid) {
                VProcTaskEntry *parent = vprocTaskFindLocked(parent_pid);
                if (parent) {
                    vprocAddChildLocked(parent, pid);
                }
            }
        }
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

static void vprocTaskTableRepairLocked(void) {
    if (gVProcTasks.items != gVProcTasksItemsStable) {
        if (getenv("PSCALI_VPROC_DEBUG")) {
            fprintf(stderr, "[vproc] task table pointer mismatch; repairing\n");
        }
        gVProcTasks.items = gVProcTasksItemsStable;
        gVProcTasks.capacity = gVProcTasksCapacityStable;
    }
    if (!gVProcTasks.items || gVProcTasks.capacity == 0) {
        gVProcTasks.items = NULL;
        gVProcTasks.count = 0;
        gVProcTasks.capacity = 0;
    } else if (gVProcTasks.count > gVProcTasks.capacity) {
        gVProcTasks.count = gVProcTasks.capacity;
    }
}

static VProcTaskEntry *vprocTaskFindLocked(int pid) {
    vprocTaskTableRepairLocked();
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
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid > 0 && parent_pid != pid) {
        (void)vprocTaskEnsureSlotLocked(parent_pid);
    }
    const VProcTaskEntry *parent_entry = vprocTaskFindLocked(parent_pid);
    if (gVProcTasks.count >= gVProcTasks.capacity) {
        size_t new_cap = gVProcTasks.capacity ? (gVProcTasks.capacity * 2) : 8;
        VProcTaskEntry *resized = realloc(gVProcTasks.items, new_cap * sizeof(VProcTaskEntry));
        if (!resized) {
            return NULL;
        }
        gVProcTasks.items = resized;
        gVProcTasks.capacity = new_cap;
        gVProcTasksItemsStable = gVProcTasks.items;
        gVProcTasksCapacityStable = gVProcTasks.capacity;
    }
    entry = &gVProcTasks.items[gVProcTasks.count++];
    vprocInitEntryDefaultsLocked(entry, pid, parent_entry);
    entry->parent_pid = parent_pid;
    if (entry->parent_pid > 0 && entry->parent_pid != pid) {
        VProcTaskEntry *parent = vprocTaskFindLocked(entry->parent_pid);
        if (parent) {
            vprocAddChildLocked(parent, pid);
        }
    }
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
    opts.job_id = 0;
    return opts;
}

VProc *vprocCreate(const VProcOptions *opts) {
    VProcOptions local = opts ? *opts : vprocDefaultOptions();
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    bool vproc_dbg = getenv("PSCALI_VPROC_DEBUG") != NULL;
    VProc *active = vprocCurrent();
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
        int parent_pid = vprocDefaultParentPid();
        if (parent_pid > 0 && parent_pid != vp->pid) {
            (void)vprocTaskEnsureSlotLocked(parent_pid);
        }
        VProcTaskEntry *parent_entry = vprocTaskFindLocked(parent_pid);
        vprocClearEntryLocked(slot);
        slot = vprocTaskFindLocked(vp->pid);
        if (slot) {
            vprocInitEntryDefaultsLocked(slot, vp->pid, parent_entry);
            vprocUpdateParentLocked(vp->pid, parent_pid);
            if (local.job_id > 0) {
                slot->job_id = local.job_id;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);

    int stdin_src = vprocSelectHostFd(active, local.stdin_fd, STDIN_FILENO);
    if (stdin_src < 0 && local.stdin_fd != -2) {
        stdin_src = open("/dev/null", O_RDONLY);
        if (vproc_dbg && stdin_src < 0) {
            fprintf(stderr, "[vproc] stdin clone failed fd=%d err=%s\n",
                    (local.stdin_fd >= 0) ? local.stdin_fd : STDIN_FILENO, strerror(errno));
        }
    }
    int stdout_src = vprocSelectHostFd(active, local.stdout_fd, STDOUT_FILENO);
    if (stdout_src < 0) {
        stdout_src = open("/dev/null", O_WRONLY);
        if (vproc_dbg && stdout_src < 0) {
            fprintf(stderr, "[vproc] stdout clone failed fd=%d err=%s\n",
                    (local.stdout_fd >= 0) ? local.stdout_fd : STDOUT_FILENO, strerror(errno));
        }
    }
    int stderr_src = vprocSelectHostFd(active, local.stderr_fd, STDERR_FILENO);
    if (stderr_src < 0) {
        stderr_src = open("/dev/null", O_WRONLY);
        if (vproc_dbg && stderr_src < 0) {
            fprintf(stderr, "[vproc] stderr clone failed fd=%d err=%s\n",
                    (local.stderr_fd >= 0) ? local.stderr_fd : STDERR_FILENO, strerror(errno));
        }
    }

    if (stdin_src < 0 || stdout_src < 0 || stderr_src < 0) {
        if (stdin_src >= 0) close(stdin_src);
        if (stdout_src >= 0) close(stdout_src);
        if (stderr_src >= 0) close(stderr_src);
        if (vproc_dbg) {
            fprintf(stderr, "[vproc] create failed stdin=%d stdout=%d stderr=%d\n",
                    stdin_src, stdout_src, stderr_src);
        }
        vprocDestroy(vp);
        return NULL;
    }
    vp->entries[0].host_fd = stdin_src;
    vp->entries[1].host_fd = stdout_src;
    vp->entries[2].host_fd = stderr_src;
    vp->stdin_fd = 0;
    vp->stdout_fd = 1;
    vp->stderr_fd = 2;
    vp->stdin_host_fd = stdin_src;
    vp->stdout_host_fd = stdout_src;
    vp->stderr_host_fd = stderr_src;
    vprocRegistryAdd(vp);
    return vp;
}

void vprocDestroy(VProc *vp) {
    if (!vp) {
        return;
    }
    vprocRegistryRemove(vp);
    // Lock shouldn't strictly be needed here if refcount is 0, but good practice
    pthread_mutex_lock(&vp->mu);
    /* Close only the vproc-owned fds; do not close the saved host stdio fds. */
    for (size_t i = 0; i < vp->capacity; ++i) {
        if (vp->entries[i].host_fd >= 0 &&
            vp->entries[i].host_fd != vp->stdin_host_fd &&
            vp->entries[i].host_fd != vp->stdout_host_fd &&
            vp->entries[i].host_fd != vp->stderr_host_fd) {
            close(vp->entries[i].host_fd);
        }
        vp->entries[i].host_fd = -1;
    }
    if (vp->stdin_host_fd >= 0) {
        close(vp->stdin_host_fd);
    }
    if (vp->stdout_host_fd >= 0) {
        close(vp->stdout_host_fd);
    }
    if (vp->stderr_host_fd >= 0) {
        close(vp->stderr_host_fd);
    }
    free(vp->entries);
    vp->entries = NULL;
    if (gVProcCurrent == vp) {
        gVProcCurrent = NULL;
    }
    for (size_t i = 0; i < gVProcStackDepth; ++i) {
        if (gVProcStack[i] == vp) {
            gVProcStack[i] = NULL;
        }
    }
    pthread_mutex_unlock(&vp->mu);
    
    pthread_mutex_destroy(&vp->mu);
    free(vp);
}

void vprocActivate(VProc *vp) {
    if (gVProcCurrent && !vprocRegistryContains(gVProcCurrent)) {
        vprocClearThreadState();
    }
    if (gVProcStackDepth < (sizeof(gVProcStack) / sizeof(gVProcStack[0]))) {
        gVProcStack[gVProcStackDepth++] = gVProcCurrent;
    }
    gVProcCurrent = vp;
}

void vprocDeactivate(void) {
    if (gVProcStackDepth > 0) {
        gVProcCurrent = gVProcStack[gVProcStackDepth - 1];
        gVProcStack[gVProcStackDepth - 1] = NULL;
        gVProcStackDepth--;
    } else {
        gVProcCurrent = NULL;
    }
}

VProc *vprocCurrent(void) {
    return vprocForThread();
}

void vprocDiscard(int pid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        vprocClearEntryLocked(entry);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocTerminateSession(int sid) {
    if (sid <= 0) {
        return;
    }
    pthread_t self = pthread_self();
    pthread_t *cancel = NULL;
    size_t cancel_count = 0;
    size_t cancel_cap = 0;
    size_t target_count = 0;
    int *target_pids = NULL;

    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->sid != sid) continue;
        target_count++;
    }
    if (target_count > 0) {
        target_pids = (int *)calloc(target_count, sizeof(int));
    }
    size_t target_index = 0;
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->sid != sid) continue;

        vprocMaybeStampRusageLocked(entry);
        entry->exit_signal = SIGKILL;
        entry->status = W_EXITCODE(128 + SIGKILL, 0);
        entry->exited = true;
        entry->zombie = false;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        vprocNotifyParentSigchldLocked(entry->parent_pid, VPROC_SIGCHLD_EVENT_EXIT);

        if (entry->tid && !pthread_equal(entry->tid, self)) {
            vprocCancelListAdd(&cancel, &cancel_count, &cancel_cap, entry->tid);
        }
        for (size_t t = 0; t < entry->thread_count; ++t) {
            pthread_t tid = entry->threads[t];
            if (tid && !pthread_equal(tid, self)) {
                vprocCancelListAdd(&cancel, &cancel_count, &cancel_cap, tid);
            }
        }
        if (target_pids && target_index < target_count) {
            target_pids[target_index++] = entry->pid;
        }
    }
    for (size_t i = 0; i < target_index; ++i) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target_pids[i]);
        if (entry) {
            vprocClearEntryLocked(entry);
        }
    }
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    for (size_t i = 0; i < cancel_count; ++i) {
        pthread_cancel(cancel[i]);
    }
    free(cancel);
    free(target_pids);
}

static void *vprocThreadTrampoline(void *arg) {
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)arg;

    if (ctx && ctx->detach) {
        pthread_detach(pthread_self());
    }

    if (ctx) {
        vprocSetShellSelfPid(ctx->shell_self_pid);
        vprocSetKernelPid(ctx->kernel_pid);
    }

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
    ctx->shell_self_pid = vprocGetShellSelfPid();
    ctx->kernel_pid = vprocGetKernelPid();
    ctx->detach = false;
    if (attr) {
        int detach_state = 0;
        if (pthread_attr_getdetachstate(attr, &detach_state) == 0 &&
            detach_state == PTHREAD_CREATE_DETACHED) {
            ctx->detach = true;
        }
    }
    return pthread_create(thread, attr, vprocThreadTrampoline, ctx);
}

int vprocTranslateFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (!vprocRegistryContains(vp)) {
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

int vprocHostDup2(int host_fd, int target_fd) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef dup2
#undef dup2
    int res = dup2(host_fd, target_fd);
#define dup2 vprocDup2Shim
#else
    int res = dup2(host_fd, target_fd);
#endif
    return res;
#else
    return dup2(host_fd, target_fd);
#endif
}

int vprocHostDup(int fd) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef dup
#undef dup
    int res = dup(fd);
#define dup vprocDupShim
#else
    int res = dup(fd);
#endif
    return res;
#else
    return dup(fd);
#endif
}

int vprocHostOpen(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
    return vprocHostOpenVirtualized(path, flags, mode);
#else
    return open(path, flags, mode);
#endif
}

int vprocHostPipe(int pipefd[2]) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef pipe
#undef pipe
    int rc = pipe(pipefd);
#define pipe vprocPipeShim
#else
    int rc = pipe(pipefd);
#endif
    return rc;
#else
    return pipe(pipefd);
#endif
}

off_t vprocHostLseek(int fd, off_t offset, int whence) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef lseek
#undef lseek
    off_t res = lseek(fd, offset, whence);
#define lseek vprocLseekShim
#else
    off_t res = lseek(fd, offset, whence);
#endif
    return res;
#else
    return lseek(fd, offset, whence);
#endif
}

int vprocHostClose(int fd) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef close
#undef close
    int res = close(fd);
#define close vprocCloseShim
#else
    int res = close(fd);
#endif
    return res;
#else
    return close(fd);
#endif
}

ssize_t vprocHostRead(int fd, void *buf, size_t count) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef read
#undef read
    ssize_t res = read(fd, buf, count);
#define read vprocReadShim
#else
    ssize_t res = read(fd, buf, count);
#endif
    return res;
#else
    return read(fd, buf, count);
#endif
}

ssize_t vprocHostWrite(int fd, const void *buf, size_t count) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef write
#undef write
    ssize_t res = write(fd, buf, count);
#define write vprocWriteShim
#else
    ssize_t res = write(fd, buf, count);
#endif
    return res;
#else
    return write(fd, buf, count);
#endif
}

int vprocHostPthreadCreate(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#ifdef pthread_create
#undef pthread_create
    int rc = pthread_create(thread, attr, start_routine, arg);
#define pthread_create vprocPthreadCreateShim
#else
    int rc = pthread_create(thread, attr, start_routine, arg);
#endif
    return rc;
#else
    return pthread_create(thread, attr, start_routine, arg);
#endif
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
        bool preserve_controlling_stdin =
            (target == STDIN_FILENO) && (vp->entries[target].host_fd == vp->stdin_host_fd);
        if (!preserve_controlling_stdin) {
            close(vp->entries[target].host_fd);
        }
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

/* Re-sync the vproc fd table to match a host fd already duplicated onto
 * target_fd at the OS level. This is used when shellRestoreExecRedirections
 * has performed a host dup2 and we need the vproc view to follow suit. */
int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src) {
    if (!vp || target_fd < 0 || host_src < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)target_fd >= vp->capacity) {
        size_t new_cap = vp->capacity ? vp->capacity : VPROC_INITIAL_CAPACITY;
        while ((size_t)target_fd >= new_cap) {
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
    if (vp->entries[target_fd].host_fd >= 0 &&
        !(target_fd == STDIN_FILENO && vp->entries[target_fd].host_fd == vp->stdin_host_fd)) {
        close(vp->entries[target_fd].host_fd);
    }
    int cloned = vprocCloneFd(host_src);
    if (cloned < 0) {
        pthread_mutex_unlock(&vp->mu);
        return -1;
    }
    vp->entries[target_fd].host_fd = cloned;
    pthread_mutex_unlock(&vp->mu);
    return target_fd;
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
    bool dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    int host_fd = vprocHostOpenVirtualized(path, flags, mode);
#if defined(PSCAL_TARGET_IOS)
    if (host_fd < 0 && errno == ENOENT) {
        if (dbg) fprintf(stderr, "[vproc-open] virtualized ENOENT for %s, fallback raw\n", path);
        /* Fallback to raw open so pipelines can read plain files even when
         * virtualization rejects the path. This mirrors shell expectations
         * for cat/head pipelines on iOS. */
        host_fd = open(path, flags, mode);
    }
    if (dbg && host_fd >= 0) {
        fprintf(stderr, "[vproc-open] opened %s -> fd=%d flags=0x%x\n", path, host_fd, flags);
    }
#endif
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

int vprocRegisterTidHint(int pid, pthread_t tid) {
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    bool vdbg = getenv("PSCALI_VPROC_DEBUG") != NULL;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        if (vdbg) {
            fprintf(stderr, "[vproc] register tid hint failed pid=%d tid=%p\n", pid, (void *)tid);
        }
        return -1;
    }
    entry->tid = tid;
    bool duplicate = false;
    for (size_t i = 0; i < entry->thread_count; ++i) {
        if (pthread_equal(entry->threads[i], tid)) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        if (entry->thread_count >= entry->thread_capacity) {
            size_t new_cap = entry->thread_capacity ? entry->thread_capacity * 2 : 4;
            pthread_t *resized = realloc(entry->threads, new_cap * sizeof(pthread_t));
            if (!resized) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = ENOMEM;
                return -1;
            }
            entry->threads = resized;
            entry->thread_capacity = new_cap;
        }
        entry->threads[entry->thread_count++] = tid;
    }
    vprocMaybeUpdateThreadNameLocked(entry);
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (vdbg) {
        fprintf(stderr, "[vproc] register tid hint pid=%d tid=%p thread_count=%zu\n",
                pid, (void *)tid, entry->thread_count);
    }
    return pid;
}

int vprocRegisterThread(VProc *vp, pthread_t tid) {
    if (!vp || vp->pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    bool vdbg = getenv("PSCALI_VPROC_DEBUG") != NULL;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(vp->pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        if (vdbg) {
            fprintf(stderr, "[vproc] register thread failed pid=%d tid=%p\n", vp->pid, (void *)tid);
        }
        return -1;
    }
    entry->tid = tid;
    bool duplicate = false;
    for (size_t i = 0; i < entry->thread_count; ++i) {
        if (pthread_equal(entry->threads[i], tid)) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate) {
        if (entry->thread_count >= entry->thread_capacity) {
            size_t new_cap = entry->thread_capacity ? entry->thread_capacity * 2 : 4;
            pthread_t *resized = realloc(entry->threads, new_cap * sizeof(pthread_t));
            if (!resized) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = ENOMEM;
                return -1;
            }
            entry->threads = resized;
            entry->thread_capacity = new_cap;
        }
        entry->threads[entry->thread_count++] = tid;
    }
    vprocMaybeUpdateThreadNameLocked(entry);
    if (vdbg) {
        fprintf(stderr, "[vproc] register thread pid=%d tid=%p thread_count=%zu\n",
                vp->pid, (void *)tid, entry->thread_count);
    }
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
        if (entry->exit_signal == 0) {
            entry->status = status;
        }
        vprocMaybeStampRusageLocked(entry);
        entry->exited = true;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        entry->zombie = true;
        int adopt_parent = vprocAdoptiveParentPidLocked(entry);
        vprocReparentChildrenLocked(vp->pid, adopt_parent);
        entry = vprocTaskFindLocked(vp->pid);
        if (!entry) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            return;
        }

        bool discard_zombie = false;
        VProcTaskEntry *parent_entry = vprocTaskFindLocked(entry->parent_pid);
        if (parent_entry) {
            struct sigaction sa = vprocGetSigactionLocked(parent_entry, SIGCHLD);
            if (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT)) {
                discard_zombie = true;
            }
        }
        if (discard_zombie) {
            entry->zombie = false;
            vprocClearEntryLocked(entry);
        } else {
            vprocNotifyParentSigchldLocked(entry->parent_pid, VPROC_SIGCHLD_EVENT_EXIT);
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocMarkGroupExit(int pid, int status) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        int pgid = entry->pgid;
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (!peer || peer->pid <= 0) continue;
            if (peer->pgid != pgid) continue;
            vprocMaybeStampRusageLocked(peer);
            peer->group_exit = true;
            peer->group_exit_code = status;
            peer->exited = true;
            peer->zombie = true;
            vprocNotifyParentSigchldLocked(peer->parent_pid, VPROC_SIGCHLD_EVENT_EXIT);
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetParent(int pid, int parent_pid) {
    bool dbg = getenv("PSCALI_VPROC_DEBUG") != NULL;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        if (dbg) {
            fprintf(stderr, "[vproc-parent] pid=%d old=%d new=%d\n",
                    pid, entry->parent_pid, parent_pid);
        }
    vprocUpdateParentLocked(pid, parent_pid);
    } else if (dbg) {
        fprintf(stderr, "[vproc-parent] pid=%d not found; new=%d\n", pid, parent_pid);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocSetPgid(int pid, int pgid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (pgid <= 0) {
        pgid = pid;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        /* Session leader cannot move to a different pgid. */
        if (entry->session_leader && entry->pid == entry->sid && entry->pgid != pgid) {
            errno = EPERM;
            rc = -1;
            goto out;
        }
        /* Ensure pgid belongs to same session if it already exists. */
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (!peer || peer->pid <= 0 || peer->pgid != pgid) continue;
            if (peer->sid != entry->sid) {
                errno = EPERM;
                rc = -1;
                goto out;
            }
        }
        entry->pgid = pgid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
out:
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
        entry->pgid = sid;
        entry->session_leader = (pid == sid);
        entry->fg_pgid = sid;
        entry->blocked_signals = 0;
        entry->pending_signals = 0;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocSetStopUnsupported(int pid, bool stop_unsupported) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->stop_unsupported = stop_unsupported;
        if (stop_unsupported && entry->stopped) {
            entry->stopped = false;
            entry->continued = true;
            entry->stop_signo = 0;
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocGetPgid(int pid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
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

int vprocSetForegroundPgid(int sid, int fg_pgid) {
    if (sid <= 0 || fg_pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *leader = NULL;
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->sid == sid && entry->session_leader) {
            leader = entry;
            break;
        }
    }
    if (!leader) {
        leader = vprocTaskEnsureSlotLocked(sid);
        if (leader) {
            leader->sid = sid;
            leader->pid = sid;
            leader->session_leader = true;
        }
    }
    if (leader) {
        leader->fg_pgid = fg_pgid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocGetForegroundPgid(int sid) {
    if (sid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int fg = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->sid == sid && entry->session_leader) {
            fg = entry->fg_pgid;
            break;
        }
    }
    if (fg < 0) errno = ESRCH;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return fg;
}

static void vprocClearEntryLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    int pid = entry->pid;
    int parent_pid = entry->parent_pid;
    if (parent_pid > 0 && pid > 0) {
        VProcTaskEntry *parent = vprocTaskFindLocked(parent_pid);
        if (parent) {
            vprocRemoveChildLocked(parent, pid);
        }
    }
    int adopt_parent = vprocAdoptiveParentPidLocked(entry);
    vprocReparentChildrenLocked(pid, adopt_parent);

    entry = vprocTaskFindLocked(pid);
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
    entry->continued = false;
    entry->stop_signo = 0;
    entry->zombie = false;
    entry->stop_unsupported = false;
    entry->job_id = 0;
    if (entry->children) {
        free(entry->children);
    }
    if (entry->threads) {
        free(entry->threads);
    }
    entry->threads = NULL;
    entry->thread_capacity = 0;
    entry->thread_count = 0;
    entry->session_leader = false;
    entry->fg_pgid = 0;
    entry->sigchld_events = 0;
    entry->sigchld_blocked = false;
    entry->children = NULL;
    entry->rusage_utime = 0;
    entry->rusage_stime = 0;
    entry->group_exit = false;
    entry->group_exit_code = 0;
    entry->blocked_signals = 0;
    entry->pending_signals = 0;
    entry->ignored_signals = 0;
    for (int i = 0; i < 32; ++i) {
        entry->pending_counts[i] = 0;
    }
    for (int i = 0; i < 32; ++i) {
        entry->pending_counts[i] = 0;
    }
    entry->fg_override_pgid = 0;
    for (int i = 0; i < 32; ++i) {
        sigemptyset(&entry->actions[i].sa_mask);
        entry->actions[i].sa_handler = SIG_DFL;
        entry->actions[i].sa_flags = 0;
    }
    entry->child_capacity = 0;
    entry->child_count = 0;
    entry->start_mono_ns = 0;
}

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) {
    size_t count = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) {
            continue;
        }
        uint64_t now = vprocNowMonoNs();
        if (out && count < capacity) {
            int fg_for_session = (entry->sid > 0) ? vprocForegroundPgidLocked(entry->sid) : -1;
            int utime = entry->rusage_utime;
            int stime = entry->rusage_stime;
            int cpu_utime = 0;
            int cpu_stime = 0;
            if (vprocComputeCpuTimesLocked(entry, &cpu_utime, &cpu_stime)) {
                if (cpu_utime > utime) {
                    utime = cpu_utime;
                }
                if (cpu_stime > stime) {
                    stime = cpu_stime;
                }
            } else if (!entry->exited && utime == 0 && stime == 0) {
                int live = vprocRuntimeCenti(entry, now);
                if (live > utime) {
                    utime = live;
                }
                if (live / 10 > stime) {
                    stime = live / 10;
                }
            }
            out[count].pid = entry->pid;
            out[count].tid = entry->tid;
            out[count].parent_pid = entry->parent_pid;
            out[count].pgid = entry->pgid;
            out[count].sid = entry->sid;
            out[count].exited = entry->exited;
            out[count].stopped = entry->stopped;
            out[count].continued = entry->continued;
            out[count].zombie = entry->zombie;
            out[count].exit_signal = entry->exit_signal;
            out[count].status = entry->status;
            out[count].stop_signo = entry->stop_signo;
            out[count].sigchld_pending = entry->sigchld_events > 0;
            out[count].rusage_utime = utime;
            out[count].rusage_stime = stime;
            out[count].fg_pgid = (fg_for_session > 0) ? fg_for_session : entry->fg_pgid;
            out[count].job_id = entry->job_id;
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
    int waiter_pid = vprocWaiterPid();
    int waiter_pgid = -1;
    if (pid == 0) {
        waiter_pgid = vprocGetPgid(waiter_pid);
    }

    pthread_mutex_lock(&gVProcTasks.mu);
    while (true) {
        VProcTaskEntry *ready = NULL;
        bool has_candidate = false;

        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (!entry || entry->pid <= 0) continue;
            if (entry->parent_pid != waiter_pid) continue;

            bool match = false;
            if (pid > 0 && entry->pid == pid) {
                match = true;
            } else if (pid == -1) {
                match = true;
            } else if (pid == 0) {
                match = (waiter_pgid > 0) ? (entry->pgid == waiter_pgid) : true;
            } else if (pid < -1) {
                match = (entry->pgid == -pid);
            }

            if (!match) continue;
            has_candidate = true;

            bool state_change = false;
            if (entry->exited) {
                state_change = true;
            } else if (allow_stop && entry->stopped && entry->stop_signo > 0) {
                state_change = true;
            } else if (allow_cont && entry->continued) {
                state_change = true;
            }

            if (state_change) {
                ready = entry;
                break;
            }
        }

        if (ready) {
            int status = 0;
            int waited_pid = ready->pid;
            VProcTaskEntry *waiter_entry = vprocTaskFindLocked(waiter_pid);

            if (ready->exited) {
                if (ready->group_exit) {
                    status = W_EXITCODE(ready->group_exit_code & 0xff, 0);
                } else if (ready->exit_signal > 0) {
                    status = (ready->exit_signal & 0x7f);
                } else {
                    status = W_EXITCODE(ready->status & 0xff, 0);
                }
            } else if (ready->stopped && ready->stop_signo > 0) {
                status = W_STOPCODE(ready->stop_signo & 0xff);
            } else if (ready->continued) {
                status = W_STOPCODE(SIGCONT);
            }

            if (status_out) {
                *status_out = status;
            }

            if (ready->exited && !nowait) {
                vprocClearEntryLocked(ready);
            } else if (ready->exited) {
                ready->zombie = true;
            } else if (ready->stopped) {
                ready->stop_signo = 0; // Clear stop signal after reporting
            } else if (ready->continued) {
                ready->continued = false;
            }
            if (waiter_entry && waiter_entry->sigchld_events > 0 && !waiter_entry->sigchld_blocked) {
                waiter_entry->sigchld_events--;
            }

            if (dbg) {
                fprintf(stderr, "[vproc-wait] pid=%d status=%d exited=%d stop=%d\n",
                        waited_pid, status, ready->exited, ready->stopped);
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
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = ECHILD;
            return -1;
        }

        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
}

static void vprocCancelListAdd(pthread_t **list, size_t *count, size_t *capacity, pthread_t tid) {
    if (!list || !count || !capacity) {
        return;
    }
    if (tid == 0) {
        return;
    }
    for (size_t i = 0; i < *count; ++i) {
        if (pthread_equal((*list)[i], tid)) {
            return;
        }
    }
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        pthread_t *resized = (pthread_t *)realloc(*list, new_cap * sizeof(pthread_t));
        if (!resized) {
            return;
        }
        *list = resized;
        *capacity = new_cap;
    }
    (*list)[(*count)++] = tid;
}

int vprocKillShim(pid_t pid, int sig) {
    bool target_group = (pid <= 0);
    bool broadcast_all = (pid == -1);
    int target = target_group ? -pid : pid;
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    if (sig == 0) {
        /* Probe for existence: succeed if we find a matching entry. */
        pthread_mutex_lock(&gVProcTasks.mu);
        bool found = false;
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (!entry || entry->pid <= 0) continue;
            if (broadcast_all) {
                found = true;
                break;
            } else if (target_group) {
                if (entry->pgid == target) {
                    found = true;
                    break;
                }
            } else if (entry->pid == target) {
                found = true;
                break;
            }
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (found) {
            return 0;
        }
        errno = ESRCH;
        return -1;
    }

    if (sig < 0 || sig >= 32) {
        if (dbg) {
            fprintf(stderr, "[vproc-kill] invalid signal=%d\n", sig);
        }
        errno = EINVAL;
        return -1;
    }

    if (pid == 0) {
        int caller = vprocGetPidShim();
        if (caller <= 0) {
            caller = vprocGetShellSelfPid();
        }
        int caller_pgid = (caller > 0) ? vprocGetPgid(caller) : -1;
        if (caller_pgid <= 0) {
            return kill(pid, sig);
        }
        target_group = true;
        target = caller_pgid;
    }

    int rc = 0;
    pthread_t *cancel_list = NULL;
    size_t cancel_count = 0;
    size_t cancel_capacity = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    if (dbg) {
        fprintf(stderr, "[vproc-kill] target=%d group=%d broadcast=%d count=%zu\n",
                target, (int)target_group, (int)broadcast_all, gVProcTasks.count);
    }
    bool delivered = false;
    
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->zombie || entry->exited) continue;
        if (entry->exited && entry->zombie) continue;
        if (dbg) {
            fprintf(stderr, "[vproc-kill] scan pid=%d pgid=%d sid=%d exited=%d zombie=%d\n",
                    entry->pid, entry->pgid, entry->sid, entry->exited, entry->zombie);
        }
        
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

        if (vprocSignalBlockedLocked(entry, sig)) {
            vprocQueuePendingSignalLocked(entry, sig);
            continue;
        }

        vprocApplySignalLocked(entry, sig);

        if (entry->exited) {
            vprocCancelListAdd(&cancel_list, &cancel_count, &cancel_capacity, entry->tid);
            for (size_t t = 0; t < entry->thread_count; ++t) {
                vprocCancelListAdd(&cancel_list, &cancel_count, &cancel_capacity, entry->threads[t]);
            }
        }
    }
    
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    for (size_t i = 0; i < cancel_count; ++i) {
        pthread_cancel(cancel_list[i]);
    }
    free(cancel_list);

    if (delivered) return rc;

    if (dbg) {
        fprintf(stderr, "[vproc-kill] no targets pid=%d target=%d group=%d broadcast=%d\n",
                (int)pid, target, (int)target_group, (int)broadcast_all);
    }
    errno = ESRCH;
    return -1;
}

/* ... (Remaining functions for getters/setters/shell-self-pid are unchanged logically) ... */
/* INCLUDED HERE FOR COMPLETENESS */

pid_t vprocGetPidShim(void) {
    VProc *vp = vprocCurrent();
    if (vp) {
        return vprocPid(vp);
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        return shell_pid;
    }
    return getpid();
}

static inline bool vprocShimHasVirtualContext(void) {
    return vprocCurrent() != NULL || vprocGetShellSelfPid() > 0;
}

pid_t vprocGetPpidShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return getppid();
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    pid_t parent = (pid_t)-1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        parent = (pid_t)entry->parent_pid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return parent;
}

bool vprocCommandScopeBegin(VProcCommandScope *scope,
                            const char *label,
                            bool force_new_vproc,
                            bool inherit_parent_pgid) {
    if (!scope) {
        return false;
    }
    memset(scope, 0, sizeof(*scope));
    scope->prev = vprocCurrent();

    int shell_pid = vprocGetShellSelfPid();
    bool need_new = force_new_vproc ||
                    scope->prev == NULL ||
                    (shell_pid > 0 && scope->prev && vprocPid(scope->prev) == shell_pid);
    if (!need_new) {
        return false;
    }

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    if (scope->prev) {
        int host_in = vprocTranslateFd(scope->prev, STDIN_FILENO);
        int host_out = vprocTranslateFd(scope->prev, STDOUT_FILENO);
        int host_err = vprocTranslateFd(scope->prev, STDERR_FILENO);
        if (host_in >= 0) opts.stdin_fd = host_in;
        if (host_out >= 0) opts.stdout_fd = host_out;
        if (host_err >= 0) opts.stderr_fd = host_err;
    } else {
        opts.stdin_fd = STDIN_FILENO;
        opts.stdout_fd = STDOUT_FILENO;
        opts.stderr_fd = STDERR_FILENO;
    }

    VProc *vp = vprocCreate(&opts);
    if (!vp) {
        opts.stdin_fd = -2;
        vp = vprocCreate(&opts);
    }
    if (!vp) {
        return false;
    }

    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);
    scope->vp = vp;
    scope->pid = pid;

    int parent_pid = (int)vprocGetPidShim();
    if (parent_pid > 0 && parent_pid != pid) {
        vprocSetParent(pid, parent_pid);
    }

    if (inherit_parent_pgid) {
        int parent_pgid = (parent_pid > 0) ? vprocGetPgid(parent_pid) : -1;
        if (parent_pgid > 0) {
            vprocSetPgid(pid, parent_pgid);
        } else {
            vprocSetPgid(pid, pid);
        }
    } else {
        vprocSetPgid(pid, pid);
    }

    if (label && *label) {
        vprocSetCommandLabel(pid, label);
    }

    if (vprocIsShellSelfThread()) {
        pthread_mutex_lock(&gVProcTasks.mu);
        VProcTaskEntry *entry = vprocTaskFindLocked(pid);
        if (entry) {
            entry->stop_unsupported = true;
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
    }

    vprocActivate(vp);
    return true;
}

void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code) {
    if (!scope || !scope->vp) {
        return;
    }

    VProc *vp = scope->vp;
    int pid = scope->pid > 0 ? scope->pid : vprocPid(vp);

    vprocDeactivate();
    vprocMarkExit(vp, W_EXITCODE(exit_code & 0xff, 0));
    vprocDiscard(pid);
    vprocDestroy(vp);

    scope->prev = NULL;
    scope->vp = NULL;
    scope->pid = 0;
}

pid_t vprocGetpgrpShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return getpgrp();
    }
    pid_t pid = vprocGetPidShim();
    int pgid = vprocGetPgid((int)pid);
    return (pid_t)pgid;
}

pid_t vprocGetpgidShim(pid_t pid) {
    if (!vprocShimHasVirtualContext()) {
        return getpgid(pid);
    }
    int target = (pid == 0) ? (int)vprocGetPidShim() : (int)pid;
    int pgid = vprocGetPgid(target);
    return (pid_t)pgid;
}

int vprocSetpgidShim(pid_t pid, pid_t pgid) {
    if (!vprocShimHasVirtualContext()) {
        return setpgid(pid, pgid);
    }
    return vprocSetPgid((int)pid, (int)pgid);
}

pid_t vprocGetsidShim(pid_t pid) {
    if (!vprocShimHasVirtualContext()) {
        return getsid(pid);
    }
    int target = (pid == 0) ? (int)vprocGetPidShim() : (int)pid;
    int sid = vprocGetSid(target);
    return (pid_t)sid;
}

pid_t vprocSetsidShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return setsid();
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    pid_t rc = (pid_t)-1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        errno = ESRCH;
        goto out;
    }
    if (entry->pgid == pid) {
        errno = EPERM;
        goto out;
    }
    entry->sid = pid;
    entry->pgid = pid;
    entry->session_leader = true;
    entry->fg_pgid = pid;
    entry->blocked_signals = 0;
    entry->pending_signals = 0;
    rc = (pid_t)pid;
out:
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

pid_t vprocTcgetpgrpShim(int fd) {
    if (!vprocShimHasVirtualContext()) {
        return tcgetpgrp(fd);
    }
    (void)fd;
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    int sid = vprocGetSid(pid);
    if (sid <= 0) {
        errno = ENOTTY;
        return (pid_t)-1;
    }
    int fg = vprocGetForegroundPgid(sid);
    return (pid_t)fg;
}

int vprocTcsetpgrpShim(int fd, pid_t pgid) {
    if (!vprocShimHasVirtualContext()) {
        return tcsetpgrp(fd, pgid);
    }
    (void)fd;
    if (pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int sid = vprocGetSid(pid);
    if (sid <= 0) {
        errno = ENOTTY;
        return -1;
    }

    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *leader = NULL;
    bool group_ok = false;
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) continue;
        if (entry->sid != sid) continue;
        if (entry->session_leader) {
            leader = entry;
        }
        if (entry->pgid == (int)pgid) {
            group_ok = true;
        }
    }
    if (!leader) {
        errno = ESRCH;
        goto out_tcset;
    }
    if (!group_ok) {
        errno = EPERM;
        goto out_tcset;
    }
    leader->fg_pgid = (int)pgid;
    rc = 0;
out_tcset:
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocSetShellSelfPid(int pid) {
    gShellSelfPid = pid;
}

int vprocGetShellSelfPid(void) {
    return gShellSelfPid;
}

void vprocSetShellSelfTid(pthread_t tid) {
    gShellSelfTid = tid;
    gShellSelfTidValid = true;
}

bool vprocIsShellSelfThread(void) {
    return gShellSelfTidValid && pthread_equal(pthread_self(), gShellSelfTid);
}

void vprocSetKernelPid(int pid) {
    gKernelPid = pid;
}

int vprocGetKernelPid(void) {
    return gKernelPid;
}

int vprocGetSessionKernelPid(void) {
    return gSessionStdio.kernel_pid;
}

void vprocSetSessionKernelPid(int pid) {
    gSessionStdio.kernel_pid = pid;
}

VProcSessionStdio *vprocSessionStdioCurrent(void) {
    return &gSessionStdio;
}

void vprocSessionStdioInit(VProcSessionStdio *stdio_ctx, int kernel_pid) {
    if (!stdio_ctx) {
        return;
    }
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->input = NULL;
    /* Duplicate current host stdio so this session owns stable copies. */
    int in = fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 0);
    int out = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 0);
    int err = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 0);
    if (in < 0 && errno == EINVAL) in = dup(STDIN_FILENO);
    if (out < 0 && errno == EINVAL) out = dup(STDOUT_FILENO);
    if (err < 0 && errno == EINVAL) err = dup(STDERR_FILENO);
    if (in >= 0) fcntl(in, F_SETFD, FD_CLOEXEC);
    if (out >= 0) fcntl(out, F_SETFD, FD_CLOEXEC);
    if (err >= 0) fcntl(err, F_SETFD, FD_CLOEXEC);
    stdio_ctx->stdin_host_fd = in;
    stdio_ctx->stdout_host_fd = out;
    stdio_ctx->stderr_host_fd = err;
}

void vprocSessionStdioActivate(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return;
    }
    gSessionStdio = *stdio_ctx;
}

void vprocSetJobId(int pid, int job_id) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
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
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
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

bool vprocSigchldPending(int pid) {
    bool pending = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        pending = entry->sigchld_events > 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pending;
}

int vprocSetSigchldBlocked(int pid, bool block) {
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sigchld_blocked = block;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    if (rc == 0 && !block) {
        vprocDeliverPendingSignalsLocked(entry);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocClearSigchldPending(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sigchld_events = 0;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetRusage(int pid, int utime, int stime) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->rusage_utime = utime;
        entry->rusage_stime = stime;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocBlockSignals(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = ((uint32_t)mask) & ~unmaskable;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->blocked_signals &= ~unmaskable;
        entry->blocked_signals |= mask_bits;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocUnblockSignals(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->blocked_signals &= ~unmaskable;
        entry->blocked_signals &= ~mask_bits;
        vprocDeliverPendingSignalsLocked(entry);
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocIgnoreSignal(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    if (mask_bits & unmaskable) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->ignored_signals &= ~unmaskable;
        entry->ignored_signals |= mask_bits;
        entry->pending_signals &= ~mask_bits;
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t bit = vprocSigMask(sig);
            if (bit & mask_bits) {
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_flags = 0;
                entry->actions[sig].sa_handler = SIG_IGN;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocDefaultSignal(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->ignored_signals &= ~unmaskable;
        entry->ignored_signals &= ~mask_bits;
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t bit = vprocSigMask(sig);
            if (bit & mask_bits) {
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_flags = 0;
                entry->actions[sig].sa_handler = SIG_DFL;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

static struct sigaction vprocGetSigactionLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = SIG_DFL;
        return sa;
    }
    return entry->actions[sig];
}

int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old) {
    if (!vprocSigIndexValid(sig) || sig == SIGKILL || sig == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    uint32_t mask = vprocSigMask(sig);
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        if (old) {
            *old = vprocGetSigactionLocked(entry, sig);
        }
        if (act) {
            entry->actions[sig] = *act;
            if (act->sa_handler == SIG_IGN) {
                entry->ignored_signals |= mask;
                entry->pending_signals &= ~mask;
            } else {
                entry->ignored_signals &= ~mask;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

static VProc *vprocForThread(void) {
    VProc *vp = gVProcCurrent;
    if (!vp) {
        return NULL;
    }
    if (!vprocRegistryContains(vp)) {
        vprocClearThreadState();
        return NULL;
    }
    return vp;
}

int vprocSigpending(int pid, sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    sigemptyset(set);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        uint32_t pending = entry->pending_signals;
        for (int sig = 1; sig < 32; ++sig) {
            if ((pending & vprocSigMask(sig)) || entry->pending_counts[sig] > 0) {
                sigaddset(set, sig);
            }
        }
    } else {
        errno = ESRCH;
        pthread_mutex_unlock(&gVProcTasks.mu);
        return -1;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

int vprocSigsuspend(int pid, const sigset_t *mask) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    uint32_t original_blocked = entry->blocked_signals;
    if (mask) {
        entry->blocked_signals = 0;
        for (int sig = 1; sig < 32; ++sig) {
            if (sigismember(mask, sig)) {
                entry->blocked_signals |= vprocSigMask(sig);
            }
        }
    }
    while (true) {
        uint32_t orig_pending = entry->pending_signals;
        vprocDeliverPendingSignalsLocked(entry);
        if (orig_pending != 0) {
            break;
        }
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
    entry->blocked_signals = original_blocked;
    pthread_mutex_unlock(&gVProcTasks.mu);
    errno = EINTR;
    return -1;
}

int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    if (oldset) {
        sigemptyset(oldset);
        for (int sig = 1; sig < 32; ++sig) {
            if (entry->blocked_signals & vprocSigMask(sig)) {
                sigaddset(oldset, sig);
            }
        }
    }
    if (!set) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return 0;
    }
    uint32_t mask_bits = 0;
    for (int sig = 1; sig < 32; ++sig) {
        if (sigismember(set, sig)) {
            mask_bits |= vprocSigMask(sig);
        }
    }
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    mask_bits &= ~unmaskable;
    if (how == SIG_BLOCK) {
        entry->blocked_signals |= mask_bits;
    } else if (how == SIG_UNBLOCK) {
        entry->blocked_signals &= ~mask_bits;
    } else if (how == SIG_SETMASK) {
        entry->blocked_signals = mask_bits;
    } else {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = EINVAL;
        return -1;
    }
    vprocDeliverPendingSignalsLocked(entry);
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

int vprocSigwait(int pid, const sigset_t *set, int *sig) {
    if (!set || !sig) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    while (true) {
        for (int s = 1; s < 32; ++s) {
            if (!sigismember(set, s)) continue;
            uint32_t bit = vprocSigMask(s);
            if (entry->pending_counts[s] > 0 || (entry->pending_signals & bit)) {
                if (entry->pending_counts[s] > 0) {
                    entry->pending_counts[s]--;
                }
                if (entry->pending_counts[s] <= 0) {
                    entry->pending_signals &= ~bit;
                    entry->pending_counts[s] = 0;
                }
                *sig = s;
                pthread_mutex_unlock(&gVProcTasks.mu);
                return 0;
            }
        }
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
}

int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig) {
    if (!set || !sig) {
        errno = EINVAL;
        return -1;
    }
    struct timespec deadline = {0, 0};
    if (timeout) {
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            return -1;
        }
        deadline.tv_sec = now.tv_sec + timeout->tv_sec;
        deadline.tv_nsec = now.tv_nsec + timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    while (true) {
        for (int s = 1; s < 32; ++s) {
            if (!sigismember(set, s)) continue;
            uint32_t bit = vprocSigMask(s);
            if (entry->pending_counts[s] > 0 || (entry->pending_signals & bit)) {
                if (entry->pending_counts[s] > 0) {
                    entry->pending_counts[s]--;
                }
                if (entry->pending_counts[s] <= 0) {
                    entry->pending_signals &= ~bit;
                    entry->pending_counts[s] = 0;
                }
                *sig = s;
                pthread_mutex_unlock(&gVProcTasks.mu);
                return s;
            }
        }
        if (timeout) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = EAGAIN;
                return -1;
            }
            pthread_cond_timedwait(&gVProcTasks.cv, &gVProcTasks.mu, &deadline);
        } else {
            pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
        }
    }
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
    VProc *vp = vprocForThread();
    bool controlling_stdin = (vp && vp->stdin_host_fd >= 0 && fd == STDIN_FILENO && host == vp->stdin_host_fd);
    if (controlling_stdin) {
        (void)vprocWaitIfStopped(vp);
    }
    if (controlling_stdin && vprocShouldStopForBackgroundTty(vprocCurrent(), SIGTTIN)) {
        errno = EINTR;
        return -1;
    }
    ssize_t res;
    if (controlling_stdin && pscalRuntimeVirtualTTYEnabled()) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && vprocSessionInputEnsure(session, vprocGetShellSelfPid(), vprocGetKernelPid())) {
            res = vprocSessionReadInput(session, buf, count);
        } else {
            res = read(host, buf, count);
        }
    } else {
        res = read(host, buf, count);
    }
    if (res <= 0 || !controlling_stdin || !vp) {
        return res;
    }
    if (!pscalRuntimeVirtualTTYEnabled()) {
        return res;
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0 && vprocPid(vp) == shell_pid) {
        return res;
    }
    bool saw_sigint = false;
    bool saw_sigtstp = false;
    unsigned char *bytes = (unsigned char *)buf;
    for (ssize_t i = 0; i < res; ++i) {
        if (bytes[i] == 3) {
            saw_sigint = true;
        } else if (bytes[i] == 26) {
            saw_sigtstp = true;
        }
    }
    if (!saw_sigint && !saw_sigtstp) {
        return res;
    }
    if (saw_sigint) {
        vprocDispatchControlSignal(vp, SIGINT);
    } else if (saw_sigtstp) {
        vprocDispatchControlSignal(vp, SIGTSTP);
    }
    errno = EINTR;
    return -1;
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
    bool dbg = getenv("PSCALI_PIPE_DEBUG") != NULL;
    int host_fd = vprocHostOpenVirtualized(path, flags, mode);
#if defined(PSCAL_TARGET_IOS)
    if (host_fd < 0 && errno == ENOENT) {
        if (dbg) fprintf(stderr, "[vproc-open] (shim) virtualized ENOENT for %s, fallback raw\n", path);
        host_fd = open(path, flags, mode);
    }
    if (dbg && host_fd >= 0) {
        fprintf(stderr, "[vproc-open] (shim) opened %s -> host_fd=%d flags=0x%x\n", path, host_fd, flags);
    }
#endif
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

int vprocSigactionShim(int sig, const struct sigaction *act, struct sigaction *oldact) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return sigaction(sig, act, oldact);
    }
    return vprocSigaction(vprocPid(vp), sig, act, oldact);
}

int vprocSigprocmaskShim(int how, const sigset_t *set, sigset_t *oldset) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return sigprocmask(how, set, oldset);
    }
    return vprocSigprocmask(vprocPid(vp), how, set, oldset);
}

int vprocSigpendingShim(sigset_t *set) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return sigpending(set);
    }
    return vprocSigpending(vprocPid(vp), set);
}

int vprocSigsuspendShim(const sigset_t *mask) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return sigsuspend(mask);
    }
    return vprocSigsuspend(vprocPid(vp), mask);
}

int vprocPthreadSigmaskShim(int how, const sigset_t *set, sigset_t *oldset) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return pthread_sigmask(how, set, oldset);
    }
    if (vprocSigprocmask(vprocPid(vp), how, set, oldset) == 0) {
        return 0;
    }
    return errno ? errno : EINVAL;
}

int vprocRaiseShim(int sig) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return raise(sig);
    }
    return vprocKillShim(vprocPid(vp), sig);
}

VProcSigHandler vprocSignalShim(int sig, VProcSigHandler handler) {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    if (vprocSigactionShim(sig, &sa, &old) != 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */
