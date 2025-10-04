#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "shell/word_encoding.h"
#include "shell/quote_markers.h"
#include "shell/function.h"
#include "shell/runner.h"
#include "vm/vm.h"
#include "Pascal/globals.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <glob.h>
#include <limits.h>
#include <regex.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

typedef enum {
    SHELL_RUNTIME_REDIR_OPEN,
    SHELL_RUNTIME_REDIR_DUP,
    SHELL_RUNTIME_REDIR_HEREDOC
} ShellRuntimeRedirectionKind;

typedef struct {
    int fd;
    ShellRuntimeRedirectionKind kind;
    int flags;
    mode_t mode;
    char *path;
    int dup_target_fd;
    bool close_target;
    char *here_doc;
    size_t here_doc_length;
} ShellRedirection;

typedef struct {
    char **argv;
    size_t argc;
    char **assignments;
    size_t assignment_count;
    ShellRedirection *redirs;
    size_t redir_count;
    bool background;
    int pipeline_index;
    bool is_pipeline_head;
    bool is_pipeline_tail;
} ShellCommand;

typedef struct {
    bool active;
    size_t stage_count;
    bool negated;
    bool *merge_stderr;
    pid_t *pids;
    int (*pipes)[2];
    size_t launched;
    bool background;
    int last_status;
    pid_t pgid;
} ShellPipelineContext;

typedef struct {
    int last_status;
    ShellPipelineContext pipeline;
    pid_t shell_pgid;
    int tty_fd;
    bool job_control_enabled;
    bool job_control_initialized;
    bool errexit_enabled;
    bool errexit_pending;
    bool trap_enabled;
    bool local_scope_active;
    bool break_requested;
    bool continue_requested;
    int break_requested_levels;
    int continue_requested_levels;
} ShellRuntimeState;

typedef struct {
    bool skip_body;
    bool break_pending;
    bool continue_pending;
} ShellLoopFrame;

static ShellLoopFrame *gShellLoopStack = NULL;
static size_t gShellLoopStackSize = 0;
static size_t gShellLoopStackCapacity = 0;

typedef struct {
    char *name;
    char *previous_value;
    bool had_previous;
} ShellAssignmentBackup;

static ShellRuntimeState gShellRuntime = {
    .last_status = 0,
    .pipeline = {0},
    .shell_pgid = 0,
    .tty_fd = -1,
    .job_control_enabled = false,
    .job_control_initialized = false,
    .errexit_enabled = false,
    .errexit_pending = false,
    .trap_enabled = false,
    .local_scope_active = false,
    .break_requested = false,
    .continue_requested = false,
    .break_requested_levels = 0,
    .continue_requested_levels = 0
};

static bool gShellExitRequested = false;
static bool gShellArithmeticErrorPending = false;
static VM *gShellCurrentVm = NULL;
static volatile sig_atomic_t gShellPendingSignals[NSIG] = {0};

static bool shellLoopEnsureCapacity(size_t needed) {
    if (gShellLoopStackCapacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellLoopStackCapacity ? gShellLoopStackCapacity * 2 : 4;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }
    ShellLoopFrame *resized = (ShellLoopFrame *)realloc(gShellLoopStack, new_capacity * sizeof(ShellLoopFrame));
    if (!resized) {
        return false;
    }
    gShellLoopStack = resized;
    gShellLoopStackCapacity = new_capacity;
    return true;
}

static ShellLoopFrame *shellLoopPushFrame(void) {
    if (!shellLoopEnsureCapacity(gShellLoopStackSize + 1)) {
        return NULL;
    }
    ShellLoopFrame frame = {false, false, false};
    gShellLoopStack[gShellLoopStackSize++] = frame;
    return &gShellLoopStack[gShellLoopStackSize - 1];
}

static ShellLoopFrame *shellLoopTop(void) {
    if (gShellLoopStackSize == 0) {
        return NULL;
    }
    return &gShellLoopStack[gShellLoopStackSize - 1];
}

static void shellLoopPopFrame(void) {
    if (gShellLoopStackSize == 0) {
        return;
    }
    gShellLoopStackSize--;
    if (gShellLoopStackSize == 0) {
        gShellRuntime.break_requested = false;
        gShellRuntime.continue_requested = false;
        gShellRuntime.break_requested_levels = 0;
        gShellRuntime.continue_requested_levels = 0;
    }
}

static void shellSignalHandler(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }
    gShellPendingSignals[signo] = 1;
}

static bool shellLoopSkipActive(void) {
    for (size_t i = gShellLoopStackSize; i > 0; --i) {
        if (gShellLoopStack[i - 1].skip_body) {
            return true;
        }
    }
    return false;
}

static void shellLoopRequestBreakLevels(int levels) {
    if (levels <= 0) {
        levels = 1;
    }
    size_t idx = gShellLoopStackSize;
    while (idx > 0 && levels > 0) {
        ShellLoopFrame *frame = &gShellLoopStack[idx - 1];
        frame->skip_body = true;
        frame->break_pending = true;
        frame->continue_pending = false;
        idx--;
        levels--;
    }
    if (levels > 0) {
        gShellExitRequested = true;
    }
}

static void shellLoopRequestContinueLevels(int levels) {
    if (levels <= 0) {
        levels = 1;
    }
    if (gShellLoopStackSize == 0) {
        return;
    }
    size_t idx = gShellLoopStackSize;
    int remaining = levels;
    while (idx > 0 && remaining > 1) {
        ShellLoopFrame *frame = &gShellLoopStack[idx - 1];
        frame->skip_body = true;
        frame->break_pending = true;
        frame->continue_pending = false;
        idx--;
        remaining--;
    }
    if (idx > 0) {
        ShellLoopFrame *target = &gShellLoopStack[idx - 1];
        target->skip_body = true;
        target->continue_pending = true;
        target->break_pending = false;
    }
}

static VM *shellSwapCurrentVm(VM *vm) {
    VM *previous = gShellCurrentVm;
    gShellCurrentVm = vm;
    return previous;
}

static void shellRestoreCurrentVm(VM *vm) {
    gShellCurrentVm = vm;
}

static void shellInitJobControlState(void) {
    if (gShellRuntime.job_control_initialized) {
        return;
    }

    gShellRuntime.job_control_initialized = true;
    gShellRuntime.tty_fd = STDIN_FILENO;

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.tty_fd = -1;
        return;
    }

    if (!isatty(gShellRuntime.tty_fd)) {
        gShellRuntime.tty_fd = -1;
        return;
    }

    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    sigemptyset(&ignore_action.sa_mask);
    ignore_action.sa_handler = SIG_IGN;
    (void)sigaction(SIGTTIN, &ignore_action, NULL);
    (void)sigaction(SIGTTOU, &ignore_action, NULL);

    pid_t shell_pid = getpid();
    pid_t current_pgid = getpgrp();
    if (current_pgid != shell_pid) {
        if (setpgid(0, 0) == 0) {
            current_pgid = shell_pid;
        } else {
            current_pgid = getpgrp();
        }
    }

    gShellRuntime.shell_pgid = current_pgid;
}

static void shellEnsureJobControl(void) {
    shellInitJobControlState();

    if (gShellRuntime.tty_fd < 0) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    if (!isatty(gShellRuntime.tty_fd)) {
        gShellRuntime.job_control_enabled = false;
        gShellRuntime.tty_fd = -1;
        return;
    }

    pid_t pgid = getpgrp();
    if (pgid <= 0) {
        gShellRuntime.job_control_enabled = false;
        return;
    }

    gShellRuntime.shell_pgid = pgid;

    while (true) {
        pid_t foreground = tcgetpgrp(gShellRuntime.tty_fd);
        if (foreground < 0) {
            if (errno == EINTR) {
                continue;
            }
            gShellRuntime.job_control_enabled = false;
            return;
        }
        if (foreground == pgid) {
            gShellRuntime.job_control_enabled = true;
            return;
        }
        if (tcsetpgrp(gShellRuntime.tty_fd, pgid) != 0) {
            if (errno == EINTR) {
                continue;
            }
            gShellRuntime.job_control_enabled = false;
            return;
        }
    }
}

static void shellJobControlSetForeground(pid_t pgid) {
    if (!gShellRuntime.job_control_enabled || gShellRuntime.tty_fd < 0 || pgid <= 0) {
        return;
    }
    while (tcsetpgrp(gShellRuntime.tty_fd, pgid) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

static void shellJobControlRestoreForeground(void) {
    if (!gShellRuntime.job_control_enabled || gShellRuntime.tty_fd < 0) {
        return;
    }
    pid_t target = gShellRuntime.shell_pgid > 0 ? gShellRuntime.shell_pgid : getpgrp();
    if (target <= 0) {
        return;
    }
    while (tcsetpgrp(gShellRuntime.tty_fd, target) != 0) {
        if (errno == EINTR) {
            continue;
        }
        break;
    }
}

typedef struct {
    char *subject;
    bool matched;
} ShellCaseContext;

typedef struct {
    ShellCaseContext *items;
    size_t count;
    size_t capacity;
} ShellCaseContextStack;

static ShellCaseContextStack gShellCaseStack = {NULL, 0, 0};

static bool shellCaseStackPush(const char *subject_text) {
    if (!subject_text) {
        subject_text = "";
    }
    char *copy = strdup(subject_text);
    if (!copy) {
        return false;
    }
    if (gShellCaseStack.count + 1 > gShellCaseStack.capacity) {
        size_t new_capacity = gShellCaseStack.capacity ? gShellCaseStack.capacity * 2 : 4;
        ShellCaseContext *items = realloc(gShellCaseStack.items, new_capacity * sizeof(ShellCaseContext));
        if (!items) {
            free(copy);
            return false;
        }
        gShellCaseStack.items = items;
        gShellCaseStack.capacity = new_capacity;
    }
    ShellCaseContext *ctx = &gShellCaseStack.items[gShellCaseStack.count++];
    ctx->subject = copy;
    ctx->matched = false;
    return true;
}

static ShellCaseContext *shellCaseStackTop(void) {
    if (gShellCaseStack.count == 0) {
        return NULL;
    }
    return &gShellCaseStack.items[gShellCaseStack.count - 1];
}

static void shellCaseStackPop(void) {
    if (gShellCaseStack.count == 0) {
        return;
    }
    ShellCaseContext *ctx = &gShellCaseStack.items[gShellCaseStack.count - 1];
    free(ctx->subject);
    ctx->subject = NULL;
    gShellCaseStack.count--;
    if (gShellCaseStack.count == 0 && gShellCaseStack.items) {
        free(gShellCaseStack.items);
        gShellCaseStack.items = NULL;
        gShellCaseStack.capacity = 0;
    }
}

typedef struct {
    pid_t pgid;
    pid_t *pids;
    size_t pid_count;
    bool running;
    bool stopped;
    int last_status;
    char *command;
} ShellJob;

static ShellJob *gShellJobs = NULL;
static size_t gShellJobCount = 0;

typedef struct {
    char **entries;
    size_t count;
    size_t capacity;
} ShellHistory;

static ShellHistory gShellHistory = {NULL, 0, 0};
static char *gShellArg0 = NULL;

typedef struct {
    char *name;
    char *parameter_metadata;
    ShellCompiledFunction *compiled;
} ShellFunctionEntry;

static ShellFunctionEntry *gShellFunctions = NULL;
static size_t gShellFunctionCount = 0;

typedef enum {
    SHELL_META_SUBSTITUTION_DOLLAR,
    SHELL_META_SUBSTITUTION_BACKTICK
} ShellMetaSubstitutionStyle;

typedef struct {
    ShellMetaSubstitutionStyle style;
    size_t span_length;
    char *command;
} ShellMetaSubstitution;

static bool shellBufferEnsure(char **buffer, size_t *length, size_t *capacity, size_t extra);
static bool shellCommandAppendArgOwned(ShellCommand *cmd, char *value);
static bool shellCommandAppendAssignmentOwned(ShellCommand *cmd, char *value);
static bool shellLooksLikeAssignment(const char *text);
static bool shellParseAssignment(const char *assignment, char **out_name, const char **out_value);
static bool shellApplyAssignmentsPermanently(const ShellCommand *cmd,
                                             const char **out_failed_assignment,
                                             bool *out_invalid_assignment);
static bool shellApplyAssignmentsTemporary(const ShellCommand *cmd,
                                           ShellAssignmentBackup **out_backups,
                                           size_t *out_count,
                                           const char **out_failed_assignment,
                                           bool *out_invalid_assignment);
static void shellRestoreAssignments(ShellAssignmentBackup *backups, size_t count);
static int shellSpawnProcess(const ShellCommand *cmd,
                             int stdin_fd,
                             int stdout_fd,
                             int stderr_fd,
                             pid_t *child_pid,
                             bool ignore_job_signals);
static int shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped);
static void shellFreeCommand(ShellCommand *cmd);
static void shellUpdateStatus(int status);

static bool shellDecodeWordSpec(const char *encoded, const char **out_text, uint8_t *out_flags,
                                const char **out_meta, size_t *out_meta_len) {
    if (out_text) {
        *out_text = encoded ? encoded : "";
    }
    if (out_flags) {
        *out_flags = 0;
    }
    if (out_meta) {
        *out_meta = NULL;
    }
    if (out_meta_len) {
        *out_meta_len = 0;
    }
    if (!encoded) {
        return false;
    }
    size_t len = strlen(encoded);
    if (len < 8 || encoded[0] != SHELL_WORD_ENCODE_PREFIX) {
        return false;
    }
    if (out_flags) {
        uint8_t stored = (uint8_t)encoded[1];
        *out_flags = stored > 0 ? (stored - 1) : 0;
    }
    char meta_len_buf[7];
    memcpy(meta_len_buf, encoded + 2, 6);
    meta_len_buf[6] = '\0';
    size_t meta_len = strtoul(meta_len_buf, NULL, 16);
    if (8 + meta_len > len) {
        return false;
    }
    if (out_meta) {
        *out_meta = encoded + 8;
    }
    if (out_meta_len) {
        *out_meta_len = meta_len;
    }
    if (out_text) {
        *out_text = encoded + 8 + meta_len;
    }
    return true;
}

static void shellFreeMetaSubstitutions(ShellMetaSubstitution *subs, size_t count) {
    if (!subs) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(subs[i].command);
    }
    free(subs);
}

