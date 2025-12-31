#define PSCAL_IOS_SHIM_IMPLEMENTATION
#include "pscal_ios_shim.h"

#ifdef PSCAL_TARGET_IOS

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "common/runtime_tty.h"
#include "common/path_truncate.h"
#include "ios/vproc.h"

int pscal_openssh_ssh_main(int argc, char **argv);
int pscal_openssh_scp_main(int argc, char **argv);
int pscal_openssh_sftp_main(int argc, char **argv);
int pscalVprocTestChildMain(int argc, char **argv);

typedef struct {
    int fd;
    int writer;
    bool active;
    struct termios term;
} pscal_ios_virtual_tty;

static pthread_mutex_t g_pscal_ios_tty_lock = PTHREAD_MUTEX_INITIALIZER;
static pscal_ios_virtual_tty g_pscal_ios_virtual_ttys[8];

static int pscal_ios_translate_fd(int fd);

typedef struct {
    bool active;
    bool in_child;
    sigjmp_buf parent_env;
    VProc *child_vp;
    int child_pid;
} pscal_ios_fork_state;

static __thread pscal_ios_fork_state g_pscal_ios_fork_state;

typedef struct {
    VProc *vp;
    int (*entry)(int, char **);
    int argc;
    char **argv;
} pscal_ios_exec_ctx;

static const char *pscal_ios_basename(const char *path) {
    if (!path) {
        return NULL;
    }
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static int pscal_ios_askpass_main(int argc, char **argv) {
    (void)argc;
    const char *prompt = (argc > 1) ? argv[1] : "Password: ";
    if (prompt && *prompt) {
        fputs(prompt, stderr);
        if (prompt[strlen(prompt) - 1] != ' ') {
            fputc(' ', stderr);
        }
        fflush(stderr);
    }
    char buf[512];
    if (!fgets(buf, sizeof(buf), stdin)) {
        return 1;
    }
    fputs(buf, stdout);
    fflush(stdout);
    return 0;
}

static char **pscal_ios_dup_argv(char *const argv[], int *out_argc) {
    int argc = 0;
    if (argv) {
        while (argv[argc]) {
            argc++;
        }
    }
    char **copy = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!copy) {
        return NULL;
    }
    for (int i = 0; i < argc; ++i) {
        const char *src = argv[i] ? argv[i] : "";
        copy[i] = strdup(src);
        if (!copy[i]) {
            for (int j = 0; j < i; ++j) {
                free(copy[j]);
            }
            free(copy);
            return NULL;
        }
    }
    copy[argc] = NULL;
    if (out_argc) {
        *out_argc = argc;
    }
    return copy;
}

