#include "ios/vproc.h"

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h> // for vsnprintf
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <unistd.h>
#include <setjmp.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/socket.h>
#include <termios.h>
#include <limits.h>
#include <sys/time.h>
#include <time.h>
#include <poll.h>
#include <dlfcn.h>
#include <pwd.h>
#include <grp.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/syscall.h>
#endif
#include "common/path_truncate.h"
#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <mach/thread_act.h>
#endif
#include "common/runtime_tty.h"
#include "ios/tty/ish_compat.h"
#include "ios/tty/pscal_pty.h"
#include "ios/tty/pscal_tty_host.h"

typedef struct PSCALRuntimeContext PSCALRuntimeContext;
typedef enum {
    VPROC_RESOURCE_GENERIC = 0,
    VPROC_RESOURCE_SOCKET = 1,
    VPROC_RESOURCE_PIPE = 2,
} VProcResourceKind;

static void vprocResourceTrack(VProc *vp, int host_fd, VProcResourceKind kind);
static void vprocResourceRemove(VProc *vp, int host_fd);

#if defined(PSCAL_TARGET_IOS)
#define PATH_VIRTUALIZATION_NO_MACROS 1
#include "common/path_virtualization.h"
#undef PATH_VIRTUALIZATION_NO_MACROS
#endif

#if defined(PSCAL_TARGET_IOS)
__attribute__((weak)) void pscalRuntimeRequestSigint(void);
__attribute__((weak)) void pscalRuntimeDebugLog(const char *message);
__attribute__((weak)) PSCALRuntimeContext *PSCALRuntimeGetCurrentRuntimeContext(void);
__attribute__((weak)) void PSCALRuntimeSetCurrentRuntimeContext(PSCALRuntimeContext *ctx);
__attribute__((weak)) int pscalRuntimeCurrentForegroundPgid(void);
#endif

#if defined(__APPLE__)
extern ssize_t __read_nocancel(int, void *, size_t) __attribute__((weak_import));
extern ssize_t __write_nocancel(int, const void *, size_t) __attribute__((weak_import));
extern int __close_nocancel(int) __attribute__((weak_import));
extern int __open(const char *, int, ...) __attribute__((weak_import));
extern int __open_nocancel(const char *, int, ...) __attribute__((weak_import));
#endif

#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)
#define VPROC_UNUSED __attribute__((unused))
static const void *vprocSelfBase(void) {
    static const void *self_base = NULL;
    if (!self_base) {
        Dl_info info;
        if (dladdr((void *)&vprocSelfBase, &info) != 0) {
            self_base = info.dli_fbase;
        }
    }
    return self_base;
}

/* ----------------------------------------------------------------------
 * passwd/group overrides for iOS container
 * ---------------------------------------------------------------------- */
#if defined(PSCAL_TARGET_IOS)
typedef struct {
    struct passwd pw;
    char *name;
    char *passwd;
    char *gecos;
    char *dir;
    char *shell;
} PscalPasswdEntry;

typedef struct {
    struct group gr;
    char *name;
    char *passwd;
    gid_t *members_gids; /* unused; placeholder */
} PscalGroupEntry;

static PscalPasswdEntry *gPscalPasswd = NULL;
static size_t gPscalPasswdCount = 0;
static bool gPscalPasswdLoaded = false;
static char gPscalPasswdPath[PATH_MAX] = {0};
static dev_t gPscalPasswdDev = 0;
static ino_t gPscalPasswdIno = 0;
static time_t gPscalPasswdMtime = 0;
static off_t gPscalPasswdSize = 0;

static PscalGroupEntry *gPscalGroup = NULL;
static size_t gPscalGroupCount = 0;
static bool gPscalGroupLoaded = false;
static char gPscalGroupPath[PATH_MAX] = {0};
static dev_t gPscalGroupDev = 0;
static ino_t gPscalGroupIno = 0;
static time_t gPscalGroupMtime = 0;
static off_t gPscalGroupSize = 0;

static void pscalFreePasswd(void) {
    if (!gPscalPasswd) return;
    for (size_t i = 0; i < gPscalPasswdCount; ++i) {
        free(gPscalPasswd[i].name);
        free(gPscalPasswd[i].passwd);
        free(gPscalPasswd[i].gecos);
        free(gPscalPasswd[i].dir);
        free(gPscalPasswd[i].shell);
    }
    free(gPscalPasswd);
    gPscalPasswd = NULL;
    gPscalPasswdCount = 0;
}

static void pscalFreeGroup(void) {
    if (!gPscalGroup) return;
    for (size_t i = 0; i < gPscalGroupCount; ++i) {
        free(gPscalGroup[i].name);
        free(gPscalGroup[i].passwd);
        free(gPscalGroup[i].members_gids);
    }
    free(gPscalGroup);
    gPscalGroup = NULL;
    gPscalGroupCount = 0;
}

static const char *pscalEtcPath(const char *leaf, char *buf, size_t buf_len) {
    if (!leaf || !buf || buf_len == 0) {
        return NULL;
    }
    const char *direct = getenv("PSCALI_ETC_ROOT");
    if (direct && direct[0] == '/' &&
        snprintf(buf, buf_len, "%s/%s", direct, leaf) < (int)buf_len &&
        access(buf, R_OK) == 0) {
        return buf;
    }

    const char *container = getenv("PSCALI_CONTAINER_ROOT");
    const char *home = getenv("HOME");
    const char *roots[][3] = {
        { container, "Documents/etc", leaf },
        { container, "etc", leaf },
        { home,       "Documents/etc", leaf },
        { home,       "etc", leaf },
    };
    for (size_t i = 0; i < sizeof(roots)/sizeof(roots[0]); ++i) {
        const char *base = roots[i][0];
        const char *sub = roots[i][1];
        if (!base || base[0] != '/') {
            continue;
        }
        if (snprintf(buf, buf_len, "%s/%s/%s", base, sub, leaf) >= (int)buf_len) {
            continue;
        }
        if (access(buf, R_OK) == 0) {
            return buf;
        }
    }
    return NULL;
}

static bool pscalContainerUserDbAvailable(const char *leaf) {
    char path[PATH_MAX];
    return pscalEtcPath(leaf, path, sizeof(path)) != NULL;
}

static void pscalLoadPasswd(void) {
    char path[PATH_MAX];
    const char *passwd_path = pscalEtcPath("passwd", path, sizeof(path));
    struct stat st;
    if (!passwd_path || stat(passwd_path, &st) != 0) {
        return;
    }
    bool needs_reload = !gPscalPasswdLoaded ||
                        strcmp(gPscalPasswdPath, passwd_path) != 0 ||
                        st.st_mtime != gPscalPasswdMtime ||
                        st.st_size != gPscalPasswdSize ||
                        st.st_ino != gPscalPasswdIno ||
                        st.st_dev != gPscalPasswdDev;
    if (!needs_reload) {
        return;
    }
    gPscalPasswdLoaded = true;
    pscalFreePasswd();
    FILE *fp = fopen(passwd_path, "r");
    if (!fp) {
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        char *saveptr = NULL;
        char *tok_name   = strtok_r(line, ":\n", &saveptr);
        char *tok_passwd = strtok_r(NULL, ":\n", &saveptr);
        char *tok_uid    = strtok_r(NULL, ":\n", &saveptr);
        char *tok_gid    = strtok_r(NULL, ":\n", &saveptr);
        char *tok_gecos  = strtok_r(NULL, ":\n", &saveptr);
        char *tok_dir    = strtok_r(NULL, ":\n", &saveptr);
        char *tok_shell  = strtok_r(NULL, ":\n", &saveptr);
        if (!tok_name || !tok_passwd || !tok_uid || !tok_gid) {
            continue;
        }
        PscalPasswdEntry *resized = realloc(gPscalPasswd, (gPscalPasswdCount + 1) * sizeof(PscalPasswdEntry));
        if (!resized) {
            break;
        }
        gPscalPasswd = resized;
        PscalPasswdEntry *entry = &gPscalPasswd[gPscalPasswdCount++];
        memset(entry, 0, sizeof(*entry));
        entry->name   = strdup(tok_name);
        entry->passwd = strdup(tok_passwd);
        entry->gecos  = tok_gecos ? strdup(tok_gecos) : strdup("");
        entry->dir    = tok_dir ? strdup(tok_dir) : strdup("/");
        entry->shell  = tok_shell ? strdup(tok_shell) : strdup("/bin/sh");
        entry->pw.pw_name   = entry->name;
        entry->pw.pw_passwd = entry->passwd;
        entry->pw.pw_uid    = (uid_t)strtoul(tok_uid, NULL, 10);
        entry->pw.pw_gid    = (gid_t)strtoul(tok_gid, NULL, 10);
        entry->pw.pw_gecos  = entry->gecos;
        entry->pw.pw_dir    = entry->dir;
        entry->pw.pw_shell  = entry->shell;
    }
    free(line);
    fclose(fp);
    strlcpy(gPscalPasswdPath, passwd_path, sizeof(gPscalPasswdPath));
    gPscalPasswdDev = st.st_dev;
    gPscalPasswdIno = st.st_ino;
    gPscalPasswdMtime = st.st_mtime;
    gPscalPasswdSize = st.st_size;
}

static void pscalLoadGroup(void) {
    char path[PATH_MAX];
    const char *group_path = pscalEtcPath("group", path, sizeof(path));
    struct stat st;
    if (!group_path || stat(group_path, &st) != 0) {
        return;
    }
    bool needs_reload = !gPscalGroupLoaded ||
                        strcmp(gPscalGroupPath, group_path) != 0 ||
                        st.st_mtime != gPscalGroupMtime ||
                        st.st_size != gPscalGroupSize ||
                        st.st_ino != gPscalGroupIno ||
                        st.st_dev != gPscalGroupDev;
    if (!needs_reload) {
        return;
    }
    gPscalGroupLoaded = true;
    pscalFreeGroup();
    FILE *fp = fopen(group_path, "r");
    if (!fp) {
        return;
    }
    char *line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, fp) != -1) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\0') {
            continue;
        }
        char *saveptr = NULL;
        char *tok_name   = strtok_r(line, ":\n", &saveptr);
        char *tok_passwd = strtok_r(NULL, ":\n", &saveptr);
        char *tok_gid    = strtok_r(NULL, ":\n", &saveptr);
        if (!tok_name || !tok_passwd || !tok_gid) {
            continue;
        }
        PscalGroupEntry *resized = realloc(gPscalGroup, (gPscalGroupCount + 1) * sizeof(PscalGroupEntry));
        if (!resized) {
            break;
        }
        gPscalGroup = resized;
        PscalGroupEntry *entry = &gPscalGroup[gPscalGroupCount++];
        memset(entry, 0, sizeof(*entry));
        entry->name   = strdup(tok_name);
        entry->passwd = strdup(tok_passwd);
        entry->gr.gr_name   = entry->name;
        entry->gr.gr_passwd = entry->passwd;
        entry->gr.gr_gid    = (gid_t)strtoul(tok_gid, NULL, 10);
        entry->gr.gr_mem    = NULL;
    }
    free(line);
    fclose(fp);
    strlcpy(gPscalGroupPath, group_path, sizeof(gPscalGroupPath));
    gPscalGroupDev = st.st_dev;
    gPscalGroupIno = st.st_ino;
    gPscalGroupMtime = st.st_mtime;
    gPscalGroupSize = st.st_size;
}

static struct passwd *pscalGetpwuid(uid_t uid) {
    pscalLoadPasswd();
    for (size_t i = 0; i < gPscalPasswdCount; ++i) {
        if (gPscalPasswd[i].pw.pw_uid == uid) {
            return &gPscalPasswd[i].pw;
        }
    }
    return NULL;
}

static struct passwd *pscalGetpwnam(const char *name) {
    if (!name) return NULL;
    pscalLoadPasswd();
    for (size_t i = 0; i < gPscalPasswdCount; ++i) {
        if (strcmp(gPscalPasswd[i].pw.pw_name, name) == 0) {
            return &gPscalPasswd[i].pw;
        }
    }
    return NULL;
}

static struct group *pscalGetgrgid(gid_t gid) {
    pscalLoadGroup();
    for (size_t i = 0; i < gPscalGroupCount; ++i) {
        if (gPscalGroup[i].gr.gr_gid == gid) {
            return &gPscalGroup[i].gr;
        }
    }
    return NULL;
}

static struct group *pscalGetgrnam(const char *name) {
    if (!name) return NULL;
    pscalLoadGroup();
    for (size_t i = 0; i < gPscalGroupCount; ++i) {
        if (strcmp(gPscalGroup[i].gr.gr_name, name) == 0) {
            return &gPscalGroup[i].gr;
        }
    }
    return NULL;
}

__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwuid(uid_t uid);
__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwnam(const char *name);
__attribute__((weak)) struct group *pscalRuntimeHostGetgrgid(gid_t gid);
__attribute__((weak)) struct group *pscalRuntimeHostGetgrnam(const char *name);

__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwuid(uid_t uid) {
    (void)uid;
    return NULL;
}
__attribute__((weak)) struct passwd *pscalRuntimeHostGetpwnam(const char *name) {
    (void)name;
    return NULL;
}
__attribute__((weak)) struct group *pscalRuntimeHostGetgrgid(gid_t gid) {
    (void)gid;
    return NULL;
}
__attribute__((weak)) struct group *pscalRuntimeHostGetgrnam(const char *name) {
    (void)name;
    return NULL;
}

struct passwd *getpwuid(uid_t uid) {
    struct passwd *pw = pscalGetpwuid(uid);
    if (pw) return pw;
    /* If container passwd exists, do not consult host account DB. */
    if (pscalContainerUserDbAvailable("passwd")) return NULL;
    if (pscalRuntimeHostGetpwuid) return pscalRuntimeHostGetpwuid(uid);
    return NULL;
}

struct passwd *getpwnam(const char *name) {
    struct passwd *pw = pscalGetpwnam(name);
    if (pw) return pw;
    if (pscalContainerUserDbAvailable("passwd")) return NULL;
    if (pscalRuntimeHostGetpwnam) return pscalRuntimeHostGetpwnam(name);
    return NULL;
}

struct group *getgrgid(gid_t gid) {
    struct group *gr = pscalGetgrgid(gid);
    if (gr) return gr;
    /* If container group exists, do not consult host account DB. */
    if (pscalContainerUserDbAvailable("group")) return NULL;
    if (pscalRuntimeHostGetgrgid) return pscalRuntimeHostGetgrgid(gid);
    return NULL;
}

struct group *getgrnam(const char *name) {
    struct group *gr = pscalGetgrnam(name);
    if (gr) return gr;
    if (pscalContainerUserDbAvailable("group")) return NULL;
    if (pscalRuntimeHostGetgrnam) return pscalRuntimeHostGetgrnam(name);
    return NULL;
}

int vprocLookupPasswdName(uid_t uid, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = '\0';

    struct passwd *pw = pscalGetpwuid(uid);
    if (!pw) {
        /* Keep container passwd authoritative when it exists. */
        if (pscalContainerUserDbAvailable("passwd")) {
            errno = ENOENT;
            return -1;
        }
        if (pscalRuntimeHostGetpwuid) {
            pw = pscalRuntimeHostGetpwuid(uid);
        }
    }
    if (!pw || !pw->pw_name || pw->pw_name[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    size_t name_len = strlen(pw->pw_name);
    if (name_len >= buffer_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, pw->pw_name, name_len + 1);
    return 0;
}

int vprocLookupGroupName(gid_t gid, char *buffer, size_t buffer_len) {
    if (!buffer || buffer_len == 0) {
        errno = EINVAL;
        return -1;
    }
    buffer[0] = '\0';

    struct group *gr = pscalGetgrgid(gid);
    if (!gr) {
        /* Keep container group authoritative when it exists. */
        if (pscalContainerUserDbAvailable("group")) {
            errno = ENOENT;
            return -1;
        }
        if (pscalRuntimeHostGetgrgid) {
            gr = pscalRuntimeHostGetgrgid(gid);
        }
    }
    if (!gr || !gr->gr_name || gr->gr_name[0] == '\0') {
        errno = ENOENT;
        return -1;
    }
    size_t name_len = strlen(gr->gr_name);
    if (name_len >= buffer_len) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, gr->gr_name, name_len + 1);
    return 0;
}
#endif /* PSCAL_TARGET_IOS */

static void *vprocFilterSelfSymbol(void *sym) {
    if (!sym) {
        return NULL;
    }
    const void *self_base = vprocSelfBase();
    if (self_base) {
        Dl_info info;
        if (dladdr(sym, &info) != 0 && info.dli_fbase == self_base) {
            return NULL;
        }
    }
    return sym;
}

static bool vprocSymbolIsLogRedirect(void *sym) {
    if (!sym) return false;
    Dl_info info;
    memset(&info, 0, sizeof(info));
    if (dladdr(sym, &info) == 0) return false;
    if (info.dli_sname) {
        if (strstr(info.dli_sname, "LogRedirect") != NULL) return true;
        if (strstr(info.dli_sname, "logredirect") != NULL) return true;
    }
    if (info.dli_fname) {
        if (strstr(info.dli_fname, "LogRedirect") != NULL) return true;
        if (strstr(info.dli_fname, "logredirect") != NULL) return true;
    }
    return false;
}

static void *vprocOpenLibSystemHandle(void) {
    static void *handle = NULL;
    static int tried = 0;
    if (tried) {
        return handle;
    }
    tried = 1;
    handle = dlopen("libSystem.B.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        handle = dlopen("libSystem.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/libSystem.B.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/libSystem.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/system/libsystem_c.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/system/libsystem_kernel.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        handle = dlopen("/usr/lib/system/libsystem_pthread.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    return handle;
}

static void *vprocResolveSymbol(const char *name) {
    void *sym = NULL;
    void *libsystem = vprocOpenLibSystemHandle();
    if (libsystem) {
        sym = vprocFilterSelfSymbol(dlsym(libsystem, name));
    }
    if (!sym) {
        sym = vprocFilterSelfSymbol(dlsym(RTLD_NEXT, name));
    }
    if (!sym) {
        sym = vprocFilterSelfSymbol(dlsym(RTLD_DEFAULT, name));
    }
    return sym;
}

static pthread_once_t vprocHostReadOnce = PTHREAD_ONCE_INIT;
static void *vprocHostReadDirect = NULL;
static ssize_t (*vprocHostReadFn)(int, void *, size_t) = NULL;

static void vprocHostReadInit(void) {
#if defined(__APPLE__)
    if (&__read_nocancel) {
        vprocHostReadDirect = vprocFilterSelfSymbol((void *)&__read_nocancel);
    }
#endif
    vprocHostReadFn = (ssize_t (*)(int, void *, size_t))vprocResolveSymbol("__read_nocancel");
    if (!vprocHostReadFn) {
        vprocHostReadFn = (ssize_t (*)(int, void *, size_t))vprocResolveSymbol("read");
    }
    if (!vprocHostReadFn) {
        vprocHostReadFn = (ssize_t (*)(int, void *, size_t))vprocResolveSymbol("read$NOCANCEL");
    }
}

static VPROC_UNUSED ssize_t vprocHostReadRaw(int fd, void *buf, size_t count) {
    pthread_once(&vprocHostReadOnce, vprocHostReadInit);
#if defined(__APPLE__)
    if (vprocHostReadDirect) {
        ssize_t (*direct_fn)(int, void *, size_t) = (ssize_t (*)(int, void *, size_t))vprocHostReadDirect;
        vprocInterposeBypassEnter();
        ssize_t res = direct_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (vprocHostReadFn) {
        ssize_t (*read_fn)(int, void *, size_t) = vprocHostReadFn;
        vprocInterposeBypassEnter();
        ssize_t res = read_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
    vprocInterposeBypassEnter();
    ssize_t res = read(fd, buf, count);
    vprocInterposeBypassExit();
    return res;
}

static pthread_once_t vprocHostWriteOnce = PTHREAD_ONCE_INIT;
static void *vprocHostWriteDirect = NULL;
static ssize_t (*vprocHostWriteFallback)(int, const void *, size_t) = NULL;
static ssize_t (*vprocHostWriteFn)(int, const void *, size_t) = NULL;

static void vprocHostWriteInit(void) {
#if defined(__APPLE__)
    if (&__write_nocancel) {
        void *direct = vprocFilterSelfSymbol((void *)&__write_nocancel);
        if (vprocSymbolIsLogRedirect(direct)) {
            vprocHostWriteFallback = (ssize_t (*)(int, const void *, size_t))direct;
        } else {
            vprocHostWriteDirect = direct;
        }
    }
#endif
    ssize_t (*fn)(int, const void *, size_t) = NULL;
    fn = (ssize_t (*)(int, const void *, size_t))vprocResolveSymbol("__write_nocancel");
    if (vprocSymbolIsLogRedirect((void *)fn)) {
        if (!vprocHostWriteFallback) {
            vprocHostWriteFallback = fn;
        }
        fn = NULL;
    }
    if (!fn) {
        fn = (ssize_t (*)(int, const void *, size_t))vprocResolveSymbol("write");
        if (vprocSymbolIsLogRedirect((void *)fn)) {
            if (!vprocHostWriteFallback) {
                vprocHostWriteFallback = fn;
            }
            fn = NULL;
        }
    }
    if (!fn) {
        fn = (ssize_t (*)(int, const void *, size_t))vprocResolveSymbol("write$NOCANCEL");
        if (vprocSymbolIsLogRedirect((void *)fn)) {
            if (!vprocHostWriteFallback) {
                vprocHostWriteFallback = fn;
            }
            fn = NULL;
        }
    }
    if (!fn && vprocHostWriteFallback && !vprocSymbolIsLogRedirect((void *)vprocHostWriteFallback)) {
        fn = vprocHostWriteFallback;
    }
    vprocHostWriteFn = fn;
}

static ssize_t vprocHostWriteRaw(int fd, const void *buf, size_t count) {
    pthread_once(&vprocHostWriteOnce, vprocHostWriteInit);
#if defined(__APPLE__)
    if (vprocHostWriteDirect) {
        ssize_t (*direct_fn)(int, const void *, size_t) = (ssize_t (*)(int, const void *, size_t))vprocHostWriteDirect;
        vprocInterposeBypassEnter();
        ssize_t res = direct_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (vprocHostWriteFn) {
        ssize_t (*write_fn)(int, const void *, size_t) = vprocHostWriteFn;
        vprocInterposeBypassEnter();
        ssize_t res = write_fn(fd, buf, count);
        vprocInterposeBypassExit();
        return res;
    }
    vprocInterposeBypassEnter();
    ssize_t res = write(fd, buf, count);
    vprocInterposeBypassExit();
    return res;
}

static int vprocHostCloseRaw(int fd) {
    static int (*fn)(int) = NULL;
#if defined(__APPLE__)
    if (&__close_nocancel) {
        vprocInterposeBypassEnter();
        int res = __close_nocancel(fd);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("__close_nocancel");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("close");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("close$NOCANCEL");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostDupRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("dup");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static VPROC_UNUSED int vprocHostDup2Raw(int fd, int target) {
    static int (*fn)(int, int) = NULL;
    if (!fn) {
        fn = (int (*)(int, int))vprocResolveSymbol("dup2");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, target);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostPipeRaw(int pipefd[2]) {
    static int (*fn)(int[2]) = NULL;
    if (!fn) {
        fn = (int (*)(int[2]))vprocResolveSymbol("pipe");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(pipefd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSocketRaw(int domain, int type, int protocol) {
    static int (*fn)(int, int, int) = NULL;
    if (!fn) {
        fn = (int (*)(int, int, int))vprocResolveSymbol("socket");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(domain, type, protocol);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostAcceptRaw(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    static int (*fn)(int, struct sockaddr *, socklen_t *) = NULL;
    if (!fn) {
        fn = (int (*)(int, struct sockaddr *, socklen_t *))vprocResolveSymbol("accept");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, addr, addrlen);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSocketpairRaw(int domain, int type, int protocol, int sv[2]) {
    static int (*fn)(int, int, int, int[2]) = NULL;
    if (!fn) {
        fn = (int (*)(int, int, int, int[2]))vprocResolveSymbol("socketpair");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(domain, type, protocol, sv);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostIoctlRaw(int fd, unsigned long request, void *arg) {
    static int (*fn)(int, unsigned long, ...) = NULL;
    if (!fn) {
        fn = (int (*)(int, unsigned long, ...))vprocResolveSymbol("ioctl");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, request, arg);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static VPROC_UNUSED off_t vprocHostLseekRaw(int fd, off_t offset, int whence) {
    static off_t (*fn)(int, off_t, int) = NULL;
    if (!fn) {
        fn = (off_t (*)(int, off_t, int))vprocResolveSymbol("lseek");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        off_t res = fn(fd, offset, whence);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (off_t)-1;
}

static int vprocHostFsyncRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("__fsync");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("fsync");
    }
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("fsync$NOCANCEL");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostFstatRaw(int fd, struct stat *st) {
    static int (*fn)(int, struct stat *) = NULL;
    if (!fn) {
        fn = (int (*)(int, struct stat *))vprocResolveSymbol("__fstat");
    }
    if (!fn) {
        fn = (int (*)(int, struct stat *))vprocResolveSymbol("fstat");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, st);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostStatRaw(const char *path, struct stat *st) {
#if defined(AT_FDCWD)
    vprocInterposeBypassEnter();
    int res = fstatat(AT_FDCWD, path, st, 0);
    vprocInterposeBypassExit();
    return res;
#else
    static int (*fn)(const char *, struct stat *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, struct stat *))vprocResolveSymbol("stat");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, st);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
#endif
}

static int vprocHostLstatRaw(const char *path, struct stat *st) {
#if defined(AT_FDCWD)
    vprocInterposeBypassEnter();
    int res = fstatat(AT_FDCWD, path, st, AT_SYMLINK_NOFOLLOW);
    vprocInterposeBypassExit();
    return res;
#else
    static int (*fn)(const char *, struct stat *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, struct stat *))vprocResolveSymbol("lstat");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, st);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
#endif
}

static int vprocHostChdirRaw(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("chdir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static char *vprocHostGetcwdRaw(char *buf, size_t size) {
    static char *(*fn)(char *, size_t) = NULL;
    if (!fn) {
        fn = (char *(*)(char *, size_t))vprocResolveSymbol("getcwd");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        char *res = fn(buf, size);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return NULL;
}

static int vprocHostAccessRaw(const char *path, int mode) {
    static int (*fn)(const char *, int) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, int))vprocResolveSymbol("__access");
    }
    if (!fn) {
        fn = (int (*)(const char *, int))vprocResolveSymbol("access");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, mode);
        vprocInterposeBypassExit();
        return res;
    }
#if defined(AT_FDCWD)
    vprocInterposeBypassEnter();
    int res = faccessat(AT_FDCWD, path, mode, 0);
    vprocInterposeBypassExit();
    return res;
#endif
    errno = ENOSYS;
    return -1;
}

static int vprocHostChmodRaw(const char *path, mode_t mode) {
    static int (*fn)(const char *, mode_t) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, mode_t))vprocResolveSymbol("chmod");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, mode);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostFchmodRaw(int fd, mode_t mode) {
    static int (*fn)(int, mode_t) = NULL;
    if (!fn) {
        fn = (int (*)(int, mode_t))vprocResolveSymbol("fchmod");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, mode);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostChownRaw(const char *path, uid_t uid, gid_t gid) {
    static int (*fn)(const char *, uid_t, gid_t) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, uid_t, gid_t))vprocResolveSymbol("chown");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, uid, gid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostFchownRaw(int fd, uid_t uid, gid_t gid) {
    static int (*fn)(int, uid_t, gid_t) = NULL;
    if (!fn) {
        fn = (int (*)(int, uid_t, gid_t))vprocResolveSymbol("fchown");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, uid, gid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostMkdirRaw(const char *path, mode_t mode) {
    static int (*fn)(const char *, mode_t) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, mode_t))vprocResolveSymbol("mkdir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path, mode);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRmdirRaw(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("rmdir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostUnlinkRaw(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("unlink");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRemoveRaw(const char *path) {
    static int (*fn)(const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *))vprocResolveSymbol("remove");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRenameRaw(const char *oldpath, const char *newpath) {
    static int (*fn)(const char *, const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, const char *))vprocResolveSymbol("rename");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(oldpath, newpath);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static DIR *vprocHostOpendirRaw(const char *name) {
    static DIR *(*fn)(const char *) = NULL;
    if (!fn) {
        fn = (DIR *(*)(const char *))vprocResolveSymbol("opendir");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        DIR *res = fn(name);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return NULL;
}

static int vprocHostSymlinkRaw(const char *target, const char *linkpath) {
    static int (*fn)(const char *, const char *) = NULL;
    if (!fn) {
        fn = (int (*)(const char *, const char *))vprocResolveSymbol("symlink");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(target, linkpath);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static ssize_t vprocHostReadlinkRaw(const char *path, char *buf, size_t size) {
    static ssize_t (*fn)(const char *, char *, size_t) = NULL;
    if (!fn) {
        fn = (ssize_t (*)(const char *, char *, size_t))vprocResolveSymbol("readlink");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        ssize_t res = fn(path, buf, size);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static char *vprocHostRealpathRaw(const char *path, char *resolved_path) {
    static char *(*fn)(const char *, char *) = NULL;
    if (!fn) {
        fn = (char *(*)(const char *, char *))vprocResolveSymbol("realpath");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        char *res = fn(path, resolved_path);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return NULL;
}

static int vprocHostStatVirtualized(const char *path, struct stat *st) {
#if defined(PSCAL_TARGET_IOS)
    if (pathTruncateEnabled()) {
        char expanded[PATH_MAX];
        if (pathTruncateExpand(path, expanded, sizeof(expanded))) {
            return vprocHostStatRaw(expanded, st);
        }
    }
#endif
    return vprocHostStatRaw(path, st);
}

static int vprocHostLstatVirtualized(const char *path, struct stat *st) {
#if defined(PSCAL_TARGET_IOS)
    if (pathTruncateEnabled()) {
        char expanded[PATH_MAX];
        if (pathTruncateExpand(path, expanded, sizeof(expanded))) {
            return vprocHostLstatRaw(expanded, st);
        }
    }
#endif
    return vprocHostLstatRaw(path, st);
}

static int vprocHostPollRaw(struct pollfd *fds, nfds_t nfds, int timeout) {
    static int (*fn)(struct pollfd *, nfds_t, int) = NULL;
    if (!fn) {
        fn = (int (*)(struct pollfd *, nfds_t, int))vprocResolveSymbol("poll");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fds, nfds, timeout);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSelectRaw(int nfds,
                              fd_set *readfds,
                              fd_set *writefds,
                              fd_set *exceptfds,
                              struct timeval *timeout) {
    static int (*fn)(int, fd_set *, fd_set *, fd_set *, struct timeval *) = NULL;
    if (!fn) {
        fn = (int (*)(int, fd_set *, fd_set *, fd_set *, struct timeval *))vprocResolveSymbol("select");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(nfds, readfds, writefds, exceptfds, timeout);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static VPROC_UNUSED int vprocHostIsattyRaw(int fd) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("isatty");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return 0;
}

static int vprocHostOpenRawInternal(const char *path, int flags, mode_t mode, bool has_mode) {
    static int (*fn)(const char *, int, ...) = NULL;
    static int (*openat_fn)(int, const char *, int, ...) = NULL;
#if defined(__APPLE__)
    if (&__open_nocancel) {
        vprocInterposeBypassEnter();
        int res = has_mode ? __open_nocancel(path, flags, mode) : __open_nocancel(path, flags);
        vprocInterposeBypassExit();
        return res;
    }
    if (&__open) {
        vprocInterposeBypassEnter();
        int res = has_mode ? __open(path, flags, mode) : __open(path, flags);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("__open_nocancel");
    }
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("__open");
    }
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("open");
    }
    if (!fn) {
        fn = (int (*)(const char *, int, ...))vprocResolveSymbol("open$NOCANCEL");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = has_mode ? fn(path, flags, mode) : fn(path, flags);
        vprocInterposeBypassExit();
        return res;
    }
#if defined(AT_FDCWD)
    if (!openat_fn) {
        openat_fn = (int (*)(int, const char *, int, ...))vprocResolveSymbol("openat");
    }
    if (openat_fn) {
        vprocInterposeBypassEnter();
        int res = has_mode ? openat_fn(AT_FDCWD, path, flags, mode) : openat_fn(AT_FDCWD, path, flags);
        vprocInterposeBypassExit();
        return res;
    }
#endif
    errno = ENOSYS;
    return -1;
}

int pscalHostOpenRaw(const char *path, int flags, mode_t mode) {
    return vprocHostOpenRawInternal(path, flags, mode, (flags & O_CREAT) != 0);
}

static pid_t vprocHostWaitpidRaw(pid_t pid, int *status_out, int options) {
    static pid_t (*fn)(pid_t, int *, int) = NULL;
    if (!fn) {
        fn = (pid_t (*)(pid_t, int *, int))vprocResolveSymbol("waitpid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(pid, status_out, options);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static int vprocHostKillRaw(pid_t pid, int sig) {
    static int (*fn)(pid_t, int) = NULL;
    if (!fn) {
        fn = (int (*)(pid_t, int))vprocResolveSymbol("kill");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(pid, sig);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static pid_t vprocHostGetpidRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("getpid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetppidRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("getppid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetpgrpRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("getpgrp");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostGetpgidRaw(pid_t pid) {
    static pid_t (*fn)(pid_t) = NULL;
    if (!fn) {
        fn = (pid_t (*)(pid_t))vprocResolveSymbol("getpgid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(pid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static int vprocHostSetpgidRaw(pid_t pid, pid_t pgid) {
    static int (*fn)(pid_t, pid_t) = NULL;
    if (!fn) {
        fn = (int (*)(pid_t, pid_t))vprocResolveSymbol("setpgid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(pid, pgid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static pid_t vprocHostGetsidRaw(pid_t pid) {
    static pid_t (*fn)(pid_t) = NULL;
    if (!fn) {
        fn = (pid_t (*)(pid_t))vprocResolveSymbol("getsid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(pid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostSetsidRaw(void) {
    static pid_t (*fn)(void) = NULL;
    if (!fn) {
        fn = (pid_t (*)(void))vprocResolveSymbol("setsid");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn();
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static pid_t vprocHostTcgetpgrpRaw(int fd) {
    static pid_t (*fn)(int) = NULL;
    if (!fn) {
        fn = (pid_t (*)(int))vprocResolveSymbol("tcgetpgrp");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        pid_t res = fn(fd);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return (pid_t)-1;
}

static int vprocHostTcsetpgrpRaw(int fd, pid_t pgid) {
    static int (*fn)(int, pid_t) = NULL;
    if (!fn) {
        fn = (int (*)(int, pid_t))vprocResolveSymbol("tcsetpgrp");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(fd, pgid);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostPthreadCreateRaw(pthread_t *thread,
                                     const pthread_attr_t *attr,
                                     void *(*start_routine)(void *),
                                     void *arg) {
    static int (*fn)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *) = NULL;
    if (!fn) {
        fn = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))vprocResolveSymbol("pthread_create");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(thread, attr, start_routine, arg);
        vprocInterposeBypassExit();
        return res;
    }
    return EINVAL;
}

static int vprocHostSigactionRaw(int sig, const struct sigaction *act, struct sigaction *oldact) {
    static int (*fn)(int, const struct sigaction *, struct sigaction *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const struct sigaction *, struct sigaction *))vprocResolveSymbol("sigaction");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(sig, act, oldact);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSigprocmaskRaw(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*fn)(int, const sigset_t *, sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const sigset_t *, sigset_t *))vprocResolveSymbol("sigprocmask");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(how, set, oldset);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSigpendingRaw(sigset_t *set) {
    static int (*fn)(sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(sigset_t *))vprocResolveSymbol("sigpending");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(set);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostSigsuspendRaw(const sigset_t *mask) {
    static int (*fn)(const sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(const sigset_t *))vprocResolveSymbol("sigsuspend");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(mask);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostRaiseRaw(int sig) {
    static int (*fn)(int) = NULL;
    if (!fn) {
        fn = (int (*)(int))vprocResolveSymbol("raise");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(sig);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}

static int vprocHostPthreadSigmaskRaw(int how, const sigset_t *set, sigset_t *oldset) {
    static int (*fn)(int, const sigset_t *, sigset_t *) = NULL;
    if (!fn) {
        fn = (int (*)(int, const sigset_t *, sigset_t *))vprocResolveSymbol("pthread_sigmask");
    }
    if (fn) {
        vprocInterposeBypassEnter();
        int res = fn(how, set, oldset);
        vprocInterposeBypassExit();
        return res;
    }
    errno = ENOSYS;
    return -1;
}
#undef VPROC_UNUSED
#endif

/* -- Compatibility Macros -- */
#ifndef W_EXITCODE
#define W_EXITCODE(ret, sig) ((ret) << 8 | (sig))
#endif

#ifndef W_STOPCODE
#define W_STOPCODE(sig) ((sig) << 8 | 0x7f)
#endif

#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
static int vprocHostOpenVirtualized(const char *path, int flags, int mode) {
    if (flags & O_CREAT) {
        return pscalPathVirtualized_open(path, flags, mode);
    }
    return pscalPathVirtualized_open(path, flags);
}
#elif defined(PSCAL_TARGET_IOS)
#define vprocHostOpenVirtualized(path, flags, mode) pscalPathVirtualized_open((path), (flags), (mode))
#else
#define vprocHostOpenVirtualized(path, flags, mode) open((path), (flags), (mode))
#endif

/* -- Undefine host macros that might conflict -- */
#ifdef waitpid
#undef waitpid
#endif
#ifdef kill
#undef kill
#endif
#ifdef getpid
#undef getpid
#endif
#ifdef getppid
#undef getppid
#endif
#ifdef getpgrp
#undef getpgrp
#endif
#ifdef getpgid
#undef getpgid
#endif
#ifdef setpgid
#undef setpgid
#endif
#ifdef getsid
#undef getsid
#endif
#ifdef setsid
#undef setsid
#endif
#ifdef tcgetpgrp
#undef tcgetpgrp
#endif
#ifdef tcsetpgrp
#undef tcsetpgrp
#endif
#ifdef sigaction
#undef sigaction
#endif
#ifdef sigprocmask
#undef sigprocmask
#endif
#ifdef sigpending
#undef sigpending
#endif
#ifdef sigsuspend
#undef sigsuspend
#endif
#ifdef signal
#undef signal
#endif
#ifdef raise
#undef raise
#endif
#ifdef pthread_sigmask
#undef pthread_sigmask
#endif

#ifndef VPROC_INITIAL_CAPACITY
#define VPROC_INITIAL_CAPACITY 16
#endif

/* -- Data Structures -- */

typedef enum {
    VPROC_FD_NONE = 0,
    VPROC_FD_HOST = 1,
    VPROC_FD_PSCAL = 2,
} VProcFdKind;

typedef struct {
    int host_fd;
    struct pscal_fd *pscal_fd;
    VProcFdKind kind;
} VProcFdEntry;

typedef struct {
    int host_fd;
    VProcResourceKind kind;
} VProcResourceEntry;

struct VProc {
    pthread_mutex_t mu;     // ADDED: Protects the FD table shared by threads
    VProcFdEntry *entries;
    size_t capacity;
    int next_fd;
    int stdin_fd;
    int stdout_fd;
    int stderr_fd;
    int stdin_host_fd;      /* stable handle for "controlling" stdin */
    int stdout_host_fd;
    int stderr_host_fd;
    bool stdin_from_session;
    VProcWinsize winsize;
    int pid;
    VProcResourceEntry *resources;
    size_t resource_count;
    size_t resource_capacity;
};

static int vprocSessionHostFdForStd(int std_fd);
static bool vprocSessionStdioMatchFd(int session_fd, int std_fd);
static int vprocHostIsatty(int fd);
static int vprocSetCompatErrno(int err);
static void vprocPtyTrace(const char *format, ...);

static __thread VProc *gVProcCurrent = NULL;
static __thread VProc *gVProcStack[16] = {0};
static __thread size_t gVProcStackDepth = 0;
static __thread unsigned long long gVProcRegistrySeenVersion = 0;
static VProc **gVProcRegistry = NULL;
static size_t gVProcRegistryCount = 0;
static size_t gVProcRegistryCapacity = 0;
static VProc *gVProcRegistryHint = NULL;
static pthread_mutex_t gVProcRegistryMu = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long gVProcRegistryVersion = 1;
static int gNextSyntheticPid = 0;
static _Thread_local int gShellSelfPid = 0;
static int gShellSelfPidGlobal = 0;
static _Thread_local int gKernelPid = 0;
static int gKernelPidGlobal = 0;
static volatile sig_atomic_t gVProcInterposeReady = 0;
static volatile sig_atomic_t gVProcTlsReady = 0; // Avoid TLV access before TLS is initialized.
static _Thread_local int gVProcInterposeBypassDepth = 0;
typedef struct {
    pthread_t *items;
    size_t count;
    size_t capacity;
    bool hint_valid;
    pthread_t hint_tid;
    size_t hint_index;
    pthread_mutex_t mu;
} VProcInterposeBypassRegistry;

static VProcInterposeBypassRegistry gVProcInterposeBypassRegistry = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .hint_valid = false,
    .hint_tid = 0,
    .hint_index = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
};
static pthread_mutex_t gPathTruncateMu = PTHREAD_MUTEX_INITIALIZER;
static bool gPathTruncateInit = false;
static VProc *gKernelVproc = NULL;
static pthread_mutex_t gKernelVprocMu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gKernelThread;
static pthread_mutex_t gKernelThreadMu = PTHREAD_MUTEX_INITIALIZER;
static bool gKernelThreadStarted = false;
static pthread_cond_t gKernelThreadCv = PTHREAD_COND_INITIALIZER;
static bool gKernelThreadReady = false;
static pthread_t gShellSelfTid;
static bool gShellSelfTidValid = false;
static VProcSessionStdio gSessionStdioDefault = {
    .stdin_host_fd = -1,
    .stdout_host_fd = -1,
    .stderr_host_fd = -1,
    .kernel_pid = 0,
    .input = NULL,
    .stdin_pscal_fd = NULL,
    .stdout_pscal_fd = NULL,
    .stderr_pscal_fd = NULL,
    .pty_master = NULL,
    .pty_slave = NULL,
    .pty_out_thread = 0,
    .pty_active = false,
    .session_id = 0,
};
static _Thread_local VProcSessionStdio *gSessionStdioTls = NULL;
static pthread_mutex_t gSessionInputInitMu = PTHREAD_MUTEX_INITIALIZER;
static const char *kLocationDevicePath = "/dev/location";
static const char *kLegacyGpsDevicePath = "/dev/gps";
static const char *kLegacyGpsDevicePath2 = "/dev/ttyGPS";

static void vprocResourceEnsureCapacity(VProc *vp, size_t needed) {
    if (!vp) {
        return;
    }
    if (vp->resource_capacity >= needed) {
        return;
    }
    size_t new_cap = vp->resource_capacity ? vp->resource_capacity * 2 : 8;
    while (new_cap < needed) {
        new_cap *= 2;
    }
    VProcResourceEntry *resized = realloc(vp->resources, new_cap * sizeof(VProcResourceEntry));
    if (!resized) {
        return;
    }
    vp->resources = resized;
    vp->resource_capacity = new_cap;
}

static void vprocResourceTrackLocked(VProc *vp, int host_fd, VProcResourceKind kind) {
    if (!vp || host_fd < 0) {
        return;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        if (vp->resources[i].host_fd == host_fd) {
            vp->resources[i].kind = kind;
            return;
        }
    }
    vprocResourceEnsureCapacity(vp, vp->resource_count + 1);
    if (vp->resource_count < vp->resource_capacity) {
        vp->resources[vp->resource_count].host_fd = host_fd;
        vp->resources[vp->resource_count].kind = kind;
        vp->resource_count++;
    }
}

static void vprocResourceTrack(VProc *vp, int host_fd, VProcResourceKind kind) {
    if (!vp || host_fd < 0) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    vprocResourceTrackLocked(vp, host_fd, kind);
    pthread_mutex_unlock(&vp->mu);
}

static bool vprocResourceRemoveLocked(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        return false;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        if (vp->resources[i].host_fd != host_fd) {
            continue;
        }
        size_t last = vp->resource_count - 1;
        if (i != last) {
            vp->resources[i] = vp->resources[last];
        }
        vp->resource_count--;
        return true;
    }
    return false;
}

static void vprocResourceRemove(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    (void)vprocResourceRemoveLocked(vp, host_fd);
    pthread_mutex_unlock(&vp->mu);
}

static void vprocResourceCloseAllLocked(VProc *vp) {
    if (!vp) {
        return;
    }
    for (size_t i = 0; i < vp->resource_count; ++i) {
        int fd = vp->resources[i].host_fd;
        if (fd < 0) {
            continue;
        }
#if defined(PSCAL_TARGET_IOS)
        vprocHostCloseRaw(fd);
#else
        close(fd);
#endif
        vp->resources[i].host_fd = -1;
    }
    vp->resource_count = 0;
}

static void vprocResourceCloseAll(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&vp->mu);
    vprocResourceCloseAllLocked(vp);
    pthread_mutex_unlock(&vp->mu);
}

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cond;
    int read_fd;
    int write_fd;
    int stub_fd;
    int readers;
    uint64_t seq;
    bool cond_init;
    char last_payload[128];
    size_t last_len;
    bool has_payload;
    bool enabled;
} VProcLocationDevice;

static VProcLocationDevice gLocationDevice = {
    PTHREAD_MUTEX_INITIALIZER,
    PTHREAD_COND_INITIALIZER,
    -1,
    -1,
    -1,
    0,
    0,
    false,
    {0},
    0,
    false,
    true
};
static bool vprocLocationDebugEnabled(void);
static void vprocLocationDebugf(const char *fmt, ...);
static bool vprocToolDebugEnabled(void);
static bool vprocVprocDebugEnabled(void);
static bool vprocPipeDebugEnabled(void);
static bool vprocKillDebugEnabled(void);

typedef struct {
    uint64_t last_seq;
    size_t offset;
    size_t len;
    bool done;
    char payload[sizeof(((VProcLocationDevice *)0)->last_payload)];
} VProcLocationReader;

/* Forward declaration; defined later. */
static void vprocDeliverPendingSignalsForCurrent(void);
static bool vprocCurrentHasPendingSignals(void);
static const struct pscal_fd_ops kVprocLocationFdOps;
typedef void (*VprocLocationReadersChangedFn)(int readers, void *context);
static VprocLocationReadersChangedFn gLocationReaderObserver = NULL;
static void *gLocationReaderObserverCtx = NULL;
static pthread_mutex_t gLocationReaderObserverMu = PTHREAD_MUTEX_INITIALIZER;

static void vprocLocationNotifyObservers(int readers) {
    VprocLocationReadersChangedFn cb = NULL;
    void *ctx = NULL;
    pthread_mutex_lock(&gLocationReaderObserverMu);
    cb = gLocationReaderObserver;
    ctx = gLocationReaderObserverCtx;
    pthread_mutex_unlock(&gLocationReaderObserverMu);
    if (cb) {
        cb(readers, ctx);
    }
}

/* -------- In-process pipe (mutex/cond-backed) -------- */

typedef struct {
    pthread_mutex_t mu;
    pthread_cond_t cond_read;
    pthread_cond_t cond_write;
    unsigned char *buf;
    size_t cap;
    size_t head;
    size_t tail;
    size_t count;
    bool read_closed;
    bool write_closed;
    int wait_readers;
    int wait_writers;
    int active_ops;
    bool freed;
    int readers;
    int writers;
} VprocInprocPipe;

typedef struct {
    VprocInprocPipe *pipe;
    bool is_reader;
} VprocInprocEnd;

static void vprocInprocPipeFree(VprocInprocPipe *pipe) {
    if (!pipe) {
        return;
    }
    pthread_mutex_destroy(&pipe->mu);
    pthread_cond_destroy(&pipe->cond_read);
    pthread_cond_destroy(&pipe->cond_write);
    free(pipe->buf);
    free(pipe);
}

static bool vprocInprocPipeShouldDestroy(VprocInprocPipe *pipe) {
    if (!pipe || pipe->freed) {
        return false;
    }
    bool destroy = pipe->read_closed &&
                   pipe->write_closed &&
                   pipe->readers <= 0 &&
                   pipe->writers <= 0 &&
                   pipe->active_ops == 0 &&
                   pipe->wait_readers == 0 &&
                   pipe->wait_writers == 0;
    if (destroy) {
        pipe->freed = true;
    }
    return destroy;
}

static ssize_t vprocInprocPipeRead(struct pscal_fd *fd, void *buf, size_t bufsize) {
    if (!fd || !buf) {
        return _EBADF;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe || !end->is_reader) {
        return _EBADF;
    }
    VprocInprocPipe *pipe = end->pipe;
    pthread_mutex_lock(&pipe->mu);
    pipe->active_ops++;
    while (pipe->count == 0 && !pipe->write_closed) {
        pipe->wait_readers++;
        pthread_cond_wait(&pipe->cond_read, &pipe->mu);
        pipe->wait_readers--;
    }
    if (pipe->count == 0 && pipe->write_closed) {
        pipe->active_ops--;
        bool destroy = vprocInprocPipeShouldDestroy(pipe);
        pthread_mutex_unlock(&pipe->mu);
        if (destroy) {
            vprocInprocPipeFree(pipe);
        }
        return 0;
    }
    size_t to_copy = bufsize;
    if (to_copy > pipe->count) {
        to_copy = pipe->count;
    }
    for (size_t i = 0; i < to_copy; ++i) {
        ((unsigned char *)buf)[i] = pipe->buf[pipe->head];
        pipe->head = (pipe->head + 1) % pipe->cap;
    }
    pipe->count -= to_copy;
    pipe->active_ops--;
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_cond_signal(&pipe->cond_write);
    pthread_mutex_unlock(&pipe->mu);
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
    pscal_fd_poll_wakeup(fd, POLLIN);
    return (ssize_t)to_copy;
}

static ssize_t vprocInprocPipeWrite(struct pscal_fd *fd, const void *buf, size_t bufsize) {
    if (!fd || !buf) {
        return _EBADF;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe || end->is_reader) {
        return _EBADF;
    }
    VprocInprocPipe *pipe = end->pipe;
    pthread_mutex_lock(&pipe->mu);
    pipe->active_ops++;
    if (pipe->write_closed) {
        pipe->active_ops--;
        pthread_mutex_unlock(&pipe->mu);
        return _EPIPE;
    }
    while (pipe->count == pipe->cap && !pipe->read_closed) {
        pipe->wait_writers++;
        pthread_cond_wait(&pipe->cond_write, &pipe->mu);
        pipe->wait_writers--;
    }
    if (pipe->read_closed) {
        pipe->active_ops--;
        bool destroy = vprocInprocPipeShouldDestroy(pipe);
        pthread_mutex_unlock(&pipe->mu);
        if (destroy) {
            vprocInprocPipeFree(pipe);
        }
        return _EPIPE;
    }
    size_t space = pipe->cap - pipe->count;
    size_t to_copy = bufsize;
    if (to_copy > space) {
        to_copy = space;
    }
    for (size_t i = 0; i < to_copy; ++i) {
        pipe->buf[pipe->tail] = ((const unsigned char *)buf)[i];
        pipe->tail = (pipe->tail + 1) % pipe->cap;
    }
    pipe->count += to_copy;
    pipe->active_ops--;
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_cond_signal(&pipe->cond_read);
    pthread_mutex_unlock(&pipe->mu);
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
    pscal_fd_poll_wakeup(fd, POLLOUT);
    return (ssize_t)to_copy;
}

static int vprocInprocPipePoll(struct pscal_fd *fd) {
    if (!fd) {
        return 0;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe) {
        return 0;
    }
    VprocInprocPipe *pipe = end->pipe;
    int events = 0;
    pthread_mutex_lock(&pipe->mu);
    if (end->is_reader) {
        if (pipe->count > 0) {
            events |= POLLIN;
        }
        if (pipe->write_closed) {
            events |= POLLHUP;
        }
    } else {
        if (!pipe->read_closed && pipe->count < pipe->cap) {
            events |= POLLOUT;
        }
        if (pipe->read_closed) {
            events |= POLLERR;
        }
    }
    pthread_mutex_unlock(&pipe->mu);
    return events;
}

static int vprocInprocPipeClose(struct pscal_fd *fd) {
    if (!fd) {
        return _EBADF;
    }
    VprocInprocEnd *end = (VprocInprocEnd *)fd->userdata;
    if (!end || !end->pipe) {
        return _EBADF;
    }
    VprocInprocPipe *pipe = end->pipe;
    pthread_mutex_lock(&pipe->mu);
    bool dbg = vprocPipeDebugEnabled();
    if (end->is_reader) {
        pipe->read_closed = true;
        if (pipe->readers > 0) {
            pipe->readers--;
        }
    } else {
        pipe->write_closed = true;
        if (pipe->writers > 0) {
            pipe->writers--;
        }
    }
    if (dbg) {
        fprintf(stderr,
                "[pipe-close] pipe=%p end=%s readers=%d writers=%d count=%zu read_closed=%d write_closed=%d active=%d wait_r=%d wait_w=%d\n",
                (void *)pipe,
                end->is_reader ? "r" : "w",
                pipe->readers,
                pipe->writers,
                pipe->count,
                (int)pipe->read_closed,
                (int)pipe->write_closed,
                pipe->active_ops,
                pipe->wait_readers,
                pipe->wait_writers);
    }
    bool destroy = vprocInprocPipeShouldDestroy(pipe);
    pthread_cond_broadcast(&pipe->cond_read);
    pthread_cond_broadcast(&pipe->cond_write);
    pthread_mutex_unlock(&pipe->mu);
    pscal_fd_poll_wakeup(fd, POLLHUP);
    free(end);
    fd->userdata = NULL;
    if (destroy) {
        vprocInprocPipeFree(pipe);
    }
    return 0;
}

static const struct pscal_fd_ops kVprocInprocPipeReadOps = {
    .read = vprocInprocPipeRead,
    .write = NULL,
    .poll = vprocInprocPipePoll,
    .ioctl_size = NULL,
    .ioctl = NULL,
    .close = vprocInprocPipeClose,
};

static const struct pscal_fd_ops kVprocInprocPipeWriteOps = {
    .read = NULL,
    .write = vprocInprocPipeWrite,
    .poll = vprocInprocPipePoll,
    .ioctl_size = NULL,
    .ioctl = NULL,
    .close = vprocInprocPipeClose,
};

int vprocCreateInprocPipe(struct pscal_fd **out_read, struct pscal_fd **out_write) {
    if (!out_read || !out_write) {
        errno = EINVAL;
        return -1;
    }
    *out_read = NULL;
    *out_write = NULL;

    VprocInprocPipe *pipe = (VprocInprocPipe *)calloc(1, sizeof(VprocInprocPipe));
    if (!pipe) {
        errno = ENOMEM;
        return -1;
    }
    pipe->cap = 8192;
    pipe->buf = (unsigned char *)calloc(pipe->cap, sizeof(unsigned char));
    pthread_mutex_init(&pipe->mu, NULL);
    pthread_cond_init(&pipe->cond_read, NULL);
    pthread_cond_init(&pipe->cond_write, NULL);
    pipe->readers = 1;
    pipe->writers = 1;

    VprocInprocEnd *read_end = (VprocInprocEnd *)calloc(1, sizeof(VprocInprocEnd));
    VprocInprocEnd *write_end = (VprocInprocEnd *)calloc(1, sizeof(VprocInprocEnd));
    if (!pipe->buf || !read_end || !write_end) {
        free(read_end);
        free(write_end);
        vprocInprocPipeFree(pipe);
        errno = ENOMEM;
        return -1;
    }
    read_end->pipe = pipe;
    read_end->is_reader = true;
    write_end->pipe = pipe;
    write_end->is_reader = false;

    struct pscal_fd *rfd = pscal_fd_create(&kVprocInprocPipeReadOps);
    struct pscal_fd *wfd = pscal_fd_create(&kVprocInprocPipeWriteOps);
    if (!rfd || !wfd) {
        if (rfd) pscal_fd_close(rfd);
        if (wfd) pscal_fd_close(wfd);
        free(read_end);
        free(write_end);
        vprocInprocPipeFree(pipe);
        errno = ENOMEM;
        return -1;
    }
    rfd->userdata = read_end;
    wfd->userdata = write_end;
    *out_read = rfd;
    *out_write = wfd;
    return 0;
}

typedef struct {
    uint64_t session_id;
    struct pscal_fd *pty_slave;
    struct pscal_fd *pty_master;
    VProcSessionOutputHandler output_handler;
    void *output_context;
} VProcSessionPtyEntry;

static struct {
    VProcSessionPtyEntry *items;
    size_t count;
    size_t capacity;
    pthread_mutex_t mu;
} gVProcSessionPtys = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
};
static uint64_t gVProcSessionPtyHintId = 0;
static size_t gVProcSessionPtyHintIndex = 0;

static bool vprocSessionPtyEntryIsEmpty(const VProcSessionPtyEntry *entry) {
    return !entry || (!entry->pty_slave && !entry->pty_master && !entry->output_handler);
}

static VProcSessionPtyEntry *vprocSessionPtyFindLocked(uint64_t session_id, size_t *out_index) {
    if (out_index) {
        *out_index = 0;
    }
    if (session_id == 0 || !gVProcSessionPtys.items || gVProcSessionPtys.count == 0) {
        return NULL;
    }
    if (gVProcSessionPtyHintId == session_id && gVProcSessionPtyHintIndex < gVProcSessionPtys.count) {
        VProcSessionPtyEntry *hint = &gVProcSessionPtys.items[gVProcSessionPtyHintIndex];
        if (hint->session_id == session_id) {
            if (out_index) {
                *out_index = gVProcSessionPtyHintIndex;
            }
            return hint;
        }
    }
    for (size_t i = 0; i < gVProcSessionPtys.count; ++i) {
        VProcSessionPtyEntry *entry = &gVProcSessionPtys.items[i];
        if (entry->session_id == session_id) {
            gVProcSessionPtyHintId = session_id;
            gVProcSessionPtyHintIndex = i;
            if (out_index) {
                *out_index = i;
            }
            return entry;
        }
    }
    return NULL;
}

static void vprocSessionPtyRemoveAtLocked(size_t idx) {
    if (idx >= gVProcSessionPtys.count) {
        return;
    }
    size_t last = gVProcSessionPtys.count - 1;
    if (idx != last) {
        gVProcSessionPtys.items[idx] = gVProcSessionPtys.items[last];
    }
    gVProcSessionPtys.count--;
    gVProcSessionPtyHintId = 0;
    gVProcSessionPtyHintIndex = 0;
}

static VProcSessionPtyEntry *vprocSessionPtyEnsureLocked(uint64_t session_id) {
    VProcSessionPtyEntry *existing = vprocSessionPtyFindLocked(session_id, NULL);
    if (existing) {
        return existing;
    }
    if (gVProcSessionPtys.count >= gVProcSessionPtys.capacity) {
        size_t new_cap = gVProcSessionPtys.capacity ? gVProcSessionPtys.capacity * 2 : 4;
        VProcSessionPtyEntry *resized = realloc(gVProcSessionPtys.items, new_cap * sizeof(VProcSessionPtyEntry));
        if (!resized) {
            return NULL;
        }
        gVProcSessionPtys.items = resized;
        gVProcSessionPtys.capacity = new_cap;
    }
    size_t idx = gVProcSessionPtys.count++;
    VProcSessionPtyEntry *slot = &gVProcSessionPtys.items[idx];
    memset(slot, 0, sizeof(*slot));
    slot->session_id = session_id;
    gVProcSessionPtyHintId = session_id;
    gVProcSessionPtyHintIndex = idx;
    return slot;
}

static bool vprocPathIsLocationDevice(const char *path);
static bool vprocPathIsLegacyGpsDevice(const char *path);
static int vprocLocationDeviceOpen(VProc *vp, int flags);
static int vprocLocationDeviceOpenHost(int flags);
static inline bool vprocShimHasVirtualContext(void);
static bool vprocToolDebugEnabled(void);
static bool vprocVprocDebugEnabled(void);
static bool vprocPipeDebugEnabled(void);
static bool vprocKillDebugEnabled(void);

static int vprocNextPidSeed(void) {
#if defined(PSCAL_TARGET_IOS)
    return 1;
#else
    int host = (int)getpid();
    if (host < 2000) {
        host += 2000;
    }
    return host;
#endif
}

static void vprocRegistryAdd(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
    }
    if (gVProcRegistryCount >= gVProcRegistryCapacity) {
        size_t new_cap = gVProcRegistryCapacity ? gVProcRegistryCapacity * 2 : 16;
        VProc **resized = (VProc **)realloc(gVProcRegistry, new_cap * sizeof(VProc *));
        if (!resized) {
            pthread_mutex_unlock(&gVProcRegistryMu);
            return;
        }
        gVProcRegistry = resized;
        gVProcRegistryCapacity = new_cap;
    }
    gVProcRegistry[gVProcRegistryCount++] = vp;
    gVProcRegistryHint = vp;
    __atomic_add_fetch(&gVProcRegistryVersion, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&gVProcRegistryMu);
}

static void vprocRegistryRemove(VProc *vp) {
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcRegistryMu);
    for (size_t i = 0; i < gVProcRegistryCount; ++i) {
        if (gVProcRegistry[i] == vp) {
            gVProcRegistry[i] = gVProcRegistry[gVProcRegistryCount - 1];
            gVProcRegistryCount--;
            if (gVProcRegistryHint == vp) {
                gVProcRegistryHint = (gVProcRegistryCount > 0) ? gVProcRegistry[0] : NULL;
            }
            __atomic_add_fetch(&gVProcRegistryVersion, 1, __ATOMIC_RELEASE);
            break;
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
}

static bool vprocRegistryContains(const VProc *vp) {
    if (!vp) {
        return false;
    }
    bool found = false;
    pthread_mutex_lock(&gVProcRegistryMu);
    if (gVProcRegistryHint == vp) {
        found = true;
    } else {
        for (size_t i = 0; i < gVProcRegistryCount; ++i) {
            if (gVProcRegistry[i] == vp) {
                found = true;
                gVProcRegistryHint = gVProcRegistry[i];
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcRegistryMu);
    return found;
}

static void vprocClearThreadState(void) {
    gVProcCurrent = NULL;
    gVProcStackDepth = 0;
    gVProcRegistrySeenVersion = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
    for (size_t i = 0; i < sizeof(gVProcStack) / sizeof(gVProcStack[0]); ++i) {
        gVProcStack[i] = NULL;
    }
}

/* Fast validation for the common case where vp is the current thread's vproc. */
static bool vprocRegistryContainsValidated(VProc *vp) {
    if (!vp) {
        return false;
    }
    if (gVProcTlsReady && vp == gVProcCurrent) {
        unsigned long long version = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
        if (gVProcRegistrySeenVersion == version) {
            return true;
        }
        if (!vprocRegistryContains(vp)) {
            vprocClearThreadState();
            return false;
        }
        gVProcRegistrySeenVersion = version;
        return true;
    }
    return vprocRegistryContains(vp);
}

static VProc *vprocForThread(void);

typedef struct {
    int pid;
    pthread_t tid;
    pthread_t *threads;
    size_t thread_count;
    size_t thread_capacity;
    int parent_pid;
    int pgid;
    int sid;
    bool session_leader;
    int fg_pgid;
    int status;
    int exit_signal;
    bool exited;
    bool stopped;
    bool continued;
    int stop_signo;
    bool zombie;
    bool stop_unsupported;
    int job_id;
    char *label;
    char comm[16];
    int *children;
    size_t child_count;
    size_t child_capacity;
    int sigchld_events;
    bool sigchld_blocked;
    int rusage_utime;
    int rusage_stime;
    bool group_exit;
    int group_exit_code;
    uint32_t blocked_signals;
    uint32_t pending_signals;
    uint32_t ignored_signals;
    int pending_counts[32];
    int fg_override_pgid;
    struct sigaction actions[32];
    uint64_t start_mono_ns;
} VProcTaskEntry;

typedef enum {
    VPROC_SIGCHLD_EVENT_EXIT = 0,
    VPROC_SIGCHLD_EVENT_STOP,
    VPROC_SIGCHLD_EVENT_CONT
} VProcSigchldEvent;

typedef struct {
    VProcTaskEntry *items;
    size_t count;
    size_t capacity;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} VProcTaskTable;

typedef struct {
    void *(*start_routine)(void *);
    void *arg;
    VProc *vp;
    VProcSessionStdio *session_stdio;
    int shell_self_pid;
    int kernel_pid;
    bool detach;
    PSCALRuntimeContext *runtime_ctx;
    pthread_mutex_t *worker_stop_mu;
    pthread_cond_t *worker_stop_cv;
    bool *worker_stopped;
} VProcThreadStartCtx;

static VProcTaskEntry *vprocTaskFindLocked(int pid);
static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid);
static void vprocClearEntryLocked(VProcTaskEntry *entry);
static void vprocCancelListAdd(pthread_t **list, size_t *count, size_t *capacity, pthread_t tid);
static void vprocTaskTableRepairLocked(void);
static int vprocSetForegroundPgidInternal(int sid, int fg_pgid, bool sync_tty);
static void vprocSyncForegroundPgidToSessionTty(int sid, int fg_pgid);
static struct pscal_fd *vprocSessionPscalFdForStd(int fd);
static __thread bool gVprocPipelineStage = false;

__attribute__((weak)) void PSCALRuntimeOnProcessGroupEmpty(int pgid);

static VProcTaskTable gVProcTasks = {
    .items = NULL,
    .count = 0,
    .capacity = 0,
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};
static VProcTaskEntry *gVProcTasksItemsStable = NULL;
static size_t gVProcTasksCapacityStable = 0;
static size_t gVProcTaskFindHint = 0;
static size_t gVProcTaskFreeHint = 0;
#define VPROC_TASK_LOOKUP_CACHE_SIZE 2048u
_Static_assert((VPROC_TASK_LOOKUP_CACHE_SIZE & (VPROC_TASK_LOOKUP_CACHE_SIZE - 1u)) == 0,
               "VPROC_TASK_LOOKUP_CACHE_SIZE must be a power of two");
typedef struct {
    int pid;
    uint32_t idx;
} VProcTaskLookupCacheEntry;
static VProcTaskLookupCacheEntry gVProcTaskLookupCache[VPROC_TASK_LOOKUP_CACHE_SIZE];

static inline size_t vprocTaskLookupSlotForPid(int pid) {
    uint32_t hash = (uint32_t)pid * 2654435761u;
    return (size_t)(hash & (VPROC_TASK_LOOKUP_CACHE_SIZE - 1u));
}

static inline void vprocTaskLookupRememberLocked(int pid, size_t idx) {
    if (pid <= 0 || idx > UINT32_MAX) {
        return;
    }
    VProcTaskLookupCacheEntry *cache = &gVProcTaskLookupCache[vprocTaskLookupSlotForPid(pid)];
    cache->pid = pid;
    cache->idx = (uint32_t)idx;
}

static inline void vprocTaskLookupForgetLocked(int pid) {
    if (pid <= 0) {
        return;
    }
    VProcTaskLookupCacheEntry *cache = &gVProcTaskLookupCache[vprocTaskLookupSlotForPid(pid)];
    if (cache->pid == pid) {
        cache->pid = 0;
        cache->idx = 0;
    }
}

/* -- Helper Functions -- */

static void vprocMaybeNotifyPgidEmptyLocked(int pgid) {
    if (pgid <= 0) {
        return;
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!entry || entry->pid <= 0) {
            continue;
        }
        if (entry->pgid == pgid && !entry->exited) {
            return;
        }
    }
    if (PSCALRuntimeOnProcessGroupEmpty) {
        PSCALRuntimeOnProcessGroupEmpty(pgid);
    }
}

static void vprocSetCommLocked(VProcTaskEntry *entry, const char *label) {
    if (!entry) return;
    if (label && *label) {
        const char *start = label;
        while (*start && isspace((unsigned char)*start)) {
            start++;
        }
        const char *end = start;
        while (*end && !isspace((unsigned char)*end)) {
            end++;
        }
        const char *base = start;
        for (const char *p = start; p < end; ++p) {
            if (*p == '/') {
                base = p + 1;
            }
        }
        size_t len = (size_t)(end - base);
        if (len >= sizeof(entry->comm)) {
            len = sizeof(entry->comm) - 1;
        }
        memcpy(entry->comm, base, len);
        entry->comm[len] = '\0';
    } else {
        memset(entry->comm, 0, sizeof(entry->comm));
    }
}

static bool vprocEntryIsKernel(const VProcTaskEntry *entry) {
    if (!entry) {
        return false;
    }
    if (entry->label && strcmp(entry->label, "kernel") == 0) {
        return true;
    }
    return entry->comm[0] != '\0' && strcmp(entry->comm, "kernel") == 0;
}

static bool vprocPrepareThreadNameLocked(const VProcTaskEntry *entry, char *name, size_t name_len) {
    if (!entry || entry->pid <= 0 || entry->tid == 0 || !name || name_len == 0) {
        return false;
    }
    if (!pthread_equal(entry->tid, pthread_self())) {
        return false;
    }
    if (vprocEntryIsKernel(entry)) {
        snprintf(name, name_len, "kernel");
        return true;
    }
    const char *base = entry->comm[0] ? entry->comm :
                       ((entry->label && entry->label[0]) ? entry->label : "vproc");
    char suffix[8];
    snprintf(suffix, sizeof(suffix), "-%d", entry->pid);
    size_t suffix_len = strlen(suffix);
    size_t base_cap = (suffix_len + 1 < name_len) ? (name_len - suffix_len - 1) : 0;
    if (base_cap > 0) {
        snprintf(name, name_len, "%.*s%s", (int)base_cap, base, suffix);
    } else {
        snprintf(name, name_len, "%s", suffix);
    }
    return true;
}

static void vprocApplyThreadName(const char *name) {
    if (!name || !name[0]) {
        return;
    }
#if defined(__APPLE__)
    if (gVProcTlsReady) {
        vprocInterposeBypassEnter();
        pthread_setname_np(name);
        vprocInterposeBypassExit();
    } else {
        pthread_setname_np(name);
    }
#else
    if (gVProcTlsReady) {
        vprocInterposeBypassEnter();
        pthread_setname_np(pthread_self(), name);
        vprocInterposeBypassExit();
    } else {
        pthread_setname_np(pthread_self(), name);
    }
#endif
}

static void vprocNotifyPidSigchldLocked(int target_pid, VProcSigchldEvent evt);
static void vprocNotifyParentSigchldLocked(const VProcTaskEntry *child, VProcSigchldEvent evt);
static struct sigaction vprocGetSigactionLocked(VProcTaskEntry *entry, int sig);
static int vprocDefaultParentPid(void) {
    VProc *active = vprocCurrent();
    if (active) {
        int active_pid = vprocPid(active);
        if (active_pid > 0) {
            return active_pid;
        }
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        return shell_pid;
    }
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
    /* Test helper threads often create synthetic tasks without first
     * initialising shell/kernel synthetic ids. Fall back to host pid so
     * waitpid parent matching remains deterministic across threads. */
    int host_pid = (int)vprocHostGetpidRaw();
    if (host_pid > 0) {
        return host_pid;
    }
#endif
    int kernel = vprocGetKernelPid();
    if (kernel > 0) {
        return kernel;
    }
    return 0;
}

static int vprocAdoptiveParentPidLocked(const VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0) {
        return 0;
    }
    if (entry->sid > 0 && entry->sid != entry->pid) {
        VProcTaskEntry *sid_entry = vprocTaskFindLocked(entry->sid);
        if (sid_entry) {
            return entry->sid;
        }
    }
    int kernel = vprocGetKernelPid();
    if (kernel > 0 && kernel != entry->pid) {
        return kernel;
    }
    int shell = vprocGetShellSelfPid();
    if (shell > 0 && shell != entry->pid) {
        return shell;
    }
    int host = (int)vprocHostGetpidRaw();
    if (host > 0 && host != entry->pid) {
        return host;
    }
    return 0;
}

static inline uint32_t vprocSigMask(int sig) {
    if (sig <= 0 || sig >= 32) return 0;
    return (1u << sig);
}

int vprocNextJobIdSeed(void) {
    static int next_job_id = 1;
    return __sync_fetch_and_add(&next_job_id, 1);
}

static int vprocRuntimeCenti(const VProcTaskEntry *entry, uint64_t now_ns) {
    if (!entry || entry->start_mono_ns == 0) {
        return 0;
    }
    uint64_t start = entry->start_mono_ns;
    uint64_t delta_ns = (now_ns > start) ? (now_ns - start) : 0;
    int centi = (int)(delta_ns / 10000000ull); /* centiseconds */
    return centi < 0 ? 0 : centi;
}

static int vprocCentiFromMicros(int64_t micros) {
    if (micros <= 0) {
        return 0;
    }
    int64_t centi = micros / 10000;
    if (centi > INT_MAX) {
        return INT_MAX;
    }
    return (int)centi;
}

static bool vprocThreadUsageMicros(pthread_t tid, int64_t *user_us, int64_t *system_us) {
#if defined(__APPLE__)
    thread_t thread_port = pthread_mach_thread_np(tid);
    if (thread_port == MACH_PORT_NULL) {
        return false;
    }
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    kern_return_t kr = thread_info(thread_port, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
    mach_port_deallocate(mach_task_self(), thread_port);
    if (kr != KERN_SUCCESS) {
        return false;
    }
    if (user_us) {
        *user_us = (int64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds;
    }
    if (system_us) {
        *system_us = (int64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
    }
    return true;
#else
    (void)tid;
    if (user_us) *user_us = 0;
    if (system_us) *system_us = 0;
    return false;
#endif
}

static bool vprocComputeCpuTimesLocked(const VProcTaskEntry *entry, int *utime_cs, int *stime_cs) {
    if (!entry) {
        return false;
    }
    int64_t user_total = 0;
    int64_t system_total = 0;
    bool saw = false;
    if (entry->thread_count == 0) {
        int64_t user_us = 0;
        int64_t system_us = 0;
        if (vprocThreadUsageMicros(entry->tid, &user_us, &system_us)) {
            user_total += user_us;
            system_total += system_us;
            saw = true;
        }
    } else {
        for (size_t i = 0; i < entry->thread_count; ++i) {
            pthread_t tid = entry->threads[i];
            int64_t user_us = 0;
            int64_t system_us = 0;
            if (vprocThreadUsageMicros(tid, &user_us, &system_us)) {
                user_total += user_us;
                system_total += system_us;
                saw = true;
            }
        }
    }
    if (!saw) {
        return false;
    }
    if (utime_cs) {
        *utime_cs = vprocCentiFromMicros(user_total);
    }
    if (stime_cs) {
        *stime_cs = vprocCentiFromMicros(system_total);
    }
    return true;
}

static uint64_t vprocNowMonoNs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000000ull + (uint64_t)tv.tv_usec * 1000ull;
}

static inline bool vprocSigIndexValid(int sig) {
    return sig > 0 && sig < 32;
}

static inline bool vprocSignalBlockable(int sig) {
    return !(sig == SIGKILL || sig == SIGSTOP);
}

static inline bool vprocSignalIgnorable(int sig) {
    return vprocSignalBlockable(sig);
}

static void vprocInitEntryDefaultsLocked(VProcTaskEntry *entry, int pid, const VProcTaskEntry *parent) {
    const VProcTaskEntry *inherit_parent = NULL;
    VProc *active = vprocCurrent();
    int shell_pid = vprocGetShellSelfPid();
    if (parent) {
        if ((active && vprocPid(active) == parent->pid) ||
            (shell_pid > 0 && parent->pid == shell_pid)) {
            inherit_parent = parent;
        }
    }
    if (!inherit_parent && (!parent || vprocEntryIsKernel(parent))) {
        int candidate_pid = 0;
        if (active) {
            candidate_pid = vprocPid(active);
        } else if (shell_pid > 0) {
            candidate_pid = shell_pid;
        }
        if (candidate_pid > 0) {
            VProcTaskEntry *candidate = vprocTaskFindLocked(candidate_pid);
            if (candidate) {
                inherit_parent = candidate;
            }
        }
    }
    if (!entry) return;
    memset(entry, 0, sizeof(*entry));
    entry->stop_unsupported = false;
    entry->pid = pid;
    entry->pgid = pid;
    entry->sid = pid;
    entry->session_leader = false;
    entry->fg_pgid = pid;
    entry->sigchld_blocked = false;
    /* Job ids are assigned explicitly by the shell; never inherit them. */
    entry->job_id = 0;
    for (int i = 0; i < 32; ++i) {
        sigemptyset(&entry->actions[i].sa_mask);
        entry->actions[i].sa_handler = SIG_DFL;
        entry->actions[i].sa_flags = 0;
    }
    if (inherit_parent) {
        if (inherit_parent->sid > 0) entry->sid = inherit_parent->sid;
        if (inherit_parent->pgid > 0) entry->pgid = inherit_parent->pgid;
        if (inherit_parent->fg_pgid > 0) entry->fg_pgid = inherit_parent->fg_pgid;
        entry->blocked_signals = inherit_parent->blocked_signals &
                                 ~(vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP));
        entry->ignored_signals = inherit_parent->ignored_signals & ~(vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP));
        entry->sigchld_blocked = inherit_parent->sigchld_blocked;
        memcpy(entry->actions, inherit_parent->actions, sizeof(entry->actions));
        if (shell_pid > 0 && inherit_parent->pid == shell_pid) {
            int job_sigs[] = { SIGTSTP, SIGTTIN, SIGTTOU };
            size_t sig_count = sizeof(job_sigs) / sizeof(job_sigs[0]);
            for (size_t i = 0; i < sig_count; ++i) {
                int sig = job_sigs[i];
                if (!vprocSigIndexValid(sig)) {
                    continue;
                }
                entry->blocked_signals &= ~vprocSigMask(sig);
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_handler = SIG_DFL;
                entry->actions[sig].sa_flags = 0;
                entry->ignored_signals &= ~vprocSigMask(sig);
            }
        }
    }
    entry->start_mono_ns = vprocNowMonoNs();
}

static bool vprocAddChildLocked(VProcTaskEntry *parent, int child_pid) {
    if (!parent || parent->pid <= 0 || child_pid <= 0 || parent->pid == child_pid) {
        return false;
    }
    for (size_t i = 0; i < parent->child_count; ++i) {
        if (parent->children[i] == child_pid) {
            return true;
        }
    }
    if (parent->child_count >= parent->child_capacity) {
        size_t new_cap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        int *resized = realloc(parent->children, new_cap * sizeof(int));
        if (!resized) return false;
        parent->children = resized;
        parent->child_capacity = new_cap;
    }
    parent->children[parent->child_count++] = child_pid;
    return true;
}

static bool vprocSignalBlockedLocked(VProcTaskEntry *entry, int sig) {
    if (!vprocSignalBlockable(sig)) {
        return false;
    }
    uint32_t mask = vprocSigMask(sig);
    return mask != 0 && (entry->blocked_signals & mask);
}

static bool vprocSignalIgnoredLocked(VProcTaskEntry *entry, int sig) {
    if (!vprocSignalIgnorable(sig)) {
        return false;
    }
    if (vprocSigIndexValid(sig) && entry->actions[sig].sa_handler == SIG_IGN) {
        return true;
    }
    uint32_t mask = vprocSigMask(sig);
    return mask != 0 && (entry->ignored_signals & mask);
}

static void vprocMaybeStampRusageLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    int utime = 0;
    int stime = 0;
    if (vprocComputeCpuTimesLocked(entry, &utime, &stime)) {
        if (utime > entry->rusage_utime) {
            entry->rusage_utime = utime;
        }
        if (stime > entry->rusage_stime) {
            entry->rusage_stime = stime;
        }
        return;
    }
    if (entry->rusage_utime == 0 && entry->rusage_stime == 0) {
        uint64_t now = vprocNowMonoNs();
        int centi = vprocRuntimeCenti(entry, now);
        if (centi > 0) {
            entry->rusage_utime = centi;
            entry->rusage_stime = centi / 10; /* crude split fallback */
        }
    }
}

typedef enum {
    VPROC_SIG_IGNORE,
    VPROC_SIG_STOP,
    VPROC_SIG_CONT,
    VPROC_SIG_KILL,
    VPROC_SIG_HANDLER
} VProcSignalAction;

static VProcSignalAction vprocDefaultSignalAction(int sig) {
    switch (sig) {
        case SIGCHLD:
#ifdef SIGWINCH
        case SIGWINCH:
#endif
#ifdef SIGURG
        case SIGURG:
#endif
#ifdef SIGIO
        case SIGIO:
#endif
            return VPROC_SIG_IGNORE;
        case SIGCONT:
            return VPROC_SIG_CONT;
        case SIGTSTP:
        case SIGSTOP:
        case SIGTTIN:
        case SIGTTOU:
            return VPROC_SIG_STOP;
        default:
            return VPROC_SIG_KILL;
    }
}

static VProcSignalAction vprocEffectiveSignalActionLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return vprocDefaultSignalAction(sig);
    }
    struct sigaction sa = entry->actions[sig];
    if (sa.sa_handler == SIG_IGN) {
        return VPROC_SIG_IGNORE;
    }
    if (sa.sa_handler != SIG_DFL) {
        return VPROC_SIG_HANDLER;
    }
    return vprocDefaultSignalAction(sig);
}

static bool vprocEntryIsCurrentThreadLocked(const VProcTaskEntry *entry) {
    if (!entry) {
        return false;
    }
    pthread_t self = pthread_self();
    if (entry->tid && pthread_equal(entry->tid, self)) {
        return true;
    }
    for (size_t i = 0; i < entry->thread_count; ++i) {
        if (pthread_equal(entry->threads[i], self)) {
            return true;
        }
    }
    return false;
}

static void vprocInvokeHandlerLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        return;
    }
    struct sigaction sa = vprocGetSigactionLocked(entry, sig);
    if (sa.sa_handler == SIG_IGN || sa.sa_handler == SIG_DFL) {
        return;
    }
    int saved_blocked = entry->blocked_signals;
    if (!(sa.sa_flags & SA_NODEFER)) {
        entry->blocked_signals |= vprocSigMask(sig);
    }
    /* Apply handler mask. */
    for (int s = 1; s < 32; ++s) {
        if (sigismember(&sa.sa_mask, s)) {
            entry->blocked_signals |= vprocSigMask(s);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (sa.sa_flags & SA_SIGINFO) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_signo = sig;
        info.si_code = SI_USER;
        info.si_pid = entry->parent_pid;
        sa.sa_sigaction(sig, &info, NULL);
    } else {
        sa.sa_handler(sig);
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    entry->blocked_signals = saved_blocked;
}

static void vprocQueuePendingSignalLocked(VProcTaskEntry *entry, int sig) {
    uint32_t mask = vprocSigMask(sig);
    if (mask != 0) {
        entry->pending_signals |= mask;
        if (sig > 0 && sig < 32) {
            if (entry->pending_counts[sig] < INT_MAX) {
                entry->pending_counts[sig]++;
            }
        }
    }
}

static void vprocApplySignalLocked(VProcTaskEntry *entry, int sig) {
    VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);

    if (vprocSignalIgnoredLocked(entry, sig) || action == VPROC_SIG_IGNORE) {
        return;
    }
    if (action == VPROC_SIG_HANDLER) {
        struct sigaction sa = vprocGetSigactionLocked(entry, sig);
        if (sa.sa_flags & SA_RESETHAND) {
            entry->actions[sig].sa_handler = SIG_DFL;
            entry->actions[sig].sa_flags = 0;
            sigemptyset(&entry->actions[sig].sa_mask);
            entry->ignored_signals &= ~vprocSigMask(sig);
        }
        entry->continued = false;
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        vprocInvokeHandlerLocked(entry, sig);
        return;
    }
    if (action == VPROC_SIG_STOP) {
        if (entry->stop_unsupported) {
            vprocQueuePendingSignalLocked(entry, sig);
            return;
        }
        entry->stopped = true;
        entry->continued = false;
        entry->exited = false;
        entry->stop_signo = sig;
        entry->exit_signal = 0;
        entry->status = 128 + sig;
        entry->zombie = false;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_STOP);
    } else if (action == VPROC_SIG_CONT) {
        entry->stopped = false;
        entry->stop_signo = 0;
        entry->exit_signal = 0;
        entry->zombie = false;
        entry->continued = true;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_CONT);
    } else if (sig > 0) {
        entry->status = entry->status & 0xff;
        entry->exit_signal = sig;
        entry->exited = true;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        entry->zombie = true;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);
    }
}

static VProcTaskEntry *vprocSessionLeaderBySidLocked(int sid) {
    if (sid <= 0) {
        return NULL;
    }
    VProcTaskEntry *leader = vprocTaskFindLocked(sid);
    if (leader && leader->sid == sid && leader->session_leader) {
        return leader;
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) {
            continue;
        }
        if (entry->sid == sid && entry->session_leader) {
            gVProcTaskFindHint = i;
            vprocTaskLookupRememberLocked(entry->pid, i);
            return entry;
        }
    }
    return NULL;
}

static int vprocForegroundPgidLocked(int sid) {
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    return leader ? leader->fg_pgid : -1;
}

static bool vprocShouldStopForBackgroundTty(VProc *vp, int sig) {
    if (!vp) {
        return false;
    }
    bool stopped = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    if (entry && entry->stop_unsupported) {
        /* Pipelines and synthetic tasks that disable stops should never be parked. */
        pthread_mutex_unlock(&gVProcTasks.mu);
        return false;
    }
    if (entry && entry->sid > 0) {
        int fg = vprocForegroundPgidLocked(entry->sid);
        if (fg > 0 && entry->pgid != fg) {
            vprocApplySignalLocked(entry, sig);
            pthread_cond_broadcast(&gVProcTasks.cv);
            stopped = true;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return stopped;
}

static bool vprocIsForegroundTask(int pid) {
    if (pid <= 0) {
        return true;
    }
    bool foreground = true;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->sid > 0) {
        int fg = vprocForegroundPgidLocked(entry->sid);
        if (fg > 0 && entry->pgid != fg) {
            foreground = false;
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return foreground;
}

static int vprocForegroundPgidForEntryLocked(const VProcTaskEntry *entry) {
    if (!entry || entry->pid <= 0) {
        return -1;
    }
    if (entry->sid > 0) {
        int fg = vprocForegroundPgidLocked(entry->sid);
        if (fg > 0) {
            return fg;
        }
    }
    if (entry->pgid > 0) {
        return entry->pgid;
    }
    return entry->pid;
}

bool vprocWaitIfStopped(VProc *vp) {
    if (!vp) {
        return false;
    }
    int pid = vprocPid(vp);
    if (pid <= 0) {
        return false;
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0 && pid == shell_pid) {
        return false;
    }
    bool waited = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->stop_unsupported && entry->stopped) {
        /* Clear any lingering stop so readers never block on job-control signals. */
        entry->stopped = false;
        entry->continued = true;
        entry->stop_signo = 0;
        entry->pending_signals &= ~vprocSigMask(SIGTSTP);
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    if (entry && entry->stop_unsupported) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return false;
    }
    while (entry && entry->stopped && !entry->exited) {
        waited = true;
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
        if (entry->pid != pid) {
            entry = vprocTaskFindLocked(pid);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return waited;
}

static void vprocDispatchControlSignal(VProc *vp, int sig) {
    if (!vp) {
        return;
    }
    int pid = vprocPid(vp);
    int shell_pid = vprocGetShellSelfPid();
    if (pid <= 0 || (shell_pid > 0 && pid == shell_pid)) {
        return;
    }
    int fg_pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    fg_pgid = vprocForegroundPgidForEntryLocked(entry);
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (fg_pgid > 0) {
        (void)vprocKillShim(-fg_pgid, sig);
    } else {
        (void)vprocKillShim(pid, sig);
    }
    if (sig == SIGTSTP) {
        (void)vprocWaitIfStopped(vp);
    }
}

static bool vprocDeliverPendingSignalsLocked(VProcTaskEntry *entry) {
    uint32_t pending = entry->pending_signals;
    bool exit_current = false;
    for (int sig = 1; sig < 32; ++sig) {
        uint32_t mask = vprocSigMask(sig);
        if (!(pending & mask)) continue;
        if (vprocSignalBlockedLocked(entry, sig)) continue;
        VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
        if (action == VPROC_SIG_IGNORE || vprocSignalIgnoredLocked(entry, sig)) {
            entry->pending_signals &= ~mask;
            entry->pending_counts[sig] = 0;
            continue;
        }
        vprocApplySignalLocked(entry, sig);
        /* If this is the current thread and the action was a terminating
         * default, arrange to self-terminate after we drop the lock. */
        if (action == VPROC_SIG_KILL && vprocEntryIsCurrentThreadLocked(entry) && entry->exited) {
            exit_current = true;
        }
        entry->pending_signals &= ~mask;
        entry->pending_counts[sig] = 0;
    }
    return exit_current;
}

typedef struct {
    VProcSessionStdio *session;
    int shell_pid;
    int kernel_pid;
    uint64_t generation;
} VProcSessionInputCtx;

static VProcSessionInput *vprocSessionInputEnsure(VProcSessionStdio *session, int shell_pid, int kernel_pid);
static ssize_t vprocSessionReadInput(VProcSessionStdio *session, void *buf, size_t count, bool nonblocking);

static int vprocSessionHostFdForStd(int std_fd) {
    VProc *vp = vprocCurrent();
    if (vp) {
        int host_fd = vprocTranslateFd(vp, std_fd);
        if (host_fd >= 0) {
            return host_fd;
        }
    }
    return std_fd;
}

static bool vprocSessionStdioMatchFd(int session_fd, int std_fd) {
    if (session_fd < 0) {
        return false;
    }
    int host_fd = vprocSessionHostFdForStd(std_fd);
    if (host_fd < 0) {
        return false;
    }
    struct stat session_st;
    struct stat std_st;
    if (vprocHostFstatRaw(session_fd, &session_st) != 0) {
        return false;
    }
    if (vprocHostFstatRaw(host_fd, &std_st) != 0) {
        return false;
    }
    return session_st.st_dev == std_st.st_dev && session_st.st_ino == std_st.st_ino;
}

static bool vprocSessionFdMatchesStd(int fd, int std_fd) {
    if (fd < 0) {
        return false;
    }
    if (fd == std_fd) {
        return true;
    }
    struct stat fd_st;
    struct stat std_st;
    if (vprocHostFstatRaw(fd, &fd_st) != 0) {
        return false;
    }
    if (vprocHostFstatRaw(std_fd, &std_st) != 0) {
        return false;
    }
    return fd_st.st_dev == std_st.st_dev && fd_st.st_ino == std_st.st_ino;
}

static bool vprocSessionFdMatchesHost(int fd, int host_fd) {
    if (fd < 0 || host_fd < 0) {
        return false;
    }
    if (fd == host_fd) {
        return true;
    }
    struct stat fd_st;
    struct stat host_st;
    if (vprocHostFstatRaw(fd, &fd_st) != 0) {
        return false;
    }
    if (vprocHostFstatRaw(host_fd, &host_st) != 0) {
        return false;
    }
    return fd_st.st_dev == host_st.st_dev && fd_st.st_ino == host_st.st_ino;
}

static bool vprocToolDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_TOOL_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocVprocDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_VPROC_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocPipeDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_PIPE_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocKillDebugEnabled(void) {
    static int cached = -1;
    if (cached < 0) {
        cached = getenv("PSCALI_KILL_DEBUG") != NULL ? 1 : 0;
    }
    return cached == 1;
}

static bool vprocIoDebugEnabled(void) {
    static int cached = -1;
    if (cached >= 0) {
        return cached == 1;
    }
    const char *env = getenv("PSCALI_IO_DEBUG");
    if (!env || env[0] == '\0' || strcmp(env, "0") == 0) {
        env = getenv("PSCALI_SSH_DEBUG");
    }
    if (!env || env[0] == '\0') {
        cached = 0;
        return false;
    }
    cached = (strcmp(env, "0") != 0) ? 1 : 0;
    return cached == 1;
}

static void vprocDebugLogf(const char *format, ...) {
    if (!format) {
        return;
    }
    char buf[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    size_t len = strlen(buf);
    if (len == 0) {
        return;
    }
    if (buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }
#if defined(__APPLE__)
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
        return;
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (pscalRuntimeDebugLog) {
        pscalRuntimeDebugLog(buf);
        return;
    }
#endif
#if !defined(PSCAL_TARGET_IOS)
    fprintf(stderr, "%s\n", buf);
#endif
}

static void vprocIoTrace(const char *format, ...) {
    if (!vprocIoDebugEnabled() || !format) {
        return;
    }
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
#if defined(__APPLE__)
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
        return;
    }
#endif
    if (pscalRuntimeDebugLog) {
        pscalRuntimeDebugLog(buf);
    }
}

static void vprocSessionResolveOutputFd(VProcSessionStdio *session, int fd, bool *is_stdout, bool *is_stderr) {
    if (is_stdout) {
        *is_stdout = false;
    }
    if (is_stderr) {
        *is_stderr = false;
    }
    if (!session || !is_stdout || !is_stderr) {
        return;
    }
    if (fd == STDOUT_FILENO) {
        *is_stdout = true;
        return;
    }
    if (fd == STDERR_FILENO) {
        *is_stderr = true;
        return;
    }
    if (vprocSessionFdMatchesStd(fd, STDOUT_FILENO)) {
        *is_stdout = true;
        return;
    }
    if (vprocSessionFdMatchesStd(fd, STDERR_FILENO)) {
        *is_stderr = true;
    }
}

static void vprocSessionPtyRegister(uint64_t session_id,
                                    struct pscal_fd *pty_slave,
                                    struct pscal_fd *pty_master) {
    if (session_id == 0 || !pty_slave || !pty_master) {
        return;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (!entry) {
        pthread_mutex_unlock(&gVProcSessionPtys.mu);
        return;
    }
    if (entry->pty_slave) {
        pscal_fd_close(entry->pty_slave);
    }
    if (entry->pty_master) {
        pscal_fd_close(entry->pty_master);
    }
    entry->pty_slave = pscal_fd_retain(pty_slave);
    entry->pty_master = pscal_fd_retain(pty_master);
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

static void vprocSessionPtyUnregister(uint64_t session_id) {
    if (session_id == 0) {
        return;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    size_t idx = 0;
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, &idx);
    if (entry) {
        if (entry->pty_slave) {
            pscal_fd_close(entry->pty_slave);
        }
        if (entry->pty_master) {
            pscal_fd_close(entry->pty_master);
        }
        vprocSessionPtyRemoveAtLocked(idx);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

int vprocSetSessionWinsize(uint64_t session_id, int cols, int rows) {
    if (session_id == 0 || cols <= 0 || rows <= 0) {
        errno = EINVAL;
        vprocIoTrace("[vproc-io] winsize session=%llu invalid cols=%d rows=%d",
                     (unsigned long long)session_id,
                     cols,
                     rows);
        return -1;
    }
    struct pscal_fd *pty_slave = NULL;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry && entry->pty_slave) {
        pty_slave = pscal_fd_retain(entry->pty_slave);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);

    if (!pty_slave || !pty_slave->tty) {
        if (pty_slave) {
            pscal_fd_close(pty_slave);
        }
        errno = ESRCH;
        vprocIoTrace("[vproc-io] winsize session=%llu missing-pty cols=%d rows=%d",
                     (unsigned long long)session_id,
                     cols,
                     rows);
        return -1;
    }
    struct winsize_ ws;
    ws.col = (word_t)cols;
    ws.row = (word_t)rows;
    ws.xpixel = 0;
    ws.ypixel = 0;
    int tty_sid_before = (int)pty_slave->tty->session;
    int tty_fg_before = (int)pty_slave->tty->fg_group;
    int map_fg_before = (tty_sid_before > 0) ? pscalTtyGetForegroundPgid(tty_sid_before) : -1;
    vprocDebugLogf("[ssh-resize] winsize set session=%llu cols=%d rows=%d tty=%p sid=%d fg=%d map_fg=%d",
                   (unsigned long long)session_id,
                   cols,
                   rows,
                   (void *)pty_slave->tty,
                   tty_sid_before,
                   tty_fg_before,
                   map_fg_before);
    vprocIoTrace("[vproc-io] winsize session=%llu before tty=%p sid=%d fg=%d map_fg=%d cols=%d rows=%d",
                 (unsigned long long)session_id,
                 (void *)pty_slave->tty,
                 tty_sid_before,
                 tty_fg_before,
                 map_fg_before,
                 cols,
                 rows);
    tty_set_winsize(pty_slave->tty, ws);
    int tty_sid_after = (int)pty_slave->tty->session;
    int tty_fg_after = (int)pty_slave->tty->fg_group;
    int map_fg_after = (tty_sid_after > 0) ? pscalTtyGetForegroundPgid(tty_sid_after) : -1;
    pscal_fd_close(pty_slave);
    vprocDebugLogf("[ssh-resize] winsize done session=%llu cols=%d rows=%d sid=%d fg=%d map_fg=%d",
                   (unsigned long long)session_id,
                   cols,
                   rows,
                   tty_sid_after,
                   tty_fg_after,
                   map_fg_after);
    vprocIoTrace("[vproc-io] winsize session=%llu applied cols=%d rows=%d tty_sid=%d tty_fg=%d map_fg=%d",
                 (unsigned long long)session_id,
                 cols,
                 rows,
                 tty_sid_after,
                 tty_fg_after,
                 map_fg_after);
    return 0;
}

void vprocSessionSetOutputHandler(uint64_t session_id,
                                  VProcSessionOutputHandler handler,
                                  void *context) {
    if (session_id == 0) {
        return;
    }
    vprocIoTrace("[vproc-io] set output handler session=%llu handler=%p ctx=%p",
                 (unsigned long long)session_id,
                 (void *)handler,
                 context);
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyEnsureLocked(session_id);
    if (entry) {
        entry->output_handler = handler;
        entry->output_context = context;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

void vprocSessionClearOutputHandler(uint64_t session_id) {
    if (session_id == 0) {
        return;
    }
    vprocIoTrace("[vproc-io] clear output handler session=%llu",
                 (unsigned long long)session_id);
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    size_t idx = 0;
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, &idx);
    if (entry) {
        entry->output_handler = NULL;
        entry->output_context = NULL;
        if (vprocSessionPtyEntryIsEmpty(entry)) {
            vprocSessionPtyRemoveAtLocked(idx);
        }
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
}

static bool vprocSessionGetOutputHandler(uint64_t session_id,
                                         VProcSessionOutputHandler *out_handler,
                                         void **out_context) {
    if (out_handler) {
        *out_handler = NULL;
    }
    if (out_context) {
        *out_context = NULL;
    }
    if (session_id == 0) {
        return false;
    }
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry) {
        if (out_handler) {
            *out_handler = entry->output_handler;
        }
        if (out_context) {
            *out_context = entry->output_context;
        }
        pthread_mutex_unlock(&gVProcSessionPtys.mu);
        return entry->output_handler != NULL;
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    return false;
}

ssize_t vprocSessionWriteToMasterMode(uint64_t session_id,
                                      const void *buf,
                                      size_t len,
                                      bool blocking) {
    if (session_id == 0 || !buf || len == 0) {
        errno = EINVAL;
        return -1;
    }
    vprocIoTrace("[vproc-io] input write session=%llu len=%zu",
                 (unsigned long long)session_id,
                 len);
    struct pscal_fd *master = NULL;
    pthread_mutex_lock(&gVProcSessionPtys.mu);
    VProcSessionPtyEntry *entry = vprocSessionPtyFindLocked(session_id, NULL);
    if (entry && entry->pty_master) {
        master = pscal_fd_retain(entry->pty_master);
    }
    pthread_mutex_unlock(&gVProcSessionPtys.mu);
    if (!master || !master->ops || !master->ops->write) {
        if (master) {
            pscal_fd_close(master);
        }
        errno = EBADF;
        return -1;
    }
    unsigned prev_flags = 0;
    bool restore_flags = false;
    if (!blocking) {
        lock(&master->lock);
        prev_flags = master->flags;
        master->flags |= O_NONBLOCK;
        unlock(&master->lock);
        restore_flags = true;
    }
    size_t off = 0;
    while (off < len) {
        ssize_t w = master->ops->write(master, (const unsigned char *)buf + off, len - off);
        if (w < 0) {
            if (w == _EINTR) {
                continue;
            }
            if (w == _EAGAIN && !blocking) {
                if (restore_flags) {
                    lock(&master->lock);
                    master->flags = prev_flags;
                    unlock(&master->lock);
                }
                pscal_fd_close(master);
                errno = pscalCompatErrno((int)w);
                return off > 0 ? (ssize_t)off : -1;
            }
            if (restore_flags) {
                lock(&master->lock);
                master->flags = prev_flags;
                unlock(&master->lock);
            }
            pscal_fd_close(master);
            errno = pscalCompatErrno((int)w);
            return off > 0 ? (ssize_t)off : -1;
        }
        if (w == 0) {
            break;
        }
        off += (size_t)w;
    }
    if (restore_flags) {
        lock(&master->lock);
        master->flags = prev_flags;
        unlock(&master->lock);
    }
    pscal_fd_close(master);
    return (ssize_t)off;
}

ssize_t vprocSessionWriteToMaster(uint64_t session_id, const void *buf, size_t len) {
    return vprocSessionWriteToMasterMode(session_id, buf, len, true);
}

static bool vprocShellOwnsForegroundLocked(int shell_pid, int *out_fgid) {
    if (out_fgid) {
        *out_fgid = -1;
    }
    if (shell_pid <= 0) {
        return true;
    }
    VProcTaskEntry *entry = vprocTaskFindLocked(shell_pid);
    if (!entry) {
        return true;
    }
    int shell_pgid = entry->pgid;
    int sid = entry->sid;
    int fg = (sid > 0) ? vprocForegroundPgidLocked(sid) : -1;
    if (out_fgid) {
        *out_fgid = fg;
    }
    if (fg <= 0 || shell_pgid <= 0) {
        return true;
    }
    return fg == shell_pgid;
}

static void vprocDispatchControlSignalToForeground(int shell_pid, int sig) {
    if (shell_pid <= 0) {
        return;
    }
    int target_fgid = -1;
    int shell_pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *shell_entry = vprocTaskFindLocked(shell_pid);
    if (shell_entry) {
        shell_pgid = shell_entry->pgid;
    }
    (void)vprocShellOwnsForegroundLocked(shell_pid, &target_fgid);
    pthread_mutex_unlock(&gVProcTasks.mu);
    int override_fg = (pscalRuntimeCurrentForegroundPgid) ? pscalRuntimeCurrentForegroundPgid() : -1;
    if (override_fg > 0) {
        target_fgid = override_fg;
    } else if (target_fgid <= 0) {
        target_fgid = shell_pgid;
    }
    if (target_fgid > 0) {
        int rc = vprocKillShim(-target_fgid, sig);
#if defined(PSCAL_TARGET_IOS)
        if (rc < 0 && sig == SIGINT && pscalRuntimeRequestSigint) {
            pscalRuntimeRequestSigint();
        }
#endif
        return;
    }
#if defined(PSCAL_TARGET_IOS)
    if (sig == SIGINT && pscalRuntimeRequestSigint) {
        pscalRuntimeRequestSigint();
    }
#endif
}

static void *vprocSessionInputThread(void *arg) {
    VProcSessionInputCtx *ctx = (VProcSessionInputCtx *)arg;
    if (!ctx || !ctx->session) {
        free(ctx);
        return NULL;
    }
    VProcSessionStdio *session = ctx->session;
    if (session) {
        vprocSessionStdioActivate(session);
    }
    if (ctx->shell_pid > 0) {
        vprocSetShellSelfPid(ctx->shell_pid);
    }
    if (ctx->kernel_pid > 0) {
        vprocSetKernelPid(ctx->kernel_pid);
    }

    VProcSessionInput *input = session ? session->input : NULL;
    int fd = session ? session->stdin_host_fd : -1;
    struct pscal_fd *pscal_fd = session ? session->stdin_pscal_fd : NULL;
    const bool tool_dbg = vprocToolDebugEnabled();
    if (pscal_fd && (!pscal_fd->ops || !pscal_fd->ops->read)) {
        pscal_fd = NULL;
    }
    unsigned char ch = 0;
    if (tool_dbg) {
        vprocDebugLogf(
                "[session-input] reader start host_fd=%d pscal_fd=%p shell=%d kernel=%d\n",
                fd,
                (void *)pscal_fd,
                ctx->shell_pid,
                ctx->kernel_pid);
    }
    while (fd >= 0 || pscal_fd) {
        if (input) {
            pthread_mutex_lock(&input->mu);
            bool stop_requested = input->stop_requested;
            pthread_mutex_unlock(&input->mu);
            if (stop_requested) {
                if (tool_dbg) {
                    vprocDebugLogf( "[session-input] reader stop fd=%d\n", fd);
                }
                break;
            }
        }
        ssize_t r = 0;
        if (fd >= 0) {
            r = vprocHostRead(fd, &ch, 1);
            if (r <= 0) {
                if (r < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                    usleep(1000);
                    continue;
                }
                if (tool_dbg) {
                    int saved_errno = errno;
                    vprocDebugLogf(
                            "[session-input] reader eof host_fd=%d r=%zd errno=%d\n",
                            fd,
                            r,
                            saved_errno);
                }
                if (input) {
                    pthread_mutex_lock(&input->mu);
                    bool is_current = (input->reader_generation == ctx->generation);
                    if (is_current) {
                        input->eof = true;
                        pthread_cond_broadcast(&input->cv);
                    }
                    pthread_mutex_unlock(&input->mu);
                }
                break;
            }
        } else {
            r = pscal_fd->ops->read(pscal_fd, &ch, 1);
            if (r <= 0) {
                if (r == _EINTR || r == _EAGAIN) {
                    if (r == _EAGAIN) {
                        usleep(1000);
                    }
                    continue;
                }
                if (r < 0) {
                    vprocSetCompatErrno((int)r);
                }
                if (tool_dbg) {
                    int saved_errno = errno;
                    vprocDebugLogf(
                            "[session-input] reader eof pscal_fd=%p r=%zd errno=%d\n",
                            (void *)pscal_fd,
                            r,
                            saved_errno);
                }
                if (input) {
                    pthread_mutex_lock(&input->mu);
                    bool is_current = (input->reader_generation == ctx->generation);
                    if (is_current) {
                        input->eof = true;
                        pthread_cond_broadcast(&input->cv);
                    }
                    pthread_mutex_unlock(&input->mu);
                }
                break;
            }
        }
        if (ch == 3 || ch == 26) {
            int sig = (ch == 3) ? SIGINT : SIGTSTP;
            vprocDispatchControlSignalToForeground(ctx->shell_pid, sig);
            if (input) {
                pthread_mutex_lock(&input->mu);
                input->interrupt_pending = true;
                pthread_cond_broadcast(&input->cv);
                pthread_mutex_unlock(&input->mu);
            }
            continue;
        }
        if (!input) {
            continue;
        }
        pthread_mutex_lock(&input->mu);
        if (input->len + 1 > input->cap) {
            size_t new_cap = input->cap ? input->cap * 2 : 256;
            unsigned char *resized = (unsigned char *)realloc(input->buf, new_cap);
            if (resized) {
                input->buf = resized;
                input->cap = new_cap;
            }
        }
        if (input->len < input->cap) {
            input->buf[input->len++] = ch;
            pthread_cond_signal(&input->cv);
        }
        pthread_mutex_unlock(&input->mu);
    }
    if (input) {
        pthread_mutex_lock(&input->mu);
        bool is_current = (input->reader_generation == ctx->generation);
        if (is_current) {
            input->reader_active = false;
            input->reader_fd = -1;
            input->stop_requested = false;
            pthread_cond_broadcast(&input->cv);
        }
        pthread_mutex_unlock(&input->mu);
    }
    if (session) {
        vprocSessionStdioActivate(NULL);
    }
    free(ctx);
    return NULL;
}

static void *vprocSessionPtyOutputThread(void *arg) {
    VProcSessionStdio *session = (VProcSessionStdio *)arg;
    if (!session) {
        return NULL;
    }
    struct pscal_fd *master = session->pty_master;
    if (!master || !master->ops || !master->ops->read) {
        vprocPtyTrace("[PTY] output thread missing master (master=%p)", (void *)master);
        return NULL;
    }
    pthread_t tid = pthread_self();
    vprocRegisterInterposeBypassThread(tid);
    pthread_setname_np("vproc-pty-out");
    vprocPtyTrace("[PTY] output thread start");
    char buf[1024];
    while (session->pty_active) {
        ssize_t r = master->ops->read(master, buf, sizeof(buf));
        if (r == 0) {
            vprocPtyTrace("[PTY] output thread EOF");
            break;
        }
        if (r < 0) {
            if (r == _EINTR || r == _EAGAIN) {
                if (r == _EAGAIN) {
                    usleep(1000);
                }
                continue;
            }
            vprocPtyTrace("[PTY] output thread read error code=%zd", r);
            session->pty_active = false;
            break;
        }
        VProcSessionOutputHandler handler = NULL;
        void *context = NULL;
        if (vprocSessionGetOutputHandler(session->session_id, &handler, &context) && handler) {
            vprocIoTrace("[vproc-io] output session=%llu len=%zd handler=%p",
                         (unsigned long long)session->session_id,
                         r,
                         (void *)handler);
            handler(session->session_id, (const unsigned char *)buf, (size_t)r, context);
            continue;
        }
        vprocIoTrace("[vproc-io] output drop session=%llu len=%zd (no handler)",
                     (unsigned long long)session->session_id,
                     r);
        vprocPtyTrace("[PTY] output thread drop len=%zd (no handler)", r);
    }
    vprocPtyTrace("[PTY] output thread exit active=%d", session->pty_active ? 1 : 0);
    vprocUnregisterInterposeBypassThread(tid);
    return NULL;
}

static VProcSessionInput *vprocSessionInputEnsure(VProcSessionStdio *session, int shell_pid, int kernel_pid) {
    if (!session) {
        return NULL;
    }
    pthread_mutex_lock(&gSessionInputInitMu);
    const bool tool_dbg = vprocToolDebugEnabled();
    if (!session->input) {
        session->input = (VProcSessionInput *)calloc(1, sizeof(VProcSessionInput));
        if (session->input) {
            pthread_mutex_init(&session->input->mu, NULL);
            pthread_cond_init(&session->input->cv, NULL);
            session->input->inited = true;
            session->input->reader_fd = -1;
        }
    }
    VProcSessionInput *input = session->input;
    if (input) {
        pthread_mutex_lock(&input->mu);
        if (!input->reader_active && input->eof) {
            input->eof = false;
            input->len = 0;
            input->interrupt_pending = false;
        }
        pthread_mutex_unlock(&input->mu);
    }
    bool has_pscal_fd = session->stdin_pscal_fd &&
            session->stdin_pscal_fd->ops &&
            session->stdin_pscal_fd->ops->read;
    if (input && !input->reader_active &&
            (session->stdin_host_fd >= 0 || has_pscal_fd)) {
        VProcSessionInputCtx *ctx = (VProcSessionInputCtx *)calloc(1, sizeof(VProcSessionInputCtx));
        if (ctx) {
            ctx->session = session;
            ctx->shell_pid = shell_pid;
            ctx->kernel_pid = kernel_pid;
            pthread_mutex_lock(&input->mu);
            input->reader_generation++;
            ctx->generation = input->reader_generation;
            input->stop_requested = false;
            pthread_mutex_unlock(&input->mu);
            pthread_t tid;
            int create_rc = vprocHostPthreadCreate(&tid, NULL, vprocSessionInputThread, ctx);
            if (create_rc == 0) {
                pthread_detach(tid);
                pthread_mutex_lock(&input->mu);
                input->reader_active = true;
                input->reader_fd = session->stdin_host_fd;
                input->stop_requested = false;
                pthread_mutex_unlock(&input->mu);
                if (tool_dbg) {
                    vprocDebugLogf(
                            "[session-input] reader spawned host_fd=%d pscal_fd=%p\n",
                            session->stdin_host_fd,
                            (void *)session->stdin_pscal_fd);
                }
            } else {
                if (tool_dbg) {
                    vprocDebugLogf(
                            "[session-input] reader spawn failed rc=%d\n",
                            create_rc);
                }
                free(ctx);
            }
        }
    }
    pthread_mutex_unlock(&gSessionInputInitMu);
    return input;
}

static ssize_t vprocSessionReadInput(VProcSessionStdio *session, void *buf, size_t count, bool nonblocking) {
    if (!session || !session->input || !buf || count == 0) {
        return 0;
    }
    VProcSessionInput *input = session->input;
    const bool tool_dbg = vprocToolDebugEnabled();
    if (tool_dbg) {
        pthread_mutex_lock(&input->mu);
        vprocDebugLogf(
                "[session-read] start len=%zu eof=%d reader=%d fd=%d stdin=%d\n",
                input->len,
                (int)input->eof,
                (int)input->reader_active,
                input->reader_fd,
                session->stdin_host_fd);
        pthread_mutex_unlock(&input->mu);
    }
    pthread_mutex_lock(&input->mu);
    if (nonblocking && input->len == 0 && !input->eof && !input->interrupt_pending) {
        pthread_mutex_unlock(&input->mu);
        errno = EAGAIN;
        return -1;
    }
    while (input->len == 0 && !input->eof && !input->interrupt_pending) {
        pthread_cond_wait(&input->cv, &input->mu);
    }
    if (input->interrupt_pending) {
        input->interrupt_pending = false;
        pthread_mutex_unlock(&input->mu);
        errno = EINTR;
        return -1;
    }
    if (input->len == 0 && input->eof) {
        if (tool_dbg) {
            vprocDebugLogf( "[session-read] eof\n");
        }
        pthread_mutex_unlock(&input->mu);
        return 0;
    }
    size_t to_copy = (count < input->len) ? count : input->len;
    memcpy(buf, input->buf, to_copy);
    input->len -= to_copy;
    if (input->len > 0) {
        memmove(input->buf, input->buf + to_copy, input->len);
    }
    pthread_mutex_unlock(&input->mu);
    return (ssize_t)to_copy;
}

ssize_t vprocSessionReadInputShim(void *buf, size_t count) {
    return vprocSessionReadInputShimMode(buf, count, false);
}

ssize_t vprocSessionReadInputShimMode(void *buf, size_t count, bool nonblocking) {
    if (!buf || count == 0) {
        errno = EINVAL;
        return -1;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        errno = EBADF;
        return -1;
    }
    const bool tool_dbg = vprocToolDebugEnabled();
    if (session->stdin_host_fd >= 0) {
        if (tool_dbg) {
            vprocDebugLogf(
                    "[session-read] direct stdin=%d nonblock=%d\n",
                    session->stdin_host_fd,
                    (int)nonblocking);
        }
        (void)nonblocking;
        return vprocHostRead(session->stdin_host_fd, buf, count);
    }
    VProcSessionInput *input = vprocSessionInputEnsure(session,
                                                       vprocGetShellSelfPid(),
                                                       vprocGetKernelPid());
    if (!input) {
        errno = EBADF;
        return -1;
    }
    if (tool_dbg) {
        vprocDebugLogf("[session-read] buffered nonblock=%d\n", (int)nonblocking);
    }
    return vprocSessionReadInput(session, buf, count, nonblocking);
}

VProcSessionInput *vprocSessionInputEnsureShim(void) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        return NULL;
    }
    int shell_pid = vprocGetShellSelfPid();
    int kernel_pid = vprocGetKernelPid();
    const bool tool_dbg = vprocToolDebugEnabled();
    if (tool_dbg) {
        vprocDebugLogf(
                "[session-input] ensure shell=%d kernel=%d stdin_host=%d input=%p\n",
                shell_pid,
                kernel_pid,
                session->stdin_host_fd,
                (void *)session->input);
    }
    return vprocSessionInputEnsure(session, shell_pid, kernel_pid);
}

bool vprocSessionInjectInputShim(const void *data, size_t len) {
    if (!data || len == 0) {
        return false;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session) {
        return false;
    }
    int shell_pid = vprocGetShellSelfPid();
    int kernel_pid = vprocGetKernelPid();
    VProcSessionInput *input = vprocSessionInputEnsure(session, shell_pid, kernel_pid);
    if (!input) {
        return false;
    }
    const bool tool_dbg = vprocToolDebugEnabled();
    pthread_mutex_lock(&input->mu);
    size_t needed = input->len + len;
    if (needed > input->cap) {
        size_t new_cap = input->cap ? input->cap : 256;
        while (new_cap < needed) {
            new_cap *= 2;
        }
        unsigned char *resized = (unsigned char *)realloc(input->buf, new_cap);
        if (!resized) {
            pthread_mutex_unlock(&input->mu);
            return false;
        }
        input->buf = resized;
        input->cap = new_cap;
    }
    memcpy(input->buf + input->len, data, len);
    input->len += len;
    pthread_cond_broadcast(&input->cv);
    if (tool_dbg) {
        vprocDebugLogf(
                "[session-input] injected len=%zu total=%zu cap=%zu\n",
                len,
                input->len,
                input->cap);
    }
    pthread_mutex_unlock(&input->mu);
    return true;
}

static void vprocRemoveChildLocked(VProcTaskEntry *parent, int child_pid) {
    if (!parent || !parent->children || parent->child_count == 0) return;
    for (size_t i = 0; i < parent->child_count;) {
        if (parent->children[i] == child_pid) {
            parent->children[i] = parent->children[parent->child_count - 1];
            parent->child_count--;
            continue;
        }
        ++i;
    }
}

static void vprocUpdateParentLocked(int child_pid, int new_parent_pid) {
    if (child_pid <= 0) return;
    if (new_parent_pid == child_pid) {
        /* Reject self-parenting to avoid cycles in parent/child bookkeeping. */
        new_parent_pid = 0;
    }
    VProcTaskEntry *child_entry = vprocTaskFindLocked(child_pid);
    if (!child_entry) return;
    int old_parent = child_entry->parent_pid;
    if (old_parent == new_parent_pid) return;
    if (old_parent > 0) {
        VProcTaskEntry *old_parent_entry = vprocTaskFindLocked(old_parent);
        if (old_parent_entry) {
            vprocRemoveChildLocked(old_parent_entry, child_pid);
        }
    }
    VProcTaskEntry *new_parent_entry = NULL;
    if (new_parent_pid > 0) {
        new_parent_entry = vprocTaskEnsureSlotLocked(new_parent_pid);
        if (!new_parent_entry || new_parent_entry->pid != new_parent_pid) {
            new_parent_pid = 0;
            new_parent_entry = NULL;
        }
    }
    child_entry->parent_pid = new_parent_pid;
    if (new_parent_pid > 0) {
        if (new_parent_entry) {
            if (!vprocAddChildLocked(new_parent_entry, child_pid)) {
                child_entry->parent_pid = 0;
            }
        }
    }
}

static void vprocReparentChildrenLocked(int parent_pid, int new_parent_pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(parent_pid);
    if (!entry || entry->child_count == 0 || !entry->children) {
        if (entry) {
            entry->child_count = 0;
        }
        return;
    }
    while (entry->child_count > 0) {
        int child_pid = entry->children[entry->child_count - 1];
        entry->child_count--;
        if (child_pid > 0) {
            vprocUpdateParentLocked(child_pid, new_parent_pid);
        }
    }
}

static void vprocNotifyPidSigchldLocked(int target_pid, VProcSigchldEvent evt) {
    if (target_pid <= 0) {
        return;
    }
    VProcTaskEntry *target_entry = vprocTaskFindLocked(target_pid);
    if (!target_entry) {
        return;
    }
    if (evt == VPROC_SIGCHLD_EVENT_STOP) {
        struct sigaction sa = vprocGetSigactionLocked(target_entry, SIGCHLD);
        if (sa.sa_flags & SA_NOCLDSTOP) {
            return;
        }
    }
    target_entry->sigchld_events++;
    vprocQueuePendingSignalLocked(target_entry, SIGCHLD);
    if (!target_entry->sigchld_blocked) {
        (void)vprocDeliverPendingSignalsLocked(target_entry);
    }
}

static void vprocNotifyParentSigchldLocked(const VProcTaskEntry *child, VProcSigchldEvent evt) {
    if (!child) {
        return;
    }
    int parent_pid = child->parent_pid;
    vprocNotifyPidSigchldLocked(parent_pid, evt);

    int kernel_pid = vprocGetKernelPid();
    if (kernel_pid > 0 && parent_pid == kernel_pid) {
        int sid = child->sid;
        if (sid > 0 && sid != parent_pid && sid != child->pid) {
            vprocNotifyPidSigchldLocked(sid, evt);
        }
    }
}

static int vprocWaiterPid(void) {
    VProc *cur = vprocCurrent();
    if (cur) {
        return vprocPid(cur);
    }
    int shell = vprocGetShellSelfPid();
    if (shell > 0) {
        return shell;
    }
    return (int)vprocHostGetpidRaw();
}

// NOTE: Caller must hold vp->mu
static int vprocAllocSlot(VProc *vp) {
    if (!vp) {
        return -1;
    }
    int capacity = (int)vp->capacity;
    if (capacity <= 0) {
        return -1;
    }
    int start = vp->next_fd;
    if (start < 0 || start >= capacity) {
        start %= capacity;
        if (start < 0) {
            start += capacity;
        }
    }
    for (int idx = start; idx < capacity; ++idx) {
        if (vp->entries[idx].kind == VPROC_FD_NONE) {
            vp->next_fd = (idx + 1) % capacity;
            return idx;
        }
    }
    for (int idx = 0; idx < start; ++idx) {
        if (vp->entries[idx].kind == VPROC_FD_NONE) {
            vp->next_fd = (idx + 1) % capacity;
            return idx;
        }
    }
    size_t new_cap = vp->capacity ? vp->capacity * 2 : VPROC_INITIAL_CAPACITY;
    VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
    if (!resized) {
        return -1;
    }
    for (size_t i = vp->capacity; i < new_cap; ++i) {
        resized[i].host_fd = -1;
        resized[i].pscal_fd = NULL;
        resized[i].kind = VPROC_FD_NONE;
    }
    int idx = (int)vp->capacity;
    vp->entries = resized;
    vp->capacity = new_cap;
    vp->next_fd = (idx + 1) % (int)vp->capacity;
    return idx;
}

// NOTE: Caller must hold vp->mu, or wrap this
static int vprocInsertLocked(VProc *vp, int host_fd) {
    if (!vp || host_fd < 0) {
        errno = EBADF;
        return -1;
    }
    int slot = vprocAllocSlot(vp);
    if (slot < 0) {
        return -1;
    }
    vp->entries[slot].host_fd = host_fd;
    vp->entries[slot].pscal_fd = NULL;
    vp->entries[slot].kind = VPROC_FD_HOST;
    vprocResourceTrackLocked(vp, host_fd, VPROC_RESOURCE_GENERIC);
    return slot;
}

static int vprocInsert(VProc *vp, int host_fd) {
    if (!vp) return -1;
    pthread_mutex_lock(&vp->mu);
    int rc = vprocInsertLocked(vp, host_fd);
    pthread_mutex_unlock(&vp->mu);
    return rc;
}

int vprocAdoptHostFd(VProc *vp, int host_fd) {
    return vprocInsert(vp, host_fd);
}

static int vprocInsertPscalFd(VProc *vp, struct pscal_fd *fd) {
    if (!vp || !fd) {
        errno = EBADF;
        return -1;
    }
    /* The fd table must own its own reference so callers can freely drop
     * theirs after insertion. */
    struct pscal_fd *retained = pscal_fd_retain(fd);
    if (!retained) {
        errno = ENOMEM;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    int slot = vprocAllocSlot(vp);
    if (slot >= 0) {
        vp->entries[slot].host_fd = -1;
        vp->entries[slot].pscal_fd = retained;
        vp->entries[slot].kind = VPROC_FD_PSCAL;
    }
    pthread_mutex_unlock(&vp->mu);
    if (slot < 0) {
        pscal_fd_close(retained);
    }
    return slot;
}

struct pscal_fd *vprocGetPscalFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        return NULL;
    }
    struct pscal_fd *res = NULL;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity && vp->entries[fd].kind == VPROC_FD_PSCAL) {
        res = pscal_fd_retain(vp->entries[fd].pscal_fd);
    }
    pthread_mutex_unlock(&vp->mu);
    return res;
}

static int vprocCloneFd(int source_fd) {
    int duped = fcntl(source_fd, F_DUPFD_CLOEXEC, 0);
    if (duped < 0 && errno == EINVAL) {
        duped = fcntl(source_fd, F_DUPFD, 0);
    }
    if (duped < 0) {
        duped = vprocHostDupRaw(source_fd);
    }
    if (duped >= 0) {
        fcntl(duped, F_SETFD, FD_CLOEXEC);
    }
    return duped;
}

static bool vprocPathMatches(const char *path, const char *target) {
    if (!path || !target) {
        return false;
    }
    if (strcmp(path, target) == 0) {
        return true;
    }
    /* Allow any sandbox path that still ends with the target (e.g., /var/.../dev/location). */
    const char *sub = strstr(path, target);
    if (sub && strcmp(sub, target) == 0) {
        return true;
    }
    /* Allow sandboxed prefixes (e.g., /private/var/.../dev/location). */
    size_t target_len = strlen(target);
    size_t path_len = strlen(path);
    if (path_len >= target_len) {
        if (strcmp(path + (path_len - target_len), target) == 0) {
            return true;
        }
    }
    if (strncmp(path, "/private", 8) == 0) {
        const char *trimmed = path + 8;
        if (strcmp(trimmed, target) == 0) {
            return true;
        }
    }
    return false;
}

static bool vprocLocationDebugEnabled(void) {
    const char *env = getenv("PSCALI_LOCATION_DEBUG");
    if (env && env[0] != '\0' && strcmp(env, "0") != 0) {
        return true;
    }
    return false;
}

static void vprocLocationDebugf(const char *fmt, ...) {
    if (!fmt || !vprocLocationDebugEnabled()) {
        return;
    }
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    vprocDebugLogf("%s", buf);
}

static void vprocEnsurePathParent(const char *path) {
    if (!path || path[0] != '/') {
        return;
    }
    char buf[PATH_MAX];
    strncpy(buf, path, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    size_t len = strlen(buf);
    for (size_t i = 1; i < len; ++i) {
        if (buf[i] == '/') {
            buf[i] = '\0';
            (void)mkdir(buf, 0777);
            buf[i] = '/';
        }
    }
}

static void vprocLocationEnsureStubPath(const char *path, int *opened_fd) {
    if (!path) {
        return;
    }
    if (vprocLocationDebugEnabled()) {
        vprocLocationDebugf("ensuring location stub at %s", path);
    }
    vprocEnsurePathParent(path);
    struct stat st;
    if (stat(path, &st) == 0) {
        if (!S_ISFIFO(st.st_mode)) {
            if (unlink(path) != 0 && vprocLocationDebugEnabled()) {
                vprocLocationDebugf("failed to unlink non-fifo location stub %s: %s",
                                    path,
                                    strerror(errno));
            }
        }
    }
    if (mkfifo(path, 0666) != 0 && errno != EEXIST) {
        if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("failed to mkfifo location stub at %s: %s",
                                path,
                                strerror(errno));
        }
    }
    if (opened_fd && *opened_fd < 0) {
        int fd = vprocHostOpenRawInternal(path, O_RDWR | O_NONBLOCK, 0666, true);
        if (fd >= 0) {
            *opened_fd = fd;
            if (vprocLocationDebugEnabled()) {
                vprocLocationDebugf("opened location stub fifo fd=%d path=%s", fd, path);
            }
        } else if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("failed to open location stub fifo %s: %s",
                                path,
                                strerror(errno));
        }
    }
}

static void vprocLocationDeviceEnsureStub(void) {
#if defined(PSCAL_TARGET_IOS)
    char expanded[PATH_MAX];
    const char *path = kLocationDevicePath;
    if (pathTruncateEnabled() && pathTruncateExpand(kLocationDevicePath, expanded, sizeof(expanded))) {
        path = expanded;
    }
    vprocLocationEnsureStubPath(path, &gLocationDevice.stub_fd);
    /* Keep legacy aliases discoverable and open to avoid FIFO open blocking. */
    const char *gps_paths[] = { kLegacyGpsDevicePath, kLegacyGpsDevicePath2 };
    for (size_t i = 0; i < sizeof(gps_paths) / sizeof(gps_paths[0]); ++i) {
        const char *gps = gps_paths[i];
        const char *target = gps;
        char expanded_gps[PATH_MAX];
        if (pathTruncateEnabled() && pathTruncateExpand(gps, expanded_gps, sizeof(expanded_gps))) {
            target = expanded_gps;
        }
        vprocLocationEnsureStubPath(target, NULL);
    }
#else
    (void)kLocationDevicePath;
#endif
}

static bool vprocPathIsLocationDevice(const char *path) {
    if (vprocPathMatches(path, kLocationDevicePath)) {
        return true;
    }
    /* Backward compatibility: some callers still open /dev/gps. */
    if (vprocPathMatches(path, kLegacyGpsDevicePath)) {
        return true;
    }
    return vprocPathMatches(path, kLegacyGpsDevicePath2);
}

static bool vprocPathIsLegacyGpsDevice(const char *path) {
    return vprocPathMatches(path, kLegacyGpsDevicePath) || vprocPathMatches(path, kLegacyGpsDevicePath2);
}

static bool vprocPathIsDevTty(const char *path) {
    return vprocPathMatches(path, "/dev/tty") || vprocPathMatches(path, "/private/dev/tty");
}

static bool vprocPathIsDevConsole(const char *path) {
    return vprocPathMatches(path, "/dev/console") || vprocPathMatches(path, "/private/dev/console");
}

static bool vprocPathIsPtyMaster(const char *path) {
    return vprocPathMatches(path, "/dev/ptmx") ||
           vprocPathMatches(path, "/private/dev/ptmx") ||
           vprocPathMatches(path, "/dev/pts/ptmx") ||
           vprocPathMatches(path, "/private/dev/pts/ptmx");
}

static bool vprocPathIsDevPtsRoot(const char *path) {
    return vprocPathMatches(path, "/dev/pts") || vprocPathMatches(path, "/private/dev/pts");
}

static bool vprocPathIsSystemPath(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    const char *prefixes[] = { "/System", "/usr", "/Library", "/Applications" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t len = strlen(prefix);
        if (strncmp(path, prefix, len) != 0) {
            continue;
        }
        if (path[len] == '\0' || path[len] == '/') {
            return true;
        }
    }
    return false;
}

static const char *vprocPathExpandForShim(const char *path, char *buf, size_t buf_size) {
    if (!path) {
        return NULL;
    }
    if (vprocPathIsSystemPath(path)) {
        return path;
    }
    if (pathTruncateEnabled() && pathTruncateExpand(path, buf, buf_size)) {
        return buf;
    }
    return path;
}

static bool vprocPathParsePtySlave(const char *path, int *out_num) {
    if (!path || !out_num) {
        return false;
    }
    const char *prefixes[] = { "/dev/pts/", "/private/dev/pts/" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(path, prefix, prefix_len) != 0) {
            continue;
        }
        const char *num_str = path + prefix_len;
        if (*num_str == '\0') {
            return false;
        }
        char *end = NULL;
        long parsed = strtol(num_str, &end, 10);
        if (!end || *end != '\0' || parsed < 0 || parsed > INT_MAX) {
            return false;
        }
        *out_num = (int)parsed;
        return true;
    }
    return false;
}

static int vprocPtyApplyAttrsByNum(int pty_num,
                                   const mode_t *mode,
                                   const uid_t *uid,
                                   const gid_t *gid) {
    mode_t_ perms_local;
    uid_t_ uid_local;
    gid_t_ gid_local;
    const mode_t_ *perms_ptr = NULL;
    const uid_t_ *uid_ptr = NULL;
    const gid_t_ *gid_ptr = NULL;
    if (mode) {
        perms_local = (mode_t_)(*mode & 0777);
        perms_ptr = &perms_local;
    }
    if (uid) {
        uid_local = (uid_t_)(*uid);
        uid_ptr = &uid_local;
    }
    if (gid) {
        gid_local = (gid_t_)(*gid);
        gid_ptr = &gid_local;
    }
    return pscalPtySetSlaveInfo(pty_num, perms_ptr, uid_ptr, gid_ptr);
}

static int vprocPtyApplyAttrsForFd(struct pscal_fd *fd,
                                   const mode_t *mode,
                                   const uid_t *uid,
                                   const gid_t *gid) {
    if (!fd || !fd->tty) {
        return _EBADF;
    }
    struct tty *tty = fd->tty;
    if (tty->driver == &pty_master) {
        tty = tty->pty.other;
    }
    if (!tty || tty->driver != &pty_slave) {
        return _ENOTTY;
    }
    return vprocPtyApplyAttrsByNum(tty->num, mode, uid, gid);
}

static bool vprocPathParseConsoleTty(const char *path, int *out_num) {
    if (!path || !out_num) {
        return false;
    }
    const char *prefixes[] = { "/dev/tty", "/private/dev/tty" };
    for (size_t i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        const char *prefix = prefixes[i];
        size_t prefix_len = strlen(prefix);
        if (strncmp(path, prefix, prefix_len) != 0) {
            continue;
        }
        const char *num_str = path + prefix_len;
        if (*num_str == '\0') {
            return false;
        }
        char *end = NULL;
        long parsed = strtol(num_str, &end, 10);
        if (!end || *end != '\0' || parsed <= 0 || parsed > INT_MAX) {
            return false;
        }
        *out_num = (int)parsed;
        return true;
    }
    return false;
}

static void vprocLocationDeviceCloseLocked(void) {
    if (gLocationDevice.stub_fd >= 0) {
        vprocHostCloseRaw(gLocationDevice.stub_fd);
        gLocationDevice.stub_fd = -1;
    }
    gLocationDevice.readers = 0;
    gLocationDevice.has_payload = false;
    gLocationDevice.last_len = 0;
}

static void vprocLocationDrainPipeLocked(void) {
    if (gLocationDevice.read_fd < 0) {
        return;
    }
    int old_flags = fcntl(gLocationDevice.read_fd, F_GETFL);
    bool restore_flags = false;
    if (old_flags >= 0 && (old_flags & O_NONBLOCK) == 0) {
        fcntl(gLocationDevice.read_fd, F_SETFL, old_flags | O_NONBLOCK);
        restore_flags = true;
    }
    unsigned char buf[256];
    for (;;) {
        ssize_t r = vprocHostReadRaw(gLocationDevice.read_fd, buf, sizeof(buf));
        if (r > 0) {
            continue;
        }
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        break;
    }
    if (restore_flags) {
        fcntl(gLocationDevice.read_fd, F_SETFL, old_flags);
    }
}

static int vprocLocationDeviceEnsurePipeLocked(void) {
    return 0;
}

static void vprocLocationEnsureCond(void) {
    if (!gLocationDevice.cond_init) {
        pthread_cond_init(&gLocationDevice.cond, NULL);
        gLocationDevice.cond_init = true;
    }
}

static int vprocLocationDeviceOpenHost(int flags) {
    int access_mode = flags & O_ACCMODE;
    if (access_mode == O_WRONLY) {
        errno = EACCES;
        return -1;
    }
    bool debug = vprocLocationDebugEnabled();
    pthread_mutex_lock(&gLocationDevice.mu);
    if (!gLocationDevice.enabled) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        if (debug) {
            vprocLocationDebugf("open /dev/location denied (device disabled)");
        }
        errno = ENOENT;
        return -1;
    }
    /* No direct host support in the per-open blocking model. */
    pthread_mutex_unlock(&gLocationDevice.mu);
    errno = ENOTSUP;
    return -1;
}

static int vprocLocationDeviceOpen(VProc *vp, int flags) {
    (void)flags;
    if (!vp) {
        return vprocLocationDeviceOpenHost(flags);
    }
    struct pscal_fd *loc_fd = pscal_fd_create(&kVprocLocationFdOps);
    if (!loc_fd) {
        errno = ENOMEM;
        return -1;
    }
    VProcLocationReader *reader = (VProcLocationReader *)calloc(1, sizeof(VProcLocationReader));
    if (!reader) {
        pscal_fd_close(loc_fd);
        errno = ENOMEM;
        return -1;
    }
    reader->last_seq = 0;
    loc_fd->userdata = reader;
    pthread_mutex_lock(&gLocationDevice.mu);
    vprocLocationEnsureCond();
    /* New readers should only observe writes that occur after open(). */
    reader->last_seq = gLocationDevice.seq;
    gLocationDevice.readers++;
    int readers = gLocationDevice.readers;
    pthread_mutex_unlock(&gLocationDevice.mu);
    vprocLocationNotifyObservers(readers);
    int slot = vprocInsertPscalFd(vp, loc_fd);
    if (slot < 0) {
        free(reader);
        pscal_fd_close(loc_fd);
        pthread_mutex_lock(&gLocationDevice.mu);
        if (gLocationDevice.readers > 0) {
            gLocationDevice.readers--;
        }
        readers = gLocationDevice.readers;
        pthread_mutex_unlock(&gLocationDevice.mu);
        vprocLocationNotifyObservers(readers);
    }
    else {
        /* Transfer ownership to the fd table. */
        pscal_fd_close(loc_fd);
    }
    return slot;
}

static ssize_t vprocLocationPscalRead(struct pscal_fd *fd, void *buf, size_t bufsize) {
    if (!fd || !buf || bufsize == 0) {
        return _EINVAL;
    }
    VProcLocationReader *reader = (VProcLocationReader *)fd->userdata;
    if (!reader) {
        return _EBADF;
    }
    if (reader->done) {
        return 0;
    }
    pthread_mutex_lock(&gLocationDevice.mu);
    vprocLocationEnsureCond();
    while (gLocationDevice.enabled && !reader->done) {
        /* If the reader already has buffered data, serve it first. */
        if (reader->len > reader->offset) {
            break;
        }
        /* If there is a fresh payload, copy it into the reader-local buffer. */
        if (gLocationDevice.has_payload && reader->last_seq != gLocationDevice.seq) {
            size_t copy_len = gLocationDevice.last_len;
            if (copy_len >= sizeof(reader->payload)) {
                copy_len = sizeof(reader->payload) - 1;
            }
            memcpy(reader->payload, gLocationDevice.last_payload, copy_len);
            reader->payload[copy_len] = '\0';
            reader->len = copy_len;
            reader->offset = 0;
            reader->last_seq = gLocationDevice.seq;
            break;
        }
        pthread_cond_wait(&gLocationDevice.cond, &gLocationDevice.mu);
    }
    if (!gLocationDevice.enabled) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        reader->done = true;
        return 0;
    }
    /* No data available and already marked done: return EOF. */
    if (reader->done) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        return 0;
    }
    size_t remaining = reader->len - reader->offset;
    /* Deliver the entire payload in one read; bail if the caller's buffer is too small. */
    if (remaining > bufsize) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        return _EINVAL;
    }
    memcpy(buf, reader->payload + reader->offset, remaining);
    reader->offset = 0;
    reader->len = 0;
    reader->done = true;
    pthread_mutex_unlock(&gLocationDevice.mu);
    return (ssize_t)remaining;
}

static int vprocLocationPscalPoll(struct pscal_fd *fd) {
    VProcLocationReader *reader = (VProcLocationReader *)fd->userdata;
    if (!reader) {
        return 0;
    }
    pthread_mutex_lock(&gLocationDevice.mu);
    int events = 0;
    if (reader->done || !gLocationDevice.enabled) {
        events = POLLHUP;
    } else if (reader->len > reader->offset) {
        events = POLLIN;
    } else if (gLocationDevice.has_payload && reader->last_seq != gLocationDevice.seq) {
        events = POLLIN;
    }
    pthread_mutex_unlock(&gLocationDevice.mu);
    return events;
}

static int vprocLocationPscalClose(struct pscal_fd *fd) {
    VProcLocationReader *reader = (VProcLocationReader *)fd->userdata;
    free(reader);
    pthread_mutex_lock(&gLocationDevice.mu);
    if (gLocationDevice.readers > 0) {
        gLocationDevice.readers--;
    }
    int readers = gLocationDevice.readers;
    pthread_mutex_unlock(&gLocationDevice.mu);
    vprocLocationNotifyObservers(readers);
    return 0;
}

static const struct pscal_fd_ops kVprocLocationFdOps = {
    .read = vprocLocationPscalRead,
    .write = NULL,
    .poll = vprocLocationPscalPoll,
    .ioctl_size = NULL,
    .ioctl = NULL,
    .close = vprocLocationPscalClose,
};

void vprocLocationDeviceSetEnabled(bool enabled) {
    pthread_mutex_lock(&gLocationDevice.mu);
    bool changed = (gLocationDevice.enabled != enabled);
    gLocationDevice.enabled = enabled;
    int readers = gLocationDevice.readers;
    if (!enabled) {
        if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("location device disabled (changed=%s)", changed ? "true" : "false");
        }
        vprocLocationDeviceCloseLocked();
        pthread_cond_broadcast(&gLocationDevice.cond);
        pscal_fd_poll_wakeup(NULL, POLLERR);
    } else {
        if (vprocLocationDebugEnabled()) {
            vprocLocationDebugf("location device enabled (changed=%s)", changed ? "true" : "false");
        }
        vprocLocationDeviceEnsureStub();
        if (vprocLocationDeviceEnsurePipeLocked() != 0 && vprocLocationDebugEnabled()) {
            vprocLocationDebugf("location pipe setup failed during enable: %s", strerror(errno));
        }
        pthread_cond_broadcast(&gLocationDevice.cond);
        pscal_fd_poll_wakeup(NULL, POLLIN);
    }
    pthread_mutex_unlock(&gLocationDevice.mu);
    if (changed) {
        vprocLocationNotifyObservers(readers);
    }
}

ssize_t vprocLocationDeviceWrite(const void *data, size_t len) {
    if (!data || len == 0) {
        return 0;
    }
    bool debug = vprocLocationDebugEnabled();
    pthread_mutex_lock(&gLocationDevice.mu);
    if (!gLocationDevice.enabled) {
        pthread_mutex_unlock(&gLocationDevice.mu);
        if (debug) {
            vprocLocationDebugf("write rejected; location device disabled");
        }
        errno = ENOENT;
        return -1;
    }
    vprocLocationEnsureCond();
    size_t copy_len = len < sizeof(gLocationDevice.last_payload) ? len : sizeof(gLocationDevice.last_payload) - 1;
    memcpy(gLocationDevice.last_payload, data, copy_len);
    gLocationDevice.last_payload[copy_len] = '\0';
    gLocationDevice.last_len = copy_len;
    gLocationDevice.has_payload = (copy_len > 0);
    gLocationDevice.seq++;
    pthread_cond_broadcast(&gLocationDevice.cond);

    if (gLocationDevice.stub_fd >= 0) {
        ssize_t wrote = vprocHostWriteRaw(gLocationDevice.stub_fd, data, len);
        if (debug && wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EPIPE) {
            vprocLocationDebugf("stub write failed: %s", strerror(errno));
        }
    }
    pthread_mutex_unlock(&gLocationDevice.mu);
    pscal_fd_poll_wakeup(NULL, POLLIN);
    if (debug) {
        vprocLocationDebugf("write success bytes=%zu seq=%llu", len, (unsigned long long)gLocationDevice.seq);
    }
    return (ssize_t)len;
}

void vprocLocationDeviceRegisterReaderObserver(VprocLocationReadersChangedFn cb, void *context) {
    pthread_mutex_lock(&gLocationReaderObserverMu);
    gLocationReaderObserver = cb;
    gLocationReaderObserverCtx = context;
    pthread_mutex_unlock(&gLocationReaderObserverMu);
    if (cb) {
        int readers = 0;
        pthread_mutex_lock(&gLocationDevice.mu);
        readers = gLocationDevice.readers;
        pthread_mutex_unlock(&gLocationDevice.mu);
        cb(readers, context);
    }
}

static int vprocSelectHostFd(VProc *inherit_from, int option_fd, int stdno) {
    /* Explicit host fd provided: clone it. */
    if (option_fd >= 0) {
        return vprocCloneFd(option_fd);
    }
    /* Force /dev/null. */
    if (option_fd == -2) {
        int flags = (stdno == STDIN_FILENO) ? O_RDONLY : O_WRONLY;
        return vprocHostOpenRawInternal("/dev/null", flags, 0, false);
    }
    /* Otherwise inherit from the active vproc's mapping, falling back to host stdno. */
    int source = stdno;
    if (inherit_from) {
        int translated = vprocTranslateFd(inherit_from, stdno);
        if (translated >= 0) {
            source = translated;
        }
    }
    return vprocCloneFd(source);
}

int vprocReservePid(void) {
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    int pid = __sync_fetch_and_add(&gNextSyntheticPid, 1);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return -1;
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid == pid) {
        parent_pid = 0;
    }
    VProcTaskEntry *parent_entry = NULL;
    if (parent_pid > 0) {
        parent_entry = vprocTaskEnsureSlotLocked(parent_pid);
        if (!parent_entry || parent_entry->pid != parent_pid) {
            parent_pid = 0;
            parent_entry = NULL;
        }
    }
    vprocClearEntryLocked(entry);
    vprocInitEntryDefaultsLocked(entry, pid, parent_entry);
    entry->parent_pid = parent_pid;
    /* Reserve creates a brand-new process group; do not inherit the shell's
     * pgid/fg_pgid or later kill/pgid lookups will miss the pre-start task. */
    entry->pgid = pid;
    entry->fg_pgid = pid;
    if (parent_entry && parent_pid > 0) {
        if (!vprocAddChildLocked(parent_entry, pid)) {
            entry->parent_pid = 0;
        }
    }
    if (gVProcTasks.items && gVProcTasks.count > 0) {
        ptrdiff_t idx = entry - gVProcTasks.items;
        if (idx >= 0 && (size_t)idx < gVProcTasks.count) {
            vprocTaskLookupRememberLocked(pid, (size_t)idx);
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pid;
}

static void vprocMaybeAdvancePidCounter(int pid_hint) {
    if (pid_hint <= 0) {
        return;
    }
    while (true) {
        int current = gNextSyntheticPid;
        if (pid_hint < current) {
            break;
        }
        if (__sync_bool_compare_and_swap(&gNextSyntheticPid, current, pid_hint + 1)) {
            break;
        }
    }
}

static void vprocTaskTableRepairLocked(void) {
    if (gVProcTasks.items != gVProcTasksItemsStable) {
        if (vprocVprocDebugEnabled()) {
            vprocDebugLogf( "[vproc] task table pointer mismatch; repairing\n");
        }
        gVProcTasks.items = gVProcTasksItemsStable;
        gVProcTasks.capacity = gVProcTasksCapacityStable;
    }
    if (!gVProcTasks.items || gVProcTasks.capacity == 0) {
        gVProcTasks.items = NULL;
        gVProcTasks.count = 0;
        gVProcTasks.capacity = 0;
        gVProcTaskFindHint = 0;
        gVProcTaskFreeHint = 0;
    } else if (gVProcTasks.count > gVProcTasks.capacity) {
        gVProcTasks.count = gVProcTasks.capacity;
    } else if (gVProcTasks.count == 0) {
        gVProcTaskFindHint = 0;
        gVProcTaskFreeHint = 0;
    }
}

static VProcTaskEntry *vprocTaskFindLocked(int pid) {
    if (pid <= 0) {
        return NULL;
    }
    vprocTaskTableRepairLocked();
    if (gVProcTasks.count > 0 && gVProcTaskFindHint < gVProcTasks.count) {
        VProcTaskEntry *hint = &gVProcTasks.items[gVProcTaskFindHint];
        if (hint->pid == pid) {
            return hint;
        }
    }
    VProcTaskLookupCacheEntry *cache = &gVProcTaskLookupCache[vprocTaskLookupSlotForPid(pid)];
    if (cache->pid == pid) {
        size_t idx = (size_t)cache->idx;
        if (idx < gVProcTasks.count && gVProcTasks.items[idx].pid == pid) {
            gVProcTaskFindHint = idx;
            return &gVProcTasks.items[idx];
        }
        cache->pid = 0;
        cache->idx = 0;
    }
    size_t start = (gVProcTaskFindHint < gVProcTasks.count) ? gVProcTaskFindHint : 0;
    for (size_t i = start; i < gVProcTasks.count; ++i) {
        if (gVProcTasks.items[i].pid == pid) {
            gVProcTaskFindHint = i;
            vprocTaskLookupRememberLocked(pid, i);
            return &gVProcTasks.items[i];
        }
    }
    for (size_t i = 0; i < start; ++i) {
        if (gVProcTasks.items[i].pid == pid) {
            gVProcTaskFindHint = i;
            vprocTaskLookupRememberLocked(pid, i);
            return &gVProcTasks.items[i];
        }
    }
    return NULL;
}

static VProcTaskEntry *vprocTaskEnsureSlotLocked(int pid) {
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        return entry;
    }
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid == pid) {
        parent_pid = 0;
    }
    const VProcTaskEntry *parent_entry = vprocTaskFindLocked(parent_pid);
    if (!parent_entry && parent_pid > 0) {
        parent_entry = vprocTaskEnsureSlotLocked(parent_pid);
    }
    if (parent_pid > 0 && (!parent_entry || parent_entry->pid != parent_pid)) {
        parent_pid = 0;
        parent_entry = NULL;
    }
    /* Preallocate generously and avoid realloc to keep table pointer stable. */
    if (gVProcTasks.capacity == 0) {
        gVProcTasks.capacity = 4096;
        gVProcTasks.items = (VProcTaskEntry *)calloc(gVProcTasks.capacity, sizeof(VProcTaskEntry));
        if (!gVProcTasks.items) {
            gVProcTasks.capacity = 0;
            return NULL;
        }
        gVProcTasksItemsStable = gVProcTasks.items;
        gVProcTasksCapacityStable = gVProcTasks.capacity;
    }
    if (gVProcTasks.count >= gVProcTasks.capacity) {
        /* Reuse cleared slots before rejecting new synthetic tasks. */
        size_t start = (gVProcTaskFreeHint < gVProcTasks.count) ? gVProcTaskFreeHint : 0;
        for (size_t idx = start; idx < gVProcTasks.count; ++idx) {
            if (gVProcTasks.items[idx].pid <= 0) {
                entry = &gVProcTasks.items[idx];
                gVProcTaskFreeHint = (idx + 1 < gVProcTasks.count) ? (idx + 1) : 0;
                break;
            }
        }
        for (size_t idx = 0; !entry && idx < start; ++idx) {
            if (gVProcTasks.items[idx].pid <= 0) {
                entry = &gVProcTasks.items[idx];
                gVProcTaskFreeHint = (idx + 1 < gVProcTasks.count) ? (idx + 1) : 0;
                break;
            }
        }
        if (!entry) {
            errno = EMFILE;
            return NULL;
        }
    } else {
        entry = &gVProcTasks.items[gVProcTasks.count++];
    }
    vprocInitEntryDefaultsLocked(entry, pid, parent_entry);
    entry->parent_pid = parent_pid;
    if (entry->parent_pid > 0) {
        VProcTaskEntry *parent = (VProcTaskEntry *)parent_entry;
        if (parent && parent->pid == entry->parent_pid) {
            if (!vprocAddChildLocked(parent, pid)) {
                entry->parent_pid = 0;
            }
        }
    }
    if (gVProcTasks.items && gVProcTasks.count > 0) {
        ptrdiff_t idx = entry - gVProcTasks.items;
        if (idx >= 0 && (size_t)idx < gVProcTasks.count) {
            vprocTaskLookupRememberLocked(pid, (size_t)idx);
        }
    }
    return entry;
}

VProcOptions vprocDefaultOptions(void) {
    VProcOptions opts;
    opts.stdin_fd = -1;
    opts.stdout_fd = -1;
    opts.stderr_fd = -1;
    opts.winsize_cols = 80;
    opts.winsize_rows = 24;
    opts.pid_hint = -1;
    opts.job_id = 0;
    return opts;
}

VProc *vprocCreate(const VProcOptions *opts) {
    VProcOptions local = opts ? *opts : vprocDefaultOptions();
    if (gNextSyntheticPid == 0) {
        gNextSyntheticPid = vprocNextPidSeed();
    }
    bool vproc_dbg = vprocVprocDebugEnabled();
    VProc *active = vprocCurrent();
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    bool inherit_pscal_stdio = false;
    if (session_stdio &&
        session_stdio->stdin_pscal_fd &&
        session_stdio->stdout_pscal_fd &&
        session_stdio->stderr_pscal_fd &&
        local.stdin_fd == -1 &&
        local.stdout_fd == -1 &&
        local.stderr_fd == -1) {
        inherit_pscal_stdio = true;
        if (vprocVprocDebugEnabled()) {
            vprocDebugLogf( "[vproc] inherit pscal stdio from session\n");
        }
    } else if (vproc_dbg && session_stdio) {
        vprocDebugLogf(
                "[vproc] skip pscal stdio inherit stdin=%d stdout=%d stderr=%d opts=(%d,%d,%d)\n",
                session_stdio->stdin_pscal_fd ? 1 : 0,
                session_stdio->stdout_pscal_fd ? 1 : 0,
                session_stdio->stderr_pscal_fd ? 1 : 0,
                local.stdin_fd,
                local.stdout_fd,
                local.stderr_fd);
    }
#endif
    VProc *vp = calloc(1, sizeof(VProc));
    if (!vp) {
        return NULL;
    }
    
    // Initialize mutex before use
    if (pthread_mutex_init(&vp->mu, NULL) != 0) {
        free(vp);
        return NULL;
    }

    vp->capacity = VPROC_INITIAL_CAPACITY;
    vp->entries = calloc(vp->capacity, sizeof(VProcFdEntry));
    if (!vp->entries) {
        pthread_mutex_destroy(&vp->mu);
        free(vp);
        return NULL;
    }
    for (size_t i = 0; i < vp->capacity; ++i) {
        vp->entries[i].host_fd = -1;
        vp->entries[i].pscal_fd = NULL;
        vp->entries[i].kind = VPROC_FD_NONE;
    }
    vp->next_fd = 3;
    vp->winsize.cols = (local.winsize_cols > 0) ? local.winsize_cols : 80;
    vp->winsize.rows = (local.winsize_rows > 0) ? local.winsize_rows : 24;

    if (local.pid_hint > 0) {
        vprocMaybeAdvancePidCounter(local.pid_hint);
        vp->pid = local.pid_hint;
    } else {
        vp->pid = __sync_fetch_and_add(&gNextSyntheticPid, 1);
    }

    /* Ensure a task slot exists for synthetic pid bookkeeping. */
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *slot = vprocTaskEnsureSlotLocked(vp->pid);
    if (!slot) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (errno == 0) {
            errno = EMFILE;
        }
        pthread_mutex_destroy(&vp->mu);
        free(vp->entries);
        free(vp);
        return NULL;
    }
    int parent_pid = vprocDefaultParentPid();
    if (parent_pid == vp->pid) {
        parent_pid = 0;
    }
    VProcTaskEntry *parent_entry = NULL;
    if (parent_pid > 0) {
        parent_entry = vprocTaskEnsureSlotLocked(parent_pid);
        if (!parent_entry || parent_entry->pid != parent_pid) {
            parent_pid = 0;
            parent_entry = NULL;
        }
    }
    /* Reinitialize the slot in place for this pid. */
    vprocClearEntryLocked(slot);
    vprocInitEntryDefaultsLocked(slot, vp->pid, parent_entry);
    slot->parent_pid = parent_pid;
    if (parent_entry && parent_pid > 0) {
        if (!vprocAddChildLocked(parent_entry, vp->pid)) {
            slot->parent_pid = 0;
        }
    }
    if (local.job_id > 0) {
        slot->job_id = local.job_id;
    }
    /* Synthetic vprocs that override stdio (common for pipeline stages) should
     * never be stopped by terminal job-control signals. */
    if (local.stdin_fd >= 0 || local.stdout_fd >= 0 || local.stderr_fd >= 0) {
        slot->stop_unsupported = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);

    int stdin_src = vprocSelectHostFd(active, local.stdin_fd, STDIN_FILENO);
    if (stdin_src < 0 && local.stdin_fd != -2) {
        stdin_src = vprocHostOpenRawInternal("/dev/null", O_RDONLY, 0, false);
        if (vproc_dbg && stdin_src < 0) {
            vprocDebugLogf( "[vproc] stdin clone failed fd=%d err=%s\n",
                    (local.stdin_fd >= 0) ? local.stdin_fd : STDIN_FILENO, strerror(errno));
        }
    }
    bool stdin_from_session = false;
#if defined(PSCAL_TARGET_IOS)
    if (stdin_src >= 0) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && session->stdin_host_fd >= 0) {
            struct stat session_st;
            struct stat stdin_st;
            if (vprocHostFstatRaw(session->stdin_host_fd, &session_st) == 0 &&
                vprocHostFstatRaw(stdin_src, &stdin_st) == 0 &&
                session_st.st_dev == stdin_st.st_dev &&
                session_st.st_ino == stdin_st.st_ino) {
                stdin_from_session = true;
            }
        }
    }
#endif
    int stdout_src = vprocSelectHostFd(active, local.stdout_fd, STDOUT_FILENO);
    if (stdout_src < 0) {
        stdout_src = vprocHostOpenRawInternal("/dev/null", O_WRONLY, 0, false);
        if (vproc_dbg && stdout_src < 0) {
            vprocDebugLogf( "[vproc] stdout clone failed fd=%d err=%s\n",
                    (local.stdout_fd >= 0) ? local.stdout_fd : STDOUT_FILENO, strerror(errno));
        }
    }
    int stderr_src = vprocSelectHostFd(active, local.stderr_fd, STDERR_FILENO);
    if (stderr_src < 0) {
        stderr_src = vprocHostOpenRawInternal("/dev/null", O_WRONLY, 0, false);
        if (vproc_dbg && stderr_src < 0) {
            vprocDebugLogf( "[vproc] stderr clone failed fd=%d err=%s\n",
                    (local.stderr_fd >= 0) ? local.stderr_fd : STDERR_FILENO, strerror(errno));
        }
    }

    if (stdin_src < 0 || stdout_src < 0 || stderr_src < 0) {
        if (stdin_src >= 0) vprocHostClose(stdin_src);
        if (stdout_src >= 0) vprocHostClose(stdout_src);
        if (stderr_src >= 0) vprocHostClose(stderr_src);
        if (vproc_dbg) {
            vprocDebugLogf( "[vproc] create failed stdin=%d stdout=%d stderr=%d\n",
                    stdin_src, stdout_src, stderr_src);
        }
        vprocDestroy(vp);
        return NULL;
    }
    vp->entries[0].host_fd = stdin_src;
    vp->entries[0].pscal_fd = NULL;
    vp->entries[0].kind = VPROC_FD_HOST;
    vp->entries[1].host_fd = stdout_src;
    vp->entries[1].pscal_fd = NULL;
    vp->entries[1].kind = VPROC_FD_HOST;
    vp->entries[2].host_fd = stderr_src;
    vp->entries[2].pscal_fd = NULL;
    vp->entries[2].kind = VPROC_FD_HOST;
    vp->stdin_fd = 0;
    vp->stdout_fd = 1;
    vp->stderr_fd = 2;
    vp->stdin_host_fd = stdin_src;
    vp->stdout_host_fd = stdout_src;
    vp->stderr_host_fd = stderr_src;
    vp->stdin_from_session = stdin_from_session;
#if defined(PSCAL_TARGET_IOS)
    if (inherit_pscal_stdio) {
        if (vprocAdoptPscalStdio(vp,
                                 session_stdio->stdin_pscal_fd,
                                 session_stdio->stdout_pscal_fd,
                                 session_stdio->stderr_pscal_fd) != 0) {
            if (vproc_dbg) {
                vprocDebugLogf( "[vproc] adopt pscal stdio failed: %s\n", strerror(errno));
            }
        }
    }
#endif
    vprocRegistryAdd(vp);
    return vp;
}

int vprocAdoptPscalStdio(VProc *vp,
                         struct pscal_fd *stdin_fd,
                         struct pscal_fd *stdout_fd,
                         struct pscal_fd *stderr_fd) {
    if (!vp || !stdin_fd || !stdout_fd || !stderr_fd) {
        errno = EINVAL;
        return -1;
    }
    (void)vprocClose(vp, STDIN_FILENO);
    (void)vprocClose(vp, STDOUT_FILENO);
    (void)vprocClose(vp, STDERR_FILENO);

    pthread_mutex_lock(&vp->mu);
    if (vp->capacity <= STDERR_FILENO) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    vp->entries[STDIN_FILENO].host_fd = -1;
    vp->entries[STDIN_FILENO].pscal_fd = pscal_fd_retain(stdin_fd);
    vp->entries[STDIN_FILENO].kind = VPROC_FD_PSCAL;

    vp->entries[STDOUT_FILENO].host_fd = -1;
    vp->entries[STDOUT_FILENO].pscal_fd = pscal_fd_retain(stdout_fd);
    vp->entries[STDOUT_FILENO].kind = VPROC_FD_PSCAL;

    vp->entries[STDERR_FILENO].host_fd = -1;
    vp->entries[STDERR_FILENO].pscal_fd = pscal_fd_retain(stderr_fd);
    vp->entries[STDERR_FILENO].kind = VPROC_FD_PSCAL;

    vp->stdin_host_fd = -1;
    vp->stdout_host_fd = -1;
    vp->stderr_host_fd = -1;
    vp->stdin_from_session = false;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocAdoptPscalFd(VProc *vp, int target_fd, struct pscal_fd *pscal_fd) {
    if (!vp || !pscal_fd || target_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    (void)vprocClose(vp, target_fd);

    pthread_mutex_lock(&vp->mu);
    if ((size_t)target_fd >= vp->capacity) {
        pthread_mutex_unlock(&vp->mu);
        if (vprocPipeDebugEnabled()) {
            vprocDebugLogf("[vproc] adopt pscal fd=%d failed: capacity %zu\n", target_fd, vp->capacity);
        }
        errno = EBADF;
        return -1;
    }
    vp->entries[target_fd].host_fd = -1;
    vp->entries[target_fd].pscal_fd = pscal_fd_retain(pscal_fd);
    vp->entries[target_fd].kind = VPROC_FD_PSCAL;
    if (target_fd == STDIN_FILENO) {
        vp->stdin_host_fd = -1;
        vp->stdin_from_session = false;
    } else if (target_fd == STDOUT_FILENO) {
        vp->stdout_host_fd = -1;
    } else if (target_fd == STDERR_FILENO) {
        vp->stderr_host_fd = -1;
    }
    pthread_mutex_unlock(&vp->mu);
    if (vprocVprocDebugEnabled()) {
        fprintf(stderr, "[vproc] adopt pscal fd=%d ptr=%p rc=0\n",
                target_fd, (void *)pscal_fd);
    }
    return 0;
}

void vprocDestroy(VProc *vp) {
    if (!vp) {
        return;
    }
    vprocRegistryRemove(vp);
    // Lock shouldn't strictly be needed here if refcount is 0, but good practice
    pthread_mutex_lock(&vp->mu);
    /* Close only the vproc-owned fds; do not close the saved host stdio fds. */
    for (size_t i = 0; i < vp->capacity; ++i) {
        if (vp->entries[i].kind == VPROC_FD_PSCAL && vp->entries[i].pscal_fd) {
            pscal_fd_close(vp->entries[i].pscal_fd);
        } else if (vp->entries[i].kind == VPROC_FD_HOST &&
                   vp->entries[i].host_fd >= 0 &&
                   vp->entries[i].host_fd != vp->stdin_host_fd &&
                   vp->entries[i].host_fd != vp->stdout_host_fd &&
                   vp->entries[i].host_fd != vp->stderr_host_fd) {
            (void)vprocResourceRemoveLocked(vp, vp->entries[i].host_fd);
            vprocHostCloseRaw(vp->entries[i].host_fd);
        }
        vp->entries[i].host_fd = -1;
        vp->entries[i].pscal_fd = NULL;
        vp->entries[i].kind = VPROC_FD_NONE;
    }
    if (vp->stdin_host_fd >= 0) {
        (void)vprocResourceRemoveLocked(vp, vp->stdin_host_fd);
        vprocHostCloseRaw(vp->stdin_host_fd);
    }
    if (vp->stdout_host_fd >= 0) {
        (void)vprocResourceRemoveLocked(vp, vp->stdout_host_fd);
        vprocHostCloseRaw(vp->stdout_host_fd);
    }
    if (vp->stderr_host_fd >= 0) {
        (void)vprocResourceRemoveLocked(vp, vp->stderr_host_fd);
        vprocHostCloseRaw(vp->stderr_host_fd);
    }
    vprocResourceCloseAllLocked(vp);
    free(vp->resources);
    vp->resources = NULL;
    vp->resource_capacity = 0;
    vp->resource_count = 0;
    free(vp->entries);
    vp->entries = NULL;
    if (gVProcCurrent == vp) {
        gVProcCurrent = NULL;
    }
    for (size_t i = 0; i < gVProcStackDepth; ++i) {
        if (gVProcStack[i] == vp) {
            gVProcStack[i] = NULL;
        }
    }
    pthread_mutex_unlock(&vp->mu);
    
    pthread_mutex_destroy(&vp->mu);
    free(vp);
}

void vprocActivate(VProc *vp) {
    gVProcTlsReady = 1;
    if (gVProcCurrent && !vprocRegistryContainsValidated(gVProcCurrent)) {
        /* vprocRegistryContainsValidated already cleared thread-local state. */
    }
    if (gVProcCurrent &&
        gVProcStackDepth < (sizeof(gVProcStack) / sizeof(gVProcStack[0]))) {
        gVProcStack[gVProcStackDepth++] = gVProcCurrent;
    }
    gVProcCurrent = vp;
    gVProcRegistrySeenVersion = __atomic_load_n(&gVProcRegistryVersion, __ATOMIC_ACQUIRE);
    gVProcInterposeReady = 1;
}

void vprocDeactivate(void) {
    if (gVProcStackDepth > 0) {
        gVProcCurrent = gVProcStack[gVProcStackDepth - 1];
        gVProcStack[gVProcStackDepth - 1] = NULL;
        gVProcStackDepth--;
    } else {
        gVProcCurrent = NULL;
    }
}

VProc *vprocCurrent(void) {
    return vprocForThread();
}

void vprocDiscard(int pid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        vprocClearEntryLocked(entry);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocTerminateSession(int sid) {
    if (sid <= 0) {
        return;
    }
    pthread_t self = pthread_self();
    pthread_t *cancel = NULL;
    size_t cancel_count = 0;
    size_t cancel_cap = 0;
    size_t target_count = 0;
    size_t *target_indices = NULL;
    bool clear_direct = false;

    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) continue;
        if (entry->sid != sid) continue;
        target_count++;
    }
    if (target_count > 0) {
        target_indices = (size_t *)calloc(target_count, sizeof(size_t));
        if (!target_indices) {
            clear_direct = true;
        }
    }
    size_t target_index = 0;
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) continue;
        if (entry->sid != sid) continue;

        vprocMaybeStampRusageLocked(entry);
        entry->exit_signal = SIGKILL;
        entry->status = W_EXITCODE(128 + SIGKILL, 0);
        entry->exited = true;
        entry->zombie = false;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);

        if (entry->tid && !pthread_equal(entry->tid, self)) {
            vprocCancelListAdd(&cancel, &cancel_count, &cancel_cap, entry->tid);
        }
        for (size_t t = 0; t < entry->thread_count; ++t) {
            pthread_t tid = entry->threads[t];
            if (tid && !pthread_equal(tid, self)) {
                vprocCancelListAdd(&cancel, &cancel_count, &cancel_cap, tid);
            }
        }
        if (target_indices && target_index < target_count) {
            target_indices[target_index++] = i;
        }
    }
    if (clear_direct) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0 || entry->sid != sid) continue;
            vprocClearEntryLocked(entry);
        }
    } else {
        for (size_t i = 0; i < target_index; ++i) {
            size_t idx = target_indices[i];
            if (idx >= gVProcTasks.count) continue;
            VProcTaskEntry *entry = &gVProcTasks.items[idx];
            if (entry->pid <= 0 || entry->sid != sid) continue;
            vprocClearEntryLocked(entry);
        }
    }
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    for (size_t i = 0; i < cancel_count; ++i) {
        pthread_cancel(cancel[i]);
    }
    free(cancel);
    free(target_indices);
}

static void *vprocThreadTrampoline(void *arg) {
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)arg;

    PSCALRuntimeContext *prev_runtime_ctx = NULL;
    bool runtime_ctx_swapped = false;
#if defined(PSCAL_TARGET_IOS)
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        prev_runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
    if (PSCALRuntimeSetCurrentRuntimeContext && ctx && ctx->runtime_ctx) {
        PSCALRuntimeSetCurrentRuntimeContext(ctx->runtime_ctx);
        runtime_ctx_swapped = true;
    }
#else
    (void)prev_runtime_ctx;
    (void)runtime_ctx_swapped;
#endif

    if (ctx && ctx->detach) {
        pthread_detach(pthread_self());
    }

    if (ctx) {
        vprocSetShellSelfPid(ctx->shell_self_pid);
        vprocSetKernelPid(ctx->kernel_pid);
        if (ctx->session_stdio) {
            vprocSessionStdioActivate(ctx->session_stdio);
        }
    }

    VProc *vp = ctx ? ctx->vp : NULL;
    if (vp) {
        vprocActivate(vp);
        vprocRegisterThread(vp, pthread_self());
    }

    void *res = NULL;
    if (ctx && ctx->start_routine) {
        res = ctx->start_routine(ctx->arg);
    }

    if (vp) {
        /* Threads share the owning vproc; do not mark the process as exited on
         * thread teardown. */
        vprocUnregisterThread(vp, pthread_self());
        vprocDeactivate();
    }

    if (ctx && ctx->session_stdio) {
        vprocSessionStdioActivate(NULL);
    }

#if defined(PSCAL_TARGET_IOS)
    if (runtime_ctx_swapped && PSCALRuntimeSetCurrentRuntimeContext) {
        PSCALRuntimeSetCurrentRuntimeContext(prev_runtime_ctx);
    }
#endif

    free(ctx);
    return res;
}

int vprocPthreadCreateShim(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg) {
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)calloc(1, sizeof(VProcThreadStartCtx));
    if (!ctx) {
        errno = ENOMEM;
        return ENOMEM;
    }
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->vp = vprocCurrent();
    ctx->session_stdio = vprocSessionStdioCurrent();
    ctx->shell_self_pid = vprocGetShellSelfPid();
    ctx->kernel_pid = vprocGetKernelPid();
    ctx->detach = false;
#if defined(PSCAL_TARGET_IOS)
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        ctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
#endif
    if (attr) {
        int detach_state = 0;
        if (pthread_attr_getdetachstate(attr, &detach_state) == 0 &&
            detach_state == PTHREAD_CREATE_DETACHED) {
            ctx->detach = true;
        }
    }
    int rc = vprocHostPthreadCreateRaw(thread, attr, vprocThreadTrampoline, ctx);
    if (rc != 0) {
        free(ctx);
        errno = rc;
    }
    return rc;
}

int vprocTranslateFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (!vprocRegistryContainsValidated(vp)) {
        errno = EBADF;
        return -1;
    }
    int host = -1;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity && vp->entries[fd].kind == VPROC_FD_HOST) {
        host = vp->entries[fd].host_fd;
    }
    pthread_mutex_unlock(&vp->mu);

    if (host < 0) {
        errno = EBADF;
        return -1;
    }
    return host;
}

int vprocHostDup2(int host_fd, int target_fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostDup2Raw(host_fd, target_fd);
#else
    return dup2(host_fd, target_fd);
#endif
}

int vprocHostDup(int fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostDupRaw(fd);
#else
    return dup(fd);
#endif
}

int vprocHostOpen(const char *path, int flags, ...) {
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
#if defined(PSCAL_TARGET_IOS)
    if (vprocPathIsLocationDevice(path)) {
        return vprocLocationDeviceOpenHost(flags);
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    return vprocHostOpenVirtualized(path, flags, mode);
#else
    return open(path, flags, mode);
#endif
}

int vprocHostPipe(int pipefd[2]) {
#if defined(PSCAL_TARGET_IOS)
    int rc = vprocHostPipeRaw(pipefd);
    VProc *vp = vprocForThread();
    if (rc == 0 && vp) {
        vprocResourceTrack(vp, pipefd[0], VPROC_RESOURCE_PIPE);
        vprocResourceTrack(vp, pipefd[1], VPROC_RESOURCE_PIPE);
    }
    return rc;
#else
    return pipe(pipefd);
#endif
}

int vprocHostSocket(int domain, int type, int protocol) {
#if defined(PSCAL_TARGET_IOS)
    int fd = vprocHostSocketRaw(domain, type, protocol);
    VProc *vp = vprocForThread();
    if (vp && fd >= 0) {
        vprocResourceTrack(vp, fd, VPROC_RESOURCE_SOCKET);
    }
    return fd;
#else
    return socket(domain, type, protocol);
#endif
}

int vprocHostAccept(int fd, struct sockaddr *addr, socklen_t *addrlen) {
#if defined(PSCAL_TARGET_IOS)
    int res = vprocHostAcceptRaw(fd, addr, addrlen);
    VProc *vp = vprocForThread();
    if (vp && res >= 0) {
        vprocResourceTrack(vp, res, VPROC_RESOURCE_SOCKET);
    }
    return res;
#else
    return accept(fd, addr, addrlen);
#endif
}

int vprocHostSocketpair(int domain, int type, int protocol, int sv[2]) {
#if defined(PSCAL_TARGET_IOS)
    int rc = vprocHostSocketpairRaw(domain, type, protocol, sv);
    VProc *vp = vprocForThread();
    if (rc == 0 && vp) {
        vprocResourceTrack(vp, sv[0], VPROC_RESOURCE_PIPE);
        vprocResourceTrack(vp, sv[1], VPROC_RESOURCE_PIPE);
    }
    return rc;
#else
    return socketpair(domain, type, protocol, sv);
#endif
}

off_t vprocHostLseek(int fd, off_t offset, int whence) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostLseekRaw(fd, offset, whence);
#else
    return lseek(fd, offset, whence);
#endif
}

int vprocHostFsync(int fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostFsyncRaw(fd);
#else
    return fsync(fd);
#endif
}

int vprocHostClose(int fd) {
#if defined(PSCAL_TARGET_IOS)
    VProc *vp = vprocForThread();
    if (vp) {
        vprocResourceRemove(vp, fd);
    }
    return vprocHostCloseRaw(fd);
#else
    return close(fd);
#endif
}

ssize_t vprocHostRead(int fd, void *buf, size_t count) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostReadRaw(fd, buf, count);
#else
    return read(fd, buf, count);
#endif
}

ssize_t vprocHostWrite(int fd, const void *buf, size_t count) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostWriteRaw(fd, buf, count);
#else
    return write(fd, buf, count);
#endif
}

int vprocHostPthreadCreate(pthread_t *thread,
                           const pthread_attr_t *attr,
                           void *(*start_routine)(void *),
                           void *arg) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostPthreadCreateRaw(thread, attr, start_routine, arg);
#else
    return pthread_create(thread, attr, start_routine, arg);
#endif
}

int vprocDup(VProc *vp, int fd) {
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        int slot = vprocInsertPscalFd(vp, pscal_fd);
        /* vprocInsertPscalFd now retains; drop our temporary ref. */
        pscal_fd_close(pscal_fd);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        return -1;
    }
    return vprocInsert(vp, cloned);
}

int vprocDup2(VProc *vp, int fd, int target) {
    if (!vp || target < 0) {
        errno = EBADF;
        return -1;
    }

    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        pthread_mutex_lock(&vp->mu);
        if ((size_t)target >= vp->capacity) {
            size_t new_cap = vp->capacity;
            if (new_cap == 0) new_cap = VPROC_INITIAL_CAPACITY;
            while ((size_t)target >= new_cap) {
                new_cap *= 2;
            }
            VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
            if (!resized) {
                pthread_mutex_unlock(&vp->mu);
                pscal_fd_close(pscal_fd);
                return -1;
            }
            for (size_t i = vp->capacity; i < new_cap; ++i) {
                resized[i].host_fd = -1;
                resized[i].pscal_fd = NULL;
                resized[i].kind = VPROC_FD_NONE;
            }
            vp->entries = resized;
            vp->capacity = new_cap;
        }
        if (vp->entries[target].kind == VPROC_FD_PSCAL && vp->entries[target].pscal_fd) {
            pscal_fd_close(vp->entries[target].pscal_fd);
        } else if (vp->entries[target].kind == VPROC_FD_HOST && vp->entries[target].host_fd >= 0) {
            vprocResourceRemoveLocked(vp, vp->entries[target].host_fd);
            vprocHostCloseRaw(vp->entries[target].host_fd);
        }
        vp->entries[target].host_fd = -1;
        vp->entries[target].pscal_fd = pscal_fd;
        vp->entries[target].kind = VPROC_FD_PSCAL;
        pthread_mutex_unlock(&vp->mu);
        return target;
    }

    // We cannot hold lock here during Clone, but we need it for Translate
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    
    pthread_mutex_lock(&vp->mu);
    if ((size_t)target >= vp->capacity) {
        size_t new_cap = vp->capacity;
        if (new_cap == 0) new_cap = VPROC_INITIAL_CAPACITY;
        while ((size_t)target >= new_cap) {
            new_cap *= 2;
        }
        VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
        if (!resized) {
            pthread_mutex_unlock(&vp->mu);
            return -1;
        }
        for (size_t i = vp->capacity; i < new_cap; ++i) {
            resized[i].host_fd = -1;
            resized[i].pscal_fd = NULL;
            resized[i].kind = VPROC_FD_NONE;
        }
        vp->entries = resized;
        vp->capacity = new_cap;
    }
    if (vp->entries[target].kind == VPROC_FD_PSCAL && vp->entries[target].pscal_fd) {
        pscal_fd_close(vp->entries[target].pscal_fd);
        vp->entries[target].pscal_fd = NULL;
        vp->entries[target].kind = VPROC_FD_NONE;
    }
    if (vp->entries[target].kind == VPROC_FD_HOST && vp->entries[target].host_fd >= 0) {
        bool preserve_controlling_stdin =
            (target == STDIN_FILENO) && (vp->entries[target].host_fd == vp->stdin_host_fd);
        if (!preserve_controlling_stdin) {
            vprocResourceRemoveLocked(vp, vp->entries[target].host_fd);
            vprocHostCloseRaw(vp->entries[target].host_fd);
        }
        vp->entries[target].host_fd = -1;
    }
    // Clone calls fcntl on host, safe to do while holding lock as it doesn't re-enter vproc
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        pthread_mutex_unlock(&vp->mu);
        return -1;
    }
    vp->entries[target].host_fd = cloned;
    vp->entries[target].pscal_fd = NULL;
    vp->entries[target].kind = VPROC_FD_HOST;
#if defined(PSCAL_TARGET_IOS)
    if (target == STDIN_FILENO) {
        vp->stdin_host_fd = cloned;
        vp->stdin_from_session = false;
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && session->stdin_host_fd >= 0) {
            struct stat session_st;
            struct stat cloned_st;
            if (vprocHostFstatRaw(session->stdin_host_fd, &session_st) == 0 &&
                vprocHostFstatRaw(cloned, &cloned_st) == 0 &&
                session_st.st_dev == cloned_st.st_dev &&
                session_st.st_ino == cloned_st.st_ino) {
                vp->stdin_from_session = true;
            }
        }
    }
#endif
    pthread_mutex_unlock(&vp->mu);
    return target;
}

/* Re-sync the vproc fd table to match a host fd already duplicated onto
 * target_fd at the OS level. This is used when shellRestoreExecRedirections
 * has performed a host dup2 and we need the vproc view to follow suit. */
int vprocRestoreHostFd(VProc *vp, int target_fd, int host_src) {
    if (!vp || target_fd < 0 || host_src < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)target_fd >= vp->capacity) {
        size_t new_cap = vp->capacity ? vp->capacity : VPROC_INITIAL_CAPACITY;
        while ((size_t)target_fd >= new_cap) {
            new_cap *= 2;
        }
        VProcFdEntry *resized = realloc(vp->entries, new_cap * sizeof(VProcFdEntry));
        if (!resized) {
            pthread_mutex_unlock(&vp->mu);
            return -1;
        }
        for (size_t i = vp->capacity; i < new_cap; ++i) {
            resized[i].host_fd = -1;
            resized[i].pscal_fd = NULL;
            resized[i].kind = VPROC_FD_NONE;
        }
        vp->entries = resized;
        vp->capacity = new_cap;
    }
    if (vp->entries[target_fd].kind == VPROC_FD_PSCAL && vp->entries[target_fd].pscal_fd) {
        pscal_fd_close(vp->entries[target_fd].pscal_fd);
        vp->entries[target_fd].pscal_fd = NULL;
        vp->entries[target_fd].kind = VPROC_FD_NONE;
    }
    if (vp->entries[target_fd].kind == VPROC_FD_HOST && vp->entries[target_fd].host_fd >= 0 &&
        !(target_fd == STDIN_FILENO && vp->entries[target_fd].host_fd == vp->stdin_host_fd)) {
        vprocResourceRemoveLocked(vp, vp->entries[target_fd].host_fd);
        vprocHostCloseRaw(vp->entries[target_fd].host_fd);
    }
    int cloned = vprocCloneFd(host_src);
    if (cloned < 0) {
        pthread_mutex_unlock(&vp->mu);
        return -1;
    }
    vp->entries[target_fd].host_fd = cloned;
    vp->entries[target_fd].pscal_fd = NULL;
    vp->entries[target_fd].kind = VPROC_FD_HOST;
#if defined(PSCAL_TARGET_IOS)
    if (target_fd == STDIN_FILENO) {
        vp->stdin_host_fd = cloned;
        vp->stdin_from_session = false;
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && session->stdin_host_fd >= 0) {
            struct stat session_st;
            struct stat cloned_st;
            if (vprocHostFstatRaw(session->stdin_host_fd, &session_st) == 0 &&
                vprocHostFstatRaw(cloned, &cloned_st) == 0 &&
                session_st.st_dev == cloned_st.st_dev &&
                session_st.st_ino == cloned_st.st_ino) {
                vp->stdin_from_session = true;
            }
        }
    }
#endif
    pthread_mutex_unlock(&vp->mu);
    return target_fd;
}

int vprocClose(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        errno = EBADF;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd >= vp->capacity) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    int host = vp->entries[fd].host_fd;
    struct pscal_fd *pscal_fd = vp->entries[fd].pscal_fd;
    VProcFdKind kind = vp->entries[fd].kind;
    if (kind == VPROC_FD_NONE) {
        pthread_mutex_unlock(&vp->mu);
        errno = EBADF;
        return -1;
    }
    vp->entries[fd].host_fd = -1;
    vp->entries[fd].pscal_fd = NULL;
    vp->entries[fd].kind = VPROC_FD_NONE;
    pthread_mutex_unlock(&vp->mu);
    if (kind == VPROC_FD_PSCAL && pscal_fd) {
        int rc = pscal_fd_close(pscal_fd);
        if (rc < 0) {
            return vprocSetCompatErrno(rc);
        }
        return 0;
    }
    if (host >= 0) {
        vprocResourceRemove(vp, host);
    }
    return vprocHostClose(host);
}

int vprocPipe(VProc *vp, int pipefd[2]) {
    if (!vp || !pipefd) {
        errno = EINVAL;
        return -1;
    }
    int raw[2];
    if (vprocHostPipeRaw(raw) != 0) {
        return -1;
    }
    // vprocInsert locks internally, so this is safe
    int left = vprocInsert(vp, raw[0]);
    int right = vprocInsert(vp, raw[1]);
    if (left < 0 || right < 0) {
        if (left >= 0) vprocClose(vp, left);
        else vprocHostClose(raw[0]);
        if (right >= 0) vprocClose(vp, right);
        else vprocHostClose(raw[1]);
        return -1;
    }
    pipefd[0] = left;
    pipefd[1] = right;
    return 0;
}

int vprocOpenAt(VProc *vp, const char *path, int flags, int mode) {
    if (!vp || !path) {
        errno = EINVAL;
        return -1;
    }
    if (vprocPathIsLegacyGpsDevice(path)) {
        errno = ENOENT;
        return -1;
    }
    if (vprocPathIsLocationDevice(path)) {
        return vprocLocationDeviceOpen(vp, flags);
    }
    bool dbg = vprocPipeDebugEnabled();
    int host_fd = vprocHostOpenVirtualized(path, flags, mode);
#if defined(PSCAL_TARGET_IOS)
    if (host_fd < 0 && errno == ENOENT) {
        if (dbg) vprocDebugLogf( "[vproc-open] virtualized ENOENT for %s, fallback raw\n", path);
        /* Fallback to raw open so pipelines can read plain files even when
         * virtualization rejects the path. This mirrors shell expectations
         * for cat/head pipelines on iOS. */
        host_fd = vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
    }
    if (dbg && host_fd >= 0) {
        vprocDebugLogf( "[vproc-open] opened %s -> fd=%d flags=0x%x\n", path, host_fd, flags);
    }
#endif
    if (host_fd < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        vprocHostClose(host_fd);
    }
    return slot;
}

int vprocSetWinsize(VProc *vp, int cols, int rows) {
    if (!vp) {
        errno = EINVAL;
        return -1;
    }
    // Winsize changes are atomic enough to not strictly need mutex for this simple struct,
    // but better safe if we expanded struct later.
    pthread_mutex_lock(&vp->mu);
    if (cols > 0) vp->winsize.cols = cols;
    if (rows > 0) vp->winsize.rows = rows;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocGetWinsize(VProc *vp, VProcWinsize *out) {
    if (!vp || !out) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&vp->mu);
    *out = vp->winsize;
    pthread_mutex_unlock(&vp->mu);
    return 0;
}

int vprocPid(VProc *vp) {
    return vp ? vp->pid : -1;
}

static bool vprocTaskEntryHasThreadLocked(const VProcTaskEntry *entry, pthread_t tid) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->tid && pthread_equal(entry->tid, tid)) {
        return true;
    }
    for (size_t i = 0; i < entry->thread_count; ++i) {
        if (entry->threads[i] && pthread_equal(entry->threads[i], tid)) {
            return true;
        }
    }
    return false;
}

int vprocRegisterTidHint(int pid, pthread_t tid) {
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    bool vdbg = vprocVprocDebugEnabled();
    char thread_name[16];
    bool rename_thread = false;
    size_t thread_count = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        if (vdbg) {
            vprocDebugLogf( "[vproc] register tid hint failed pid=%d tid=%p\n", pid, (void *)tid);
        }
        return -1;
    }
    bool duplicate = vprocTaskEntryHasThreadLocked(entry, tid);
    entry->tid = tid;
    if (!duplicate) {
        if (entry->thread_count >= entry->thread_capacity) {
            size_t new_cap = entry->thread_capacity ? entry->thread_capacity * 2 : 4;
            pthread_t *resized = realloc(entry->threads, new_cap * sizeof(pthread_t));
            if (!resized) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = ENOMEM;
                return -1;
            }
            entry->threads = resized;
            entry->thread_capacity = new_cap;
        }
        entry->threads[entry->thread_count++] = tid;
    }
    rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
    thread_count = entry->thread_count;
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
    if (vdbg) {
        vprocDebugLogf( "[vproc] register tid hint pid=%d tid=%p thread_count=%zu\n",
                pid, (void *)tid, thread_count);
    }
    return pid;
}

int vprocRegisterThread(VProc *vp, pthread_t tid) {
    if (!vp || vp->pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    bool vdbg = vprocVprocDebugEnabled();
    char thread_name[16];
    bool rename_thread = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(vp->pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ENOMEM;
        if (vdbg) {
            vprocDebugLogf( "[vproc] register thread failed pid=%d tid=%p\n", vp->pid, (void *)tid);
        }
        return -1;
    }
    bool duplicate = vprocTaskEntryHasThreadLocked(entry, tid);
    entry->tid = tid;
    if (!duplicate) {
        if (entry->thread_count >= entry->thread_capacity) {
            size_t new_cap = entry->thread_capacity ? entry->thread_capacity * 2 : 4;
            pthread_t *resized = realloc(entry->threads, new_cap * sizeof(pthread_t));
            if (!resized) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = ENOMEM;
                return -1;
            }
            entry->threads = resized;
            entry->thread_capacity = new_cap;
        }
        entry->threads[entry->thread_count++] = tid;
    }
    rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
    if (vdbg) {
        vprocDebugLogf( "[vproc] register thread pid=%d tid=%p thread_count=%zu\n",
                vp->pid, (void *)tid, entry->thread_count);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
    return vp->pid;
}

void vprocUnregisterThread(VProc *vp, pthread_t tid) {
    if (!vp || vp->pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vp->pid);
    if (entry) {
        if (entry->tid && pthread_equal(entry->tid, tid)) {
            entry->tid = 0;
        }
        for (size_t i = 0; i < entry->thread_count; ++i) {
            if (entry->threads[i] && pthread_equal(entry->threads[i], tid)) {
                entry->threads[i] = entry->threads[entry->thread_count - 1];
                entry->thread_count--;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocSpawnThread(VProc *vp, void *(*start_routine)(void *), void *arg, pthread_t *thread_out) {
    if (!vp || !start_routine) {
        errno = EINVAL;
        return EINVAL;
    }
    VProcThreadStartCtx *ctx = (VProcThreadStartCtx *)calloc(1, sizeof(VProcThreadStartCtx));
    if (!ctx) {
        errno = ENOMEM;
        return ENOMEM;
    }
    ctx->start_routine = start_routine;
    ctx->arg = arg;
    ctx->vp = vp;
    ctx->session_stdio = vprocSessionStdioCurrent();
    ctx->shell_self_pid = vprocGetShellSelfPid();
    ctx->kernel_pid = vprocGetKernelPid();
    ctx->detach = false;
#if defined(PSCAL_TARGET_IOS)
    if (PSCALRuntimeGetCurrentRuntimeContext) {
        ctx->runtime_ctx = PSCALRuntimeGetCurrentRuntimeContext();
    }
#endif
    pthread_t thread;
    int rc = vprocHostPthreadCreateRaw(&thread, NULL, vprocThreadTrampoline, ctx);
    if (rc != 0) {
        free(ctx);
        errno = rc;
        return rc;
    }
    if (thread_out) {
        *thread_out = thread;
    }
    return 0;
}

void vprocMarkExit(VProc *vp, int status) {
    if (!vp || vp->pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vp->pid);
    if (entry) {
        if (entry->exit_signal == 0) {
            entry->status = status;
        }
        vprocMaybeStampRusageLocked(entry);
        entry->exited = true;
        entry->stopped = false;
        entry->continued = false;
        entry->stop_signo = 0;
        entry->zombie = true;
        int adopt_parent = vprocAdoptiveParentPidLocked(entry);
        vprocReparentChildrenLocked(vp->pid, adopt_parent);

        bool discard_zombie = false;
        VProcTaskEntry *parent_entry = vprocTaskFindLocked(entry->parent_pid);
        if (parent_entry) {
            struct sigaction sa = vprocGetSigactionLocked(parent_entry, SIGCHLD);
            if (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT)) {
                discard_zombie = true;
            }
        }
        int kernel_pid = vprocGetKernelPid();
        if (!discard_zombie && kernel_pid > 0 && entry->parent_pid == kernel_pid &&
            entry->sid > 0 && entry->sid != entry->pid) {
            VProcTaskEntry *session_entry = vprocTaskFindLocked(entry->sid);
            if (session_entry) {
                struct sigaction sa = vprocGetSigactionLocked(session_entry, SIGCHLD);
                if (sa.sa_handler == SIG_IGN || (sa.sa_flags & SA_NOCLDWAIT)) {
                    discard_zombie = true;
                }
            }
        }
        if (discard_zombie) {
            entry->zombie = false;
            vprocClearEntryLocked(entry);
        } else {
            vprocNotifyParentSigchldLocked(entry, VPROC_SIGCHLD_EVENT_EXIT);
        }
        vprocMaybeNotifyPgidEmptyLocked(entry->pgid);
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocMarkGroupExit(int pid, int status) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        int pgid = entry->pgid;
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (!peer || peer->pid <= 0) continue;
            if (peer->pgid != pgid) continue;
            vprocMaybeStampRusageLocked(peer);
            peer->group_exit = true;
            peer->group_exit_code = status;
            peer->exited = true;
            peer->zombie = true;
            vprocNotifyParentSigchldLocked(peer, VPROC_SIGCHLD_EVENT_EXIT);
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetParent(int pid, int parent_pid) {
    if (pid <= 0) {
        return;
    }
    bool dbg = vprocVprocDebugEnabled();
    pthread_mutex_lock(&gVProcTasks.mu);
    /* Keep explicit parent assignments intact. The parent task can be
     * registered slightly later than the child; forcing kernel_pid here
     * collapses lineage and makes children appear attached to the kernel. */
    if (parent_pid <= 0) {
        int kernel_pid = vprocGetKernelPid();
        if (kernel_pid > 0 && pid != kernel_pid) {
            parent_pid = kernel_pid;
        }
    }
    if (dbg) {
        VProcTaskEntry *entry = vprocTaskFindLocked(pid);
        if (entry) {
            vprocDebugLogf( "[vproc-parent] pid=%d old=%d new=%d\n",
                    pid, entry->parent_pid, parent_pid);
        } else {
            vprocDebugLogf( "[vproc-parent] pid=%d not found; new=%d\n", pid, parent_pid);
        }
    }
    vprocUpdateParentLocked(pid, parent_pid);
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocSetPgid(int pid, int pgid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    if (pgid <= 0) {
        pgid = pid;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        if (entry->pgid == pgid) {
            rc = 0;
            goto out;
        }
        /* Session leaders cannot change process groups. */
        if (entry->session_leader && entry->pid == entry->sid && entry->pgid != pgid) {
            errno = EPERM;
            rc = -1;
            goto out;
        }
        /* Ensure pgid belongs to same session if it already exists. */
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (!peer || peer->pid <= 0 || peer->pgid != pgid) continue;
            if (peer->sid != entry->sid) {
                errno = EPERM;
                rc = -1;
                goto out;
            }
        }
        entry->pgid = pgid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
out:
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocSetSid(int pid, int sid) {
    if (pid <= 0 || sid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sid = sid;
        entry->pgid = sid;
        entry->session_leader = (pid == sid);
        entry->fg_pgid = sid;
        entry->blocked_signals = 0;
        entry->pending_signals = 0;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocSetStopUnsupported(int pid, bool stop_unsupported) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->stop_unsupported = stop_unsupported;
        if (stop_unsupported && entry->stopped) {
            entry->stopped = false;
            entry->continued = true;
            entry->stop_signo = 0;
        }
        pthread_cond_broadcast(&gVProcTasks.cv);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetPipelineStage(bool active) {
    gVprocPipelineStage = active;
}

int vprocGetPgid(int pid) {
    if (pid == 0) {
        pid = vprocGetPidShim();
    }
    int pgid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        pgid = entry->pgid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pgid;
}

int vprocGetSid(int pid) {
    int sid = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        sid = entry->sid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return sid;
}

static void vprocSyncForegroundPgidToSessionTty(int sid, int fg_pgid) {
#if defined(PSCAL_TARGET_IOS) || defined(VPROC_ENABLE_STUBS_FOR_TESTS)
    /* Keep the controlling TTY's foreground group aligned with the vproc
     * session leader so job-control signals flow to the right pgid. */
    if (sid <= 0 || fg_pgid <= 0) {
        return;
    }
    struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(STDIN_FILENO);
    if (pscal_fd) {
        if (pscal_fd->ops && pscal_fd->ops->ioctl) {
            dword_t fg = (dword_t)fg_pgid;
            (void)pscal_fd->ops->ioctl(pscal_fd, TIOCSPGRP_, &fg);
        }
        pscal_fd_close(pscal_fd);
        return;
    }
    (void)vprocHostTcsetpgrpRaw(STDIN_FILENO, (pid_t)fg_pgid);
#else
    (void)sid;
    (void)fg_pgid;
#endif
}

static int vprocSetForegroundPgidInternal(int sid, int fg_pgid, bool sync_tty) {
    if (sid <= 0 || fg_pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    if (!leader) {
        leader = vprocTaskEnsureSlotLocked(sid);
        if (leader) {
            leader->sid = sid;
            leader->pid = sid;
            leader->session_leader = true;
        }
    }
    if (leader) {
        leader->fg_pgid = fg_pgid;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rc == 0 && sync_tty) {
        vprocSyncForegroundPgidToSessionTty(sid, fg_pgid);
    }
    return rc;
}

int vprocSetForegroundPgid(int sid, int fg_pgid) {
    return vprocSetForegroundPgidInternal(sid, fg_pgid, true);
}

int vprocGetForegroundPgid(int sid) {
    if (sid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int fg = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    if (leader) {
        fg = leader->fg_pgid;
    }
    if (fg < 0) errno = ESRCH;
    pthread_mutex_unlock(&gVProcTasks.mu);
    return fg;
}

static void vprocClearEntryLocked(VProcTaskEntry *entry) {
    if (!entry) {
        return;
    }
    int pid = entry->pid;
    int parent_pid = entry->parent_pid;
    int sid = entry->sid;
    if (parent_pid > 0 && pid > 0) {
        VProcTaskEntry *parent = vprocTaskFindLocked(parent_pid);
        if (parent) {
            vprocRemoveChildLocked(parent, pid);
        }
    }
    int adopt_parent = vprocAdoptiveParentPidLocked(entry);
    vprocReparentChildrenLocked(pid, adopt_parent);

    if (sid > 0) {
        bool drop_session = true;
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *peer = &gVProcTasks.items[i];
            if (!peer || peer->pid <= 0) {
                continue;
            }
            if (peer->pid == pid) {
                continue;
            }
            if (peer->sid == sid) {
                drop_session = false;
                break;
            }
        }
        if (drop_session) {
            pscalTtyDropSession((pid_t_)sid);
        }
    }

    if (entry->label) {
        free(entry->label);
        entry->label = NULL;
    }
    vprocTaskLookupForgetLocked(pid);
    memset(entry->comm, 0, sizeof(entry->comm));
    entry->pid = 0;
    entry->tid = 0;
    entry->parent_pid = 0;
    entry->pgid = 0;
    entry->sid = 0;
    entry->status = 0;
    entry->exit_signal = 0;
    entry->exited = false;
    entry->stopped = false;
    entry->continued = false;
    entry->stop_signo = 0;
    entry->zombie = false;
    entry->stop_unsupported = false;
    entry->job_id = 0;
    if (entry->children) {
        free(entry->children);
    }
    if (entry->threads) {
        free(entry->threads);
    }
    entry->threads = NULL;
    entry->thread_capacity = 0;
    entry->thread_count = 0;
    entry->session_leader = false;
    entry->fg_pgid = 0;
    entry->sigchld_events = 0;
    entry->sigchld_blocked = false;
    entry->children = NULL;
    entry->rusage_utime = 0;
    entry->rusage_stime = 0;
    entry->group_exit = false;
    entry->group_exit_code = 0;
    entry->blocked_signals = 0;
    entry->pending_signals = 0;
    entry->ignored_signals = 0;
    for (int i = 0; i < 32; ++i) {
        entry->pending_counts[i] = 0;
    }
    entry->fg_override_pgid = 0;
    for (int i = 0; i < 32; ++i) {
        sigemptyset(&entry->actions[i].sa_mask);
        entry->actions[i].sa_handler = SIG_DFL;
        entry->actions[i].sa_flags = 0;
    }
    entry->child_capacity = 0;
    entry->child_count = 0;
    entry->start_mono_ns = 0;
    if (gVProcTasks.items && gVProcTasks.count > 0) {
        ptrdiff_t idx = entry - gVProcTasks.items;
        if (idx >= 0 && (size_t)idx < gVProcTasks.count) {
            gVProcTaskFreeHint = (size_t)idx;
        }
    }

}

size_t vprocSnapshot(VProcSnapshot *out, size_t capacity) {
    size_t count = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) {
            continue;
        }
        uint64_t now = vprocNowMonoNs();
        if (out && count < capacity) {
            int fg_for_session = (entry->sid > 0) ? vprocForegroundPgidLocked(entry->sid) : -1;
            int utime = entry->rusage_utime;
            int stime = entry->rusage_stime;
            int cpu_utime = 0;
            int cpu_stime = 0;
            if (vprocComputeCpuTimesLocked(entry, &cpu_utime, &cpu_stime)) {
                if (cpu_utime > utime) {
                    utime = cpu_utime;
                }
                if (cpu_stime > stime) {
                    stime = cpu_stime;
                }
            } else if (!entry->exited && utime == 0 && stime == 0) {
                int live = vprocRuntimeCenti(entry, now);
                if (live > utime) {
                    utime = live;
                }
                if (live / 10 > stime) {
                    stime = live / 10;
                }
            }
            out[count].pid = entry->pid;
            out[count].tid = entry->tid;
            out[count].parent_pid = entry->parent_pid;
            out[count].pgid = entry->pgid;
            out[count].sid = entry->sid;
            out[count].exited = entry->exited;
            out[count].stopped = entry->stopped;
            out[count].continued = entry->continued;
            out[count].zombie = entry->zombie;
            out[count].exit_signal = entry->exit_signal;
            out[count].status = entry->status;
            out[count].stop_signo = entry->stop_signo;
            out[count].sigchld_pending = entry->sigchld_events > 0;
            out[count].rusage_utime = utime;
            out[count].rusage_stime = stime;
            out[count].fg_pgid = (fg_for_session > 0) ? fg_for_session : entry->fg_pgid;
            out[count].job_id = entry->job_id;
            strncpy(out[count].comm, entry->comm, sizeof(out[count].comm) - 1);
            out[count].comm[sizeof(out[count].comm) - 1] = '\0';
            if (entry->label) {
                strncpy(out[count].command, entry->label, sizeof(out[count].command) - 1);
                out[count].command[sizeof(out[count].command) - 1] = '\0';
            } else if (entry->comm[0]) {
                strncpy(out[count].command, entry->comm, sizeof(out[count].command) - 1);
                out[count].command[sizeof(out[count].command) - 1] = '\0';
            } else {
                out[count].command[0] = '\0';
            }
        }
        ++count;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return count;
}

static inline bool vprocWaitParentMatchesLocked(const VProcTaskEntry *entry,
                                                int waiter_pid,
                                                int kernel_pid) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->parent_pid == waiter_pid) {
        return true;
    }
    return kernel_pid > 0 &&
           entry->parent_pid == kernel_pid &&
           entry->sid == waiter_pid;
}

static inline bool vprocWaitPidMatchesLocked(const VProcTaskEntry *entry,
                                             pid_t pid,
                                             int waiter_pgid) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (pid > 0) {
        return entry->pid == pid;
    }
    if (pid == -1) {
        return true;
    }
    if (pid == 0) {
        return (waiter_pgid > 0) ? (entry->pgid == waiter_pgid) : true;
    }
    return entry->pgid == -pid;
}

static inline bool vprocWaitStateChangeMatchesLocked(const VProcTaskEntry *entry,
                                                     bool allow_stop,
                                                     bool allow_cont) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->exited) {
        return true;
    }
    if (allow_stop && entry->stopped && entry->stop_signo > 0) {
        return true;
    }
    if (allow_cont && entry->continued) {
        return true;
    }
    return false;
}

static bool vprocHasWaitCandidateLocked(pid_t pid,
                                        int waiter_pid,
                                        int waiter_pgid,
                                        int kernel_pid) {
    if (pid > 0) {
        VProcTaskEntry *entry = vprocTaskFindLocked((int)pid);
        if (!entry) {
            return false;
        }
        return vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid);
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (!vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid)) {
            continue;
        }
        if (vprocWaitPidMatchesLocked(entry, pid, waiter_pgid)) {
            return true;
        }
    }
    return false;
}

static bool vprocHasKillTargetLocked(pid_t pid) {
    if (pid == 0) {
        return false;
    }
    bool broadcast_all = (pid == -1);
    bool target_group = (pid <= 0);
    int target = target_group ? -pid : pid;
    if (!broadcast_all && !target_group) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target);
        return entry != NULL;
    }
    if (target_group) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target);
        if (entry && entry->pgid == target) {
            return true;
        }
    }
    for (size_t i = 0; i < gVProcTasks.count; ++i) {
        VProcTaskEntry *entry = &gVProcTasks.items[i];
        if (entry->pid <= 0) continue;
        if (broadcast_all) {
            return true;
        }
        if (target_group) {
            if (entry->pgid == target) {
                return true;
            }
        } else if (entry->pid == target) {
            return true;
        }
    }
    return false;
}

pid_t vprocWaitPidShim(pid_t pid, int *status_out, int options) {
    bool allow_stop = (options & WUNTRACED) != 0;
    bool allow_cont = (options & WCONTINUED) != 0;
    bool nohang = (options & WNOHANG) != 0;
    bool nowait = (options & WNOWAIT) != 0;
    bool dbg = vprocKillDebugEnabled();
    int waiter_pid = vprocWaiterPid();
    int waiter_pgid = -1;
    int kernel_pid = vprocGetKernelPid();
    if (pid == 0) {
        waiter_pgid = vprocGetPgid(waiter_pid);
    }

    if (!vprocShimHasVirtualContext()) {
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
        pthread_mutex_lock(&gVProcTasks.mu);
        bool has_candidate = vprocHasWaitCandidateLocked(pid, waiter_pid, waiter_pgid, kernel_pid);
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (!has_candidate) {
            return vprocHostWaitpidRaw(pid, status_out, options);
        }
#else
        return vprocHostWaitpidRaw(pid, status_out, options);
#endif
    }

    pthread_mutex_lock(&gVProcTasks.mu);
    while (true) {
        VProcTaskEntry *ready = NULL;
        bool has_candidate = false;

        if (pid > 0) {
            VProcTaskEntry *entry = vprocTaskFindLocked((int)pid);
            if (entry && vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid)) {
                has_candidate = true;
                if (vprocWaitStateChangeMatchesLocked(entry, allow_stop, allow_cont)) {
                    ready = entry;
                }
            }
        } else {
            for (size_t i = 0; i < gVProcTasks.count; ++i) {
                VProcTaskEntry *entry = &gVProcTasks.items[i];
                if (!vprocWaitParentMatchesLocked(entry, waiter_pid, kernel_pid)) continue;
                if (!vprocWaitPidMatchesLocked(entry, pid, waiter_pgid)) continue;
                has_candidate = true;

                if (vprocWaitStateChangeMatchesLocked(entry, allow_stop, allow_cont)) {
                    ready = entry;
                    break;
                }
            }
        }

        if (ready) {
            int status = 0;
            int waited_pid = ready->pid;
            VProcTaskEntry *waiter_entry = NULL;
            if (waiter_pid > 0) {
                waiter_entry = vprocTaskFindLocked(waiter_pid);
            }

            if (ready->exited) {
                if (ready->group_exit) {
                    status = W_EXITCODE(ready->group_exit_code & 0xff, 0);
                } else if (ready->exit_signal > 0) {
                    status = (ready->exit_signal & 0x7f);
                } else {
                    status = W_EXITCODE(ready->status & 0xff, 0);
                }
            } else if (ready->stopped && ready->stop_signo > 0) {
                status = W_STOPCODE(ready->stop_signo & 0xff);
            } else if (ready->continued) {
                status = W_STOPCODE(SIGCONT);
            }

            if (status_out) {
                *status_out = status;
            }

            if (ready->exited && !nowait) {
                vprocClearEntryLocked(ready);
            } else if (ready->exited) {
                ready->zombie = true;
            } else if (ready->stopped) {
                ready->stop_signo = 0; // Clear stop signal after reporting
            } else if (ready->continued) {
                ready->continued = false;
            }
            if (waiter_entry && waiter_entry->sigchld_events > 0 && !waiter_entry->sigchld_blocked) {
                waiter_entry->sigchld_events--;
            }

            if (dbg) {
                vprocDebugLogf( "[vproc-wait] pid=%d status=%d exited=%d stop=%d\n",
                        waited_pid, status, ready->exited, ready->stopped);
            }
            pthread_mutex_unlock(&gVProcTasks.mu);
            return waited_pid;
        }

        if (nohang) {
            if (status_out) *status_out = 0;
            pthread_mutex_unlock(&gVProcTasks.mu);
            return 0;
        }

        if (!has_candidate) {
            pthread_mutex_unlock(&gVProcTasks.mu);
            errno = ECHILD;
            return -1;
        }

        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
}

static void vprocCancelListAdd(pthread_t **list, size_t *count, size_t *capacity, pthread_t tid) {
    if (!list || !count || !capacity) {
        return;
    }
    if (tid == 0) {
        return;
    }
    for (size_t i = 0; i < *count; ++i) {
        if (pthread_equal((*list)[i], tid)) {
            return;
        }
    }
    if (*count >= *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        pthread_t *resized = (pthread_t *)realloc(*list, new_cap * sizeof(pthread_t));
        if (!resized) {
            return;
        }
        *list = resized;
        *capacity = new_cap;
    }
    (*list)[(*count)++] = tid;
}

static bool vprocKillDeliverEntryLocked(VProcTaskEntry *entry,
                                        pid_t requested_pid,
                                        int sig,
                                        bool dbg,
                                        pthread_t self,
                                        pthread_t **cancel_list,
                                        size_t *cancel_count,
                                        size_t *cancel_capacity) {
    if (!entry || entry->pid <= 0) {
        return false;
    }
    if (entry->zombie || entry->exited) {
        return false;
    }
    if (dbg) {
        vprocDebugLogf( "[vproc-kill] pid=%d sig=%d entry_pid=%d tid=%p\n",
                (int)requested_pid, sig, entry->pid, (void *)entry->tid);
    }

    bool shell_thread = gShellSelfTidValid && pthread_equal(entry->tid, gShellSelfTid);

    if (shell_thread && (sig == SIGINT || sig == SIGTSTP)) {
#if defined(PSCAL_TARGET_IOS)
        if (sig == SIGINT && pscalRuntimeRequestSigint) {
            pscalRuntimeRequestSigint();
        }
#endif
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    if (vprocSignalBlockedLocked(entry, sig)) {
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    VProcSignalAction action = vprocEffectiveSignalActionLocked(entry, sig);
    if (action == VPROC_SIG_HANDLER && !vprocEntryIsCurrentThreadLocked(entry)) {
        vprocQueuePendingSignalLocked(entry, sig);
        return true;
    }

    vprocApplySignalLocked(entry, sig);

    if (entry->exited) {
        if (entry->tid && !pthread_equal(entry->tid, self)) {
            vprocCancelListAdd(cancel_list, cancel_count, cancel_capacity, entry->tid);
        }
        for (size_t t = 0; t < entry->thread_count; ++t) {
            pthread_t tid = entry->threads[t];
            if (tid && !pthread_equal(tid, self)) {
                vprocCancelListAdd(cancel_list, cancel_count, cancel_capacity, tid);
            }
        }
    }
    return true;
}

int vprocKillShim(pid_t pid, int sig) {
    if (!vprocShimHasVirtualContext()) {
#if defined(VPROC_ENABLE_STUBS_FOR_TESTS)
        pthread_mutex_lock(&gVProcTasks.mu);
        bool has_target = vprocHasKillTargetLocked(pid);
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (!has_target) {
            return vprocHostKillRaw(pid, sig);
        }
#else
        return vprocHostKillRaw(pid, sig);
#endif
    }
    bool target_group = (pid <= 0);
    bool broadcast_all = (pid == -1);
    int target = target_group ? -pid : pid;
    bool dbg = vprocKillDebugEnabled();
    if (sig < 0 || sig >= 32) {
        if (dbg) {
            vprocDebugLogf( "[vproc-kill] invalid signal=%d\n", sig);
        }
        errno = EINVAL;
        return -1;
    }

    if (pid == 0) {
        int caller = vprocGetPidShim();
        if (caller <= 0) {
            caller = vprocGetShellSelfPid();
        }
        int caller_pgid = (caller > 0) ? vprocGetPgid(caller) : -1;
        if (caller_pgid <= 0) {
            return vprocHostKillRaw(pid, sig);
        }
        target_group = true;
        target = caller_pgid;
    }

    if (sig == 0) {
        /* Probe for existence: succeed if we find a matching entry. */
        pid_t probe_pid = broadcast_all ? -1 : (target_group ? (pid_t)(-target) : (pid_t)target);
        pthread_mutex_lock(&gVProcTasks.mu);
        bool found = vprocHasKillTargetLocked(probe_pid);
        pthread_mutex_unlock(&gVProcTasks.mu);
        if (found) {
            return 0;
        }
        errno = ESRCH;
        return -1;
    }

    int rc = 0;
    pthread_t self = pthread_self();
    int self_pid = broadcast_all ? (int)vprocGetPidShim() : -1;
    pthread_t *cancel_list = NULL;
    size_t cancel_count = 0;
    size_t cancel_capacity = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    if (dbg) {
        vprocDebugLogf( "[vproc-kill] target=%d group=%d broadcast=%d count=%zu\n",
                target, (int)target_group, (int)broadcast_all, gVProcTasks.count);
    }
    bool delivered = false;
    
    if (!broadcast_all && !target_group) {
        VProcTaskEntry *entry = vprocTaskFindLocked(target);
        if (entry && vprocKillDeliverEntryLocked(entry, pid, sig, dbg, self,
                                                 &cancel_list, &cancel_count, &cancel_capacity)) {
            delivered = true;
        }
    } else {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0) continue;
            if (entry->zombie || entry->exited) continue;
            if (dbg) {
                vprocDebugLogf( "[vproc-kill] scan pid=%d pgid=%d sid=%d exited=%d zombie=%d\n",
                        entry->pid, entry->pgid, entry->sid, entry->exited, entry->zombie);
            }

            if (broadcast_all) {
                // Don't kill self in broadcast
                if (self_pid > 0 && entry->pid == self_pid) continue;
            } else if (target_group) {
                if (entry->pgid != target) continue;
            }

            if (vprocKillDeliverEntryLocked(entry, pid, sig, dbg, self,
                                            &cancel_list, &cancel_count, &cancel_capacity)) {
                delivered = true;
            }
        }
    }
    
    pthread_cond_broadcast(&gVProcTasks.cv);
    pthread_mutex_unlock(&gVProcTasks.mu);

    for (size_t i = 0; i < cancel_count; ++i) {
        pthread_cancel(cancel_list[i]);
    }
    free(cancel_list);

    if (delivered) return rc;

    if (dbg) {
        vprocDebugLogf( "[vproc-kill] no targets pid=%d target=%d group=%d broadcast=%d\n",
                (int)pid, target, (int)target_group, (int)broadcast_all);
    }
    errno = ESRCH;
    return -1;
}

/* ... (Remaining functions for getters/setters/shell-self-pid are unchanged logically) ... */
/* INCLUDED HERE FOR COMPLETENESS */

pid_t vprocGetPidShim(void) {
    VProc *vp = vprocCurrent();
    if (vp) {
        return vprocPid(vp);
    }
    int shell_pid = vprocGetShellSelfPid();
    if (shell_pid > 0) {
        return shell_pid;
    }
    return vprocHostGetpidRaw();
}

static inline bool vprocShimHasVirtualContext(void) {
    return vprocCurrent() != NULL || vprocGetShellSelfPid() > 0;
}

typedef struct {
    bool active;
    bool in_child;
    sigjmp_buf parent_env;
    VProc *child_vp;
    int child_pid;
} VProcSimForkState;

typedef struct {
    VProc *vp;
    VProcExecEntryFn entry;
    int argc;
    char **argv;
} VProcSimExecCtx;

static __thread VProcSimForkState gVProcSimForkState;

static bool vprocSimForkDebugEnabled(void) {
    const char *tool_debug = getenv("PSCALI_TOOL_DEBUG");
    const char *ssh_debug = getenv("PSCALI_SSH_DEBUG");
    if ((tool_debug && *tool_debug && strcmp(tool_debug, "0") != 0) ||
        (ssh_debug && *ssh_debug && strcmp(ssh_debug, "0") != 0)) {
        return true;
    }
    return false;
}

static void vprocSimForkLog(const char *fmt, ...) {
    if (!vprocSimForkDebugEnabled() || !fmt) {
        return;
    }
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#if defined(PSCAL_TARGET_IOS)
    if (&pscalRuntimeDebugLog != NULL) {
        pscalRuntimeDebugLog(buf);
    } else {
        fprintf(stderr, "%s\n", buf);
    }
#else
    fprintf(stderr, "%s\n", buf);
#endif
}

static char **vprocSimDupArgv(char *const argv[], int *out_argc) {
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

static void vprocSimFreeArgv(char **argv, int argc) {
    if (!argv) {
        return;
    }
    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);
}

static void *vprocSimExecThread(void *arg) {
    VProcSimExecCtx *ctx = (VProcSimExecCtx *)arg;
    int status = 127;
    if (ctx && ctx->entry) {
        status = ctx->entry(ctx->argc, ctx->argv);
    }
    if (ctx && ctx->vp) {
        vprocMarkExit(ctx->vp, status);
        vprocDestroy(ctx->vp);
    }
    if (ctx) {
        vprocSimFreeArgv(ctx->argv, ctx->argc);
        free(ctx);
    }
    return (void *)(intptr_t)status;
}

static int vprocSimSpawnChild(VProc *vp, VProcExecEntryFn entry, char *const argv[]) {
    int argc = 0;
    char **argv_copy = vprocSimDupArgv(argv, &argc);
    if (!argv_copy) {
        errno = ENOMEM;
        return -1;
    }
    VProcSimExecCtx *ctx = (VProcSimExecCtx *)calloc(1, sizeof(VProcSimExecCtx));
    if (!ctx) {
        vprocSimFreeArgv(argv_copy, argc);
        errno = ENOMEM;
        return -1;
    }
    ctx->vp = vp;
    ctx->entry = entry;
    ctx->argc = argc;
    ctx->argv = argv_copy;

    pthread_t tid;
    int err = vprocSpawnThread(vp, vprocSimExecThread, ctx, &tid);
    if (err != 0) {
        vprocSimFreeArgv(argv_copy, argc);
        free(ctx);
        errno = err;
        vprocSimForkLog("[vproc-fork] spawn thread failed err=%d", err);
        return -1;
    }
    pthread_detach(tid);
    vprocSimForkLog("[vproc-fork] spawn thread ok");
    return 0;
}

pid_t vprocGetPpidShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetppidRaw();
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    pid_t parent = (pid_t)-1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        parent = (pid_t)entry->parent_pid;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return parent;
}

pid_t vprocSimulatedFork(const char *label, bool inherit_parent_pgid) {
    VProcSimForkState *state = &gVProcSimForkState;
    const char *fork_label = (label && *label) ? label : "fork";
    vprocSimForkLog("[vproc-fork] fork enter active=%d in_child=%d",
                    state->active ? 1 : 0,
                    state->in_child ? 1 : 0);
    if (state->active) {
        errno = EAGAIN;
        return -1;
    }
    volatile int jump_rc = sigsetjmp(state->parent_env, 1);
    if (jump_rc != 0) {
        state = &gVProcSimForkState;
        state->active = false;
        state->in_child = false;
        state->child_vp = NULL;
        int pid = state->child_pid;
        state->child_pid = 0;
        vprocSimForkLog("[vproc-fork] fork parent resume pid=%d", pid);
        return (pid_t)pid;
    }

    VProcCommandScope scope;
    if (!vprocCommandScopeBegin(&scope, fork_label, true, inherit_parent_pgid)) {
        errno = ENOSYS;
        vprocSimForkLog("[vproc-fork] vprocCommandScopeBegin failed");
        return -1;
    }

    state->active = true;
    state->in_child = true;
    state->child_vp = scope.vp;
    state->child_pid = scope.pid;
    vprocSimForkLog("[vproc-fork] fork child pid=%d", scope.pid);
    return 0;
}

int vprocSimulatedExec(VProcExecEntryFn entry, char *const argv[]) {
    VProcSimForkState *state = &gVProcSimForkState;
    vprocSimForkLog("[vproc-fork] exec entry=%p active=%d in_child=%d child_vp=%p child_pid=%d",
                    (void *)entry,
                    state->active ? 1 : 0,
                    state->in_child ? 1 : 0,
                    (void *)state->child_vp,
                    state->child_pid);
    if (!state->active || !state->in_child || !state->child_vp) {
        errno = ENOSYS;
        vprocSimForkLog("[vproc-fork] exec invalid fork state");
        return -1;
    }
    if (!entry) {
        state->active = false;
        state->in_child = false;
        state->child_vp = NULL;
        state->child_pid = 0;
        errno = ENOENT;
        vprocSimForkLog("[vproc-fork] exec missing entry");
        return -1;
    }
    if (vprocSimSpawnChild(state->child_vp, entry, argv) != 0) {
        if (errno == 0) {
            errno = EIO;
        }
        vprocSimForkLog("[vproc-fork] exec spawn failed errno=%d", errno);
        state->active = false;
        state->in_child = false;
        state->child_vp = NULL;
        state->child_pid = 0;
        return -1;
    }
    vprocSimForkLog("[vproc-fork] exec spawn ok, jumping to parent");
    vprocUnregisterThread(state->child_vp, pthread_self());
    vprocDeactivate();
    siglongjmp(state->parent_env, 1);
    return -1;
}

bool vprocCommandScopeBegin(VProcCommandScope *scope,
                            const char *label,
                            bool force_new_vproc,
                            bool inherit_parent_pgid) {
    if (!scope) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    (void)vprocEnsureKernelPid();
#endif
    memset(scope, 0, sizeof(*scope));
    scope->prev = vprocCurrent();

    int shell_pid = vprocGetShellSelfPid();
    bool need_new = force_new_vproc ||
                    scope->prev == NULL ||
                    (shell_pid > 0 && scope->prev && vprocPid(scope->prev) == shell_pid);
    if (!need_new) {
        return false;
    }

    VProcOptions opts = vprocDefaultOptions();
    opts.pid_hint = vprocReservePid();
    if (scope->prev) {
        int host_in = vprocTranslateFd(scope->prev, STDIN_FILENO);
        int host_out = vprocTranslateFd(scope->prev, STDOUT_FILENO);
        int host_err = vprocTranslateFd(scope->prev, STDERR_FILENO);
        if (host_in >= 0) opts.stdin_fd = host_in;
        if (host_out >= 0) opts.stdout_fd = host_out;
        if (host_err >= 0) opts.stderr_fd = host_err;
    } else {
        opts.stdin_fd = STDIN_FILENO;
        opts.stdout_fd = STDOUT_FILENO;
        opts.stderr_fd = STDERR_FILENO;
    }

    VProc *vp = vprocCreate(&opts);
    if (!vp) {
        opts.stdin_fd = -2;
        vp = vprocCreate(&opts);
    }
    if (!vp) {
        return false;
    }

    vprocRegisterThread(vp, pthread_self());
    int pid = vprocPid(vp);
    scope->vp = vp;
    scope->pid = pid;

    int owner_pid = scope->prev ? vprocPid(scope->prev) : vprocGetShellSelfPid();
    int kernel_pid = vprocGetKernelPid();
    int parent_pid = owner_pid;
    if (parent_pid <= 0 || parent_pid == pid) {
        parent_pid = (kernel_pid > 0) ? kernel_pid : owner_pid;
    }
    if (parent_pid > 0 && parent_pid != pid) {
        vprocSetParent(pid, parent_pid);
    }

    if (inherit_parent_pgid) {
        int parent_pgid = (owner_pid > 0) ? vprocGetPgid(owner_pid) : -1;
        if (parent_pgid > 0) {
            vprocSetPgid(pid, parent_pgid);
        } else {
            vprocSetPgid(pid, pid);
        }
    } else {
        vprocSetPgid(pid, pid);
    }

    if (label && *label) {
        vprocSetCommandLabel(pid, label);
    }

    if (vprocIsShellSelfThread() && !force_new_vproc) {
        pthread_mutex_lock(&gVProcTasks.mu);
        VProcTaskEntry *entry = vprocTaskFindLocked(pid);
        if (entry) {
            entry->stop_unsupported = true;
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
    }

    vprocActivate(vp);
    return true;
}

void vprocCommandScopeEnd(VProcCommandScope *scope, int exit_code) {
    if (!scope || !scope->vp) {
        return;
    }

    VProc *vp = scope->vp;
    int pid = scope->pid > 0 ? scope->pid : vprocPid(vp);

    vprocDeactivate();
    vprocMarkExit(vp, exit_code);
    vprocDiscard(pid);
    vprocDestroy(vp);

    scope->prev = NULL;
    scope->vp = NULL;
    scope->pid = 0;
}

pid_t vprocGetpgrpShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetpgrpRaw();
    }
    pid_t pid = vprocGetPidShim();
    int pgid = vprocGetPgid((int)pid);
    return (pid_t)pgid;
}

pid_t vprocGetpgidShim(pid_t pid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetpgidRaw(pid);
    }
    int target = (pid == 0) ? (int)vprocGetPidShim() : (int)pid;
    int pgid = vprocGetPgid(target);
    return (pid_t)pgid;
}

int vprocSetpgidShim(pid_t pid, pid_t pgid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostSetpgidRaw(pid, pgid);
    }
    return vprocSetPgid((int)pid, (int)pgid);
}

pid_t vprocGetsidShim(pid_t pid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostGetsidRaw(pid);
    }
    int target = (pid == 0) ? (int)vprocGetPidShim() : (int)pid;
    int sid = vprocGetSid(target);
    return (pid_t)sid;
}

pid_t vprocSetsidShim(void) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostSetsidRaw();
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    pid_t rc = (pid_t)-1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        errno = ESRCH;
        goto out;
    }
    if (entry->pgid == pid) {
        errno = EPERM;
        goto out;
    }
    entry->sid = pid;
    entry->pgid = pid;
    entry->session_leader = true;
    entry->fg_pgid = pid;
    entry->blocked_signals = 0;
    entry->pending_signals = 0;
    rc = (pid_t)pid;
out:
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

pid_t vprocTcgetpgrpShim(int fd) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostTcgetpgrpRaw(fd);
    }
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            dword_t fg = 0;
            int res = _ENOTTY;
            if (pscal_fd->ops && pscal_fd->ops->ioctl) {
                res = pscal_fd->ops->ioctl(pscal_fd, TIOCGPGRP_, &fg);
            }
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                vprocSetCompatErrno(res);
                return (pid_t)-1;
            }
            return (pid_t)fg;
        }
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return (pid_t)-1;
    }
    int sid = vprocGetSid(pid);
    if (sid <= 0) {
        errno = ENOTTY;
        return (pid_t)-1;
    }
    int fg = vprocGetForegroundPgid(sid);
    return (pid_t)fg;
}

int vprocTcsetpgrpShim(int fd, pid_t pgid) {
    if (!vprocShimHasVirtualContext()) {
        return vprocHostTcsetpgrpRaw(fd, pgid);
    }
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            dword_t fg = (dword_t)pgid;
            int res = _ENOTTY;
            if (pscal_fd->ops && pscal_fd->ops->ioctl) {
                res = pscal_fd->ops->ioctl(pscal_fd, TIOCSPGRP_, &fg);
            }
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                vprocSetCompatErrno(res);
                return -1;
            }
            return 0;
        }
    }
    if (pgid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int pid = (int)vprocGetPidShim();
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    int sid = vprocGetSid(pid);
    if (sid <= 0) {
        errno = ENOTTY;
        return -1;
    }

    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    vprocTaskTableRepairLocked();
    VProcTaskEntry *leader = vprocSessionLeaderBySidLocked(sid);
    bool group_ok = false;
    if (leader && leader->sid == sid && leader->pgid == (int)pgid) {
        group_ok = true;
    }
    if (!group_ok) {
        VProcTaskEntry *pgid_entry = vprocTaskFindLocked((int)pgid);
        if (pgid_entry &&
            pgid_entry->sid == sid &&
            pgid_entry->pgid == (int)pgid) {
            group_ok = true;
        }
    }
    if (!group_ok) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0) continue;
            if (entry->sid != sid) continue;
            if (entry->pgid == (int)pgid) {
                group_ok = true;
                break;
            }
        }
    }
    if (!leader) {
        errno = ESRCH;
        goto out_tcset;
    }
    if (!group_ok) {
        errno = EPERM;
        goto out_tcset;
    }
    leader->fg_pgid = (int)pgid;
    rc = 0;
out_tcset:
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rc == 0) {
        vprocSyncForegroundPgidToSessionTty(sid, (int)pgid);
    }
    return rc;
}

void vprocSetShellSelfPid(int pid) {
    gShellSelfPid = pid;
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && !vprocSessionStdioIsDefault(session)) {
        session->shell_pid = pid;
    } else if (pid > 0) {
        gShellSelfPidGlobal = pid;
    }
    if (pid > 0) {
        gVProcInterposeReady = 1;
    }
}

int vprocGetShellSelfPid(void) {
    if (gShellSelfPid > 0) {
        return gShellSelfPid;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session && !vprocSessionStdioIsDefault(session) && session->shell_pid > 0) {
        return session->shell_pid;
    }
    return gShellSelfPidGlobal;
}

int vprocThreadIsRegistered(pthread_t tid) {
    bool registered = false;
    int self_pid_hint = -1;
    if (pthread_equal(tid, pthread_self())) {
        VProc *current = vprocCurrent();
        if (current) {
            self_pid_hint = vprocPid(current);
        }
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    if (self_pid_hint > 0) {
        VProcTaskEntry *entry = vprocTaskFindLocked(self_pid_hint);
        registered = vprocTaskEntryHasThreadLocked(entry, tid);
    }
    if (!registered) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (vprocTaskEntryHasThreadLocked(entry, tid)) {
                registered = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return registered ? 1 : 0;
}

int vprocThreadIsRegisteredNonblocking(pthread_t tid) {
    bool registered = false;
    int self_pid_hint = -1;
    if (pthread_equal(tid, pthread_self())) {
        VProc *current = vprocCurrent();
        if (current) {
            self_pid_hint = vprocPid(current);
        }
    }
    if (pthread_mutex_trylock(&gVProcTasks.mu) != 0) {
        return 0;
    }
    if (self_pid_hint > 0) {
        VProcTaskEntry *entry = vprocTaskFindLocked(self_pid_hint);
        registered = vprocTaskEntryHasThreadLocked(entry, tid);
    }
    if (!registered) {
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (vprocTaskEntryHasThreadLocked(entry, tid)) {
                registered = true;
                break;
            }
        }
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return registered ? 1 : 0;
}

static bool vprocInterposeBypassFindIndexLocked(pthread_t tid, size_t *out_index) {
    if (out_index) {
        *out_index = 0;
    }
    if (gVProcInterposeBypassRegistry.hint_valid &&
        gVProcInterposeBypassRegistry.hint_index < gVProcInterposeBypassRegistry.count &&
        pthread_equal(gVProcInterposeBypassRegistry.hint_tid, tid)) {
        size_t idx = gVProcInterposeBypassRegistry.hint_index;
        if (pthread_equal(gVProcInterposeBypassRegistry.items[idx], tid)) {
            if (out_index) {
                *out_index = idx;
            }
            return true;
        }
    }
    for (size_t i = 0; i < gVProcInterposeBypassRegistry.count; ++i) {
        if (!pthread_equal(gVProcInterposeBypassRegistry.items[i], tid)) {
            continue;
        }
        gVProcInterposeBypassRegistry.hint_valid = true;
        gVProcInterposeBypassRegistry.hint_tid = tid;
        gVProcInterposeBypassRegistry.hint_index = i;
        if (out_index) {
            *out_index = i;
        }
        return true;
    }
    return false;
}

int vprocThreadIsInterposeBypassed(pthread_t tid) {
    bool bypassed = false;
    pthread_mutex_lock(&gVProcInterposeBypassRegistry.mu);
    bypassed = vprocInterposeBypassFindIndexLocked(tid, NULL);
    pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
    return bypassed ? 1 : 0;
}

void vprocRegisterInterposeBypassThread(pthread_t tid) {
    pthread_mutex_lock(&gVProcInterposeBypassRegistry.mu);
    if (vprocInterposeBypassFindIndexLocked(tid, NULL)) {
        pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
        return;
    }
    if (gVProcInterposeBypassRegistry.count >= gVProcInterposeBypassRegistry.capacity) {
        size_t new_capacity = gVProcInterposeBypassRegistry.capacity ? gVProcInterposeBypassRegistry.capacity * 2 : 8;
        pthread_t *items = realloc(gVProcInterposeBypassRegistry.items, new_capacity * sizeof(*items));
        if (!items) {
            pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
            return;
        }
        gVProcInterposeBypassRegistry.items = items;
        gVProcInterposeBypassRegistry.capacity = new_capacity;
    }
    size_t idx = gVProcInterposeBypassRegistry.count++;
    gVProcInterposeBypassRegistry.items[idx] = tid;
    gVProcInterposeBypassRegistry.hint_valid = true;
    gVProcInterposeBypassRegistry.hint_tid = tid;
    gVProcInterposeBypassRegistry.hint_index = idx;
    pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
}

void vprocUnregisterInterposeBypassThread(pthread_t tid) {
    pthread_mutex_lock(&gVProcInterposeBypassRegistry.mu);
    size_t idx = 0;
    if (vprocInterposeBypassFindIndexLocked(tid, &idx)) {
        size_t last = gVProcInterposeBypassRegistry.count - 1;
        gVProcInterposeBypassRegistry.items[idx] = gVProcInterposeBypassRegistry.items[last];
        gVProcInterposeBypassRegistry.count--;
        gVProcInterposeBypassRegistry.hint_valid = false;
        gVProcInterposeBypassRegistry.hint_tid = 0;
        gVProcInterposeBypassRegistry.hint_index = 0;
    }
    pthread_mutex_unlock(&gVProcInterposeBypassRegistry.mu);
}

int vprocInterposeReady(void) {
    return gVProcInterposeReady != 0;
}

int vprocThreadHasActiveVproc(void) {
    if (!gVProcTlsReady) {
        return 0;
    }
    return gVProcCurrent != NULL;
}

void vprocInterposeBypassEnter(void) {
    gVProcInterposeBypassDepth++;
}

void vprocInterposeBypassExit(void) {
    if (gVProcInterposeBypassDepth > 0) {
        gVProcInterposeBypassDepth--;
    }
}

int vprocInterposeBypassActive(void) {
    if (!gVProcTlsReady) {
        return 0;
    }
    return gVProcInterposeBypassDepth > 0;
}

static void vprocEnsurePathTruncationDefault(void) {
    if (__atomic_load_n(&gPathTruncateInit, __ATOMIC_ACQUIRE)) {
        return;
    }
    pthread_mutex_lock(&gPathTruncateMu);
    if (gPathTruncateInit) {
        pthread_mutex_unlock(&gPathTruncateMu);
        return;
    }
    __atomic_store_n(&gPathTruncateInit, true, __ATOMIC_RELEASE);
    const char *disabled = getenv("PSCALI_PATH_TRUNCATE_DISABLED");
    if (disabled && disabled[0] != '\0') {
        pthread_mutex_unlock(&gPathTruncateMu);
        return;
    }
    const char *container_root = getenv("PSCALI_CONTAINER_ROOT");
    size_t container_root_len = (container_root && container_root[0] != '\0')
            ? strlen(container_root)
            : 0;
    const char *raw_prefix = getenv("PATH_TRUNCATE");
    if (!raw_prefix || raw_prefix[0] == '\0') {
        raw_prefix = container_root;
    }
    if (!raw_prefix || raw_prefix[0] != '/') {
        raw_prefix = getenv("HOME");
    }
    if (!raw_prefix || raw_prefix[0] != '/') {
        pthread_mutex_unlock(&gPathTruncateMu);
        return;
    }
    char prefbuf[PATH_MAX];
    const char *prefix = raw_prefix;
    /* On iOS, most runtime assets live under <container>/Documents. When PATH
     * truncation is anchored at the container root, /bin expands to
     * <container>/bin which does not exist. Prefer the Documents root so /bin
     * maps to <container>/Documents/bin where exsh assets are staged. */
    if (container_root_len > 0 &&
        strncmp(raw_prefix, container_root, container_root_len) == 0) {
        size_t n = snprintf(prefbuf, sizeof(prefbuf), "%s/Documents", raw_prefix);
        if (n > 0 && n < sizeof(prefbuf)) {
            prefix = prefbuf;
        }
    }
    pathTruncateApplyEnvironment(prefix);
    pthread_mutex_unlock(&gPathTruncateMu);
}

void vprocApplyPathTruncation(const char *prefix) {
    pthread_mutex_lock(&gPathTruncateMu);
    __atomic_store_n(&gPathTruncateInit, true, __ATOMIC_RELEASE);
    if (prefix && prefix[0] == '/') {
        unsetenv("PSCALI_PATH_TRUNCATE_DISABLED");
        const char *container_root = getenv("PSCALI_CONTAINER_ROOT");
        size_t container_root_len = (container_root && container_root[0] != '\0')
                ? strlen(container_root)
                : 0;
        char prefbuf[PATH_MAX];
        const char *use_prefix = prefix;
        if (container_root_len > 0 &&
            strncmp(prefix, container_root, container_root_len) == 0) {
            size_t n = snprintf(prefbuf, sizeof(prefbuf), "%s/Documents", prefix);
            if (n > 0 && n < sizeof(prefbuf)) {
                use_prefix = prefbuf;
            }
        }
        pathTruncateApplyEnvironment(use_prefix);
    } else {
        setenv("PSCALI_PATH_TRUNCATE_DISABLED", "1", 1);
        pathTruncateApplyEnvironment(NULL);
    }
    pthread_mutex_unlock(&gPathTruncateMu);
}

void vprocSetShellSelfTid(pthread_t tid) {
    gShellSelfTid = tid;
    gShellSelfTidValid = true;
}

bool vprocIsShellSelfThread(void) {
    return gShellSelfTidValid && pthread_equal(pthread_self(), gShellSelfTid);
}

static void *vprocKernelThreadMain(void *arg) {
    int pid = (int)(intptr_t)arg;
    sigset_t mask;
    sigfillset(&mask);
    vprocHostPthreadSigmaskRaw(SIG_BLOCK, &mask, NULL);
    vprocRegisterInterposeBypassThread(pthread_self());
    if (pid > 0) {
        vprocSetKernelPid(pid);
        vprocRegisterTidHint(pid, pthread_self());
    }
#if defined(__APPLE__)
    pthread_setname_np("kernel");
#else
    pthread_setname_np(pthread_self(), "kernel");
#endif
    pthread_mutex_lock(&gKernelThreadMu);
    gKernelThreadReady = true;
    pthread_cond_broadcast(&gKernelThreadCv);
    pthread_mutex_unlock(&gKernelThreadMu);
    for (;;) {
        pause();
    }
    return NULL;
}

static void vprocWaitForKernelThreadReady(void) {
    pthread_mutex_lock(&gKernelThreadMu);
    if (!gKernelThreadReady) {
        struct timespec deadline;
        if (clock_gettime(CLOCK_REALTIME, &deadline) == 0) {
            deadline.tv_sec += 2;
            while (!gKernelThreadReady) {
                if (pthread_cond_timedwait(&gKernelThreadCv, &gKernelThreadMu, &deadline) == ETIMEDOUT) {
                    break;
                }
            }
        } else {
            while (!gKernelThreadReady) {
                pthread_cond_wait(&gKernelThreadCv, &gKernelThreadMu);
            }
        }
    }
    pthread_mutex_unlock(&gKernelThreadMu);
}

static void vprocEnsureKernelThread(int pid) {
    if (pid <= 0) {
        return;
    }
    pthread_mutex_lock(&gKernelThreadMu);
    if (gKernelThreadStarted) {
        pthread_mutex_unlock(&gKernelThreadMu);
        vprocWaitForKernelThreadReady();
        return;
    }
    gKernelThreadStarted = true;
    gKernelThreadReady = false;
    pthread_mutex_unlock(&gKernelThreadMu);

    pthread_t tid;
    int rc = vprocHostPthreadCreateRaw(&tid, NULL, vprocKernelThreadMain, (void *)(intptr_t)pid);
    if (rc != 0) {
        pthread_mutex_lock(&gKernelThreadMu);
        gKernelThreadStarted = false;
        gKernelThreadReady = false;
        pthread_mutex_unlock(&gKernelThreadMu);
        return;
    }
    pthread_detach(tid);
    pthread_mutex_lock(&gKernelThreadMu);
    gKernelThread = tid;
    pthread_mutex_unlock(&gKernelThreadMu);
    vprocWaitForKernelThreadReady();
}

void vprocSetKernelPid(int pid) {
    gKernelPid = pid;
    if (pid > 0) {
        gKernelPidGlobal = pid;
    }
}

int vprocGetKernelPid(void) {
    return (gKernelPid > 0) ? gKernelPid : gKernelPidGlobal;
}

void vprocClearKernelPidGlobal(void) {
    gKernelPidGlobal = 0;
}

int vprocGetSessionKernelPid(void) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    return session ? session->kernel_pid : 0;
}

void vprocSetSessionKernelPid(int pid) {
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (session) {
        session->kernel_pid = pid;
    }
}

int vprocEnsureKernelPid(void) {
    bool created = false;
    vprocEnsurePathTruncationDefault();
    pthread_mutex_lock(&gKernelVprocMu);
    int pid = vprocGetKernelPid();
    if (pid > 0) {
        pthread_mutex_unlock(&gKernelVprocMu);
        vprocEnsureKernelThread(pid);
        return pid;
    }

    VProcOptions kopts = vprocDefaultOptions();
    kopts.stdin_fd = -2;
    kopts.stdout_fd = -2;
    kopts.stderr_fd = -2;
    int kpid_hint = vprocReservePid();
    if (kpid_hint > 0) {
        kopts.pid_hint = kpid_hint;
    }
    gKernelVproc = vprocCreate(&kopts);
    if (gKernelVproc) {
        pid = vprocPid(gKernelVproc);
    } else if (kpid_hint > 0) {
        pid = kpid_hint;
    }
    if (pid > 0) {
        vprocSetKernelPid(pid);
        vprocSetParent(pid, 0);
        (void)vprocSetSid(pid, pid);
        vprocSetCommandLabel(pid, "kernel");
        created = true;
    }
    pthread_mutex_unlock(&gKernelVprocMu);
    vprocEnsureKernelThread(pid);
    if (created && pid > 0) {
        pthread_mutex_lock(&gVProcTasks.mu);
        for (size_t i = 0; i < gVProcTasks.count; ++i) {
            VProcTaskEntry *entry = &gVProcTasks.items[i];
            if (entry->pid <= 0) {
                continue;
            }
            if (entry->pid == pid) {
                continue;
            }
            if (entry->parent_pid != pid) {
                vprocUpdateParentLocked(entry->pid, pid);
            }
        }
        pthread_mutex_unlock(&gVProcTasks.mu);
    }
    return pid;
}

VProcSessionStdio *vprocSessionStdioCurrent(void) {
    return gSessionStdioTls ? gSessionStdioTls : &gSessionStdioDefault;
}

static int vprocSessionHostFdForStd(int std_fd);
static bool vprocSessionStdioMatchFd(int session_fd, int std_fd);

void vprocSessionStdioInit(VProcSessionStdio *stdio_ctx, int kernel_pid) {
    if (!stdio_ctx) {
        return;
    }
    VProcSessionInput *input = stdio_ctx->input;
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->shell_pid = 0;
    /* Duplicate current host stdio so this session owns stable copies. */
    int host_in = vprocSessionHostFdForStd(STDIN_FILENO);
    int host_out = vprocSessionHostFdForStd(STDOUT_FILENO);
    int host_err = vprocSessionHostFdForStd(STDERR_FILENO);
    int in = (host_in >= 0) ? fcntl(host_in, F_DUPFD_CLOEXEC, 0) : -1;
    int out = (host_out >= 0) ? fcntl(host_out, F_DUPFD_CLOEXEC, 0) : -1;
    int err = (host_err >= 0) ? fcntl(host_err, F_DUPFD_CLOEXEC, 0) : -1;
    if (in < 0 && errno == EINVAL && host_in >= 0) in = vprocHostDupRaw(host_in);
    if (out < 0 && errno == EINVAL && host_out >= 0) out = vprocHostDupRaw(host_out);
    if (err < 0 && errno == EINVAL && host_err >= 0) err = vprocHostDupRaw(host_err);
    if (in >= 0) fcntl(in, F_SETFD, FD_CLOEXEC);
    if (out >= 0) fcntl(out, F_SETFD, FD_CLOEXEC);
    if (err >= 0) fcntl(err, F_SETFD, FD_CLOEXEC);
    stdio_ctx->stdin_host_fd = in;
    stdio_ctx->stdout_host_fd = out;
    stdio_ctx->stderr_host_fd = err;
    stdio_ctx->input = input;
    stdio_ctx->stdin_pscal_fd = NULL;
    stdio_ctx->stdout_pscal_fd = NULL;
    stdio_ctx->stderr_pscal_fd = NULL;
    stdio_ctx->pty_master = NULL;
    stdio_ctx->pty_slave = NULL;
    stdio_ctx->pty_out_thread = 0;
    stdio_ctx->pty_active = false;
    stdio_ctx->session_id = 0;
}

void vprocSessionStdioActivate(VProcSessionStdio *stdio_ctx) {
    gSessionStdioTls = stdio_ctx ? stdio_ctx : &gSessionStdioDefault;
    vprocIoTrace("[vproc-io] activate stdio=%p session=%llu pty_active=%d host=(%d,%d,%d) pscal=(%p,%p,%p)",
                 (void *)gSessionStdioTls,
                 (unsigned long long)(gSessionStdioTls ? gSessionStdioTls->session_id : 0),
                 (gSessionStdioTls && gSessionStdioTls->pty_active) ? 1 : 0,
                 gSessionStdioTls ? gSessionStdioTls->stdin_host_fd : -1,
                 gSessionStdioTls ? gSessionStdioTls->stdout_host_fd : -1,
                 gSessionStdioTls ? gSessionStdioTls->stderr_host_fd : -1,
                 gSessionStdioTls ? (void *)gSessionStdioTls->stdin_pscal_fd : NULL,
                 gSessionStdioTls ? (void *)gSessionStdioTls->stdout_pscal_fd : NULL,
                 gSessionStdioTls ? (void *)gSessionStdioTls->stderr_pscal_fd : NULL);
    /* Ensure the session PTY is marked as the controlling terminal for this
     * session so /dev/tty resolves for non-leader vprocs (pipeline stages). */
    if (gSessionStdioTls && gSessionStdioTls->pty_slave && gSessionStdioTls->pty_slave->tty) {
        struct tty *tty = gSessionStdioTls->pty_slave->tty;
        int sid = pscalTtyCurrentSid();
        if (sid > 0) {
            lock(&tty->lock);
            if (tty->session == 0) {
                tty->session = (pid_t_)sid;
                tty->fg_group = (pid_t_)sid;
            }
            unlock(&tty->lock);
            pscalTtySetControlling(tty);
        }
    }
    if (!gSessionStdioTls || vprocSessionStdioIsDefault(gSessionStdioTls)) {
        gShellSelfPid = 0;
    } else if (gSessionStdioTls->shell_pid > 0) {
        gShellSelfPid = gSessionStdioTls->shell_pid;
    }
}

void vprocSessionStdioInitWithFds(VProcSessionStdio *stdio_ctx,
                                  int stdin_fd,
                                  int stdout_fd,
                                  int stderr_fd,
                                  int kernel_pid) {
    if (!stdio_ctx) {
        return;
    }
    VProcSessionInput *input = stdio_ctx->input;
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->shell_pid = 0;
    int in = (stdin_fd >= 0) ? fcntl(stdin_fd, F_DUPFD_CLOEXEC, 0) : -1;
    int out = (stdout_fd >= 0) ? fcntl(stdout_fd, F_DUPFD_CLOEXEC, 0) : -1;
    int err = (stderr_fd >= 0) ? fcntl(stderr_fd, F_DUPFD_CLOEXEC, 0) : -1;
    if (in < 0 && errno == EINVAL && stdin_fd >= 0) in = vprocHostDupRaw(stdin_fd);
    if (out < 0 && errno == EINVAL && stdout_fd >= 0) out = vprocHostDupRaw(stdout_fd);
    if (err < 0 && errno == EINVAL && stderr_fd >= 0) err = vprocHostDupRaw(stderr_fd);
    if (in >= 0) fcntl(in, F_SETFD, FD_CLOEXEC);
    if (out >= 0) fcntl(out, F_SETFD, FD_CLOEXEC);
    if (err >= 0) fcntl(err, F_SETFD, FD_CLOEXEC);
    stdio_ctx->stdin_host_fd = in;
    stdio_ctx->stdout_host_fd = out;
    stdio_ctx->stderr_host_fd = err;
    stdio_ctx->input = input;
    stdio_ctx->stdin_pscal_fd = NULL;
    stdio_ctx->stdout_pscal_fd = NULL;
    stdio_ctx->stderr_pscal_fd = NULL;
    stdio_ctx->pty_master = NULL;
    stdio_ctx->pty_slave = NULL;
    stdio_ctx->pty_out_thread = 0;
    stdio_ctx->pty_active = false;
    stdio_ctx->session_id = 0;
}

int vprocSessionStdioInitWithPty(VProcSessionStdio *stdio_ctx,
                                 struct pscal_fd *pty_slave,
                                 struct pscal_fd *pty_master,
                                 uint64_t session_id,
                                 int kernel_pid) {
    if (!stdio_ctx || !pty_slave || !pty_master) {
        errno = EINVAL;
        return -1;
    }
    stdio_ctx->kernel_pid = kernel_pid;
    stdio_ctx->shell_pid = 0;
    stdio_ctx->session_id = session_id;
    stdio_ctx->stdin_host_fd = -1;
    stdio_ctx->stdout_host_fd = -1;
    stdio_ctx->stderr_host_fd = -1;
    stdio_ctx->input = NULL;

    stdio_ctx->pty_master = pty_master;
    stdio_ctx->pty_slave = pty_slave;
    stdio_ctx->stdin_pscal_fd = pty_slave;
    stdio_ctx->stdout_pscal_fd = pscal_fd_retain(pty_slave);
    stdio_ctx->stderr_pscal_fd = pscal_fd_retain(pty_slave);
    stdio_ctx->pty_active = true;
    vprocIoTrace("[vproc-io] stdio init session=%llu master=%p slave=%p",
                 (unsigned long long)session_id,
                 (void *)pty_master,
                 (void *)pty_slave);
    vprocPtyTrace("[PTY] init session=%llu master=%p slave=%p",
                  (unsigned long long)session_id,
                  (void *)pty_master,
                  (void *)pty_slave);

    int rc = 0;
    rc = vprocHostPthreadCreate(&stdio_ctx->pty_out_thread, NULL, vprocSessionPtyOutputThread, stdio_ctx);
    if (rc != 0) {
        vprocPtyTrace("[PTY] output thread create failed rc=%d", rc);
        stdio_ctx->pty_active = false;
        if (stdio_ctx->stdout_pscal_fd) {
            pscal_fd_close(stdio_ctx->stdout_pscal_fd);
            stdio_ctx->stdout_pscal_fd = NULL;
        }
        if (stdio_ctx->stderr_pscal_fd) {
            pscal_fd_close(stdio_ctx->stderr_pscal_fd);
            stdio_ctx->stderr_pscal_fd = NULL;
        }
        stdio_ctx->stdin_pscal_fd = NULL;
        stdio_ctx->pty_master = NULL;
        stdio_ctx->pty_slave = NULL;
        errno = rc;
        return -1;
    }

    vprocSessionPtyRegister(session_id, pty_slave, pty_master);
    return 0;
}

VProcSessionStdio *vprocSessionStdioCreate(void) {
    VProcSessionStdio *session = (VProcSessionStdio *)calloc(1, sizeof(VProcSessionStdio));
    if (!session) {
        return NULL;
    }
    session->stdin_host_fd = -1;
    session->stdout_host_fd = -1;
    session->stderr_host_fd = -1;
    session->kernel_pid = 0;
    session->shell_pid = 0;
    session->input = NULL;
    session->stdin_pscal_fd = NULL;
    session->stdout_pscal_fd = NULL;
    session->stderr_pscal_fd = NULL;
    session->pty_master = NULL;
    session->pty_slave = NULL;
    session->pty_out_thread = 0;
    session->pty_active = false;
    session->session_id = 0;
    return session;
}

void vprocSessionStdioDestroy(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return;
    }
    if (gSessionStdioTls == stdio_ctx) {
        gSessionStdioTls = &gSessionStdioDefault;
    }
    if (stdio_ctx->session_id) {
        vprocSessionPtyUnregister(stdio_ctx->session_id);
    }
    if (stdio_ctx->pty_master && stdio_ctx->pty_master->tty) {
        lock(&stdio_ctx->pty_master->tty->lock);
        tty_hangup(stdio_ctx->pty_master->tty);
        unlock(&stdio_ctx->pty_master->tty->lock);
    }
    stdio_ctx->pty_active = false;
    if (stdio_ctx->pty_out_thread) {
        pthread_join(stdio_ctx->pty_out_thread, NULL);
        stdio_ctx->pty_out_thread = 0;
    }
    VProcSessionInput *input = stdio_ctx->input;
    if (input) {
        pthread_mutex_lock(&input->mu);
        input->stop_requested = true;
        input->len = 0;
        input->eof = true;
        input->interrupt_pending = false;
        pthread_cond_broadcast(&input->cv);
        pthread_mutex_unlock(&input->mu);
    }
    if (stdio_ctx->stdin_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdin_host_fd);
    }
    if (stdio_ctx->stdout_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdout_host_fd);
    }
    if (stdio_ctx->stderr_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stderr_host_fd);
    }
    stdio_ctx->stdin_host_fd = -1;
    stdio_ctx->stdout_host_fd = -1;
    stdio_ctx->stderr_host_fd = -1;
    if (stdio_ctx->stdout_pscal_fd) {
        pscal_fd_close(stdio_ctx->stdout_pscal_fd);
        stdio_ctx->stdout_pscal_fd = NULL;
    }
    if (stdio_ctx->stderr_pscal_fd) {
        pscal_fd_close(stdio_ctx->stderr_pscal_fd);
        stdio_ctx->stderr_pscal_fd = NULL;
    }
    if (stdio_ctx->stdin_pscal_fd) {
        pscal_fd_close(stdio_ctx->stdin_pscal_fd);
        stdio_ctx->stdin_pscal_fd = NULL;
    }
    stdio_ctx->pty_slave = NULL;
    if (stdio_ctx->pty_master) {
        pscal_fd_close(stdio_ctx->pty_master);
        stdio_ctx->pty_master = NULL;
    }
    if (input) {
        pthread_mutex_lock(&input->mu);
        while (input->reader_active) {
            pthread_cond_wait(&input->cv, &input->mu);
        }
        pthread_mutex_unlock(&input->mu);
        pthread_mutex_destroy(&input->mu);
        pthread_cond_destroy(&input->cv);
        free(input->buf);
        free(input);
        stdio_ctx->input = NULL;
    }
    free(stdio_ctx);
}

void vprocSessionStdioSetDefault(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return;
    }
    gSessionStdioDefault = *stdio_ctx;
    if (!gSessionStdioTls || gSessionStdioTls == &gSessionStdioDefault) {
        gSessionStdioTls = &gSessionStdioDefault;
    }
}

bool vprocSessionStdioIsDefault(VProcSessionStdio *stdio_ctx) {
    return stdio_ctx == &gSessionStdioDefault;
}

bool vprocSessionStdioNeedsRefresh(VProcSessionStdio *stdio_ctx) {
    if (!stdio_ctx) {
        return true;
    }
    if (stdio_ctx->pty_active || stdio_ctx->stdin_pscal_fd) {
        return false;
    }
    if (!vprocSessionStdioMatchFd(stdio_ctx->stdin_host_fd, STDIN_FILENO)) {
        return true;
    }
    if (!vprocSessionStdioMatchFd(stdio_ctx->stdout_host_fd, STDOUT_FILENO)) {
        return true;
    }
    if (!vprocSessionStdioMatchFd(stdio_ctx->stderr_host_fd, STDERR_FILENO)) {
        return true;
    }
    return false;
}

void vprocSessionStdioRefresh(VProcSessionStdio *stdio_ctx, int kernel_pid) {
    if (!stdio_ctx || !vprocSessionStdioNeedsRefresh(stdio_ctx)) {
        return;
    }
    if (vprocToolDebugEnabled()) {
        vprocDebugLogf(
                "[session-stdio] refresh stdin=%d stdout=%d stderr=%d\n",
                stdio_ctx->stdin_host_fd,
                stdio_ctx->stdout_host_fd,
                stdio_ctx->stderr_host_fd);
    }
    VProcSessionInput *input = stdio_ctx->input;
    if (input) {
        pthread_mutex_lock(&input->mu);
        input->stop_requested = true;
        input->len = 0;
        input->eof = false;
        input->interrupt_pending = false;
        pthread_cond_broadcast(&input->cv);
        pthread_mutex_unlock(&input->mu);
    }
    if (stdio_ctx->stdin_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdin_host_fd);
    }
    if (stdio_ctx->stdout_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stdout_host_fd);
    }
    if (stdio_ctx->stderr_host_fd >= 0) {
        vprocHostClose(stdio_ctx->stderr_host_fd);
    }
    stdio_ctx->stdin_host_fd = -1;
    stdio_ctx->stdout_host_fd = -1;
    stdio_ctx->stderr_host_fd = -1;
    if (input) {
        pthread_mutex_lock(&input->mu);
        while (input->reader_active) {
            pthread_cond_wait(&input->cv, &input->mu);
        }
        input->reader_fd = -1;
        pthread_mutex_unlock(&input->mu);
    }
    vprocSessionStdioInit(stdio_ctx, kernel_pid);
}

void vprocSetJobId(int pid, int job_id) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        entry->job_id = job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocGetJobId(int pid) {
    int id = 0;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        id = entry->job_id;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return id;
}

void vprocSetCommandLabel(int pid, const char *label) {
    char thread_name[16];
    bool rename_thread = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        free(entry->label);
        entry->label = NULL;
        if (label && *label) {
            entry->label = strdup(label);
        }
        vprocSetCommLocked(entry, label);
        rename_thread = vprocPrepareThreadNameLocked(entry, thread_name, sizeof(thread_name));
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (rename_thread) {
        vprocApplyThreadName(thread_name);
    }
}

bool vprocGetCommandLabel(int pid, char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) {
        return false;
    }
    bool ok = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry && entry->label && entry->label[0]) {
        strncpy(buf, entry->label, buf_len - 1);
        buf[buf_len - 1] = '\0';
        ok = true;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return ok;
}

bool vprocSigchldPending(int pid) {
    bool pending = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        pending = entry->sigchld_events > 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pending;
}

int vprocSetSigchldBlocked(int pid, bool block) {
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sigchld_blocked = block;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    if (rc == 0 && !block) {
        vprocDeliverPendingSignalsLocked(entry);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

void vprocClearSigchldPending(int pid) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->sigchld_events = 0;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

void vprocSetRusage(int pid, int utime, int stime) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->rusage_utime = utime;
        entry->rusage_stime = stime;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
}

int vprocBlockSignals(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = ((uint32_t)mask) & ~unmaskable;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->blocked_signals &= ~unmaskable;
        entry->blocked_signals |= mask_bits;
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocUnblockSignals(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->blocked_signals &= ~unmaskable;
        entry->blocked_signals &= ~mask_bits;
        vprocDeliverPendingSignalsLocked(entry);
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocIgnoreSignal(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    if (mask_bits & unmaskable) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->ignored_signals &= ~unmaskable;
        entry->ignored_signals |= mask_bits;
        entry->pending_signals &= ~mask_bits;
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t bit = vprocSigMask(sig);
            if (bit & mask_bits) {
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_flags = 0;
                entry->actions[sig].sa_handler = SIG_IGN;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

int vprocDefaultSignal(int pid, int mask) {
    int rc = -1;
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    uint32_t mask_bits = (uint32_t)mask;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        entry->ignored_signals &= ~unmaskable;
        entry->ignored_signals &= ~mask_bits;
        for (int sig = 1; sig < 32; ++sig) {
            uint32_t bit = vprocSigMask(sig);
            if (bit & mask_bits) {
                sigemptyset(&entry->actions[sig].sa_mask);
                entry->actions[sig].sa_flags = 0;
                entry->actions[sig].sa_handler = SIG_DFL;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

static struct sigaction vprocGetSigactionLocked(VProcTaskEntry *entry, int sig) {
    if (!entry || !vprocSigIndexValid(sig)) {
        struct sigaction sa;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sa.sa_handler = SIG_DFL;
        return sa;
    }
    return entry->actions[sig];
}

int vprocSigaction(int pid, int sig, const struct sigaction *act, struct sigaction *old) {
    if (!vprocSigIndexValid(sig) || sig == SIGKILL || sig == SIGSTOP) {
        errno = EINVAL;
        return -1;
    }
    uint32_t mask = vprocSigMask(sig);
    int rc = -1;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskEnsureSlotLocked(pid);
    if (entry) {
        if (old) {
            *old = vprocGetSigactionLocked(entry, sig);
        }
        if (act) {
            entry->actions[sig] = *act;
            if (act->sa_handler == SIG_IGN) {
                entry->ignored_signals |= mask;
                entry->pending_signals &= ~mask;
            } else {
                entry->ignored_signals &= ~mask;
            }
        }
        rc = 0;
    } else {
        errno = ESRCH;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return rc;
}

static VProc *vprocForThread(void) {
    if (!gVProcTlsReady) {
        return NULL;
    }
    VProc *vp = gVProcCurrent;
    if (!vp) {
        return NULL;
    }
    if (!vprocRegistryContainsValidated(vp)) {
        return NULL;
    }
    return vp;
}

static void vprocDeliverPendingSignalsForCurrent(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    bool exit_current = false;
    if (entry) {
        exit_current = vprocDeliverPendingSignalsLocked(entry);
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    if (exit_current) {
        pthread_exit(NULL);
    }
}

static bool vprocCurrentHasPendingSignals(void) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return false;
    }
    bool pending = false;
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(vprocPid(vp));
    if (entry) {
        pending = (entry->pending_signals != 0) || entry->exited || entry->zombie || entry->stopped;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return pending;
}

int vprocSigpending(int pid, sigset_t *set) {
    if (!set) {
        errno = EINVAL;
        return -1;
    }
    sigemptyset(set);
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (entry) {
        uint32_t pending = entry->pending_signals;
        for (int sig = 1; sig < 32; ++sig) {
            if ((pending & vprocSigMask(sig)) || entry->pending_counts[sig] > 0) {
                sigaddset(set, sig);
            }
        }
    } else {
        errno = ESRCH;
        pthread_mutex_unlock(&gVProcTasks.mu);
        return -1;
    }
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

int vprocSigsuspend(int pid, const sigset_t *mask) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    uint32_t original_blocked = entry->blocked_signals;
    if (mask) {
        entry->blocked_signals = 0;
        for (int sig = 1; sig < 32; ++sig) {
            if (sigismember(mask, sig)) {
                entry->blocked_signals |= vprocSigMask(sig);
            }
        }
    }
    while (true) {
        uint32_t orig_pending = entry->pending_signals;
        vprocDeliverPendingSignalsLocked(entry);
        if (orig_pending != 0) {
            break;
        }
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
    entry->blocked_signals = original_blocked;
    pthread_mutex_unlock(&gVProcTasks.mu);
    errno = EINTR;
    return -1;
}

int vprocSigprocmask(int pid, int how, const sigset_t *set, sigset_t *oldset) {
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    if (oldset) {
        sigemptyset(oldset);
        for (int sig = 1; sig < 32; ++sig) {
            if (entry->blocked_signals & vprocSigMask(sig)) {
                sigaddset(oldset, sig);
            }
        }
    }
    if (!set) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        return 0;
    }
    uint32_t mask_bits = 0;
    for (int sig = 1; sig < 32; ++sig) {
        if (sigismember(set, sig)) {
            mask_bits |= vprocSigMask(sig);
        }
    }
    uint32_t unmaskable = vprocSigMask(SIGKILL) | vprocSigMask(SIGSTOP);
    mask_bits &= ~unmaskable;
    if (how == SIG_BLOCK) {
        entry->blocked_signals |= mask_bits;
    } else if (how == SIG_UNBLOCK) {
        entry->blocked_signals &= ~mask_bits;
    } else if (how == SIG_SETMASK) {
        entry->blocked_signals = mask_bits;
    } else {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = EINVAL;
        return -1;
    }
    vprocDeliverPendingSignalsLocked(entry);
    pthread_mutex_unlock(&gVProcTasks.mu);
    return 0;
}

int vprocSigwait(int pid, const sigset_t *set, int *sig) {
    if (!set || !sig) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    while (true) {
        for (int s = 1; s < 32; ++s) {
            if (!sigismember(set, s)) continue;
            uint32_t bit = vprocSigMask(s);
            if (entry->pending_counts[s] > 0 || (entry->pending_signals & bit)) {
                if (entry->pending_counts[s] > 0) {
                    entry->pending_counts[s]--;
                }
                if (entry->pending_counts[s] <= 0) {
                    entry->pending_signals &= ~bit;
                    entry->pending_counts[s] = 0;
                }
                *sig = s;
                pthread_mutex_unlock(&gVProcTasks.mu);
                return 0;
            }
        }
        pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
    }
}

int vprocSigtimedwait(int pid, const sigset_t *set, const struct timespec *timeout, int *sig) {
    if (!set || !sig) {
        errno = EINVAL;
        return -1;
    }
    struct timespec deadline = {0, 0};
    if (timeout) {
        struct timespec now;
        if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
            return -1;
        }
        deadline.tv_sec = now.tv_sec + timeout->tv_sec;
        deadline.tv_nsec = now.tv_nsec + timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    pthread_mutex_lock(&gVProcTasks.mu);
    VProcTaskEntry *entry = vprocTaskFindLocked(pid);
    if (!entry) {
        pthread_mutex_unlock(&gVProcTasks.mu);
        errno = ESRCH;
        return -1;
    }
    while (true) {
        for (int s = 1; s < 32; ++s) {
            if (!sigismember(set, s)) continue;
            uint32_t bit = vprocSigMask(s);
            if (entry->pending_counts[s] > 0 || (entry->pending_signals & bit)) {
                if (entry->pending_counts[s] > 0) {
                    entry->pending_counts[s]--;
                }
                if (entry->pending_counts[s] <= 0) {
                    entry->pending_signals &= ~bit;
                    entry->pending_counts[s] = 0;
                }
                *sig = s;
                pthread_mutex_unlock(&gVProcTasks.mu);
                return s;
            }
        }
        if (timeout) {
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
                pthread_mutex_unlock(&gVProcTasks.mu);
                errno = EAGAIN;
                return -1;
            }
            pthread_cond_timedwait(&gVProcTasks.cv, &gVProcTasks.mu, &deadline);
        } else {
            pthread_cond_wait(&gVProcTasks.cv, &gVProcTasks.mu);
        }
    }
}

static int shimTranslate(int fd, int allow_real) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return allow_real ? fd : -1;
    }
    int host = vprocTranslateFd(vp, fd);
    if (host < 0 && allow_real && fd >= 0) {
        struct stat st;
        if (vprocHostFstatRaw(fd, &st) == 0) {
            return fd;
        }
    }
    return host;
}

static int vprocSetCompatErrno(int err) {
    errno = pscalCompatErrno(err);
    return -1;
}

static void vprocPtyTrace(const char *format, ...) {
    static int enabled = -1;
    const char *env = getenv("PSCALI_PTY_TRACE");
    if (enabled < 0 || (enabled == 0 && env && *env && strcmp(env, "0") != 0)) {
        enabled = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    }
    if (!enabled || !format) {
        return;
    }
    char buf[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
#if defined(__APPLE__)
    static void (*log_line)(const char *message) = NULL;
    static int log_line_checked = 0;
    if (!log_line_checked) {
        log_line_checked = 1;
        log_line = (void (*)(const char *))dlsym(RTLD_DEFAULT, "PSCALRuntimeLogLine");
    }
    if (log_line) {
        log_line(buf);
        return;
    }
#endif
    if (pscalRuntimeDebugLog) {
        pscalRuntimeDebugLog(buf);
    }
}

static int vprocHostIsatty(int fd) {
#if defined(PSCAL_TARGET_IOS)
    return vprocHostIsattyRaw(fd);
#else
    return isatty(fd);
#endif
}

static struct pscal_fd *vprocSessionPscalFdForStd(int fd);

int vprocIsattyShim(int fd) {
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            int res = (pscal_fd->tty != NULL) ? 1 : 0;
            pscal_fd_close(pscal_fd);
            return res;
        }
        int host_fd = vprocTranslateFd(vp, fd);
        if (host_fd >= 0) {
            return vprocHostIsatty(host_fd);
        }
    }
#if defined(PSCAL_TARGET_IOS)
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(fd);
        if (pscal_fd) {
            int res = (pscal_fd->tty != NULL) ? 1 : 0;
            pscal_fd_close(pscal_fd);
            return res;
        }
    }
#endif
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return 0;
    }
    return vprocHostIsatty(host);
}

int pscalTtyCurrentPid(void) {
    return (int)vprocGetPidShim();
}

int pscalTtyCurrentPgid(void) {
    return (int)vprocGetpgrpShim();
}

int pscalTtyCurrentSid(void) {
    return (int)vprocGetsidShim(0);
}

bool pscalTtyIsSessionLeader(void) {
    int pid = pscalTtyCurrentPid();
    if (pid <= 0) {
        return false;
    }
    int sid = vprocGetSid(pid);
    return sid > 0 && sid == pid;
}

int pscalTtySendGroupSignal(int pgid, int sig) {
    if (pgid <= 0) {
        return _ESRCH;
    }
    if (sig == SIGWINCH_) {
        vprocDebugLogf("[ssh-resize] send-group-signal pgid=%d sig=SIGWINCH", pgid);
    }
    if (vprocKillShim(-pgid, sig) < 0) {
        if (sig == SIGWINCH_) {
            vprocDebugLogf("[ssh-resize] send-group-signal failed pgid=%d sig=SIGWINCH errno=%d", pgid, errno);
        }
        return _ESRCH;
    }
    if (sig == SIGWINCH_) {
        vprocDebugLogf("[ssh-resize] send-group-signal ok pgid=%d sig=SIGWINCH", pgid);
    }
    return 0;
}

void pscalTtySetForegroundPgid(int sid, int fg_pgid) {
    if (sid <= 0 || fg_pgid <= 0) {
        return;
    }
    (void)vprocSetForegroundPgidInternal(sid, fg_pgid, false);
}

int pscalTtyGetForegroundPgid(int sid) {
    if (sid <= 0) {
        return -1;
    }
    return vprocGetForegroundPgid(sid);
}

int PSCALRuntimeSetSessionWinsize(uint64_t session_id, int cols, int rows) {
    return vprocSetSessionWinsize(session_id, cols, rows);
}

static int gVprocPipelineReadLogCount = 0;
static int gVprocPipelineWriteLogCount = 0;

ssize_t vprocReadShim(int fd, void *buf, size_t count) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            if (gVprocPipelineStage &&
                vprocVprocDebugEnabled() &&
                gVprocPipelineReadLogCount < 32) {
                gVprocPipelineReadLogCount++;
                fprintf(stderr, "[vproc-read] fd=%d using pscal=%p count=%zu\n",
                        fd, (void *)pscal_fd, count);
            }
            ssize_t res = pscal_fd->ops->read(pscal_fd, buf, count);
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                return vprocSetCompatErrno((int)res);
            }
            return res;
        }
    }
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    bool controlling_stdin = (vp && vp->stdin_host_fd >= 0 && fd == STDIN_FILENO && host == vp->stdin_host_fd);
    if (vprocToolDebugEnabled() && fd == STDIN_FILENO) {
        vprocDebugLogf(
                "[vproc-read] stdin host=%d stdin_host=%d controlling=%d from_session=%d\n",
                host,
                vp ? vp->stdin_host_fd : -1,
                (int)controlling_stdin,
                (int)(vp ? vp->stdin_from_session : 0));
    }
    if (controlling_stdin) {
        (void)vprocWaitIfStopped(vp);
    }
    if (controlling_stdin && !gVprocPipelineStage &&
        vprocShouldStopForBackgroundTty(vprocCurrent(), SIGTTIN)) {
        errno = EINTR;
        return -1;
    }
    ssize_t res;
    /* On iOS, default to direct host reads to avoid a second reader competing
     * with foreground programs. */
    res = vprocHostRead(host, buf, count);
    return res;
}

ssize_t vprocWriteShim(int fd, const void *buf, size_t count) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (vp) {
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
        if (pscal_fd) {
            if (gVprocPipelineStage &&
                vprocVprocDebugEnabled() &&
                gVprocPipelineWriteLogCount < 32) {
                gVprocPipelineWriteLogCount++;
                fprintf(stderr, "[vproc-write] fd=%d using pscal=%p count=%zu\n",
                        fd, (void *)pscal_fd, count);
            }
            ssize_t res = pscal_fd->ops->write(pscal_fd, buf, count);
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                return vprocSetCompatErrno((int)res);
            }
            return res;
        }
    }
    int host = -1;
#if defined(PSCAL_TARGET_IOS)
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    bool is_stdout = false;
    bool is_stderr = false;
    vprocSessionResolveOutputFd(session, fd, &is_stdout, &is_stderr);
    host = shimTranslate(fd, 1);
    int session_host_fd = -1;
    if (is_stdout || is_stderr) {
        session_host_fd = is_stdout ? session->stdout_host_fd : session->stderr_host_fd;
    }
    bool use_session_output = false;
    bool host_is_tty = false;
    if (host >= 0 && is_stdout) {
        host_is_tty = isatty(host);
    }
    if ((is_stdout || is_stderr) && session) {
        bool session_has_virtual =
            (session->stdout_pscal_fd != NULL) ||
            (session->stderr_pscal_fd != NULL) ||
            (session->pty_slave != NULL);
        if (session_has_virtual) {
            if (host < 0) {
                use_session_output = true;
            } else if (session_host_fd >= 0 &&
                       vprocSessionFdMatchesHost(host, session_host_fd)) {
                use_session_output = true;
            } else if (host_is_tty) {
                use_session_output = true;
            }
        }
    }
    if (use_session_output && session) {
        struct pscal_fd *target =
            is_stdout ? session->stdout_pscal_fd : session->stderr_pscal_fd;
        if (!target) {
            target = session->pty_slave;
        }
        if (target && target->ops && target->ops->write) {
            ssize_t res = target->ops->write(target, buf, count);
            if (res < 0) {
                /* If the virtual stream rejected the write, fall back to a
                 * concrete host fd to avoid dropping output. */
                int fallback_host = host;
                if (fallback_host < 0 && session_host_fd >= 0) {
                    fallback_host = session_host_fd;
                }
                if (fallback_host < 0) {
                    int tty_fd = vprocHostOpen("/dev/tty", O_WRONLY);
                    if (tty_fd < 0) {
                        tty_fd = vprocHostOpen("/dev/tty", O_WRONLY | O_NONBLOCK);
                    }
                    if (tty_fd >= 0) {
                        fallback_host = tty_fd;
                    }
                }
                if (fallback_host >= 0) {
                    res = vprocHostWrite(fallback_host, buf, count);
                    if (fallback_host != host && fallback_host != session_host_fd) {
                        vprocHostClose(fallback_host);
                    }
                }
            } else {
                /* Mirror to the host side as well when it differs so Xcode still sees logs. */
                if (host >= 0 && session_host_fd >= 0 && !vprocSessionFdMatchesHost(host, session_host_fd)) {
                    (void)vprocHostWrite(host, buf, count);
                }
            }
            if (res < 0) {
                return vprocSetCompatErrno((int)res);
            }
            return res;
        }
    }
#else
    host = shimTranslate(fd, 1);
#endif
    /* If host resolution failed, fall back to the session stream or host fd so
     * stdout/stderr is not dropped when stdio is virtualised. */
    if (host < 0) {
        if (session && (is_stdout || is_stderr)) {
            struct pscal_fd *fallback =
                is_stdout ? session->stdout_pscal_fd : session->stderr_pscal_fd;
            if (!fallback) {
                fallback = session->pty_slave;
            }
            if (fallback && fallback->ops && fallback->ops->write) {
                ssize_t res = fallback->ops->write(fallback, buf, count);
                if (res < 0) {
                    return vprocSetCompatErrno((int)res);
                }
                return res;
            }
            if (session_host_fd >= 0) {
                host = session_host_fd;
            }
        }
    }
    if (host < 0) {
        host = shimTranslate(fd, 1);
    }
    int write_fd = host;
    if (write_fd < 0 && session_host_fd >= 0) {
        write_fd = session_host_fd;
    }
    if (write_fd < 0) {
        int tty_fd = vprocHostOpen("/dev/tty", O_WRONLY);
        if (tty_fd < 0) {
            tty_fd = vprocHostOpen("/dev/tty", O_WRONLY | O_NONBLOCK);
        }
        write_fd = tty_fd;
    }
    if (write_fd < 0) {
        return -1;
    }
    if (vprocToolDebugEnabled()) {
        vprocDebugLogf( "[vwrite] fd=%d -> host=%d write_fd=%d count=%zu\n", fd, host, write_fd, count);
    }
    ssize_t res = vprocHostWrite(write_fd, buf, count);
    if (write_fd != host) {
        vprocHostClose(write_fd);
    }
    return res;
}

int vprocDupShim(int fd) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostDup(fd);
    }
    int duped = vprocDup(vp, fd);
    if (duped >= 0) {
        return duped;
    }
    int saved_errno = errno;
    if (saved_errno != EBADF) {
        return -1;
    }
    int host_fd = shimTranslate(fd, 1);
    if (host_fd < 0) {
        errno = saved_errno;
        return -1;
    }
    int cloned = vprocCloneFd(host_fd);
    if (cloned < 0) {
        return -1;
    }
    int slot = vprocInsert(vp, cloned);
    if (slot < 0) {
        vprocHostClose(cloned);
        return -1;
    }
    return slot;
}

int vprocDup2Shim(int fd, int target) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostDup2(fd, target);
    }
    int rc = vprocDup2(vp, fd, target);
    if (rc >= 0) {
        return rc;
    }
    int saved_errno = errno;
    if (saved_errno != EBADF) {
        return -1;
    }
    struct stat st;
    if (vprocHostFstatRaw(fd, &st) != 0) {
        errno = saved_errno;
        return -1;
    }
    return vprocRestoreHostFd(vp, target, fd);
}

static bool vprocHasFd(VProc *vp, int fd) {
    if (!vp || fd < 0) {
        return false;
    }
    if (!vprocRegistryContainsValidated(vp)) {
        return false;
    }
    bool has = false;
    pthread_mutex_lock(&vp->mu);
    if ((size_t)fd < vp->capacity && vp->entries[fd].kind != VPROC_FD_NONE) {
        has = true;
    }
    pthread_mutex_unlock(&vp->mu);
    return has;
}

int vprocCloseShim(int fd) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostClose(fd);
    }
    if (vprocHasFd(vp, fd)) {
        return vprocClose(vp, fd);
    }
    struct stat st;
    if (vprocHostFstatRaw(fd, &st) == 0) {
        return vprocHostClose(fd);
    }
    errno = EBADF;
    return -1;
}

int vprocFsyncShim(int fd) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFsyncRaw(fd);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        pscal_fd_close(pscal_fd);
        return 0;
    }
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostFsyncRaw(host);
}

int vprocPipeShim(int pipefd[2]) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostPipe(pipefd);
    }
    return vprocPipe(vp, pipefd);
}

int vprocSocketShim(int domain, int type, int protocol) {
    vprocDeliverPendingSignalsForCurrent();
    int fd = vprocHostSocket(domain, type, protocol);
    VProc *vp = vprocForThread();
    if (vp && fd >= 0) {
        vprocResourceTrack(vp, fd, VPROC_RESOURCE_SOCKET);
    }
    return fd;
}

int vprocAcceptShim(int fd, struct sockaddr *addr, socklen_t *addrlen) {
    vprocDeliverPendingSignalsForCurrent();
    int res = vprocHostAccept(fd, addr, addrlen);
    VProc *vp = vprocForThread();
    if (vp && res >= 0) {
        vprocResourceTrack(vp, res, VPROC_RESOURCE_SOCKET);
    }
    return res;
}

int vprocSocketpairShim(int domain, int type, int protocol, int sv[2]) {
    vprocDeliverPendingSignalsForCurrent();
    int rc = vprocHostSocketpair(domain, type, protocol, sv);
    VProc *vp = vprocForThread();
    if (rc == 0 && vp) {
        vprocResourceTrack(vp, sv[0], VPROC_RESOURCE_PIPE);
        vprocResourceTrack(vp, sv[1], VPROC_RESOURCE_PIPE);
    }
    return rc;
}

int vprocFstatShim(int fd, struct stat *st) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFstatRaw(fd, st);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        if (st) {
            memset(st, 0, sizeof(*st));
            st->st_mode = S_IFCHR | 0600;
            st->st_nlink = 1;
            if (pscal_fd->tty) {
                st->st_rdev = makedev(pscal_fd->tty->type, pscal_fd->tty->num);
                st->st_ino = (ino_t)(pscal_fd->tty->num + 3);
                st->st_uid = (uid_t)geteuid();
                st->st_gid = (gid_t)getegid();
                if (pscalPtyIsSlave(pscal_fd)) {
                    mode_t_ perms = 0620;
                    uid_t_ uid = 0;
                    gid_t_ gid = 0;
                    if (pscalPtyGetSlaveInfo(pscal_fd->tty->num, &perms, &uid, &gid) == 0) {
                        st->st_mode = S_IFCHR | (mode_t)perms;
                        st->st_uid = (uid_t)uid;
                        st->st_gid = (gid_t)gid;
                    } else {
                        st->st_mode = S_IFCHR | 0620;
                    }
                }
            }
        }
        pscal_fd_close(pscal_fd);
        return 0;
    }
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostFstatRaw(host, st);
}

static int vprocStatFillChar(struct stat *st,
                             mode_t mode,
                             dev_t rdev,
                             ino_t ino,
                             uid_t uid,
                             gid_t gid) {
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | (mode & 0777);
    st->st_nlink = 1;
    st->st_rdev = rdev;
    st->st_ino = ino;
    st->st_uid = uid;
    st->st_gid = gid;
    return 0;
}

static int vprocStatDevptsRoot(struct stat *st) {
    if (!st) {
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFDIR | 0755;
    st->st_nlink = 1;
    st->st_ino = 1;
    st->st_uid = 0;
    st->st_gid = 0;
    return 0;
}

static int vprocStatPtySlave(int pty_num, struct stat *st) {
    mode_t_ perms = 0620;
    uid_t_ uid = 0;
    gid_t_ gid = 0;
    if (pscalPtyGetSlaveInfo(pty_num, &perms, &uid, &gid) != 0) {
        errno = ENOENT;
        return -1;
    }
    return vprocStatFillChar(st,
                             (mode_t)perms,
                             makedev(TTY_PSEUDO_SLAVE_MAJOR, pty_num),
                             (ino_t)(pty_num + 3),
                             (uid_t)uid,
                             (gid_t)gid);
}

static int vprocStatShimInternal(const char *path, struct stat *st, bool follow) {
    if (!path || !st) {
        errno = EINVAL;
        return -1;
    }
    if (!vprocForThread()) {
        return follow ? vprocHostStatRaw(path, st) : vprocHostLstatRaw(path, st);
    }
    if (vprocPathIsSystemPath(path)) {
        return follow ? vprocHostStatRaw(path, st) : vprocHostLstatRaw(path, st);
    }
    if (vprocPathIsDevPtsRoot(path)) {
        return vprocStatDevptsRoot(st);
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        return vprocStatPtySlave(pty_num, st);
    }
    if (vprocPathIsDevTty(path)) {
        return vprocStatFillChar(st,
                                 0666,
                                 makedev(TTY_ALTERNATE_MAJOR, DEV_TTY_MINOR),
                                 2,
                                 0,
                                 0);
    }
    if (vprocPathIsDevConsole(path)) {
        return vprocStatFillChar(st,
                                 0666,
                                 makedev(TTY_ALTERNATE_MAJOR, DEV_CONSOLE_MINOR),
                                 3,
                                 0,
                                 0);
    }
    if (vprocPathIsPtyMaster(path)) {
        return vprocStatFillChar(st,
                                 0666,
                                 makedev(TTY_ALTERNATE_MAJOR, DEV_PTMX_MINOR),
                                 4,
                                 0,
                                 0);
    }
    int tty_num = -1;
    if (vprocPathParseConsoleTty(path, &tty_num)) {
        if (tty_num >= 1 && tty_num <= 7) {
            return vprocStatFillChar(st,
                                     0666,
                                     makedev(TTY_CONSOLE_MAJOR, tty_num),
                                     (ino_t)(10 + tty_num),
                                     0,
                                     0);
        }
        errno = ENOENT;
        return -1;
    }
    return follow ? vprocHostStatVirtualized(path, st) : vprocHostLstatVirtualized(path, st);
}

int vprocStatShim(const char *path, struct stat *st) {
    return vprocStatShimInternal(path, st, true);
}

int vprocLstatShim(const char *path, struct stat *st) {
    return vprocStatShimInternal(path, st, false);
}

int vprocChdirShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostChdirRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostChdirRaw(target ? target : path);
}

char *vprocGetcwdShim(char *buffer, size_t size) {
    VProc *vp = vprocForThread();
    char *res = vprocHostGetcwdRaw(buffer, size);
    if (!vp || !res || !pathTruncateEnabled()) {
        return res;
    }
    char stripped[PATH_MAX];
    if (!pathTruncateStrip(res, stripped, sizeof(stripped))) {
        return res;
    }
    size_t len = strlen(stripped);
    if (buffer && size > 0 && len + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(res, stripped, len + 1);
    return res;
}

int vprocAccessShim(const char *path, int mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostAccessRaw(path, mode);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostAccessRaw(target ? target : path, mode);
}

int vprocChmodShim(const char *path, mode_t mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostChmodRaw(path, mode);
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        int err = vprocPtyApplyAttrsByNum(pty_num, &mode, NULL, NULL);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostChmodRaw(target ? target : path, mode);
}

int vprocChownShim(const char *path, uid_t uid, gid_t gid) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostChownRaw(path, uid, gid);
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        int err = vprocPtyApplyAttrsByNum(pty_num, NULL, &uid, &gid);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostChownRaw(target ? target : path, uid, gid);
}

int vprocFchmodShim(int fd, mode_t mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFchmodRaw(fd, mode);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        int err = vprocPtyApplyAttrsForFd(pscal_fd, &mode, NULL, NULL);
        pscal_fd_close(pscal_fd);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    return vprocHostFchmodRaw(host_fd, mode);
}

int vprocFchownShim(int fd, uid_t uid, gid_t gid) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostFchownRaw(fd, uid, gid);
    }
    struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fd);
    if (pscal_fd) {
        int err = vprocPtyApplyAttrsForFd(pscal_fd, NULL, &uid, &gid);
        pscal_fd_close(pscal_fd);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        return 0;
    }
    int host_fd = vprocTranslateFd(vp, fd);
    if (host_fd < 0) {
        return -1;
    }
    return vprocHostFchownRaw(host_fd, uid, gid);
}

int vprocMkdirShim(const char *path, mode_t mode) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostMkdirRaw(path, mode);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostMkdirRaw(target ? target : path, mode);
}

int vprocRmdirShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRmdirRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostRmdirRaw(target ? target : path);
}

int vprocUnlinkShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostUnlinkRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostUnlinkRaw(target ? target : path);
}

int vprocRemoveShim(const char *path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRemoveRaw(path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    return vprocHostRemoveRaw(target ? target : path);
}

int vprocRenameShim(const char *oldpath, const char *newpath) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRenameRaw(oldpath, newpath);
    }
    char expanded_old[PATH_MAX];
    char expanded_new[PATH_MAX];
    const char *old_target = vprocPathExpandForShim(oldpath, expanded_old, sizeof(expanded_old));
    const char *new_target = vprocPathExpandForShim(newpath, expanded_new, sizeof(expanded_new));
    return vprocHostRenameRaw(old_target ? old_target : oldpath,
                              new_target ? new_target : newpath);
}

DIR *vprocOpendirShim(const char *name) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostOpendirRaw(name);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(name, expanded, sizeof(expanded));
    return vprocHostOpendirRaw(target ? target : name);
}

int vprocSymlinkShim(const char *target, const char *linkpath) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostSymlinkRaw(target, linkpath);
    }
    char expanded_link[PATH_MAX];
    const char *link_target = vprocPathExpandForShim(linkpath, expanded_link, sizeof(expanded_link));
    return vprocHostSymlinkRaw(target, link_target ? link_target : linkpath);
}

ssize_t vprocReadlinkShim(const char *path, char *buf, size_t size) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostReadlinkRaw(path, buf, size);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    ssize_t res = vprocHostReadlinkRaw(target ? target : path, buf, size);
    if (res >= 0 && pathTruncateEnabled() && size > 0 && (size_t)res < size) {
        buf[res] = '\0';
        char stripped[PATH_MAX];
        if (pathTruncateStrip(buf, stripped, sizeof(stripped))) {
            size_t len = strlen(stripped);
            if (len < size) {
                memcpy(buf, stripped, len + 1);
                res = (ssize_t)len;
            }
        }
    }
    return res;
}

char *vprocRealpathShim(const char *path, char *resolved_path) {
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostRealpathRaw(path, resolved_path);
    }
    char expanded[PATH_MAX];
    const char *target = vprocPathExpandForShim(path, expanded, sizeof(expanded));
    char *res = vprocHostRealpathRaw(target ? target : path, resolved_path);
    if (res && pathTruncateEnabled()) {
        char stripped[PATH_MAX];
        if (pathTruncateStrip(res, stripped, sizeof(stripped))) {
            size_t len = strlen(stripped);
            memcpy(res, stripped, len + 1);
        }
    }
    return res;
}

static void vprocTermiosFromHost(const struct termios *src, struct termios_ *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->cflags = (dword_t)src->c_cflag;
#define MAP_IN(flag) \
    do { \
        if (src->c_iflag & (flag)) dst->iflags |= flag##_; \
    } while (0)
#define MAP_OUT(flag) \
    do { \
        if (src->c_oflag & (flag)) dst->oflags |= flag##_; \
    } while (0)
#define MAP_L(flag) \
    do { \
        if (src->c_lflag & (flag)) dst->lflags |= flag##_; \
    } while (0)
#ifdef INLCR
    MAP_IN(INLCR);
#endif
#ifdef IGNCR
    MAP_IN(IGNCR);
#endif
#ifdef ICRNL
    MAP_IN(ICRNL);
#endif
#ifdef IXON
    MAP_IN(IXON);
#endif
#ifdef OPOST
    MAP_OUT(OPOST);
#endif
#ifdef ONLCR
    MAP_OUT(ONLCR);
#endif
#ifdef OCRNL
    MAP_OUT(OCRNL);
#endif
#ifdef ONOCR
    MAP_OUT(ONOCR);
#endif
#ifdef ONLRET
    MAP_OUT(ONLRET);
#endif
#ifdef ISIG
    MAP_L(ISIG);
#endif
#ifdef ICANON
    MAP_L(ICANON);
#endif
#ifdef ECHO
    MAP_L(ECHO);
#endif
#ifdef ECHOE
    MAP_L(ECHOE);
#endif
#ifdef ECHOK
    MAP_L(ECHOK);
#endif
#ifdef ECHOKE
    MAP_L(ECHOKE);
#endif
#ifdef NOFLSH
    MAP_L(NOFLSH);
#endif
#ifdef ECHOCTL
    MAP_L(ECHOCTL);
#endif
#ifdef IEXTEN
    MAP_L(IEXTEN);
#endif
#undef MAP_IN
#undef MAP_OUT
#undef MAP_L

#define MAP_CC(idx, name) \
    do { \
        if (name < (int)(sizeof(src->c_cc) / sizeof(src->c_cc[0]))) { \
            dst->cc[idx] = (byte_t)src->c_cc[name]; \
        } \
    } while (0)
#ifdef VINTR
    MAP_CC(VINTR_, VINTR);
#endif
#ifdef VQUIT
    MAP_CC(VQUIT_, VQUIT);
#endif
#ifdef VERASE
    MAP_CC(VERASE_, VERASE);
#endif
#ifdef VKILL
    MAP_CC(VKILL_, VKILL);
#endif
#ifdef VEOF
    MAP_CC(VEOF_, VEOF);
#endif
#ifdef VTIME
    MAP_CC(VTIME_, VTIME);
#endif
#ifdef VMIN
    MAP_CC(VMIN_, VMIN);
#endif
#ifdef VSTART
    MAP_CC(VSTART_, VSTART);
#endif
#ifdef VSTOP
    MAP_CC(VSTOP_, VSTOP);
#endif
#ifdef VSUSP
    MAP_CC(VSUSP_, VSUSP);
#endif
#ifdef VEOL
    MAP_CC(VEOL_, VEOL);
#endif
#ifdef VREPRINT
    MAP_CC(VREPRINT_, VREPRINT);
#endif
#ifdef VDISCARD
    MAP_CC(VDISCARD_, VDISCARD);
#endif
#ifdef VWERASE
    MAP_CC(VWERASE_, VWERASE);
#endif
#ifdef VLNEXT
    MAP_CC(VLNEXT_, VLNEXT);
#endif
#ifdef VEOL2
    MAP_CC(VEOL2_, VEOL2);
#endif
#undef MAP_CC
}

static void vprocTermiosToHost(const struct termios_ *src, struct termios *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->c_cflag = (tcflag_t)src->cflags;
#define MAP_IN(flag) \
    do { \
        if (src->iflags & flag##_) dst->c_iflag |= (flag); \
    } while (0)
#define MAP_OUT(flag) \
    do { \
        if (src->oflags & flag##_) dst->c_oflag |= (flag); \
    } while (0)
#define MAP_L(flag) \
    do { \
        if (src->lflags & flag##_) dst->c_lflag |= (flag); \
    } while (0)
#ifdef INLCR
    MAP_IN(INLCR);
#endif
#ifdef IGNCR
    MAP_IN(IGNCR);
#endif
#ifdef ICRNL
    MAP_IN(ICRNL);
#endif
#ifdef IXON
    MAP_IN(IXON);
#endif
#ifdef OPOST
    MAP_OUT(OPOST);
#endif
#ifdef ONLCR
    MAP_OUT(ONLCR);
#endif
#ifdef OCRNL
    MAP_OUT(OCRNL);
#endif
#ifdef ONOCR
    MAP_OUT(ONOCR);
#endif
#ifdef ONLRET
    MAP_OUT(ONLRET);
#endif
#ifdef ISIG
    MAP_L(ISIG);
#endif
#ifdef ICANON
    MAP_L(ICANON);
#endif
#ifdef ECHO
    MAP_L(ECHO);
#endif
#ifdef ECHOE
    MAP_L(ECHOE);
#endif
#ifdef ECHOK
    MAP_L(ECHOK);
#endif
#ifdef ECHOKE
    MAP_L(ECHOKE);
#endif
#ifdef NOFLSH
    MAP_L(NOFLSH);
#endif
#ifdef ECHOCTL
    MAP_L(ECHOCTL);
#endif
#ifdef IEXTEN
    MAP_L(IEXTEN);
#endif
#undef MAP_IN
#undef MAP_OUT
#undef MAP_L

#define MAP_CC(idx, name) \
    do { \
        if (name < (int)(sizeof(dst->c_cc) / sizeof(dst->c_cc[0]))) { \
            dst->c_cc[name] = (cc_t)src->cc[idx]; \
        } \
    } while (0)
#ifdef VINTR
    MAP_CC(VINTR_, VINTR);
#endif
#ifdef VQUIT
    MAP_CC(VQUIT_, VQUIT);
#endif
#ifdef VERASE
    MAP_CC(VERASE_, VERASE);
#endif
#ifdef VKILL
    MAP_CC(VKILL_, VKILL);
#endif
#ifdef VEOF
    MAP_CC(VEOF_, VEOF);
#endif
#ifdef VTIME
    MAP_CC(VTIME_, VTIME);
#endif
#ifdef VMIN
    MAP_CC(VMIN_, VMIN);
#endif
#ifdef VSTART
    MAP_CC(VSTART_, VSTART);
#endif
#ifdef VSTOP
    MAP_CC(VSTOP_, VSTOP);
#endif
#ifdef VSUSP
    MAP_CC(VSUSP_, VSUSP);
#endif
#ifdef VEOL
    MAP_CC(VEOL_, VEOL);
#endif
#ifdef VREPRINT
    MAP_CC(VREPRINT_, VREPRINT);
#endif
#ifdef VDISCARD
    MAP_CC(VDISCARD_, VDISCARD);
#endif
#ifdef VWERASE
    MAP_CC(VWERASE_, VWERASE);
#endif
#ifdef VLNEXT
    MAP_CC(VLNEXT_, VLNEXT);
#endif
#ifdef VEOL2
    MAP_CC(VEOL2_, VEOL2);
#endif
#undef MAP_CC
}

static void vprocWinsizeFromHost(const struct winsize *src, struct winsize_ *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->row = (word_t)src->ws_row;
    dst->col = (word_t)src->ws_col;
    dst->xpixel = (word_t)src->ws_xpixel;
    dst->ypixel = (word_t)src->ws_ypixel;
}

static void vprocWinsizeToHost(const struct winsize_ *src, struct winsize *dst) {
    if (!dst) {
        return;
    }
    memset(dst, 0, sizeof(*dst));
    if (!src) {
        return;
    }
    dst->ws_row = (unsigned short)src->row;
    dst->ws_col = (unsigned short)src->col;
    dst->ws_xpixel = (unsigned short)src->xpixel;
    dst->ws_ypixel = (unsigned short)src->ypixel;
}

static int vprocIoctlTranslate(unsigned long cmd) {
    switch (cmd) {
#ifdef TIOCGWINSZ
        case TIOCGWINSZ:
            return TIOCGWINSZ_;
#endif
#ifdef TIOCSWINSZ
        case TIOCSWINSZ:
            return TIOCSWINSZ_;
#endif
#ifdef TIOCGETA
        case TIOCGETA:
            return TCGETS_;
#endif
#ifdef TIOCSETA
        case TIOCSETA:
            return TCSETS_;
#endif
#ifdef TIOCSETAW
        case TIOCSETAW:
            return TCSETSW_;
#endif
#ifdef TIOCSETAF
        case TIOCSETAF:
            return TCSETSF_;
#endif
#ifdef TCGETS
        case TCGETS:
            return TCGETS_;
#endif
#ifdef TCSETS
        case TCSETS:
            return TCSETS_;
#endif
#ifdef TCSETSW
        case TCSETSW:
            return TCSETSW_;
#endif
#ifdef TCSETSF
        case TCSETSF:
            return TCSETSF_;
#endif
#ifdef TIOCGPGRP
        case TIOCGPGRP:
            return TIOCGPGRP_;
#endif
#ifdef TIOCSPGRP
        case TIOCSPGRP:
            return TIOCSPGRP_;
#endif
#ifdef TIOCSCTTY
        case TIOCSCTTY:
            return TIOCSCTTY_;
#endif
#ifdef TIOCGPTN
        case TIOCGPTN:
            return TIOCGPTN_;
#endif
#ifdef TIOCGPTPEER
        case TIOCGPTPEER:
            return TIOCGPTPEER_;
#endif
#ifdef TIOCSPTLCK
        case TIOCSPTLCK:
            return TIOCSPTLCK_;
#endif
#ifdef TIOCPKT
        case TIOCPKT:
            return TIOCPKT_;
#endif
#ifdef TIOCGPKT
        case TIOCGPKT:
            return TIOCGPKT_;
#endif
#ifdef TCFLSH
        case TCFLSH:
            return TCFLSH_;
#endif
#ifdef FIONREAD
        case FIONREAD:
            return FIONREAD_;
#endif
        default:
            return (int)cmd;
    }
}

static struct pscal_fd *vprocSessionPscalFdForStd(int fd) {
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return NULL;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session || vprocSessionStdioIsDefault(session)) {
        return NULL;
    }
    struct pscal_fd *candidate = NULL;
    if (fd == STDIN_FILENO) {
        candidate = session->stdin_pscal_fd ? session->stdin_pscal_fd : session->pty_slave;
    } else if (fd == STDOUT_FILENO) {
        candidate = session->stdout_pscal_fd ? session->stdout_pscal_fd : session->pty_slave;
    } else if (fd == STDERR_FILENO) {
        candidate = session->stderr_pscal_fd ? session->stderr_pscal_fd : session->pty_slave;
    }
    if (!candidate) {
        return NULL;
    }
    return pscal_fd_retain(candidate);
}

bool vprocSessionStdioFetchTermios(int fd, struct termios *termios) {
    if (!termios) {
        return false;
    }
    struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(fd);
    if (!pscal_fd) {
        return false;
    }
    int res = _ENOTTY;
    if (pscal_fd->ops && pscal_fd->ops->ioctl) {
        struct termios_ termios_val;
        res = pscal_fd->ops->ioctl(pscal_fd, TCGETS_, &termios_val);
        if (res == 0) {
            vprocTermiosToHost(&termios_val, termios);
        }
    }
    pscal_fd_close(pscal_fd);
    return res == 0;
}

bool vprocSessionStdioApplyTermios(int fd, int action, const struct termios *termios) {
    if (!termios) {
        return false;
    }
    int cmd = 0;
    switch (action) {
        case TCSANOW:
            cmd = TCSETS_;
            break;
        case TCSADRAIN:
            cmd = TCSETSW_;
            break;
        case TCSAFLUSH:
            cmd = TCSETSF_;
            break;
        default:
            cmd = TCSETS_;
            break;
    }
    struct pscal_fd *pscal_fd = vprocSessionPscalFdForStd(fd);
    if (!pscal_fd) {
        return false;
    }
    int res = _ENOTTY;
    if (pscal_fd->ops && pscal_fd->ops->ioctl) {
        struct termios_ termios_val;
        vprocTermiosFromHost(termios, &termios_val);
        res = pscal_fd->ops->ioctl(pscal_fd, cmd, &termios_val);
    }
    pscal_fd_close(pscal_fd);
    return res == 0;
}

int vprocIoctlShim(int fd, unsigned long request, ...) {
    vprocDeliverPendingSignalsForCurrent();
    uintptr_t arg_val = 0;
    va_list ap;
    va_start(ap, request);
    arg_val = va_arg(ap, uintptr_t);
    va_end(ap);

    VProc *vp = vprocForThread();
    struct pscal_fd *pscal_fd = NULL;
    if (vp) {
        pscal_fd = vprocGetPscalFd(vp, fd);
    }
    if (!pscal_fd) {
        pscal_fd = vprocSessionPscalFdForStd(fd);
    }
    if (pscal_fd) {
        int cmd = vprocIoctlTranslate(request);
        if (cmd == TIOCGPTPEER_) {
            int res = _ENOTTY;
            if (vp && pscal_fd->tty && pscalPtyIsMaster(pscal_fd)) {
                int flags = (int)arg_val;
                if (flags == 0) {
                    flags = O_RDWR;
                }
                struct pscal_fd *peer = NULL;
                int err = pscalPtyOpenSlave(pscal_fd->tty->num, flags, &peer);
                if (err < 0) {
                    res = err;
                } else {
                    int slot = vprocInsertPscalFd(vp, peer);
                    /* fd table retains; drop our reference. */
                    pscal_fd_close(peer);
                    res = (slot < 0) ? _EMFILE : slot;
                }
            }
            pscal_fd_close(pscal_fd);
            if (res < 0) {
                return vprocSetCompatErrno(res);
            }
            return res;
        }
        int res = _ENOTTY;
        if (pscal_fd->ops && pscal_fd->ops->ioctl) {
            switch (cmd) {
                case TCGETS_: {
                    if (!arg_val) {
                        res = _EINVAL;
                        break;
                    }
                    struct termios_ termios;
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &termios);
                    if (res == 0) {
                        vprocTermiosToHost(&termios, (struct termios *)arg_val);
                    }
                    break;
                }
                case TCSETS_:
                case TCSETSW_:
                case TCSETSF_: {
                    if (!arg_val) {
                        res = _EINVAL;
                        break;
                    }
                    struct termios_ termios;
                    vprocTermiosFromHost((const struct termios *)arg_val, &termios);
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &termios);
                    break;
                }
                case TIOCGWINSZ_: {
                    if (!arg_val) {
                        res = _EINVAL;
                        break;
                    }
                    struct winsize_ ws;
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &ws);
                    if (res == 0) {
                        vprocWinsizeToHost(&ws, (struct winsize *)arg_val);
                    }
                    break;
                }
                case TIOCSWINSZ_: {
                    if (!arg_val) {
                        res = _EINVAL;
                        break;
                    }
                    struct winsize_ ws;
                    vprocWinsizeFromHost((const struct winsize *)arg_val, &ws);
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, &ws);
                    break;
                }
                default:
                    res = pscal_fd->ops->ioctl(pscal_fd, cmd, (void *)arg_val);
                    break;
            }
        }
        pscal_fd_close(pscal_fd);
        if (res < 0) {
            return vprocSetCompatErrno(res);
        }
        return res;
    }

    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostIoctlRaw(host, request, (void *)arg_val);
}

off_t vprocLseekShim(int fd, off_t offset, int whence) {
    int host = shimTranslate(fd, 1);
    if (host < 0) {
        return -1;
    }
    return vprocHostLseek(host, offset, whence);
}

static short vprocPollMapReady(int pscal_events, short requested) {
    short ready = 0;
    if (pscal_events & POLL_READ) ready |= POLLIN;
    if (pscal_events & POLL_WRITE) ready |= POLLOUT;
    if (pscal_events & POLL_PRI) ready |= POLLPRI;
    if (pscal_events & POLL_ERR) ready |= POLLERR;
    if (pscal_events & POLL_HUP) ready |= POLLHUP;
    if (pscal_events & POLL_NVAL) ready |= POLLNVAL;
    short mask = (short)(requested | POLLERR | POLLHUP | POLLNVAL | POLLPRI);
    return (short)(ready & mask);
}

#define VPROC_POLL_STACK_FDS 64
int vprocPollShim(struct pollfd *fds, nfds_t nfds, int timeout) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostPollRaw(fds, nfds, timeout);
    }
    if (!fds || nfds == 0) {
        return vprocHostPollRaw(fds, nfds, timeout);
    }

    struct pscal_fd *pscal_fds_stack[VPROC_POLL_STACK_FDS];
    int host_index_stack[VPROC_POLL_STACK_FDS + 1];
    struct pollfd host_fds_stack[VPROC_POLL_STACK_FDS + 1];
    bool use_stack = nfds <= VPROC_POLL_STACK_FDS;
    bool use_heap = false;

    struct pscal_fd **pscal_fds = NULL;
    int *host_index = NULL;
    struct pollfd *host_fds = NULL;
    if (use_stack) {
        memset(pscal_fds_stack, 0, sizeof(pscal_fds_stack));
        memset(host_index_stack, 0, sizeof(host_index_stack));
        memset(host_fds_stack, 0, sizeof(host_fds_stack));
        pscal_fds = pscal_fds_stack;
        host_index = host_index_stack;
        host_fds = host_fds_stack;
    } else {
        pscal_fds = calloc(nfds, sizeof(*pscal_fds));
        host_index = calloc(nfds + 1, sizeof(*host_index));
        host_fds = calloc(nfds + 1, sizeof(*host_fds));
        if (!pscal_fds || !host_index || !host_fds) {
            free(pscal_fds);
            free(host_index);
            free(host_fds);
            errno = ENOMEM;
            return -1;
        }
        use_heap = true;
    }

    int ready_count = 0;
    int pscal_ready_initial = 0;
    bool has_pscal = false;
    int host_count = 0;
    for (nfds_t i = 0; i < nfds; ++i) {
        fds[i].revents = 0;
        if (fds[i].fd < 0) {
            continue;
        }
        struct pscal_fd *pscal_fd = vprocGetPscalFd(vp, fds[i].fd);
        if (pscal_fd) {
            has_pscal = true;
            pscal_fds[i] = pscal_fd;
            int pscal_events = pscal_fd->ops && pscal_fd->ops->poll
                ? pscal_fd->ops->poll(pscal_fd)
                : POLL_ERR;
            short ready = vprocPollMapReady(pscal_events, fds[i].events);
            if (ready) {
                fds[i].revents = ready;
                ready_count++;
                pscal_ready_initial++;
            }
            continue;
        }

        int host_fd = vprocTranslateFd(vp, fds[i].fd);
        if (host_fd < 0) {
            struct stat st;
            if (vprocHostFstatRaw(fds[i].fd, &st) == 0) {
                host_fd = fds[i].fd;
            }
        }
        if (host_fd < 0) {
            fds[i].revents = POLLNVAL;
            ready_count++;
            continue;
        }
        host_fds[host_count] = fds[i];
        host_fds[host_count].fd = host_fd;
        host_index[host_count] = (int)i;
        host_count++;
    }

    int host_ready = 0;
    bool recheck_pscal = false;
    if (host_count > 0 || has_pscal) {
        int poll_timeout = (ready_count > 0) ? 0 : timeout;
        int wake_fd = -1;
        if (has_pscal) {
            wake_fd = pscalPollWakeFd();
            if (wake_fd >= 0) {
                host_fds[host_count].fd = wake_fd;
                host_fds[host_count].events = POLLIN;
                host_fds[host_count].revents = 0;
                host_index[host_count] = -1;
                host_count++;
            }
        }

        if (host_count > 0) {
            host_ready = vprocHostPollRaw(host_fds, (nfds_t)host_count, poll_timeout);
            if (host_ready < 0 && ready_count == 0) {
                for (nfds_t i = 0; i < nfds; ++i) {
                    if (pscal_fds[i]) {
                        pscal_fd_close(pscal_fds[i]);
                    }
                }
                if (use_heap) {
                    free(pscal_fds);
                    free(host_index);
                    free(host_fds);
                }
                return -1;
            }
            if (wake_fd >= 0 && (host_fds[host_count - 1].revents & POLLIN)) {
                pscalPollDrain();
                recheck_pscal = true;
            }
        }
    }

    if (host_ready > 0) {
        for (int j = 0; j < host_count; ++j) {
            int orig = host_index[j];
            if (orig < 0) {
                continue;
            }
            if (host_fds[j].revents) {
                fds[orig].revents = host_fds[j].revents;
                ready_count++;
            }
        }
    }

    if (has_pscal && (recheck_pscal || ready_count == 0)) {
        int host_ready_count = ready_count - pscal_ready_initial;
        if (host_ready_count < 0) {
            host_ready_count = 0;
        }
        int pscal_ready = 0;
        for (nfds_t i = 0; i < nfds; ++i) {
            if (!pscal_fds[i]) {
                continue;
            }
            int pscal_events = pscal_fds[i]->ops && pscal_fds[i]->ops->poll
                ? pscal_fds[i]->ops->poll(pscal_fds[i])
                : POLL_ERR;
            short ready = vprocPollMapReady(pscal_events, fds[i].events);
            fds[i].revents = ready;
            if (ready) {
                pscal_ready++;
            }
        }
        ready_count = host_ready_count + pscal_ready;
    }

    for (nfds_t i = 0; i < nfds; ++i) {
        if (pscal_fds[i]) {
            pscal_fd_close(pscal_fds[i]);
        }
    }
    if (use_heap) {
        free(pscal_fds);
        free(host_index);
        free(host_fds);
    }
    return ready_count;
}

#define VPROC_SELECT_STACK_FDS 128
int vprocSelectShim(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds, struct timeval *timeout) {
    vprocDeliverPendingSignalsForCurrent();
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostSelectRaw(nfds, readfds, writefds, exceptfds, timeout);
    }
    if (nfds <= 0) {
        return vprocHostSelectRaw(nfds, readfds, writefds, exceptfds, timeout);
    }

    int count = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (readfds && FD_ISSET(fd, readfds)) events |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds)) events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
        if (events != 0) {
            count++;
        }
    }

    int timeout_ms = -1;
    if (timeout) {
        timeout_ms = (int)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000);
    }

    if (count == 0) {
        int res = vprocHostPollRaw(NULL, 0, timeout_ms);
        if (res < 0) {
            return -1;
        }
        if (readfds) FD_ZERO(readfds);
        if (writefds) FD_ZERO(writefds);
        if (exceptfds) FD_ZERO(exceptfds);
        return 0;
    }

    struct pollfd pfds_stack[VPROC_SELECT_STACK_FDS];
    int fd_map_stack[VPROC_SELECT_STACK_FDS];
    bool use_stack = count <= VPROC_SELECT_STACK_FDS;
    bool use_heap = false;
    struct pollfd *pfds = NULL;
    int *fd_map = NULL;
    if (use_stack) {
        memset(pfds_stack, 0, sizeof(pfds_stack));
        memset(fd_map_stack, 0, sizeof(fd_map_stack));
        pfds = pfds_stack;
        fd_map = fd_map_stack;
    } else {
        pfds = calloc((size_t)count, sizeof(*pfds));
        fd_map = calloc((size_t)count, sizeof(*fd_map));
        if (!pfds || !fd_map) {
            free(pfds);
            free(fd_map);
            errno = ENOMEM;
            return -1;
        }
        use_heap = true;
    }

    int cursor = 0;
    for (int fd = 0; fd < nfds; ++fd) {
        short events = 0;
        if (readfds && FD_ISSET(fd, readfds)) events |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds)) events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds)) events |= POLLPRI;
        if (events == 0) {
            continue;
        }
        pfds[cursor].fd = fd;
        pfds[cursor].events = events;
        pfds[cursor].revents = 0;
        fd_map[cursor] = fd;
        cursor++;
    }
    int res = vprocPollShim(pfds, (nfds_t)count, timeout_ms);
    if (res < 0) {
        if (use_heap) {
            free(pfds);
            free(fd_map);
        }
        return -1;
    }

    if (readfds) FD_ZERO(readfds);
    if (writefds) FD_ZERO(writefds);
    if (exceptfds) FD_ZERO(exceptfds);
    int ready = 0;
    for (int i = 0; i < count; ++i) {
        short revents = pfds[i].revents;
        int fd = fd_map[i];
        bool fd_ready = false;
        if (revents & POLLIN) {
            if (readfds) FD_SET(fd, readfds);
            fd_ready = true;
        }
        if (revents & POLLOUT) {
            if (writefds) FD_SET(fd, writefds);
            fd_ready = true;
        }
        if (revents & POLLPRI) {
            if (exceptfds) FD_SET(fd, exceptfds);
            fd_ready = true;
        }
        if (revents & (POLLERR | POLLHUP | POLLNVAL)) {
            if (exceptfds) FD_SET(fd, exceptfds);
            fd_ready = true;
        }
        if (fd_ready) {
            ready++;
        }
    }

    if (use_heap) {
        free(pfds);
        free(fd_map);
    }
    return ready;
}

int vprocOpenShim(const char *path, int flags, ...) {
    vprocDeliverPendingSignalsForCurrent();
    int mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, int);
        va_end(ap);
    }
    VProc *vp = vprocForThread();
    if (!vp) {
        return vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
    }
    if (vprocPathIsSystemPath(path)) {
        int host_fd = vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
        if (host_fd < 0) {
            return -1;
        }
        int slot = vprocInsert(vp, host_fd);
        if (slot < 0) {
            vprocHostClose(host_fd);
        }
        return slot;
    }
    if (vprocPathIsDevTty(path)) {
        struct pscal_fd *tty_fd = NULL;
        int err = pscalTtyOpenControlling(flags, &tty_fd);
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (err == _ENXIO || err == _ENOTTY) {
            /* If this vproc never claimed a controlling TTY, bind the session
             * PTY as the controlling terminal for the current sid and retry. */
            int sid = vprocGetsidShim(0);
            if (session && session->pty_slave && session->pty_slave->tty && sid > 0) {
                struct tty *tty = session->pty_slave->tty;
                lock(&tty->lock);
                if (tty->session == 0 || tty->session == (pid_t_)sid) {
                    if (tty->session == 0) {
                        tty->session = (pid_t_)sid;
                        int fg = vprocGetForegroundPgid(sid);
                        tty->fg_group = (fg > 0) ? (pid_t_)fg : (pid_t_)sid;
                    }
                    unlock(&tty->lock);
                    pscalTtySetControlling(tty);
                    err = pscalTtyOpenControlling(flags, &tty_fd);
                } else {
                    unlock(&tty->lock);
                }
            }
        }
        if (err < 0) {
            /* Fall back to the session PTY if present so /dev/tty resolves for
             * pipeline stages that otherwise lack a controlling terminal. */
            if (session && session->pty_slave) {
                struct pscal_fd *retained = pscal_fd_retain(session->pty_slave);
                if (retained) {
                    int slot = vprocInsertPscalFd(vp, retained);
                    /* Table retains; release caller ref. */
                    pscal_fd_close(retained);
                    if (slot >= 0) {
                        return slot;
                    }
                }
            }
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(tty_fd);
            errno = ENOTSUP;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, tty_fd);
        /* fd table retains; drop caller ref regardless of outcome. */
        pscal_fd_close(tty_fd);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    if (vprocPathIsDevConsole(path)) {
        struct pscal_fd *tty_fd = NULL;
        int err = pscalTtyOpenControlling(flags, &tty_fd);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(tty_fd);
            errno = ENOTSUP;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, tty_fd);
        /* fd table retains; release caller ref. */
        pscal_fd_close(tty_fd);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    int tty_num = -1;
    if (vprocPathParseConsoleTty(path, &tty_num)) {
        if (tty_num == 1) {
            struct pscal_fd *tty_fd = NULL;
            int err = pscalTtyOpenControlling(flags, &tty_fd);
            if (err < 0) {
                return vprocSetCompatErrno(err);
            }
            if (!vp) {
                pscal_fd_close(tty_fd);
                errno = ENOTSUP;
                return -1;
            }
            int slot = vprocInsertPscalFd(vp, tty_fd);
            /* fd table retains; release caller ref. */
            pscal_fd_close(tty_fd);
            if (slot < 0) {
                return -1;
            }
            return slot;
        }
        const char *fallback = "/dev/null";
        if (!vp) {
            return vprocHostOpenVirtualized(fallback, flags, mode);
        }
        int host_fd = vprocHostOpenVirtualized(fallback, flags, mode);
        if (host_fd < 0) {
            return -1;
        }
        int slot = vprocInsert(vp, host_fd);
        if (slot < 0) {
            vprocHostClose(host_fd);
        }
        return slot;
    }
    if (vprocPathIsPtyMaster(path)) {
        struct pscal_fd *pty = NULL;
        int pty_num = -1;
        int err = pscalPtyOpenMaster(flags, &pty, &pty_num);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(pty);
            errno = ENOTSUP;
            return -1;
        }
        VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
        uint64_t session_id = (session_stdio && !vprocSessionStdioIsDefault(session_stdio))
                ? session_stdio->session_id
                : 0;
        struct pscal_fd *session_slave = NULL;
        if (session_id != 0) {
            int slave_flags = O_RDWR;
            if (flags & O_NONBLOCK) {
                slave_flags |= O_NONBLOCK;
            }
            int slave_err = pscalPtyOpenSlave(pty_num, slave_flags, &session_slave);
            if (slave_err < 0) {
                session_slave = NULL;
            }
        }
        int slot = vprocInsertPscalFd(vp, pty);
        if (slot < 0) {
            if (session_slave) {
                pscal_fd_close(session_slave);
            }
            /* drop caller ref */
            pscal_fd_close(pty);
            return -1;
        }
        if (session_id != 0 && session_slave) {
            /* register retains both ends */
            vprocSessionPtyRegister(session_id, session_slave, pty);
        }
        if (session_slave) {
            pscal_fd_close(session_slave);
        }
        /* fd table (and session registry) retain; drop caller ref last */
        pscal_fd_close(pty);
        return slot;
    }
    int pty_num = -1;
    if (vprocPathParsePtySlave(path, &pty_num)) {
        struct pscal_fd *pty = NULL;
        int err = pscalPtyOpenSlave(pty_num, flags, &pty);
        if (err < 0) {
            return vprocSetCompatErrno(err);
        }
        if (!vp) {
            pscal_fd_close(pty);
            errno = ENOTSUP;
            return -1;
        }
        int slot = vprocInsertPscalFd(vp, pty);
        /* fd table retains; release caller ref. */
        pscal_fd_close(pty);
        if (slot < 0) {
            return -1;
        }
        return slot;
    }
    if (vprocPathIsLocationDevice(path)) {
        return vprocLocationDeviceOpen(vp, flags);
    }
    if (!vp) {
        /* Apply path virtualization even when vproc is inactive. */
        return vprocHostOpenVirtualized(path, flags, mode);
    }
    bool dbg = vprocPipeDebugEnabled();
    int host_fd = vprocHostOpenVirtualized(path, flags, mode);
#if defined(PSCAL_TARGET_IOS)
    if (host_fd < 0 && errno == ENOENT) {
        if (dbg) vprocDebugLogf( "[vproc-open] (shim) virtualized ENOENT for %s, fallback raw\n", path);
        host_fd = vprocHostOpenRawInternal(path, flags, (mode_t)mode, (flags & O_CREAT) != 0);
    }
    if (dbg && host_fd >= 0) {
        vprocDebugLogf( "[vproc-open] (shim) opened %s -> host_fd=%d flags=0x%x\n", path, host_fd, flags);
    }
#endif
    if (host_fd < 0) {
        if (vprocToolDebugEnabled()) {
            vprocDebugLogf( "[vproc-open] path=%s flags=%d errno=%d\n", path, flags, errno);
        }
        return -1;
    }
    int slot = vprocInsert(vp, host_fd);
    if (slot < 0) {
        vprocHostClose(host_fd);
    }
    return slot;
}

int vprocSigactionShim(int sig, const struct sigaction *act, struct sigaction *oldact) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigactionRaw(sig, act, oldact);
    }
    int rc = vprocSigaction(vprocPid(vp), sig, act, oldact);
#if defined(PSCAL_TARGET_IOS)
    /* Resize notifications from the runtime use pthread_kill(SIGWINCH) on the
     * host thread. Mirror SIGWINCH handlers into host sigaction so those
     * notifications can wake OpenSSH clientloop even when signal handling is
     * virtualized through vproc. */
    if (rc == 0 && act && sig == SIGWINCH) {
        (void)vprocHostSigactionRaw(sig, act, NULL);
    }
#endif
    return rc;
}

int vprocSigprocmaskShim(int how, const sigset_t *set, sigset_t *oldset) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigprocmaskRaw(how, set, oldset);
    }
    return vprocSigprocmask(vprocPid(vp), how, set, oldset);
}

int vprocSigpendingShim(sigset_t *set) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigpendingRaw(set);
    }
    return vprocSigpending(vprocPid(vp), set);
}

int vprocSigsuspendShim(const sigset_t *mask) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostSigsuspendRaw(mask);
    }
    return vprocSigsuspend(vprocPid(vp), mask);
}

int vprocPthreadSigmaskShim(int how, const sigset_t *set, sigset_t *oldset) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostPthreadSigmaskRaw(how, set, oldset);
    }
    if (vprocSigprocmask(vprocPid(vp), how, set, oldset) == 0) {
        return 0;
    }
    return errno ? errno : EINVAL;
}

int vprocRaiseShim(int sig) {
    VProc *vp = vprocCurrent();
    if (!vp) {
        return vprocHostRaiseRaw(sig);
    }
    return vprocKillShim(vprocPid(vp), sig);
}

VProcSigHandler vprocSignalShim(int sig, VProcSigHandler handler) {
    struct sigaction sa;
    struct sigaction old;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_handler = handler;
    if (vprocSigactionShim(sig, &sa, &old) != 0) {
        return SIG_ERR;
    }
    return old.sa_handler;
}

#endif /* PSCAL_TARGET_IOS || VPROC_ENABLE_STUBS_FOR_TESTS */
