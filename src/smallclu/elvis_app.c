#include "smallclu/elvis_app.h"

#if defined(PSCAL_TARGET_IOS)

#include "pscal_paths.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <unistd.h>
#include <sys/stat.h>

extern int elvis_main_entry(int argc, char **argv);
void pscalRuntimeDebugLog(const char *message);

static jmp_buf g_elvis_exit_env;
static bool g_elvis_exit_active = false;
static int g_elvis_exit_status = 0;
static char *g_elvis_session_dir = NULL;

static char *smallcluOverrideEnv(const char *name, const char *value) {
    const char *current = getenv(name);
    char *saved = current ? strdup(current) : NULL;
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
    return saved;
}

static void smallcluRestoreEnv(const char *name, char *saved) {
    if (saved) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

static void smallcluCleanupDirectory(const char *path, bool removeSelf) {
    if (!path || !*path) {
        return;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char pathbuf[PATH_MAX];
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", path, entry->d_name);
        unlink(pathbuf);
    }
    closedir(dir);
    if (removeSelf) {
        rmdir(path);
    }
}

static void smallcluCleanupSessionFiles(void) {
    const char *tmpDir = getenv("TMPDIR");
    if (!tmpDir || !*tmpDir) {
        return;
    }
    DIR *dir = opendir(tmpDir);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "elvis", 5) != 0) {
            continue;
        }
        const char *suffix = strrchr(entry->d_name, '.');
        if (!suffix || strcmp(suffix, ".ses") != 0) {
            continue;
        }
        char pathbuf[PATH_MAX];
        snprintf(pathbuf, sizeof(pathbuf), "%s/%s", tmpDir, entry->d_name);
        unlink(pathbuf);
    }
    closedir(dir);
}

#define ELVIS_SESSION_MAGIC 0x0200DEADL
#define ELVIS_SESSION_MAGIC_SWAPPED 0xADDE0002L

static void smallcluCleanupLegacyRamSession(void) {
    const char *legacyName = "ram";
    struct stat st;
    if (stat(legacyName, &st) != 0 || !S_ISREG(st.st_mode)) {
        return;
    }
    FILE *fp = fopen(legacyName, "rb");
    if (!fp) {
        return;
    }
    unsigned long magic = 0;
    size_t readBytes = fread(&magic, 1, sizeof(magic), fp);
    fclose(fp);
    if (readBytes == sizeof(magic) &&
        (magic == ELVIS_SESSION_MAGIC || magic == ELVIS_SESSION_MAGIC_SWAPPED)) {
        unlink(legacyName);
    }
}

static const char *smallcluEnsureSessionDirectory(void) {
    if (g_elvis_session_dir && *g_elvis_session_dir) {
        return g_elvis_session_dir;
    }
    const char *tmpDir = getenv("TMPDIR");
    if (!tmpDir || !*tmpDir) {
        return NULL;
    }
    char templatePath[PATH_MAX];
    snprintf(templatePath, sizeof(templatePath), "%s/pscal_elvis.%06uXXXXXX", tmpDir, arc4random_uniform(999999));
    char *dirString = strdup(templatePath);
    if (!dirString) {
        return NULL;
    }
    if (!mkdtemp(dirString)) {
        free(dirString);
        return NULL;
    }
    g_elvis_session_dir = dirString;
    return g_elvis_session_dir;
}

static char *smallcluBuildElvisPath(void) {
    const char *lib_dir = PSCAL_LIB_DIR;
    if (!lib_dir || !*lib_dir) {
        return NULL;
    }
    const char *suffix_a = "/elvis/data";
    const char *suffix_b = "/elvis/doc";
    size_t len = strlen(lib_dir);
    size_t total = len * 2 + strlen(suffix_a) + strlen(suffix_b) + 2; /* colon + null */
    char *buffer = (char *)malloc(total);
    if (!buffer) {
        return NULL;
    }
    snprintf(buffer, total, "%s%s:%s%s", lib_dir, suffix_a, lib_dir, suffix_b);
    return buffer;
}

void elvis_exit(int status) {
    if (!g_elvis_exit_active) {
        exit(status);
    }
    g_elvis_exit_status = status;
    longjmp(g_elvis_exit_env, 1);
}

