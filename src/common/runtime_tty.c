#include "common/runtime_tty.h"

#include <unistd.h>

static bool sVirtualTTYEnabled = false;

void pscalRuntimeSetVirtualTTYEnabled(bool enabled) {
    sVirtualTTYEnabled = enabled;
}

bool pscalRuntimeVirtualTTYEnabled(void) {
    return sVirtualTTYEnabled;
}

static bool pscalIsStandardFd(int fd) {
    return fd == STDIN_FILENO || fd == STDOUT_FILENO || fd == STDERR_FILENO;
}

bool pscalRuntimeFdIsInteractive(int fd) {
    if (fd < 0) {
        return false;
    }
    if (isatty(fd)) {
        return true;
    }
    if (!sVirtualTTYEnabled) {
        return false;
    }
    return pscalIsStandardFd(fd);
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

