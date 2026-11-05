/* A task's fatal signal to itself acts before the call returns on Linux. Here what that takes depends
 * on the thread: one of its own ends, a shell's or subshell's own code acts between commands, and a
 * program on a thread that has to go on leaves by its frame. A signal to a task that has ended
 * succeeds and does nothing, for a pid, a group and everything, while what Ctrl-C and Ctrl-Z fall back
 * from still sees no one to take it. Each check runs in a child, with a clean table. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) { (void)message; }
#endif
#include "ios/vproc.h"

int pscalTtySendGroupSignal(int pgid, int sig);

static int g_fails = 0;
static size_t g_host_kills = 0;

static int record_kill(pid_t pid, int sig) {
    (void)pid;
    (void)sig;
    g_host_kills++;
    return 0;
}

/* Nothing in vproc reaches the host's kill. If something did, it lands here and signals nothing. */
int kill(pid_t pid, int sig) {
    return record_kill(pid, sig);
}

int killpg(pid_t pgrp, int sig) {
    return record_kill(-pgrp, sig);
}

static void fail(const char *what, const char *detail) {
    fprintf(stderr, "FAIL %s: %s\n", what, detail ? detail : "(null)");
    g_fails++;
}

static void expect(bool ok, const char *what, const char *detail) {
    if (!ok) {
        fail(what, detail);
    }
}

static void expect_kill(const char *what, pid_t pid, int sig, int want_rc, int want_errno) {
    errno = 0;
    int rc = vprocKillShim(pid, sig);
    if (rc != want_rc || (want_rc != 0 && errno != want_errno)) {
        char detail[80];
        snprintf(detail, sizeof(detail), "kill(%d, %d) = %d errno %d", (int)pid, sig, rc, errno);
        fail(what, detail);
    }
}

static void expect_live_kill(const char *what, pid_t pid, int sig, int want_rc, int want_errno) {
    errno = 0;
    int rc = vprocKillLive(pid, sig);
    if (rc != want_rc || (want_rc != 0 && errno != want_errno)) {
        char detail[80];
        snprintf(detail, sizeof(detail), "live kill(%d, %d) = %d errno %d", (int)pid, sig, rc, errno);
        fail(what, detail);
    }
}

static bool snapshot_of(int pid, VProcSnapshot *out) {
    VProcSnapshot snap[64];
    size_t n = vprocSnapshot(snap, 64);
    for (size_t i = 0; i < n; ++i) {
        if (snap[i].pid == pid) {
            *out = snap[i];
            return true;
        }
    }
    return false;
}

static int signal_of(int pid) {
    VProcSnapshot snap;
    return snapshot_of(pid, &snap) ? snap.exit_signal : -1;
}

static bool has_ended(int pid) {
    VProcSnapshot snap;
    return snapshot_of(pid, &snap) && snap.exited;
}

static void expect_signalled(const char *what, int pid, int sig) {
    int status = 0;
    if (vprocWaitPidShim(pid, &status, 0) != pid) {
        fail(what, "wait did not find it");
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != sig) {
        char detail[64];
        snprintf(detail, sizeof(detail), "wait status 0x%x, wanted signal %d", status, sig);
        fail(what, detail);
    }
}

/* This thread is the session's shell. */
static VProc *become_shell(void) {
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fail("vprocCreate", strerror(errno));
        _exit(1);
    }
    int pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    vprocSetShellSelfPid(pid);
    vprocSetShellSelfTid(pthread_self());
    (void)vprocSetSid(pid, pid);
    (void)vprocSetPgid(pid, pid);
    vprocSetUnwindsItself(pid, true);
    return vp;
}

/* A child of this thread's task, in a group of its own unless a pgid is given. */
static VProc *new_task(int pgid) {
    VProc *parent = vprocCurrent();
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fail("vprocCreate", strerror(errno));
        _exit(1);
    }
    int pid = vprocPid(vp);
    if (parent) {
        vprocSetParent(pid, vprocPid(parent));
    }
    if (vprocSetPgid(pid, pgid > 0 ? pgid : pid) != 0) {
        fail("vprocSetPgid", strerror(errno));
    }
    return vp;
}

static volatile int g_caught = 0;
static void on_signal(int sig) {
    (void)sig;
    g_caught++;
}

/* How the task ends itself, on a thread of its own. */
typedef enum { VIA_KILL, VIA_RAISE, VIA_KILL_ZERO, VIA_KILL_GROUP, VIA_UNBLOCK, VIA_SUSPEND } SelfVia;

typedef struct {
    VProc *vp;
    int sig;
    SelfVia via;
    bool returned;
    bool cleaned;
    bool ended_before_cleanup;
} SelfArg;

static void self_cleanup(void *arg) {
    SelfArg *a = (SelfArg *)arg;
    a->cleaned = true;
    a->ended_before_cleanup = has_ended(vprocPid(a->vp));
    vprocDeactivate();
    vprocMarkExit(a->vp, 99);
}

