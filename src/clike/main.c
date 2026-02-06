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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <signal.h>
#include "compiler/bytecode.h"
#include "clike/parser.h"
#include "clike/codegen.h"
#include "clike/builtins.h"
#include "clike/semantics.h"
#include "clike/state.h"
#include "clike/errors.h"
#include "clike/opt.h"
#include "clike/preproc.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "pscal_paths.h"
#include "core/utils.h"
#include "core/build_info.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/dump.h"
#include "common/frontend_kind.h"
#include "common/path_virtualization.h"
#include <fcntl.h>
#include <unistd.h>

int clike_error_count = 0;
int clike_warning_count = 0;

static void clikeApplyBgRedirectionFromEnv(void) {
#if defined(PSCAL_TARGET_IOS)
    /* iOS shares process fds across threads; redirecting here would steal the shell's TTY.
     * Applets that need logging (e.g., simple_web_server) should handle PSCALI_BG_* themselves. */
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

static void initSymbolSystemClike(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

static VM *g_sigint_vm = NULL;

static void clikeHandleSigint(int signo) {
    (void)signo;
    if (g_sigint_vm) {
        g_sigint_vm->abort_requested = true;
        g_sigint_vm->exit_requested = true;
    }
}

static void clikeInstallSigint(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = clikeHandleSigint;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
}

static const char *CLIKE_USAGE =
    "Usage: clike <options> <source.cl> [program_parameters...]\n"
    "   Options:\n"
    "     -v                          Display version.\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n"
    "     --dump-bytecode-only        Dump compiled bytecode and exit (no execution).\n"
    "     --dump-ext-builtins         List extended builtin inventory and exit.\n"
    "     --no-cache                  Compile fresh (ignore cached bytecode).\n"
    "     --verbose                 Print compilation/cache status messages.\n"
    "     --vm-trace-head=N           Trace first N VM instructions (also enabled by 'trace on' in source).\n"
    "\n"
    "   Thread helpers registered by the REPL/front end:\n"
    "     thread_spawn_named(target, name, ...)  Launch allow-listed builtin on worker thread.\n"
    "     thread_pool_submit(target, name, ...) Queue work on the shared pool without blocking the caller.\n"
    "     thread_pause/resume/cancel(handle)    Mirror the VM control operations; return 1 on success.\n"
    "     thread_get_status(handle, drop)       Fetch success flags (pass non-zero drop to free the slot).\n"
    "     thread_stats()                        Array describing active worker slots for dashboards/metrics.\n";

static const char *const kClikeCompilerId = "clike";

static char* resolveImportPath(const char* orig_path) {
    FILE *f = fopen(orig_path, "rb");
    if (f) { fclose(f); return strdup(orig_path); }
    const char *lib_dir = getenv("CLIKE_LIB_DIR");
    if (lib_dir && *lib_dir) {
        size_t len = strlen(lib_dir) + 1 + strlen(orig_path) + 1;
        char *path = (char*)malloc(len);
        if (path) {
            snprintf(path, len, "%s/%s", lib_dir, orig_path);
            f = fopen(path, "rb");
            if (f) { fclose(f); return path; }
            free(path);
        }
    }
    const char *default_dir = PSCAL_CLIKE_LIB_DIR;
    size_t len = strlen(default_dir) + 1 + strlen(orig_path) + 1;
    char *path = (char*)malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/%s", default_dir, orig_path);
    f = fopen(path, "rb");
    if (f) { fclose(f); return path; }
    free(path);
    return NULL;
}

int clike_main(int argc, char **argv) {
    /* Ensure a clean slate when clike is run in-process multiple times. */
    clikeInvalidateGlobalState();

    clikeApplyBgRedirectionFromEnv();
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_CLIKE);
#define CLIKE_RETURN(value)            \
    do {                               \
        int __clike_rc = (value);      \
        frontendPopKind(previousKind); \
        return __clike_rc;             \
    } while (0)
    clike_error_count = 0;
    clike_warning_count = 0;
    // Keep terminal untouched for clike: no raw mode or color push
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    int dump_ext_builtins_flag = 0;
    int vm_trace_head = 0;
    int no_cache_flag = 0;
    int verbose_flag = 0;
    const char *path = NULL;
    int clike_params_start = 0;

    if (argc == 1) {
        fprintf(stderr, "%s\n", CLIKE_USAGE);
        CLIKE_RETURN(EXIT_FAILURE);
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("%s", CLIKE_USAGE);
            CLIKE_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
        } else if (strcmp(argv[i], "-v") == 0) {
            printf("Clike Compiler Version: %s (latest tag: %s)\n",
                   pscal_program_version_string(), pscal_git_tag_string());
            CLIKE_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
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
            vm_trace_head = atoi(argv[i] + 16);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s\n", argv[i], CLIKE_USAGE);
            CLIKE_RETURN(EXIT_FAILURE);
        } else {
            path = argv[i];
            clike_params_start = i + 1;
            break;
        }
    }

    if (dump_ext_builtins_flag) {
        registerExtendedBuiltins();
        extBuiltinDumpInventory(stdout);
        CLIKE_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }

    if (!path) {
        fprintf(stderr, "Error: No source file specified.\n%s\n", CLIKE_USAGE);
        CLIKE_RETURN(EXIT_FAILURE);
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); CLIKE_RETURN(EXIT_FAILURE); }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    char *src = (char*)malloc(len + 1);
    if (!src) { fclose(f); CLIKE_RETURN(EXIT_FAILURE); }
    size_t bytes_read = fread(src,1,len,f);
    if (bytes_read != (size_t)len) {
        fprintf(stderr, "Error reading source file '%s'\n", path);
        free(src);
        fclose(f);
        CLIKE_RETURN(EXIT_FAILURE);
    }
    src[len]='\0'; fclose(f);

    const char *defines[1];
    int define_count = 0;
