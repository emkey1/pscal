#include "common/path_truncate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/un.h>
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <dlfcn.h>
#if defined(__APPLE__)
#include <net/if_dl.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <mach-o/dyld.h>
#if !TARGET_OS_IPHONE
#include <libproc.h>
#endif
#endif
#include <time.h>
#include <pthread.h>
#include "vm/vm.h"

static char g_pathTruncatePrimary[PATH_MAX];
static size_t g_pathTruncatePrimaryLen = 0;
static char g_pathTruncateAlias[PATH_MAX];
static size_t g_pathTruncateAliasLen = 0;
static _Thread_local int g_pathTruncateResolving = 0;
static pthread_mutex_t g_pathTruncateMutex = PTHREAD_MUTEX_INITIALIZER;
static time_t g_procBootTime = 0;
static char g_procBootId[37] = {0};
static uint64_t g_procRefreshLastFullMs = 0;
static uint64_t g_procRefreshLastNetMs = 0;
static uint64_t g_procRefreshLastDeviceMs = 0;
static uint64_t g_procRefreshLastVmMs = 0;
static uint64_t g_procRefreshLastPruneMs = 0;
static uint64_t g_procRefreshLastDevicePruneMs = 0;
static bool g_procBaseSeeded = false;
static bool g_procPrunePending = false;
static bool g_procDevicePrunePending = false;

static inline void pathTruncateLock(void)   { pthread_mutex_lock(&g_pathTruncateMutex); }
static inline void pathTruncateUnlock(void) { pthread_mutex_unlock(&g_pathTruncateMutex); }
static void pathTruncateEnsureDir(const char *path);
static void pathTruncateProvisionUsrBin(const char *prefix);

typedef struct PathTruncateSmallclueApplet {
    const char *name;
    int (*entry)(int argc, char **argv);
    const char *description;
} PathTruncateSmallclueApplet;

#if defined(__APPLE__)
extern const PathTruncateSmallclueApplet *smallclueGetApplets(size_t *count) __attribute__((weak_import));
#else
extern const PathTruncateSmallclueApplet *smallclueGetApplets(size_t *count) __attribute__((weak));
#endif

static uint64_t pathTruncateMonotonicMs(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ull) + ((uint64_t)ts.tv_nsec / 1000000ull);
}

static bool pathTruncateAtomicWriteBytes(const char *path, const void *data, size_t len) {
    if (!path || path[0] == '\0') {
        return false;
    }
    if (len > 0 && !data) {
        return false;
    }

    char tmp_path[PATH_MAX];
    uintptr_t tid = (uintptr_t)pthread_self();
    int tmp_n = snprintf(tmp_path,
                         sizeof(tmp_path),
                         "%s.tmp.%d.%llx.XXXXXX",
                         path,
                         (int)getpid(),
                         (unsigned long long)tid);
    if (tmp_n <= 0 || (size_t)tmp_n >= sizeof(tmp_path)) {
        return false;
    }

    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        return false;
    }
    (void)fchmod(fd, 0644);

    const unsigned char *bytes = (const unsigned char *)data;
    size_t total_written = 0;
    while (total_written < len) {
        ssize_t written = write(fd, bytes + total_written, len - total_written);
        if (written <= 0) {
            close(fd);
            unlink(tmp_path);
            return false;
        }
        total_written += (size_t)written;
    }

    if (close(fd) != 0) {
        unlink(tmp_path);
        return false;
    }
    if (rename(tmp_path, path) != 0) {
        unlink(tmp_path);
        return false;
    }
    return true;
}

static void write_text_file(const char *path, const char *contents) {
    if (!path || !contents) {
        return;
    }
    size_t len = strlen(contents);
    if (pathTruncateAtomicWriteBytes(path, contents, len)) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fputs(contents, f);
    fclose(f);
}

static bool pathTruncateProcPrefixMatch(const char *path, const char *prefix) {
    if (!path || !prefix) {
        return false;
    }
    size_t len = strlen(prefix);
    if (len == 0) {
        return false;
    }
    return strncmp(path, prefix, len) == 0 &&
           (path[len] == '\0' || path[len] == '/');
}

static bool pathTruncateIsProcRequestPath(const char *path) {
    return pathTruncateProcPrefixMatch(path, "/proc") ||
           pathTruncateProcPrefixMatch(path, "/private/proc");
}

static void pathTruncateProcStripContainerPrefix(const char *prefix,
                                                 const char *input,
                                                 char *out,
                                                 size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!input || input[0] == '\0') {
        snprintf(out, out_size, "/");
        return;
    }
    if (!prefix || prefix[0] != '/') {
        snprintf(out, out_size, "%s", input);
        return;
    }

    const char *match_prefix = NULL;
    size_t match_len = 0;
    size_t prefix_len = strlen(prefix);
    if (pathTruncateProcPrefixMatch(input, prefix)) {
        match_prefix = prefix;
        match_len = prefix_len;
    } else {
        const char *private_prefix = "/private";
        size_t private_len = strlen(private_prefix);
        if (strncmp(prefix, private_prefix, private_len) == 0) {
            const char *trimmed = prefix + private_len;
            if (pathTruncateProcPrefixMatch(input, trimmed)) {
                match_prefix = trimmed;
                match_len = strlen(trimmed);
            }
        } else if (pathTruncateProcPrefixMatch(input, "/private")) {
            char prefixed[PATH_MAX];
            if (snprintf(prefixed, sizeof(prefixed), "/private%s", prefix) > 0 &&
                pathTruncateProcPrefixMatch(input, prefixed)) {
                match_prefix = prefixed;
                match_len = strlen(prefixed);
            }
        }
    }

    if (!match_prefix) {
        snprintf(out, out_size, "%s", input);
        return;
    }

    const char *remainder = input + match_len;
    while (*remainder == '/') {
        remainder++;
    }
    if (*remainder == '\0') {
        snprintf(out, out_size, "/");
        return;
    }
    snprintf(out, out_size, "/%s", remainder);
}

static void pathTruncateEnsureSymlink(const char *link_path, const char *target) {
    if (!link_path || !target || target[0] == '\0') {
        return;
    }
    char existing[PATH_MAX];
    ssize_t n = readlink(link_path, existing, sizeof(existing) - 1);
    if (n >= 0) {
        existing[n] = '\0';
        if (strcmp(existing, target) == 0) {
            return;
        }
    }
    char temp_path[PATH_MAX];
    uintptr_t tid = (uintptr_t)pthread_self();
    if (snprintf(temp_path, sizeof(temp_path), "%s.tmp.%d.%llx",
                 link_path,
                 (int)getpid(),
                 (unsigned long long)tid) <= 0) {
        return;
    }
    unlink(temp_path);
    if (symlink(target, temp_path) != 0) {
        return;
    }
    if (rename(temp_path, link_path) != 0) {
        unlink(temp_path);
    }
}

static bool pathTruncatePathIsUsrBinTree(const char *path) {
    static const char *kUsrBin = "/usr/bin";
    if (!path || path[0] != '/') {
        return false;
    }
    size_t len = strlen(kUsrBin);
    if (strncmp(path, kUsrBin, len) != 0) {
        return false;
    }
    return path[len] == '\0' || path[len] == '/';
}

static void pathTruncateProvisionUsrBinLink(const char *usr_bin_dir,
                                            const char *name,
                                            const char *target) {
    if (!usr_bin_dir || !name || !target || name[0] == '\0' || target[0] == '\0') {
        return;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strchr(name, '/')) {
        return;
    }
    char link_path[PATH_MAX];
    int written = snprintf(link_path, sizeof(link_path), "%s/%s", usr_bin_dir, name);
    if (written <= 0 || (size_t)written >= sizeof(link_path)) {
        return;
    }
    pathTruncateEnsureSymlink(link_path, target);
}

typedef struct PathTruncateFrontendAlias {
    const char *name;
    const char *target;
} PathTruncateFrontendAlias;

static const PathTruncateFrontendAlias kPathTruncateFrontendAliases[] = {
    {"pascal", "/bin/pscal_tool_runner"},
    {"clike", "/bin/pscal_tool_runner"},
    {"rea", "/bin/pscal_tool_runner"},
    {"pscalvm", "/bin/pscal_tool_runner"},
    {"pscaljson2bc", "/bin/pscal_tool_runner"},
#ifdef BUILD_DASCAL
    {"dascal", "/bin/pscal_tool_runner"},
#endif
#ifdef BUILD_PSCALD
    {"pscald", "/bin/pscal_tool_runner"},
    {"pscalasm", "/bin/pscal_tool_runner"},
#endif
#if defined(PSCAL_TARGET_IOS)
    {"ssh", "/bin/pscal_tool_runner"},
    {"scp", "/bin/pscal_tool_runner"},
    {"sftp", "/bin/pscal_tool_runner"},
#endif
    {"exsh", "/bin/exsh"},
    {"sh", "/bin/exsh"},
    {"smallclue", "/bin/exsh"},
};

static bool pathTruncateUsrBinIsFrontendAlias(const char *name) {
    if (!name || !*name) {
        return false;
    }
    for (size_t i = 0; i < sizeof(kPathTruncateFrontendAliases) / sizeof(kPathTruncateFrontendAliases[0]); ++i) {
        if (strcmp(kPathTruncateFrontendAliases[i].name, name) == 0) {
            return true;
        }
    }
    return false;
}

static bool pathTruncateVirtualBinHasName(const char *prefix, const char *name) {
    if (!prefix || prefix[0] != '/' || !name || name[0] == '\0') {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strchr(name, '/')) {
        return false;
    }
    char path[PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/bin/%s", prefix, name);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return false;
    }
    struct stat st;
    return lstat(path, &st) == 0;
}

static void pathTruncateProvisionUsrBinFromBinDirectory(const char *prefix, const char *usr_bin_dir) {
    if (!prefix || prefix[0] != '/' || !usr_bin_dir || usr_bin_dir[0] == '\0') {
        return;
    }
    char host_bin[PATH_MAX];
    int written = snprintf(host_bin, sizeof(host_bin), "%s/bin", prefix);
    if (written <= 0 || (size_t)written >= sizeof(host_bin)) {
        return;
    }
    DIR *dir = opendir(host_bin);
    if (!dir) {
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        if (!name || name[0] == '\0') {
            continue;
        }
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strchr(name, '/')) {
            continue;
        }
        char target[PATH_MAX];
        int target_written = snprintf(target, sizeof(target), "/bin/%s", name);
        if (target_written <= 0 || (size_t)target_written >= sizeof(target)) {
            continue;
        }
        pathTruncateProvisionUsrBinLink(usr_bin_dir, name, target);
    }
    closedir(dir);
}

static void pathTruncateProvisionUsrBin(const char *prefix) {
    if (!prefix || prefix[0] != '/') {
        return;
    }
    char usr_dir[PATH_MAX];
    int written = snprintf(usr_dir, sizeof(usr_dir), "%s/usr", prefix);
    if (written <= 0 || (size_t)written >= sizeof(usr_dir)) {
        return;
    }
    pathTruncateEnsureDir(usr_dir);

    char usr_bin_dir[PATH_MAX];
    written = snprintf(usr_bin_dir, sizeof(usr_bin_dir), "%s/usr/bin", prefix);
    if (written <= 0 || (size_t)written >= sizeof(usr_bin_dir)) {
        return;
    }
    pathTruncateEnsureDir(usr_bin_dir);

    pathTruncateProvisionUsrBinFromBinDirectory(prefix, usr_bin_dir);

    for (size_t i = 0; i < sizeof(kPathTruncateFrontendAliases) / sizeof(kPathTruncateFrontendAliases[0]); ++i) {
        const PathTruncateFrontendAlias *alias = &kPathTruncateFrontendAliases[i];
        if (pathTruncateVirtualBinHasName(prefix, alias->name)) {
            continue;
        }
        pathTruncateProvisionUsrBinLink(usr_bin_dir, alias->name, alias->target);
    }

    if (smallclueGetApplets) {
        size_t applet_count = 0;
        const PathTruncateSmallclueApplet *applets = smallclueGetApplets(&applet_count);
        for (size_t i = 0; applets && i < applet_count; ++i) {
            const char *name = applets[i].name;
            if (!name || name[0] == '\0') {
                continue;
            }
            if (pathTruncateUsrBinIsFrontendAlias(name)) {
                continue;
            }
            if (pathTruncateVirtualBinHasName(prefix, name)) {
                continue;
            }
            pathTruncateProvisionUsrBinLink(usr_bin_dir, name, "/bin/exsh");
        }
    }
}

static bool pathTruncateParseNumericName(const char *name, long *out_value) {
    if (!name || !name[0] || !out_value) {
        return false;
    }
    for (const char *cursor = name; *cursor; ++cursor) {
        if (!isdigit((unsigned char)*cursor)) {
            return false;
        }
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(name, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 0) {
        return false;
    }
    *out_value = value;
    return true;
}

static void pathTruncatePruneNumericDirectoryChildren(const char *dir_path,
                                                      const bool *keep,
                                                      long keep_count) {
    if (!dir_path || dir_path[0] == '\0' || !keep || keep_count <= 0) {
        return;
    }
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        long index = -1;
        if (!pathTruncateParseNumericName(entry->d_name, &index)) {
            continue;
        }
        if (index >= 0 && index < keep_count && keep[index]) {
            continue;
        }
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name) <= 0) {
            continue;
        }
        unlink(child);
    }
    closedir(dir);
}

static void pathTruncateRemoveTree(const char *path) {
    if (!path || path[0] == '\0') {
        return;
    }
    struct stat st;
    if (lstat(path, &st) != 0) {
        return;
    }
    if (S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode)) {
        DIR *dir = opendir(path);
        if (dir) {
            struct dirent *entry = NULL;
            while ((entry = readdir(dir)) != NULL) {
                if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                char child[PATH_MAX];
                if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) <= 0) {
                    continue;
                }
                pathTruncateRemoveTree(child);
            }
            closedir(dir);
        }
        rmdir(path);
        return;
    }
    unlink(path);
}

static bool pathTruncatePidInList(const int *pids, size_t count, int pid) {
    if (!pids || count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (pids[i] == pid) {
            return true;
        }
    }
    return false;
}

static bool pathTruncatePruneNumericDirectoryChildrenByPidList(const char *dir_path,
                                                               const int *keep_pids,
                                                               size_t keep_count,
                                                               size_t max_remove) {
    if (!dir_path || dir_path[0] == '\0') {
        return false;
    }
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return false;
    }
    size_t removed = 0;
    bool more_candidates = false;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (max_remove > 0 && removed >= max_remove) {
            more_candidates = true;
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        long pid_long = -1;
        if (!pathTruncateParseNumericName(entry->d_name, &pid_long)) {
            continue;
        }
        if (pid_long <= 0 || pid_long > INT32_MAX) {
            continue;
        }
        int pid = (int)pid_long;
        if (pathTruncatePidInList(keep_pids, keep_count, pid)) {
            continue;
        }
        char child[PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dir_path, entry->d_name) <= 0) {
            continue;
        }
        pathTruncateRemoveTree(child);
        removed++;
    }
    closedir(dir);
    return more_candidates;
}

static void pathTruncateWriteBinaryFile(const char *path, const void *data, size_t len) {
    if (!path || !data) {
        return;
    }
    if (pathTruncateAtomicWriteBytes(path, data, len)) {
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    (void)fwrite(data, 1, len, f);
    fclose(f);
}

static void pathTruncateWriteProcEnviron(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        return;
    }
    extern char **environ;
    if (environ) {
        for (char **cursor = environ; *cursor; ++cursor) {
            const char *entry = *cursor;
            size_t len = strlen(entry);
            if (len > 0) {
                (void)fwrite(entry, 1, len, f);
            }
            fputc('\0', f);
        }
    }
    fclose(f);
}

static void pathTruncateWriteProcLimits(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Limit                     Soft Limit           Hard Limit           Units\n");
    struct {
        int resource;
        const char *name;
        const char *units;
    } resources[] = {
        { RLIMIT_CPU, "Max cpu time", "seconds" },
        { RLIMIT_FSIZE, "Max file size", "bytes" },
        { RLIMIT_DATA, "Max data size", "bytes" },
        { RLIMIT_STACK, "Max stack size", "bytes" },
        { RLIMIT_CORE, "Max core file size", "bytes" },
        { RLIMIT_RSS, "Max resident set", "bytes" },
        { RLIMIT_NOFILE, "Max open files", "files" },
        { RLIMIT_AS, "Max address space", "bytes" },
    };
    for (size_t i = 0; i < sizeof(resources) / sizeof(resources[0]); ++i) {
        struct rlimit lim;
        if (getrlimit(resources[i].resource, &lim) != 0) {
            continue;
        }
        char soft[32];
        char hard[32];
        if (lim.rlim_cur == RLIM_INFINITY) {
            snprintf(soft, sizeof(soft), "unlimited");
        } else {
            snprintf(soft, sizeof(soft), "%llu", (unsigned long long)lim.rlim_cur);
        }
        if (lim.rlim_max == RLIM_INFINITY) {
            snprintf(hard, sizeof(hard), "unlimited");
        } else {
            snprintf(hard, sizeof(hard), "%llu", (unsigned long long)lim.rlim_max);
        }
        fprintf(f, "%-25s %-20s %-20s %s\n", resources[i].name, soft, hard, resources[i].units);
    }
    fclose(f);
}

typedef struct {
    char name[IFNAMSIZ];
    uint64_t rx_bytes;
    uint64_t rx_packets;
    uint64_t rx_errors;
    uint64_t rx_drop;
    uint64_t tx_bytes;
    uint64_t tx_packets;
    uint64_t tx_errors;
    uint64_t tx_drop;
    bool used;
} ProcNetDevRow;

typedef struct {
    unsigned total;
    unsigned unix_stream;
    unsigned unix_dgram;
    unsigned tcp4;
    unsigned tcp6;
    unsigned udp4;
    unsigned udp6;
    unsigned raw4;
    unsigned raw6;
} ProcSocketStats;