static void *self_main(void *arg) {
    SelfArg *a = (SelfArg *)arg;
    int pid = vprocPid(a->vp);
    vprocRegisterThread(a->vp, pthread_self());
    vprocActivate(a->vp);
    pthread_cleanup_push(self_cleanup, a);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, a->sig);
    switch (a->via) {
        case VIA_KILL:
            (void)vprocKillShim(vprocGetPidShim(), a->sig);
            break;
        case VIA_RAISE:
            (void)vprocRaiseShim(a->sig);
            break;
        case VIA_KILL_ZERO:
            (void)vprocKillShim(0, a->sig);
            break;
        case VIA_KILL_GROUP:
            (void)vprocKillShim(-vprocGetPgid(pid), a->sig);
            break;
        case VIA_UNBLOCK:
            (void)vprocSigprocmaskShim(SIG_BLOCK, &set, NULL);
            if (vprocKillShim(pid, a->sig) != 0) {
                fail("kill while blocked", strerror(errno));
            }
            (void)vprocSigprocmaskShim(SIG_UNBLOCK, &set, NULL);
            break;
        case VIA_SUSPEND: {
            (void)vprocSigprocmaskShim(SIG_BLOCK, &set, NULL);
            if (vprocKillShim(pid, a->sig) != 0) {
                fail("kill while blocked", strerror(errno));
            }
            sigset_t none;
            sigemptyset(&none);
            (void)vprocSigsuspendShim(&none);
            break;
        }
    }
    a->returned = true;
    pthread_cleanup_pop(1);
    return NULL;
}

static void expect_ends_itself(const char *what, SelfVia via, int sig) {
    (void)become_shell();
    VProc *vp = new_task(0);
    int pid = vprocPid(vp);
    SelfArg a = { .vp = vp, .sig = sig, .via = via };
    pthread_t tid;
    if (pthread_create(&tid, NULL, self_main, &a) != 0) {
        fail(what, "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(!a.returned, what, "the call returned");
    expect(a.cleaned, what, "the thread's handlers did not run");
    expect(ret == PTHREAD_CANCELED, what, "the thread did not end as a killed one does");
    expect(!a.ended_before_cleanup, what, "reported ended while its thread still held what it holds");
    expect_signalled(what, pid, sig);
    vprocDestroy(vp);
}

static void own_thread_kill(void) { expect_ends_itself("kill(getpid(), SIGTERM)", VIA_KILL, SIGTERM); }
static void own_thread_sigkill(void) { expect_ends_itself("kill(getpid(), SIGKILL)", VIA_KILL, SIGKILL); }
static void own_thread_sighup(void) { expect_ends_itself("kill(getpid(), SIGHUP)", VIA_KILL, SIGHUP); }
static void own_thread_raise(void) { expect_ends_itself("raise(SIGTERM)", VIA_RAISE, SIGTERM); }
static void own_thread_raise_usr1(void) { expect_ends_itself("raise(SIGUSR1)", VIA_RAISE, SIGUSR1); }
static void own_thread_kill_zero(void) { expect_ends_itself("kill(0, SIGTERM)", VIA_KILL_ZERO, SIGTERM); }
static void own_thread_kill_group(void) { expect_ends_itself("kill(-pgid, SIGTERM)", VIA_KILL_GROUP, SIGTERM); }
static void own_thread_unblock(void) { expect_ends_itself("sigprocmask lets one in", VIA_UNBLOCK, SIGTERM); }
static void own_thread_suspend(void) { expect_ends_itself("sigsuspend lets one in", VIA_SUSPEND, SIGTERM); }

/* What does not end a task returns as it always did. */
static void *not_fatal_main(void *arg) {
    SelfArg *a = (SelfArg *)arg;
    int pid = vprocPid(a->vp);
    vprocRegisterThread(a->vp, pthread_self());
    vprocActivate(a->vp);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = on_signal;
    (void)vprocSigactionShim(SIGUSR1, &sa, NULL);
    expect_kill("to itself, caught", pid, SIGUSR1, 0, 0);
    expect(g_caught == 1, "to itself, caught", "the handler did not run");
    sa.sa_handler = SIG_IGN;
    (void)vprocSigactionShim(SIGTERM, &sa, NULL);
    expect_kill("to itself, ignored", pid, SIGTERM, 0, 0);
    expect_kill("to itself, ignored by default", pid, SIGCHLD, 0, 0);
    expect_kill("to itself, ignored by default", pid, SIGWINCH, 0, 0);
    expect_kill("to itself, nothing to stop", pid, SIGCONT, 0, 0);
    expect_kill("to itself, a probe", pid, 0, 0, 0);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    (void)vprocSigprocmaskShim(SIG_BLOCK, &set, NULL);
    expect_kill("to itself, blocked", pid, SIGHUP, 0, 0);
    expect(signal_of(pid) == 0, "to itself, blocked", "it acted");
    a->returned = true;
    vprocDeactivate();
    return NULL;
}

static void not_fatal_returns(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    SelfArg a = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, not_fatal_main, &a) != 0) {
        fail("not fatal", "pthread_create");
        return;
    }
    pthread_join(tid, NULL);
    expect(a.returned, "not fatal", "the thread ended");
}

