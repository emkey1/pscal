#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#include "ios/tty/pscal_pty.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

#include "ios/vproc.h"

static int current_waiter_pid(void);

static void burn_cpu_for_ms(int ms) {
    struct timespec start;
    assert(clock_gettime(CLOCK_MONOTONIC, &start) == 0);
    for (;;) {
        struct timespec now;
        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
        int64_t elapsed_ms = (int64_t)(now.tv_sec - start.tv_sec) * 1000 +
                             (int64_t)(now.tv_nsec - start.tv_nsec) / 1000000;
        if (elapsed_ms >= ms) {
            break;
        }
    }
}

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

static void assert_pipe_cross_vproc(void) {
    int p[2];
    assert(vprocHostPipe(p) == 0);

    VProc *writer = vprocCreate(NULL);
    VProc *reader = vprocCreate(NULL);
    assert(writer && reader);

    vprocActivate(writer);
    int wfd = vprocAdoptHostFd(writer, p[1]);
    assert(wfd >= 0);
    const char *msg = "ok";
    assert(vprocWriteShim(wfd, msg, 2) == 2);
    assert(vprocCloseShim(wfd) == 0);
    vprocDeactivate();
    vprocDestroy(writer);

    vprocActivate(reader);
    int rfd = vprocAdoptHostFd(reader, p[0]);
    char buf[4] = {0};
    assert(vprocReadShim(rfd, buf, sizeof(buf)) == 2);
    assert(strncmp(buf, "ok", 2) == 0);
    assert(vprocReadShim(rfd, buf, sizeof(buf)) == 0);
    assert(vprocCloseShim(rfd) == 0);
    vprocDeactivate();
    vprocDestroy(reader);
}

