#include <ctype.h>
#include <errno.h>
#include <glob.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <wchar.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <dirent.h>
#include "shell/parser.h"
#include "shell/semantics.h"
#include "shell/codegen.h"
#include "shell/opt.h"
#include "shell/builtins.h"
#include "vm/vm.h"
#include "shell/runner.h"
#include "core/preproc.h"
#include "core/build_info.h"
#include "core/cache.h"
#include "compiler/bytecode.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/register.h"
#include "Pascal/globals.h"
#include "vm/vm.h"
#include "symbol/symbol.h"
#include "common/frontend_kind.h"
#include "common/runtime_tty.h"
#if defined(PSCAL_TARGET_IOS)
#include "common/path_truncate.h"
#include "ios/vproc.h"
#endif

static struct termios gInteractiveOriginalTermios;
static volatile sig_atomic_t gInteractiveTermiosValid = 0;
static struct sigaction gInteractiveOldSigintAction;
static volatile sig_atomic_t gInteractiveHasOldSigint = 0;
static struct sigaction gInteractiveOldSigtstpAction;
static volatile sig_atomic_t gInteractiveHasOldSigtstp = 0;
static bool gInteractiveLineDrawn = false;
#if defined(PSCAL_TARGET_IOS)
static _Thread_local VProc *gKernelVproc = NULL;
static _Thread_local VProc *gShellSelfVproc = NULL;
static _Thread_local bool gShellSelfVprocActivated = false;
static void shellSetupSelfVproc(void) {
    if (gShellSelfVproc) {
        return;
    }

    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    if (!session_stdio) {
        static VProcSessionStdio fallback_stdio = { .stdin_host_fd = -1, .stdout_host_fd = -1, .stderr_host_fd = -1, .kernel_pid = 0 };
        vprocSessionStdioActivate(&fallback_stdio);
        session_stdio = vprocSessionStdioCurrent();
    }
    if (session_stdio && session_stdio->stdin_host_fd < 0 && session_stdio->stdout_host_fd < 0 && session_stdio->stderr_host_fd < 0) {
        vprocSessionStdioInit(session_stdio, 0);
    }
    if (session_stdio && vprocSessionStdioNeedsRefresh(session_stdio)) {
        vprocSessionStdioRefresh(session_stdio, 0);
    }

    if (!gKernelVproc) {
        VProcOptions kopts = vprocDefaultOptions();
        kopts.stdin_fd = (session_stdio && session_stdio->stdin_host_fd >= 0) ? session_stdio->stdin_host_fd : STDIN_FILENO;
        kopts.stdout_fd = (session_stdio && session_stdio->stdout_host_fd >= 0) ? session_stdio->stdout_host_fd : STDOUT_FILENO;
        kopts.stderr_fd = (session_stdio && session_stdio->stderr_host_fd >= 0) ? session_stdio->stderr_host_fd : STDERR_FILENO;
        int kpid_hint = vprocReservePid();
        kopts.pid_hint = kpid_hint;
        gKernelVproc = vprocCreate(&kopts);
        if (!gKernelVproc) {
            kopts.stdin_fd = -2;
            gKernelVproc = vprocCreate(&kopts);
        }
        if (gKernelVproc) {
            int kpid = vprocPid(gKernelVproc);
            vprocSetKernelPid(kpid);
            vprocSetSessionKernelPid(kpid);
            vprocSetParent(kpid, 0);
            (void)vprocSetSid(kpid, kpid);
            vprocSetCommandLabel(kpid, "kernel");
        } else if (kpid_hint > 0) {
            vprocSetKernelPid(kpid_hint);
            vprocSetSessionKernelPid(kpid_hint);
            vprocSetParent(kpid_hint, 0);
            (void)vprocSetSid(kpid_hint, kpid_hint);
            vprocSetCommandLabel(kpid_hint, "kernel");
            if (getenv("PSCALI_VPROC_DEBUG")) {
                fprintf(stderr, "[vproc] kernel vproc init failed; using pid=%d without fd table\n", kpid_hint);
            }
        }
    }

    int kernel_pid = vprocGetKernelPid();
    VProcSessionStdio *session_stdio_ref = vprocSessionStdioCurrent();
    if (session_stdio_ref) {
        if (session_stdio_ref->kernel_pid <= 0 && kernel_pid > 0) {
            session_stdio_ref->kernel_pid = kernel_pid;
        }
    }
    int session_stdin = (session_stdio_ref && session_stdio_ref->stdin_host_fd >= 0) ? session_stdio_ref->stdin_host_fd : -2;
    int session_stdout = (session_stdio_ref && session_stdio_ref->stdout_host_fd >= 0) ? session_stdio_ref->stdout_host_fd : -1;
    int session_stderr = (session_stdio_ref && session_stdio_ref->stderr_host_fd >= 0) ? session_stdio_ref->stderr_host_fd : -1;

    VProcOptions opts = vprocDefaultOptions();
    opts.stdin_fd = session_stdin;
    opts.stdout_fd = session_stdout;
    opts.stderr_fd = session_stderr;
    int shell_pid_hint = vprocReservePid();
    opts.pid_hint = shell_pid_hint;
    gShellSelfVproc = vprocCreate(&opts);
    if (!gShellSelfVproc) {
        /* Some iOS entrypoints may not have stdio wired up at the time the
         * shell starts. Fall back to /dev/null for stdin so we still get a
         * stable session leader entry for job control and process listings. */
        opts.stdin_fd = -2;
        gShellSelfVproc = vprocCreate(&opts);
    }
    if (gShellSelfVproc) {
        vprocRegisterThread(gShellSelfVproc, pthread_self());
        int shell_pid = vprocPid(gShellSelfVproc);
        vprocSetShellSelfPid(shell_pid);
        vprocSetShellSelfTid(pthread_self());
        if (kernel_pid > 0 && kernel_pid != shell_pid) {
            vprocSetParent(shell_pid, kernel_pid);
            (void)vprocSetSid(shell_pid, kernel_pid);
            (void)vprocSetPgid(shell_pid, shell_pid);
            (void)vprocSetForegroundPgid(kernel_pid, shell_pid);
        } else {
            (void)vprocSetSid(shell_pid, shell_pid);
        }
        vprocSetCommandLabel(shell_pid, "shell");
        if (kernel_pid > 0) {
            vprocSetParent(kernel_pid, 0);
        }
        /* Always activate the shell's vproc so shims and stdio inheritance work
         * consistently for pipelines and background workers. */
        vprocActivate(gShellSelfVproc);
        gShellSelfVprocActivated = true;
    } else if (shell_pid_hint > 0) {
        /* Ensure the shell has a stable synthetic pid even if the fd table
         * could not be initialised. */
        vprocSetShellSelfPid(shell_pid_hint);
        vprocSetShellSelfTid(pthread_self());
        if (kernel_pid > 0 && kernel_pid != shell_pid_hint) {
            vprocSetParent(shell_pid_hint, kernel_pid);
            (void)vprocSetSid(shell_pid_hint, kernel_pid);
            (void)vprocSetPgid(shell_pid_hint, shell_pid_hint);
            (void)vprocSetForegroundPgid(kernel_pid, shell_pid_hint);
        } else {
            (void)vprocSetSid(shell_pid_hint, shell_pid_hint);
        }
        vprocSetCommandLabel(shell_pid_hint, "shell");
        if (kernel_pid > 0) {
            vprocSetParent(kernel_pid, 0);
        }
        if (getenv("PSCALI_VPROC_DEBUG")) {
            fprintf(stderr, "[vproc] shell self-vproc init failed; using pid=%d without fd table\n", shell_pid_hint);
        }
    }
}

static void shellTeardownSelfVproc(int status) {
    if (!gShellSelfVproc) {
        return;
    }
    extern VProcSessionStdio *vprocSessionStdioCurrent(void);
    VProcSessionStdio *session_stdio = vprocSessionStdioCurrent();
    int sid = vprocGetSid(vprocPid(gShellSelfVproc));
    if (sid <= 0) {
        sid = vprocGetSid(vprocGetShellSelfPid());
    }
    if (sid > 0) {
        vprocTerminateSession(sid);
    }
    if (gShellSelfVprocActivated) {
        vprocDeactivate();
        gShellSelfVprocActivated = false;
    }
    vprocMarkExit(gShellSelfVproc, status);
    vprocDestroy(gShellSelfVproc);
    gShellSelfVproc = NULL;
    if (session_stdio) {
        if (session_stdio->stdin_host_fd >= 0) close(session_stdio->stdin_host_fd);
        if (session_stdio->stdout_host_fd >= 0) close(session_stdio->stdout_host_fd);
        if (session_stdio->stderr_host_fd >= 0) close(session_stdio->stderr_host_fd);
        session_stdio->stdin_host_fd = -1;
        session_stdio->stdout_host_fd = -1;
        session_stdio->stderr_host_fd = -1;
    }

    if (gKernelVproc) {
        vprocMarkExit(gKernelVproc, status);
        vprocDestroy(gKernelVproc);
        gKernelVproc = NULL;
    } else if (vprocGetKernelPid() > 0) {
        vprocDiscard(vprocGetKernelPid());
        vprocSetKernelPid(0);
    }
}
#endif

static bool interactiveUpdateScratch(char **scratch, const char *buffer, size_t length);
static size_t interactiveCommonPrefixLength(char **items, size_t count);

static int interactiveTerminalWidth(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        return ws.ws_col;
    }
    const char *columns_env = getenv("COLUMNS");
    if (columns_env && *columns_env) {
        char *endptr = NULL;
        long value = strtol(columns_env, &endptr, 10);
        if (endptr && *endptr == '\0' && value > 0 && value <= INT_MAX) {
            return (int)value;
        }
    }
    return 80;
}

static void interactiveAdvancePosition(size_t term_width,
                                       size_t *row,
                                       size_t *col,
                                       unsigned char ch) {
    if (!row || !col) {
        return;
    }
    if (ch == '\n') {
        (*row)++;
        *col = 0;
        return;
    }
    if (ch == '\r') {
        *col = 0;
        return;
    }
    if (ch == '\t') {
        size_t next_tab_stop = ((*col) / 8 + 1) * 8;
        *col = next_tab_stop;
    } else {
        (*col)++;
    }
    if (term_width > 0 && *col >= term_width) {
        *row += *col / term_width;
        *col %= term_width;
    }
}

static const unsigned char *interactiveSkipAnsiSequence(const unsigned char *p) {
    if (!p || *p != '\033') {
        return p;
    }
    ++p;
    if (!*p) {
        return p;
    }

    if (*p == '[') {
        ++p;
        while (*p && !(*p >= '@' && *p <= '~')) {
            ++p;
        }
        if (*p) {
            ++p;
        }
        return p;
    }

    if (*p == ']' || *p == 'P' || *p == '^' || *p == '_') {
        ++p;
        while (*p) {
            if (*p == '\a') {
                ++p;
                break;
            }
            if (*p == '\033' && p[1] == '\\') {
                p += 2;
                break;
            }
            ++p;
        }
        return p;
    }

    if ((*p >= '(' && *p <= '/') || *p == '%') {
        ++p;
        if (*p) {
            ++p;
        }
        return p;
    }

    if (*p) {
        ++p;
    }
    return p;
}

static int interactiveGlyphWidth(const char *s, size_t max_len, size_t *out_bytes) {
    if (out_bytes) {
        *out_bytes = 1;
    }
    if (!s || max_len == 0) {
        return 0;
    }
    mbstate_t st;
    memset(&st, 0, sizeof(st));
    wchar_t wc;
    size_t consumed = mbrtowc(&wc, s, max_len, &st);
    if (consumed == (size_t)-1 || consumed == (size_t)-2 || consumed == 0) {
        if (out_bytes) {
            *out_bytes = 1;
        }
        return 1;
    }
    if (out_bytes) {
        *out_bytes = consumed;
    }
    int width = wcwidth(wc);
    if (width < 0) {
        width = 0;
    }
    return width;
}

static void interactiveAdvanceColumns(size_t term_width,
                                      size_t *row,
                                      size_t *col,
                                      size_t width) {
    for (size_t i = 0; i < width; ++i) {
        interactiveAdvancePosition(term_width, row, col, ' ');
    }
}

