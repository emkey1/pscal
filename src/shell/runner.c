#include "shell/runner.h"

#include "backend_ast/builtin.h"
#include "compiler/bytecode.h"
#include "core/cache.h"
#include "core/preproc.h"
#include "shell/builtins.h"
#include "shell/codegen.h"
#include "shell/opt.h"
#include "shell/parser.h"
#include "shell/semantics.h"
#include "symbol/symbol.h"
#include "vm/vm.h"
#include "Pascal/globals.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const kShellCompilerId = "shell";

static int gShellSymbolTableDepth = 0;

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
    registerAllBuiltins();

    int exit_code = EXIT_FAILURE;
    ShellProgram *program = NULL;
    ShellSemanticContext sem_ctx;
    bool sem_ctx_initialized = false;
    BytecodeChunk chunk;
    bool chunk_initialized = false;
    VM vm;
    bool vm_initialized = false;
    bool exit_flag = false;
    bool should_run_exit_trap = false;

    ShellParser parser;
    program = shellParseString(pre_src ? pre_src : source, &parser);
    shellParserFree(&parser);
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
    ShellSemanticResult sem_result = shellAnalyzeProgram(&sem_ctx, program);
    if (sem_result.warning_count > 0 && !options->suppress_warnings) {
        fprintf(stderr, "Semantic analysis produced %d warning(s).\n", sem_result.warning_count);
    }
    if (sem_result.error_count > 0) {
        fprintf(stderr, "Semantic analysis failed with %d error(s).\n", sem_result.error_count);
        goto cleanup;
    }

    initBytecodeChunk(&chunk);
    chunk_initialized = true;
    bool used_cache = false;
    if (!options->no_cache && path && path[0]) {
        used_cache = loadBytecodeFromCache(path, kShellCompilerId, options->frontend_path, NULL, 0, &chunk);
    }

    if (!used_cache) {
        ShellOptConfig opt_config = { false };
        shellRunOptimizations(program, &opt_config);
        shellCompile(program, &chunk);
        if (path && path[0]) {
            saveBytecodeToCache(path, kShellCompilerId, &chunk);
        }
        if (!options->quiet) {
            fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (options->dump_bytecode) {
            fprintf(stderr, "--- Compiling Shell Script to Bytecode ---\n");
            disassembleBytecodeChunk(&chunk, path ? path : "script", procedure_table);
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
            disassembleBytecodeChunk(&chunk, path ? path : "script", procedure_table);
            if (!options->dump_bytecode_only) {
                fprintf(stderr, "\n--- executing Script with VM (cached) ---\n");
            }
        }
    }

    if (options->dump_bytecode_only) {
        exit_code = EXIT_SUCCESS;
        goto cleanup;
    }

    initVM(&vm);
    vm_initialized = true;
    if (options->vm_trace_head > 0) {
        vm.trace_head_instructions = options->vm_trace_head;
    }

    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    int last_status = shellRuntimeLastStatus();
    exit_flag = shellRuntimeConsumeExitRequested();
    if (result == INTERPRET_RUNTIME_ERROR && exit_flag) {
        result = INTERPRET_OK;
    }
    should_run_exit_trap = shellRuntimeIsOutermostScript() &&
                           (!shellRuntimeIsInteractive() || exit_flag);
    exit_code = (result == INTERPRET_OK) ? last_status : EXIT_FAILURE;

cleanup:
    if (should_run_exit_trap) {
        shellRuntimeRunExitTrap();
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
    if (vm_initialized) {
        freeVM(&vm);
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
    return exit_code;
}