static bool shellParseCommandMetadata(const char *meta, size_t meta_len,
                                      ShellMetaSubstitution **out_subs, size_t *out_count) {
    if (out_subs) {
        *out_subs = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (!meta || meta_len == 0) {
        return true;
    }
    if (meta_len < 4) {
        return false;
    }
    char count_buf[5];
    memcpy(count_buf, meta, 4);
    count_buf[4] = '\0';
    size_t count = strtoul(count_buf, NULL, 16);
    if (count == 0) {
        return true;
    }
    ShellMetaSubstitution *subs = (ShellMetaSubstitution *)calloc(count, sizeof(ShellMetaSubstitution));
    if (!subs) {
        return false;
    }
    size_t offset = 4;
    for (size_t i = 0; i < count; ++i) {
        if (offset + 1 + 6 + 6 > meta_len) {
            shellFreeMetaSubstitutions(subs, count);
            return false;
        }
        char style_char = meta[offset++];
        ShellMetaSubstitutionStyle style = (style_char == 'B') ?
            SHELL_META_SUBSTITUTION_BACKTICK : SHELL_META_SUBSTITUTION_DOLLAR;

        char span_buf[7];
        memcpy(span_buf, meta + offset, 6);
        span_buf[6] = '\0';
        size_t span = strtoul(span_buf, NULL, 16);
        offset += 6;

        char len_buf[7];
        memcpy(len_buf, meta + offset, 6);
        len_buf[6] = '\0';
        size_t cmd_len = strtoul(len_buf, NULL, 16);
        offset += 6;
        if (offset + cmd_len > meta_len) {
            shellFreeMetaSubstitutions(subs, count);
            return false;
        }
        char *command = (char *)malloc(cmd_len + 1);
        if (!command && cmd_len > 0) {
            shellFreeMetaSubstitutions(subs, count);
            return false;
        }
        if (cmd_len > 0) {
            memcpy(command, meta + offset, cmd_len);
        }
        if (command) {
            command[cmd_len] = '\0';
        } else {
            command = strdup("");
            if (!command) {
                shellFreeMetaSubstitutions(subs, count);
                return false;
            }
        }
        offset += cmd_len;
        subs[i].style = style;
        subs[i].span_length = span;
        subs[i].command = command;
    }
    if (out_subs) {
        *out_subs = subs;
    } else {
        shellFreeMetaSubstitutions(subs, count);
    }
    if (out_count) {
        *out_count = count;
    }
    return true;
}

static char *shellRunCommandSubstitution(const char *command) {
    int pipes[2] = {-1, -1};
    ShellCommand cmd;
    memset(&cmd, 0, sizeof(cmd));

    if (pipe(pipes) != 0) {
        return strdup("");
    }
    const char *shell_path = "/bin/sh";
    if (!shellCommandAppendArgOwned(&cmd, strdup(shell_path)) ||
        !shellCommandAppendArgOwned(&cmd, strdup("-c")) ||
        !shellCommandAppendArgOwned(&cmd, strdup(command ? command : ""))) {
        goto cleanup;
    }

    pid_t child = -1;
    int spawn_err = shellSpawnProcess(&cmd, -1, pipes[1], -1, &child, false);
    close(pipes[1]);
    pipes[1] = -1;
    if (spawn_err != 0) {
        goto cleanup;
    }

    char *output = NULL;
    size_t length = 0;
    size_t capacity = 0;
    char buffer[256];
    while (true) {
        ssize_t n = read(pipes[0], buffer, sizeof(buffer));
        if (n > 0) {
            if (!shellBufferEnsure(&output, &length, &capacity, (size_t)n)) {
                free(output);
                output = NULL;
                break;
            }
            memcpy(output + length, buffer, (size_t)n);
            length += (size_t)n;
            output[length] = '\0';
        } else if (n == 0) {
            break;
        } else if (errno == EINTR) {
            continue;
        } else {
            free(output);
            output = NULL;
            break;
        }
    }
    close(pipes[0]);
    pipes[0] = -1;

    int status = 0;
    shellWaitPid(child, &status, false, NULL);
    shellRuntimeProcessPendingSignals();
    shellFreeCommand(&cmd);

    if (!output) {
        return strdup("");
    }
    while (length > 0 && (output[length - 1] == '\n' || output[length - 1] == '\r')) {
        output[--length] = '\0';
    }
    return output;

cleanup:
    if (pipes[0] >= 0) {
        close(pipes[0]);
    }
    if (pipes[1] >= 0) {
        close(pipes[1]);
    }
    shellFreeCommand(&cmd);
    return strdup("");
}

static bool shellBufferEnsure(char **buffer, size_t *length, size_t *capacity, size_t extra) {
    if (!buffer || !length || !capacity) {
        return false;
    }
    size_t needed = *length + extra + 1; // +1 for null terminator
    if (needed <= *capacity) {
        return true;
    }
    size_t new_capacity = *capacity ? *capacity : 32;
    while (needed > new_capacity) {
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

static void shellDisposeCompiledFunction(ShellCompiledFunction *fn) {
    if (!fn) {
        return;
    }
    freeBytecodeChunk(&fn->chunk);
    free(fn);
}

static ShellFunctionEntry *shellFindFunctionEntry(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellFunctionCount; ++i) {
        ShellFunctionEntry *entry = &gShellFunctions[i];
        if (entry->name && strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

static bool shellStoreFunction(const char *name, const char *param_meta, ShellCompiledFunction *compiled) {
    if (!name || !compiled) {
        return false;
    }
    char *name_copy = strdup(name);
    char *meta_copy = NULL;
    if (!name_copy) {
        return false;
    }
    if (param_meta && *param_meta) {
        meta_copy = strdup(param_meta);
        if (!meta_copy) {
            free(name_copy);
            return false;
        }
    }
    ShellFunctionEntry *existing = shellFindFunctionEntry(name);
    if (existing) {
        if (existing->compiled && existing->compiled != compiled) {
            shellDisposeCompiledFunction(existing->compiled);
        }
        free(existing->name);
        free(existing->parameter_metadata);
        existing->name = name_copy;
        existing->parameter_metadata = meta_copy;
        existing->compiled = compiled;
        return true;
    }
    ShellFunctionEntry *entries = realloc(gShellFunctions, sizeof(ShellFunctionEntry) * (gShellFunctionCount + 1));
    if (!entries) {
        free(name_copy);
        free(meta_copy);
        return false;
    }
    gShellFunctions = entries;
    ShellFunctionEntry *entry = &gShellFunctions[gShellFunctionCount++];
    entry->name = name_copy;
    entry->parameter_metadata = meta_copy;
    entry->compiled = compiled;
    return true;
}

static bool shellCommandAppendArgOwned(ShellCommand *cmd, char *value) {
    if (!cmd || !value) {
        free(value);
        return false;
    }
    char **new_argv = realloc(cmd->argv, sizeof(char*) * (cmd->argc + 2));
    if (!new_argv) {
        free(value);
        return false;
    }
    cmd->argv = new_argv;
    cmd->argv[cmd->argc] = value;
    cmd->argc++;
    cmd->argv[cmd->argc] = NULL;
    return true;
}

static bool shellCommandAppendAssignmentOwned(ShellCommand *cmd, char *value) {
    if (!cmd || !value) {
        free(value);
        return false;
    }
    char **new_assignments = realloc(cmd->assignments, sizeof(char *) * (cmd->assignment_count + 1));
    if (!new_assignments) {
        free(value);
        return false;
    }
    cmd->assignments = new_assignments;
    cmd->assignments[cmd->assignment_count++] = value;
    return true;
}

static bool shellLooksLikeAssignment(const char *text) {
    if (!text) {
        return false;
    }
    const char *eq = strchr(text, '=');
    if (!eq || eq == text) {
        return false;
    }
    for (const char *cursor = text; cursor < eq; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (cursor == text) {
            if (!isalpha(ch) && ch != '_') {
                return false;
            }
        } else if (!isalnum(ch) && ch != '_') {
            return false;
        }
    }
    return true;
}

static bool shellParseAssignment(const char *assignment, char **out_name, const char **out_value) {
    if (out_name) {
        *out_name = NULL;
    }
    if (out_value) {
        *out_value = NULL;
    }
    if (!assignment) {
        return false;
    }
    const char *eq = strchr(assignment, '=');
    if (!eq || eq == assignment) {
        return false;
    }
    size_t name_len = (size_t)(eq - assignment);
    for (size_t i = 0; i < name_len; ++i) {
        unsigned char ch = (unsigned char)assignment[i];
        if (i == 0) {
            if (!isalpha(ch) && ch != '_') {
                return false;
            }
        } else if (!isalnum(ch) && ch != '_') {
            return false;
        }
    }
    char *name = (char *)malloc(name_len + 1);
    if (!name) {
        return false;
    }
    memcpy(name, assignment, name_len);
    name[name_len] = '\0';
    if (out_name) {
        *out_name = name;
    } else {
        free(name);
    }
    if (out_value) {
        *out_value = eq + 1;
    }
    return true;
}

static bool shellApplyAssignmentsPermanently(const ShellCommand *cmd,
                                             const char **out_failed_assignment,
                                             bool *out_invalid_assignment) {
    if (out_failed_assignment) {
        *out_failed_assignment = NULL;
    }
    if (out_invalid_assignment) {
        *out_invalid_assignment = false;
    }
    if (!cmd) {
        return true;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        const char *assignment = cmd->assignments[i];
        char *name = NULL;
        const char *value = NULL;
        if (!shellParseAssignment(assignment, &name, &value)) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            if (out_invalid_assignment) {
                *out_invalid_assignment = true;
            }
            free(name);
            return false;
        }
        if (setenv(name, value ? value : "", 1) != 0) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            free(name);
            return false;
        }
        free(name);
    }
    return true;
}

static void shellRestoreAssignments(ShellAssignmentBackup *backups, size_t count) {
    if (!backups) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        ShellAssignmentBackup *backup = &backups[i];
        if (!backup->name) {
            continue;
        }
        if (backup->had_previous) {
            setenv(backup->name, backup->previous_value ? backup->previous_value : "", 1);
        } else {
            unsetenv(backup->name);
        }
        free(backup->name);
        free(backup->previous_value);
    }
    free(backups);
}

static bool shellApplyAssignmentsTemporary(const ShellCommand *cmd,
                                           ShellAssignmentBackup **out_backups,
                                           size_t *out_count,
                                           const char **out_failed_assignment,
                                           bool *out_invalid_assignment) {
    if (out_backups) {
        *out_backups = NULL;
    }
    if (out_count) {
        *out_count = 0;
    }
    if (out_failed_assignment) {
        *out_failed_assignment = NULL;
    }
    if (out_invalid_assignment) {
        *out_invalid_assignment = false;
    }
    if (!cmd || cmd->assignment_count == 0) {
        return true;
    }
    ShellAssignmentBackup *backups = calloc(cmd->assignment_count, sizeof(ShellAssignmentBackup));
    if (!backups) {
        return false;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        const char *assignment = cmd->assignments[i];
        char *name = NULL;
        const char *value = NULL;
        if (!shellParseAssignment(assignment, &name, &value)) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            if (out_invalid_assignment) {
                *out_invalid_assignment = true;
            }
            shellRestoreAssignments(backups, i);
            return false;
        }
        backups[i].name = name;
        const char *previous = getenv(name);
        if (previous) {
            backups[i].previous_value = strdup(previous);
            if (!backups[i].previous_value) {
                shellRestoreAssignments(backups, i + 1);
                return false;
            }
            backups[i].had_previous = true;
        } else {
            backups[i].previous_value = NULL;
            backups[i].had_previous = false;
        }
        if (setenv(name, value ? value : "", 1) != 0) {
            if (out_failed_assignment) {
                *out_failed_assignment = assignment;
            }
            shellRestoreAssignments(backups, i + 1);
            return false;
        }
    }
    if (out_backups) {
        *out_backups = backups;
    } else {
        shellRestoreAssignments(backups, cmd->assignment_count);
    }
    if (out_count) {
        *out_count = cmd->assignment_count;
    }
    return true;
}

static bool shellWordShouldGlob(uint8_t flags, const char *text) {
    if (!text || !*text) {
        return false;
    }
    if (flags & (SHELL_WORD_FLAG_SINGLE_QUOTED | SHELL_WORD_FLAG_DOUBLE_QUOTED)) {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '*' || *cursor == '?' || *cursor == '[') {
            return true;
        }
    }
    return false;
}

static void shellBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char c) {
    if (!shellBufferEnsure(buffer, length, capacity, 1)) {
        return;
    }
    (*buffer)[(*length)++] = c;
    (*buffer)[*length] = '\0';
}

static void shellBufferAppendString(char **buffer, size_t *length, size_t *capacity, const char *str) {
    if (!str) {
        return;
    }
    size_t add = strlen(str);
    if (add == 0) {
        return;
    }
    if (!shellBufferEnsure(buffer, length, capacity, add)) {
        return;
    }
    memcpy(*buffer + *length, str, add);
    *length += add;
    (*buffer)[*length] = '\0';
}

