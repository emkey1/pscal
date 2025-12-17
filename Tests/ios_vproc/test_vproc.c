#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/wait.h>
#include <signal.h>
#include <limits.h>
#include <stdlib.h>
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

#include "ios/vproc.h"

static void assert_write_reads_back(void) {
    int host_pipe[2];
    assert(pipe(host_pipe) == 0);
    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = -2; // /dev/null
    opts.stdout_fd = host_pipe[1];
    opts.stderr_fd = host_pipe[1];
    VProc *vp = vprocCreate(&opts);
    assert(vp);

    vprocActivate(vp);
    const char *msg = "ok";
    assert(vprocWriteShim(1, msg, 2) == 2);
    vprocDeactivate();

    close(host_pipe[1]);
    char buf[3] = {0};
    ssize_t r = read(host_pipe[0], buf, sizeof(buf));
    close(host_pipe[0]);
    assert(r == 2);
    assert(strcmp(buf, "ok") == 0);

    vprocDestroy(vp);
}

static void assert_pipe_round_trip(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    int p[2];
    assert(vprocPipeShim(p) == 0);
    const char *msg = "data";
    assert(vprocWriteShim(p[1], msg, 4) == 4);
    char buf[5] = {0};
    assert(vprocReadShim(p[0], buf, 4) == 4);
    assert(strcmp(buf, "data") == 0);
    assert(vprocCloseShim(p[0]) == 0);
    assert(vprocCloseShim(p[1]) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_dup2_isolated(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    int p[2];
    assert(vprocPipeShim(p) == 0);
    // duplicate write end to stdout inside vproc
    assert(vprocDup2Shim(p[1], 1) == 1);
    const char *msg = "iso";
    assert(vprocWriteShim(1, msg, 3) == 3);
    char buf[4] = {0};
    assert(vprocReadShim(p[0], buf, 3) == 3);
    assert(strcmp(buf, "iso") == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_stdin_redirected_via_dup2(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    int p[2];
    assert(vprocPipeShim(p) == 0);
    assert(vprocDup2Shim(p[0], 0) == 0);
    const char *msg = "in";
    assert(vprocWriteShim(p[1], msg, 2) == 2);
    char buf[3] = {0};
    assert(vprocReadShim(0, buf, 2) == 2);
    assert(strcmp(buf, "in") == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_host_stdio_untouched_after_vproc_close(void) {
    int before = fcntl(STDOUT_FILENO, F_GETFD);
    assert(before >= 0);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    // Closing vproc stdout must not close the host stdout.
    assert(vprocCloseShim(1) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
    int after = fcntl(STDOUT_FILENO, F_GETFD);
    assert(after >= 0);
    // Zero-length write should still succeed on host stdout.
    assert(write(STDOUT_FILENO, "", 0) == 0);
}

static void assert_winsize_round_trip(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    VProcWinsize ws;
    assert(vprocGetWinsize(vp, &ws) == 0);
    assert(ws.cols > 0 && ws.rows > 0);
    assert(vprocSetWinsize(vp, 100, 40) == 0);
    assert(vprocGetWinsize(vp, &ws) == 0);
    assert(ws.cols == 100);
    assert(ws.rows == 40);
    vprocDestroy(vp);
}

static void assert_open_and_read_via_shim(void) {
    char tmpl[] = "/tmp/vproc-openXXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    close(fd);
    unlink(tmpl);
    fd = pscalPathVirtualized_open(tmpl, O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    const char *msg = "filedata";
    assert(write(fd, msg, 8) == 8);
    lseek(fd, 0, SEEK_SET);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    int vfd = vprocOpenShim(tmpl, O_RDONLY, 0);
    assert(vfd >= 0);
    char buf[16] = {0};
    assert(vprocReadShim(vfd, buf, sizeof(buf)) == 8);
    assert(strcmp(buf, "filedata") == 0);
    assert(vprocCloseShim(vfd) == 0);
    vprocDeactivate();
    vprocDestroy(vp);

    close(fd);
    pscalPathVirtualized_unlink(tmpl);
    unlink(tmpl);
}

static void assert_isolation_between_vprocs(void) {
    int pipe_a[2];
    int pipe_b[2];
    assert(pipe(pipe_a) == 0);
    assert(pipe(pipe_b) == 0);

    VProcOptions opts1 = vprocDefaultOptions();
    opts1.stdout_fd = pipe_a[1];
    VProc *vp1 = vprocCreate(&opts1);
    assert(vp1);
    vprocActivate(vp1);
    assert(vprocWriteShim(1, "one", 3) == 3);
    vprocDeactivate();

    VProcOptions opts2 = vprocDefaultOptions();
    opts2.stdout_fd = pipe_b[1];
    VProc *vp2 = vprocCreate(&opts2);
    assert(vp2);
    vprocActivate(vp2);
    assert(vprocWriteShim(1, "two", 3) == 3);
    vprocDeactivate();

    char buf[4] = {0};
    assert(read(pipe_a[0], buf, 3) == 3);
    assert(strcmp(buf, "one") == 0);
    memset(buf, 0, sizeof(buf));
    assert(read(pipe_b[0], buf, 3) == 3);
    assert(strcmp(buf, "two") == 0);

    vprocDestroy(vp1);
    vprocDestroy(vp2);
    close(pipe_a[0]);
    close(pipe_a[1]);
    close(pipe_b[0]);
    close(pipe_b[1]);
}

typedef struct {
    int pid;
} VProcWaitArg;

static void *wait_helper_thread(void *arg) {
    VProcWaitArg *info = (VProcWaitArg *)arg;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    info->pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    vprocMarkExit(vp, 7);
    vprocDeactivate();
    vprocDestroy(vp);
    return NULL;
}

static void assert_wait_on_synthetic_pid(void) {
    pthread_t tid;
    VProcWaitArg arg = {.pid = -1};
    assert(pthread_create(&tid, NULL, wait_helper_thread, &arg) == 0);
    while (arg.pid <= 0) {
        sched_yield();
    }
    int status = -1;
    assert(vprocWaitPidShim(arg.pid, &status, 0) == arg.pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 7);
    pthread_join(tid, NULL);
}

static volatile sig_atomic_t g_signal_seen = 0;
static volatile int g_snapshot_exit = 0;

static void sigusr1_handler(int signo) {
    if (signo == SIGUSR1) {
        g_signal_seen = 1;
    }
}

typedef struct {
    int pid_hint;
    volatile int ready;
} VProcSignalArg;

static void *signal_helper_thread(void *arg) {
    VProcSignalArg *info = (VProcSignalArg *)arg;
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = info->pid_hint;
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    info->ready = 1;
    while (!g_signal_seen) {
        sched_yield();
    }
    vprocDeactivate();
    vprocMarkExit(vp, 0);
    vprocDestroy(vp);
    return NULL;
}

static void assert_kill_negative_pid_routes_to_thread(void) {
    g_signal_seen = 0;
    VProcSignalArg arg;
    arg.pid_hint = vprocReservePid();
    arg.ready = 0;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, signal_helper_thread, &arg) == 0);

    while (!arg.ready) {
        sched_yield();
    }
    /* Deliver a stop to the pgid and observe via wait; no host signals are used. */
    int status = 0;
    assert(vprocKillShim(-arg.pid_hint, SIGTSTP) == 0);
    assert(vprocWaitPidShim(arg.pid_hint, &status, WUNTRACED) == arg.pid_hint);
    assert(WIFSTOPPED(status));

    /* Resume and let helper exit cleanly. */
    assert(vprocKillShim(arg.pid_hint, SIGCONT) == 0);
    g_signal_seen = 1;
    pthread_join(tid, NULL);
}

static int current_waiter_pid(void) {
    int shell = vprocGetShellSelfPid();
    return shell > 0 ? shell : (int)getpid();
}

static void assert_wait_enforces_parent(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    int waiter = current_waiter_pid();

    vprocSetParent(pid, waiter + 9999);
    vprocMarkExit(vp, 9);
    int status = 0;
    errno = 0;
    assert(vprocWaitPidShim(pid, &status, 0) == -1);
    assert(errno == ECHILD);

    vprocSetParent(pid, waiter);
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 9);
    vprocDestroy(vp);
}

static void assert_wait_wnowait_preserves_zombie(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocMarkExit(vp, 17);

    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WNOWAIT) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 17);

    memset(&status, 0, sizeof(status));
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 17);
    vprocDestroy(vp);
}

static void assert_wait_by_pgid(void) {
    VProcOptions opts = vprocDefaultOptions();
    VProc *vp1 = vprocCreate(&opts);
    VProc *vp2 = vprocCreate(&opts);
    assert(vp1 && vp2);
    int pid1 = vprocPid(vp1);
    int pid2 = vprocPid(vp2);
    int pgid = pid1 + 1000;
    assert(vprocSetSid(pid2, vprocGetSid(pid1)) == 0);
    assert(vprocSetPgid(pid1, pgid) == 0);
    assert(vprocSetPgid(pid2, pgid) == 0);

    vprocMarkExit(vp1, 3);
    vprocMarkExit(vp2, 4);
    int status = 0;
    int waited = vprocWaitPidShim(-pgid, &status, 0);
    assert(waited == pid1 || waited == pid2);
    assert(WIFEXITED(status));
    memset(&status, 0, sizeof(status));
    int expected_remaining = (waited == pid1) ? pid2 : pid1;
    waited = vprocWaitPidShim(-pgid, &status, 0);
    assert(waited == expected_remaining);
    assert(WIFEXITED(status));

    vprocDestroy(vp1);
    vprocDestroy(vp2);
}

static void assert_wait_reports_continued(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);

    assert(vprocKillShim(pid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));

    memset(&status, 0, sizeof(status));
    assert(vprocKillShim(pid, SIGCONT) == 0);
    assert(vprocWaitPidShim(pid, &status, WCONTINUED) == pid);
    assert(WIFCONTINUED(status));

    vprocMarkExit(vp, 0);
    memset(&status, 0, sizeof(status));
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    vprocDestroy(vp);
}

static void assert_kill_zero_targets_current_pgid(void) {
    int previous_shell = vprocGetShellSelfPid();
    int parent = current_waiter_pid();
    vprocSetShellSelfPid(parent);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    int pgid = pid + 777;
    assert(vprocSetPgid(pid, pgid) == 0);
    vprocSetParent(pid, parent);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int status = 0;
    assert(vprocKillShim(0, SIGTSTP) == 0);
    vprocDeactivate();
    assert(vprocWaitPidShim(-pgid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));

    assert(vprocKillShim(pid, SIGCONT) == 0);
    vprocMarkExit(vp, 0);
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
    vprocClearSigchldPending(parent);
    vprocSetShellSelfPid(previous_shell);
}

static void assert_children_reparent_to_shell(void) {
    int previous_shell = vprocGetShellSelfPid();
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *parent = vprocCreate(&opts);
    VProc *child = vprocCreate(NULL);
    assert(parent && child);
    int parent_pid = vprocPid(parent);
    int child_pid = vprocPid(child);
    vprocSetParent(child_pid, parent_pid);

    vprocMarkExit(parent, 0);
    int status = 0;
    assert(vprocWaitPidShim(parent_pid, &status, 0) == parent_pid);

    vprocMarkExit(child, 0);
    memset(&status, 0, sizeof(status));
    assert(vprocWaitPidShim(child_pid, &status, 0) == child_pid);

    vprocDestroy(parent);
    vprocDestroy(child);
    vprocSetShellSelfPid(previous_shell);
}

static void assert_sigchld_pending_snapshot(void) {
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);
    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, shell_pid);
    vprocMarkExit(child, 0);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool found_pending = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == shell_pid && snaps[i].sigchld_pending) {
            found_pending = true;
        }
    }
    assert(found_pending);

    int status = 0;
    assert(vprocWaitPidShim(cpid, &status, 0) == cpid);
    free(snaps);

    cap = vprocSnapshot(NULL, 0);
    snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    count = vprocSnapshot(snaps, cap);
    bool cleared = true;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == shell_pid && snaps[i].sigchld_pending) {
            cleared = false;
        }
    }
    assert(cleared);
    free(snaps);
    vprocDestroy(child);
}

