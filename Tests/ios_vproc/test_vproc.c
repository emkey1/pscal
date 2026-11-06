#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <glob.h>
#include <unistd.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <locale.h>
#include <xlocale.h>
#endif
#include <sched.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#include <malloc/malloc.h>
#endif
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_truncate.h"
#include "common/path_virtualization.h"
#include "ios/tty/pscal_pty.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

#include "ios/vproc.h"

static volatile int gRuntimeSigintReenterEnabled = 0;
static volatile int gRuntimeSigintCallbackCount = 0;
static volatile int gRuntimeSigintShellPid = -1;
static volatile int gRuntimeSigtstpReenterEnabled = 0;
static volatile int gRuntimeSigtstpCallbackCount = 0;
static volatile int gRuntimeSigtstpShellPid = -1;
static volatile int gRuntimeSigtstpTargetPgid = -1;
static volatile sig_atomic_t gHostSigintTrapCount = 0;
static volatile sig_atomic_t gHostSigtstpTrapCount = 0;

static void hostSigintTrapHandler(int signo) {
    (void)signo;
    gHostSigintTrapCount++;
}

static void hostSigtstpTrapHandler(int signo) {
    (void)signo;
    gHostSigtstpTrapCount++;
}

void pscalRuntimeRequestSigint(void) {
    gRuntimeSigintCallbackCount++;
    if (!gRuntimeSigintReenterEnabled) {
        return;
    }
    int pid = (int)gRuntimeSigintShellPid;
    if (pid > 0) {
        (void)vprocGetSid(pid);
    }
}

void pscalRuntimeRequestSigtstp(void) {
    gRuntimeSigtstpCallbackCount++;
    if (!gRuntimeSigtstpReenterEnabled) {
        return;
    }
    if (gRuntimeSigtstpCallbackCount > 8) {
        gRuntimeSigtstpReenterEnabled = 0;
        return;
    }
    int target_pgid = (int)gRuntimeSigtstpTargetPgid;
    if (target_pgid > 0) {
        (void)vprocKillShim(-target_pgid, SIGTSTP);
        return;
    }
    int pid = (int)gRuntimeSigtstpShellPid;
    if (pid > 0) {
        (void)vprocGetSid(pid);
    }
}

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

/* A closed stdout or stderr is closed: no write, no stat, and nothing stands in for it. A slot
 * that gets a file again works again. */
static void assert_closed_output_slot_stays_closed(void) {
    int p[2];
    assert(vprocHostPipe(p) == 0);
    int slots[] = {STDOUT_FILENO, STDERR_FILENO};
    for (size_t i = 0; i < sizeof(slots) / sizeof(slots[0]); ++i) {
        int slot = slots[i];
        VProc *vp = vprocCreate(NULL);
        assert(vp);
        vprocActivate(vp);
        assert(vprocClose(vp, slot) == 0);

        errno = 0;
        assert(vprocWriteShim(slot, "x", 1) == -1 && errno == EBADF);
        struct stat st;
        errno = 0;
        assert(vprocFstatShim(slot, &st) == -1 && errno == EBADF);
        errno = 0;
        assert(vprocDupShim(slot) == -1 && errno == EBADF);

        assert(vprocRestoreHostFd(vp, slot, p[1]) == slot);
        assert(vprocWriteShim(slot, "y", 1) == 1);
        char c = 0;
        assert(vprocHostRead(p[0], &c, 1) == 1 && c == 'y');
        vprocDeactivate();
        vprocDestroy(vp);
    }
    vprocHostClose(p[0]);
    vprocHostClose(p[1]);
}

static int gForkExecInheritedWriteFd = -1;
static int gForkExecSocketpairChildPeerFd = -1;
static int gForkExecSocketpairChildCloseFd = -1;

static int vprocForkExecWriteEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (gForkExecInheritedWriteFd < 0) {
        return 111;
    }
    const char marker = 'K';
    ssize_t wrote = vprocWriteShim(gForkExecInheritedWriteFd, &marker, 1);
    return (wrote == 1) ? 0 : 110;
}

static int vprocForkExecSocketpairEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    if (gForkExecSocketpairChildPeerFd < 0 || gForkExecSocketpairChildCloseFd < 0) {
        return 221;
    }
    if (vprocDup2Shim(gForkExecSocketpairChildPeerFd, STDIN_FILENO) != STDIN_FILENO) {
        return 222;
    }
    if (vprocDup2Shim(gForkExecSocketpairChildPeerFd, STDOUT_FILENO) != STDOUT_FILENO) {
        return 223;
    }
    if (vprocCloseShim(gForkExecSocketpairChildCloseFd) != 0) {
        return 224;
    }
    if (vprocCloseShim(gForkExecSocketpairChildPeerFd) != 0) {
        return 225;
    }

    unsigned char ch = 0;
    if (vprocReadShim(STDIN_FILENO, &ch, 1) != 1) {
        return 226;
    }
    if (vprocWriteShim(STDOUT_FILENO, &ch, 1) != 1) {
        return 227;
    }
    return 0;
}

static void assert_simulated_fork_exec_inherits_nonstdio_fds(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    int p[2];
    assert(vprocPipeShim(p) == 0);
    gForkExecInheritedWriteFd = p[1];

    sigjmp_buf env;
    pid_t child_pid = -1;
    int jump_rc = sigsetjmp(env, 0);
    if (jump_rc == 0) {
        child_pid = vprocSimulatedForkWithEnv("fork-fd-inherit", true, &env);
        assert(child_pid >= 0);
        if (child_pid == 0) {
            char *argv[] = {"vproc-fork-fd-inherit", NULL};
            int rc = vprocSimulatedExec(vprocForkExecWriteEntry, argv);
            (void)rc;
            assert(!"vprocSimulatedExec should not return in child");
        }
        assert(!"simulated fork child path should longjmp to parent");
    }

    child_pid = vprocSimulatedForkParentResume();
    assert(child_pid > 0);

    assert(vprocCloseShim(p[1]) == 0);
    char marker = '\0';
    assert(vprocReadShim(p[0], &marker, 1) == 1);
    assert(marker == 'K');

    int status = 0;
    assert(vprocWaitPidShim(child_pid, &status, 0) == child_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(vprocCloseShim(p[0]) == 0);

    gForkExecInheritedWriteFd = -1;
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_simulated_fork_exec_socketpair_parent_fd_survives(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);

    int sv[2] = {-1, -1};
    assert(vprocSocketpairShim(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    gForkExecSocketpairChildPeerFd = sv[0];
    gForkExecSocketpairChildCloseFd = sv[1];

    sigjmp_buf env;
    pid_t child_pid = -1;
    int jump_rc = sigsetjmp(env, 0);
    if (jump_rc == 0) {
        child_pid = vprocSimulatedForkWithEnv("fork-sockpair", true, &env);
        assert(child_pid >= 0);
        if (child_pid == 0) {
            char *argv[] = {"vproc-fork-sockpair", NULL};
            int rc = vprocSimulatedExec(vprocForkExecSocketpairEntry, argv);
            (void)rc;
            assert(!"vprocSimulatedExec should not return in child");
        }
        assert(!"simulated fork child path should longjmp to parent");
    }

    child_pid = vprocSimulatedForkParentResume();
    assert(child_pid > 0);
    assert(vprocCloseShim(sv[0]) == 0);

    unsigned char tx = 'Q';
    assert(vprocWriteShim(sv[1], &tx, 1) == 1);
    unsigned char rx = 0;
    assert(vprocReadShim(sv[1], &rx, 1) == 1);
    assert(rx == tx);

    int status = 0;
    assert(vprocWaitPidShim(child_pid, &status, 0) == child_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    assert(vprocCloseShim(sv[1]) == 0);

    gForkExecSocketpairChildPeerFd = -1;
    gForkExecSocketpairChildCloseFd = -1;
    vprocDeactivate();
    vprocDestroy(vp);
}

/* Puts a live descriptor on the number the entry just gave up, as another command's
 * open would, and checks that ending the vproc leaves it alone. */
static void assert_closed_stdio_number_survives_destroy(VProc *vp, int old_host_fd, int any_fd) {
    int other = fcntl(any_fd, F_DUPFD, old_host_fd);
    assert(other == old_host_fd);
    vprocDestroy(vp);
    assert(fcntl(other, F_GETFD) != -1);
    vprocHostClose(other);
}

static void assert_redirected_stdio_is_closed_once(void) {
    int p[2];
    assert(vprocHostPipe(p) == 0);
    int stdio_fds[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    for (size_t i = 0; i < sizeof(stdio_fds) / sizeof(stdio_fds[0]); ++i) {
        int target = stdio_fds[i];
        int source = target == STDERR_FILENO ? STDOUT_FILENO : STDERR_FILENO;

        VProc *closed = vprocCreate(NULL);
        assert(closed);
        int old_host_fd = vprocTranslateFd(closed, target);
        assert(old_host_fd >= 0);
        assert(vprocClose(closed, target) == 0);
        assert_closed_stdio_number_survives_destroy(closed, old_host_fd, p[0]);

        /* The new entry takes the lowest free number. Leave one below the old
         * entry's, or it takes the old number and nothing is left to close twice. */
        int lower = vprocHostDup(p[0]);
        VProc *duped = vprocCreate(NULL);
        assert(duped);
        old_host_fd = vprocTranslateFd(duped, target);
        assert(old_host_fd > lower);
        vprocHostClose(lower);
        assert(vprocDup2(duped, source, target) == target);
        assert(vprocTranslateFd(duped, target) != old_host_fd);
        assert_closed_stdio_number_survives_destroy(duped, old_host_fd, p[0]);

        lower = vprocHostDup(p[0]);
        VProc *restored = vprocCreate(NULL);
        assert(restored);
        old_host_fd = vprocTranslateFd(restored, target);
        assert(old_host_fd > lower);
        vprocHostClose(lower);
        assert(vprocRestoreHostFd(restored, target, vprocTranslateFd(restored, source)) == target);
        assert(vprocTranslateFd(restored, target) != old_host_fd);
        assert_closed_stdio_number_survives_destroy(restored, old_host_fd, p[0]);
    }
    vprocHostClose(p[0]);
    vprocHostClose(p[1]);
}

/* vprocDup2's other branch, with an in-process pipe as the source. In a pipeline that is `2>&1`:
 * `ls x 2>&1 | wc -l | cat` lost cat's output, its stdout having taken the number by then. */
static void assert_stdio_replaced_by_inproc_pipe_is_closed_once(void) {
    int p[2];
    assert(vprocHostPipe(p) == 0);
    for (int target = STDIN_FILENO; target <= STDERR_FILENO; ++target) {
        int source = target == STDERR_FILENO ? STDOUT_FILENO : STDERR_FILENO;
        struct pscal_fd *read_end = NULL;
        struct pscal_fd *write_end = NULL;
        assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
        VProc *vp = vprocCreate(NULL);
        assert(vp);
        assert(vprocAdoptPscalFd(vp, source, write_end) == 0);
        pscal_fd_close(write_end);
        int old_host_fd = vprocTranslateFd(vp, target);
        assert(old_host_fd >= 0);
        assert(vprocDup2(vp, source, target) == target);
        assert_closed_stdio_number_survives_destroy(vp, old_host_fd, p[0]);
        pscal_fd_close(read_end);
    }
    vprocHostClose(p[0]);
    vprocHostClose(p[1]);
}

static int count_open_host_fds(void) {
    int count = 0;
    for (int fd = 0; fd < 1024; ++fd) {
        if (fcntl(fd, F_GETFD) != -1) {
            count++;
        }
    }
    return count;
}

/* `cat < f`, as the shell does it: stdin replaced, then put back through both restores. Each round
 * used to leave 2 or 3 descriptors, and a loop of them ran out after about 116 rounds. */
static void assert_replacing_stdin_leaves_no_descriptor(void) {
    int at_start = count_open_host_fds();
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int saved = vprocDup(vp, STDIN_FILENO);
    assert(saved >= 0);
    int before = count_open_host_fds();
    for (int round = 0; round < 16; ++round) {
        assert(vprocDup2(vp, STDERR_FILENO, STDIN_FILENO) == STDIN_FILENO);
        assert(vprocRestoreHostFd(vp, STDIN_FILENO, vprocTranslateFd(vp, saved)) == STDIN_FILENO);
        assert(vprocDup2(vp, saved, STDIN_FILENO) == STDIN_FILENO);
    }
    assert(count_open_host_fds() == before);

    /* The source can be the descriptor being replaced. */
    int host = vprocTranslateFd(vp, STDIN_FILENO);
    assert(vprocRestoreHostFd(vp, STDIN_FILENO, host) == STDIN_FILENO);
    assert(fcntl(vprocTranslateFd(vp, STDIN_FILENO), F_GETFD) != -1);
    assert(count_open_host_fds() == before);

    assert(vprocClose(vp, saved) == 0);
    vprocDestroy(vp);
    assert(count_open_host_fds() == at_start);
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

    int s = vprocSocketShim(AF_INET, SOCK_STREAM, 0);
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
    // Nor may a write to the closed slot find it.
    errno = 0;
    assert(vprocWriteShim(1, "x", 1) == -1);
    assert(errno == EBADF);
    assert(vprocCloseShim(0) == 0);
    char ch = 0;
    errno = 0;
    assert(vprocReadShim(0, &ch, 1) == -1);
    assert(errno == EBADF);
    vprocDeactivate();
    vprocDestroy(vp);
    int after = fcntl(STDOUT_FILENO, F_GETFD);
    assert(after >= 0);
    // Zero-length write should still succeed on host stdout.
    assert(write(STDOUT_FILENO, "", 0) == 0);
}

/* A slot number no host descriptor uses until a test puts an unrelated file there.
 * That is how the raw calls went wrong: they reached whichever host descriptor had
 * the slot's number, or failed EBADF when none did. */
/* A socket's number or a pipe's is a host descriptor, and a slot can have the same number. */
static void assert_tagged_host_fd_skips_the_slot(void) {
    char path[] = "/tmp/vproc_tag_XXXXXX";
    int file_host = mkstemp(path);
    assert(file_host >= 0);
    close(file_host);

    assert(vprocTagHostFd(7) == 7);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    assert(vprocTagHostFd(-1) == -1);

    int host_pipe[2];
    assert(vprocHostPipeUnowned(host_pipe) == 0);
    int opened = vprocOpenShim(path, O_RDWR);
    assert(opened >= 0);
    if (opened != host_pipe[0]) {
        assert(vprocDup2Shim(opened, host_pipe[0]) == host_pipe[0]);
        assert(vprocCloseShim(opened) == 0);
    }

    /* The plain number is the slot's regular file, always ready. Tagged, it is the empty pipe. */
    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = host_pipe[0];
    pfd.events = POLLIN;
    assert(vprocPollShim(&pfd, 1, 0) == 1);
    pfd.fd = vprocTagHostFd(host_pipe[0]);
    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, 0) == 0);
    assert(vprocHostWrite(host_pipe[1], "x", 1) == 1);
    pfd.revents = 0;
    assert(vprocPollShim(&pfd, 1, 1000) == 1 && (pfd.revents & POLLIN));

    struct stat st;
    assert(vprocFstatShim(host_pipe[0], &st) == 0 && S_ISREG(st.st_mode));
    assert(vprocFstatShim(vprocTagHostFd(host_pipe[0]), &st) == 0 && S_ISFIFO(st.st_mode));
    int pending = -1;
    assert(vprocIoctlShim(vprocTagHostFd(host_pipe[0]), FIONREAD, &pending) == 0 && pending == 1);

    /* close takes the socket and leaves the slot, and only once. */
    int sock = vprocSocketShim(AF_INET, SOCK_DGRAM, 0);
    assert(sock >= 0);
    opened = vprocOpenShim(path, O_RDWR);
    assert(opened >= 0);
    if (opened != sock) {
        assert(vprocDup2Shim(opened, sock) == sock);
        assert(vprocCloseShim(opened) == 0);
    }
    assert(vprocCloseShim(vprocTagHostFd(sock)) == 0);
    assert(vprocFstatShim(sock, &st) == 0 && S_ISREG(st.st_mode));
    errno = 0;
    assert(fcntl(sock, F_GETFD) == -1 && errno == EBADF);
    errno = 0;
    assert(vprocCloseShim(vprocTagHostFd(sock)) == -1 && errno == EBADF);
    assert(vprocFstatShim(sock, &st) == 0 && S_ISREG(st.st_mode));
    assert(vprocCloseShim(sock) == 0);

    /* A number this vproc does not hold stays open: a program must not close the app's. */
    errno = 0;
    assert(vprocCloseShim(vprocTagHostFd(host_pipe[1])) == -1 && errno == EBADF);
    assert(fcntl(host_pipe[1], F_GETFD) >= 0);

    assert(vprocCloseShim(host_pipe[0]) == 0);
    assert(vprocHostClose(host_pipe[0]) == 0);
    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
    unlink(path);
}

enum { SHIM_TEST_SLOT = 40 };

static void assert_fcntl_pread_pwrite_take_slots(void) {
    char mine[] = "/tmp/vproc_shim_mine_XXXXXX";
    char other[] = "/tmp/vproc_shim_other_XXXXXX";
    int mine_host = mkstemp(mine);
    assert(mine_host >= 0);
    close(mine_host);
    int other_host = mkstemp(other);
    assert(other_host >= 0);
    assert(write(other_host, "unrelated", 9) == 9);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    int opened = vprocOpenShim(mine, O_RDWR);
    assert(opened >= 0);
    assert(vprocDup2Shim(opened, SHIM_TEST_SLOT) == SHIM_TEST_SLOT);
    assert(vprocCloseShim(opened) == 0);

    /* No host descriptor has the number: what utmp_rec.c's lock and writes saw. */
    errno = 0;
    assert(fcntl(SHIM_TEST_SLOT, F_GETFD) == -1 && errno == EBADF);
    assert(vprocPwriteShim(SHIM_TEST_SLOT, "abcdef", 6, 0) == 6);
    char buf[16] = {0};
    assert(vprocPreadShim(SHIM_TEST_SLOT, buf, 3, 2) == 3);
    assert(memcmp(buf, "cde", 3) == 0);
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_GETFD) >= 0);
    assert((vprocFcntlShim(SHIM_TEST_SLOT, F_GETFL) & O_ACCMODE) == O_RDWR);
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_WRLCK;
    fl.l_whence = SEEK_SET;
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_SETLKW, &fl) == 0);
    fl.l_type = F_UNLCK;
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_SETLK, &fl) == 0);

    /* An unrelated file has the number: what could take a utmp record. */
    assert(dup2(other_host, SHIM_TEST_SLOT) == SHIM_TEST_SLOT);
    int other_flags = fcntl(SHIM_TEST_SLOT, F_GETFL);
    assert(vprocPwriteShim(SHIM_TEST_SLOT, "XY", 2, 0) == 2);
    assert(vprocPreadShim(SHIM_TEST_SLOT, buf, 6, 0) == 6);
    assert(memcmp(buf, "XYcdef", 6) == 0);
    int flags = vprocFcntlShim(SHIM_TEST_SLOT, F_GETFL);
    assert(flags >= 0 && !(flags & O_NONBLOCK));
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_GETFL) & O_NONBLOCK);
    assert(fcntl(SHIM_TEST_SLOT, F_GETFL) == other_flags);
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_SETFL, flags) == 0);
    assert(!(vprocFcntlShim(SHIM_TEST_SLOT, F_GETFL) & O_NONBLOCK));
    assert(pread(other_host, buf, sizeof(buf), 0) == 9);
    assert(memcmp(buf, "unrelated", 9) == 0);

    /* F_SETFD, as the shell sets close-on-exec on each dup of its stdio. */
    int other_fd_flags = fcntl(SHIM_TEST_SLOT, F_GETFD);
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_SETFD, 0) == 0);
    assert(!(vprocFcntlShim(SHIM_TEST_SLOT, F_GETFD) & FD_CLOEXEC));
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_SETFD, FD_CLOEXEC) == 0);
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_GETFD) & FD_CLOEXEC);
    assert(fcntl(SHIM_TEST_SLOT, F_GETFD) == other_fd_flags);

    /* F_DUPFD makes slots, from the lowest free one at or above its argument. */
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_DUPFD, 50) == 50);
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_DUPFD_CLOEXEC, 50) == 51);
    assert(vprocPreadShim(51, buf, 2, 0) == 2);
    assert(memcmp(buf, "XY", 2) == 0);
    assert(vprocCloseShim(50) == 0);
    assert(vprocCloseShim(51) == 0);
    errno = 0;
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_DUPFD, -1) == -1 && errno == EINVAL);

    /* A closed slot is closed, whatever the host has at that number: the pager
     * relies on this to tell a dead descriptor from one poll cannot wait on. */
    assert(vprocCloseShim(SHIM_TEST_SLOT) == 0);
    errno = 0;
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_GETFD) == -1 && errno == EBADF);
    errno = 0;
    assert(vprocFcntlShim(SHIM_TEST_SLOT, F_DUPFD, 50) == -1 && errno == EBADF);
    errno = 0;
    assert(vprocPreadShim(SHIM_TEST_SLOT, buf, 1, 0) == -1 && errno == EBADF);
    errno = 0;
    assert(vprocPwriteShim(SHIM_TEST_SLOT, "z", 1, 0) == -1 && errno == EBADF);
    assert(pread(other_host, buf, sizeof(buf), 0) == 9);
    assert(memcmp(buf, "unrelated", 9) == 0);
    close(SHIM_TEST_SLOT);

    /* What fileno gives an applet: a host descriptor, tagged. */
    FILE *stream = fopen(mine, "r");
    assert(stream);
    int tagged = vprocFilenoShim(stream);
    assert(tagged != fileno(stream));
    flags = vprocFcntlShim(tagged, F_GETFL);
    assert(flags >= 0);
    assert(vprocFcntlShim(tagged, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(fcntl(fileno(stream), F_GETFL) & O_NONBLOCK);
    assert(vprocPreadShim(tagged, buf, 2, 0) == 2);
    assert(memcmp(buf, "XY", 2) == 0);
    fclose(stream);

    vprocDeactivate();
    vprocDestroy(vp);

    /* With no vproc, numbers are the host's. */
    assert(vprocFcntlShim(other_host, F_GETFL) == fcntl(other_host, F_GETFL));
    assert(vprocPreadShim(other_host, buf, 2, 0) == 2);
    assert(memcmp(buf, "un", 2) == 0);
    close(other_host);
    unlink(mine);
    unlink(other);
}

static void assert_fcntl_on_inproc_pipe(void) {
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    int r = 10;
    int w = 11;
    assert(vprocAdoptPscalFd(vp, r, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, w, write_end) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);

    assert(vprocFcntlShim(r, F_GETFL) == O_RDONLY);
    assert(vprocFcntlShim(w, F_GETFL) == O_WRONLY);

    /* O_NONBLOCK is real: a read of nothing and a write to a full pipe don't wait. */
    char buf[9000];
    memset(buf, 'x', sizeof(buf));
    assert(vprocFcntlShim(r, F_SETFL, O_NONBLOCK) == 0);
    assert(vprocFcntlShim(r, F_GETFL) == (O_RDONLY | O_NONBLOCK));
    errno = 0;
    assert(vprocReadShim(r, buf, 1) == -1 && errno == EAGAIN);
    assert(vprocFcntlShim(w, F_SETFL, O_NONBLOCK) == 0);
    ssize_t wrote = vprocWriteShim(w, buf, sizeof(buf));
    assert(wrote > 0 && wrote < (ssize_t)sizeof(buf));
    errno = 0;
    assert(vprocWriteShim(w, buf, 1) == -1 && errno == EAGAIN);
    assert(vprocReadShim(r, buf, sizeof(buf)) == wrote);
    errno = 0;
    assert(vprocReadShim(r, buf, 1) == -1 && errno == EAGAIN);

    /* A dup is the same open file, so it shares the flag. */
    assert(vprocFcntlShim(r, F_DUPFD, 50) == 50);
    assert(vprocFcntlShim(50, F_GETFL) == (O_RDONLY | O_NONBLOCK));
    assert(vprocFcntlShim(50, F_SETFL, 0) == 0);
    assert(vprocFcntlShim(r, F_GETFL) == O_RDONLY);
    assert(vprocCloseShim(50) == 0);

    assert(vprocFcntlShim(r, F_GETFD) == 0);
    assert(vprocFcntlShim(r, F_SETFD, FD_CLOEXEC) == 0);
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_RDLCK;
    fl.l_whence = SEEK_SET;
    errno = 0;
    assert(vprocFcntlShim(r, F_SETLK, &fl) == -1 && errno == EINVAL);
    errno = 0;
    assert(vprocPreadShim(r, buf, 1, 0) == -1 && errno == ESPIPE);
    errno = 0;
    assert(vprocPwriteShim(w, "z", 1, 0) == -1 && errno == ESPIPE);

    assert(vprocCloseShim(w) == 0);
    assert(vprocCloseShim(r) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

/* A shell tab's terminal can name another tab's session, so the session's shell says whose tasks a
 * close ends. Going by the terminal, closing a second tab whose shell would not end took the first
 * tab's shell and left the closed tab running. */
static void assert_terminate_session_by_id_goes_by_its_shell(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, 8765, 0) == 0);
    vprocSessionStdioActivate(session);

    VProc *other = vprocCreate(NULL);
    VProc *own = vprocCreate(NULL);
    assert(other && own);
    int other_pid = vprocPid(other);
    int own_pid = vprocPid(own);
    assert(vprocSetSid(other_pid, other_pid) == 0);
    assert(vprocSetSid(own_pid, own_pid) == 0);
    int prev_shell = vprocGetShellSelfPid();
    vprocSetShellSelfPid(own_pid);
    slave->tty->session = (pid_t_)other_pid;

    assert(vprocTerminateSessionById(8765));
    assert(vprocGetSid(other_pid) == other_pid);
    assert(vprocGetSid(own_pid) == -1);
    /* Its shell is gone now, and that is no reason to believe the terminal. */
    assert(!vprocTerminateSessionById(8765));
    assert(vprocGetSid(other_pid) == other_pid);

    vprocSetShellSelfPid(prev_shell);
    vprocDestroy(own);
    vprocDestroy(other);
    vprocSessionStdioDestroy(session);
}

static void assert_fcntl_on_pty(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, 4321, 0) == 0);
    vprocSessionStdioActivate(session);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    /* The session's own terminal: tail -f on it asks for O_NONBLOCK, and its
     * reader and every later command would be left with it. */
    int slot = 10;
    assert(vprocAdoptPscalFd(vp, slot, slave) == 0);
    assert(vprocFcntlShim(slot, F_GETFL) == O_RDWR);
    assert(vprocFcntlShim(slot, F_SETFL, O_RDWR | O_NONBLOCK) == 0);
    assert(vprocFcntlShim(slot, F_GETFL) == O_RDWR);
    assert(!(slave->flags & O_NONBLOCK));
    assert(vprocCloseShim(slot) == 0);

    /* The same terminal opened by name is another open file, as stty -F does:
     * opened O_NONBLOCK so that it cannot wait for carrier, then made blocking. */
    struct pscal_fd *named = NULL;
    assert(pscalPtyOpenSlave(pty_num, O_RDONLY | O_NONBLOCK, &named) == 0);
    assert(vprocAdoptPscalFd(vp, slot, named) == 0);
    pscal_fd_close(named);
    int flags = vprocFcntlShim(slot, F_GETFL);
    assert(flags == (O_RDONLY | O_NONBLOCK));
    assert(vprocFcntlShim(slot, F_SETFL, flags & ~O_NONBLOCK) == 0);
    assert(vprocFcntlShim(slot, F_GETFL) == O_RDONLY);
    char buf[1];
    errno = 0;
    assert(vprocPreadShim(slot, buf, 1, 0) == -1 && errno == ESPIPE);
    assert(vprocCloseShim(slot) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
    vprocSessionStdioDestroy(session);
}

/* What a pipeline's pipe and a terminal look like: tail asks, to know what to follow. */
static void assert_fstat_and_ttyname_on_slots(void) {
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    struct pscal_fd *other_read = NULL;
    struct pscal_fd *other_write = NULL;
    assert(vprocCreateInprocPipe(&other_read, &other_write) == 0);
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    assert(vprocAdoptPscalFd(vp, 10, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, 11, write_end) == 0);
    assert(vprocAdoptPscalFd(vp, 12, other_read) == 0);
    assert(vprocAdoptPscalFd(vp, 13, other_write) == 0);
    assert(vprocAdoptPscalFd(vp, 14, slave) == 0);
    assert(vprocAdoptPscalFd(vp, 15, master) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
    pscal_fd_close(other_read);
    pscal_fd_close(other_write);
    pscal_fd_close(slave);
    pscal_fd_close(master);

    struct stat a;
    struct stat b;
    assert(vprocFstatShim(10, &a) == 0 && S_ISFIFO(a.st_mode));
    assert(vprocFstatShim(11, &b) == 0 && S_ISFIFO(b.st_mode));
    assert(a.st_ino == b.st_ino);
    assert(vprocFstatShim(12, &b) == 0 && S_ISFIFO(b.st_mode));
    assert(a.st_ino != b.st_ino);
    assert(vprocFstatShim(14, &a) == 0 && S_ISCHR(a.st_mode));
    assert(vprocFstatShim(15, &a) == 0 && S_ISCHR(a.st_mode));

    errno = 0;
    assert(vprocTtynameShim(10) == NULL && errno == ENOTTY);
    char expected[32];
    snprintf(expected, sizeof(expected), "/dev/pts/%d", pty_num);
    const char *name = vprocTtynameShim(14);
    assert(name && strcmp(name, expected) == 0);
    name = vprocTtynameShim(15);
    assert(name && strcmp(name, "/dev/ptmx") == 0);

    /* A host file in a slot is no terminal, whatever host descriptor has the slot's number. */
    int file = vprocOpenShim("/dev/null", O_RDONLY);
    assert(file >= 0);
    errno = 0;
    assert(vprocTtynameShim(file) == NULL && errno != 0);
    assert(vprocCloseShim(file) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
}

static int gSpawnSlot = -1;
static int gSpawnOtherSlot = -1;

static int vprocSpawnWriteEntry(int argc, char **argv) {
    (void)argc;
    const char *value = vprocGetenvShim("SPAWN_TEST_VALUE");
    if (!value || strcmp(value, "parent") != 0) {
        return 90;
    }
    vprocSetenvShim("SPAWN_TEST_VALUE", "child", 1);
    if (vprocWriteShim(gSpawnSlot, argv[1], 1) != 1) {
        return 91;
    }
    return 7;
}

static int vprocSpawnBlockedReadEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char c;
    (void)vprocCloseShim(gSpawnOtherSlot);
    return vprocReadShim(gSpawnSlot, &c, 1) == 1 ? 80 : 81;
}

static int vprocSpawnBlockedWriteEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char buf[4096];
    memset(buf, 'w', sizeof(buf));
    (void)vprocCloseShim(gSpawnOtherSlot);
    for (;;) {
        if (vprocWriteShim(gSpawnSlot, buf, sizeof(buf)) < 0) {
            return 82;
        }
    }
}

static int vprocSpawnWaitEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char *child_argv[] = {"spawn-grandchild", NULL};
    pid_t child = vprocSpawnEntry("spawn-grandchild", true, vprocSpawnBlockedReadEntry, child_argv);
    if (child < 0) {
        return 83;
    }
    int status = 0;
    (void)vprocWaitPidShim(child, &status, 0);
    return 84;
}

/* What a nested shell's command is: vprocs of its own on the same thread, a command scope
 * and a stage run in line. */
static struct pscal_fd *gSpawnScopeHeld = NULL;
static atomic_int gSpawnScopePid;

