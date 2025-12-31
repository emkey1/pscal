#include "common/runtime_tty.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(PSCAL_TARGET_IOS)
#include "ios/vproc.h"
#endif

typedef struct {
    bool valid;
    dev_t dev;
    ino_t ino;
} VirtualTTYDescriptor;

typedef struct {
    bool initialized;
    struct termios termios;
    struct winsize winsize;
} VirtualTTYState;

static _Thread_local bool sVirtualTTYEnabled = false;
static _Thread_local VirtualTTYDescriptor sVirtualTTY[3];
static _Thread_local VirtualTTYState sVirtualTTYState;

static int stdFdToIndex(int fd) {
    if (fd == STDIN_FILENO) return 0;
    if (fd == STDOUT_FILENO) return 1;
    if (fd == STDERR_FILENO) return 2;
    return -1;
}

void pscalRuntimeRegisterVirtualTTYFd(int std_fd, int fd) {
    int idx = stdFdToIndex(std_fd);
    if (idx < 0) {
        return;
    }
    VirtualTTYDescriptor *slot = &sVirtualTTY[idx];
    if (fd < 0) {
        slot->valid = false;
        slot->dev = 0;
        slot->ino = 0;
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        slot->valid = false;
        slot->dev = 0;
        slot->ino = 0;
        return;
    }
    slot->valid = true;
    slot->dev = st.st_dev;
    slot->ino = st.st_ino;
    sVirtualTTYEnabled = true;
}

void pscalRuntimeSetVirtualTTYEnabled(bool enabled) {
    sVirtualTTYEnabled = enabled;
    if (!enabled) {
        for (int i = 0; i < 3; ++i) {
            sVirtualTTY[i].valid = false;
            sVirtualTTY[i].dev = 0;
            sVirtualTTY[i].ino = 0;
        }
        pscalRuntimeVirtualTTYReset();
    }
}

bool pscalRuntimeVirtualTTYEnabled(void) {
    return sVirtualTTYEnabled;
}

static bool pscalRuntimeFdUsesVirtualTTY(int fd) {
    if (!sVirtualTTYEnabled || fd < 0) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return false;
    }
    for (int i = 0; i < 3; ++i) {
        VirtualTTYDescriptor *slot = &sVirtualTTY[i];
        if (!slot->valid) {
            continue;
        }
        if (st.st_dev == slot->dev && st.st_ino == slot->ino) {
            return true;
        }
    }
    return false;
}

#if defined(PSCAL_TARGET_IOS)
static bool pscalRuntimeSessionFdIsInteractive(int fd) {
    if (fd != STDIN_FILENO && fd != STDOUT_FILENO && fd != STDERR_FILENO) {
        return false;
    }
    VProcSessionStdio *session = vprocSessionStdioCurrent();
    if (!session || vprocSessionStdioIsDefault(session)) {
        return false;
    }
    return true;
}
#endif

bool pscalRuntimeFdIsInteractive(int fd) {
    if (fd < 0) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if ((fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO)) {
        if (pscalRuntimeVirtualTTYEnabled()) {
            return true;
        }
        if (pscalRuntimeSessionFdIsInteractive(fd)) {
            return true;
        }
    }
#endif
    if (isatty(fd)) {
        return true;
    }
    if (pscalRuntimeFdUsesVirtualTTY(fd)) {
        return true;
    }
    return false;
}

bool pscalRuntimeStdinIsInteractive(void) {
    return pscalRuntimeFdIsInteractive(STDIN_FILENO);
}

bool pscalRuntimeStdoutIsInteractive(void) {
    return pscalRuntimeFdIsInteractive(STDOUT_FILENO);
}

bool pscalRuntimeStderrIsInteractive(void) {
    return pscalRuntimeFdIsInteractive(STDERR_FILENO);
}

bool pscalRuntimeFdHasRealTTY(int fd) {
    if (fd < 0) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    if (fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO) {
        VProcSessionStdio *session = vprocSessionStdioCurrent();
        if (session && !vprocSessionStdioIsDefault(session)) {
            if (session->pty_active) {
                /* PTY-backed stdio supports termios even without a host TTY. */
                return true;
            }
            if (session->stdin_pscal_fd ||
                session->stdout_pscal_fd ||
                session->stderr_pscal_fd) {
                return false;
            }
        }
    }
#endif
    return isatty(fd) != 0;
}

