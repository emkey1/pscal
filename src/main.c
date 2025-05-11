#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "utils.h"
#include "list.h"
#include "globals.h" // Should include SDL.h and SDL_ttf.h via its own includes or directly
#include "interpreter.h"
#include "builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
// No need for a separate "#include "sdl.h"" if globals.h or types.h handles SDL includes

/* Global variables */
int gParamCount = 0;
char **gParamValues = NULL;

// This is only used in DEBUG mode, so it's fine inside the #ifdef
#ifdef DEBUG
List *inserted_global_names = NULL; // Keep this if used by other DEBUG utilities
void initSymbolSystem(void) {
    inserted_global_names = createList(); // Assuming createList() is available
    InitializeTextureSystem(); // Good place for one-time initializations like this too
}
#else
// For non-debug, still need to call InitializeTextureSystem if it's not in initSymbolSystem
// Alternatively, call it at the start of runProgram.
#endif

/*
 * execute_with_ast_dump - Dumps the full AST to a file then executes the program.
 * (Content of this function remains the same as your provided version)
 */
void executeWithASTDump(AST *program_ast, const char *program_name) {
    struct stat st = {0};
    if (stat("/tmp/pscal", &st) == -1) {
        if (mkdir("/tmp/pscal", 0777) != 0) {
            perror("mkdir");
            EXIT_FAILURE_HANDLER();
        }
    }

    pid_t pid = getpid();
    char filename[256];

    const char *base = strrchr(program_name, '/');
    if (base)
        base++;
    else
        base = program_name;

    snprintf(filename, sizeof(filename), "/tmp/pscal/%s.%d", base, (int)pid);

    FILE *f = fopen(filename, "w");
    if (!f) {
        perror("fopen");
        EXIT_FAILURE_HANDLER();
    }

    FILE *old_stdout = stdout;
    stdout = f;
    debugASTFile(program_ast); // Assumes this function exists and works
    stdout = old_stdout;
    fclose(f);

    executeWithScope(program_ast, true);
}

/*
 * runProgram - Common routine to register built-ins, parse, and execute the program.
 */
