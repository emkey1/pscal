#define PSCAL_IOS_SHIM_IMPLEMENTATION
#include "pscal_ios_shim.h"

#ifdef PSCAL_TARGET_IOS

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#include "common/runtime_tty.h"

typedef struct {
    int fd;
    int writer;
    bool active;
    struct termios term;
} pscal_ios_virtual_tty;

static pthread_mutex_t g_pscal_ios_tty_lock = PTHREAD_MUTEX_INITIALIZER;
static pscal_ios_virtual_tty g_pscal_ios_virtual_ttys[8];

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
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        const pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (entry->active && entry->fd == fd) {
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
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (entry->active && entry->fd == fd) {
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
    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (entry->active && entry->fd == fd) {
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
    int read_fd = dup(STDIN_FILENO);
    if (read_fd == -1) {
        return -1;
    }
    int write_fd = dup(STDOUT_FILENO);
    if (write_fd == -1) {
        write_fd = dup(STDERR_FILENO);
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
    if (fd == STDIN_FILENO) {
        interactive = pscalRuntimeStdinIsInteractive();
    } else if (fd == STDOUT_FILENO) {
        interactive = pscalRuntimeStdoutIsInteractive();
    } else if (fd == STDERR_FILENO) {
        interactive = pscalRuntimeStderrIsInteractive();
    }

    if (!interactive) {
        return;
    }

    if (pscal_ios_virtual_tty_exists(fd)) {
        return;
    }

    struct termios defaults;
    pscal_ios_init_termios(&defaults);

    pthread_mutex_lock(&g_pscal_ios_tty_lock);
    // Double check active status under lock
    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        if (g_pscal_ios_virtual_ttys[i].active &&
            g_pscal_ios_virtual_ttys[i].fd == fd) {
            pthread_mutex_unlock(&g_pscal_ios_tty_lock);
            return;
        }
    }

    for (size_t i = 0; i < sizeof(g_pscal_ios_virtual_ttys) /
         sizeof(g_pscal_ios_virtual_ttys[0]); ++i) {
        pscal_ios_virtual_tty *entry = &g_pscal_ios_virtual_ttys[i];
        if (!entry->active) {
            entry->fd = fd;
            // For std streams, we map the writer to the fd itself
            // (or -1 for stdin to indicate no writing).
            // This ensures pscal_ios_write writes to the correct fd.
            entry->writer = (fd == STDIN_FILENO) ? -1 : fd;
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

    char remapped_path[PATH_MAX];
    const char *target_path = path;
    if (path != NULL &&
        pscal_ios_translate_etc_path(path, remapped_path,
            sizeof(remapped_path))) {
        target_path = remapped_path;
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

    if (path != NULL && path[0] == '/') {
        if (has_mode) {
            return pscal_ios_open(path, oflag, mode);
        }
        return pscal_ios_open(path, oflag);
    }

    if (path != NULL && pscal_ios_path_is_devtty(path)) {
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
    const char *target_path = path;
    if (path != NULL &&
        pscal_ios_translate_etc_path(path, remapped_path,
            sizeof(remapped_path))) {
        target_path = remapped_path;
    }

    if (has_mode) {
        return openat(fd, target_path, oflag, mode);
    }
    return openat(fd, target_path, oflag);
}

ssize_t pscal_ios_write(int fd, const void *buf, size_t nbyte) {
    pscal_ios_virtual_tty entry;
    if (pscal_ios_virtual_tty_snapshot(fd, &entry)) {
        int target = entry.writer >= 0 ? entry.writer : STDOUT_FILENO;
        return write(target, buf, nbyte);
    }
    return write(fd, buf, nbyte);
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
    return close(fd);
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
    return isatty(fd);
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

    if (arg != NULL) {
        return ioctl(fd, request, arg);
    }
    return ioctl(fd, request);
}

static const char *pscal_ios_effective_path(const char *path,
    char *buffer, size_t buflen) {
    if (path == NULL) {
        return NULL;
    }
    if (pscal_ios_translate_etc_path(path, buffer, buflen)) {
        return buffer;
    }
    return path;
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

pid_t pscal_ios_fork(void) {
    errno = ENOSYS;
    return -1;
}

int pscal_ios_execv(const char *path, char *const argv[]) {
    (void)path;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

int pscal_ios_execvp(const char *file, char *const argv[]) {
    (void)file;
    (void)argv;
    errno = ENOSYS;
    return -1;
}

int pscal_ios_execl(const char *path, const char *arg, ...) {
    (void)path;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

int pscal_ios_execle(const char *path, const char *arg, ...) {
    (void)path;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

int pscal_ios_execlp(const char *file, const char *arg, ...) {
    (void)file;
    (void)arg;
    errno = ENOSYS;
    return -1;
}

#endif /* PSCAL_TARGET_IOS */
