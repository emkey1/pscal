#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "types.h"
#include "utils.h"
#include "list.h"
#include "globals.h"
#include "interpreter.h"
#include "builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <string.h>

/* Global variables */
int gParamCount = 0;
char **gParamValues = NULL;

List *inserted_global_names = NULL;

#ifdef DEBUG
void initSymbolSystem(void) {
    inserted_global_names = createList();
}
#endif

/*
 * execute_with_ast_dump - Dumps the full AST to a file then executes the program.
 *
 * Parameters:
 * program_ast  - Pointer to the root of the AST for the parsed program.
 * program_name - Name of the Pascal program (used to build the dump filename).
 *
 * The dump file will be created at /tmp/pscal/[program_name].[pid]. The directory
 * is created if it doesn't exist.
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

    // Extract basename from program_name.
    const char *base = strrchr(program_name, '/');
    if (base)
        base++;  // Skip the slash.
    else
        base = program_name;

    // Build filename using the basename.
    snprintf(filename, sizeof(filename), "/tmp/pscal/%s.%d", base, (int)pid);

    FILE *f = fopen(filename, "w");
    if (!f) {
        perror("fopen");
        EXIT_FAILURE_HANDLER();
    }

    FILE *old_stdout = stdout;
    stdout = f;
    debugASTFile(program_ast);
    stdout = old_stdout;
    fclose(f);

    executeWithScope(program_ast, true);
}

/*
 * runProgram - Common routine to register built-ins, parse, and execute the program.
 *
 * Parameters:
 * source      - The Pascal source code to run.
 * programName - The program name (used for dump filename, etc.).
 *
 * Returns EXIT_SUCCESS on success.
 */