static void interactiveComputeDisplayMetrics(const char *prompt,
                                             const char *buffer,
                                             size_t length,
                                             size_t cursor,
                                             size_t term_width,
                                             size_t *out_total_rows,
                                             size_t *out_cursor_row,
                                             size_t *out_cursor_col,
                                             size_t *out_end_row,
                                             size_t *out_end_col) {
    size_t row = 0;
    size_t col = 0;
    size_t total_rows = 1;

    if (prompt) {
        const char *p = prompt;
        while (*p) {
            if ((unsigned char)*p == '\033') {
                const unsigned char *next = interactiveSkipAnsiSequence((const unsigned char *)p);
                if (next == (const unsigned char *)p) {
                    ++p;
                } else {
                    p = (const char *)next;
                }
                continue;
            }
            if (*p == '\n' || *p == '\r' || *p == '\t') {
                interactiveAdvancePosition(term_width, &row, &col, (unsigned char)*p);
                if (*p == '\n' || *p == '\r') {
                    size_t used_rows = row + 1;
                    if (used_rows > total_rows) {
                        total_rows = used_rows;
                    }
                }
                ++p;
                continue;
            }
            size_t bytes = 1;
            int width = interactiveGlyphWidth(p, strlen(p), &bytes);
            if (width > 0) {
                interactiveAdvanceColumns(term_width, &row, &col, (size_t)width);
            }
            size_t used_rows = row + 1;
            if (used_rows > total_rows) {
                total_rows = used_rows;
            }
            p += bytes;
        }
    }

    if (cursor == 0 && out_cursor_row && out_cursor_col) {
        *out_cursor_row = row;
        *out_cursor_col = col;
    }

    if (buffer && length > 0) {
        size_t byte_index = 0;
        while (byte_index < length) {
            if (byte_index == cursor && out_cursor_row && out_cursor_col) {
                *out_cursor_row = row;
                *out_cursor_col = col;
            }
            unsigned char c = (unsigned char)buffer[byte_index];
            if (c == '\n' || c == '\r' || c == '\t') {
                interactiveAdvancePosition(term_width, &row, &col, c);
                if (c == '\n' || c == '\r') {
                    size_t used_rows = row + 1;
                    if (used_rows > total_rows) {
                        total_rows = used_rows;
                    }
                }
                byte_index++;
                continue;
            }
            size_t bytes = 1;
            int width = interactiveGlyphWidth(buffer + byte_index, length - byte_index, &bytes);
            if (width > 0) {
                interactiveAdvanceColumns(term_width, &row, &col, (size_t)width);
            }
            size_t used_rows = row + 1;
            if (used_rows > total_rows) {
                total_rows = used_rows;
            }
            byte_index += bytes;
        }
    }

    if (cursor >= length && out_cursor_row && out_cursor_col) {
        *out_cursor_row = row;
        *out_cursor_col = col;
    }

    if (out_end_row) {
        *out_end_row = row;
    }
    if (out_end_col) {
        *out_end_col = col;
    }
    if (out_total_rows) {
        *out_total_rows = total_rows;
    }
}

static void interactiveRestoreTerminal(void) {
    if (gInteractiveTermiosValid) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &gInteractiveOriginalTermios);
        gInteractiveTermiosValid = 0;
    }
}

static void interactiveRestoreSigintHandler(void) {
    if (gInteractiveHasOldSigint) {
        (void)sigaction(SIGINT, &gInteractiveOldSigintAction, NULL);
        gInteractiveHasOldSigint = 0;
    }
}

static void interactiveRestoreSigtstpHandler(void) {
    if (gInteractiveHasOldSigtstp) {
        (void)sigaction(SIGTSTP, &gInteractiveOldSigtstpAction, NULL);
        gInteractiveHasOldSigtstp = 0;
    }
}

static void interactiveSigintHandler(int signo) {
    interactiveRestoreSigintHandler();
    interactiveRestoreSigtstpHandler();
    interactiveRestoreTerminal();
    raise(signo);
}

static const char *SHELL_USAGE =
    "EXtensible SHell (sh/bash/zsh replacement)\n\n"
    "Usage: exsh <options> <script.sh> [args...]\n"
    "   Options:\n"
    "     -c <command> [arg0] [args...]  Execute command string. Optional arg0"
    "                                     becomes $0.\n"
    "     -v                          Display version information.\n"
    "     --dump-ast-json             Dump parsed AST as JSON.\n"
    "     --dump-bytecode             Disassemble generated bytecode.\n"
    "     --dump-bytecode-only        Disassemble bytecode and exit.\n"
    "     --dump-ext-builtins         List builtin commands.\n"
    "     --no-cache                  Compile fresh (ignore cached bytecode).\n"
    "     --semantic-warnings         Emit semantic analysis warnings.\n"
    "     --vm-trace-head=N           Trace first N VM instructions.\n"
    "     --verbose                 Print compilation/cache status messages.\n"
    "     -d                          Enable verbose VM error diagnostics.\n";

static char *readStream(FILE *stream) {
    if (!stream) {
        return NULL;
    }
    size_t capacity = 4096;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return NULL;
    }
    size_t length = 0;
    while (true) {
        if (capacity - length <= 1) {
            size_t new_capacity = capacity * 2;
            char *new_buffer = (char *)realloc(buffer, new_capacity);
            if (!new_buffer) {
                free(buffer);
                return NULL;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }
        size_t chunk_size = capacity - length - 1;
        size_t read_count = fread(buffer + length, 1, chunk_size, stream);
        length += read_count;
        if (read_count < chunk_size) {
            if (ferror(stream)) {
                free(buffer);
                return NULL;
            }
            break;
        }
    }
    buffer[length] = '\0';
    return buffer;
}

static bool promptBufferEnsureCapacity(char **buffer, size_t *capacity, size_t needed) {
    if (!buffer || !capacity) {
        return false;
    }
    if (needed <= *capacity) {
        return true;
    }
    size_t new_capacity = *capacity ? *capacity : 64;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    char *new_buffer = (char *)realloc(*buffer, new_capacity);
    if (!new_buffer) {
        return false;
    }
    *buffer = new_buffer;
    *capacity = new_capacity;
    return true;
}

static bool promptBufferAppendChar(char **buffer,
                                   size_t *length,
                                   size_t *capacity,
                                   char c) {
    if (!buffer || !length || !capacity) {
        return false;
    }
    size_t needed = (*length) + 2;
    if (!promptBufferEnsureCapacity(buffer, capacity, needed)) {
        return false;
    }
    (*buffer)[*length] = c;
    (*length)++;
    (*buffer)[*length] = '\0';
    return true;
}

static bool promptBufferAppendString(char **buffer,
                                     size_t *length,
                                     size_t *capacity,
                                     const char *text) {
    if (!text) {
        return true;
    }
    size_t text_len = strlen(text);
    if (text_len == 0) {
        return true;
    }
    size_t needed = (*length) + text_len + 1;
    if (!promptBufferEnsureCapacity(buffer, capacity, needed)) {
        return false;
    }
    memcpy((*buffer) + *length, text, text_len);
    *length += text_len;
    (*buffer)[*length] = '\0';
    return true;
}

static bool shellRunStartupConfig(const ShellRunOptions *base_options, int *out_status) {
    if (out_status) {
        *out_status = EXIT_SUCCESS;
    }
    const char *skip_rc = getenv("EXSH_SKIP_RC");
    if (skip_rc && skip_rc[0] && strcmp(skip_rc, "0") != 0) {
        return false;
    }
    const char *no_rc = getenv("EXSH_NO_RC");
    if (no_rc && no_rc[0] && strcmp(no_rc, "0") != 0) {
        return false;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return false;
    }
    size_t home_len = strlen(home);
    const char *rc_name = ".exshrc";
    bool needs_separator = (home_len > 0 && home[home_len - 1] == '/') ? false : true;
    size_t rc_name_len = strlen(rc_name);
    size_t path_len = home_len + (needs_separator ? 1 : 0) + rc_name_len + 1;
    char *rc_path = (char *)malloc(path_len);
    if (!rc_path) {
        return false;
    }
    if (needs_separator) {
        int written = snprintf(rc_path, path_len, "%s/%s", home, rc_name);
        if (written < 0 || (size_t)written >= path_len) {
            free(rc_path);
            return false;
        }
    } else {
        int written = snprintf(rc_path, path_len, "%s%s", home, rc_name);
        if (written < 0 || (size_t)written >= path_len) {
            free(rc_path);
            return false;
        }
    }
    const char *disable_name = ".exshrc.disable";
    size_t disable_len = strlen(disable_name);
    size_t disable_path_len = home_len + (needs_separator ? 1 : 0) + disable_len + 1;
    char *disable_path = (char *)malloc(disable_path_len);
    if (disable_path) {
        if (needs_separator) {
            snprintf(disable_path, disable_path_len, "%s/%s", home, disable_name);
        } else {
            snprintf(disable_path, disable_path_len, "%s%s", home, disable_name);
        }
        if (access(disable_path, F_OK) == 0) {
            fprintf(stderr,
                    "exsh: startup file disabled by '%s'\n",
                    disable_path);
            free(disable_path);
            free(rc_path);
            return false;
        }
        free(disable_path);
    }
    if (access(rc_path, F_OK) != 0) {
        free(rc_path);
        return false;
    }
    char *source = shellLoadFile(rc_path);
    if (!source) {
        free(rc_path);
        return false;
    }
    if (source[0] == '#' && source[1] == '!') {
        const char *line_end = strchr(source, '\n');
        size_t line_len = line_end ? (size_t)(line_end - source) : strlen(source);
        bool looks_like_exsh = false;
        if (line_len > 2) {
            const char *interp = source + 2;
            const char *interp_end = source + line_len;
            for (const char *p = interp; p + 3 < interp_end; ++p) {
                if (p[0] == 'e' && p[1] == 'x' && p[2] == 's' && p[3] == 'h') {
                    looks_like_exsh = true;
                    break;
                }
            }
        }
        if (!looks_like_exsh) {
            fprintf(stderr,
                    "exsh: skipping startup file '%s' (non-exsh shebang)\n",
                    rc_path);
            free(source);
            free(rc_path);
            return false;
        }
    }

    ShellRunOptions rc_options = {0};
    if (base_options) {
        rc_options.verbose_errors = base_options->verbose_errors;
        rc_options.frontend_path = base_options->frontend_path;
        rc_options.suppress_warnings = base_options->suppress_warnings;
    } else {
        rc_options.suppress_warnings = true;
    }
    rc_options.no_cache = 1;
    rc_options.quiet = base_options ? base_options->quiet : true;

    const char *restore_path = (base_options && base_options->frontend_path)
                                   ? base_options->frontend_path
                                   : NULL;
    gParamCount = 0;
    gParamValues = NULL;
    shellRuntimeSetArg0(rc_path);
    bool exit_requested = false;
    int status = shellRunSource(source, rc_path, &rc_options, &exit_requested);
    free(source);
    shellRuntimeSetArg0(restore_path);
    free(rc_path);
    if (out_status) {
        *out_status = status;
    }
    return exit_requested;
}

static bool promptBufferAppendTime(char **buffer,
                                   size_t *length,
                                   size_t *capacity,
                                   const char *format) {
    time_t now = time(NULL);
    struct tm tm_info;
    struct tm *tm_ptr = localtime(&now);
    if (!tm_ptr) {
        return true;
    }
    tm_info = *tm_ptr;
    char formatted[64];
    size_t written = strftime(formatted, sizeof(formatted), format, &tm_info);
    if (written == 0) {
        return true;
    }
    formatted[written] = '\0';
    return promptBufferAppendString(buffer, length, capacity, formatted);
}

static bool promptBufferAppendWorkingDir(char **buffer,
                                         size_t *length,
                                         size_t *capacity,
                                         bool basename_only) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        return true;
    }
    char display[PATH_MAX + 2];
    strncpy(display, cwd, sizeof(display));
    display[sizeof(display) - 1] = '\0';

    const char *home = getenv("HOME");
    if (home && *home) {
        size_t home_len = strlen(home);
        if (home_len > 0 && strncmp(display, home, home_len) == 0 &&
            (display[home_len] == '/' || display[home_len] == '\0')) {
            char replaced[PATH_MAX + 2];
            replaced[0] = '~';
            size_t rest_len = strlen(display + home_len);
            if (rest_len > sizeof(replaced) - 2) {
                rest_len = sizeof(replaced) - 2;
            }
            memcpy(replaced + 1, display + home_len, rest_len + 1);
            strncpy(display, replaced, sizeof(display));
            display[sizeof(display) - 1] = '\0';
        }
    }

    size_t disp_len = strlen(display);
    while (disp_len > 1 && display[disp_len - 1] == '/') {
        display[disp_len - 1] = '\0';
        disp_len--;
    }

    const char *segment = display;
    if (basename_only) {
        if (strcmp(display, "~") == 0) {
            segment = "~";
        } else {
            char *slash = strrchr(display, '/');
            if (slash) {
                if (slash == display) {
                    if (slash[1] == '\0') {
                        segment = "/";
                    } else {
                        segment = slash + 1;
                    }
                } else if (slash[1] != '\0') {
                    segment = slash + 1;
                } else {
                    *slash = '\0';
                    slash = strrchr(display, '/');
                    if (!slash) {
                        segment = display;
                    } else if (slash[1] != '\0') {
                        segment = slash + 1;
                    } else if (slash == display) {
                        segment = "/";
                    }
                }
            }
        }
    }

    return promptBufferAppendString(buffer, length, capacity, segment);
}