static void assert_socket_closed_on_destroy(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    assert(s >= 0);
    int reuse = 1;
    assert(setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    socklen_t addrlen = sizeof(addr);
    int bind_rc = bind(s, (struct sockaddr *)&addr, addrlen);
    if (bind_rc != 0) {
        if (errno == EPERM || errno == EACCES) {
            /* Some sandboxes block AF_INET binds; fall back to a socketpair-based closure check. */
            close(s);
            vprocDeactivate();
            vprocDestroy(vp);

            int sv[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
            VProc *sp = vprocCreate(NULL);
            assert(sp);
            vprocActivate(sp);
            int tracked = vprocAdoptHostFd(sp, sv[0]);
            assert(tracked >= 0);
            vprocDeactivate();
            vprocDestroy(sp);
            char tmp;
            assert(read(sv[1], &tmp, 1) == 0);
            close(sv[1]);
            return;
        }
        fprintf(stderr, "bind failed: %s\n", strerror(errno));
    }
    assert(bind_rc == 0);
    assert(getsockname(s, (struct sockaddr *)&addr, &addrlen) == 0);
    int port = ntohs(addr.sin_port);
    assert(listen(s, 1) == 0);

    vprocDeactivate();
    vprocDestroy(vp);

    int s2 = socket(AF_INET, SOCK_STREAM, 0);
    assert(s2 >= 0);
    assert(setsockopt(s2, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);
    addr.sin_port = htons(port);
    assert(bind(s2, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(s2);
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

static void assert_dev_tty_available_in_pipeline(void) {
    struct pscal_fd *pty_master = NULL;
    struct pscal_fd *pty_slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &pty_master, &pty_num) == 0);
    assert(pscalPtyUnlock(pty_master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &pty_slave) == 0);

    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, pty_slave, pty_master, 777, 1) == 0);
    vprocSessionStdioActivate(session);

    VProc *shell = vprocCreate(NULL);
    assert(shell);
    int shell_pid = vprocPid(shell);
    vprocActivate(shell);
    assert(vprocAdoptPscalStdio(shell,
                                session->stdin_pscal_fd,
                                session->stdout_pscal_fd,
                                session->stderr_pscal_fd) == 0);
    vprocSetShellSelfPid(shell_pid);
    vprocSetSid(shell_pid, shell_pid);
    vprocSetPgid(shell_pid, shell_pid);
    vprocSetForegroundPgid(shell_pid, shell_pid);
    vprocDeactivate();

    int pipefd[2];
    assert(vprocHostPipe(pipefd) == 0);
    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = pipefd[0];
    opts.stdout_fd = pipefd[1];
    opts.stderr_fd = pipefd[1];
    VProc *stage = vprocCreate(&opts);
    close(pipefd[0]);
    close(pipefd[1]);
    assert(stage);
    vprocSetSid(vprocPid(stage), shell_pid);
    vprocSetPgid(vprocPid(stage), shell_pid);
    vprocActivate(stage);

    int tty_vfd = vprocOpenShim("/dev/tty", O_RDWR, 0);
    assert(tty_vfd >= 0);
    assert(vprocIsattyShim(tty_vfd) == 1);
    assert(vprocCloseShim(tty_vfd) == 0);

    vprocDeactivate();
    vprocDestroy(stage);
    vprocDestroy(shell);
    vprocSessionStdioActivate(NULL);
    vprocSessionStdioDestroy(session);
}

typedef struct {
    int pid;
    int parent_pid;
} VProcWaitArg;

static void *wait_helper_thread(void *arg) {
    VProcWaitArg *info = (VProcWaitArg *)arg;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    if (info->parent_pid > 0) {
        vprocSetParent(pid, info->parent_pid);
    }
    info->pid = pid;
    vprocMarkExit(vp, 7);
    vprocDeactivate();
    vprocDestroy(vp);
    return NULL;
}

static void assert_wait_on_synthetic_pid(void) {
    pthread_t tid;
    VProcWaitArg arg = {.pid = -1, .parent_pid = current_waiter_pid()};
    struct sigaction sa_reset;
    memset(&sa_reset, 0, sizeof(sa_reset));
    sa_reset.sa_handler = SIG_DFL;
    sa_reset.sa_flags = 0;
    sigemptyset(&sa_reset.sa_mask);
    assert(vprocSigaction(arg.parent_pid, SIGCHLD, &sa_reset, NULL) == 0);
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
static volatile sig_atomic_t g_handler_hits = 0;
static volatile sig_atomic_t g_handler_sig = 0;
static volatile sig_atomic_t g_siginfo_hits = 0;
static volatile sig_atomic_t g_siginfo_signo = 0;

static void test_handler(int signo) {
    g_handler_hits++;
    g_handler_sig = signo;
}

static void test_siginfo_handler(int signo, siginfo_t *info, void *ctx) {
    (void)ctx;
    g_siginfo_hits++;
    g_siginfo_signo = (info ? info->si_signo : 0);
    g_handler_sig = signo;
}

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
    int prev_shell = vprocGetShellSelfPid();
    int waiter = (int)getpid();
    vprocSetShellSelfPid(waiter);
    struct sigaction sa_reset;
    memset(&sa_reset, 0, sizeof(sa_reset));
    sa_reset.sa_handler = SIG_DFL;
    sa_reset.sa_flags = 0;
    sigemptyset(&sa_reset.sa_mask);
    assert(vprocSigaction(waiter, SIGCHLD, &sa_reset, NULL) == 0);

    /* Child owned by a different parent must not be waitable. */
    VProc *vp_other_parent = vprocCreate(NULL);
    assert(vp_other_parent);
    int other_pid = vprocPid(vp_other_parent);
    vprocSetParent(other_pid, waiter + 9999);
    vprocMarkExit(vp_other_parent, 9);

    int status = 0;
    errno = 0;
    int got = vprocWaitPidShim(other_pid, &status, 0);
    assert(got == -1);
    assert(errno == ECHILD);
    vprocDestroy(vp_other_parent);

    /* Child owned by waiter must be waitable. */
    VProc *vp_waiter_parent = vprocCreate(NULL);
    assert(vp_waiter_parent);
    int own_pid = vprocPid(vp_waiter_parent);
    vprocSetParent(own_pid, waiter);
    struct sigaction sa_now;
    memset(&sa_now, 0, sizeof(sa_now));
    assert(vprocSigaction(waiter, SIGCHLD, NULL, &sa_now) == 0);
    vprocMarkExit(vp_waiter_parent, 9);

    errno = 0;
    got = vprocWaitPidShim(own_pid, &status, 0);
    if (got != own_pid) {
        size_t cap = vprocSnapshot(NULL, 0);
        VProcSnapshot *snaps = (VProcSnapshot *)calloc(cap ? cap : 1, sizeof(VProcSnapshot));
        size_t count = snaps ? vprocSnapshot(snaps, cap ? cap : 1) : 0;
        fprintf(stderr,
                "  [wait-parent2] own_pid=%d got=%d errno=%d status=%d waiter=%d shell=%d host=%d count=%zu\n",
                own_pid, got, errno, status, waiter, vprocGetShellSelfPid(), (int)getpid(), count);
        fprintf(stderr, "  [wait-parent2] waiter SIGCHLD handler=%p flags=0x%x\n",
                (void *)sa_now.sa_handler, sa_now.sa_flags);
        for (size_t i = 0; i < count; ++i) {
            if (snaps[i].pid == own_pid || snaps[i].pid == waiter || snaps[i].pid == waiter + 9999) {
                fprintf(stderr,
                        "  [wait-parent2] snap pid=%d ppid=%d exited=%d zombie=%d sigchld=%d status=%d\n",
                        snaps[i].pid,
                        snaps[i].parent_pid,
                        snaps[i].exited ? 1 : 0,
                        snaps[i].zombie ? 1 : 0,
                        snaps[i].sigchld_pending ? 1 : 0,
                        snaps[i].status);
            }
        }
        free(snaps);
    }
    assert(got == own_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 9);
    vprocDestroy(vp_waiter_parent);
    vprocSetShellSelfPid(prev_shell);
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
    int prev_shell = vprocGetShellSelfPid();

    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int sid = vprocPid(leader);
    vprocSetShellSelfPid(sid);
    assert(vprocSetSid(sid, sid) == 0);

    VProcOptions opts = vprocDefaultOptions();
    VProc *vp1 = vprocCreate(&opts);
    VProc *vp2 = vprocCreate(&opts);
    assert(vp1 && vp2);
    int pid1 = vprocPid(vp1);
    int pid2 = vprocPid(vp2);
    int pgid = pid1;
    assert(vprocGetSid(pid1) == sid);
    assert(vprocGetSid(pid2) == sid);
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
    vprocMarkExit(leader, 0);
    vprocDiscard(sid);
    vprocDestroy(leader);
    vprocSetShellSelfPid(prev_shell);
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

static void assert_task_slots_reused_after_reap(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);

    for (int i = 0; i < 4200; ++i) {
        VProc *vp = vprocCreate(NULL);
        assert(vp);
        int pid = vprocPid(vp);
        vprocSetParent(pid, shell_pid);
        vprocMarkExit(vp, i & 0xff);

        int status = 0;
        assert(vprocWaitPidShim(pid, &status, 0) == pid);
        assert(WIFEXITED(status));
        assert(WEXITSTATUS(status) == (i & 0xff));
        vprocDestroy(vp);
    }

    vprocSetShellSelfPid(prev_shell);
}

static void assert_reserve_pid_reports_capacity(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);

    const int attempts = 5000;
    int *reserved = (int *)calloc((size_t)attempts, sizeof(int));
    assert(reserved);

    int reserved_count = 0;
    bool saw_capacity_error = false;
    for (int i = 0; i < attempts; ++i) {
        errno = 0;
        int pid = vprocReservePid();
        if (pid < 0) {
            assert(errno == EMFILE);
            saw_capacity_error = true;
            break;
        }
        reserved[reserved_count++] = pid;
    }
    assert(saw_capacity_error);

    for (int i = 0; i < reserved_count; ++i) {
        vprocDiscard(reserved[i]);
    }
    free(reserved);
    vprocSetShellSelfPid(prev_shell);
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
    assert(vprocKillShim(0, 0) == 0);
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
            assert(snaps[i].rusage_utime >= 5);
            assert(snaps[i].rusage_stime >= 7);
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

static void assert_rusage_populated_on_exit(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    burn_cpu_for_ms(30);
    vprocMarkExit(vp, 0);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            found = true;
            assert(snaps[i].rusage_utime >= 1);
        }
    }
    assert(found);
    free(snaps);
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

static void assert_child_inherits_sid_and_pgid(void) {
    VProc *parent = vprocCreate(NULL);
    assert(parent);
    int parent_pid = vprocPid(parent);
    int sid = parent_pid;
    int pgid = sid;
    int fg = sid + 7;
    assert(vprocSetSid(parent_pid, sid) == 0);
    assert(vprocSetPgid(parent_pid, pgid) == 0);
    assert(vprocSetForegroundPgid(sid, fg) == 0);

    vprocActivate(parent);
    VProc *child = vprocCreate(NULL);
    vprocDeactivate();
    assert(child);
    int child_pid = vprocPid(child);
    assert(vprocGetSid(child_pid) == sid);
    assert(vprocGetPgid(child_pid) == pgid);
    assert(vprocGetForegroundPgid(sid) == fg);

    vprocMarkExit(parent, 0);
    vprocMarkExit(child, 0);
    int status = 0;
    (void)vprocWaitPidShim(parent_pid, &status, 0);
    (void)vprocWaitPidShim(child_pid, &status, 0);
    vprocDestroy(parent);
    vprocDestroy(child);
}

static void assert_child_inherits_signal_state(void) {
    VProc *parent = vprocCreate(NULL);
    assert(parent);
    int parent_pid = vprocPid(parent);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = test_handler;
    sigemptyset(&sa.sa_mask);
    assert(vprocSigaction(parent_pid, SIGUSR1, &sa, NULL) == 0);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    assert(vprocSigprocmask(parent_pid, SIG_BLOCK, &mask, NULL) == 0);

    vprocActivate(parent);
    VProc *child = vprocCreate(NULL);
    vprocDeactivate();
    assert(child);
    int child_pid = vprocPid(child);

    struct sigaction child_sa;
    memset(&child_sa, 0, sizeof(child_sa));
    assert(vprocSigaction(child_pid, SIGUSR1, NULL, &child_sa) == 0);
    assert(child_sa.sa_handler == test_handler);

    sigset_t child_mask;
    sigemptyset(&child_mask);
    assert(vprocSigprocmask(child_pid, SIG_BLOCK, NULL, &child_mask) == 0);
    assert(sigismember(&child_mask, SIGUSR2));

    vprocMarkExit(parent, 0);
    vprocMarkExit(child, 0);
    int status = 0;
    (void)vprocWaitPidShim(parent_pid, &status, 0);
    (void)vprocWaitPidShim(child_pid, &status, 0);
    vprocDestroy(parent);
    vprocDestroy(child);
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

static void assert_sigchld_nocldstop(void) {
    int parent = current_waiter_pid();
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_NOCLDSTOP;
    assert(vprocSigaction(parent, SIGCHLD, &sa, NULL) == 0);
    vprocClearSigchldPending(parent);

    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, parent);

    assert(vprocKillShim(cpid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(cpid, &status, WUNTRACED) == cpid);
    assert(WIFSTOPPED(status));
    /* Should not have SIGCHLD pending due to SA_NOCLDSTOP. */
    assert(!vprocSigchldPending(parent));
    assert(vprocKillShim(cpid, SIGCONT) == 0);
    vprocMarkExit(child, 0);
    (void)vprocWaitPidShim(cpid, &status, 0);
    vprocDestroy(child);
}

static void assert_sigchld_nocldwait_reaps(void) {
    int parent = current_waiter_pid();
    struct sigaction sa = {0};
    sa.sa_handler = SIG_DFL;
    sa.sa_flags = SA_NOCLDWAIT;
    assert(vprocSigaction(parent, SIGCHLD, &sa, NULL) == 0);

    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, parent);
    vprocMarkExit(child, 0);
    int status = 0;
    errno = 0;
    assert(vprocWaitPidShim(cpid, &status, 0) == -1);
    assert(errno == ECHILD);
    sigset_t pending;
    assert(vprocSigpending(parent, &pending) == 0);
    assert(!sigismember(&pending, SIGCHLD));
    vprocDestroy(child);
    /* Reset to defaults to avoid side effects. */
    struct sigaction sa_reset = {0};
    sa_reset.sa_handler = SIG_DFL;
    sa_reset.sa_flags = 0;
    vprocSigaction(parent, SIGCHLD, &sa_reset, NULL);
}

static void assert_sigsuspend_drains_pending(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocBlockSignals(pid, 1 << SIGUSR1) == 0);
    assert(vprocKillShim(pid, SIGUSR1) == 0);
    sigset_t mask;
    sigemptyset(&mask);
    errno = 0;
    assert(vprocSigsuspend(pid, &mask) == -1);
    assert(errno == EINTR);
    sigset_t pending;
    assert(vprocSigpending(pid, &pending) == 0);
    assert(!sigismember(&pending, SIGUSR1));
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void handler_resetting(int signo) { (void)signo; }

static void assert_sighandler_resets_with_sa_resethand(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    struct sigaction sa = {0};
    sa.sa_handler = handler_resetting;
    sa.sa_flags = SA_RESETHAND;
    assert(vprocSigaction(pid, SIGUSR2, &sa, NULL) == 0);

    /* First delivery should be treated as handled and reset disposition. */
    assert(vprocKillShim(pid, SIGUSR2) == 0);
    sigset_t pending;
    assert(vprocSigpending(pid, &pending) == 0);
    assert(!sigismember(&pending, SIGUSR2));

    /* Second delivery should follow default and terminate the vproc. */
    assert(vprocKillShim(pid, SIGUSR2) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGUSR2);
    vprocDestroy(vp);
}

static void assert_sigprocmask_round_trip(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGTERM);
    sigset_t old;
    assert(vprocSigprocmask(pid, SIG_SETMASK, &mask, &old) == 0);
    sigset_t now;
    sigemptyset(&now);
    assert(vprocSigpending(pid, &now) == 0);
    /* Verify mask set by blocking and delivering a signal then unblocking. */
    assert(vprocKillShim(pid, SIGTERM) == 0);
    sigset_t pending;
    assert(vprocSigpending(pid, &pending) == 0);
    assert(sigismember(&pending, SIGTERM));
    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGTERM);
    assert(vprocSigprocmask(pid, SIG_UNBLOCK, &unblock, NULL) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGTERM);
    vprocDestroy(vp);
}