static void pscal_ios_free_argv(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static void *pscal_ios_exec_thread(void *arg) {
    pscal_ios_exec_ctx *ctx = (pscal_ios_exec_ctx *)arg;
    int status = 127;
    if (ctx && ctx->vp && ctx->entry) {
        vprocActivate(ctx->vp);
        vprocRegisterThread(ctx->vp, pthread_self());
        status = ctx->entry(ctx->argc, ctx->argv);
        vprocMarkExit(ctx->vp, W_EXITCODE(status & 0xff, 0));
        vprocDeactivate();
        vprocDestroy(ctx->vp);
    }
    if (ctx) {
        pscal_ios_free_argv(ctx->argv, ctx->argc);
        free(ctx);
    }
    return (void *)(intptr_t)status;
}

static int pscal_ios_spawn_child(VProc *vp, int (*entry)(int, char **),
                                 char *const argv[]) {
    int argc = 0;
    char **argv_copy = pscal_ios_dup_argv(argv, &argc);
    if (!argv_copy) {
        errno = ENOMEM;
        return -1;
    }
    pscal_ios_exec_ctx *ctx = (pscal_ios_exec_ctx *)calloc(1, sizeof(pscal_ios_exec_ctx));
    if (!ctx) {
        pscal_ios_free_argv(argv_copy, argc);
        errno = ENOMEM;
        return -1;
    }
    ctx->vp = vp;
    ctx->entry = entry;
    ctx->argc = argc;
    ctx->argv = argv_copy;
    pthread_t tid;
    int err = pthread_create(&tid, NULL, pscal_ios_exec_thread, ctx);
    if (err != 0) {
        pscal_ios_free_argv(argv_copy, argc);
        free(ctx);
        errno = err;
        return -1;
    }
    pthread_detach(tid);
    return 0;
}

static bool pscal_ios_path_is_devtty(const char *path) {
    if (path == NULL) {
        return false;
    }
    if (strcmp(path, "/dev/tty") == 0) {
        return true;
    }
    if (strcmp(path, "/private/dev/tty") == 0) {
        return true;
    }
    return false;
}

static bool pscal_ios_get_clean_sysroot(char *out, size_t outlen) {
    if (!out || outlen == 0) {
        return false;
    }
    const char *root = getenv("PSCALI_SYSFILES_ROOT");
    if (!root || *root == '\0') {
        return false;
    }
    size_t root_len = strlen(root);
    if (root_len == 0 || root_len >= outlen) {
        return false;
    }
    memcpy(out, root, root_len);
    out[root_len] = '\0';
    while (root_len > 1 && out[root_len - 1] == '/') {
        out[--root_len] = '\0';
    }
    return true;
}

static bool pscal_ios_translate_etc_path(const char *path, char *out,
    size_t outlen) {
    if (path == NULL || out == NULL || outlen == 0) {
        return false;
    }
    static const char *kPrefixes[] = {
        "/etc/ssh",
        "/private/etc/ssh"
    };
    size_t matched_len = 0;
    for (size_t i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); ++i) {
        const char *prefix = kPrefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(path, prefix, prefix_len) == 0 &&
            (path[prefix_len] == '\0' || path[prefix_len] == '/')) {
            matched_len = prefix_len;
            break;
        }
    }
    if (matched_len == 0) {
        return false;
    }

    char sysroot[PATH_MAX];
    if (!pscal_ios_get_clean_sysroot(sysroot, sizeof(sysroot))) {
        return false;
    }
    size_t sysroot_len = strlen(sysroot);
    if (sysroot_len == 0) {
        return false;
    }
    if (strncmp(path, sysroot, sysroot_len) == 0 &&
        (path[sysroot_len] == '\0' || path[sysroot_len] == '/')) {
        return false;
    }

    const char *suffix = path + matched_len;
    if (*suffix == '/') {
        ++suffix;
    }
    int written;
    if (*suffix != '\0') {
        written = snprintf(out, outlen, "%s/etc/ssh/%s", sysroot, suffix);
    } else {
        written = snprintf(out, outlen, "%s/etc/ssh", sysroot);
    }
    if (written <= 0 || (size_t)written >= outlen) {
        return false;
    }
    return true;
}

static bool pscal_ios_virtual_tty_snapshot(int fd,
    pscal_ios_virtual_tty *out) {
    bool found = false;
    int key = pscal_ios_translate_fd(fd);
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        const pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (entry->active && entry->fd == key) {
            if (out) {
                *out = *entry;
            }
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_pscal_ios_tty_lock);
    return found;
}

static bool pscal_ios_virtual_tty_release(int fd, int *writer_out) {
    bool released = false;
    int key = pscal_ios_translate_fd(fd);
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (entry->active && entry->fd == key) {
            if (writer_out) {
                *writer_out = entry->writer;
            }
            entry->active = false;
            entry->fd = -1;
            entry->writer = -1;
            released = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_pscal_ios_tty_lock);
    return released;
}

static bool pscal_ios_virtual_tty_update_termios(int fd,
    const struct termios *termios_p) {
    bool updated = false;
    if (!termios_p) {
        return true;
    }
    int key = pscal_ios_translate_fd(fd);
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (entry->active && entry->fd == key) {
            entry->term = *termios_p;
            updated = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_pscal_ios_tty_lock);
    return updated;
}

static bool pscal_ios_virtual_tty_exists(int fd) {
    return pscal_ios_virtual_tty_snapshot(fd, NULL);
}