static char *shellFormatPrompt(const char *input) {
    if (!input) {
        return NULL;
    }
    char *buffer = NULL;
    size_t length = 0;
    size_t capacity = 0;

    for (size_t i = 0; input[i]; ++i) {
        char c = input[i];
        if (c != '\\') {
            if (!promptBufferAppendChar(&buffer, &length, &capacity, c)) {
                free(buffer);
                return NULL;
            }
            continue;
        }

        ++i;
        char next = input[i];
        if (next == '\0') {
            if (!promptBufferAppendChar(&buffer, &length, &capacity, '\\')) {
                free(buffer);
                return NULL;
            }
            break;
        }

        switch (next) {
            case '[':
            case ']':
                break;
            case '\\':
                if (!promptBufferAppendChar(&buffer, &length, &capacity, '\\')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'a':
                if (!promptBufferAppendChar(&buffer, &length, &capacity, '\a')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'e':
            case 'E':
                if (!promptBufferAppendChar(&buffer, &length, &capacity, '\033')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'n':
                if (!promptBufferAppendChar(&buffer, &length, &capacity, '\n')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'r':
                if (!promptBufferAppendChar(&buffer, &length, &capacity, '\r')) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 't':
                if (!promptBufferAppendTime(&buffer, &length, &capacity, "%H:%M:%S")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'T':
                if (!promptBufferAppendTime(&buffer, &length, &capacity, "%I:%M:%S")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '@':
                if (!promptBufferAppendTime(&buffer, &length, &capacity, "%I:%M%p")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'A':
                if (!promptBufferAppendTime(&buffer, &length, &capacity, "%H:%M")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'd':
                if (!promptBufferAppendTime(&buffer, &length, &capacity, "%a %b %d")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'D':
                if (!promptBufferAppendTime(&buffer, &length, &capacity, "%m/%d/%y")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'w':
                if (!promptBufferAppendWorkingDir(&buffer, &length, &capacity, false)) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'W':
                if (!promptBufferAppendWorkingDir(&buffer, &length, &capacity, true)) {
                    free(buffer);
                    return NULL;
                }
                break;
            case 'u': {
                const char *user = getenv("USER");
                if (!user || !*user) {
                    user = getenv("USERNAME");
                }
                if (user && *user) {
                    if (!promptBufferAppendString(&buffer, &length, &capacity, user)) {
                        free(buffer);
                        return NULL;
                    }
                }
                break;
            }
            case 'h':
            case 'H': {
                char hostname[256];
                if (gethostname(hostname, sizeof(hostname)) == 0) {
                    hostname[sizeof(hostname) - 1] = '\0';
                    if (next == 'h') {
                        char *dot = strchr(hostname, '.');
                        if (dot) {
                            *dot = '\0';
                        }
                    }
                    if (!promptBufferAppendString(&buffer, &length, &capacity, hostname)) {
                        free(buffer);
                        return NULL;
                    }
                }
                break;
            }
            case 's':
                if (!promptBufferAppendString(&buffer, &length, &capacity, "exsh")) {
                    free(buffer);
                    return NULL;
                }
                break;
            case '$': {
                char symbol = (geteuid() == 0) ? '#' : '$';
                if (!promptBufferAppendChar(&buffer, &length, &capacity, symbol)) {
                    free(buffer);
                    return NULL;
                }
                break;
            }
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7': {
                int value = next - '0';
                size_t consumed = 0;
                while (consumed < 2 && input[i + 1] >= '0' && input[i + 1] <= '7') {
                    value = (value * 8) + (input[i + 1] - '0');
                    ++i;
                    ++consumed;
                }
                if (!promptBufferAppendChar(&buffer, &length, &capacity, (char)value)) {
                    free(buffer);
                    return NULL;
                }
                break;
            }
            case 'x':
            case 'X': {
                int value = 0;
                size_t consumed = 0;
                while (consumed < 2) {
                    char hex = input[i + 1];
                    if (!isxdigit((unsigned char)hex)) {
                        break;
                    }
                    value *= 16;
                    if (hex >= '0' && hex <= '9') {
                        value += hex - '0';
                    } else {
                        value += 10 + (tolower((unsigned char)hex) - 'a');
                    }
                    ++i;
                    ++consumed;
                }
                if (consumed == 0) {
                    if (!promptBufferAppendChar(&buffer, &length, &capacity, next)) {
                        free(buffer);
                        return NULL;
                    }
                } else {
                    if (!promptBufferAppendChar(&buffer, &length, &capacity, (char)value)) {
                        free(buffer);
                        return NULL;
                    }
                }
                break;
            }
            default:
                if (!promptBufferAppendChar(&buffer, &length, &capacity, next)) {
                    free(buffer);
                    return NULL;
                }
                break;
        }
    }

    if (!buffer) {
        buffer = strdup("");
    }
    return buffer;
}

static char *shellResolveInteractivePrompt(void) {
    const char *env_prompt = getenv("PS1");
    if (!env_prompt || !*env_prompt) {
        env_prompt = "\\e[38;5;39mexsh\\e[0m \\e[1;35m\\W\\e[0m ⚡ ";
    }
    char *formatted = shellFormatPrompt(env_prompt);
    if (formatted) {
        return formatted;
    }
    return strdup(env_prompt);
}

static size_t shellPromptLineBreakCount(const char *prompt) {
    size_t total_rows = 1;
    size_t term_width = (size_t)interactiveTerminalWidth();
    interactiveComputeDisplayMetrics(prompt,
                                     NULL,
                                     0,
                                     0,
                                     term_width,
                                     &total_rows,
                                     NULL,
                                     NULL,
                                     NULL,
                                     NULL);
    if (total_rows == 0) {
        return 0;
    }
    return (total_rows > 0) ? (total_rows - 1) : 0;
}

static void redrawInteractiveLine(const char *prompt,
                                  const char *buffer,
                                  size_t length,
                                  size_t cursor,
                                  size_t *displayed_length,
                                  size_t *displayed_prompt_lines) {
    size_t previous_prompt_lines =
        displayed_prompt_lines ? *displayed_prompt_lines : 0;
    size_t term_width = (size_t)interactiveTerminalWidth();

    size_t total_rows = 1;
    size_t cursor_row = 0;
    size_t cursor_col = 0;
    size_t end_row = 0;
    size_t end_col = 0;
    interactiveComputeDisplayMetrics(prompt,
                                     buffer,
                                     length,
                                     cursor,
                                     term_width,
                                     &total_rows,
                                     &cursor_row,
                                     &cursor_col,
                                     &end_row,
                                     &end_col);

    size_t rows_to_prompt = gInteractiveLineDrawn ? previous_prompt_lines : 0;
    for (size_t i = 0; i < rows_to_prompt; ++i) {
        fputs("\033[A", stdout);
    }
    fputs("\r", stdout);
    fputs("\033[J", stdout);
    if (prompt) {
        fputs(prompt, stdout);
    }
    if (buffer && length > 0) {
        fwrite(buffer, 1, length, stdout);
    }
    fflush(stdout);

    if (end_row > cursor_row) {
        size_t diff = end_row - cursor_row;
        for (size_t i = 0; i < diff; ++i) {
            fputs("\033[A", stdout);
        }
    } else if (cursor_row > end_row) {
        size_t diff = cursor_row - end_row;
        for (size_t i = 0; i < diff; ++i) {
            fputs("\033[B", stdout);
        }
    }

    if (end_col > cursor_col) {
        size_t diff = end_col - cursor_col;
        for (size_t i = 0; i < diff; ++i) {
            fputs("\033[D", stdout);
        }
    } else if (cursor_col > end_col) {
        size_t diff = cursor_col - end_col;
        for (size_t i = 0; i < diff; ++i) {
            fputs("\033[C", stdout);
        }
    }
    fflush(stdout);

    if (displayed_length) {
        *displayed_length = length;
    }
    if (displayed_prompt_lines) {
        *displayed_prompt_lines =
            (total_rows > 0) ? (total_rows - 1) : 0;
    }
    gInteractiveLineDrawn = true;
}

static bool interactiveEnsureCapacity(char **buffer, size_t *capacity, size_t needed) {
    if (!buffer || !capacity) {
        return false;
    }
    if (needed <= *capacity) {
        return true;
    }
    size_t new_capacity = *capacity ? *capacity : 128;
    while (new_capacity < needed) {
        if (new_capacity > SIZE_MAX / 2) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2;
    }
    char *new_buffer = (char *)realloc(*buffer, new_capacity);
    if (!new_buffer) {
        return false;
    }
    *buffer = new_buffer;
    *capacity = new_capacity;
    return true;
}

static bool interactiveSetKillBuffer(char **kill_buffer, const char *text, size_t length) {
    if (!kill_buffer) {
        return false;
    }
    char *replacement = NULL;
    if (text && length > 0) {
        replacement = (char *)malloc(length + 1);
        if (!replacement) {
            return false;
        }
        memcpy(replacement, text, length);
        replacement[length] = '\0';
    }
    free(*kill_buffer);
    *kill_buffer = replacement;
    return true;
}

static bool interactiveInsertText(char **buffer,
                                  size_t *length,
                                  size_t *capacity,
                                  size_t *cursor,
                                  const char *text,
                                  size_t text_len) {
    if (!buffer || !length || !capacity || !cursor || !text) {
        return false;
    }
    if (!interactiveEnsureCapacity(buffer, capacity, (*length) + text_len + 1)) {
        return false;
    }
    if (*cursor < *length) {
        memmove((*buffer) + *cursor + text_len,
                (*buffer) + *cursor,
                (*length) - *cursor);
    }
    memcpy((*buffer) + *cursor, text, text_len);
    *cursor += text_len;
    *length += text_len;
    (*buffer)[*length] = '\0';
    return true;
}

static bool interactiveHistoryNavigateUp(const char *prompt,
                                         char **buffer,
                                         size_t *length,
                                         size_t *cursor,
                                         size_t *capacity,
                                         size_t *displayed_length,
                                         size_t *displayed_prompt_lines,
                                         size_t *history_index,
                                         char **scratch) {
    if (!prompt || !buffer || !length || !cursor || !capacity || !displayed_length ||
        !displayed_prompt_lines || !history_index || !scratch) {
        return false;
    }
    size_t history_count = shellRuntimeHistoryCount();
    if (*history_index >= history_count) {
        return false;
    }
    if (*history_index == 0) {
        if (!interactiveUpdateScratch(scratch, *buffer, *length)) {
            return false;
        }
    }
    (*history_index)++;
    char *entry = NULL;
    if (!shellRuntimeHistoryGetEntry((*history_index) - 1, &entry)) {
        (*history_index)--;
        return false;
    }
    size_t entry_len = strlen(entry);
    if (!interactiveEnsureCapacity(buffer, capacity, entry_len + 1)) {
        free(entry);
        (*history_index)--;
        return false;
    }
    memcpy(*buffer, entry, entry_len);
    (*buffer)[entry_len] = '\0';
    *length = entry_len;
    *cursor = *length;
    redrawInteractiveLine(prompt,
                          *buffer,
                          *length,
                          *cursor,
                          displayed_length,
                          displayed_prompt_lines);
    free(entry);
    return true;
}

static bool interactiveHistoryNavigateDown(const char *prompt,
                                           char **buffer,
                                           size_t *length,
                                           size_t *cursor,
                                           size_t *capacity,
                                           size_t *displayed_length,
                                           size_t *displayed_prompt_lines,
                                           size_t *history_index,
                                           char **scratch) {
    if (!prompt || !buffer || !length || !cursor || !capacity || !displayed_length ||
        !displayed_prompt_lines || !history_index || !scratch) {
        return false;
    }
    if (*history_index == 0) {
        return false;
    }
    (*history_index)--;
    const char *replacement = "";
    char *entry = NULL;
    if (*history_index > 0) {
        if (shellRuntimeHistoryGetEntry((*history_index) - 1, &entry)) {
            replacement = entry;
        }
    } else if (*scratch) {
        replacement = *scratch;
    }
    size_t entry_len = strlen(replacement);
    if (!interactiveEnsureCapacity(buffer, capacity, entry_len + 1)) {
        free(entry);
        (*history_index)++;
        return false;
    }
    memcpy(*buffer, replacement, entry_len);
    (*buffer)[entry_len] = '\0';
    *length = entry_len;
    *cursor = *length;
    redrawInteractiveLine(prompt,
                          *buffer,
                          *length,
                          *cursor,
                          displayed_length,
                          displayed_prompt_lines);
    if (*history_index == 0) {
        interactiveUpdateScratch(scratch, *buffer, *length);
    }
    free(entry);
    return true;
}

static bool interactiveExtractLastArgument(size_t skip_commands,
                                           char **out_argument) {
    if (!out_argument) {
        return false;
    }
    *out_argument = NULL;
    size_t history_count = shellRuntimeHistoryCount();
    size_t index = skip_commands;
    while (index < history_count) {
        char *entry = NULL;
        if (!shellRuntimeHistoryGetEntry(index, &entry)) {
            index++;
            continue;
        }
        size_t len = strlen(entry);
        if (len == 0) {
            free(entry);
            index++;
            continue;
        }
        ssize_t pos = (ssize_t)len - 1;
        while (pos >= 0 && isspace((unsigned char)entry[pos])) {
            pos--;
        }
        if (pos < 0) {
            free(entry);
            index++;
            continue;
        }
        ssize_t end = pos;
        while (pos >= 0 && !isspace((unsigned char)entry[pos])) {
            pos--;
        }
        size_t arg_len = (size_t)(end - pos);
        char *argument = (char *)malloc(arg_len + 1);
        if (!argument) {
            free(entry);
            return false;
        }
        memcpy(argument, entry + pos + 1, arg_len);
        argument[arg_len] = '\0';
        free(entry);
        *out_argument = argument;
        return true;
    }
    return true;
}

static bool interactiveFindHistoryMatch(const char *query,
                                        size_t start_offset,
                                        char **out_entry,
                                        size_t *out_reverse_index) {
    if (out_entry) {
        *out_entry = NULL;
    }
    size_t history_count = shellRuntimeHistoryCount();
    size_t query_len = query ? strlen(query) : 0;
    for (size_t reverse_index = start_offset; reverse_index < history_count; ++reverse_index) {
        char *candidate = NULL;
        if (!shellRuntimeHistoryGetEntry(reverse_index, &candidate)) {
            continue;
        }
        bool matches = (query_len == 0) ||
                       (candidate && strstr(candidate, query) != NULL);
        if (matches) {
            if (out_entry) {
                *out_entry = candidate;
            } else {
                free(candidate);
            }
            if (out_reverse_index) {
                *out_reverse_index = reverse_index;
            }
            return true;
        }
        free(candidate);
    }
    return false;
}

static void interactiveRenderSearchPrompt(const char *query, const char *match) {
    fputs("\r", stdout);
    fputs("\033[K", stdout);
    if (match) {
        fprintf(stdout, "(reverse-i-search) '%s': %s", query ? query : "", match);
    } else {
        fprintf(stdout, "(reverse-i-search) '%s': ", query ? query : "");
    }
    fflush(stdout);
}

static bool interactiveReverseSearch(const char *prompt,
                                     char **buffer,
                                     size_t *length,
                                     size_t *capacity,
                                     size_t *cursor,
                                     size_t *displayed_length,
                                     size_t *displayed_prompt_lines,
                                     size_t *history_index,
                                     char **scratch,
                                     bool *out_submit_line) {
    if (!prompt || !buffer || !length || !capacity || !cursor || !displayed_length ||
        !displayed_prompt_lines || !history_index || !scratch || !out_submit_line) {
        return false;
    }
    *out_submit_line = false;

    char *saved_line = (char *)malloc((*length) + 1);
    if (!saved_line) {
        return false;
    }
    memcpy(saved_line, *buffer, *length);
    saved_line[*length] = '\0';
    size_t saved_length = *length;
    size_t saved_cursor = *cursor;

    size_t query_capacity = 32;
    char *query = (char *)malloc(query_capacity);
    if (!query) {
        free(saved_line);
        return false;
    }
    size_t query_length = 0;
    query[0] = '\0';

    char *match = NULL;
    size_t match_index = 0;
    bool has_match = interactiveFindHistoryMatch(query, 0, &match, &match_index);
    interactiveRenderSearchPrompt(query, has_match ? match : NULL);

    while (true) {
        unsigned char input = 0;
        ssize_t count = read(STDIN_FILENO, &input, 1);
        if (count <= 0) {
            break;
        }

        if (input == 7) { /* Ctrl-G */
            if (!interactiveEnsureCapacity(buffer, capacity, saved_length + 1)) {
                break;
            }
            memcpy(*buffer, saved_line, saved_length + 1);
            *length = saved_length;
            *cursor = saved_cursor;
            fputs("\r", stdout);
            fputs("\033[K", stdout);
            redrawInteractiveLine(prompt,
                                  *buffer,
                                  *length,
                                  *cursor,
                                  displayed_length,
                                  displayed_prompt_lines);
            interactiveUpdateScratch(scratch, *buffer, *length);
            free(match);
            free(query);
            free(saved_line);
            return true;
        }

        if (input == '\r' || input == '\n') {
            if (has_match && match) {
                size_t match_len = strlen(match);
                if (!interactiveEnsureCapacity(buffer, capacity, match_len + 1)) {
                    break;
                }
                memcpy(*buffer, match, match_len + 1);
                *length = match_len;
                *cursor = *length;
                redrawInteractiveLine(prompt,
                                      *buffer,
                                      *length,
                                      *cursor,
                                      displayed_length,
                                      displayed_prompt_lines);
                interactiveUpdateScratch(scratch, *buffer, *length);
                *history_index = 0;
                *out_submit_line = true;
            }
            break;
        }

        if (input == 18) { /* Ctrl-R cycles matches */
            if (has_match) {
                size_t next_start = match_index + 1;
                free(match);
                match = NULL;
                has_match = interactiveFindHistoryMatch(query, next_start, &match, &match_index);
                if (!has_match) {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
            }
            interactiveRenderSearchPrompt(query, has_match ? match : NULL);
            continue;
        }

        if (input == 127 || input == 8) { /* Backspace */
            if (query_length > 0) {
                query_length--;
                query[query_length] = '\0';
                size_t next_start = 0;
                free(match);
                match = NULL;
                has_match = interactiveFindHistoryMatch(query, next_start, &match, &match_index);
                if (!has_match) {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            interactiveRenderSearchPrompt(query, has_match ? match : NULL);
            continue;
        }

        if (isprint(input)) {
            if (query_length + 1 >= query_capacity) {
                size_t new_capacity = query_capacity * 2;
                char *new_query = (char *)realloc(query, new_capacity);
                if (!new_query) {
                    break;
                }
                query = new_query;
                query_capacity = new_capacity;
            }
            query[query_length++] = (char)input;
            query[query_length] = '\0';
            size_t next_start = 0;
            free(match);
            match = NULL;
            has_match = interactiveFindHistoryMatch(query, next_start, &match, &match_index);
            if (!has_match) {
                fputc('\a', stdout);
                fflush(stdout);
            }
            interactiveRenderSearchPrompt(query, has_match ? match : NULL);
            continue;
        }

        fputc('\a', stdout);
        fflush(stdout);
    }

    fputs("\r", stdout);
    fputs("\033[K", stdout);
    redrawInteractiveLine(prompt,
                          *buffer,
                          *length,
                          *cursor,
                          displayed_length,
                          displayed_prompt_lines);

    free(match);
    free(query);
    free(saved_line);
    return true;
}

static bool interactiveUpdateScratch(char **scratch, const char *buffer, size_t length) {
    if (!scratch) {
        return false;
    }
    char *new_scratch = (char *)realloc(*scratch, length + 1);
    if (!new_scratch) {
        return false;
    }
    if (buffer && length > 0) {
        memcpy(new_scratch, buffer, length);
    }
    new_scratch[length] = '\0';
    *scratch = new_scratch;
    return true;
}

static size_t interactivePreviousWord(const char *buffer, size_t length, size_t cursor) {
    if (!buffer || length == 0 || cursor == 0) {
        return 0;
    }
    size_t pos = cursor;
    while (pos > 0) {
        unsigned char c = (unsigned char)buffer[pos - 1];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        pos--;
    }
    while (pos > 0) {
        unsigned char c = (unsigned char)buffer[pos - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            break;
        }
        pos--;
    }
    return pos;
}

static size_t interactiveNextWord(const char *buffer, size_t length, size_t cursor) {
    if (!buffer || cursor >= length) {
        return length;
    }
    size_t pos = cursor;
    while (pos < length) {
        unsigned char c = (unsigned char)buffer[pos];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            break;
        }
        pos++;
    }
    while (pos < length) {
        unsigned char c = (unsigned char)buffer[pos];
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            break;
        }
        pos++;
    }
    return pos;
}

static size_t interactiveFindWordStart(const char *buffer, size_t length) {
    if (!buffer || length == 0) {
        return length;
    }
    size_t index = length;
    while (index > 0) {
        unsigned char c = (unsigned char)buffer[index - 1];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            break;
        }
        index--;
    }
    return index;
}

static bool interactiveExtractCommandToken(const char *buffer,
                                           size_t length,
                                           size_t word_start,
                                           size_t *out_start,
                                           size_t *out_len) {
    if (!buffer || word_start > length) {
        return false;
    }

    size_t command_start = 0;
    for (size_t i = 0; i < word_start; ++i) {
        unsigned char c = (unsigned char)buffer[i];
        if (c == ';' || c == '&' || c == '|' || c == '\n' || c == '\r' ||
            c == '(' || c == ')') {
            command_start = i + 1;
        }
    }

    while (command_start < word_start &&
           isspace((unsigned char)buffer[command_start])) {
        command_start++;
    }

    if (command_start >= length) {
        return false;
    }

    size_t command_end = command_start;
    while (command_end < length) {
        unsigned char c = (unsigned char)buffer[command_end];
        if (isspace(c) || c == ';' || c == '&' || c == '|' ||
            c == '(' || c == ')') {
            break;
        }
        command_end++;
    }

    if (command_end <= command_start) {
        return false;
    }

    if (out_start) {
        *out_start = command_start;
    }
    if (out_len) {
        *out_len = command_end - command_start;
    }
    return true;
}

static bool interactiveWordLooksDynamic(const char *word) {
    if (!word || !*word) {
        return false;
    }
    bool escaped = false;
    for (const char *cursor = word; *cursor; ++cursor) {
        if (escaped) {
            escaped = false;
            continue;
        }
        if (*cursor == '\\') {
            escaped = true;
            continue;
        }
        if (*cursor == '\'' || *cursor == '"' || *cursor == '$' || *cursor == '`') {
            return true;
        }
    }
    return false;
}

static size_t interactiveCommonPrefixLength(char **items, size_t count) {
    if (!items || count == 0) {
        return 0;
    }
    const char *first = items[0];
    if (!first) {
        return 0;
    }
    size_t prefix_len = strlen(first);
    for (size_t i = 1; i < count && prefix_len > 0; ++i) {
        const char *item = items[i];
        if (!item) {
            prefix_len = 0;
            break;
        }
        size_t j = 0;
        while (j < prefix_len && first[j] == item[j]) {
            ++j;
        }
        prefix_len = j;
    }
    return prefix_len;
}

static void interactiveFreeMatches(char **matches, size_t count) {
    if (!matches) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(matches[i]);
    }
    free(matches);
}

typedef struct {
    const char *prefix;
    size_t prefix_len;
    char ***matches;
    size_t *count;
    size_t *capacity;
} InteractiveCompletionContext;

static bool interactiveAddCompletionMatch(const char *name,
                                          const char *prefix,
                                          size_t prefix_len,
                                          char ***matches,
                                          size_t *count,
                                          size_t *capacity) {
    if (!name || !*name || !matches || !count || !capacity) {
        return true;
    }
    if (prefix_len > 0 && strncasecmp(name, prefix, prefix_len) != 0) {
        return true;
    }
    for (size_t i = 0; i < *count; ++i) {
        const char *existing = (*matches)[i];
        if (existing && strcasecmp(existing, name) == 0) {
            return true;
        }
    }
    if (*count >= *capacity) {
        size_t new_cap = (*capacity > 0) ? (*capacity * 2) : 8;
        char **resized = (char **)realloc(*matches, new_cap * sizeof(char *));
        if (!resized) {
            return false;
        }
        *matches = resized;
        *capacity = new_cap;
    }
    char *copy = strdup(name);
    if (!copy) {
        return false;
    }
    (*matches)[(*count)++] = copy;
    return true;
}

static void interactiveBuiltinCompletionVisitor(const char *name,
                                                const char *canonical,
                                                int id,
                                                void *context) {
    (void)canonical;
    (void)id;
    InteractiveCompletionContext *ctx = (InteractiveCompletionContext *)context;
    if (!ctx) {
        return;
    }
    (void)interactiveAddCompletionMatch(name,
                                        ctx->prefix,
                                        ctx->prefix_len,
                                        ctx->matches,
                                        ctx->count,
                                        ctx->capacity);
}

static bool interactiveCollectPathExecutables(const char *prefix,
                                              size_t prefix_len,
                                              char ***matches,
                                              size_t *count,
                                              size_t *capacity) {
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) {
        return true;
    }

    char *copy = strdup(path_env);
    if (!copy) {
        return false;
    }

    bool ok = true;
    char *saveptr = NULL;
    char *dir = strtok_r(copy, ":", &saveptr);
    while (ok && dir) {
        const char *real_dir = (*dir == '\0') ? "." : dir;
        DIR *d = opendir(real_dir);
        if (d) {
            struct dirent *ent = NULL;
            while (ok && (ent = readdir(d)) != NULL) {
                if (ent->d_name[0] == '\0') {
                    continue;
                }
                if (ent->d_name[0] == '.' && prefix_len == 0) {
                    continue;
                }
                char full[PATH_MAX];
                size_t dir_len = strlen(real_dir);
                size_t name_len = strlen(ent->d_name);
                if (dir_len + 1 + name_len + 1 >= sizeof(full)) {
                    continue;
                }
                memcpy(full, real_dir, dir_len);
                if (dir_len > 0 && real_dir[dir_len - 1] != '/') {
                    full[dir_len] = '/';
                    dir_len += 1;
                }
                memcpy(full + dir_len, ent->d_name, name_len + 1);
                struct stat st;
                if (stat(full, &st) != 0) {
                    continue;
                }
                if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) {
                    continue;
                }
                if (access(full, X_OK) != 0) {
                    continue;
                }
                if (!interactiveAddCompletionMatch(ent->d_name,
                                                   prefix,
                                                   prefix_len,
                                                   matches,
                                                   count,
                                                   capacity)) {
                    ok = false;
                    break;
                }
            }
            closedir(d);
        }
        dir = strtok_r(NULL, ":", &saveptr);
    }

    free(copy);
    return ok;
}

static void interactivePrintMatchesInColumns(char *const *items, size_t count) {
    if (!items || count == 0) {
        return;
    }

    size_t valid_count = 0;
    size_t max_len = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!items[i]) {
            continue;
        }
        size_t len = strlen(items[i]);
        if (len > max_len) {
            max_len = len;
        }
        valid_count++;
    }
    if (valid_count == 0) {
        return;
    }

    char **entries = (char **)malloc(valid_count * sizeof(char *));
    if (!entries) {
        for (size_t i = 0; i < count; ++i) {
            if (!items[i]) {
                continue;
            }
            fputs(items[i], stdout);
            fputc('\n', stdout);
        }
        return;
    }

    size_t index = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!items[i]) {
            continue;
        }
        entries[index++] = items[i];
    }

    int width = interactiveTerminalWidth();
    if (width <= 0) {
        width = 80;
    }

    size_t col_width = (max_len > 0 ? max_len : 1) + 2;
    if ((size_t)width <= col_width) {
        col_width = (max_len > 0 ? max_len : 1) + 1;
    }

    size_t columns = (size_t)width / col_width;
    if (columns == 0) {
        columns = 1;
    }
    size_t rows = (valid_count + columns - 1) / columns;

    for (size_t row = 0; row < rows; ++row) {
        for (size_t col = 0; col < columns; ++col) {
            size_t entry_index = col * rows + row;
            if (entry_index >= valid_count) {
                continue;
            }
            const char *entry = entries[entry_index];
            size_t len = strlen(entry);
            fputs(entry, stdout);
            if (col + 1 < columns) {
                size_t next_index = (col + 1) * rows + row;
                if (next_index < valid_count) {
                    size_t padding = (col_width > len) ? (col_width - len) : 1;
                    for (size_t pad = 0; pad < padding; ++pad) {
                        fputc(' ', stdout);
                    }
                }
            }
        }
        fputc('\n', stdout);
    }

    free(entries);
}

static bool interactiveHandleTabCompletion(const char *prompt,
                                           char **buffer,
                                           size_t *length,
                                           size_t *cursor,
                                           size_t *capacity,
                                           size_t *displayed_length,
                                           size_t *displayed_prompt_lines,
                                           char **scratch) {
    if (!buffer || !*buffer || !length || !capacity || !cursor) {
        return false;
    }
    if (*cursor != *length) {
        return false;
    }
    size_t word_start = interactiveFindWordStart(*buffer, *length);
    char *word = *buffer + word_start;
    size_t word_len = *length - word_start;
    if (interactiveWordLooksDynamic(word)) {
        return false;
    }
    for (size_t i = 0; i < word_len; ++i) {
        if (word[i] == '*' || word[i] == '?' || word[i] == '[') {
            return false;
        }
    }

    const char *glob_base = word;
    size_t glob_base_len = word_len;
    bool had_trailing_slash = (word_len > 0 && word[word_len - 1] == '/');
#if defined(PSCAL_TARGET_IOS)
    bool glob_used_virtual = false;
    if (pathTruncateEnabled() && word_len > 0 && word[0] == '/') {
        char word_copy[PATH_MAX];
        if (word_len < sizeof(word_copy)) {
            memcpy(word_copy, word, word_len);
            word_copy[word_len] = '\0';
            char expanded[PATH_MAX];
            if (pathTruncateExpand(word_copy, expanded, sizeof(expanded))) {
                glob_base = expanded;
                glob_base_len = strlen(expanded);
                glob_used_virtual = true;
            }
        }
    }
#endif
    while (glob_base_len > 1 && glob_base[glob_base_len - 1] == '/') {
        glob_base_len--;
    }

    size_t command_start = 0;
    size_t command_len = 0;
    bool completing_command = false;
    bool command_is_cd = false;
    if (interactiveExtractCommandToken(*buffer,
                                       *length,
                                       word_start,
                                       &command_start,
                                       &command_len)) {
        completing_command = (word_start == command_start);
        if (command_len == 2 && strncasecmp(*buffer + command_start, "cd", command_len) == 0) {
            command_is_cd = true;
        }
    }

    if (completing_command && strchr(word, '/') == NULL) {
        char **matches = NULL;
        size_t match_count = 0;
        size_t match_capacity = 0;

        InteractiveCompletionContext ctx = {
            .prefix = word,
            .prefix_len = word_len,
            .matches = &matches,
            .count = &match_count,
            .capacity = &match_capacity
        };
        shellVisitBuiltins(interactiveBuiltinCompletionVisitor, &ctx);
        bool path_ok = interactiveCollectPathExecutables(word, word_len, &matches, &match_count, &match_capacity);
        if (match_count > 0) {
            size_t replacement_len = 0;
            bool append_space = false;
            if (match_count == 1) {
                replacement_len = strlen(matches[0]);
                append_space = true;
            } else {
                replacement_len = interactiveCommonPrefixLength(matches, match_count);
                if (replacement_len <= word_len) {
                    putchar('\n');
                    interactivePrintMatchesInColumns(matches, match_count);
                    interactiveFreeMatches(matches, match_count);
                    fflush(stdout);

                    *cursor = *length;
                    redrawInteractiveLine(prompt,
                                          *buffer,
                                          *length,
                                          *cursor,
                                          displayed_length,
                                          displayed_prompt_lines);
                    if (scratch) {
                        interactiveUpdateScratch(scratch, *buffer, *length);
                    }
                    return true;
                }
            }

            size_t total_len = word_start + replacement_len + (append_space ? 1 : 0);
            if (!interactiveEnsureCapacity(buffer, capacity, total_len + 1)) {
                interactiveFreeMatches(matches, match_count);
                return false;
            }

            memcpy(*buffer + word_start, matches[0], replacement_len);
            if (append_space) {
                (*buffer)[word_start + replacement_len] = ' ';
                replacement_len += 1;
            }
            *length = word_start + replacement_len;
            (*buffer)[*length] = '\0';

            *cursor = *length;
            redrawInteractiveLine(prompt,
                                  *buffer,
                                  *length,
                                  *cursor,
                                  displayed_length,
                                  displayed_prompt_lines);
            if (scratch) {
                interactiveUpdateScratch(scratch, *buffer, *length);
            }
            interactiveFreeMatches(matches, match_count);
            return true;
        }
        interactiveFreeMatches(matches, match_count);
        if (!path_ok) {
            return false;
        }
    }

    size_t pattern_len = glob_base_len + (had_trailing_slash ? 3 : 2);
    char *pattern = (char *)malloc(pattern_len);
    if (!pattern) {
        return false;
    }
    memcpy(pattern, glob_base, glob_base_len);
    size_t write_idx = glob_base_len;
    if (had_trailing_slash) {
        if (write_idx == 0 || pattern[write_idx - 1] != '/') {
            pattern[write_idx++] = '/';
        }
    }
    pattern[write_idx++] = '*';
    pattern[write_idx] = '\0';

    glob_t results;
    memset(&results, 0, sizeof(results));
    int glob_flags = GLOB_TILDE | GLOB_MARK;
    int glob_status = glob(pattern, glob_flags, NULL, &results);
    free(pattern);
    if (glob_status != 0 || results.gl_pathc == 0) {
        globfree(&results);
        return false;
    }

#if defined(PSCAL_TARGET_IOS)
    if (glob_used_virtual) {
        for (size_t i = 0; i < results.gl_pathc; ++i) {
            const char *match = results.gl_pathv[i];
            if (!match) {
                continue;
            }
            char stripped[PATH_MAX];
            if (pathTruncateStrip(match, stripped, sizeof(stripped))) {
                char *copy = strdup(stripped);
                if (copy) {
                    free(results.gl_pathv[i]);
                    results.gl_pathv[i] = copy;
                }
            }
        }
    }
#endif

    /* command_is_cd is computed above when finding the command token. */
    if (command_is_cd) {
        size_t write_index = 0;
        for (size_t i = 0; i < results.gl_pathc; ++i) {
            const char *match = results.gl_pathv[i];
            if (!match) {
                continue;
            }
            size_t match_len = strlen(match);
            if (match_len > 0 && match[match_len - 1] == '/') {
                results.gl_pathv[write_index++] = results.gl_pathv[i];
            }
        }
        results.gl_pathc = write_index;
        if (results.gl_pathc == 0) {
            globfree(&results);
            return false;
        }
    }

    size_t replacement_len = 0;
    bool append_space = false;
    bool append_slash = false;
    if (results.gl_pathc == 1) {
        const char *match = results.gl_pathv[0];
        if (!match) {
            globfree(&results);
            return false;
        }
        replacement_len = strlen(match);
        if (replacement_len > 0 && match[replacement_len - 1] != '/') {
            append_space = true;
        }
    } else {
        replacement_len = interactiveCommonPrefixLength(results.gl_pathv, results.gl_pathc);
        if (replacement_len <= word_len) {
            putchar('\n');
            interactivePrintMatchesInColumns(results.gl_pathv, results.gl_pathc);
            globfree(&results);
            fflush(stdout);

            *cursor = *length;
            redrawInteractiveLine(prompt,
                                  *buffer,
                                  *length,
                                  *cursor,
                                  displayed_length,
                                  displayed_prompt_lines);
            if (scratch) {
                interactiveUpdateScratch(scratch, *buffer, *length);
            }
            return true;
        }
    }

    /* If PATH_TRUNCATE is active, double-check directory matches so we keep the trailing slash. */
#if defined(PSCAL_TARGET_IOS)
    if (glob_used_virtual && results.gl_pathv[0] && replacement_len > 0) {
        const char *visible = results.gl_pathv[0];
        char expanded[PATH_MAX];
        const char *real_path = visible;
        if (pathTruncateExpand(visible, expanded, sizeof(expanded))) {
            real_path = expanded;
        }
        struct stat st;
        if (real_path && stat(real_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (visible[replacement_len - 1] != '/') {
                append_slash = true;
            }
            append_space = false;
        }
    }
#endif

    size_t total_len = word_start + replacement_len + (append_slash ? 1 : 0) + (append_space ? 1 : 0);
    if (!interactiveEnsureCapacity(buffer, capacity, total_len + 1)) {
        globfree(&results);
        return false;
    }

    memcpy(*buffer + word_start, results.gl_pathv[0], replacement_len);
    if (append_slash) {
        (*buffer)[word_start + replacement_len] = '/';
        replacement_len += 1;
    }
    if (append_space) {
        (*buffer)[word_start + replacement_len] = ' ';
        replacement_len += 1;
    }
    *length = word_start + replacement_len;
    (*buffer)[*length] = '\0';
    globfree(&results);

    *cursor = *length;
    redrawInteractiveLine(prompt,
                          *buffer,
                          *length,
                          *cursor,
                          displayed_length,
                          displayed_prompt_lines);
    if (scratch) {
        interactiveUpdateScratch(scratch, *buffer, *length);
    }
    return true;
}

static char *interactiveExpandTilde(const char *line) {
    if (!line) {
        return NULL;
    }
    const char *home = getenv("HOME");
    if (!home || !*home) {
        return strdup(line);
    }
    size_t home_len = strlen(home);
    size_t capacity = strlen(line) + home_len + 1;
    char *result = (char *)malloc(capacity);
    if (!result) {
        return NULL;
    }
    size_t out_len = 0;
    bool in_single_quote = false;
    bool in_double_quote = false;
    bool escaped = false;
    for (size_t i = 0; line[i] != '\0'; ++i) {
        unsigned char c = (unsigned char)line[i];
        if (escaped) {
            if (!interactiveEnsureCapacity(&result, &capacity, out_len + 2)) {
                free(result);
                return NULL;
            }
            result[out_len++] = (char)c;
            escaped = false;
            continue;
        }
        if (c == '\\') {
            if (!interactiveEnsureCapacity(&result, &capacity, out_len + 2)) {
                free(result);
                return NULL;
            }
            result[out_len++] = (char)c;
            escaped = true;
            continue;
        }
        if (c == '\'' && !in_double_quote) {
            in_single_quote = !in_single_quote;
        } else if (c == '"' && !in_single_quote) {
            in_double_quote = !in_double_quote;
        }
        bool expand = false;
        if (!in_single_quote && !in_double_quote && c == '~') {
            char prev = (i > 0) ? line[i - 1] : '\0';
            if (i == 0 || prev == ' ' || prev == '\t' || prev == '\n' || prev == '\r' || prev == '=') {
                char next = line[i + 1];
                if (next == '/' || next == '\0') {
                    expand = true;
                }
            }
        }
        if (expand) {
            if (!interactiveEnsureCapacity(&result, &capacity, out_len + home_len + 1)) {
                free(result);
                return NULL;
            }
            memcpy(result + out_len, home, home_len);
            out_len += home_len;
            continue;
        }
        if (!interactiveEnsureCapacity(&result, &capacity, out_len + 2)) {
            free(result);
            return NULL;
        }
        result[out_len++] = (char)c;
    }
    char *new_result = (char *)realloc(result, out_len + 1);
    if (new_result) {
        result = new_result;
    }
    result[out_len] = '\0';
    return result;
}

static char *interactiveRewriteCombinedRedirects(const char *line) {
    if (!line) {
        return NULL;
    }
    size_t len = strlen(line);
    size_t capacity = len + 32;
    char *out = (char *)malloc(capacity);
    if (!out) {
        return NULL;
    }
    size_t out_len = 0;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (size_t i = 0; line[i] != '\0'; ++i) {
        char c = line[i];
        if (escaped) {
            if (out_len + 2 >= capacity) {
                capacity *= 2;
                char *resized = (char *)realloc(out, capacity);
                if (!resized) {
                    free(out);
                    return NULL;
                }
                out = resized;
            }
            out[out_len++] = c;
            escaped = false;
            continue;
        }
        if (c == '\\' && !in_single) {
            if (out_len + 2 >= capacity) {
                capacity *= 2;
                char *resized = (char *)realloc(out, capacity);
                if (!resized) {
                    free(out);
                    return NULL;
                }
                out = resized;
            }
            out[out_len++] = c;
            escaped = true;
            continue;
        }
        if (c == '\'' && !in_double) {
            in_single = !in_single;
        } else if (c == '"' && !in_single) {
            in_double = !in_double;
        }

        if (!in_single && !in_double && c == '&' && line[i + 1] == '>') {
            bool append = (line[i + 2] == '>');
            size_t j = i + (append ? 3 : 2);
            /* Skip whitespace */
            while (line[j] == ' ' || line[j] == '\t') {
                j++;
            }
            /* Capture the following word (respect simple quotes/backslashes) */
            bool word_single = false, word_double = false, word_escaped = false;
            size_t start = j;
            size_t end = j;
            for (; line[end] != '\0'; ++end) {
                char wc = line[end];
                if (word_escaped) {
                    word_escaped = false;
                    continue;
                }
                if (wc == '\\') {
                    word_escaped = true;
                    continue;
                }
                if (wc == '\'' && !word_double) {
                    word_single = !word_single;
                    continue;
                }
                if (wc == '"' && !word_single) {
                    word_double = !word_double;
                    continue;
                }
                if (!word_single && !word_double && isspace((unsigned char)wc)) {
                    break;
                }
            }
            if (start >= end) {
                /* No target found; emit original chars */
                goto copy_char;
            }
            size_t path_len = end - start;
            /* Emit > or >> then space then path then " 2>&1" */
            const char *extra = " 2>&1";
            size_t extra_len = strlen(extra);
            size_t needed = out_len + (append ? 2 : 1) + 1 + path_len + extra_len + 1;
            if (needed > capacity) {
                capacity = needed + len + 16;
                char *resized = (char *)realloc(out, capacity);
                if (!resized) {
                    free(out);
                    return NULL;
                }
                out = resized;
            }
            out[out_len++] = '>';
            if (append) {
                out[out_len++] = '>';
            }
            out[out_len++] = ' ';
            memcpy(out + out_len, line + start, path_len);
            out_len += path_len;
            memcpy(out + out_len, extra, extra_len);
            out_len += extra_len;
            i = end - 1;
            continue;
        }

copy_char:
        if (out_len + 2 >= capacity) {
            capacity *= 2;
            char *resized = (char *)realloc(out, capacity);
            if (!resized) {
                free(out);
                return NULL;
            }
            out = resized;
        }
        out[out_len++] = c;
    }

    out[out_len] = '\0';
    return out;
}

static char *readInteractiveLine(const char *prompt,
                                 bool *out_eof,
                                 bool *out_editor_failed) {
    bool installed_sigint_handler = false;
    const bool has_real_tty = pscalRuntimeStdinHasRealTTY() &&
                              !pscalRuntimeVirtualTTYEnabled();
    if (out_eof) {
        *out_eof = false;
    }
    if (out_editor_failed) {
        *out_editor_failed = false;
    }
    if (!prompt) {
        prompt = "";
    }

    struct termios original_termios;
    struct termios raw_termios;
    struct sigaction sigint_action;
    struct sigaction sigtstp_action;
    if (has_real_tty) {
        if (tcgetattr(STDIN_FILENO, &original_termios) != 0) {
            if (out_editor_failed) {
                *out_editor_failed = true;
            }
            return NULL;
        }

        raw_termios = original_termios;
        raw_termios.c_lflag &= ~(ICANON | ECHO);
        raw_termios.c_cc[VMIN] = 1;
        raw_termios.c_cc[VTIME] = 0;
        gInteractiveOriginalTermios = original_termios;
        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios) != 0) {
            if (out_editor_failed) {
                *out_editor_failed = true;
            }
            return NULL;
        }
        gInteractiveTermiosValid = 1;

        memset(&sigint_action, 0, sizeof(sigint_action));
        sigemptyset(&sigint_action.sa_mask);
        sigint_action.sa_handler = interactiveSigintHandler;
        if (sigaction(SIGINT, &sigint_action, &gInteractiveOldSigintAction) != 0) {
            interactiveRestoreTerminal();
            if (out_editor_failed) {
                *out_editor_failed = true;
            }
            return NULL;
        }
        gInteractiveHasOldSigint = 1;
        installed_sigint_handler = true;

        memset(&sigtstp_action, 0, sizeof(sigtstp_action));
        sigemptyset(&sigtstp_action.sa_mask);
#if !defined(PSCAL_TARGET_IOS)
        sigtstp_action.sa_handler = SIG_IGN;
        if (sigaction(SIGTSTP, &sigtstp_action, &gInteractiveOldSigtstpAction) != 0) {
            interactiveRestoreSigintHandler();
            installed_sigint_handler = false;
            interactiveRestoreTerminal();
            if (out_editor_failed) {
                *out_editor_failed = true;
            }
            return NULL;
        }
        gInteractiveHasOldSigtstp = 1;
#endif
    } else {
        memset(&original_termios, 0, sizeof(original_termios));
        memset(&raw_termios, 0, sizeof(raw_termios));
        memset(&sigint_action, 0, sizeof(sigint_action));
        memset(&sigtstp_action, 0, sizeof(sigtstp_action));
        gInteractiveTermiosValid = 0;
    }

    size_t capacity = 128;
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        if (installed_sigint_handler) {
            interactiveRestoreSigintHandler();
        }
        interactiveRestoreSigtstpHandler();
        interactiveRestoreTerminal();
        if (out_editor_failed) {
            *out_editor_failed = true;
        }
        return NULL;
    }
    buffer[0] = '\0';
    size_t length = 0;
    size_t displayed_length = 0;
    size_t displayed_prompt_lines = shellPromptLineBreakCount(prompt);
    gInteractiveLineDrawn = false;
    size_t cursor = 0;
    size_t history_index = 0;
    char *scratch = NULL;
    interactiveUpdateScratch(&scratch, buffer, length);
    char *kill_buffer = NULL;
    size_t alt_dot_offset = 0;
    bool alt_dot_active = false;

    redrawInteractiveLine(prompt,
                          buffer,
                          length,
                          cursor,
                          &displayed_length,
                          &displayed_prompt_lines);

    bool done = false;
    bool eof_requested = false;

    while (!done) {
        unsigned char ch = 0;
        ssize_t read_count = read(STDIN_FILENO, &ch, 1);
        if (read_count < 0) {
            if (errno == EINTR) {
                continue;
            }
            eof_requested = true;
            break;
        }

        if (read_count == 0) {
            eof_requested = true;
            break;
        }

        if (ch == '\r' || ch == '\n') {
            fputc('\n', stdout);
            fflush(stdout);
            done = true;
            alt_dot_active = false;
            break;
        }

        if (ch == 4) { /* Ctrl-D */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (length == 0) {
                eof_requested = true;
                break;
            }
            if (cursor < length) {
                memmove(buffer + cursor, buffer + cursor + 1, length - cursor);
                length--;
                buffer[length] = '\0';
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 3) { /* Ctrl-C */
            alt_dot_active = false;
            alt_dot_offset = 0;
            fputs("^C\n", stdout);
            fflush(stdout);
#if defined(PSCAL_TARGET_IOS)
            raise(SIGINT);
            shellRuntimeProcessPendingSignals();
#endif
            length = 0;
            cursor = 0;
            buffer[0] = '\0';
            displayed_length = 0;
            displayed_prompt_lines = shellPromptLineBreakCount(prompt);
            history_index = 0;
            interactiveUpdateScratch(&scratch, buffer, length);
            fputs(prompt, stdout);
            fflush(stdout);
            continue;
        }

        if (ch == 26) { /* Ctrl-Z */
#if defined(PSCAL_TARGET_IOS)
            /* In virtual TTY mode, deliver SIGTSTP to the running builtin/VM and reset the prompt. */
            alt_dot_active = false;
            alt_dot_offset = 0;
            fputs("^Z\n", stdout);
            fflush(stdout);
            raise(SIGTSTP);
#if defined(PSCAL_TARGET_IOS)
            shellRuntimeProcessPendingSignals();
#endif
            length = 0;
            cursor = 0;
            buffer[0] = '\0';
            displayed_length = 0;
            displayed_prompt_lines = shellPromptLineBreakCount(prompt);
            history_index = 0;
            interactiveUpdateScratch(&scratch, buffer, length);
            redrawInteractiveLine(prompt,
                                  buffer,
                                  length,
                                  cursor,
                                  &displayed_length,
                                  &displayed_prompt_lines);
            continue;
#endif
            if (!has_real_tty || pscalRuntimeVirtualTTYEnabled()) {
                fputs("job control (Ctrl-Z) not supported on this terminal\n", stdout);
                fflush(stdout);
                continue;
            }
            alt_dot_active = false;
            alt_dot_offset = 0;
            interactiveRestoreSigintHandler();
            installed_sigint_handler = false;
            interactiveRestoreSigtstpHandler();
            interactiveRestoreTerminal();
            raise(SIGTSTP);
            if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw_termios) != 0) {
                if (out_editor_failed) {
                    *out_editor_failed = true;
                }
                done = true;
                break;
            }
            gInteractiveTermiosValid = 1;
            if (sigaction(SIGINT, &sigint_action, &gInteractiveOldSigintAction) != 0) {
                interactiveRestoreTerminal();
                if (out_editor_failed) {
                    *out_editor_failed = true;
                }
                done = true;
                break;
            }
            gInteractiveHasOldSigint = 1;
            installed_sigint_handler = true;
            if (sigaction(SIGTSTP, &sigtstp_action, &gInteractiveOldSigtstpAction) != 0) {
                interactiveRestoreSigintHandler();
                installed_sigint_handler = false;
                interactiveRestoreTerminal();
                if (out_editor_failed) {
                    *out_editor_failed = true;
                }
                done = true;
                break;
            }
            gInteractiveHasOldSigtstp = 1;
            redrawInteractiveLine(prompt,
                                  buffer,
                                  length,
                                  cursor,
                                  &displayed_length,
                                  &displayed_prompt_lines);
            continue;
        }

        if (ch == 12) { /* Ctrl-L */
            alt_dot_active = false;
            alt_dot_offset = 0;
            fputs("\033[H\033[J", stdout);
            redrawInteractiveLine(prompt,
                                  buffer,
                                  length,
                                  cursor,
                                  &displayed_length,
                                  &displayed_prompt_lines);
            continue;
        }

        if (ch == 19) { /* Ctrl-S */
            alt_dot_active = false;
            alt_dot_offset = 0;
            tcflow(STDOUT_FILENO, TCOOFF);
            continue;
        }

        if (ch == 17) { /* Ctrl-Q */
            alt_dot_active = false;
            alt_dot_offset = 0;
            tcflow(STDOUT_FILENO, TCOON);
            continue;
        }

        if (ch == 16) { /* Ctrl-P */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (!interactiveHistoryNavigateUp(prompt,
                                              &buffer,
                                              &length,
                                              &cursor,
                                              &capacity,
                                              &displayed_length,
                                              &displayed_prompt_lines,
                                              &history_index,
                                              &scratch)) {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 14) { /* Ctrl-N */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (!interactiveHistoryNavigateDown(prompt,
                                                &buffer,
                                                &length,
                                                &cursor,
                                                &capacity,
                                                &displayed_length,
                                                &displayed_prompt_lines,
                                                &history_index,
                                                &scratch)) {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 18) { /* Ctrl-R */
            alt_dot_active = false;
            alt_dot_offset = 0;
            bool submit_line = false;
            if (!interactiveReverseSearch(prompt,
                                          &buffer,
                                          &length,
                                          &capacity,
                                          &cursor,
                                          &displayed_length,
                                          &displayed_prompt_lines,
                                          &history_index,
                                          &scratch,
                                          &submit_line)) {
                fputc('\a', stdout);
                fflush(stdout);
            } else if (submit_line) {
                fputc('\n', stdout);
                fflush(stdout);
                done = true;
                break;
            }
            continue;
        }

        if (ch == 21) { /* Ctrl-U */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor > 0) {
                if (!interactiveSetKillBuffer(&kill_buffer, buffer, cursor)) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    continue;
                }
                memmove(buffer, buffer + cursor, length - cursor + 1);
                length -= cursor;
                cursor = 0;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 11) { /* Ctrl-K */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor < length) {
                if (!interactiveSetKillBuffer(&kill_buffer, buffer + cursor, length - cursor)) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    continue;
                }
                buffer[cursor] = '\0';
                length = cursor;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                interactiveSetKillBuffer(&kill_buffer, NULL, 0);
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 23) { /* Ctrl-W */
            alt_dot_active = false;
            alt_dot_offset = 0;
            size_t prev = interactivePreviousWord(buffer, length, cursor);
            if (prev < cursor) {
                size_t removed_len = cursor - prev;
                if (!interactiveSetKillBuffer(&kill_buffer, buffer + prev, removed_len)) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    continue;
                }
                memmove(buffer + prev, buffer + cursor, length - cursor + 1);
                length -= removed_len;
                cursor = prev;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 25) { /* Ctrl-Y */
            alt_dot_offset = 0;
            if (kill_buffer && *kill_buffer) {
                size_t kill_len = strlen(kill_buffer);
                if (!interactiveInsertText(&buffer,
                                           &length,
                                           &capacity,
                                           &cursor,
                                           kill_buffer,
                                           kill_len)) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    continue;
                }
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            alt_dot_active = false;
            continue;
        }

        if (ch == 20) { /* Ctrl-T */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (length >= 2 && cursor > 0) {
                size_t pos1 = (cursor == length) ? length - 2 : cursor - 1;
                size_t pos2 = (cursor == length) ? length - 1 : cursor;
                char tmp = buffer[pos1];
                buffer[pos1] = buffer[pos2];
                buffer[pos2] = tmp;
                if (cursor < length) {
                    cursor++;
                }
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 1) { /* Ctrl-A */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor > 0) {
                cursor = 0;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 5) { /* Ctrl-E */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor < length) {
                cursor = length;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 2) { /* Ctrl-B */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor > 0) {
                cursor--;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 6) { /* Ctrl-F */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor < length) {
                cursor++;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
            }
            continue;
        }

        if (ch == 127 || ch == 8) { /* Backspace */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (cursor > 0) {
                memmove(buffer + cursor - 1, buffer + cursor, length - cursor + 1);
                cursor--;
                length--;
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
            } else {
                fputc('\a', stdout);
                fflush(stdout);
                /* Ensure the prompt stays intact even when backspace is hit at
                 * the start of input. */
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
            }
            continue;
        }

        if (ch == 27) { /* Escape sequence */
            unsigned char seq[3];
            if (read(STDIN_FILENO, &seq[0], 1) <= 0) {
                continue;
            }
            if (seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) <= 0) {
                    continue;
                }
                if (seq[1] == 'A') { /* Up arrow */
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    if (!interactiveHistoryNavigateUp(prompt,
                                                      &buffer,
                                                      &length,
                                                      &cursor,
                                                      &capacity,
                                                      &displayed_length,
                                                      &displayed_prompt_lines,
                                                      &history_index,
                                                      &scratch)) {
                        fputc('\a', stdout);
                        fflush(stdout);
                    }
                    continue;
                } else if (seq[1] == 'B') { /* Down arrow */
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    if (!interactiveHistoryNavigateDown(prompt,
                                                        &buffer,
                                                        &length,
                                                        &cursor,
                                                        &capacity,
                                                        &displayed_length,
                                                        &displayed_prompt_lines,
                                                        &history_index,
                                                        &scratch)) {
                        fputc('\a', stdout);
                        fflush(stdout);
                    }
                    continue;
                } else if (seq[1] == 'C') { /* Right arrow */
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    if (cursor < length) {
                        cursor++;
                        redrawInteractiveLine(prompt,
                                              buffer,
                                              length,
                                              cursor,
                                              &displayed_length,
                                              &displayed_prompt_lines);
                    } else {
                        fputc('\a', stdout);
                        fflush(stdout);
                    }
                    continue;
                } else if (seq[1] == 'D') { /* Left arrow */
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    if (cursor > 0) {
                        cursor--;
                        redrawInteractiveLine(prompt,
                                              buffer,
                                              length,
                                              cursor,
                                              &displayed_length,
                                              &displayed_prompt_lines);
                    } else {
                        fputc('\a', stdout);
                        fflush(stdout);
                    }
                    continue;
                } else if (seq[1] >= '0' && seq[1] <= '9') {
                    if (read(STDIN_FILENO, &seq[2], 1) <= 0) {
                        continue;
                    }
                    if (seq[1] == '3' && seq[2] == '~') { /* Delete */
                        alt_dot_active = false;
                        alt_dot_offset = 0;
                        if (cursor < length) {
                            memmove(buffer + cursor, buffer + cursor + 1, length - cursor);
                            length--;
                            buffer[length] = '\0';
                            redrawInteractiveLine(prompt,
                                                  buffer,
                                                  length,
                                                  cursor,
                                                  &displayed_length,
                                                  &displayed_prompt_lines);
                            history_index = 0;
                            interactiveUpdateScratch(&scratch, buffer, length);
                        } else {
                            fputc('\a', stdout);
                            fflush(stdout);
                        }
                        continue;
                    }
                }
            } else if (seq[0] == 'f' || seq[0] == 'F') { /* Alt+F */
                alt_dot_active = false;
                alt_dot_offset = 0;
                size_t next = interactiveNextWord(buffer, length, cursor);
                if (next != cursor) {
                    cursor = next;
                    redrawInteractiveLine(prompt,
                                          buffer,
                                          length,
                                          cursor,
                                          &displayed_length,
                                          &displayed_prompt_lines);
                } else {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
                continue;
            } else if (seq[0] == 'b' || seq[0] == 'B') { /* Alt+B */
                alt_dot_active = false;
                alt_dot_offset = 0;
                size_t prev = interactivePreviousWord(buffer, length, cursor);
                if (prev != cursor) {
                    cursor = prev;
                    redrawInteractiveLine(prompt,
                                          buffer,
                                          length,
                                          cursor,
                                          &displayed_length,
                                          &displayed_prompt_lines);
                } else {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
                continue;
            } else if (seq[0] == 'd' || seq[0] == 'D') { /* Alt+D */
                alt_dot_active = false;
                alt_dot_offset = 0;
                size_t next = interactiveNextWord(buffer, length, cursor);
                if (next > cursor) {
                    size_t removed_len = next - cursor;
                    if (!interactiveSetKillBuffer(&kill_buffer, buffer + cursor, removed_len)) {
                        fputc('\a', stdout);
                        fflush(stdout);
                        continue;
                    }
                    memmove(buffer + cursor, buffer + next, length - next + 1);
                    length -= removed_len;
                    redrawInteractiveLine(prompt,
                                          buffer,
                                          length,
                                          cursor,
                                          &displayed_length,
                                          &displayed_prompt_lines);
                    history_index = 0;
                    interactiveUpdateScratch(&scratch, buffer, length);
                } else {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
                continue;
            } else if (seq[0] == 't' || seq[0] == 'T') { /* Alt+T */
                alt_dot_active = false;
                alt_dot_offset = 0;
                if (length > 0 && cursor > 0) {
                    size_t current_start = cursor;
                    if (current_start > 0 && isspace((unsigned char)buffer[current_start])) {
                        while (current_start < length && isspace((unsigned char)buffer[current_start])) {
                            current_start++;
                        }
                        if (current_start >= length) {
                            fputc('\a', stdout);
                            fflush(stdout);
                            continue;
                        }
                    } else {
                        while (current_start > 0 && !isspace((unsigned char)buffer[current_start - 1])) {
                            current_start--;
                        }
                    }
                    size_t current_end = current_start;
                    while (current_end < length && !isspace((unsigned char)buffer[current_end])) {
                        current_end++;
                    }
                    size_t prev_end = current_start;
                    while (prev_end > 0 && isspace((unsigned char)buffer[prev_end - 1])) {
                        prev_end--;
                    }
                    if (prev_end == 0) {
                        fputc('\a', stdout);
                        fflush(stdout);
                        continue;
                    }
                    size_t prev_start = prev_end;
                    while (prev_start > 0 && !isspace((unsigned char)buffer[prev_start - 1])) {
                        prev_start--;
                    }
                    size_t word1_len = prev_end - prev_start;
                    size_t middle_len = current_start - prev_end;
                    size_t word2_len = current_end - current_start;
                    char *temp = (char *)malloc(length + 1);
                    if (!temp) {
                        fputc('\a', stdout);
                        fflush(stdout);
                        continue;
                    }
                    memcpy(temp, buffer, prev_start);
                    size_t offset = prev_start;
                    memcpy(temp + offset, buffer + current_start, word2_len);
                    offset += word2_len;
                    memcpy(temp + offset, buffer + prev_end, middle_len);
                    offset += middle_len;
                    memcpy(temp + offset, buffer + prev_start, word1_len);
                    offset += word1_len;
                    memcpy(temp + offset, buffer + current_end, length - current_end + 1);
                    memcpy(buffer, temp, length + 1);
                    free(temp);
                    cursor = prev_start + word2_len + middle_len + word1_len;
                    redrawInteractiveLine(prompt,
                                          buffer,
                                          length,
                                          cursor,
                                          &displayed_length,
                                          &displayed_prompt_lines);
                    history_index = 0;
                    interactiveUpdateScratch(&scratch, buffer, length);
                } else {
                    fputc('\a', stdout);
                    fflush(stdout);
                }
                continue;
            } else if (seq[0] == '.') { /* Alt+. */
                if (!alt_dot_active) {
                    alt_dot_offset = 0;
                } else {
                    alt_dot_offset++;
                }
                char *argument = NULL;
                if (!interactiveExtractLastArgument(alt_dot_offset, &argument)) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    continue;
                }
                if (!argument) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    continue;
                }
                size_t arg_len = strlen(argument);
                if (!interactiveInsertText(&buffer,
                                           &length,
                                           &capacity,
                                           &cursor,
                                           argument,
                                           arg_len)) {
                    fputc('\a', stdout);
                    fflush(stdout);
                    free(argument);
                    alt_dot_active = false;
                    alt_dot_offset = 0;
                    continue;
                }
                free(argument);
                redrawInteractiveLine(prompt,
                                      buffer,
                                      length,
                                      cursor,
                                      &displayed_length,
                                      &displayed_prompt_lines);
                history_index = 0;
                interactiveUpdateScratch(&scratch, buffer, length);
                alt_dot_active = true;
                continue;
            }
            alt_dot_active = false;
            alt_dot_offset = 0;
            continue;
        }

        if (ch == '\t') { /* Tab completion */
            alt_dot_active = false;
            alt_dot_offset = 0;
            if (!interactiveHandleTabCompletion(prompt,
                                                &buffer,
                                                &length,
                                                &cursor,
                                                &capacity,
                                                &displayed_length,
                                                &displayed_prompt_lines,
                                                &scratch)) {
                fputc('\a', stdout);
                fflush(stdout);
            } else {
                history_index = 0;
            }
            continue;
        }

        if (!isprint(ch)) {
            alt_dot_active = false;
            alt_dot_offset = 0;
            fputc('\a', stdout);
            fflush(stdout);
            continue;
        }

        alt_dot_active = false;
        alt_dot_offset = 0;

        if (length + 1 >= capacity) {
            size_t new_capacity = capacity * 2;
            if (new_capacity <= length + 1) {
                new_capacity = length + 2;
            }
            char *new_buffer = (char *)realloc(buffer, new_capacity);
            if (!new_buffer) {
                fputc('\a', stdout);
                fflush(stdout);
                continue;
            }
            buffer = new_buffer;
            capacity = new_capacity;
        }

        if (cursor < length) {
            memmove(buffer + cursor + 1, buffer + cursor, length - cursor);
        }
        buffer[cursor] = (char)ch;
        cursor++;
        length++;
        buffer[length] = '\0';
        redrawInteractiveLine(prompt,
                              buffer,
                              length,
                              cursor,
                              &displayed_length,
                              &displayed_prompt_lines);
        history_index = 0;
        interactiveUpdateScratch(&scratch, buffer, length);
    }

    if (installed_sigint_handler) {
        interactiveRestoreSigintHandler();
    }
    interactiveRestoreSigtstpHandler();
    interactiveRestoreTerminal();
    free(scratch);
    free(kill_buffer);

    if (eof_requested && length == 0) {
        free(buffer);
        if (out_eof) {
            *out_eof = true;
        }
        return NULL;
    }

    if (!done) {
        free(buffer);
        if (out_editor_failed) {
            *out_editor_failed = true;
        }
        return NULL;
    }

    char *result = (char *)realloc(buffer, length + 1);
    if (!result) {
        result = buffer;
    }
    result[length] = '\0';
    return result;
}

