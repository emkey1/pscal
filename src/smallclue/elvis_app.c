#include "smallclue/elvis_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int nextvi_main_entry(int argc, char **argv);

#if defined(PSCAL_TARGET_IOS)
void pscalRuntimeDebugLog(const char *message);
#else
static void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
#endif

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

int smallclueRunElvis(int argc, char **argv) {
    char *saved_term = smallclueOverrideEnv("TERM", "vt100");

    pscalRuntimeDebugLog("[smallclue] launching nextvi");
    int status = nextvi_main_entry(argc, argv);

    char resultBuf[128];
    snprintf(resultBuf, sizeof(resultBuf), "[smallclue] nextvi returned %d", status);
    pscalRuntimeDebugLog(resultBuf);

    smallclueRestoreEnv("TERM", saved_term);
    return status;
}
