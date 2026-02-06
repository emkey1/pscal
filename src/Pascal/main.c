/*
 * MIT License
 *
 * Copyright (c) 2024 PSCAL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Note: PSCAL versions prior to 2.22 were released under the Unlicense.
 */

#include "lexer.h"
#include "parser.h"
#include "Pascal/semantic.h"
#include "ast/ast.h"
#include "opt.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/list.h"
#include "core/preproc.h"
#include "core/build_info.h"
#include "common/frontend_kind.h"
#include "common/pascal_state.h"
#include "globals.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/dump.h"
#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "core/cache.h"
#include "symbol/symbol.h"
#include "common/path_virtualization.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <fcntl.h>
#ifdef SDL
#include "core/sdl_headers.h"
#include PSCALI_SDL_TTF_HEADER
#endif
#include "vm/vm.h"
// ast.h is already included via globals.h or directly, no need for duplicate

static int s_vm_trace_head = 0;

static void pascalApplyBgRedirectionFromEnv(void) {
#if defined(PSCAL_TARGET_IOS)
    /* Avoid process-wide fd redirection on iOS; background jobs would steal the shell TTY. */
    return;
#endif
    const char *stdout_path = getenv("PSCALI_BG_STDOUT");
    const char *stdout_append = getenv("PSCALI_BG_STDOUT_APPEND");
    const char *stderr_path = getenv("PSCALI_BG_STDERR");
    const char *stderr_append = getenv("PSCALI_BG_STDERR_APPEND");

    if (stdout_path && *stdout_path) {
        int flags = O_CREAT | O_WRONLY | ((stdout_append && strcmp(stdout_append, "1") == 0) ? O_APPEND : O_TRUNC);
        int fd = open(stdout_path, flags, 0666);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            close(fd);
        }
    }
    if (stderr_path && *stderr_path) {
        int flags = O_CREAT | O_WRONLY | ((stderr_append && strcmp(stderr_append, "1") == 0) ? O_APPEND : O_TRUNC);
        int fd = open(stderr_path, flags, 0666);
        if (fd >= 0) {
            dup2(fd, STDERR_FILENO);
            close(fd);
        }
    } else if (stdout_path && *stdout_path && stderr_append && strcmp(stderr_append, "1") == 0) {
        dup2(STDOUT_FILENO, STDERR_FILENO);
    }
}
static VM *s_sigint_vm = NULL;

static void pascalHandleSigint(int signo) {
    (void)signo;
    if (s_sigint_vm) {
        s_sigint_vm->abort_requested = true;
        s_sigint_vm->exit_requested = true;
    }
}

static void pascalInstallSigint(VM *vm) {
    s_sigint_vm = vm;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = pascalHandleSigint;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
}

typedef struct {
    size_t partial_match_len;
} CachedMessageScannerState;

static char* canonicalizePath(const char* path) {
    if (!path) {
        return NULL;
    }
#ifdef _WIN32
    char resolved[_MAX_PATH];
    if (_fullpath(resolved, path, _MAX_PATH)) {
        return _strdup(resolved);
    }
#else
    char* resolved = realpath(path, NULL);
    if (resolved) {
        return resolved;
    }
#endif
    return strdup(path);
}

static bool bufferContainsCachedMessage(const char *buf, size_t len, CachedMessageScannerState *state) {
    static const char cached_msg[] = "Loaded cached bytecode";
    const size_t needle_len = sizeof(cached_msg) - 1;

    if (!state || needle_len == 0) {
        return false;
    }

    size_t matched = state->partial_match_len;

    for (size_t i = 0; i < len; ++i) {
        const char c = buf[i];

        while (matched > 0 && c != cached_msg[matched]) {
            matched = 0;
        }

        if (c == cached_msg[matched]) {
            matched++;
            if (matched == needle_len) {
                state->partial_match_len = 0;
                return true;
            }
        } else {
            matched = 0;
        }
    }

    state->partial_match_len = matched;
    return false;
}

const char *PASCAL_USAGE =
    "Usage: pascal <options> <source_file> [program_parameters...]\n"
    "   Options:\n"
    "     -v                          Display version.\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n"
    "     --dump-bytecode-only        Dump compiled bytecode and exit (no execution).\n"
    "     --dump-ext-builtins         List extended builtin inventory and exit.\n"
    "     --no-cache                  Compile fresh (ignore cached bytecode).\n"
    "     --verbose                 Print compilation/cache status messages.\n"
    "     --vm-trace-head=N           Trace first N VM instructions (also enabled by '{trace on}' in source).\n"
    "   or: pascal (with no arguments to display version and usage)";

