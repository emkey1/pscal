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

extern int elvis_main_entry(int argc, char **argv);

static jmp_buf g_elvis_exit_env;
static bool g_elvis_exit_active = false;
static int g_elvis_exit_status = 0;

static const char *smallcluEmbeddedVt100Termcap(void) {
    return "vt100|vt100-am|dec vt100:"
           ":am:bs:xn:km:mi:ms:xo:"
           ":co#80:li#24:it#8:"
           ":cl=\\E[H\\E[2J:cd=\\E[J:ce=\\E[K:"
           ":cm=\\E[%i%p1%d;%p2%dH:ho=\\E[H:"
           ":up=\\E[A:do=\\E[B:nd=\\E[C:le=\\E[D:"
           ":AL=\\E[%dL:DL=\\E[%dM:al=\\E[L:dl=\\E[M:"
           ":IC=\\E[%d@:DC=\\E[%dP:ic=\\E[@:dc=\\E[P:"
           ":ks=\\E[?1h\\E=:ke=\\E[?1l\\E>:"
           ":ti=\\E[?1049h:te=\\E[?1049l:"
           ":so=\\E[7m:se=\\E[27m:"
           ":us=\\E[4m:ue=\\E[24m:"
           ":md=\\E[1m:mr=\\E[7m:me=\\E[0m:mh=\\E[2m:"
           ":vi=\\E[?25l:ve=\\E[?25h:vs=\\E[?25h:"
           ":sr=\\EM:SF=\\E[%dS:SR=\\E[%dT:"
           ":cs=\\E[%i%p1%d;%p2%dr:"
           ":sc=\\E7:rc=\\E8:"
           ":as=\\E(0:ae=\\E(B:";
}

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

    char *saved_elvis_path = smallcluOverrideEnv("ELVISPATH", elvis_path);
    char *saved_term = smallcluOverrideEnv("TERM", "vt100");
    char *saved_elvis_term = smallcluOverrideEnv("ELVISTERM", "vt100");
    char *saved_elvis_gui = smallcluOverrideEnv("ELVISGUI", "termcap");
    char *saved_force_termcap = smallcluOverrideEnv("PSCALI_FORCE_TERMCAP", "1");
    char *saved_termcap = smallcluOverrideEnv("TERMCAP", smallcluEmbeddedVt100Termcap());

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
    wrapped_argv[2] = "termcap";
    for (int i = 1; i < argc; ++i) {
        wrapped_argv[i + 2] = argv[i];
    }

    int status = 0;
    g_elvis_exit_active = true;
    if (setjmp(g_elvis_exit_env) == 0) {
        g_elvis_exit_status = elvis_main_entry(wrapped_argc, wrapped_argv);
    }
    status = g_elvis_exit_status;
    g_elvis_exit_active = false;

    free(wrapped_argv);
    smallcluRestoreEnv("ELVISPATH", saved_elvis_path);
    smallcluRestoreEnv("TERM", saved_term);
    smallcluRestoreEnv("ELVISTERM", saved_elvis_term);
    smallcluRestoreEnv("ELVISGUI", saved_elvis_gui);
    smallcluRestoreEnv("PSCALI_FORCE_TERMCAP", saved_force_termcap);
    smallcluRestoreEnv("TERMCAP", saved_termcap);
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