static unsigned pathTruncateCountPrefixBits(const unsigned char *bytes, size_t len) {
    if (!bytes || len == 0) {
        return 0;
    }
    unsigned count = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char value = bytes[i];
        if (value == 0xffu) {
            count += 8;
            continue;
        }
        for (int bit = 7; bit >= 0; --bit) {
            if ((value & (unsigned char)(1u << bit)) != 0) {
                count++;
            } else {
                return count;
            }
        }
    }
    return count;
}

static unsigned pathTruncateNetmaskPrefixLength(const struct sockaddr *netmask) {
    if (!netmask) {
        return 0;
    }
    if (netmask->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)netmask;
        return pathTruncateCountPrefixBits((const unsigned char *)&sin->sin_addr, sizeof(sin->sin_addr));
    }
    if (netmask->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)netmask;
        return pathTruncateCountPrefixBits((const unsigned char *)&sin6->sin6_addr, sizeof(sin6->sin6_addr));
    }
    return 0;
}

static void pathTruncateCollectSocketStats(ProcSocketStats *stats) {
    if (!stats) {
        return;
    }
    memset(stats, 0, sizeof(*stats));

    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max <= 0 || open_max > 1024) {
        open_max = 256;
    }
    for (long fd = 0; fd < open_max; ++fd) {
        if (fcntl((int)fd, F_GETFD) < 0) {
            continue;
        }
        struct sockaddr_storage local;
        socklen_t llen = sizeof(local);
        if (getsockname((int)fd, (struct sockaddr *)&local, &llen) != 0) {
            continue;
        }
        int type = 0;
        socklen_t type_len = sizeof(type);
        if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0) {
            continue;
        }
        stats->total++;
        if (local.ss_family == AF_INET) {
            if (type == SOCK_STREAM) {
                stats->tcp4++;
            } else if (type == SOCK_DGRAM) {
                stats->udp4++;
            } else if (type == SOCK_RAW) {
                stats->raw4++;
            }
        } else if (local.ss_family == AF_INET6) {
            if (type == SOCK_STREAM) {
                stats->tcp6++;
            } else if (type == SOCK_DGRAM) {
                stats->udp6++;
            } else if (type == SOCK_RAW) {
                stats->raw6++;
            }
        } else if (local.ss_family == AF_UNIX) {
            if (type == SOCK_STREAM) {
                stats->unix_stream++;
            } else if (type == SOCK_DGRAM) {
                stats->unix_dgram++;
            }
        }
    }
}

static int pathTruncateFindOrAddDevRow(ProcNetDevRow *rows, size_t max_rows, const char *name) {
    if (!rows || !name || name[0] == '\0') {
        return -1;
    }
    for (size_t i = 0; i < max_rows; ++i) {
        if (rows[i].used && strcmp(rows[i].name, name) == 0) {
            return (int)i;
        }
    }
    for (size_t i = 0; i < max_rows; ++i) {
        if (!rows[i].used) {
            rows[i].used = true;
            snprintf(rows[i].name, sizeof(rows[i].name), "%s", name);
            return (int)i;
        }
    }
    return -1;
}

static void pathTruncateWriteProcNetDev(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f,
            "Inter-|   Receive                                                |  Transmit\n"
            " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");

    ProcNetDevRow rows[128];
    memset(rows, 0, sizeof(rows));

    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || ifa->ifa_name[0] == '\0') {
                continue;
            }
            int idx = pathTruncateFindOrAddDevRow(rows, sizeof(rows) / sizeof(rows[0]), ifa->ifa_name);
            if (idx < 0) {
                continue;
            }
#if defined(__APPLE__)
            if (ifa->ifa_addr && ifa->ifa_addr->sa_family == AF_LINK && ifa->ifa_data) {
                const struct if_data *d = (const struct if_data *)ifa->ifa_data;
                rows[idx].rx_bytes = (uint64_t)d->ifi_ibytes;
                rows[idx].rx_packets = (uint64_t)d->ifi_ipackets;
                rows[idx].rx_errors = (uint64_t)d->ifi_ierrors;
                rows[idx].rx_drop = 0;
                rows[idx].tx_bytes = (uint64_t)d->ifi_obytes;
                rows[idx].tx_packets = (uint64_t)d->ifi_opackets;
                rows[idx].tx_errors = (uint64_t)d->ifi_oerrors;
                rows[idx].tx_drop = 0;
            }
#else
            (void)ifa;
#endif
        }
        freeifaddrs(ifaddr);
    } else {
        int idx = pathTruncateFindOrAddDevRow(rows, sizeof(rows) / sizeof(rows[0]), "lo");
        if (idx >= 0) {
            rows[idx].used = true;
        }
    }

    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        if (!rows[i].used) {
            continue;
        }
        const char *name = rows[i].name;
        if (strcmp(name, "lo0") == 0) {
            name = "lo";
        }
        fprintf(f,
                "%6s: %-8llu %-7llu %-4llu %-4llu 0    0     0          0 "
                "%-8llu %-7llu %-4llu %-4llu 0    0     0       0\n",
                name,
                (unsigned long long)rows[i].rx_bytes,
                (unsigned long long)rows[i].rx_packets,
                (unsigned long long)rows[i].rx_errors,
                (unsigned long long)rows[i].rx_drop,
                (unsigned long long)rows[i].tx_bytes,
                (unsigned long long)rows[i].tx_packets,
                (unsigned long long)rows[i].tx_errors,
                (unsigned long long)rows[i].tx_drop);
    }
    fclose(f);
}

static void pathTruncateWriteProcNetRoute(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Iface\tDestination\tGateway\tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");

    struct ifaddrs *ifaddr = NULL;
    bool wrote_any = false;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr || !ifa->ifa_netmask) {
                continue;
            }
            if (ifa->ifa_addr->sa_family != AF_INET || ifa->ifa_netmask->sa_family != AF_INET) {
                continue;
            }
            const struct sockaddr_in *sin_addr = (const struct sockaddr_in *)ifa->ifa_addr;
            const struct sockaddr_in *sin_mask = (const struct sockaddr_in *)ifa->ifa_netmask;
            uint32_t addr = ntohl(sin_addr->sin_addr.s_addr);
            uint32_t mask = ntohl(sin_mask->sin_addr.s_addr);
            uint32_t dest = addr & mask;
            unsigned flags = 0x0001;
            if (ifa->ifa_flags & IFF_LOOPBACK) {
                flags |= 0x0004;
            }
            const char *ifname = strcmp(ifa->ifa_name, "lo0") == 0 ? "lo" : ifa->ifa_name;
            fprintf(f,
                    "%s\t%08X\t%08X\t%04X\t0\t0\t0\t%08X\t0\t0\t0\n",
                    ifname,
                    dest,
                    0u,
                    flags,
                    mask);
            wrote_any = true;
        }
        freeifaddrs(ifaddr);
    }
    if (!wrote_any) {
        fprintf(f, "lo\t0000007F\t00000000\t0001\t0\t0\t0\t000000FF\t0\t0\t0\n");
    }
    fclose(f);
}

static void pathTruncateWriteProcNetArp(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path, "IP address       HW type     Flags       HW address            Mask     Device\n");
}

static void pathTruncateWriteProcNetIfInet6(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    struct ifaddrs *ifaddr = NULL;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || !ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET6) {
                continue;
            }
            const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)ifa->ifa_addr;
            char addr_hex[33];
            for (int i = 0; i < 16; ++i) {
                snprintf(addr_hex + (i * 2), 3, "%02x", sin6->sin6_addr.s6_addr[i]);
            }
            addr_hex[32] = '\0';

            unsigned ifindex = if_nametoindex(ifa->ifa_name);
            unsigned prefix_len = pathTruncateNetmaskPrefixLength(ifa->ifa_netmask);
            unsigned scope = 0x00;
            if (IN6_IS_ADDR_LOOPBACK(&sin6->sin6_addr)) {
                scope = 0x80;
            } else if (IN6_IS_ADDR_LINKLOCAL(&sin6->sin6_addr)) {
                scope = 0x20;
            } else if (IN6_IS_ADDR_SITELOCAL(&sin6->sin6_addr)) {
                scope = 0x40;
            }
            const unsigned flags = 0x80;
            const char *ifname = strcmp(ifa->ifa_name, "lo0") == 0 ? "lo" : ifa->ifa_name;
            fprintf(f,
                    "%s %02x %02x %02x %02x %s\n",
                    addr_hex,
                    (unsigned)(ifindex & 0xffu),
                    (unsigned)(prefix_len & 0xffu),
                    scope,
                    flags,
                    ifname);
        }
        freeifaddrs(ifaddr);
    }
    fclose(f);
}

static void pathTruncateWriteProcNetInet(const char *path, int sock_type, bool ipv6) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");

    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max <= 0 || open_max > 1024) {
        open_max = 256;
    }
    uid_t uid = getuid();
    int sl = 0;
    for (long fd = 0; fd < open_max; ++fd) {
        if (fcntl((int)fd, F_GETFD) < 0) {
            continue;
        }
        struct sockaddr_storage local;
        socklen_t llen = sizeof(local);
        if (getsockname((int)fd, (struct sockaddr *)&local, &llen) != 0) {
            continue;
        }

        int family = local.ss_family;
        if ((ipv6 && family != AF_INET6) || (!ipv6 && family != AF_INET)) {
            continue;
        }

        int type = 0;
        socklen_t type_len = sizeof(type);
        if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0 || type != sock_type) {
            continue;
        }

        struct sockaddr_storage remote;
        socklen_t rlen = sizeof(remote);
        bool connected = (getpeername((int)fd, (struct sockaddr *)&remote, &rlen) == 0);
        int state = 0x07;
        if (sock_type == SOCK_STREAM) {
            if (connected) {
                state = 0x01;
            } else {
                int accepting = 0;
                socklen_t accepting_len = sizeof(accepting);
                if (getsockopt((int)fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &accepting_len) == 0 && accepting) {
                    state = 0x0A;
                }
            }
        }

        struct stat st;
        unsigned long long inode = 0;
        if (fstat((int)fd, &st) == 0) {
            inode = (unsigned long long)st.st_ino;
        }

        if (!ipv6) {
            const struct sockaddr_in *sin_local = (const struct sockaddr_in *)&local;
            uint32_t local_addr = ntohl(sin_local->sin_addr.s_addr);
            unsigned local_port = ntohs(sin_local->sin_port);
            uint32_t remote_addr = 0;
            unsigned remote_port = 0;
            if (connected && remote.ss_family == AF_INET) {
                const struct sockaddr_in *sin_remote = (const struct sockaddr_in *)&remote;
                remote_addr = ntohl(sin_remote->sin_addr.s_addr);
                remote_port = ntohs(sin_remote->sin_port);
            }
            fprintf(f,
                    "%4d: %08X:%04X %08X:%04X %02X 00000000:00000000 00:00000000 00000000 %5u        0 %llu 1 0000000000000000 100 0 0 10 0\n",
                    sl++,
                    local_addr,
                    local_port,
                    remote_addr,
                    remote_port,
                    state,
                    (unsigned)uid,
                    inode);
        } else {
            const struct sockaddr_in6 *sin_local = (const struct sockaddr_in6 *)&local;
            char local_hex[33];
            memset(local_hex, '0', sizeof(local_hex));
            local_hex[32] = '\0';
            for (int i = 0; i < 16; ++i) {
                snprintf(local_hex + (i * 2), 3, "%02X", sin_local->sin6_addr.s6_addr[i]);
            }
            unsigned local_port = ntohs(sin_local->sin6_port);

            char remote_hex[33];
            memset(remote_hex, '0', sizeof(remote_hex));
            remote_hex[32] = '\0';
            unsigned remote_port = 0;
            if (connected && remote.ss_family == AF_INET6) {
                const struct sockaddr_in6 *sin_remote = (const struct sockaddr_in6 *)&remote;
                for (int i = 0; i < 16; ++i) {
                    snprintf(remote_hex + (i * 2), 3, "%02X", sin_remote->sin6_addr.s6_addr[i]);
                }
                remote_port = ntohs(sin_remote->sin6_port);
            }
            fprintf(f,
                    "%4d: %s:%04X %s:%04X %02X 00000000:00000000 00:00000000 00000000 %5u        0 %llu 1 0000000000000000 100 0 0 10 0\n",
                    sl++,
                    local_hex,
                    local_port,
                    remote_hex,
                    remote_port,
                    state,
                    (unsigned)uid,
                    inode);
        }
    }
    fclose(f);
}

static void pathTruncateWriteProcNetSockstat(const char *path, bool ipv6_only) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    ProcSocketStats stats;
    pathTruncateCollectSocketStats(&stats);
    if (!ipv6_only) {
        unsigned tcp = stats.tcp4 + stats.tcp6;
        unsigned udp = stats.udp4 + stats.udp6;
        unsigned raw = stats.raw4 + stats.raw6;
        unsigned unix_total = stats.unix_stream + stats.unix_dgram;
        fprintf(f, "sockets: used %u\n", stats.total);
        fprintf(f, "TCP: inuse %u orphan 0 tw 0 alloc %u mem 0\n", tcp, tcp);
        fprintf(f, "UDP: inuse %u mem 0\n", udp);
        fprintf(f, "UDPLITE: inuse 0\n");
        fprintf(f, "RAW: inuse %u\n", raw);
        fprintf(f, "FRAG: inuse 0 memory 0\n");
        fprintf(f, "UNIX: inuse %u\n", unix_total);
    } else {
        fprintf(f, "TCP6: inuse %u\n", stats.tcp6);
        fprintf(f, "UDP6: inuse %u\n", stats.udp6);
        fprintf(f, "UDPLITE6: inuse 0\n");
        fprintf(f, "RAW6: inuse %u\n", stats.raw6);
        fprintf(f, "FRAG6: inuse 0 memory 0\n");
    }
    fclose(f);
}

static void pathTruncateWriteProcNetSnmp(const char *path) {
    if (!path) {
        return;
    }
    ProcSocketStats stats;
    pathTruncateCollectSocketStats(&stats);
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Ip: Forwarding DefaultTTL InReceives InDelivers OutRequests OutDiscards OutNoRoutes\n");
    fprintf(f, "Ip: 1 64 0 0 0 0 0\n");
    fprintf(f, "Icmp: InMsgs InErrors OutMsgs OutErrors\n");
    fprintf(f, "Icmp: 0 0 0 0\n");
    fprintf(f, "Tcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts\n");
    fprintf(f, "Tcp: 1 200 120000 -1 0 0 0 0 %u 0 0 0 0 0\n", stats.tcp4 + stats.tcp6);
    fprintf(f, "Udp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors\n");
    fprintf(f, "Udp: 0 0 0 0 0 0\n");
    fprintf(f, "UdpLite: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors SndbufErrors\n");
    fprintf(f, "UdpLite: 0 0 0 0 0 0\n");
    fclose(f);
}

static void pathTruncateWriteProcNetSnmp6(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "Ip6InReceives 0\n"
                    "Ip6InHdrErrors 0\n"
                    "Ip6InAddrErrors 0\n"
                    "Ip6InDiscards 0\n"
                    "Ip6OutRequests 0\n"
                    "Ip6OutDiscards 0\n"
                    "Icmp6InMsgs 0\n"
                    "Icmp6OutMsgs 0\n"
                    "Udp6InDatagrams 0\n"
                    "Udp6OutDatagrams 0\n");
}

static void pathTruncateWriteProcNetNetstat(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "TcpExt: SyncookiesSent SyncookiesRecv SyncookiesFailed EmbryonicRsts PruneCalled RcvPruned OfoPruned OutOfWindowIcmps LockDroppedIcmps\n"
                    "TcpExt: 0 0 0 0 0 0 0 0 0 0\n"
                    "IpExt: InNoRoutes InTruncatedPkts InMcastPkts OutMcastPkts InBcastPkts OutBcastPkts\n"
                    "IpExt: 0 0 0 0 0 0\n");
}

static void pathTruncateWriteProcNetPacket(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path, "sk               RefCnt Type Proto  Iface R Rmem   User   Inode\n");
}

static void pathTruncateWriteProcNetProtocols(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f,
            "protocol  size sockets  memory press maxhdr  slab module     cl co di ac io in de sh ss gs se re sp bi br ha uh gp em\n");
    fprintf(f,
            "TCP       1352      0       0   no     0      0 kernel      yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes\n");
    fprintf(f,
            "UDP       1152      0       0   no     0      0 kernel      yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes\n");
    fprintf(f,
            "RAW       1024      0       0   no     0      0 kernel      yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes\n");
    fprintf(f,
            "UNIX      1088      0       0   no     0      0 kernel      yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes yes\n");
    fclose(f);
}

static void pathTruncateWriteProcNetWireless(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n");
    fprintf(f, " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n");

    struct ifaddrs *ifaddr = NULL;
    bool wrote_any = false;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || ifa->ifa_name[0] == '\0') {
                continue;
            }
            const char *ifname = strcmp(ifa->ifa_name, "lo0") == 0 ? "lo" : ifa->ifa_name;
            fprintf(f,
                    "%6s: 0000   0.    0.    0.        0      0      0      0      0        0\n",
                    ifname);
            wrote_any = true;
        }
        freeifaddrs(ifaddr);
    }
    if (!wrote_any) {
        fprintf(f, "%6s: 0000   0.    0.    0.        0      0      0      0      0        0\n", "lo");
    }
    fclose(f);
}

static void pathTruncateWriteProcNetSoftnetStat(const char *path, int ncpu) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    if (ncpu <= 0) {
        ncpu = 1;
    }
    for (int i = 0; i < ncpu; ++i) {
        fprintf(f, "00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000 00000000\n");
    }
    fclose(f);
}

