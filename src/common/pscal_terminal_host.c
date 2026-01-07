#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <termios.h>
#include <sys/select.h>
#include <sys/ioctl.h>
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
    if (row < 0) row = 0;
    if (count < 1) count = 1;
    printf("\033[%d;%dH\033[%dL", row + 1, 1, count);
    fflush(stdout);
}
PSCAL_WEAK void pscalTerminalDeleteLines(int row, int count) {
    if (row < 0) row = 0;
    if (count < 1) count = 1;
    printf("\033[%d;%dH\033[%dM", row + 1, 1, count);
    fflush(stdout);
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
static int g_margin_top = 0;
static int g_margin_bottom = 23;
static int g_origin_mode = 0;
static int g_wrap_mode = 1;
static int g_saved_row = 0;
static int g_saved_col = 0;
static int g_tab_width = 8;
static unsigned char g_tabs[256];

typedef struct {
    long fg;
    long bg;
    int attr; /* bit0=bold, bit1=underline, bit2=inverse, bit3=blink, bit4=faint, bit5=italic, bit6=strike */
} HostAttrState;

static HostAttrState g_attr_state = { .fg = -1, .bg = -1, .attr = 0 };

static void pscalTerminalQuerySize(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        if (ws.ws_col > 0) g_cols = ws.ws_col;
        if (ws.ws_row > 0) g_rows = ws.ws_row;
    }
    if (g_rows < 1) g_rows = 24;
    if (g_cols < 1) g_cols = 80;
    g_margin_top = 0;
    g_margin_bottom = g_rows - 1;
}

static void pscalTerminalResetTabs(void) {
    int limit = g_cols < (int)sizeof(g_tabs) ? g_cols : (int)sizeof(g_tabs);
    for (int i = 0; i < limit; ++i) {
        g_tabs[i] = (i % g_tab_width) == 0 ? 1 : 0;
    }
}

static void pscalTerminalMove(int row, int col) {
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    printf("\033[%d;%dH", row + 1, col + 1);
}

static void pscalTerminalApplyAttrs(long fg, long bg, int attr) {
    /* Cache to avoid redundant SGR output. */
    if (fg == g_attr_state.fg && bg == g_attr_state.bg && attr == g_attr_state.attr) {
        return;
    }
    g_attr_state.fg = fg;
    g_attr_state.bg = bg;
    g_attr_state.attr = attr;
    printf("\033[0m");
    if (attr & 1) printf("\033[1m");          /* bold */
    if (attr & 2) printf("\033[4m");          /* underline */
    if (attr & 4) printf("\033[7m");          /* inverse */
    if (attr & 8) printf("\033[5m");          /* blink */
    if (attr & 16) printf("\033[2m");         /* faint */
    if (attr & 32) printf("\033[3m");         /* italic */
    if (attr & 64) printf("\033[9m");         /* strike */
    if (fg >= 0) printf("\033[38;5;%ldm", fg);
    if (bg >= 0) printf("\033[48;5;%ldm", bg);
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

static int pscalTerminalRowBase(void) {
    return g_origin_mode ? g_margin_top : 0;
}

static void pscalTerminalClampCursor(int *row, int *col) {
    int base = pscalTerminalRowBase();
    int top = g_origin_mode ? g_margin_top : 0;
    int bottom = g_origin_mode ? g_margin_bottom : g_rows - 1;
    if (*row < top) *row = top;
    if (*row > bottom) *row = bottom;
    if (*col < 0) *col = 0;
    if (*col >= g_cols) *col = g_cols - 1;
    (void)base;
}

static void pscalTerminalInitDebugFlag(void) {
    const char *env = getenv("PSCALI_DEBUG_EDITOR");
  //  if (env && *env && strcmp(env, "0") != 0) {
        g_debug_log_enabled = true;
        g_debug_log_checked = true;
        return;
  //  }
    if (g_debug_log_checked)
        return;
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
    pscalTerminalQuerySize();
    pscalTerminalEnterRaw();
    printf("\033[?1049h\033[2J\033[H");
    g_margin_top = 0;
    g_margin_bottom = g_rows - 1;
    g_origin_mode = 0;
    g_wrap_mode = 1;
    g_saved_row = 0;
    g_saved_col = 0;
    g_attr_state.fg = -1;
    g_attr_state.bg = -1;
    g_attr_state.attr = 0;
    pscalTerminalResetTabs();
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
    pscalTerminalQuerySize();
    g_margin_top = 0;
    g_margin_bottom = g_rows - 1;
    pscalTerminalResetTabs();
}

void pscalTerminalRender(const char *utf8, int len, int row, int col,
    long fg, long bg, int attr) {
    if (!utf8 || len <= 0) {
        return;
    }
    pscalTerminalClampCursor(&row, &col);
    pscalTerminalMove(row, col);
    pscalTerminalApplyAttrs(fg, bg, attr);
    fwrite(utf8, 1, (size_t)len, stdout);
    fflush(stdout);
}

void pscalTerminalClear(void) {
    printf("\033[2J\033[H");
    fflush(stdout);
}

void pscalTerminalClearEol(int row, int col) {
    pscalTerminalClampCursor(&row, &col);
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
    pscalTerminalClampCursor(&row, &col);
    pscalTerminalMove(row, col);
    printf("\033[0J");
    fflush(stdout);
}
void pscalTerminalClearScreenToCursor(int row, int col) {
    pscalTerminalClampCursor(&row, &col);
    pscalTerminalMove(row, col);
    printf("\033[1J");
    fflush(stdout);
}
void pscalTerminalInsertChars(int row, int col, int count) {
    pscalTerminalClampCursor(&row, &col);
    pscalTerminalMove(row, col);
    printf("\033[%d@", count);
    fflush(stdout);
}
void pscalTerminalDeleteChars(int row, int col, int count) {
    pscalTerminalClampCursor(&row, &col);
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
    printf("\033[?25%c", visible ? 'h' : 'l');
    fflush(stdout);
}

void pscalTerminalMoveCursor(int row, int col) {
    pscalTerminalClampCursor(&row, &col);
    pscalTerminalMove(row, col);
    fflush(stdout);
}

void pscalTerminalInsertLines(int row, int count) {
    if (count <= 0) count = 1;
    if (row < g_margin_top) row = g_margin_top;
    if (row > g_margin_bottom) row = g_margin_bottom;
    pscalTerminalMove(row, 0);
    printf("\033[%dL", count);
    fflush(stdout);
}

void pscalTerminalDeleteLines(int row, int count) {
    if (count <= 0) count = 1;
    if (row < g_margin_top) row = g_margin_top;
    if (row > g_margin_bottom) row = g_margin_bottom;
    pscalTerminalMove(row, 0);
    printf("\033[%dM", count);
    fflush(stdout);
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