static int vprocSpawnScopeEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    VProcCommandScope scope;
    if (!vprocCommandScopeBegin(&scope, "spawn-scope", true, true)) {
        return 85;
    }
    VProc *stage = vprocCreate(NULL);
    if (!stage) {
        return 89;
    }
    vprocActivate(stage);
    if (vprocAdoptPscalFd(stage, 10, gSpawnScopeHeld) != 0) {
        return 86;
    }
    atomic_store(&gSpawnScopePid, scope.pid);
    for (;;) {
        usleep(10000);
    }
}

/* Ends only by a kill, and holds the read end of gSpawnSlot until then. */
static int vprocSpawnHoldEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    (void)vprocCloseShim(gSpawnOtherSlot);
    for (;;) {
        usleep(10000);
    }
}

static int vprocSpawnHolderParentEntry(int argc, char **argv) {
    (void)argc;
    (void)argv;
    char *child_argv[] = {"spawn-holder", NULL};
    pid_t child = vprocSpawnEntry("spawn-holder", true, vprocSpawnHoldEntry, child_argv);
    if (child < 0) {
        return 87;
    }
    int status = 0;
    (void)vprocWaitPidShim(child, &status, 0);
    return 88;
}

/* A kill unwinds the thread past the scope's end, and the scope has to end anyway. */
static void assert_kill_ends_scopes_and_descendants(void) {
    alarm(60);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    assert(vprocAdoptPscalFd(vp, 10, read_end) == 0);
    pscal_fd_close(read_end);
    gSpawnScopeHeld = write_end;
    atomic_store(&gSpawnScopePid, 0);
    char *scope_argv[] = {"spawn-scope", NULL};
    pid_t child = vprocSpawnEntry("spawn-scope", true, vprocSpawnScopeEntry, scope_argv);
    assert(child > 0);
    while (atomic_load(&gSpawnScopePid) == 0) {
        usleep(1000);
    }
    pscal_fd_close(write_end);
    assert(vprocKillShim(atomic_load(&gSpawnScopePid), 0) == 0);
    assert(vprocKillShim(child, SIGTERM) == 0);
    int status = 0;
    assert(vprocWaitPidShim(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    char c;
    assert(vprocReadShim(10, &c, 1) == 0);
    /* The stage ends first and its descriptors go with it, so the scope is a moment behind. */
    while (vprocKillShim(atomic_load(&gSpawnScopePid), 0) == 0) {
        usleep(1000);
    }
    assert(errno == ESRCH);
    assert(vprocCloseShim(10) == 0);

    /* Down the tree: the grandchild's read end is the last one, so the writer sees it go. */
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    assert(vprocAdoptPscalFd(vp, 10, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, 11, write_end) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
    gSpawnSlot = 10;
    gSpawnOtherSlot = 11;
    char *holder_argv[] = {"spawn-holder-parent", NULL};
    child = vprocSpawnEntry("spawn-holder-parent", true, vprocSpawnHolderParentEntry, holder_argv);
    assert(child > 0);
    usleep(200000);
    assert(vprocCloseShim(10) == 0);
    assert(vprocKillTree(child, SIGKILL) == 0);
    assert(vprocWaitPidShim(child, &status, 0) == child);
    ssize_t wrote;
    do {
        usleep(1000);
        wrote = vprocWriteShim(11, "x", 1);
    } while (wrote == 1);
    assert(wrote == -1 && errno == EPIPE);
    assert(vprocCloseShim(11) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
    alarm(0);
}

/* A kill cancels the child's thread: nothing it was waiting in may stay locked, and what it
 * held has to close. A hang here is the failure, so alarm() turns it into one. */
static void assert_spawn_entry_runs_as_a_child(void) {
    alarm(120);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    vprocSetenvShim("SPAWN_TEST_VALUE", "parent", 1);

    /* Runs, on the caller's descriptors and environment, and gets waited for. */
    int p[2];
    assert(vprocPipeShim(p) == 0);
    gSpawnSlot = p[1];
    gSpawnOtherSlot = p[0];
    char *argv[] = {"spawn-child", "K", NULL};
    pid_t child = vprocSpawnEntry("spawn-child", true, vprocSpawnWriteEntry, argv);
    assert(child > 0);
    assert(child != vprocPid(vp));
    assert(vprocGetPgid((int)child) == vprocGetPgid(vprocPid(vp)));
    int status = 0;
    assert(vprocWaitPidShim(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 7);
    char c = 0;
    assert(vprocCloseShim(p[1]) == 0);
    assert(vprocReadShim(p[0], &c, 1) == 1 && c == 'K');
    assert(vprocReadShim(p[0], &c, 1) == 0);
    assert(vprocCloseShim(p[0]) == 0);
    assert(strcmp(vprocGetenvShim("SPAWN_TEST_VALUE"), "parent") == 0);
    vprocUnsetenvShim("SPAWN_TEST_VALUE");

    /* Its own group, when asked for. */
    assert(vprocPipeShim(p) == 0);
    gSpawnSlot = p[1];
    gSpawnOtherSlot = p[0];
    child = vprocSpawnEntry("spawn-child", false, vprocSpawnWriteEntry, argv);
    assert(child > 0);
    assert(vprocGetPgid((int)child) == (int)child);
    assert(vprocWaitPidShim(child, &status, 0) == child);
    assert(vprocCloseShim(p[1]) == 0);
    assert(vprocCloseShim(p[0]) == 0);

    /* Killed while waiting on a pipe with nothing in it: the writer then sees it gone. */
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    assert(vprocAdoptPscalFd(vp, 10, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, 11, write_end) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
    gSpawnSlot = 10;
    gSpawnOtherSlot = 11;
    char *blocked_argv[] = {"spawn-blocked", NULL};
    child = vprocSpawnEntry("spawn-blocked", true, vprocSpawnBlockedReadEntry, blocked_argv);
    assert(child > 0);
    assert(vprocCloseShim(10) == 0);
    usleep(200000);
    assert(vprocKillShim(child, SIGTERM) == 0);
    assert(vprocWaitPidShim(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    ssize_t wrote = 1;
    for (int i = 0; i < 200 && wrote > 0; ++i) {
        usleep(10000);
        wrote = vprocWriteShim(11, "x", 1);
    }
    assert(wrote == -1 && errno == EPIPE);
    assert(vprocCloseShim(11) == 0);

    /* Killed while waiting to write to a full pipe: the reader then reaches end of file. */
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    assert(vprocAdoptPscalFd(vp, 10, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, 11, write_end) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
    gSpawnSlot = 11;
    gSpawnOtherSlot = 10;
    child = vprocSpawnEntry("spawn-blocked", true, vprocSpawnBlockedWriteEntry, blocked_argv);
    assert(child > 0);
    assert(vprocCloseShim(11) == 0);
    usleep(200000);
    assert(vprocKillShim(child, SIGKILL) == 0);
    assert(vprocWaitPidShim(child, &status, 0) == child);
    char buf[4096];
    ssize_t got;
    do {
        got = vprocReadShim(10, buf, sizeof(buf));
    } while (got > 0);
    assert(got == 0);
    assert(vprocCloseShim(10) == 0);

    /* Killed in waitpid, under the task table's lock: everything else needs that lock. */
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    assert(vprocAdoptPscalFd(vp, 10, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, 11, write_end) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
    gSpawnSlot = 10;
    gSpawnOtherSlot = 11;
    char *wait_argv[] = {"spawn-waiter", NULL};
    child = vprocSpawnEntry("spawn-waiter", true, vprocSpawnWaitEntry, wait_argv);
    assert(child > 0);
    usleep(200000);
    assert(vprocKillShim(child, SIGTERM) == 0);
    assert(vprocWaitPidShim(child, &status, 0) == child);
    assert(WIFSIGNALED(status));
    assert(vprocGetPgid(vprocPid(vp)) > 0);
    /* The grandchild only ends with its pipe. */
    assert(vprocCloseShim(11) == 0);
    assert(vprocCloseShim(10) == 0);

    /* Killed before its thread got going, or just after: the status says killed either way. */
    assert(vprocPipeShim(p) == 0);
    gSpawnSlot = p[0];
    gSpawnOtherSlot = p[1];
    for (int i = 0; i < 20; ++i) {
        child = vprocSpawnEntry("spawn-child", true, vprocSpawnBlockedReadEntry, blocked_argv);
        assert(child > 0);
        assert(vprocKillShim(child, SIGTERM) == 0);
        assert(vprocWaitPidShim(child, &status, 0) == child);
        assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
    }
    assert(vprocCloseShim(p[1]) == 0);
    assert(vprocCloseShim(p[0]) == 0);

    vprocDeactivate();
    vprocDestroy(vp);
    alarm(0);
}

/* A cancelled wait on a terminal must not take the terminal with it. */
static void *vprocTtyReadCancelThread(void *arg) {
    struct pscal_fd *slave = (struct pscal_fd *)arg;
    char c;
    (void)slave->ops->read(slave, &c, 1);
    return NULL;
}

static void assert_cancelled_terminal_read_leaves_terminal_usable(void) {
    alarm(60);
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    pthread_t reader;
    assert(pthread_create(&reader, NULL, vprocTtyReadCancelThread, slave) == 0);
    usleep(200000);
    assert(pthread_cancel(reader) == 0);
    void *result = NULL;
    assert(pthread_join(reader, &result) == 0);
    assert(result == PTHREAD_CANCELED);
    /* Takes the lock the cancelled read would have kept. */
    assert(slave->ops->write(slave, "x", 1) == 1);
    pscal_fd_close(slave);
    pscal_fd_close(master);
    alarm(0);
}

static void assert_fcntl_keeps_session_host_stdin_blocking(void) {
    int session_pipe[2];
    assert(pipe(session_pipe) == 0);
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    vprocSessionStdioInitWithFds(session, session_pipe[0], session_pipe[1], session_pipe[1], 0);
    vprocSessionStdioActivate(session);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    int slot = vprocAdoptHostFd(vp, dup(session_pipe[0]));
    assert(slot >= 0);
    int flags = vprocFcntlShim(slot, F_GETFL);
    assert(flags >= 0);
    assert(vprocFcntlShim(slot, F_SETFL, flags | O_NONBLOCK) == 0);
    assert(!(vprocFcntlShim(slot, F_GETFL) & O_NONBLOCK));
    assert(!(fcntl(session_pipe[0], F_GETFL) & O_NONBLOCK));
    assert(vprocCloseShim(slot) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
    vprocSessionStdioDestroy(session);
    close(session_pipe[0]);
    close(session_pipe[1]);
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
    errno = 0;
    got = vprocWaitPidShim(other_pid, &status, WNOHANG);
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

    /* The shell takes it in without having started it, so there is nothing to wait for. */
    vprocMarkExit(child, 0);
    errno = 0;
    assert(vprocWaitPidShim(child_pid, &status, 0) == -1 && errno == ECHILD);

    vprocDestroy(parent);
    vprocDestroy(child);
    vprocSetShellSelfPid(previous_shell);
}

static void assert_children_pass_to_parent(void) {
    VProc *waiter = vprocCreate(NULL);
    VProc *stage = vprocCreate(NULL);
    VProc *job = vprocCreate(NULL);
    assert(waiter && stage && job);
    int stage_pid = vprocPid(stage);
    int job_pid = vprocPid(job);
    vprocSetParent(stage_pid, vprocPid(waiter));
    vprocSetParent(job_pid, stage_pid);

    vprocPassChildrenToParent(stage_pid);
    vprocMarkExit(stage, 0);
    vprocMarkExit(job, 0);

    vprocActivate(waiter);
    int status = 0;
    assert(vprocWaitPidShim(job_pid, &status, 0) == job_pid);
    assert(vprocWaitPidShim(stage_pid, &status, 0) == stage_pid);
    vprocDeactivate();

    int waiter_pid = vprocPid(waiter);
    vprocMarkExit(waiter, 0);
    assert(vprocWaitPidShim(waiter_pid, &status, 0) == waiter_pid);
    vprocDestroy(job);
    vprocDestroy(stage);
    vprocDestroy(waiter);
}

/* A caller sizes its buffer with one call and fills it with the next, and a task can start in
 * between. It loops over what the second call returns, so that must be what the buffer holds. */
static void assert_snapshot_fits_its_buffer(void) {
    VProc *first = vprocCreate(NULL);
    VProc *second = vprocCreate(NULL);
    assert(first && second);
    int first_pid = vprocPid(first);
    int second_pid = vprocPid(second);
    assert(vprocSnapshot(NULL, 0) >= 2);

    VProcSnapshot snaps[2];
    memset(snaps, 0, sizeof(snaps));
    assert(vprocSnapshot(snaps, 1) == 1);
    assert(snaps[0].pid > 0);
    assert(snaps[1].pid == 0);

    vprocMarkExit(first, 0);
    vprocMarkExit(second, 0);
    vprocDiscard(first_pid);
    vprocDiscard(second_pid);
    vprocDestroy(first);
    vprocDestroy(second);
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

static void assert_sigchld_aggregation_preserves_multi_child_reap(void) {
    int shell_pid = current_waiter_pid();
    vprocSetShellSelfPid(shell_pid);

    VProc *child_a = vprocCreate(NULL);
    VProc *child_b = vprocCreate(NULL);
    assert(child_a && child_b);
    int pid_a = vprocPid(child_a);
    int pid_b = vprocPid(child_b);
    vprocSetParent(pid_a, shell_pid);
    vprocSetParent(pid_b, shell_pid);

    assert(vprocSetSigchldBlocked(shell_pid, true) == 0);
    vprocMarkExit(child_a, 11);
    vprocMarkExit(child_b, 22);
    assert(vprocSigchldPending(shell_pid));

    assert(vprocSetSigchldBlocked(shell_pid, false) == 0);
    assert(vprocSigchldPending(shell_pid));

    int status_a = 0;
    int status_b = 0;
    assert(vprocWaitPidShim(pid_a, &status_a, 0) == pid_a);
    assert(vprocWaitPidShim(pid_b, &status_b, 0) == pid_b);
    assert(WIFEXITED(status_a));
    assert(WEXITSTATUS(status_a) == 11);
    assert(WIFEXITED(status_b));
    assert(WEXITSTATUS(status_b) == 22);

    vprocClearSigchldPending(shell_pid);
    assert(!vprocSigchldPending(shell_pid));

    vprocDestroy(child_a);
    vprocDestroy(child_b);
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

static void assert_foreground_handoff_resumes_stopped_group(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    assert(vprocKillShim(worker_pid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, WUNTRACED) == worker_pid);
    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGTSTP);

    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    int continued_status = 0;
    int continued_waited = 0;
    for (int i = 0; i < 100; ++i) {
        int rc = vprocWaitPidShim(worker_pid, &continued_status, WCONTINUED | WNOHANG);
        if (rc == worker_pid) {
            continued_waited = 1;
            break;
        }
        assert(rc == 0);
        usleep(5000);
    }
    assert(continued_waited);
    assert(WIFCONTINUED(continued_status));

    vprocMarkExit(worker_vp, 0);
    status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);

    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
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

static void assert_stop_and_continue_with_stdio_overrides(void) {
    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = host_pipe[0];
    opts.stdout_fd = host_pipe[1];
    opts.stderr_fd = host_pipe[1];
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    assert(pid > 0);

    /* vprocCreate duplicates stdio endpoints, so close the setup fds. */
    vprocHostClose(host_pipe[0]);
    vprocHostClose(host_pipe[1]);

    assert(vprocKillShim(pid, SIGTSTP) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED) == pid);
    assert(WIFSTOPPED(status));

    assert(vprocKillShim(pid, SIGCONT) == 0);
    vprocMarkExit(vp, 0);
    status = 0;
    assert(vprocWaitPidShim(pid, &status, 0) == pid);
    assert(WIFEXITED(status));

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

static void assert_sigtimedwait_rejects_invalid_timeout(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    int sig = 0;

    struct timespec bad_nsec = {.tv_sec = 0, .tv_nsec = 1000000000L};
    errno = 0;
    assert(vprocSigtimedwait(pid, &set, &bad_nsec, &sig) == -1);
    assert(errno == EINVAL);

    struct timespec bad_sec = {.tv_sec = -1, .tv_nsec = 0};
    errno = 0;
    assert(vprocSigtimedwait(pid, &set, &bad_sec, &sig) == -1);
    assert(errno == EINVAL);

    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDestroy(vp);
}

static void assert_itimer_real_pending_when_blocked(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);

    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGALRM);
    assert(vprocSigprocmask(pid, SIG_BLOCK, &block, NULL) == 0);

    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 50000; /* 50ms */
    assert(vprocSetitimerShim(ITIMER_REAL, &timer, NULL) == 0);

    bool saw_pending = false;
    for (int i = 0; i < 50; ++i) {
        sigset_t pending;
        sigemptyset(&pending);
        assert(vprocSigpending(pid, &pending) == 0);
        if (sigismember(&pending, SIGALRM) == 1) {
            saw_pending = true;
            break;
        }
        usleep(5000);
    }
    assert(saw_pending);

    struct itimerval now;
    memset(&now, 0, sizeof(now));
    assert(vprocGetitimerShim(ITIMER_REAL, &now) == 0);
    assert(now.it_value.tv_sec == 0);
    assert(now.it_value.tv_usec == 0);

    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGALRM);
    int got = 0;
    assert(vprocSigwait(pid, &waitset, &got) == 0);
    assert(got == SIGALRM);

    sigset_t pending_after;
    sigemptyset(&pending_after);
    assert(vprocSigpending(pid, &pending_after) == 0);
    assert(sigismember(&pending_after, SIGALRM) == 0);

    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_sigwait_wakes_for_itimer_real(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);

    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGALRM);
    assert(vprocSigprocmask(pid, SIG_BLOCK, &waitset, NULL) == 0);

    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    timer.it_value.tv_usec = 30000; /* 30ms */
    assert(vprocSetitimerShim(ITIMER_REAL, &timer, NULL) == 0);

    int got = 0;
    assert(vprocSigwait(pid, &waitset, &got) == 0);
    assert(got == SIGALRM);

    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_alarm_cancel_returns_remaining(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);

    unsigned int old = vprocAlarmShim(2);
    assert(old == 0);
    usleep(200000); /* 200ms */
    old = vprocAlarmShim(0);
    assert(old >= 1);
    assert(old <= 2);

    struct itimerval now;
    memset(&now, 0, sizeof(now));
    assert(vprocGetitimerShim(ITIMER_REAL, &now) == 0);
    assert(now.it_value.tv_sec == 0);
    assert(now.it_value.tv_usec == 0);

    sigset_t pending;
    sigemptyset(&pending);
    assert(vprocSigpending(pid, &pending) == 0);
    assert(sigismember(&pending, SIGALRM) == 0);

    vprocMarkExit(vp, 0);
    int status = 0;
    (void)vprocWaitPidShim(pid, &status, 0);
    vprocDeactivate();
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

/* A task with a thread of its own, parked until the test says what it should do next. */
typedef struct {
    VProc *vp;
    int pid;
    int stage_pid;
    bool unwinds_itself;
    bool with_stage;
    volatile int step;
    volatile int delivered_fatal;
    volatile int hits_after_delivery;
    volatile uint32_t caught;
    volatile int done;
} vproc_task_thread_ctx;

static volatile sig_atomic_t g_noted_task = 0;

static void noting_handler(int signo) {
    g_handler_hits++;
    g_noted_task = vprocHandlerTaskPid();
    vprocNoteCaughtSignal(vprocHandlerTaskPid(), signo);
}

static void task_ctx_await(vproc_task_thread_ctx *ctx, int step) {
    while (__atomic_load_n(&ctx->step, __ATOMIC_ACQUIRE) < step) {
        usleep(1000);
    }
}

static void task_ctx_advance(vproc_task_thread_ctx *ctx, int step) {
    __atomic_store_n(&ctx->step, step, __ATOMIC_RELEASE);
}

static void *vproc_task_thread(void *arg) {
    vproc_task_thread_ctx *ctx = (vproc_task_thread_ctx *)arg;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    ctx->vp = vp;
    ctx->pid = vprocPid(vp);
    vprocSetUnwindsItself(ctx->pid, ctx->unwinds_itself);
    struct sigaction sa = {0};
    sa.sa_handler = noting_handler;
    assert(vprocSigaction(ctx->pid, SIGUSR1, &sa, NULL) == 0);
    VProc *stage = NULL;
    if (ctx->with_stage) {
        stage = vprocCreate(NULL);
        assert(stage);
        vprocRegisterThread(stage, pthread_self());
        ctx->stage_pid = vprocPid(stage);
    }
    task_ctx_advance(ctx, 1);
    task_ctx_await(ctx, 2);
    pthread_testcancel();
    ctx->delivered_fatal = vprocDeliverTaskSignals(ctx->pid);
    ctx->hits_after_delivery = g_handler_hits;
    ctx->caught = vprocCaughtSignals(ctx->pid);
    if (ctx->caught & (1u << SIGUSR1)) {
        assert(vprocTakeCaughtSignal(ctx->pid, SIGUSR1));
        assert(!vprocTakeCaughtSignal(ctx->pid, SIGUSR1));
    }
    if (stage) {
        vprocUnregisterThread(stage, pthread_self());
    }
    ctx->done = 1;
    task_ctx_await(ctx, 3);
    return NULL;
}

/* A signal for a task that is another thread's waits for that thread, and a child ending on this
 * thread does not run it here. The task's own thread runs it and finds the note, on that task. */
static void assert_handler_waits_for_its_task(void) {
    g_handler_hits = 0;
    g_noted_task = 0;
    vproc_task_thread_ctx ctx = {0};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, vproc_task_thread, &ctx) == 0);
    task_ctx_await(&ctx, 1);

    assert(vprocKillShim(ctx.pid, SIGUSR1) == 0);
    assert(g_handler_hits == 0);
    VProc *child = vprocCreate(NULL);
    assert(child);
    vprocSetParent(vprocPid(child), ctx.pid);
    vprocMarkExit(child, 0);
    assert(g_handler_hits == 0);
    assert(vprocCaughtSignals(ctx.pid) == 0);
    vprocDestroy(child);

    task_ctx_advance(&ctx, 2);
    while (!ctx.done) {
        usleep(1000);
    }
    assert(ctx.hits_after_delivery == 1);
    assert(g_noted_task == ctx.pid);
    assert(ctx.delivered_fatal == 0);
    assert(ctx.caught & (1u << SIGUSR1));
    /* The note can be read only by the task's thread. */
    assert(vprocCaughtSignals(ctx.pid) == 0);
    task_ctx_advance(&ctx, 3);
    assert(pthread_join(tid, NULL) == 0);
    vprocMarkExit(ctx.vp, 0);
    vprocDiscard(ctx.pid);
    vprocDestroy(ctx.vp);
}

/* A shell is not cancelled: what would end it waits for it to look, and it stays a live task. */
static void assert_unwinding_task_is_not_cancelled(void) {
    vproc_task_thread_ctx ctx = {0};
    ctx.unwinds_itself = true;
    ctx.with_stage = true;
    pthread_t tid;
    assert(pthread_create(&tid, NULL, vproc_task_thread, &ctx) == 0);
    task_ctx_await(&ctx, 1);

    assert(vprocKillShim(ctx.pid, SIGTERM) == 0);
    assert(vprocKillShim(ctx.pid, 0) == 0);
    /* Nor is a stage cancelled that runs on the shell's thread: pthread_testcancel below would
     * end the thread with no result. */
    assert(vprocKillShim(ctx.stage_pid, SIGTERM) == 0);
    /* It counts as ended once its thread has unwound, or after a grace period. */
    int status = 0;
    pid_t waited = vprocWaitPidShim(ctx.stage_pid, &status, WNOHANG);
    assert(waited <= 0 || WIFSIGNALED(status));

    task_ctx_advance(&ctx, 2);
    while (!ctx.done) {
        usleep(1000);
    }
    assert(ctx.delivered_fatal == SIGTERM);
    task_ctx_advance(&ctx, 3);
    void *ret = NULL;
    assert(pthread_join(tid, &ret) == 0);
    assert(ret != PTHREAD_CANCELED);
    vprocMarkExit(ctx.vp, 0);
    vprocDiscard(ctx.pid);
    vprocDestroy(ctx.vp);
}

/* SIGCONT continues whatever its disposition; a handler or a block only delays the handler. And
 * pending, a stop and a continue cancel each other. */
static void assert_sigcont_continues_whatever_its_disposition(void) {
    int parent = current_waiter_pid();
    VProc *child = vprocCreate(NULL);
    assert(child);
    int cpid = vprocPid(child);
    vprocSetParent(cpid, parent);
    int status = 0;

    struct sigaction sa = {0};
    sa.sa_handler = test_handler;
    assert(vprocSigaction(cpid, SIGCONT, &sa, NULL) == 0);
    g_handler_hits = 0;
    assert(vprocKillShim(cpid, SIGSTOP) == 0);
    assert(vprocWaitPidShim(cpid, &status, WUNTRACED) == cpid);
    assert(WIFSTOPPED(status));
    assert(vprocKillShim(cpid, SIGCONT) == 0);
    assert(vprocWaitPidShim(cpid, &status, WCONTINUED | WNOHANG) == cpid);
    assert(WIFCONTINUED(status));
    /* Its handler is the task's to run, and this thread isn't the task's. */
    assert(g_handler_hits == 0);
    sigset_t pending;
    assert(vprocSigpending(cpid, &pending) == 0);
    assert(sigismember(&pending, SIGCONT));

    /* A stop takes back a continue still pending. */
    assert(vprocKillShim(cpid, SIGSTOP) == 0);
    assert(vprocSigpending(cpid, &pending) == 0);
    assert(!sigismember(&pending, SIGCONT));
    assert(vprocWaitPidShim(cpid, &status, WUNTRACED) == cpid);
    assert(WIFSTOPPED(status));

    /* Blocked: it continues, and the signal stays pending. */
    struct sigaction sa_default = {0};
    sa_default.sa_handler = SIG_DFL;
    assert(vprocSigaction(cpid, SIGCONT, &sa_default, NULL) == 0);
    assert(vprocBlockSignals(cpid, 1 << SIGCONT) == 0);
    assert(vprocKillShim(cpid, SIGCONT) == 0);
    assert(vprocWaitPidShim(cpid, &status, WCONTINUED | WNOHANG) == cpid);
    assert(WIFCONTINUED(status));
    assert(vprocSigpending(cpid, &pending) == 0);
    assert(sigismember(&pending, SIGCONT));

    vprocMarkExit(child, 0);
    (void)vprocWaitPidShim(cpid, &status, 0);
    vprocDestroy(child);
}

/* A task in a slot an earlier one used starts clean: one that unwinds itself would not end when
 * it was killed. */
static void assert_reused_slot_starts_clean(void) {
    int parent = current_waiter_pid();
    for (int i = 0; i < 2; ++i) {
        VProc *vp = vprocCreate(NULL);
        assert(vp);
        int pid = vprocPid(vp);
        vprocSetParent(pid, parent);
        assert(vprocKillShim(pid, SIGTERM) == 0);
        int status = 0;
        assert(vprocWaitPidShim(pid, &status, WNOHANG) == pid);
        assert(WIFSIGNALED(status));
        vprocSetUnwindsItself(pid, true);
        vprocSetSilentToParent(pid, true);
        vprocSetNotAProcess(pid);
        vprocNoteCaughtSignal(pid, SIGUSR1);
        vprocDiscard(pid);
        vprocDestroy(vp);
    }
}

/* A `.` or `eval` is not a process: what it starts sees the shell as its parent, and only what
 * was marked is passed over. */
static void assert_getppid_passes_over_what_is_no_process(void) {
    int shell = current_waiter_pid();
    VProc *stage = vprocCreate(NULL);
    assert(stage);
    int stage_pid = vprocPid(stage);
    vprocSetParent(stage_pid, shell);
    VProc *child = vprocCreate(NULL);
    assert(child);
    int child_pid = vprocPid(child);
    vprocSetParent(child_pid, stage_pid);

    vprocActivate(child);
    assert(vprocGetPpidShim() == stage_pid);
    vprocDeactivate();

    vprocSetNotAProcess(stage_pid);
    vprocActivate(child);
    assert(vprocGetPpidShim() == shell);
    vprocDeactivate();

    /* The stage is still what waits for it. */
    vprocActivate(stage);
    int status = 0;
    assert(vprocWaitPidShim(child_pid, &status, WNOHANG) == 0);
    vprocDeactivate();

    vprocDiscard(child_pid);
    vprocDestroy(child);
    vprocDiscard(stage_pid);
    vprocDestroy(stage);
}

/* As exec does. SIGINT and SIGQUIT stay, for a stage's Ctrl-C. */
static void assert_reset_caught_signals(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pid = vprocPid(vp);
    struct sigaction sa = {0};
    sa.sa_handler = test_handler;
    struct sigaction ign = {0};
    ign.sa_handler = SIG_IGN;
    assert(vprocSigaction(pid, SIGUSR1, &sa, NULL) == 0);
    assert(vprocSigaction(pid, SIGINT, &sa, NULL) == 0);
    assert(vprocSigaction(pid, SIGUSR2, &ign, NULL) == 0);
    vprocResetCaughtSignals(pid);
    struct sigaction now = {0};
    assert(vprocSigaction(pid, SIGUSR1, NULL, &now) == 0);
    assert(now.sa_handler == SIG_DFL);
    assert(vprocSigaction(pid, SIGINT, NULL, &now) == 0);
    assert(now.sa_handler == test_handler);
    assert(vprocSigaction(pid, SIGUSR2, NULL, &now) == 0);
    assert(now.sa_handler == SIG_IGN);
    vprocMarkExit(vp, 0);
    vprocDiscard(pid);
    vprocDestroy(vp);
}

typedef struct {
    VProc *parent;
    int child_pid;
    int parked;
    int returned;
} vproc_cancelled_waiter_ctx;

static void *vproc_cancelled_waiter_thread(void *arg) {
    vproc_cancelled_waiter_ctx *ctx = (vproc_cancelled_waiter_ctx *)arg;
    vprocActivate(ctx->parent);
    __atomic_store_n(&ctx->parked, 1, __ATOMIC_RELEASE);
    int status = 0;
    (void)vprocWaitPidShim(ctx->child_pid, &status, 0);
    ctx->returned = 1;
    return NULL;
}

/* `kill` cancels a job's threads. A thread cancelled in a condition wait gets the mutex back
 * before it dies, and the task table must not stay locked: every vproc call would hang. */
static void assert_cancelled_wait_lets_go_of_task_table(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *parent = vprocCreate(&opts);
    assert(parent);
    opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *child = vprocCreate(&opts);
    assert(child);
    int child_pid = vprocPid(child);
    vprocSetParent(child_pid, vprocPid(parent));

    vproc_cancelled_waiter_ctx ctx = {.parent = parent, .child_pid = child_pid};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, vproc_cancelled_waiter_thread, &ctx) == 0);
    while (!__atomic_load_n(&ctx.parked, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 50000000};
    nanosleep(&nap, NULL);
    assert(pthread_cancel(tid) == 0);
    void *ret = NULL;
    assert(pthread_join(tid, &ret) == 0);
    assert(ret == PTHREAD_CANCELED);
    assert(ctx.returned == 0);

    alarm(30);
    assert(vprocGetPgid(child_pid) > 0);
    alarm(0);
    vprocDiscard(child_pid);
    vprocDestroy(child);
    vprocDiscard(vprocPid(parent));
    vprocDestroy(parent);
}

typedef struct {
    VProc *vp;
    int ready;
    int cancel_sent;
} vproc_cancelled_clone_ctx;

static void *vproc_cancelled_clone_thread(void *arg) {
    vproc_cancelled_clone_ctx *ctx = (vproc_cancelled_clone_ctx *)arg;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&ctx->cancel_sent, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
    /* Duplicates a descriptor under the vproc's lock, with a cancel pending. */
    (void)vprocRestoreHostFd(ctx->vp, STDOUT_FILENO, STDOUT_FILENO);
    pthread_testcancel();
    return NULL;
}

static void assert_cancel_pending_lets_go_of_vproc_lock(void) {
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    vproc_cancelled_clone_ctx ctx = {.vp = vp};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, vproc_cancelled_clone_thread, &ctx) == 0);
    while (!__atomic_load_n(&ctx.ready, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    assert(pthread_cancel(tid) == 0);
    __atomic_store_n(&ctx.cancel_sent, 1, __ATOMIC_RELEASE);
    void *ret = NULL;
    assert(pthread_join(tid, &ret) == 0);
    assert(ret == PTHREAD_CANCELED);

    /* This takes the lock the thread must have let go of. */
    alarm(30);
    struct pscal_fd *held = vprocGetPscalFd(vp, STDOUT_FILENO);
    alarm(0);
    if (held) {
        pscal_fd_close(held);
    }
    vprocDiscard(vprocPid(vp));
    vprocDestroy(vp);
}

/* The job table clears what it kept on a job once it has reaped it, and the job's thread ends
 * its vproc when it gets there. Neither may bring the pid back as a row. */
static void assert_reaped_job_stays_gone(void) {
    int prev_shell = vprocGetShellSelfPid();
    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int leader_pid = vprocPid(leader);
    vprocSetShellSelfPid(leader_pid);
    VProc *prev = vprocCurrent();
    vprocActivate(leader);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    vprocSetParent(pid, leader_pid);
    assert(vprocKillShim(pid, SIGTERM) == 0);
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WNOHANG) == pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGTERM);

    /* Reaped: a second wait has nothing to find. */
    errno = 0;
    assert(vprocWaitPidShim(pid, &status, WNOHANG) == -1);
    assert(errno == ECHILD);

    vprocSetJobId(pid, 0);
    vprocSetCommandLabel(pid, NULL);
    assert(vprocGetPgid(pid) == -1);
    vprocMarkExit(vp, 128 + SIGKILL);
    assert(vprocGetPgid(pid) == -1);
    vprocDestroy(vp);

    vprocDeactivate();
    if (prev) {
        vprocActivate(prev);
    }
    vprocSetShellSelfPid(prev_shell);
    vprocDiscard(leader_pid);
    vprocDestroy(leader);
}

typedef struct {
    int scope_pid;
    int ready;
} vproc_scope_thread_ctx;

static void *vproc_scope_thread(void *arg) {
    vproc_scope_thread_ctx *ctx = (vproc_scope_thread_ctx *)arg;
    VProcCommandScope scope;
    assert(vprocCommandScopeBegin(&scope, "scoped", true, false));
    ctx->scope_pid = scope.pid;
    __atomic_store_n(&ctx->ready, 1, __ATOMIC_RELEASE);
    for (;;) {
        struct timespec nap = {.tv_sec = 1, .tv_nsec = 0};
        nanosleep(&nap, NULL);
    }
    return NULL;
}

/* A killed thread never reaches the end of a command scope it made, and what the scope holds
 * would stay open. The vproc it was started under has an owner, which ends that. */
#if defined(__APPLE__)
static void *vproc_ended_thread(void *arg) {
    return arg;
}

static mach_port_t vproc_new_port_right(void) {
    mach_port_t right = MACH_PORT_NULL;
    assert(mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &right) == KERN_SUCCESS);
    assert(mach_port_insert_right(mach_task_self(), right, right, MACH_MSG_TYPE_MAKE_SEND) == KERN_SUCCESS);
    return right;
}

static void vproc_drop_port_right(mach_port_t right) {
    mach_port_mod_refs(mach_task_self(), right, MACH_PORT_RIGHT_SEND, -1);
    mach_port_mod_refs(mach_task_self(), right, MACH_PORT_RIGHT_RECEIVE, -1);
}

/* A thread that ended and wasn't joined still gives its port's old name, and the name comes back
 * for another right. A snapshot looked the thread up by that name: thread_info ended the process
 * for a right like this one, and the release after it took a right someone else owned. */
static void assert_snapshot_leaves_an_ended_threads_port_name_alone(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    pthread_t ended;
    assert(pthread_create(&ended, NULL, vproc_ended_thread, NULL) == 0);
    vprocRegisterThread(vp, ended);
    while (pthread_kill(ended, 0) == 0) {
        sched_yield();
    }
    mach_port_t name = pthread_mach_thread_np(ended);
    assert(name != MACH_PORT_NULL);
    /* Held until the kernel's free list reaches the name's slot, then taken and dropped there:
     * each new owner of a slot gets the next generation of its name, and they come round. */
    enum { HELD_MAX = 65536 };
    mach_port_t *held = (mach_port_t *)calloc(HELD_MAX, sizeof(mach_port_t));
    assert(held);
    size_t held_count = 0;
    mach_port_t right = vproc_new_port_right();
    while (MACH_PORT_INDEX(right) != MACH_PORT_INDEX(name)) {
        assert(held_count < HELD_MAX);
        held[held_count++] = right;
        right = vproc_new_port_right();
    }
    for (int i = 0; i < 1000000 && right != name; ++i) {
        vproc_drop_port_right(right);
        right = vproc_new_port_right();
    }
    assert(right == name);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = (VProcSnapshot *)calloc(cap, sizeof(VProcSnapshot));
    assert(snaps);
    (void)vprocSnapshot(snaps, cap);
    free(snaps);

    mach_port_urefs_t refs = 0;
    assert(mach_port_get_refs(mach_task_self(), right, MACH_PORT_RIGHT_SEND, &refs) == KERN_SUCCESS);
    assert(refs == 1);
    vproc_drop_port_right(right);
    while (held_count > 0) {
        vproc_drop_port_right(held[--held_count]);
    }
    free(held);
    vprocUnregisterThread(vp, ended);
    pthread_join(ended, NULL);
    int pid = vprocPid(vp);
    vprocMarkExit(vp, 0);
    vprocDiscard(pid);
    vprocDestroy(vp);
}
#endif

static void assert_kill_ends_what_the_thread_made(void) {
    VProc *shell = vprocCreate(NULL);
    assert(shell);
    vprocActivate(shell);
    VProc *job = vprocCreate(NULL);
    assert(job);
    int job_pid = vprocPid(job);
    vproc_scope_thread_ctx ctx = {0};
    pthread_t tid;
    assert(vprocSpawnThread(job, vproc_scope_thread, &ctx, &tid) == 0);
    while (!__atomic_load_n(&ctx.ready, __ATOMIC_ACQUIRE)) {
        sched_yield();
    }
    assert(vprocGetPgid(ctx.scope_pid) > 0);
    assert(vprocKillShim(job_pid, SIGTERM) == 0);
    void *ret = NULL;
    assert(pthread_join(tid, &ret) == 0);
    assert(ret == PTHREAD_CANCELED);
    assert(vprocGetPgid(ctx.scope_pid) == -1);
    assert(vprocGetPgid(job_pid) > 0);
    vprocDiscard(job_pid);
    vprocDestroy(job);
    vprocDeactivate();
    vprocDiscard(vprocPid(shell));
    vprocDestroy(shell);
}

typedef struct {
    VProc *vp;
    int fd;
} vproc_cancelled_read_ctx;

static void *vproc_cancelled_read_thread(void *arg) {
    vproc_cancelled_read_ctx *ctx = (vproc_cancelled_read_ctx *)arg;
    vprocActivate(ctx->vp);
    char c;
    (void)vprocReadShim(ctx->fd, &c, 1);
    return NULL;
}

/* A read holds a reference to its descriptor. Left behind by a cancel, that end of the pipe
 * would never close, and the writer would never hear that nobody reads. */
static void assert_cancelled_read_lets_go_of_its_descriptor(void) {
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    assert(vprocAdoptPscalFd(vp, STDIN_FILENO, read_end) == 0);
    pscal_fd_close(read_end);

    vproc_cancelled_read_ctx ctx = {.vp = vp, .fd = STDIN_FILENO};
    pthread_t tid;
    assert(pthread_create(&tid, NULL, vproc_cancelled_read_thread, &ctx) == 0);
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 300000000};
    nanosleep(&nap, NULL);
    assert(pthread_cancel(tid) == 0);
    void *ret = NULL;
    assert(pthread_join(tid, &ret) == 0);
    assert(ret == PTHREAD_CANCELED);

    /* The vproc's was the only other reference. */
    vprocDiscard(vprocPid(vp));
    vprocDestroy(vp);
    char c = 'x';
    assert(write_end->ops->write(write_end, &c, 1) < 0);
    pscal_fd_close(write_end);
}

static int open_test_terminal(int *out_master) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    assert(master >= 0);
    assert(grantpt(master) == 0);
    assert(unlockpt(master) == 0);
    int terminal = open(ptsname(master), O_RDWR | O_NOCTTY);
    assert(terminal >= 0);
    *out_master = master;
    return terminal;
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

    int master = -1;
    int terminal = open_test_terminal(&master);
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    opts.stdin_fd = terminal;
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
    close(terminal);
    close(master);
}

/* Only the terminal stops a job that reads it: not the /dev/null it starts with, and not a file it
 * redirected stdin from, which `cat < f &` waited on for ever. */
static void assert_background_read_of_a_file_does_not_stop(void) {
    int prev_shell = vprocGetShellSelfPid();
    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int sid = vprocPid(leader);
    vprocSetShellSelfPid(sid);
    assert(vprocSetSid(sid, sid) == 0);
    assert(vprocSetForegroundPgid(sid, sid) == 0);

    char path[] = "/tmp/vproc_bg_read_XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    assert(write(file, "f", 1) == 1);
    assert(lseek(file, 0, SEEK_SET) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    opts.stdin_fd = -2;
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocSetPgid(pid, sid + 5) == 0);

    VProc *prev = vprocCurrent();
    vprocActivate(vp);
    char ch = 0;
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == 0);
    int slot = vprocAdoptHostFd(vp, file);
    assert(slot >= 0);
    assert(vprocDup2Shim(slot, STDIN_FILENO) == STDIN_FILENO);
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == 1);
    assert(ch == 'f');
    vprocDeactivate();
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED | WNOHANG) == 0);
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
    unlink(path);
}