static int runInteractiveSession(const ShellRunOptions *options) {
    ShellRunOptions exec_opts = *options;
    exec_opts.no_cache = 1;
    exec_opts.quiet = options->quiet;
    exec_opts.exit_on_signal = false;

    int last_status = shellRuntimeLastStatus();
    bool tty = pscalRuntimeStdinIsInteractive();
    const char *force_no_tty = getenv("PSCAL_FORCE_NO_TTY");
    if (force_no_tty && *force_no_tty && force_no_tty[0] != '0') {
        tty = false;
    }

    while (true) {
        pscalRuntimeConsumeSigint();
        pscalRuntimeClearInterruptFlag();
        shellRuntimeEnsureStandardFds();
        char *prompt_storage = shellResolveInteractivePrompt();
        const char *prompt = prompt_storage ? prompt_storage : "exsh$ ";
        char *line = NULL;
        bool interactive_eof = false;
        bool editor_failed = false;
        if (tty) {
            line = readInteractiveLine(prompt, &interactive_eof, &editor_failed);
            if (!line && editor_failed) {
                tty = false;
            }
            if (!line && interactive_eof) {
                fputc('\n', stdout);
                free(prompt_storage);
                break;
            }
        }
        size_t line_capacity = 0;
        ssize_t read = 0;
        if (!line) {
            if (pscalRuntimeStdinIsInteractive()) {
                fputs(prompt, stdout);
                fflush(stdout);
            }
            read = getline(&line, &line_capacity, stdin);
            if (read < 0) {
                int saved_errno = errno;
                shellRuntimeEnsureStandardFds();
                if (saved_errno == EBADF || saved_errno == EIO) {
                    free(prompt_storage);
                    free(line);
                    continue;
                }
                free(line);
                if (pscalRuntimeStdinIsInteractive()) {
                    fputc('\n', stdout);
                }
                free(prompt_storage);
                break;
            }
        } else {
            read = (ssize_t)strlen(line);
        }
        free(prompt_storage);
        bool only_whitespace = true;
        for (ssize_t i = 0; i < read; ++i) {
            if (line[i] != ' ' && line[i] != '\t' && line[i] != '\n' && line[i] != '\r') {
                only_whitespace = false;
                break;
            }
        }
        if (only_whitespace) {
            free(line);
            continue;
        }

        char *expanded_line = NULL;
        bool used_history = false;
        char *history_error = NULL;
        if (!shellRuntimeExpandHistoryReference(line, &expanded_line, &used_history, &history_error)) {
            if (history_error) {
                fprintf(stderr, "exsh: %s: event not found\n", history_error);
                free(history_error);
            } else {
                fprintf(stderr, "exsh: history expansion failed\n");
            }
            free(line);
            continue;
        }
        if (used_history && pscalRuntimeStdinIsInteractive()) {
            printf("%s\n", expanded_line);
            fflush(stdout);
        }
        if (history_error) {
            free(history_error);
        }
        free(line);
        line = expanded_line;

        char *rewritten_line = interactiveRewriteCombinedRedirects(line);
        if (!rewritten_line) {
            fprintf(stderr, "exsh: failed to rewrite redirects\n");
            free(line);
            continue;
        }
        free(line);
        line = rewritten_line;

        char *expanded_tilde = interactiveExpandTilde(line);
        if (!expanded_tilde) {
            fprintf(stderr, "exsh: failed to expand home directory\n");
            free(line);
            continue;
        }
        shellRuntimeRecordHistory(line);
        bool exit_requested = false;
        last_status = shellRunSource(expanded_tilde, "<stdin>", &exec_opts, &exit_requested);
        free(expanded_tilde);
        free(line);
        if (exit_requested) {
            break;
        }
    }

    return last_status;
}

