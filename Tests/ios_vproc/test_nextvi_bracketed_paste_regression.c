#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

void nextvi_reset_state(void);
int term_read(void);
int term_bracketed_paste_active(void);
int nextviTestResolveHostPathForAtomicWrite(const char *input, char *out, size_t out_sz);
const char *nextviTestVisiblePath(const char *path, char *buf, size_t buf_len);

extern _Thread_local unsigned int ibuf_sz;
extern _Thread_local unsigned char *ibuf;
extern _Thread_local unsigned int ibuf_cnt;
extern _Thread_local unsigned int ibuf_pos;
extern _Thread_local unsigned int icmd_pos;
extern _Thread_local unsigned int texec;
extern _Thread_local int xquit;
extern _Thread_local int xrows;
extern _Thread_local int xcols;

typedef struct VProc VProc;

typedef enum {
    INPUT_EVENT_BYTE = 0,
    INPUT_EVENT_TIMEOUT = 1,
    INPUT_EVENT_EOF = 2
} InputEventKind;

typedef struct {
    InputEventKind kind;
    unsigned char byte;
} InputEvent;

static const InputEvent *gEvents = NULL;
static size_t gEventCount = 0;
static size_t gEventIndex = 0;

static unsigned char *gAllocatedIbuf = NULL;
static char gHostRoot[PATH_MAX];
static char gVirtualCwd[PATH_MAX] = "/";

/* Runtime hooks referenced by nextvi iOS glue in test-only linkage. */
void pscalRuntimeDebugLog(const char *message) { (void)message; }
int pscalRuntimeOpenShellTab(void) { return -1; }
VProc *vprocCurrent(void) { return NULL; }
int vprocAdoptHostFd(VProc *vp, int host_fd) { (void)vp; return host_fd; }
int vprocHostClose(int fd) { return close(fd); }

bool pathTruncateExpand(const char *input_path, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    if (input_path == NULL) {
        out[0] = '\0';
        return false;
    }
    if (gHostRoot[0] == '\0') {
        snprintf(out, out_size, "%s", input_path);
        return true;
    }
    if (input_path[0] != '/') {
        snprintf(out, out_size, "%s", input_path);
        return true;
    }
    if (strncmp(input_path, gHostRoot, strlen(gHostRoot)) == 0) {
        snprintf(out, out_size, "%s", input_path);
        return true;
    }
    if (snprintf(out, out_size, "%s%s", gHostRoot, input_path) >= (int)out_size) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

bool pathTruncateStrip(const char *absolute_path, char *out, size_t out_size) {
    if (out == NULL || out_size == 0) {
        return false;
    }
    if (absolute_path == NULL) {
        out[0] = '\0';
        return false;
    }
    if (gHostRoot[0] != '\0') {
        size_t root_len = strlen(gHostRoot);
        if (strncmp(absolute_path, gHostRoot, root_len) == 0) {
            const char *suffix = absolute_path + root_len;
            if (*suffix == '\0') {
                snprintf(out, out_size, "/");
                return true;
            }
            if (*suffix == '/') {
                snprintf(out, out_size, "%s", suffix);
                return true;
            }
        }
    }
    snprintf(out, out_size, "%s", absolute_path);
    return true;
}

static int ensureDirRecursive(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        return -1;
    }
    memcpy(buf, path, len + 1);
    for (char *p = buf + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            (void)mkdir(buf, 0777);
            *p = '/';
        }
    }
    return mkdir(buf, 0777) == 0 || errno == EEXIST ? 0 : -1;
}

static int ensureParentDirs(const char *path) {
    char buf[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buf, path, len + 1);
    for (char *p = buf + len; p >= buf; --p) {
        if (*p == '/') {
            if (p == buf) {
                return 0;
            }
            *p = '\0';
            break;
        }
    }
    return ensureDirRecursive(buf);
}

