#include "lexer.h"
#include "parser.h"
#include "ast/ast.h"
#include "opt.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/list.h"
#include "globals.h"
#include "backend_ast/builtin.h"
#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "core/cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#endif
#include "vm/vm.h"
// ast.h is already included via globals.h or directly, no need for duplicate

/* Global variables */
int gParamCount = 0;
char **gParamValues = NULL;

#ifdef DEBUG
List *inserted_global_names = NULL;
#endif

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "undefined.version_DEV"
#endif

static int s_vm_trace_head = 0;

const char *PASCAL_USAGE =
    "Usage: pascal <options> <source_file> [program_parameters...]\n"
    "   Options:\n"
    "     -v                          Display version.\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n"
    "     --dump-bytecode-only        Dump compiled bytecode and exit (no execution).\n"
    "     --vm-trace-head=N           Trace first N VM instructions (also enabled by '{trace on}' in source).\n"
    "   or: pascal (with no arguments to display version and usage)";

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

int runProgram(const char *source, const char *programName, const char *frontend_path, int dump_ast_json_flag, int dump_bytecode_flag, int dump_bytecode_only_flag) {
    if (globalSymbols == NULL) {
        fprintf(stderr, "Internal error: globalSymbols hash table is NULL at the start of runProgram.\n");
        EXIT_FAILURE_HANDLER();
    }
    
    /* Register built-in functions and procedures. */
    registerAllBuiltins();
    
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
    GlobalAST = buildProgramAST(&parser, &chunk);
    if (parser.current_token) { freeToken(parser.current_token); parser.current_token = NULL; }

    if (GlobalAST && GlobalAST->type == AST_PROGRAM) {
        annotateTypes(GlobalAST, NULL, GlobalAST);
        if ((pascal_semantic_error_count > 0 || pascal_parser_error_count > 0) && !dump_ast_json_flag) {
            fprintf(stderr, "Compilation failed with errors.\n");
            overall_success_status = false;
        } else if (dump_ast_json_flag) {
            fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
            dumpASTJSON(GlobalAST, stdout);
            fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
            overall_success_status = true;
        } else {
            GlobalAST = optimizePascalAST(GlobalAST);
            used_cache = loadBytecodeFromCache(programName, frontend_path, NULL, 0, &chunk);
            bool compilation_ok_for_vm = true;
            if (!used_cache) {
                if (dump_bytecode_flag) {
                    fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
                }
                compilation_ok_for_vm = compileASTToBytecode(GlobalAST, &chunk);
                if (compilation_ok_for_vm) {
                    finalizeBytecode(&chunk);
                    saveBytecodeToCache(programName, &chunk);
                    // Silence successful compilation message for cleaner test stderr.
                    // fprintf(stderr, "Compilation successful. Byte code size: %d bytes, Constants: %d\n", chunk.count, chunk.constants_count);
                    if (dump_bytecode_flag) {
                        disassembleBytecodeChunk(&chunk, programName ? programName : "CompiledChunk", procedure_table);
                        if (!dump_bytecode_only_flag) {
                            fprintf(stderr, "\n--- executing Program with VM ---\n");
                        }
                    }
                }
            } else {
                // Always emit the cache message so callers can detect cache reuse;
                // the top-level stderr capture in main() will decide whether to replay.
                fprintf(stderr, "Loaded cached byte code. Byte code size: %d bytes, Constants: %d\n", chunk.count, chunk.constants_count);
                if (dump_bytecode_flag) {
                    disassembleBytecodeChunk(&chunk, programName ? programName : "CompiledChunk", procedure_table);
                    if (!dump_bytecode_only_flag) {
                        fprintf(stderr, "\n--- executing Program with VM (cached) ---\n");
                    }
                }
            }

            if (compilation_ok_for_vm) {
                if (dump_bytecode_only_flag) {
                    overall_success_status = true;
                } else {
                VM vm;
                initVM(&vm);
                // Inline trace toggle via source comment: {trace on} / {trace off}
                if (s_vm_trace_head > 0) vm.trace_head_instructions = s_vm_trace_head;
                else if (source && strstr(source, "trace on")) vm.trace_head_instructions = 16;
                InterpretResult result_vm = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
                freeVM(&vm);
                globalSymbols = NULL;
                if (result_vm == INTERPRET_OK) {
                    overall_success_status = true;
                } else {
                    fprintf(stderr, "--- VM execution Failed (%s) ---\n",
                            result_vm == INTERPRET_RUNTIME_ERROR ? "Runtime Error" : "Compile Error (VM stage)");
                    overall_success_status = false;
                    vmDumpStackInfo(&vm);
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
                ssize_t w = write(STDERR_FILENO, buf + total, n - total);
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

int main(int argc, char *argv[]) {
    const char* initTerm = getenv("PSCAL_INIT_TERM");
    if (initTerm && *initTerm && *initTerm != '0') vmInitTerminalState();
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    const char *sourceFile = NULL;
    const char *programName = argv[0]; // Default program name to executable name
    int pscal_params_start_index = 0; // Will be set after source file is identified

    if (argc == 1) {
        printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
        printf("%s\n", PASCAL_USAGE);
        return vmExitWithCleanup(EXIT_SUCCESS);
    }

    // Parse options first
    int i = 1;
    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) {
            printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
            return vmExitWithCleanup(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode-only") == 0) {
            dump_bytecode_flag = 1;
            dump_bytecode_only_flag = 1;
        } else if (strncmp(argv[i], "--vm-trace-head=", 16) == 0) {
            s_vm_trace_head = atoi(argv[i] + 16);
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "%s\n", PASCAL_USAGE);
            return vmExitWithCleanup(EXIT_FAILURE);
        } else {
            // First non-option argument is the source file
            sourceFile = argv[i];
            programName = sourceFile; // Use actual filename for logs/dumps
            pscal_params_start_index = i + 1; // Next args are for the front end program
            break; // Stop parsing options, rest are program args
        }
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
            return vmExitWithCleanup(EXIT_FAILURE);
        }
    }


    if (!sourceFile) {
        fprintf(stderr, "Error: No source file specified.\n");
        fprintf(stderr, "%s\n", PASCAL_USAGE);
        return vmExitWithCleanup(EXIT_FAILURE);
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
        return vmExitWithCleanup(EXIT_FAILURE);
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
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    size_t bytes_read = fread(source_buffer, 1, fsize, file);
    if (bytes_read != (size_t)fsize) {
        fprintf(stderr, "Error reading source file '%s'\n", sourceFile);
        free(source_buffer);
        fclose(file);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    source_buffer[fsize] = '\0';
    fclose(file);

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
    int result = runProgram(source_buffer, programName, argv[0], dump_ast_json_flag, dump_bytecode_flag, dump_bytecode_only_flag);

    // Restore stderr and conditionally replay
    if (capture_stderr) {
        fflush(stderr);
        if (s_saved_stderr_fd != -1) { dup2(s_saved_stderr_fd, STDERR_FILENO); close(s_saved_stderr_fd); s_saved_stderr_fd = -1; }
        if (s_stderr_tmp) {
            rewind(s_stderr_tmp);
            // Scan buffer for non-whitespace or cached message
            int has_non_ws = 0, has_cached = 0; char buf[4096]; size_t n;
            while ((n = fread(buf, 1, sizeof(buf), s_stderr_tmp)) > 0) {
                for (size_t i = 0; i < n; i++) {
                    char c = buf[i];
                    if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) { has_non_ws = 1; break; }
                }
                if (!has_cached) {
                    if (memmem(buf, n, "Loaded cached byte code", 24) != NULL) has_cached = 1;
                }
                if (has_non_ws && has_cached) break;
            }
            // Decide replay: nonzero exit or cached or non-whitespace
            if (result != EXIT_SUCCESS || has_cached || has_non_ws) {
                rewind(s_stderr_tmp);
                while ((n = fread(buf, 1, sizeof(buf), s_stderr_tmp)) > 0) {
                    size_t total = 0;
                    while (total < n) {
                        ssize_t w = write(STDERR_FILENO, buf + total, n - total);
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

    free(source_buffer); // Free the source code buffer
    return vmExitWithCleanup(result);
}
