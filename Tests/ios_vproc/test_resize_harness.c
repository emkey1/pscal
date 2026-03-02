#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "common/runtime_tty.h"
#include "ios/vproc.h"
#include "ios/tty/pscal_pty.h"
#include "ios/tty/pscal_tty.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

typedef struct {
    uint64_t session_id;
    VProcSessionStdio *session_stdio;
    VProc *shell_vp;
    int previous_shell_pid;
} ResizeHarnessContext;

typedef struct {
    int set_rc;
    int signal_rc;
} ResizeDispatchResult;

typedef struct {
    uint64_t bound_session_id;
    int deferred_cols;
    int deferred_rows;
    bool has_deferred;
    int runtime_cols;
    int runtime_rows;
    bool has_runtime_override;
} FrontendResizeRelay;

static ResizeDispatchResult runtimeLikeResizeRequest(uint64_t session_id, int cols, int rows, pthread_t shell_thread) {
    ResizeDispatchResult result;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", cols);
    setenv("COLUMNS", buf, 1);
    snprintf(buf, sizeof(buf), "%d", rows);
    setenv("LINES", buf, 1);
    result.set_rc = vprocSetSessionWinsize(session_id, cols, rows);
    result.signal_rc = -1;
    if (result.set_rc == 0 && shell_thread) {
        result.signal_rc = pthread_kill(shell_thread, SIGWINCH);
    }
    return result;
}

static void frontendResizeRelayInit(FrontendResizeRelay *relay) {
    assert(relay);
    memset(relay, 0, sizeof(*relay));
}

static void frontendResizeRelayRecordRuntimeUpdate(FrontendResizeRelay *relay,
                                                   int cols,
                                                   int rows) {
    assert(relay);
    if (cols <= 0 || rows <= 0) {
        return;
    }
    relay->runtime_cols = cols;
    relay->runtime_rows = rows;
    relay->has_runtime_override = true;
}

static bool frontendResizeRelayHandleNative(FrontendResizeRelay *relay,
                                            int cols,
                                            int rows,
                                            pthread_t shell_thread,
                                            ResizeDispatchResult *out_dispatch) {
    assert(relay);
    if (out_dispatch) {
        out_dispatch->set_rc = -1;
        out_dispatch->signal_rc = -1;
    }
    if (cols <= 0 || rows <= 0) {
        return false;
    }
    if (relay->bound_session_id == 0) {
        relay->deferred_cols = cols;
        relay->deferred_rows = rows;
        relay->has_deferred = true;
        return false;
    }
    ResizeDispatchResult dispatch = runtimeLikeResizeRequest(relay->bound_session_id, cols, rows, shell_thread);
    if (out_dispatch) {
        *out_dispatch = dispatch;
    }
    return true;
}

static bool frontendResizeRelayBindSession(FrontendResizeRelay *relay,
                                           uint64_t session_id,
                                           pthread_t shell_thread,
                                           ResizeDispatchResult *out_dispatch) {
    assert(relay);
    if (out_dispatch) {
        out_dispatch->set_rc = -1;
        out_dispatch->signal_rc = -1;
    }
    relay->bound_session_id = session_id;
    if (session_id == 0 || !relay->has_deferred) {
        return false;
    }
    if (relay->has_runtime_override &&
        (relay->runtime_cols != relay->deferred_cols ||
         relay->runtime_rows != relay->deferred_rows)) {
        relay->has_deferred = false;
        relay->deferred_cols = 0;
        relay->deferred_rows = 0;
        return false;
    }
    ResizeDispatchResult dispatch = runtimeLikeResizeRequest(session_id,
                                                             relay->deferred_cols,
                                                             relay->deferred_rows,
                                                             shell_thread);
    relay->has_deferred = false;
    relay->deferred_cols = 0;
    relay->deferred_rows = 0;
    if (out_dispatch) {
        *out_dispatch = dispatch;
    }
    return true;
}