static void pathTruncateWriteProcNetDevMcast(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    struct ifaddrs *ifaddr = NULL;
    bool wrote_any = false;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || ifa->ifa_name[0] == '\0') {
                continue;
            }
            unsigned ifindex = if_nametoindex(ifa->ifa_name);
            if (ifindex == 0) {
                continue;
            }
            const char *ifname = strcmp(ifa->ifa_name, "lo0") == 0 ? "lo" : ifa->ifa_name;
            /* all-nodes IPv6 multicast group ff02::1 */
            fprintf(f, "%4u %-15s %5u %5u %s\n",
                    ifindex,
                    ifname,
                    1u,
                    0u,
                    "333300000001");
            wrote_any = true;
        }
        freeifaddrs(ifaddr);
    }
    if (!wrote_any) {
        fprintf(f, "%4u %-15s %5u %5u %s\n", 1u, "lo", 1u, 0u, "333300000001");
    }
    fclose(f);
}

static void pathTruncateWriteProcNetIgmp(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Idx\tDevice    : Count Querier\tGroup    Users Timer\tReporter\n");
    struct ifaddrs *ifaddr = NULL;
    bool wrote_any = false;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || ifa->ifa_name[0] == '\0') {
                continue;
            }
            unsigned ifindex = if_nametoindex(ifa->ifa_name);
            if (ifindex == 0) {
                continue;
            }
            const char *ifname = strcmp(ifa->ifa_name, "lo0") == 0 ? "lo" : ifa->ifa_name;
            fprintf(f, "%u\t%-10s: %5u %-8s\n", ifindex, ifname, 1u, "V3");
            /* 224.0.0.1 in hex in little-endian proc formatting. */
            fprintf(f, "\t\t\t\t010000E0 %5u 0:00000000\t\t0\n", 1u);
            wrote_any = true;
        }
        freeifaddrs(ifaddr);
    }
    if (!wrote_any) {
        fprintf(f, "%u\t%-10s: %5u %-8s\n", 1u, "lo", 1u, "V3");
        fprintf(f, "\t\t\t\t010000E0 %5u 0:00000000\t\t0\n", 1u);
    }
    fclose(f);
}

static void pathTruncateWriteProcNetIgmp6(const char *path) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    struct ifaddrs *ifaddr = NULL;
    bool wrote_any = false;
    if (getifaddrs(&ifaddr) == 0 && ifaddr) {
        for (struct ifaddrs *ifa = ifaddr; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_name || ifa->ifa_name[0] == '\0') {
                continue;
            }
            unsigned ifindex = if_nametoindex(ifa->ifa_name);
            if (ifindex == 0) {
                continue;
            }
            const char *ifname = strcmp(ifa->ifa_name, "lo0") == 0 ? "lo" : ifa->ifa_name;
            fprintf(f, "%u %-8s %s %5u %08x %u\n",
                    ifindex,
                    ifname,
                    "ff020000000000000000000000000001",
                    1u,
                    0x00000004u,
                    0u);
            wrote_any = true;
        }
        freeifaddrs(ifaddr);
    }
    if (!wrote_any) {
        fprintf(f, "%u %-8s %s %5u %08x %u\n",
                1u,
                "lo",
                "ff020000000000000000000000000001",
                1u,
                0x00000004u,
                0u);
    }
    fclose(f);
}

static void pathTruncateWriteProcNetIpv6Route(const char *path) {
    if (!path) {
        return;
    }
    /* Minimal synthetic default/loopback-like route rows. */
    write_text_file(path,
                    "00000000000000000000000000000000 00 "
                    "00000000000000000000000000000000 00 "
                    "00000000000000000000000000000000 "
                    "00000000 00000000 00000000 00000001 lo\n"
                    "00000000000000000000000000000001 80 "
                    "00000000000000000000000000000000 00 "
                    "00000000000000000000000000000000 "
                    "00000000 00000000 00000000 00000001 lo\n");
}

static void pathTruncateWriteProcNetRt6Stats(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path, "0001 0001 0001 0001 0000 0000 0000\n");
}

static void pathTruncateWriteProcNetFibTrie(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "Main:\n"
                    "  +-- 0.0.0.0/0 3 0 5\n"
                    "     +-- 127.0.0.0/8 2 0 2\n"
                    "        |-- 127.0.0.0\n"
                    "           /8 host LOCAL\n"
                    "        |-- 127.0.0.1\n"
                    "           /32 host LOCAL\n"
                    "Local:\n"
                    "  +-- 127.0.0.0/8 2 0 2\n"
                    "     |-- 127.0.0.1\n"
                    "        /32 host LOCAL\n");
}

static void pathTruncateWriteProcNetFibTrieStat(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "Basic info: size 1 depth 2 leaves 2 prefixes 2\n"
                    "Counters: gets 0 backtracks 0 semantic_match_passed 0 semantic_match_miss 0\n");
}

static void pathTruncateWriteProcNetNetlink(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "sk               Eth Pid        Groups   Rmem     Wmem     Dump  Locks    Drops    Inode\n"
                    "0000000000000000 0   0          00000000 0        0        0     0        0        0\n");
}

static void pathTruncateWriteProcNetPtype(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "Type Device      Function\n"
                    "0800 lo          ip_rcv\n"
                    "86dd lo          ipv6_rcv\n");
}

static void pathTruncateWriteProcNetPsched(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path, "000003e8 00000040 000f4240 3b9aca00\n");
}

static void pathTruncateWriteProcNetXfrmStat(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "XfrmInError              0\n"
                    "XfrmInBufferError        0\n"
                    "XfrmInHdrError           0\n"
                    "XfrmInNoStates           0\n"
                    "XfrmOutError             0\n");
}

static void pathTruncateWriteProcNetStatTable(const char *path, const char *header, const char *row) {
    if (!path || !header || !row) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "%s\n", header);
    fprintf(f, "%s\n", row);
    fclose(f);
}

static void pathTruncateWriteProcBuddyinfo(const char *path, int ncpu) {
    (void)ncpu;
    if (!path) {
        return;
    }
    write_text_file(path,
                    "Node 0, zone      DMA      1      1      1      1      1      1      1      1      1      1      1\n"
                    "Node 0, zone   Normal    128     64     32     16      8      4      2      1      1      1      1\n");
}

static void pathTruncateWriteProcZoneinfo(const char *path, uint64_t mem_total_kb) {
    if (!path) {
        return;
    }
    unsigned long long managed = (unsigned long long)(mem_total_kb / 4ull);
    unsigned long long present = (unsigned long long)(mem_total_kb / 4ull);
    char buf[2048];
    snprintf(buf,
             sizeof(buf),
             "Node 0, zone      DMA\n"
             "  pages free     16\n"
             "        min      4\n"
             "        low      8\n"
             "        high     12\n"
             "        managed  64\n"
             "Node 0, zone   Normal\n"
             "  pages free     %llu\n"
             "        min      %llu\n"
             "        low      %llu\n"
             "        high     %llu\n"
             "        present  %llu\n"
             "        managed  %llu\n",
             managed / 8ull,
             managed / 64ull,
             managed / 48ull,
             managed / 32ull,
             present,
             managed);
    write_text_file(path, buf);
}

static void pathTruncateWriteProcPagetypeinfo(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "Page block order: 9\n"
                    "Pages per block:  512\n"
                    "\n"
                    "Free pages count per migrate type at order       0      1      2      3      4      5\n"
                    "Node    0, zone   Normal, type    Unmovable    16      8      4      2      1      0\n"
                    "Node    0, zone   Normal, type      Movable    32     16      8      4      2      1\n");
}

static void pathTruncateWriteProcSlabinfo(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "slabinfo - version: 2.1\n"
                    "# name            <active_objs> <num_objs> <objsize> <objperslab> <pagesperslab>\n"
                    "kmalloc-64               64         64        64           64              1\n"
                    "kmalloc-128              32         32       128           32              1\n");
}

static void pathTruncateWriteProcPartitions(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "major minor  #blocks  name\n"
                    "\n"
                    "   1        0   1048576 vda\n");
}

static void pathTruncateWriteProcLocks(const char *path) {
    if (!path) {
        return;
    }
    write_text_file(path,
                    "1: POSIX  ADVISORY  WRITE 1 00:00:0 0 EOF\n");
}

static void pathTruncateWriteProcSysvipcTable(const char *path, const char *header) {
    if (!path || !header) {
        return;
    }
    write_text_file(path, header);
}

static void pathTruncateWriteProcNetUnix(const char *path, const char *prefix) {
    if (!path) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fprintf(f, "Num       RefCount Protocol Flags    Type St Inode Path\n");

    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max <= 0 || open_max > 1024) {
        open_max = 256;
    }
    unsigned long long row = 0;
    for (long fd = 0; fd < open_max; ++fd) {
        if (fcntl((int)fd, F_GETFD) < 0) {
            continue;
        }
        struct sockaddr_storage local;
        socklen_t llen = sizeof(local);
        if (getsockname((int)fd, (struct sockaddr *)&local, &llen) != 0 || local.ss_family != AF_UNIX) {
            continue;
        }
        int type = 0;
        socklen_t type_len = sizeof(type);
        if (getsockopt((int)fd, SOL_SOCKET, SO_TYPE, &type, &type_len) != 0) {
            continue;
        }
        struct stat st;
        unsigned long long inode = 0;
        if (fstat((int)fd, &st) == 0) {
            inode = (unsigned long long)st.st_ino;
        }

        const struct sockaddr_un *sun = (const struct sockaddr_un *)&local;
        char visible_path[PATH_MAX];
        visible_path[0] = '\0';
        if (sun->sun_path[0] != '\0') {
            pathTruncateProcStripContainerPrefix(prefix, sun->sun_path, visible_path, sizeof(visible_path));
        }

        fprintf(f,
                "%016llX: %08X %08X %04X %02X %llu %s\n",
                0x100000000ull + row++,
                1u,
                0u,
                (unsigned)(type & 0xffff),
                1u,
                inode,
                visible_path);
    }
    fclose(f);
}

static double pathTruncateTimespecToSeconds(struct timespec ts) {
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
}

static double pathTruncateThreadCpuSeconds(const ThreadMetrics *metrics) {
    if (!metrics || !metrics->start.valid || !metrics->end.valid) {
        return 0.0;
    }
    double start = pathTruncateTimespecToSeconds(metrics->start.cpuTime);
    double end = pathTruncateTimespecToSeconds(metrics->end.cpuTime);
    if (end < start) {
        return 0.0;
    }
    return end - start;
}

static int pathTruncateCompareVmSnapshotByAddress(const void *lhs, const void *rhs) {
    const VMProcSnapshot *a = (const VMProcSnapshot *)lhs;
    const VMProcSnapshot *b = (const VMProcSnapshot *)rhs;
    if (a->vm_address < b->vm_address) {
        return -1;
    }
    if (a->vm_address > b->vm_address) {
        return 1;
    }
    return 0;
}

static void pathTruncatePruneProcVmDirs(const char *vm_by_addr_dir,
                                        const VMProcSnapshot *snapshots,
                                        size_t snapshot_count) {
    (void)vm_by_addr_dir;
    (void)snapshots;
    (void)snapshot_count;
    /* Intentionally keep historic /proc/vm/by-address entries so dynamic
     * refreshes never invalidate a caller's current working directory. */
}

static size_t pathTruncateSnapshotProcVmState(VMProcSnapshot *out, size_t capacity) {
    typedef size_t (*SnapshotFn)(VMProcSnapshot *, size_t);
    static SnapshotFn snapshot_fn = NULL;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        snapshot_fn = (SnapshotFn)dlsym(RTLD_DEFAULT, "vmSnapshotProcState");
    }
    if (!snapshot_fn) {
        return 0;
    }
    return snapshot_fn(out, capacity);
}

static size_t pathTruncateSnapshotProcVmWorkers(uintptr_t vm_address,
                                                VMProcWorkerSnapshot *out,
                                                size_t capacity) {
    typedef size_t (*WorkersFn)(uintptr_t, VMProcWorkerSnapshot *, size_t);
    static WorkersFn workers_fn = NULL;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        workers_fn = (WorkersFn)dlsym(RTLD_DEFAULT, "vmSnapshotProcWorkers");
    }
    if (!workers_fn) {
        return 0;
    }
    return workers_fn(vm_address, out, capacity);
}

typedef struct {
    int pid;
    pthread_t tid;
    int parent_pid;
    int pgid;
    int sid;
    bool exited;
    bool stopped;
    bool continued;
    bool zombie;
    int exit_signal;
    int status;
    int stop_signo;
    bool sigchld_pending;
    int rusage_utime;
    int rusage_stime;
    int fg_pgid;
    int job_id;
    char comm[16];
    char command[64];
} PathTruncateVProcSnapshot;

typedef struct {
    int pid;
    int ppid;
    char name[64];
} PathTruncateDeviceProcSnapshot;

static size_t pathTruncateSnapshotVProcState(PathTruncateVProcSnapshot *out, size_t capacity) {
    typedef size_t (*SnapshotFn)(PathTruncateVProcSnapshot *, size_t);
    static SnapshotFn snapshot_fn = NULL;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        snapshot_fn = (SnapshotFn)dlsym(RTLD_DEFAULT, "vprocSnapshot");
    }
    if (!snapshot_fn) {
        return 0;
    }
    return snapshot_fn(out, capacity);
}

static int pathTruncateCurrentVProcPid(void) {
    typedef pid_t (*GetPidFn)(void);
    static GetPidFn getpid_fn = NULL;
    static bool resolved = false;
    if (!resolved) {
        resolved = true;
        getpid_fn = (GetPidFn)dlsym(RTLD_DEFAULT, "vprocGetPidShim");
    }
    if (!getpid_fn) {
        return -1;
    }
    pid_t pid = getpid_fn();
    if (pid <= 0) {
        return -1;
    }
    return (int)pid;
}

static size_t pathTruncateSnapshotDeviceProcesses(PathTruncateDeviceProcSnapshot *out,
                                                  size_t capacity) {
    if (!out || capacity == 0) {
        return 0;
    }
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    int bytes = proc_listpids(PROC_ALL_PIDS, 0, NULL, 0);
    if (bytes <= 0) {
        return 0;
    }
    size_t pid_count = (size_t)bytes / sizeof(int);
    int *pid_list = (int *)calloc(pid_count, sizeof(int));
    if (!pid_list) {
        return 0;
    }
    bytes = proc_listpids(PROC_ALL_PIDS, 0, pid_list, (int)(pid_count * sizeof(int)));
    if (bytes <= 0) {
        free(pid_list);
        return 0;
    }
    pid_count = (size_t)bytes / sizeof(int);

    size_t out_count = 0;
    for (size_t i = 0; i < pid_count && out_count < capacity; ++i) {
        int pid = pid_list[i];
        if (pid <= 0) {
            continue;
        }
        struct proc_bsdinfo bsdinfo;
        int info_bytes = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsdinfo, (int)sizeof(bsdinfo));
        if (info_bytes <= 0) {
            continue;
        }
        PathTruncateDeviceProcSnapshot *entry = &out[out_count++];
        entry->pid = pid;
        entry->ppid = (int)bsdinfo.pbi_ppid;
        entry->name[0] = '\0';
        int name_len = proc_name(pid, entry->name, (uint32_t)sizeof(entry->name));
        if (name_len <= 0 || entry->name[0] == '\0') {
            snprintf(entry->name, sizeof(entry->name), "pid-%d", pid);
        }
    }
    free(pid_list);
    return out_count;
#else
    (void)out;
    (void)capacity;
    return 0;
#endif
}

static void pathTruncateWriteProcDevicePidEntry(const char *device_dir,
                                                pid_t pid,
                                                pid_t ppid,
                                                const char *name,
                                                uint64_t mem_total_kb,
                                                double uptime_secs) {
    if (!device_dir || !name || pid <= 0) {
        return;
    }
    char pid_dir[PATH_MAX];
    if (snprintf(pid_dir, sizeof(pid_dir), "%s/%d", device_dir, (int)pid) <= 0) {
        return;
    }
    pathTruncateEnsureDir(pid_dir);

    char pathbuf[PATH_MAX];
    if (snprintf(pathbuf, sizeof(pathbuf), "%s/comm", pid_dir) > 0) {
        char line[128];
        snprintf(line, sizeof(line), "%s\n", name);
        write_text_file(pathbuf, line);
    }
    if (snprintf(pathbuf, sizeof(pathbuf), "%s/cmdline", pid_dir) > 0) {
        char cmdline[128];
        size_t n = (size_t)snprintf(cmdline, sizeof(cmdline), "%s", name);
        if (n + 1 < sizeof(cmdline)) {
            cmdline[n] = '\0';
            pathTruncateWriteBinaryFile(pathbuf, cmdline, n + 1);
        } else {
            write_text_file(pathbuf, name);
        }
    }
    if (snprintf(pathbuf, sizeof(pathbuf), "%s/status", pid_dir) > 0) {
        FILE *f = fopen(pathbuf, "w");
        if (f) {
            uid_t uid = getuid();
            gid_t gid = getgid();
            fprintf(f, "Name:\t%s\n", name);
            fprintf(f, "State:\tR (running)\n");
            fprintf(f, "Tgid:\t%d\n", (int)pid);
            fprintf(f, "Pid:\t%d\n", (int)pid);
            fprintf(f, "PPid:\t%d\n", (int)ppid);
            fprintf(f, "Uid:\t%u\t%u\t%u\t%u\n", (unsigned)uid, (unsigned)uid, (unsigned)uid, (unsigned)uid);
            fprintf(f, "Gid:\t%u\t%u\t%u\t%u\n", (unsigned)gid, (unsigned)gid, (unsigned)gid, (unsigned)gid);
            fprintf(f, "Threads:\t1\n");
            fprintf(f, "VmSize:\t%llu kB\n", (unsigned long long)(mem_total_kb / 8ull));
            fprintf(f, "VmRSS:\t%llu kB\n", (unsigned long long)(mem_total_kb / 16ull));
            fclose(f);
        }
    }
    if (snprintf(pathbuf, sizeof(pathbuf), "%s/stat", pid_dir) > 0) {
        FILE *f = fopen(pathbuf, "w");
        if (f) {
            long hz = sysconf(_SC_CLK_TCK);
            if (hz <= 0) {
                hz = 100;
            }
            unsigned long long start_ticks = (unsigned long long)(uptime_secs * (double)hz * 0.1);
            unsigned long long utime = (unsigned long long)(uptime_secs * (double)hz * 0.01);
            unsigned long long stime = (unsigned long long)(uptime_secs * (double)hz * 0.005);
            unsigned long long vsize = (unsigned long long)(mem_total_kb * 1024ull / 8ull);
            long rss = (long)(mem_total_kb / 16ull);
            fprintf(f,
                    "%d (%s) R %d %d %d 0 -1 4194304 0 0 0 0 %llu %llu 0 0 20 0 1 0 %llu %llu %ld 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                    (int)pid,
                    name,
                    (int)ppid,
                    (int)pid,
                    (int)pid,
                    utime,
                    stime,
                    start_ticks,
                    vsize,
                    rss);
            fclose(f);
        }
    }
    if (snprintf(pathbuf, sizeof(pathbuf), "%s/io", pid_dir) > 0) {
        write_text_file(pathbuf,
                        "rchar: 0\n"
                        "wchar: 0\n"
                        "syscr: 0\n"
                        "syscw: 0\n"
                        "read_bytes: 0\n"
                        "write_bytes: 0\n"
                        "cancelled_write_bytes: 0\n");
    }
}

