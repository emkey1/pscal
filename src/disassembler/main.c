/*
 * MIT License
 *
 * Copyright (c) 2024 PSCAL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Note: PSCAL versions prior to 2.22 were released under the Unlicense.
 */

#include "core/cache.h"
#include "core/utils.h"
#include "core/list.h"
#include "vm/vm.h"
#include "Pascal/globals.h"
#include "symbol/symbol.h"
#include "backend_ast/builtin.h"
#include "compiler/bytecode.h"
#include "common/frontend_kind.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static const char *PSCALD_USAGE = "Usage: pscald <bytecode_file>\n";

int pscald_main(int argc, char* argv[]) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
#define PSCALD_RETURN(value)            \
    do {                                \
        int __pscald_rc = (value);      \
        frontendPopKind(previousKind);  \
        return __pscald_rc;             \
    } while (0)
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("%s", PSCALD_USAGE);
        PSCALD_RETURN(EXIT_SUCCESS);
    }

    if (argc != 2) {
        fprintf(stderr, "%s", PSCALD_USAGE);
        PSCALD_RETURN(EXIT_FAILURE);
    }

    const char* path = argv[1];
    initSymbolSystem();
    registerAllBuiltins();

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    if (!loadBytecodeFromFile(path, &chunk)) {
        fprintf(stderr, "Failed to load bytecode from %s\n", path);
        PSCALD_RETURN(EXIT_FAILURE);
    }

    const char* disasm_name = bytecodeDisplayNameForPath(path);
    disassembleBytecodeChunk(&chunk, disasm_name ? disasm_name : path, procedure_table);

    freeBytecodeChunk(&chunk);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);
    PSCALD_RETURN(EXIT_SUCCESS);
}
#undef PSCALD_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char* argv[]) {
    return pscald_main(argc, argv);
}
#endif
