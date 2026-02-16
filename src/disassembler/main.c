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
#include "ast/ast.h"
#include "compiler/bytecode.h"
#include "common/frontend_kind.h"
#include "disassembler/opcode_meta.h"
#include "shell/function.h"
#include "vm/string_sentinels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

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
    "Usage: pscald [--asm] [--emit-asm] <bytecode_file>\n"
    "       pscald --help\n";

typedef struct {
    Symbol **items;
    size_t count;
    size_t capacity;
} ProcVector;

static int emitAsmV2(FILE *out, const BytecodeChunk *chunk, HashTable *procedureTable);

static void procVectorFree(ProcVector *vec) {
    if (!vec) {
        return;
    }
    free(vec->items);
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

static int procVectorAppend(ProcVector *vec, Symbol *sym) {
    if (!vec || !sym) {
        return 0;
    }
    if (vec->count == vec->capacity) {
        size_t new_capacity = vec->capacity == 0 ? 16u : vec->capacity * 2u;
        Symbol **new_items = (Symbol **)realloc(vec->items, sizeof(Symbol *) * new_capacity);
        if (!new_items) {
            return 0;
        }
        vec->items = new_items;
        vec->capacity = new_capacity;
    }
    vec->items[vec->count++] = sym;
    return 1;
}

static int collectProceduresRecursive(HashTable *table, ProcVector *out) {
    if (!table) {
        return 1;
    }
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (Symbol *sym = table->buckets[i]; sym; sym = sym->next) {
            if (!sym || sym->is_alias) {
                continue;
            }
            if (!procVectorAppend(out, sym)) {
                return 0;
            }
            if (sym->type_def && sym->type_def->symbol_table) {
                HashTable *nested = (HashTable *)sym->type_def->symbol_table;
                if (!collectProceduresRecursive(nested, out)) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int findProcedureIndexByPointer(const ProcVector *vec, const Symbol *sym) {
    if (!vec || !sym) {
        return -1;
    }
    for (size_t i = 0; i < vec->count; ++i) {
        if (vec->items[i] == sym) {
            return (int)i;
        }
    }
    return -1;
}

static void printEscapedQuoted(FILE *out, const char *text) {
    fputc('"', out);
    if (text) {
        for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            switch (*p) {
                case '\\': fputs("\\\\", out); break;
                case '"':  fputs("\\\"", out); break;
                case '\n': fputs("\\n", out); break;
                case '\r': fputs("\\r", out); break;
                case '\t': fputs("\\t", out); break;
                default:
                    fputc((int)*p, out);
                    break;
            }
        }
    }
    fputc('"', out);
}

static int astToJsonString(const AST *node, char **json_out) {
    if (!node || !json_out) {
        return 0;
    }

    FILE *tmp = tmpfile();
    if (!tmp) {
        return 0;
    }

    dumpASTJSON((AST *)node, tmp);
    if (fflush(tmp) != 0 || fseek(tmp, 0, SEEK_END) != 0) {
        fclose(tmp);
        return 0;
    }

    long json_size = ftell(tmp);
    if (json_size < 0 || fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        return 0;
    }

    char *json = (char *)malloc((size_t)json_size + 1u);
    if (!json) {
        fclose(tmp);
        return 0;
    }

    if (json_size > 0) {
        size_t nread = fread(json, 1, (size_t)json_size, tmp);
        if (nread != (size_t)json_size) {
            free(json);
            fclose(tmp);
            return 0;
        }
    }
    json[json_size] = '\0';
    fclose(tmp);

    *json_out = json;
    return 1;
}

static int bytecodeChunkToAsmStringIsolated(const BytecodeChunk *chunk, char **asm_out) {
    if (!chunk || !asm_out) {
        return 0;
    }

    FILE *tmp = tmpfile();
    if (!tmp) {
        return 0;
    }

    HashTable *saved_global_symbols = globalSymbols;
    TypeEntry *saved_type_table = type_table;
    globalSymbols = NULL;
    type_table = NULL;
    int emit_ok = emitAsmV2(tmp, chunk, NULL);
    globalSymbols = saved_global_symbols;
    type_table = saved_type_table;
    if (!emit_ok) {
        fclose(tmp);
        return 0;
    }

    if (fflush(tmp) != 0 || fseek(tmp, 0, SEEK_END) != 0) {
        fclose(tmp);
        return 0;
    }

    long text_size = ftell(tmp);
    if (text_size < 0 || fseek(tmp, 0, SEEK_SET) != 0) {
        fclose(tmp);
        return 0;
    }

    char *text = (char *)malloc((size_t)text_size + 1u);
    if (!text) {
        fclose(tmp);
        return 0;
    }

    if (text_size > 0) {
        size_t nread = fread(text, 1, (size_t)text_size, tmp);
        if (nread != (size_t)text_size) {
            free(text);
            fclose(tmp);
            return 0;
        }
    }
    text[text_size] = '\0';
    fclose(tmp);

    *asm_out = text;
    return 1;
}

static int emitAsmV2ValuePayload(FILE *out, const Value *value) {
    if (!out || !value) {
        return 0;
    }
    switch (value->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT64:
            fprintf(out, " %lld", value->i_val);
            break;
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
            fprintf(out, " %llu", value->u_val);
            break;
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            fprintf(out, " %.21Lg", AS_REAL(*value));
            break;
        case TYPE_STRING:
            fputc(' ', out);
            printEscapedQuoted(out, value->s_val);
            break;
        case TYPE_CHAR:
            fprintf(out, " %d", value->c_val);
            break;
        case TYPE_NIL:
            break;
        case TYPE_ENUM:
            fputc(' ', out);
            printEscapedQuoted(out, value->enum_val.enum_name ? value->enum_val.enum_name : "");
            fprintf(out, " %d", value->enum_val.ordinal);
            break;
        case TYPE_SET:
            fprintf(out, " %d", value->set_val.set_size);
            for (int i = 0; i < value->set_val.set_size; ++i) {
                fprintf(out, " %lld", value->set_val.set_values ? value->set_val.set_values[i] : 0LL);
            }
            break;
        case TYPE_POINTER:
            if (value->ptr_val == NULL) {
                fprintf(out, " null");
                break;
            }
            if (value->base_type_node == STRING_CHAR_PTR_SENTINEL ||
                value->base_type_node == SERIALIZED_CHAR_PTR_SENTINEL) {
                fprintf(out, " charptr ");
                printEscapedQuoted(out, (const char *)value->ptr_val);
                break;
            }
            if (value->base_type_node == SHELL_FUNCTION_PTR_SENTINEL) {
                ShellCompiledFunction *compiled = (ShellCompiledFunction *)value->ptr_val;
                if (!compiled || compiled->magic != SHELL_COMPILED_FUNCTION_MAGIC) {
                    fprintf(stderr, "pscald: unsupported pointer constant payload in --emit-asm.\n");
                    return 0;
                }
                char *nested_asm = NULL;
                if (!bytecodeChunkToAsmStringIsolated(&compiled->chunk, &nested_asm)) {
                    fprintf(stderr, "pscald: failed to emit nested shell function chunk in --emit-asm.\n");
                    return 0;
                }
                fprintf(out, " shellfn_asm ");
                printEscapedQuoted(out, nested_asm);
                free(nested_asm);
                break;
            }
            fprintf(out, " opaque_addr %llu",
                    (unsigned long long)(uintptr_t)value->ptr_val);
            break;
        case TYPE_ARRAY: {
            int dims = value->dimensions;
            if (dims <= 0 || !value->lower_bounds || !value->upper_bounds) {
                fprintf(stderr, "pscald: invalid array constant shape in --emit-asm.\n");
                return 0;
            }
            int total = calculateArrayTotalSize(value);
            if (total < 0) {
                fprintf(stderr, "pscald: invalid array constant size in --emit-asm.\n");
                return 0;
            }

            fprintf(out, " dims %d elem %d bounds", dims, (int)value->element_type);
            for (int i = 0; i < dims; ++i) {
                fprintf(out, " %d %d", value->lower_bounds[i], value->upper_bounds[i]);
            }
            fprintf(out, " values %d", total);

            if (total > 0) {
                if (arrayUsesPackedBytes(value)) {
                    if (!value->array_raw) {
                        fprintf(stderr, "pscald: packed array constant missing raw bytes.\n");
                        return 0;
                    }
                    for (int i = 0; i < total; ++i) {
                        fprintf(out, " %u", (unsigned)value->array_raw[i]);
                    }
                } else {
                    if (!value->array_val) {
                        fprintf(stderr, "pscald: array constant missing elements.\n");
                        return 0;
                    }
                    for (int i = 0; i < total; ++i) {
                        const Value *elem = &value->array_val[i];
                        switch (value->element_type) {
                            case TYPE_INT32:
                            case TYPE_WORD:
                            case TYPE_BYTE:
                            case TYPE_BOOLEAN:
                            case TYPE_INT8:
                            case TYPE_INT16:
                            case TYPE_INT64:
                                fprintf(out, " %lld", elem->i_val);
                                break;
                            case TYPE_UINT8:
                            case TYPE_UINT16:
                            case TYPE_UINT32:
                            case TYPE_UINT64:
                                fprintf(out, " %llu", elem->u_val);
                                break;
                            case TYPE_FLOAT:
                            case TYPE_DOUBLE:
                            case TYPE_LONG_DOUBLE:
                                fprintf(out, " %.21Lg", AS_REAL(*elem));
                                break;
                            case TYPE_STRING:
                                fputc(' ', out);
                                printEscapedQuoted(out, elem->s_val);
                                break;
                            case TYPE_CHAR:
                                fprintf(out, " %d", elem->c_val);
                                break;
                            case TYPE_NIL:
                                fprintf(out, " nil");
                                break;
                            default:
                                fprintf(stderr,
                                        "pscald: unsupported array element type %s in --emit-asm.\n",
                                        varTypeToString(value->element_type));
                                return 0;
                        }
                    }
                }
            }
            break;
        }
        default:
            fprintf(stderr, "pscald: unsupported constant type in --emit-asm: %s\n",
                    varTypeToString(value->type));
            return 0;
    }
    return 1;
}

static int emitAsmV2Constant(FILE *out, int idx, const Value *value) {
    if (!out || !value) {
        return 0;
    }
    fprintf(out, "const %d %d", idx, (int)value->type);
    if (!emitAsmV2ValuePayload(out, value)) {
        return 0;
    }
    fputc('\n', out);
    return 1;
}

static int emitAsmV2(FILE *out, const BytecodeChunk *chunk, HashTable *procedureTable) {
    if (!out || !chunk) {
        return 0;
    }

    fprintf(out, "PSCALASM2\n");
    fprintf(out, "version %u\n", (unsigned)chunk->version);
    fprintf(out, "constants %d\n", chunk->constants_count);
    for (int i = 0; i < chunk->constants_count; ++i) {
        if (!emitAsmV2Constant(out, i, &chunk->constants[i])) {
            return 0;
        }
    }

    int builtin_map_count = 0;
    if (chunk->builtin_lowercase_indices) {
        for (int i = 0; i < chunk->constants_count; ++i) {
            int lower_idx = chunk->builtin_lowercase_indices[i];
            if (lower_idx >= 0 && lower_idx < chunk->constants_count) {
                builtin_map_count++;
            }
        }
    }
    fprintf(out, "builtin_map %d\n", builtin_map_count);
    if (chunk->builtin_lowercase_indices) {
        for (int i = 0; i < chunk->constants_count; ++i) {
            int lower_idx = chunk->builtin_lowercase_indices[i];
            if (lower_idx >= 0 && lower_idx < chunk->constants_count) {
                fprintf(out, "builtin %d %d\n", i, lower_idx);
            }
        }
    }

    int const_symbol_count = 0;
    if (globalSymbols) {
        for (int b = 0; b < HASHTABLE_SIZE; ++b) {
            for (Symbol *sym = globalSymbols->buckets[b]; sym; sym = sym->next) {
                if (!sym || sym->is_alias || !sym->is_const || !sym->name || !sym->value) {
                    continue;
                }
                const_symbol_count++;
            }
        }
    }
    fprintf(out, "const_symbols %d\n", const_symbol_count);
    if (globalSymbols) {
        for (int b = 0; b < HASHTABLE_SIZE; ++b) {
            for (Symbol *sym = globalSymbols->buckets[b]; sym; sym = sym->next) {
                if (!sym || sym->is_alias || !sym->is_const || !sym->name || !sym->value) {
                    continue;
                }
                fprintf(out, "const_symbol ");
                printEscapedQuoted(out, sym->name);
                fprintf(out, " %d", (int)sym->type);
                if (!emitAsmV2ValuePayload(out, sym->value)) {
                    fprintf(stderr, "pscald: failed to emit const symbol '%s'.\n", sym->name);
                    return 0;
                }
                fputc('\n', out);
            }
        }
    }

    int type_count = 0;
    for (TypeEntry *entry = type_table; entry; entry = entry->next) {
        if (entry->name && entry->typeAST) {
            type_count++;
        }
    }
    fprintf(out, "types %d\n", type_count);
    for (TypeEntry *entry = type_table; entry; entry = entry->next) {
        if (!entry->name || !entry->typeAST) {
            continue;
        }
        char *json = NULL;
        if (!astToJsonString(entry->typeAST, &json)) {
            fprintf(stderr, "pscald: failed to emit type '%s'.\n", entry->name);
            return 0;
        }
        fprintf(out, "type ");
        printEscapedQuoted(out, entry->name);
        fputc(' ', out);
        printEscapedQuoted(out, json);
        fputc('\n', out);
        free(json);
    }

    ProcVector procs = {0};
    if (!collectProceduresRecursive(procedureTable, &procs)) {
        procVectorFree(&procs);
        fprintf(stderr, "pscald: out of memory while exporting procedure metadata.\n");
        return 0;
    }

    uint8_t *label_offsets = (uint8_t *)calloc((size_t)chunk->count + 1u, sizeof(uint8_t));
    if (!label_offsets) {
        procVectorFree(&procs);
        fprintf(stderr, "pscald: out of memory while preparing labels for --emit-asm.\n");
        return 0;
    }

    fprintf(out, "procedures %zu\n", procs.count);
    for (size_t i = 0; i < procs.count; ++i) {
        Symbol *sym = procs.items[i];
        Symbol *enclosing = resolveSymbolAlias(sym->enclosing);
        int enclosing_idx = findProcedureIndexByPointer(&procs, enclosing);
        if (sym->bytecode_address >= 0 && sym->bytecode_address <= chunk->count) {
            label_offsets[sym->bytecode_address] = 1;
        }
        fprintf(out, "proc %zu ", i);
        printEscapedQuoted(out, sym->name ? sym->name : "");
        fprintf(out, " %d %u %u %d %u %d\n",
                sym->bytecode_address,
                (unsigned)sym->locals_count,
                (unsigned)sym->upvalue_count,
                (int)sym->type,
                (unsigned)sym->arity,
                enclosing_idx);
        for (int uv = 0; uv < sym->upvalue_count; ++uv) {
            fprintf(out, "upvalue %zu %d %u %u %u\n",
                    i,
                    uv,
                    (unsigned)sym->upvalues[uv].index,
                    sym->upvalues[uv].isLocal ? 1u : 0u,
                    sym->upvalues[uv].is_ref ? 1u : 0u);
        }
    }
    procVectorFree(&procs);

    for (int offset = 0; offset < chunk->count; ) {
        uint8_t opcode = chunk->code[offset];
        int length = getInstructionLength((BytecodeChunk *)chunk, offset);
        if (length <= 0 || (offset + length) > chunk->count) {
            free(label_offsets);
            fprintf(stderr, "pscald: invalid instruction length at offset %d during --emit-asm.\n",
                    offset);
            return 0;
        }
        if ((opcode == JUMP || opcode == JUMP_IF_FALSE) && length >= 3) {
            int16_t distance = (int16_t)(((uint16_t)chunk->code[offset + 1] << 8) |
                                         (uint16_t)chunk->code[offset + 2]);
            int target = offset + 3 + (int)distance;
            if (target >= 0 && target <= chunk->count) {
                label_offsets[target] = 1;
            }
        }
        offset += length;
    }

    fprintf(out, "code %d\n", chunk->count);
    for (int offset = 0; offset < chunk->count; ) {
        if (label_offsets[offset]) {
            fprintf(out, "label L%04d\n", offset);
        }
        int line = (chunk->lines && offset < chunk->count) ? chunk->lines[offset] : 0;
        uint8_t opcode = chunk->code[offset];
        const char *name = pscalOpcodeName(opcode);
        if (!name) {
            free(label_offsets);
            fprintf(stderr, "pscald: unknown opcode %u at offset %d during --emit-asm.\n",
                    (unsigned)opcode, offset);
            return 0;
        }
        int length = getInstructionLength((BytecodeChunk *)chunk, offset);
        if (length <= 0 || (offset + length) > chunk->count) {
            free(label_offsets);
            fprintf(stderr, "pscald: invalid instruction length at offset %d during --emit-asm.\n",
                    offset);
            return 0;
        }

        fprintf(out, "inst %d %s", line, name);
        if ((opcode == JUMP || opcode == JUMP_IF_FALSE) && length >= 3) {
            int16_t distance = (int16_t)(((uint16_t)chunk->code[offset + 1] << 8) |
                                         (uint16_t)chunk->code[offset + 2]);
            int target = offset + 3 + (int)distance;
            if (target >= 0 && target <= chunk->count && label_offsets[target]) {
                fprintf(out, " @L%04d", target);
            } else {
                fprintf(out, " %u %u",
                        (unsigned)chunk->code[offset + 1],
                        (unsigned)chunk->code[offset + 2]);
            }
        } else {
            for (int i = 1; i < length; ++i) {
                fprintf(out, " %u", (unsigned)chunk->code[offset + i]);
            }
        }
        fputc('\n', out);
        offset += length;
    }
    free(label_offsets);
    fprintf(out, "end\n");
    return 1;
}

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
    bool emit_asm_v2 = false;
    const char *path = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--asm") == 0 || strcmp(argv[i], "-a") == 0) {
            emit_asm_block = true;
            continue;
        }
        if (strcmp(argv[i], "--emit-asm") == 0) {
            emit_asm_v2 = true;
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

    if (emit_asm_v2 && !emitAsmV2(stdout, &chunk, procedure_table)) {
        freeBytecodeChunk(&chunk);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        PSCALD_RETURN(EXIT_FAILURE);
    }

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