static int resolveHostPath(const char *input, char *out, size_t out_size) {
    if (!input || !out || out_size == 0) {
        errno = EINVAL;
        return -1;
    }
    char virtual_path[PATH_MAX];
    if (input[0] == '/') {
        snprintf(virtual_path, sizeof(virtual_path), "%s", input);
    } else if (strcmp(gVirtualCwd, "/") == 0) {
        snprintf(virtual_path, sizeof(virtual_path), "/%s", input);
    } else {
        snprintf(virtual_path, sizeof(virtual_path), "%s/%s", gVirtualCwd, input);
    }
    if (!pathTruncateExpand(virtual_path, out, out_size)) {
        return -1;
    }
    return 0;
}

int pscalPathVirtualized_chdir(const char *path) {
    if (!path || path[0] == '\0') {
        errno = EINVAL;
        return -1;
    }
    if (path[0] == '/') {
        snprintf(gVirtualCwd, sizeof(gVirtualCwd), "%s", path);
    } else if (strcmp(gVirtualCwd, "/") == 0) {
        snprintf(gVirtualCwd, sizeof(gVirtualCwd), "/%s", path);
    } else {
        snprintf(gVirtualCwd, sizeof(gVirtualCwd), "%s/%s", gVirtualCwd, path);
    }
    return 0;
}