/* Every thread of the task goes. */
typedef struct {
    VProc *vp;
    volatile bool ready;
    bool returned;
} MateArg;

static void *mate_main(void *arg) {
    MateArg *m = (MateArg *)arg;
    vprocRegisterThread(m->vp, pthread_self());
    vprocActivate(m->vp);
    m->ready = true;
    for (int i = 0; i < 20000; ++i) {
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    m->returned = true;
    return NULL;
}

static void *mate_killer_main(void *arg) {
    MateArg *m = (MateArg *)arg;
    vprocRegisterThread(m->vp, pthread_self());
    vprocActivate(m->vp);
    (void)vprocKillShim(vprocGetPidShim(), SIGTERM);
    fail("a second thread's kill", "it returned");
    return NULL;
}

static void other_threads_go_too(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    MateArg m = { .vp = vp };
    pthread_t mate, killer;
    if (pthread_create(&mate, NULL, mate_main, &m) != 0) {
        fail("threads of a task", "pthread_create");
        return;
    }
    while (!m.ready) {
        usleep(1000);
    }
    if (pthread_create(&killer, NULL, mate_killer_main, &m) != 0) {
        fail("threads of a task", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(killer, &ret);
    expect(ret == PTHREAD_CANCELED, "the thread that sent it", "it did not end");
    pthread_join(mate, &ret);
    expect(ret == PTHREAD_CANCELED && !m.returned, "the other thread", "it went on");
}

/* In setup or teardown a kill waits until cancels go on. */
typedef struct {
    VProc *vp;
    bool returned_while_off;
    bool returned_after;
} OffArg;

static void *cancels_off_main(void *arg) {
    OffArg *a = (OffArg *)arg;
    vprocRegisterThread(a->vp, pthread_self());
    vprocActivate(a->vp);
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    (void)vprocKillShim(vprocGetPidShim(), SIGTERM);
    a->returned_while_off = true;
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    pthread_testcancel();
    a->returned_after = true;
    return NULL;
}

static void cancels_off_waits(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    OffArg a = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, cancels_off_main, &a) != 0) {
        fail("cancels off", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(a.returned_while_off, "cancels off", "the thread ended while they were off");
    expect(!a.returned_after && ret == PTHREAD_CANCELED, "cancels on again", "it went on");
}

/* The shell's own: cancelling its thread would leave what it holds, so it looks between commands. */
static void shell_own_thread_waits_for_it(void) {
    VProc *shell = become_shell();
    int pid = vprocPid(shell);
    expect_kill("the shell to itself", pid, SIGTERM, 0, 0);
    expect(signal_of(pid) == 0, "the shell to itself", "marked as if it were gone");
    expect(!has_ended(pid), "the shell to itself", "reported ended");
    expect(vprocDeliverTaskSignals(pid) == SIGTERM, "the shell to itself", "not handed back");
    expect(vprocDeliverTaskSignals(pid) == 0, "the shell to itself", "handed back twice");
}

/* A program on the shell's thread. With no frame there is nothing to take it off a thread that must
 * go on. It is as killed from another thread, and the shell's thread is still here. */
static void program_on_shell_thread_no_frame(void) {
    VProc *shell = become_shell();
    VProc *prog = new_task(0);
    int pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    expect_kill("a program on the shell's thread", pid, SIGTERM, 0, 0);
    /* Its next look finds it killed, and still nothing may end this thread. */
    vprocDeliverSignalsHere();
    expect(vprocCurrent() == prog, "a program on the shell's thread", "its task was taken away");
    expect(signal_of(pid) == SIGTERM, "a program on the shell's thread", "no signal recorded");
    expect(!has_ended(pid), "a program on the shell's thread", "reported ended before it has");
    expect(signal_of(vprocPid(shell)) == 0, "a program on the shell's thread", "the shell was marked");
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 1);
    expect_signalled("a program on the shell's thread", pid, SIGTERM);
    vprocDestroy(prog);
}

/* One let in by unblocking takes the same care. pthread_exit here ended the shell's thread. */
static void program_on_shell_thread_unblock(void) {
    (void)become_shell();
    VProc *prog = new_task(0);
    int pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGTERM);
    (void)vprocSigprocmaskShim(SIG_BLOCK, &set, NULL);
    expect_kill("blocked, on the shell's thread", pid, SIGTERM, 0, 0);
    (void)vprocSigprocmaskShim(SIG_UNBLOCK, &set, NULL);
    expect(signal_of(pid) == SIGTERM, "let in, on the shell's thread", "no signal recorded");
    /* Any shim looks for what is pending. */
    (void)vprocGetPidShim();
    (void)vprocSigpendingShim(&set);
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 1);
    expect_signalled("let in, on the shell's thread", pid, SIGTERM);
    vprocDestroy(prog);
}