static void shellBufferAppendSlice(char **buffer,
                                   size_t *length,
                                   size_t *capacity,
                                   const char *data,
                                   size_t slice_len) {
    if (!data || slice_len == 0) {
        return;
    }
    if (!shellBufferEnsure(buffer, length, capacity, slice_len)) {
        return;
    }
    memcpy(*buffer + *length, data, slice_len);
    *length += slice_len;
    (*buffer)[*length] = '\0';
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellHistoryWordArray;

static void shellHistoryWordArrayFree(ShellHistoryWordArray *array) {
    if (!array || !array->items) {
        return;
    }
    for (size_t i = 0; i < array->count; ++i) {
        free(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static bool shellHistoryWordArrayAppend(ShellHistoryWordArray *array, char *word) {
    if (!array || !word) {
        return false;
    }
    if (array->count == array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 8;
        char **items = realloc(array->items, new_capacity * sizeof(char *));
        if (!items) {
            return false;
        }
        array->items = items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = word;
    return true;
}

static bool shellTokenizeHistoryEntry(const char *entry, ShellHistoryWordArray *out_words) {
    if (!entry || !out_words) {
        return false;
    }
    out_words->items = NULL;
    out_words->count = 0;
    out_words->capacity = 0;

    char *current = NULL;
    size_t current_len = 0;
    size_t current_cap = 0;
    bool in_single = false;
    bool in_double = false;
    bool escape = false;
    bool word_active = false;

    for (size_t i = 0;; ++i) {
        char c = entry[i];
        bool at_end = (c == '\0');

        if (!at_end && escape) {
            shellBufferAppendChar(&current, &current_len, &current_cap, c);
            escape = false;
            word_active = true;
            continue;
        }

        if (!at_end && c == '\\' && !escape) {
            escape = true;
            word_active = true;
            continue;
        }

        if (!at_end && c == '\'' && !in_double) {
            in_single = !in_single;
            word_active = true;
            continue;
        }

        if (!at_end && c == '"' && !in_single) {
            in_double = !in_double;
            word_active = true;
            continue;
        }

        if (at_end && escape) {
            shellBufferAppendChar(&current, &current_len, &current_cap, '\\');
            escape = false;
        }

        bool is_space = (!in_single && !in_double && (c == ' ' || c == '\t'));
        if (at_end || is_space) {
            if (word_active) {
                const char *src = current ? current : "";
                char *word = strdup(src);
                if (!word) {
                    free(current);
                    shellHistoryWordArrayFree(out_words);
                    return false;
                }
                if (!shellHistoryWordArrayAppend(out_words, word)) {
                    free(word);
                    free(current);
                    shellHistoryWordArrayFree(out_words);
                    return false;
                }
            }
            free(current);
            current = NULL;
            current_len = 0;
            current_cap = 0;
            word_active = false;
            if (at_end) {
                break;
            }
            continue;
        }

        shellBufferAppendChar(&current, &current_len, &current_cap, c);
        word_active = true;
    }

    return true;
}

static char *shellJoinHistoryWords(char **items, size_t start, size_t end) {
    if (!items || start >= end) {
        return strdup("");
    }
    size_t total = 0;
    for (size_t i = start; i < end; ++i) {
        total += strlen(items[i]);
        if (i + 1 < end) {
            total += 1;
        }
    }
    char *result = (char *)malloc(total + 1);
    if (!result) {
        return NULL;
    }
    size_t pos = 0;
    for (size_t i = start; i < end; ++i) {
        size_t len = strlen(items[i]);
        memcpy(result + pos, items[i], len);
        pos += len;
        if (i + 1 < end) {
            result[pos++] = ' ';
        }
    }
    result[pos] = '\0';
    return result;
}

static bool shellHistoryCollectUntil(const char **cursor, char delim, char **out_value) {
    if (!cursor || !*cursor || !out_value) {
        return false;
    }
    const char *p = *cursor;
    char *value = NULL;
    size_t len = 0;
    size_t cap = 0;
    bool escape = false;
    while (*p) {
        char c = *p;
        if (!escape && c == '\\') {
            escape = true;
            p++;
            continue;
        }
        if (!escape && c == delim) {
            *cursor = p + 1;
            if (!value) {
                value = strdup("");
            }
            if (!value) {
                return false;
            }
            *out_value = value;
            return true;
        }
        if (escape) {
            if (c != delim && c != '\\') {
                shellBufferAppendChar(&value, &len, &cap, '\\');
            }
            shellBufferAppendChar(&value, &len, &cap, c);
            escape = false;
        } else {
            shellBufferAppendChar(&value, &len, &cap, c);
        }
        p++;
    }
    free(value);
    return false;
}

static bool shellHistoryParseSubstitutionSpec(const char *spec,
                                              bool *out_is_substitution,
                                              bool *out_global,
                                              char **out_pattern,
                                              char **out_replacement) {
    if (out_is_substitution) {
        *out_is_substitution = false;
    }
    if (out_global) {
        *out_global = false;
    }
    if (out_pattern) {
        *out_pattern = NULL;
    }
    if (out_replacement) {
        *out_replacement = NULL;
    }
    if (!spec) {
        return true;
    }
    const char *cursor = spec;
    bool prefix_global = false;
    if (cursor[0] == 'g' && cursor[1] == 's') {
        prefix_global = true;
        cursor++;
    }
    if (*cursor != 's') {
        return true;
    }
    cursor++;
    if (*cursor == '\0') {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        return false;
    }
    char delim = *cursor++;
    char *pattern = NULL;
    if (!shellHistoryCollectUntil(&cursor, delim, &pattern)) {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        free(pattern);
        return false;
    }
    char *replacement = NULL;
    if (!shellHistoryCollectUntil(&cursor, delim, &replacement)) {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        free(pattern);
        free(replacement);
        return false;
    }
    bool trailing_global = false;
    if (*cursor == 'g') {
        trailing_global = true;
        cursor++;
    }
    if (*cursor != '\0') {
        if (out_is_substitution) {
            *out_is_substitution = true;
        }
        free(pattern);
        free(replacement);
        return false;
    }
    if (out_is_substitution) {
        *out_is_substitution = true;
    }
    if (out_global) {
        *out_global = prefix_global || trailing_global;
    }
    if (out_pattern) {
        *out_pattern = pattern;
    } else {
        free(pattern);
    }
    if (out_replacement) {
        *out_replacement = replacement;
    } else {
        free(replacement);
    }
    return true;
}

static void shellHistoryAppendReplacement(char **buffer,
                                          size_t *length,
                                          size_t *capacity,
                                          const char *replacement,
                                          const char *match_start,
                                          size_t match_len) {
    if (!replacement) {
        return;
    }
    for (size_t i = 0; replacement[i]; ++i) {
        char c = replacement[i];
        if (c == '&') {
            if (match_start && match_len > 0) {
                shellBufferAppendSlice(buffer, length, capacity, match_start, match_len);
            }
            continue;
        }
        if (c == '\\') {
            char next = replacement[i + 1];
            if (next == '\0') {
                shellBufferAppendChar(buffer, length, capacity, '\\');
                continue;
            }
            i++;
            switch (next) {
                case 't':
                    shellBufferAppendChar(buffer, length, capacity, '\t');
                    break;
                case 'n':
                    shellBufferAppendChar(buffer, length, capacity, '\n');
                    break;
                case '\\':
                    shellBufferAppendChar(buffer, length, capacity, '\\');
                    break;
                case '&':
                    shellBufferAppendChar(buffer, length, capacity, '&');
                    break;
                default:
                    shellBufferAppendChar(buffer, length, capacity, next);
                    break;
            }
            continue;
        }
        shellBufferAppendChar(buffer, length, capacity, c);
    }
}

static char *shellHistoryApplyRegexSubstitution(const char *entry,
                                                const char *pattern,
                                                const char *replacement,
                                                bool global) {
    if (!entry || !pattern || !replacement) {
        return NULL;
    }
    regex_t regex;
    int rc = regcomp(&regex, pattern, REG_EXTENDED);
    if (rc != 0) {
        return NULL;
    }

    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;
    const char *cursor = entry;
    bool replaced = false;

    while (cursor && *cursor) {
        regmatch_t match;
        int flags = (cursor != entry) ? REG_NOTBOL : 0;
        rc = regexec(&regex, cursor, 1, &match, flags);
        if (rc != 0) {
            shellBufferAppendString(&result, &length, &capacity, cursor);
            break;
        }
        replaced = true;
        size_t match_len = (size_t)(match.rm_eo - match.rm_so);
        shellBufferAppendSlice(&result, &length, &capacity, cursor, (size_t)match.rm_so);
        const char *match_start = cursor + match.rm_so;
        shellHistoryAppendReplacement(&result, &length, &capacity, replacement, match_start, match_len);
        cursor += match.rm_eo;
        if (!global) {
            shellBufferAppendString(&result, &length, &capacity, cursor);
            break;
        }
        if (match_len == 0) {
            if (*cursor == '\0') {
                break;
            }
            shellBufferAppendChar(&result, &length, &capacity, *cursor);
            cursor++;
        }
    }

    if (!replaced) {
        if (result) {
            free(result);
        }
        result = strdup(entry);
    }

    regfree(&regex);
    return result;
}

static bool shellApplyHistoryDesignator(const char *entry, const char *designator, size_t len, char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (!entry || !out_line) {
        return false;
    }
    if (!designator || len == 0) {
        *out_line = strdup(entry);
        return *out_line != NULL;
    }

    char *spec = strndup(designator, len);
    if (!spec) {
        return false;
    }

    ShellHistoryWordArray words = {0};
    if (!shellTokenizeHistoryEntry(entry, &words)) {
        free(spec);
        return false;
    }

    bool success = true;
    char *result = NULL;

    bool is_substitution = false;
    bool substitution_global = false;
    char *sub_pattern = NULL;
    char *sub_replacement = NULL;
    if (!shellHistoryParseSubstitutionSpec(spec, &is_substitution, &substitution_global, &sub_pattern, &sub_replacement)) {
        success = false;
    } else if (is_substitution) {
        result = shellHistoryApplyRegexSubstitution(entry,
                                                    sub_pattern ? sub_pattern : "",
                                                    sub_replacement ? sub_replacement : "",
                                                    substitution_global);
        if (!result) {
            success = false;
        }
    } else if (strcmp(spec, "*") == 0) {
        if (words.count <= 1) {
            result = strdup("");
        } else {
            result = shellJoinHistoryWords(words.items, 1, words.count);
        }
    } else if (strcmp(spec, "^") == 0) {
        if (words.count <= 1) {
            success = false;
        } else {
            result = strdup(words.items[1]);
        }
    } else if (strcmp(spec, "$") == 0) {
        if (words.count == 0) {
            success = false;
        } else {
            result = strdup(words.items[words.count - 1]);
        }
    } else {
        char *endptr = NULL;
        long index = strtol(spec, &endptr, 10);
        if (endptr && *endptr == '\0') {
            if (index < 0 || (size_t)index >= words.count) {
                success = false;
            } else {
                result = strdup(words.items[index]);
            }
        } else {
            success = false;
        }
    }

    free(spec);
    free(sub_pattern);
    free(sub_replacement);

    if (!success || !result) {
        free(result);
        shellHistoryWordArrayFree(&words);
        return false;
    }

    *out_line = result;
    shellHistoryWordArrayFree(&words);
    return true;
}

static const char *shellHistoryEntryByIndex(long index) {
    if (gShellHistory.count == 0 || index == 0) {
        return NULL;
    }
    if (index > 0) {
        if ((size_t)index > gShellHistory.count) {
            return NULL;
        }
        return gShellHistory.entries[index - 1];
    }
    size_t offset = (size_t)(-index);
    if (offset == 0 || offset > gShellHistory.count) {
        return NULL;
    }
    return gShellHistory.entries[gShellHistory.count - offset];
}

static const char *shellHistoryFindByPrefix(const char *prefix, size_t len) {
    if (!prefix || len == 0) {
        return NULL;
    }
    for (size_t i = gShellHistory.count; i > 0; --i) {
        const char *entry = gShellHistory.entries[i - 1];
        if (!entry) {
            continue;
        }
        const char *trimmed = entry;
        while (*trimmed == ' ' || *trimmed == '\t') {
            trimmed++;
        }
        if (strncmp(trimmed, prefix, len) == 0) {
            char next = trimmed[len];
            if (next == '\0' || next == ' ' || next == '\t') {
                return entry;
            }
        }
    }
    return NULL;
}

static const char *shellHistoryFindBySubstring(const char *needle, size_t len) {
    if (!needle || len == 0) {
        return NULL;
    }
    char *pattern = strndup(needle, len);
    if (!pattern) {
        return NULL;
    }
    for (size_t i = gShellHistory.count; i > 0; --i) {
        const char *entry = gShellHistory.entries[i - 1];
        if (entry && strstr(entry, pattern)) {
            free(pattern);
            return entry;
        }
    }
    free(pattern);
    return NULL;
}

static const char *shellHistoryFindByRegex(const char *pattern, size_t len, bool *out_invalid) {
    if (out_invalid) {
        *out_invalid = false;
    }
    if (!pattern || len == 0) {
        return NULL;
    }
    char *expr = strndup(pattern, len);
    if (!expr) {
        if (out_invalid) {
            *out_invalid = true;
        }
        return NULL;
    }
    regex_t regex;
    int rc = regcomp(&regex, expr, REG_EXTENDED | REG_NOSUB);
    free(expr);
    if (rc != 0) {
        if (out_invalid) {
            *out_invalid = true;
        }
        return NULL;
    }
    const char *result = NULL;
    for (size_t i = gShellHistory.count; i > 0; --i) {
        const char *entry = gShellHistory.entries[i - 1];
        if (entry && regexec(&regex, entry, 0, NULL, 0) == 0) {
            result = entry;
            break;
        }
    }
    regfree(&regex);
    return result;
}

static char *shellJoinPositionalParameters(void) {
    if (gParamCount <= 0 || !gParamValues) {
        return strdup("");
    }
    size_t total = 0;
    for (int i = 0; i < gParamCount; ++i) {
        if (gParamValues[i]) {
            total += strlen(gParamValues[i]);
        }
        if (i + 1 < gParamCount) {
            total += 1; // space separator
        }
    }
    char *result = (char *)malloc(total + 1);
    if (!result) {
        return NULL;
    }
    size_t pos = 0;
    for (int i = 0; i < gParamCount; ++i) {
        const char *value = gParamValues[i] ? gParamValues[i] : "";
        size_t len = strlen(value);
        memcpy(result + pos, value, len);
        pos += len;
        if (i + 1 < gParamCount) {
            result[pos++] = ' ';
        }
    }
    result[pos] = '\0';
    return result;
}

static char *shellLookupParameterValue(const char *name, size_t len) {
    if (!name || len == 0) {
        return strdup("");
    }
    if (len == 1) {
        switch (name[0]) {
            case '?': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
                return strdup(buffer);
            }
            case '$': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", (int)getpid());
                return strdup(buffer);
            }
            case '#': {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d", gParamCount);
                return strdup(buffer);
            }
            case '*':
            case '@':
                return shellJoinPositionalParameters();
            case '0': {
                if (gShellArg0) {
                    return strdup(gShellArg0);
                }
                return strdup("exsh");
            }
            default:
                break;
        }
    }

    bool numeric = true;
    for (size_t i = 0; i < len; ++i) {
        if (!isdigit((unsigned char)name[i])) {
            numeric = false;
            break;
        }
    }
    if (numeric) {
        long index = strtol(name, NULL, 10);
        if (index >= 1 && index <= gParamCount && gParamValues) {
            const char *value = gParamValues[index - 1] ? gParamValues[index - 1] : "";
            return strdup(value);
        }
        return strdup("");
    }

    char *key = (char *)malloc(len + 1);
    if (!key) {
        return NULL;
    }
    memcpy(key, name, len);
    key[len] = '\0';
    const char *env = getenv(key);
    free(key);
    if (!env) {
        return strdup("");
    }
    return strdup(env);
}

static char *shellExpandParameter(const char *input, size_t *out_consumed) {
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || !*input) {
        return NULL;
    }
    if (*input == '{') {
        const char *cursor = input + 1;
        bool length_only = false;
        if (*cursor == '#') {
            length_only = true;
            cursor++;
        }
        const char *name_start = cursor;
        while (*cursor && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
            cursor++;
        }
        if (*cursor != '}' || cursor == name_start) {
            return NULL;
        }
        size_t name_len = (size_t)(cursor - name_start);
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input) + 1;
        }
        char *value = shellLookupParameterValue(name_start, name_len);
        if (!value) {
            return NULL;
        }
        if (length_only) {
            size_t val_len = strlen(value);
            free(value);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%zu", val_len);
            return strdup(buffer);
        }
        return value;
    }

    if (*input == '$') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", (int)getpid());
        return strdup(buffer);
    }

    if (*input == '?') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
        return strdup(buffer);
    }

    if (*input == '#') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gParamCount);
        return strdup(buffer);
    }

    if (*input == '*' || *input == '@') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return shellJoinPositionalParameters();
    }

    if (*input == '0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        if (gShellArg0) {
            return strdup(gShellArg0);
        }
        return strdup("exsh");
    }

    if (isdigit((unsigned char)*input)) {
        const char *cursor = input;
        while (isdigit((unsigned char)*cursor)) {
            cursor++;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellLookupParameterValue(input, (size_t)(cursor - input));
    }

    if (isalpha((unsigned char)*input) || *input == '_') {
        const char *cursor = input + 1;
        while (isalnum((unsigned char)*cursor) || *cursor == '_') {
            cursor++;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellLookupParameterValue(input, (size_t)(cursor - input));
    }

    return NULL;
}

static void shellMarkArithmeticError(void) {
    shellUpdateStatus(1);
    gShellArithmeticErrorPending = true;
}

typedef struct {
    const char *input;
    size_t length;
    size_t pos;
} ShellArithmeticParser;

static void shellArithmeticSkipWhitespace(ShellArithmeticParser *parser) {
    if (!parser) {
        return;
    }
    while (parser->pos < parser->length &&
           isspace((unsigned char)parser->input[parser->pos])) {
        parser->pos++;
    }
}

static bool shellArithmeticParseValueString(const char *text, long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *endptr = NULL;
    long long value = strtoll(text, &endptr, 0);
    if (errno != 0) {
        return false;
    }
    if (endptr && *endptr != '\0') {
        while (endptr && *endptr && isspace((unsigned char)*endptr)) {
            endptr++;
        }
        if (endptr && *endptr != '\0') {
            return false;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticParseExpression(ShellArithmeticParser *parser, long long *out_value);

static bool shellArithmeticParsePrimary(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return false;
    }
    char c = parser->input[parser->pos];
    if (c == '(') {
        parser->pos++;
        if (!shellArithmeticParseExpression(parser, out_value)) {
            return false;
        }
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length || parser->input[parser->pos] != ')') {
            return false;
        }
        parser->pos++;
        return true;
    }
    if (c == '$') {
        parser->pos++;
        size_t consumed = 0;
        char *value = shellExpandParameter(parser->input + parser->pos, &consumed);
        if (!value) {
            return false;
        }
        parser->pos += consumed;
        long long parsed = 0;
        bool ok = shellArithmeticParseValueString(value, &parsed);
        free(value);
        if (!ok) {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = parser->pos;
        parser->pos++;
        while (parser->pos < parser->length) {
            char ch = parser->input[parser->pos];
            if (!isalnum((unsigned char)ch) && ch != '_') {
                break;
            }
            parser->pos++;
        }
        size_t len = parser->pos - start;
        char *value = shellLookupParameterValue(parser->input + start, len);
        if (!value) {
            return false;
        }
        long long parsed = 0;
        bool ok = shellArithmeticParseValueString(value, &parsed);
        free(value);
        if (!ok) {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    if (isdigit((unsigned char)c)) {
        const char *start_ptr = parser->input + parser->pos;
        errno = 0;
        char *endptr = NULL;
        long long value = strtoll(start_ptr, &endptr, 0);
        if (errno != 0 || endptr == start_ptr) {
            return false;
        }
        size_t consumed = (size_t)(endptr - start_ptr);
        parser->pos += consumed;
        if (parser->pos < parser->length) {
            char next = parser->input[parser->pos];
            if (isalnum((unsigned char)next) || next == '_') {
                return false;
            }
        }
        *out_value = value;
        return true;
    }
    return false;
}

static bool shellArithmeticParseUnary(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return false;
    }
    char c = parser->input[parser->pos];
    if (c == '+') {
        parser->pos++;
        return shellArithmeticParseUnary(parser, out_value);
    }
    if (c == '-') {
        parser->pos++;
        long long value = 0;
        if (!shellArithmeticParseUnary(parser, &value)) {
            return false;
        }
        *out_value = -value;
        return true;
    }
    return shellArithmeticParsePrimary(parser, out_value);
}

static bool shellArithmeticParseTerm(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    long long value = 0;
    if (!shellArithmeticParseUnary(parser, &value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length) {
            break;
        }
        char op = parser->input[parser->pos];
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        parser->pos++;
        long long rhs = 0;
        if (!shellArithmeticParseUnary(parser, &rhs)) {
            return false;
        }
        if (op == '*') {
            value *= rhs;
        } else if (op == '/') {
            if (rhs == 0) {
                return false;
            }
            value /= rhs;
        } else {
            if (rhs == 0) {
                return false;
            }
            value %= rhs;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticParseExpression(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    long long value = 0;
    if (!shellArithmeticParseTerm(parser, &value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length) {
            break;
        }
        char op = parser->input[parser->pos];
        if (op != '+' && op != '-') {
            break;
        }
        parser->pos++;
        long long rhs = 0;
        if (!shellArithmeticParseTerm(parser, &rhs)) {
            return false;
        }
        if (op == '+') {
            value += rhs;
        } else {
            value -= rhs;
        }
    }
    *out_value = value;
    return true;
}

static char *shellEvaluateArithmetic(const char *expr, bool *out_error) {
    if (out_error) {
        *out_error = false;
    }
    if (!expr) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    ShellArithmeticParser parser;
    parser.input = expr;
    parser.length = strlen(expr);
    parser.pos = 0;
    long long value = 0;
    if (!shellArithmeticParseExpression(&parser, &value)) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    shellArithmeticSkipWhitespace(&parser);
    if (parser.pos < parser.length) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%lld", value);
    if (written < 0) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char *result = strdup(buffer);
    if (!result) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    return result;
}

static char *shellExpandWord(const char *text, uint8_t flags, const char *meta, size_t meta_len) {
    if (!text) {
        return strdup("");
    }
    ShellMetaSubstitution *subs = NULL;
    size_t sub_count = 0;
    if (!shellParseCommandMetadata(meta, meta_len, &subs, &sub_count)) {
        subs = NULL;
        sub_count = 0;
    }
    size_t text_len = strlen(text);
    size_t length = 0;
    size_t capacity = text_len + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        shellFreeMetaSubstitutions(subs, sub_count);
        return NULL;
    }
    buffer[0] = '\0';
    bool base_single = (flags & SHELL_WORD_FLAG_SINGLE_QUOTED) != 0;
    bool base_double = (flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    bool in_single_segment = false;
    bool in_double_segment = false;
    bool saw_single_marker = false;
    bool saw_double_marker = false;
    bool has_arithmetic = (flags & SHELL_WORD_FLAG_HAS_ARITHMETIC) != 0;
    size_t sub_index = 0;
    for (size_t i = 0; i < text_len;) {
        char c = text[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
            i++;
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
            i++;
            continue;
        }
        bool effective_single = in_single_segment || (!saw_single_marker && base_single);
        bool effective_double = in_double_segment || (!saw_double_marker && base_double);
        if (effective_single) {
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        bool handled = false;
        if (sub_index < sub_count) {
            ShellMetaSubstitution *sub = &subs[sub_index];
            size_t span = sub->span_length;
            if (sub->style == SHELL_META_SUBSTITUTION_DOLLAR && c == '$' && i + 1 < text_len && text[i + 1] == '(') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            } else if (sub->style == SHELL_META_SUBSTITUTION_BACKTICK && c == '`') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            }
        }
        if (handled) {
            continue;
        }
        if (c == '$' && has_arithmetic && i + 2 < text_len && text[i + 1] == '(' && text[i + 2] == '(') {
            size_t expr_start = i + 3;
            size_t j = expr_start;
            int depth = 1;
            while (j < text_len) {
                char inner = text[j];
                if (inner == '(') {
                    depth++;
                } else if (inner == ')') {
                    depth--;
                    if (depth == 0) {
                        break;
                    }
                }
                j++;
            }
            size_t span = 0;
            if (depth == 0 && j + 1 < text_len && text[j + 1] == ')') {
                span = (j + 2) - i;
                size_t expr_len = j - expr_start;
                char *expr = (char *)malloc(expr_len + 1);
                if (!expr) {
                    shellMarkArithmeticError();
                    shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                    i += span;
                    continue;
                }
                if (expr_len > 0) {
                    memcpy(expr, text + expr_start, expr_len);
                }
                expr[expr_len] = '\0';
                bool eval_error = false;
                char *result = shellEvaluateArithmetic(expr, &eval_error);
                free(expr);
                if (!eval_error && result) {
                    shellBufferAppendString(&buffer, &length, &capacity, result);
                    free(result);
                } else {
                    shellMarkArithmeticError();
                    shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                }
                i += span;
                continue;
            } else {
                span = text_len - i;
                shellMarkArithmeticError();
                shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                i = text_len;
                continue;
            }
        }
        bool treat_as_double = effective_double;
        if (c == '\\') {
            if (i + 1 < text_len) {
                char next = text[i + 1];
                if (!treat_as_double || next == '$' || next == '"' || next == '\\' || next == '`' || next == '\n') {
                    shellBufferAppendChar(&buffer, &length, &capacity, next);
                    i += 2;
                    continue;
                }
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '$') {
            size_t consumed = 0;
            char *expanded = shellExpandParameter(text + i + 1, &consumed);
            if (expanded) {
                shellBufferAppendString(&buffer, &length, &capacity, expanded);
                free(expanded);
                i += consumed + 1;
                continue;
            }
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }
    shellFreeMetaSubstitutions(subs, sub_count);
    return buffer;
}

static void shellFreeRedirections(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        free(cmd->redirs[i].path);
        free(cmd->redirs[i].here_doc);
    }
    free(cmd->redirs);
    cmd->redirs = NULL;
    cmd->redir_count = 0;
}

static void shellFreeCommand(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        free(cmd->assignments[i]);
    }
    free(cmd->assignments);
    cmd->assignments = NULL;
    cmd->assignment_count = 0;
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    cmd->argv = NULL;
    cmd->argc = 0;
    shellFreeRedirections(cmd);
}

static bool shellParseBool(const char *value, bool *out_flag) {
    if (!value || !out_flag) {
        return false;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        *out_flag = true;
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0) {
        *out_flag = false;
        return true;
    }
    return false;
}

static bool shellStringEqualsIgnoreCase(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    return strcasecmp(lhs, rhs) == 0;
}

static bool shellTryParseIntegerLiteral(const char *text, long long *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellLooksLikeFloatLiteral(const char *text) {
    if (!text) {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') {
            return true;
        }
    }
    if (shellStringEqualsIgnoreCase(text, "inf") ||
        shellStringEqualsIgnoreCase(text, "+inf") ||
        shellStringEqualsIgnoreCase(text, "-inf") ||
        shellStringEqualsIgnoreCase(text, "infinity") ||
        shellStringEqualsIgnoreCase(text, "+infinity") ||
        shellStringEqualsIgnoreCase(text, "-infinity") ||
        shellStringEqualsIgnoreCase(text, "nan") ||
        shellStringEqualsIgnoreCase(text, "+nan") ||
        shellStringEqualsIgnoreCase(text, "-nan")) {
        return true;
    }
    return false;
}

static bool shellTryParseFloatLiteral(const char *text, double *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static Value shellConvertBuiltinArgument(const char *text) {
    if (!text) {
        return makeString("");
    }

    enum {
        SHELL_ARG_MODE_AUTO,
        SHELL_ARG_MODE_STRING,
        SHELL_ARG_MODE_BOOL,
        SHELL_ARG_MODE_INT,
        SHELL_ARG_MODE_FLOAT,
        SHELL_ARG_MODE_NIL
    } mode = SHELL_ARG_MODE_AUTO;

    const char *payload = text;

    if (strncasecmp(payload, "str:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "string:", 7) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 7;
    } else if (strncasecmp(payload, "raw:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "bool:", 5) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 5;
    } else if (strncasecmp(payload, "boolean:", 8) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 8;
    } else if (strncasecmp(payload, "int:", 4) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 4;
    } else if (strncasecmp(payload, "integer:", 8) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 8;
    } else if (strncasecmp(payload, "float:", 6) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 6;
    } else if (strncasecmp(payload, "double:", 7) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 7;
    } else if (strncasecmp(payload, "real:", 5) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 5;
    } else if (strncasecmp(payload, "nil:", 4) == 0) {
        mode = SHELL_ARG_MODE_NIL;
        payload += 4;
    }

    if (mode == SHELL_ARG_MODE_STRING) {
        return makeString(payload ? payload : "");
    }

    if (mode == SHELL_ARG_MODE_NIL) {
        return makeNil();
    }

    if (mode == SHELL_ARG_MODE_BOOL || mode == SHELL_ARG_MODE_AUTO) {
        bool flag = false;
        if (shellParseBool(payload, &flag)) {
            return makeBoolean(flag ? 1 : 0);
        }
        if (mode == SHELL_ARG_MODE_BOOL) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_INT || mode == SHELL_ARG_MODE_AUTO) {
        long long int_value = 0;
        if (shellTryParseIntegerLiteral(payload, &int_value)) {
            return makeInt(int_value);
        }
        if (mode == SHELL_ARG_MODE_INT) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_FLOAT || mode == SHELL_ARG_MODE_AUTO) {
        if (mode == SHELL_ARG_MODE_FLOAT || shellLooksLikeFloatLiteral(payload)) {
            double dbl_value = 0.0;
            if (shellTryParseFloatLiteral(payload, &dbl_value)) {
                return makeDouble(dbl_value);
            }
            if (mode == SHELL_ARG_MODE_FLOAT) {
                return makeString(payload ? payload : "");
            }
        }
    }

    if (mode == SHELL_ARG_MODE_AUTO && payload && *payload) {
        if (shellStringEqualsIgnoreCase(payload, "nil") ||
            shellStringEqualsIgnoreCase(payload, "null")) {
            return makeNil();
        }
    }

    return makeString(payload ? payload : "");
}

static void shellUpdateStatus(int status) {
    if (gShellArithmeticErrorPending) {
        status = 1;
        gShellArithmeticErrorPending = false;
    }
    gShellRuntime.last_status = status;
    char buffer[16];
    snprintf(buffer, sizeof(buffer), "%d", status);
    setenv("PSCALSHELL_LAST_STATUS", buffer, 1);
    if (status != 0) {
        if (gShellRuntime.errexit_enabled) {
            gShellRuntime.errexit_pending = true;
            gShellExitRequested = true;
            if (gShellCurrentVm) {
                gShellCurrentVm->abort_requested = true;
                gShellCurrentVm->exit_requested = true;
                gShellCurrentVm->current_builtin_name = "errexit";
            }
        }
    } else {
        gShellRuntime.errexit_pending = false;
    }
}

/*
 * POSIX specifies that foreground commands should see the shell's inherited
 * signal dispositions, except that asynchronous lists without job control must
 * inherit SIG_IGN for SIGINT and SIGQUIT, and that traps fire only after the
 * foreground job or wait completes.  We record pending signals in an
 * async-signal-safe manner and reconcile them once we're back in the main
 * interpreter loop so the runtime can unwind cleanly before honouring traps.
 */
static void shellHandlePendingSignal(int signo) {
    if (signo != SIGINT && signo != SIGQUIT && signo != SIGTSTP) {
        return;
    }

    shellUpdateStatus(128 + signo);

    if (gShellCurrentVm) {
        gShellCurrentVm->exit_requested = true;
        gShellCurrentVm->current_builtin_name = "signal";
    }

    if (gShellLoopStackSize > 0) {
        gShellRuntime.break_requested = true;
        gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
        shellLoopRequestBreakLevels((int)gShellLoopStackSize);
    }
}

void shellRuntimeProcessPendingSignals(void) {
    for (int signo = 1; signo < NSIG; ++signo) {
        if (!gShellPendingSignals[signo]) {
            continue;
        }
        gShellPendingSignals[signo] = 0;
        shellHandlePendingSignal(signo);
    }
}

void shellRuntimeInitSignals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = shellSignalHandler;
    action.sa_flags |= SA_RESTART;

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGTSTP, &action, NULL);
}

static bool shellHistoryEnsureCapacity(size_t needed) {
    if (gShellHistory.capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellHistory.capacity ? gShellHistory.capacity * 2 : 16;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(char *)) {
        return false;
    }
    char **entries = realloc(gShellHistory.entries, new_capacity * sizeof(char *));
    if (!entries) {
        return false;
    }
    gShellHistory.entries = entries;
    gShellHistory.capacity = new_capacity;
    return true;
}

void shellRuntimeRecordHistory(const char *line) {
    if (!line) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        --len;
    }
    if (len == 0) {
        return;
    }
    bool has_content = false;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != ' ' && line[i] != '\t') {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return;
    }
    if (!shellHistoryEnsureCapacity(gShellHistory.count + 1)) {
        return;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    gShellHistory.entries[gShellHistory.count++] = copy;
}

size_t shellRuntimeHistoryCount(void) {
    return gShellHistory.count;
}

bool shellRuntimeHistoryGetEntry(size_t reverse_index, char **out_line) {
    if (!out_line) {
        return false;
    }
    *out_line = NULL;
    if (reverse_index >= gShellHistory.count) {
        return false;
    }
    const char *entry = gShellHistory.entries[gShellHistory.count - reverse_index - 1];
    if (!entry) {
        return false;
    }
    char *copy = strdup(entry);
    if (!copy) {
        return false;
    }
    *out_line = copy;
    return true;
}

void shellRuntimeSetArg0(const char *name) {
    char *copy = NULL;
    if (name && *name) {
        copy = strdup(name);
        if (!copy) {
            return;
        }
    }
    free(gShellArg0);
    gShellArg0 = copy;
}

const char *shellRuntimeGetArg0(void) {
    return gShellArg0;
}

void shellRuntimeInitJobControl(void) {
    shellEnsureJobControl();
}

typedef enum {
    SHELL_HISTORY_EXPAND_OK,
    SHELL_HISTORY_EXPAND_NOT_FOUND,
    SHELL_HISTORY_EXPAND_INVALID
} ShellHistoryExpandResult;

static bool shellIsHistoryTerminator(char c) {
    switch (c) {
        case '\0':
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case ';':
        case '&':
        case '|':
        case '(':
        case ')':
        case '<':
        case '>':
            return true;
        default:
            return false;
    }
}

static ShellHistoryExpandResult shellExpandHistoryDesignatorAt(const char *input,
                                                               size_t *out_consumed,
                                                               char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || input[0] != '!') {
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *cursor = input + 1;
    if (*cursor == '\0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *designator_start = NULL;
    size_t designator_len = 0;
    const char *entry = NULL;

    if (*cursor == '$' || *cursor == '*' || *cursor == '^') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
        entry = shellHistoryEntryByIndex(-1);
        if (!entry) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_NOT_FOUND;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    long numeric_index = 0;
    bool has_index = false;
    const char *search_token_start = NULL;
    size_t search_token_len = 0;
    bool search_substring = false;
    bool search_regex = false;

    if (*cursor == '!') {
        numeric_index = -1;
        has_index = true;
        cursor++;
    } else if (*cursor == '-') {
        char *endptr = NULL;
        long value = strtol(cursor + 1, &endptr, 10);
        if (endptr == cursor + 1) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor + 1 - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = -value;
        cursor = endptr;
        has_index = true;
    } else if (isdigit((unsigned char)*cursor)) {
        char *endptr = NULL;
        long value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = value;
        cursor = endptr;
        has_index = true;
    } else if (*cursor == '?') {
        cursor++;
        const char *start = cursor;
        const char *closing = strchr(cursor, '?');
        if (!closing) {
            if (out_consumed) {
                *out_consumed = strlen(input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(closing - start);
        if (search_token_len >= 2 && start[0] == '/' && start[search_token_len - 1] == '/') {
            search_regex = true;
            search_token_start = start + 1;
            search_token_len -= 2;
            if (search_token_len == 0) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        }
        cursor = closing + 1;
        search_substring = true;
    } else {
        const char *start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor) &&
               *cursor != ':' && *cursor != '$' && *cursor != '^' && *cursor != '*') {
            cursor++;
        }
        if (cursor == start) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(cursor - start);
    }

    if (*cursor == '$' || *cursor == '^' || *cursor == '*') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
    } else if (*cursor == ':') {
        cursor++;
        designator_start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor)) {
            cursor++;
        }
        designator_len = (size_t)(cursor - designator_start);
        if (designator_len == 0) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
    }

    if (has_index) {
        entry = shellHistoryEntryByIndex(numeric_index);
    } else if (search_substring) {
        if (search_regex) {
            bool invalid = false;
            entry = shellHistoryFindByRegex(search_token_start, search_token_len, &invalid);
            if (!entry && invalid) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        } else {
            entry = shellHistoryFindBySubstring(search_token_start, search_token_len);
        }
    } else {
        entry = shellHistoryFindByPrefix(search_token_start, search_token_len);
    }

    if (!entry) {
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return SHELL_HISTORY_EXPAND_NOT_FOUND;
    }

    if (out_consumed) {
        *out_consumed = (size_t)(cursor - input);
    }

    if (designator_start && designator_len > 0) {
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    *out_line = strdup(entry);
    if (!*out_line) {
        return SHELL_HISTORY_EXPAND_INVALID;
    }
    return SHELL_HISTORY_EXPAND_OK;
}

bool shellRuntimeExpandHistoryReference(const char *input,
                                        char **out_line,
                                        bool *out_did_expand,
                                        char **out_error_token) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_did_expand) {
        *out_did_expand = false;
    }
    if (out_error_token) {
        *out_error_token = NULL;
    }
    if (!input || !out_line) {
        return false;
    }

    size_t capacity = strlen(input) + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return false;
    }
    buffer[0] = '\0';
    size_t length = 0;

    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; input[i];) {
        char c = input[i];
        if (c == '\\' && !in_single) {
            if (input[i + 1] == '!') {
                shellBufferAppendChar(&buffer, &length, &capacity, '!');
                i += 2;
                continue;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '\'') {
            if (!in_double) {
                in_single = !in_single;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '"') {
            if (!in_single) {
                in_double = !in_double;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '!' && !in_single) {
            size_t consumed = 0;
            char *replacement = NULL;
            ShellHistoryExpandResult result =
                shellExpandHistoryDesignatorAt(input + i, &consumed, &replacement);
            if (result != SHELL_HISTORY_EXPAND_OK) {
                if (out_error_token) {
                    size_t error_len = consumed > 0 ? consumed : 1;
                    char *token = strndup(input + i, error_len);
                    *out_error_token = token;
                }
                free(replacement);
                free(buffer);
                return false;
            }
            if (replacement) {
                shellBufferAppendString(&buffer, &length, &capacity, replacement);
                free(replacement);
            }
            if (out_did_expand) {
                *out_did_expand = true;
            }
            i += consumed;
            continue;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    *out_line = buffer;
    return true;
}

static bool shellIsRuntimeBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    static const char *kBuiltins[] = {"cd",    "pwd",    "exit",    "export",  "unset",    "setenv",   "unsetenv",
                                      "set",   "trap",   "local",  "break",   "continue", "alias",    "history",
                                      "jobs",  "fg",     "bg",     "wait",    "builtin",  "source",   ":"};

    size_t count = sizeof(kBuiltins) / sizeof(kBuiltins[0]);
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(name, kBuiltins[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool shellInvokeFunction(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    const char *name = cmd->argv[0];
    ShellFunctionEntry *entry = shellFindFunctionEntry(name);
    if (!entry || !entry->compiled) {
        return false;
    }
    VM function_vm;
    initVM(&function_vm);
    InterpretResult result = interpretBytecode(&function_vm, &entry->compiled->chunk,
                                               globalSymbols, constGlobalSymbols, procedure_table, 0);
    if (result != INTERPRET_OK) {
        shellUpdateStatus(1);
    } else {
        shellUpdateStatus(shellRuntimeLastStatus());
    }
    freeVM(&function_vm);
    return true;
}

static bool shellInvokeBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    if (shellInvokeFunction(vm, cmd)) {
        return true;
    }
    const char *name = cmd->argv[0];
    if (!name || !shellIsRuntimeBuiltin(name)) {
        return false;
    }
    VmBuiltinFn handler = getVmBuiltinHandler(name);
    if (!handler) {
        return false;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    Value *args = NULL;
    if (arg_count > 0) {
        args = calloc((size_t)arg_count, sizeof(Value));
        if (!args) {
            runtimeError(vm, "shell builtin '%s': out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        for (int i = 0; i < arg_count; ++i) {
            args[i] = makeString(cmd->argv[i + 1]);
        }
    }
    handler(vm, arg_count, args);
    if (args) {
        for (int i = 0; i < arg_count; ++i) {
            freeValue(&args[i]);
        }
        free(args);
    }
    return true;
}

static int shellStatusFromWait(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return status;
}

static void shellFreeJob(ShellJob *job) {
    if (!job) {
        return;
    }
    free(job->command);
    job->command = NULL;
    free(job->pids);
    job->pids = NULL;
    job->pid_count = 0;
    job->pgid = -1;
    job->running = false;
    job->stopped = false;
    job->last_status = 0;
}

static void shellRemoveJobAt(size_t index) {
    if (index >= gShellJobCount) {
        return;
    }
    ShellJob *job = &gShellJobs[index];
    shellFreeJob(job);
    if (index + 1 < gShellJobCount) {
        gShellJobs[index] = gShellJobs[gShellJobCount - 1];
    }
    gShellJobCount--;
    if (gShellJobCount == 0 && gShellJobs) {
        free(gShellJobs);
        gShellJobs = NULL;
    }
}

static ShellJob *shellRegisterJob(pid_t pgid, const pid_t *pids, size_t pid_count, const ShellCommand *cmd) {
    if (pgid <= 0 || !pids || pid_count == 0 || !cmd) {
        return NULL;
    }

    pid_t *pid_copy = malloc(sizeof(pid_t) * pid_count);
    if (!pid_copy) {
        return NULL;
    }
    memcpy(pid_copy, pids, sizeof(pid_t) * pid_count);

    char *summary = NULL;
    if (cmd->argc > 0 && cmd->argv) {
        size_t len = 0;
        for (size_t i = 0; i < cmd->argc; ++i) {
            len += strlen(cmd->argv[i]) + 1;
        }
        summary = malloc(len + 1);
        if (summary) {
            summary[0] = '\0';
            for (size_t i = 0; i < cmd->argc; ++i) {
                strcat(summary, cmd->argv[i]);
                if (i + 1 < cmd->argc) {
                    strcat(summary, " ");
                }
            }
        }
    }

    ShellJob *new_jobs = realloc(gShellJobs, sizeof(ShellJob) * (gShellJobCount + 1));
    if (!new_jobs) {
        free(summary);
        free(pid_copy);
        return NULL;
    }

    gShellJobs = new_jobs;
    ShellJob *job = &gShellJobs[gShellJobCount++];
    job->pgid = pgid;
    job->pids = pid_copy;
    job->pid_count = pid_count;
    job->running = true;
    job->stopped = false;
    job->last_status = 0;
    job->command = summary;
    return job;
}

static int shellCollectJobs(void) {
    int reaped = 0;
    for (size_t i = 0; i < gShellJobCount;) {
        ShellJob *job = &gShellJobs[i];
        bool job_active = false;

        if (!job->pids || job->pid_count == 0) {
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        for (size_t j = 0; j < job->pid_count; ++j) {
            pid_t pid = job->pids[j];
            if (pid <= 0) {
                continue;
            }
            int status = 0;
            pid_t res = waitpid(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (res == 0) {
                job_active = true;
                continue;
            }
            if (res < 0) {
                if (errno == EINTR) {
                    job_active = true;
                    continue;
                }
                if (errno == ECHILD) {
                    job->pids[j] = -1;
                }
                continue;
            }
            if (WIFSTOPPED(status)) {
                job->stopped = true;
                job->running = false;
                job_active = true;
            } else if (WIFCONTINUED(status)) {
                job->stopped = false;
                job->running = true;
                job_active = true;
            } else {
                job->last_status = shellStatusFromWait(status);
                job->pids[j] = -1;
            }
        }

        if (!job->stopped) {
            for (size_t j = 0; j < job->pid_count; ++j) {
                if (job->pids[j] > 0) {
                    job_active = true;
                    job->running = true;
                    break;
                }
            }
        }

        bool all_done = true;
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] > 0) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            shellUpdateStatus(job->last_status);
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        if (!job_active && !job->stopped) {
            job->running = true;
        }

        ++i;
    }
    return reaped;
}

static bool shellResolveJobIndex(VM *vm, const char *name, int arg_count, Value *args, size_t *out_index) {
    if (gShellJobCount == 0) {
        runtimeError(vm, "%s: no current job", name);
        return false;
    }
    if (arg_count == 0) {
        *out_index = gShellJobCount - 1;
        return true;
    }
    if (arg_count > 1) {
        runtimeError(vm, "%s: too many arguments", name);
        return false;
    }

    Value spec = args[0];
    if (spec.type == TYPE_STRING && spec.s_val) {
        const char *text = spec.s_val;
        if (text[0] == '%') {
            text++;
        }
        if (*text == '\0') {
            runtimeError(vm, "%s: invalid job spec", name);
            return false;
        }
        char *end = NULL;
        long index = strtol(text, &end, 10);
        if (*end != '\0' || index <= 0 || (size_t)index > gShellJobCount) {
            runtimeError(vm, "%s: invalid job '%s'", name, spec.s_val);
            return false;
        }
        *out_index = (size_t)index - 1;
        return true;
    }

    if (IS_INTLIKE(spec)) {
        long index = (long)AS_INTEGER(spec);
        if (index <= 0 || (size_t)index > gShellJobCount) {
            runtimeError(vm, "%s: invalid job index", name);
            return false;
        }
        *out_index = (size_t)index - 1;
        return true;
    }

    runtimeError(vm, "%s: job spec must be a string or integer", name);
    return false;
}

static void shellParseMetadata(const char *meta, ShellCommand *cmd) {
    if (!meta || !cmd) {
        return;
    }
    char *copy = strdup(meta);
    if (!copy) {
        return;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "bg") == 0) {
                shellParseBool(value, &cmd->background);
            } else if (strcmp(key, "pipe") == 0) {
                cmd->pipeline_index = atoi(value);
            } else if (strcmp(key, "head") == 0) {
                shellParseBool(value, &cmd->is_pipeline_head);
            } else if (strcmp(key, "tail") == 0) {
                shellParseBool(value, &cmd->is_pipeline_tail);
            }
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    free(copy);
}

static bool shellAddArg(ShellCommand *cmd, const char *arg, bool *saw_command_word) {
    if (!cmd || !arg) {
        return false;
    }
    const char *text = NULL;
    const char *meta = NULL;
    size_t meta_len = 0;
    uint8_t flags = 0;
    if (!shellDecodeWordSpec(arg, &text, &flags, &meta, &meta_len)) {
        return false;
    }
    char *expanded = shellExpandWord(text, flags, meta, meta_len);
    if (!expanded) {
        return false;
    }
    if (saw_command_word && !*saw_command_word) {
        if ((flags & SHELL_WORD_FLAG_ASSIGNMENT) && shellLooksLikeAssignment(expanded)) {
            if (!shellCommandAppendAssignmentOwned(cmd, expanded)) {
                return false;
            }
            return true;
        }
    }
    if (shellWordShouldGlob(flags, expanded)) {
        glob_t glob_result;
        int glob_status = glob(expanded, 0, NULL, &glob_result);
        if (glob_status == 0) {
            size_t original_argc = cmd->argc;
            bool ok = true;
            for (size_t i = 0; i < glob_result.gl_pathc; ++i) {
                char *dup = strdup(glob_result.gl_pathv[i]);
                if (!dup) {
                    ok = false;
                    break;
                }
                if (!shellCommandAppendArgOwned(cmd, dup)) {
                    ok = false;
                    break;
                }
            }
            if (!ok) {
                while (cmd->argc > original_argc) {
                    free(cmd->argv[cmd->argc - 1]);
                    cmd->argc--;
                }
                if (cmd->argv) {
                    cmd->argv[cmd->argc] = NULL;
                }
                globfree(&glob_result);
                free(expanded);
                return false;
            }
            globfree(&glob_result);
            free(expanded);
            if (saw_command_word) {
                *saw_command_word = true;
            }
            return true;
        }
        if (glob_status != GLOB_NOMATCH) {
            fprintf(stderr, "exsh: glob failed for '%s'\n", expanded);
        }
    }
    if (!shellCommandAppendArgOwned(cmd, expanded)) {
        return false;
    }
    if (saw_command_word) {
        *saw_command_word = true;
    }
    return true;
}

static int decodeHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

static char *decodeHexString(const char *hex, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!hex || *hex == '\0') {
        if (out_len) {
            *out_len = 0;
        }
        return strdup("");
    }
    size_t len = strlen(hex);
    if ((len & 1) != 0) {
        return NULL;
    }
    size_t decoded_len = len / 2;
    char *decoded = (char *)malloc(decoded_len + 1);
    if (!decoded) {
        return NULL;
    }
    for (size_t i = 0; i < decoded_len; ++i) {
        int high = decodeHexDigit(hex[i * 2]);
        int low = decodeHexDigit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            free(decoded);
            return NULL;
        }
        decoded[i] = (char)((high << 4) | low);
    }
    decoded[decoded_len] = '\0';
    if (out_len) {
        *out_len = decoded_len;
    }
    return decoded;
}

static bool shellAddRedirection(ShellCommand *cmd, const char *spec) {
    if (!cmd || !spec) {
        return false;
    }
    if (strncmp(spec, "redir:", 6) != 0) {
        return false;
    }
    const char *payload = spec + 6;
    char *copy = strdup(payload);
    if (!copy) {
        return false;
    }

    const char *fd_text = "";
    const char *type_text = "";
    const char *word_hex = "";
    const char *here_hex = "";

    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        const char *key = cursor;
        const char *value = "";
        if (eq) {
            *eq = '\0';
            value = eq + 1;
        }
        if (strcmp(key, "fd") == 0) {
            fd_text = value;
        } else if (strcmp(key, "type") == 0) {
            type_text = value;
        } else if (strcmp(key, "word") == 0) {
            word_hex = value;
        } else if (strcmp(key, "here") == 0) {
            here_hex = value;
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }

    if (!type_text || *type_text == '\0') {
        free(copy);
        return false;
    }

    int fd = -1;
    if (fd_text && *fd_text) {
        fd = atoi(fd_text);
    } else if (strcmp(type_text, "<") == 0 || strcmp(type_text, "<<") == 0 || strcmp(type_text, "<&") == 0 || strcmp(type_text, "<>") == 0) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }

    ShellRedirection redir;
    memset(&redir, 0, sizeof(redir));
    redir.fd = fd;

    char *word_encoded = decodeHexString(word_hex, NULL);
    const char *target_spec = (word_encoded && *word_encoded) ? word_encoded : "";
    const char *target_text = NULL;
    const char *target_meta = NULL;
    size_t target_meta_len = 0;
    uint8_t target_flags = 0;
    if (target_spec && *target_spec) {
        shellDecodeWordSpec(target_spec, &target_text, &target_flags, &target_meta, &target_meta_len);
    }

    char *expanded_target = NULL;
    if (strcmp(type_text, "<<") != 0) {
        if (!target_spec || *target_spec == '\0') {
            free(word_encoded);
            free(copy);
            return false;
        }
        expanded_target = shellExpandWord(target_text, target_flags, target_meta, target_meta_len);
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
    }

    if (strcmp(type_text, "<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDONLY;
        redir.mode = 0;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_APPEND;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDWR | O_CREAT;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">|") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<&") == 0 || strcmp(type_text, ">&") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_DUP;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        if (strcmp(expanded_target, "-") == 0) {
            redir.close_target = true;
        } else {
            char *endptr = NULL;
            errno = 0;
            long value = strtol(expanded_target, &endptr, 10);
            if (errno != 0 || !endptr || *endptr != '\0') {
                free(expanded_target);
                free(copy);
                return false;
            }
            redir.dup_target_fd = (int)value;
        }
        free(expanded_target);
        expanded_target = NULL;
    } else if (strcmp(type_text, "<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HEREDOC;
        size_t body_len = 0;
        char *decoded = decodeHexString(here_hex ? here_hex : "", &body_len);
        if (!decoded) {
            free(word_encoded);
            free(copy);
            return false;
        }
        redir.here_doc = decoded;
        redir.here_doc_length = body_len;
    } else {
        free(expanded_target);
        free(word_encoded);
        free(copy);
        return false;
    }

    ShellRedirection *new_redirs = realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 1));
    if (!new_redirs) {
        free(expanded_target);
        free(redir.path);
        free(redir.here_doc);
        free(word_encoded);
        free(copy);
        return false;
    }
    cmd->redirs = new_redirs;
    cmd->redirs[cmd->redir_count++] = redir;
    free(word_encoded);
    free(copy);
    return true;
}

static bool shellBuildCommand(VM *vm, int arg_count, Value *args, ShellCommand *out_cmd) {
    if (!out_cmd) {
        return false;
    }
    memset(out_cmd, 0, sizeof(*out_cmd));
    if (arg_count <= 0) {
        runtimeError(vm, "shell exec: missing metadata argument");
        return false;
    }
    Value meta = args[0];
    if (meta.type != TYPE_STRING || !meta.s_val) {
        runtimeError(vm, "shell exec: metadata must be a string");
        return false;
    }
    shellParseMetadata(meta.s_val, out_cmd);
    bool saw_command_word = false;
    for (int i = 1; i < arg_count; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "shell exec: arguments must be strings");
            shellFreeCommand(out_cmd);
            return false;
        }
        if (strncmp(v.s_val, "redir:", 6) == 0) {
            if (!shellAddRedirection(out_cmd, v.s_val)) {
                runtimeError(vm, "shell exec: invalid redirection '%s'", v.s_val);
                shellFreeCommand(out_cmd);
                return false;
            }
        } else {
            if (!shellAddArg(out_cmd, v.s_val, &saw_command_word)) {
                runtimeError(vm, "shell exec: unable to add argument");
                shellFreeCommand(out_cmd);
                return false;
            }
        }
    }
    return true;
}

typedef enum {
    SHELL_REDIR_OP_OPEN,
    SHELL_REDIR_OP_DUP,
    SHELL_REDIR_OP_HEREDOC
} ShellRuntimeRedirOpType;

typedef struct {
    ShellRuntimeRedirOpType type;
    int target_fd;
    int source_fd;
    int write_fd;
    const char *here_body;
    size_t here_length;
    bool close_target;
} ShellRuntimeRedirOp;

static int shellSpawnProcess(const ShellCommand *cmd,
                             int stdin_fd,
                             int stdout_fd,
                             int stderr_fd,
                             pid_t *child_pid,
                             bool ignore_job_signals) {
    if (!cmd || cmd->argc == 0 || !cmd->argv || !cmd->argv[0] || !child_pid) {
        return EINVAL;
    }

    ShellRuntimeRedirOp local_ops[16];
    memset(local_ops, 0, sizeof(local_ops));
    ShellRuntimeRedirOp *ops = local_ops;
    size_t op_capacity = sizeof(local_ops) / sizeof(local_ops[0]);
    if (cmd->redir_count > op_capacity) {
        ops = (ShellRuntimeRedirOp *)calloc(cmd->redir_count, sizeof(ShellRuntimeRedirOp));
        if (!ops) {
            return ENOMEM;
        }
        op_capacity = cmd->redir_count;
    }

    size_t op_count = 0;
    int prep_error = 0;
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        if (op_count >= op_capacity) {
            prep_error = ENOMEM;
            goto spawn_cleanup;
        }
        const ShellRedirection *redir = &cmd->redirs[i];
        ShellRuntimeRedirOp op;
        memset(&op, 0, sizeof(op));
        op.target_fd = redir->fd;
        switch (redir->kind) {
            case SHELL_RUNTIME_REDIR_OPEN: {
                if (!redir->path) {
                    prep_error = EINVAL;
                    goto spawn_cleanup;
                }
                int fd = open(redir->path, redir->flags, redir->mode);
                if (fd < 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.type = SHELL_REDIR_OP_OPEN;
                op.source_fd = fd;
                break;
            }
            case SHELL_RUNTIME_REDIR_DUP: {
                op.type = SHELL_REDIR_OP_DUP;
                op.close_target = redir->close_target;
                op.source_fd = redir->dup_target_fd;
                if (!op.close_target && op.source_fd < 0) {
                    prep_error = EBADF;
                    goto spawn_cleanup;
                }
                break;
            }
            case SHELL_RUNTIME_REDIR_HEREDOC: {
                int pipefd[2];
                if (pipe(pipefd) != 0) {
                    prep_error = errno;
                    goto spawn_cleanup;
                }
                op.type = SHELL_REDIR_OP_HEREDOC;
                op.source_fd = pipefd[0];
                op.write_fd = pipefd[1];
                op.here_body = redir->here_doc ? redir->here_doc : "";
                op.here_length = redir->here_doc_length;
                break;
            }
            default:
                prep_error = EINVAL;
                goto spawn_cleanup;
        }
        ops[op_count++] = op;
    }

    {
        pid_t child = fork();
        if (child < 0) {
            prep_error = errno;
            goto spawn_cleanup;
        }

        if (child == 0) {
            ShellPipelineContext *ctx = &gShellRuntime.pipeline;
            pid_t desired_pgid = getpid();
            if (ctx->active && ctx->pgid > 0) {
                desired_pgid = ctx->pgid;
            }
            if (setpgid(0, desired_pgid) != 0) {
                /* best-effort; ignore errors */
            }

            if (ignore_job_signals) {
                signal(SIGINT, SIG_IGN);
                signal(SIGQUIT, SIG_IGN);
            } else {
                signal(SIGINT, SIG_DFL);
                signal(SIGQUIT, SIG_DFL);
            }
            signal(SIGTSTP, SIG_DFL);
            signal(SIGTTIN, SIG_DFL);
            signal(SIGTTOU, SIG_DFL);
            signal(SIGCHLD, SIG_DFL);

            if (ctx->active && ctx->pipes) {
                size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
                for (size_t i = 0; i < pipe_count; ++i) {
                    int r = ctx->pipes[i][0];
                    int w = ctx->pipes[i][1];
                    if (r >= 0 && r != stdin_fd && r != stdout_fd && r != stderr_fd) {
                        close(r);
                    }
                    if (w >= 0 && w != stdin_fd && w != stdout_fd && w != stderr_fd) {
                        close(w);
                    }
                }
            }

            if (stdin_fd >= 0 && dup2(stdin_fd, STDIN_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stdin: %s\n", strerror(err));
                _exit(126);
            }
            if (stdout_fd >= 0 && dup2(stdout_fd, STDOUT_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stdout: %s\n", strerror(err));
                _exit(126);
            }
            if (stderr_fd >= 0 && dup2(stderr_fd, STDERR_FILENO) < 0) {
                int err = errno;
                fprintf(stderr, "exsh: failed to setup stderr: %s\n", strerror(err));
                _exit(126);
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                if (op->type == SHELL_REDIR_OP_HEREDOC && op->write_fd >= 0) {
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                switch (op->type) {
                    case SHELL_REDIR_OP_OPEN:
                    case SHELL_REDIR_OP_HEREDOC:
                        if (dup2(op->source_fd, op->target_fd) < 0) {
                            int err = errno;
                            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
                            _exit(126);
                        }
                        break;
                    case SHELL_REDIR_OP_DUP:
                        if (op->close_target) {
                            close(op->target_fd);
                        } else if (dup2(op->source_fd, op->target_fd) < 0) {
                            int err = errno;
                            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
                            _exit(126);
                        }
                        break;
                }
            }

            if (stdin_fd >= 0 && stdin_fd != STDIN_FILENO) {
                close(stdin_fd);
            }
            if (stdout_fd >= 0 && stdout_fd != STDOUT_FILENO && stdout_fd != stderr_fd) {
                close(stdout_fd);
            }
            if (stderr_fd >= 0 && stderr_fd != STDERR_FILENO) {
                close(stderr_fd);
            }

            for (size_t i = 0; i < op_count; ++i) {
                ShellRuntimeRedirOp *op = &ops[i];
                if ((op->type == SHELL_REDIR_OP_OPEN || op->type == SHELL_REDIR_OP_HEREDOC) &&
                    op->source_fd >= 0 && op->source_fd != op->target_fd) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
            }

            execvp(cmd->argv[0], cmd->argv);
            int err = errno;
            fprintf(stderr, "exsh: %s: %s\n", cmd->argv[0], strerror(err));
            _exit((err == ENOENT) ? 127 : 126);
        }
        for (size_t j = 0; j < op_count; ++j) {
            ShellRuntimeRedirOp *op = &ops[j];
            if (op->type == SHELL_REDIR_OP_OPEN) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
            } else if (op->type == SHELL_REDIR_OP_HEREDOC) {
                if (op->source_fd >= 0) {
                    close(op->source_fd);
                    op->source_fd = -1;
                }
                if (op->write_fd >= 0) {
                    const char *body = op->here_body ? op->here_body : "";
                    size_t remaining = op->here_length;
                    const char *cursor = body;
                    while (remaining > 0) {
                        ssize_t written = write(op->write_fd, cursor, remaining);
                        if (written < 0) {
                            if (errno == EINTR) {
                                continue;
                            }
                            break;
                        }
                        cursor += written;
                        remaining -= (size_t)written;
                    }
                    close(op->write_fd);
                    op->write_fd = -1;
                }
            }
        }
        if (ops != local_ops) {
            free(ops);
        }
        *child_pid = child;
        return 0;
    }

spawn_cleanup:
    for (size_t j = 0; j < op_count; ++j) {
        if (ops[j].type == SHELL_REDIR_OP_OPEN || ops[j].type == SHELL_REDIR_OP_HEREDOC) {
            if (ops[j].source_fd >= 0) {
                close(ops[j].source_fd);
                ops[j].source_fd = -1;
            }
        }
        if (ops[j].type == SHELL_REDIR_OP_HEREDOC && ops[j].write_fd >= 0) {
            close(ops[j].write_fd);
            ops[j].write_fd = -1;
        }
    }
    if (ops != local_ops) {
        free(ops);
    }
    return prep_error;
}

static int shellWaitPid(pid_t pid, int *status_out, bool allow_stop, bool *out_stopped) {
    if (out_stopped) {
        *out_stopped = false;
    }
    int status = 0;
    int options = allow_stop ? WUNTRACED : 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, options);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) {
        return errno;
    }
    if (out_stopped && WIFSTOPPED(status)) {
        *out_stopped = true;
    }
    if (status_out) {
        if (WIFEXITED(status)) {
            *status_out = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            *status_out = 128 + WTERMSIG(status);
        } else if (WIFSTOPPED(status)) {
            *status_out = 128 + WSTOPSIG(status);
        } else {
            *status_out = status;
        }
    }
    return 0;
}

static void shellResetPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }
    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) close(ctx->pipes[i][0]);
            if (ctx->pipes[i][1] >= 0) close(ctx->pipes[i][1]);
        }
        free(ctx->pipes);
        ctx->pipes = NULL;
    }
    free(ctx->pids);
    ctx->pids = NULL;
    free(ctx->merge_stderr);
    ctx->merge_stderr = NULL;
    ctx->active = false;
    ctx->stage_count = 0;
    ctx->launched = 0;
    ctx->background = false;
    ctx->last_status = 0;
    ctx->pgid = -1;
}

