#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ios/tty/pscal_fd.h"
#include "ios/vproc.h"

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

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
    bool started;
    bool done;
    int rc;
    char pass[64];
} scp_like_prompt_ctx;

static void *scp_like_prompt_thread(void *arg) {
    scp_like_prompt_ctx *ctx = (scp_like_prompt_ctx *)arg;
    pthread_mutex_lock(&ctx->mu);
    ctx->started = true;
    pthread_mutex_unlock(&ctx->mu);

    int rc = -1;
    size_t len = 0;
    char out[sizeof(ctx->pass)];
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
    }
    ctx->rc = rc;
    ctx->done = true;
    pthread_mutex_unlock(&ctx->mu);
    return NULL;
}

static bool wait_started(scp_like_prompt_ctx *ctx, int timeout_ms) {
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

static bool wait_done(scp_like_prompt_ctx *ctx, int timeout_ms) {
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

static bool is_done(scp_like_prompt_ctx *ctx) {
    pthread_mutex_lock(&ctx->mu);
    bool done = ctx->done;
    pthread_mutex_unlock(&ctx->mu);
    return done;
}

static void read_exact_from_stdin_shim(char *out, size_t len) {
    size_t got = 0;
    while (got < len) {
        ssize_t rd = vprocReadShim(STDIN_FILENO, out + got, len - got);
        assert(rd > 0);
        got += (size_t)rd;
    }
}

int main(void) {
    fprintf(stderr, "TEST scp_prompt_first_char_does_not_terminate\n");

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

    VProc *shell_vp = vprocCreate(NULL);
    assert(shell_vp);
    int shell_pid = vprocPid(shell_vp);
    vprocSetShellSelfPid(shell_pid);
    assert(vprocSetSid(shell_pid, shell_pid) == 0);
    assert(vprocSetPgid(shell_pid, shell_pid) == 0);
    assert(vprocSetForegroundPgid(shell_pid, shell_pid) == 0);

    VProc *child_vp = vprocCreate(NULL);
    assert(child_vp);
    vprocSetParent(vprocPid(child_vp), shell_pid);
    assert(vprocSetSid(vprocPid(child_vp), shell_pid) == 0);
    assert(vprocSetPgid(vprocPid(child_vp), vprocPid(child_vp)) == 0);

    scp_like_prompt_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    assert(pthread_mutex_init(&ctx.mu, NULL) == 0);
    ctx.started = false;
    ctx.done = false;
    ctx.rc = -1;
    ctx.pass[0] = '\0';

    pthread_t prompt_thread = 0;
    assert(vprocSpawnThread(child_vp, scp_like_prompt_thread, &ctx, &prompt_thread) == 0);

    assert(wait_started(&ctx, 500));
    usleep(50000);
    assert(!is_done(&ctx));

    static const unsigned char first[] = {'s'};
    interactive_input_push(stdin_fd, first, sizeof(first));
    usleep(50000);
    assert(!is_done(&ctx));

    static const unsigned char rest[] = {'e', 'c', 'r', 'e', 't', '\n'};
    interactive_input_push(stdin_fd, rest, sizeof(rest));
    assert(wait_done(&ctx, 1500));

    pthread_join(prompt_thread, NULL);
    pthread_mutex_lock(&ctx.mu);
    assert(ctx.rc == 0);
    assert(strcmp(ctx.pass, "secret") == 0);
    pthread_mutex_unlock(&ctx.mu);

    /* Regression guard: after prompt flow, shell stdin reads must not compete
     * with the session input reader thread (no alternating/drop behavior). */
    static const unsigned char probe[] = {'p', 'i', 'n', 'g', '\n'};
    interactive_input_push(stdin_fd, probe, sizeof(probe));
    usleep(50000); /* let session reader drain source into shared queue */
    vprocActivate(shell_vp);
    char probe_out[sizeof(probe) + 1];
    memset(probe_out, 0, sizeof(probe_out));
    read_exact_from_stdin_shim(probe_out, sizeof(probe));
    vprocDeactivate();
    assert(memcmp(probe_out, probe, sizeof(probe)) == 0);

    pthread_mutex_destroy(&ctx.mu);
    vprocDestroy(child_vp);
    vprocDestroy(shell_vp);
    session_input_stop_reader(input);
    vprocSessionStdioDestroy(session);

    fprintf(stderr, "scp prompt regression: passed\n");
    return 0;
}