int runProgram(const char *source, const char *programName) {
    /* Register built-in functions/procedures. */
    registerBuiltinFunction("cos",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("sin",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("tan",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("sqrt",      AST_PROCEDURE_DECL);
    registerBuiltinFunction("ln",        AST_PROCEDURE_DECL);
    registerBuiltinFunction("exp",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("abs",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("assign",    AST_PROCEDURE_DECL);
    registerBuiltinFunction("pos",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("close",     AST_PROCEDURE_DECL);
    registerBuiltinFunction("copy",      AST_PROCEDURE_DECL);
    registerBuiltinFunction("halt",      AST_PROCEDURE_DECL);
    registerBuiltinFunction("inc",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("ioresult",  AST_FUNCTION_DECL);
    registerBuiltinFunction("length",    AST_PROCEDURE_DECL);
    registerBuiltinFunction("randomize", AST_PROCEDURE_DECL);
    registerBuiltinFunction("random",    AST_PROCEDURE_DECL);
    registerBuiltinFunction("reset",     AST_PROCEDURE_DECL);
    registerBuiltinFunction("rewrite",   AST_PROCEDURE_DECL);
    registerBuiltinFunction("trunc",     AST_PROCEDURE_DECL);
    registerBuiltinFunction("chr",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("ord",       AST_PROCEDURE_DECL);
    registerBuiltinFunction("upcase",    AST_PROCEDURE_DECL);
    registerBuiltinFunction("mstreamcreate", AST_FUNCTION_DECL);
    registerBuiltinFunction("mstreamloadfromfile", AST_PROCEDURE_DECL);
    registerBuiltinFunction("mstreamsavetofile", AST_PROCEDURE_DECL);
    registerBuiltinFunction("mstreamfree", AST_PROCEDURE_DECL);
    registerBuiltinFunction("api_send",  AST_FUNCTION_DECL);
    registerBuiltinFunction("api_receive", AST_FUNCTION_DECL);
    registerBuiltinFunction("paramcount", AST_FUNCTION_DECL);
    registerBuiltinFunction("paramstr",  AST_FUNCTION_DECL);
    registerBuiltinFunction("readkey",  AST_FUNCTION_DECL);
    registerBuiltinFunction("delay",  AST_FUNCTION_DECL);
    registerBuiltinFunction("keypressed",  AST_FUNCTION_DECL);
    registerBuiltinFunction("low",  AST_FUNCTION_DECL);
    registerBuiltinFunction("high", AST_FUNCTION_DECL);
    registerBuiltinFunction("succ", AST_FUNCTION_DECL);
    registerBuiltinFunction("inttostr",  AST_FUNCTION_DECL);
    registerBuiltinFunction("screencols", AST_FUNCTION_DECL);
    registerBuiltinFunction("screenrows", AST_FUNCTION_DECL);
    registerBuiltinFunction("dec", AST_PROCEDURE_DECL); 
    registerBuiltinFunction("succ", AST_FUNCTION_DECL);
    registerBuiltinFunction("textcolore", AST_PROCEDURE_DECL);
    registerBuiltinFunction("textbackgrounde", AST_PROCEDURE_DECL); 
    registerBuiltinFunction("new", AST_PROCEDURE_DECL);
    registerBuiltinFunction("dispose", AST_PROCEDURE_DECL);
    registerBuiltinFunction("initgraph", AST_PROCEDURE_DECL);
    registerBuiltinFunction("closegraph", AST_PROCEDURE_DECL);
    registerBuiltinFunction("graphloop", AST_PROCEDURE_DECL);

    /* Initialize lexer and parser. */
    Lexer lexer;
    initLexer(&lexer, source);

#ifdef DEBUG
    /* In debug builds, also dump AST to stdout for immediate feedback */
    printf("\n--- Build AST Before Execution START---\n");
#endif

    Parser parser;
    parser.lexer = &lexer;
    parser.current_token = getNextToken(&lexer);
    AST *GlobalAST = buildProgramAST(&parser);
    annotateTypes(GlobalAST, NULL, GlobalAST);
    //dumpAST(GlobalAST, 0);
    //return(0);
#ifdef DEBUG
fprintf(stderr, "--- Verifying AST Links ---\n");
if (verifyASTLinks(GlobalAST, NULL)) { // Initial call expects NULL parent for the root
    fprintf(stderr, "--- AST Link Verification Passed ---\n");
} else {
    fprintf(stderr, "--- AST Link Verification FAILED ---\n");
    // You might want to dump the AST here or exit if links are bad
    // dumpAST(GlobalAST, 0);
    // EXIT_FAILURE_HANDLER(); // Or just let it continue to see the free crash
}
#endif

#ifdef DEBUG
    DEBUG_DUMP_AST(GlobalAST, 0);
    printf("\n--- Build AST Before Execution END---\n");
    printf("\n--- Executing Program AST ---\n");
#endif

    /* Seed random number generator. */
    srand((unsigned int)time(NULL));
#ifdef DEBUG
#endif
    /* Always dump AST to file in /tmp/pscal, regardless of debug mode. */
#ifdef DEBUG
    printf(" ===== FINAL AST DUMP START =====\n");
    dumpAST(GlobalAST, 0);
    printf(" ===== FINAL AST DUMP END =====\n");
#endif
    executeWithASTDump(GlobalAST, programName);

#ifdef DEBUG
    dumpSymbolTable();
#endif

    freeProcedureTable(); // Original position

     // --- Free the main AST Tree FIRST ---
     freeAST(GlobalAST); // Original position

     // --- Free the shared AST nodes stored in the type table ---
     freeTypeTableASTNodes(); // Original position

     // --- Free the type table linked list itself ---
     freeTypeTable(); // Original position

     return EXIT_SUCCESS;
}

#ifdef DEBUG
/* Debug mode: If a filename is specified on the command line, read the file;
   otherwise, if debug_source is non-empty, use that; if neither, print usage info and exit. */
int main(int argc, char *argv[]) {

    initSymbolSystem();

    char *source = NULL;
    const char *programName = "debug_program";
    const char *debug_source =
    "program ArrayOfRecordsTest;"
    "type"
    "    TStudent = record"
    "        id: integer;"
    "        name: string;"
    "    end;"
    "var"
    "    students: array[1..2] of TStudent;"
    "    i: integer;"
    "begin"
    "    { Assign values to the array of records }"
    "    students[1].id := 101;"
    "    students[1].name := 'Alice';"
    "    students[2].id := 102;"
    "    students[2].name := 'Bob';"
    "    { Print out the student records }"
    "    for i := 1 to 2 do"
    "    begin"
    "        writeln('Student ', i, ': id = ', students[i].id, ', name = ', students[i].name);"
    "    end;"
    "end.";

    if (argc > 1) {
        // Filename provided on command line: read the file.
        const char *sourceFile = argv[1];
        FILE *file = fopen(sourceFile, "r");
        if (!file) {
            printf("Error opening source file %s\n", sourceFile);
            return EXIT_FAILURE;
        }
        fseek(file, 0, SEEK_END);
        long fsize = ftell(file);
        rewind(file);
        source = malloc(fsize + 1);
        if (!source) {
            fprintf(stderr, "Memory allocation error\n");
            fclose(file);
            return EXIT_FAILURE;
        }
        fread(source, 1, fsize, file);
        source[fsize] = '\0';
        fclose(file);
        programName = sourceFile;
    } else if (debug_source && strlen(debug_source) > 0) {
        // No filename provided; use debug_source.
        source = strdup(debug_source);
        if (!source) {
            fprintf(stderr, "Memory allocation error\n");
            return EXIT_FAILURE;
        }
    } else {
        // No source available.
        fprintf(stderr, "Usage (DEBUG mode): %s <source_file.p>\n", argv[0]);
        return EXIT_FAILURE;
    }

    int result = runProgram(source, programName);
    free(source);
    
    if (gSdlInitialized) {
        SDL_Quit();
    }
    
    return result;
}
#else
/* Normal main: read source file and pass it along. */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <source_file.p> [parameters...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    char *sourceFile = argv[1];
    char *JustFileName = "TestCase";

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
        printf("Error opening source file %s\n", sourceFile);
        return EXIT_FAILURE;
    }
    fseek(file, 0, SEEK_END);
    long fsize = ftell(file);
    rewind(file);
    char *source = malloc(fsize + 1);
    if (!source) {
        fprintf(stderr, "Memory allocation error\n");
        fclose(file);
        return EXIT_FAILURE;
    }
    fread(source, 1, fsize, file);
    source[fsize] = '\0';
    fclose(file);

    int result = runProgram(source, JustFileName);
    free(source);
    
    if (gSdlInitialized) {
        SDL_Quit();
    }
    
    return result;
}
#endif
