#include "frontend/lexer.h"
#include "frontend/parser.h"
#include "frontend/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "core/list.h"
#include "globals.h"
#include "backend_ast/interpreter.h"
#include "backend_ast/builtin.h"
#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include "vm/vm.h"
// frontend/ast.h is already included via globals.h or directly, no need for duplicate

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
    "     --use-ast-interpreter       Use AST interpreter instead of VM.\n"
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
    DEBUG_PRINT("[DEBUG MAIN] Created procedure hash table %p.\n", (void*)procedure_table);
    InitializeTextureSystem();
}

void executeWithASTDump(AST *program_ast, const char *program_name) {
    // Your existing executeWithASTDump function body is fine
    // For brevity, I'm not repeating it here. Ensure it uses executeWithScope.
    // Example (ensure this is your actual correct logic):
    struct stat st = {0};
    if (stat("/tmp/pscal", &st) == -1) {
        if (mkdir("/tmp/pscal", 0777) != 0) {
            perror("mkdir /tmp/pscal failed");
            fprintf(stderr, "Warning: Could not create /tmp/pscal for AST dump. AST dump skipped.\n");
            executeWithScope(program_ast, true);
            return;
        }
    }
    pid_t pid = getpid();
    char filename[256];
    const char *base = strrchr(program_name, '/');
    if (base) base++; else base = program_name;
    int chars_written = snprintf(filename, sizeof(filename), "/tmp/pscal/%s.%d.ast", base, (int)pid);
    if (chars_written < 0 || (size_t)chars_written >= sizeof(filename)) {
         fprintf(stderr, "Warning: AST dump filename is too long or invalid. AST dump skipped.\n");
         executeWithScope(program_ast, true);
         return;
    }
    FILE *f = fopen(filename, "wb");
    if (!f) {
        perror("fopen for AST dump failed");
        fprintf(stderr, "Warning: Could not open AST dump file '%s'. AST dump skipped.\n", filename);
        executeWithScope(program_ast, true);
        return;
    }
    FILE *old_stdout = stdout;
    stdout = f;
    debugASTFile(program_ast);
    stdout = old_stdout;
    fclose(f);
    executeWithScope(program_ast, true);
}