static void resizeHarnessInitSession(ResizeHarnessContext *ctx, uint64_t session_id) {
    assert(ctx);
    memset(ctx, 0, sizeof(*ctx));
    ctx->session_id = session_id;
    ctx->previous_shell_pid = vprocGetShellSelfPid();

    struct pscal_fd *master = NULL;
    struct pscal_fd *slave = NULL;
    int pty_num = -1;
    assert(pscalPtyOpenMaster(O_RDWR, &master, &pty_num) == 0);
    assert(pscalPtyUnlock(master) == 0);
    assert(pscalPtyOpenSlave(pty_num, O_RDWR, &slave) == 0);

    ctx->session_stdio = vprocSessionStdioCreate();
    assert(ctx->session_stdio);
    assert(vprocSessionStdioInitWithPty(ctx->session_stdio, slave, master, session_id, 0) == 0);
    vprocSessionStdioActivate(ctx->session_stdio);

    VProcOptions shell_opts = vprocDefaultOptions();
    shell_opts.pid_hint = vprocReservePid();
    ctx->shell_vp = vprocCreate(&shell_opts);
    assert(ctx->shell_vp);
    int shell_pid = vprocPid(ctx->shell_vp);
    assert(shell_pid > 0);

    vprocActivate(ctx->shell_vp);
    assert(vprocAdoptPscalStdio(ctx->shell_vp,
                                ctx->session_stdio->stdin_pscal_fd,
                                ctx->session_stdio->stdout_pscal_fd,
                                ctx->session_stdio->stderr_pscal_fd) == 0);
    vprocSetShellSelfPid(shell_pid);
    vprocSetShellSelfTid(pthread_self());
    assert(vprocRegisterTidHint(shell_pid, pthread_self()) == shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);
}

static void resizeHarnessDestroySession(ResizeHarnessContext *ctx) {
    assert(ctx);
    if (ctx->shell_vp) {
        vprocDeactivate();
        vprocDestroy(ctx->shell_vp);
        ctx->shell_vp = NULL;
    }
    if (ctx->session_stdio) {
        vprocSessionStdioActivate(NULL);
        vprocSessionStdioDestroy(ctx->session_stdio);
        ctx->session_stdio = NULL;
    }
    if (ctx->previous_shell_pid > 0) {
        vprocSetShellSelfPid(ctx->previous_shell_pid);
    }
}

static void resizeHarnessAssertSessionSize(uint64_t session_id, int expected_cols, int expected_rows) {
    int session_cols = 0;
    int session_rows = 0;
    assert(vprocGetSessionWinsize(session_id, &session_cols, &session_rows) == 0);
    assert(session_cols == expected_cols);
    assert(session_rows == expected_rows);

    struct winsize_ tty_ws;
    memset(&tty_ws, 0, sizeof(tty_ws));
    assert(vprocIoctlShim(STDOUT_FILENO, TIOCGWINSZ_, &tty_ws) == 0);
    assert((int)tty_ws.col == expected_cols);
    assert((int)tty_ws.row == expected_rows);
}

static void resizeHarnessAssertRuntimeDetectedSize(int expected_cols, int expected_rows) {
    int detected_cols = pscalRuntimeDetectWindowCols();
    int detected_rows = pscalRuntimeDetectWindowRows();
    assert(detected_cols == expected_cols);
    assert(detected_rows == expected_rows);
}

static void assert_micro_pipe_probe_follows_env(void) {
    int saved_stdout = dup(STDOUT_FILENO);
    assert(saved_stdout >= 0);
    int p[2] = {-1, -1};
    assert(pipe(p) == 0);
    assert(dup2(p[1], STDOUT_FILENO) == STDOUT_FILENO);

    setenv("COLUMNS", "155", 1);
    setenv("LINES", "47", 1);
    int detected_cols = pscalRuntimeDetectWindowCols();
    int detected_rows = pscalRuntimeDetectWindowRows();
    assert(detected_cols == 155);
    assert(detected_rows == 47);
    fprintf(stderr,
            "[editor-probe] target=micro mode=pipe-env cols=%d rows=%d\n",
            detected_cols,
            detected_rows);

    assert(dup2(saved_stdout, STDOUT_FILENO) == STDOUT_FILENO);
    close(saved_stdout);
    close(p[0]);
    close(p[1]);
}