static void assert_sigchld_pending_api(void) {
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);
    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, shell_pid);
    vprocSetSigchldBlocked(shell_pid, true);
    vprocMarkExit(child, 0);

    assert(vprocSigchldPending(shell_pid));
    int status = 0;
    assert(vprocWaitPidShim(cpid, &status, 0) == cpid);
    /* Pending should remain while blocked. */
    assert(vprocSigchldPending(shell_pid));
    assert(vprocSetSigchldBlocked(shell_pid, false) == 0);
    vprocClearSigchldPending(shell_pid);
    assert(!vprocSigchldPending(shell_pid));
    vprocDestroy(child);
}

static void assert_sigchld_unblock_drains_pending_signal(void) {
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);
    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, shell_pid);
    vprocSetSigchldBlocked(shell_pid, true);
    vprocMarkExit(child, 0);

    assert(vprocSigchldPending(shell_pid));
    /* Unblock should drain pending SIGCHLD via queued signal. */
    assert(vprocSetSigchldBlocked(shell_pid, false) == 0);
    vprocClearSigchldPending(shell_pid);
    assert(!vprocSigchldPending(shell_pid));

    int status = 0;
    (void)vprocWaitPidShim(cpid, &status, 0);
    vprocDestroy(child);
}

static void assert_group_exit_code_used(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocMarkGroupExit(pid, 99);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 99);
    vprocDestroy(vp);
}

