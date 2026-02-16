#include "common/path_truncate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/utsname.h>
#include <sys/mount.h>
#endif
#include <time.h>
#include <pthread.h>

static char g_pathTruncatePrimary[PATH_MAX];
static size_t g_pathTruncatePrimaryLen = 0;
static char g_pathTruncateAlias[PATH_MAX];
static size_t g_pathTruncateAliasLen = 0;
static _Thread_local int g_pathTruncateResolving = 0;
static pthread_mutex_t g_pathTruncateMutex = PTHREAD_MUTEX_INITIALIZER;

static inline void pathTruncateLock(void)   { pthread_mutex_lock(&g_pathTruncateMutex); }
static inline void pathTruncateUnlock(void) { pthread_mutex_unlock(&g_pathTruncateMutex); }

static void write_text_file(const char *path, const char *contents) {
    if (!path || !contents) {
        return;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    fputs(contents, f);
    fclose(f);
}

static void pathTruncateResetCaches(void) {
    g_pathTruncatePrimary[0] = '\0';
    g_pathTruncatePrimaryLen = 0;
    g_pathTruncateAlias[0] = '\0';
    g_pathTruncateAliasLen = 0;
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
    if (!prefix || *prefix != '/') {
        return;
    }
    char procdir[PATH_MAX];
    int written = snprintf(procdir, sizeof(procdir), "%s/proc", prefix);
    if (written <= 0 || (size_t)written >= sizeof(procdir)) {
        return;
    }
    pathTruncateEnsureDir(procdir);

    char cpuinfo_path[PATH_MAX];
    written = snprintf(cpuinfo_path, sizeof(cpuinfo_path), "%s/cpuinfo", procdir);
    if (written > 0 && (size_t)written < sizeof(cpuinfo_path)) {
        FILE *f = fopen(cpuinfo_path, "w");
        if (f) {
            int ncpu = 1;
            uint64_t freq = 0;
            char machine[128] = {0};
            char model[128] = {0};
    #if defined(__APPLE__)
            size_t sz = sizeof(ncpu);
            sysctlbyname("hw.ncpu", &ncpu, &sz, NULL, 0);
            sz = sizeof(freq);
            sysctlbyname("hw.cpufrequency", &freq, &sz, NULL, 0);
            sz = sizeof(machine);
            sysctlbyname("hw.machine", machine, &sz, NULL, 0);
            sz = sizeof(model);
            sysctlbyname("hw.model", model, &sz, NULL, 0);
    #endif
            if (ncpu <= 0) ncpu = 1;
            for (int i = 0; i < ncpu; ++i) {
                fprintf(f, "processor\t: %d\n", i);
                fprintf(f, "model name\t: PSCAL virtual CPU\n");
                if (freq > 0) {
                    double mhz = (double)freq / 1e6;
                    fprintf(f, "cpu MHz\t\t: %.0f\n", mhz);
                }
                fprintf(f, "Hardware\t: %s %s\n",
                        machine[0] ? machine : "arm64",
                        model[0] ? model : "");
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }

    /* /proc/meminfo */
    char meminfo_path[PATH_MAX];
    written = snprintf(meminfo_path, sizeof(meminfo_path), "%s/meminfo", procdir);
    if (written > 0 && (size_t)written < sizeof(meminfo_path)) {
        FILE *f = fopen(meminfo_path, "w");
        if (f) {
            uint64_t mem_bytes = 0;
    #if defined(__APPLE__)
            size_t sz = sizeof(mem_bytes);
            sysctlbyname("hw.memsize", &mem_bytes, &sz, NULL, 0);
    #endif
            unsigned long mem_kb = (unsigned long)(mem_bytes / 1024);
            fprintf(f, "MemTotal:       %lu kB\n", mem_kb);
            fprintf(f, "MemFree:        %lu kB\n", mem_kb / 4);   /* placeholder */
            fprintf(f, "MemAvailable:   %lu kB\n", mem_kb / 2);   /* placeholder */
            fprintf(f, "Buffers:        0 kB\n");
            fprintf(f, "Cached:         0 kB\n");
            fprintf(f, "SwapCached:     0 kB\n");
            fprintf(f, "SwapTotal:      0 kB\n");
            fprintf(f, "SwapFree:       0 kB\n");
            fclose(f);
        }
    }

    /* /proc/uptime */
    char uptime_path[PATH_MAX];
    written = snprintf(uptime_path, sizeof(uptime_path), "%s/uptime", procdir);
    if (written > 0 && (size_t)written < sizeof(uptime_path)) {
        FILE *f = fopen(uptime_path, "w");
        if (f) {
            struct timespec ts;
            double secs = 0.0;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
                secs = ts.tv_sec + ts.tv_nsec / 1e9;
            }
            fprintf(f, "%.2f %.2f\n", secs, secs); /* idle time placeholder */
            fclose(f);
        }
    }

    /* /proc/version */
    char version_path[PATH_MAX];
    written = snprintf(version_path, sizeof(version_path), "%s/version", procdir);
    if (written > 0 && (size_t)written < sizeof(version_path)) {
        struct utsname un;
        uname(&un);
        char buf[256];
        snprintf(buf, sizeof(buf), "PSCALI %s %s %s\n", un.sysname, un.release, un.version);
        write_text_file(version_path, buf);
    }

    /* /proc/cmdline */
    char cmdline_path[PATH_MAX];
    written = snprintf(cmdline_path, sizeof(cmdline_path), "%s/cmdline", procdir);
    if (written > 0 && (size_t)written < sizeof(cmdline_path)) {
        write_text_file(cmdline_path, "pscal sandbox\n");
    }

    /* /proc/sys/kernel/hostname (best-effort) */
    char sysdir[PATH_MAX];
    written = snprintf(sysdir, sizeof(sysdir), "%s/sys/kernel", procdir);
    if (written > 0 && (size_t)written < sizeof(sysdir)) {
        pathTruncateEnsureDir(sysdir);
        char hostbuf[256] = "pscal";
        if (gethostname(hostbuf, sizeof(hostbuf) - 1) != 0 || hostbuf[0] == '\0') {
            strcpy(hostbuf, "pscal");
        }
        char hostname_path[PATH_MAX];
        if (snprintf(hostname_path, sizeof(hostname_path), "%s/hostname", sysdir) > 0) {
            write_text_file(hostname_path, hostbuf);
        }
    }

    /* /proc/device-tree/model (best-effort) */
    char dtree_dir[PATH_MAX];
    written = snprintf(dtree_dir, sizeof(dtree_dir), "%s/device-tree", procdir);
    if (written > 0 && (size_t)written < sizeof(dtree_dir)) {
        pathTruncateEnsureDir(dtree_dir);
        char model_path[PATH_MAX];
        if (snprintf(model_path, sizeof(model_path), "%s/model", dtree_dir) > 0) {
            char model[128] = "pscal";
#if defined(__APPLE__)
            size_t sz = sizeof(model);
            sysctlbyname("hw.model", model, &sz, NULL, 0);
#endif
            write_text_file(model_path, model);
        }
    }

    /* /proc/pscal_env: sandbox info */
    char info_path[PATH_MAX];
    written = snprintf(info_path, sizeof(info_path), "%s/pscal_env", procdir);
    if (written > 0 && (size_t)written < sizeof(info_path)) {
        FILE *f = fopen(info_path, "w");
        if (f) {
            const char *pth = getenv("PATH_TRUNCATE");
            const char *home = getenv("HOME");
            fprintf(f, "PATH_TRUNCATE=%s\n", pth ? pth : "");
            fprintf(f, "HOME=%s\n", home ? home : "");
            fclose(f);
        }
    }

    /* /proc/stat */
    char stat_path[PATH_MAX];
    written = snprintf(stat_path, sizeof(stat_path), "%s/stat", procdir);
    if (written > 0 && (size_t)written < sizeof(stat_path)) {
        FILE *f = fopen(stat_path, "w");
        if (f) {
            fprintf(f, "cpu  1 1 1 1 0 0 0 0 0 0\n");
            fprintf(f, "intr 0\n");
            fprintf(f, "ctxt 0\n");
            fprintf(f, "btime %ld\n", (long)time(NULL));
            fprintf(f, "processes 0\n");
            fprintf(f, "procs_running 0\n");
            fprintf(f, "procs_blocked 0\n");
            fclose(f);
        }
    }

    /* /proc/mounts */
    char mounts_path[PATH_MAX];
    written = snprintf(mounts_path, sizeof(mounts_path), "%s/mounts", procdir);
    if (written > 0 && (size_t)written < sizeof(mounts_path)) {
        FILE *f = fopen(mounts_path, "w");
        if (f) {
            struct statfs sfs;
            if (statfs("/", &sfs) == 0) {
                fprintf(f, "%s / %s rw 0 0\n",
                        sfs.f_mntfromname[0] ? sfs.f_mntfromname : "rootfs",
                        sfs.f_fstypename[0] ? sfs.f_fstypename : "ext4");
            } else {
                fprintf(f, "rootfs / ext4 rw 0 0\n");
            }
            fclose(f);
        }
    }

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
