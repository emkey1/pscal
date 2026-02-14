#include "shell/runner.h"

#include "compiler/bytecode.h"
#include "core/cache.h"
#include "core/preproc.h"
#include "shell/builtins.h"
#include "shell/codegen.h"
#include "shell/opt.h"
#include "shell/parser.h"
#include "shell/semantics.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include "vm/vm.h"
#include "Pascal/globals.h"
#include "common/path_virtualization.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#if defined(PSCAL_TARGET_IOS)
#include <spawn.h>
#include <sys/wait.h>
#include "ios/vproc.h"
extern char **environ;
#endif

#if defined(PSCAL_TARGET_IOS)
static void shellRunnerFreeParamArray(char **values, int count) {
    if (!values) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(values[i]);
    }
    free(values);
}
#endif

static const char *const kShellCompilerId = "shell";

#if defined(PSCAL_TARGET_IOS)
extern void pscalRuntimeDebugLog(const char *message);

/* The Swift-side logger allocates, which can trip guard allocators if some
 * other component has previously scribbled on the heap. To keep the shell
 * usable even when optional diagnostics are enabled (MallocScribble, guard
 * malloc, etc.), we make runtime debug logging opt‑in on iOS.
 *
 * Enable by setting PSCALI_RUNTIME_DEBUG=1 in the environment. */
static bool runtimeDebugLogEnabled(void) {
    static bool initialized = false;
    static bool enabled = false;
    if (!initialized) {
        const char *env = getenv("PSCALI_RUNTIME_DEBUG");
        enabled = (env && *env && strcmp(env, "0") != 0);
        initialized = true;
    }
    return enabled;
}

static void runtimeDebugLog(const char *message) {
    if (!message) {
        return;
    }
    if (!runtimeDebugLogEnabled()) {
        return;
    }
    if (&pscalRuntimeDebugLog != NULL) {
        pscalRuntimeDebugLog(message);
    }
}
#else
static bool g_shell_debug_enabled = false;
static bool g_shell_debug_inited = false;

static void runtimeDebugLog(const char *message) {
    if (!g_shell_debug_inited) {
        const char *env = getenv("PSCAL_SHELL_DEBUG");
        g_shell_debug_enabled = (env && *env && strcmp(env, "0") != 0);
        g_shell_debug_inited = true;
    }
    if (message && g_shell_debug_enabled) {
        fprintf(stderr, "%s\n", message);
    }
}
#endif

static PSCAL_THREAD_LOCAL int gShellSymbolTableDepth = 0;
static PSCAL_THREAD_LOCAL VM *gShellThreadOwnerVm = NULL;

#if defined(PSCAL_TARGET_IOS)
static bool shellReadShebangLine(const char *path, char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (!path || !out_line) {
        return false;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return false;
    }
    char buffer[512];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0) {
        return false;
    }
    buffer[n] = '\0';
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t offset = 0;
    if (n >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        offset = 3;
    }
    if (buffer[offset] != '#' || buffer[offset + 1] != '!') {
        return false;
    }
    const char *line_start = buffer + offset + 2;
    while (*line_start == ' ' || *line_start == '\t') {
        line_start++;
    }
    const char *line_end = line_start;
    while (*line_end && *line_end != '\n' && *line_end != '\r') {
        line_end++;
    }
    size_t line_len = (size_t)(line_end - line_start);
    char *line = (char *)malloc(line_len + 1);
    if (!line) {
        return false;
    }
    memcpy(line, line_start, line_len);
    line[line_len] = '\0';
    *out_line = line;
    return true;
}