/* A session's stdin is its terminal, tty or not. A file in its place doesn't stop a job, and given
 * back, it does again. */
static void assert_background_read_of_session_stdin_stops(void) {
    int prev_shell = vprocGetShellSelfPid();
    int session_pipe[2];
    assert(pipe(session_pipe) == 0);
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    vprocSessionStdioInitWithFds(session, session_pipe[0], session_pipe[1], session_pipe[1], 0);
    vprocSessionStdioActivate(session);
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
    opts.stdin_fd = session_pipe[0];
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocSetPgid(pid, sid + 5) == 0);

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

    char path[] = "/tmp/vproc_bg_read_XXXXXX";
    int file = mkstemp(path);
    assert(file >= 0);
    assert(write(file, "f", 1) == 1);
    assert(lseek(file, 0, SEEK_SET) == 0);
    vprocActivate(vp);
    int slot = vprocAdoptHostFd(vp, file);
    assert(slot >= 0);
    assert(vprocDup2Shim(slot, STDIN_FILENO) == STDIN_FILENO);
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == 1);
    assert(ch == 'f');
    vprocDeactivate();
    assert(vprocWaitPidShim(pid, &status, WUNTRACED | WNOHANG) == 0);

    assert(vprocRestoreHostFd(vp, STDIN_FILENO, session_pipe[0]) == STDIN_FILENO);
    vprocActivate(vp);
    errno = 0;
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == -1);
    assert(errno == EINTR);
    vprocDeactivate();
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
    vprocSessionStdioDestroy(session);
    close(session_pipe[0]);
    close(session_pipe[1]);
    vprocSetShellSelfPid(prev_shell);
    unlink(path);
}

/* A socket on stdin, as an rsync server has, reads like any descriptor. A terminal put back with
 * dup2 stops the job again. */
static void assert_background_read_of_a_socket_does_not_stop(void) {
    int prev_shell = vprocGetShellSelfPid();
    VProcOptions leader_opts = vprocDefaultOptions();
    leader_opts.pid_hint = vprocReservePid();
    VProc *leader = vprocCreate(&leader_opts);
    assert(leader);
    int sid = vprocPid(leader);
    vprocSetShellSelfPid(sid);
    assert(vprocSetSid(sid, sid) == 0);
    assert(vprocSetForegroundPgid(sid, sid) == 0);

    int master = -1;
    int terminal = open_test_terminal(&master);
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    opts.stdin_fd = terminal;
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int pid = vprocPid(vp);
    assert(vprocSetPgid(pid, sid + 5) == 0);

    VProc *prev = vprocCurrent();
    vprocActivate(vp);
    int saved = vprocDupShim(STDIN_FILENO);
    assert(saved >= 0);
    int sv[2];
    assert(vprocSocketpairShim(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(vprocWriteShim(sv[1], "s", 1) == 1);
    assert(vprocDup2Shim(sv[0], STDIN_FILENO) == STDIN_FILENO);
    char ch = 0;
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == 1);
    assert(ch == 's');
    vprocDeactivate();
    int status = 0;
    assert(vprocWaitPidShim(pid, &status, WUNTRACED | WNOHANG) == 0);

    vprocActivate(vp);
    assert(vprocDup2Shim(saved, STDIN_FILENO) == STDIN_FILENO);
    errno = 0;
    assert(vprocReadShim(STDIN_FILENO, &ch, 1) == -1);
    assert(errno == EINTR);
    vprocDeactivate();
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
    close(terminal);
    close(master);
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

    /* A missing parent is ENOENT, as on any host: open does not make it. */
    errno = 0;
    assert(pscalPathVirtualized_open("/no-such-dir/sub/f.txt", O_CREAT | O_WRONLY, 0600) < 0);
    assert(errno == ENOENT);
    char missing_dir[PATH_MAX];
    snprintf(missing_dir, sizeof(missing_dir), "%s/no-such-dir", root);
    assert(access(missing_dir, F_OK) != 0);

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

static ssize_t read_virtual_file(const char *path, char *buffer, size_t buffer_size) {
    assert(path);
    assert(buffer);
    assert(buffer_size > 1);
    int fd = pscalPathVirtualized_open(path, O_RDONLY, 0);
    assert(fd >= 0);
    ssize_t n = read(fd, buffer, buffer_size - 1);
    assert(n >= 0);
    buffer[n] = '\0';
    close(fd);
    return n;
}

static void assert_path_truncate_mount_rewrites_virtual_paths(void) {
    char source_templ[] = "/tmp/vproc-mount-src-XXXXXX";
    char *source_root = mkdtemp(source_templ);
    assert(source_root);
    char root_templ[] = "/tmp/vproc-mount-root-XXXXXX";
    char *root = mkdtemp(root_templ);
    assert(root);

    pathTruncateMountClearAll();
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/mnt", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/mnt/ext", 0700) == 0 || errno == EEXIST);

    char source_file[PATH_MAX];
    snprintf(source_file, sizeof(source_file), "%s/hello.txt", source_root);
    int source_fd = open(source_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(source_fd >= 0);
    assert(write(source_fd, "mounted", 7) == 7);
    close(source_fd);

    assert(pathTruncateMountAdd(source_root, "/mnt/ext", "bind", "rw", 0));

    char expanded[PATH_MAX];
    assert(pathTruncateExpand("/mnt/ext/hello.txt", expanded, sizeof(expanded)));
    assert(strcmp(expanded, source_file) == 0);

    int fd = pscalPathVirtualized_open("/mnt/ext/hello.txt", O_RDONLY, 0);
    assert(fd >= 0);
    char buf[16] = {0};
    assert(read(fd, buf, sizeof(buf)) == 7);
    assert(strncmp(buf, "mounted", 7) == 0);
    close(fd);

    char stripped[PATH_MAX];
    assert(pathTruncateStrip(source_file, stripped, sizeof(stripped)));
    assert(strcmp(stripped, "/mnt/ext/hello.txt") == 0);

    pathTruncateMountClearAll();
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX * 2 + 32];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s", root, source_root);
    (void)system(cleanup_cmd);
}

static void assert_path_truncate_mount_remove_unmaps_virtual_paths(void) {
    char source_templ[] = "/tmp/vproc-umount-src-XXXXXX";
    char *source_root = mkdtemp(source_templ);
    assert(source_root);
    char root_templ[] = "/tmp/vproc-umount-root-XXXXXX";
    char *root = mkdtemp(root_templ);
    assert(root);

    pathTruncateMountClearAll();
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/mnt", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/mnt/ext", 0700) == 0 || errno == EEXIST);

    char source_file[PATH_MAX];
    snprintf(source_file, sizeof(source_file), "%s/hello.txt", source_root);
    int source_fd = open(source_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(source_fd >= 0);
    assert(write(source_fd, "mounted", 7) == 7);
    close(source_fd);

    assert(pathTruncateMountAdd(source_root, "/mnt/ext", "bind", "rw", 0));

    int fd = pscalPathVirtualized_open("/mnt/ext/hello.txt", O_RDONLY, 0);
    assert(fd >= 0);
    close(fd);

    assert(pathTruncateMountRemove("/mnt/ext"));
    assert(!pathTruncateMountRemove("/mnt/ext"));
    assert(errno == ENOENT);

    char expanded[PATH_MAX];
    assert(pathTruncateExpand("/mnt/ext/hello.txt", expanded, sizeof(expanded)));
    assert(strcmp(expanded, source_file) != 0);
    assert(strstr(expanded, "/mnt/ext/hello.txt") != NULL);

    errno = 0;
    fd = pscalPathVirtualized_open("/mnt/ext/hello.txt", O_RDONLY, 0);
    assert(fd < 0);
    assert(errno == ENOENT);

    char mounts_buf[8192];
    assert(read_virtual_file("/proc/mounts", mounts_buf, sizeof(mounts_buf)) > 0);
    assert(strstr(mounts_buf, " /mnt/ext ") == NULL);

    pathTruncateMountClearAll();
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX * 2 + 32];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s", root, source_root);
    (void)system(cleanup_cmd);
}

static void assert_proc_mount_files_include_virtual_mounts(void) {
    char source_templ[] = "/tmp/vproc-proc-mount-src-XXXXXX";
    char *source_root = mkdtemp(source_templ);
    assert(source_root);
    char root_templ[] = "/tmp/vproc-proc-mount-root-XXXXXX";
    char *root = mkdtemp(root_templ);
    assert(root);

    pathTruncateMountClearAll();
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);
    assert(pscalPathVirtualized_mkdir("/mnt", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/mnt/ext", 0700) == 0 || errno == EEXIST);

    assert(pathTruncateMountAdd(source_root, "/mnt/ext", "bind", "ro,nodev", 0));

    char buf[8192];
    assert(read_virtual_file("/proc/mounts", buf, sizeof(buf)) > 0);
    assert(strstr(buf, source_root) != NULL);
    assert(strstr(buf, " /mnt/ext ") != NULL);
    assert(strstr(buf, " bind ") != NULL);

    assert(read_virtual_file("/proc/mountinfo", buf, sizeof(buf)) > 0);
    assert(strstr(buf, source_root) != NULL);
    assert(strstr(buf, " /mnt/ext ") != NULL);

    pathTruncateMountClearAll();
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 32];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s", root, source_root);
    (void)system(cleanup_cmd);
}

static bool testEncodeFstabField(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0) {
        return false;
    }
    size_t out_len = 0;
    for (size_t i = 0; input[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)input[i];
        if (ch == '\\' || isspace(ch) || ch == '#') {
            if (out_len + 4 >= out_size) {
                return false;
            }
            out[out_len++] = '\\';
            out[out_len++] = (char)('0' + ((ch >> 6) & 0x07));
            out[out_len++] = (char)('0' + ((ch >> 3) & 0x07));
            out[out_len++] = (char)('0' + (ch & 0x07));
            continue;
        }
        if (out_len + 1 >= out_size) {
            return false;
        }
        out[out_len++] = (char)ch;
    }
    out[out_len] = '\0';
    return true;
}

static void assert_kernel_mounts_from_fstab_at_startup(void) {
    char source_templ[] = "/tmp/vproc-fstab-src-XXXXXX";
    char *source_root = mkdtemp(source_templ);
    assert(source_root);
    char root_templ[] = "/tmp/vproc-fstab-root-XXXXXX";
    char *root = mkdtemp(root_templ);
    assert(root);

    pathTruncateMountClearAll();
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char etc_path[PATH_MAX];
    snprintf(etc_path, sizeof(etc_path), "%s/etc", root);
    assert(mkdir(etc_path, 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/mnt", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/mnt/ext", 0700) == 0 || errno == EEXIST);

    char source_file[PATH_MAX];
    char source_with_space[PATH_MAX];
    snprintf(source_with_space, sizeof(source_with_space), "%s/dir with space", source_root);
    assert(mkdir(source_with_space, 0700) == 0 || errno == EEXIST);
    snprintf(source_file, sizeof(source_file), "%s/hello.txt", source_with_space);
    int source_fd = open(source_file, O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(source_fd >= 0);
    assert(write(source_fd, "fstab-mounted", 13) == 13);
    close(source_fd);

    char fstab_path[PATH_MAX];
    snprintf(fstab_path, sizeof(fstab_path), "%s/etc/fstab", root);
    FILE *fstab = fopen(fstab_path, "w");
    assert(fstab);
    char source_escaped[PATH_MAX * 4];
    assert(testEncodeFstabField(source_with_space, source_escaped, sizeof(source_escaped)));
    assert(fprintf(fstab, "# test mount\n%s /mnt/ext bind rw 0 0\n", source_escaped) > 0);
    assert(fclose(fstab) == 0);

    vprocResetStartupFstabStateForTests();
    assert(vprocEnsureKernelPid() > 0);

    char expanded[PATH_MAX];
    assert(pathTruncateExpand("/mnt/ext/hello.txt", expanded, sizeof(expanded)));
    assert(strstr(expanded, "/hello.txt") != NULL);

    int mounted_fd = pscalPathVirtualized_open("/mnt/ext/hello.txt", O_RDONLY, 0);
    assert(mounted_fd >= 0);
    char buf[32] = {0};
    assert(read(mounted_fd, buf, sizeof(buf)) == 13);
    assert(strncmp(buf, "fstab-mounted", 13) == 0);
    close(mounted_fd);

    char mounts_buf[8192];
    assert(read_virtual_file("/proc/mounts", mounts_buf, sizeof(mounts_buf)) > 0);
    assert(strstr(mounts_buf, source_root) != NULL);
    assert(strstr(mounts_buf, " /mnt/ext ") != NULL);

    pathTruncateMountClearAll();
    setenv("PATH_TRUNCATE", "/tmp", 1);
    char cleanup_cmd[PATH_MAX + 32];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s %s", root, source_root);
    (void)system(cleanup_cmd);
}

static long long monotonic_ms(void) {
    struct timespec ts;
    assert(clock_gettime(CLOCK_MONOTONIC, &ts) == 0);
    return ((long long)ts.tv_sec * 1000ll) + ((long long)ts.tv_nsec / 1000000ll);
}

static bool int_list_contains(const int *values, size_t count, int needle) {
    for (size_t i = 0; i < count; ++i) {
        if (values[i] == needle) {
            return true;
        }
    }
    return false;
}

static size_t read_numeric_virtual_entries(const char *dir_path, int *out, size_t out_capacity) {
    assert(dir_path);
    assert(out);
    DIR *dir = pscalPathVirtualized_opendir(dir_path);
    assert(dir);
    size_t count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '\0') {
            continue;
        }
        char *endptr = NULL;
        errno = 0;
        long parsed = strtol(entry->d_name, &endptr, 10);
        if (errno != 0 || !endptr || *endptr != '\0' || parsed <= 0 || parsed > INT_MAX) {
            continue;
        }
        if (count < out_capacity) {
            out[count++] = (int)parsed;
        }
    }
    closedir(dir);
    return count;
}