char *pscalPathVirtualized_getcwd(char *buffer, size_t size) {
    if (!buffer || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    if (snprintf(buffer, size, "%s", gVirtualCwd) >= (int)size) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return buffer;
}

int pscalPathVirtualized_open(const char *path, int oflag, ...) {
    char host_path[PATH_MAX];
    if (resolveHostPath(path, host_path, sizeof(host_path)) != 0) {
        return -1;
    }
    if (oflag & O_CREAT) {
        va_list ap;
        mode_t mode;
        va_start(ap, oflag);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
        if (ensureParentDirs(host_path) != 0 && errno != EEXIST) {
            return -1;
        }
        return open(host_path, oflag, mode);
    }
    return open(host_path, oflag);
}

FILE *pscalPathVirtualized_fopen(const char *path, const char *mode) {
    char host_path[PATH_MAX];
    if (resolveHostPath(path, host_path, sizeof(host_path)) != 0) {
        return NULL;
    }
    return fopen(host_path, mode);
}

int pscalPathVirtualized_stat(const char *path, struct stat *buf) {
    char host_path[PATH_MAX];
    if (resolveHostPath(path, host_path, sizeof(host_path)) != 0) {
        return -1;
    }
    return stat(host_path, buf);
}

int pscalPathVirtualized_lstat(const char *path, struct stat *buf) {
    char host_path[PATH_MAX];
    if (resolveHostPath(path, host_path, sizeof(host_path)) != 0) {
        return -1;
    }
    return lstat(host_path, buf);
}

int pscalPathVirtualized_unlink(const char *path) {
    char host_path[PATH_MAX];
    if (resolveHostPath(path, host_path, sizeof(host_path)) != 0) {
        return -1;
    }
    return unlink(host_path);
}

int pscalPathVirtualized_rename(const char *oldpath, const char *newpath) {
    char host_old[PATH_MAX];
    char host_new[PATH_MAX];
    if (resolveHostPath(oldpath, host_old, sizeof(host_old)) != 0) {
        return -1;
    }
    if (resolveHostPath(newpath, host_new, sizeof(host_new)) != 0) {
        return -1;
    }
    return rename(host_old, host_new);
}

DIR *pscalPathVirtualized_opendir(const char *name) {
    char host_path[PATH_MAX];
    if (resolveHostPath(name, host_path, sizeof(host_path)) != 0) {
        return NULL;
    }
    return opendir(host_path);
}

static void setupPathFixture(void) {
    if (gHostRoot[0] != '\0') {
        return;
    }
    char template_path[] = "/tmp/nextvi-path-regression-XXXXXX";
    char *root = mkdtemp(template_path);
    assert(root != NULL);
    snprintf(gHostRoot, sizeof(gHostRoot), "%s", root);
    assert(ensureDirRecursive("/tmp") == 0 || errno == EEXIST);
    assert(pscalPathVirtualized_chdir("/home/project") == 0);
    {
        char host_cwd[PATH_MAX];
        assert(resolveHostPath("/home/project", host_cwd, sizeof(host_cwd)) == 0);
        assert(ensureDirRecursive(host_cwd) == 0 || errno == EEXIST);
    }
}

static void setInputEvents(const InputEvent *events, size_t count) {
    gEvents = events;
    gEventCount = count;
    gEventIndex = 0;
}

static void resetTermStateForTest(void) {
    free(gAllocatedIbuf);
    gAllocatedIbuf = NULL;
    nextvi_reset_state();
    gAllocatedIbuf = malloc(ibuf_sz);
    assert(gAllocatedIbuf != NULL);
    ibuf = gAllocatedIbuf;
    ibuf_cnt = 0;
    ibuf_pos = 0;
    icmd_pos = 0;
    texec = 0;
    xquit = 0;
    xrows = 24;
    xcols = 80;
}

/* Nextvi iOS render bridge stubs (unused in this regression harness). */
void pscalTerminalBegin(int columns, int rows) { (void)columns; (void)rows; }
void pscalTerminalEnd(void) {}
void pscalTerminalRender(const char *utf8, int len, int row, int col, long fg, long bg, int attr) {
    (void)utf8;
    (void)len;
    (void)row;
    (void)col;
    (void)fg;
    (void)bg;
    (void)attr;
}
void pscalTerminalClear(void) {}
void pscalTerminalMoveCursor(int row, int col) { (void)row; (void)col; }
void pscalTerminalClearEol(int row, int col) { (void)row; (void)col; }
void pscalTerminalClearBol(int row, int col) { (void)row; (void)col; }
void pscalTerminalClearLine(int row) { (void)row; }
void pscalTerminalClearScreenFromCursor(int row, int col) { (void)row; (void)col; }
void pscalTerminalClearScreenToCursor(int row, int col) { (void)row; (void)col; }
void pscalTerminalInsertChars(int row, int col, int count) { (void)row; (void)col; (void)count; }
void pscalTerminalDeleteChars(int row, int col, int count) { (void)row; (void)col; (void)count; }
void pscalTerminalEnterAltScreen(void) {}
void pscalTerminalExitAltScreen(void) {}
void pscalTerminalSetCursorVisible(int visible) { (void)visible; }
void pscalTerminalInsertLines(int row, int count) { (void)row; (void)count; }
void pscalTerminalDeleteLines(int row, int count) { (void)row; (void)count; }
int pscalRuntimeDetectWindowRows(void) { return 24; }
int pscalRuntimeDetectWindowCols(void) { return 80; }

int pscalTerminalRead(unsigned char *buffer, int maxlen, int timeout_ms) {
    (void)timeout_ms;
    assert(buffer != NULL);
    assert(maxlen > 0);
    if (gEventIndex >= gEventCount) {
        return -1;
    }
    const InputEvent ev = gEvents[gEventIndex++];
    switch (ev.kind) {
        case INPUT_EVENT_BYTE:
            buffer[0] = ev.byte;
            return 1;
        case INPUT_EVENT_TIMEOUT:
            return 0;
        case INPUT_EVENT_EOF:
        default:
            return -1;
    }
}

static void testBracketedPasteMarkersAreFilteredAndStateTracked(void) {
    static const InputEvent events[] = {
        {INPUT_EVENT_BYTE, 0x1b},
        {INPUT_EVENT_BYTE, '['},
        {INPUT_EVENT_BYTE, '2'},
        {INPUT_EVENT_BYTE, '0'},
        {INPUT_EVENT_BYTE, '0'},
        {INPUT_EVENT_BYTE, '~'},
        {INPUT_EVENT_BYTE, 'a'},
        {INPUT_EVENT_BYTE, '\n'},
        {INPUT_EVENT_BYTE, 0x1b},
        {INPUT_EVENT_BYTE, '['},
        {INPUT_EVENT_BYTE, '2'},
        {INPUT_EVENT_BYTE, '0'},
        {INPUT_EVENT_BYTE, '1'},
        {INPUT_EVENT_BYTE, '~'},
        {INPUT_EVENT_BYTE, 'Z'},
        {INPUT_EVENT_EOF, 0}
    };

    resetTermStateForTest();
    setInputEvents(events, sizeof(events) / sizeof(events[0]));

    int c1 = term_read();
    assert(c1 == 'a');
    assert(term_bracketed_paste_active() == 1);

    int c2 = term_read();
    assert(c2 == '\n');
    assert(term_bracketed_paste_active() == 1);

    int c3 = term_read();
    assert(c3 == 'Z');
    assert(term_bracketed_paste_active() == 0);

    int c4 = term_read();
    assert(c4 == 0);
}

static void testEscFlushesOnTimeoutInsteadOfBeingLost(void) {
    static const InputEvent events[] = {
        {INPUT_EVENT_BYTE, 0x1b},
        {INPUT_EVENT_TIMEOUT, 0},
        {INPUT_EVENT_BYTE, 'x'},
        {INPUT_EVENT_EOF, 0}
    };

    resetTermStateForTest();
    setInputEvents(events, sizeof(events) / sizeof(events[0]));

    int c1 = term_read();
    assert(c1 == 0x1b);
    assert(term_bracketed_paste_active() == 0);

    int c2 = term_read();
    assert(c2 == 'x');

    int c3 = term_read();
    assert(c3 == 0);
}

static void testAtomicWriteResolverUsesVirtualCwdForRelativePaths(void) {
    setupPathFixture();
    char out[PATH_MAX];
    char expected[PATH_MAX];
    int rc = nextviTestResolveHostPathForAtomicWrite("note.txt", out, sizeof(out));
    assert(rc == 0);
    snprintf(expected, sizeof(expected), "%s/home/project/note.txt", gHostRoot);
    assert(strcmp(out, expected) == 0);
}

static void testAtomicWriteResolverExpandsAbsoluteVirtualPaths(void) {
    setupPathFixture();
    char out[PATH_MAX];
    char expected[PATH_MAX];
    int rc = nextviTestResolveHostPathForAtomicWrite("/etc/hosts", out, sizeof(out));
    assert(rc == 0);
    snprintf(expected, sizeof(expected), "%s/etc/hosts", gHostRoot);
    assert(strcmp(out, expected) == 0);
}

static void testVisiblePathStripsContainerPrefix(void) {
    setupPathFixture();
    char input[PATH_MAX];
    char stripped[PATH_MAX];
    snprintf(input, sizeof(input), "%s/home/project/note.txt", gHostRoot);
    const char *visible = nextviTestVisiblePath(input, stripped, sizeof(stripped));
    assert(visible != NULL);
    assert(strcmp(visible, "/home/project/note.txt") == 0);
}

int main(void) {
    fprintf(stderr, "TEST nextvi bracketed paste markers are filtered/stateful\n");
    testBracketedPasteMarkersAreFilteredAndStateTracked();

    fprintf(stderr, "TEST nextvi lone ESC survives timeout flush\n");
    testEscFlushesOnTimeoutInsteadOfBeingLost();

    fprintf(stderr, "TEST nextvi atomic-write resolver maps relative paths via virtual cwd\n");
    testAtomicWriteResolverUsesVirtualCwdForRelativePaths();

    fprintf(stderr, "TEST nextvi atomic-write resolver maps absolute virtual paths to host\n");
    testAtomicWriteResolverExpandsAbsoluteVirtualPaths();

    fprintf(stderr, "TEST nextvi visible-path stripping removes container host prefix\n");
    testVisiblePathStripsContainerPrefix();

    free(gAllocatedIbuf);
    gAllocatedIbuf = NULL;
    fprintf(stderr, "PASS nextvi iOS regression tests (paste + path virtualization)\n");
    return 0;
}