int exsh_main(int argc, char **argv) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_SHELL);
#if defined(PSCAL_TARGET_IOS)
#define EXSH_RETURN(value)                             \
    do {                                               \
        int __exsh_rc = (value);                       \
        shellTeardownSelfVproc(__exsh_rc);             \
        frontendPopKind(previousKind);                 \
        return __exsh_rc;                              \
    } while (0)
#else
#define EXSH_RETURN(value)                             \
    do {                                               \
        int __exsh_rc = (value);                       \
        frontendPopKind(previousKind);                 \
        return __exsh_rc;                              \
    } while (0)
#endif
    ShellRunOptions options = {0};
    options.frontend_path = (argc > 0) ? argv[0] : "exsh";
    options.quiet = true;
    options.suppress_warnings = true;

    registerShellFrontendBuiltins();
    vmSetSuppressStateDump(true);

#if defined(PSCAL_TARGET_IOS)
void pscalRuntimeDebugLog(const char *message);
    if (getenv("PSCALI_PIPE_DEBUG")) {
        char logbuf[1024];
        int written = snprintf(logbuf, sizeof(logbuf), "[exsh-ios] argc=%d", argc);
        for (int i = 0; i < argc && written < (int)sizeof(logbuf) - 8; ++i) {
            int n = snprintf(logbuf + written, sizeof(logbuf) - (size_t)written,
                             " argv[%d]='%s'", i, argv[i] ? argv[i] : "(null)");
            if (n <= 0) {
                break;
            }
            written += n;
        }
        pscalRuntimeDebugLog(logbuf);
        /* Also emit to stderr so interactive runs surface argv immediately. */
        fprintf(stderr, "%s\n", logbuf);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    shellSetupSelfVproc();
#endif

    shellRuntimeSetArg0(options.frontend_path);
    shellRuntimeSetInteractive(false);

    int dump_ext_builtins_flag = 0;
    const char *path = NULL;
    int arg_start_index = 0;
    const char *command_string = NULL;
    const char *command_arg0 = NULL;
    int command_param_start = -1;

    /* Self-hosted jobspec test helper for environments (iOS) where the test
     * script file is not present on disk. Invoked as `exsh testjobs`. */
    static const char *kJobspecSelfTest =
        "set -e\n"
        "set -m\n"
        "echo --DB1--\n"
        "if [[ foo == foo ]]; then echo OK; else echo BAD; exit 1; fi\n"
        "echo --J1--\n"
        "sleep 60 &\n"
        "sleep 60 &\n"
        "jobs\n"
        "echo --K1--\n"
        "kill %1\n"
        "sleep 1\n"
        "echo --J2--\n"
        "jobs\n"
        "echo --K2--\n"
        "kill %2 || true\n"
        "sleep 1\n"
        "echo --J3--\n"
        "jobs\n"
        "echo --M1--\n"
        "sleep 60 &\n"
        "sleep 60 &\n"
        "sleep 60 &\n"
        "echo --J4--\n"
        "jobs\n"
        "echo --Kmid--\n"
        "kill %2\n"
        "sleep 1\n"
        "echo --J5--\n"
        "jobs\n"
        "echo --Kall--\n"
        "kill %1 || true\n"
        "kill %3 || true\n";

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("%s", SHELL_USAGE);
            EXSH_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
        } else if (strcmp(argv[i], "-v") == 0) {
            printf("Shell Frontend Version: %s (latest tag: %s)\n",
                   pscal_program_version_string(), pscal_git_tag_string());
            EXSH_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            options.dump_ast_json = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            options.dump_bytecode = 1;
        } else if (strcmp(argv[i], "--dump-bytecode-only") == 0) {
            options.dump_bytecode = 1;
            options.dump_bytecode_only = 1;
        } else if (strcmp(argv[i], "--dump-ext-builtins") == 0) {
            dump_ext_builtins_flag = 1;
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            options.no_cache = 1;
        } else if (strcmp(argv[i], "--semantic-warnings") == 0) {
            options.suppress_warnings = false;
        } else if (strncmp(argv[i], "--vm-trace-head=", 16) == 0) {
            options.vm_trace_head = atoi(argv[i] + 16);
        } else if (strcmp(argv[i], "--verbose") == 0) {
            options.quiet = false;
        } else if (strcmp(argv[i], "-d") == 0) {
            options.verbose_errors = true;
        } else if (strcmp(argv[i], "-c") == 0) {
            if (i + 1 >= argc) {
                const char *program = options.frontend_path ? options.frontend_path : "exsh";
                const char *slash = strrchr(program, '/');
                const char *backslash = strrchr(program, '\\');
                if (backslash && (!slash || backslash > slash)) {
                    slash = backslash;
                }
                const char *program_name = (slash && slash[1]) ? slash + 1 : program;
                fprintf(stderr, "%s: -c: option requires an argument\n", program_name);
                EXSH_RETURN(vmExitWithCleanup(2));
            }
            command_string = argv[i + 1];
            if (i + 2 < argc) {
                command_arg0 = argv[i + 2];
                command_param_start = i + 3;
            } else {
                command_param_start = i + 2;
            }
            break;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s\n", argv[i], SHELL_USAGE);
            EXSH_RETURN(EXIT_FAILURE);
        } else {
            if (strcmp(argv[i], "testjobs") == 0) {
                command_string = kJobspecSelfTest;
                command_param_start = argc;
                command_arg0 = argv[0];
                break;
            }
            path = argv[i];
            arg_start_index = i + 1;
            break;
        }
    }

    if (dump_ext_builtins_flag) {
        shellDumpBuiltins(stdout);
        EXSH_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }

    setenv("EXSH_LAST_STATUS", "0", 1);
    shellRuntimeInitSignals();

    if (path) {
        shellRuntimeSetInteractive(false);
        char *src = shellLoadFile(path);
        if (!src) {
            EXSH_RETURN(EXIT_FAILURE);
        }
        if (arg_start_index < argc) {
            gParamCount = argc - arg_start_index;
            gParamValues = &argv[arg_start_index];
        }
        shellRuntimeSetArg0(path);
        bool exit_requested = false;
        ShellRunOptions script_options = options;
        script_options.exit_on_signal = true;
        int status = shellRunSource(src, path, &script_options, &exit_requested);
        (void)exit_requested;
        free(src);
        shellRuntimeSetArg0(options.frontend_path);
        EXSH_RETURN(vmExitWithCleanup(status));
    }

    if (command_string) {
        if (command_arg0) {
            shellRuntimeSetArg0(command_arg0);
        }
        if (command_param_start < 0 || command_param_start > argc) {
            command_param_start = argc;
        }
        if (command_param_start < argc) {
            gParamCount = argc - command_param_start;
            gParamValues = &argv[command_param_start];
        } else {
            gParamCount = 0;
            gParamValues = NULL;
        }
        shellRuntimeSetInteractive(false);
        ShellRunOptions command_options = options;
        command_options.no_cache = 1;
        command_options.exit_on_signal = true;
        bool exit_requested = false;
        int status = shellRunSource(command_string, "<command>", &command_options, &exit_requested);
        (void)exit_requested;
        shellRuntimeSetArg0(options.frontend_path);
        EXSH_RETURN(vmExitWithCleanup(status));
    }

    gParamCount = 0;
    gParamValues = NULL;

    if (pscalRuntimeStdinIsInteractive()) {
        shellRuntimeSetInteractive(true);
        shellRuntimeInitJobControl();
        int rc_status = EXIT_SUCCESS;
        if (shellRunStartupConfig(&options, &rc_status)) {
            EXSH_RETURN(vmExitWithCleanup(rc_status));
        }
        int status = runInteractiveSession(&options);
        EXSH_RETURN(vmExitWithCleanup(status));
    }

    shellRuntimeSetInteractive(false);
    char *stdin_src = readStream(stdin);
    if (!stdin_src) {
        EXSH_RETURN(EXIT_FAILURE);
    }

    ShellRunOptions stdin_opts = options;
    stdin_opts.no_cache = 1;
    stdin_opts.exit_on_signal = true;
    bool exit_requested = false;
    int status = shellRunSource(stdin_src, "<stdin>", &stdin_opts, &exit_requested);
    (void)exit_requested;
    free(stdin_src);
    EXSH_RETURN(vmExitWithCleanup(status));
}
#undef EXSH_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char **argv) {
    return exsh_main(argc, argv);
}
#elif defined(PSCAL_TARGET_IOS) && defined(SDL)
#include "SDL_main.h"
int SDL_main(int argc, char **argv) {
    return exsh_main(argc, argv);
}
#endif