static int pscal_ios_translate_fd(int fd) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return fd;
    }
    int saved_errno = errno;
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        errno = saved_errno;
        return fd;
    }
    return host_fd;
}

static bool pscal_ios_session_stdio_matches(int fd) {
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return false;
    }
    VProc *vp = vprocCurrent();
    if (!vp) {
        return false;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return false;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        return false;
    }
    int session_fd = -1;
    if (fd == STDIN_FILENO) {
        session_fd = session->stdin_host_fd;
    } else if (fd == STDOUT_FILENO) {
        session_fd = session->stdout_host_fd;
    } else {
        session_fd = session->stderr_host_fd;
    }
    if (session_fd < 0) {
        return false;
    }
    struct stat host_st;
    struct stat session_st;
    if (fstat(host_fd, &host_st) != 0 || fstat(session_fd, &session_st) != 0) {
        return false;
    }
    return host_st.st_dev == session_st.st_dev &&
           host_st.st_ino == session_st.st_ino;
}

static void pscal_ios_init_termios(struct termios *out) {
    if (!out) {
        return;
    }
    if (tcgetattr(STDIN_FILENO, out) == 0) {
        return;
    }
    if (tcgetattr(STDOUT_FILENO, out) == 0) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->c_lflag = ECHO | ICANON;
}

static int pscal_ios_register_virtual_tty(void) {
    int read_fd = dup(pscal_ios_translate_fd(STDIN_FILENO));
    if (read_fd == -1) {
        return -1;
    }
    int write_fd = dup(pscal_ios_translate_fd(STDOUT_FILENO));
    if (write_fd == -1) {
        write_fd = dup(pscal_ios_translate_fd(STDERR_FILENO));
        if (write_fd == -1) {
            close(read_fd);
            return -1;
        }
    }

    struct termios defaults;
    pscal_ios_init_termios(&defaults);

    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (!entry->active) {
            entry->fd = read_fd;
            entry->writer = write_fd;
            entry->active = true;
            entry->term = defaults;
            pthread_mutex_unlock(&g_pscal_ios_tty_lock);
            return read_fd;
        }
    }
    pthread_mutex_unlock(&g_pscal_ios_tty_lock);

    close(read_fd);
    close(write_fd);
    errno = EMFILE;
    return -1;
}

static void pscal_ios_ensure_std_virtual_tty(int fd) {
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return;
    }

    bool interactive = false;
    if (pscal_ios_session_stdio_matches(fd)) {
        interactive = true;
    }
    if (fd == STDIN_FILENO) {
        interactive = interactive || pscalRuntimeStdinIsInteractive();
    } else if (fd == STDOUT_FILENO) {
        interactive = interactive || pscalRuntimeStdoutIsInteractive();
    } else if (fd == STDERR_FILENO) {
        interactive = interactive || pscalRuntimeStderrIsInteractive();
    }

    if (!interactive) {
        return;
    }

    if (pscal_ios_virtual_tty_exists(fd)) {
        return;
    }

    struct termios defaults;
    pscal_ios_init_termios(&defaults);

    int host_fd = pscal_ios_translate_fd(fd);
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    // Double check active status under lock
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        if (g_pscal_ios_virtual_ttys[i].active &&
            g_pscal_ios_virtual_ttys[i].fd == host_fd) {
            pthread_mutex_unlock(&g_pscal_ios_tty_lock);
            return;
        }
    }

    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (!entry->active) {
            entry->fd = host_fd;
            // For std streams, we map the writer to the fd itself
            // (or -1 for stdin to indicate no writing).
            // This ensures pscal_ios_write writes to the correct fd.
            entry->writer = (fd == STDIN_FILENO) ? -1 : host_fd;
            entry->active = true;
            entry->term = defaults;
            break;
        }
    }
    pthread_mutex_unlock(&g_pscal_ios_tty_lock);
}

