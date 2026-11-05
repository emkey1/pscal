/* A thread a task starts doesn't take over as its main thread. On the shell's task that hid the shell's
 * own thread, so a stage on it killed from elsewhere took the shell with it, and it made a worker the
 * shell started look like the shell, which could not be cancelled. Each check runs in a child with a
 * clean table. In a child gone wrong a parked thread keeps the process alive, so alarm ends it, rather
 * than the last thread's end looking like a pass. */
#define _GNU_SOURCE 1
#include <errno.h>
#include <pthread.h>
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

static int g_fails = 0;

static void expect(bool ok, const char *what, const char *detail) {
    if (!ok) {
        fprintf(stderr, "FAIL %s: %s\n", what, detail);
        g_fails++;
    }
}

static int signal_of(int pid) {
    VProcSnapshot snap[64];
    size_t n = vprocSnapshot(snap, 64);
    for (size_t i = 0; i < n; ++i) {
        if (snap[i].pid == pid) {
            return snap[i].exit_signal;
        }
    }
    return -1;
}

static void expect_signalled(const char *what, int pid, int sig) {
    int status = 0;
    if (vprocWaitPidShim(pid, &status, 0) != pid) {
        expect(false, what, "wait did not find it");
    } else {
        expect(WIFSIGNALED(status) && WTERMSIG(status) == sig, what, "wrong wait status");
    }
}

