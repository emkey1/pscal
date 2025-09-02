#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clike/parser.h"
#include "clike/codegen.h"
#include "clike/builtins.h"
#include "clike/semantics.h"
#include "clike/errors.h"
#include "clike/opt.h"
#include "clike/preproc.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"

int gParamCount = 0;
char **gParamValues = NULL;

int clike_error_count = 0;
int clike_warning_count = 0;

static void initSymbolSystemClike(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

static const char *CLIKE_USAGE =
    "Usage: clike <options> <source.cl> [program_parameters...]\n"
    "   Options:\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n";

int main(int argc, char **argv) {
    // Keep terminal untouched for clike: no raw mode or color push
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    const char *path = NULL;
    int clike_params_start = 0;

    if (argc == 1) {
        fprintf(stderr, "%s\n", CLIKE_USAGE);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s\n", argv[i], CLIKE_USAGE);
            return EXIT_FAILURE;
        } else {
            path = argv[i];
            clike_params_start = i + 1;
            break;
        }
    }

    if (!path) {
        fprintf(stderr, "Error: No source file specified.\n%s\n", CLIKE_USAGE);
        return EXIT_FAILURE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return EXIT_FAILURE; }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    char *src = (char*)malloc(len + 1);
    if (!src) { fclose(f); return EXIT_FAILURE; }
    size_t bytes_read = fread(src,1,len,f);
    if (bytes_read != (size_t)len) {
        fprintf(stderr, "Error reading source file '%s'\n", path);
        free(src);
        fclose(f);
        return EXIT_FAILURE;
    }
    src[len]='\0'; fclose(f);

    const char *defines[1];
    int define_count = 0;
#ifdef SDL
    defines[define_count++] = "SDL_ENABLED";
#endif
    char *pre_src = clikePreprocess(src, defines, define_count);

    ParserClike parser; initParserClike(&parser, pre_src ? pre_src : src);
    ASTNodeClike *prog = parseProgramClike(&parser);
    freeParserClike(&parser);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after parsing.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    if (dump_ast_json_flag) {
        fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
        dumpASTClikeJSON(prog, stdout);
        fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        return EXIT_SUCCESS;
    }

    if (clike_params_start < argc) {
        gParamCount = argc - clike_params_start;
        gParamValues = &argv[clike_params_start];
    }

    initSymbolSystemClike();
    clikeRegisterBuiltins();
    analyzeSemanticsClike(prog);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after semantic analysis.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }

    if (clike_warning_count > 0) {
        fprintf(stderr, "Compilation finished with %d warning(s).\n", clike_warning_count);
    }
    if (clike_error_count > 0) {
        fprintf(stderr, "Compilation halted with %d error(s).\n", clike_error_count);
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return clike_error_count > 255 ? 255 : clike_error_count;
    }
    prog = optimizeClikeAST(prog);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after optimization.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }

    BytecodeChunk chunk; clikeCompile(prog, &chunk);
    if (dump_bytecode_flag) {
        fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
        disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
        fprintf(stderr, "\n--- executing Program with VM ---\n");
    }

    VM vm; initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    freeASTClike(prog);
    clikeFreeStructs();
    free(src);
    if (pre_src) free(pre_src);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);
    return result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
