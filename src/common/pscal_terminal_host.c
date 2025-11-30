#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <string.h>
#include <stdlib.h>

#ifdef PSCAL_TARGET_IOS
#define PSCAL_WEAK __attribute__((weak))

PSCAL_WEAK void pscalRuntimeDebugLog(const char *message) {
    (void)message;
}
PSCAL_WEAK void pscalTerminalBegin(int columns, int rows) {
    (void)columns;
    (void)rows;
}
PSCAL_WEAK void pscalTerminalEnd(void) {}
PSCAL_WEAK void pscalTerminalResize(int columns, int rows) {
    (void)columns;
    (void)rows;
}
PSCAL_WEAK void pscalTerminalRender(const char *utf8, int len, int row, int col,
    long fg, long bg, int attr) {
    (void)utf8;
    (void)len;
    (void)row;
    (void)col;
    (void)fg;
    (void)bg;
    (void)attr;
}
PSCAL_WEAK void pscalTerminalClear(void) {}
PSCAL_WEAK void pscalTerminalClearEol(int row, int col) {
    (void)row;
    (void)col;
}
PSCAL_WEAK void pscalTerminalClearBol(int row, int col) {
    (void)row;
    (void)col;
}
PSCAL_WEAK void pscalTerminalClearLine(int row) {
    (void)row;
}
PSCAL_WEAK void pscalTerminalClearScreenFromCursor(int row, int col) {
    (void)row;
    (void)col;
}
PSCAL_WEAK void pscalTerminalClearScreenToCursor(int row, int col) {
    (void)row;
    (void)col;
}
PSCAL_WEAK void pscalTerminalInsertChars(int row, int col, int count) {
    (void)row;
    (void)col;
    (void)count;
}
PSCAL_WEAK void pscalTerminalDeleteChars(int row, int col, int count) {
    (void)row;
    (void)col;
    (void)count;
}
PSCAL_WEAK void pscalTerminalEnterAltScreen(void) {}
PSCAL_WEAK void pscalTerminalExitAltScreen(void) {}
PSCAL_WEAK void pscalTerminalSetCursorVisible(int visible) {
    (void)visible;
}
PSCAL_WEAK void pscalTerminalInsertLines(int row, int count) {
    (void)row;
    (void)count;
}
PSCAL_WEAK void pscalTerminalDeleteLines(int row, int count) {
    (void)row;
    (void)count;
}
PSCAL_WEAK void pscalTerminalMoveCursor(int row, int col) {
    (void)row;
    (void)col;
}

PSCAL_WEAK int pscalTerminalRead(uint8_t *buffer, int maxlen, int timeout_ms) {
    (void)buffer;
    (void)maxlen;
    (void)timeout_ms;
    return 0;
}

#else

#include <signal.h>

extern volatile sig_atomic_t g_smallclue_openssh_exit_requested __attribute__((weak));

static struct termios g_saved_termios;
static bool g_termios_saved = false;
static bool g_raw_mode = false;
static int g_rows = 24;
static int g_cols = 80;

static void pscalTerminalMove(int row, int col) {
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    printf("\033[%d;%dH", row + 1, col + 1);
}

static void pscalTerminalEnterRaw(void) {
    if (g_raw_mode) {
        return;
    }
    if (tcgetattr(STDIN_FILENO, &g_saved_termios) == 0) {
        struct termios raw = g_saved_termios;
        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
            g_termios_saved = true;
            g_raw_mode = true;
        }
    }
}

static void pscalTerminalLeaveRaw(void) {
    if (g_raw_mode && g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_termios);
    }
    g_raw_mode = false;
}

static bool g_debug_log_checked = false;
static bool g_debug_log_enabled = false;

static void pscalTerminalInitDebugFlag(void) {
    if (g_debug_log_checked)
        return;
    const char *env = getenv("PSCALI_DEBUG_ELVIS");
    if (env && *env && strcmp(env, "0") != 0) {
        g_debug_log_enabled = true;
    }
    g_debug_log_checked = true;
}

