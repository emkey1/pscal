#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#undef PATH_VIRTUALIZATION_NO_MACROS

#if defined(PSCAL_TARGET_IOS)
extern void pscalRuntimeDebugLog(const char *message);
#define LOG_EXPANSION(op, original, resolved)                       \
    do {                                                            \
        if (&pscalRuntimeDebugLog != NULL) {                        \
            char logbuf[PATH_MAX * 2];                              \
            snprintf(logbuf, sizeof(logbuf),                        \
                     "[pathvirt] %s original=%s resolved=%s",       \
                     op,                                            \
                     (original) ? (original) : "(null)",            \
                     (resolved) ? (resolved) : "(null)");           \
            pscalRuntimeDebugLog(logbuf);                           \
        }                                                           \
    } while (0)
#else
#define LOG_EXPANSION(op, original, resolved) ((void)0)
#endif

#if defined(PSCAL_TARGET_IOS)
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "common/path_truncate.h"
#include "ios/vproc.h"
#include "ios/tty/pscal_fd.h"

#if defined(PSCAL_TARGET_IOS)
__attribute__((weak)) int pscalHostOpenRaw(const char *path, int flags, mode_t mode);
#endif

#if defined(PSCAL_TARGET_IOS)
static bool pathVirtualizationExplicit(void) {
    const char *env = getenv("PATH_TRUNCATE");
    if (env && env[0] != '\0') {
        return true;
    }
    env = getenv("PSCALI_CONTAINER_ROOT");
    return env && env[0] == '/';
}
#endif

static bool pathVirtualizationActive(void) {
    if (!pathTruncateEnabled()) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocCurrent();
    if (vp != NULL) return true;
    /* Only honor explicit truncation outside vproc to avoid HOME fallback surprises. */
    return pathVirtualizationExplicit();
#endif
    return true;
}

#if defined(PSCAL_TARGET_IOS)
static bool pathVirtualizedIsVprocDevicePath(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    /* Allow container-prefixed device paths anywhere in the string. */
    if (strstr(path, "/dev/location") != NULL || strstr(path, "/dev/gps") != NULL) {
        return true;
    }
    const char *candidate = path;
    if (strncmp(path, "/private", 8) == 0) {
        candidate = path + 8;
    }
    if (strncmp(candidate, "/dev/", 5) != 0) {
        return false;
    }
    if (strcmp(candidate, "/dev/tty") == 0 ||
        strcmp(candidate, "/dev/console") == 0 ||
        strcmp(candidate, "/dev/ptmx") == 0 ||
        strcmp(candidate, "/dev/location") == 0 ||
        strcmp(candidate, "/dev/gps") == 0) {
        return true;
    }
    if (strncmp(candidate, "/dev/pts/", 9) == 0) {
        return true;
    }
    if (strncmp(candidate, "/dev/tty", 8) == 0) {
        const char *digits = candidate + 8;
        if (*digits == '\0') {
            return true;
        }
        while (*digits) {
            if (*digits < '0' || *digits > '9') {
                return false;
            }
            digits++;
        }
        return true;
    }
    return false;
}
#endif

static bool vprocFdIsPscal(int fd) {
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocCurrent();
    if (!vp) {
        return false;
    }
    struct pscal_fd *psfd = vprocGetPscalFd(vp, fd);
    if (psfd) {
        pscal_fd_close(psfd);
        return true;
    }
#else
    (void)fd;
#endif
    return false;
}

#if defined(PSCAL_TARGET_IOS)
static int vprocStreamRead(void *cookie, char *buf, int len) {
    if (!buf || len <= 0) {
        return 0;
    }
    int fd = (int)(intptr_t)cookie;
    ssize_t res = vprocReadShim(fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)res;
}

