#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"
#include "rea/parser.h"
#include "rea/ast.h"

int gParamCount = 0;
char **gParamValues = NULL;

static void initSymbolSystem(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

static const char *REA_USAGE =
    "Usage: rea <options> <source.rea> [program_parameters...]\n"
    "   Options:\n"
    "     --dump-ast-json       Dump AST to JSON and exit.\n";

int main(int argc, char **argv) {
    vmInitTerminalState();

    int dump_ast_json = 0;
    int argi = 1;
    if (argc > argi && strcmp(argv[argi], "--dump-ast-json") == 0) {
        dump_ast_json = 1;
        argi++;
    }

    if (argc <= argi) {
        fprintf(stderr, "%s", REA_USAGE);
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    const char *path = argv[argi];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("open");
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *src = (char *)malloc(len + 1);
    if (!src) {
        fclose(f);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    size_t bytes_read = fread(src, 1, len, f);
    fclose(f);
    if (bytes_read != (size_t)len) {
        free(src);
        fprintf(stderr, "Error reading source file '%s'\n", path);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    src[len] = '\0';

    ReaAST *ast = parseRea(src);
    if (dump_ast_json) {
        reaDumpASTJSON(ast, stdout);
        reaFreeAST(ast);
        free(src);
        return vmExitWithCleanup(EXIT_SUCCESS);
    }
    if (ast) {
        reaFreeAST(ast);
    }

    // Placeholder: no compilation yet. Just run an empty chunk.
    initSymbolSystem();
    registerAllBuiltins();

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    writeBytecodeChunk(&chunk, OP_HALT, 0);

    if (argc > argi + 1) {
        gParamCount = argc - (argi + 1);
        gParamValues = &argv[argi + 1];
    }

    VM vm;
    initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);

    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);

    free(src);
    return vmExitWithCleanup(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE);
}

