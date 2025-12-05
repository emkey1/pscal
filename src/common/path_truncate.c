#include "common/path_truncate.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

static char g_pathTruncatePrimary[PATH_MAX];
static size_t g_pathTruncatePrimaryLen = 0;
static char g_pathTruncateAlias[PATH_MAX];
static size_t g_pathTruncateAliasLen = 0;

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
    }
}

static bool pathTruncateFetchPrefix(const char **out_prefix, size_t *out_length) {
    if (!out_prefix || !out_length) {
        return false;
    }
    const char *env = getenv("PATH_TRUNCATE");
    if (!env) {
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
    if (realpath(env, canonical)) {
        source = canonical;
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
    const char *prefix = NULL;
    size_t length = 0;
    return pathTruncateFetchPrefix(&prefix, &length);
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
    if (!out || out_size == 0) {
        errno = EINVAL;
        return false;
    }
    if (!absolute_path) {
        return pathTruncateCopyString("", out, out_size);
    }
    const char *prefix = NULL;
    size_t prefix_len = 0;
    if (!pathTruncateFetchPrefix(&prefix, &prefix_len)) {
        return pathTruncateCopyString(absolute_path, out, out_size);
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
        return pathTruncateCopyString(source_path, out, out_size);
    }

    const char *remainder = source_path + matched_len;
    while (*remainder == '/') {
        remainder++;
    }
    if (*remainder == '\0') {
        return pathTruncateCopyString("/", out, out_size);
    }
    int written = snprintf(out, out_size, "/%s", remainder);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

void pathTruncateApplyEnvironment(const char *prefix) {
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
        /* Seed emulated /dev with symlinks to system devices. */
        pathTruncateProvisionDev(prefix);
    } else {
        unsetenv("PATH_TRUNCATE");
    }
    pathTruncateResetCaches();
}

static const char *pathTruncateSkipLeadingSlashes(const char *input) {
    const char *cursor = input;
    while (*cursor == '/') {
        cursor++;
    }
    return cursor;
}

bool pathTruncateExpand(const char *input_path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        errno = EINVAL;
        return false;
    }
    if (!input_path) {
        return pathTruncateCopyString("", out, out_size);
    }
    const char *prefix = NULL;
    size_t prefix_len = 0;
    if (!pathTruncateFetchPrefix(&prefix, &prefix_len) || input_path[0] != '/') {
        return pathTruncateCopyString(input_path, out, out_size);
    }
    /* Map /dev/null and /dev/zero into the sandboxed /dev so they always exist. */
    if (strcmp(input_path, "/dev/null") == 0 || strcmp(input_path, "/dev/zero") == 0) {
        const char *leaf = input_path + 5; // skip "/dev/"
        int written = snprintf(out, out_size, "%s/dev/%s", prefix, leaf);
        if (written < 0 || (size_t)written >= out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        return true;
    }

    const char *source_path = input_path;
    char normalized[PATH_MAX];
    if (input_path[0] == '/' &&
        pathTruncateNormalizeAbsolute(input_path, normalized, sizeof(normalized))) {
        source_path = normalized;
    }

    size_t source_len = strlen(source_path);
    const char *matched_prefix = NULL;
    size_t matched_len = 0;
    if (pathTruncateMatchesStoredPrefix(source_path, source_len, &matched_prefix, &matched_len)) {
        if (matched_prefix == g_pathTruncatePrimary) {
            return pathTruncateCopyString(source_path, out, out_size);
        }
        size_t suffix_len = source_len - matched_len;
        if (g_pathTruncatePrimaryLen + suffix_len + 1 > out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(out, g_pathTruncatePrimary, g_pathTruncatePrimaryLen);
        memcpy(out + g_pathTruncatePrimaryLen, source_path + matched_len, suffix_len);
        out[g_pathTruncatePrimaryLen + suffix_len] = '\0';
        return true;
    }

    const char *trimmed = pathTruncateSkipLeadingSlashes(source_path);
    if (*trimmed == '\0') {
        if (g_pathTruncatePrimaryLen >= out_size) {
            errno = ENAMETOOLONG;
            return false;
        }
        memcpy(out, g_pathTruncatePrimary, g_pathTruncatePrimaryLen);
        out[g_pathTruncatePrimaryLen] = '\0';
        return true;
    }
    int written = snprintf(out, out_size, "%.*s/%s", (int)g_pathTruncatePrimaryLen, g_pathTruncatePrimary, trimmed);
    if (written < 0 || (size_t)written >= out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}
