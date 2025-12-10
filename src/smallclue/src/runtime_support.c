#include "common/runtime_tty.h"
#include "common/runtime_clipboard.h"
#include "common/path_truncate.h"
#include "common/pscal_hosts.h"

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <netdb.h>

static bool g_clipboard_init = false;
static char *g_clipboard_data = NULL;
static size_t g_clipboard_len = 0;

__attribute__((weak)) bool pscalRuntimeStdoutIsInteractive(void) {
    return isatty(STDOUT_FILENO);
}

__attribute__((weak)) bool pscalRuntimeStdinIsInteractive(void) {
    return isatty(STDIN_FILENO);
}

__attribute__((weak)) bool pscalRuntimeFdIsInteractive(int fd) {
    return fd >= 0 && isatty(fd);
}

__attribute__((weak)) bool pscalRuntimeStdinHasRealTTY(void) {
    return isatty(STDIN_FILENO);
}

static int pscalRuntimeDetectWindowDim(int dim_default, bool want_cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (want_cols && ws.ws_col > 0) return ws.ws_col;
        if (!want_cols && ws.ws_row > 0) return ws.ws_row;
    }
    const char *env = getenv(want_cols ? "COLUMNS" : "LINES");
    if (env && *env) {
        int val = atoi(env);
        if (val > 0) {
            return val;
        }
    }
    return dim_default;
}

__attribute__((weak)) int pscalRuntimeDetectWindowCols(void) {
    return pscalRuntimeDetectWindowDim(80, true);
}

__attribute__((weak)) int pscalRuntimeDetectWindowRows(void) {
    return pscalRuntimeDetectWindowDim(24, false);
}

__attribute__((weak)) bool pscalRuntimeConsumeSigint(void) {
    /* Standalone build does not provide an out-of-band SIGINT queue. */
    return false;
}

__attribute__((weak)) void pscalRuntimeDebugLog(const char *message) {
    if (message && *message) {
        fprintf(stderr, "%s\n", message);
    }
}

__attribute__((weak)) int runtimeClipboardSet(const char *data, size_t len) {
    free(g_clipboard_data);
    g_clipboard_data = NULL;
    g_clipboard_len = 0;
    if (!data || len == 0) {
        g_clipboard_init = true;
        return 0;
    }
    g_clipboard_data = (char *)malloc(len);
    if (!g_clipboard_data) {
        return ENOMEM;
    }
    memcpy(g_clipboard_data, data, len);
    g_clipboard_len = len;
    g_clipboard_init = true;
    return 0;
}

__attribute__((weak)) char *runtimeClipboardGet(size_t *len_out) {
    if (!g_clipboard_init || !g_clipboard_data) {
        return NULL;
    }
    char *copy = (char *)malloc(g_clipboard_len);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, g_clipboard_data, g_clipboard_len);
    if (len_out) {
        *len_out = g_clipboard_len;
    }
    return copy;
}

__attribute__((weak)) bool pathTruncateExpand(const char *path, char *buffer, size_t buflen) {
    if (!path || !buffer || buflen == 0) {
        return false;
    }
    /* No virtualization required on desktop builds; simply copy when asked. */
    size_t needed = strlen(path) + 1;
    if (needed > buflen) {
        return false;
    }
    memcpy(buffer, path, needed);
    return true;
}

__attribute__((weak)) void pscalHostsSetLogEnabled(int enabled) {
    (void)enabled;
}

__attribute__((weak)) int pscalHostsGetAddrInfo(const char *node,
                          const char *service,
                          const struct addrinfo *hints,
                          struct addrinfo **res) {
    return getaddrinfo(node, service, hints, res);
}

__attribute__((weak)) void pscalHostsFreeAddrInfo(struct addrinfo *res) {
    freeaddrinfo(res);
}

__attribute__((weak)) const char *pscalHostsGetContainerPath(void) {
    const char *root = getenv("PSCALI_CONTAINER_ROOT");
    return (root && *root) ? root : "/";
}