void pscalRuntimeDebugLog(const char *message) {
    if (!message) {
        return;
    }
    pscalTerminalInitDebugFlag();
    if (!g_debug_log_enabled) {
        return;
    }
    fprintf(stderr, "%s\n", message);
}

void pscalTerminalBegin(int columns, int rows) {
    if (columns > 0) g_cols = columns;
    if (rows > 0) g_rows = rows;
    pscalTerminalEnterRaw();
    printf("\033[?1049h\033[2J\033[H");
    fflush(stdout);
}

void pscalTerminalEnd(void) {
    printf("\033[?1049l\033[0m\n");
    fflush(stdout);
    pscalTerminalLeaveRaw();
}

void pscalTerminalResize(int columns, int rows) {
    if (columns > 0) g_cols = columns;
    if (rows > 0) g_rows = rows;
}

void pscalTerminalRender(const char *utf8, int len, int row, int col,
    long fg, long bg, int attr) {
    (void)fg;
    (void)bg;
    (void)attr;
    if (!utf8 || len <= 0) {
        return;
    }
    pscalTerminalMove(row, col);
    fwrite(utf8, 1, (size_t)len, stdout);
    fflush(stdout);
}

void pscalTerminalClear(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

void pscalTerminalClearEol(int row, int col) {
    pscalTerminalMove(row, col);
    printf("\033[K");
    fflush(stdout);
}
void pscalTerminalClearBol(int row, int col) {
    (void)col;
    if (row < 0) row = 0;
    pscalTerminalMove(row, 0);
    printf("\033[1K");
    fflush(stdout);
}
void pscalTerminalClearLine(int row) {
    if (row < 0) row = 0;
    pscalTerminalMove(row, 0);
    printf("\033[2K");
    fflush(stdout);
}
void pscalTerminalClearScreenFromCursor(int row, int col) {
    pscalTerminalMove(row, col);
    printf("\033[0J");
    fflush(stdout);
}
void pscalTerminalClearScreenToCursor(int row, int col) {
    pscalTerminalMove(row, col);
    printf("\033[1J");
    fflush(stdout);
}
void pscalTerminalInsertChars(int row, int col, int count) {
    pscalTerminalMove(row, col);
    printf("\033[%d@", count);
    fflush(stdout);
}
void pscalTerminalDeleteChars(int row, int col, int count) {
    pscalTerminalMove(row, col);
    printf("\033[%dP", count);
    fflush(stdout);
}
void pscalTerminalEnterAltScreen(void) {
    printf("\033[?1049h");
    fflush(stdout);
}
void pscalTerminalExitAltScreen(void) {
    printf("\033[?1049l");
    fflush(stdout);
}
void pscalTerminalSetCursorVisible(int visible) {
    printf("\033[?25%cm", visible ? 'h' : 'l');
    fflush(stdout);
}

void pscalTerminalMoveCursor(int row, int col) {
    pscalTerminalMove(row, col);
    fflush(stdout);
}

void pscalTerminalInsertLines(int row, int count) {
    (void)row;
    (void)count;
    /* insert-line handling is not required for host terminal */
}

void pscalTerminalDeleteLines(int row, int count) {
    (void)row;
    (void)count;
    /* delete-line handling is not required for host terminal */
}

int pscalTerminalRead(uint8_t *buffer, int maxlen, int timeout_ms) {
    if (!buffer || maxlen <= 0) {
        return 0;
    }
    if (g_smallclue_openssh_exit_requested) {
        return -1;
    }
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(STDIN_FILENO, &rfds);
    struct timeval tv;
    struct timeval *tv_ptr = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tv_ptr = &tv;
    }
    int ready = select(STDIN_FILENO + 1, &rfds, NULL, NULL, tv_ptr);
    if (ready <= 0) {
        return ready;
    }
    ssize_t n = read(STDIN_FILENO, buffer, (size_t)maxlen);
    if (n < 0) {
        return -1;
    }
    return (int)n;
}

#endif /* !PSCAL_TARGET_IOS */
