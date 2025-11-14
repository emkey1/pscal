#include "pscal_paths.h"

#if defined(PSCAL_TARGET_IOS)

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *install_root;
    char *lib_dir;
    char *pascal_lib_dir;
    char *clike_lib_dir;
    char *rea_lib_dir;
    char *docs_dir;
    char *etc_dir;
    char *fonts_dir;
    char *sounds_dir;
    char *misc_dir;
} PscaliRuntimePaths;

static PscaliRuntimePaths s_runtime_paths;
static pthread_once_t s_runtime_paths_once = PTHREAD_ONCE_INIT;

static void pscaliAbortOutOfMemory(void) {
    fprintf(stderr, "PSCAL iOS: failed to allocate runtime path buffer\n");
    abort();
}

static char *pscaliDuplicateString(const char *value) {
    if (!value) {
        return NULL;
    }
    size_t len = strlen(value) + 1;
    char *copy = (char *)malloc(len);
    if (!copy) {
        pscaliAbortOutOfMemory();
    }
    memcpy(copy, value, len);
    return copy;
}

static char *pscaliJoinPath(const char *base, const char *suffix) {
    if (!base) {
        return NULL;
    }
    if (!suffix || suffix[0] == '\0') {
        return pscaliDuplicateString(base);
    }

    size_t base_len = strlen(base);
    bool base_has_sep = base_len > 0 && base[base_len - 1] == '/';

    const char *suffix_ptr = suffix;
    while (*suffix_ptr == '/') {
        ++suffix_ptr;
    }

    size_t suffix_len = strlen(suffix_ptr);
    bool need_separator = (base_len > 0) && !base_has_sep && suffix_len > 0;

    size_t total = base_len + (need_separator ? 1 : 0) + suffix_len + 1;
    char *buffer = (char *)malloc(total);
    if (!buffer) {
        pscaliAbortOutOfMemory();
    }

    if (need_separator) {
        snprintf(buffer, total, "%s/%s", base, suffix_ptr);
    } else {
        snprintf(buffer, total, "%s%s", base, suffix_ptr);
    }
    return buffer;
}

static void pscaliInitRuntimePaths(void) {
    const char *install_root_env = getenv("PSCALI_INSTALL_ROOT");
    const char *install_root = (install_root_env && *install_root_env) ? install_root_env : PSCAL_INSTALL_ROOT_FALLBACK;

    s_runtime_paths.install_root = pscaliDuplicateString(install_root);
    s_runtime_paths.lib_dir = pscaliJoinPath(s_runtime_paths.install_root, "lib");
    s_runtime_paths.pascal_lib_dir = pscaliJoinPath(s_runtime_paths.install_root, "pascal/lib");
    s_runtime_paths.clike_lib_dir = pscaliJoinPath(s_runtime_paths.install_root, "clike/lib");
    s_runtime_paths.rea_lib_dir = pscaliJoinPath(s_runtime_paths.install_root, "rea/lib");
    s_runtime_paths.docs_dir = pscaliJoinPath(s_runtime_paths.install_root, "docs");
    s_runtime_paths.etc_dir = pscaliJoinPath(s_runtime_paths.install_root, "etc");
    s_runtime_paths.fonts_dir = pscaliJoinPath(s_runtime_paths.install_root, "fonts");
    s_runtime_paths.sounds_dir = pscaliJoinPath(s_runtime_paths.lib_dir, "sounds");
    s_runtime_paths.misc_dir = pscaliJoinPath(s_runtime_paths.install_root, "misc");

    const char *etc_override = getenv("PSCALI_ETC_ROOT");
    if (etc_override && *etc_override) {
        if (s_runtime_paths.etc_dir) {
            free(s_runtime_paths.etc_dir);
        }
        s_runtime_paths.etc_dir = pscaliDuplicateString(etc_override);
    }
}

static void pscaliEnsureRuntimePaths(void) {
    pthread_once(&s_runtime_paths_once, pscaliInitRuntimePaths);
}

const char *pscaliRuntimeInstallRoot(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.install_root ? s_runtime_paths.install_root : PSCAL_INSTALL_ROOT_FALLBACK;
}

const char *pscaliRuntimeLibDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.lib_dir ? s_runtime_paths.lib_dir : PSCAL_LIB_DIR_FALLBACK;
}

const char *pscaliRuntimePascalLibDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.pascal_lib_dir ? s_runtime_paths.pascal_lib_dir : PSCAL_PASCAL_LIB_DIR_FALLBACK;
}

const char *pscaliRuntimeClikeLibDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.clike_lib_dir ? s_runtime_paths.clike_lib_dir : PSCAL_CLIKE_LIB_DIR_FALLBACK;
}

const char *pscaliRuntimeReaLibDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.rea_lib_dir ? s_runtime_paths.rea_lib_dir : PSCAL_REA_LIB_DIR_FALLBACK;
}

const char *pscaliRuntimeDocsDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.docs_dir ? s_runtime_paths.docs_dir : PSCAL_DOCS_DIR_FALLBACK;
}

const char *pscaliRuntimeEtcDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.etc_dir ? s_runtime_paths.etc_dir : PSCAL_ETC_DIR_FALLBACK;
}

const char *pscaliRuntimeFontsDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.fonts_dir ? s_runtime_paths.fonts_dir : PSCAL_FONTS_DIR_FALLBACK;
}

const char *pscaliRuntimeSoundsDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.sounds_dir ? s_runtime_paths.sounds_dir : PSCAL_SOUNDS_DIR_FALLBACK;
}

const char *pscaliRuntimeMiscDir(void) {
    pscaliEnsureRuntimePaths();
    return s_runtime_paths.misc_dir ? s_runtime_paths.misc_dir : PSCAL_MISC_DIR_FALLBACK;
}

#endif /* PSCAL_TARGET_IOS */