static int vprocStreamWrite(void *cookie, const char *buf, int len) {
    if (!buf || len <= 0) {
        return 0;
    }
    int fd = (int)(intptr_t)cookie;
    ssize_t res = vprocWriteShim(fd, buf, (size_t)len);
    if (res < 0) {
        return -1;
    }
    if (res > INT_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return (int)res;
}

static int vprocStreamClose(void *cookie) {
    int fd = (int)(intptr_t)cookie;
    return vprocCloseShim(fd);
}

static FILE *vprocFdopenCompat(int fd, const char *mode) {
    bool is_pscal = vprocFdIsPscal(fd);
    if (!is_pscal) {
        FILE *fp = fdopen(fd, mode);
        if (!fp) {
            vprocCloseShim(fd);
        }
        return fp;
    }
    FILE *fp = funopen((void *)(intptr_t)fd,
                       vprocStreamRead,
                       vprocStreamWrite,
                       NULL,
                       vprocStreamClose);
    if (!fp) {
        vprocCloseShim(fd);
    }
    return fp;
}
#endif /* PSCAL_TARGET_IOS */

static const char *pathVirtualizedExpand(const char *input, char *buffer, size_t buffer_len) {
    if (!input) {
        return NULL;
    }
    if (!pathTruncateExpand(input, buffer, buffer_len)) {
        return input;
    }
    return buffer;
}

static void pathVirtualizedStripInPlace(char *buffer, size_t current_length) {
    if (!buffer) {
        return;
    }
    char stripped[PATH_MAX];
    if (!pathTruncateStrip(buffer, stripped, sizeof(stripped))) {
        return;
    }
    size_t len = strlen(stripped);
    if (len + 1 > current_length) {
        return;
    }
    memcpy(buffer, stripped, len + 1);
}

int pscalPathVirtualized_chdir(const char *path) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return chdir(path);
    }
    if (!pathVirtualizationActive()) {
        return chdir(path);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    return chdir(target ? target : path);
}

char *pscalPathVirtualized_getcwd(char *buffer, size_t size) {
    if (!pathVirtualizationActive()) {
        return getcwd(buffer, size);
    }
    char *result = getcwd(buffer, size);
    if (!result) {
        return result;
    }
    pathVirtualizedStripInPlace(buffer, strlen(buffer) + 1);
    return result;
}

int pscalPathVirtualized_stat(const char *path, struct stat *buf) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return stat(path, buf);
    }
    if (!pathVirtualizationActive()) {
        return stat(path, buf);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    LOG_EXPANSION("stat", path, target);
    return stat(target ? target : path, buf);
}

int pscalPathVirtualized_lstat(const char *path, struct stat *buf) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return lstat(path, buf);
    }
    if (!pathVirtualizationActive()) {
        return lstat(path, buf);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    LOG_EXPANSION("lstat", path, target);
    return lstat(target ? target : path, buf);
}

int pscalPathVirtualized_access(const char *path, int mode) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return access(path, mode);
    }
    if (!pathVirtualizationActive()) {
        return access(path, mode);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    return access(target ? target : path, mode);
}

int pscalPathVirtualized_mkdir(const char *path, mode_t mode) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return mkdir(path, mode);
    }
    if (!pathVirtualizationActive()) {
        return mkdir(path, mode);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    return mkdir(target ? target : path, mode);
}

int pscalPathVirtualized_rmdir(const char *path) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return rmdir(path);
    }
    if (!pathVirtualizationActive()) {
        return rmdir(path);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    return rmdir(target ? target : path);
}

int pscalPathVirtualized_unlink(const char *path) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return unlink(path);
    }
    if (!pathVirtualizationActive()) {
        return unlink(path);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    return unlink(target ? target : path);
}

int pscalPathVirtualized_remove(const char *path) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return remove(path);
    }
    if (!pathVirtualizationActive()) {
        return remove(path);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    return remove(target ? target : path);
}

int pscalPathVirtualized_rename(const char *oldpath, const char *newpath) {
    if (pathVirtualizedIsVprocDevicePath(oldpath) ||
        pathVirtualizedIsVprocDevicePath(newpath)) {
        return rename(oldpath, newpath);
    }
    if (!pathVirtualizationActive()) {
        return rename(oldpath, newpath);
    }
    char expanded_old[PATH_MAX];
    char expanded_new[PATH_MAX];
    const char *old_target = pathVirtualizedExpand(oldpath, expanded_old, sizeof(expanded_old));
    const char *new_target = pathVirtualizedExpand(newpath, expanded_new, sizeof(expanded_new));
    return rename(old_target ? old_target : oldpath,
                  new_target ? new_target : newpath);
}

DIR *pscalPathVirtualized_opendir(const char *name) {
    if (pathVirtualizedIsVprocDevicePath(name)) {
        return opendir(name);
    }
    if (!pathVirtualizationActive()) {
        return opendir(name);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(name, expanded, sizeof(expanded));
    LOG_EXPANSION("opendir", name, target);
    return opendir(target ? target : name);
}

int pscalPathVirtualized_symlink(const char *target, const char *linkpath) {
    if (pathVirtualizedIsVprocDevicePath(linkpath)) {
        return symlink(target, linkpath);
    }
    if (!pathVirtualizationActive()) {
        return symlink(target, linkpath);
    }
    char expanded_link[PATH_MAX];
    const char *link_target = pathVirtualizedExpand(linkpath, expanded_link, sizeof(expanded_link));
    return symlink(target, link_target ? link_target : linkpath);
}

ssize_t pscalPathVirtualized_readlink(const char *path, char *buf, size_t size) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return readlink(path, buf, size);
    }
    if (!pathVirtualizationActive()) {
        return readlink(path, buf, size);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    ssize_t result = readlink(target ? target : path, buf, size);
    if (result >= 0 && (size_t)result < size) {
        buf[result] = '\0';
        pathVirtualizedStripInPlace(buf, strlen(buf) + 1);
        result = (ssize_t)strlen(buf);
    }
    return result;
}