int pscal_ios_open(const char *path, int oflag, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        has_mode = true;
    }

    if (path != NULL && pscal_ios_path_is_devtty(path)) {
        if (!pscalRuntimeStdinIsInteractive()) {
            errno = ENOTTY;
            return -1;
        }
        return pscal_ios_register_virtual_tty();
    }

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    char remapped_path[PATH_MAX];
    char expanded_path[PATH_MAX];
    const char *target_path = path;
    if (pscal_ios_translate_etc_path(path, remapped_path,
            sizeof(remapped_path))) {
        target_path = remapped_path;
    } else if (!pathTruncateExpand(path, expanded_path, sizeof(expanded_path))) {
        return -1;
    } else {
        target_path = expanded_path;
    }

    if (has_mode) {
        return open(target_path, oflag, mode);
    }
    return open(target_path, oflag);
}

int pscal_ios_openat(int fd, const char *path, int oflag, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        has_mode = true;
    }

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    if (pscal_ios_path_is_devtty(path)) {
        if (!pscalRuntimeStdinIsInteractive()) {
            errno = ENOTTY;
            return -1;
        }
        int tty_fd = pscal_ios_register_virtual_tty();
        if (tty_fd >= 0) {
            return tty_fd;
        }
    }

    char remapped_path[PATH_MAX];
    char expanded_path[PATH_MAX];
    const char *target_path = path;
    if (pscal_ios_translate_etc_path(path, remapped_path,
            sizeof(remapped_path))) {
        target_path = remapped_path;
    } else if (!pathTruncateExpand(path, expanded_path, sizeof(expanded_path))) {
        return -1;
    } else {
        target_path = expanded_path;
    }

    if (has_mode) {
        return openat(fd, target_path, oflag, mode);
    }
    return openat(fd, target_path, oflag);
}

ssize_t pscal_ios_read(int fd, void *buf, size_t nbyte) {
    if (fd == STDIN_FILENO) {
        ssize_t res = vprocReadShim(fd, buf, nbyte);
        if (res < 0 && getenv("PSCALI_TOOL_DEBUG")) {
            int saved_errno = errno;
            VProc *vp = vprocCurrent();
            int host = -1;
            int host_errno = 0;
            if (vp) {
                host = vprocTranslateFd(vp, fd);
                host_errno = errno;
            }
            fprintf(stderr,
                    "[pscal-ios-read] fd=%d res=%zd errno=%d vp=%p host=%d host_errno=%d\n",
                    fd, res, saved_errno, (void *)vp, host, host_errno);
            errno = saved_errno;
        }
        return res;
    }
    int host_fd = pscal_ios_translate_fd(fd);
    if (pscalRuntimeStdinIsInteractive()) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && session->stdin_host_fd >= 0) {
            if (session->stdin_host_fd == host_fd) {
                int flags = fcntl(host_fd, F_GETFL, 0);
                bool nonblocking = (flags >= 0) && ((flags & O_NONBLOCK) != 0);
                return vprocSessionReadInputShimMode(buf, nbyte, nonblocking);
            }
            struct stat session_st;
            struct stat fd_st;
            if (fstat(session->stdin_host_fd, &session_st) == 0 &&
                fstat(host_fd, &fd_st) == 0 &&
                session_st.st_dev == fd_st.st_dev &&
                session_st.st_ino == fd_st.st_ino) {
                int flags = fcntl(host_fd, F_GETFL, 0);
                bool nonblocking = (flags >= 0) && ((flags & O_NONBLOCK) != 0);
                return vprocSessionReadInputShimMode(buf, nbyte, nonblocking);
            }
        }
        struct stat stdin_st;
        struct stat fd_st;
        if (fstat(STDIN_FILENO, &stdin_st) == 0 &&
            fstat(host_fd, &fd_st) == 0 &&
            stdin_st.st_dev == fd_st.st_dev &&
            stdin_st.st_ino == fd_st.st_ino) {
            int flags = fcntl(host_fd, F_GETFL, 0);
            bool nonblocking = (flags >= 0) && ((flags & O_NONBLOCK) != 0);
            return vprocSessionReadInputShimMode(buf, nbyte, nonblocking);
        }
    }
    return read(host_fd, buf, nbyte);
}

