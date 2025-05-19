#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "utils.h"
#include "list.h"
// Include globals.h first, which includes necessary headers and forward declarations.
#include "globals.h" // This should include symbol.h and define HashTable

// Interpreter builtins and execution functions.
#include "interpreter.h"
#include "builtin.h"

// Bytecode stuff
#include "compiler/bytecode.h"
#include "compiler/compiler.h"

// Standard C libraries.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // For getpid
#include <sys/stat.h> // For stat and mkdir
#include <time.h>   // For time (for srand)
#include <string.h> // For strrchr, strcmp

// SDL and SDL_ttf libraries (keep these if used directly in main for cleanup or init)
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
// Note: No need for a separate "#include "sdl.h"" if globals.h or its includes handle it


/* Global variables */
// These are declared as extern in globals.h and defined here.
// gParamCount and gParamValues are used for command-line argument handling.
int gParamCount = 0;
char **gParamValues = NULL;

// globalRoot is often used to hold the root of the parsed AST.
// It might be declared as extern in a header if accessed from other files.
// extern AST *globalRoot; // Example: If globalRoot is accessed elsewhere.

#ifdef DEBUG
// In DEBUG mode, this list might be used by symbol table debugging utilities.
List *inserted_global_names = NULL; // Keep this if used by other DEBUG utilities
#endif

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "undefined.version_DEV" // Fallback version
#endif

const char *PSCAL_USAGE =
    "Usage: pscal <source_file> [program_parameters...]\n"
    "   or: pscal -v (to display version)\n"
    "   or: pscal --dump-ast-json <source_file>\n"
    "   or: pscal (with no arguments to display version and usage)";

