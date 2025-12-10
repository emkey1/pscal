#include "nextvi_app.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <termios.h>
#if defined(PSCAL_TARGET_IOS)
#include "common/path_virtualization.h"
#endif

extern int nextvi_main_entry(int argc, char **argv);
void nextvi_reset_state(void);

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

static void smallclueResetNextviGlobals(void) {
    /* nextvi_reset_state() is called inside nextvi_main_entry; avoid double-free */
}

#if defined(PSCAL_TARGET_IOS)
static int smallclueOpenPty(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);
        return -1;
    }
    char slave_name[128];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0) {
        close(master);
        return -1;
    }
    int slave = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave < 0) {
        close(master);
        return -1;
    }
    if (dup2(slave, STDIN_FILENO) < 0 ||
        dup2(slave, STDOUT_FILENO) < 0 ||
        dup2(slave, STDERR_FILENO) < 0) {
        close(slave);
        close(master);
        return -1;
    }
    if (slave != STDIN_FILENO && slave != STDOUT_FILENO && slave != STDERR_FILENO) {
        close(slave);
    }
    close(master);
    return STDIN_FILENO;
}
#endif

static int smallclueSetupTty(void) {
    int fd = open("/dev/tty", O_RDWR);
    if (fd >= 0) {
        if (fd != STDIN_FILENO) {
            dup2(fd, STDIN_FILENO);
        }
        if (fd != STDOUT_FILENO) {
            dup2(fd, STDOUT_FILENO);
        }
    }
#if defined(PSCAL_TARGET_IOS)
    if (fd < 0) {
        fd = smallclueOpenPty();
    }
#endif
    return fd;
}

int smallclueRunElvis(int argc, char **argv) {
    /* nextvi expects xterm-ish behavior; advertise xterm-256color so SGR works. */
    char *saved_term = smallclueOverrideEnv("TERM", "xterm-256color");

    smallclueResetNextviGlobals();

    int dup_fd = smallclueSetupTty();
    struct termios saved_ios;
    int tty_fd = STDIN_FILENO;
    bool have_tty = false;
    if (tcgetattr(tty_fd, &saved_ios) != 0) {
        tty_fd = dup_fd >= 0 ? dup_fd : open("/dev/tty", O_RDWR);
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
    if (dup_fd >= 0 && dup_fd != STDIN_FILENO && dup_fd != tty_fd) {
        close(dup_fd);
    }
    smallclueRestoreEnv("TERM", saved_term);
    return status;
}