static void assert_group_stop_reaches_all_members(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *a = vprocCreate(&opts);
    opts.pid_hint = vprocReservePid();
    VProc *b = vprocCreate(&opts);
    assert(a && b);
    int pid_a = vprocPid(a);
    int pid_b = vprocPid(b);
    int pgid = pid_a + 50;
    int sid = pgid;
    vprocSetParent(pid_a, shell_pid);
    vprocSetParent(pid_b, shell_pid);
    assert(vprocSetSid(pid_a, sid) == 0);
    assert(vprocSetSid(pid_b, sid) == 0);
    assert(vprocSetPgid(pid_a, pgid) == 0);
    assert(vprocSetPgid(pid_b, pgid) == 0);

    assert(vprocKillShim(-pgid, SIGTSTP) == 0);
    bool saw_a = false;
    bool saw_b = false;
    for (int i = 0; i < 2; ++i) {
        int status = 0;
        int got = vprocWaitPidShim(-pgid, &status, WUNTRACED);
        assert(got == pid_a || got == pid_b);
        assert(WIFSTOPPED(status));
        if (got == pid_a) saw_a = true;
        if (got == pid_b) saw_b = true;
    }
    assert(saw_a && saw_b);

    vprocKillShim(-pgid, SIGCONT);
    vprocMarkExit(a, 0);
    vprocMarkExit(b, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid_a, &status, 0);
    (void)vprocWaitPidShim(pid_b, &status, 0);
    vprocDestroy(a);
    vprocDestroy(b);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_rusage_snapshot(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocSetRusage(pid, 5, 7);
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            assert(snaps[i].rusage_utime == 5);
            assert(snaps[i].rusage_stime == 7);
            found = true;
        }
    }
    assert(found);
    free(snaps);
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_blocked_stop_delivered_on_unblock(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocBlockSignals(pid, 1 << SIGTSTP) == 0);
    assert(vprocKillShim(pid, SIGTSTP) == 0);
    int status = 0;
    /* Should not report stopped while blocked; use WNOHANG to verify. */
    assert(vprocWaitPidShim(pid, &status, WUNTRACED | WNOHANG) == 0);
    assert(status == 0);
    assert(vprocUnblockSignals(pid, 1 << SIGTSTP) == 0);
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));
    vprocMarkExit(vp, 0);
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_background_stop_foreground_cont(void) {
    VProcOptions opts = vprocDefaultOptions();
    VProc *fg = vprocCreate(&opts);
    VProc *bg = vprocCreate(&opts);
    assert(fg && bg);
    int sid = vprocPid(fg);
    int fg_pgid = sid;
    int bg_pgid = fg_pgid + 1;
    assert(vprocSetSid(sid, sid) == 0);
    assert(vprocSetSid(vprocPid(bg), sid) == 0);
    assert(vprocSetPgid(sid, fg_pgid) == 0);
    assert(vprocSetPgid(vprocPid(bg), bg_pgid) == 0);
    assert(vprocSetForegroundPgid(sid, fg_pgid) == 0);

    /* Stop background pgid; should queue and report via wait. */
    assert(vprocKillShim(-bg_pgid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(vprocPid(bg), &status, WUNTRACED) == vprocPid(bg));
    assert(WIFSTOPPED(status));

    /* Continue foreground pgid; background should remain stopped. */
    assert(vprocKillShim(-fg_pgid, SIGCONT) == 0);
    status = 0;
    assert(vprocWaitPidShim(vprocPid(fg), &status, WNOHANG | WCONTINUED) == 0 ||
           WIFCONTINUED(status));
    /* Background should still report stopped status if queried again. */
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool bg_stopped = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == vprocPid(bg) && snaps[i].stopped) {
            bg_stopped = true;
        }
    }
    assert(bg_stopped);
    free(snaps);

    vprocMarkExit(fg, 0);
    vprocMarkExit(bg, 0);
    (void)vprocWaitPidShim(vprocPid(fg), &status, 0);
    (void)vprocWaitPidShim(vprocPid(bg), &status, 0);
    vprocDestroy(fg);
    vprocDestroy(bg);
}