static void pathTruncateWriteProcVm(const char *procdir) {
    if (!procdir || procdir[0] == '\0') {
        return;
    }

    char vm_dir[PATH_MAX];
    if (snprintf(vm_dir, sizeof(vm_dir), "%s/vm", procdir) <= 0) {
        return;
    }
    pathTruncateEnsureDir(vm_dir);

    VMProcSnapshot snapshots[256];
    size_t snapshot_count = pathTruncateSnapshotProcVmState(
        snapshots,
        sizeof(snapshots) / sizeof(snapshots[0]));
    if (snapshot_count > 1) {
        qsort(snapshots, snapshot_count, sizeof(snapshots[0]), pathTruncateCompareVmSnapshotByAddress);
    }

    size_t root_count = 0;
    size_t total_worker_slots = 0;
    size_t max_stack_depth = 0;
    size_t max_frames = 0;
    size_t aborting = 0;
    size_t exiting = 0;
    size_t suspended = 0;
    for (size_t i = 0; i < snapshot_count; ++i) {
        const VMProcSnapshot *snapshot = &snapshots[i];
        if (snapshot->is_root_vm) {
            root_count++;
            if (snapshot->worker_count > 0) {
                total_worker_slots += (size_t)snapshot->worker_count;
            }
        }
        if (snapshot->stack_depth > max_stack_depth) {
            max_stack_depth = snapshot->stack_depth;
        }
        if ((size_t)snapshot->frame_count > max_frames) {
            max_frames = (size_t)snapshot->frame_count;
        }
        if (snapshot->abort_requested) {
            aborting++;
        }
        if (snapshot->exit_requested) {
            exiting++;
        }
        if (snapshot->suspend_unwind_requested) {
            suspended++;
        }
    }

    char summary_path[PATH_MAX];
    if (snprintf(summary_path, sizeof(summary_path), "%s/summary", vm_dir) > 0) {
        FILE *f = fopen(summary_path, "w");
        if (f) {
            fprintf(f, "vm_count %zu\n", snapshot_count);
            fprintf(f, "root_vm_count %zu\n", root_count);
            fprintf(f, "worker_slot_count %zu\n", total_worker_slots);
            fprintf(f, "max_stack_depth %zu\n", max_stack_depth);
            fprintf(f, "max_frame_count %zu\n", max_frames);
            fprintf(f, "abort_requested %zu\n", aborting);
            fprintf(f, "exit_requested %zu\n", exiting);
            fprintf(f, "suspend_unwind_requested %zu\n", suspended);
            fclose(f);
        }
    }

    char list_path[PATH_MAX];
    if (snprintf(list_path, sizeof(list_path), "%s/list", vm_dir) > 0) {
        FILE *f = fopen(list_path, "w");
        if (f) {
            fprintf(f,
                    "idx vm_addr owner_addr root thread_id thread_count worker_count avail_workers "
                    "stack_depth frames chunk_bytes globals const_globals procedures shell_indexing\n");
            for (size_t i = 0; i < snapshot_count; ++i) {
                const VMProcSnapshot *snapshot = &snapshots[i];
                fprintf(f,
                        "%zu 0x%016" PRIxPTR " 0x%016" PRIxPTR " %d %d %d %d %d %zu %d %d %zu %zu %zu %d\n",
                        i,
                        snapshot->vm_address,
                        snapshot->thread_owner_address,
                        snapshot->is_root_vm ? 1 : 0,
                        snapshot->thread_id,
                        snapshot->thread_count,
                        snapshot->worker_count,
                        snapshot->available_workers,
                        snapshot->stack_depth,
                        snapshot->frame_count,
                        snapshot->chunk_bytecode_count,
                        snapshot->global_symbol_count,
                        snapshot->const_symbol_count,
                        snapshot->procedure_symbol_count,
                        snapshot->shell_indexing ? 1 : 0);
            }
            fclose(f);
        }
    }

    char by_addr_dir[PATH_MAX];
    if (snprintf(by_addr_dir, sizeof(by_addr_dir), "%s/by-address", vm_dir) <= 0) {
        return;
    }
    pathTruncateEnsureDir(by_addr_dir);
    pathTruncatePruneProcVmDirs(by_addr_dir, snapshots, snapshot_count);

    for (size_t i = 0; i < snapshot_count; ++i) {
        const VMProcSnapshot *snapshot = &snapshots[i];

        char vm_key[32];
        snprintf(vm_key, sizeof(vm_key), "%016" PRIxPTR, snapshot->vm_address);

        char vm_entry_dir[PATH_MAX];
        if (snprintf(vm_entry_dir, sizeof(vm_entry_dir), "%s/%s", by_addr_dir, vm_key) <= 0) {
            continue;
        }
        pathTruncateEnsureDir(vm_entry_dir);

        char info_path[PATH_MAX];
        if (snprintf(info_path, sizeof(info_path), "%s/info", vm_entry_dir) > 0) {
            FILE *f = fopen(info_path, "w");
            if (f) {
                fprintf(f, "vm_address 0x%016" PRIxPTR "\n", snapshot->vm_address);
                fprintf(f, "thread_owner_address 0x%016" PRIxPTR "\n", snapshot->thread_owner_address);
                fprintf(f, "frontend_context_address 0x%016" PRIxPTR "\n", snapshot->frontend_context_address);
                fprintf(f, "chunk_address 0x%016" PRIxPTR "\n", snapshot->chunk_address);
                fprintf(f, "globals_address 0x%016" PRIxPTR "\n", snapshot->globals_address);
                fprintf(f, "const_globals_address 0x%016" PRIxPTR "\n", snapshot->const_globals_address);
                fprintf(f, "procedures_address 0x%016" PRIxPTR "\n", snapshot->procedures_address);
                fprintf(f, "mutex_owner_address 0x%016" PRIxPTR "\n", snapshot->mutex_owner_address);
                fprintf(f, "thread_id %d\n", snapshot->thread_id);
                fprintf(f, "thread_count %d\n", snapshot->thread_count);
                fprintf(f, "worker_count %d\n", snapshot->worker_count);
                fprintf(f, "available_workers %d\n", snapshot->available_workers);
                fprintf(f, "mutex_count %d\n", snapshot->mutex_count);
                fprintf(f, "frame_count %d\n", snapshot->frame_count);
                fprintf(f, "trace_head_instructions %d\n", snapshot->trace_head_instructions);
                fprintf(f, "trace_executed %d\n", snapshot->trace_executed);
                fprintf(f, "chunk_bytecode_count %d\n", snapshot->chunk_bytecode_count);
                fprintf(f, "stack_depth %zu\n", snapshot->stack_depth);
                fprintf(f, "global_symbol_count %zu\n", snapshot->global_symbol_count);
                fprintf(f, "const_symbol_count %zu\n", snapshot->const_symbol_count);
                fprintf(f, "procedure_symbol_count %zu\n", snapshot->procedure_symbol_count);
                fprintf(f, "is_root_vm %d\n", snapshot->is_root_vm ? 1 : 0);
                fprintf(f, "has_job_queue %d\n", snapshot->has_job_queue ? 1 : 0);
                fprintf(f, "shell_indexing %d\n", snapshot->shell_indexing ? 1 : 0);
                fprintf(f, "exit_requested %d\n", snapshot->exit_requested ? 1 : 0);
                fprintf(f, "abort_requested %d\n", snapshot->abort_requested ? 1 : 0);
                fprintf(f, "suspend_unwind_requested %d\n", snapshot->suspend_unwind_requested ? 1 : 0);
                fclose(f);
            }
        }

        char workers_path[PATH_MAX];
        if (snprintf(workers_path, sizeof(workers_path), "%s/workers", vm_entry_dir) <= 0) {
            continue;
        }
        FILE *workers_file = fopen(workers_path, "w");
        if (!workers_file) {
            continue;
        }
        if (!snapshot->is_root_vm) {
            fprintf(workers_file,
                    "worker vm; inspect owner 0x%016" PRIxPTR " for shared pool state\n",
                    snapshot->thread_owner_address);
            fclose(workers_file);
            continue;
        }

        VMProcWorkerSnapshot workers[VM_MAX_THREADS];
        size_t worker_count = pathTruncateSnapshotProcVmWorkers(
            snapshot->vm_address,
            workers,
            sizeof(workers) / sizeof(workers[0]));
        fprintf(workers_file,
                "slot vm_addr in_pool active idle paused cancel kill owns_vm pool_worker "
                "awaiting_reuse ready_for_reuse status_ready result_ready generation "
                "queued_at started_at finished_at cpu_seconds rss_start rss_end name\n");
        for (size_t w = 0; w < worker_count; ++w) {
            const VMProcWorkerSnapshot *worker = &workers[w];
            double cpu_seconds = pathTruncateThreadCpuSeconds(&worker->metrics);
            unsigned long long rss_start = (unsigned long long)worker->metrics.start.rssBytes;
            unsigned long long rss_end = (unsigned long long)worker->metrics.end.rssBytes;
            fprintf(workers_file,
                    "%d 0x%016" PRIxPTR " %d %d %d %d %d %d %d %d %d %d %d %d %d "
                    "%.6f %.6f %.6f %.6f %llu %llu %s\n",
                    worker->slot_id,
                    worker->vm_address,
                    worker->in_pool ? 1 : 0,
                    worker->active ? 1 : 0,
                    worker->idle ? 1 : 0,
                    worker->paused ? 1 : 0,
                    worker->cancel_requested ? 1 : 0,
                    worker->kill_requested ? 1 : 0,
                    worker->owns_vm ? 1 : 0,
                    worker->pool_worker ? 1 : 0,
                    worker->awaiting_reuse ? 1 : 0,
                    worker->ready_for_reuse ? 1 : 0,
                    worker->status_ready ? 1 : 0,
                    worker->result_ready ? 1 : 0,
                    worker->pool_generation,
                    pathTruncateTimespecToSeconds(worker->queued_at),
                    pathTruncateTimespecToSeconds(worker->started_at),
                    pathTruncateTimespecToSeconds(worker->finished_at),
                    cpu_seconds,
                    rss_start,
                    rss_end,
                    worker->name[0] ? worker->name : "-");
        }
        fclose(workers_file);
    }
}

static void pathTruncateEnsureProcBootIdentity(void) {
    if (g_procBootTime == 0) {
        struct timespec mono = {0};
        time_t now = time(NULL);
        if (clock_gettime(CLOCK_MONOTONIC, &mono) == 0 && now > (time_t)mono.tv_sec) {
            g_procBootTime = now - (time_t)mono.tv_sec;
        } else {
            g_procBootTime = now;
        }
    }
    if (g_procBootId[0] == '\0') {
        uint64_t a = (uint64_t)time(NULL);
        uint64_t b = (uint64_t)getpid() ^ (uint64_t)(uintptr_t)&a;
        uint32_t p0 = (uint32_t)(a & 0xffffffffu);
        uint16_t p1 = (uint16_t)((a >> 32) & 0xffffu);
        uint16_t p2 = (uint16_t)((a >> 48) & 0xffffu);
        uint16_t p3 = (uint16_t)(b & 0xffffu);
        uint16_t p4a = (uint16_t)((b >> 16) & 0xffffu);
        uint16_t p4b = (uint16_t)((b >> 32) & 0xffffu);
        uint16_t p4c = (uint16_t)((b >> 48) & 0xffffu);
        snprintf(g_procBootId,
                 sizeof(g_procBootId),
                 "%08x-%04x-%04x-%04x-%04x%04x%04x",
                 p0, p1, p2, p3, p4a, p4b, p4c);
    }
}

static void pathTruncateResolveExePath(const char *prefix, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    char host_path[PATH_MAX];
    host_path[0] = '\0';
#if defined(__APPLE__)
    uint32_t size = (uint32_t)sizeof(host_path);
    if (_NSGetExecutablePath(host_path, &size) == 0) {
        char resolved[PATH_MAX];
        if (realpath(host_path, resolved)) {
            snprintf(host_path, sizeof(host_path), "%s", resolved);
        }
    }
#endif
    if (host_path[0] == '\0') {
        snprintf(out, out_size, "/bin/exsh");
        return;
    }
    pathTruncateProcStripContainerPrefix(prefix, host_path, out, out_size);
}

static void pathTruncateEnsureProcPidDir(const char *procdir, int pid) {
    if (!procdir || pid <= 0) {
        return;
    }
    char pid_dir[PATH_MAX];
    if (snprintf(pid_dir, sizeof(pid_dir), "%s/%d", procdir, pid) <= 0) {
        return;
    }
    pathTruncateEnsureDir(pid_dir);
}

