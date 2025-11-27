#include "smallclue/elvis_app.h"

#include "pscal_paths.h"

#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

extern int elvis_main_entry(int argc, char **argv);
#if defined(PSCAL_TARGET_IOS)
void pscalRuntimeDebugLog(const char *message);
#else
static void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif
#include "elvis.h"

static jmp_buf g_elvis_exit_env;
static bool g_elvis_exit_active = false;
static int g_elvis_exit_status = 0;

static char *smallclueGenerateSessionPath(void) {
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir || !*tmpdir) {
        tmpdir = "/tmp";
    }
    char templ[PATH_MAX];
    int written = snprintf(templ, sizeof(templ), "%s/pscal_elvis_%d_XXXXXX.ses", tmpdir, (int)getpid());
    if (written <= 0 || (size_t)written >= sizeof(templ)) {
        return NULL;
    }
    int fd = mkstemp(templ);
    if (fd >= 0) {
        close(fd);
        /* Don't leave the empty file behind; Elvis expects to create it. */
        unlink(templ);
        return strdup(templ);
    }
    return NULL;
}

static char *smallclueOverrideEnv(const char *name, const char *value) {
    const char *current = getenv(name);
    char *saved = current ? strdup(current) : NULL;
    if (value) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
    return saved;
}

static void smallclueRestoreEnv(const char *name, char *saved) {
    if (saved) {
        setenv(name, saved, 1);
        free(saved);
    } else {
        unsetenv(name);
    }
}

static char *smallclueBuildElvisPath(void) {
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

int smallclueRunElvis(int argc, char **argv) {
    char *elvis_path = smallclueBuildElvisPath();
    if (!elvis_path) {
        fprintf(stderr, "elvis: unable to resolve runtime path\n");
        return 1;
    }

    char *saved_elvis_path = smallclueOverrideEnv("ELVISPATH", elvis_path);
    char *saved_term = smallclueOverrideEnv("TERM", "vt100");
    char *saved_elvis_term = smallclueOverrideEnv("ELVISTERM", "vt100");
#if defined(PSCAL_TARGET_IOS)
    const char *default_gui = "pscal";
#else
    const char *default_gui = "tcap";
#endif
    char *saved_elvis_gui = smallclueOverrideEnv("ELVISGUI", default_gui);
#if defined(PSCAL_TARGET_IOS)
    char *saved_force_termcap = smallclueOverrideEnv("PSCALI_FORCE_TERMCAP", "1");
    char *saved_no_ttyraw = smallclueOverrideEnv("PSCALI_NO_TTYRAW", "1");
    char termcapPath[PATH_MAX];
    const char *sysRoot = getenv("PSCALI_SYSFILES_ROOT");
    if (!sysRoot || !*sysRoot) {
        sysRoot = ".";
    }
    snprintf(termcapPath, sizeof(termcapPath), "%s/etc/termcap", sysRoot);
    char *saved_termcap = smallclueOverrideEnv("TERMCAP", termcapPath);
#endif
    char *session_path_owned = smallclueGenerateSessionPath();
    const char *session_path = session_path_owned;
    bool use_custom_session = session_path && session_path[0] != '\0';

    int extra_args = use_custom_session ? 5 : 3; /* argv0, -G, gui, [-f session] */
    int wrapped_argc = argc + extra_args;
    char **wrapped_argv = (char **)calloc((size_t)wrapped_argc, sizeof(char *));
    if (!wrapped_argv) {
        fprintf(stderr, "elvis: out of memory\n");
        smallclueRestoreEnv("ELVISPATH", saved_elvis_path);
        smallclueRestoreEnv("TERM", saved_term);
        smallclueRestoreEnv("ELVISTERM", saved_elvis_term);
        smallclueRestoreEnv("ELVISGUI", saved_elvis_gui);
        free(elvis_path);
        return 1;
    }
    int argi = 0;
    wrapped_argv[argi++] = argv[0];
    wrapped_argv[argi++] = "-G";
    wrapped_argv[argi++] = "pscal";
    if (use_custom_session) {
        wrapped_argv[argi++] = "-f";
        wrapped_argv[argi++] = session_path;
    }
    for (int i = 1; i < argc; ++i) {
        wrapped_argv[argi++] = argv[i];
    }
    pscalRuntimeDebugLog("[smallclue] launching elvis_main_entry");
    for (int i = 0; i < wrapped_argc; ++i) {
        if (wrapped_argv[i]) {
            size_t len = strlen(wrapped_argv[i]) + 32;
            char *buf = (char *)malloc(len);
            if (buf) {
                snprintf(buf, len, "[smallclue] argv[%d]=%s", i, wrapped_argv[i]);
                pscalRuntimeDebugLog(buf);
                free(buf);
            }
        }
    }
    const char *env = getenv("ELVISGUI");
    if (env) {
        size_t len = strlen(env) + 64;
        char *buf = (char *)malloc(len);
        snprintf(buf, len, "[smallclue] ELVISGUI=%s", env);
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
    snprintf(resultBuf, sizeof(resultBuf), "[smallclue] elvis_main_entry returned %d", status);
    pscalRuntimeDebugLog(resultBuf);

    free(wrapped_argv);
    smallclueRestoreEnv("ELVISPATH", saved_elvis_path);
    smallclueRestoreEnv("TERM", saved_term);
    smallclueRestoreEnv("ELVISTERM", saved_elvis_term);
    smallclueRestoreEnv("ELVISGUI", saved_elvis_gui);
#if defined(PSCAL_TARGET_IOS)
    smallclueRestoreEnv("PSCALI_FORCE_TERMCAP", saved_force_termcap);
    smallclueRestoreEnv("PSCALI_NO_TTYRAW", saved_no_ttyraw);
    smallclueRestoreEnv("TERMCAP", saved_termcap);
#endif
    free(elvis_path);
    /* Ensure elvis session state is fully torn down before next launch. */
    sesclose();
    if (session_path_owned) {
        unlink(session_path_owned);
        free(session_path_owned);
    }
    return status;
}
