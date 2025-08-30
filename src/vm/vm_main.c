#include "core/cache.h"
#include "core/utils.h"
#include "core/list.h"
#include "vm/vm.h"
#include "globals.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int gParamCount = 0;
char **gParamValues = NULL;

/* Local copy of initSymbolSystem from main.c to set up tables. */
static void initSymbolSystem(void) {
#ifdef DEBUG
    inserted_global_names = createList();
#endif
    globalSymbols = createHashTable();
    if (!globalSymbols) {
        fprintf(stderr, "FATAL: Failed to create global symbol hash table.\n");
        EXIT_FAILURE_HANDLER();
    }

    procedure_table = createHashTable();
    if (!procedure_table) {
        fprintf(stderr, "FATAL: Failed to create procedure hash table.\n");
        EXIT_FAILURE_HANDLER();
    }
    current_procedure_table = procedure_table;
#ifdef SDL
    InitializeTextureSystem();
#endif
}

int main(int argc, char* argv[]) {
    vmInitTerminalState();
    if (argc < 2) {
        fprintf(stderr, "Usage: pscalvm <bytecode_file> [program_parameters...]\n");
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    const char* bytecode_path = argv[1];
    gParamCount = argc - 2;
    gParamValues = (gParamCount > 0) ? &argv[2] : NULL;

    initSymbolSystem();
    registerAllBuiltins();

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    if (!loadBytecodeFromFile(bytecode_path, &chunk)) {
        fprintf(stderr, "Failed to load bytecode from %s\n", bytecode_path);
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    VM vm;
    initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, procedure_table, 0);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (procedure_table) freeHashTable(procedure_table);

    return vmExitWithCleanup(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE);
}