static void pathTruncateWriteProcPidEntries(const char *procdir,
                                            const char *prefix,
                                            pid_t pid,
                                            pid_t ppid,
                                            const char *proc_name,
                                            uint64_t mem_total_kb,
                                            double uptime_secs) {
    if (!procdir || !proc_name) {
        return;
    }

    char pid_dir[PATH_MAX];
    if (snprintf(pid_dir, sizeof(pid_dir), "%s/%d", procdir, (int)pid) <= 0) {
        return;
    }
    pathTruncateEnsureDir(pid_dir);

    char comm_path[PATH_MAX];
    if (snprintf(comm_path, sizeof(comm_path), "%s/comm", pid_dir) > 0) {
        char line[128];
        snprintf(line, sizeof(line), "%s\n", proc_name);
        write_text_file(comm_path, line);
    }

    char cmdline_path[PATH_MAX];
    if (snprintf(cmdline_path, sizeof(cmdline_path), "%s/cmdline", pid_dir) > 0) {
        char cmdline[128];
        size_t n = (size_t)snprintf(cmdline, sizeof(cmdline), "%s", proc_name);
        if (n + 1 < sizeof(cmdline)) {
            cmdline[n] = '\0';
            pathTruncateWriteBinaryFile(cmdline_path, cmdline, n + 1);
        } else {
            write_text_file(cmdline_path, "exsh");
        }
    }
    char environ_path[PATH_MAX];
    if (snprintf(environ_path, sizeof(environ_path), "%s/environ", pid_dir) > 0) {
        pathTruncateWriteProcEnviron(environ_path);
    }

    char status_path[PATH_MAX];
    if (snprintf(status_path, sizeof(status_path), "%s/status", pid_dir) > 0) {
        FILE *f = fopen(status_path, "w");
        if (f) {
            uid_t uid = getuid();
            gid_t gid = getgid();
            long open_max = sysconf(_SC_OPEN_MAX);
            if (open_max <= 0 || open_max > 4096) {
                open_max = 256;
            }
            fprintf(f, "Name:\t%s\n", proc_name);
            fprintf(f, "State:\tR (running)\n");
            fprintf(f, "Tgid:\t%d\n", (int)pid);
            fprintf(f, "Pid:\t%d\n", (int)pid);
            fprintf(f, "PPid:\t%d\n", (int)ppid);
            fprintf(f, "Uid:\t%u\t%u\t%u\t%u\n", (unsigned)uid, (unsigned)uid, (unsigned)uid, (unsigned)uid);
            fprintf(f, "Gid:\t%u\t%u\t%u\t%u\n", (unsigned)gid, (unsigned)gid, (unsigned)gid, (unsigned)gid);
            fprintf(f, "FDSize:\t%ld\n", open_max);
            fprintf(f, "Threads:\t1\n");
            fprintf(f, "VmSize:\t%llu kB\n", (unsigned long long)(mem_total_kb / 8));
            fprintf(f, "VmRSS:\t%llu kB\n", (unsigned long long)(mem_total_kb / 16));
            fclose(f);
        }
    }
    char statm_path[PATH_MAX];
    if (snprintf(statm_path, sizeof(statm_path), "%s/statm", pid_dir) > 0) {
        long page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0) {
            page_size = 4096;
        }
        unsigned long long size_pages = (mem_total_kb * 1024ull / 8ull) / (unsigned long long)page_size;
        unsigned long long rss_pages = (mem_total_kb * 1024ull / 16ull) / (unsigned long long)page_size;
        char buf[128];
        snprintf(buf, sizeof(buf), "%llu %llu 0 0 0 0 0\n", size_pages, rss_pages);
        write_text_file(statm_path, buf);
    }

    char stat_path[PATH_MAX];
    if (snprintf(stat_path, sizeof(stat_path), "%s/stat", pid_dir) > 0) {
        FILE *f = fopen(stat_path, "w");
        if (f) {
            long hz = sysconf(_SC_CLK_TCK);
            if (hz <= 0) {
                hz = 100;
            }
            unsigned long long start_ticks = (unsigned long long)(uptime_secs * (double)hz * 0.1);
            unsigned long long utime = (unsigned long long)(uptime_secs * (double)hz * 0.02);
            unsigned long long stime = (unsigned long long)(uptime_secs * (double)hz * 0.01);
            unsigned long long vsize = (unsigned long long)(mem_total_kb * 1024ull / 8ull);
            long rss = (long)(mem_total_kb / 16ull);
            fprintf(f,
                    "%d (%s) R %d %d %d 0 -1 4194304 0 0 0 0 %llu %llu 0 0 20 0 1 0 %llu %llu %ld 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                    (int)pid,
                    proc_name,
                    (int)ppid,
                    (int)pid,
                    (int)pid,
                    utime,
                    stime,
                    start_ticks,
                    vsize,
                    rss);
            fclose(f);
        }
    }
    char io_path[PATH_MAX];
    if (snprintf(io_path, sizeof(io_path), "%s/io", pid_dir) > 0) {
        write_text_file(io_path,
                        "rchar: 0\n"
                        "wchar: 0\n"
                        "syscr: 0\n"
                        "syscw: 0\n"
                        "read_bytes: 0\n"
                        "write_bytes: 0\n"
                        "cancelled_write_bytes: 0\n");
    }
    char cgroup_path[PATH_MAX];
    if (snprintf(cgroup_path, sizeof(cgroup_path), "%s/cgroup", pid_dir) > 0) {
        write_text_file(cgroup_path, "0::/\n");
    }
    char limits_path[PATH_MAX];
    if (snprintf(limits_path, sizeof(limits_path), "%s/limits", pid_dir) > 0) {
        pathTruncateWriteProcLimits(limits_path);
    }
    char wchan_path[PATH_MAX];
    if (snprintf(wchan_path, sizeof(wchan_path), "%s/wchan", pid_dir) > 0) {
        write_text_file(wchan_path, "0\n");
    }
    char sched_path[PATH_MAX];
    if (snprintf(sched_path, sizeof(sched_path), "%s/sched", pid_dir) > 0) {
        FILE *f = fopen(sched_path, "w");
        if (f) {
            fprintf(f, "%s (%d, #threads: 1)\n", proc_name, (int)pid);
            fprintf(f, "se.exec_start                                : 0.000000\n");
            fprintf(f, "se.vruntime                                  : 0.000000\n");
            fprintf(f, "se.sum_exec_runtime                          : 0.000000\n");
            fprintf(f, "nr_switches                                  : 0\n");
            fprintf(f, "nr_voluntary_switches                        : 0\n");
            fprintf(f, "nr_involuntary_switches                      : 0\n");
            fclose(f);
        }
    }
    char schedstat_path[PATH_MAX];
    if (snprintf(schedstat_path, sizeof(schedstat_path), "%s/schedstat", pid_dir) > 0) {
        write_text_file(schedstat_path, "0 0 0\n");
    }
    char stack_path[PATH_MAX];
    if (snprintf(stack_path, sizeof(stack_path), "%s/stack", pid_dir) > 0) {
        write_text_file(stack_path, "[<0>] userspace\n");
    }
    char cpuset_path[PATH_MAX];
    if (snprintf(cpuset_path, sizeof(cpuset_path), "%s/cpuset", pid_dir) > 0) {
        write_text_file(cpuset_path, "/\n");
    }
    char oom_score_path[PATH_MAX];
    if (snprintf(oom_score_path, sizeof(oom_score_path), "%s/oom_score", pid_dir) > 0) {
        write_text_file(oom_score_path, "0\n");
    }
    char oom_adj_path[PATH_MAX];
    if (snprintf(oom_adj_path, sizeof(oom_adj_path), "%s/oom_score_adj", pid_dir) > 0) {
        write_text_file(oom_adj_path, "0\n");
    }
    char personality_path[PATH_MAX];
    if (snprintf(personality_path, sizeof(personality_path), "%s/personality", pid_dir) > 0) {
        write_text_file(personality_path, "00000000\n");
    }
    char loginuid_path[PATH_MAX];
    if (snprintf(loginuid_path, sizeof(loginuid_path), "%s/loginuid", pid_dir) > 0) {
        write_text_file(loginuid_path, "4294967295\n");
    }
    char sessionid_path[PATH_MAX];
    if (snprintf(sessionid_path, sizeof(sessionid_path), "%s/sessionid", pid_dir) > 0) {
        write_text_file(sessionid_path, "0\n");
    }
    char attr_dir[PATH_MAX];
    if (snprintf(attr_dir, sizeof(attr_dir), "%s/attr", pid_dir) > 0) {
        pathTruncateEnsureDir(attr_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/current", attr_dir) > 0) {
            write_text_file(pathbuf, "unconfined\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/prev", attr_dir) > 0) {
            write_text_file(pathbuf, "unconfined\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/exec", attr_dir) > 0) {
            write_text_file(pathbuf, "unconfined\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/fscreate", attr_dir) > 0) {
            write_text_file(pathbuf, "unconfined\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/keycreate", attr_dir) > 0) {
            write_text_file(pathbuf, "unconfined\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/sockcreate", attr_dir) > 0) {
            write_text_file(pathbuf, "unconfined\n");
        }
    }
    char mounts_path[PATH_MAX];
    if (snprintf(mounts_path, sizeof(mounts_path), "%s/mounts", pid_dir) > 0) {
        pathTruncateEnsureSymlink(mounts_path, "../mounts");
    }
    char net_path[PATH_MAX];
    if (snprintf(net_path, sizeof(net_path), "%s/net", pid_dir) > 0) {
        pathTruncateEnsureSymlink(net_path, "../net");
    }
    char mountinfo_path[PATH_MAX];
    if (snprintf(mountinfo_path, sizeof(mountinfo_path), "%s/mountinfo", pid_dir) > 0) {
        pathTruncateEnsureSymlink(mountinfo_path, "../mountinfo");
    }
    char task_dir[PATH_MAX];
    if (snprintf(task_dir, sizeof(task_dir), "%s/task", pid_dir) > 0) {
        pathTruncateEnsureDir(task_dir);
        char task_tid_dir[PATH_MAX];
        if (snprintf(task_tid_dir, sizeof(task_tid_dir), "%s/%d", task_dir, (int)pid) > 0) {
            struct stat task_st;
            if (lstat(task_tid_dir, &task_st) == 0 && S_ISLNK(task_st.st_mode)) {
                unlink(task_tid_dir);
            }
            pathTruncateEnsureDir(task_tid_dir);

            char pathbuf[PATH_MAX];
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/comm", task_tid_dir) > 0) {
                char line[128];
                snprintf(line, sizeof(line), "%s\n", proc_name);
                write_text_file(pathbuf, line);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/status", task_tid_dir) > 0) {
                FILE *f = fopen(pathbuf, "w");
                if (f) {
                    uid_t uid = getuid();
                    gid_t gid = getgid();
                    fprintf(f, "Name:\t%s\n", proc_name);
                    fprintf(f, "State:\tR (running)\n");
                    fprintf(f, "Tgid:\t%d\n", (int)pid);
                    fprintf(f, "Pid:\t%d\n", (int)pid);
                    fprintf(f, "PPid:\t%d\n", (int)ppid);
                    fprintf(f, "Uid:\t%u\t%u\t%u\t%u\n",
                            (unsigned)uid, (unsigned)uid, (unsigned)uid, (unsigned)uid);
                    fprintf(f, "Gid:\t%u\t%u\t%u\t%u\n",
                            (unsigned)gid, (unsigned)gid, (unsigned)gid, (unsigned)gid);
                    fprintf(f, "Threads:\t1\n");
                    fclose(f);
                }
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/stat", task_tid_dir) > 0) {
                FILE *f = fopen(pathbuf, "w");
                if (f) {
                    long hz = sysconf(_SC_CLK_TCK);
                    if (hz <= 0) {
                        hz = 100;
                    }
                    unsigned long long start_ticks = (unsigned long long)(uptime_secs * (double)hz * 0.1);
                    unsigned long long utime = (unsigned long long)(uptime_secs * (double)hz * 0.02);
                    unsigned long long stime = (unsigned long long)(uptime_secs * (double)hz * 0.01);
                    fprintf(f,
                            "%d (%s) R %d %d %d 0 -1 4194304 0 0 0 0 %llu %llu 0 0 20 0 1 0 %llu 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                            (int)pid,
                            proc_name,
                            (int)ppid,
                            (int)pid,
                            (int)pid,
                            utime,
                            stime,
                            start_ticks);
                    fclose(f);
                }
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/sched", task_tid_dir) > 0) {
                FILE *f = fopen(pathbuf, "w");
                if (f) {
                    fprintf(f, "%s (%d, #threads: 1)\n", proc_name, (int)pid);
                    fprintf(f, "se.exec_start                                : 0.000000\n");
                    fprintf(f, "se.vruntime                                  : 0.000000\n");
                    fprintf(f, "se.sum_exec_runtime                          : 0.000000\n");
                    fprintf(f, "nr_switches                                  : 0\n");
                    fprintf(f, "nr_voluntary_switches                        : 0\n");
                    fprintf(f, "nr_involuntary_switches                      : 0\n");
                    fclose(f);
                }
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/schedstat", task_tid_dir) > 0) {
                write_text_file(pathbuf, "0 0 0\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/stack", task_tid_dir) > 0) {
                write_text_file(pathbuf, "[<0>] userspace\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/wchan", task_tid_dir) > 0) {
                write_text_file(pathbuf, "0\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/cgroup", task_tid_dir) > 0) {
                write_text_file(pathbuf, "0::/\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/io", task_tid_dir) > 0) {
                write_text_file(pathbuf,
                                "rchar: 0\n"
                                "wchar: 0\n"
                                "syscr: 0\n"
                                "syscw: 0\n"
                                "read_bytes: 0\n"
                                "write_bytes: 0\n"
                                "cancelled_write_bytes: 0\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/cpuset", task_tid_dir) > 0) {
                write_text_file(pathbuf, "/\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/personality", task_tid_dir) > 0) {
                write_text_file(pathbuf, "00000000\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/children", task_tid_dir) > 0) {
                write_text_file(pathbuf, "\n");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/cwd", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../cwd");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/exe", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../exe");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/root", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../root");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/fd", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../fd");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/fdinfo", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../fdinfo");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/mounts", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../mounts");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/mountinfo", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../mountinfo");
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/net", task_tid_dir) > 0) {
                pathTruncateEnsureSymlink(pathbuf, "../../net");
            }
        }
    }

    char cwd_path[PATH_MAX];
    if (snprintf(cwd_path, sizeof(cwd_path), "%s/cwd", pid_dir) > 0) {
        char cwd_host[PATH_MAX];
        if (!getcwd(cwd_host, sizeof(cwd_host))) {
            snprintf(cwd_host, sizeof(cwd_host), "/");
        }
        char cwd_virtual[PATH_MAX];
        pathTruncateProcStripContainerPrefix(prefix, cwd_host, cwd_virtual, sizeof(cwd_virtual));
        pathTruncateEnsureSymlink(cwd_path, cwd_virtual[0] ? cwd_virtual : "/");
    }

    char exe_path[PATH_MAX];
    if (snprintf(exe_path, sizeof(exe_path), "%s/exe", pid_dir) > 0) {
        char exe_virtual[PATH_MAX];
        pathTruncateResolveExePath(prefix, exe_virtual, sizeof(exe_virtual));
        pathTruncateEnsureSymlink(exe_path, exe_virtual[0] ? exe_virtual : "/bin/exsh");
    }
    char root_path[PATH_MAX];
    if (snprintf(root_path, sizeof(root_path), "%s/root", pid_dir) > 0) {
        pathTruncateEnsureSymlink(root_path, "/");
    }

    char fd_dir[PATH_MAX];
    fd_dir[0] = '\0';
    if (snprintf(fd_dir, sizeof(fd_dir), "%s/fd", pid_dir) > 0) {
        pathTruncateEnsureDir(fd_dir);
    }
    char fdinfo_dir[PATH_MAX];
    fdinfo_dir[0] = '\0';
    if (snprintf(fdinfo_dir, sizeof(fdinfo_dir), "%s/fdinfo", pid_dir) > 0) {
        pathTruncateEnsureDir(fdinfo_dir);
    }

    long open_max = sysconf(_SC_OPEN_MAX);
    if (open_max <= 0 || open_max > 1024) {
        open_max = 256;
    }
    bool *fd_keep = (bool *)calloc((size_t)open_max, sizeof(bool));
    for (long fd = 0; fd < open_max; ++fd) {
        errno = 0;
        int fd_flags = fcntl((int)fd, F_GETFD);
        if (fd_flags < 0) {
            continue;
        }
        if (fd_keep) {
            fd_keep[fd] = true;
        }

        char fd_entry[PATH_MAX];
        if (snprintf(fd_entry, sizeof(fd_entry), "%s/%ld", fd_dir, fd) <= 0) {
            continue;
        }

        char host_ref[64];
        char host_target[PATH_MAX];
        if (snprintf(host_ref, sizeof(host_ref), "/dev/fd/%ld", fd) <= 0) {
            continue;
        }
        ssize_t n = readlink(host_ref, host_target, sizeof(host_target) - 1);
        if (n < 0) {
            snprintf(host_target, sizeof(host_target), "/dev/fd/%ld", fd);
        } else {
            host_target[n] = '\0';
        }
        char virtual_target[PATH_MAX];
        if (host_target[0] == '/') {
            pathTruncateProcStripContainerPrefix(prefix, host_target, virtual_target, sizeof(virtual_target));
        } else {
            snprintf(virtual_target, sizeof(virtual_target), "%s", host_target);
        }
        pathTruncateEnsureSymlink(fd_entry, virtual_target[0] ? virtual_target : "/dev/null");

        char fdinfo_path[PATH_MAX];
        if (snprintf(fdinfo_path, sizeof(fdinfo_path), "%s/%ld", fdinfo_dir, fd) <= 0) {
            continue;
        }
        FILE *f = fopen(fdinfo_path, "w");
        if (!f) {
            continue;
        }
        off_t pos = lseek((int)fd, 0, SEEK_CUR);
        if (pos < 0) {
            pos = 0;
        }
        int open_flags = fcntl((int)fd, F_GETFL);
        if (open_flags < 0) {
            open_flags = 0;
        }
        fprintf(f, "pos:\t%lld\n", (long long)pos);
        fprintf(f, "flags:\t0%o\n", open_flags);
        fprintf(f, "mnt_id:\t0\n");
        fclose(f);
    }
    if (fd_keep) {
        pathTruncatePruneNumericDirectoryChildren(fd_dir, fd_keep, open_max);
        pathTruncatePruneNumericDirectoryChildren(fdinfo_dir, fd_keep, open_max);
        free(fd_keep);
    }
}

static void pathTruncateRefreshProc(const char *prefix, const char *request_path) {
    if (!prefix || prefix[0] != '/') {
        return;
    }
    if (request_path && !pathTruncateIsProcRequestPath(request_path)) {
        return;
    }
    bool request_net = request_path &&
        (pathTruncateProcPrefixMatch(request_path, "/proc/net") ||
         pathTruncateProcPrefixMatch(request_path, "/private/proc/net"));
    bool request_device = request_path &&
        (pathTruncateProcPrefixMatch(request_path, "/proc/device") ||
         pathTruncateProcPrefixMatch(request_path, "/private/proc/device"));
    bool request_vm = request_path &&
        (pathTruncateProcPrefixMatch(request_path, "/proc/vm") ||
         pathTruncateProcPrefixMatch(request_path, "/private/proc/vm"));
    bool request_proc_root = request_path &&
        (strcmp(request_path, "/proc") == 0 ||
         strcmp(request_path, "/proc/") == 0 ||
         strcmp(request_path, "/private/proc") == 0 ||
         strcmp(request_path, "/private/proc/") == 0);

    uint64_t now_ms = pathTruncateMonotonicMs();
    uint64_t *bucket = &g_procRefreshLastFullMs;
    uint64_t min_interval_ms = 250;
    if (request_net) {
        bucket = &g_procRefreshLastNetMs;
        min_interval_ms = 200;
    } else if (request_device) {
        bucket = &g_procRefreshLastDeviceMs;
        min_interval_ms = 300;
    } else if (request_vm) {
        bucket = &g_procRefreshLastVmMs;
        min_interval_ms = 200;
    }
    if (now_ms != 0 && *bucket != 0 && now_ms > *bucket &&
        (now_ms - *bucket) < min_interval_ms) {
        return;
    }
    if (now_ms != 0) {
        *bucket = now_ms;
    }

    bool refresh_pid_entries = true;
    if (request_net) {
        refresh_pid_entries = false;
    }
    bool refresh_device_entries = refresh_pid_entries;
    if (!request_device) {
        refresh_device_entries = false;
    }

    char procdir[PATH_MAX];
    if (snprintf(procdir, sizeof(procdir), "%s/proc", prefix) <= 0) {
        return;
    }
    pathTruncateEnsureDir(procdir);

    char cpuinfo_sentinel[PATH_MAX];
    bool has_cpuinfo = false;
    if (snprintf(cpuinfo_sentinel, sizeof(cpuinfo_sentinel), "%s/cpuinfo", procdir) > 0) {
        has_cpuinfo = (access(cpuinfo_sentinel, F_OK) == 0);
    }
    bool seed_needed = !g_procBaseSeeded || !has_cpuinfo;

    struct timespec mono = {0};
    double uptime_secs = 0.0;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) == 0) {
        uptime_secs = (double)mono.tv_sec + ((double)mono.tv_nsec / 1e9);
    }

    pathTruncateEnsureProcBootIdentity();
    time_t now = time(NULL);
    if (g_procBootTime == 0 || g_procBootTime > now) {
        g_procBootTime = now;
    }
    pid_t host_pid = getpid();
    if (host_pid <= 0) {
        host_pid = 1;
    }

    if (request_proc_root && !seed_needed) {
        int current_vproc_pid = pathTruncateCurrentVProcPid();
        PathTruncateVProcSnapshot vproc_snapshots[512];
        size_t vproc_snapshot_count = pathTruncateSnapshotVProcState(
            vproc_snapshots,
            sizeof(vproc_snapshots) / sizeof(vproc_snapshots[0]));
        bool wrote_any = false;
        int keep_pids[512];
        size_t keep_count = 0;
        for (size_t i = 0; i < vproc_snapshot_count; ++i) {
            if (vproc_snapshots[i].pid <= 0) {
                continue;
            }
            pathTruncateEnsureProcPidDir(procdir, vproc_snapshots[i].pid);
            if (keep_count < (sizeof(keep_pids) / sizeof(keep_pids[0]))) {
                keep_pids[keep_count++] = vproc_snapshots[i].pid;
            }
            wrote_any = true;
        }
        if (!wrote_any && current_vproc_pid > 0) {
            pathTruncateEnsureProcPidDir(procdir, current_vproc_pid);
            keep_pids[keep_count++] = current_vproc_pid;
            wrote_any = true;
        }
        if (!wrote_any) {
            pathTruncateEnsureProcPidDir(procdir, (int)host_pid);
            current_vproc_pid = (int)host_pid;
            keep_pids[keep_count++] = (int)host_pid;
        }
        char self_path[PATH_MAX];
        if (snprintf(self_path, sizeof(self_path), "%s/self", procdir) > 0) {
            char pid_link[32];
            int self_link_pid = current_vproc_pid > 0 ? current_vproc_pid : (int)host_pid;
            snprintf(pid_link, sizeof(pid_link), "%d", self_link_pid);
            pathTruncateEnsureSymlink(self_path, pid_link);
        }
        bool allow_vproc_prune = true;
        uint64_t vproc_prune_interval_ms = g_procPrunePending ? 250 : 2000;
        if (now_ms != 0 && g_procRefreshLastPruneMs != 0 && now_ms > g_procRefreshLastPruneMs) {
            allow_vproc_prune = (now_ms - g_procRefreshLastPruneMs) >= vproc_prune_interval_ms;
        }
        if (allow_vproc_prune) {
            g_procPrunePending = pathTruncatePruneNumericDirectoryChildrenByPidList(procdir,
                                                                                     keep_pids,
                                                                                     keep_count,
                                                                                     64);
            if (now_ms != 0) {
                g_procRefreshLastPruneMs = now_ms;
            }
        }
        return;
    }

    int ncpu = 1;
    uint64_t freq = 0;
    uint64_t mem_bytes = 0;
    char machine[128] = {0};
    char model[128] = {0};