int runProgram(const char *source, const char *programName, int dump_ast_json_flag, int use_ast_interpreter_flag) {
    if (globalSymbols == NULL) {
        fprintf(stderr, "Internal error: globalSymbols hash table is NULL at the start of runProgram.\n");
        EXIT_FAILURE_HANDLER();
    }

    /* Register built-in functions and procedures. */
    // (Keep all your registerBuiltinFunction calls here - they are extensive and seem fine)
    registerBuiltinFunction("cos",    AST_FUNCTION_DECL, NULL);
    // ... (all other built-ins) ...
    registerBuiltinFunction("GetTicks", AST_FUNCTION_DECL, NULL);
    
#ifdef DEBUG
    fprintf(stderr, "Completed all built-in registrations. About to init lexer.\n");
    fflush(stderr);
#endif

    Lexer lexer;
    initLexer(&lexer, source);

#ifdef DEBUG
    fprintf(stderr, "\n--- Build AST Before Execution START (stderr print)---\n");
#endif

    Parser parser;
    parser.lexer = &lexer;
    parser.current_token = getNextToken(&lexer);

    AST *GlobalAST = buildProgramAST(&parser);

    if (parser.current_token) {
        freeToken(parser.current_token);
        parser.current_token = NULL;
    }

    bool overall_success_status = false;

    if (GlobalAST && GlobalAST->type == AST_PROGRAM) {
        annotateTypes(GlobalAST, NULL, GlobalAST);

#ifdef DEBUG
        fprintf(stderr, "--- Verifying AST Links ---\n");
        if (verifyASTLinks(GlobalAST, NULL)) {
            fprintf(stderr, "--- AST Link Verification Passed ---\n");
        } else {
            fprintf(stderr, "--- AST Link Verification FAILED ---\n");
        }
        fprintf(stderr, "\n--- Build AST Before Execution END (stderr print)---\n");
#endif

        if (dump_ast_json_flag) {
            fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
            dumpASTJSON(GlobalAST, stdout);
            fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
            overall_success_status = true;
        } else if (use_ast_interpreter_flag) {
            fprintf(stderr, "\n--- Executing Program with AST Interpreter (selected by flag) ---\n");
#ifdef DEBUG
            if (dumpExec) { // Use your dumpExec flag for AST dump if running AST walker
                fprintf(stderr, " ===== FINAL AST DUMP START (Textual to stderr if AST walker selected) =====\n");
                dumpAST(GlobalAST, 0);
                fprintf(stderr, " ===== FINAL AST DUMP END (Textual to stderr) =====\n");
                dumpSymbolTable();
            }
#endif
            executeWithScope(GlobalAST, true);
            overall_success_status = true;
        } else {
            // --- DEFAULT: COMPILE TO BYTECODE AND EXECUTE WITH VM ---
            BytecodeChunk chunk;
            initBytecodeChunk(&chunk); // Initialize chunk here
            fprintf(stderr, "--- Compiling AST to Bytecode ---\n");
            bool compilation_ok_for_vm = compileASTToBytecode(GlobalAST, &chunk);
            
            if (compilation_ok_for_vm) {
                fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n", chunk.count, chunk.constants_count);
#ifdef DEBUG
                if (dumpExec) {
                    disassembleBytecodeChunk(&chunk, programName ? programName : "CompiledChunk", procedure_table);
                }
#endif
                fprintf(stderr, "\n--- Executing Program with VM ---\n");
                VM vm;
                initVM(&vm);
                InterpretResult result_vm = interpretBytecode(&vm, &chunk, globalSymbols, procedure_table); 
                freeVM(&vm);
                
                if (result_vm == INTERPRET_OK) {
                    fprintf(stderr, "--- VM Execution Finished Successfully ---\n");
                    overall_success_status = true;
                } else {
                    fprintf(stderr, "--- VM Execution Failed (%s) ---\n",
                            result_vm == INTERPRET_RUNTIME_ERROR ? "Runtime Error" : "Compile Error (VM stage)");
                    overall_success_status = false;
                }
            } else {
                fprintf(stderr, "Compilation to bytecode failed.\n");
                overall_success_status = false;
#ifdef DEBUG
                // Optional: Fallback to AST walker on compilation failure in DEBUG mode
                // if (dumpExec) { // Or some other condition
                //     fprintf(stderr, "\n--- Bytecode compilation failed. Attempting AST Walker Execution (DEBUG fallback) ---\n");
                //     executeWithScope(GlobalAST, true);
                //     overall_success_status = true; // Mark success if AST walker completes
                // }
#endif
            }
            freeBytecodeChunk(&chunk); // Free chunk resources after use
        }
    } else if (!dump_ast_json_flag) {
        fprintf(stderr, "Failed to build Program AST for execution.\n");
        overall_success_status = false;
    } else if (dump_ast_json_flag && (!GlobalAST || GlobalAST->type != AST_PROGRAM)) {
        fprintf(stderr, "Failed to build Program AST for JSON dump.\n");
        overall_success_status = false;
    }

    // --- Cleanup ---
    freeProcedureTable();
    freeTypeTableASTNodes();
    freeTypeTable();
    if (GlobalAST) {
        freeAST(GlobalAST);
        GlobalAST = NULL;
    }
    SdlCleanupAtExit();
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

    return overall_success_status ? EXIT_SUCCESS : EXIT_FAILURE;
}


// Consistent main function structure for argument parsing
int main(int argc, char *argv[]) {
    int dump_ast_json_flag = 0;
    int use_ast_interpreter_flag = 0;
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
        } else if (strcmp(argv[i], "--use-ast-interpreter") == 0) {
            use_ast_interpreter_flag = 1;
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
    int result = runProgram(source_buffer, programName, dump_ast_json_flag, use_ast_interpreter_flag);
    free(source_buffer); // Free the source code buffer

    return result;
}