static void shellAbortPipeline(void) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return;
    }

    if (ctx->pipes) {
        size_t pipe_count = (ctx->stage_count > 0) ? (ctx->stage_count - 1) : 0;
        for (size_t i = 0; i < pipe_count; ++i) {
            if (ctx->pipes[i][0] >= 0) {
                close(ctx->pipes[i][0]);
                ctx->pipes[i][0] = -1;
            }
            if (ctx->pipes[i][1] >= 0) {
                close(ctx->pipes[i][1]);
                ctx->pipes[i][1] = -1;
            }
        }
    }

    for (size_t i = 0; i < ctx->launched; ++i) {
        pid_t pid = ctx->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res = -1;
        do {
            res = waitpid(pid, &status, WNOHANG);
        } while (res < 0 && errno == EINTR);
        if (res == 0) {
            kill(pid, SIGTERM);
            do {
                res = waitpid(pid, &status, 0);
            } while (res < 0 && errno == EINTR);
        }
    }

    shellResetPipeline();
}

static bool shellEnsurePipeline(size_t stages, bool negated) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    shellResetPipeline();
    ctx->stage_count = stages;
    ctx->negated = negated;
    ctx->active = true;
    ctx->launched = 0;
    ctx->last_status = 0;
    ctx->background = false;
    ctx->pgid = -1;
    ctx->pids = calloc(stages, sizeof(pid_t));
    if (!ctx->pids) {
        shellResetPipeline();
        return false;
    }
    if (stages > 0) {
        ctx->merge_stderr = (bool *)calloc(stages, sizeof(bool));
        if (!ctx->merge_stderr) {
            shellResetPipeline();
            return false;
        }
    }
    if (stages > 1) {
        ctx->pipes = calloc(stages - 1, sizeof(int[2]));
        if (!ctx->pipes) {
            shellResetPipeline();
            return false;
        }
        for (size_t i = 0; i < stages - 1; ++i) {
            ctx->pipes[i][0] = -1;
            ctx->pipes[i][1] = -1;
            if (pipe(ctx->pipes[i]) != 0) {
                shellResetPipeline();
                return false;
            }
        }
    }
    return true;
}