static int shellRunExshShebang(const char *path, char *const *argv) {
    if (!path || !*path) {
        return -1;
    }
    char *source = shellLoadFile(path);
    if (!source) {
        return -1;
    }

    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gParamValuesOwned;

    char **new_params = NULL;
    int new_params_count = 0;
    int new_count = 0;
    if (argv) {
        while (argv[1 + new_count]) {
            new_count++;
        }
    }
    bool replaced_params = false;
    if (new_count > 0) {
        new_params = (char **)calloc((size_t)new_count, sizeof(char *));
        if (!new_params) {
            free(source);
            return -1;
        }
        new_params_count = new_count;
        bool ok = true;
        for (int i = 0; i < new_count; ++i) {
            new_params[i] = strdup(argv[i + 1] ? argv[i + 1] : "");
            if (!new_params[i]) {
                shellRunnerFreeParamArray(new_params, i);
                free(source);
                ok = false;
                break;
            }
        }
        if (!ok) {
            return -1;
        }
        gParamValues = new_params;
        gParamCount = new_count;
        gParamValuesOwned = true;
        replaced_params = true;
    }

    const char *previous_arg0 = shellRuntimeGetArg0();
    char *saved_arg0 = previous_arg0 ? strdup(previous_arg0) : NULL;
    shellRuntimeSetArg0(path);

    ShellRunOptions opts = {0};
    opts.no_cache = 1;
    opts.quiet = true;
    opts.exit_on_signal = shellRuntimeExitOnSignal();
    opts.suppress_warnings = true;
    opts.frontend_path = previous_arg0 ? previous_arg0 : "exsh";

    bool exit_requested = false;
    int status = shellRunSource(source, path, &opts, &exit_requested);

    shellRuntimeSetArg0(saved_arg0);
    free(saved_arg0);

    if (replaced_params) {
        bool freed_alloc = false;
        if (gParamValuesOwned && gParamValues) {
            shellRunnerFreeParamArray(gParamValues, gParamCount);
            freed_alloc = (gParamValues == new_params);
        }
        if (new_params && !freed_alloc) {
            shellRunnerFreeParamArray(new_params, new_params_count);
        }
    }
    gParamValues = saved_params;
    gParamCount = saved_count;
    gParamValuesOwned = saved_owned;

    free(source);

    /* Shebang scripts run as command bodies, not as sourced shell state.
     * Keep an internal `exit` scoped to the script invocation. */
    (void)exit_requested;
    return status;
}

extern int pascal_main(int argc, char **argv);
extern int clike_main(int argc, char **argv);
extern int rea_main(int argc, char **argv);
extern int pscalvm_main(int argc, char **argv);
extern int pscaljson2bc_main(int argc, char **argv);
#ifdef BUILD_DASCAL
extern int dascal_main(int argc, char **argv);
#endif
#ifdef BUILD_PSCALD
extern int pscald_main(int argc, char **argv);
extern int pscalasm_main(int argc, char **argv);
#endif

#if defined(PSCAL_TARGET_IOS)
static bool shellRunnerIsStopStatus(int status) {
    if (status < 128 || status >= 128 + NSIG) {
        return false;
    }
    int sig = status - 128;
    return sig == SIGTSTP || sig == SIGSTOP || sig == SIGTTIN || sig == SIGTTOU;
}
#endif