static void assert_proc_vm_files_present_and_stable(void) {
    char templ[] = "/tmp/vproc-procvm-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    /* Pre-create a stale by-address VM directory and ensure refreshes do not
     * delete it (directory churn can invalidate CWD in other sessions). */
    assert(pscalPathVirtualized_mkdir("/proc/vm", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/proc/vm/by-address", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/proc/vm/by-address/deadbeef", 0700) == 0 || errno == EEXIST);

    char first[1024] = {0};
    char second[1024] = {0};
    for (int pass = 0; pass < 2; ++pass) {
        int fd = pscalPathVirtualized_open("/proc/vm/summary", O_RDONLY, 0);
        assert(fd >= 0);
        char *target = (pass == 0) ? first : second;
        ssize_t n = read(fd, target, sizeof(first) - 1);
        assert(n >= 0);
        target[n] = '\0';
        close(fd);
    }
    assert(strstr(first, "vm_count ") != NULL);
    assert(strstr(second, "vm_count ") != NULL);

    int list_fd = pscalPathVirtualized_open("/proc/vm/list", O_RDONLY, 0);
    assert(list_fd >= 0);
    char list_buf[1024] = {0};
    ssize_t list_n = read(list_fd, list_buf, sizeof(list_buf) - 1);
    assert(list_n >= 0);
    list_buf[list_n] = '\0';
    close(list_fd);
    assert(strstr(list_buf, "idx vm_addr owner_addr") != NULL);

    struct stat st;
    assert(pscalPathVirtualized_stat("/proc/vm/by-address", &st) == 0);
    assert(S_ISDIR(st.st_mode));
    assert(pscalPathVirtualized_stat("/proc/vm/by-address/deadbeef", &st) == 0);
    assert(S_ISDIR(st.st_mode));

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_core_and_net_entries_present(void) {
    char templ[] = "/tmp/vproc-procnet-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char buf[4096];
    assert(read_virtual_file("/proc/interrupts", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "CPU0") != NULL);
    assert(strstr(buf, "PSCAL-virt-timer") != NULL);

    assert(read_virtual_file("/proc/softirqs", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "TIMER") != NULL);
    assert(strstr(buf, "RCU") != NULL);

    assert(read_virtual_file("/proc/modules", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "vproc") != NULL);

    assert(read_virtual_file("/proc/net/protocols", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "TCP") != NULL);
    assert(strstr(buf, "UNIX") != NULL);

    assert(read_virtual_file("/proc/net/wireless", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Inter-|") != NULL);

    assert(read_virtual_file("/proc/net/softnet_stat", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "00000000") != NULL);

    assert(read_virtual_file("/proc/net/dev_mcast", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "333300000001") != NULL);

    assert(read_virtual_file("/proc/net/igmp", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Group") != NULL);
    assert(strstr(buf, "010000E0") != NULL);

    assert(read_virtual_file("/proc/net/igmp6", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "ff020000000000000000000000000001") != NULL);

    assert(read_virtual_file("/proc/net/ipv6_route", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "00000000000000000000000000000000") != NULL);

    assert(read_virtual_file("/proc/net/rt6_stats", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0001") != NULL);

    assert(read_virtual_file("/proc/net/fib_trie", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Main:") != NULL);

    assert(read_virtual_file("/proc/net/fib_triestat", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Basic info:") != NULL);

    assert(read_virtual_file("/proc/net/netlink", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Pid") != NULL);

    assert(read_virtual_file("/proc/net/ptype", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Type") != NULL);
    assert(strstr(buf, "0800") != NULL);

    assert(read_virtual_file("/proc/net/psched", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "000003e8") != NULL);

    assert(read_virtual_file("/proc/net/xfrm_stat", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "XfrmInError") != NULL);

    assert(read_virtual_file("/proc/net/stat/rt_cache", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "entries") != NULL);

    assert(read_virtual_file("/proc/net/stat/arp_cache", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "allocs") != NULL);

    assert(read_virtual_file("/proc/net/stat/ndisc_cache", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "lookups") != NULL);

    char target[PATH_MAX];
    ssize_t link_n = pscalPathVirtualized_readlink("/proc/self/root", target, sizeof(target) - 1);
    assert(link_n > 0);
    target[link_n] = '\0';
    assert(strcmp(target, "/") == 0);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_sys_entries_present(void) {
    char templ[] = "/tmp/vproc-procsys-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char buf[4096];
    assert(read_virtual_file("/proc/sys/kernel/hostname", buf, sizeof(buf)) > 0);
    assert(read_virtual_file("/proc/sys/kernel/pid_max", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "4194304") != NULL);
    assert(read_virtual_file("/proc/sys/kernel/threads-max", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "65535") != NULL);
    assert(read_virtual_file("/proc/sys/kernel/random/boot_id", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "-") != NULL);
    assert(read_virtual_file("/proc/sys/kernel/random/uuid", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "-") != NULL);

    assert(read_virtual_file("/proc/sys/vm/swappiness", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "60") != NULL);
    assert(read_virtual_file("/proc/sys/vm/overcommit_memory", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);
    assert(read_virtual_file("/proc/sys/vm/max_map_count", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "65530") != NULL);
    assert(read_virtual_file("/proc/sys/vm/min_free_kbytes", buf, sizeof(buf)) > 0);

    assert(read_virtual_file("/proc/sys/fs/file-max", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "1048576") != NULL);
    assert(read_virtual_file("/proc/sys/fs/inode-nr", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "16384") != NULL);

    assert(read_virtual_file("/proc/sys/net/core/somaxconn", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "4096") != NULL);
    assert(read_virtual_file("/proc/sys/net/ipv4/ip_forward", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);
    assert(read_virtual_file("/proc/sys/net/ipv4/ip_local_port_range", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "32768") != NULL);
    assert(read_virtual_file("/proc/sys/net/ipv6/conf/all/forwarding", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_misc_entries_present(void) {
    char templ[] = "/tmp/vproc-procmisc-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char buf[4096];
    assert(read_virtual_file("/proc/partitions", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "#blocks") != NULL);
    assert(strstr(buf, "vda") != NULL);

    assert(read_virtual_file("/proc/locks", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "POSIX") != NULL);

    assert(read_virtual_file("/proc/pressure/cpu", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "some avg10=") != NULL);
    assert(strstr(buf, "full avg10=") != NULL);

    assert(read_virtual_file("/proc/pressure/memory", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "some avg10=") != NULL);

    assert(read_virtual_file("/proc/pressure/io", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "some avg10=") != NULL);

    assert(read_virtual_file("/proc/sysvipc/msg", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "msqid") != NULL);

    assert(read_virtual_file("/proc/sysvipc/sem", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "semid") != NULL);

    assert(read_virtual_file("/proc/sysvipc/shm", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "shmid") != NULL);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_self_entries_present(void) {
    char templ[] = "/tmp/vproc-procself-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char buf[4096];
    assert(read_virtual_file("/proc/self/status", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Name:\t") != NULL);
    assert(strstr(buf, "Pid:\t") != NULL);

    assert(read_virtual_file("/proc/self/limits", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Max open files") != NULL);

    assert(read_virtual_file("/proc/self/io", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "read_bytes:") != NULL);

    assert(read_virtual_file("/proc/self/cgroup", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0::/") != NULL);

    assert(read_virtual_file("/proc/self/sched", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "#threads: 1") != NULL);

    assert(read_virtual_file("/proc/self/schedstat", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0 0 0") != NULL);

    assert(read_virtual_file("/proc/self/stack", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "userspace") != NULL);

    assert(read_virtual_file("/proc/self/oom_score", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);

    assert(read_virtual_file("/proc/self/oom_score_adj", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);

    assert(read_virtual_file("/proc/self/personality", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "00000000") != NULL);

    assert(read_virtual_file("/proc/self/loginuid", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "4294967295") != NULL);

    assert(read_virtual_file("/proc/self/cpuset", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "/") != NULL);

    assert(read_virtual_file("/proc/self/attr/current", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "unconfined") != NULL);

    assert(read_virtual_file("/proc/self/wchan", buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

enum {
    PROC_FD_TEST_PIPE_READ = 214,
    PROC_FD_TEST_PIPE_WRITE = 215,
    PROC_FD_TEST_LATER_SLOT = 216,
    PROC_FD_TEST_SLOT = 217,
    PROC_FD_TEST_HOST = 218,
};

/* /proc/<pid>/fd is that vproc's table: its slots, not the host's descriptors, and
 * from any vproc, as `ls /proc/$$/fd` reads the shell's from a child. */
static void assert_proc_fd_lists_the_vprocs_slots(void) {
    char data[] = "/tmp/vproc-procfd-data-XXXXXX";
    int data_host = mkstemp(data);
    assert(data_host >= 0);
    assert(write(data_host, "abcdef", 6) == 6);
    assert(lseek(data_host, 0, SEEK_SET) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);
    int opened = vprocAdoptHostFd(vp, data_host);
    assert(opened >= 0);
    assert(vprocDup2Shim(opened, PROC_FD_TEST_SLOT) == PROC_FD_TEST_SLOT);
    char two[2];
    assert(vprocReadShim(PROC_FD_TEST_SLOT, two, 2) == 2);
    /* A host descriptor whose number no slot has. */
    assert(dup2(STDERR_FILENO, PROC_FD_TEST_HOST) == PROC_FD_TEST_HOST);

    char templ[] = "/tmp/vproc-procfd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    int fds[1024];
    size_t count = read_numeric_virtual_entries("/proc/self/fd", fds, sizeof(fds) / sizeof(fds[0]));
    assert(int_list_contains(fds, count, STDOUT_FILENO));
    assert(int_list_contains(fds, count, PROC_FD_TEST_SLOT));
    assert(!int_list_contains(fds, count, PROC_FD_TEST_HOST));

    char path[64];
    char buf[256];
    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", PROC_FD_TEST_SLOT);
    assert(read_virtual_file(path, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "pos:\t2\n") != NULL);

    /* A slot opened since the last refresh: only asking its vproc's table from the
     * other vproc can list this one. The sleep is that refresh's 250 ms limit. */
    assert(vprocDup2Shim(opened, PROC_FD_TEST_LATER_SLOT) == PROC_FD_TEST_LATER_SLOT);
    opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *reader = vprocCreate(&opts);
    assert(reader);
    vprocRegisterThread(reader, pthread_self());
    vprocActivate(reader);
    usleep(300000);
    snprintf(path, sizeof(path), "/proc/%d/fd", vprocPid(vp));
    count = read_numeric_virtual_entries(path, fds, sizeof(fds) / sizeof(fds[0]));
    assert(int_list_contains(fds, count, PROC_FD_TEST_LATER_SLOT));
    assert(int_list_contains(fds, count, PROC_FD_TEST_SLOT));
    assert(!int_list_contains(fds, count, PROC_FD_TEST_HOST));
    vprocDeactivate();
    vprocDestroy(reader);

    /* A closed slot leaves the listing, whatever the host has at that number. */
    assert(vprocCloseShim(PROC_FD_TEST_LATER_SLOT) == 0);
    assert(vprocCloseShim(opened) == 0);
    count = read_numeric_virtual_entries("/proc/self/fd", fds, sizeof(fds) / sizeof(fds[0]));
    assert(!int_list_contains(fds, count, PROC_FD_TEST_LATER_SLOT));
    assert(int_list_contains(fds, count, PROC_FD_TEST_SLOT));
    /* An in-process pipe has no host descriptor: its ends' flags are their own. */
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    assert(vprocAdoptPscalFd(vp, PROC_FD_TEST_PIPE_READ, read_end) == 0);
    assert(vprocAdoptPscalFd(vp, PROC_FD_TEST_PIPE_WRITE, write_end) == 0);
    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
    assert(vprocFcntlShim(PROC_FD_TEST_PIPE_READ, F_SETFL, O_NONBLOCK) == 0);

    VProcFdSnapshot table[256];
    ssize_t rows = vprocSnapshotFds(vprocPid(vp), table, 256);
    assert(rows > 0);
    int seen = 0;
    for (ssize_t i = 0; i < rows; ++i) {
        assert(table[i].fd >= 0 && table[i].fd < 256);
        assert(i == 0 || table[i].fd > table[i - 1].fd);
        if (table[i].fd == PROC_FD_TEST_SLOT) {
            seen++;
            assert(table[i].pos == 2);
            assert((table[i].open_flags & O_ACCMODE) == O_RDWR);
        } else if (table[i].fd == PROC_FD_TEST_PIPE_READ) {
            seen++;
            assert(table[i].pos == 0);
            assert(table[i].open_flags == (O_RDONLY | O_NONBLOCK));
        } else if (table[i].fd == PROC_FD_TEST_PIPE_WRITE) {
            seen++;
            assert(table[i].open_flags == O_WRONLY);
        }
    }
    assert(seen == 3);
    assert(vprocCloseShim(PROC_FD_TEST_PIPE_READ) == 0);
    assert(vprocCloseShim(PROC_FD_TEST_PIPE_WRITE) == 0);
    /* Only the numbers below capacity, however many the table has. */
    assert(vprocSnapshotFds(vprocPid(vp), table, 3) <= 3);
    assert(vprocSnapshotFds(-1, table, 256) == -1);
    close(PROC_FD_TEST_HOST);

    vprocDeactivate();
    int gone_pid = vprocPid(vp);
    vprocDestroy(vp);
    assert(vprocSnapshotFds(gone_pid, table, 256) == -1);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
    unlink(data);
}

static void assert_proc_task_entries_present(void) {
    char templ[] = "/tmp/vproc-proctask-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char self_link[64] = {0};
    ssize_t self_n = pscalPathVirtualized_readlink("/proc/self", self_link, sizeof(self_link) - 1);
    assert(self_n > 0);
    self_link[self_n] = '\0';

    char thread_self[128] = {0};
    ssize_t thread_n = pscalPathVirtualized_readlink("/proc/thread-self", thread_self, sizeof(thread_self) - 1);
    assert(thread_n > 0);
    thread_self[thread_n] = '\0';
    assert(strstr(thread_self, "/task/") != NULL);
    assert(strstr(thread_self, self_link) == thread_self);

    char pathbuf[PATH_MAX];
    char buf[4096];

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/comm", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(buf[0] != '\0');
    assert(buf[0] != '\n');

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/status", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Name:\t") != NULL);
    assert(strstr(buf, "Threads:\t1") != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/stat", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strchr(buf, '(') != NULL);
    assert(strchr(buf, ')') != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/sched", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "#threads: 1") != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/schedstat", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0 0 0") != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/io", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "read_bytes:") != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/cgroup", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0::/") != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/wchan", self_link);
    assert(read_virtual_file(pathbuf, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "0") != NULL);

    snprintf(pathbuf, sizeof(pathbuf), "/proc/self/task/%s/cwd", self_link);
    char target[PATH_MAX];
    ssize_t link_n = pscalPathVirtualized_readlink(pathbuf, target, sizeof(target) - 1);
    assert(link_n > 0);
    target[link_n] = '\0';
    assert(strstr(target, "../../") == target);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_device_entries_present(void) {
    char templ[] = "/tmp/vproc-procdevice-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    char self_target[64] = {0};
    ssize_t n = pscalPathVirtualized_readlink("/proc/device/self", self_target, sizeof(self_target) - 1);
    assert(n > 0);
    self_target[n] = '\0';

    int host_pid = getpid();
    char expected[32];
    snprintf(expected, sizeof(expected), "%d", host_pid);
    assert(strcmp(self_target, expected) == 0);

    char status_path[PATH_MAX];
    snprintf(status_path, sizeof(status_path), "/proc/device/%s/status", self_target);
    char buf[4096];
    assert(read_virtual_file(status_path, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Name:\t") != NULL);
    assert(strstr(buf, "Pid:\t") != NULL);

    snprintf(status_path, sizeof(status_path), "/proc/device/%s/io", self_target);
    assert(read_virtual_file(status_path, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "read_bytes:") != NULL);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_device_self_survives_full_snapshot(void) {
    char templ[] = "/tmp/vproc-procdevfull-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    /* The snapshot keeps 256 processes, newest first, so these children fill it
     * before it reaches this one. Each waits on the pipe, so a failed assert
     * below ends them too. */
    int hold[2];
    assert(pipe(hold) == 0);
    pid_t kids[300];
    size_t kid_count = 0;
    while (kid_count < sizeof(kids) / sizeof(kids[0])) {
        pid_t kid = fork();
        if (kid < 0) {
            break;
        }
        if (kid == 0) {
            close(hold[1]);
            char byte;
            (void)read(hold[0], &byte, 1);
            _exit(0);
        }
        kids[kid_count++] = kid;
    }

    int host_pid = getpid();
    char expected[32];
    snprintf(expected, sizeof(expected), "%d", host_pid);
    char self_target[64] = {0};
    ssize_t n = pscalPathVirtualized_readlink("/proc/device/self", self_target, sizeof(self_target) - 1);
    assert(n > 0);
    self_target[n] = '\0';
    assert(strcmp(self_target, expected) == 0);

    char status_path[PATH_MAX];
    snprintf(status_path, sizeof(status_path), "/proc/device/%s/status", self_target);
    char buf[4096];
    assert(read_virtual_file(status_path, buf, sizeof(buf)) > 0);
    assert(strstr(buf, "Name:\t") != NULL);

    int device_pids[1024];
    size_t device_pid_count = read_numeric_virtual_entries("/proc/device",
                                                           device_pids,
                                                           sizeof(device_pids) / sizeof(device_pids[0]));
    assert(int_list_contains(device_pids, device_pid_count, host_pid));
#if defined(__APPLE__)
    /* Only the host's process list can fill it. A machine at its process limit
     * can leave fork short of that, which is no fault of the code under test. */
    if (kid_count >= 256) {
        assert(device_pid_count >= 256);
    }
#endif

    close(hold[0]);
    close(hold[1]);
    for (size_t i = 0; i < kid_count; ++i) {
        while (waitpid(kids[i], NULL, 0) < 0 && errno == EINTR) {
        }
    }

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_pid_namespace_and_device_namespace_separated(void) {
    char templ[] = "/tmp/vproc-procpidns-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    int vproc_pid = vprocPid(vp);
    assert(vproc_pid > 0);

    char buf[256];
    vprocActivate(vp);
    assert(read_virtual_file("/proc/loadavg", buf, sizeof(buf)) > 0);

    int proc_pids[1024];
    size_t proc_pid_count = read_numeric_virtual_entries("/proc", proc_pids, sizeof(proc_pids) / sizeof(proc_pids[0]));
    assert(proc_pid_count > 0);
    assert(int_list_contains(proc_pids, proc_pid_count, vproc_pid));

    int host_pid = getpid();
    char proc_self_target[64] = {0};
    ssize_t proc_self_n = pscalPathVirtualized_readlink("/proc/self",
                                                        proc_self_target,
                                                        sizeof(proc_self_target) - 1);
    assert(proc_self_n > 0);
    proc_self_target[proc_self_n] = '\0';
    char expected_vproc[32];
    snprintf(expected_vproc, sizeof(expected_vproc), "%d", vproc_pid);
    assert(strcmp(proc_self_target, expected_vproc) == 0);

    char self_target[64] = {0};
    ssize_t self_n = pscalPathVirtualized_readlink("/proc/device/self", self_target, sizeof(self_target) - 1);
    assert(self_n > 0);
    self_target[self_n] = '\0';
    char expected[32];
    snprintf(expected, sizeof(expected), "%d", host_pid);
    assert(strcmp(self_target, expected) == 0);

    int device_pids[1024];
    size_t device_pid_count = read_numeric_virtual_entries("/proc/device",
                                                           device_pids,
                                                           sizeof(device_pids) / sizeof(device_pids[0]));
    assert(device_pid_count > 0);
    assert(int_list_contains(device_pids, device_pid_count, host_pid));

    vprocDeactivate();
    vprocDestroy(vp);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_proc_root_listing_latency_regression(void) {
    char templ[] = "/tmp/vproc-proclatency-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    int pids[1024];
    size_t warm_count = read_numeric_virtual_entries("/proc", pids, sizeof(pids) / sizeof(pids[0]));
    assert(warm_count > 0);

    long long total_ms = 0;
    long long max_ms = 0;
    const int iterations = 5;
    for (int i = 0; i < iterations; ++i) {
        long long start_ms = monotonic_ms();
        size_t count = read_numeric_virtual_entries("/proc", pids, sizeof(pids) / sizeof(pids[0]));
        long long elapsed_ms = monotonic_ms() - start_ms;
        assert(count > 0);
        total_ms += elapsed_ms;
        if (elapsed_ms > max_ms) {
            max_ms = elapsed_ms;
        }
        assert(elapsed_ms < 2000);
    }
    long long avg_ms = total_ms / iterations;
    assert(avg_ms < 1000);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_path_virtualized_glob_matches_dynamic_proc_entries(void) {
    char templ[] = "/tmp/vproc-procglob-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    assert(vprocChdirShim("/proc/net") == 0);

    glob_t relative_results;
    memset(&relative_results, 0, sizeof(relative_results));
    int rc = pscalPathVirtualized_glob("net*", 0, NULL, &relative_results);
    assert(rc == 0);
    bool found_relative = false;
    for (size_t i = 0; i < relative_results.gl_pathc; ++i) {
        if (relative_results.gl_pathv[i] &&
            strcmp(relative_results.gl_pathv[i], "netstat") == 0) {
            found_relative = true;
            break;
        }
    }
    assert(found_relative);
    globfree(&relative_results);

    glob_t absolute_results;
    memset(&absolute_results, 0, sizeof(absolute_results));
    rc = pscalPathVirtualized_glob("/proc/net/net*", 0, NULL, &absolute_results);
    assert(rc == 0);
    bool found_absolute = false;
    for (size_t i = 0; i < absolute_results.gl_pathc; ++i) {
        if (absolute_results.gl_pathv[i] &&
            strcmp(absolute_results.gl_pathv[i], "/proc/net/netstat") == 0) {
            found_absolute = true;
            break;
        }
    }
    assert(found_absolute);
    globfree(&absolute_results);

    vprocDeactivate();
    vprocDestroy(vp);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

#if defined(__APPLE__)
static void *assert_setlocale_thread_main(void *arg) {
    (void)arg;
    for (int i = 0; i < 20; ++i) {
        const char *set = vprocSetlocaleShim(LC_CTYPE, "en_US.UTF-8");
        assert(set && strcmp(set, "en_US.UTF-8") == 0);
        assert(strcmp(vprocSetlocaleShim(LC_CTYPE, NULL), "en_US.UTF-8") == 0);
    }
    return NULL;
}

static atomic_bool g_env_capture_stop = false;

static void *assert_env_capture_writer(void *arg) {
    (void)arg;
    while (!atomic_load(&g_env_capture_stop)) {
        assert(vprocSetenvShim("PSCAL_ENV_CAPTURE_TEST", "value", 1) == 0);
        assert(vprocUnsetenvShim("PSCAL_ENV_CAPTURE_TEST") == 0);
    }
    return NULL;
}

/* A capture counted libc's environment and then read it, and an unsetenv between the two gave it a
 * NULL, which ended the app. */
static void assert_env_capture_survives_changes(void) {
    char name[32];
    for (int i = 0; i < 200; ++i) {
        snprintf(name, sizeof(name), "PSCAL_ENV_CAPTURE_%d", i);
        assert(vprocSetenvShim(name, "1", 1) == 0);
    }
    pthread_t writers[4];
    atomic_store(&g_env_capture_stop, false);
    for (int i = 0; i < 4; ++i) {
        assert(vprocHostPthreadCreate(&writers[i], NULL, assert_env_capture_writer, NULL) == 0);
    }
    for (int i = 0; i < 3000; ++i) {
        VProcEnv *env = vprocEnvCapture();
        assert(env);
        vprocEnvRelease(env);
    }
    atomic_store(&g_env_capture_stop, true);
    for (int i = 0; i < 4; ++i) {
        pthread_join(writers[i], NULL);
    }
    /* The copy holds no lock while it is walked, so what walks it can change the environment. */
    VProcEnv *walked = vprocEnvCapture();
    assert(walked);
    size_t seen = 0;
    for (char **entry = vprocEnvEntries(walked); entry && *entry; ++entry) {
        seen++;
        assert(vprocSetenvShim("PSCAL_ENV_CAPTURE_TEST", *entry, 1) == 0);
        assert(vprocUnsetenvShim("PSCAL_ENV_CAPTURE_TEST") == 0);
    }
    assert(seen >= 200);
    vprocEnvRelease(walked);
    for (int i = 0; i < 200; ++i) {
        snprintf(name, sizeof(name), "PSCAL_ENV_CAPTURE_%d", i);
        assert(vprocUnsetenvShim(name) == 0);
    }
}

static void assert_setlocale_is_the_threads_own(void) {
    void *start = vprocLocaleSave();
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, NULL), "C") == 0);

    pthread_t threads[8];
    for (int i = 0; i < 8; ++i) {
        assert(vprocHostPthreadCreate(&threads[i], NULL, assert_setlocale_thread_main, NULL) == 0);
    }
    for (int i = 0; i < 8; ++i) {
        pthread_join(threads[i], NULL);
    }
    /* None of that reached this thread or the whole app's locale. */
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, NULL), "C") == 0);
    assert(strcmp(querylocale(LC_CTYPE_MASK, LC_GLOBAL_LOCALE), "C") == 0);

    /* "" is what the environment says. */
    assert(vprocUnsetenvShim("LC_ALL") == 0);
    assert(vprocUnsetenvShim("LC_CTYPE") == 0);
    assert(vprocSetenvShim("LANG", "en_US.UTF-8", 1) == 0);
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, ""), "en_US.UTF-8") == 0);
    assert(vprocUnsetenvShim("LANG") == 0);

    /* What was saved comes back, past another made and freed inside. */
    void *saved = vprocLocaleSave();
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, "C"), "C") == 0);
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, "en_US.ISO8859-1"), "en_US.ISO8859-1") == 0);
    vprocLocaleRestore(saved);
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, NULL), "en_US.UTF-8") == 0);

    /* A name that isn't there changes nothing. */
    assert(vprocSetlocaleShim(LC_CTYPE, "nosuch.UTF-8") == NULL);
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, NULL), "en_US.UTF-8") == 0);

    vprocLocaleRestore(start);
    assert(strcmp(vprocSetlocaleShim(LC_CTYPE, NULL), "C") == 0);
}
#endif

/* Each eval or sourced file runs under a vproc stacked on the shell's thread, and that stack has
 * no limit. One of 16 dropped what came after, and on the way out the thread ended with no vproc:
 * paths and descriptors then went straight to the host's. */
static void assert_vproc_stack_has_no_limit(void) {
    /* Each holds descriptors, and a process may have 256: past the first spill is enough here. */
    enum { kDepth = 40 };
    char templ[] = "/tmp/vproc-stack-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    int prev_shell = vprocGetShellSelfPid();
    VProc *below = vprocCurrent();

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/moved", root);
    assert(mkdir(path, 0700) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *shell_vp = vprocCreate(&opts);
    assert(shell_vp);
    vprocSetShellSelfPid(vprocPid(shell_vp));
    vprocActivate(shell_vp);

    VProc *stacked[kDepth];
    mode_t mask = vprocShellUmaskShim(027);
    for (int i = 0; i < kDepth; ++i) {
        opts = vprocDefaultOptions();
        opts.pid_hint = vprocReservePid();
        stacked[i] = vprocCreate(&opts);
        assert(stacked[i]);
        vprocActivate(stacked[i]);
        assert(vprocCurrent() == stacked[i]);
    }

    /* cd and umask at the top reach every level below it. */
    assert(vprocShellChdirShim("/moved") == 0);
    assert(vprocShellUmaskShim(022) == 027);
    for (int i = kDepth - 1; i >= 0; --i) {
        char cwd[PATH_MAX];
        assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
        assert(strcmp(cwd, "/moved") == 0);
        vprocDeactivate();
        assert(vprocCurrent() == (i > 0 ? stacked[i - 1] : shell_vp));
        vprocDestroy(stacked[i]);
    }

    /* Down a long way and back a few times, so the block is made, grown and freed again. */
    for (int round = 0; round < 3; ++round) {
        for (int i = 0; i < 1000; ++i) {
            vprocActivate(shell_vp);
        }
        for (int i = 0; i < 1000; ++i) {
            assert(vprocCurrent() == shell_vp);
            vprocDeactivate();
        }
    }
    assert(vprocCurrent() == shell_vp);

    /* A vproc destroyed while below the top leaves an empty place, not a dangling one. */
    opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *deep = vprocCreate(&opts);
    assert(deep);
    for (int i = 0; i < 20; ++i) {
        vprocActivate(i == 0 ? deep : shell_vp);
    }
    vprocActivate(shell_vp);
    vprocDestroy(deep);
    for (int i = 0; i < 21; ++i) {
        vprocDeactivate();
    }
    assert(vprocCurrent() == shell_vp);

    vprocShellUmaskShim(mask);
    vprocDeactivate();
    assert(vprocCurrent() == below);
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_empty_path_names_nothing(void) {
    char templ[] = "/tmp/vproc-empty-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/empty", 0700) == 0 || errno == EEXIST);

    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    assert(vprocChdirShim("/tmp/empty") == 0);

    struct stat st;
    char link[16];
    errno = 0;
    assert(vprocStatShim("", &st) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocLstatShim("", &st) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocAccessShim("", F_OK) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocOpenShim("", O_RDONLY) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocOpendirShim("") == NULL && errno == ENOENT);
    errno = 0;
    assert(vprocMkdirShim("", 0700) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocRmdirShim("") == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocUnlinkShim("") == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocRenameShim("", "moved") == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocSymlinkShim("target", "") == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocReadlinkShim("", link, sizeof(link)) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocChdirShim("") == -1 && errno == ENOENT);

    char cwd[PATH_MAX];
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/empty") == 0);

    vprocDeactivate();
    vprocDestroy(vp);

    /* The same names reached without a vproc. */
    glob_t matches;
    memset(&matches, 0, sizeof(matches));
    errno = 0;
    assert(pscalPathVirtualized_lstat("", &st) == -1 && errno == ENOENT);
    errno = 0;
    assert(pscalPathVirtualized_opendir("") == NULL && errno == ENOENT);
    errno = 0;
    assert(pscalPathVirtualized_rmdir("") == -1 && errno == ENOENT);
    assert(pscalPathVirtualized_glob("", 0, NULL, &matches) == GLOB_NOMATCH);

    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_vproc_cwd_isolated_between_vprocs(void) {
    char templ[] = "/tmp/vproc-cwd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-b", 0700) == 0 || errno == EEXIST);

    VProc *vp_a = vprocCreate(NULL);
    VProc *vp_b = vprocCreate(NULL);
    assert(vp_a);
    assert(vp_b);

    vprocActivate(vp_a);
    assert(vprocChdirShim("/tmp/cwd-a") == 0);
    char cwd_a[PATH_MAX];
    assert(vprocGetcwdShim(cwd_a, sizeof(cwd_a)) != NULL);
    assert(strcmp(cwd_a, "/tmp/cwd-a") == 0);
    int fd_a = vprocOpenShim("a.txt", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd_a >= 0);
    assert(vprocWriteShim(fd_a, "a", 1) == 1);
    assert(vprocCloseShim(fd_a) == 0);
    vprocDeactivate();

    vprocActivate(vp_b);
    assert(vprocChdirShim("/tmp/cwd-b") == 0);
    char cwd_b[PATH_MAX];
    assert(vprocGetcwdShim(cwd_b, sizeof(cwd_b)) != NULL);
    assert(strcmp(cwd_b, "/tmp/cwd-b") == 0);
    int fd_b = vprocOpenShim("b.txt", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(fd_b >= 0);
    assert(vprocWriteShim(fd_b, "b", 1) == 1);
    assert(vprocCloseShim(fd_b) == 0);
    vprocDeactivate();

    vprocActivate(vp_a);
    assert(vprocGetcwdShim(cwd_a, sizeof(cwd_a)) != NULL);
    assert(strcmp(cwd_a, "/tmp/cwd-a") == 0);
    vprocDeactivate();

    struct stat st;
    assert(pscalPathVirtualized_stat("/tmp/cwd-a/a.txt", &st) == 0);
    assert(pscalPathVirtualized_stat("/tmp/cwd-b/b.txt", &st) == 0);
    assert(pscalPathVirtualized_stat("/tmp/cwd-b/a.txt", &st) != 0);
    assert(pscalPathVirtualized_stat("/tmp/cwd-a/b.txt", &st) != 0);

    vprocDestroy(vp_a);
    vprocDestroy(vp_b);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
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

static void assert_virtual_control_signals_do_not_hit_host_process(void) {
#if defined(PSCAL_TARGET_IOS)
    struct sigaction old_sigint;
    struct sigaction old_sigtstp;
    struct sigaction trap;
    memset(&trap, 0, sizeof(trap));
    sigemptyset(&trap.sa_mask);

    trap.sa_handler = hostSigintTrapHandler;
    assert(sigaction(SIGINT, &trap, &old_sigint) == 0);
    trap.sa_handler = hostSigtstpTrapHandler;
    assert(sigaction(SIGTSTP, &trap, &old_sigtstp) == 0);

    /* Run this regression before any test seeds shell identity; we need
     * no virtual context so old code would fall back to host kill(). */
    int prev_shell = vprocGetShellSelfPid();
    assert(prev_shell <= 0);

    gHostSigintTrapCount = 0;
    gHostSigtstpTrapCount = 0;

    errno = 0;
    int rc_int = vprocKillShim(0, SIGINT);
    int int_errno = errno;
    errno = 0;
    int rc_tstp = vprocKillShim(0, SIGTSTP);
    int tstp_errno = errno;

    assert((rc_int == 0) || (rc_int == -1 && int_errno == ESRCH));
    assert((rc_tstp == 0) || (rc_tstp == -1 && tstp_errno == ESRCH));
    assert(gHostSigintTrapCount == 0);
    assert(gHostSigtstpTrapCount == 0);

    assert(sigaction(SIGINT, &old_sigint, NULL) == 0);
    assert(sigaction(SIGTSTP, &old_sigtstp, NULL) == 0);
#endif
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

static void assert_select_rejects_oversize_fdset(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    fd_set rfds;
    FD_ZERO(&rfds);
    errno = 0;
    int rc = vprocSelectShim(FD_SETSIZE + 1, &rfds, NULL, NULL, NULL);
    assert(rc == -1);
    assert(errno == EINVAL);

    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_select_rejects_invalid_timeval(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    fd_set rfds;
    FD_ZERO(&rfds);

    struct timeval bad_neg = {.tv_sec = -1, .tv_usec = 0};
    errno = 0;
    assert(vprocSelectShim(0, &rfds, NULL, NULL, &bad_neg) == -1);
    assert(errno == EINVAL);

    struct timeval bad_usec = {.tv_sec = 0, .tv_usec = 1000000};
    errno = 0;
    assert(vprocSelectShim(0, &rfds, NULL, NULL, &bad_usec) == -1);
    assert(errno == EINVAL);

    vprocDeactivate();
    vprocDestroy(vp);
}

static void assert_fcntl_on_location_device(void) {
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocRegisterThread(vp, pthread_self());
    vprocActivate(vp);

    int fd = vprocOpenShim("/dev/location", O_RDONLY);
    assert(fd >= 0);
    assert(vprocFcntlShim(fd, F_GETFL) == O_RDONLY);
    assert(vprocFcntlShim(fd, F_SETFL, O_NONBLOCK) == 0);
    assert(vprocFcntlShim(fd, F_GETFL) == (O_RDONLY | O_NONBLOCK));
    char buf[16] = {0};
    errno = 0;
    assert(vprocReadShim(fd, buf, sizeof(buf)) == -1 && errno == EAGAIN);

    const char *payload = "nonblock\n";
    assert(vprocLocationDeviceWrite(payload, strlen(payload)) == (ssize_t)strlen(payload));
    assert(vprocReadShim(fd, buf, sizeof(buf)) == (ssize_t)strlen(payload));
    assert(strcmp(buf, payload) == 0);

    assert(vprocCloseShim(fd) == 0);
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

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    unsigned char buf[64];
    size_t len;
} session_output_capture;

static void session_output_capture_handler(uint64_t session_id,
                                           const unsigned char *data,
                                           size_t len,
                                           void *context) {
    (void)session_id;
    session_output_capture *capture = (session_output_capture *)context;
    if (!capture || !data || len == 0) {
        return;
    }
    pthread_mutex_lock(&capture->mu);
    size_t room = sizeof(capture->buf) - capture->len;
    if (room > 0) {
        size_t n = len < room ? len : room;
        memcpy(capture->buf + capture->len, data, n);
        capture->len += n;
    }
    pthread_cond_broadcast(&capture->cv);
    pthread_mutex_unlock(&capture->mu);
}

static bool session_output_capture_wait_len(session_output_capture *capture, size_t needed, int timeout_ms) {
    if (!capture) {
        return false;
    }
    pthread_mutex_lock(&capture->mu);
    int remaining = timeout_ms;
    while (capture->len < needed && remaining > 0) {
        struct timespec now;
        assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
        int slice_ms = remaining > 10 ? 10 : remaining;
        struct timespec deadline = now;
        deadline.tv_nsec += (long)slice_ms * 1000L * 1000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        (void)pthread_cond_timedwait(&capture->cv, &capture->mu, &deadline);
        remaining -= slice_ms;
    }
    bool ok = (capture->len >= needed);
    pthread_mutex_unlock(&capture->mu);
    return ok;
}

static bool buffer_contains_token(const unsigned char *buf, size_t len, const char *token, size_t token_len) {
    if (!buf || !token || token_len == 0 || len < token_len) {
        return false;
    }
    for (size_t i = 0; i + token_len <= len; ++i) {
        if (memcmp(buf + i, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}

static void assert_session_output_handler_delayed_attach_receives_pending_output(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);

    uint64_t session_id = 4321;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);

    const char *msg = "late";
    assert(slave->ops && slave->ops->write);
    assert(slave->ops->write(slave, msg, 4) == 4);

    /* Attach handler after write to exercise delayed dispatch retry path. */
    usleep(50000);

    session_output_capture capture;
    memset(&capture, 0, sizeof(capture));
    pthread_mutex_init(&capture.mu, NULL);
    pthread_cond_init(&capture.cv, NULL);
    vprocSessionSetOutputHandler(session_id, session_output_capture_handler, &capture);

    assert(session_output_capture_wait_len(&capture, 4, 1000));
    pthread_mutex_lock(&capture.mu);
    assert(memcmp(capture.buf, msg, 4) == 0);
    pthread_mutex_unlock(&capture.mu);

    vprocSessionClearOutputHandler(session_id);
    pthread_cond_destroy(&capture.cv);
    pthread_mutex_destroy(&capture.mu);
    vprocSessionStdioDestroy(session);
}

static void assert_session_output_handler_burst_tabs(void) {
    enum { kSessions = 5 };
    VProcSessionStdio *sessions[kSessions];
    session_output_capture captures[kSessions];
    uint64_t session_ids[kSessions];
    char messages[kSessions][8];
    memset(sessions, 0, sizeof(sessions));
    memset(captures, 0, sizeof(captures));
    memset(session_ids, 0, sizeof(session_ids));
    memset(messages, 0, sizeof(messages));

    for (int i = 0; i < kSessions; ++i) {
        struct pscal_fd *master = NULL;
        struct pscal_fd *slave = NULL;
        int pty_num = -1;
        assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
        assert(pscalPtyUnlock(master) == 0);
        assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);

        uint64_t session_id = 6000 + (uint64_t)i;
        VProcSessionStdio *session = vprocSessionStdioCreate();
        assert(session);
        assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
        vprocSessionStdioActivate(session);

        assert(pthread_mutex_init(&captures[i].mu, NULL) == 0);
        assert(pthread_cond_init(&captures[i].cv, NULL) == 0);
        sessions[i] = session;
        session_ids[i] = session_id;

        snprintf(messages[i], sizeof(messages[i]), "TB%d", i);
        assert(slave->ops && slave->ops->write);
        assert(slave->ops->write(slave, messages[i], strlen(messages[i])) == (ssize_t)strlen(messages[i]));
    }

    /* Simulate burst startup where output appears before each tab's handler attaches. */
    usleep(70000);

    for (int i = 0; i < kSessions; ++i) {
        vprocSessionSetOutputHandler(session_ids[i], session_output_capture_handler, &captures[i]);
    }

    for (int i = 0; i < kSessions; ++i) {
        size_t msg_len = strlen(messages[i]);
        assert(session_output_capture_wait_len(&captures[i], msg_len, 1200));
        pthread_mutex_lock(&captures[i].mu);
        assert(buffer_contains_token(captures[i].buf, captures[i].len, messages[i], msg_len));
        pthread_mutex_unlock(&captures[i].mu);
    }

    for (int i = 0; i < kSessions; ++i) {
        vprocSessionClearOutputHandler(session_ids[i]);
        pthread_cond_destroy(&captures[i].cv);
        pthread_mutex_destroy(&captures[i].mu);
        vprocSessionStdioDestroy(sessions[i]);
    }
}

static void assert_session_output_pause_resume_flushes_backlog(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);

    uint64_t session_id = 7654;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);

    session_output_capture capture;
    memset(&capture, 0, sizeof(capture));
    pthread_mutex_init(&capture.mu, NULL);
    pthread_cond_init(&capture.cv, NULL);
    vprocSessionSetOutputHandler(session_id, session_output_capture_handler, &capture);
    vprocSessionSetOutputPaused(session_id, true);

    const char *msg = "pause";
    assert(slave->ops && slave->ops->write);
    assert(slave->ops->write(slave, msg, 5) == 5);
    usleep(50000);
    pthread_mutex_lock(&capture.mu);
    assert(capture.len == 0);
    pthread_mutex_unlock(&capture.mu);

    vprocSessionSetOutputPaused(session_id, false);
    assert(session_output_capture_wait_len(&capture, 5, 1000));
    pthread_mutex_lock(&capture.mu);
    assert(memcmp(capture.buf, msg, 5) == 0);
    pthread_mutex_unlock(&capture.mu);

    vprocSessionClearOutputHandler(session_id);
    pthread_cond_destroy(&capture.cv);
    pthread_mutex_destroy(&capture.mu);
    vprocSessionStdioDestroy(session);
}

static void assert_session_write_to_master_nonblocking_respects_capacity(void) {
    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);

    /* In line mode what does not fit in a line is dropped, as on any terminal. */
    struct termios_ termios;
    assert(slave->ops->ioctl(slave, TCGETS_, &termios) == 0);
    termios.lflags &= ~ICANON_;
    assert(slave->ops->ioctl(slave, TCSETS_, &termios) == 0);

    uint64_t session_id = 2468;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);

    char big[16384];
    memset(big, 'x', sizeof(big));

    ssize_t first = vprocSessionWriteToMasterMode(session_id, big, sizeof(big), false);
    assert(first > 0);
    assert((size_t)first < sizeof(big));

    errno = 0;
    ssize_t second = vprocSessionWriteToMasterMode(session_id, big, sizeof(big), false);
    assert(second == -1);
    assert(errno == EAGAIN);

    vprocSessionStdioDestroy(session);
}

static void assert_session_input_inject_read_queue(void) {
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);

    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
        session->stdin_host_fd = -1;
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
        session->stdout_host_fd = -1;
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
        session->stderr_host_fd = -1;
    }

    vprocSessionStdioActivate(session);

    const char payload1[] = "abcdef";
    const char payload2[] = "ghij";
    assert(vprocSessionInjectInputShim(payload1, sizeof(payload1) - 1));
    assert(vprocSessionInjectInputShim(payload2, sizeof(payload2) - 1));

    char buf[16] = {0};
    assert(vprocSessionReadInputShimMode(buf, 3, true) == 3);
    assert(memcmp(buf, "abc", 3) == 0);
    memset(buf, 0, sizeof(buf));
    assert(vprocSessionReadInputShimMode(buf, 4, true) == 4);
    assert(memcmp(buf, "defg", 4) == 0);
    memset(buf, 0, sizeof(buf));
    assert(vprocSessionReadInputShimMode(buf, 3, true) == 3);
    assert(memcmp(buf, "hij", 3) == 0);

    memset(buf, 0, sizeof(buf));
    errno = 0;
    assert(vprocSessionReadInputShimMode(buf, 1, true) == -1);
    assert(errno == EAGAIN);

    vprocSessionStdioDestroy(session);
}

static bool session_input_wait_len(VProcSessionInput *input, size_t needed, int timeout_ms) {
    if (!input) {
        return false;
    }
    int waited_ms = 0;
    while (waited_ms <= timeout_ms) {
        pthread_mutex_lock(&input->mu);
        size_t len = input->len;
        pthread_mutex_unlock(&input->mu);
        if (len >= needed) {
            return true;
        }
        usleep(5000);
        waited_ms += 5;
    }
    return false;
}

static bool session_input_wait_interrupt(VProcSessionInput *input, int timeout_ms) {
    if (!input) {
        return false;
    }
    int waited_ms = 0;
    while (waited_ms <= timeout_ms) {
        pthread_mutex_lock(&input->mu);
        bool pending = input->interrupt_pending;
        pthread_mutex_unlock(&input->mu);
        if (pending) {
            return true;
        }
        usleep(5000);
        waited_ms += 5;
    }
    return false;
}

static void session_input_clear_interrupt(VProcSessionInput *input) {
    if (!input) {
        return;
    }
    pthread_mutex_lock(&input->mu);
    input->interrupt_pending = false;
    pthread_mutex_unlock(&input->mu);
}

static void session_input_stop_reader(VProcSessionInput *input) {
    if (!input) {
        return;
    }
    pthread_mutex_lock(&input->mu);
    input->stop_requested = true;
    pthread_cond_broadcast(&input->cv);
    while (input->reader_active) {
        pthread_cond_wait(&input->cv, &input->mu);
    }
    pthread_mutex_unlock(&input->mu);
}

typedef struct {
    pthread_mutex_t mu;
    const unsigned char *payload;
    size_t payload_len;
    size_t payload_off;
    int leading_zero_reads;
} transient_input_state;

static ssize_t transient_input_read(struct pscal_fd *fd, void *buf, size_t bufsize) {
    transient_input_state *state = (transient_input_state *)fd->userdata;
    if (!state || !buf || bufsize == 0) {
        return 0;
    }
    pthread_mutex_lock(&state->mu);
    if (state->leading_zero_reads > 0) {
        state->leading_zero_reads--;
        pthread_mutex_unlock(&state->mu);
        return 0;
    }
    if (state->payload_off < state->payload_len) {
        ((unsigned char *)buf)[0] = state->payload[state->payload_off++];
        pthread_mutex_unlock(&state->mu);
        return 1;
    }
    pthread_mutex_unlock(&state->mu);
    return _EAGAIN;
}

static ssize_t transient_input_write(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    (void)fd;
    (void)buf;
    (void)bufsize;
    return _EIO;
}

static int transient_input_poll(struct pscal_fd *fd) {
    (void)fd;
    return 0;
}

static ssize_t transient_input_ioctl_size(int cmd) {
    (void)cmd;
    return 0;
}

static int transient_input_ioctl(struct pscal_fd *fd, int cmd, void *arg) {
    (void)fd;
    (void)cmd;
    (void)arg;
    return _ENOTTY;
}

static int transient_input_close(struct pscal_fd *fd) {
    transient_input_state *state = (transient_input_state *)fd->userdata;
    if (state) {
        pthread_mutex_destroy(&state->mu);
        free(state);
        fd->userdata = NULL;
    }
    return 0;
}

static const struct pscal_fd_ops gTransientInputOps = {
    .read = transient_input_read,
    .write = transient_input_write,
    .poll = transient_input_poll,
    .ioctl_size = transient_input_ioctl_size,
    .ioctl = transient_input_ioctl,
    .close = transient_input_close,
};

static struct pscal_fd *create_transient_input_fd(const unsigned char *payload,
                                                  size_t payload_len,
                                                  int leading_zero_reads) {
    transient_input_state *state =
            (transient_input_state *)calloc(1, sizeof(transient_input_state));
    assert(state);
    assert(pthread_mutex_init(&state->mu, NULL) == 0);
    state->payload = payload;
    state->payload_len = payload_len;
    state->payload_off = 0;
    state->leading_zero_reads = leading_zero_reads;

    struct pscal_fd *fd = pscal_fd_create(&gTransientInputOps);
    assert(fd);
    fd->userdata = state;
    return fd;
}

typedef struct {
    pthread_mutex_t mu;
    unsigned char buf[128];
    size_t off;
    size_t len;
    int leading_zero_reads;
    int transient_eio_after_bytes;
    bool eio_emitted;
    size_t bytes_delivered;
} interactive_input_state;

static ssize_t interactive_input_read(struct pscal_fd *fd, void *buf, size_t bufsize) {
    interactive_input_state *state = (interactive_input_state *)fd->userdata;
    if (!state || !buf || bufsize == 0) {
        return _EINVAL;
    }
    pthread_mutex_lock(&state->mu);
    if (state->leading_zero_reads > 0) {
        state->leading_zero_reads--;
        pthread_mutex_unlock(&state->mu);
        return 0;
    }
    if (!state->eio_emitted &&
        state->transient_eio_after_bytes >= 0 &&
        state->bytes_delivered >= (size_t)state->transient_eio_after_bytes) {
        state->eio_emitted = true;
        pthread_mutex_unlock(&state->mu);
        return _EIO;
    }
    if (state->len == 0) {
        pthread_mutex_unlock(&state->mu);
        return _EAGAIN;
    }
    ((unsigned char *)buf)[0] = state->buf[state->off];
    state->off++;
    state->len--;
    if (state->len == 0) {
        state->off = 0;
    }
    state->bytes_delivered++;
    pthread_mutex_unlock(&state->mu);
    return 1;
}

static ssize_t interactive_input_write(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    (void)fd;
    (void)buf;
    (void)bufsize;
    return _EIO;
}

static int interactive_input_poll(struct pscal_fd *fd) {
    (void)fd;
    return 0;
}

static ssize_t interactive_input_ioctl_size(int cmd) {
    (void)cmd;
    return 0;
}

static int interactive_input_ioctl(struct pscal_fd *fd, int cmd, void *arg) {
    (void)fd;
    (void)cmd;
    (void)arg;
    return _ENOTTY;
}

static int interactive_input_close(struct pscal_fd *fd) {
    interactive_input_state *state = (interactive_input_state *)fd->userdata;
    if (state) {
        pthread_mutex_destroy(&state->mu);
        free(state);
        fd->userdata = NULL;
    }
    return 0;
}

static const struct pscal_fd_ops gInteractiveInputOps = {
    .read = interactive_input_read,
    .write = interactive_input_write,
    .poll = interactive_input_poll,
    .ioctl_size = interactive_input_ioctl_size,
    .ioctl = interactive_input_ioctl,
    .close = interactive_input_close,
};

static struct pscal_fd *create_interactive_input_fd(int leading_zero_reads,
                                                    int transient_eio_after_bytes) {
    interactive_input_state *state =
            (interactive_input_state *)calloc(1, sizeof(interactive_input_state));
    assert(state);
    assert(pthread_mutex_init(&state->mu, NULL) == 0);
    state->off = 0;
    state->len = 0;
    state->leading_zero_reads = leading_zero_reads;
    state->transient_eio_after_bytes = transient_eio_after_bytes;
    state->eio_emitted = false;
    state->bytes_delivered = 0;

    struct pscal_fd *fd = pscal_fd_create(&gInteractiveInputOps);
    assert(fd);
    fd->userdata = state;
    return fd;
}

static void interactive_input_push(struct pscal_fd *fd, const unsigned char *data, size_t data_len) {
    interactive_input_state *state = fd ? (interactive_input_state *)fd->userdata : NULL;
    assert(state);
    assert(data || data_len == 0);
    pthread_mutex_lock(&state->mu);
    if (state->off > 0 && state->len > 0) {
        memmove(state->buf, state->buf + state->off, state->len);
        state->off = 0;
    } else if (state->len == 0) {
        state->off = 0;
    }
    assert(data_len <= sizeof(state->buf) - state->len);
    if (data_len > 0) {
        memcpy(state->buf + state->len, data, data_len);
        state->len += data_len;
    }
    pthread_mutex_unlock(&state->mu);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool started;
    bool done;
    int rc;
    char pass[64];
} scp_like_prompt_ctx;

static void *scp_like_prompt_thread(void *arg) {
    scp_like_prompt_ctx *ctx = (scp_like_prompt_ctx *)arg;
    pthread_mutex_lock(&ctx->mu);
    ctx->started = true;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);

    int rc = -1;
    char out[sizeof(ctx->pass)];
    size_t len = 0;
    memset(out, 0, sizeof(out));
    while (len + 1 < sizeof(out)) {
        char ch = '\0';
        ssize_t rd = vprocSessionReadInputShim(&ch, 1);
        if (rd <= 0) {
            rc = -1;
            break;
        }
        if (ch == '\n' || ch == '\r') {
            out[len] = '\0';
            rc = 0;
            break;
        }
        out[len++] = ch;
    }
    pthread_mutex_lock(&ctx->mu);
    if (rc == 0) {
        memcpy(ctx->pass, out, sizeof(ctx->pass));
    } else {
        ctx->pass[0] = '\0';
    }
    ctx->rc = rc;
    ctx->done = true;
    pthread_cond_broadcast(&ctx->cv);
    pthread_mutex_unlock(&ctx->mu);
    return NULL;
}

static bool scp_like_prompt_wait_started(scp_like_prompt_ctx *ctx, int timeout_ms) {
    int waited_ms = 0;
    while (waited_ms <= timeout_ms) {
        pthread_mutex_lock(&ctx->mu);
        bool started = ctx->started;
        pthread_mutex_unlock(&ctx->mu);
        if (started) {
            return true;
        }
        usleep(5000);
        waited_ms += 5;
    }
    return false;
}

static bool scp_like_prompt_wait_done(scp_like_prompt_ctx *ctx, int timeout_ms) {
    int waited_ms = 0;
    while (waited_ms <= timeout_ms) {
        pthread_mutex_lock(&ctx->mu);
        bool done = ctx->done;
        pthread_mutex_unlock(&ctx->mu);
        if (done) {
            return true;
        }
        usleep(5000);
        waited_ms += 5;
    }
    return false;
}

static bool scp_like_prompt_is_done(scp_like_prompt_ctx *ctx) {
    pthread_mutex_lock(&ctx->mu);
    bool done = ctx->done;
    pthread_mutex_unlock(&ctx->mu);
    return done;
}

static void assert_session_input_reader_ignores_transient_pty_zero_read(void) {
    static const unsigned char payload[] = {'p', 'w'};
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);

    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
        session->stdin_host_fd = -1;
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
        session->stdout_host_fd = -1;
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
        session->stderr_host_fd = -1;
    }

    struct pscal_fd *stdin_fd = create_transient_input_fd(payload, sizeof(payload), 1);
    session->stdin_pscal_fd = stdin_fd;
    session->pty_slave = stdin_fd;
    session->pty_active = true;

    vprocSessionStdioActivate(session);
    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    assert(session_input_wait_len(input, sizeof(payload), 1000));

    unsigned char buf[4] = {0};
    assert(vprocSessionReadInputShimMode(buf, sizeof(payload), true) == (ssize_t)sizeof(payload));
    assert(memcmp(buf, payload, sizeof(payload)) == 0);

    memset(buf, 0, sizeof(buf));
    errno = 0;
    assert(vprocSessionReadInputShimMode(buf, 1, true) == -1);
    assert(errno == EAGAIN);

    pthread_mutex_lock(&input->mu);
    assert(!input->eof);
    pthread_mutex_unlock(&input->mu);

    session_input_stop_reader(input);
    vprocSessionStdioDestroy(session);
}

static void assert_scp_like_prompt_survives_first_character_input(void) {
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);

    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
        session->stdin_host_fd = -1;
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
        session->stdout_host_fd = -1;
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
        session->stderr_host_fd = -1;
    }

    struct pscal_fd *stdin_fd = create_interactive_input_fd(1, -1);
    session->stdin_pscal_fd = stdin_fd;
    session->stdout_pscal_fd = pscal_fd_retain(stdin_fd);
    session->stderr_pscal_fd = pscal_fd_retain(stdin_fd);
    session->pty_slave = stdin_fd;
    session->pty_active = true;

    vprocSessionStdioActivate(session);
    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    VProcOptions shell_opts = vprocDefaultOptions();
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    int shell_pid = vprocPid(shell_vp);
    vprocSetShellSelfPid(shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    VProcOptions child_opts = vprocDefaultOptions();
    VProc *child_vp = vprocCreate(&child_opts);
    assert(child_vp);
    vprocSetParent(vprocPid(child_vp), shell_pid);
    assert(vprocSetSid(vprocPid(child_vp), shell_pid) == 0);
    assert(vprocSetPgid(vprocPid(child_vp), vprocPid(child_vp)) == 0);

    scp_like_prompt_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert(pthread_mutex_init(&ctx.mu, NULL) == 0);
    assert(pthread_cond_init(&ctx.cv, NULL) == 0);
    ctx.started = false;
    ctx.done = false;
    ctx.rc = -1;

    pthread_t prompt_thread = 0;
    assert(vprocSpawnThread(child_vp, scp_like_prompt_thread, &ctx, &prompt_thread) == 0);

    /* Ensure prompt loop is running and still alive before any typed input. */
    assert(scp_like_prompt_wait_started(&ctx, 500));
    usleep(50000);
    assert(!scp_like_prompt_is_done(&ctx));

    /* First keypress must not terminate prompt handling. */
    static const unsigned char first[] = {'s'};
    interactive_input_push(stdin_fd, first, sizeof(first));
    usleep(50000);
    assert(!scp_like_prompt_is_done(&ctx));

    static const unsigned char rest[] = {'e', 'c', 'r', 'e', 't', '\n'};
    interactive_input_push(stdin_fd, rest, sizeof(rest));
    assert(scp_like_prompt_wait_done(&ctx, 1500));

    pthread_join(prompt_thread, NULL);
    pthread_mutex_lock(&ctx.mu);
    assert(ctx.rc == 0);
    assert(strcmp(ctx.pass, "secret") == 0);
    pthread_mutex_unlock(&ctx.mu);

    pthread_cond_destroy(&ctx.cv);
    pthread_mutex_destroy(&ctx.mu);
    vprocDestroy(child_vp);
    vprocDestroy(shell_vp);
    session_input_stop_reader(input);
    vprocSessionStdioDestroy(session);
}

static void assert_scp_like_prompt_survives_transient_eio_after_first_character(void) {
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);

    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
        session->stdin_host_fd = -1;
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
        session->stdout_host_fd = -1;
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
        session->stderr_host_fd = -1;
    }

    struct pscal_fd *stdin_fd = create_interactive_input_fd(1, 1);
    session->stdin_pscal_fd = stdin_fd;
    session->stdout_pscal_fd = pscal_fd_retain(stdin_fd);
    session->stderr_pscal_fd = pscal_fd_retain(stdin_fd);
    session->pty_slave = stdin_fd;
    session->pty_active = true;

    vprocSessionStdioActivate(session);
    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    VProcOptions shell_opts = vprocDefaultOptions();
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    int shell_pid = vprocPid(shell_vp);
    vprocSetShellSelfPid(shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    VProcOptions child_opts = vprocDefaultOptions();
    VProc *child_vp = vprocCreate(&child_opts);
    assert(child_vp);
    vprocSetParent(vprocPid(child_vp), shell_pid);
    assert(vprocSetSid(vprocPid(child_vp), shell_pid) == 0);
    assert(vprocSetPgid(vprocPid(child_vp), vprocPid(child_vp)) == 0);

    scp_like_prompt_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert(pthread_mutex_init(&ctx.mu, NULL) == 0);
    assert(pthread_cond_init(&ctx.cv, NULL) == 0);
    ctx.started = false;
    ctx.done = false;
    ctx.rc = -1;

    pthread_t prompt_thread = 0;
    assert(vprocSpawnThread(child_vp, scp_like_prompt_thread, &ctx, &prompt_thread) == 0);

    assert(scp_like_prompt_wait_started(&ctx, 500));
    usleep(50000);
    assert(!scp_like_prompt_is_done(&ctx));

    static const unsigned char first[] = {'s'};
    interactive_input_push(stdin_fd, first, sizeof(first));
    usleep(50000);
    assert(!scp_like_prompt_is_done(&ctx));

    static const unsigned char rest[] = {'e', 'c', 'r', 'e', 't', '\n'};
    interactive_input_push(stdin_fd, rest, sizeof(rest));
    assert(scp_like_prompt_wait_done(&ctx, 1500));

    pthread_join(prompt_thread, NULL);
    pthread_mutex_lock(&ctx.mu);
    assert(ctx.rc == 0);
    assert(strcmp(ctx.pass, "secret") == 0);
    pthread_mutex_unlock(&ctx.mu);

    pthread_cond_destroy(&ctx.cv);
    pthread_mutex_destroy(&ctx.mu);
    vprocDestroy(child_vp);
    vprocDestroy(shell_vp);
    session_input_stop_reader(input);
    vprocSessionStdioDestroy(session);
}

static VProcSessionStdio *session_create_with_host_stdin(int stdin_host_fd) {
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
    }
    session->stdin_host_fd = stdin_host_fd;
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    return session;
}

static void assert_session_control_chars_route_to_shell_input_when_shell_foreground(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
    }
    session->stdin_host_fd = host_pipe[0];
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);
    vprocSetShellPromptReadActive(shell_pid, true);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;
    unsigned char controls[2] = { 3, 26 };
    assert(vprocHostWrite(host_pipe[1], controls, sizeof(controls)) == (ssize_t)sizeof(controls));
    assert(session_input_wait_len(input, 2, 500));

    pthread_mutex_lock(&input->mu);
    assert(!input->interrupt_pending);
    assert(input->len >= 2);
    assert(input->buf[input->off] == 3);
    assert(input->buf[input->off + 1] == 26);
    pthread_mutex_unlock(&input->mu);

    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);
    vprocSetShellPromptReadActive(shell_pid, false);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_fallback_byte_remains_readable_without_forced_interrupt(void) {
    int prev_shell = vprocGetShellSelfPid();
    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    unsigned char ctrl_c = 3;
    assert(vprocHostWrite(host_pipe[1], &ctrl_c, 1) == 1);
    assert(session_input_wait_len(input, 1, 500));

    char got = 0;
    errno = 0;
    assert(vprocSessionReadInputShimMode(&got, 1, true) == 1);
    assert((unsigned char)got == ctrl_c);

    pthread_mutex_lock(&input->mu);
    assert(!input->interrupt_pending);
    pthread_mutex_unlock(&input->mu);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_c_dispatches_to_foreground_job_when_not_shell_foreground(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
    }
    session->stdin_host_fd = host_pipe[0];
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);
    int other_shell_pid = vprocReservePid();
    assert(other_shell_pid > 0);
    VProcOptions other_shell_opts = vprocDefaultOptions();
    other_shell_opts.pid_hint = other_shell_pid;
    VProc *other_shell_vp = vprocCreate(&other_shell_opts);
    assert(other_shell_vp);
    assert(vprocPid(other_shell_vp) == other_shell_pid);
    assert(vprocSetSid(other_shell_pid, other_shell_pid) == 0);
    assert(vprocSetPgid(other_shell_pid, other_shell_pid) == 0);
    vprocSetShellPromptReadActive(other_shell_pid, true);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;
    unsigned char ctrl_c = 3;
    assert(vprocHostWrite(host_pipe[1], &ctrl_c, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));

    pthread_mutex_lock(&input->mu);
    assert(input->len == 0);
    pthread_mutex_unlock(&input->mu);

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    vprocSetShellPromptReadActive(other_shell_pid, false);
    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(other_shell_vp);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_z_stops_foreground_job_when_not_shell_foreground(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
    }
    session->stdin_host_fd = host_pipe[0];
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);
    int other_shell_pid = vprocReservePid();
    assert(other_shell_pid > 0);
    VProcOptions other_shell_opts = vprocDefaultOptions();
    other_shell_opts.pid_hint = other_shell_pid;
    VProc *other_shell_vp = vprocCreate(&other_shell_opts);
    assert(other_shell_vp);
    assert(vprocPid(other_shell_vp) == other_shell_pid);
    assert(vprocSetSid(other_shell_pid, other_shell_pid) == 0);
    assert(vprocSetPgid(other_shell_pid, other_shell_pid) == 0);
    vprocSetShellPromptReadActive(other_shell_pid, true);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;
    unsigned char ctrl_z = 26;
    assert(vprocHostWrite(host_pipe[1], &ctrl_z, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));

    pthread_mutex_lock(&input->mu);
    assert(input->len == 0);
    pthread_mutex_unlock(&input->mu);

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGTSTP);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocKillShim(worker_pid, SIGCONT) == 0);
    vprocMarkExit(worker_vp, 0);
    status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);

    vprocSetShellPromptReadActive(other_shell_pid, false);
    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(other_shell_vp);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}


static void assert_session_ctrl_z_then_ctrl_c_stop_unsupported_foreground_job(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);
    vprocSetCommandLabel(worker_pid, "watch");
    vprocSetStopUnsupported(worker_pid, true);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
    }
    session->stdin_host_fd = host_pipe[0];
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;

    unsigned char ctrl_z = 26;
    assert(vprocHostWrite(host_pipe[1], &ctrl_z, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));
    session_input_clear_interrupt(input);

    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG) == 0);

    sigset_t pending;
    sigemptyset(&pending);
    assert(vprocSigpending(worker_pid, &pending) == 0);
    assert(sigismember(&pending, SIGTSTP) == 1);

    unsigned char ctrl_c = 3;
    assert(vprocHostWrite(host_pipe[1], &ctrl_c, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));

    int waited = 0;
    status = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_c_dispatches_to_frontend_like_foreground_group(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int pascal_pid = vprocReservePid();
    int rea_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(pascal_pid > 0);
    assert(rea_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions pascal_opts = vprocDefaultOptions();
    pascal_opts.pid_hint = pascal_pid;
    VProc *pascal_vp = vprocCreate(&pascal_opts);
    assert(pascal_vp);
    assert(vprocPid(pascal_vp) == pascal_pid);

    VProcOptions rea_opts = vprocDefaultOptions();
    rea_opts.pid_hint = rea_pid;
    VProc *rea_vp = vprocCreate(&rea_opts);
    assert(rea_vp);
    assert(vprocPid(rea_vp) == rea_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);

    vprocSetParent(pascal_pid, shell_pid);
    assert(vprocSetSid(pascal_pid, shell_pid) == 0);
    assert(vprocSetPgid(pascal_pid, pascal_pid) == 0);
    vprocSetCommandLabel(pascal_pid, "pascal");

    vprocSetParent(rea_pid, shell_pid);
    assert(vprocSetSid(rea_pid, shell_pid) == 0);
    assert(vprocSetPgid(rea_pid, pascal_pid) == 0);
    vprocSetCommandLabel(rea_pid, "rea");

    assert(vprocSetForegroundPgid(shell_pid, pascal_pid) == 0);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);

    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    if (session->stdin_host_fd >= 0) {
        assert(vprocHostClose(session->stdin_host_fd) == 0);
    }
    if (session->stdout_host_fd >= 0) {
        assert(vprocHostClose(session->stdout_host_fd) == 0);
    }
    if (session->stderr_host_fd >= 0) {
        assert(vprocHostClose(session->stderr_host_fd) == 0);
    }
    session->stdin_host_fd = host_pipe[0];
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;

    unsigned char ctrl_c = 3;
    assert(vprocHostWrite(host_pipe[1], &ctrl_c, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));

    int pascal_status = 0;
    int rea_status = 0;
    int pascal_waited = 0;
    int rea_waited = 0;
    for (int i = 0; i < 100; ++i) {
        if (!pascal_waited) {
            int rc = vprocWaitPidShim(pascal_pid, &pascal_status, WNOHANG);
            if (rc == pascal_pid) {
                pascal_waited = 1;
            } else {
                assert(rc == 0);
            }
        }
        if (!rea_waited) {
            int rc = vprocWaitPidShim(rea_pid, &rea_status, WNOHANG);
            if (rc == rea_pid) {
                rea_waited = 1;
            } else {
                assert(rc == 0);
            }
        }
        if (pascal_waited && rea_waited) {
            break;
        }
        usleep(5000);
    }
    assert(pascal_waited);
    assert(rea_waited);
    assert(WIFSIGNALED(pascal_status));
    assert(WTERMSIG(pascal_status) == SIGINT);
    assert(WIFSIGNALED(rea_status));
    assert(WTERMSIG(rea_status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(rea_vp);
    vprocDestroy(pascal_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_z_dispatches_to_frontend_like_foreground_group(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int pascal_pid = vprocReservePid();
    int rea_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(pascal_pid > 0);
    assert(rea_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions pascal_opts = vprocDefaultOptions();
    pascal_opts.pid_hint = pascal_pid;
    VProc *pascal_vp = vprocCreate(&pascal_opts);
    assert(pascal_vp);
    assert(vprocPid(pascal_vp) == pascal_pid);

    VProcOptions rea_opts = vprocDefaultOptions();
    rea_opts.pid_hint = rea_pid;
    VProc *rea_vp = vprocCreate(&rea_opts);
    assert(rea_vp);
    assert(vprocPid(rea_vp) == rea_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);

    vprocSetParent(pascal_pid, shell_pid);
    assert(vprocSetSid(pascal_pid, shell_pid) == 0);
    assert(vprocSetPgid(pascal_pid, pascal_pid) == 0);
    vprocSetCommandLabel(pascal_pid, "pascal");

    vprocSetParent(rea_pid, shell_pid);
    assert(vprocSetSid(rea_pid, shell_pid) == 0);
    assert(vprocSetPgid(rea_pid, pascal_pid) == 0);
    vprocSetCommandLabel(rea_pid, "rea");

    assert(vprocSetForegroundPgid(shell_pid, pascal_pid) == 0);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);
    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;

    unsigned char ctrl_z = 26;
    assert(vprocHostWrite(host_pipe[1], &ctrl_z, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));

    int pascal_status = 0;
    int rea_status = 0;
    int pascal_waited = 0;
    int rea_waited = 0;
    for (int i = 0; i < 100; ++i) {
        if (!pascal_waited) {
            int rc = vprocWaitPidShim(pascal_pid, &pascal_status, WUNTRACED | WNOHANG);
            if (rc == pascal_pid) {
                pascal_waited = 1;
            } else {
                assert(rc == 0);
            }
        }
        if (!rea_waited) {
            int rc = vprocWaitPidShim(rea_pid, &rea_status, WUNTRACED | WNOHANG);
            if (rc == rea_pid) {
                rea_waited = 1;
            } else {
                assert(rc == 0);
            }
        }
        if (pascal_waited && rea_waited) {
            break;
        }
        usleep(5000);
    }
    assert(pascal_waited);
    assert(rea_waited);
    assert(WIFSTOPPED(pascal_status));
    assert(WSTOPSIG(pascal_status) == SIGTSTP);
    assert(WIFSTOPPED(rea_status));
    assert(WSTOPSIG(rea_status) == SIGTSTP);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocKillShim(pascal_pid, SIGCONT) == 0);
    assert(vprocKillShim(rea_pid, SIGCONT) == 0);
    vprocMarkExit(pascal_vp, 0);
    vprocMarkExit(rea_vp, 0);
    pascal_status = 0;
    rea_status = 0;
    assert(vprocWaitPidShim(pascal_pid, &pascal_status, 0) == pascal_pid);
    assert(vprocWaitPidShim(rea_pid, &rea_status, 0) == rea_pid);
    assert(WIFEXITED(pascal_status));
    assert(WEXITSTATUS(pascal_status) == 0);
    assert(WIFEXITED(rea_status));
    assert(WEXITSTATUS(rea_status) == 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(rea_vp);
    vprocDestroy(pascal_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_z_then_ctrl_c_stop_unsupported_frontend_group(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int pascal_pid = vprocReservePid();
    int rea_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(pascal_pid > 0);
    assert(rea_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions pascal_opts = vprocDefaultOptions();
    pascal_opts.pid_hint = pascal_pid;
    VProc *pascal_vp = vprocCreate(&pascal_opts);
    assert(pascal_vp);
    assert(vprocPid(pascal_vp) == pascal_pid);

    VProcOptions rea_opts = vprocDefaultOptions();
    rea_opts.pid_hint = rea_pid;
    VProc *rea_vp = vprocCreate(&rea_opts);
    assert(rea_vp);
    assert(vprocPid(rea_vp) == rea_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);

    vprocSetParent(pascal_pid, shell_pid);
    assert(vprocSetSid(pascal_pid, shell_pid) == 0);
    assert(vprocSetPgid(pascal_pid, pascal_pid) == 0);
    vprocSetCommandLabel(pascal_pid, "pascal");
    vprocSetStopUnsupported(pascal_pid, true);

    vprocSetParent(rea_pid, shell_pid);
    assert(vprocSetSid(rea_pid, shell_pid) == 0);
    assert(vprocSetPgid(rea_pid, pascal_pid) == 0);
    vprocSetCommandLabel(rea_pid, "rea");
    vprocSetStopUnsupported(rea_pid, true);

    assert(vprocSetForegroundPgid(shell_pid, pascal_pid) == 0);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);
    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    vprocSessionStdioActivate(session);

    VProcSessionInput *input = vprocSessionInputEnsureShim();
    assert(input);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;

    unsigned char ctrl_z = 26;
    assert(vprocHostWrite(host_pipe[1], &ctrl_z, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));
    session_input_clear_interrupt(input);

    int status = 0;
    assert(vprocWaitPidShim(pascal_pid, &status, WUNTRACED | WNOHANG) == 0);
    assert(vprocWaitPidShim(rea_pid, &status, WUNTRACED | WNOHANG) == 0);

    sigset_t pending;
    sigemptyset(&pending);
    assert(vprocSigpending(pascal_pid, &pending) == 0);
    assert(sigismember(&pending, SIGTSTP) == 1);
    sigemptyset(&pending);
    assert(vprocSigpending(rea_pid, &pending) == 0);
    assert(sigismember(&pending, SIGTSTP) == 1);

    unsigned char ctrl_c = 3;
    assert(vprocHostWrite(host_pipe[1], &ctrl_c, 1) == 1);
    assert(session_input_wait_interrupt(input, 500));

    int pascal_status = 0;
    int rea_status = 0;
    int pascal_waited = 0;
    int rea_waited = 0;
    for (int i = 0; i < 100; ++i) {
        if (!pascal_waited) {
            int rc = vprocWaitPidShim(pascal_pid, &pascal_status, WNOHANG);
            if (rc == pascal_pid) {
                pascal_waited = 1;
            } else {
                assert(rc == 0);
            }
        }
        if (!rea_waited) {
            int rc = vprocWaitPidShim(rea_pid, &rea_status, WNOHANG);
            if (rc == rea_pid) {
                rea_waited = 1;
            } else {
                assert(rc == 0);
            }
        }
        if (pascal_waited && rea_waited) {
            break;
        }
        usleep(5000);
    }
    assert(pascal_waited);
    assert(rea_waited);
    assert(WIFSIGNALED(pascal_status));
    assert(WTERMSIG(pascal_status) == SIGINT);
    assert(WIFSIGNALED(rea_status));
    assert(WTERMSIG(rea_status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(rea_vp);
    vprocDestroy(pascal_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_c_does_not_bleed_between_sessions(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_a_pid = vprocReservePid();
    int worker_a_pid = vprocReservePid();
    int shell_b_pid = vprocReservePid();
    int worker_b_pid = vprocReservePid();
    assert(shell_a_pid > 0);
    assert(worker_a_pid > 0);
    assert(shell_b_pid > 0);
    assert(worker_b_pid > 0);

    VProcOptions shell_a_opts = vprocDefaultOptions();
    shell_a_opts.pid_hint = shell_a_pid;
    VProc *shell_a_vp = vprocCreate(&shell_a_opts);
    assert(shell_a_vp);
    assert(vprocPid(shell_a_vp) == shell_a_pid);

    VProcOptions worker_a_opts = vprocDefaultOptions();
    worker_a_opts.pid_hint = worker_a_pid;
    VProc *worker_a_vp = vprocCreate(&worker_a_opts);
    assert(worker_a_vp);
    assert(vprocPid(worker_a_vp) == worker_a_pid);

    VProcOptions shell_b_opts = vprocDefaultOptions();
    shell_b_opts.pid_hint = shell_b_pid;
    VProc *shell_b_vp = vprocCreate(&shell_b_opts);
    assert(shell_b_vp);
    assert(vprocPid(shell_b_vp) == shell_b_pid);

    VProcOptions worker_b_opts = vprocDefaultOptions();
    worker_b_opts.pid_hint = worker_b_pid;
    VProc *worker_b_vp = vprocCreate(&worker_b_opts);
    assert(worker_b_vp);
    assert(vprocPid(worker_b_vp) == worker_b_pid);

    vprocSetShellSelfTid(pthread_self());

    vprocSetShellSelfPid(shell_a_pid);
    assert(vprocRegisterTidHint(shell_a_pid, pthread_self()) == shell_a_pid);
    vprocRegisterThread(shell_a_vp, pthread_self());
    assert(vprocSetSid(shell_a_pid, shell_a_pid) == 0);
    assert(vprocSetPgid(shell_a_pid, shell_a_pid) == 0);
    vprocSetParent(worker_a_pid, shell_a_pid);
    assert(vprocSetSid(worker_a_pid, shell_a_pid) == 0);
    assert(vprocSetPgid(worker_a_pid, worker_a_pid) == 0);
    assert(vprocSetForegroundPgid(shell_a_pid, worker_a_pid) == 0);

    vprocSetShellSelfPid(shell_b_pid);
    assert(vprocRegisterTidHint(shell_b_pid, pthread_self()) == shell_b_pid);
    vprocRegisterThread(shell_b_vp, pthread_self());
    assert(vprocSetSid(shell_b_pid, shell_b_pid) == 0);
    assert(vprocSetPgid(shell_b_pid, shell_b_pid) == 0);
    vprocSetParent(worker_b_pid, shell_b_pid);
    assert(vprocSetSid(worker_b_pid, shell_b_pid) == 0);
    assert(vprocSetPgid(worker_b_pid, worker_b_pid) == 0);
    assert(vprocSetForegroundPgid(shell_b_pid, worker_b_pid) == 0);

    int host_pipe_a[2];
    int host_pipe_b[2];
    assert(vprocHostPipe(host_pipe_a) == 0);
    assert(vprocHostPipe(host_pipe_b) == 0);

    VProcSessionStdio *session_a = session_create_with_host_stdin(host_pipe_a[0]);
    VProcSessionStdio *session_b = session_create_with_host_stdin(host_pipe_b[0]);

    vprocSessionStdioActivate(session_a);
    vprocSetShellSelfPid(shell_a_pid);
    VProcSessionInput *input_a = vprocSessionInputEnsureShim();
    assert(input_a);

    vprocSessionStdioActivate(session_b);
    vprocSetShellSelfPid(shell_b_pid);
    VProcSessionInput *input_b = vprocSessionInputEnsureShim();
    assert(input_b);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;

    unsigned char ctrl_c = 3;
    assert(vprocHostWrite(host_pipe_a[1], &ctrl_c, 1) == 1);
    assert(session_input_wait_interrupt(input_a, 500));

    int status_a = 0;
    int waited_a = 0;
    vprocSetShellSelfPid(shell_a_pid);
    for (int i = 0; i < 100; ++i) {
        waited_a = vprocWaitPidShim(worker_a_pid, &status_a, WNOHANG);
        if (waited_a == worker_a_pid) {
            break;
        }
        assert(waited_a == 0);
        usleep(5000);
    }
    assert(waited_a == worker_a_pid);
    assert(WIFSIGNALED(status_a));
    assert(WTERMSIG(status_a) == SIGINT);

    int status_b = 0;
    vprocSetShellSelfPid(shell_b_pid);
    for (int i = 0; i < 20; ++i) {
        int rc = vprocWaitPidShim(worker_b_pid, &status_b, WUNTRACED | WNOHANG);
        assert(rc == 0);
        usleep(5000);
    }

    vprocMarkExit(worker_b_vp, 0);
    assert(vprocWaitPidShim(worker_b_pid, &status_b, 0) == worker_b_pid);
    assert(WIFEXITED(status_b));
    assert(WEXITSTATUS(status_b) == 0);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocHostClose(host_pipe_a[1]) == 0);
    assert(vprocHostClose(host_pipe_b[1]) == 0);
    vprocSessionStdioDestroy(session_b);
    vprocSessionStdioDestroy(session_a);
    vprocDestroy(worker_b_vp);
    vprocDestroy(shell_b_vp);
    vprocDestroy(worker_a_vp);
    vprocDestroy(shell_a_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_ctrl_z_does_not_bleed_between_sessions(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_a_pid = vprocReservePid();
    int worker_a_pid = vprocReservePid();
    int shell_b_pid = vprocReservePid();
    int worker_b_pid = vprocReservePid();
    assert(shell_a_pid > 0);
    assert(worker_a_pid > 0);
    assert(shell_b_pid > 0);
    assert(worker_b_pid > 0);

    VProcOptions shell_a_opts = vprocDefaultOptions();
    shell_a_opts.pid_hint = shell_a_pid;
    VProc *shell_a_vp = vprocCreate(&shell_a_opts);
    assert(shell_a_vp);
    assert(vprocPid(shell_a_vp) == shell_a_pid);

    VProcOptions worker_a_opts = vprocDefaultOptions();
    worker_a_opts.pid_hint = worker_a_pid;
    VProc *worker_a_vp = vprocCreate(&worker_a_opts);
    assert(worker_a_vp);
    assert(vprocPid(worker_a_vp) == worker_a_pid);

    VProcOptions shell_b_opts = vprocDefaultOptions();
    shell_b_opts.pid_hint = shell_b_pid;
    VProc *shell_b_vp = vprocCreate(&shell_b_opts);
    assert(shell_b_vp);
    assert(vprocPid(shell_b_vp) == shell_b_pid);

    VProcOptions worker_b_opts = vprocDefaultOptions();
    worker_b_opts.pid_hint = worker_b_pid;
    VProc *worker_b_vp = vprocCreate(&worker_b_opts);
    assert(worker_b_vp);
    assert(vprocPid(worker_b_vp) == worker_b_pid);

    vprocSetShellSelfTid(pthread_self());

    vprocSetShellSelfPid(shell_a_pid);
    assert(vprocRegisterTidHint(shell_a_pid, pthread_self()) == shell_a_pid);
    vprocRegisterThread(shell_a_vp, pthread_self());
    assert(vprocSetSid(shell_a_pid, shell_a_pid) == 0);
    assert(vprocSetPgid(shell_a_pid, shell_a_pid) == 0);
    vprocSetParent(worker_a_pid, shell_a_pid);
    assert(vprocSetSid(worker_a_pid, shell_a_pid) == 0);
    assert(vprocSetPgid(worker_a_pid, worker_a_pid) == 0);
    assert(vprocSetForegroundPgid(shell_a_pid, worker_a_pid) == 0);

    vprocSetShellSelfPid(shell_b_pid);
    assert(vprocRegisterTidHint(shell_b_pid, pthread_self()) == shell_b_pid);
    vprocRegisterThread(shell_b_vp, pthread_self());
    assert(vprocSetSid(shell_b_pid, shell_b_pid) == 0);
    assert(vprocSetPgid(shell_b_pid, shell_b_pid) == 0);
    vprocSetParent(worker_b_pid, shell_b_pid);
    assert(vprocSetSid(worker_b_pid, shell_b_pid) == 0);
    assert(vprocSetPgid(worker_b_pid, worker_b_pid) == 0);
    assert(vprocSetForegroundPgid(shell_b_pid, worker_b_pid) == 0);

    int host_pipe_a[2];
    int host_pipe_b[2];
    assert(vprocHostPipe(host_pipe_a) == 0);
    assert(vprocHostPipe(host_pipe_b) == 0);

    VProcSessionStdio *session_a = session_create_with_host_stdin(host_pipe_a[0]);
    VProcSessionStdio *session_b = session_create_with_host_stdin(host_pipe_b[0]);

    vprocSessionStdioActivate(session_a);
    vprocSetShellSelfPid(shell_a_pid);
    VProcSessionInput *input_a = vprocSessionInputEnsureShim();
    assert(input_a);

    vprocSessionStdioActivate(session_b);
    vprocSetShellSelfPid(shell_b_pid);
    VProcSessionInput *input_b = vprocSessionInputEnsureShim();
    assert(input_b);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigtstpCallbackCount = 0;

    unsigned char ctrl_z = 26;
    assert(vprocHostWrite(host_pipe_a[1], &ctrl_z, 1) == 1);
    assert(session_input_wait_interrupt(input_a, 500));

    int status_a = 0;
    int waited_a = 0;
    vprocSetShellSelfPid(shell_a_pid);
    for (int i = 0; i < 100; ++i) {
        waited_a = vprocWaitPidShim(worker_a_pid, &status_a, WUNTRACED | WNOHANG);
        if (waited_a == worker_a_pid) {
            break;
        }
        assert(waited_a == 0);
        usleep(5000);
    }
    assert(waited_a == worker_a_pid);
    assert(WIFSTOPPED(status_a));
    assert(WSTOPSIG(status_a) == SIGTSTP);

    int status_b = 0;
    vprocSetShellSelfPid(shell_b_pid);
    for (int i = 0; i < 20; ++i) {
        int rc = vprocWaitPidShim(worker_b_pid, &status_b, WUNTRACED | WNOHANG);
        assert(rc == 0);
        usleep(5000);
    }

    vprocSetShellSelfPid(shell_a_pid);
    assert(vprocKillShim(worker_a_pid, SIGCONT) == 0);
    vprocMarkExit(worker_a_vp, 0);
    status_a = 0;
    assert(vprocWaitPidShim(worker_a_pid, &status_a, 0) == worker_a_pid);
    assert(WIFEXITED(status_a));
    assert(WEXITSTATUS(status_a) == 0);

    vprocSetShellSelfPid(shell_b_pid);
    vprocMarkExit(worker_b_vp, 0);
    assert(vprocWaitPidShim(worker_b_pid, &status_b, 0) == worker_b_pid);
    assert(WIFEXITED(status_b));
    assert(WEXITSTATUS(status_b) == 0);
    assert(gRuntimeSigintCallbackCount == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocHostClose(host_pipe_a[1]) == 0);
    assert(vprocHostClose(host_pipe_b[1]) == 0);
    vprocSessionStdioDestroy(session_b);
    vprocSessionStdioDestroy(session_a);
    vprocDestroy(worker_b_vp);
    vprocDestroy(shell_b_vp);
    vprocDestroy(worker_a_vp);
    vprocDestroy(shell_a_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_session_cwd_does_not_bleed_between_reused_worker_contexts(void) {
    int prev_shell = vprocGetShellSelfPid();
    char templ[] = "/tmp/vproc-session-cwd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-b", 0700) == 0 || errno == EEXIST);

    int shell_a_pid = vprocReservePid();
    int shell_b_pid = vprocReservePid();
    assert(shell_a_pid > 0);
    assert(shell_b_pid > 0);

    VProcOptions shell_a_opts = vprocDefaultOptions();
    shell_a_opts.pid_hint = shell_a_pid;
    VProc *shell_a_vp = vprocCreate(&shell_a_opts);
    assert(shell_a_vp);
    assert(vprocPid(shell_a_vp) == shell_a_pid);

    VProcOptions shell_b_opts = vprocDefaultOptions();
    shell_b_opts.pid_hint = shell_b_pid;
    VProc *shell_b_vp = vprocCreate(&shell_b_opts);
    assert(shell_b_vp);
    assert(vprocPid(shell_b_vp) == shell_b_pid);

    int host_pipe_a[2];
    int host_pipe_b[2];
    assert(vprocHostPipe(host_pipe_a) == 0);
    assert(vprocHostPipe(host_pipe_b) == 0);

    VProcSessionStdio *session_a = session_create_with_host_stdin(host_pipe_a[0]);
    VProcSessionStdio *session_b = session_create_with_host_stdin(host_pipe_b[0]);
    session_a->session_id = 9701;
    session_b->session_id = 9702;

    /* Seed per-session shell identity and registry metadata. */
    vprocSessionStdioActivate(session_a);
    vprocSetShellSelfPid(shell_a_pid);
    vprocSessionStdioActivate(session_b);
    vprocSetShellSelfPid(shell_b_pid);

    /* Simulate worker contexts that lost the per-session cached shell pid. */
    session_a->shell_pid = 0;
    session_b->shell_pid = 0;

    /* Worker executes tab A command first. */
    vprocSessionStdioActivate(NULL);
    vprocSetShellSelfPid(shell_a_pid);
    vprocSessionStdioActivate(session_a);
    assert(vprocChdirShim("/tmp/cwd-a") == 0);

    char cwd[PATH_MAX];
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    /* Reused worker then serves tab B. */
    vprocSessionStdioActivate(NULL);
    vprocSetShellSelfPid(shell_b_pid);
    vprocSessionStdioActivate(session_b);
    assert(vprocChdirShim("/tmp/cwd-b") == 0);
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-b") == 0);

    /* Switching back to tab A must restore A's cwd and not bleed B's state. */
    vprocSessionStdioActivate(session_a);
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    assert(vprocHostClose(host_pipe_a[1]) == 0);
    assert(vprocHostClose(host_pipe_b[1]) == 0);
    vprocSessionStdioDestroy(session_b);
    vprocSessionStdioDestroy(session_a);
    vprocDestroy(shell_b_vp);
    vprocDestroy(shell_a_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_path_virtualized_chdir_updates_session_cwd_when_interpose_bypassed(void) {
    int prev_shell = vprocGetShellSelfPid();
    char templ[] = "/tmp/vproc-pathvirt-cwd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-b", 0700) == 0 || errno == EEXIST);

    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);
    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    session->session_id = 9801;
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);

    assert(vprocChdirShim("/tmp/cwd-a") == 0);

    char cwd[PATH_MAX];
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    /*
     * With interpose bypassed, path virtualization must still update the
     * session's virtual cwd (not just host process cwd).
     */
    vprocRegisterInterposeBypassThread(pthread_self());
    assert(pscalPathVirtualized_chdir("/tmp/cwd-b") == 0);
    vprocUnregisterInterposeBypassThread(pthread_self());

    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-b") == 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_vproc_chdir_accepts_expanded_container_path(void) {
    int prev_shell = vprocGetShellSelfPid();
    char templ[] = "/tmp/vproc-expanded-cwd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-b", 0700) == 0 || errno == EEXIST);

    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);
    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    session->session_id = 9802;
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);

    assert(vprocChdirShim("/tmp/cwd-a") == 0);

    char expanded_path[PATH_MAX];
    snprintf(expanded_path, sizeof(expanded_path), "%s/tmp/cwd-b", root);
    assert(vprocChdirShim(expanded_path) == 0);

    char cwd[PATH_MAX];
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-b") == 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_path_virtualized_relative_ops_follow_session_cwd(void) {
    int prev_shell = vprocGetShellSelfPid();
    char templ[] = "/tmp/vproc-relative-cwd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/home", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);

    int host_only_fd = pscalPathVirtualized_open("/home/host-only.txt", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(host_only_fd >= 0);
    assert(write(host_only_fd, "host", 4) == 4);
    close(host_only_fd);

    int tab_only_fd = pscalPathVirtualized_open("/tmp/cwd-a/tab-only.txt", O_CREAT | O_WRONLY | O_TRUNC, 0600);
    assert(tab_only_fd >= 0);
    assert(write(tab_only_fd, "tab", 3) == 3);
    close(tab_only_fd);

    char expanded_home[PATH_MAX];
    snprintf(expanded_home, sizeof(expanded_home), "%s/home", root);
    assert(chdir(expanded_home) == 0);

    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);
    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    session->session_id = 9903;
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);

    assert(pscalPathVirtualized_chdir("/tmp/cwd-a") == 0);

    char cwd[PATH_MAX];
    assert(pscalPathVirtualized_getcwd(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    DIR *dir = pscalPathVirtualized_opendir(".");
    assert(dir);
    bool saw_tab_only = false;
    bool saw_host_only = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, "tab-only.txt") == 0) {
            saw_tab_only = true;
        } else if (strcmp(entry->d_name, "host-only.txt") == 0) {
            saw_host_only = true;
        }
    }
    closedir(dir);
    assert(saw_tab_only);
    assert(!saw_host_only);

    struct stat st;
    assert(pscalPathVirtualized_stat("tab-only.txt", &st) == 0);
    assert(pscalPathVirtualized_stat("host-only.txt", &st) != 0);

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_session_cwd_prefers_active_session_over_stale_thread_hint(void) {
    int prev_shell = vprocGetShellSelfPid();
    char templ[] = "/tmp/vproc-session-cwd-priority-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-b", 0700) == 0 || errno == EEXIST);

    int shell_a_pid = vprocReservePid();
    int shell_b_pid = vprocReservePid();
    assert(shell_a_pid > 0);
    assert(shell_b_pid > 0);

    VProcOptions shell_a_opts = vprocDefaultOptions();
    shell_a_opts.pid_hint = shell_a_pid;
    VProc *shell_a_vp = vprocCreate(&shell_a_opts);
    assert(shell_a_vp);
    assert(vprocPid(shell_a_vp) == shell_a_pid);

    VProcOptions shell_b_opts = vprocDefaultOptions();
    shell_b_opts.pid_hint = shell_b_pid;
    VProc *shell_b_vp = vprocCreate(&shell_b_opts);
    assert(shell_b_vp);
    assert(vprocPid(shell_b_vp) == shell_b_pid);

    int host_pipe_a[2];
    int host_pipe_b[2];
    assert(vprocHostPipe(host_pipe_a) == 0);
    assert(vprocHostPipe(host_pipe_b) == 0);

    VProcSessionStdio *session_a = session_create_with_host_stdin(host_pipe_a[0]);
    VProcSessionStdio *session_b = session_create_with_host_stdin(host_pipe_b[0]);
    session_a->session_id = 9901;
    session_b->session_id = 9902;

    /* Seed each session's shell pid and cwd. */
    vprocSessionStdioActivate(session_a);
    vprocSetShellSelfPid(shell_a_pid);
    assert(vprocChdirShim("/tmp/cwd-a") == 0);

    vprocSessionStdioActivate(session_b);
    vprocSetShellSelfPid(shell_b_pid);
    assert(vprocChdirShim("/tmp/cwd-b") == 0);

    /* Simulate a reused worker thread carrying a stale pid hint from tab B. */
    assert(vprocRegisterTidHint(shell_b_pid, pthread_self()) == shell_b_pid);

    char cwd[PATH_MAX];
    vprocSessionStdioActivate(session_a);
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    assert(vprocHostClose(host_pipe_a[1]) == 0);
    assert(vprocHostClose(host_pipe_b[1]) == 0);
    vprocSessionStdioDestroy(session_b);
    vprocSessionStdioDestroy(session_a);
    vprocDestroy(shell_b_vp);
    vprocDestroy(shell_a_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
}

static void assert_shell_cwd_apis_target_shell_vproc_with_worker_active(void) {
    int prev_shell = vprocGetShellSelfPid();
    char templ[] = "/tmp/vproc-shell-cwd-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    assert(chdir(root) == 0);

    assert(pscalPathVirtualized_mkdir("/tmp", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-a", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-b", 0700) == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_mkdir("/tmp/cwd-c", 0700) == 0 || errno == EEXIST);

    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    int host_pipe[2];
    assert(vprocHostPipe(host_pipe) == 0);
    VProcSessionStdio *session = session_create_with_host_stdin(host_pipe[0]);
    session->session_id = 9910;
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);

    assert(vprocShellChdirShim("/tmp/cwd-a") == 0);

    char cwd[PATH_MAX];
    assert(vprocShellGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    vprocRegisterThread(worker_vp, pthread_self());
    vprocActivate(worker_vp);
    assert(vprocChdirShim("/tmp/cwd-b") == 0);
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-b") == 0);

    assert(vprocShellGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-a") == 0);

    VProcOptions bystander_opts = vprocDefaultOptions();
    bystander_opts.pid_hint = vprocReservePid();
    VProc *bystander_vp = vprocCreate(&bystander_opts);
    assert(bystander_vp);
    vprocActivate(bystander_vp);
    assert(vprocChdirShim("/tmp/cwd-b") == 0);
    vprocDeactivate();

    assert(vprocShellChdirShim("/tmp/cwd-c") == 0);
    assert(vprocShellGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-c") == 0);

    /* A vproc stacked on the shell's thread is a stage running for the shell, as
     * in `. script`, so the shell's cd moves it too. */
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-c") == 0);
    vprocDeactivate();

    /* One that is not on the stack keeps its own directory. */
    vprocActivate(bystander_vp);
    assert(vprocGetcwdShim(cwd, sizeof(cwd)) != NULL);
    assert(strcmp(cwd, "/tmp/cwd-b") == 0);
    vprocDeactivate();

    assert(vprocHostClose(host_pipe[1]) == 0);
    vprocSessionStdioDestroy(session);
    vprocDestroy(bystander_vp);
    vprocDestroy(worker_vp);
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
    unsetenv("PATH_TRUNCATE");
    char cleanup_cmd[PATH_MAX + 16];
    snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", root);
    (void)system(cleanup_cmd);
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

static void assert_pid_hint_not_self_parented(void) {
    int prev_shell = vprocGetShellSelfPid();

    int forced_pid = vprocReservePid();
    assert(forced_pid > 0);
    vprocDiscard(forced_pid);

    vprocSetShellSelfPid(forced_pid);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = forced_pid;
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    assert(vprocPid(vp) == forced_pid);

    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = vprocSnapshot(snaps, cap ? cap : 1);
    assert(snapshot_find_parent(snaps, count, forced_pid) != forced_pid);
    free(snaps);

    vprocDestroy(vp);
    vprocSetShellSelfPid(prev_shell);
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

/* The parent of a task in the table, or -1 with no row for it. */
static int task_row_parent(int pid) {
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    assert(snaps);
    size_t count = vprocSnapshot(snaps, cap);
    int parent = snapshot_find_parent(snaps, count, pid);
    free(snaps);
    return parent;
}

/* Nothing waits for an orphan: what took it in only waits for its own jobs. Its row goes when it
 * ends, and it says nothing to whoever took it in, as init reaps one on Linux. */
static void assert_orphan_rows_go_when_they_end(void) {
    int prev_shell = vprocGetShellSelfPid();
    int prev_kernel = vprocGetKernelPid();
    VProc *kernel = vprocCreate(NULL);
    VProc *leader = vprocCreate(NULL);
    assert(kernel && leader);
    int kernel_pid = vprocPid(kernel);
    int leader_pid = vprocPid(leader);
    vprocSetKernelPid(kernel_pid);
    vprocSetShellSelfPid(leader_pid);
    vprocSetParent(kernel_pid, 0);
    vprocSetParent(leader_pid, kernel_pid);
    assert(vprocSetSid(leader_pid, leader_pid) == 0);

    VProc *parent = vprocCreate(NULL);
    VProc *running = vprocCreate(NULL);
    VProc *ended = vprocCreate(NULL);
    VProc *middle = vprocCreate(NULL);
    VProc *below = vprocCreate(NULL);
    VProc *threadless = vprocCreate(NULL);
    VProc *taken_back = vprocCreate(NULL);
    assert(parent && running && ended && middle && below && threadless && taken_back);
    int parent_pid = vprocPid(parent);
    int running_pid = vprocPid(running);
    int ended_pid = vprocPid(ended);
    int middle_pid = vprocPid(middle);
    int below_pid = vprocPid(below);
    int threadless_pid = vprocPid(threadless);
    int taken_back_pid = vprocPid(taken_back);
    int tasks[] = { parent_pid, running_pid, ended_pid, middle_pid, below_pid, threadless_pid, taken_back_pid };
    for (size_t i = 0; i < sizeof(tasks) / sizeof(tasks[0]); ++i) {
        assert(vprocSetSid(tasks[i], leader_pid) == 0);
        vprocSetParent(tasks[i], tasks[i] == parent_pid ? leader_pid : parent_pid);
    }
    vprocSetParent(below_pid, middle_pid);

    /* It ended before its parent: only the parent could have waited, and that is gone too. */
    vprocMarkExit(ended, 0);
    assert(task_row_parent(ended_pid) == parent_pid);
    /* Its own child goes on, and is an orphan in its turn. */
    vprocMarkExit(middle, 0);
    assert(task_row_parent(middle_pid) == parent_pid);

    vprocMarkExit(parent, 0);
    assert(task_row_parent(ended_pid) == -1);
    assert(task_row_parent(middle_pid) == -1);
    vprocActivate(leader);
    int status = 0;
    assert(vprocWaitPidShim(parent_pid, &status, 0) == parent_pid);
    vprocDeactivate();

    /* Still running, it is there to see and to signal. */
    assert(task_row_parent(running_pid) == leader_pid);
    assert(task_row_parent(below_pid) == leader_pid);
    assert(vprocKillShim(running_pid, 0) == 0);

    vprocMarkExit(running, 0);
    assert(task_row_parent(running_pid) == -1);
    errno = 0;
    assert(vprocKillShim(running_pid, 0) == -1 && errno == ESRCH);
    vprocMarkExit(below, 0);
    assert(task_row_parent(below_pid) == -1);
    assert(!vprocSigchldPending(leader_pid));

    /* With no thread, a kill is the end of it, and nothing else would say so. */
    assert(task_row_parent(threadless_pid) == leader_pid);
    assert(vprocKillShim(threadless_pid, SIGTERM) == 0);
    assert(task_row_parent(threadless_pid) == -1);

    /* Given a parent again, it has one to wait for it. */
    vprocSetParent(taken_back_pid, leader_pid);
    vprocMarkExit(taken_back, 3);
    assert(task_row_parent(taken_back_pid) == leader_pid);
    vprocActivate(leader);
    assert(vprocWaitPidShim(taken_back_pid, &status, 0) == taken_back_pid);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 3);
    vprocDeactivate();

    /* A parent reaped in place of ending leaves its children the same way. */
    VProc *reaped = vprocCreate(NULL);
    VProc *under_reaped = vprocCreate(NULL);
    assert(reaped && under_reaped);
    int reaped_pid = vprocPid(reaped);
    int under_reaped_pid = vprocPid(under_reaped);
    assert(vprocSetSid(reaped_pid, leader_pid) == 0);
    assert(vprocSetSid(under_reaped_pid, leader_pid) == 0);
    vprocSetParent(reaped_pid, leader_pid);
    vprocSetParent(under_reaped_pid, reaped_pid);
    vprocDiscard(reaped_pid);
    assert(task_row_parent(under_reaped_pid) == leader_pid);
    vprocMarkExit(under_reaped, 0);
    assert(task_row_parent(under_reaped_pid) == -1);

    VProc *all[] = { reaped, under_reaped, parent, running, ended, middle, below, threadless, taken_back };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        vprocDestroy(all[i]);
    }
    vprocDiscard(leader_pid);
    vprocDestroy(leader);
    vprocDiscard(kernel_pid);
    vprocDestroy(kernel);
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

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool creator_gone;
    bool saw_vproc;
    VProc *current;
} CreatorGoneArg;

static void *creator_gone_thread(void *arg) {
    CreatorGoneArg *state = (CreatorGoneArg *)arg;
    pthread_mutex_lock(&state->mu);
    while (!state->creator_gone) {
        pthread_cond_wait(&state->cv, &state->mu);
    }
    pthread_mutex_unlock(&state->mu);
    state->saw_vproc = vprocCurrent() != NULL;
    return NULL;
}

static bool snapshot_has_pid(int pid) {
    size_t cap = vprocSnapshot(NULL, 0);
    VProcSnapshot *snaps = calloc(cap ? cap : 1, sizeof(VProcSnapshot));
    size_t count = snaps ? vprocSnapshot(snaps, cap) : 0;
    bool found = false;
    for (size_t i = 0; i < count; ++i) {
        if (snaps[i].pid == pid) {
            found = true;
        }
    }
    free(snaps);
    return found;
}

/* A creator does not wait for its thread. It usually gets here first, so the thread's first
 * look finds the vproc gone: activating it then marked the freed pointer as checked, and
 * registering the thread brought back the task the creator had just discarded. */
static void assert_thread_starting_after_creator_ended(void) {
    for (int round = 0; round < 500; ++round) {
        VProc *vp = vprocCreate(NULL);
        assert(vp);
        int pid = vprocPid(vp);
        vprocActivate(vp);

        CreatorGoneArg state;
        memset(&state, 0, sizeof(state));
        pthread_mutex_init(&state.mu, NULL);
        pthread_cond_init(&state.cv, NULL);
        pthread_t t;
        assert(vprocPthreadCreateShim(&t, NULL, creator_gone_thread, &state) == 0);

        vprocDeactivate();
        vprocMarkExit(vp, 0);
        vprocDiscard(pid);
        vprocDestroy(vp);

        pthread_mutex_lock(&state.mu);
        state.creator_gone = true;
        pthread_cond_signal(&state.cv);
        pthread_mutex_unlock(&state.mu);
        assert(pthread_join(t, NULL) == 0);

        assert(!state.saw_vproc);
        assert(!snapshot_has_pid(pid));
        pthread_cond_destroy(&state.cv);
        pthread_mutex_destroy(&state.mu);
    }
}

static void *spawned_thread_entry(void *arg) {
    CreatorGoneArg *state = (CreatorGoneArg *)arg;
    state->current = vprocCurrent();
    return NULL;
}

/* The thread runs under the vproc it was given and owes nothing to whoever made that one. */
static void assert_spawned_thread_outlives_creator(void) {
    VProc *creator = vprocCreate(NULL);
    assert(creator);
    vprocActivate(creator);
    int pipefd[2];
    assert(vprocHostPipeUnowned(pipefd) == 0);
    assert(vprocRestoreHostFd(creator, 7, pipefd[1]) == 7);
    vprocMarkFdInherited(creator, 7);

    VProc *job = vprocCreate(NULL);
    assert(job);
    vprocDeactivate();
    int creator_pid = vprocPid(creator);
    vprocMarkExit(creator, 0);
    vprocDiscard(creator_pid);
    vprocDestroy(creator);
    vprocHostClose(pipefd[1]);

    CreatorGoneArg state;
    memset(&state, 0, sizeof(state));
    pthread_t t;
    assert(vprocSpawnThread(job, spawned_thread_entry, &state, &t) == 0);
    assert(pthread_join(t, NULL) == 0);
    assert(state.current == job);

    /* The copy made when the job's vproc was created outlived the creator. */
    vprocActivate(job);
    assert(vprocWriteShim(7, "x", 1) == 1);
    vprocDeactivate();
    char byte = 0;
    assert(vprocHostRead(pipefd[0], &byte, 1) == 1);
    assert(byte == 'x');

    int job_pid = vprocPid(job);
    vprocMarkExit(job, 0);
    vprocDiscard(job_pid);
    vprocDestroy(job);
    assert(vprocHostRead(pipefd[0], &byte, 1) == 0);
    vprocHostClose(pipefd[0]);
}

static void *spawned_sleeper_thread(void *arg) {
    (void)arg;
    for (;;) {
        pthread_testcancel();
        sched_yield();
    }
    return NULL;
}

/* `cmd & kill $!`: the kill comes before the job's thread has run, and it still has to end it. */
static void assert_kill_reaches_spawned_thread_before_it_runs(void) {
    VProc *shell = vprocCreate(NULL);
    assert(shell);
    vprocActivate(shell);
    for (int round = 0; round < 50; ++round) {
        VProc *job = vprocCreate(NULL);
        assert(job);
        int pid = vprocPid(job);
        pthread_t t;
        assert(vprocSpawnThread(job, spawned_sleeper_thread, NULL, &t) == 0);
        assert(vprocKillShim(pid, SIGTERM) == 0);
        void *ret = NULL;
        assert(pthread_join(t, &ret) == 0);
        assert(ret == PTHREAD_CANCELED);
        vprocDiscard(pid);
        vprocDestroy(job);
    }
    vprocDeactivate();
    vprocDestroy(shell);
}

/* A job's own redirection leaves nothing of what it replaced, so whoever reads that pipe sees
 * the end of it while the job runs. */
static void assert_replace_stdio_keeps_no_copy(void) {
    int pipefd[2];
    assert(vprocHostPipeUnowned(pipefd) == 0);
    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = pipefd[0];
    opts.stdout_fd = pipefd[1];
    VProc *vp = vprocCreate(&opts);
    assert(vp);

    int null_fd = vprocHostOpen("/dev/null", O_RDWR);
    assert(null_fd >= 0);
    struct stat null_st;
    assert(fstat(null_fd, &null_st) == 0);
    assert(vprocReplaceStdio(vp, STDOUT_FILENO, null_fd) == STDOUT_FILENO);
    assert(vprocReplaceStdio(vp, STDIN_FILENO, null_fd) == STDIN_FILENO);
    struct stat slot_st;
    assert(fstat(vprocTranslateFd(vp, STDOUT_FILENO), &slot_st) == 0);
    assert(slot_st.st_rdev == null_st.st_rdev);
    assert(fstat(vprocTranslateFd(vp, STDIN_FILENO), &slot_st) == 0);
    assert(slot_st.st_rdev == null_st.st_rdev);

    /* The vproc held the only other copy of the write end. */
    vprocHostClose(pipefd[1]);
    char byte = 0;
    assert(vprocHostRead(pipefd[0], &byte, 1) == 0);

    /* A pipe's write end is gone when nothing holds its read end. */
    int second[2];
    assert(vprocHostPipeUnowned(second) == 0);
    VProcOptions second_opts = vprocDefaultOptions();
    second_opts.stdin_fd = second[0];
    VProc *reader = vprocCreate(&second_opts);
    assert(reader);
    assert(vprocReplaceStdio(reader, STDIN_FILENO, null_fd) == STDIN_FILENO);
    vprocHostClose(second[0]);
    void (*prev_sigpipe)(int) = signal(SIGPIPE, SIG_IGN);
    errno = 0;
    assert(vprocHostWrite(second[1], "x", 1) == -1);
    assert(errno == EPIPE);
    signal(SIGPIPE, prev_sigpipe);
    vprocHostClose(second[1]);

    /* Destroying them closes each slot once: a descriptor opened since stays open. */
    int bystander = vprocHostOpen("/dev/null", O_RDONLY);
    assert(bystander >= 0);
    int pid = vprocPid(vp);
    vprocMarkExit(vp, 0);
    vprocDiscard(pid);
    vprocDestroy(vp);
    int reader_pid = vprocPid(reader);
    vprocMarkExit(reader, 0);
    vprocDiscard(reader_pid);
    vprocDestroy(reader);
    assert(fstat(bystander, &slot_st) == 0);
    vprocHostClose(bystander);
    vprocHostClose(null_fd);
    vprocHostClose(pipefd[0]);
}

static void assert_runtime_request_ctrl_c_dispatches_to_foreground_job(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigintCallbackCount = 0;
    assert(vprocRequestControlSignal(SIGINT));

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);

    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_z_stops_foreground_job(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);
    assert(vprocPid(worker_vp) == worker_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigtstpCallbackCount = 0;
    assert(vprocRequestControlSignal(SIGTSTP));

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGTSTP);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocKillShim(worker_pid, SIGCONT) == 0);
    vprocMarkExit(worker_vp, 0);
    status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);

    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_c_dispatches_with_explicit_shell_pid(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);

    vprocSetShellSelfPid(0);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigintCallbackCount = 0;
    assert(vprocRequestControlSignalForShell(shell_pid, SIGINT));

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);

    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_z_stops_with_explicit_shell_pid(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);

    vprocSetShellSelfPid(0);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigtstpCallbackCount = 0;
    assert(vprocRequestControlSignalForShell(shell_pid, SIGTSTP));

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGTSTP);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocKillShim(worker_pid, SIGCONT) == 0);
    vprocMarkExit(worker_vp, 0);
    status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);

    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_ctrl_c_ends_foreground_job_in_session(const char *label, uint64_t session_id) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    if (label) {
        vprocSetCommandLabel(worker_pid, label);
    }
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigintCallbackCount = 0;
    assert(vprocRequestControlSignalForSession(session_id, SIGINT));

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGINT);
    assert(gRuntimeSigintCallbackCount == 0);

    vprocSessionStdioDestroy(session);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_c_dispatches_with_explicit_session_id(void) {
    assert_ctrl_c_ends_foreground_job_in_session(NULL, 9501);
}