static int shellFinishPipeline(const ShellCommand *tail_cmd) {
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    if (!ctx->active) {
        return gShellRuntime.last_status;
    }

    int final_status = ctx->last_status;
    pid_t job_pgid = (ctx->pgid > 0) ? ctx->pgid : ((ctx->launched > 0) ? ctx->pids[0] : -1);

    if (!ctx->background) {
        shellEnsureJobControl();
        bool job_control = gShellRuntime.job_control_enabled && job_pgid > 0;
        bool stopped_job = false;

        if (job_control) {
            shellJobControlSetForeground(job_pgid);
        }

        for (size_t i = 0; i < ctx->launched; ++i) {
            pid_t pid = ctx->pids[i];
            if (pid <= 0) {
                continue;
            }
            bool stopped = false;
            int status = 0;
            int err = shellWaitPid(pid, &status, job_control, &stopped);
            if (err != 0) {
                continue;
            }
            if (stopped) {
                stopped_job = true;
                final_status = status;
            } else {
                final_status = status;
                ctx->pids[i] = -1;
            }
        }

        if (job_control) {
            shellJobControlRestoreForeground();
        }

        shellRuntimeProcessPendingSignals();

        if (stopped_job && job_control) {
            ShellJob *job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
            if (job) {
                job->stopped = true;
                job->running = false;
                job->last_status = final_status;
            }
            ctx->last_status = final_status;
            shellResetPipeline();
            shellUpdateStatus(final_status);
            return final_status;
        }
    } else if (ctx->launched > 0) {
        ShellJob *job = shellRegisterJob(job_pgid, ctx->pids, ctx->launched, tail_cmd);
        if (job) {
            job->running = true;
            job->stopped = false;
            job->last_status = 0;
        }
        final_status = 0;
    }

    if (ctx->negated) {
        final_status = (final_status == 0) ? 1 : 0;
    }

    ctx->last_status = final_status;
    shellResetPipeline();
    shellUpdateStatus(final_status);
    return final_status;
}