// Function to initialize core systems like symbol tables and SDL.
// This function should be called early in main, before parsing or execution.
void initSymbolSystem(void) {
#ifdef DEBUG
    // Keep existing DEBUG list initialization if it's used by debug features.
    // This list might track names inserted globally for debugging purposes.
    inserted_global_names = createList(); // Assumes createList() is available and defined in list.h/list.c
#endif

    // <<< Create the global symbol hash table >>>
    // Allocate memory for the global hash table structure which will hold global symbols.
    globalSymbols = createHashTable(); // Assumes createHashTable is defined in symbol.h/symbol.c
    // Check if the hash table creation failed (e.g., due to memory allocation failure).
    if (!globalSymbols) {
        fprintf(stderr, "FATAL: Failed to create global symbol hash table.\n");
        // Terminate the program immediately if a critical resource cannot be created.
        EXIT_FAILURE_HANDLER(); // Assumes EXIT_FAILURE_HANDLER is defined in globals.h
    }
    DEBUG_PRINT("[DEBUG MAIN] Created global symbol table %p.\n", (void*)globalSymbols); // Assumes DEBUG_PRINT is defined in utils.h
    
    procedure_table = createHashTable();
    if (!procedure_table) {
        fprintf(stderr, "FATAL: Failed to create procedure hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    DEBUG_PRINT("[DEBUG MAIN] Created procedure hash table %p.\n", (void*)procedure_table);

    // Initialize the texture system used by SDL graphics builtins.
    // This typically initializes the global gSdlTextures array to NULLs.
    InitializeTextureSystem(); // Assumes InitializeTextureSystem is defined in sdl.h/sdl.c

    // Add other system initializations here if needed early in the program lifecycle.
    // For example, if audio needs early initialization regardless of InitSoundSystem call.
    // Audio_InitSystem(); // Currently called by InitSoundSystem builtin, might not be needed here.
}


/*
 * execute_with_ast_dump - Dumps the full AST to a file then executes the program.
 * This function is typically used in DEBUG mode to visualize the parsed AST.
 *
 * @param program_ast A pointer to the root AST node of the parsed program.
 * @param program_name The name of the program (used for the dump filename).
 */
void executeWithASTDump(AST *program_ast, const char *program_name) {
    struct stat st = {0};
    // Create a directory for debug dumps if it doesn't exist.
    if (stat("/tmp/pscal", &st) == -1) {
        if (mkdir("/tmp/pscal", 0777) != 0) {
            perror("mkdir /tmp/pscal failed");
            // Decide if this is a fatal error for program execution or just skip the dump.
            // Let's allow execution to continue without the dump if mkdir fails.
            // EXIT_FAILURE_HANDLER(); // Consider if this should be fatal.
             fprintf(stderr, "Warning: Could not create /tmp/pscal for AST dump. AST dump skipped.\n");
             // If mkdir failed, proceed with program execution without dumping the AST.
             executeWithScope(program_ast, true); // Execute the program anyway. Assumes executeWithScope is defined in interpreter.h/interpreter.c
             return; // Exit executeWithASTDump function.
        }
    }

    // Get the process ID to make the dump filename unique.
    pid_t pid = getpid();
    char filename[256]; // Buffer to store the dump file path.

    // Determine the base name of the program file for the dump filename.
    // Find the last occurrence of '/' to get the filename part.
    const char *base = strrchr(program_name, '/');
    if (base)
        base++; // Move past the last slash if found.
    else
        base = program_name; // If no slash, use the entire program_name.

    // Create the full dump file path string.
    // snprintf returns the number of characters that *would* have been written
    // if the buffer was large enough, excluding the null terminator.
    // Check against sizeof(filename) to detect truncation and potential issues.
    int chars_written = snprintf(filename, sizeof(filename), "/tmp/pscal/%s.%d.ast", base, (int)pid);

    // Check if snprintf resulted in truncation or an error.
    if (chars_written < 0 || chars_written >= sizeof(filename)) {
         fprintf(stderr, "Warning: AST dump filename is too long or invalid. AST dump skipped.\n");
         // Execute the program even if dump filename creation failed.
         executeWithScope(program_ast, true); // Assumes executeWithScope is defined.
         return; // Exit executeWithASTDump.
    }


    // Open the AST dump file for writing in binary mode ("wb").
    FILE *f = fopen(filename, "wb"); // Using "wb" for binary compatibility, though dumping text.
    if (!f) {
        perror("fopen for AST dump failed");
        // Decide if this is a fatal error for program execution.
        // Let's allow execution to continue without the dump if opening the file fails.
        // EXIT_FAILURE_HANDLER(); // Consider if this should be fatal.
         fprintf(stderr, "Warning: Could not open AST dump file '%s'. AST dump skipped.\n", filename);
         executeWithScope(program_ast, true); // Assumes executeWithScope is defined.
         return; // Exit executeWithASTDump.
    }

    // Temporarily redirect stdout to the dump file for the dumping process.
    FILE *old_stdout = stdout;
    stdout = f;

    // Dump the AST structure to the file.
    // debugASTFile is assumed to recursively print the AST structure.
    debugASTFile(program_ast); // Assumes debugASTFile is defined in utils.h/utils.c

    // Restore stdout.
    stdout = old_stdout;

    // Close the AST dump file.
    fclose(f);

    // Execute the program after the AST has been successfully dumped (or attempted).
    executeWithScope(program_ast, true); // Assumes executeWithScope is defined.
}


/*
 * runProgram - Common routine to register built-ins, parse, and execute the program.
 * This function assumes that initSymbolSystem() has been called BEFOREHAND
 * in the main function to create the necessary global symbol table hash table.
 *
 * @param source      The source code string of the Pscal program.
 * @param programName The name of the program (used for messages/dump).
 * @return EXIT_SUCCESS if parsing and execution complete without fatal errors handled by EXIT_FAILURE_HANDLER, EXIT_FAILURE if parsing fails.
 */
int runProgram(const char *source, const char *programName, int dump_ast_json_flag) {
    // Ensure the globalSymbols hash table exists before proceeding.
    // It should have been created by initSymbolSystem() called in main.
    if (globalSymbols == NULL) {
        fprintf(stderr, "Internal error: globalSymbols hash table is NULL at the start of runProgram.\n");
        EXIT_FAILURE_HANDLER(); // This indicates a critical internal setup problem.
    }


    /* Register built-in functions and procedures. */
    // This populates the procedure_table with dummy AST nodes representing builtins.
    // The registerBuiltinFunction should correctly configure these dummy nodes
    // based on the builtin's parameters and return type.
    registerBuiltinFunction("cos",       AST_FUNCTION_DECL, NULL); // Assumes AST_FUNCTION_DECL and AST_PROCEDURE_DECL are defined in ast.h
    registerBuiltinFunction("sin",       AST_FUNCTION_DECL, NULL); // Assumes registerBuiltinFunction is defined in builtin.h/builtin.c
    registerBuiltinFunction("tan",       AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("sqrt",      AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("ln",        AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("exp",       AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("abs",       AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("assign",    AST_PROCEDURE_DECL, NULL); // File I/O: Assigns a filename to a file variable.
    registerBuiltinFunction("pos",       AST_FUNCTION_DECL, NULL); // String: Finds substring position.
    registerBuiltinFunction("close",     AST_PROCEDURE_DECL, NULL); // File I/O: Closes a file.
    registerBuiltinFunction("copy",      AST_FUNCTION_DECL, NULL); // String: Copies substring.
    registerBuiltinFunction("halt",      AST_PROCEDURE_DECL, NULL); // System: Exits program.
    registerBuiltinFunction("inc",       AST_PROCEDURE_DECL, NULL); // System: Increments ordinal variable.
    registerBuiltinFunction("ioresult",  AST_FUNCTION_DECL, NULL); // File I/O: Returns status of last I/O op.
    registerBuiltinFunction("length",    AST_FUNCTION_DECL, NULL); // String: Returns length of string.
    registerBuiltinFunction("randomize", AST_PROCEDURE_DECL, NULL); // System: Seeds random number generator.
    registerBuiltinFunction("random",    AST_FUNCTION_DECL, NULL); // System: Generates random number.
    registerBuiltinFunction("reset",     AST_PROCEDURE_DECL, NULL); // File I/O: Opens file for reading.
    registerBuiltinFunction("rewrite",   AST_PROCEDURE_DECL, NULL); // File I/O: Opens file for writing (truncates).
    registerBuiltinFunction("trunc",       AST_FUNCTION_DECL, NULL); // Math: Truncates Real to Integer.
    registerBuiltinFunction("chr",       AST_FUNCTION_DECL, NULL); // Ordinal: Converts integer ordinal to char.
    registerBuiltinFunction("ord",       AST_FUNCTION_DECL, NULL); // Ordinal: Returns ordinal value.
    registerBuiltinFunction("upcase",    AST_FUNCTION_DECL, NULL); // String: Converts character to uppercase.
    registerBuiltinFunction("mstreamcreate", AST_FUNCTION_DECL, NULL); // Memory Stream: Creates a memory stream object.
    registerBuiltinFunction("mstreamloadfromfile", AST_PROCEDURE_DECL, NULL); // Memory Stream: Loads from file into stream.
    registerBuiltinFunction("mstreamsavetofile", AST_PROCEDURE_DECL, NULL); // Memory Stream: Saves stream to file.
    registerBuiltinFunction("mstreamfree", AST_PROCEDURE_DECL, NULL); // Memory Stream: Frees a memory stream object.
    registerBuiltinFunction("api_send",  AST_FUNCTION_DECL, NULL); // Networking: Sends API request.
    registerBuiltinFunction("api_receive", AST_FUNCTION_DECL, NULL); // Networking: Receives API response.
    registerBuiltinFunction("paramcount", AST_FUNCTION_DECL, NULL); // CmdLine: Returns number of args.
    registerBuiltinFunction("paramstr",  AST_FUNCTION_DECL, NULL); // CmdLine: Returns a specific arg string.
    registerBuiltinFunction("readkey",   AST_FUNCTION_DECL, NULL); // Terminal IO: Reads a single key press.
    registerBuiltinFunction("delay",     AST_PROCEDURE_DECL, NULL); // System: Pauses execution.
    registerBuiltinFunction("keypressed", AST_FUNCTION_DECL, NULL); // Terminal IO: Checks if a key is in buffer.
    registerBuiltinFunction("low",       AST_FUNCTION_DECL, NULL); // Ordinal: Returns lowest value of a type.
    registerBuiltinFunction("high",      AST_FUNCTION_DECL, NULL); // Ordinal: Returns highest value of a type.
    registerBuiltinFunction("succ",      AST_FUNCTION_DECL, NULL); // Ordinal: Returns successor of ordinal value.
    registerBuiltinFunction("sqr",       AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("inttostr",  AST_FUNCTION_DECL, NULL); // Conversion: Integer to String.
    registerBuiltinFunction("screencols", AST_FUNCTION_DECL, NULL); // Terminal IO: Get terminal width.
    registerBuiltinFunction("screenrows", AST_FUNCTION_DECL, NULL); // Terminal IO: Get terminal height.
    registerBuiltinFunction("dec",       AST_PROCEDURE_DECL, NULL); // System: Decrements ordinal variable.
    registerBuiltinFunction("textcolore", AST_PROCEDURE_DECL, NULL); // Terminal IO: Set foreground color (256).
    registerBuiltinFunction("textbackgrounde", AST_PROCEDURE_DECL, NULL); // Terminal IO: Set background color (256).
    registerBuiltinFunction("new",       AST_PROCEDURE_DECL, NULL); // Memory: Allocates memory for pointer.
    registerBuiltinFunction("dispose",   AST_PROCEDURE_DECL, NULL); // Memory: Frees memory for pointer.
    registerBuiltinFunction("round",     AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("real",      AST_FUNCTION_DECL, NULL);  // Convert value to type real
    registerBuiltinFunction("RealToStr", AST_FUNCTION_DECL, NULL);

    // SDL Graphics built-ins
    registerBuiltinFunction("initgraph", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("closegraph", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("graphloop", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("getmaxx", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("getmaxy", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("setcolor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("putpixel", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("updatescreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("drawrect", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("waitkeyevent", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("ClearDevice", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("setrgbcolor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("drawline", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("fillcircle", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("fillrect", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("drawcircle", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("rendercopyex", AST_PROCEDURE_DECL, NULL);

    // SDL_ttf built-ins
    registerBuiltinFunction("inittextsystem", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("outtextxy", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("quittextsystem", AST_PROCEDURE_DECL, NULL);

    // SDL Misc Built-ins
    registerBuiltinFunction("quitrequested", AST_FUNCTION_DECL, NULL);

    // Mouse
    registerBuiltinFunction("getmousestate", AST_PROCEDURE_DECL, NULL);

    // Texture built-ins
    registerBuiltinFunction("createtexture", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("destroytexture", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("updatetexture", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("rendercopy", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("rendercopyrect", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("setAlphaBlend", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("createTargetTexture", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("RenderTextToTexture", AST_FUNCTION_DECL, NULL);

    // SDL Sound subsystem
    registerBuiltinFunction("initsoundsystem",   AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("loadsound",         AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("playsound",         AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("freesound",         AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("quitsoundsystem",   AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("issoundplaying",    AST_FUNCTION_DECL, NULL);
    
    // SDL Misc
    registerBuiltinFunction("GetTicks", AST_FUNCTION_DECL, NULL);

    // Note: Write, Writeln, Read, Readln might be handled directly in interpreter.c
    // based on AST node type, rather than dispatched via the builtin table.
    // If they were registered here, they would need AST_PROCEDURE_DECL and correct parameter setup.
    
#ifdef DEBUG
    fprintf(stderr, "Completed all built-in registrations. About to init lexer.\n");
    fflush(stderr); // Ensure it prints immediately
#endif

    /* Initialize lexer and parser. */
    Lexer lexer; // Assumes Lexer struct and initLexer are defined in lexer.h/lexer.c
    initLexer(&lexer, source);

#ifdef DEBUG
    // Print debug message indicating the start of the parsing phase.
    printf("\n--- Build AST Before Execution START---\n");
#endif

    Parser parser; // Assumes Parser struct is defined in parser.h
    parser.lexer = &lexer;
    parser.current_token = getNextToken(&lexer); // Assumes getNextToken is defined in lexer.h/lexer.c

    // Build the Program AST. This function parses the source code and creates the AST structure.
    // It also handles Unit parsing and linking, which will use insertGlobalSymbol and addProcedure.
    AST *GlobalAST = buildProgramAST(&parser); // Assumes buildProgramAST is defined in parser.h/parser.c

    // Free the final token owned by the parser after parsing is complete.
    // The parser's current_token holds the token that caused parsing to stop (usually EOF).
    if (parser.current_token) {
        freeToken(parser.current_token); // Assumes freeToken is defined in utils.h/utils.c
        parser.current_token = NULL; // Prevent dangling pointer.
    }


    // Check if parsing was successful before proceeding to annotation and execution.
    // buildProgramAST should return a valid AST_PROGRAM node or NULL/error indicator on failure.
    if (GlobalAST && GlobalAST->type == AST_PROGRAM) {
        // Annotate the AST with type information after parsing.
        // This pass resolves types and performs static type checking.
        // It needs access to global symbols (including builtins) and registered types.
        annotateTypes(GlobalAST, NULL, GlobalAST); // Assumes annotateTypes is defined in ast.h/ast.c
        
        if (!dump_ast_json_flag) { // Only compile if not just dumping AST
            BytecodeChunk chunk;
            initBytecodeChunk(&chunk);
            fprintf(stderr, "--- Compiling AST to Bytecode ---\n");
            if (compileASTToBytecode(GlobalAST, &chunk)) {
                fprintf(stderr, "Compilation successful. Bytecode size: %d bytes, Constants: %d\n", chunk.count, chunk.constants_count);
                
                // <<< ADD THIS CALL TO THE DISASSEMBLER >>>
                #ifdef DEBUG // Only disassemble in debug builds, or if a flag is set
                if (dumpExec) { // Or some other flag to control verbosity
                    disassembleBytecodeChunk(&chunk, programName ? programName : "CompiledChunk");
                }
                #endif
                // <<< END OF ADDED CALL >>>

                // TODO: Eventually, pass 'chunk' to the VM for execution.
                // vmExecuteChunk(&chunk);
            } else {
                fprintf(stderr, "Compilation failed.\n");
            }
            freeBytecodeChunk(&chunk);
        }

#ifdef DEBUG
        // In DEBUG mode, verify the parent/child links in the AST for structural correctness.
        fprintf(stderr, "--- Verifying AST Links ---\n");
        if (verifyASTLinks(GlobalAST, NULL)) { // Assumes verifyASTLinks is defined in ast.h/ast.c
            fprintf(stderr, "--- AST Link Verification Passed ---\n");
        } else {
            fprintf(stderr, "--- AST Link Verification FAILED ---\n");
            // Decide if link failure should be fatal. It often indicates a parser bug.
            // EXIT_FAILURE_HANDLER(); // Consider if this should be fatal.
        }
        // Dump the final annotated AST for debugging the structure and types.
        DEBUG_DUMP_AST(GlobalAST, 0); // Assumes DEBUG_DUMP_AST is defined in utils.h (macro)
        printf("\n--- Build AST Before Execution END---\n");
        printf("\n--- Executing Program AST ---\n");
#endif

        if (dump_ast_json_flag) {
            printf("--- Dumping AST to JSON ---\n"); // Message to stderr if stdout is for JSON
            dumpASTJSON(GlobalAST, stdout); // Dump to stdout
            printf("\n--- AST JSON Dump Complete ---\n"); // Message to stderr
            // Clean up and exit, do not execute
            freeProcedureTable();
            freeTypeTableASTNodes();
            freeTypeTable();
            if (GlobalAST) freeAST(GlobalAST);
            SdlCleanupAtExit();
            if (globalSymbols) freeHashTable(globalSymbols);
#ifdef DEBUG
            if (inserted_global_names) freeList(inserted_global_names);
#endif
            return EXIT_SUCCESS; // Successfully dumped
        }
        
        // Seed the random number generator if the Pscal program doesn't call Randomize.
        // It's better practice to require the user program to call Randomize for control.
        // If you want a default seed, do it here or in initSymbolSystem.
        // srand((unsigned int)time(NULL)); // Assumes time is included from <time.h>


#ifdef DEBUG
        // Dump the final state of the symbol tables after parsing/annotation/builtin registration.
        printf(" ===== FINAL AST DUMP START =====\n");
        // Use dumpAST (to stdout/stderr) or debugASTFile (to a file) for the AST structure.
        // debugASTFile(GlobalAST, programName); // If you want to dump AST to file.
        dumpAST(GlobalAST, 0); // If you want to dump AST to stderr/console. Assumes dumpAST is defined in ast.h/ast.c
        printf(" ===== FINAL AST DUMP END =====\n");

        // Dump symbol tables after parsing/annotation/registration.
        dumpSymbolTable(); // Assumes dumpSymbolTable is defined in utils.h/symbol.c
#endif

        // Execute the parsed program AST.
        // The initial call starts execution from the root of the AST (the PROGRAM node).
        // The initial scope is the global scope (is_global_scope = true).
        executeWithScope(GlobalAST, true); // Assumes executeWithScope is defined in interpreter.h/interpreter.c

    } else {
        // If parsing failed (buildProgramAST returned NULL or an invalid AST).
        fprintf(stderr, "Failed to build Program AST. Execution aborted.\n");
        // The parser's errorParser should ideally handle printing specific parser errors.
        // This message is a fallback if buildProgramAST returns NULL or an invalid AST type.
        // No AST to free if it returned NULL. Other tables might need freeing if partially populated.
        // The cleanup section below will handle freeing global resources like symbol tables.
    }

    // --- Cleanup: Free dynamically allocated interpreter resources. ---
    // Ensure memory allocated for AST nodes, symbol tables, type table,
    // procedure table, SDL/Audio resources, etc., is released to prevent memory leaks.
    // The order of cleanup needs careful consideration based on resource ownership.

    // The main GlobalAST structure owns most AST nodes, but some are owned by
    // the type_table (type definitions) and procedure_table (copied proc declarations).
    // Symbol table entries own their Value structs. Value structs own their contents (strings, records, arrays...).
    // HashTables own their Symbol struct nodes.

    // Suggested Cleanup Order (ensure your free functions handle NULL pointers safely):

    // 1. Free Procedure Table: Frees the Procedure structs and the copied/dummy AST nodes they own.
    freeProcedureTable(); // Assumes this function exists and frees Procedure structs and their owned ASTs.

    // 2. Free AST nodes owned by the Type Table: These are the actual type definition ASTs (RECORD_TYPE, ENUM_TYPE, etc.).
    freeTypeTableASTNodes(); // Assumes this function exists and frees ASTs pointed to by TypeEntry->typeAST.

    // 3. Free the Type Table structure itself: Frees the list of TypeEntry structs and their names.
    freeTypeTable(); // Assumes this function exists and frees TypeEntry structs and their names.

    // 4. Free the main Program AST: Recursively frees the rest of the AST nodes.
    //    The freeAST function should be designed to SKIP nodes that are owned by
    //    the type_table or procedure_table to avoid double freeing.
    if (GlobalAST) { // Check if parsing was successful and GlobalAST is not NULL
        freeAST(GlobalAST); // Assumes freeAST exists and handles recursive freeing and ownership checks.
        GlobalAST = NULL; // Set to NULL after freeing to prevent dangling pointer.
    } else {
        // If parsing failed (GlobalAST is NULL), no main AST structure to free.
    }

    // 5. Free SDL/Audio resources: Clean up SDL, SDL_mixer, SDL_ttf systems and associated global state (windows, renderers, fonts, loaded sounds, textures).
    SdlCleanupAtExit(); // Assumes this exists and cleans up SDL/SDL_mixer/SDL_ttf systems and global state.

    // 6. Free global symbol table: Frees the HashTable structure and all Symbol structs within its buckets,
    //    including the Value structs owned by Symbols (and their contents).
    //    The local symbol table (localSymbols) should be NULL at this point
    //    if popLocalEnv was correctly called for the outermost scope.
    if (globalSymbols) { // Check if the global table was successfully created
        freeHashTable(globalSymbols); // This frees Symbols and their Values within the global table
        globalSymbols = NULL; // Set to NULL after freeing.
    }

    // Local symbol table (localSymbols) should be NULL if the scope exit handling is correct.
    // If there's a possibility localSymbols is not NULL here, a final freeHashTable(localSymbols)
    // might be needed, but the scope-based popLocalEnv is the intended mechanism.


#ifdef DEBUG
    // Free debug-only resources that might have been allocated.
    if (inserted_global_names) { // Check if the debug list was created
        freeList(inserted_global_names); // Assumes freeList frees nodes and string values.
        inserted_global_names = NULL;
    }
#endif


    // Return the result status from runProgram (typically EXIT_SUCCESS or EXIT_FAILURE).
    // If EXIT_FAILURE_HANDLER() was called, the program would have already exited.
    return GlobalAST ? EXIT_SUCCESS : EXIT_FAILURE; // Return failure status if parsing failed.
}

#ifdef DEBUG
/* Debug mode main */
int main(int argc, char *argv[]) {
    int dump_ast_json_flag = 0;
    const char *sourceFile = NULL;
    const char *programName = "pscal_program"; // Default program name
    int pscal_params_start_index = argc; // Initialize to an out-of-bounds index

    if (argc == 1) {
        printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
        printf("%s\n", PSCAL_USAGE);
        printf("Debug mode options:\n");
        printf("  pscal_gcc <source_file> [program_params...]\n");
        printf("  pscal_gcc --dump-ast-json <source_file> [program_params...]\n");
        printf("  pscal_gcc -v\n");
        return EXIT_SUCCESS;
    }

    // --- Argument Parsing Logic ---
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) {
            printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else {
            // The first non-flag argument is assumed to be the source file
            if (sourceFile == NULL) {
                sourceFile = argv[i];
                programName = sourceFile; // Use actual filename
                pscal_params_start_index = i + 1; // Pscal params would start after this
            }
            // Subsequent non-flag arguments are considered Pscal program parameters,
            // handled by the gParamCount/gParamValues logic below.
        }
    }

    // Validate that a source file was provided if we are not just printing version
    if (!sourceFile) {
        fprintf(stderr, "Error: No source file specified.\n");
        fprintf(stderr, "Usage (DEBUG mode): %s [--dump-ast-json] <source_file> [params...]\n", argv[0]);
        fprintf(stderr, "Or: %s -v\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Initialize core systems
    initSymbolSystem();

    char *source = NULL;

    // Read source file
    FILE *file = fopen(sourceFile, "r");
    if (!file) {
        perror("Error opening source file");
        // (Minimal cleanup if initSymbolSystem did very little before this point)
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);
    source = malloc(fsize + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation error reading file\n");
        fclose(file);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }
    fread(source, 1, fsize, file);
    source[fsize] = '\0';
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
    int result = runProgram(source, programName, dump_ast_json_flag);
    free(source); // Free the source code buffer

    // runProgram now handles most of the cleanup, including SDL if it was used.
    // The cleanup sequence in runProgram ensures things are freed in the correct order.

    return result;
}

#else /* Non-DEBUG build - main function */

int main(int argc, char *argv[]) {
    int dump_ast_json_flag = 0;
    char *sourceFile = NULL;
    int pscal_params_start_index = argc; // Initialize to an out-of-bounds index

    if (argc == 1) {
        printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
        printf("%s\n", PSCAL_USAGE);
        printf("Release mode options:\n");
        printf("  pscal <source_file> [program_params...]\n");
        printf("  pscal --dump-ast-json <source_file> [program_params...]\n");
        printf("  pscal -v\n");
        return EXIT_SUCCESS;
    }

    // Argument Parsing Logic (similar to DEBUG version)
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-v") == 0) {
            printf("Pscal Interpreter Version: %s\n", PROGRAM_VERSION);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else {
            if (sourceFile == NULL) {
                sourceFile = argv[i];
                pscal_params_start_index = i + 1;
            }
        }
    }

    if (!sourceFile) {
        fprintf(stderr, "Error: No source file specified.\n");
        fprintf(stderr, "Usage: %s [--dump-ast-json] <source_file> [parameters...]\n", argv[0]);
        fprintf(stderr, "Or: %s -v\n", argv[0]);
        return EXIT_FAILURE;
    }

    initSymbolSystem();

    FILE *file = fopen(sourceFile, "r");
    if (!file) {
        perror("Error opening source file");
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }

    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);
    char *source = malloc(fsize + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation error reading file\n");
        fclose(file);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }
    fread(source, 1, fsize, file);
    source[fsize] = '\0';
    fclose(file);

    if (pscal_params_start_index < argc) {
        gParamCount = argc - pscal_params_start_index;
        gParamValues = &argv[pscal_params_start_index];
    } else {
        gParamCount = 0;
        gParamValues = NULL;
    }

    int result = runProgram(source, sourceFile, dump_ast_json_flag);
    free(source);

    return result;
}

#endif /* Non-DEBUG build */