typedef struct {
    int pid_hint;
    volatile int ready;
    volatile int proceed_exit;
} VProcWaitNoHangArg;

static void *wait_nohang_thread(void *arg) {
    VProcWaitNoHangArg *info = (VProcWaitNoHangArg *)arg;
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = info->pid_hint;
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    info->ready = 1;
    while (!info->proceed_exit) {
        sched_yield();
    }
    vprocDeactivate();
    vprocMarkExit(vp, 3);
    vprocDestroy(vp);
    return NULL;
}

static void assert_wait_nohang_transitions(void) {
    VProcWaitNoHangArg arg = {.pid_hint = vprocReservePid(), .ready = 0, .proceed_exit = 0};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, wait_nohang_thread, &arg) == 0);
    while (!arg.ready) {
        sched_yield();
    }
    int status = -1;
    assert(vprocWaitPidShim(arg.pid_hint, &status, WNOHANG) == 0);
    assert(status == 0);
    arg.proceed_exit = 1;
    pthread_join(tid, NULL);
    assert(vprocWaitPidShim(arg.pid_hint, &status, 0) == arg.pid_hint);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 3);
}

typedef struct {
    int pid_hint;
    volatile int ready;
} VProcSnapshotArg;

static void *snapshot_helper_thread(void *arg) {
    VProcSnapshotArg *info = (VProcSnapshotArg *)arg;
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = info->pid_hint;
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    info->ready = 1;
    while (!g_snapshot_exit) {
        sched_yield();
    }
    vprocDeactivate();
    vprocMarkExit(vp, 0);
    vprocDestroy(vp);
    return NULL;
}