static void assert_resize_request_apply_and_dispatch(void) {
    ResizeHarnessContext ctx;
    resizeHarnessInitSession(&ctx, 9701);

    const int requested_sizes[][2] = {
        {121, 37},
        {132, 38},
        {105, 32},
        {142, 41},
    };
    const size_t request_count = sizeof(requested_sizes) / sizeof(requested_sizes[0]);
    for (size_t i = 0; i < request_count; ++i) {
        const int cols = requested_sizes[i][0];
        const int rows = requested_sizes[i][1];
        ResizeDispatchResult dispatch = runtimeLikeResizeRequest(ctx.session_id, cols, rows, pthread_self());
        assert(dispatch.set_rc == 0);
        assert(dispatch.signal_rc == 0);
        resizeHarnessAssertSessionSize(ctx.session_id, cols, rows);
        resizeHarnessAssertRuntimeDetectedSize(cols, rows);
        fprintf(stderr,
                "[resize-harness] step=%zu request=%dx%d set_rc=%d signal_rc=%d session=%llu\n",
                i + 1,
                cols,
                rows,
                dispatch.set_rc,
                dispatch.signal_rc,
                (unsigned long long)ctx.session_id);
        fprintf(stderr,
                "[editor-probe] target=nextvi mode=pty-runtime cols=%d rows=%d\n",
                cols,
                rows);
    }

    ResizeDispatchResult invalid = runtimeLikeResizeRequest(0, 99, 33, pthread_self());
    assert(invalid.set_rc != 0);
    assert(invalid.signal_rc == -1);

    resizeHarnessDestroySession(&ctx);
}

static void assert_deferred_resize_before_pty_bind_is_replayed(void) {
    const uint64_t deferred_session = 9717;
    const int deferred_cols = 149;
    const int deferred_rows = 46;

    ResizeDispatchResult dispatch = runtimeLikeResizeRequest(deferred_session,
                                                             deferred_cols,
                                                             deferred_rows,
                                                             (pthread_t)0);
    assert(dispatch.set_rc != 0);
    assert(dispatch.signal_rc == -1);

    /* The runtime path still updates env even if the PTY has not been bound. */
    resizeHarnessAssertRuntimeDetectedSize(deferred_cols, deferred_rows);

    ResizeHarnessContext ctx;
    resizeHarnessInitSession(&ctx, deferred_session);
    resizeHarnessAssertSessionSize(deferred_session, deferred_cols, deferred_rows);
    fprintf(stderr,
            "[resize-harness] deferred-bind session=%llu request=%dx%d replayed=1\n",
            (unsigned long long)deferred_session,
            deferred_cols,
            deferred_rows);
    resizeHarnessDestroySession(&ctx);
}