static Value shellExecuteCommand(VM *vm, ShellCommand *cmd) {
    if (!cmd) {
        return makeVoid();
    }
    shellRuntimeProcessPendingSignals();
    if (shellLoopSkipActive()) {
        shellFreeCommand(cmd);
        return makeVoid();
    }
    ShellPipelineContext *ctx = &gShellRuntime.pipeline;
    ShellAssignmentBackup *assignment_backups = NULL;
    size_t assignment_backup_count = 0;
    bool assignments_applied = false;

    if (cmd->argc == 0) {
        const char *failed_assignment = NULL;
        bool invalid_assignment = false;
        if (cmd->assignment_count > 0) {
            if (!shellApplyAssignmentsPermanently(cmd, &failed_assignment, &invalid_assignment)) {
                if (invalid_assignment) {
                    runtimeError(vm, "shell exec: invalid assignment '%s'",
                                 failed_assignment ? failed_assignment : "<assignment>");
                    shellUpdateStatus(1);
                } else {
                    runtimeError(vm, "shell exec: failed to apply assignment '%s': %s",
                                 failed_assignment ? failed_assignment : "<assignment>",
                                 strerror(errno));
                    shellUpdateStatus(errno ? errno : 1);
                }
            } else {
                shellUpdateStatus(0);
            }
        } else {
            shellUpdateStatus(0);
        }
        if (ctx->active) {
            int status = gShellRuntime.last_status;
            if (ctx->stage_count <= 1 && ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            if (ctx->stage_count <= 1) {
                shellResetPipeline();
            }
        }
        shellFreeCommand(cmd);
        return makeVoid();
    }

    const char *failed_assignment = NULL;
    bool invalid_assignment = false;
    if (cmd->assignment_count > 0) {
        if (!shellApplyAssignmentsTemporary(cmd, &assignment_backups, &assignment_backup_count,
                                            &failed_assignment, &invalid_assignment)) {
            if (invalid_assignment) {
                runtimeError(vm, "shell exec: invalid assignment '%s'",
                             failed_assignment ? failed_assignment : "<assignment>");
                shellUpdateStatus(1);
            } else {
                runtimeError(vm, "shell exec: failed to apply assignment '%s': %s",
                             failed_assignment ? failed_assignment : "<assignment>",
                             strerror(errno));
                shellUpdateStatus(errno ? errno : 1);
            }
            if (ctx->active) {
                shellAbortPipeline();
            }
            shellFreeCommand(cmd);
            return makeVoid();
        }
        assignments_applied = true;
    }

    int stdin_fd = -1;
    int stdout_fd = -1;
    int stderr_fd = -1;
    if (ctx->active) {
        if (ctx->stage_count == 1 && shellInvokeBuiltin(vm, cmd)) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            int status = gShellRuntime.last_status;
            if (ctx->negated) {
                status = (status == 0) ? 1 : 0;
                shellUpdateStatus(status);
            }
            ctx->last_status = status;
            shellResetPipeline();
            shellFreeCommand(cmd);
            return makeVoid();
        }
        size_t idx = (size_t)cmd->pipeline_index;
        if (idx >= ctx->stage_count) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
            }
            assignment_backups = NULL;
            assignment_backup_count = 0;
            runtimeError(vm, "shell exec: pipeline index out of range");
            shellFreeCommand(cmd);
            shellResetPipeline();
            return makeVoid();
        }
        if (ctx->stage_count > 1) {
            if (!cmd->is_pipeline_head) {
                stdin_fd = ctx->pipes[idx - 1][0];
            }
            if (!cmd->is_pipeline_tail) {
                stdout_fd = ctx->pipes[idx][1];
            }
        }
        if (ctx->merge_stderr && idx < ctx->stage_count && ctx->merge_stderr[idx]) {
            stderr_fd = stdout_fd;
        }
    } else {
        if (shellInvokeBuiltin(vm, cmd)) {
            if (assignments_applied) {
                shellRestoreAssignments(assignment_backups, assignment_backup_count);
                assignments_applied = false;
                assignment_backups = NULL;
                assignment_backup_count = 0;
            }
            shellFreeCommand(cmd);
            return makeVoid();
        }
    }

    bool background_execution = cmd->background;
    if (ctx->active) {
        if (ctx->background) {
            background_execution = true;
        }
        if (cmd->background) {
            ctx->background = true;
        }
    }

    pid_t child = -1;
    int spawn_err = shellSpawnProcess(cmd,
                                      stdin_fd,
                                      stdout_fd,
                                      stderr_fd,
                                      &child,
                                      background_execution && !gShellRuntime.job_control_enabled);
    if (assignments_applied) {
        shellRestoreAssignments(assignment_backups, assignment_backup_count);
        assignments_applied = false;
        assignment_backups = NULL;
        assignment_backup_count = 0;
    }
    if (spawn_err != 0) {
        runtimeError(vm, "shell exec: failed to spawn '%s': %s", cmd->argv[0], strerror(spawn_err));
        if (ctx->active) {
            shellAbortPipeline();
        }
        shellFreeCommand(cmd);
        shellUpdateStatus(127);
        return makeVoid();
    }

    if (ctx->active) {
        pid_t target_pgid = (ctx->pgid > 0) ? ctx->pgid : child;
        if (setpgid(child, target_pgid) != 0) {
            if (errno != EACCES && errno != ESRCH) {
                /* best-effort: ignore errors from lack of job control */
            }
        }
        if (ctx->pgid <= 0) {
            ctx->pgid = target_pgid;
        }
    } else {
        if (setpgid(child, child) != 0) {
            if (errno != EACCES && errno != ESRCH) {
                /* ignore */
            }
        }
    }

    if (ctx->active) {
        if (!cmd->is_pipeline_head && stdin_fd >= 0) {
            close(stdin_fd);
            if (cmd->pipeline_index > 0) {
                ctx->pipes[cmd->pipeline_index - 1][0] = -1;
            }
        }
        if (!cmd->is_pipeline_tail && stdout_fd >= 0) {
            close(stdout_fd);
            ctx->pipes[cmd->pipeline_index][1] = -1;
        }
        ctx->pids[ctx->launched++] = child;
        if (cmd->is_pipeline_tail) {
            ctx->background = cmd->background;
            shellFinishPipeline(cmd);
            shellRuntimeProcessPendingSignals();
        }
    } else {
        int status = 0;
        if (!cmd->background) {
            shellEnsureJobControl();
            bool job_control = gShellRuntime.job_control_enabled;
            bool stopped = false;
            if (job_control) {
                shellJobControlSetForeground(child);
            }
            shellWaitPid(child, &status, job_control, &stopped);
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            shellRuntimeProcessPendingSignals();
            if (stopped && job_control) {
                pid_t job_pids[1];
                job_pids[0] = child;
                ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
                if (job) {
                    job->stopped = true;
                    job->running = false;
                    job->last_status = status;
                }
                shellUpdateStatus(status);
                shellFreeCommand(cmd);
                return makeVoid();
            }
        } else {
            pid_t job_pids[1];
            job_pids[0] = child;
            ShellJob *job = shellRegisterJob(child, job_pids, 1, cmd);
            if (job) {
                job->running = true;
                job->stopped = false;
                job->last_status = 0;
            }
            status = 0;
        }
        shellUpdateStatus(status);
        shellRuntimeProcessPendingSignals();
    }

    shellFreeCommand(cmd);
    return makeVoid();
}