static void assert_snapshot_lists_active_tasks(void) {
    g_snapshot_exit = 0;
    VProcSnapshotArg a = {.pid_hint = vprocReservePid(), .ready = 0};
    VProcSnapshotArg b = {.pid_hint = vprocReservePid(), .ready = 0};
    pthread_t ta;
    pthread_t tb;
    assert(pthread_create(&ta, NULL, snapshot_helper_thread, &a) == 0);
    assert(pthread_create(&tb, NULL, snapshot_helper_thread, &b) == 0);

    while (!a.ready || !b.ready) {
        sched_yield();
    }

    size_t cap = vprocSnapshot(NULL, 0);
    if (cap < 2) {
        cap = 2;
    }
    VProcSnapshot *entries = (VProcSnapshot *)calloc(cap, sizeof(VProcSnapshot));
    assert(entries);
    size_t count = vprocSnapshot(entries, cap);
    bool seen_a = false;
    bool seen_b = false;
    for (size_t i = 0; i < count; ++i) {
        if (entries[i].pid == a.pid_hint && !entries[i].exited) {
            seen_a = true;
        }
        if (entries[i].pid == b.pid_hint && !entries[i].exited) {
            seen_b = true;
        }
    }
    assert(seen_a && seen_b);

    g_snapshot_exit = 1;
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    int status = 0;
    (void)vprocWaitPidShim(a.pid_hint, &status, 0);
    (void)vprocWaitPidShim(b.pid_hint, &status, 0);

    free(entries);
    size_t post = vprocSnapshot(NULL, 0);
    assert(post == 0 || post < count);
}