static void assert_sigwait_receives_pending(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGUSR1);
    assert(vprocBlockSignals(pid, 1 << SIGUSR1) == 0);
    assert(vprocKillShim(pid, SIGUSR1) == 0);
    int got = 0;
    assert(vprocSigwait(pid, &waitset, &got) == 0);
    assert(got == SIGUSR1);
    sigset_t pending;
    assert(vprocSigpending(pid, &pending) == 0);
    assert(!sigismember(&pending, SIGUSR1));
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_sigtimedwait_timeout_and_drains(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    struct timespec to = {.tv_sec = 0, .tv_nsec = 1000000};
    int sig = 0;
    errno = 0;
    assert(vprocSigtimedwait(pid, &set, &to, &sig) == -1);
    assert(errno == EAGAIN);

    /* Queue two signals, ensure both are consumed separately. */
    assert(vprocBlockSignals(pid, 1 << SIGUSR1) == 0);
    assert(vprocKillShim(pid, SIGUSR1) == 0);
    assert(vprocKillShim(pid, SIGUSR1) == 0);
    assert(vprocSigtimedwait(pid, &set, NULL, &sig) == SIGUSR1);
    assert(vprocSigtimedwait(pid, &set, NULL, &sig) == SIGUSR1);
    sigset_t pending;
    assert(vprocSigpending(pid, &pending) == 0);
    assert(!sigismember(&pending, SIGUSR1));
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_signal_handler_invoked(void) {
    g_handler_hits = 0;
    g_handler_sig = 0;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    struct sigaction sa = {0};
    sa.sa_handler = test_handler;
    sa.sa_flags = 0;
    assert(vprocSigaction(pid, SIGUSR1, &sa, NULL) == 0);
    assert(vprocKillShim(pid, SIGUSR1) == 0);
    assert(g_handler_hits == 1);
    assert(g_handler_sig == SIGUSR1);
    sigset_t pending;
    assert(vprocSigpending(pid, &pending) == 0);
    assert(!sigismember(&pending, SIGUSR1));
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_siginfo_handler_invoked(void) {
    g_siginfo_hits = 0;
    g_siginfo_signo = 0;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocRegisterThread(vp, pthread_self());
    struct sigaction sa = {0};
    sa.sa_sigaction = test_siginfo_handler;
    sa.sa_flags = SA_SIGINFO;
    assert(vprocSigaction(pid, SIGUSR2, &sa, NULL) == 0);
    assert(vprocKillShim(pid, SIGUSR2) == 0);
    assert(g_siginfo_hits == 1);
    assert(g_siginfo_signo == SIGUSR2);
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

typedef struct {
    volatile int completed;
} vproc_self_cancel_ctx;

static void *vproc_self_cancel_thread(void *arg) {
    vproc_self_cancel_ctx *ctx = (vproc_self_cancel_ctx *)arg;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);
    assert(vprocKillShim(pid, SIGTERM) == 0);
    /* If self-cancel regresses, this call will cancel the thread immediately. */
    pthread_testcancel();
    vprocDeactivate();
    vprocDestroy(vp);
    ctx->completed = 1;
    return NULL;
}

static void assert_kill_does_not_self_cancel(void) {
    vproc_self_cancel_ctx ctx = {0};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, vproc_self_cancel_thread, &ctx) == 0);
    void *ret = NULL;
    assert(pthread_join(tid, &ret) == 0);
    assert(ret != PTHREAD_CANCELED);
    assert(ctx.completed == 1);
}