/* With a frame the program leaves, and the thread goes on. */
static void program_on_shell_thread_frame(void) {
    VProc *shell = become_shell();
    VProc *prog = new_task(0);
    int pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    VProcProgramFrame frame;
    frame.pushed = false;
    volatile int status = 0;
    int jumped = setjmp(frame.env);
    if (jumped == 0) {
        vprocProgramFramePush(&frame);
        (void)vprocRaiseShim(SIGUSR2);
        fail("a program on the shell's thread, with a frame", "raise returned");
    } else {
        status = jumped;
    }
    vprocProgramFramePop(&frame);
    expect(status == 128 + SIGUSR2, "a program on the shell's thread, with a frame", "wrong status");
    expect(vprocCurrent() == prog, "a program on the shell's thread, with a frame", "its task was taken away");
    expect(signal_of(vprocPid(shell)) == 0, "a program on the shell's thread, with a frame", "the shell was marked");
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, status);
    expect_signalled("a program on the shell's thread, with a frame", pid, SIGUSR2);
    vprocDestroy(prog);
}

/* The frame is its program's. One started under it with no frame of its own doesn't leave by it. */
static void frame_is_for_its_own_program(void) {
    (void)become_shell();
    VProc *prog = new_task(0);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    VProcProgramFrame frame;
    frame.pushed = false;
    if (setjmp(frame.env) != 0) {
        fail("a frame and a program under it", "it left by another's frame");
    } else {
        vprocProgramFramePush(&frame);
        VProc *inner = new_task(0);
        vprocRegisterThread(inner, pthread_self());
        vprocActivate(inner);
        expect_kill("a frame and a program under it", vprocPid(inner), SIGTERM, 0, 0);
        vprocDeactivate();
        vprocUnregisterThread(inner, pthread_self());
        vprocMarkExit(inner, 1);
    }
    vprocProgramFramePop(&frame);
    expect(signal_of(vprocPid(prog)) == 0, "a frame and a program under it", "the outer one was marked");
}

/* A frontend on the shell's thread has no frame, and its VM asks before each instruction. */
static void program_on_shell_thread_vm_asks(void) {
    (void)become_shell();
    VProc *prog = new_task(0);
    int pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    (void)vprocWaitIfStopped(prog);
    expect(!vprocTaskKilledHere(), "a VM on the shell's thread", "killed before anything was sent");
    expect_kill("a VM on the shell's thread", pid, SIGTERM, 0, 0);
    (void)vprocWaitIfStopped(prog);
    expect(vprocTaskKilledHere(), "a VM on the shell's thread", "it did not hear");
    /* The shell's own VM asks next, and must not hear the program's kill. */
    vprocDeactivate();
    (void)vprocWaitIfStopped(vprocCurrent());
    expect(!vprocTaskKilledHere(), "the shell's VM after a program's", "it heard the program's kill");
    vprocActivate(prog);
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 1);
    expect_signalled("a VM on the shell's thread", pid, SIGTERM);
    vprocDestroy(prog);
}

/* A frame is for its own program. A program's signal to a task under it on this thread goes to that
 * task, which is shell code, and the program runs on. */
static void program_signals_its_subshell(void) {
    (void)become_shell();
    VProc *body = new_task(0);
    int body_pid = vprocPid(body);
    vprocRegisterThread(body, pthread_self());
    vprocActivate(body);
    vprocSetShellTask(body_pid, true);
    VProc *prog = new_task(0);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    VProcProgramFrame frame;
    frame.pushed = false;
    if (setjmp(frame.env) != 0) {
        fail("a program signalling its subshell", "it was the program that left");
    } else {
        vprocProgramFramePush(&frame);
        expect_kill("a program signalling its subshell", body_pid, SIGTERM, 0, 0);
    }
    vprocProgramFramePop(&frame);
    expect(signal_of(vprocPid(prog)) == 0, "a program signalling its subshell", "the program was marked");
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 0);
    vprocDiscard(vprocPid(prog));
    vprocDestroy(prog);
    expect(vprocDeliverTaskSignals(body_pid) == SIGTERM, "a program signalling its subshell", "not handed back");
}

/* A subshell: its own thread, its own shell code, and from there nothing is cancelled. */
typedef struct {
    VProc *vp;
    int handed_back;
    int signal_on_entry;
    bool returned;
    volatile bool ready;
    volatile bool stop;
} BodyArg;

static void *body_kills_itself_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    int pid = vprocPid(b->vp);
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    vprocSetShellTask(pid, true);
    expect_kill("a subshell to itself", pid, SIGTERM, 0, 0);
    b->signal_on_entry = signal_of(pid);
    b->handed_back = vprocDeliverTaskSignals(pid);
    b->returned = true;
    vprocDeactivate();
    vprocMarkExit(b->vp, 128 + SIGTERM);
    return NULL;
}