static void assert_frontend_deferred_rebind_flow(void) {
    FrontendResizeRelay relay;
    frontendResizeRelayInit(&relay);

    ResizeDispatchResult dispatch;

    /* Native resize events while hterm is unbound are deferred. */
    bool forwarded = frontendResizeRelayHandleNative(&relay, 121, 37, pthread_self(), &dispatch);
    assert(!forwarded);
    assert(dispatch.set_rc == -1);
    assert(dispatch.signal_rc == -1);

    forwarded = frontendResizeRelayHandleNative(&relay, 142, 41, pthread_self(), &dispatch);
    assert(!forwarded);
    assert(dispatch.set_rc == -1);
    assert(dispatch.signal_rc == -1);

    ResizeHarnessContext ctx;
    resizeHarnessInitSession(&ctx, 9724);

    /* Binding to a real session should replay the latest deferred size. */
    bool replayed = frontendResizeRelayBindSession(&relay, ctx.session_id, pthread_self(), &dispatch);
    assert(replayed);
    assert(dispatch.set_rc == 0);
    assert(dispatch.signal_rc == 0);
    resizeHarnessAssertSessionSize(ctx.session_id, 142, 41);

    /* Subsequent live resize should forward immediately while bound. */
    forwarded = frontendResizeRelayHandleNative(&relay, 132, 38, pthread_self(), &dispatch);
    assert(forwarded);
    assert(dispatch.set_rc == 0);
    assert(dispatch.signal_rc == 0);
    resizeHarnessAssertSessionSize(ctx.session_id, 132, 38);

    /* If hterm detaches, resize defers again, then rebind replays latest. */
    replayed = frontendResizeRelayBindSession(&relay, 0, pthread_self(), &dispatch);
    assert(!replayed);
    forwarded = frontendResizeRelayHandleNative(&relay, 155, 48, pthread_self(), &dispatch);
    assert(!forwarded);
    replayed = frontendResizeRelayBindSession(&relay, ctx.session_id, pthread_self(), &dispatch);
    assert(replayed);
    assert(dispatch.set_rc == 0);
    assert(dispatch.signal_rc == 0);
    resizeHarnessAssertSessionSize(ctx.session_id, 155, 48);

    fprintf(stderr,
            "[resize-harness] frontend-defer-rebind session=%llu replayed-latest=%dx%d\n",
            (unsigned long long)ctx.session_id,
            155,
            48);
    resizeHarnessDestroySession(&ctx);
}

static void assert_frontend_stale_deferred_replay_is_skipped(void) {
    FrontendResizeRelay relay;
    frontendResizeRelayInit(&relay);
    ResizeDispatchResult dispatch;

    ResizeHarnessContext ctx;
    resizeHarnessInitSession(&ctx, 9731);

    dispatch = runtimeLikeResizeRequest(ctx.session_id, 132, 38, pthread_self());
    assert(dispatch.set_rc == 0);
    assert(dispatch.signal_rc == 0);
    resizeHarnessAssertSessionSize(ctx.session_id, 132, 38);
    frontendResizeRelayRecordRuntimeUpdate(&relay, 132, 38);

    bool replayed = frontendResizeRelayBindSession(&relay, 0, pthread_self(), &dispatch);
    assert(!replayed);
    bool forwarded = frontendResizeRelayHandleNative(&relay, 86, 25, pthread_self(), &dispatch);
    assert(!forwarded);

    replayed = frontendResizeRelayBindSession(&relay, ctx.session_id, pthread_self(), &dispatch);
    assert(!replayed);
    assert(dispatch.set_rc == -1);
    assert(dispatch.signal_rc == -1);
    resizeHarnessAssertSessionSize(ctx.session_id, 132, 38);

    fprintf(stderr,
            "[resize-harness] frontend-stale-replay-skipped session=%llu kept=%dx%d deferred=%dx%d\n",
            (unsigned long long)ctx.session_id,
            132,
            38,
            86,
            25);
    resizeHarnessDestroySession(&ctx);
}

int main(void) {
    setenv("PATH_TRUNCATE", "/tmp", 1);
    fprintf(stderr, "TEST resize_request_apply_and_dispatch\n");
    assert_resize_request_apply_and_dispatch();
    fprintf(stderr, "TEST deferred_resize_before_pty_bind_is_replayed\n");
    assert_deferred_resize_before_pty_bind_is_replayed();
    fprintf(stderr, "TEST frontend_deferred_rebind_flow\n");
    assert_frontend_deferred_rebind_flow();
    fprintf(stderr, "TEST frontend_stale_deferred_replay_is_skipped\n");
    assert_frontend_stale_deferred_replay_is_skipped();
    fprintf(stderr, "TEST micro_pipe_probe_follows_env\n");
    assert_micro_pipe_probe_follows_env();
    fprintf(stderr, "PASS ios resize harness\n");
    return 0;
}