static void assert_background_tty_signals(void) {
    int prev_shell = vprocGetShellSelfPid();
    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int sid = vprocPid(leader);
    vprocSetShellSelfPid(sid);
    assert(vprocSetSid(sid, sid) == 0);
    assert(vprocSetForegroundPgid(sid, sid) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    int fg = sid;
    int bg = sid + 5;
    assert(vprocSetForegroundPgid(sid, fg) == 0);
    assert(vprocSetPgid(pid, bg) == 0);

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

static void assert_getpid_falls_back_to_shell_and_create_inherits_session(void) {
    int prev_shell = vprocGetShellSelfPid();

    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int sid = vprocPid(leader);
    vprocSetShellSelfPid(sid);
    assert(vprocGetPidShim() == sid);
    assert(vprocSetSid(sid, sid) == 0);

    VProc *child = vprocCreate(NULL);
    assert(child);
    int child_pid = vprocPid(child);
    assert(vprocGetSid(child_pid) == sid);
    assert(vprocGetPgid(child_pid) == vprocGetPgid(sid));

    vprocMarkExit(child, 0);
    int status = 0;
    (void)vprocWaitPidShim(child_pid, &status, 0);
    vprocDestroy(child);

    vprocMarkExit(leader, 0);
    vprocDiscard(sid);
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

static void assert_gps_alias_reads_location_payload(void) {
    const char *payload = "gps-payload";

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/gps", O_RDONLY);
    assert(fd >= 0);
    assert(vprocLocationDeviceWrite(payload, strlen(payload)) == (ssize_t)strlen(payload));
    char buf[32] = {0};
    ssize_t r = vprocReadShim(fd, buf, sizeof(buf));
    assert(r == (ssize_t)strlen(payload));
    assert(strncmp(buf, payload, (size_t)r) == 0);
    assert(vprocCloseShim(fd) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
}

typedef struct {
    const char *payload;
} location_writer_ctx;

static void *location_writer_thread(void *arg) {
    location_writer_ctx *ctx = (location_writer_ctx *)arg;
    usleep(50000); /* 50ms */
    assert(vprocLocationDeviceWrite(ctx->payload, strlen(ctx->payload)) == (ssize_t)strlen(ctx->payload));
    return NULL;
}

static void assert_location_read_returns_full_line_and_eof(void) {
    const char *payload = "abcde12345\n";
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/location", O_RDONLY);
    assert(fd >= 0);

    /* Delay writing so the first read blocks until the payload arrives. */
    pthread_t writer;
    location_writer_ctx ctx = { .payload = payload };
    int rc = pthread_create(&writer, NULL, location_writer_thread, &ctx);
    assert(rc == 0);

    char buf[32] = {0};
    ssize_t r1 = vprocReadShim(fd, buf, sizeof(buf));
    assert(r1 == (ssize_t)strlen(payload));
    assert(strcmp(buf, payload) == 0);

    pthread_join(writer, NULL);

    /* Subsequent reads should return EOF so tail-like consumers exit. */
    errno = 0;
    ssize_t r2 = vprocReadShim(fd, buf, sizeof(buf));
    assert(r2 == 0);
    assert(errno == 0);

    assert(vprocCloseShim(fd) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_location_poll_wakes_on_payload(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/location", O_RDONLY);
    assert(fd >= 0);

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    /* No payload yet, should time out. */
    assert(vprocPollShim(&pfd, 1, 50) == 0);

    const char *payload = "pollwake\n";
    assert(vprocLocationDeviceWrite(payload, strlen(payload)) == (ssize_t)strlen(payload));

    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, 250) == 1);
    assert((pfd.revents & POLLIN) != 0);

    char buf[16] = {0};
    ssize_t r1 = vprocReadShim(fd, buf, sizeof(buf));
    assert(r1 == (ssize_t)strlen(payload));
    assert(strcmp(buf, payload) == 0);

    /* After the line is consumed, poll should report hangup (EOF). */
    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, 0) == 1);
    assert((pfd.revents & POLLHUP) != 0);

    assert(vprocCloseShim(fd) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_select_sparse_fdset_works(void) {
    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int read_fd = vprocAdoptHostFd(vp, host_pipe[0]);
    assert(read_fd >= 0);
    assert(read_fd < FD_SETSIZE);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(read_fd, &rfds);
    struct timeval tv = {0, 0};
    assert(vprocSelectShim(1024, &rfds, NULL, NULL, &tv) == 0);

    const char byte = 'x';
    assert(vprocHostWrite(host_pipe[1], &byte, 1) == 1);

    FD_ZERO(&rfds);
    FD_SET(read_fd, &rfds);
    tv.tv_sec = 0;
    tv.tv_usec = 0;
    assert(vprocSelectShim(1024, &rfds, NULL, NULL, &tv) == 1);
    assert(FD_ISSET(read_fd, &rfds) != 0);

    char got = 0;
    assert(vprocReadShim(read_fd, &got, 1) == 1);
    assert(got == byte);

    assert(vprocCloseShim(read_fd) == 0);
    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_select_empty_set_honors_timeout(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    fd_set rfds;
    FD_ZERO(&rfds);
    struct timeval tv = {0, 0};
    assert(vprocSelectShim(512, &rfds, NULL, NULL, &tv) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_location_disable_unblocks_and_errors(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/location", O_RDONLY);
    assert(fd >= 0);

    /* Disable the device globally and ensure readers wake with error. */
    vprocLocationDeviceSetEnabled(false);

    struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
    int pr = vprocPollShim(&pfd, 1, 200);
    assert(pr == 1);
    assert((pfd.revents & POLLHUP) != 0);

    char buf[8];
    errno = 0;
    ssize_t r = vprocReadShim(fd, buf, sizeof(buf));
    assert(r == 0);
    assert(errno == 0);

    /* Re-enable for subsequent tests. */
    vprocLocationDeviceSetEnabled(true);
    assert(vprocCloseShim(fd) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

typedef struct {
    int values[4];
    int count;
} location_observer_state;

static void location_reader_observer(int readers, void *context) {
    location_observer_state *st = (location_observer_state *)context;
    if (!st || st->count >= (int)(sizeof(st->values) / sizeof(st->values[0]))) {
        return;
    }
    st->values[st->count++] = readers;
}

static void assert_location_reader_observer_fires(void) {
    location_observer_state state = { .values = { -1, -1, -1, -1 }, .count = 0 };
    vprocLocationDeviceRegisterReaderObserver(location_reader_observer, &state);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/location", O_RDONLY);
    assert(fd >= 0);
    assert(vprocCloseShim(fd) == 0);

    vprocDeactivate();
    vprocDestroy(vp);

    /* Initial callback reports current readers (0), then open bumps to 1, close back to 0. */
    assert(state.count >= 3);
    assert(state.values[0] == 0);
    assert(state.values[1] == 1);
    assert(state.values[2] == 0);

    /* Unregister to avoid leaking across tests. */
    vprocLocationDeviceRegisterReaderObserver(NULL, NULL);
}

static void assert_device_stat_bypasses_truncation(void) {
    struct stat st;
    /* Should hit the real device path, not PATH_TRUNCATE expansion. */
    assert(pscalPathVirtualized_stat("/dev/ptmx", &st) == 0);
}

static void assert_ptmx_open_registers_session(void) {
    /* Create a session with an initial pty to seed session_id. */
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);

    uint64_t session_id = 1234;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/ptmx", O_RDWR | O_NOCTTY);
    assert(fd >= 0);
    /* Master registered with session should accept writes via session API. */
    const char *msg = "hi";
    assert(vprocSessionWriteToMaster(session_id, msg, 2) == 2);
    assert(vprocCloseShim(fd) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
    vprocSessionStdioDestroy(session);
}

static void assert_job_id_and_label_round_trip(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    vprocSetJobId(pid, 7);
    vprocSetCommandLabel(pid, "jobcmd");
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            found = true;
            assert(snaps[i].job_id == 7);
            assert(strcmp(snaps[i].command, "jobcmd") == 0);
        }
    }
    assert(found);
    free(snaps);

    vprocSetJobId(pid, 0);
    vprocSetCommandLabel(pid, NULL);
    cap = vprocSnapshot(NULL, 0);
    snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    count = vprocSnapshot(snaps, cap);
    found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            found = true;
            assert(snaps[i].job_id == 0);
            assert(snaps[i].command[0] == '\0');
        }
    }
    assert(found);
    free(snaps);
    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_vproc_activation_stack_restores_previous(void) {
    /* Ensure vprocActivate/vprocDeactivate are nestable so the shell can keep a
     * baseline vproc active while pipeline stages temporarily override it. */
    VProc *vp1 = vprocCreate(NULL);
    VProc *vp2 = vprocCreate(NULL);
    assert(vp1 && vp2);

    assert(vprocCurrent() == NULL);
    vprocActivate(vp1);
    assert(vprocCurrent() == vp1);
    vprocActivate(vp2);
    assert(vprocCurrent() == vp2);
    vprocDeactivate();
    assert(vprocCurrent() == vp1);
    vprocDeactivate();
    assert(vprocCurrent() == NULL);

    vprocDestroy(vp2);
    vprocDestroy(vp1);
}

static int snapshot_find_parent(const VProcSnapshot *snaps, size_t count, int pid) {
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            return snaps[i].parent_pid;
        }
    }
    return -1;
}

static void assert_self_parent_is_rejected(void) {
    VProc *parent = vprocCreate(NULL);
    VProc *child = vprocCreate(NULL);
    assert(parent && child);

    int parent_pid = vprocPid(parent);
    int child_pid = vprocPid(child);
    vprocSetParent(child_pid, parent_pid);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap ? cap : 1);
    assert(snapshot_find_parent(snaps, count, child_pid) == parent_pid);
    free(snaps);

    /* vproc must never allow a process to parent itself (cycle). */
    vprocSetParent(child_pid, child_pid);
    cap = vprocSnapshot(NULL, 0);
    snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    count = vprocSnapshot(snaps, cap ? cap : 1);
    assert(snapshot_find_parent(snaps, count, child_pid) != child_pid);
    free(snaps);

    vprocDestroy(child);
    vprocDestroy(parent);
}

static void assert_reserved_pid_not_self_parented(void) {
    int pid = vprocReservePid();
    assert(pid > 0);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap ? cap : 1);
    assert(snapshot_find_parent(snaps, count, pid) != pid);
    free(snaps);

    vprocDiscard(pid);
}

static void assert_reparenting_uses_session_leader_sid(void) {
    int prev_shell = vprocGetShellSelfPid();
    int prev_kernel = vprocGetKernelPid();

    VProc *kernel1 = vprocCreate(NULL);
    VProc *shell1 = vprocCreate(NULL);
    VProc *kernel2 = vprocCreate(NULL);
    VProc *shell2 = vprocCreate(NULL);
    assert(kernel1 && shell1 && kernel2 && shell2);

    int k1 = vprocPid(kernel1);
    int s1 = vprocPid(shell1);
    int k2 = vprocPid(kernel2);
    int s2 = vprocPid(shell2);

    vprocSetParent(k1, 0);
    assert(vprocSetSid(k1, k1) == 0);
    vprocSetCommandLabel(k1, "kernel");
    vprocSetParent(s1, k1);
    assert(vprocSetSid(s1, k1) == 0);
    assert(vprocSetPgid(s1, s1) == 0);
    assert(vprocSetForegroundPgid(k1, s1) == 0);
    vprocSetCommandLabel(s1, "shell");

    vprocSetParent(k2, 0);
    assert(vprocSetSid(k2, k2) == 0);
    vprocSetCommandLabel(k2, "kernel");
    vprocSetParent(s2, k2);
    assert(vprocSetSid(s2, k2) == 0);
    assert(vprocSetPgid(s2, s2) == 0);
    assert(vprocSetForegroundPgid(k2, s2) == 0);
    vprocSetCommandLabel(s2, "shell");

    /* Pretend the current runtime thread belongs to session 2 so any global
     * fallback would target the wrong kernel/shell. Reparenting should still
     * prefer the exiting entry's SID (session leader). */
    vprocSetKernelPid(k2);
    vprocSetShellSelfPid(s2);

    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, s1);
    assert(vprocSetSid(cpid, k1) == 0);
    vprocSetCommandLabel(cpid, "child");

    vprocMarkExit(shell1, 0);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    int parent_after = snapshot_find_parent(snaps, count, cpid);
    free(snaps);
    assert(parent_after == k1);

    vprocMarkExit(child, 0);
    vprocDiscard(cpid);
    vprocDestroy(child);

    vprocDiscard(s1);
    vprocDestroy(shell1);
    vprocDiscard(k1);
    vprocDestroy(kernel1);
    vprocDiscard(s2);
    vprocDestroy(shell2);
    vprocDiscard(k2);
    vprocDestroy(kernel2);

    vprocSetKernelPid(prev_kernel);
    vprocSetShellSelfPid(prev_shell);
}

static bool snapshot_contains_sid(const VProcSnapshot *snaps, size_t count, int sid) {
    if (sid <= 0) return false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid > 0 && snaps[i].sid == sid) {
            return true;
        }
    }
    return false;
}