static int shellSpawnToolRunner(const char *tool_name, int argc, char **argv) {
    /* On iOS we cannot reliably spawn; dispatch tool entrypoints directly. */
    struct {
        const char *name;
        int (*entry)(int, char **);
    } table[] = {
        {"pascal", pascal_main},
        {"clike", clike_main},
        {"rea", rea_main},
        {"pscalvm", pscalvm_main},
        {"pscaljson2bc", pscaljson2bc_main},
#ifdef BUILD_DASCAL
        {"dascal", dascal_main},
#endif
#ifdef BUILD_PSCALD
        {"pscald", pscald_main},
        {"pscalasm", pscalasm_main},
#endif
    };
    const char *name = tool_name && *tool_name ? tool_name : (argc > 0 && argv && argv[0] ? argv[0] : NULL);
    if (!name) {
        return 127;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcasecmp(name, table[i].name) == 0) {
#if defined(PSCAL_TARGET_IOS)
            VProc *active_vp = vprocCurrent();
            int stage_pid_for_stop = active_vp ? vprocPid(active_vp) : -1;
            int shell_pid_for_stop = vprocGetShellSelfPid();
            bool cooperative_stop_scope = false;
            if (stage_pid_for_stop > 0 &&
                stage_pid_for_stop != shell_pid_for_stop &&
                vprocIsShellSelfThread()) {
                /* Shebang frontends executing inline on the shell thread must
                 * keep SIGTSTP cooperative so Ctrl-Z unwinds to the prompt. */
                vprocSetStopUnsupported(stage_pid_for_stop, true);
                cooperative_stop_scope = true;
                /*
                 * Frontend VMs report cooperative Ctrl-Z through shell runtime
                 * status. Reset the marker so stale values never leak into a
                 * fresh invocation.
                 */
                shellRuntimeSetLastStatus(0);
            }
#endif
            int status = table[i].entry(argc, argv);
#if defined(PSCAL_TARGET_IOS)
            if (cooperative_stop_scope && status == EXIT_SUCCESS) {
                int runtime_status = shellRuntimeLastStatus();
                if (shellRunnerIsStopStatus(runtime_status)) {
                    status = runtime_status;
                }
            }
            if (cooperative_stop_scope && stage_pid_for_stop > 0) {
                vprocSetStopUnsupported(stage_pid_for_stop, false);
            }
#endif
            return status;
        }
    }
    fprintf(stderr, "%s: tool runner unavailable for '%s'\n",
            name, name);
    return 127;
}

static const char *shellResolveToolName(const char *interpreter) {
    if (!interpreter || !*interpreter) {
        return NULL;
    }
    const char *base = strrchr(interpreter, '/');
    base = base ? base + 1 : interpreter;
    if (strcasecmp(base, "pascal") == 0) return "pascal";
    if (strcasecmp(base, "clike") == 0) return "clike";
    if (strcasecmp(base, "rea") == 0) return "rea";
    if (strcasecmp(base, "pscalvm") == 0) return "pscalvm";
    if (strcasecmp(base, "pscaljson2bc") == 0) return "pscaljson2bc";
    if (strcasecmp(base, "dascal") == 0) return "dascal";
    if (strcasecmp(base, "pscald") == 0) return "pscald";
    if (strcasecmp(base, "pscalasm") == 0) return "pscalasm";
    if (strcasecmp(base, "sh") == 0) return "exsh";
    if (strcasecmp(base, "exsh") == 0) return "exsh";
    return NULL;
}

int shellMaybeExecShebangTool(const char *path, char *const *argv) {
    if (!path || !*path) {
        return -1;
    }
    char *line = NULL;
    if (!shellReadShebangLine(path, &line)) {
        return -1;
    }

    char *tokens[8];
    size_t token_count = 0;
    char *saveptr = NULL;
    char *token = strtok_r(line, " \t", &saveptr);
    while (token && token_count < 8) {
        tokens[token_count++] = token;
        token = strtok_r(NULL, " \t", &saveptr);
    }
    if (token_count == 0) {
        free(line);
        return -1;
    }

    size_t interpreter_index = 0;
    const char *interpreter = tokens[interpreter_index];
    const char *base = strrchr(interpreter, '/');
    base = base ? base + 1 : interpreter;
    if (strcmp(base, "env") == 0 && token_count >= 2) {
        interpreter_index = 1;
        interpreter = tokens[interpreter_index];
    }
    const char *tool_name = shellResolveToolName(interpreter);
    if (!tool_name) {
        free(line);
        return -1;
    }
    if (strcasecmp(tool_name, "exsh") == 0) {
        int status = shellRunExshShebang(path, argv);
        free(line);
        return status;
    }

    size_t shebang_argc = 0;
    if (token_count > interpreter_index + 1) {
        shebang_argc = token_count - (interpreter_index + 1);
    }
    size_t script_argc = 0;
    if (argv) {
        while (argv[1 + script_argc]) {
            script_argc++;
        }
    }

    size_t total_args = 1 + shebang_argc + 1 + script_argc;
    char **tool_argv = (char **)calloc(total_args, sizeof(char *));
    if (!tool_argv) {
        free(line);
        return EXIT_FAILURE;
    }

    bool ok = true;
    size_t idx = 0;
    tool_argv[idx++] = strdup(tool_name);
    for (size_t i = 0; ok && i < shebang_argc; ++i) {
        tool_argv[idx++] = strdup(tokens[interpreter_index + 1 + i]);
        if (!tool_argv[idx - 1]) {
            ok = false;
        }
    }
    tool_argv[idx++] = strdup(path);
    if (!tool_argv[idx - 1]) {
        ok = false;
    }
    for (size_t i = 0; ok && i < script_argc; ++i) {
        const char *arg = argv[1 + i];
        tool_argv[idx++] = strdup(arg ? arg : "");
        if (!tool_argv[idx - 1]) {
            ok = false;
        }
    }

    int status = EXIT_FAILURE;
    if (ok) {
        status = shellSpawnToolRunner(tool_name, (int)total_args, tool_argv);
    } else {
        fprintf(stderr, "%s: out of memory launching tool runner\n", tool_name);
    }

    for (size_t i = 0; i < total_args; ++i) {
        free(tool_argv[i]);
    }
    free(tool_argv);
    free(line);
    return status;
}
#endif
void shellSymbolTableScopeInit(ShellSymbolTableScope *scope) {
    if (!scope) {
        return;
    }
    memset(scope, 0, sizeof(*scope));
}