#if defined(__APPLE__)
    size_t sz = sizeof(ncpu);
    sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0);
    sz = sizeof(freq);
    sysctlbyname("hw.cpufrequency", &freq, &sz, NULL, 0);
    sz = sizeof(mem_bytes);
    sysctlbyname("hw.memsize", &mem_bytes, &sz, NULL, 0);
    sz = sizeof(machine);
    sysctlbyname("hw.machine", machine, &sz, NULL, 0);
    sz = sizeof(model);
    sysctlbyname("hw.model", model, &sz, NULL, 0);
#endif
    if (ncpu <= 0) {
        ncpu = 1;
    }
    uint64_t mem_total_kb = mem_bytes / 1024ull;
    if (mem_total_kb == 0) {
        mem_total_kb = 1024ull * 1024ull;
    }

    int current_vproc_pid = pathTruncateCurrentVProcPid();
    PathTruncateVProcSnapshot vproc_snapshots[512];
    size_t vproc_snapshot_count = 0;
    if (refresh_pid_entries) {
        vproc_snapshot_count = pathTruncateSnapshotVProcState(
            vproc_snapshots,
            sizeof(vproc_snapshots) / sizeof(vproc_snapshots[0]));
        if (vproc_snapshot_count == 0 && current_vproc_pid == (int)host_pid) {
            current_vproc_pid = -1;
        }
        if (current_vproc_pid <= 0 && vproc_snapshot_count > 0 && vproc_snapshots[0].pid > 0) {
            current_vproc_pid = vproc_snapshots[0].pid;
        }
    }
    int proc_display_pid = current_vproc_pid > 0 ? current_vproc_pid : (int)host_pid;

    char cpuinfo_path[PATH_MAX];
    if (snprintf(cpuinfo_path, sizeof(cpuinfo_path), "%s/cpuinfo", procdir) > 0) {
        FILE *f = fopen(cpuinfo_path, "w");
        if (f) {
            for (int i = 0; i < ncpu; ++i) {
                fprintf(f, "processor\t: %d\n", i);
                fprintf(f, "model name\t: PSCAL virtual CPU\n");
                if (freq > 0) {
                    fprintf(f, "cpu MHz\t\t: %.0f\n", (double)freq / 1e6);
                }
                fprintf(f, "Hardware\t: %s %s\n",
                        machine[0] ? machine : "arm64",
                        model[0] ? model : "");
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }

    char meminfo_path[PATH_MAX];
    if (snprintf(meminfo_path, sizeof(meminfo_path), "%s/meminfo", procdir) > 0) {
        FILE *f = fopen(meminfo_path, "w");
        if (f) {
            fprintf(f, "MemTotal:       %llu kB\n", (unsigned long long)mem_total_kb);
            fprintf(f, "MemFree:        %llu kB\n", (unsigned long long)(mem_total_kb / 4));
            fprintf(f, "MemAvailable:   %llu kB\n", (unsigned long long)(mem_total_kb / 2));
            fprintf(f, "Buffers:        0 kB\n");
            fprintf(f, "Cached:         0 kB\n");
            fprintf(f, "SwapCached:     0 kB\n");
            fprintf(f, "SwapTotal:      0 kB\n");
            fprintf(f, "SwapFree:       0 kB\n");
            fclose(f);
        }
    }

    char uptime_path[PATH_MAX];
    if (snprintf(uptime_path, sizeof(uptime_path), "%s/uptime", procdir) > 0) {
        FILE *f = fopen(uptime_path, "w");
        if (f) {
            fprintf(f, "%.2f %.2f\n", uptime_secs, uptime_secs);
            fclose(f);
        }
    }

    char version_path[PATH_MAX];
    if (snprintf(version_path, sizeof(version_path), "%s/version", procdir) > 0) {
        struct utsname un;
        uname(&un);
        char buf[256];
        snprintf(buf, sizeof(buf), "PSCALI %s %s %s\n", un.sysname, un.release, un.version);
        write_text_file(version_path, buf);
    }

    char cmdline_path[PATH_MAX];
    if (snprintf(cmdline_path, sizeof(cmdline_path), "%s/cmdline", procdir) > 0) {
        static const char kCmdline[] = "pscal sandbox";
        pathTruncateWriteBinaryFile(cmdline_path, kCmdline, sizeof(kCmdline));
    }

    char stat_path[PATH_MAX];
    if (snprintf(stat_path, sizeof(stat_path), "%s/stat", procdir) > 0) {
        FILE *f = fopen(stat_path, "w");
        if (f) {
            long hz = sysconf(_SC_CLK_TCK);
            if (hz <= 0) {
                hz = 100;
            }
            unsigned long long total_ticks = (unsigned long long)(uptime_secs * (double)hz);
            unsigned long long user_ticks = total_ticks / 6ull;
            unsigned long long system_ticks = total_ticks / 10ull;
            unsigned long long idle_ticks = total_ticks > (user_ticks + system_ticks)
                ? (total_ticks - user_ticks - system_ticks)
                : total_ticks;
            fprintf(f, "cpu  %llu 0 %llu %llu 0 0 0 0 0 0\n",
                    user_ticks, system_ticks, idle_ticks);
            fprintf(f, "intr 0\n");
            fprintf(f, "ctxt %llu\n", total_ticks / 3ull);
            fprintf(f, "btime %ld\n", (long)g_procBootTime);
            fprintf(f, "processes 1\n");
            fprintf(f, "procs_running 1\n");
            fprintf(f, "procs_blocked 0\n");
            fclose(f);
        }
    }

    char loadavg_path[PATH_MAX];
    if (snprintf(loadavg_path, sizeof(loadavg_path), "%s/loadavg", procdir) > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), "0.00 0.00 0.00 1/1 %d\n", proc_display_pid);
        write_text_file(loadavg_path, buf);
    }

    char interrupts_path[PATH_MAX];
    if (snprintf(interrupts_path, sizeof(interrupts_path), "%s/interrupts", procdir) > 0) {
        FILE *f = fopen(interrupts_path, "w");
        if (f) {
            fprintf(f, "            ");
            for (int i = 0; i < ncpu; ++i) {
                fprintf(f, "CPU%-8d", i);
            }
            fprintf(f, "\n");
            fprintf(f, "  0:");
            for (int i = 0; i < ncpu; ++i) {
                fprintf(f, " %10u", 0u);
            }
            fprintf(f, "  PSCAL-virt-timer\n");
            fprintf(f, "  1:");
            for (int i = 0; i < ncpu; ++i) {
                fprintf(f, " %10u", 0u);
            }
            fprintf(f, "  PSCAL-virt-io\n");
            fclose(f);
        }
    }

    char softirqs_path[PATH_MAX];
    if (snprintf(softirqs_path, sizeof(softirqs_path), "%s/softirqs", procdir) > 0) {
        FILE *f = fopen(softirqs_path, "w");
        if (f) {
            fprintf(f, "                    ");
            for (int i = 0; i < ncpu; ++i) {
                fprintf(f, "CPU%-8d", i);
            }
            fprintf(f, "\n");
            const char *rows[] = { "HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "IRQ_POLL", "TASKLET", "SCHED", "HRTIMER", "RCU" };
            for (size_t r = 0; r < sizeof(rows) / sizeof(rows[0]); ++r) {
                fprintf(f, "%-10s:", rows[r]);
                for (int i = 0; i < ncpu; ++i) {
                    fprintf(f, " %10u", 0u);
                }
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }

    char modules_path[PATH_MAX];
    if (snprintf(modules_path, sizeof(modules_path), "%s/modules", procdir) > 0) {
        write_text_file(modules_path, "vproc 16384 0 - Live 0x0000000000000000\n");
    }

    char vmstat_path[PATH_MAX];
    if (snprintf(vmstat_path, sizeof(vmstat_path), "%s/vmstat", procdir) > 0) {
        write_text_file(vmstat_path,
                        "pgpgin 0\npgpgout 0\npswpin 0\npswpout 0\npgfault 0\npgmajfault 0\n");
    }
    char buddyinfo_path[PATH_MAX];
    if (snprintf(buddyinfo_path, sizeof(buddyinfo_path), "%s/buddyinfo", procdir) > 0) {
        pathTruncateWriteProcBuddyinfo(buddyinfo_path, ncpu);
    }
    char zoneinfo_path[PATH_MAX];
    if (snprintf(zoneinfo_path, sizeof(zoneinfo_path), "%s/zoneinfo", procdir) > 0) {
        pathTruncateWriteProcZoneinfo(zoneinfo_path, mem_total_kb);
    }
    char pagetypeinfo_path[PATH_MAX];
    if (snprintf(pagetypeinfo_path, sizeof(pagetypeinfo_path), "%s/pagetypeinfo", procdir) > 0) {
        pathTruncateWriteProcPagetypeinfo(pagetypeinfo_path);
    }
    char slabinfo_path[PATH_MAX];
    if (snprintf(slabinfo_path, sizeof(slabinfo_path), "%s/slabinfo", procdir) > 0) {
        pathTruncateWriteProcSlabinfo(slabinfo_path);
    }

    char diskstats_path[PATH_MAX];
    if (snprintf(diskstats_path, sizeof(diskstats_path), "%s/diskstats", procdir) > 0) {
        write_text_file(diskstats_path, "   1       0 vda 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n");
    }
    char partitions_path[PATH_MAX];
    if (snprintf(partitions_path, sizeof(partitions_path), "%s/partitions", procdir) > 0) {
        pathTruncateWriteProcPartitions(partitions_path);
    }
    char locks_path[PATH_MAX];
    if (snprintf(locks_path, sizeof(locks_path), "%s/locks", procdir) > 0) {
        pathTruncateWriteProcLocks(locks_path);
    }

    char swaps_path[PATH_MAX];
    if (snprintf(swaps_path, sizeof(swaps_path), "%s/swaps", procdir) > 0) {
        write_text_file(swaps_path, "Filename\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
    }

    char filesystems_path[PATH_MAX];
    if (snprintf(filesystems_path, sizeof(filesystems_path), "%s/filesystems", procdir) > 0) {
        write_text_file(filesystems_path, "nodev\tsysfs\nnodev\tproc\nnodev\ttmpfs\n\text4\n");
    }

    char mounts_path[PATH_MAX];
    char mountinfo_path[PATH_MAX];
    struct statfs sfs;
    const char *mnt_from = "rootfs";
    const char *fs_type = "ext4";
    if (statfs("/", &sfs) == 0) {
        if (sfs.f_mntfromname[0]) {
            mnt_from = sfs.f_mntfromname;
        }
        if (sfs.f_fstypename[0]) {
            fs_type = sfs.f_fstypename;
        }
    }
    if (snprintf(mounts_path, sizeof(mounts_path), "%s/mounts", procdir) > 0) {
        FILE *f = fopen(mounts_path, "w");
        if (f) {
            fprintf(f, "%s / %s rw 0 0\n", mnt_from, fs_type);
            fclose(f);
        }
    }
    if (snprintf(mountinfo_path, sizeof(mountinfo_path), "%s/mountinfo", procdir) > 0) {
        FILE *f = fopen(mountinfo_path, "w");
        if (f) {
            fprintf(f, "1 0 0:1 / / rw - %s %s rw\n", fs_type, mnt_from);
            fclose(f);
        }
    }

    char sys_kernel_dir[PATH_MAX];
    if (snprintf(sys_kernel_dir, sizeof(sys_kernel_dir), "%s/sys/kernel", procdir) > 0) {
        pathTruncateEnsureDir(sys_kernel_dir);
        const char *hostbuf = "pscal";
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/hostname", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, hostbuf);
        }
        struct utsname un;
        uname(&un);
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/osrelease", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, un.release);
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/ostype", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, un.sysname);
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/version", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, un.version);
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/pid_max", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, "4194304\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/threads-max", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, "65535\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/sched_child_runs_first", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, "0\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/panic", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, "0\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/core_pattern", sys_kernel_dir) > 0) {
            write_text_file(pathbuf, "core\n");
        }
        char random_dir[PATH_MAX];
        if (snprintf(random_dir, sizeof(random_dir), "%s/random", sys_kernel_dir) > 0) {
            pathTruncateEnsureDir(random_dir);
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/boot_id", random_dir) > 0) {
                char boot_line[64];
                snprintf(boot_line, sizeof(boot_line), "%s\n", g_procBootId);
                write_text_file(pathbuf, boot_line);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/uuid", random_dir) > 0) {
                char uuid_line[64];
                snprintf(uuid_line, sizeof(uuid_line), "%s\n", g_procBootId);
                write_text_file(pathbuf, uuid_line);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/entropy_avail", random_dir) > 0) {
                write_text_file(pathbuf, "256\n");
            }
        }
    }

    char sys_vm_dir[PATH_MAX];
    if (snprintf(sys_vm_dir, sizeof(sys_vm_dir), "%s/sys/vm", procdir) > 0) {
        pathTruncateEnsureDir(sys_vm_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/swappiness", sys_vm_dir) > 0) {
            write_text_file(pathbuf, "60\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/overcommit_memory", sys_vm_dir) > 0) {
            write_text_file(pathbuf, "0\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/overcommit_ratio", sys_vm_dir) > 0) {
            write_text_file(pathbuf, "50\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/max_map_count", sys_vm_dir) > 0) {
            write_text_file(pathbuf, "65530\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/dirty_background_ratio", sys_vm_dir) > 0) {
            write_text_file(pathbuf, "10\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/dirty_ratio", sys_vm_dir) > 0) {
            write_text_file(pathbuf, "20\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/min_free_kbytes", sys_vm_dir) > 0) {
            char min_free[64];
            unsigned long long min_kb = mem_total_kb / 200ull;
            if (min_kb < 1024ull) {
                min_kb = 1024ull;
            }
            snprintf(min_free, sizeof(min_free), "%llu\n", min_kb);
            write_text_file(pathbuf, min_free);
        }
    }

    char sys_fs_dir[PATH_MAX];
    if (snprintf(sys_fs_dir, sizeof(sys_fs_dir), "%s/sys/fs", procdir) > 0) {
        pathTruncateEnsureDir(sys_fs_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/file-max", sys_fs_dir) > 0) {
            write_text_file(pathbuf, "1048576\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/inode-nr", sys_fs_dir) > 0) {
            write_text_file(pathbuf, "16384\t0\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/inode-state", sys_fs_dir) > 0) {
            write_text_file(pathbuf, "16384\t0\t0\t0\t0\t0\t0\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/aio-max-nr", sys_fs_dir) > 0) {
            write_text_file(pathbuf, "65536\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/aio-nr", sys_fs_dir) > 0) {
            write_text_file(pathbuf, "0\n");
        }
    }

    char sys_net_core_dir[PATH_MAX];
    if (snprintf(sys_net_core_dir, sizeof(sys_net_core_dir), "%s/sys/net/core", procdir) > 0) {
        pathTruncateEnsureDir(sys_net_core_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/somaxconn", sys_net_core_dir) > 0) {
            write_text_file(pathbuf, "4096\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/rmem_default", sys_net_core_dir) > 0) {
            write_text_file(pathbuf, "212992\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/wmem_default", sys_net_core_dir) > 0) {
            write_text_file(pathbuf, "212992\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/rmem_max", sys_net_core_dir) > 0) {
            write_text_file(pathbuf, "212992\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/wmem_max", sys_net_core_dir) > 0) {
            write_text_file(pathbuf, "212992\n");
        }
    }

    char sys_net_ipv4_dir[PATH_MAX];
    if (snprintf(sys_net_ipv4_dir, sizeof(sys_net_ipv4_dir), "%s/sys/net/ipv4", procdir) > 0) {
        pathTruncateEnsureDir(sys_net_ipv4_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/ip_forward", sys_net_ipv4_dir) > 0) {
            write_text_file(pathbuf, "0\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/tcp_syncookies", sys_net_ipv4_dir) > 0) {
            write_text_file(pathbuf, "1\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/tcp_fin_timeout", sys_net_ipv4_dir) > 0) {
            write_text_file(pathbuf, "60\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/tcp_keepalive_time", sys_net_ipv4_dir) > 0) {
            write_text_file(pathbuf, "7200\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/ip_local_port_range", sys_net_ipv4_dir) > 0) {
            write_text_file(pathbuf, "32768\t60999\n");
        }
    }

    char sys_net_ipv6_all_dir[PATH_MAX];
    if (snprintf(sys_net_ipv6_all_dir, sizeof(sys_net_ipv6_all_dir), "%s/sys/net/ipv6/conf/all", procdir) > 0) {
        pathTruncateEnsureDir(sys_net_ipv6_all_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/forwarding", sys_net_ipv6_all_dir) > 0) {
            write_text_file(pathbuf, "0\n");
        }
    }

    char thread_self_path[PATH_MAX];
    if (snprintf(thread_self_path, sizeof(thread_self_path), "%s/thread-self", procdir) > 0) {
        char target[64];
        snprintf(target, sizeof(target), "%d/task/%d", proc_display_pid, proc_display_pid);
        pathTruncateEnsureSymlink(thread_self_path, target);
    }

    char dtree_dir[PATH_MAX];
    if (snprintf(dtree_dir, sizeof(dtree_dir), "%s/device-tree", procdir) > 0) {
        pathTruncateEnsureDir(dtree_dir);
        char model_path[PATH_MAX];
        if (snprintf(model_path, sizeof(model_path), "%s/model", dtree_dir) > 0) {
            write_text_file(model_path, model[0] ? model : "pscal");
        }
    }

    char info_path[PATH_MAX];
    if (snprintf(info_path, sizeof(info_path), "%s/pscal_env", procdir) > 0) {
        FILE *f = fopen(info_path, "w");
        if (f) {
            const char *pth = getenv("PATH_TRUNCATE");
            const char *home = getenv("HOME");
            fprintf(f, "PATH_TRUNCATE=%s\n", pth ? pth : "");
            fprintf(f, "HOME=%s\n", home ? home : "");
            fclose(f);
        }
    }

    char net_dir[PATH_MAX];
    if (snprintf(net_dir, sizeof(net_dir), "%s/net", procdir) > 0) {
        pathTruncateEnsureDir(net_dir);
        if (request_net) {
            char pathbuf[PATH_MAX];
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/dev", net_dir) > 0) {
                pathTruncateWriteProcNetDev(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/arp", net_dir) > 0) {
                pathTruncateWriteProcNetArp(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/if_inet6", net_dir) > 0) {
                pathTruncateWriteProcNetIfInet6(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/route", net_dir) > 0) {
                pathTruncateWriteProcNetRoute(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/raw", net_dir) > 0) {
                pathTruncateWriteProcNetInet(pathbuf, SOCK_RAW, false);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/raw6", net_dir) > 0) {
                pathTruncateWriteProcNetInet(pathbuf, SOCK_RAW, true);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/tcp", net_dir) > 0) {
                pathTruncateWriteProcNetInet(pathbuf, SOCK_STREAM, false);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/udp", net_dir) > 0) {
                pathTruncateWriteProcNetInet(pathbuf, SOCK_DGRAM, false);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/tcp6", net_dir) > 0) {
                pathTruncateWriteProcNetInet(pathbuf, SOCK_STREAM, true);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/udp6", net_dir) > 0) {
                pathTruncateWriteProcNetInet(pathbuf, SOCK_DGRAM, true);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/unix", net_dir) > 0) {
                pathTruncateWriteProcNetUnix(pathbuf, prefix);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/packet", net_dir) > 0) {
                pathTruncateWriteProcNetPacket(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/sockstat", net_dir) > 0) {
                pathTruncateWriteProcNetSockstat(pathbuf, false);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/sockstat6", net_dir) > 0) {
                pathTruncateWriteProcNetSockstat(pathbuf, true);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/snmp", net_dir) > 0) {
                pathTruncateWriteProcNetSnmp(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/snmp6", net_dir) > 0) {
                pathTruncateWriteProcNetSnmp6(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/netstat", net_dir) > 0) {
                pathTruncateWriteProcNetNetstat(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/protocols", net_dir) > 0) {
                pathTruncateWriteProcNetProtocols(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/wireless", net_dir) > 0) {
                pathTruncateWriteProcNetWireless(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/softnet_stat", net_dir) > 0) {
                pathTruncateWriteProcNetSoftnetStat(pathbuf, ncpu);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/dev_mcast", net_dir) > 0) {
                pathTruncateWriteProcNetDevMcast(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/igmp", net_dir) > 0) {
                pathTruncateWriteProcNetIgmp(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/igmp6", net_dir) > 0) {
                pathTruncateWriteProcNetIgmp6(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/ipv6_route", net_dir) > 0) {
                pathTruncateWriteProcNetIpv6Route(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/rt6_stats", net_dir) > 0) {
                pathTruncateWriteProcNetRt6Stats(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/fib_trie", net_dir) > 0) {
                pathTruncateWriteProcNetFibTrie(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/fib_triestat", net_dir) > 0) {
                pathTruncateWriteProcNetFibTrieStat(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/netlink", net_dir) > 0) {
                pathTruncateWriteProcNetNetlink(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/ptype", net_dir) > 0) {
                pathTruncateWriteProcNetPtype(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/psched", net_dir) > 0) {
                pathTruncateWriteProcNetPsched(pathbuf);
            }
            if (snprintf(pathbuf, sizeof(pathbuf), "%s/xfrm_stat", net_dir) > 0) {
                pathTruncateWriteProcNetXfrmStat(pathbuf);
            }
            char stat_dir[PATH_MAX];
            if (snprintf(stat_dir, sizeof(stat_dir), "%s/stat", net_dir) > 0) {
                pathTruncateEnsureDir(stat_dir);
                if (snprintf(pathbuf, sizeof(pathbuf), "%s/rt_cache", stat_dir) > 0) {
                    pathTruncateWriteProcNetStatTable(
                        pathbuf,
                        "entries in_hit in_slow_tot in_slow_mc in_no_route in_brd in_martian_dst in_martian_src out_hit out_slow_tot out_slow_mc gc_total gc_ignored gc_goal_miss gc_dst_overflow in_hlist_search out_hlist_search",
                        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0");
                }
                if (snprintf(pathbuf, sizeof(pathbuf), "%s/arp_cache", stat_dir) > 0) {
                    pathTruncateWriteProcNetStatTable(
                        pathbuf,
                        "entries allocs destroys hash_grows lookups hits res_failed rcv_probes_mcast rcv_probes_ucast periodic_gc_runs forced_gc_runs unresolved_discards",
                        "0 0 0 0 0 0 0 0 0 0 0 0");
                }
                if (snprintf(pathbuf, sizeof(pathbuf), "%s/ndisc_cache", stat_dir) > 0) {
                    pathTruncateWriteProcNetStatTable(
                        pathbuf,
                        "entries allocs destroys hash_grows lookups hits res_failed rcv_probes_mcast rcv_probes_ucast periodic_gc_runs forced_gc_runs unresolved_discards",
                        "0 0 0 0 0 0 0 0 0 0 0");
                }
            }
        }
    }

    if (request_vm) {
        pathTruncateWriteProcVm(procdir);
    } else {
        char vm_dir[PATH_MAX];
        if (snprintf(vm_dir, sizeof(vm_dir), "%s/vm", procdir) > 0) {
            pathTruncateEnsureDir(vm_dir);
        }
    }

    char pressure_dir[PATH_MAX];
    if (snprintf(pressure_dir, sizeof(pressure_dir), "%s/pressure", procdir) > 0) {
        pathTruncateEnsureDir(pressure_dir);
        char pathbuf[PATH_MAX];
        const char *psi_line = "some avg10=0.00 avg60=0.00 avg300=0.00 total=0\nfull avg10=0.00 avg60=0.00 avg300=0.00 total=0\n";
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/cpu", pressure_dir) > 0) {
            write_text_file(pathbuf, psi_line);
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/memory", pressure_dir) > 0) {
            write_text_file(pathbuf, psi_line);
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/io", pressure_dir) > 0) {
            write_text_file(pathbuf, psi_line);
        }
    }

    char sysvipc_dir[PATH_MAX];
    if (snprintf(sysvipc_dir, sizeof(sysvipc_dir), "%s/sysvipc", procdir) > 0) {
        pathTruncateEnsureDir(sysvipc_dir);
        char pathbuf[PATH_MAX];
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/msg", sysvipc_dir) > 0) {
            pathTruncateWriteProcSysvipcTable(pathbuf,
                                              "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid      stime      rtime      ctime\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/sem", sysvipc_dir) > 0) {
            pathTruncateWriteProcSysvipcTable(pathbuf,
                                              "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n");
        }
        if (snprintf(pathbuf, sizeof(pathbuf), "%s/shm", sysvipc_dir) > 0) {
            pathTruncateWriteProcSysvipcTable(pathbuf,
                                              "       key      shmid perms      size  cpid  lpid nattch   uid   gid  cuid  cgid      atime      dtime      ctime\n");
        }
    }

    if (refresh_pid_entries) {
        pid_t host_ppid = getppid();
        if (host_ppid < 0) {
            host_ppid = 0;
        }

        char device_dir[PATH_MAX];
        if (snprintf(device_dir, sizeof(device_dir), "%s/device", procdir) > 0) {
            pathTruncateEnsureDir(device_dir);
            if (refresh_device_entries) {
                PathTruncateDeviceProcSnapshot device_snapshots[256];
                size_t device_count = pathTruncateSnapshotDeviceProcesses(
                    device_snapshots,
                    sizeof(device_snapshots) / sizeof(device_snapshots[0]));
                bool host_present = false;
                for (size_t i = 0; i < device_count; ++i) {
                    if (device_snapshots[i].pid == (int)host_pid) {
                        host_present = true;
                        break;
                    }
                }
                if (!host_present && device_count < (sizeof(device_snapshots) / sizeof(device_snapshots[0]))) {
                    PathTruncateDeviceProcSnapshot *entry = &device_snapshots[device_count++];
                    entry->pid = (int)host_pid;
                    entry->ppid = (int)host_ppid;
                    snprintf(entry->name, sizeof(entry->name), "%s", "pscal-host");
                }
                int device_keep_pids[256];
                size_t device_keep_count = 0;
                for (size_t i = 0; i < device_count; ++i) {
                    const PathTruncateDeviceProcSnapshot *snapshot = &device_snapshots[i];
                    if (snapshot->pid <= 0) {
                        continue;
                    }
                    pathTruncateWriteProcDevicePidEntry(device_dir,
                                                        (pid_t)snapshot->pid,
                                                        (pid_t)snapshot->ppid,
                                                        snapshot->name[0] ? snapshot->name : "proc",
                                                        mem_total_kb,
                                                        uptime_secs);
                    if (device_keep_count < (sizeof(device_keep_pids) / sizeof(device_keep_pids[0]))) {
                        device_keep_pids[device_keep_count++] = snapshot->pid;
                    }
                }
                bool allow_device_prune = true;
                uint64_t device_prune_interval_ms = g_procDevicePrunePending ? 250 : 2000;
                if (now_ms != 0 && g_procRefreshLastDevicePruneMs != 0 && now_ms > g_procRefreshLastDevicePruneMs) {
                    allow_device_prune = (now_ms - g_procRefreshLastDevicePruneMs) >= device_prune_interval_ms;
                }
                if (allow_device_prune) {
                    g_procDevicePrunePending =
                        pathTruncatePruneNumericDirectoryChildrenByPidList(device_dir,
                                                                           device_keep_pids,
                                                                           device_keep_count,
                                                                           64);
                    if (now_ms != 0) {
                        g_procRefreshLastDevicePruneMs = now_ms;
                    }
                }
            }
            char device_self_path[PATH_MAX];
            if (snprintf(device_self_path, sizeof(device_self_path), "%s/self", device_dir) > 0) {
                char pid_link[32];
                snprintf(pid_link, sizeof(pid_link), "%d", (int)host_pid);
                pathTruncateEnsureSymlink(device_self_path, pid_link);
            }
        }

        bool wrote_vproc = false;
        bool host_pid_is_vproc = false;
        for (size_t i = 0; i < vproc_snapshot_count; ++i) {
            const PathTruncateVProcSnapshot *snapshot = &vproc_snapshots[i];
            if (snapshot->pid <= 0) {
                continue;
            }
            if (snapshot->pid == (int)host_pid) {
                host_pid_is_vproc = true;
            }
            const char *name = snapshot->command[0] ? snapshot->command :
                               (snapshot->comm[0] ? snapshot->comm : "vproc");
            int parent_pid = snapshot->parent_pid;
            if (parent_pid < 0) {
                parent_pid = 0;
            }
            pathTruncateWriteProcPidEntries(procdir,
                                            prefix,
                                            (pid_t)snapshot->pid,
                                            (pid_t)parent_pid,
                                            name,
                                            mem_total_kb,
                                            uptime_secs);
            wrote_vproc = true;
        }

        if (!wrote_vproc && current_vproc_pid > 0) {
            pathTruncateWriteProcPidEntries(procdir,
                                            prefix,
                                            (pid_t)current_vproc_pid,
                                            0,
                                            "vproc",
                                            mem_total_kb,
                                            uptime_secs);
            wrote_vproc = true;
            if (current_vproc_pid == (int)host_pid) {
                host_pid_is_vproc = true;
            }
        }
        if (!wrote_vproc) {
            pathTruncateWriteProcPidEntries(procdir,
                                            prefix,
                                            host_pid,
                                            host_ppid,
                                            "proc",
                                            mem_total_kb,
                                            uptime_secs);
            wrote_vproc = true;
            host_pid_is_vproc = true;
        }

        char self_path[PATH_MAX];
        if (snprintf(self_path, sizeof(self_path), "%s/self", procdir) > 0) {
            char pid_link[32];
            int self_link_pid = wrote_vproc ? proc_display_pid : (int)host_pid;
            snprintf(pid_link, sizeof(pid_link), "%d", self_link_pid);
            pathTruncateEnsureSymlink(self_path, pid_link);
        }

        int vproc_keep_pids[512];
        size_t vproc_keep_count = 0;
        if (wrote_vproc) {
            for (size_t i = 0; i < vproc_snapshot_count; ++i) {
                if (vproc_snapshots[i].pid > 0 &&
                    vproc_keep_count < (sizeof(vproc_keep_pids) / sizeof(vproc_keep_pids[0]))) {
                    vproc_keep_pids[vproc_keep_count++] = vproc_snapshots[i].pid;
                }
            }
            if (vproc_snapshot_count == 0 && current_vproc_pid > 0 &&
                vproc_keep_count < (sizeof(vproc_keep_pids) / sizeof(vproc_keep_pids[0]))) {
                vproc_keep_pids[vproc_keep_count++] = current_vproc_pid;
            }
        }
        bool allow_vproc_prune = true;
        uint64_t vproc_prune_interval_ms = g_procPrunePending ? 250 : 2000;
        if (now_ms != 0 && g_procRefreshLastPruneMs != 0 && now_ms > g_procRefreshLastPruneMs) {
            allow_vproc_prune = (now_ms - g_procRefreshLastPruneMs) >= vproc_prune_interval_ms;
        }
        if (allow_vproc_prune) {
            g_procPrunePending = pathTruncatePruneNumericDirectoryChildrenByPidList(procdir,
                                                                                     vproc_keep_pids,
                                                                                     vproc_keep_count,
                                                                                     64);
            if (now_ms != 0) {
                g_procRefreshLastPruneMs = now_ms;
            }
        }

        if (!host_pid_is_vproc) {
            char legacy_host_dir[PATH_MAX];
            if (snprintf(legacy_host_dir, sizeof(legacy_host_dir), "%s/%d", procdir, (int)host_pid) > 0) {
                pathTruncateRemoveTree(legacy_host_dir);
            }
        }
    }
    g_procBaseSeeded = true;
}

static void pathTruncateResetCaches(void) {
    g_pathTruncatePrimary[0] = '\0';
    g_pathTruncatePrimaryLen = 0;
    g_pathTruncateAlias[0] = '\0';
    g_pathTruncateAliasLen = 0;
    g_procRefreshLastFullMs = 0;
    g_procRefreshLastNetMs = 0;
    g_procRefreshLastDeviceMs = 0;
    g_procRefreshLastVmMs = 0;
    g_procRefreshLastPruneMs = 0;
    g_procRefreshLastDevicePruneMs = 0;
    g_procBaseSeeded = false;
    g_procPrunePending = false;
    g_procDevicePrunePending = false;
}

static void pathTruncateEnsureDir(const char *path) {
    if (!path || *path == '\0') {
        return;
    }
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len + 1 > sizeof(buf)) {
        return;
    }
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buf, 0777);
            *p = '/';
        }
    }
    mkdir(buf, 0777);
}

static bool pathTruncateNormalizeAbsolute(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size < 2) {
        errno = EINVAL;
        return false;
    }
    if (input[0] != '/') {
        errno = EINVAL;
        return false;
    }

    size_t anchors[PATH_MAX / 2];
    size_t depth = 0;
    size_t length = 1;
    out[0] = '/';
    out[1] = '\0';

    const char *cursor = input;
    while (*cursor == '/') {
        cursor++;
    }
    while (*cursor != '\0') {
        const char *segment_start = cursor;
        while (*cursor != '\0' && *cursor != '/') {
            cursor++;
        }
        size_t segment_len = (size_t)(cursor - segment_start);
        while (*cursor == '/') {
            cursor++;
        }
        if (segment_len == 0) {
            continue;
        }
        if (segment_len == 1 && segment_start[0] == '.') {
            continue;
        }
        if (segment_len == 2 && segment_start[0] == '.' && segment_start[1] == '.') {
            if (depth > 0) {
                depth--;
                length = anchors[depth];
                out[length] = '\0';
            }
            continue;
        }
        if (depth >= sizeof(anchors) / sizeof(anchors[0])) {
            errno = ENAMETOOLONG;
            return false;
        }
        if (length > 1) {
            if (length + 1 >= out_size) {
                errno = ENAMETOOLONG;
                return false;
            }
            out[length++] = '/';
        }
        size_t segment_start_pos = length;
        if (length + segment_len >= out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(out + length, segment_start, segment_len);
        length += segment_len;
        out[length] = '\0';

        size_t anchor = (segment_start_pos > 1) ? (segment_start_pos - 1) : 1;
        anchors[depth++] = anchor;
    }

    return true;
}

static void pathTruncateStorePrefix(const char *source, size_t length) {
    if (length >= sizeof(g_pathTruncatePrimary)) {
        length = sizeof(g_pathTruncatePrimary) - 1;
    }
    memcpy(g_pathTruncatePrimary, source, length);
    g_pathTruncatePrimary[length] = '\0';
    g_pathTruncatePrimaryLen = length;

    g_pathTruncateAliasLen = 0;
    g_pathTruncateAlias[0] = '\0';
    const char prefix_private[] = "/private";
    size_t private_len = sizeof(prefix_private) - 1;
    if (length > private_len && strncmp(g_pathTruncatePrimary, prefix_private, private_len) == 0) {
        size_t alias_len = length - private_len;
        if (alias_len < sizeof(g_pathTruncateAlias)) {
            memcpy(g_pathTruncateAlias, g_pathTruncatePrimary + private_len, alias_len);
            g_pathTruncateAlias[alias_len] = '\0';
            g_pathTruncateAliasLen = alias_len;
        }
        return;
    }
    const char prefix_var[] = "/var";
    size_t var_len = sizeof(prefix_var) - 1;
    if (length >= var_len &&
        strncmp(g_pathTruncatePrimary, prefix_var, var_len) == 0 &&
        (length == var_len || g_pathTruncatePrimary[var_len] == '/')) {
        size_t alias_len = private_len + length;
        if (alias_len < sizeof(g_pathTruncateAlias)) {
            memcpy(g_pathTruncateAlias, prefix_private, private_len);
            memcpy(g_pathTruncateAlias + private_len, g_pathTruncatePrimary, length);
            g_pathTruncateAlias[alias_len] = '\0';
            g_pathTruncateAliasLen = alias_len;
        }
    }
}

static bool pathTruncateFetchPrefix(const char **out_prefix, size_t *out_length) {
    if (!out_prefix || !out_length) {
        return false;
    }
    const char *disabled = getenv("PSCALI_PATH_TRUNCATE_DISABLED");
    if (disabled && disabled[0] != '\0') {
        return false;
    }
    const char *env = getenv("PATH_TRUNCATE");
    if (!env || env[0] == '\0') {
        env = getenv("PSCALI_CONTAINER_ROOT");
    }
    if (!env || env[0] == '\0') {
        env = getenv("HOME");
    }
    if (!env || env[0] != '/') {
        return false;
    }
    while (*env == ' ' || *env == '\t') {
        env++;
    }
    if (*env == '\0' || env[0] != '/') {
        return false;
    }
    const char *source = env;
    char canonical[PATH_MAX];
    if (!g_pathTruncateResolving) {
        g_pathTruncateResolving = 1;
        if (realpath(env, canonical)) {
            source = canonical;
        }
        g_pathTruncateResolving = 0;
    }
    size_t length = strlen(source);
    while (length > 1 && source[length - 1] == '/') {
        length--;
    }
    if (length == 0) {
        return false;
    }
    pathTruncateStorePrefix(source, length);
    if (g_pathTruncatePrimaryLen == 1 && g_pathTruncatePrimary[0] == '/') {
        /* A PATH_TRUNCATE of "/" is not useful; fall back to the sandbox home. */
        const char *home = getenv("HOME");
        if (home && home[0] == '/') {
            size_t home_len = strlen(home);
            while (home_len > 1 && home[home_len - 1] == '/') {
                home_len--;
            }
            if (home_len > 0) {
                pathTruncateStorePrefix(home, home_len);
            }
        }
    }
    *out_prefix = g_pathTruncatePrimary;
    *out_length = g_pathTruncatePrimaryLen;
    return true;
}

static bool pathTruncateMatchesStoredPrefix(const char *path,
                                            size_t path_len,
                                            const char **matched_prefix,
                                            size_t *matched_len) {
    if (!path || path_len == 0 || g_pathTruncatePrimaryLen == 0) {
        return false;
    }
    if (path_len >= g_pathTruncatePrimaryLen &&
        strncmp(path, g_pathTruncatePrimary, g_pathTruncatePrimaryLen) == 0 &&
        (path[g_pathTruncatePrimaryLen] == '\0' || path[g_pathTruncatePrimaryLen] == '/')) {
        if (matched_prefix) {
            *matched_prefix = g_pathTruncatePrimary;
        }
        if (matched_len) {
            *matched_len = g_pathTruncatePrimaryLen;
        }
        return true;
    }
    if (g_pathTruncateAliasLen > 0 &&
        path_len >= g_pathTruncateAliasLen &&
        strncmp(path, g_pathTruncateAlias, g_pathTruncateAliasLen) == 0 &&
        (path[g_pathTruncateAliasLen] == '\0' || path[g_pathTruncateAliasLen] == '/')) {
        if (matched_prefix) {
            *matched_prefix = g_pathTruncateAlias;
        }
        if (matched_len) {
            *matched_len = g_pathTruncateAliasLen;
        }
        return true;
    }
    return false;
}

bool pathTruncateEnabled(void) {
    pathTruncateLock();
    const char *prefix = NULL;
    size_t length = 0;
    bool enabled = pathTruncateFetchPrefix(&prefix, &length);
    pathTruncateUnlock();
    return enabled;
}

static bool pathTruncateCopyString(const char *input, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        errno = EINVAL;
        return false;
    }
    const char *source = input ? input : "";
    size_t length = strlen(source);
    if (length >= out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    memcpy(out, source, length + 1);
    return true;
}

bool pathTruncateStrip(const char *absolute_path, char *out, size_t out_size) {
    pathTruncateLock();
    if (!out || out_size == 0) {
        pathTruncateUnlock();
        errno = EINVAL;
        return false;
    }
    if (!absolute_path) {
        bool res = pathTruncateCopyString("", out, out_size);
        pathTruncateUnlock();
        return res;
    }
    const char *prefix = NULL;
    size_t prefix_len = 0;
    if (!pathTruncateFetchPrefix(&prefix, &prefix_len)) {
        bool res = pathTruncateCopyString(absolute_path, out, out_size);
        pathTruncateUnlock();
        return res;
    }

    const char *source_path = absolute_path;
    char normalized[PATH_MAX];
    if (absolute_path[0] == '/' &&
        pathTruncateNormalizeAbsolute(absolute_path, normalized, sizeof(normalized))) {
        source_path = normalized;
    }

    size_t source_len = strlen(source_path);
    const char *matched_prefix = NULL;
    size_t matched_len = 0;
    if (!pathTruncateMatchesStoredPrefix(source_path, source_len, &matched_prefix, &matched_len)) {
        bool res = pathTruncateCopyString(source_path, out, out_size);
        pathTruncateUnlock();
        return res;
    }

    const char *remainder = source_path + matched_len;
    while (*remainder == '/') {
        remainder++;
    }
    if (*remainder == '\0') {
        bool res = pathTruncateCopyString("/", out, out_size);
        pathTruncateUnlock();
        return res;
    }
    int written = snprintf(out, out_size, "/%s", remainder);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        pathTruncateUnlock();
        return false;
    }
    pathTruncateUnlock();
    return true;
}

void pathTruncateApplyEnvironment(const char *prefix) {
    pathTruncateLock();
    if (prefix && prefix[0] == '/') {
        setenv("PATH_TRUNCATE", prefix, 1);
        /* Seed common root directories so path virtualization has writable parents. */
        char tmpbuf[PATH_MAX];
        int written = snprintf(tmpbuf, sizeof(tmpbuf), "%s/tmp", prefix);
        if (written > 0 && (size_t)written < sizeof(tmpbuf)) {
            pathTruncateEnsureDir(tmpbuf);
        }
        written = snprintf(tmpbuf, sizeof(tmpbuf), "%s/var/tmp", prefix);
        if (written > 0 && (size_t)written < sizeof(tmpbuf)) {
            pathTruncateEnsureDir(tmpbuf);
        }
        written = snprintf(tmpbuf, sizeof(tmpbuf), "%s/var/log", prefix);
        if (written > 0 && (size_t)written < sizeof(tmpbuf)) {
            pathTruncateEnsureDir(tmpbuf);
        }
        written = snprintf(tmpbuf, sizeof(tmpbuf), "%s/var/local", prefix);
        if (written > 0 && (size_t)written < sizeof(tmpbuf)) {
            pathTruncateEnsureDir(tmpbuf);
        }
        /* Seed emulated /dev with symlinks to system devices. */
        pathTruncateProvisionDev(prefix);
        /* Seed a minimal /proc tree with cpuinfo. */
        pathTruncateProvisionProc(prefix);
        /* Seed a virtual /usr/bin catalog for frontends and smallclue applets. */
        pathTruncateProvisionUsrBin(prefix);
    } else {
        unsetenv("PATH_TRUNCATE");
    }
    pathTruncateResetCaches();
    pathTruncateUnlock();
}

void pathTruncateProvisionDev(const char *prefix) {
    if (!prefix || *prefix != '/') {
        return;
    }
    char devdir[PATH_MAX];
    int written = snprintf(devdir, sizeof(devdir), "%s/dev", prefix);
    if (written <= 0 || (size_t)written >= sizeof(devdir)) {
        return;
    }
    pathTruncateEnsureDir(devdir);
    const struct {
        const char *name;
        const char *target;
    } links[] = {
        { "null", "/dev/null" },
        { "zero", "/dev/zero" },
        { "random", "/dev/random" },
        { "ptmx", "/dev/null" }
    };
    for (size_t i = 0; i < sizeof(links) / sizeof(links[0]); ++i) {
        char link_path[PATH_MAX];
        if (snprintf(link_path, sizeof(link_path), "%s/%s", devdir, links[i].name) >= (int)sizeof(link_path)) {
            continue;
        }
        struct stat st;
        if (lstat(link_path, &st) == 0) {
            continue; // already exists
        }
        symlink(links[i].target, link_path);
    }

    char ptsdir[PATH_MAX];
    if (snprintf(ptsdir, sizeof(ptsdir), "%s/pts", devdir) > 0 &&
        (size_t)strlen(ptsdir) < sizeof(ptsdir)) {
        pathTruncateEnsureDir(ptsdir);
    }
}

void pathTruncateProvisionProc(const char *prefix) {
    pathTruncateRefreshProc(prefix, "/proc");
}

static const char *pathTruncateSkipLeadingSlashes(const char *input) {
    const char *cursor = input;
    while (*cursor == '/') {
        cursor++;
    }
    return cursor;
}

static bool pathTruncatePrefixMatch(const char *path, const char *prefix, size_t prefix_len) {
    if (!path || !prefix || prefix_len == 0) {
        return false;
    }
    return strncmp(path, prefix, prefix_len) == 0 &&
           (path[prefix_len] == '\0' || path[prefix_len] == '/');
}

static bool pathTruncateMatchesEnvRoot(const char *path, const char *env_name) {
    const char *root = getenv(env_name);
    if (!root || root[0] != '/') {
        return false;
    }
    size_t len = strlen(root);
    while (len > 1 && root[len - 1] == '/') {
        len--;
    }
    if (len == 0) {
        return false;
    }
    if (pathTruncatePrefixMatch(path, root, len)) {
        return true;
    }
    const char *private_prefix = "/private";
    size_t private_len = strlen(private_prefix);
    if (len > private_len && strncmp(root, private_prefix, private_len) == 0) {
        if (pathTruncatePrefixMatch(path, root + private_len, len - private_len)) {
            return true;
        }
    } else if (strncmp(path, private_prefix, private_len) == 0) {
        if (pathTruncatePrefixMatch(path + private_len, root, len)) {
            return true;
        }
    }
    return false;
}

static bool pathTruncateIsSystemPath(const char *path) {
    if (!path || path[0] != '/') {
        return false;
    }
    if (pathTruncatePathIsUsrBinTree(path)) {
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
    const char *env_roots[] = {
        "PSCALI_INSTALL_ROOT",
        "PSCAL_INSTALL_ROOT",
        "PSCAL_INSTALL_ROOT_RESOLVED",
        "PASCAL_LIB_DIR",
        "CLIKE_LIB_DIR",
        "REA_LIB_DIR",
        "PSCALI_ETC_ROOT",
        "PSCALI_DOCS_ROOT",
        "PSCAL_EXAMPLES_ROOT",
        "PSCALI_SYSFILES_ROOT"
    };
    for (size_t i = 0; i < sizeof(env_roots) / sizeof(env_roots[0]); ++i) {
        if (pathTruncateMatchesEnvRoot(path, env_roots[i])) {
            return true;
        }
    }
    return false;
}

bool pathTruncateExpand(const char *input_path, char *out, size_t out_size) {
    pathTruncateLock();
    if (!out || out_size == 0) {
        pathTruncateUnlock();
        errno = EINVAL;
        return false;
    }
    if (!input_path) {
        bool res = pathTruncateCopyString("", out, out_size);
        pathTruncateUnlock();
        return res;
    }
    const char *prefix = NULL;
    size_t prefix_len = 0;
    if (!pathTruncateFetchPrefix(&prefix, &prefix_len) || input_path[0] != '/') {
        bool res = pathTruncateCopyString(input_path, out, out_size);
        pathTruncateUnlock();
        return res;
    }
    if ((strncmp(input_path, "/etc", 4) == 0 &&
         (input_path[4] == '\0' || input_path[4] == '/')) ||
        (strncmp(input_path, "/private/etc", 12) == 0 &&
         (input_path[12] == '\0' || input_path[12] == '/'))) {
        const char *etc_root = getenv("PSCALI_ETC_ROOT");
        if (!etc_root || etc_root[0] != '/') {
            bool res = pathTruncateCopyString(input_path, out, out_size);
            pathTruncateUnlock();
            return res;
        }
    }
    if (pathTruncateIsSystemPath(input_path)) {
        bool res = pathTruncateCopyString(input_path, out, out_size);
        pathTruncateUnlock();
        return res;
    }
    /* Device nodes: leave untouched so they resolve to the real device. */
    if (strcmp(input_path, "/dev/null") == 0 ||
        strcmp(input_path, "/dev/zero") == 0 ||
        strcmp(input_path, "/dev/random") == 0) {
        bool res = pathTruncateCopyString(input_path, out, out_size);
        pathTruncateUnlock();
        return res;
    }

    const char *source_path = input_path;
    char normalized[PATH_MAX];
    if (input_path[0] == '/' &&
        pathTruncateNormalizeAbsolute(input_path, normalized, sizeof(normalized))) {
        source_path = normalized;
    }
    if (pathTruncateIsProcRequestPath(source_path)) {
        pathTruncateRefreshProc(prefix, source_path);
    }

    /* Map /etc and /private/etc to the sandbox etc root when provided. This
     * keeps dictionary and passwd/group lookups inside the app container even
     * when PATH_TRUNCATE would otherwise prepend the container root directly. */
    const char *etc_root = getenv("PSCALI_ETC_ROOT");
    if (etc_root && etc_root[0] == '/') {
        const char *etc_suffix = NULL;
        if (strncmp(source_path, "/etc", 4) == 0 &&
            (source_path[4] == '/' || source_path[4] == '\0')) {
            etc_suffix = source_path + 4; /* keep leading slash after /etc */
        } else if (strncmp(source_path, "/private/etc", 12) == 0 &&
                   (source_path[12] == '/' || source_path[12] == '\0')) {
            etc_suffix = source_path + 12;
        }
        if (etc_suffix) {
            size_t root_len = strlen(etc_root);
            size_t suffix_len = strlen(etc_suffix);
            if (root_len + suffix_len + 1 <= out_size) {
                memcpy(out, etc_root, root_len);
                memcpy(out + root_len, etc_suffix, suffix_len);
                out[root_len + suffix_len] = '\0';
                pathTruncateUnlock();
                return true;
            }
            errno = ENAMETOOLONG;
            pathTruncateUnlock();
            return false;
        }
    }

    size_t source_len = strlen(source_path);
    const char *matched_prefix = NULL;
    size_t matched_len = 0;
    if (pathTruncateMatchesStoredPrefix(source_path, source_len, &matched_prefix, &matched_len)) {
        if (matched_prefix == g_pathTruncatePrimary) {
            bool res = pathTruncateCopyString(source_path, out, out_size);
            pathTruncateUnlock();
            return res;
        }
        size_t suffix_len = source_len - matched_len;
        if (g_pathTruncatePrimaryLen + suffix_len + 1 > out_size) {
            errno = ENAMETOOLONG;
            pathTruncateUnlock();
            return false;
        }
        memcpy(out, g_pathTruncatePrimary, g_pathTruncatePrimaryLen);
        memcpy(out + g_pathTruncatePrimaryLen, source_path + matched_len, suffix_len);
        out[g_pathTruncatePrimaryLen + suffix_len] = '\0';
        pathTruncateUnlock();
        return true;
    }

    const char *trimmed = pathTruncateSkipLeadingSlashes(source_path);
    if (*trimmed == '\0') {
        if (g_pathTruncatePrimaryLen >= out_size) {
            errno = ENAMETOOLONG;
            pathTruncateUnlock();
            return false;
        }
        memcpy(out, g_pathTruncatePrimary, g_pathTruncatePrimaryLen);
        out[g_pathTruncatePrimaryLen] = '\0';
        pathTruncateUnlock();
        return true;
    }
    int written = snprintf(out, out_size, "%.*s/%s", (int)g_pathTruncatePrimaryLen, g_pathTruncatePrimary, trimmed);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        pathTruncateUnlock();
        return false;
    }
    pathTruncateUnlock();
    return true;
}
