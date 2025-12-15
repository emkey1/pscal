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
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    assert(sigaction(SIGUSR1, &sa, NULL) == 0);

    g_signal_seen = 0;
    VProcSignalArg arg;
    arg.pid_hint = vprocReservePid();
    arg.ready = 0;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, signal_helper_thread, &arg) == 0);

    while (!arg.ready) {
        sched_yield();
    }
    assert(vprocKillShim(-arg.pid_hint, SIGUSR1) == 0);

    int attempts = 0;
    while (!g_signal_seen && attempts < 1000) {
        struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
        nanosleep(&ts, NULL);
        attempts++;
    }
    assert(g_signal_seen);
    pthread_join(tid, NULL);
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

    unsetenv("PATH_TRUNCATE");
    unlink(host_path);
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
    fprintf(stderr, "TEST wait_nohang_transitions\n");
    assert_wait_nohang_transitions();
    fprintf(stderr, "TEST snapshot_lists_active_tasks\n");
    assert_snapshot_lists_active_tasks();
    fprintf(stderr, "TEST stop_and_continue_round_trip\n");
    assert_stop_and_continue_round_trip();
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