bool shellSymbolTableScopePush(ShellSymbolTableScope *scope) {
    if (!scope) {
        return false;
    }

    HashTable *new_global = createHashTable();
    HashTable *new_const = createHashTable();
    HashTable *new_procedure = createHashTable();
    if (!new_global || !new_const || !new_procedure) {
        if (new_global) {
            freeHashTable(new_global);
        }
        if (new_const) {
            freeHashTable(new_const);
        }
        if (new_procedure) {
            freeHashTable(new_procedure);
        }
        return false;
    }

    scope->saved_global = globalSymbols;
    scope->saved_const_global = constGlobalSymbols;
    scope->saved_procedure_table = procedure_table;
    scope->saved_current_procedure_table = current_procedure_table;

    scope->new_global = new_global;
    scope->new_const_global = new_const;
    scope->new_procedure_table = new_procedure;
    scope->active = true;

    globalSymbols = new_global;
    constGlobalSymbols = new_const;
    procedure_table = new_procedure;
    current_procedure_table = procedure_table;
    gShellSymbolTableDepth++;
    return true;
}

void shellSymbolTableScopePop(ShellSymbolTableScope *scope) {
    if (!scope || !scope->active) {
        return;
    }

    if (gShellSymbolTableDepth > 0) {
        gShellSymbolTableDepth--;
    }

    if (globalSymbols == scope->new_global) {
        freeHashTable(globalSymbols);
    }
    if (constGlobalSymbols == scope->new_const_global) {
        freeHashTable(constGlobalSymbols);
    }
    if (procedure_table == scope->new_procedure_table) {
        freeHashTable(procedure_table);
    }

    globalSymbols = scope->saved_global;
    constGlobalSymbols = scope->saved_const_global;
    procedure_table = scope->saved_procedure_table;
    current_procedure_table = scope->saved_current_procedure_table;

    scope->new_global = NULL;
    scope->new_const_global = NULL;
    scope->new_procedure_table = NULL;
    scope->active = false;
}

bool shellSymbolTableScopeIsActive(void) {
    return gShellSymbolTableDepth > 0;
}

char *shellLoadFile(const char *path) {
    if (!path) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Unable to open '%s': %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buffer = (char *)malloc((size_t)len + 1);
    if (!buffer) {
        fclose(f);
        return NULL;
    }
    size_t read = fread(buffer, 1, (size_t)len, f);
    fclose(f);
    if (read != (size_t)len) {
        free(buffer);
        return NULL;
    }
    buffer[len] = '\0';
    return buffer;
}