static void assert_terminate_session_discards_entries(void) {
    int prev_shell = vprocGetShellSelfPid();
    int prev_kernel = vprocGetKernelPid();

    VProc *kernel = vprocCreate(NULL);
    VProc *shell = vprocCreate(NULL);
    VProc *child = vprocCreate(NULL);
    assert(kernel && shell && child);

    int kpid = vprocPid(kernel);
    int spid = vprocPid(shell);
    int cpid = vprocPid(child);

    vprocSetParent(kpid, 0);
    assert(vprocSetSid(kpid, kpid) == 0);
    vprocSetParent(spid, kpid);
    assert(vprocSetSid(spid, kpid) == 0);
    assert(vprocSetPgid(spid, spid) == 0);
    assert(vprocSetForegroundPgid(kpid, spid) == 0);
    vprocSetParent(cpid, spid);
    assert(vprocSetSid(cpid, kpid) == 0);
    assert(vprocSetPgid(cpid, cpid) == 0);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap);
    assert(snapshot_contains_sid(snaps, count, kpid));
    free(snaps);

    vprocTerminateSession(kpid);

    cap = vprocSnapshot(NULL, 0);
    snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    count = vprocSnapshot(snaps, cap);
    assert(!snapshot_contains_sid(snaps, count, kpid));
    free(snaps);

    vprocDestroy(child);
    vprocDestroy(shell);
    vprocDestroy(kernel);
    vprocSetShellSelfPid(prev_shell);
    vprocSetKernelPid(prev_kernel);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool done;
    int got_shell;
    int got_kernel;
    bool got_vproc;
} ThreadInheritResult;