static void subshell_to_itself_acts_between_commands(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    int pid = vprocPid(vp);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, body_kills_itself_main, &b) != 0) {
        fail("a subshell to itself", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(ret != PTHREAD_CANCELED && b.returned, "a subshell to itself", "its thread was ended");
    expect(b.signal_on_entry == 0, "a subshell to itself", "marked before its code looked");
    expect(b.handed_back == SIGTERM, "a subshell to itself", "not handed back");
    int status = 0;
    expect(vprocWaitPidShim(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGTERM,
           "a subshell to itself", "wrong status");
    vprocDestroy(vp);
}

static void body_cancelled_cleanup(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    vprocDeactivate();
    vprocMarkExit(b->vp, 1);
}

static void *body_waits_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    vprocSetShellTask(vprocPid(b->vp), true);
    pthread_cleanup_push(body_cancelled_cleanup, b);
    b->ready = true;
    for (int i = 0; i < 20000 && !b->stop; ++i) {
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    b->returned = true;
    pthread_cleanup_pop(1);
    return NULL;
}

/* From anywhere else it is wherever it is, and can be cancelled. */
static void subshell_from_another_thread_is_cancelled(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    int pid = vprocPid(vp);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, body_waits_main, &b) != 0) {
        fail("a subshell from another thread", "pthread_create");
        return;
    }
    while (!b.ready) {
        usleep(1000);
    }
    expect_kill("a subshell from another thread", pid, SIGTERM, 0, 0);
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(ret == PTHREAD_CANCELED && !b.returned, "a subshell from another thread", "it went on");
    expect_signalled("a subshell from another thread", pid, SIGTERM);
    vprocDestroy(vp);
}

/* Queued for shell code, then the task is a program, as after `exec`: where it next looks is where it
 * ends, with no call that can be cancelled before that. */
static void *queued_then_program_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    int pid = vprocPid(b->vp);
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    pthread_cleanup_push(body_cancelled_cleanup, b);
    vprocSetShellTask(pid, true);
    expect_kill("a queued signal, then a program", pid, SIGTERM, 0, 0);
    vprocSetShellTask(pid, false);
    (void)vprocDeliverTaskSignals(pid);
    b->returned = true;
    pthread_cleanup_pop(1);
    return NULL;
}

static void queued_signal_finds_the_program(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    int pid = vprocPid(vp);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, queued_then_program_main, &b) != 0) {
        fail("a queued signal, then a program", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(ret == PTHREAD_CANCELED && !b.returned, "a queued signal, then a program", "it went on past its look");
    expect_signalled("a queued signal, then a program", pid, SIGTERM);
    vprocDestroy(vp);
}

/* A subshell carried by the shell's own thread can't be cancelled, so it too looks. */
typedef struct {
    int pid;
    int rc;
} RemoteKill;

static void *remote_kill_main(void *arg) {
    RemoteKill *r = (RemoteKill *)arg;
    r->rc = vprocKillShim(r->pid, SIGTERM);
    return NULL;
}

static void subshell_on_shell_thread_waits_for_it(void) {
    VProc *shell = become_shell();
    VProc *body = new_task(0);
    int pid = vprocPid(body);
    vprocRegisterThread(body, pthread_self());
    vprocActivate(body);
    vprocSetShellTask(pid, true);
    RemoteKill r = { .pid = pid };
    pthread_t tid;
    if (pthread_create(&tid, NULL, remote_kill_main, &r) != 0) {
        fail("a subshell on the shell's thread", "pthread_create");
        return;
    }
    pthread_join(tid, NULL);
    pthread_testcancel();
    expect(r.rc == 0, "a subshell on the shell's thread", "the kill failed");
    expect(signal_of(pid) == 0, "a subshell on the shell's thread", "marked before its code looked");
    expect(vprocDeliverTaskSignals(pid) == SIGTERM, "a subshell on the shell's thread", "not handed back");
    expect(signal_of(vprocPid(shell)) == 0, "a subshell on the shell's thread", "the shell was marked");
}

/* A program under a subshell's thread: the subshell goes on after it, so no cancel, and a frame. */
static void *program_on_body_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    int pid = vprocPid(b->vp);
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    vprocSetShellTask(pid, true);

    VProc *prog = new_task(0);
    int prog_pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    VProcProgramFrame frame;
    frame.pushed = false;
    if (setjmp(frame.env) == 0) {
        vprocProgramFramePush(&frame);
        (void)vprocKillShim(prog_pid, SIGTERM);
        fail("a program on a subshell's thread", "kill returned");
    }
    vprocProgramFramePop(&frame);
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 1);
    expect_signalled("a program on a subshell's thread", prog_pid, SIGTERM);
    vprocDestroy(prog);

    expect(signal_of(pid) == 0, "a program on a subshell's thread", "the subshell was marked");
    b->returned = true;
    vprocDeactivate();
    vprocMarkExit(b->vp, 0);
    return NULL;
}