ssize_t pscal_ios_write(int fd, const void *buf, size_t nbyte) {
    pscal_ios_virtual_tty entry;
    if (pscal_ios_virtual_tty_snapshot(fd, &entry)) {
        int target = entry.writer >= 0 ? entry.writer : pscal_ios_translate_fd(fd);
        return write(target, buf, nbyte);
    }
    return write(pscal_ios_translate_fd(fd), buf, nbyte);
}

int pscal_ios_close(int fd) {
    if (fd >= 0 && fd <= 2) {
        int writer = -1;
        pscal_ios_virtual_tty_release(fd, &writer);
        return 0;
    }
    int writer = -1;
    if (pscal_ios_virtual_tty_release(fd, &writer) && writer >= 0 &&
        writer != fd) {
        close(writer);
    }
    return close(pscal_ios_translate_fd(fd));
}

int pscal_ios_tcgetattr(int fd, struct termios *termios_p) {
    pscal_ios_ensure_std_virtual_tty(fd);
    pscal_ios_virtual_tty entry;
    if (pscal_ios_virtual_tty_snapshot(fd, &entry)) {
        if (termios_p) {
            *termios_p = entry.term;
        }
        return 0;
    }
    return tcgetattr(fd, termios_p);
}

int pscal_ios_tcsetattr(int fd, int optional_actions,
    const struct termios *termios_p) {
    (void)optional_actions;
    pscal_ios_ensure_std_virtual_tty(fd);
    if (pscal_ios_virtual_tty_update_termios(fd, termios_p)) {
        return 0;
    }
    return tcsetattr(fd, optional_actions, termios_p);
}

int pscal_ios_isatty(int fd) {
    pscal_ios_ensure_std_virtual_tty(fd);
    if (pscal_ios_virtual_tty_exists(fd)) {
        return 1;
    }
    return isatty(pscal_ios_translate_fd(fd));
}

int pscal_ios_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    void *arg = NULL;
    va_start(ap, request);
    if (request != (unsigned long)0) {
        arg = va_arg(ap, void *);
    }
    va_end(ap);

    pscal_ios_ensure_std_virtual_tty(fd);

    if (pscal_ios_virtual_tty_exists(fd) &&
        request == (unsigned long)TIOCGWINSZ && arg != NULL) {
        struct winsize *wsz = (struct winsize *)arg;
        memset(wsz, 0, sizeof(*wsz));
        unsigned short rows = (unsigned short)pscalRuntimeDetectWindowRows();
        unsigned short cols = (unsigned short)pscalRuntimeDetectWindowCols();
        if (rows == 0) {
            rows = 24;
        }
        if (cols == 0) {
            cols = 80;
        }
        wsz->ws_row = rows;
        wsz->ws_col = cols;
        return 0;
    }

    if (pscal_ios_virtual_tty_exists(fd) &&
        request == (unsigned long)TIOCSWINSZ && arg != NULL) {
        struct winsize *wsz = (struct winsize *)arg;
        char buffer[16];
        if (wsz->ws_col > 0) {
            snprintf(buffer, sizeof(buffer), "%u", (unsigned)wsz->ws_col);
            setenv("COLUMNS", buffer, 1);
        }
        if (wsz->ws_row > 0) {
            snprintf(buffer, sizeof(buffer), "%u", (unsigned)wsz->ws_row);
            setenv("LINES", buffer, 1);
        }
        return 0;
    }

    int host_fd = pscal_ios_translate_fd(fd);
    if (arg != NULL) {
        return ioctl(host_fd, request, arg);
    }
    return ioctl(host_fd, request);
}

static const char *pscal_ios_effective_path(const char *path,
    char *buffer, size_t buflen) {
    if (path == NULL) {
        return NULL;
    }
    if (pscal_ios_translate_etc_path(path, buffer, buflen)) {
        return buffer;
    }
    if (!pathTruncateExpand(path, buffer, buflen)) {
        return NULL;
    }
    return buffer;
}

int pscal_ios_stat(const char *path, struct stat *buf) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return stat(target, buf);
}

int pscal_ios_lstat(const char *path, struct stat *buf) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return lstat(target, buf);
}

int pscal_ios_access(const char *path, int mode) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return access(target, mode);
}