/* ^C belongs to the remote end only when ssh has a terminal there, and that sets
 * the session's passthrough flag. By name alone it's a job like any other. */
static void assert_runtime_request_ctrl_c_ends_ssh_without_passthrough(void) {
    assert_ctrl_c_ends_foreground_job_in_session("ssh example.com", 9506);
}

static void assert_runtime_request_ctrl_z_stops_with_explicit_session_id(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    uint64_t session_id = 9502;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigtstpCallbackCount = 0;
    assert(vprocRequestControlSignalForSession(session_id, SIGTSTP));

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 100; ++i) {
        waited = vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG);
        if (waited == worker_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == worker_pid);
    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGTSTP);
    assert(gRuntimeSigtstpCallbackCount == 0);

    assert(vprocKillShim(worker_pid, SIGCONT) == 0);
    vprocMarkExit(worker_vp, 0);
    status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);

    vprocSessionStdioDestroy(session);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_signals_defer_for_remote_foreground_session(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int remote_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(remote_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions remote_opts = vprocDefaultOptions();
    remote_opts.pid_hint = remote_pid;
    VProc *remote_vp = vprocCreate(&remote_opts);
    assert(remote_vp);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(remote_pid, shell_pid);
    assert(vprocSetSid(remote_pid, shell_pid) == 0);
    assert(vprocSetPgid(remote_pid, remote_pid) == 0);
    vprocSetCommandLabel(remote_pid, "scp a example.com:b");
    assert(vprocSetForegroundPgid(shell_pid, remote_pid) == 0);

    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    uint64_t session_id = 9503;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocSetForegroundPgid(shell_pid, remote_pid) == 0);

    assert(!vprocRequestControlSignalForSession(session_id, SIGINT));
    assert(!vprocRequestControlSignalForSession(session_id, SIGTSTP));

    int status = 0;
    assert(vprocWaitPidShim(remote_pid, &status, WUNTRACED | WNOHANG) == 0);

    vprocMarkExit(remote_vp, 0);
    assert(vprocWaitPidShim(remote_pid, &status, 0) == remote_pid);
    assert(WIFEXITED(status));

    vprocSessionStdioDestroy(session);
    vprocDestroy(remote_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_signals_defer_when_session_passthrough(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    uint64_t session_id = 9505;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    session->control_bytes_passthrough = true;
    vprocSessionSetControlBytePassthrough(session_id, true);
    assert(vprocSessionGetControlBytePassthrough(session_id));

    assert(!vprocRequestControlSignalForSession(session_id, SIGINT));
    assert(!vprocRequestControlSignalForSession(session_id, SIGTSTP));

    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG) == 0);

    vprocSessionSetControlBytePassthrough(session_id, false);
    vprocMarkExit(worker_vp, 0);
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);
    assert(WIFEXITED(status));

    vprocSessionStdioDestroy(session);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_runtime_request_ctrl_signals_defer_when_soft_disabled_env(void) {
    char *saved_flag = NULL;
    const char *current_flag = getenv("PSCALI_DISABLE_SOFT_SIGNALING");
    if (current_flag) {
        saved_flag = strdup(current_flag);
    }
    setenv("PSCALI_DISABLE_SOFT_SIGNALING", "1", 1);

    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);

    VProcOptions worker_opts = vprocDefaultOptions();
    worker_opts.pid_hint = worker_pid;
    VProc *worker_vp = vprocCreate(&worker_opts);
    assert(worker_vp);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);

    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetParent(worker_pid, shell_pid);
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);
    uint64_t session_id = 9504;
    VProcSessionStdio *session = vprocSessionStdioCreate();
    assert(session);
    assert(vprocSessionStdioInitWithPty(session, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(session);
    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    assert(!vprocRequestControlSignalForSession(session_id, SIGINT));
    assert(!vprocRequestControlSignalForSession(session_id, SIGTSTP));

    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG) == 0);

    vprocMarkExit(worker_vp, 0);
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);
    assert(WIFEXITED(status));

    vprocSessionStdioDestroy(session);
    vprocDestroy(worker_vp);
    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);

    if (saved_flag) {
        setenv("PSCALI_DISABLE_SOFT_SIGNALING", saved_flag, 1);
        free(saved_flag);
    } else {
        unsetenv("PSCALI_DISABLE_SOFT_SIGNALING");
    }
}