bool pscalRuntimeStdinHasRealTTY(void) {
    return pscalRuntimeFdHasRealTTY(STDIN_FILENO);
}

bool pscalRuntimeStdoutHasRealTTY(void) {
    return pscalRuntimeFdHasRealTTY(STDOUT_FILENO);
}

bool pscalRuntimeStderrHasRealTTY(void) {
    return pscalRuntimeFdHasRealTTY(STDERR_FILENO);
}

static int pscalRuntimeParseEnvInt(const char *name) {
    const char *value = getenv(name);
    if (!value || !*value) {
        return -1;
    }
    char *endptr = NULL;
    long parsed = strtol(value, &endptr, 10);
    if (!endptr || *endptr != '\0' || parsed <= 0 || parsed > 1000) {
        return -1;
    }
    return (int)parsed;
}

static int pscalRuntimeDetectDimension(bool rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        unsigned short value = rows ? ws.ws_row : ws.ws_col;
        if (value > 0) {
            return (int)value;
        }
    }
    int envValue = pscalRuntimeParseEnvInt(rows ? "LINES" : "COLUMNS");
    if (envValue > 0) {
        return envValue;
    }
    return rows ? 24 : 80;
}

int pscalRuntimeDetectWindowRows(void) {
    return pscalRuntimeDetectDimension(true);
}

int pscalRuntimeDetectWindowCols(void) {
    return pscalRuntimeDetectDimension(false);
}

static void pscalRuntimeVirtualTTYInitDefaults(void) {
    if (sVirtualTTYState.initialized) {
        return;
    }
    memset(&sVirtualTTYState, 0, sizeof(sVirtualTTYState));
    struct termios term;
    memset(&term, 0, sizeof(term));
    term.c_iflag = ICRNL | IXON;
#ifdef IUTF8
    term.c_iflag |= IUTF8;
#endif
    term.c_oflag = OPOST | ONLCR;
    term.c_cflag = CS8 | CREAD;
    term.c_lflag = ISIG | ECHO;
    term.c_cc[VINTR] = 0x03;
    term.c_cc[VQUIT] = 0x1c;
    term.c_cc[VSUSP] = 0x1a;
    term.c_cc[VEOF] = 0x04;
    term.c_cc[VEOL] = '\n';
    term.c_cc[VEOL2] = '\r';
    sVirtualTTYState.termios = term;
    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    ws.ws_col = (unsigned short)pscalRuntimeDetectWindowCols();
    ws.ws_row = (unsigned short)pscalRuntimeDetectWindowRows();
    sVirtualTTYState.winsize = ws;
    sVirtualTTYState.initialized = true;
}

bool pscalRuntimeVirtualTTYGetTermios(struct termios *out) {
    if (!out) {
        return false;
    }
    pscalRuntimeVirtualTTYInitDefaults();
    *out = sVirtualTTYState.termios;
    return true;
}

void pscalRuntimeVirtualTTYSetTermios(const struct termios *termios) {
    if (!termios) {
        return;
    }
    pscalRuntimeVirtualTTYInitDefaults();
    sVirtualTTYState.termios = *termios;
}

bool pscalRuntimeVirtualTTYGetWinsize(struct winsize *out) {
    if (!out) {
        return false;
    }
    pscalRuntimeVirtualTTYInitDefaults();
    *out = sVirtualTTYState.winsize;
    return true;
}

void pscalRuntimeVirtualTTYSetWinsize(const struct winsize *winsize) {
    if (!winsize) {
        return;
    }
    pscalRuntimeVirtualTTYInitDefaults();
    sVirtualTTYState.winsize = *winsize;
}

void pscalRuntimeVirtualTTYReset(void) {
    sVirtualTTYState.initialized = false;
    memset(&sVirtualTTYState.termios, 0, sizeof(sVirtualTTYState.termios));
    memset(&sVirtualTTYState.winsize, 0, sizeof(sVirtualTTYState.winsize));
}
