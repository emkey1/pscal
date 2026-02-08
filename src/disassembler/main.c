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
#include <errno.h>
#include <stdbool.h>

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

static const char *PSCALD_USAGE =
    "Usage: pscald [--asm] <bytecode_file>\n"
    "       pscald --help\n";

static int pscaldDumpAsmBlock(const char *path) {
    FILE *in = fopen(path, "rb");
    if (!in) {
        fprintf(stderr, "pscald: failed to open '%s' for asm export: %s\n", path, strerror(errno));
        return 0;
    }

    uint8_t *bytes = NULL;
    size_t count = 0;
    size_t capacity = 0;
    uint8_t buffer[4096];

    for (;;) {
        size_t n = fread(buffer, 1, sizeof(buffer), in);
        if (n > 0) {
            if (count + n > capacity) {
                size_t new_capacity = capacity ? capacity : 8192u;
                while (new_capacity < count + n) {
                    new_capacity *= 2u;
                }
                uint8_t *new_bytes = (uint8_t *)realloc(bytes, new_capacity);
                if (!new_bytes) {
                    fprintf(stderr, "pscald: out of memory while exporting asm block.\n");
                    free(bytes);
                    fclose(in);
                    return 0;
                }
                bytes = new_bytes;
                capacity = new_capacity;
            }
            memcpy(bytes + count, buffer, n);
            count += n;
        }

        if (n < sizeof(buffer)) {
            if (ferror(in)) {
                fprintf(stderr, "pscald: read error exporting '%s': %s\n", path, strerror(errno));
                free(bytes);
                fclose(in);
                return 0;
            }
            break;
        }
    }

    fclose(in);

    fprintf(stderr, "== PSCALASM BEGIN v1 ==\n");
    fprintf(stderr, "bytes: %zu\n", count);
    fprintf(stderr, "hex:\n");
    for (size_t i = 0; i < count; ++i) {
        if (i % 16 == 0) {
            fprintf(stderr, "  ");
        }
        fprintf(stderr, "%02x", bytes[i]);
        if ((i % 16) == 15 || i + 1 == count) {
            fprintf(stderr, "\n");
        } else {
            fprintf(stderr, " ");
        }
    }
    fprintf(stderr, "== PSCALASM END ==\n");

    free(bytes);
    return 1;
}

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

    bool emit_asm_block = false;
    const char *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--asm") == 0 || strcmp(argv[i], "-a") == 0) {
            emit_asm_block = true;
            continue;
        }
        if (!path) {
            path = argv[i];
            continue;
        }
        fprintf(stderr, "%s", PSCALD_USAGE);
        PSCALD_RETURN(EXIT_FAILURE);
    }

    if (!path) {
        fprintf(stderr, "%s", PSCALD_USAGE);
        PSCALD_RETURN(EXIT_FAILURE);
    }

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

    if (emit_asm_block && !pscaldDumpAsmBlock(path)) {
        freeBytecodeChunk(&chunk);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        PSCALD_RETURN(EXIT_FAILURE);
    }

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