static void assert_sigint_runtime_callback_reenters_without_deadlock(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    gRuntimeSigintShellPid = shell_pid;
    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigintReenterEnabled = 1;
    assert(vprocKillShim(shell_pid, SIGINT) == 0);
    gRuntimeSigintReenterEnabled = 0;

    assert(gRuntimeSigintCallbackCount > 0);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_sigtstp_runtime_callback_reenters_without_deadlock(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    gRuntimeSigtstpShellPid = shell_pid;
    gRuntimeSigtstpCallbackCount = 0;
    gRuntimeSigtstpReenterEnabled = 1;
    assert(vprocKillShim(shell_pid, SIGTSTP) == 0);
    gRuntimeSigtstpReenterEnabled = 0;

    /* SIGTSTP delivery is now fully virtualized in vproc for shell-owned
     * control flow; no out-of-band runtime callback should be needed. */
    assert(gRuntimeSigtstpCallbackCount == 0);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_sigtstp_runtime_callback_foreground_reentry_is_single_shot(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int fg_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(fg_pid > 0);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    vprocSetStopUnsupported(shell_pid, true);

    assert(vprocSetSid(fg_pid, shell_pid) == 0);
    assert(vprocSetPgid(fg_pid, fg_pid) == 0);
    vprocSetStopUnsupported(fg_pid, true);
    assert(vprocSetForegroundPgid(shell_pid, fg_pid) == 0);

    gRuntimeSigtstpShellPid = shell_pid;
    gRuntimeSigtstpTargetPgid = fg_pid;
    gRuntimeSigtstpCallbackCount = 0;
    gRuntimeSigtstpReenterEnabled = 1;

    assert(vprocKillShim(shell_pid, SIGTSTP) == 0);

    gRuntimeSigtstpReenterEnabled = 0;
    gRuntimeSigtstpTargetPgid = -1;
    assert(gRuntimeSigtstpCallbackCount == 0);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_sigtstp_non_shell_same_tid_does_not_request_runtime_suspend(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = worker_pid;
    VProc *worker = vprocCreate(&opts);
    assert(worker);
    assert(vprocPid(worker) == worker_pid);
    vprocRegisterThread(worker, pthread_self());
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigtstpCallbackCount = 0;
    gRuntimeSigtstpReenterEnabled = 0;
    assert(vprocKillShim(worker_pid, SIGTSTP) == 0);
    assert(gRuntimeSigtstpCallbackCount == 0);

    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, WUNTRACED) == worker_pid);
    assert(WIFSTOPPED(status));
    assert(vprocKillShim(worker_pid, SIGCONT) == 0);
    vprocMarkExit(worker, 0);
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);
    vprocDestroy(worker);

    vprocSetShellSelfPid(prev_shell);
}