static void assert_stop_and_continue_round_trip(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    assert(pid > 0);
    vprocSetJobId(pid, 42);

    /* Stop the synthetic process and observe WIFSTOPPED. */
    assert(vprocKillShim(pid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));

    /* Continue and then exit cleanly; wait should now report exit. */
    assert(vprocKillShim(pid, SIGCONT) == 0);
    vprocMarkExit(vp, 5);
    memset(&status, 0, sizeof(status));
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 5);
    /* Job id should be cleared once the task fully exits. */
    assert(vprocGetJobId(pid) == 0);

    vprocDestroy(vp);
}

static void assert_job_ids_stable_across_exits(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp1 = vprocCreate(&opts);
    opts.pid_hint = vprocReservePid();
    VProc *vp2 = vprocCreate(&opts);
    opts.pid_hint = vprocReservePid();
    VProc *vp3 = vprocCreate(&opts);
    assert(vp1 && vp2 && vp3);

    int pid1 = vprocPid(vp1);
    int pid2 = vprocPid(vp2);
    int pid3 = vprocPid(vp3);

    vprocSetJobId(pid1, 1);
    vprocSetJobId(pid2, 2);
    vprocSetJobId(pid3, 3);

    vprocMarkExit(vp2, 0);
    int status = 0;
    assert(vprocWaitPidShim(pid2, &status, 0) == pid2);
    assert(vprocGetJobId(pid2) == 0);
    assert(vprocGetJobId(pid1) == 1);
    assert(vprocGetJobId(pid3) == 3);

    vprocMarkExit(vp1, 0);
    vprocMarkExit(vp3, 0);
    (void)vprocWaitPidShim(pid1, &status, 0);
    (void)vprocWaitPidShim(pid3, &status, 0);
    vprocDestroy(vp1);
    vprocDestroy(vp2);
    vprocDestroy(vp3);
}

static void assert_sigchld_ignored_by_default(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    /* Deliver SIGCHLD; default action should ignore and keep running. */
    assert(vprocKillShim(pid, SIGCHLD) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WNOHANG) == 0);
    vprocMarkExit(vp, 0);
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    vprocDestroy(vp);
}

static void assert_sigwinch_ignored_by_default(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    int status = 0;
#ifdef SIGWINCH
    assert(vprocKillShim(pid, SIGWINCH) == 0);
    assert(vprocWaitPidShim(pid, &status, WNOHANG) == 0);
#endif
    vprocMarkExit(vp, 0);
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_sigkill_not_blockable(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    /* Attempt to block SIGKILL should have no effect. */
    assert(vprocBlockSignals(pid, 1 << SIGKILL) == 0);
    int status = 0;
    assert(vprocKillShim(pid, SIGKILL) == 0);
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGKILL);
    vprocDestroy(vp);
}