int runProgram(const char *source, const char *programName) {
    // Call one-time initializers here if not handled in main() or initSymbolSystem()
    // For non-DEBUG builds, ensure texture system (and potentially symbol system if needed globally) is initialized.
#ifndef DEBUG
    InitializeTextureSystem(); // Assuming this initializes gSdlTextures array to NULLs
#endif

    /* Register built-in functions/procedures. */
    // Corrected ASTNodeTypes for functions
    registerBuiltinFunction("cos",       AST_FUNCTION_DECL);
    registerBuiltinFunction("sin",       AST_FUNCTION_DECL);
    registerBuiltinFunction("tan",       AST_FUNCTION_DECL);
    registerBuiltinFunction("sqrt",      AST_FUNCTION_DECL);
    registerBuiltinFunction("ln",        AST_FUNCTION_DECL);
    registerBuiltinFunction("exp",       AST_FUNCTION_DECL);
    registerBuiltinFunction("abs",       AST_FUNCTION_DECL);
    registerBuiltinFunction("assign",    AST_PROCEDURE_DECL); // Assign is a file operation procedure
    registerBuiltinFunction("pos",       AST_FUNCTION_DECL);
    registerBuiltinFunction("close",     AST_PROCEDURE_DECL);
    registerBuiltinFunction("copy",      AST_FUNCTION_DECL);
    registerBuiltinFunction("halt",      AST_PROCEDURE_DECL);
    registerBuiltinFunction("inc",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("ioresult",  AST_FUNCTION_DECL);
    registerBuiltinFunction("length",    AST_FUNCTION_DECL);
    registerBuiltinFunction("randomize", AST_PROCEDURE_DECL);
    registerBuiltinFunction("random",    AST_FUNCTION_DECL); // Can be function (returns Real) or procedure (with Int param)
                                                           // For registration, if it can return a value, treat as FUNCTION.
                                                           // The C implementation (executeBuiltinRandom) handles 0 or 1 arg.
    registerBuiltinFunction("reset",     AST_PROCEDURE_DECL);
    registerBuiltinFunction("rewrite",   AST_PROCEDURE_DECL);
    registerBuiltinFunction("trunc",     AST_FUNCTION_DECL);
    registerBuiltinFunction("chr",       AST_FUNCTION_DECL);
    registerBuiltinFunction("ord",       AST_FUNCTION_DECL);
    registerBuiltinFunction("upcase",    AST_FUNCTION_DECL); // Corrected
    registerBuiltinFunction("mstreamcreate", AST_FUNCTION_DECL);
    registerBuiltinFunction("mstreamloadfromfile", AST_PROCEDURE_DECL);
    registerBuiltinFunction("mstreamsavetofile", AST_PROCEDURE_DECL);
    registerBuiltinFunction("mstreamfree", AST_PROCEDURE_DECL);
    registerBuiltinFunction("api_send",  AST_FUNCTION_DECL);
    registerBuiltinFunction("api_receive", AST_FUNCTION_DECL);
    registerBuiltinFunction("paramcount", AST_FUNCTION_DECL);
    registerBuiltinFunction("paramstr",  AST_FUNCTION_DECL);
    registerBuiltinFunction("readkey",   AST_FUNCTION_DECL); // Returns Char/String
    registerBuiltinFunction("delay",     AST_PROCEDURE_DECL); // Delay is a procedure
    registerBuiltinFunction("keypressed", AST_FUNCTION_DECL);
    registerBuiltinFunction("low",       AST_FUNCTION_DECL);
    registerBuiltinFunction("high",      AST_FUNCTION_DECL);
    registerBuiltinFunction("succ",      AST_FUNCTION_DECL); // pred would also be FUNCTION
    registerBuiltinFunction("sqr",       AST_FUNCTION_DECL); 
    registerBuiltinFunction("inttostr",  AST_FUNCTION_DECL);
    registerBuiltinFunction("screencols", AST_FUNCTION_DECL);
    registerBuiltinFunction("screenrows", AST_FUNCTION_DECL);
    registerBuiltinFunction("dec",       AST_PROCEDURE_DECL);
    // "succ" is already registered as FUNCTION_DECL above. If you have a separate "pred", register it too.
    registerBuiltinFunction("textcolore", AST_PROCEDURE_DECL);
    registerBuiltinFunction("textbackgrounde", AST_PROCEDURE_DECL);
    registerBuiltinFunction("new",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("dispose",   AST_PROCEDURE_DECL);

    // SDL Graphics built-ins
    registerBuiltinFunction("initgraph", AST_PROCEDURE_DECL);
    registerBuiltinFunction("closegraph", AST_PROCEDURE_DECL);
    registerBuiltinFunction("graphloop", AST_PROCEDURE_DECL);
    registerBuiltinFunction("getmaxx", AST_FUNCTION_DECL);
    registerBuiltinFunction("getmaxy", AST_FUNCTION_DECL);
    registerBuiltinFunction("setcolor", AST_PROCEDURE_DECL);      // Sets global color state
    registerBuiltinFunction("putpixel", AST_PROCEDURE_DECL);
    registerBuiltinFunction("updatescreen", AST_PROCEDURE_DECL);
    registerBuiltinFunction("drawrect", AST_PROCEDURE_DECL);
    registerBuiltinFunction("waitkeyevent", AST_PROCEDURE_DECL);
    registerBuiltinFunction("ClearDevice", AST_PROCEDURE_DECL); // Ensure case consistency if lexer is case sensitive for builtins
    registerBuiltinFunction("setrgbcolor", AST_PROCEDURE_DECL);
    registerBuiltinFunction("drawline", AST_PROCEDURE_DECL);
    registerBuiltinFunction("fillcircle", AST_PROCEDURE_DECL);
    registerBuiltinFunction("fillrect", AST_PROCEDURE_DECL);
    registerBuiltinFunction("drawcircle", AST_PROCEDURE_DECL);

    // SDL_ttf built-ins
    registerBuiltinFunction("inittextsystem", AST_PROCEDURE_DECL);
    registerBuiltinFunction("outtextxy", AST_PROCEDURE_DECL);
    registerBuiltinFunction("quittextsystem", AST_PROCEDURE_DECL);
    
    // SDL Misc Built-ins
    registerBuiltinFunction("quitrequested", AST_FUNCTION_DECL); 

    // Mouse
    registerBuiltinFunction("getmousestate", AST_PROCEDURE_DECL);

    // Texture built-ins
    registerBuiltinFunction("createtexture", AST_FUNCTION_DECL); // Returns an Integer ID
    registerBuiltinFunction("destroytexture", AST_PROCEDURE_DECL);
    registerBuiltinFunction("updatetexture", AST_PROCEDURE_DECL);
    registerBuiltinFunction("rendercopy", AST_PROCEDURE_DECL);
    registerBuiltinFunction("rendercopyrect", AST_PROCEDURE_DECL);
    
    // SDL Sound subsystem
    registerBuiltinFunction("initsoundsystem", AST_PROCEDURE_DECL);
    registerBuiltinFunction("loadsound",       AST_FUNCTION_DECL); // Function (returns Integer ID)
    registerBuiltinFunction("playsound",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("quitsoundsystem", AST_PROCEDURE_DECL);
    registerBuiltinFunction("freesound",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("issoundplaying",    AST_FUNCTION_DECL);

    /* Initialize lexer and parser. */
    Lexer lexer;
    initLexer(&lexer, source);

#ifdef DEBUG
    printf("\n--- Build AST Before Execution START---\n");
#endif

    Parser parser;
    parser.lexer = &lexer;
    parser.current_token = getNextToken(&lexer);
    AST *GlobalAST = buildProgramAST(&parser);

    if (GlobalAST) { // Only proceed if parsing was successful
        annotateTypes(GlobalAST, NULL, GlobalAST);
#ifdef DEBUG
        fprintf(stderr, "--- Verifying AST Links ---\n");
        if (verifyASTLinks(GlobalAST, NULL)) {
            fprintf(stderr, "--- AST Link Verification Passed ---\n");
        } else {
            fprintf(stderr, "--- AST Link Verification FAILED ---\n");
        }
        DEBUG_DUMP_AST(GlobalAST, 0);
        printf("\n--- Build AST Before Execution END---\n");
        printf("\n--- Executing Program AST ---\n");
#endif

        srand((unsigned int)time(NULL)); // Seed random number generator

#ifdef DEBUG
        printf(" ===== FINAL AST DUMP START =====\n");
        dumpAST(GlobalAST, 0);
        printf(" ===== FINAL AST DUMP END =====\n");
#endif
        executeWithASTDump(GlobalAST, programName); // Executes the program

#ifdef DEBUG
        dumpSymbolTable();
#endif
    } else {
        fprintf(stderr, "Failed to build Program AST. Execution aborted.\n");
        // No AST to free, but other tables might need freeing if partially populated
    }

    // Cleanup (ensure these are safe to call even if GlobalAST is NULL, e.g., they check if table is NULL)
    freeProcedureTable();
    if (GlobalAST) {
        freeAST(GlobalAST);
    }
    freeTypeTableASTNodes();
    freeTypeTable();

    return GlobalAST ? EXIT_SUCCESS : EXIT_FAILURE; // Return status based on parsing success
}

#ifdef DEBUG
/* Debug mode main */
int main(int argc, char *argv[]) {
    initSymbolSystem(); // Initializes texture system too in DEBUG

    char *source = NULL;
    const char *programName = "debug_program"; // Default for built-in source
    const char *debug_source =
    "program DefaultDebugSource;\n"
    "begin\n"
    "  writeln('Hello from default debug source!');\n"
    "end.\n";

    if (argc > 1) {
        const char *sourceFile = argv[1];
        FILE *file = fopen(sourceFile, "r");
        if (!file) {
            perror("Error opening source file");
            return EXIT_FAILURE;
        }
        fseek(file, 0, SEEK_END);
        long fsize = ftell(file);
        rewind(file);
        source = malloc(fsize + 1);
        if (!source) {
            fprintf(stderr, "Memory allocation error reading file\n");
            fclose(file);
            return EXIT_FAILURE;
        }
        fread(source, 1, fsize, file);
        source[fsize] = '\0';
        fclose(file);
        programName = sourceFile; // Use actual filename for AST dump
    } else if (debug_source && strlen(debug_source) > 0) {
        source = strdup(debug_source);
        if (!source) {
            fprintf(stderr, "Memory allocation error for debug_source\n");
            return EXIT_FAILURE;
        }
    } else {
        fprintf(stderr, "Usage (DEBUG mode): %s [<source_file.p>]\n", argv[0]);
        return EXIT_FAILURE;
    }

    // Set up command-line parameters if any beyond the source file
    if (argc > 2) {
        gParamCount = argc - 2;
        gParamValues = &argv[2];
    } else {
        gParamCount = 0;
        gParamValues = NULL;
    }

    int result = runProgram(source, programName);
    free(source);

    // SDL and Resource Cleanup
    if (gSdlFont) {
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
    }
    if (gSdlTtfInitialized) { // Use the specific flag for TTF system
        TTF_Quit();
        gSdlTtfInitialized = false;
    }
    for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
        if (gSdlTextures[i] != NULL) {
            SDL_DestroyTexture(gSdlTextures[i]);
            gSdlTextures[i] = NULL;
        }
    }
    if (gSdlInitialized) { // For core SDL_Init(VIDEO)
        // Renderer and Window are usually destroyed by CloseGraph built-in or before SDL_Quit
        if (gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
        if (gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
        SDL_Quit();
        gSdlInitialized = false;
    }

    return result;
}
#else
/* Normal main: read source file and pass it along. */
int main(int argc, char *argv[]) {
    // For non-debug, ensure InitializeTextureSystem is called if not in runProgram
    // Or ensure runProgram calls it. Let's assume runProgram handles necessary initializations.

    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source_file.p> [parameters...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *sourceFile = argv[1];
    // Use the actual source file name for programName if desired for AST dump
    // char *programNameForDump = sourceFile;
    char *programNameForDump = "PscalProgram"; // Or a generic name

    /* Set up command-line parameters */
    if (argc > 2) {
        gParamCount = argc - 2;
        gParamValues = &argv[2];
    } else {
        gParamCount = 0;
        gParamValues = NULL;
    }

    FILE *file = fopen(sourceFile, "r");
    if (!file) {
        perror("Error opening source file"); // Use perror for system errors
        return EXIT_FAILURE;
    }
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);
    char *source = malloc(fsize + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation error reading file\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    fread(source, 1, fsize, file);
    source[fsize] = '\0';
    fclose(file);

    int result = runProgram(source, programNameForDump);
    free(source);

    // SDL and Resource Cleanup (same as DEBUG path)
    if (gSdlFont) {
        TTF_CloseFont(gSdlFont);
        gSdlFont = NULL;
    }
    if (gSdlTtfInitialized) { // Use the specific flag
        TTF_Quit();
        gSdlTtfInitialized = false;
    }
    // Destroy any remaining SDL textures
    if (gSdlRenderer) { // Check if renderer was created implies textures might exist
        for (int i = 0; i < MAX_SDL_TEXTURES; ++i) {
            if (gSdlTextures[i] != NULL) {
                SDL_DestroyTexture(gSdlTextures[i]);
                gSdlTextures[i] = NULL;
            }
        }
    }
    // Explicitly destroy renderer and window if not handled by CloseGraph or similar
    if (gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
    if (gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }

    if (gSdlInitialized) { // For core SDL_Init(VIDEO)
        SDL_Quit();
        gSdlInitialized = false;
    }

    return result;
}
#endif