Value vmBuiltinShellExec(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    shellCollectJobs();
    ShellCommand cmd;
    if (!shellBuildCommand(vm, arg_count, args, &cmd)) {
        goto cleanup;
    }
    result = shellExecuteCommand(vm, &cmd);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellPipeline(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "shell pipeline: expected metadata string");
        goto cleanup;
    }
    if (shellLoopSkipActive()) {
        goto cleanup;
    }
    const char *meta = args[0].s_val;
    size_t stages = 0;
    bool negated = false;
    char *merge_pattern = NULL;
    char *copy = strdup(meta);
    if (!copy) {
        runtimeError(vm, "shell pipeline: out of memory");
        goto cleanup;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) *next = '\0';
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "stages") == 0) {
                stages = (size_t)strtoul(value, NULL, 10);
            } else if (strcmp(key, "negated") == 0) {
                shellParseBool(value, &negated);
            } else if (strcmp(key, "merge") == 0) {
                free(merge_pattern);
                merge_pattern = strdup(value ? value : "");
            }
        }
        if (!next) break;
        cursor = next + 1;
    }
    free(copy);
    if (stages == 0) {
        runtimeError(vm, "shell pipeline: invalid stage count");
        free(merge_pattern);
        goto cleanup;
    }
    if (!shellEnsurePipeline(stages, negated)) {
        runtimeError(vm, "shell pipeline: unable to allocate context");
        free(merge_pattern);
        goto cleanup;
    }

    if (merge_pattern) {
        ShellPipelineContext *ctx = &gShellRuntime.pipeline;
        size_t pattern_len = strlen(merge_pattern);
        for (size_t i = 0; i < stages; ++i) {
            bool merge = false;
            if (i < pattern_len) {
                merge = (merge_pattern[i] == '1');
            }
            if (ctx->merge_stderr && i < stages) {
                ctx->merge_stderr[i] = merge;
            }
        }
        free(merge_pattern);
        merge_pattern = NULL;
    }