#ifdef SDL
    defines[define_count++] = "SDL_ENABLED";
#endif
    char *pre_src = clikePreprocess(src, path, defines, define_count);

    ParserClike parser; initParserClike(&parser, pre_src ? pre_src : src);
    ASTNodeClike *prog = parseProgramClike(&parser);
    freeParserClike(&parser);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after parsing.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        CLIKE_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }

    if (dump_ast_json_flag) {
        fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
        dumpASTClikeJSON(prog, stdout);
        fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        CLIKE_RETURN(EXIT_SUCCESS);
    }

    if (clike_params_start < argc) {
        gParamCount = argc - clike_params_start;
        gParamValues = &argv[clike_params_start];
    }

    initSymbolSystemClike();
    clikeRegisterBuiltins();
    analyzeSemanticsClike(prog, path);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after semantic analysis.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        clikeResetSymbolState();
        CLIKE_RETURN(EXIT_FAILURE);
    }

    if (clike_warning_count > 0) {
        fprintf(stderr, "Compilation finished with %d warning(s).\n", clike_warning_count);
    }
    if (clike_error_count > 0) {
        fprintf(stderr, "Compilation halted with %d error(s).\n", clike_error_count);
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        clikeResetSymbolState();
        CLIKE_RETURN(clike_error_count > 255 ? 255 : clike_error_count);
    }
    prog = optimizeClikeAST(prog);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after optimization.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        clikeResetSymbolState();
        CLIKE_RETURN(EXIT_FAILURE);
    }

    char **dep_paths = NULL;
    if (clike_import_count > 0) {
        dep_paths = (char**)malloc(sizeof(char*) * clike_import_count);
        if (dep_paths) {
            for (int i = 0; i < clike_import_count; ++i) {
                dep_paths[i] = resolveImportPath(clike_imports[i]);
                if (!dep_paths[i]) dep_paths[i] = strdup(clike_imports[i]);
            }
        }
    }
    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    bool used_cache = false;
    if (!no_cache_flag) {
        used_cache = loadBytecodeFromCache(path, kClikeCompilerId, argv[0], (const char**)dep_paths, clike_import_count, &chunk);
    }
    if (dep_paths) {
        for (int i = 0; i < clike_import_count; ++i) free(dep_paths[i]);
        free(dep_paths);
    }
    if (used_cache) {
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st) ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st) ((st).st_mtim.tv_sec)
#endif
        char* cache_path = buildCachePath(path, kClikeCompilerId);
        struct stat cache_stat;
        if (!cache_path || stat(cache_path, &cache_stat) != 0) {
            if (cache_path) free(cache_path);
            freeBytecodeChunk(&chunk);
            initBytecodeChunk(&chunk);
            used_cache = false;
        } else {
            for (int i = 0; i < clike_import_count && used_cache; ++i) {
                struct stat dep_stat;
                if (stat(clike_imports[i], &dep_stat) != 0 ||
                    PSCAL_STAT_SEC(cache_stat) <= PSCAL_STAT_SEC(dep_stat)) {
                    freeBytecodeChunk(&chunk);
                    initBytecodeChunk(&chunk);
                    used_cache = false;
                    break;
                }
            }
            free(cache_path);
        }
#undef PSCAL_STAT_SEC
    }
    if (!used_cache) {
        clikeCompile(prog, &chunk);
        saveBytecodeToCache(path, kClikeCompilerId, &chunk);
        if (verbose_flag) {
            fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (dump_bytecode_flag) {
            fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "CompiledChunk";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (!dump_bytecode_only_flag) {
                fprintf(stderr, "\n--- executing Program with VM ---\n");
            }
        }
    } else {
        if (verbose_flag) {
            fprintf(stderr, "Loaded cached bytecode. Bytecode size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
        }
        if (dump_bytecode_flag) {
            const char* disasm_name = path ? bytecodeDisplayNameForPath(path) : "CompiledChunk";
            disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
            if (!dump_bytecode_only_flag) {
                fprintf(stderr, "\n--- executing Program with VM (cached) ---\n");
            }
        }
    }

    if (dump_bytecode_only_flag) {
        // Cleanup and exit without executing
        freeBytecodeChunk(&chunk);
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (pre_src) free(pre_src);
        clikeResetSymbolState();
        CLIKE_RETURN(EXIT_SUCCESS);
    }

    clikeInstallSigint();
    VM vm; initVM(&vm);
    // Inline trace toggle via comment: /* trace on */ or // trace on
    if (vm_trace_head > 0) {
        vm.trace_head_instructions = vm_trace_head;
    } else if ((pre_src && strstr(pre_src, "trace on")) || (src && strstr(src, "trace on"))) {
        vm.trace_head_instructions = 16;
    }
    g_sigint_vm = &vm;
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    g_sigint_vm = NULL;
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    freeASTClike(prog);
    clikeFreeStructs();
    free(src);
    if (pre_src) free(pre_src);
    clikeResetSymbolState();
    CLIKE_RETURN(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE);
}
#undef CLIKE_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char **argv) {
    return clike_main(argc, argv);
}
#endif