char *pscalPathVirtualized_realpath(const char *path, char *resolved_path) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return realpath(path, resolved_path);
    }
    if (!pathVirtualizationActive()) {
        return realpath(path, resolved_path);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedExpand(path, expanded, sizeof(expanded));
    char *result = realpath(target ? target : path, resolved_path);
    if (result) {
        pathVirtualizedStripInPlace(result, strlen(result) + 1);
    }
    return result;
}

static const char *pathVirtualizedPrepare(const char *path, char expanded[PATH_MAX]) {
    const char *target = pathVirtualizedExpand(path, expanded, PATH_MAX);
    return target ? target : path;
}

static void pathVirtualizedEnsureParent(const char *path) {
    if (!path || *path != '/') {
        return;
    }
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        return;
    }
    memcpy(buf, path, len + 1);
    for (char *p = buf + len; p >= buf; --p) {
        if (*p == '/') {
            *p = '\0';
            break;
        }
    }
    if (buf[0] == '\0') {
        return;
    }
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

static int pathVirtualizedOpenHost(const char *path, int oflag, mode_t mode, bool has_mode) {
#if defined(PSCAL_TARGET_IOS)
    if (pscalHostOpenRaw) {
        return pscalHostOpenRaw(path, oflag, mode);
    }
#endif
    return has_mode ? open(path, oflag, mode) : open(path, oflag);
}

int pscalPathVirtualized_open(const char *path, int oflag, ...) {
    mode_t mode = 0;
    bool has_mode = false;
    if (oflag & O_CREAT) {
        va_list ap;
        va_start(ap, oflag);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        has_mode = true;
    }
#if defined(PSCAL_TARGET_IOS)
    if (pathVirtualizedIsVprocDevicePath(path)) {
        return has_mode ? vprocOpenShim(path, oflag, mode) : vprocOpenShim(path, oflag);
    }
#endif
    if (!pathVirtualizationActive()) {
        return has_mode
            ? pathVirtualizedOpenHost(path, oflag, mode, true)
            : pathVirtualizedOpenHost(path, oflag, 0, false);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedPrepare(path, expanded);
    LOG_EXPANSION("open", path, target);
    if (has_mode) {
        pathVirtualizedEnsureParent(target);
        return pathVirtualizedOpenHost(target, oflag, mode, true);
    }
    return pathVirtualizedOpenHost(target, oflag, 0, false);
}

FILE *pscalPathVirtualized_fopen(const char *path, const char *mode) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        int flags = O_RDONLY;
        if (mode && (mode[0] == 'w' || mode[0] == 'a')) {
            flags = O_WRONLY | O_CREAT;
            if (mode[0] == 'w') {
                flags |= O_TRUNC;
            } else {
                flags |= O_APPEND;
            }
        }
        if (mode && strchr(mode, '+')) {
            flags = (flags & ~O_ACCMODE) | O_RDWR;
        }
#ifdef O_CLOEXEC
        flags |= O_CLOEXEC;
#endif
        int fd = vprocOpenShim(path, flags);
        if (fd < 0) {
            return NULL;
        }
        return vprocFdopenCompat(fd, mode);
    }
    if (!pathVirtualizationActive()) {
        return fopen(path, mode);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedPrepare(path, expanded);
    return fopen(target, mode);
}

FILE *pscalPathVirtualized_freopen(const char *path, const char *mode, FILE *stream) {
    if (pathVirtualizedIsVprocDevicePath(path)) {
        if (stream) {
            fclose(stream);
        }
        return pscalPathVirtualized_fopen(path, mode);
    }
    if (!pathVirtualizationActive()) {
        return freopen(path, mode, stream);
    }
    char expanded[PATH_MAX];
    const char *target = pathVirtualizedPrepare(path, expanded);
    return freopen(target, mode, stream);
}

#endif /* PSCAL_TARGET_IOS */