static void program_on_subshell_thread(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, program_on_body_main, &b) != 0) {
        fail("a program on a subshell's thread", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(ret != PTHREAD_CANCELED && b.returned, "a program on a subshell's thread", "the subshell ended");
}

/* Killed from another thread, on a subshell's thread: that thread isn't cancelled, and the program
 * leaves by its frame at its next look. The subshell goes on after it, as under a real kill. */
static void *killed_on_body_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    int pid = vprocPid(b->vp);
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    vprocSetShellTask(pid, true);

    VProc *prog = new_task(0);
    int prog_pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    RemoteKill r = { .pid = prog_pid };
    VProcProgramFrame frame;
    frame.pushed = false;
    volatile int status = 0;
    volatile bool past_shim = false;
    int jumped = setjmp(frame.env);
    if (jumped == 0) {
        vprocProgramFramePush(&frame);
        pthread_t killer;
        if (pthread_create(&killer, NULL, remote_kill_main, &r) != 0) {
            fail("a program killed on a subshell's thread", "pthread_create");
            _exit(1);
        }
        pthread_join(killer, NULL);
        pthread_testcancel();
        /* Not inside an ordinary shim, where the program may hold a lock of its own. */
        (void)vprocCloseShim(-1);
        past_shim = true;
        vprocDeliverSignalsHere();
        fail("a program killed on a subshell's thread", "it did not leave at its next look");
    } else {
        status = jumped;
    }
    vprocProgramFramePop(&frame);
    expect(past_shim, "a program killed on a subshell's thread", "it left from inside a shim");
    expect(r.rc == 0, "a program killed on a subshell's thread", "the kill failed");
    expect(status == 128 + SIGTERM, "a program killed on a subshell's thread", "wrong status");
    expect(vprocTaskExitSignal(prog_pid) == SIGTERM, "a program killed on a subshell's thread", "no signal recorded");
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, status);
    expect_signalled("a program killed on a subshell's thread", prog_pid, SIGTERM);
    vprocDestroy(prog);

    expect(signal_of(pid) == 0, "a program killed on a subshell's thread", "the subshell was marked");
    b->returned = true;
    vprocDeactivate();
    vprocMarkExit(b->vp, 0);
    return NULL;
}

static void program_killed_on_subshell_thread(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, killed_on_body_main, &b) != 0) {
        fail("a program killed on a subshell's thread", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(ret != PTHREAD_CANCELED && b.returned, "a program killed on a subshell's thread", "the subshell ended");
}

/* A shell run as a command on its caller's thread is above the caller, not beneath it: killing the
 * caller takes the thread, the nested shell with it. */
static void *nested_shell_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    int pid = vprocPid(b->vp);
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    vprocSetShellTask(pid, true);
    VProc *nested = new_task(0);
    vprocRegisterThread(nested, pthread_self());
    vprocActivate(nested);
    vprocSetShellTask(vprocPid(nested), true);
    pthread_cleanup_push(body_cancelled_cleanup, b);
    b->ready = true;
    for (int i = 0; i < 20000 && !b->stop; ++i) {
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    b->returned = true;
    pthread_cleanup_pop(1);
    return NULL;
}

static void nested_shell_goes_with_its_caller(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    int pid = vprocPid(vp);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, nested_shell_main, &b) != 0) {
        fail("a nested shell's caller", "pthread_create");
        return;
    }
    while (!b.ready) {
        usleep(1000);
    }
    expect_kill("a nested shell's caller", pid, SIGTERM, 0, 0);
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(ret == PTHREAD_CANCELED && !b.returned, "a nested shell's caller", "its thread went on");
}

/* What the program pushed goes with its stack: a later cancel must not walk it. */
static volatile int g_program_handler_ran = 0;
static void program_handler(void *arg) {
    (void)arg;
    g_program_handler_ran++;
}

static void *frame_drops_cleanups_main(void *arg) {
    BodyArg *b = (BodyArg *)arg;
    vprocRegisterThread(b->vp, pthread_self());
    vprocActivate(b->vp);
    vprocSetShellTask(vprocPid(b->vp), true);
    VProc *prog = new_task(0);
    int prog_pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    VProcProgramFrame frame;
    frame.pushed = false;
    if (setjmp(frame.env) == 0) {
        vprocProgramFramePush(&frame);
        pthread_cleanup_push(program_handler, NULL);
        (void)vprocKillShim(prog_pid, SIGTERM);
        fail("a program's own handlers", "kill returned");
        pthread_cleanup_pop(0);
    }
    vprocProgramFramePop(&frame);
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 1);
    vprocDeactivate();
    vprocMarkExit(b->vp, 0);
    pthread_exit(NULL);
    return NULL;
}

static void frame_leaves_no_dangling_cleanup(void) {
    (void)become_shell();
    VProc *vp = new_task(0);
    BodyArg b = { .vp = vp };
    pthread_t tid;
    if (pthread_create(&tid, NULL, frame_drops_cleanups_main, &b) != 0) {
        fail("a program's own handlers", "pthread_create");
        return;
    }
    pthread_join(tid, NULL);
    expect(g_program_handler_ran == 0, "a program's own handlers", "ran from a stack that has gone");
}

/* A simulated fork's child is on its parent's thread, and the parent goes on from its fork(). */
typedef struct {
    bool parent_resumed;
} ForkArg;