static const char *const kPascalCompilerId = "pascal";

void initSymbolSystem(void) {
#ifdef DEBUG
    inserted_global_names = createList();
#endif
    globalSymbols = createHashTable();
    if (!globalSymbols) {
        fprintf(stderr, "FATAL: Failed to create global symbol hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    DEBUG_PRINT("[DEBUG MAIN] Created global symbol table %p.\n", (void*)globalSymbols);

    insertGlobalSymbol("TextAttr", TYPE_BYTE, NULL);
    Symbol *textAttrSym = lookupGlobalSymbol("TextAttr");
    if (textAttrSym) {
        insertGlobalAlias("CRT.TextAttr", textAttrSym);
    }
    syncTextAttrSymbol();

    constGlobalSymbols = createHashTable();
    if (!constGlobalSymbols) {
        fprintf(stderr, "FATAL: Failed to create constant symbol hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    
    procedure_table = createHashTable();
    if (!procedure_table) {
        fprintf(stderr, "FATAL: Failed to create procedure hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    current_procedure_table = procedure_table;
    DEBUG_PRINT("[DEBUG MAIN] Created procedure hash table %p.\n", (void*)procedure_table);
#ifdef SDL
    initializeTextureSystem();
#endif
}

int runProgram(const char *source, const char *programName, const char *frontend_path,
               int dump_ast_json_flag, int dump_bytecode_flag, int dump_bytecode_only_flag,
               int no_cache_flag, int verbose_flag) {
    if (globalSymbols == NULL) {
        fprintf(stderr, "Internal error: globalSymbols hash table is NULL at the start of runProgram.\n");
        EXIT_FAILURE_HANDLER();
    }

    gSuppressWriteSpacing = 1;
    gUppercaseBooleans = 1;

    /* Register built-in functions and procedures. */
    registerAllBuiltins();
    registerBuiltinFunction("mstreamappendbyte", AST_FUNCTION_DECL, NULL);

#ifdef DEBUG
    fprintf(stderr, "Completed all built-in registrations. About to init lexer.\n");
    fflush(stderr);
#endif

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);

    AST *GlobalAST = NULL;
    bool overall_success_status = false;
    bool used_cache = false;

    // Note: stderr capture is handled at the top-level in main() only.
    // Avoid nested capture here to ensure early exits flush through main's handler.

    Lexer lexer;
    initLexer(&lexer, source);
    Parser parser;
    parser.lexer = &lexer;
    parser.current_token = getNextToken(&lexer);
    parser.current_unit_name_context = NULL;
    parser.dependency_paths = createList();
    GlobalAST = buildProgramAST(&parser, &chunk);
    if (parser.current_token) { freeToken(parser.current_token); parser.current_token = NULL; }

    if (GlobalAST && GlobalAST->type == AST_PROGRAM) {
        annotateTypes(GlobalAST, NULL, GlobalAST);
        int semantic_errors_before = pascal_semantic_error_count;
        pascalPerformSemanticAnalysis(GlobalAST);
        bool semantic_errors_increased = pascal_semantic_error_count > semantic_errors_before;
        if ((pascal_semantic_error_count > 0 || pascal_parser_error_count > 0 || semantic_errors_increased) &&
            !dump_ast_json_flag) {
            fprintf(stderr, "Compilation failed with errors.\n");
            overall_success_status = false;
        } else if (dump_ast_json_flag) {
            fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
            dumpASTJSON(GlobalAST, stdout);
            fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
            overall_success_status = true;
        } else {
            GlobalAST = optimizePascalAST(GlobalAST);
            const char **dep_array = NULL;
            int dep_count = 0;
            if (parser.dependency_paths) {
                dep_count = listSize(parser.dependency_paths);
                if (dep_count > 0) {
                    dep_array = (const char**)malloc(sizeof(char*) * dep_count);
                    if (!dep_array) {
                        fprintf(stderr, "Out of memory while collecting unit dependencies.\n");
                        EXIT_FAILURE_HANDLER();
                    }
                    for (int i = 0; i < dep_count; ++i) {
                        dep_array[i] = listGet(parser.dependency_paths, i);
                    }
                }
            }

            if (!no_cache_flag) {
                used_cache = loadBytecodeFromCache(programName, kPascalCompilerId, frontend_path, dep_array, dep_count, &chunk);
            }
            if (dep_array) {
                free(dep_array);
                dep_array = NULL;
            }
            if (parser.dependency_paths) {
                freeList(parser.dependency_paths);
                parser.dependency_paths = NULL;
            }
            bool compilation_ok_for_vm = true;
            if (!used_cache) {
                if (dump_bytecode_flag) {
                    fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
                }
                compilation_ok_for_vm = compileASTToBytecode(GlobalAST, &chunk);
                if (compilation_ok_for_vm) {
                    finalizeBytecode(&chunk);
                    saveBytecodeToCache(programName, kPascalCompilerId, &chunk);
                    if (verbose_flag) {
                        fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n",
                                chunk.count, chunk.constants_count);
                    }
                    if (dump_bytecode_flag) {
                        const char* disasm_name = programName ? bytecodeDisplayNameForPath(programName) : "CompiledChunk";
                        disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
                        if (!dump_bytecode_only_flag) {
                            fprintf(stderr, "\n--- executing Program with VM ---\n");
                        }
                    }
                }
            } else {
                if (verbose_flag) {
                    fprintf(stderr, "Loaded cached bytecode. Bytecode size: %d bytes, Constants: %d\n",
                            chunk.count, chunk.constants_count);
                }
                if (dump_bytecode_flag) {
                    const char* disasm_name = programName ? bytecodeDisplayNameForPath(programName) : "CompiledChunk";
                    disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
                    if (!dump_bytecode_only_flag) {
                        fprintf(stderr, "\n--- executing Program with VM (cached) ---\n");
                    }
                }
            }

            if (compilation_ok_for_vm) {
                if (dump_bytecode_only_flag) {
                    overall_success_status = true;
                } else {
                    VM *vm = (VM *)calloc(1, sizeof(VM));
                    if (!vm) {
                        fprintf(stderr, "Out of memory while creating VM.\n");
                        overall_success_status = false;
                    } else {
                        initVM(vm);
                        vmSetVerboseErrors(true);
                        if (s_vm_trace_head > 0) {
                            vm->trace_head_instructions = s_vm_trace_head;
                        } else if (source && strstr(source, "trace on")) {
                            vm->trace_head_instructions = 16;
                        }
                        pascalInstallSigint(vm);
                        InterpretResult result_vm =
                            interpretBytecode(vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
                        if (result_vm == INTERPRET_OK) {
                            overall_success_status = true;
                        } else {
                            fprintf(stderr, "--- VM execution Failed (%s) ---\n",
                                    result_vm == INTERPRET_RUNTIME_ERROR ? "Runtime Error" : "Compile Error (VM stage)");
                            overall_success_status = false;
                            vmDumpStackInfo(vm);
                        }
                        s_sigint_vm = NULL;
                        freeVM(vm);
                        free(vm);
                        globalSymbols = NULL;
                    }
                }
            } else {
                fprintf(stderr, "Compilation failed with errors.\n");
                overall_success_status = false;
            }
        }
            } else if (!dump_ast_json_flag) {
        fprintf(stderr, "Failed to build Program AST for execution.\n");
        overall_success_status = false;
    } else if (dump_ast_json_flag && (!GlobalAST || GlobalAST->type != AST_PROGRAM)) {
        fprintf(stderr, "Failed to build Program AST for JSON dump.\n");
        overall_success_status = false;
    }

    // No local stderr capture/restore here.

    if (parser.dependency_paths) {
        freeList(parser.dependency_paths);
        parser.dependency_paths = NULL;
    }

    freeBytecodeChunk(&chunk);
    freeProcedureTable();
    freeTypeTableASTNodes();
    freeTypeTable();
    if (globalSymbols) {
        freeHashTable(globalSymbols);
        globalSymbols = NULL;
    }
    if (constGlobalSymbols) {
        freeHashTable(constGlobalSymbols);
        constGlobalSymbols = NULL;
    }
#ifdef DEBUG
    if (inserted_global_names) {
        freeList(inserted_global_names);
        inserted_global_names = NULL;
    }
#endif
    if (GlobalAST) {
        freeAST(GlobalAST);
        GlobalAST = NULL;
    }
    compilerResetState();
#ifdef SDL
    sdlCleanupAtExit();
#endif
    return overall_success_status ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Consistent main function structure for argument parsing
// Top-level stderr capture state for clean-success suppression
static FILE* s_stderr_tmp = NULL;
static int s_saved_stderr_fd = -1;
static int s_capture_active = 0;

static ssize_t pascalWriteStderr(const void *buf, size_t len) {
#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
    return vprocWriteShim(STDERR_FILENO, buf, len);
#else
    return write(STDERR_FILENO, buf, len);
#endif
}

static void flushCapturedStderrAtExit(void) {
    if (!s_capture_active) return;
    fflush(stderr);
    int saved = s_saved_stderr_fd;
    FILE* tmp = s_stderr_tmp;
    s_capture_active = 0;
    s_saved_stderr_fd = -1;
    s_stderr_tmp = NULL;
    if (saved != -1) {
        dup2(saved, STDERR_FILENO);
        close(saved);
    }
    if (tmp) {
        rewind(tmp);
        char buf[4096]; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), tmp)) > 0) {
            size_t total = 0;
            while (total < n) {
                ssize_t w = pascalWriteStderr(buf + total, n - total);
                if (w <= 0) {
                    total = n; // force exit outer loop on error
                    break;
                }
                total += (size_t)w;
            }
            if (total < n) break;
        }
        fclose(tmp);
    }
}

#ifndef PSCAL_PASCAL_ENTRY_SYMBOL
#define PSCAL_PASCAL_ENTRY_SYMBOL pascal_main
#endif

#define PAS_RETURN(value)                \
    do {                                 \
        int __rc = vmExitWithCleanup(value); \
        frontendPopKind(previousKind);   \
        return __rc;                     \
    } while (0)

int PSCAL_PASCAL_ENTRY_SYMBOL(int argc, char *argv[]) {
    /* Reset global Pascal state so repeated in-process runs start clean. */
    pascalInvalidateGlobalState();

    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
    const char* initTerm = getenv("PSCAL_INIT_TERM");
    if (initTerm && *initTerm && *initTerm != '0') vmInitTerminalState();
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    int dump_ext_builtins_flag = 0;
    int verbose_flag = 0;
    int no_cache_flag = 0;
    const char *sourceFile = NULL;
    const char *programName = argv[0]; // Default program name to executable name
    int pscal_params_start_index = 0; // Will be set after source file is identified

    if (argc == 1) {
        printf("Pascal Version: %s (latest tag: %s)\n",
               pscal_program_version_string(), pscal_git_tag_string());
        printf("%s\n", PASCAL_USAGE);
        PAS_RETURN(EXIT_SUCCESS);
    }

    // Parse options first
    int i = 1;
    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("%s\n", PASCAL_USAGE);
            PAS_RETURN(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "-v") == 0) {
            printf("Pascal Version: %s (latest tag: %s)\n",
                   pscal_program_version_string(), pscal_git_tag_string());
            PAS_RETURN(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode-only") == 0) {
            dump_bytecode_flag = 1;
            dump_bytecode_only_flag = 1;
        } else if (strcmp(argv[i], "--dump-ext-builtins") == 0) {
            dump_ext_builtins_flag = 1;
        } else if (strcmp(argv[i], "--no-cache") == 0) {
            no_cache_flag = 1;
        } else if (strcmp(argv[i], "--verbose") == 0) {
            verbose_flag = 1;
        } else if (strncmp(argv[i], "--vm-trace-head=", 16) == 0) {
            s_vm_trace_head = atoi(argv[i] + 16);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "%s\n", PASCAL_USAGE);
            PAS_RETURN(EXIT_FAILURE);
        } else {
            // First non-option argument is the source file
            sourceFile = argv[i];
            programName = sourceFile; // Use actual filename for logs/dumps
            pscal_params_start_index = i + 1; // Next args are for the front end program
            break; // Stop parsing options, rest are program args
        }
    }

    if (dump_ext_builtins_flag) {
        registerExtendedBuiltins();
        extBuiltinDumpInventory(stdout);
        PAS_RETURN(EXIT_SUCCESS);
    }

    // If --dump-ast-json was specified but no source file yet, check next arg
    if (dump_ast_json_flag && !sourceFile) {
        if (i < argc && argv[i][0] != '-') { // Check if current argv[i] is a potential source file
            sourceFile = argv[i];
            programName = sourceFile;
            pscal_params_start_index = i + 1;
            i++; // Consume this argument
        } else {
            fprintf(stderr, "Error: --dump-ast-json requires a <source_file> argument.\n");
            PAS_RETURN(EXIT_FAILURE);
        }
    }


    if (!sourceFile) {
        fprintf(stderr, "Error: No source file specified.\n");
        fprintf(stderr, "%s\n", PASCAL_USAGE);
        PAS_RETURN(EXIT_FAILURE);
    }

    char* canonical_source_path = canonicalizePath(sourceFile);
    if (canonical_source_path) {
        sourceFile = canonical_source_path;
        programName = canonical_source_path;
    }

    // Initialize core systems
    initSymbolSystem();

    char *source_buffer = NULL;
    FILE *file = fopen(sourceFile, "r");
    if (!file) {
        perror("Error opening source file");
        // Minimal cleanup if initSymbolSystem did very little before this point
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        if (canonical_source_path) free(canonical_source_path);
        PAS_RETURN(EXIT_FAILURE);
    }
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);
    source_buffer = malloc(fsize + 1);
    if (!source_buffer) {
        fprintf(stderr, "Memory allocation error reading file\n");
        fclose(file);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        if (canonical_source_path) free(canonical_source_path);
        PAS_RETURN(EXIT_FAILURE);
    }
    size_t bytes_read = fread(source_buffer, 1, fsize, file);
    if (bytes_read != (size_t)fsize) {
        fprintf(stderr, "Error reading source file '%s'\n", sourceFile);
        free(source_buffer);
        fclose(file);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        if (canonical_source_path) free(canonical_source_path);
        PAS_RETURN(EXIT_FAILURE);
    }
    source_buffer[fsize] = '\0';
    fclose(file);

    const char *defines[1] = {NULL};
    int define_count = 0;
