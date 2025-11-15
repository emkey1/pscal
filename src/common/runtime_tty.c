#include "common/runtime_tty.h"

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    bool valid;
    dev_t dev;
    ino_t ino;
} VirtualTTYDescriptor;

static bool sVirtualTTYEnabled = false;
static VirtualTTYDescriptor sVirtualTTY[3];

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
}

void pscalRuntimeSetVirtualTTYEnabled(bool enabled) {
    sVirtualTTYEnabled = enabled;
    if (!enabled) {
        for (int i = 0; i < 3; ++i) {
            sVirtualTTY[i].valid = false;
            sVirtualTTY[i].dev = 0;
            sVirtualTTY[i].ino = 0;
        }
    }
}

bool pscalRuntimeVirtualTTYEnabled(void) {
    return sVirtualTTYEnabled;
}

static bool pscalRuntimeFdUsesVirtualTTY(int fd) {
    if (!sVirtualTTYEnabled) {
        return false;
    }
    int idx = stdFdToIndex(fd);
    if (idx < 0) {
        return false;
    }
    VirtualTTYDescriptor *slot = &sVirtualTTY[idx];
    if (!slot->valid) {
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        return false;
    }
    return st.st_dev == slot->dev && st.st_ino == slot->ino;
}

bool pscalRuntimeFdIsInteractive(int fd) {
    if (fd < 0) {
        return false;
    }
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