static void *fork_main(void *arg) {
    ForkArg *f = (ForkArg *)arg;
    VProc *vp = new_task(0);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    sigjmp_buf env;
    if (sigsetjmp(env, 1) == 0) {
        pid_t child = vprocSimulatedForkWithEnv("killed", false, &env);
        if (child != 0) {
            fail("a simulated fork", strerror(errno));
            return NULL;
        }
        (void)vprocKillShim(vprocGetPidShim(), SIGTERM);
        fail("a simulated fork's child", "kill returned");
        _exit(1);
    }
    pid_t child = vprocSimulatedForkParentResume();
    f->parent_resumed = true;
    expect_signalled("a simulated fork's child", child, SIGTERM);
    expect(signal_of(vprocPid(vp)) == 0, "a simulated fork's parent", "it was marked");
    vprocDeactivate();
    vprocMarkExit(vp, 0);
    return NULL;
}

static void simulated_fork_child(void) {
    (void)become_shell();
    ForkArg f = { false };
    pthread_t tid;
    if (pthread_create(&tid, NULL, fork_main, &f) != 0) {
        fail("a simulated fork", "pthread_create");
        return;
    }
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(f.parent_resumed && ret != PTHREAD_CANCELED, "a simulated fork's parent", "it ended with the child");
}

/* A task that has ended. */
static VProc *ended_task(int pgid, int status) {
    VProc *vp = new_task(pgid);
    vprocMarkExit(vp, status);
    return vp;
}

static void signal_to_ended_pid(void) {
    (void)become_shell();
    VProc *vp = ended_task(0, 7);
    int pid = vprocPid(vp);
    const int sigs[] = { SIGTERM, SIGKILL, SIGHUP, SIGUSR1, SIGINT, SIGQUIT, SIGTSTP, SIGSTOP, SIGCONT, SIGCHLD, SIGWINCH };
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); ++i) {
        expect_kill("a signal to an ended pid", pid, sigs[i], 0, 0);
    }
    expect_kill("a probe of an ended pid", pid, 0, 0, 0);
    expect_live_kill("the kernel's own kill of an ended pid", pid, SIGTERM, -1, ESRCH);
    /* It does nothing: what it ended of stays. */
    int status = 0;
    expect(vprocWaitPidShim(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
           "a signal to an ended pid", "it changed the status");
    /* Reaped, it is gone. */
    expect_kill("a reaped pid", pid, SIGTERM, -1, ESRCH);
    expect_kill("a probe of a reaped pid", pid, 0, -1, ESRCH);
    expect_kill("no such pid", 77777, SIGTERM, -1, ESRCH);
    expect_kill("no such group", -77777, SIGTERM, -1, ESRCH);
}

static void signal_to_ended_group(void) {
    (void)become_shell();
    VProc *a = ended_task(0, 7);
    int pgid = vprocPid(a);
    VProc *b = ended_task(pgid, 8);
    expect_kill("a group of ended tasks", -pgid, SIGTERM, 0, 0);
    expect_kill("a probe of a group of ended tasks", -pgid, 0, 0, 0);
    expect_live_kill("the kernel's own kill of a group of ended tasks", -pgid, SIGTERM, -1, ESRCH);
    int status = 0;
    expect(vprocWaitPidShim(vprocPid(a), &status, 0) == pgid && WIFEXITED(status) && WEXITSTATUS(status) == 7,
           "a group of ended tasks", "it changed the status");
    expect(vprocWaitPidShim(vprocPid(b), &status, 0) == vprocPid(b) && WIFEXITED(status) && WEXITSTATUS(status) == 8,
           "a group of ended tasks", "it changed the status");
    expect_kill("a group, reaped", -pgid, SIGTERM, -1, ESRCH);
}

static void signal_to_mixed_group(void) {
    (void)become_shell();
    VProc *a = ended_task(0, 7);
    int pgid = vprocPid(a);
    VProc *b = new_task(pgid);
    expect_kill("a group with one left", -pgid, SIGTERM, 0, 0);
    expect(signal_of(vprocPid(b)) == SIGTERM, "a group with one left", "the living one was not reached");
    VProc *c = ended_task(0, 7);
    VProc *d = new_task(vprocPid(c));
    expect_live_kill("the kernel's own kill of a group with one left", -vprocPid(c), SIGTERM, 0, 0);
    expect(signal_of(vprocPid(d)) == SIGTERM, "the kernel's own kill of a group with one left", "not reached");
}

static void signal_to_everything_ended(void) {
    (void)vprocEnsureKernelPid();
    VProc *me = new_task(0);
    vprocRegisterThread(me, pthread_self());
    vprocActivate(me);
    expect_kill("kill(-1) with no one", -1, SIGTERM, -1, ESRCH);
    VProc *a = ended_task(0, 7);
    expect_kill("kill(-1) with only an ended task", -1, SIGTERM, 0, 0);
    expect_kill("kill(-1, 0) with only an ended task", -1, 0, 0, 0);
    expect_live_kill("the kernel's own kill(-1) with only an ended task", -1, SIGTERM, -1, ESRCH);
    int status = 0;
    expect(vprocWaitPidShim(vprocPid(a), &status, 0) == vprocPid(a) && WIFEXITED(status) && WEXITSTATUS(status) == 7,
           "kill(-1) with only an ended task", "it changed the status");
    expect_kill("kill(-1) when it is reaped", -1, SIGTERM, -1, ESRCH);
    expect(signal_of(vprocPid(me)) == 0, "kill(-1)", "reached the caller");
}