int pscal_ios_faccessat(int fd, const char *path, int mode, int flag) {
    if (path != NULL && path[0] == '/') {
        char remapped[PATH_MAX];
        const char *target = pscal_ios_effective_path(path, remapped,
            sizeof(remapped));
        if (target == NULL) {
            errno = EFAULT;
            return -1;
        }
        return faccessat(fd, target, mode, flag);
    }
    return faccessat(fd, path, mode, flag);
}

FILE *pscal_ios_fopen(const char *path, const char *mode) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return NULL;
    }
    return fopen(target, mode);
}

DIR *pscal_ios_opendir(const char *path) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return NULL;
    }
    return opendir(target);
}

int pscal_ios_mkdir(const char *path, mode_t mode) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return mkdir(target, mode);
}

int pscal_ios_rmdir(const char *path) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return rmdir(target);
}

int pscal_ios_unlink(const char *path) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return unlink(target);
}

int pscal_ios_remove(const char *path) {
    char remapped[PATH_MAX];
    const char *target = pscal_ios_effective_path(path, remapped,
        sizeof(remapped));
    if (target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return remove(target);
}

int pscal_ios_rename(const char *oldpath, const char *newpath) {
    char remapped_old[PATH_MAX];
    char remapped_new[PATH_MAX];
    const char *old_target = pscal_ios_effective_path(oldpath, remapped_old,
        sizeof(remapped_old));
    const char *new_target = pscal_ios_effective_path(newpath, remapped_new,
        sizeof(remapped_new));
    if (old_target == NULL || new_target == NULL) {
        errno = EFAULT;
        return -1;
    }
    return rename(old_target, new_target);
}

int pscal_ios_link(const char *target, const char *linkpath) {
    char remapped_target[PATH_MAX];
    char remapped_link[PATH_MAX];
    const char *link_target = pscal_ios_effective_path(linkpath, remapped_link,
        sizeof(remapped_link));
    const char *target_path = pscal_ios_effective_path(target, remapped_target,
        sizeof(remapped_target));
    if (link_target == NULL || target_path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return link(target_path, link_target);
}

int pscal_ios_symlink(const char *target, const char *linkpath) {
    char remapped_target[PATH_MAX];
    char remapped_link[PATH_MAX];
    const char *link_target = pscal_ios_effective_path(linkpath, remapped_link,
        sizeof(remapped_link));
    const char *target_path = pscal_ios_effective_path(target, remapped_target,
        sizeof(remapped_target));
    if (link_target == NULL || target_path == NULL) {
        errno = EFAULT;
        return -1;
    }
    return symlink(target_path, link_target);
}

pid_t pscal_ios_fork(void) {
    pscal_ios_fork_state *state = &g_pscal_ios_fork_state;
    if (state->active) {
        errno = EAGAIN;
        return -1;
    }
    volatile int jump_rc = sigsetjmp(state->parent_env, 1);
    if (jump_rc != 0) {
        state = &g_pscal_ios_fork_state;
        state->active = false;
        state->in_child = false;
        state->child_vp = NULL;
        int pid = state->child_pid;
        state->child_pid = 0;
        return (pid_t)pid;
    }

    VProcCommandScope scope;
    if (!vprocCommandScopeBegin(&scope, "fork", true, true)) {
        errno = ENOSYS;
        return -1;
    }

    state->active = true;
    state->in_child = true;
    state->child_vp = scope.vp;
    state->child_pid = scope.pid;
    return 0;
}

int pscal_ios_execv(const char *path, char *const argv[]) {
    const char *base = pscal_ios_basename(path);
    int (*entry)(int, char **) = NULL;
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr, "[fork-exec] path=%s base=%s\n",
                path ? path : "<null>",
                base ? base : "<null>");
    }
    if (base) {
        if (strcmp(base, "ssh") == 0) {
            entry = pscal_openssh_ssh_main;
        } else if (strcmp(base, "scp") == 0) {
            entry = pscal_openssh_scp_main;
        } else if (strcmp(base, "sftp") == 0) {
            entry = pscal_openssh_sftp_main;
        } else if (strcmp(base, "pscal-vproc-test-child") == 0) {
            entry = pscalVprocTestChildMain;
        } else if (strstr(base, "ssh-askpass") != NULL) {
            entry = pscal_ios_askpass_main;
        }
    }
    pscal_ios_fork_state *state = &g_pscal_ios_fork_state;
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr,
                "[fork-exec] entry=%p active=%d in_child=%d child_vp=%p child_pid=%d\n",
                (void *)entry,
                state->active ? 1 : 0,
                state->in_child ? 1 : 0,
                (void *)state->child_vp,
                state->child_pid);
    }
    if (!state->active || !state->in_child || !state->child_vp) {
        errno = ENOSYS;
        if (getenv("PSCALI_TOOL_DEBUG")) {
            fprintf(stderr, "[fork-exec] invalid fork state\n");
        }
        return -1;
    }
    if (!entry) {
        state->active = false;
        state->in_child = false;
        state->child_vp = NULL;
        state->child_pid = 0;
        errno = ENOENT;
        if (getenv("PSCALI_TOOL_DEBUG")) {
            fprintf(stderr, "[fork-exec] no entry for %s\n", base ? base : "<null>");
        }
        return -1;
    }
    if (pscal_ios_spawn_child(state->child_vp, entry, argv) != 0) {
        if (errno == 0) {
            errno = EIO;
        }
        if (getenv("PSCALI_TOOL_DEBUG")) {
            fprintf(stderr, "[fork-exec] spawn failed errno=%d\n", errno);
        }
        state->active = false;
        state->in_child = false;
        state->child_vp = NULL;
        state->child_pid = 0;
        return -1;
    }
    if (getenv("PSCALI_TOOL_DEBUG")) {
        fprintf(stderr, "[fork-exec] spawn ok, jumping to parent\n");
    }
    vprocUnregisterThread(state->child_vp, pthread_self());
    vprocDeactivate();
    siglongjmp(state->parent_env, 1);
    return -1;
}