cleanup:
    free(merge_pattern);
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellAnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status != 0) {
        shellUpdateStatus(gShellRuntime.last_status);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellOr(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    if (gShellRuntime.last_status == 0) {
        shellUpdateStatus(0);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellSubshell(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    shellResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellLoop(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    bool parent_skip = shellLoopSkipActive();
    ShellLoopFrame *frame = shellLoopPushFrame();
    if (!frame) {
        runtimeError(vm, "shell loop: out of memory");
        shellRestoreCurrentVm(previous_vm);
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (parent_skip) {
        frame->skip_body = true;
    }
    shellResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellLoopEnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    ShellLoopFrame *frame = shellLoopTop();
    if (frame) {
        bool propagate_continue = frame->continue_pending;
        bool propagate_break = frame->break_pending;
        shellLoopPopFrame();
        if (gShellLoopStackSize == 0) {
            if (propagate_break) {
                gShellRuntime.break_requested = false;
                gShellRuntime.break_requested_levels = 0;
            }
            if (propagate_continue) {
                gShellRuntime.continue_requested = false;
                gShellRuntime.continue_requested_levels = 0;
            }
        }
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellIf(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    shellResetPipeline();
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellCase(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 2 || args[1].type != TYPE_STRING || !args[1].s_val) {
        runtimeError(vm, "shell case: expected metadata and subject word");
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *subject_spec = args[1].s_val;
    const char *subject_text = subject_spec;
    const char *subject_meta = NULL;
    size_t subject_meta_len = 0;
    uint8_t subject_flags = 0;
    if (!shellDecodeWordSpec(subject_spec, &subject_text, &subject_flags, &subject_meta, &subject_meta_len)) {
        subject_text = subject_spec ? subject_spec : "";
        subject_flags = 0;
    }
    char *expanded_subject = shellExpandWord(subject_text, subject_flags, subject_meta, subject_meta_len);
    if (!expanded_subject) {
        runtimeError(vm, "shell case: out of memory");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (!shellCaseStackPush(expanded_subject)) {
        free(expanded_subject);
        runtimeError(vm, "shell case: out of memory");
        shellUpdateStatus(1);
        goto cleanup;
    }
    free(expanded_subject);
    shellUpdateStatus(1);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseClause(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count < 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "shell case clause: expected metadata");
        shellUpdateStatus(1);
        goto cleanup;
    }
    ShellCaseContext *ctx = shellCaseStackTop();
    if (!ctx) {
        runtimeError(vm, "shell case clause: no active case");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (ctx->matched) {
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *subject = ctx->subject ? ctx->subject : "";
    bool matched = false;
    for (int i = 1; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            continue;
        }
        const char *pattern_spec = args[i].s_val;
        const char *pattern_text = pattern_spec;
        const char *pattern_meta = NULL;
        size_t pattern_meta_len = 0;
        uint8_t pattern_flags = 0;
        if (!shellDecodeWordSpec(pattern_spec, &pattern_text, &pattern_flags, &pattern_meta, &pattern_meta_len)) {
            pattern_text = pattern_spec ? pattern_spec : "";
            pattern_flags = 0;
        }
        char *expanded_pattern = shellExpandWord(pattern_text, pattern_flags, pattern_meta, pattern_meta_len);
        if (!expanded_pattern) {
            runtimeError(vm, "shell case clause: out of memory");
            shellUpdateStatus(1);
            goto cleanup;
        }
        if (shellWordShouldGlob(pattern_flags, expanded_pattern)) {
            if (fnmatch(expanded_pattern, subject, 0) == 0) {
                free(expanded_pattern);
                matched = true;
                break;
            }
        } else {
            if (strcmp(expanded_pattern, subject) == 0) {
                free(expanded_pattern);
                matched = true;
                break;
            }
        }
        free(expanded_pattern);
    }
    if (matched) {
        ctx->matched = true;
        shellUpdateStatus(0);
    } else {
        shellUpdateStatus(1);
    }

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCaseEnd(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    (void)arg_count;
    (void)args;
    ShellCaseContext *ctx = shellCaseStackTop();
    if (!ctx) {
        runtimeError(vm, "shell case end: no active case");
        shellUpdateStatus(1);
        shellRestoreCurrentVm(previous_vm);
        return makeVoid();
    }
    bool matched = ctx->matched;
    shellCaseStackPop();
    if (!matched) {
        shellUpdateStatus(1);
    }
    shellRestoreCurrentVm(previous_vm);
    return makeVoid();
}

Value vmBuiltinShellDefineFunction(VM *vm, int arg_count, Value *args) {
    VM *previous_vm = shellSwapCurrentVm(vm);
    Value result = makeVoid();
    if (arg_count != 3) {
        runtimeError(vm, "shell define function: expected name, parameters, and body");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "shell define function: name must be a non-empty string");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (args[1].type != TYPE_STRING && args[1].type != TYPE_VOID && args[1].type != TYPE_NIL) {
        runtimeError(vm, "shell define function: parameter metadata must be a string");
        shellUpdateStatus(1);
        goto cleanup;
    }
    if (args[2].type != TYPE_POINTER || !args[2].ptr_val) {
        runtimeError(vm, "shell define function: missing compiled body");
        shellUpdateStatus(1);
        goto cleanup;
    }
    const char *name = args[0].s_val;
    const char *param_meta = NULL;
    if (args[1].type == TYPE_STRING && args[1].s_val) {
        param_meta = args[1].s_val;
    }
    ShellCompiledFunction *compiled = (ShellCompiledFunction *)args[2].ptr_val;
    if (!shellStoreFunction(name, param_meta, compiled)) {
        shellDisposeCompiledFunction(compiled);
        runtimeError(vm, "shell define function: failed to store '%s'", name);
        shellUpdateStatus(1);
        goto cleanup;
    }
    args[2].ptr_val = NULL;
    shellUpdateStatus(0);

cleanup:
    shellRestoreCurrentVm(previous_vm);
    return result;
}

Value vmBuiltinShellCd(VM *vm, int arg_count, Value *args) {
    const char *path = NULL;
    if (arg_count == 0) {
        path = getenv("HOME");
        if (!path) {
            runtimeError(vm, "cd: HOME not set");
            shellUpdateStatus(1);
            return makeVoid();
        }
    } else if (args[0].type == TYPE_STRING && args[0].s_val) {
        path = args[0].s_val;
    } else {
        runtimeError(vm, "cd: expected directory path");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (chdir(path) != 0) {
        runtimeError(vm, "cd: %s", strerror(errno));
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd))) {
        setenv("PWD", cwd, 1);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellPwd(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) {
        runtimeError(vm, "pwd: %s", strerror(errno));
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }
    printf("%s\n", cwd);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellColon(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellSource(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "source: expected path to script");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *path = args[0].s_val;
    char *source = shellLoadFile(path);
    if (!source) {
        runtimeError(vm, "source: unable to read '%s'", path);
        shellUpdateStatus(errno ? errno : 1);
        return makeVoid();
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;

    int new_count = (arg_count > 1) ? (arg_count - 1) : 0;
    char **new_params = NULL;
    bool replaced_params = false;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            runtimeError(vm, "source: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < new_count; ++i) {
            if (args[i + 1].type != TYPE_STRING || !args[i + 1].s_val) {
                for (int j = 0; j < i; ++j) {
                    free(new_params[j]);
                }
                free(new_params);
                free(source);
                runtimeError(vm, "source: arguments must be strings");
                shellUpdateStatus(1);
                return makeVoid();
            }
            new_params[i] = strdup(args[i + 1].s_val);
            if (!new_params[i]) {
                for (int j = 0; j < i; ++j) {
                    free(new_params[j]);
                }
                free(new_params);
                free(source);
                runtimeError(vm, "source: out of memory");
                shellUpdateStatus(1);
                return makeVoid();
            }
        }
        gParamValues = new_params;
        gParamCount = new_count;
        replaced_params = true;
    }

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    const char *frontend_path = shellRuntimeGetArg0();
    opts.frontend_path = frontend_path ? frontend_path : "exsh";

    bool exit_requested = false;
    int status = shellRunSource(source, path, &opts, &exit_requested);
    free(source);

    if (new_params) {
        for (int i = 0; i < new_count; ++i) {
            free(new_params[i]);
        }
        free(new_params);
    }

    if (replaced_params) {
        gParamValues = saved_params;
        gParamCount = saved_count;
    }

    if (exit_requested) {
        gShellExitRequested = true;
        if (vm) {
            vm->exit_requested = true;
        }
    }

    shellUpdateStatus(status);
    return makeVoid();
}

Value vmBuiltinShellExit(VM *vm, int arg_count, Value *args) {
    int code = 0;
    if (arg_count >= 1 && IS_INTLIKE(args[0])) {
        code = (int)AS_INTEGER(args[0]);
    }
    shellUpdateStatus(code);
    gShellExitRequested = true;
    vm->exit_requested = true;
    vm->current_builtin_name = "exit";
    return makeVoid();
}

Value vmBuiltinShellSetenv(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        if (environ) {
            for (char **env = environ; *env; ++env) {
                puts(*env);
            }
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "setenv: expected NAME [VALUE]");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "setenv: variable name must be a non-empty string");
        shellUpdateStatus(1);
        return makeVoid();
    }
    if (strchr(args[0].s_val, '=')) {
        runtimeError(vm, "setenv: variable name must not contain '='");
        shellUpdateStatus(1);
        return makeVoid();
    }
    const char *value = "";
    if (arg_count > 1) {
        if (args[1].type != TYPE_STRING || !args[1].s_val) {
            runtimeError(vm, "setenv: value must be a string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        value = args[1].s_val;
    }
    if (setenv(args[0].s_val, value, 1) != 0) {
        runtimeError(vm, "setenv: unable to set '%s': %s", args[0].s_val, strerror(errno));
        shellUpdateStatus(1);
        return makeVoid();
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellExport(VM *vm, int arg_count, Value *args) {
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "export: expected name=value string");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *assignment = args[i].s_val;
        const char *eq = strchr(assignment, '=');
        if (!eq || eq == assignment) {
            runtimeError(vm, "export: invalid assignment '%s'", assignment);
            shellUpdateStatus(1);
            return makeVoid();
        }
        size_t name_len = (size_t)(eq - assignment);
        char *name = strndup(assignment, name_len);
        const char *value = eq + 1;
        if (!name) {
            runtimeError(vm, "export: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        setenv(name, value, 1);
        free(name);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellUnset(VM *vm, int arg_count, Value *args) {
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "unset: expected variable name");
            shellUpdateStatus(1);
            return makeVoid();
        }
        unsetenv(args[i].s_val);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellUnsetenv(VM *vm, int arg_count, Value *args) {
    return vmBuiltinShellUnset(vm, arg_count, args);
}

static bool shellParseLoopLevel(const char *text, int *out_level) {
    if (!text || !out_level) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value <= 0 || value > INT_MAX) {
        return false;
    }
    *out_level = (int)value;
    return true;
}

Value vmBuiltinShellSet(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    bool parsing_options = true;
    for (int i = 0; i < arg_count && ok; ++i) {
        Value v = args[i];
        if (v.type != TYPE_STRING || !v.s_val) {
            runtimeError(vm, "set: expected string argument");
            ok = false;
            break;
        }
        const char *token = v.s_val;
        if (parsing_options && strcmp(token, "--") == 0) {
            parsing_options = false;
            continue;
        }
        if (!parsing_options) {
            continue;
        }
        if (strcmp(token, "-e") == 0) {
            gShellRuntime.errexit_enabled = true;
            gShellRuntime.errexit_pending = false;
        } else if (strcmp(token, "+e") == 0) {
            gShellRuntime.errexit_enabled = false;
            gShellRuntime.errexit_pending = false;
        } else if ((strcmp(token, "-o") == 0 || strcmp(token, "+o") == 0)) {
            bool enable = (token[0] == '-');
            if (i + 1 >= arg_count) {
                runtimeError(vm, "set: missing option name for %s", token);
                ok = false;
                break;
            }
            Value name_val = args[++i];
            if (name_val.type != TYPE_STRING || !name_val.s_val) {
                runtimeError(vm, "set: option name must be a string");
                ok = false;
                break;
            }
            if (strcasecmp(name_val.s_val, "errexit") == 0) {
                gShellRuntime.errexit_enabled = enable;
                if (!enable) {
                    gShellRuntime.errexit_pending = false;
                }
            }
        }
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellTrap(VM *vm, int arg_count, Value *args) {
    bool ok = true;
    if (arg_count == 0) {
        gShellRuntime.trap_enabled = false;
    } else {
        for (int i = 0; i < arg_count; ++i) {
            if (args[i].type != TYPE_STRING || !args[i].s_val) {
                runtimeError(vm, "trap: expected string arguments");
                ok = false;
                break;
            }
        }
        if (ok) {
            gShellRuntime.trap_enabled = true;
        }
    }
    shellUpdateStatus(ok ? 0 : 1);
    return makeVoid();
}

Value vmBuiltinShellLocal(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    gShellRuntime.local_scope_active = true;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellBreak(VM *vm, int arg_count, Value *args) {
    int levels = 1;
    if (arg_count > 0) {
        if (args[0].type != TYPE_STRING || !args[0].s_val ||
            !shellParseLoopLevel(args[0].s_val, &levels)) {
            runtimeError(vm, "break: expected positive integer");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }
    gShellRuntime.break_requested = true;
    gShellRuntime.break_requested_levels = levels;
    shellLoopRequestBreakLevels(levels);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellContinue(VM *vm, int arg_count, Value *args) {
    int levels = 1;
    if (arg_count > 0) {
        if (args[0].type != TYPE_STRING || !args[0].s_val ||
            !shellParseLoopLevel(args[0].s_val, &levels)) {
            runtimeError(vm, "continue: expected positive integer");
            shellUpdateStatus(1);
            return makeVoid();
        }
    }
    gShellRuntime.continue_requested = true;
    gShellRuntime.continue_requested_levels = levels;
    shellLoopRequestContinueLevels(levels);
    shellUpdateStatus(0);
    return makeVoid();
}

typedef struct {
    char *name;
    char *value;
} ShellAlias;

static ShellAlias *gShellAliases = NULL;
static size_t gShellAliasCount = 0;

static ShellAlias *shellFindAlias(const char *name) {
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < gShellAliasCount; ++i) {
        if (strcmp(gShellAliases[i].name, name) == 0) {
            return &gShellAliases[i];
        }
    }
    return NULL;
}

static bool shellSetAlias(const char *name, const char *value) {
    if (!name || !value) {
        return false;
    }
    ShellAlias *existing = shellFindAlias(name);
    if (existing) {
        char *copy = strdup(value);
        if (!copy) {
            return false;
        }
        free(existing->value);
        existing->value = copy;
        return true;
    }
    ShellAlias *new_aliases = realloc(gShellAliases, sizeof(ShellAlias) * (gShellAliasCount + 1));
    if (!new_aliases) {
        return false;
    }
    gShellAliases = new_aliases;
    ShellAlias *alias = &gShellAliases[gShellAliasCount++];
    alias->name = strdup(name);
    alias->value = strdup(value);
    if (!alias->name || !alias->value) {
        free(alias->name);
        free(alias->value);
        gShellAliasCount--;
        return false;
    }
    return true;
}

Value vmBuiltinShellAlias(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        for (size_t i = 0; i < gShellAliasCount; ++i) {
            printf("alias %s='%s'\n", gShellAliases[i].name, gShellAliases[i].value);
        }
        shellUpdateStatus(0);
        return makeVoid();
    }
    for (int i = 0; i < arg_count; ++i) {
        if (args[i].type != TYPE_STRING || !args[i].s_val) {
            runtimeError(vm, "alias: expected name=value");
            shellUpdateStatus(1);
            return makeVoid();
        }
        const char *assignment = args[i].s_val;
        const char *eq = strchr(assignment, '=');
        if (!eq || eq == assignment) {
            runtimeError(vm, "alias: invalid assignment '%s'", assignment);
            shellUpdateStatus(1);
            return makeVoid();
        }
        size_t name_len = (size_t)(eq - assignment);
        char *name = strndup(assignment, name_len);
        const char *value = eq + 1;
        if (!name) {
            runtimeError(vm, "alias: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        if (!shellSetAlias(name, value)) {
            free(name);
            runtimeError(vm, "alias: failed to store alias");
            shellUpdateStatus(1);
            return makeVoid();
        }
        free(name);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellHistory(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    for (size_t i = 0; i < gShellHistory.count; ++i) {
        printf("%zu  %s\n", i + 1, gShellHistory.entries[i]);
    }
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellJobs(VM *vm, int arg_count, Value *args) {
    (void)vm;
    (void)arg_count;
    (void)args;
    shellCollectJobs();
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        const char *state = job->stopped ? "Stopped" : "Running";
        const char *command = job->command ? job->command : "";
        printf("[%zu] %s %s\n", i + 1, state, command);
    }
    fflush(stdout);
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellFg(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "fg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    shellEnsureJobControl();
    bool job_control = gShellRuntime.job_control_enabled && job->pgid > 0;
    if (job_control) {
        shellJobControlSetForeground(job->pgid);
    }
    if (job->pgid > 0) {
        if (kill(-job->pgid, SIGCONT) != 0 && errno != ESRCH) {
            /* ignore */
        }
    } else {
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid > 0) {
                kill(pid, SIGCONT);
            }
        }
    }
    job->stopped = false;
    job->running = true;
    int final_status = job->last_status;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res;
        do {
            res = waitpid(pid, &status, WUNTRACED);
        } while (res < 0 && errno == EINTR);
        if (res < 0) {
            continue;
        }
        if (WIFSTOPPED(status)) {
            job->stopped = true;
            job->running = false;
            job->last_status = shellStatusFromWait(status);
            if (job_control) {
                shellJobControlRestoreForeground();
            }
            shellUpdateStatus(job->last_status);
            return makeVoid();
        }
        final_status = shellStatusFromWait(status);
        job->pids[i] = -1;
    }
    if (job_control) {
        shellJobControlRestoreForeground();
    }
    shellRemoveJobAt(index);
    shellUpdateStatus(final_status);
    return makeVoid();
}

Value vmBuiltinShellBg(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "bg", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    if (job->pgid > 0) {
        if (kill(-job->pgid, SIGCONT) != 0 && errno != ESRCH) {
            /* ignore */
        }
    } else {
        for (size_t i = 0; i < job->pid_count; ++i) {
            pid_t pid = job->pids[i];
            if (pid > 0) {
                kill(pid, SIGCONT);
            }
        }
    }
    job->stopped = false;
    job->running = true;
    shellUpdateStatus(0);
    return makeVoid();
}

Value vmBuiltinShellWait(VM *vm, int arg_count, Value *args) {
    shellCollectJobs();
    if (gShellJobCount == 0) {
        shellUpdateStatus(0);
        return makeVoid();
    }
    size_t index = 0;
    if (!shellResolveJobIndex(vm, "wait", arg_count, args, &index)) {
        shellUpdateStatus(1);
        return makeVoid();
    }
    ShellJob *job = &gShellJobs[index];
    int final_status = job->last_status;
    for (size_t i = 0; i < job->pid_count; ++i) {
        pid_t pid = job->pids[i];
        if (pid <= 0) {
            continue;
        }
        int status = 0;
        pid_t res;
        do {
            res = waitpid(pid, &status, 0);
        } while (res < 0 && errno == EINTR);
        if (res < 0) {
            continue;
        }
        final_status = shellStatusFromWait(status);
        job->pids[i] = -1;
    }
    shellRemoveJobAt(index);
    shellUpdateStatus(final_status);
    return makeVoid();
}

Value vmBuiltinShellBuiltin(VM *vm, int arg_count, Value *args) {
    if (arg_count < 1 || args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "builtin: expected VM builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *name = args[0].s_val;
    VmBuiltinFn handler = getVmBuiltinHandler(name);
    if (!handler) {
        runtimeError(vm, "builtin: unknown VM builtin '%s'", name);
        shellUpdateStatus(1);
        return makeVoid();
    }

    int call_argc = arg_count - 1;
    Value *call_args = NULL;
    if (call_argc > 0) {
        call_args = (Value *)calloc((size_t)call_argc, sizeof(Value));
        if (!call_args) {
            runtimeError(vm, "builtin: out of memory");
            shellUpdateStatus(1);
            return makeVoid();
        }
        for (int i = 0; i < call_argc; ++i) {
            Value src = args[i + 1];
            if (src.type == TYPE_STRING && src.s_val) {
                call_args[i] = shellConvertBuiltinArgument(src.s_val);
            } else if (src.type == TYPE_NIL) {
                call_args[i] = makeNil();
            } else {
                call_args[i] = makeString("");
            }
        }
    }

    int previous_status = shellRuntimeLastStatus();
    Value result = handler(vm, call_argc, call_args);

    if (call_args) {
        for (int i = 0; i < call_argc; ++i) {
            freeValue(&call_args[i]);
        }
        free(call_args);
    }

    if (vm && vm->abort_requested && shellRuntimeLastStatus() == previous_status) {
        shellUpdateStatus(1);
    }

    int status = shellRuntimeLastStatus();

    if (status == 0 && result.type != TYPE_VOID) {
        printValueToStream(result, stdout);
        fputc('\n', stdout);
    }

    freeValue(&result);
    return makeVoid();
}

Value vmHostShellLastStatus(VM *vm) {
    (void)vm;
    return makeInt(gShellRuntime.last_status);
}

Value vmHostShellLoopShouldBreak(VM *vm) {
    (void)vm;
    shellRuntimeProcessPendingSignals();
    ShellLoopFrame *frame = shellLoopTop();
    bool should_break = frame && frame->break_pending;
    return makeBoolean(should_break);
}

Value vmHostShellPollJobs(VM *vm) {
    (void)vm;
    return makeInt(shellCollectJobs());
}

bool shellRuntimeConsumeExitRequested(void) {
    bool requested = gShellExitRequested;
    gShellExitRequested = false;
    return requested;
}

int shellRuntimeLastStatus(void) {
    return gShellRuntime.last_status;
}
