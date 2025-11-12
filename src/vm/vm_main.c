#include "core/cache.h"
#include "core/utils.h"
#include "core/list.h"
#include "vm/vm.h"
#include "Pascal/globals.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include "common/frontend_kind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *PSCALVM_USAGE = "Usage: pscalvm <bytecode_file> [program_parameters...]\n";

int pscalvm_main(int argc, char* argv[]) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
#define PSCALVM_RETURN(value)           \
    do {                                \
        int __vm_rc = (value);          \
        frontendPopKind(previousKind);  \
        return __vm_rc;                 \
    } while (0)
    vmInitTerminalState();
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("%s", PSCALVM_USAGE);
        PSCALVM_RETURN(vmExitWithCleanup(EXIT_SUCCESS));
    }
    if (argc < 2) {
        fprintf(stderr, "%s", PSCALVM_USAGE);
        PSCALVM_RETURN(vmExitWithCleanup(EXIT_FAILURE));
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
        PSCALVM_RETURN(vmExitWithCleanup(EXIT_FAILURE));
    }

    VM vm;
    initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);

    PSCALVM_RETURN(vmExitWithCleanup(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE));
}
#undef PSCALVM_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char* argv[]) {
    return pscalvm_main(argc, argv);
}
#endif
