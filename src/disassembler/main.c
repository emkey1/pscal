#include "core/cache.h"
#include "core/utils.h"
#include "core/list.h"
#include "vm/vm.h"
#include "Pascal/globals.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include "compiler/bytecode.h"
#include <stdio.h>
#include <stdlib.h>

int gParamCount = 0;
char **gParamValues = NULL;

/* Initialize symbol tables needed for bytecode loading and disassembly. */
static void initSymbolSystem(void) {
#ifdef DEBUG
    inserted_global_names = createList();
#endif
    globalSymbols = createHashTable();
    if (!globalSymbols) {
        fprintf(stderr, "FATAL: Failed to create global symbol hash table.\n");
        EXIT_FAILURE_HANDLER();
    }

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
#ifdef SDL
    initializeTextureSystem();
#endif
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: disassembler <bytecode_file>\n");
        return EXIT_FAILURE;
    }

    const char* path = argv[1];
    initSymbolSystem();
    registerAllBuiltins();

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    if (!loadBytecodeFromFile(path, &chunk)) {
        fprintf(stderr, "Failed to load bytecode from %s\n", path);
        return EXIT_FAILURE;
    }

    disassembleBytecodeChunk(&chunk, path, procedure_table);

    freeBytecodeChunk(&chunk);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);
    return EXIT_SUCCESS;
}

