#include <stdio.h>
#include <stdlib.h>
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"
#include "rea/parser.h"

int gParamCount = 0;
char **gParamValues = NULL;

static void initSymbolSystem(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

int main(int argc, char **argv) {
    vmInitTerminalState();

    if (argc < 2) {
        fprintf(stderr, "Usage: rea <source.rea> [program_parameters...]\n");
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    const char *path = argv[1];
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

    AST *ast = parseRea(src);
    if (ast) {
        freeAST(ast);
    }

    // Placeholder: no compilation yet. Just run an empty chunk.
    initSymbolSystem();
    registerAllBuiltins();

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    writeBytecodeChunk(&chunk, OP_HALT, 0);

    if (argc > 2) {
        gParamCount = argc - 2;
        gParamValues = &argv[2];
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