int pscal_ios_execvp(const char *file, char *const argv[]) {
    return pscal_ios_execv(file, argv);
}

int pscal_ios_execl(const char *path, const char *arg, ...) {
    va_list ap;
    int argc = 0;
    const char *scan = arg;
    va_start(ap, arg);
    while (scan) {
        argc++;
        scan = va_arg(ap, const char *);
    }
    va_end(ap);

    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    argv[0] = (char *)arg;
    va_start(ap, arg);
    for (int i = 1; i < argc; ++i) {
        argv[i] = va_arg(ap, char *);
    }
    va_end(ap);
    argv[argc] = NULL;
    int rc = pscal_ios_execv(path, argv);
    free(argv);
    return rc;
}

int pscal_ios_execle(const char *path, const char *arg, ...) {
    va_list ap;
    int argc = 0;
    const char *scan = arg;
    va_start(ap, arg);
    while (scan) {
        argc++;
        scan = va_arg(ap, const char *);
    }
    char **envp = va_arg(ap, char **);
    va_end(ap);
    (void)envp;

    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    argv[0] = (char *)arg;
    va_start(ap, arg);
    for (int i = 1; i < argc; ++i) {
        argv[i] = va_arg(ap, char *);
    }
    va_end(ap);
    argv[argc] = NULL;
    int rc = pscal_ios_execv(path, argv);
    free(argv);
    return rc;
}

int pscal_ios_execlp(const char *file, const char *arg, ...) {
    va_list ap;
    int argc = 0;
    const char *scan = arg;
    va_start(ap, arg);
    while (scan) {
        argc++;
        scan = va_arg(ap, const char *);
    }
    va_end(ap);

    char **argv = (char **)calloc((size_t)argc + 1, sizeof(char *));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }
    argv[0] = (char *)arg;
    va_start(ap, arg);
    for (int i = 1; i < argc; ++i) {
        argv[i] = va_arg(ap, char *);
    }
    va_end(ap);
    argv[argc] = NULL;
    int rc = pscal_ios_execvp(file, argv);
    free(argv);
    return rc;
}

#endif /* PSCAL_TARGET_IOS */