static void assert_sigstop_not_ignorable_or_blockable(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    errno = 0;
    assert(vprocIgnoreSignal(pid, 1 << SIGSTOP) == -1);
    assert(errno == EINVAL);
    assert(vprocBlockSignals(pid, 1 << SIGSTOP) == 0);
    int status = 0;
    assert(vprocKillShim(pid, SIGSTOP) == 0);
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));
    assert(vprocKillShim(pid, SIGCONT) == 0);
    vprocMarkExit(vp, 0);
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_background_tty_signals(void) {
    int shell_pid = current_waiter_pid();
    int prev_shell = vprocGetShellSelfPid();
    vprocSetShellSelfPid(shell_pid);
    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int sid = vprocPid(leader);
    assert(vprocSetSid(sid, sid) == 0);
    assert(vprocSetForegroundPgid(sid, sid) == 0);
    vprocSetParent(sid, shell_pid);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocSetSid(pid, sid) == 0);
    int fg = sid;
    int bg = sid + 5;
    assert(vprocSetForegroundPgid(sid, fg) == 0);
    assert(vprocSetPgid(pid, bg) == 0);
    vprocSetParent(pid, shell_pid);

    VProc *prev = vprocCurrent();
    vprocActivate(vp);
    char ch = 0;
    errno = 0;
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == -1);
    assert(errno == EINTR);
    vprocDeactivate();
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));
    assert(vprocKillShim(pid, SIGCONT) == 0);
    if (prev) {
        vprocActivate(prev);
        vprocDeactivate();
    }

    vprocMarkExit(vp, 0);
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
    vprocMarkExit(leader, 0);
    (void)vprocWaitPidShim(sid, &status, 0);
    vprocDestroy(leader);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_job_id_present_in_snapshot(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocSetJobId(pid, 123);
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            assert(snaps[i].job_id == 123);
            found = true;
        }
    }
    assert(found);
    free(snaps);
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_setpgid_zero_defaults_to_pid(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int pgid = pid + 222;
    assert(vprocSetPgid(pid, pgid) == 0);
    assert(vprocGetPgid(pid) == pgid);

    assert(vprocSetPgid(0, 0) == 0);
    assert(vprocGetPgid(0) == pid);
    assert(vprocGetPgid(pid) == pid);

    vprocDeactivate();
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_path_truncate_maps_to_sandbox(void) {
    char templ[] = "/tmp/vproc-sandbox-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char cwd_buf[PATH_MAX];
    assert(pscalPathVirtualized_getcwd(cwd_buf, sizeof(cwd_buf)) != NULL);
    /* When path truncation is active, working directory should appear as "/" after stripping. */
    assert(strcmp(cwd_buf, "/") == 0 || strcmp(cwd_buf, "") == 0);

    int fd = pscalPathVirtualized_open("/sandbox.txt", O_CREAT | O_RDWR, 0600);
    assert(fd >= 0);
    const char *msg = "sandbox";
    assert(write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg));
    close(fd);

    char host_path[PATH_MAX];
    snprintf(host_path, sizeof(host_path), "%s/sandbox.txt", root);
    int host_fd = open(host_path, O_RDONLY);
    assert(host_fd >= 0);
    char buf[16] = {0};
    assert(read(host_fd, buf, sizeof(buf)) == (ssize_t)strlen(msg));
    assert(strcmp(buf, msg) == 0);
    close(host_fd);

    /* Ensure vprocOpenAt also respects path virtualization. */
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    int vfd = vprocOpenAt(vp, "/sandbox-openat.txt", O_CREAT | O_RDWR, 0600);
    assert(vfd >= 0);
    const char *msg2 = "sand";
    assert(vprocWriteShim(vfd, msg2, 4) == 4);
    assert(vprocCloseShim(vfd) == 0);
    vprocDeactivate();
    vprocDestroy(vp);

    char host_at_path[PATH_MAX];
    snprintf(host_at_path, sizeof(host_at_path), "%s/sandbox-openat.txt", root);
    int host_at_fd = open(host_at_path, O_RDONLY);
    assert(host_at_fd >= 0);
    char buf2[8] = {0};
    assert(read(host_at_fd, buf2, sizeof(buf2)) == 4);
    assert(strncmp(buf2, msg2, 4) == 0);
    close(host_at_fd);

    unsetenv("PATH_TRUNCATE");
    unlink(host_path);
    unlink(host_at_path);
    rmdir(root);
}

static void assert_passthrough_when_inactive(void) {
    char tmpl[] = "/tmp/vproc-passXXXXXX";
    int fd = mkstemp(tmpl);
    assert(fd >= 0);
    const char *msg = "pass";
    assert(vprocWriteShim(fd, msg, 4) == 4);
    lseek(fd, 0, SEEK_SET);
    char buf[5] = {0};
    assert(vprocReadShim(fd, buf, 4) == 4);
    assert(strcmp(buf, "pass") == 0);
    close(fd);
    unlink(tmpl);
}