#ifdef SDL
    defines[define_count++] = "SDL_ENABLED";
#endif
    char *preprocessed_source = preprocessConditionals(source_buffer, defines, define_count);
    const char *effective_source = preprocessed_source ? preprocessed_source : source_buffer;

    // Set up front end program's command-line parameters
    if (pscal_params_start_index < argc) {
        gParamCount = argc - pscal_params_start_index;
        gParamValues = &argv[pscal_params_start_index];
    } else {
        gParamCount = 0;
        gParamValues = NULL;
    }

    // Strict success mode capture (default ON; disable with PSCAL_STRICT_SUCCESS=0)
    int capture_stderr = 0;
    {
        int strict_success = 1; const char* strict_env = getenv("PSCAL_STRICT_SUCCESS");
        if (strict_env && *strict_env == '0') strict_success = 0;
        if (strict_success && !dump_ast_json_flag && !dump_bytecode_flag && !dump_bytecode_only_flag) {
            s_saved_stderr_fd = dup(STDERR_FILENO);
            s_stderr_tmp = tmpfile();
            if (s_stderr_tmp) { dup2(fileno(s_stderr_tmp), STDERR_FILENO); capture_stderr = 1; s_capture_active = 1; }
            atexit(flushCapturedStderrAtExit);
        }
    }

    // Call runProgram
    int result = runProgram(effective_source, programName, argv[0], dump_ast_json_flag,
                            dump_bytecode_flag, dump_bytecode_only_flag, no_cache_flag, verbose_flag);

    // Restore stderr and conditionally replay
    if (capture_stderr) {
        fflush(stderr);
        if (s_saved_stderr_fd != -1) { dup2(s_saved_stderr_fd, STDERR_FILENO); close(s_saved_stderr_fd); s_saved_stderr_fd = -1; }
        if (s_stderr_tmp) {
            rewind(s_stderr_tmp);
            // Scan buffer for non-whitespace or cached message
            int has_non_ws = 0, has_cached = 0; char buf[4096]; size_t n;
            CachedMessageScannerState cached_scan = {0};
            while ((n = fread(buf, 1, sizeof(buf), s_stderr_tmp)) > 0) {
                for (size_t i = 0; i < n; i++) {
                    char c = buf[i];
                    if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) { has_non_ws = 1; break; }
                }
                if (!has_cached && bufferContainsCachedMessage(buf, n, &cached_scan)) {
                    has_cached = 1;
                }
                if (has_non_ws && has_cached) break;
            }
            // Decide replay: nonzero exit or cached or non-whitespace
            if (result != EXIT_SUCCESS || has_cached || has_non_ws) {
                rewind(s_stderr_tmp);
                while ((n = fread(buf, 1, sizeof(buf), s_stderr_tmp)) > 0) {
                    size_t total = 0;
                    while (total < n) {
                        ssize_t w = pascalWriteStderr(buf + total, n - total);
                        if (w <= 0) {
                            total = n;
                            break;
                        }
                        total += (size_t)w;
                    }
                    if (total < n) break;
                }
            }
            fclose(s_stderr_tmp);
            s_stderr_tmp = NULL;
        }
        s_capture_active = 0; // disable atexit replay; we've handled it
    }

    if (preprocessed_source) {
        free(preprocessed_source);
    }
    free(source_buffer); // Free the source code buffer
    if (canonical_source_path) {
        free(canonical_source_path);
    }
    PAS_RETURN(result);
}

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char *argv[]) {
    pascalApplyBgRedirectionFromEnv();
    return PSCAL_PASCAL_ENTRY_SYMBOL(argc, argv);
}
#endif
