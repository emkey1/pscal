#include "common/runtime_tty.h"

#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>
#if defined(PSCAL_TARGET_IOS)
#include "ios/vproc.h"
#endif

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
        if (pscalRuntimeSessionFdIsInteractive(fd)) {
            return true;
        }
    }
#endif
    if (isatty(fd)) {
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