static void assert_command_scope_from_shell_is_stoppable(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    VProcCommandScope scope;
    assert(vprocCommandScopeBegin(&scope, "scope-stop-test", false, true));
    int child_pid = scope.pid;
    assert(child_pid > 0);
    assert(child_pid != shell_pid);
    assert(vprocSetForegroundPgid(shell_pid, child_pid) == 0);

    assert(vprocKillShim(child_pid, SIGTSTP) == 0);
    vprocDeactivate(); /* scope child -> shell parent */
    int status = 0;
    int waited = 0;
    for (int i = 0; i < 20; ++i) {
        waited = vprocWaitPidShim(child_pid, &status, WUNTRACED | WNOHANG);
        if (waited == child_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == child_pid);
    assert(WIFSTOPPED(status));

    assert(vprocKillShim(child_pid, SIGCONT) == 0);
    vprocActivate(scope.vp);
    vprocCommandScopeEnd(&scope, 0);

    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_command_scope_end_preserves_stop_status(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    assert(shell_pid > 0);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = shell_pid;
    VProc *shell_vp = vprocCreate(&shell_opts);
    assert(shell_vp);
    assert(vprocPid(shell_vp) == shell_pid);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    vprocRegisterThread(shell_vp, pthread_self());
    vprocActivate(shell_vp);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    VProcCommandScope scope;
    assert(vprocCommandScopeBegin(&scope, "scope-end-stop", false, true));
    int child_pid = scope.pid;
    assert(child_pid > 0);
    assert(child_pid != shell_pid);
    assert(vprocSetForegroundPgid(shell_pid, child_pid) == 0);

    /* Mirror watch-style scoped applets that temporarily disable direct stops. */
    vprocSetStopUnsupported(child_pid, true);
    vprocCommandScopeEnd(&scope, 128 + SIGTSTP);

    int status = 0;
    int waited = 0;
    for (int i = 0; i < 20; ++i) {
        waited = vprocWaitPidShim(child_pid, &status, WUNTRACED | WNOHANG);
        if (waited == child_pid) {
            break;
        }
        assert(waited == 0);
        usleep(5000);
    }
    assert(waited == child_pid);
    assert(WIFSTOPPED(status));
    assert(WSTOPSIG(status) == SIGTSTP);

    assert(vprocKillShim(child_pid, SIGKILL) == 0);
    status = 0;
    assert(vprocWaitPidShim(child_pid, &status, 0) == child_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGKILL);

    vprocDeactivate();
    vprocDestroy(shell_vp);
    vprocSetShellSelfPid(prev_shell);
}

static void assert_stop_unsupported_sigtstp_queues_pending_signal(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = worker_pid;
    VProc *worker = vprocCreate(&opts);
    assert(worker);
    assert(vprocPid(worker) == worker_pid);
    vprocRegisterThread(worker, pthread_self());
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    vprocSetStopUnsupported(worker_pid, true);
    assert(vprocKillShim(worker_pid, SIGTSTP) == 0);

    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, WUNTRACED | WNOHANG) == 0);

    sigset_t pending;
    sigemptyset(&pending);
    assert(vprocSigpending(worker_pid, &pending) == 0);
    assert(sigismember(&pending, SIGTSTP) == 1);

    sigset_t waitset;
    sigemptyset(&waitset);
    sigaddset(&waitset, SIGTSTP);
    int signo = 0;
    assert(vprocSigwait(worker_pid, &waitset, &signo) == 0);
    assert(signo == SIGTSTP);

    sigemptyset(&pending);
    assert(vprocSigpending(worker_pid, &pending) == 0);
    assert(sigismember(&pending, SIGTSTP) == 0);

    vprocMarkExit(worker, 0);
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == 0);
    vprocDestroy(worker);

    vprocSetShellSelfPid(prev_shell);
}

static void assert_sigint_non_shell_same_tid_does_not_request_runtime_interrupt(void) {
    int prev_shell = vprocGetShellSelfPid();
    int shell_pid = vprocReservePid();
    int worker_pid = vprocReservePid();
    assert(shell_pid > 0);
    assert(worker_pid > 0);

    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = worker_pid;
    VProc *worker = vprocCreate(&opts);
    assert(worker);
    assert(vprocPid(worker) == worker_pid);
    vprocRegisterThread(worker, pthread_self());
    assert(vprocSetSid(worker_pid, shell_pid) == 0);
    assert(vprocSetPgid(worker_pid, worker_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, worker_pid) == 0);

    gRuntimeSigintCallbackCount = 0;
    gRuntimeSigintReenterEnabled = 0;
    assert(vprocKillShim(worker_pid, SIGINT) == 0);
    assert(gRuntimeSigintCallbackCount == 0);

    int status = 0;
    assert(vprocWaitPidShim(worker_pid, &status, 0) == worker_pid);
    assert(WIFSIGNALED(status));
    assert(WTERMSIG(status) == SIGINT);
    vprocDestroy(worker);

    vprocSetShellSelfPid(prev_shell);
}

/* Replacing slot 2 closes the number vprocCreate saved for it. vprocDestroy closed that number
 * again, and by then it could be anything. It was the poll wake pipe's when Aether muted stderr
 * around its pre-pass, and wakeups then wrote NULs to whatever had the number next. */
/* A thread's stream at its end: the buffer goes out only when asked, and either way the
 * descriptor closes, so the reader sees the end. */
static void assert_stream_end_with_thread(void) {
    for (int write_out = 0; write_out <= 1; ++write_out) {
        int p[2];
        assert(vprocHostPipeUnowned(p) == 0);
        FILE *stream = (fdopen)(p[1], "w");
        assert(stream);
        assert(setvbuf(stream, NULL, _IOFBF, 4096) == 0);
        assert(fwrite("abc", 1, 3, stream) == 3);
        vprocStreamEndWithThread(stream, write_out != 0);
        char buf[8];
        ssize_t got = vprocHostRead(p[0], buf, sizeof(buf));
        if (write_out) {
            assert(got == 3 && memcmp(buf, "abc", 3) == 0);
            got = vprocHostRead(p[0], buf, sizeof(buf));
        }
        assert(got == 0);
        vprocHostClose(p[0]);
    }
}

#if defined(__APPLE__)
/* A poll over more descriptors than fit on the stack grows buffers of the thread's own. A
 * thread that ends outside the vproc trampoline frees them from a key destructor, where
 * Darwin's __thread values already read as zero, so each one used to leak about 5 KB. */
static void *poll_wide_on_own_vproc(void *arg) {
    (void)arg;
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    struct pollfd fds[200];
    for (int i = 0; i < 200; ++i) {
        fds[i].fd = -1;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }
    assert(vprocPollShim(fds, 200, 0) == 0);
    vprocDeactivate();
    vprocDestroy(vp);
    return NULL;
}

static size_t heap_in_use(void) {
    malloc_statistics_t stats;
    malloc_zone_statistics(NULL, &stats);
    return stats.size_in_use;
}

static void assert_poll_scratch_freed_at_thread_end(void) {
    pthread_t thread;
    assert(vprocHostPthreadCreate(&thread, NULL, poll_wide_on_own_vproc, NULL) == 0);
    assert(pthread_join(thread, NULL) == 0);
    size_t before = heap_in_use();
    for (int i = 0; i < 200; ++i) {
        assert(vprocHostPthreadCreate(&thread, NULL, poll_wide_on_own_vproc, NULL) == 0);
        assert(pthread_join(thread, NULL) == 0);
    }
    size_t after = heap_in_use();
    /* The leak was 200 x 5 KB. */
    assert(after < before + 200 * 1024);
}
#endif

static void assert_destroy_leaves_replaced_stdio_number_alone(void) {
    int hole = vprocHostOpen("/dev/null", O_RDONLY);
    assert(hole >= 0);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    int pipefd[2];
    assert(vprocPipe(vp, pipefd) == 0);
    int saved = vprocTranslateFd(vp, STDERR_FILENO);
    assert(saved > hole);
    vprocHostClose(hole);
    assert(vprocDup2(vp, pipefd[1], STDERR_FILENO) == STDERR_FILENO);
    assert(vprocTranslateFd(vp, STDERR_FILENO) != saved);

    /* Whatever the process opens next can get the number. */
    int takers[256];
    int count = 0;
    int taker = -1;
    while (count < 256 && taker != saved) {
        taker = vprocHostOpen("/dev/null", O_RDONLY);
        assert(taker >= 0 && taker <= saved);
        takers[count++] = taker;
    }
    assert(taker == saved);

    vprocDestroy(vp);
    assert((fcntl)(saved, F_GETFD) >= 0);
    for (int i = 0; i < count; ++i) {
        vprocHostClose(takers[i]);
    }
}

/* The links in /dev and /proc lead to this vproc's slots, past anything the host's descriptors
 * hold, and opening one acts as on Linux: a file is opened again, and nothing lands under the root. */