static void *inherit_thread_entry(void *arg) {
    ThreadInheritResult *res = (ThreadInheritResult *)arg;
    int shell = vprocGetShellSelfPid();
    int kernel = vprocGetKernelPid();
    bool has_vproc = (vprocCurrent() != NULL);

    pthread_mutex_lock(&res->mu);
    res->got_shell = shell;
    res->got_kernel = kernel;
    res->got_vproc = has_vproc;
    res->done = true;
    pthread_cond_signal(&res->cv);
    pthread_mutex_unlock(&res->mu);
    return NULL;
}

static void assert_pthread_inherits_session_ids(void) {
    int prev_shell = vprocGetShellSelfPid();
    int prev_kernel = vprocGetKernelPid();

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    vprocSetShellSelfPid(42420);
    vprocSetKernelPid(42421);

    ThreadInheritResult res;
    memset(&res, 0, sizeof(res));
    pthread_mutex_init(&res.mu, NULL);
    pthread_cond_init(&res.cv, NULL);

    pthread_t t;
    assert(vprocPthreadCreateShim(&t, NULL, inherit_thread_entry, &res) == 0);

    pthread_mutex_lock(&res.mu);
    while (!res.done) {
        pthread_cond_wait(&res.cv, &res.mu);
    }
    pthread_mutex_unlock(&res.mu);

    pthread_cond_destroy(&res.cv);
    pthread_mutex_destroy(&res.mu);

    assert(res.got_shell == 42420);
    assert(res.got_kernel == 42421);
    assert(res.got_vproc);

    vprocDeactivate();
    vprocDestroy(vp);

    vprocSetShellSelfPid(prev_shell);
    vprocSetKernelPid(prev_kernel);
}