/* Ctrl-C and Ctrl-Z: nothing alive took it, so what they fall back on must still happen. */
static void control_signal_sees_no_one(void) {
    VProc *shell = become_shell();
    int shell_pid = vprocPid(shell);
    VProc *job = new_task(0);
    int job_pid = vprocPid(job);
    /* Joining a session puts it in the leader's group. */
    (void)vprocSetSid(job_pid, shell_pid);
    (void)vprocSetPgid(job_pid, job_pid);
    if (vprocSetForegroundPgid(shell_pid, job_pid) != 0) {
        fail("foreground group", strerror(errno));
    }
    expect(vprocRequestControlSignal(SIGINT), "Ctrl-C with a foreground group alive", "not taken");
    vprocMarkExit(job, 7);
    if (vprocGetForegroundPgid(shell_pid) != job_pid) {
        /* Something handed it back to the shell, which says nothing about this. */
        (void)vprocSetForegroundPgid(shell_pid, job_pid);
    }
    expect(vprocGetForegroundPgid(shell_pid) == job_pid, "Ctrl-C with no one alive", "no foreground group to test");
    expect(!vprocRequestControlSignal(SIGINT), "Ctrl-C with no one alive", "it counted as delivered");
    expect(!vprocRequestControlSignal(SIGTSTP), "Ctrl-Z with no one alive", "it counted as delivered");
    expect(pscalTtySendGroupSignal(job_pid, SIGINT) != 0, "the terminal's signal to no one alive", "it counted as sent");
}

/* A positive pid never reached the host through any of this. */
static void in_child(const char *name, void (*check)(void)) {
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) {
        fail("fork", name);
        return;
    }
    if (child == 0) {
        alarm(30);
        g_fails = 0;
        g_host_kills = 0;
        check();
        if (g_host_kills != 0) {
            fail(name, "went to the host");
        }
        fflush(stderr);
        _exit(g_fails == 0 ? 0 : 1);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "  wait status 0x%x\n", status);
        fail(name, "failed in its child");
    }
}

int main(void) {
    in_child("own thread, kill", own_thread_kill);
    in_child("own thread, SIGKILL", own_thread_sigkill);
    in_child("own thread, SIGHUP", own_thread_sighup);
    in_child("own thread, raise", own_thread_raise);
    in_child("own thread, raise SIGUSR1", own_thread_raise_usr1);
    in_child("own thread, kill(0)", own_thread_kill_zero);
    in_child("own thread, group", own_thread_kill_group);
    in_child("own thread, unblock", own_thread_unblock);
    in_child("own thread, sigsuspend", own_thread_suspend);
    in_child("not fatal", not_fatal_returns);
    in_child("threads of a task", other_threads_go_too);
    in_child("cancels off", cancels_off_waits);
    in_child("the shell's own", shell_own_thread_waits_for_it);
    in_child("a program on the shell's thread", program_on_shell_thread_no_frame);
    in_child("a program on the shell's thread, unblocking", program_on_shell_thread_unblock);
    in_child("a program on the shell's thread, with a frame", program_on_shell_thread_frame);
    in_child("a frame and a program under it", frame_is_for_its_own_program);
    in_child("a VM on the shell's thread", program_on_shell_thread_vm_asks);
    in_child("a program signalling its subshell", program_signals_its_subshell);
    in_child("a subshell to itself", subshell_to_itself_acts_between_commands);
    in_child("a subshell from another thread", subshell_from_another_thread_is_cancelled);
    in_child("a subshell on the shell's thread", subshell_on_shell_thread_waits_for_it);
    in_child("a program on a subshell's thread", program_on_subshell_thread);
    in_child("a program killed on a subshell's thread", program_killed_on_subshell_thread);
    in_child("a nested shell's caller", nested_shell_goes_with_its_caller);
    in_child("a queued signal, then a program", queued_signal_finds_the_program);
    in_child("a program's own handlers", frame_leaves_no_dangling_cleanup);
    in_child("a simulated fork's child", simulated_fork_child);
    in_child("an ended pid", signal_to_ended_pid);
    in_child("an ended group", signal_to_ended_group);
    in_child("a mixed group", signal_to_mixed_group);
    in_child("everything ended", signal_to_everything_ended);
    in_child("Ctrl-C and Ctrl-Z with no one alive", control_signal_sees_no_one);
    if (g_fails != 0) {
        fprintf(stderr, "test_self_signal: %d failures\n", g_fails);
        return 1;
    }
    printf("test_self_signal: ok\n");
    return 0;
}