int smallcluRunElvis(int argc, char **argv) {
    char *elvis_path = smallcluBuildElvisPath();
    if (!elvis_path) {
        fprintf(stderr, "elvis: unable to resolve runtime path\n");
        return 1;
    }

    smallcluCleanupSessionFiles();
    smallcluCleanupLegacyRamSession();

    char *saved_elvis_path = smallcluOverrideEnv("ELVISPATH", elvis_path);
    char *saved_term = smallcluOverrideEnv("TERM", "vt100");
    char *saved_elvis_term = smallcluOverrideEnv("ELVISTERM", "vt100");
    char *saved_elvis_gui = smallcluOverrideEnv("ELVISGUI", "pscal");
    char *saved_force_termcap = smallcluOverrideEnv("PSCALI_FORCE_TERMCAP", "1");
    char *saved_no_ttyraw = smallcluOverrideEnv("PSCALI_NO_TTYRAW", "1");
    char termcapPath[PATH_MAX];
    const char *sysRoot = getenv("PSCALI_SYSFILES_ROOT");
    if (!sysRoot || !*sysRoot) {
        sysRoot = ".";
    }
    snprintf(termcapPath, sizeof(termcapPath), "%s/etc/termcap", sysRoot);
    char *saved_termcap = smallcluOverrideEnv("TERMCAP", termcapPath);
    const char *tmpDir = getenv("TMPDIR");
    const char *session_dir = smallcluEnsureSessionDirectory();
    char *saved_session_path = NULL;
    if (session_dir) {
        smallcluCleanupDirectory(session_dir, false);
        saved_session_path = smallcluOverrideEnv("SESSIONPATH", session_dir);
    } else if (tmpDir && *tmpDir) {
        saved_session_path = smallcluOverrideEnv("SESSIONPATH", tmpDir);
    }

    int wrapped_argc = argc + 2;
    char **wrapped_argv = (char **)calloc((size_t)wrapped_argc, sizeof(char *));
    if (!wrapped_argv) {
        fprintf(stderr, "elvis: out of memory\n");
        smallcluRestoreEnv("ELVISPATH", saved_elvis_path);
        smallcluRestoreEnv("TERM", saved_term);
        smallcluRestoreEnv("ELVISTERM", saved_elvis_term);
        smallcluRestoreEnv("ELVISGUI", saved_elvis_gui);
        free(elvis_path);
        return 1;
    }
    wrapped_argv[0] = argv[0];
    wrapped_argv[1] = "-G";
    wrapped_argv[2] = "pscal";
    for (int i = 1; i < argc; ++i) {
        wrapped_argv[i + 2] = argv[i];
    }
    pscalRuntimeDebugLog("[smallclu] launching elvis_main_entry");
    for (int i = 0; i < wrapped_argc; ++i) {
        if (wrapped_argv[i]) {
            size_t len = strlen(wrapped_argv[i]) + 32;
            char *buf = (char *)malloc(len);
            if (buf) {
                snprintf(buf, len, "[smallclu] argv[%d]=%s", i, wrapped_argv[i]);
                pscalRuntimeDebugLog(buf);
                free(buf);
            }
        }
    }
    const char *env = getenv("ELVISGUI");
    if (env) {
        size_t len = strlen(env) + 64;
        char *buf = (char *)malloc(len);
        snprintf(buf, len, "[smallclu] ELVISGUI=%s", env);
        pscalRuntimeDebugLog(buf);
        free(buf);
    }

    int status = 0;
    g_elvis_exit_active = true;
    if (setjmp(g_elvis_exit_env) == 0) {
        g_elvis_exit_status = elvis_main_entry(wrapped_argc, wrapped_argv);
    }
    status = g_elvis_exit_status;
    g_elvis_exit_active = false;

    char resultBuf[64];
    snprintf(resultBuf, sizeof(resultBuf), "[smallclu] elvis_main_entry returned %d", status);
    pscalRuntimeDebugLog(resultBuf);

    free(wrapped_argv);
    smallcluRestoreEnv("ELVISPATH", saved_elvis_path);
    smallcluRestoreEnv("TERM", saved_term);
    smallcluRestoreEnv("ELVISTERM", saved_elvis_term);
    smallcluRestoreEnv("ELVISGUI", saved_elvis_gui);
    smallcluRestoreEnv("PSCALI_FORCE_TERMCAP", saved_force_termcap);
    smallcluRestoreEnv("PSCALI_NO_TTYRAW", saved_no_ttyraw);
    smallcluRestoreEnv("TERMCAP", saved_termcap);
    if (session_dir) {
        smallcluCleanupDirectory(session_dir, false);
    }
    smallcluRestoreEnv("SESSIONPATH", saved_session_path);
    free(elvis_path);
    return status;
}

#else

#include <stdio.h>

int smallcluRunElvis(int argc, char **argv) {
    (void)argc;
    (void)argv;
    fprintf(stderr, "elvis: builtin not available on this platform\n");
    return 127;
}

#endif