/* This thread is the session's shell. */
static VProc *become_shell(void) {
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fprintf(stderr, "FAIL vprocCreate: %s\n", strerror(errno));
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

/* A child of this thread's task, in a group of its own. */
static VProc *new_task(void) {
    VProc *parent = vprocCurrent();
    VProc *vp = vprocCreate(NULL);
    if (!vp) {
        fprintf(stderr, "FAIL vprocCreate: %s\n", strerror(errno));
        _exit(1);
    }
    int pid = vprocPid(vp);
    if (parent) {
        vprocSetParent(pid, vprocPid(parent));
    }
    (void)vprocSetPgid(pid, pid);
    return vp;
}

typedef struct {
    volatile bool ready;
    volatile bool stop;
} ParkArg;

static void *park_main(void *arg) {
    ParkArg *p = (ParkArg *)arg;
    p->ready = true;
    while (!p->stop) {
        usleep(1000);
    }
    return NULL;
}

/* Started under the current task, as a pipeline's first stage is under the shell. */
static void start_parked(ParkArg *p, pthread_t *tid) {
    p->ready = false;
    p->stop = false;
    if (vprocPthreadCreateShim(tid, NULL, park_main, p) != 0) {
        fprintf(stderr, "FAIL vprocPthreadCreateShim\n");
        _exit(1);
    }
    while (!p->ready) {
        usleep(1000);
    }
}

typedef struct {
    int pid;
    int sig;
    int rc;
} RemoteKill;

static void *remote_kill_main(void *arg) {
    RemoteKill *r = (RemoteKill *)arg;
    r->rc = vprocKillShim(r->pid, r->sig);
    return NULL;
}

static void kill_from_elsewhere(int pid, int sig) {
    RemoteKill r = { pid, sig, -1 };
    pthread_t killer;
    if (pthread_create(&killer, NULL, remote_kill_main, &r) != 0) {
        fprintf(stderr, "FAIL pthread_create\n");
        _exit(1);
    }
    pthread_join(killer, NULL);
    expect(r.rc == 0, "a kill from elsewhere", "it failed");
}

/* The last stage of `a | b`, on the shell's thread, with a's thread started under the shell. */
static void stage_on_shell_thread_keeps_it(void) {
    VProc *shell = become_shell();
    int shell_pid = vprocPid(shell);
    ParkArg park;
    pthread_t first_stage;
    start_parked(&park, &first_stage);
    VProc *stage = new_task();
    int stage_pid = vprocPid(stage);
    vprocRegisterThread(stage, pthread_self());
    vprocActivate(stage);
    kill_from_elsewhere(stage_pid, SIGTERM);
    /* A cancel takes effect at these. */
    pthread_testcancel();
    for (int i = 0; i < 20; ++i) {
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    expect(signal_of(stage_pid) == SIGTERM, "a stage on the shell's thread", "no signal recorded");
    expect(signal_of(shell_pid) == 0, "a stage on the shell's thread", "the shell was marked");
    vprocDeactivate();
    vprocUnregisterThread(stage, pthread_self());
    vprocMarkExit(stage, 1);
    expect_signalled("a stage on the shell's thread", stage_pid, SIGTERM);
    vprocDestroy(stage);
    park.stop = true;
    pthread_join(first_stage, NULL);
}

/* What the shell looks at between commands finds it while a thread it started is running. */
static void shell_hears_beside_a_thread_it_started(void) {
    VProc *shell = become_shell();
    int shell_pid = vprocPid(shell);
    ParkArg park;
    pthread_t first_stage;
    start_parked(&park, &first_stage);
    kill_from_elsewhere(shell_pid, SIGUSR1);
    expect(vprocDeliverTaskSignals(shell_pid) == SIGUSR1, "the shell beside a thread it started", "not handed back");
    vprocNoteCaughtSignal(shell_pid, SIGUSR2);
    expect((vprocCaughtSignals(shell_pid) & (1u << SIGUSR2)) != 0, "the shell beside a thread it started",
           "it could not see what was noted for it");
    expect(vprocTakeCaughtSignal(shell_pid, SIGUSR2), "the shell beside a thread it started", "it could not take it");
    park.stop = true;
    pthread_join(first_stage, NULL);
}

/* A worker the shell started is not the shell, and can be cancelled. */
typedef struct {
    VProc *vp;
    volatile bool ready;
    bool returned;
} WorkerArg;

static void worker_cleanup(void *arg) {
    WorkerArg *w = (WorkerArg *)arg;
    vprocDeactivate();
    vprocMarkExit(w->vp, 1);
}

static void *worker_main(void *arg) {
    WorkerArg *w = (WorkerArg *)arg;
    vprocRegisterThread(w->vp, pthread_self());
    vprocActivate(w->vp);
    pthread_cleanup_push(worker_cleanup, w);
    w->ready = true;
    for (int i = 0; i < 20000; ++i) {
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    w->returned = true;
    pthread_cleanup_pop(1);
    return NULL;
}

static void worker_started_by_shell_is_cancelled(void) {
    VProc *shell = become_shell();
    VProc *vp = new_task();
    int pid = vprocPid(vp);
    WorkerArg w = { vp, false, false };
    pthread_t tid;
    if (vprocPthreadCreateShim(&tid, NULL, worker_main, &w) != 0) {
        expect(false, "a worker the shell started", "pthread_create");
        return;
    }
    while (!w.ready) {
        usleep(1000);
    }
    kill_from_elsewhere(pid, SIGTERM);
    void *ret = NULL;
    pthread_join(tid, &ret);
    expect(!w.returned, "a worker the shell started", "it went on");
    expect(signal_of(vprocPid(shell)) == 0, "a worker the shell started", "the shell was marked");
    expect_signalled("a worker the shell started", pid, SIGTERM);
    vprocDestroy(vp);
}

/* It stops and continues as ever, and the shell doesn't stop with it. */
static void *stop_worker_main(void *arg) {
    WorkerArg *w = (WorkerArg *)arg;
    vprocRegisterThread(w->vp, pthread_self());
    vprocActivate(w->vp);
    w->ready = true;
    for (int i = 0; i < 20000 && !w->returned; ++i) {
        (void)vprocWaitIfStopped(w->vp);
        struct timespec nap = { 0, 1000000 };
        nanosleep(&nap, NULL);
    }
    vprocDeactivate();
    vprocMarkExit(w->vp, 0);
    return NULL;
}

static bool task_is_stopped(int pid) {
    VProcSnapshot snap[64];
    size_t n = vprocSnapshot(snap, 64);
    for (size_t i = 0; i < n; ++i) {
        if (snap[i].pid == pid) {
            return snap[i].stopped;
        }
    }
    return false;
}

static void worker_stops_and_continues(void) {
    VProc *shell = become_shell();
    VProc *vp = new_task();
    int pid = vprocPid(vp);
    WorkerArg w = { vp, false, false };
    pthread_t tid;
    if (vprocPthreadCreateShim(&tid, NULL, stop_worker_main, &w) != 0) {
        expect(false, "a worker stopping", "pthread_create");
        return;
    }
    while (!w.ready) {
        usleep(1000);
    }
    kill_from_elsewhere(pid, SIGSTOP);
    expect(task_is_stopped(pid), "a worker stopping", "it did not stop");
    expect(!task_is_stopped(vprocPid(shell)), "a worker stopping", "the shell stopped with it");
    usleep(20000);
    expect(task_is_stopped(pid), "a worker stopping", "it did not stay stopped");
    kill_from_elsewhere(pid, SIGCONT);
    expect(!task_is_stopped(pid), "a worker continuing", "it stayed stopped");
    w.returned = true;
    pthread_join(tid, NULL);
    int status = 0;
    expect(vprocWaitPidShim(pid, &status, 0) == pid && WIFEXITED(status), "a worker continuing", "wrong wait status");
    vprocDestroy(vp);
}

/* A stop on the shell's thread turns into a pending signal, so the prompt isn't parked. A thread the
 * program started used to hide that the program is on the shell's thread. */
static void stop_on_shell_thread_does_not_park(void) {
    (void)become_shell();
    VProc *prog = new_task();
    int pid = vprocPid(prog);
    vprocRegisterThread(prog, pthread_self());
    vprocActivate(prog);
    ParkArg park;
    pthread_t helper;
    start_parked(&park, &helper);
    expect(vprocKillShim(pid, SIGTSTP) == 0, "a stop on the shell's thread", "the kill failed");
    /* With the bug this never returns, and alarm ends the child. */
    (void)vprocWaitIfStopped(prog);
    expect(!task_is_stopped(pid), "a stop on the shell's thread", "it stayed stopped");
    park.stop = true;
    pthread_join(helper, NULL);
    vprocDeactivate();
    vprocUnregisterThread(prog, pthread_self());
    vprocMarkExit(prog, 0);
}

static void in_child(const char *name, void (*check)(void)) {
    fflush(stdout);
    fflush(stderr);
    pid_t child = fork();
    if (child < 0) {
        expect(false, name, "fork");
        return;
    }
    if (child == 0) {
        alarm(20);
        g_fails = 0;
        check();
        fflush(stderr);
        _exit(g_fails == 0 ? 0 : 1);
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "  wait status 0x%x\n", status);
        expect(false, name, "failed in its child");
    }
}

int main(void) {
    in_child("a stage on the shell's thread", stage_on_shell_thread_keeps_it);
    in_child("the shell beside a thread it started", shell_hears_beside_a_thread_it_started);
    in_child("a worker the shell started", worker_started_by_shell_is_cancelled);
    in_child("a worker stopping and continuing", worker_stops_and_continues);
    in_child("a stop on the shell's thread", stop_on_shell_thread_does_not_park);
    if (g_fails != 0) {
        fprintf(stderr, "test_first_thread: %d failures\n", g_fails);
        return 1;
    }
    printf("test_first_thread: ok\n");
    return 0;
}