int main(void) {
    /* Default truncation path for tests to keep path virtualization in /tmp. */
    setenv("PATH_TRUNCATE", "/tmp", 1);
    fprintf(stderr, "TEST pipe_round_trip\n");
    assert_pipe_round_trip();
    fprintf(stderr, "TEST pipe_cross_vproc\n");
    assert_pipe_cross_vproc();
    fprintf(stderr, "TEST socket_closed_on_destroy\n");
    assert_socket_closed_on_destroy();
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
    fprintf(stderr, "TEST dev_tty_available_in_pipeline\n");
    assert_dev_tty_available_in_pipeline();
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
    fprintf(stderr, "TEST task_slots_reused_after_reap\n");
    assert_task_slots_reused_after_reap();
    fprintf(stderr, "TEST reserve_pid_reports_capacity\n");
    assert_reserve_pid_reports_capacity();
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
    fprintf(stderr, "TEST child_inherits_sid_and_pgid\n");
    assert_child_inherits_sid_and_pgid();
    fprintf(stderr, "TEST child_inherits_signal_state\n");
    assert_child_inherits_signal_state();
    fprintf(stderr, "TEST group_exit_code_used\n");
    assert_group_exit_code_used();
    fprintf(stderr, "TEST group_stop_reaches_all_members\n");
    assert_group_stop_reaches_all_members();
    fprintf(stderr, "TEST rusage_snapshot\n");
    assert_rusage_snapshot();
    fprintf(stderr, "TEST rusage_populated_on_exit\n");
    assert_rusage_populated_on_exit();
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
    fprintf(stderr, "TEST sigchld_nocldstop\n");
    assert_sigchld_nocldstop();
    fprintf(stderr, "TEST sigchld_nocldwait_reaps\n");
    assert_sigchld_nocldwait_reaps();
    fprintf(stderr, "TEST sigsuspend_drains_pending\n");
    assert_sigsuspend_drains_pending();
    fprintf(stderr, "TEST sigprocmask_round_trip\n");
    assert_sigprocmask_round_trip();
    fprintf(stderr, "TEST sighandler_resets_with_sa_resethand\n");
    assert_sighandler_resets_with_sa_resethand();
    fprintf(stderr, "TEST sigwait_receives_pending\n");
    assert_sigwait_receives_pending();
    fprintf(stderr, "TEST sigtimedwait_timeout_and_drains\n");
    assert_sigtimedwait_timeout_and_drains();
    fprintf(stderr, "TEST signal_handler_invoked\n");
    assert_signal_handler_invoked();
    fprintf(stderr, "TEST siginfo_handler_invoked\n");
    assert_siginfo_handler_invoked();
    fprintf(stderr, "TEST kill_does_not_self_cancel\n");
    assert_kill_does_not_self_cancel();
    fprintf(stderr, "TEST sigkill_not_blockable\n");
    assert_sigkill_not_blockable();
    fprintf(stderr, "TEST sigstop_not_ignorable_or_blockable\n");
    assert_sigstop_not_ignorable_or_blockable();
    fprintf(stderr, "TEST background_tty_signals\n");
    assert_background_tty_signals();
    fprintf(stderr, "TEST getpid_fallback_and_create_inherits\n");
    assert_getpid_falls_back_to_shell_and_create_inherits_session();
    fprintf(stderr, "TEST job_id_present_in_snapshot\n");
    assert_job_id_present_in_snapshot();
    fprintf(stderr, "TEST vproc_activation_stack\n");
    assert_vproc_activation_stack_restores_previous();
    fprintf(stderr, "TEST self_parent_is_rejected\n");
    assert_self_parent_is_rejected();
    fprintf(stderr, "TEST reserved_pid_not_self_parented\n");
    assert_reserved_pid_not_self_parented();
    fprintf(stderr, "TEST reparenting_uses_sid\n");
    assert_reparenting_uses_session_leader_sid();
    fprintf(stderr, "TEST terminate_session_discards_entries\n");
    assert_terminate_session_discards_entries();
    fprintf(stderr, "TEST pthread_inherits_session_ids\n");
    assert_pthread_inherits_session_ids();
    fprintf(stderr, "TEST setpgid_zero_defaults_to_pid\n");
    assert_setpgid_zero_defaults_to_pid();
    fprintf(stderr, "TEST path_truncate_maps_to_sandbox\n");
    assert_path_truncate_maps_to_sandbox();
    fprintf(stderr, "TEST write_reads_back\n");
    assert_write_reads_back();
    fprintf(stderr, "TEST passthrough_when_inactive\n");
    assert_passthrough_when_inactive();
    fprintf(stderr, "TEST gps_alias_reads_location_payload\n");
    assert_gps_alias_reads_location_payload();
    fprintf(stderr, "TEST location_read_returns_full_line_and_eof\n");
    assert_location_read_returns_full_line_and_eof();
    fprintf(stderr, "TEST location_poll_wakes_on_payload\n");
    assert_location_poll_wakes_on_payload();
    fprintf(stderr, "TEST select_sparse_fdset_works\n");
    assert_select_sparse_fdset_works();
    fprintf(stderr, "TEST select_empty_set_honors_timeout\n");
    assert_select_empty_set_honors_timeout();
    fprintf(stderr, "TEST location_disable_unblocks_and_errors\n");
    assert_location_disable_unblocks_and_errors();
    fprintf(stderr, "TEST location_reader_observer_fires\n");
    assert_location_reader_observer_fires();
    fprintf(stderr, "TEST device_stat_bypasses_truncation\n");
    assert_device_stat_bypasses_truncation();
    fprintf(stderr, "TEST ptmx_open_registers_session\n");
    assert_ptmx_open_registers_session();
#if defined(PSCAL_TARGET_IOS)
    /* Ensure path virtualization macros remain visible even when vproc shim is included. */
    int (*fn)(const char *) = chdir;
    (void)fn;
#endif
    return 0;
}