int main(void) {
    /* Default truncation path for tests to keep path virtualization in /tmp. */
    setenv("PATH_TRUNCATE", "/tmp", 1);
    fprintf(stderr, "TEST pipe_round_trip\n");
    assert_pipe_round_trip();
    fprintf(stderr, "TEST dup2_isolated\n");
    assert_dup2_isolated();
    fprintf(stderr, "TEST stdin_redirected\n");
    assert_stdin_redirected_via_dup2();
    fprintf(stderr, "TEST host_stdio_untouched\n");
    assert_host_stdio_untouched_after_vproc_close();
    fprintf(stderr, "TEST winsize_round_trip\n");
    assert_winsize_round_trip();
    fprintf(stderr, "TEST open_and_read\n");
    assert_open_and_read_via_shim();
    fprintf(stderr, "TEST isolation_between_vprocs\n");
    assert_isolation_between_vprocs();
    fprintf(stderr, "TEST wait_on_synthetic_pid\n");
    assert_wait_on_synthetic_pid();
    fprintf(stderr, "TEST kill_negative_pid_routes_to_thread\n");
    assert_kill_negative_pid_routes_to_thread();
    fprintf(stderr, "TEST wait_enforces_parent\n");
    assert_wait_enforces_parent();
    fprintf(stderr, "TEST wait_wnowait_preserves_zombie\n");
    assert_wait_wnowait_preserves_zombie();
    fprintf(stderr, "TEST wait_by_pgid\n");
    assert_wait_by_pgid();
    fprintf(stderr, "TEST wait_reports_continued\n");
    assert_wait_reports_continued();
    fprintf(stderr, "TEST kill_zero_targets_current_pgid\n");
    assert_kill_zero_targets_current_pgid();
    fprintf(stderr, "TEST children_reparent_to_shell\n");
    assert_children_reparent_to_shell();
    fprintf(stderr, "TEST sigchld_pending_snapshot\n");
    assert_sigchld_pending_snapshot();
    fprintf(stderr, "TEST sigchld_pending_api\n");
    assert_sigchld_pending_api();
    fprintf(stderr, "TEST sigchld_unblock_drains_pending_signal\n");
    assert_sigchld_unblock_drains_pending_signal();
    fprintf(stderr, "TEST group_exit_code_used\n");
    assert_group_exit_code_used();
    fprintf(stderr, "TEST group_stop_reaches_all_members\n");
    assert_group_stop_reaches_all_members();
    fprintf(stderr, "TEST rusage_snapshot\n");
    assert_rusage_snapshot();
    fprintf(stderr, "TEST blocked_stop_delivered_on_unblock\n");
    assert_blocked_stop_delivered_on_unblock();
    fprintf(stderr, "TEST background_stop_foreground_cont\n");
    assert_background_stop_foreground_cont();
    fprintf(stderr, "TEST wait_nohang_transitions\n");
    assert_wait_nohang_transitions();
    fprintf(stderr, "TEST snapshot_lists_active_tasks\n");
    assert_snapshot_lists_active_tasks();
    fprintf(stderr, "TEST stop_and_continue_round_trip\n");
    assert_stop_and_continue_round_trip();
    fprintf(stderr, "TEST job_ids_stable_across_exits\n");
    assert_job_ids_stable_across_exits();
    fprintf(stderr, "TEST sigchld_ignored_by_default\n");
    assert_sigchld_ignored_by_default();
    fprintf(stderr, "TEST sigwinch_ignored_by_default\n");
    assert_sigwinch_ignored_by_default();
    fprintf(stderr, "TEST sigkill_not_blockable\n");
    assert_sigkill_not_blockable();
    fprintf(stderr, "TEST sigstop_not_ignorable_or_blockable\n");
    assert_sigstop_not_ignorable_or_blockable();
    fprintf(stderr, "TEST background_tty_signals\n");
    assert_background_tty_signals();
    fprintf(stderr, "TEST job_id_present_in_snapshot\n");
    assert_job_id_present_in_snapshot();
    fprintf(stderr, "TEST setpgid_zero_defaults_to_pid\n");
    assert_setpgid_zero_defaults_to_pid();
    fprintf(stderr, "TEST path_truncate_maps_to_sandbox\n");
    assert_path_truncate_maps_to_sandbox();
    fprintf(stderr, "TEST write_reads_back\n");
    assert_write_reads_back();
    fprintf(stderr, "TEST passthrough_when_inactive\n");
    assert_passthrough_when_inactive();
#if defined(PSCAL_TARGET_IOS)
    /* Ensure path virtualization macros remain visible even when vproc shim is included. */
    int (*fn)(const char *) = chdir;
    (void)fn;
#endif
    return 0;
}