static void assert_named_descriptors_act_as_on_linux(void) {
    char templ[] = "/tmp/vproc-devstdio-XXXXXX";
    char *root = mkdtemp(templ);
    assert(root);
    setenv("PATH_TRUNCATE", root, 1);
    pathTruncateProvisionDev(root);
    VProc *vp = vprocCreate(NULL);
    assert(vp);
    vprocActivate(vp);
    int pid = vprocPid(vp);

    assert(vprocPathNamedFd("/dev/stdin") == STDIN_FILENO);
    assert(vprocPathNamedFd("/dev/stdout") == STDOUT_FILENO);
    assert(vprocPathNamedFd("/dev/stderr") == STDERR_FILENO);
    assert(vprocPathNamedFd("//dev/./stdout") == STDOUT_FILENO);
    assert(vprocPathNamedFd("/private/dev/stderr") == STDERR_FILENO);
    assert(vprocPathNamedFd("/dev/fd/12") == 12);
    assert(vprocPathNamedFd("/proc/self/fd/3") == 3);
    char own[64];
    snprintf(own, sizeof(own), "/proc/%d/fd/4", pid);
    assert(vprocPathNamedFd(own) == 4);
    snprintf(own, sizeof(own), "/proc/%d/fd/4", pid + 1);
    assert(vprocPathNamedFd(own) == -1);
    assert(vprocPathNamedFd("/dev/stdout2") == -1);
    assert(vprocPathNamedFd("/home/dev/stdout") == -1);
    assert(vprocPathNamedFd("stdout") == -1);
    assert(vprocPathNamedFd("/dev/null") == -1);
    assert(vprocPathNamedFd("/dev/fd") == -1);
    assert(vprocPathNamedFd("/dev/fd/") == -1);
    assert(vprocPathNamedFd("/dev/fd/1x") == -1);
    assert(vprocPathNamedFd("/dev/fd/99999999999") == -1);
    assert(vprocPathNamedFd("") == -1);
    assert(vprocPathNamedFd(NULL) == -1);

    /* What readlink and lstat see. */
    char link[PATH_MAX];
    ssize_t link_len = pscalPathVirtualized_readlink("/dev/stdout", link, sizeof(link) - 1);
    assert(link_len > 0);
    link[link_len] = '\0';
    assert(strcmp(link, "/proc/self/fd/1") == 0);
    struct stat st;
    assert(pscalPathVirtualized_lstat("/dev/stdout", &st) == 0 && S_ISLNK(st.st_mode));

    /* A pipe: a copy of the end it has. */
    int pipefd[2];
    assert(vprocPipe(vp, pipefd) == 0);
    assert(vprocDup2(vp, pipefd[1], STDOUT_FILENO) == STDOUT_FILENO);
    assert(vprocDup2(vp, pipefd[0], STDIN_FILENO) == STDIN_FILENO);
    int fd = vprocOpenShim("/dev/stdout", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    assert(fd > STDERR_FILENO);
    assert(vprocWriteShim(fd, "a", 1) == 1);
    assert(vprocCloseShim(fd) == 0);
    assert(vprocWriteShim(STDOUT_FILENO, "b", 1) == 1);

    /* Through a stream, with no host descriptor, and it does not wait for a close. */
    FILE *out = pscalPathVirtualized_fopen("/dev/stdout", "w");
    assert(out);
    int slot = vprocFilenoShim(out);
    assert(slot > STDERR_FILENO);
    assert(pscalPathVirtualized_streamSlot(out) == slot);
    assert(pscalPathVirtualized_streamSlot(stdout) == -1);
    assert(fputs("c", out) >= 0);
    char got[16] = {0};
    assert(vprocReadShim(STDIN_FILENO, got, 3) == 3);
    assert(memcmp(got, "abc", 3) == 0);
    assert(vprocWriteShim(slot, "d\n", 2) == 2);
    assert(fclose(out) == 0);
    FILE *in = pscalPathVirtualized_fopen("/dev/stdin", "r");
    assert(in);
    assert(fgets(got, sizeof(got), in) != NULL);
    assert(strcmp(got, "d\n") == 0);
    assert(fclose(in) == 0);
    assert(pscalPathVirtualized_access("/dev/stdout", F_OK) == 0);
    errno = 0;
    assert(pscalPathVirtualized_access("/dev/stdout", X_OK) == -1 && errno == EACCES);
    errno = 0;
    assert(vprocOpenShim("/dev/stdin", O_WRONLY) == -1 && errno == EACCES);
    assert(vprocOpenShim("/dev/fd/1", O_WRONLY) > STDERR_FILENO);
    assert(vprocFdLinkTarget(pid, STDOUT_FILENO, link, sizeof(link)));
    assert(strncmp(link, "pipe:[", 6) == 0);

    /* A file: opened again, whatever the descriptor's mode, offset or flags. */
    int file = vprocOpenShim("/named.txt", O_RDONLY | O_CREAT, 0644);
    assert(file >= 0);
    assert(vprocDup2(vp, file, STDIN_FILENO) == STDIN_FILENO);
    fd = vprocOpenShim("/dev/stdin", O_WRONLY);
    assert(fd > STDERR_FILENO);
    assert(vprocWriteShim(fd, "hello\n", 6) == 6);
    assert(vprocCloseShim(fd) == 0);
    assert(vprocReadShim(STDIN_FILENO, got, 2) == 2);
    fd = vprocOpenShim("/proc/self/fd/0", O_RDONLY);
    assert(fd > STDERR_FILENO);
    memset(got, 0, sizeof(got));
    assert(vprocReadShim(fd, got, 5) == 5);
    assert(strcmp(got, "hello") == 0);
    assert(vprocCloseShim(fd) == 0);
    fd = vprocOpenShim("/dev/stdin", O_WRONLY | O_APPEND);
    assert(vprocWriteShim(fd, "x", 1) == 1);
    assert(vprocCloseShim(fd) == 0);
    assert(pscalPathVirtualized_stat("/dev/stdin", &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 7);
    fd = vprocOpenShim("/dev/fd/0", O_WRONLY | O_TRUNC);
    assert(vprocCloseShim(fd) == 0);
    assert(pscalPathVirtualized_stat("/dev/stdin", &st) == 0 && st.st_size == 0);
    assert(pscalPathVirtualized_access("/dev/stdin", W_OK) == 0);
    errno = 0;
    assert(vprocOpenShim("/dev/stdin", O_WRONLY | O_CREAT | O_EXCL, 0644) == -1 && errno == EEXIST);
    assert(vprocFdLinkTarget(pid, STDIN_FILENO, link, sizeof(link)));
    char *real = vprocRealpathShim("/dev/stdin", NULL);
    assert(real && strcmp(real, "/named.txt") == 0);
    free(real);
    assert(pscalPathVirtualized_realpath("/dev/fd/0", got) == NULL || strcmp(got, "/named.txt") == 0);

    /* The name went to another file: that one stays as it is, and the descriptor is what counts. */
    assert(pscalPathVirtualized_rename("/named.txt", "/moved.txt") == 0);
    int taker = vprocOpenShim("/named.txt", O_WRONLY | O_CREAT, 0644);
    assert(taker >= 0);
    assert(vprocWriteShim(taker, "keep", 4) == 4);
    assert(vprocCloseShim(taker) == 0);
    fd = vprocOpenShim("/dev/stdin", O_RDONLY | O_TRUNC);
    assert(fd > STDERR_FILENO);
    assert(vprocCloseShim(fd) == 0);
    assert(pscalPathVirtualized_stat("/named.txt", &st) == 0 && st.st_size == 4);

    /* vprocOpenAt gives a slot, and vprocHostOpen a host descriptor of that file. */
    fd = vprocOpenAt(vp, "/dev/stdin", O_RDONLY, 0);
    assert(fd > STDERR_FILENO);
    assert(vprocCloseShim(fd) == 0);
    int host = vprocHostOpen("/dev/stdin", O_RDONLY);
    assert(host >= 0);
    assert(vprocHostClose(host) == 0);

    /* A closed descriptor's link leads nowhere. */
    assert(vprocClose(vp, STDERR_FILENO) == 0);
    errno = 0;
    assert(vprocOpenShim("/dev/stderr", O_WRONLY) == -1 && errno == ENOENT);
    errno = 0;
    assert(pscalPathVirtualized_stat("/dev/stderr", &st) == -1 && errno == ENOENT);
    errno = 0;
    assert(pscalPathVirtualized_access("/dev/stderr", F_OK) == -1 && errno == ENOENT);
    errno = 0;
    assert(vprocRealpathShim("/dev/stderr", NULL) == NULL && errno == ENOENT);
    errno = 0;
    assert(vprocHostOpen("/dev/stderr", O_WRONLY) == -1 && errno == ENOENT);
    assert(!vprocFdLinkTarget(pid, STDERR_FILENO, link, sizeof(link)));

    /* /proc/<pid>/fd/N is that vproc's descriptor, whoever asks. */
    VProc *other = vprocCreate(NULL);
    assert(other);
    int ends[2];
    assert(vprocPipe(other, ends) == 0);
    char other_name[64];
    snprintf(other_name, sizeof(other_name), "/proc/%d/fd/%d", vprocPid(other), ends[1]);
    assert(vprocPathNamedFd(other_name) == ends[1]);
    assert(pscalPathVirtualized_stat(other_name, &st) == 0 && S_ISFIFO(st.st_mode));
    int other_out = vprocOpenShim(other_name, O_WRONLY);
    assert(other_out >= 0);
    assert(vprocWriteShim(other_out, "z", 1) == 1);
    assert(vprocCloseShim(other_out) == 0);
    snprintf(other_name, sizeof(other_name), "/proc/%d/fd/%d", vprocPid(other), ends[0]);
    int other_in = vprocOpenShim(other_name, O_RDONLY);
    assert(other_in >= 0);
    assert(vprocReadShim(other_in, got, 1) == 1 && got[0] == 'z');
    assert(vprocCloseShim(other_in) == 0);
    snprintf(other_name, sizeof(other_name), "/proc/%d/fd/200", vprocPid(other));
    errno = 0;
    assert(vprocOpenShim(other_name, O_RDONLY) == -1 && errno == ENOENT);
    errno = 0;
    assert(pscalPathVirtualized_stat(other_name, &st) == -1 && errno == ENOENT);
    vprocDestroy(other);

    char stray[PATH_MAX];
    struct stat stray_st;
    const char *names[] = {"stdin", "stdout", "stderr"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        snprintf(stray, sizeof(stray), "%s/dev/%s", root, names[i]);
        assert((lstat)(stray, &stray_st) == 0 && S_ISLNK(stray_st.st_mode));
    }

    vprocDeactivate();
    vprocDestroy(vp);
    setenv("PATH_TRUNCATE", "/tmp", 1);
}

static void *inproc_pipe_read_thread(void *arg) {
    struct pscal_fd *fd = (struct pscal_fd *)arg;
    char c;
    (void)fd->ops->read(fd, &c, 1);
    return NULL;
}

static void *inproc_pipe_fill_thread(void *arg) {
    struct pscal_fd *fd = (struct pscal_fd *)arg;
    char buf[4096];
    memset(buf, 'x', sizeof(buf));
    while (fd->ops->write(fd, buf, sizeof(buf)) > 0) {
    }
    return NULL;
}

static void cancel_after_it_parks(void *(*start)(void *), struct pscal_fd *fd) {
    pthread_t tid;
    void *ret = NULL;
    assert(pthread_create(&tid, NULL, start, fd) == 0);
    usleep(300000);
    assert(pthread_cancel(tid) == 0);
    assert(pthread_join(tid, &ret) == 0);
    assert(ret == PTHREAD_CANCELED);
}

/* ssh a | ssh b: both ends poll with no timeout, and each read or write moves as much as the
 * pipe holds, so each end goes back to sleep at once. A wakeup the other end misses stops both for good. */
typedef struct {
    struct pscal_fd *fd;
    bool reader;
    size_t total;
    atomic_size_t moved;
    int wake_fd;
    atomic_int *finished;
} vproc_poll_pipe_ctx;

static void *vproc_poll_pipe_thread(void *arg) {
    vproc_poll_pipe_ctx *ctx = (vproc_poll_pipe_ctx *)arg;
    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    VProc *vp = vprocCreate(&opts);
    assert(vp);
    vprocActivate(vp);
    assert(vprocAdoptPscalFd(vp, STDIN_FILENO, ctx->fd) == 0);
    ctx->fd->flags |= O_NONBLOCK;
    ctx->wake_fd = pscalPollWakeFd();
    char buf[32 * 1024];
    memset(buf, 'x', sizeof(buf));
    struct pollfd p = {.fd = STDIN_FILENO, .events = ctx->reader ? POLLIN : POLLOUT};
    while (atomic_load(&ctx->moved) < ctx->total) {
        assert(vprocPollShim(&p, 1, -1) >= 0);
        ssize_t n = ctx->reader ? ctx->fd->ops->read(ctx->fd, buf, sizeof(buf))
                                : ctx->fd->ops->write(ctx->fd, buf, sizeof(buf));
        if (n > 0) {
            atomic_fetch_add(&ctx->moved, (size_t)n);
        }
    }
    atomic_fetch_add(ctx->finished, 1);
    vprocDeactivate();
    vprocDiscard(vprocPid(vp));
    vprocDestroy(vp);
    return NULL;
}

static void assert_two_pollers_on_a_pipe_keep_moving(void) {
    enum { THREADS = 2 };
    atomic_int finished = 0;
    vproc_poll_pipe_ctx ctx[THREADS];
    pthread_t tid[THREADS];
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);
    for (int i = 0; i < THREADS; ++i) {
        memset(&ctx[i], 0, sizeof(ctx[i]));
        ctx[i].fd = i == 0 ? read_end : write_end;
        ctx[i].reader = i == 0;
        ctx[i].total = 16u * 1024u * 1024u;
        ctx[i].finished = &finished;
    }
    for (int i = 0; i < THREADS; ++i) {
        assert(pthread_create(&tid[i], NULL, vproc_poll_pipe_thread, &ctx[i]) == 0);
    }
    /* A missed wakeup never ends, so this has a limit where the threads have none: ten
     * seconds with not a byte moving, however busy the machine. */
    size_t last_moved = 0;
    for (int idle = 0; atomic_load(&finished) < THREADS; ++idle) {
        size_t moved = atomic_load(&ctx[0].moved) + atomic_load(&ctx[1].moved);
        if (moved != last_moved) {
            last_moved = moved;
            idle = 0;
        }
        if (idle >= 200) {
            fprintf(stderr, "a poller missed its wakeup: %d of %d threads finished, %zu bytes moved\n",
                    atomic_load(&finished), THREADS, moved);
            assert(!"a poller missed its wakeup");
        }
        struct timespec nap = {.tv_sec = 0, .tv_nsec = 50000000};
        nanosleep(&nap, NULL);
    }
    for (int i = 0; i < THREADS; ++i) {
        assert(pthread_join(tid[i], NULL) == 0);
    }
    /* A thread's wake pipe goes with it, or every command would leave two descriptors. */
    for (int i = 0; i < THREADS; ++i) {
        errno = 0;
        assert(fcntl(ctx[i].wake_fd, F_GETFD) == -1);
        assert(errno == EBADF);
        pscal_fd_close(ctx[i].fd);
    }
}

/* A cancelled thread leaves a cond wait holding the mutex. A hang here is that
 * mutex: whoever has the other end of the pipe would wait for it for good. */
static void assert_cancelled_pipe_wait_leaves_pipe_usable(void) {
    struct pscal_fd *read_end = NULL;
    struct pscal_fd *write_end = NULL;
    assert(vprocCreateInprocPipe(&read_end, &write_end) == 0);

    cancel_after_it_parks(inproc_pipe_read_thread, read_end);
    char c = 'y';
    assert(write_end->ops->write(write_end, &c, 1) == 1);
    c = 0;
    assert(read_end->ops->read(read_end, &c, 1) == 1 && c == 'y');

    cancel_after_it_parks(inproc_pipe_fill_thread, write_end);
    char buf[4096];
    assert(read_end->ops->read(read_end, buf, sizeof(buf)) > 0);

    pscal_fd_close(read_end);
    pscal_fd_close(write_end);
}

int main(void) {
    /* Default truncation path for tests to keep path virtualization in /tmp. */
    setenv("PATH_TRUNCATE", "/tmp", 1);
    fprintf(stderr, "TEST virtual_control_signals_do_not_hit_host_process\n");
    assert_virtual_control_signals_do_not_hit_host_process();
    fprintf(stderr, "TEST pipe_round_trip\n");
    assert_pipe_round_trip();
    fprintf(stderr, "TEST snapshot_fits_its_buffer\n");
    assert_snapshot_fits_its_buffer();
    fprintf(stderr, "TEST closed_output_slot_stays_closed\n");
    assert_closed_output_slot_stays_closed();
    fprintf(stderr, "TEST simulated_fork_exec_inherits_nonstdio_fds\n");
    assert_simulated_fork_exec_inherits_nonstdio_fds();
    fprintf(stderr, "TEST simulated_fork_exec_socketpair_parent_fd_survives\n");
    assert_simulated_fork_exec_socketpair_parent_fd_survives();
    fprintf(stderr, "TEST redirected_stdio_is_closed_once\n");
    assert_redirected_stdio_is_closed_once();
    fprintf(stderr, "TEST stdio_replaced_by_inproc_pipe_is_closed_once\n");
    assert_stdio_replaced_by_inproc_pipe_is_closed_once();
    fprintf(stderr, "TEST replacing_stdin_leaves_no_descriptor\n");
    assert_replacing_stdin_leaves_no_descriptor();
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
    fprintf(stderr, "TEST tagged_host_fd_skips_the_slot\n");
    assert_tagged_host_fd_skips_the_slot();
    fprintf(stderr, "TEST fcntl_pread_pwrite_take_slots\n");
    assert_fcntl_pread_pwrite_take_slots();
    fprintf(stderr, "TEST fcntl_on_inproc_pipe\n");
    assert_fcntl_on_inproc_pipe();
    fprintf(stderr, "TEST fcntl_on_pty\n");
    assert_fcntl_on_pty();
    assert_terminate_session_by_id_goes_by_its_shell();
    fprintf(stderr, "TEST fstat_and_ttyname_on_slots\n");
    assert_fstat_and_ttyname_on_slots();
    fprintf(stderr, "TEST spawn_entry_runs_as_a_child\n");
    assert_spawn_entry_runs_as_a_child();
    fprintf(stderr, "TEST kill_ends_scopes_and_descendants\n");
    assert_kill_ends_scopes_and_descendants();
    fprintf(stderr, "TEST cancelled_terminal_read_leaves_terminal_usable\n");
    assert_cancelled_terminal_read_leaves_terminal_usable();
    fprintf(stderr, "TEST fcntl_keeps_session_host_stdin_blocking\n");
    assert_fcntl_keeps_session_host_stdin_blocking();
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
    fprintf(stderr, "TEST children_pass_to_parent\n");
    assert_children_pass_to_parent();
    fprintf(stderr, "TEST sigchld_pending_snapshot\n");
    assert_sigchld_pending_snapshot();
    fprintf(stderr, "TEST sigchld_pending_api\n");
    assert_sigchld_pending_api();
    fprintf(stderr, "TEST sigchld_unblock_drains_pending_signal\n");
    assert_sigchld_unblock_drains_pending_signal();
    fprintf(stderr, "TEST sigchld_aggregation_preserves_multi_child_reap\n");
    assert_sigchld_aggregation_preserves_multi_child_reap();
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
    fprintf(stderr, "TEST foreground_handoff_resumes_stopped_group\n");
    assert_foreground_handoff_resumes_stopped_group();
    fprintf(stderr, "TEST wait_nohang_transitions\n");
    assert_wait_nohang_transitions();
    fprintf(stderr, "TEST snapshot_lists_active_tasks\n");
    assert_snapshot_lists_active_tasks();
    fprintf(stderr, "TEST stop_and_continue_round_trip\n");
    assert_stop_and_continue_round_trip();
    fprintf(stderr, "TEST stop_and_continue_with_stdio_overrides\n");
    assert_stop_and_continue_with_stdio_overrides();
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
    fprintf(stderr, "TEST sigtimedwait_rejects_invalid_timeout\n");
    assert_sigtimedwait_rejects_invalid_timeout();
    fprintf(stderr, "TEST itimer_real_pending_when_blocked\n");
    assert_itimer_real_pending_when_blocked();
    fprintf(stderr, "TEST sigwait_wakes_for_itimer_real\n");
    assert_sigwait_wakes_for_itimer_real();
    fprintf(stderr, "TEST alarm_cancel_returns_remaining\n");
    assert_alarm_cancel_returns_remaining();
    fprintf(stderr, "TEST signal_handler_invoked\n");
    assert_signal_handler_invoked();
    fprintf(stderr, "TEST siginfo_handler_invoked\n");
    assert_siginfo_handler_invoked();
    fprintf(stderr, "TEST kill_does_not_self_cancel\n");
    assert_kill_does_not_self_cancel();
    fprintf(stderr, "TEST handler_waits_for_its_task\n");
    assert_handler_waits_for_its_task();
    fprintf(stderr, "TEST unwinding_task_is_not_cancelled\n");
    assert_unwinding_task_is_not_cancelled();
    fprintf(stderr, "TEST sigcont_continues_whatever_its_disposition\n");
    assert_sigcont_continues_whatever_its_disposition();
    fprintf(stderr, "TEST reset_caught_signals\n");
    assert_reset_caught_signals();
    fprintf(stderr, "TEST reused_slot_starts_clean\n");
    assert_reused_slot_starts_clean();
    fprintf(stderr, "TEST getppid_passes_over_what_is_no_process\n");
    assert_getppid_passes_over_what_is_no_process();
    fprintf(stderr, "TEST cancelled_wait_lets_go_of_task_table\n");
    assert_cancelled_wait_lets_go_of_task_table();
    fprintf(stderr, "TEST cancel_pending_lets_go_of_vproc_lock\n");
    assert_cancel_pending_lets_go_of_vproc_lock();
    fprintf(stderr, "TEST reaped_job_stays_gone\n");
    assert_reaped_job_stays_gone();
#if defined(__APPLE__)
    fprintf(stderr, "TEST snapshot_leaves_an_ended_threads_port_name_alone\n");
    assert_snapshot_leaves_an_ended_threads_port_name_alone();
#endif
    fprintf(stderr, "TEST kill_ends_what_the_thread_made\n");
    assert_kill_ends_what_the_thread_made();
    fprintf(stderr, "TEST cancelled_read_lets_go_of_its_descriptor\n");
    assert_cancelled_read_lets_go_of_its_descriptor();
    fprintf(stderr, "TEST sigkill_not_blockable\n");
    assert_sigkill_not_blockable();
    fprintf(stderr, "TEST sigstop_not_ignorable_or_blockable\n");
    assert_sigstop_not_ignorable_or_blockable();
    fprintf(stderr, "TEST background_tty_signals\n");
    assert_background_tty_signals();

    fprintf(stderr, "TEST background_read_of_a_file_does_not_stop\n");
    assert_background_read_of_a_file_does_not_stop();
    fprintf(stderr, "TEST background_read_of_session_stdin_stops\n");
    assert_background_read_of_session_stdin_stops();
    fprintf(stderr, "TEST background_read_of_a_socket_does_not_stop\n");
    assert_background_read_of_a_socket_does_not_stop();
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
    fprintf(stderr, "TEST pid_hint_not_self_parented\n");
    assert_pid_hint_not_self_parented();
    fprintf(stderr, "TEST reparenting_uses_sid\n");
    assert_reparenting_uses_session_leader_sid();
    fprintf(stderr, "TEST orphan_rows_go_when_they_end\n");
    assert_orphan_rows_go_when_they_end();
    fprintf(stderr, "TEST terminate_session_discards_entries\n");
    assert_terminate_session_discards_entries();
    fprintf(stderr, "TEST pthread_inherits_session_ids\n");
    assert_pthread_inherits_session_ids();
    fprintf(stderr, "TEST thread_starting_after_creator_ended\n");
    assert_thread_starting_after_creator_ended();
    fprintf(stderr, "TEST spawned_thread_outlives_creator\n");
    assert_spawned_thread_outlives_creator();
    fprintf(stderr, "TEST kill_reaches_spawned_thread_before_it_runs\n");
    assert_kill_reaches_spawned_thread_before_it_runs();
    fprintf(stderr, "TEST replace_stdio_keeps_no_copy\n");
    assert_replace_stdio_keeps_no_copy();
    fprintf(stderr, "TEST setpgid_zero_defaults_to_pid\n");
    assert_setpgid_zero_defaults_to_pid();
    fprintf(stderr, "TEST path_truncate_maps_to_sandbox\n");
    assert_path_truncate_maps_to_sandbox();
    fprintf(stderr, "TEST path_truncate_mount_rewrites_virtual_paths\n");
    assert_path_truncate_mount_rewrites_virtual_paths();
    fprintf(stderr, "TEST path_truncate_mount_remove_unmaps_virtual_paths\n");
    assert_path_truncate_mount_remove_unmaps_virtual_paths();
    fprintf(stderr, "TEST proc_mount_files_include_virtual_mounts\n");
    assert_proc_mount_files_include_virtual_mounts();
    fprintf(stderr, "TEST proc_vm_files_present_and_stable\n");
    assert_proc_vm_files_present_and_stable();
    fprintf(stderr, "TEST proc_core_and_net_entries_present\n");
    assert_proc_core_and_net_entries_present();
    fprintf(stderr, "TEST proc_sys_entries_present\n");
    assert_proc_sys_entries_present();
    fprintf(stderr, "TEST proc_misc_entries_present\n");
    assert_proc_misc_entries_present();
    fprintf(stderr, "TEST proc_self_entries_present\n");
    assert_proc_self_entries_present();
    fprintf(stderr, "TEST proc_fd_lists_the_vprocs_slots\n");
    assert_proc_fd_lists_the_vprocs_slots();
    fprintf(stderr, "TEST proc_task_entries_present\n");
    assert_proc_task_entries_present();
    fprintf(stderr, "TEST proc_device_entries_present\n");
    assert_proc_device_entries_present();
    fprintf(stderr, "TEST proc_device_self_survives_full_snapshot\n");
    assert_proc_device_self_survives_full_snapshot();
    fprintf(stderr, "TEST proc_pid_namespace_and_device_namespace_separated\n");
    assert_proc_pid_namespace_and_device_namespace_separated();
    fprintf(stderr, "TEST proc_root_listing_latency_regression\n");
    assert_proc_root_listing_latency_regression();
    fprintf(stderr, "TEST path_virtualized_glob_matches_dynamic_proc_entries\n");
    assert_path_virtualized_glob_matches_dynamic_proc_entries();
    fprintf(stderr, "TEST vproc_cwd_isolated_between_vprocs\n");
    assert_vproc_cwd_isolated_between_vprocs();
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
    fprintf(stderr, "TEST fcntl_on_location_device\n");
    assert_fcntl_on_location_device();
    fprintf(stderr, "TESTselect_sparse_fdset_works\n");
    assert_select_sparse_fdset_works();
    fprintf(stderr, "TEST select_empty_set_honors_timeout\n");
    assert_select_empty_set_honors_timeout();
    fprintf(stderr, "TEST select_rejects_oversize_fdset\n");
    assert_select_rejects_oversize_fdset();
    fprintf(stderr, "TEST select_rejects_invalid_timeval\n");
    assert_select_rejects_invalid_timeval();
    fprintf(stderr, "TEST location_disable_unblocks_and_errors\n");
    assert_location_disable_unblocks_and_errors();
    fprintf(stderr, "TEST location_reader_observer_fires\n");
    assert_location_reader_observer_fires();
    fprintf(stderr, "TEST device_stat_bypasses_truncation\n");
    assert_device_stat_bypasses_truncation();
    fprintf(stderr, "TEST ptmx_open_registers_session\n");
    assert_ptmx_open_registers_session();
    fprintf(stderr, "TEST session_output_handler_delayed_attach_receives_pending_output\n");
    assert_session_output_handler_delayed_attach_receives_pending_output();
    fprintf(stderr, "TEST session_output_handler_burst_tabs\n");
    assert_session_output_handler_burst_tabs();
    fprintf(stderr, "TEST session_output_pause_resume_flushes_backlog\n");
    assert_session_output_pause_resume_flushes_backlog();
    fprintf(stderr, "TEST session_write_to_master_nonblocking_respects_capacity\n");
    assert_session_write_to_master_nonblocking_respects_capacity();
    fprintf(stderr, "TEST session_input_inject_read_queue\n");
    assert_session_input_inject_read_queue();
    fprintf(stderr, "TEST session_input_reader_ignores_transient_pty_zero_read\n");
    assert_session_input_reader_ignores_transient_pty_zero_read();
    fprintf(stderr, "TEST scp_like_prompt_survives_first_character_input\n");
    assert_scp_like_prompt_survives_first_character_input();
    fprintf(stderr, "TEST scp_like_prompt_survives_transient_eio_after_first_character\n");
    assert_scp_like_prompt_survives_transient_eio_after_first_character();
    fprintf(stderr, "TEST session_control_chars_route_to_shell_input_when_shell_foreground\n");
    assert_session_control_chars_route_to_shell_input_when_shell_foreground();
    fprintf(stderr, "TEST session_ctrl_fallback_byte_remains_readable_without_forced_interrupt\n");
    assert_session_ctrl_fallback_byte_remains_readable_without_forced_interrupt();
    fprintf(stderr, "TEST session_ctrl_c_dispatches_to_foreground_job_when_not_shell_foreground\n");
    assert_session_ctrl_c_dispatches_to_foreground_job_when_not_shell_foreground();
    fprintf(stderr, "TEST session_ctrl_z_stops_foreground_job_when_not_shell_foreground\n");
    assert_session_ctrl_z_stops_foreground_job_when_not_shell_foreground();
    fprintf(stderr, "TEST session_ctrl_z_then_ctrl_c_stop_unsupported_foreground_job\n");
    assert_session_ctrl_z_then_ctrl_c_stop_unsupported_foreground_job();
    fprintf(stderr, "TEST session_ctrl_c_dispatches_to_frontend_like_foreground_group\n");
    assert_session_ctrl_c_dispatches_to_frontend_like_foreground_group();
    fprintf(stderr, "TEST session_ctrl_z_dispatches_to_frontend_like_foreground_group\n");
    assert_session_ctrl_z_dispatches_to_frontend_like_foreground_group();
    fprintf(stderr, "TEST session_ctrl_z_then_ctrl_c_stop_unsupported_frontend_group\n");
    assert_session_ctrl_z_then_ctrl_c_stop_unsupported_frontend_group();
    fprintf(stderr, "TEST session_ctrl_c_does_not_bleed_between_sessions\n");
    assert_session_ctrl_c_does_not_bleed_between_sessions();
    fprintf(stderr, "TEST session_ctrl_z_does_not_bleed_between_sessions\n");
    assert_session_ctrl_z_does_not_bleed_between_sessions();
    fprintf(stderr, "TEST session_cwd_does_not_bleed_between_reused_worker_contexts\n");
    assert_session_cwd_does_not_bleed_between_reused_worker_contexts();
    fprintf(stderr, "TEST path_virtualized_chdir_updates_session_cwd_when_interpose_bypassed\n");
    assert_path_virtualized_chdir_updates_session_cwd_when_interpose_bypassed();
    fprintf(stderr, "TEST vproc_chdir_accepts_expanded_container_path\n");
    assert_vproc_chdir_accepts_expanded_container_path();
    fprintf(stderr, "TEST path_virtualized_relative_ops_follow_session_cwd\n");
    assert_path_virtualized_relative_ops_follow_session_cwd();
    fprintf(stderr, "TEST session_cwd_prefers_active_session_over_stale_thread_hint\n");
    assert_session_cwd_prefers_active_session_over_stale_thread_hint();
    fprintf(stderr, "TEST shell_cwd_apis_target_shell_vproc_with_worker_active\n");
    assert_shell_cwd_apis_target_shell_vproc_with_worker_active();
    fprintf(stderr, "TEST runtime_request_ctrl_c_dispatches_to_foreground_job\n");
    assert_runtime_request_ctrl_c_dispatches_to_foreground_job();
    fprintf(stderr, "TEST runtime_request_ctrl_z_stops_foreground_job\n");
    assert_runtime_request_ctrl_z_stops_foreground_job();
    fprintf(stderr, "TEST runtime_request_ctrl_c_dispatches_with_explicit_shell_pid\n");
    assert_runtime_request_ctrl_c_dispatches_with_explicit_shell_pid();
    fprintf(stderr, "TEST runtime_request_ctrl_z_stops_with_explicit_shell_pid\n");
    assert_runtime_request_ctrl_z_stops_with_explicit_shell_pid();
    fprintf(stderr, "TEST runtime_request_ctrl_c_dispatches_with_explicit_session_id\n");
    assert_runtime_request_ctrl_c_dispatches_with_explicit_session_id();
    fprintf(stderr, "TEST runtime_request_ctrl_z_stops_with_explicit_session_id\n");
    assert_runtime_request_ctrl_z_stops_with_explicit_session_id();
    fprintf(stderr, "TEST runtime_request_ctrl_c_ends_ssh_without_passthrough\n");
    assert_runtime_request_ctrl_c_ends_ssh_without_passthrough();
    fprintf(stderr, "TEST runtime_request_ctrl_signals_defer_for_remote_foreground_session\n");
    assert_runtime_request_ctrl_signals_defer_for_remote_foreground_session();
    fprintf(stderr, "TEST runtime_request_ctrl_signals_defer_when_session_passthrough\n");
    assert_runtime_request_ctrl_signals_defer_when_session_passthrough();
    fprintf(stderr, "TEST runtime_request_ctrl_signals_defer_when_soft_disabled_env\n");
    assert_runtime_request_ctrl_signals_defer_when_soft_disabled_env();
    fprintf(stderr, "TEST sigint_runtime_callback_reenters_without_deadlock\n");
    assert_sigint_runtime_callback_reenters_without_deadlock();
    fprintf(stderr, "TEST sigtstp_runtime_callback_reenters_without_deadlock\n");
    assert_sigtstp_runtime_callback_reenters_without_deadlock();
    fprintf(stderr, "TEST sigtstp_runtime_callback_foreground_reentry_is_single_shot\n");
    assert_sigtstp_runtime_callback_foreground_reentry_is_single_shot();
    fprintf(stderr, "TEST sigtstp_non_shell_same_tid_does_not_request_runtime_suspend\n");
    assert_sigtstp_non_shell_same_tid_does_not_request_runtime_suspend();
    fprintf(stderr, "TEST command_scope_from_shell_is_stoppable\n");
    assert_command_scope_from_shell_is_stoppable();
    fprintf(stderr, "TEST command_scope_end_preserves_stop_status\n");
    assert_command_scope_end_preserves_stop_status();
    fprintf(stderr, "TEST stop_unsupported_sigtstp_queues_pending_signal\n");
    assert_stop_unsupported_sigtstp_queues_pending_signal();
    fprintf(stderr, "TEST sigint_non_shell_same_tid_does_not_request_runtime_interrupt\n");
    assert_sigint_non_shell_same_tid_does_not_request_runtime_interrupt();
    fprintf(stderr, "TEST destroy_leaves_replaced_stdio_number_alone\n");
    assert_destroy_leaves_replaced_stdio_number_alone();
    fprintf(stderr, "TEST named_descriptors_act_as_on_linux\n");
    assert_named_descriptors_act_as_on_linux();
    fprintf(stderr, "TEST stream_end_with_thread\n");
    assert_stream_end_with_thread();
#if defined(__APPLE__)
    fprintf(stderr, "TEST poll_scratch_freed_at_thread_end\n");
    assert_poll_scratch_freed_at_thread_end();
#endif
    fprintf(stderr, "TEST cancelled_pipe_wait_leaves_pipe_usable\n");
    assert_cancelled_pipe_wait_leaves_pipe_usable();
    fprintf(stderr, "TEST two_pollers_on_a_pipe_keep_moving\n");
    assert_two_pollers_on_a_pipe_keep_moving();
    fprintf(stderr, "TEST kernel_mounts_from_fstab_at_startup\n");
    assert_kernel_mounts_from_fstab_at_startup();
    fprintf(stderr, "TEST empty_path_names_nothing\n");
    assert_empty_path_names_nothing();
    fprintf(stderr, "TEST vproc_stack_has_no_limit\n");
    assert_vproc_stack_has_no_limit();
#if defined(__APPLE__)
    fprintf(stderr, "TEST setlocale_is_the_threads_own\n");
    assert_setlocale_is_the_threads_own();
    fprintf(stderr, "TEST env_capture_survives_changes\n");
    assert_env_capture_survives_changes();
#endif
#if defined(PSCAL_TARGET_IOS)
    /* Ensure path virtualization macros remain visible even when vproc shim is included. */
    int (*fn)(const char *) = chdir;
    (void)fn;
#endif
    return 0;
}
