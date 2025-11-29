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

    struct termios saved_ios;
    bool have_tty = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    if (have_tty && tcgetattr(STDIN_FILENO, &saved_ios) == 0) {
        struct termios raw = saved_ios;
        raw.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
        raw.c_iflag &= ~(ICRNL | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        tcflush(STDIN_FILENO, TCIFLUSH);
    } else {
        have_tty = false;
    }

    pscalRuntimeDebugLog("[smallclue] launching nextvi");
    int status = nextvi_main_entry(argc, argv);

    char resultBuf[128];
    snprintf(resultBuf, sizeof(resultBuf), "[smallclue] nextvi returned %d", status);
    pscalRuntimeDebugLog(resultBuf);

    if (have_tty) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_ios);
    }
    smallclueRestoreEnv("TERM", saved_term);
    return status;
}
