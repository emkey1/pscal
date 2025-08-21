#include "Pascal/lexer.h"
#include "Pascal/parser.h"
#include "Pascal/ast.h"
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
#ifdef SDL
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#endif
#include "vm/vm.h"
// Pascal/ast.h is already included via globals.h or directly, no need for duplicate

/* Global variables */
int gParamCount = 0;
char **gParamValues = NULL;

#ifdef DEBUG
List *inserted_global_names = NULL;
#endif

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "undefined.version_DEV"
#endif

const char *PSCAL_USAGE =
    "Usage: pscal <options> <source_file> [program_parameters...]\n"
    "   Options:\n"
    "     -v                          Display version.\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n"
    "   or: pscal (with no arguments to display version and usage)";

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
    
    procedure_table = createHashTable();
    if (!procedure_table) {
        fprintf(stderr, "FATAL: Failed to create procedure hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    current_procedure_table = procedure_table;
    DEBUG_PRINT("[DEBUG MAIN] Created procedure hash table %p.\n", (void*)procedure_table);
#ifdef SDL
    InitializeTextureSystem();
#endif
}

int runProgram(const char *source, const char *programName, int dump_ast_json_flag, int dump_bytecode_flag) {
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

    Lexer lexer;
    initLexer(&lexer, source);
    Parser parser;
    parser.lexer = &lexer;
    parser.current_token = getNextToken(&lexer);
    GlobalAST = buildProgramAST(&parser, &chunk);
    if (parser.current_token) { freeToken(parser.current_token); parser.current_token = NULL; }

    if (GlobalAST && GlobalAST->type == AST_PROGRAM) {
        annotateTypes(GlobalAST, NULL, GlobalAST);
        if (dump_ast_json_flag) {
            fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
            dumpASTJSON(GlobalAST, stdout);
            fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
            overall_success_status = true;
        } else {
            used_cache = loadBytecodeFromCache(programName, &chunk);
            bool compilation_ok_for_vm = true;
            if (!used_cache) {
                if (dump_bytecode_flag) {
                    fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
                }
                compilation_ok_for_vm = compileASTToBytecode(GlobalAST, &chunk);
                if (compilation_ok_for_vm) {
                    finalizeBytecode(&chunk);
                    saveBytecodeToCache(programName, &chunk);
                    fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n", chunk.count, chunk.constants_count);
                    if (dump_bytecode_flag) {
                        disassembleBytecodeChunk(&chunk, programName ? programName : "CompiledChunk", procedure_table);
                        fprintf(stderr, "\n--- Executing Program with VM ---\n");
                    }
                }
            } else if (dump_bytecode_flag) {
                disassembleBytecodeChunk(&chunk, programName ? programName : "CompiledChunk", procedure_table);
                fprintf(stderr, "\n--- Executing Program with VM (cached) ---\n");
            }

            if (compilation_ok_for_vm) {
                VM vm;
                initVM(&vm);
                InterpretResult result_vm = interpretBytecode(&vm, &chunk, globalSymbols, procedure_table);
                freeVM(&vm);
                globalSymbols = NULL;
                if (result_vm == INTERPRET_OK) {
                    overall_success_status = true;
                } else {
                    fprintf(stderr, "--- VM Execution Failed (%s) ---\n",
                            result_vm == INTERPRET_RUNTIME_ERROR ? "Runtime Error" : "Compile Error (VM stage)");
                    overall_success_status = false;
                    vmDumpStackInfo(&vm);
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

    freeBytecodeChunk(&chunk);
    freeProcedureTable();
    freeTypeTableASTNodes();
    freeTypeTable();
    if (globalSymbols) {
        freeHashTable(globalSymbols);
        globalSymbols = NULL;
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
    SdlCleanupAtExit();
#endif
    return overall_success_status ? EXIT_SUCCESS : EXIT_FAILURE;
}

// Consistent main function structure for argument parsing
int main(int argc, char *argv[]) {
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    const char *sourceFile = NULL;
    const char *programName = argv[0]; // Default program name to executable name
    int pscal_params_start_index = 0; // Will be set after source file is identified

    if (argc == 1) {
        printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
        printf("%s\n", PSCAL_USAGE);
        return EXIT_SUCCESS;
    }

    // Parse options first
    int i = 1;
    for (; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) {
            printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            fprintf(stderr, "%s\n", PSCAL_USAGE);
            return EXIT_FAILURE;
        } else {
            // First non-option argument is the source file
            sourceFile = argv[i];
            programName = sourceFile; // Use actual filename for logs/dumps
            pscal_params_start_index = i + 1; // Next args are for the Pscal program
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
            return EXIT_FAILURE;
        }
    }


    if (!sourceFile) {
        fprintf(stderr, "Error: No source file specified.\n");
        fprintf(stderr, "%s\n", PSCAL_USAGE);
        return EXIT_FAILURE;
    }

    // Initialize core systems
    initSymbolSystem();

    char *source_buffer = NULL;
    FILE *file = fopen(sourceFile, "r");
    if (!file) {
        perror("Error opening source file");
        // Minimal cleanup if initSymbolSystem did very little before this point
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);
    source_buffer = malloc(fsize + 1);
    if (!source_buffer) {
        fprintf(stderr, "Memory allocation error reading file\n");
        fclose(file);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }
    fread(source_buffer, 1, fsize, file);
    source_buffer[fsize] = '\0';
    fclose(file);

    // Set up Pscal program's command-line parameters
    if (pscal_params_start_index < argc) {
        gParamCount = argc - pscal_params_start_index;
        gParamValues = &argv[pscal_params_start_index];
    } else {
        gParamCount = 0;
        gParamValues = NULL;
    }

    // Call runProgram
    int result = runProgram(source_buffer, programName, dump_ast_json_flag, dump_bytecode_flag);
    free(source_buffer); // Free the source code buffer

    return result;
}