static char *shellRewriteCombinedRedirectsInSource(const char *src) {
    if (!src) {
        return NULL;
    }
    size_t len = strlen(src);
    size_t capacity = len + 32;
    char *out = (char *)malloc(capacity);
    if (!out) {
        return NULL;
    }
    size_t out_len = 0;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;

    for (size_t i = 0; src[i] != '\0'; ++i) {
        char c = src[i];
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
        if (c == '\\') {
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

        if (!in_single && !in_double && c == '&' && src[i + 1] == '>') {
            bool append = (src[i + 2] == '>');
            size_t j = i + (append ? 3 : 2);
            while (src[j] == ' ' || src[j] == '\t') {
                j++;
            }
            bool word_single = false, word_double = false, word_escaped = false;
            size_t start = j;
            size_t end = j;
            for (; src[end] != '\0'; ++end) {
                char wc = src[end];
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
                goto copy_char;
            }
            size_t path_len = end - start;
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
            memcpy(out + out_len, src + start, path_len);
            out_len += path_len;
            memcpy(out + out_len, extra, extra_len);
            out_len += extra_len;
            fprintf(stderr, "[rewrite] &%s> -> %s path='%.*s'\n",
                    append ? ">>" : ">", append ? ">>" : ">", (int)path_len, src + start);
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

int shellRunSource(const char *source,
                   const char *path,
                   const ShellRunOptions *options,
                   bool *out_exit_requested) {
    if (out_exit_requested) {
        *out_exit_requested = false;
    }
    if (!source || !options) {
        return EXIT_FAILURE;
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] enter path=%s source='%s'\n",
                path ? path : "(null)",
                source ? source : "(null)");
    }

#if defined(SIGPIPE)
    static bool sigpipe_ignored = false;
    if (!sigpipe_ignored) {
        signal(SIGPIPE, SIG_IGN);
        sigpipe_ignored = true;
    }
#endif

    bool previous_suppress = shellSemanticsWarningsSuppressed();
    shellSemanticsSetWarningSuppressed(options->suppress_warnings);

    bool previous_exit_on_signal = shellRuntimeExitOnSignal();

    shellRuntimePushScript();

    bool source_pushed = false;
    if (!shellRuntimeTrackSourcePush(path ? path : "")) {
        shellRuntimePopScript();
        shellRuntimeSetExitOnSignal(previous_exit_on_signal);
        shellSemanticsSetWarningSuppressed(previous_suppress);
        return EXIT_FAILURE;
    }
    source_pushed = true;

    vmSetVerboseErrors(options->verbose_errors);

    const char *defines[1];
    int define_count = 0;
    char *pre_src = preprocessConditionals(source, defines, define_count);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] preprocessed\n");
    }

    if (options->exit_on_signal) {
        shellRuntimeSetExitOnSignal(true);
    }

    ShellSymbolTableScope table_scope;
    shellSymbolTableScopeInit(&table_scope);
    bool table_scope_owned = false;
    if (!shellSymbolTableScopeIsActive()) {
        if (!shellSymbolTableScopePush(&table_scope)) {
            fprintf(stderr, "shell: failed to allocate symbol tables.\n");
            if (pre_src) {
                free(pre_src);
            }
            if (source_pushed) {
                shellRuntimeTrackSourcePop();
                source_pushed = false;
            }
            shellRuntimePopScript();
            shellRuntimeSetExitOnSignal(previous_exit_on_signal);
            shellSemanticsSetWarningSuppressed(previous_suppress);
            return EXIT_FAILURE;
        }
        table_scope_owned = true;
    } else {
        current_procedure_table = procedure_table;
    }
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] symbols-ready\n");
    }
    registerAllBuiltins();
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] builtins-registered\n");
    }

    int exit_code = EXIT_FAILURE;
    ShellProgram *program = NULL;
    ShellSemanticContext sem_ctx;
    bool sem_ctx_initialized = false;
    BytecodeChunk chunk;
    bool chunk_initialized = false;
    VM *vm = NULL;
    bool vm_initialized = false;
    ShellRuntimeState *vm_shell_ctx = NULL;
    VM *previous_vm_for_context = NULL;
    bool vm_context_swapped = false;
    bool vm_stack_dumped = false;
    VM *previous_thread_owner = NULL;
    bool assigned_thread_owner = false;
    bool exit_flag = false;
    bool should_run_exit_trap = false;
    bool trap_exit_requested = false;

    char *rewrite_src = shellRewriteCombinedRedirectsInSource(pre_src ? pre_src : source);
    const char *parse_src = rewrite_src ? rewrite_src : (pre_src ? pre_src : source);

    ShellParser parser;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] parsing\n");
    }
    program = shellParseString(parse_src, &parser);
    shellParserFree(&parser);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] parsed had_error=%d program=%p\n",
                (int)parser.had_error,
                (void *)program);
    }
    if (rewrite_src) {
        free(rewrite_src);
        rewrite_src = NULL;
    }
    if (parser.had_error || !program) {
        fprintf(stderr, "Parsing failed.\n");
        goto cleanup;
    }

    if (options->dump_ast_json) {
        shellDumpAstJson(stdout, program);
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    shellInitSemanticContext(&sem_ctx);
    sem_ctx_initialized = true;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] semantic-analyze\n");
    }
    ShellSemanticResult sem_result = shellAnalyzeProgram(&sem_ctx, program);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] semantic-done err=%d warn=%d\n",
                sem_result.error_count,
                sem_result.warning_count);
    }
    if (sem_result.warning_count > 0 && !options->suppress_warnings) {
        fprintf(stderr, "Semantic analysis produced %d warning(s).\n", sem_result.warning_count);
    }
    if (sem_result.error_count > 0) {
        fprintf(stderr, "Semantic analysis failed with %d error(s).\n", sem_result.error_count);
        goto cleanup;
    }

    initBytecodeChunk(&chunk);
    chunk_initialized = true;
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] chunk-init\n");
    }
    bool used_cache = false;
    if (!options->no_cache && path && path[0]) {
        used_cache = loadBytecodeFromCache(path, kShellCompilerId, options->frontend_path, NULL, 0, &chunk);
    }

    if (!used_cache) {
        ShellOptConfig opt_config = { false };
        shellRunOptimizations(program, &opt_config);
        shellCompile(program, &chunk);
        if (getenv("PSCALI_PROMPT_DEBUG")) {
            fprintf(stderr, "[shell-run] compile-done\n");
        }
        if (!options->no_cache && path && path[0]) {
            saveBytecodeToCache(path, kShellCompilerId, &chunk);
        }
        if (!options->quiet) {
            fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (options->dump_bytecode) {
            fprintf(stderr, "--- Compiling Shell Script to Bytecode ---\n");
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "script";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (!options->dump_bytecode_only) {
                fprintf(stderr, "\n--- executing Script with VM ---\n");
            }
        }
    } else {
        if (!options->quiet) {
            fprintf(stderr, "Loaded cached bytecode. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (options->dump_bytecode) {
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "script";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (!options->dump_bytecode_only) {
                fprintf(stderr, "\n--- executing Script with VM (cached) ---\n");
            }
        }
    }

    if (options->dump_bytecode_only) {
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    vm = (VM *)calloc(1, sizeof(VM));
    if (!vm) {
        fprintf(stderr, "shell: failed to allocate VM instance.\n");
        goto cleanup;
    }
    initVM(vm);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] vm-init\n");
    }
    vm_initialized = true;
    vm_shell_ctx = shellRuntimeCreateContext();
    if (!vm_shell_ctx) {
        fprintf(stderr, "shell: failed to allocate shell runtime context.\n");
        goto cleanup;
    }
    vm->frontendContext = vm_shell_ctx;
    previous_thread_owner = gShellThreadOwnerVm;
    if (!gShellThreadOwnerVm) {
        gShellThreadOwnerVm = vm;
        assigned_thread_owner = true;
    }
    vm->threadOwner = gShellThreadOwnerVm ? gShellThreadOwnerVm : vm;
    if (options->vm_trace_head > 0) {
        vm->trace_head_instructions = options->vm_trace_head;
    }
    previous_vm_for_context = shellSwapCurrentVm(vm);
    vm_context_swapped = true;

    InterpretResult result = interpretBytecode(vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    if (getenv("PSCALI_PROMPT_DEBUG")) {
        fprintf(stderr, "[shell-run] interpret-done result=%d\n", (int)result);
    }
    if (result == INTERPRET_RUNTIME_ERROR) {
        runtimeDebugLog("[shell] interpretBytecode -> runtime error; dumping VM stack");
        vmDumpStackInfoDetailed(vm, "shell runtime error");
        vm_stack_dumped = true;
    }
    int last_status = shellRuntimeLastStatus();
    exit_flag = shellRuntimeConsumeExitRequested();
    if (result == INTERPRET_RUNTIME_ERROR && exit_flag) {
        result = INTERPRET_OK;
    }
    should_run_exit_trap = shellRuntimeIsOutermostScript() &&
                           (!shellRuntimeIsInteractive() || exit_flag);
    exit_code = (result == INTERPRET_OK) ? last_status : EXIT_FAILURE;
    {
        char log_buf[256];
        snprintf(log_buf, sizeof(log_buf),
                 "[shell] interpret result=%d last_status=%d exit_flag=%s exit_code=%d",
                 (int)result,
                 last_status,
                 exit_flag ? "true" : "false",
                 exit_code);
        runtimeDebugLog(log_buf);
    }
    if (exit_code != EXIT_SUCCESS && vm && !vm_stack_dumped) {
        char dump_label[64];
        snprintf(dump_label, sizeof(dump_label), "shell exit code %d", exit_code);
        vmDumpStackInfoDetailed(vm, dump_label);
        vm_stack_dumped = true;
    }
cleanup:
    if (should_run_exit_trap) {
        shellRuntimeRunExitTrap();
        trap_exit_requested = shellRuntimeConsumeExitRequested();
        exit_flag = exit_flag || trap_exit_requested;
        if (trap_exit_requested) {
            exit_code = shellRuntimeLastStatus();
        }
    }
    if ((exit_code != EXIT_SUCCESS || exit_flag) ) {
        char final_log[256];
        snprintf(final_log, sizeof(final_log),
                 "[shell] final exit_code=%d exit_flag=%s trap_exit=%s",
                 exit_code,
                 exit_flag ? "true" : "false",
                 trap_exit_requested ? "true" : "false");
        runtimeDebugLog(final_log);
    }
    if (exit_code != EXIT_SUCCESS && vm && !vm_stack_dumped) {
        char dump_label[64];
        snprintf(dump_label, sizeof(dump_label), "shell final exit %d", exit_code);
        vmDumpStackInfoDetailed(vm, dump_label);
        vm_stack_dumped = true;
    }
    if (source_pushed) {
        shellRuntimeTrackSourcePop();
        source_pushed = false;
    }
    shellSemanticsSetWarningSuppressed(previous_suppress);
    shellRuntimePopScript();
    shellRuntimeSetExitOnSignal(previous_exit_on_signal);
    if (out_exit_requested) {
        *out_exit_requested = exit_flag;
    } else {
        (void)exit_flag;
    }
    if (vm_context_swapped) {
        shellRestoreCurrentVm(previous_vm_for_context);
        vm_context_swapped = false;
    }
    if (assigned_thread_owner) {
        gShellThreadOwnerVm = previous_thread_owner;
    }
    if (vm_initialized && vm) {
        freeVM(vm);
    }
    if (vm_shell_ctx) {
        shellRuntimeDestroyContext(vm_shell_ctx);
        vm_shell_ctx = NULL;
    }
    if (vm) {
        free(vm);
    }
    if (chunk_initialized) {
        freeBytecodeChunk(&chunk);
    }
    if (sem_ctx_initialized) {
        shellFreeSemanticContext(&sem_ctx);
    }
    if (program) {
        shellFreeProgram(program);
    }
    if (table_scope_owned) {
        shellSymbolTableScopePop(&table_scope);
    }
    if (pre_src) {
        free(pre_src);
    }
    vmOpcodeProfileDump();
    return exit_code;
}
