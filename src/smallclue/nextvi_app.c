#include "smallclue/nextvi_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>

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
    int tty_fd = STDIN_FILENO;
    bool have_tty = false;
    if (tcgetattr(tty_fd, &saved_ios) != 0) {
        tty_fd = open("/dev/tty", O_RDWR);
        if (tty_fd >= 0 && tcgetattr(tty_fd, &saved_ios) == 0) {
            have_tty = true;
        }
    } else {
        have_tty = true;
    }
    if (have_tty) {
        struct termios raw = saved_ios;
        raw.c_lflag &= ~(ICANON | ECHO | IEXTEN | ISIG);
        raw.c_iflag &= ~(ICRNL | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(tty_fd, TCSAFLUSH, &raw);
        tcflush(tty_fd, TCIFLUSH);
    } else {
        have_tty = false;
    }

    pscalRuntimeDebugLog("[smallclue] launching nextvi");
    int status = nextvi_main_entry(argc, argv);

    char resultBuf[128];
    snprintf(resultBuf, sizeof(resultBuf), "[smallclue] nextvi returned %d", status);
    pscalRuntimeDebugLog(resultBuf);

    if (have_tty) {
        tcsetattr(tty_fd, TCSAFLUSH, &saved_ios);
        if (tty_fd != STDIN_FILENO) {
            close(tty_fd);
        }
    }
    smallclueRestoreEnv("TERM", saved_term);
    return status;
}
