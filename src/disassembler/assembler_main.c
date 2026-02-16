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

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "Pascal/globals.h"
#include "Pascal/type_registry.h"
#include "ast/ast.h"
#include "common/frontend_kind.h"
#include "compiler/bytecode.h"
#include "core/cache.h"
#include "core/utils.h"
#include "core/version.h"
#include "disassembler/opcode_meta.h"
#include "shell/function.h"
#include "symbol/symbol.h"
#include "tools/ast_json_loader.h"
#include "vm/string_sentinels.h"

static const char *PSCALASM_USAGE =
    "Usage: pscalasm <assembly.txt|-> <output.pbc>\n"
    "       pscald --emit-asm <input.pbc> > dump.asm\n"
    "       pscalasm dump.asm rebuilt.pbc\n"
    "\n"
    "Legacy fallback remains supported:\n"
    "       pscald --asm <input.pbc> 2> dump.txt\n"
    "       pscalasm dump.txt rebuilt.pbc\n";

typedef struct {
    int original_idx;
    int lowercase_idx;
} BuiltinMapEntry;

typedef struct {
    uint8_t index;
    bool is_local;
    bool is_ref;
    bool is_set;
} ParsedUpvalue;

typedef struct {
    char *name;
    int bytecode_address;
    uint16_t locals_count;
    uint8_t upvalue_count;
    VarType type;
    uint8_t arity;
    int enclosing_index;
    bool is_set;
    ParsedUpvalue upvalues[256];
} ParsedProcedure;

typedef struct {
    char *name;
    VarType type;
    Value value;
    bool is_set;
} ParsedConstSymbol;

typedef struct {
    char *name;
    char *json;
    bool is_set;
} ParsedTypeEntry;

typedef struct {
    bool has_header;
    bool has_version;
    uint32_t version;

    bool has_constants;
    int constants_count;
    Value *constants;
    uint8_t *constant_set;

    bool has_builtin_map;
    int expected_builtin_entries;
    BuiltinMapEntry *builtin_entries;
    int builtin_entry_count;
    int builtin_entry_capacity;

    bool has_procedures;
    int procedures_count;
    ParsedProcedure *procedures;
    uint8_t *procedure_set;

    bool has_const_symbols;
    int expected_const_symbol_count;
    ParsedConstSymbol *const_symbols;
    int const_symbol_count;
    int const_symbol_capacity;

    bool has_types;
    int expected_type_count;
    ParsedTypeEntry *types;
    int type_count;
    int type_capacity;

    bool has_code;
    int expected_code_count;
    uint8_t *code;
    int *lines;
    int code_count;
    int code_capacity;
} ParsedAsmProgram;

typedef struct {
    int asm_line_number;
    int line;
    uint8_t opcode;
    char **operands;
    int operand_count;
    int operand_capacity;
} ParsedInstruction;

typedef struct {
    char *name;
    int asm_line_number;
    int instruction_index;
} ParsedLabel;

static int parsePscalasm2(const char *input_text, ParsedAsmProgram *program);
static int assembleAndWritePscalasm2(const ParsedAsmProgram *program,
                                     const char *source_hint,
                                     const char *output_path);
static int ensureAssemblerSymbolTables(void);
static void cleanupAssemblerSymbolTables(void);

static void initParsedAsmProgram(ParsedAsmProgram *program) {
    memset(program, 0, sizeof(*program));
}

static void freeParsedAsmProgram(ParsedAsmProgram *program) {
    if (!program) {
        return;
    }

    if (program->constants) {
        for (int i = 0; i < program->constants_count; ++i) {
            freeValue(&program->constants[i]);
        }
    }
    free(program->constants);
    free(program->constant_set);

    if (program->procedures) {
        for (int i = 0; i < program->procedures_count; ++i) {
            free(program->procedures[i].name);
        }
    }
    free(program->procedures);
    free(program->procedure_set);

    if (program->const_symbols) {
        for (int i = 0; i < program->const_symbol_count; ++i) {
            free(program->const_symbols[i].name);
            freeValue(&program->const_symbols[i].value);
        }
    }
    free(program->const_symbols);

    if (program->types) {
        for (int i = 0; i < program->type_count; ++i) {
            free(program->types[i].name);
            free(program->types[i].json);
        }
    }
    free(program->types);

    free(program->builtin_entries);
    free(program->code);
    free(program->lines);

    initParsedAsmProgram(program);
}

static char *trimInPlace(char *line) {
    while (*line && isspace((unsigned char)*line)) {
        ++line;
    }
    size_t len = strlen(line);
    while (len > 0 && isspace((unsigned char)line[len - 1])) {
        line[--len] = '\0';
    }
    return line;
}

static void skipSpaces(const char **cursor) {
    while (**cursor && isspace((unsigned char)**cursor)) {
        ++(*cursor);
    }
}

static int parseWordToken(const char **cursor, char *buffer, size_t buffer_size) {
    skipSpaces(cursor);
    if (**cursor == '\0') {
        return 0;
    }
    size_t out = 0;
    while (**cursor && !isspace((unsigned char)**cursor)) {
        if (out + 1 < buffer_size) {
            buffer[out++] = **cursor;
        }
        ++(*cursor);
    }
    if (out == 0) {
        return 0;
    }
    buffer[out] = '\0';
    return 1;
}

static int parseLongLongToken(const char **cursor, long long *value_out) {
    skipSpaces(cursor);
    if (**cursor == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    long long value = strtoll(*cursor, &end, 0);
    if (errno != 0 || end == *cursor) {
        return 0;
    }
    *cursor = end;
    *value_out = value;
    return 1;
}

static int parseLongDoubleToken(const char **cursor, long double *value_out) {
    skipSpaces(cursor);
    if (**cursor == '\0') {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    long double value = strtold(*cursor, &end);
    if (errno != 0 || end == *cursor) {
        return 0;
    }
    *cursor = end;
    *value_out = value;
    return 1;
}

static int parseQuotedStringToken(const char **cursor, char **value_out) {
    skipSpaces(cursor);
    if (**cursor != '"') {
        return 0;
    }
    ++(*cursor);

    size_t capacity = 32;
    size_t count = 0;
    char *result = (char *)malloc(capacity);
    if (!result) {
        return 0;
    }

    while (**cursor && **cursor != '"') {
        char ch = **cursor;
        ++(*cursor);
        if (ch == '\\') {
            char esc = **cursor;
            if (!esc) {
                free(result);
                return 0;
            }
            ++(*cursor);
            switch (esc) {
                case 'n': ch = '\n'; break;
                case 'r': ch = '\r'; break;
                case 't': ch = '\t'; break;
                case '\\': ch = '\\'; break;
                case '"': ch = '"'; break;
                default: ch = esc; break;
            }
        }

        if (count + 1 >= capacity) {
            size_t new_capacity = capacity * 2u;
            char *new_result = (char *)realloc(result, new_capacity);
            if (!new_result) {
                free(result);
                return 0;
            }
            result = new_result;
            capacity = new_capacity;
        }
        result[count++] = ch;
    }

    if (**cursor != '"') {
        free(result);
        return 0;
    }
    ++(*cursor);

    result[count] = '\0';
    *value_out = result;
    return 1;
}

static int appendBuiltinEntry(ParsedAsmProgram *program, int original_idx, int lowercase_idx) {
    if (program->builtin_entry_count >= program->builtin_entry_capacity) {
        int new_capacity = program->builtin_entry_capacity == 0 ? 16 : program->builtin_entry_capacity * 2;
        BuiltinMapEntry *new_entries =
            (BuiltinMapEntry *)realloc(program->builtin_entries, sizeof(BuiltinMapEntry) * (size_t)new_capacity);
        if (!new_entries) {
            return 0;
        }
        program->builtin_entries = new_entries;
        program->builtin_entry_capacity = new_capacity;
    }
    program->builtin_entries[program->builtin_entry_count].original_idx = original_idx;
    program->builtin_entries[program->builtin_entry_count].lowercase_idx = lowercase_idx;
    program->builtin_entry_count++;
    return 1;
}

static int appendConstSymbol(ParsedAsmProgram *program,
                             const char *name,
                             VarType type,
                             const Value *value) {
    if (!program || !name || !value) {
        return 0;
    }
    if (program->const_symbol_count >= program->const_symbol_capacity) {
        int new_capacity = program->const_symbol_capacity == 0 ? 8 : program->const_symbol_capacity * 2;
        ParsedConstSymbol *new_symbols =
            (ParsedConstSymbol *)realloc(program->const_symbols,
                                         sizeof(ParsedConstSymbol) * (size_t)new_capacity);
        if (!new_symbols) {
            return 0;
        }
        program->const_symbols = new_symbols;
        program->const_symbol_capacity = new_capacity;
    }

    ParsedConstSymbol *slot = &program->const_symbols[program->const_symbol_count];
    memset(slot, 0, sizeof(*slot));
    slot->name = strdup(name);
    if (!slot->name) {
        return 0;
    }
    slot->type = type;
    slot->value = makeCopyOfValue(value);
    slot->is_set = true;
    program->const_symbol_count++;
    return 1;
}

static int appendTypeEntry(ParsedAsmProgram *program,
                           const char *name,
                           const char *json) {
    if (!program || !name || !json) {
        return 0;
    }
    if (program->type_count >= program->type_capacity) {
        int new_capacity = program->type_capacity == 0 ? 8 : program->type_capacity * 2;
        ParsedTypeEntry *new_types =
            (ParsedTypeEntry *)realloc(program->types, sizeof(ParsedTypeEntry) * (size_t)new_capacity);
        if (!new_types) {
            return 0;
        }
        program->types = new_types;
        program->type_capacity = new_capacity;
    }

    ParsedTypeEntry *slot = &program->types[program->type_count];
    memset(slot, 0, sizeof(*slot));
    slot->name = strdup(name);
    slot->json = strdup(json);
    if (!slot->name || !slot->json) {
        free(slot->name);
        free(slot->json);
        memset(slot, 0, sizeof(*slot));
        return 0;
    }
    slot->is_set = true;
    program->type_count++;
    return 1;
}

static int appendCodeByte(ParsedAsmProgram *program, uint8_t byte, int line) {
    if (program->code_count >= program->code_capacity) {
        int new_capacity = program->code_capacity == 0 ? 256 : program->code_capacity * 2;
        uint8_t *new_code = (uint8_t *)realloc(program->code, sizeof(uint8_t) * (size_t)new_capacity);
        if (!new_code) {
            return 0;
        }
        program->code = new_code;

        int *new_lines = (int *)realloc(program->lines, sizeof(int) * (size_t)new_capacity);
        if (!new_lines) {
            return 0;
        }
        program->lines = new_lines;
        program->code_capacity = new_capacity;
    }
    program->code[program->code_count] = byte;
    program->lines[program->code_count] = line;
    program->code_count++;
    return 1;
}

static void freeParsedInstruction(ParsedInstruction *inst) {
    if (!inst) {
        return;
    }
    for (int i = 0; i < inst->operand_count; ++i) {
        free(inst->operands[i]);
    }
    free(inst->operands);
    memset(inst, 0, sizeof(*inst));
}

static int appendInstructionOperand(ParsedInstruction *inst, const char *token) {
    if (!inst || !token) {
        return 0;
    }
    if (inst->operand_count >= inst->operand_capacity) {
        int new_capacity = inst->operand_capacity == 0 ? 4 : inst->operand_capacity * 2;
        char **new_operands = (char **)realloc(inst->operands, sizeof(char *) * (size_t)new_capacity);
        if (!new_operands) {
            return 0;
        }
        inst->operands = new_operands;
        inst->operand_capacity = new_capacity;
    }
    char *copy = strdup(token);
    if (!copy) {
        return 0;
    }
    inst->operands[inst->operand_count++] = copy;
    return 1;
}

static int appendInstruction(ParsedInstruction **items,
                             int *count,
                             int *capacity,
                             ParsedInstruction *inst) {
    if (!items || !count || !capacity || !inst) {
        return 0;
    }
    if (*count >= *capacity) {
        int new_capacity = *capacity == 0 ? 64 : (*capacity * 2);
        ParsedInstruction *new_items =
            (ParsedInstruction *)realloc(*items, sizeof(ParsedInstruction) * (size_t)new_capacity);
        if (!new_items) {
            return 0;
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[*count] = *inst;
    (*count)++;
    memset(inst, 0, sizeof(*inst));
    return 1;
}

static int appendLabel(ParsedLabel **items,
                       int *count,
                       int *capacity,
                       const char *name,
                       int asm_line_number,
                       int instruction_index) {
    if (!items || !count || !capacity || !name) {
        return 0;
    }
    if (*count >= *capacity) {
        int new_capacity = *capacity == 0 ? 32 : (*capacity * 2);
        ParsedLabel *new_items = (ParsedLabel *)realloc(*items, sizeof(ParsedLabel) * (size_t)new_capacity);
        if (!new_items) {
            return 0;
        }
        *items = new_items;
        *capacity = new_capacity;
    }
    (*items)[*count].name = strdup(name);
    if (!(*items)[*count].name) {
        return 0;
    }
    (*items)[*count].asm_line_number = asm_line_number;
    (*items)[*count].instruction_index = instruction_index;
    (*count)++;
    return 1;
}

static void freeLabels(ParsedLabel *labels, int count) {
    if (!labels) {
        return;
    }
    for (int i = 0; i < count; ++i) {
        free(labels[i].name);
    }
    free(labels);
}

static int parseInt64Exact(const char *text, long long *out) {
    if (!text || !*text || !out) {
        return 0;
    }
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    *out = value;
    return 1;
}

static int parseByteExact(const char *text, uint8_t *out) {
    long long value = 0;
    if (!parseInt64Exact(text, &value) || value < 0 || value > 255) {
        return 0;
    }
    *out = (uint8_t)value;
    return 1;
}

static const ParsedLabel *findLabelByName(const ParsedLabel *labels, int label_count, const char *name) {
    if (!labels || !name) {
        return NULL;
    }
    for (int i = 0; i < label_count; ++i) {
        if (labels[i].name && strcmp(labels[i].name, name) == 0) {
            return &labels[i];
        }
    }
    return NULL;
}

static int instructionLengthForAsm(const ParsedInstruction *inst, int *out_length) {
    if (!inst || !out_length) {
        return 0;
    }
    int length = 1;
    if (inst->opcode == JUMP || inst->opcode == JUMP_IF_FALSE) {
        if (inst->operand_count == 1 || inst->operand_count == 2) {
            length = 3;
        } else {
            return 0;
        }
    } else {
        int exact_operands = -1;
        int minimum_operands = 0;
        if (!pscalOpcodeOperandInfo(inst->opcode, &exact_operands, &minimum_operands)) {
            return 0;
        }
        if (exact_operands >= 0) {
            if (inst->operand_count != exact_operands) {
                return 0;
            }
            length = 1 + exact_operands;
        } else {
            if (inst->operand_count < minimum_operands) {
                return 0;
            }
            length = 1 + inst->operand_count;
        }
    }
    *out_length = length;
    return 1;
}

static int buildCodeFromInstructions(ParsedAsmProgram *program,
                                     const ParsedInstruction *instructions,
                                     int instruction_count,
                                     const ParsedLabel *labels,
                                     int label_count) {
    if (!program) {
        return 0;
    }

    int *offsets = NULL;
    if (instruction_count > 0) {
        offsets = (int *)malloc(sizeof(int) * (size_t)(instruction_count + 1));
        if (!offsets) {
            return 0;
        }
    }

    int running_offset = 0;
    for (int i = 0; i < instruction_count; ++i) {
        offsets[i] = running_offset;
        int inst_len = 0;
        if (!instructionLengthForAsm(&instructions[i], &inst_len)) {
            fprintf(stderr, "pscalasm:%d: invalid operand count for opcode '%s'.\n",
                    instructions[i].asm_line_number,
                    pscalOpcodeName(instructions[i].opcode) ? pscalOpcodeName(instructions[i].opcode) : "<unknown>");
            free(offsets);
            return 0;
        }
        running_offset += inst_len;
    }
    if (instruction_count > 0) {
        offsets[instruction_count] = running_offset;
    }

    for (int i = 0; i < label_count; ++i) {
        if (labels[i].instruction_index < 0 || labels[i].instruction_index > instruction_count) {
            fprintf(stderr, "pscalasm:%d: label '%s' has invalid position.\n",
                    labels[i].asm_line_number, labels[i].name ? labels[i].name : "<unnamed>");
            free(offsets);
            return 0;
        }
    }

    for (int i = 0; i < instruction_count; ++i) {
        const ParsedInstruction *inst = &instructions[i];
        if (!appendCodeByte(program, inst->opcode, inst->line)) {
            free(offsets);
            return 0;
        }

        if (inst->opcode == JUMP || inst->opcode == JUMP_IF_FALSE) {
            int16_t distance = 0;
            if (inst->operand_count == 2) {
                uint8_t b0 = 0;
                uint8_t b1 = 0;
                if (!parseByteExact(inst->operands[0], &b0) ||
                    !parseByteExact(inst->operands[1], &b1)) {
                    fprintf(stderr, "pscalasm:%d: jump with two operands requires byte values.\n",
                            inst->asm_line_number);
                    free(offsets);
                    return 0;
                }
                distance = (int16_t)(((uint16_t)b0 << 8) | (uint16_t)b1);
            } else if (inst->operand_count == 1) {
                const char *op = inst->operands[0];
                long long numeric_distance = 0;
                if (parseInt64Exact(op, &numeric_distance)) {
                    if (numeric_distance < INT16_MIN || numeric_distance > INT16_MAX) {
                        fprintf(stderr, "pscalasm:%d: jump distance out of range: %lld.\n",
                                inst->asm_line_number, numeric_distance);
                        free(offsets);
                        return 0;
                    }
                    distance = (int16_t)numeric_distance;
                } else {
                    const char *label_name = op;
                    if (*label_name == '@') {
                        ++label_name;
                    }
                    const ParsedLabel *target = findLabelByName(labels, label_count, label_name);
                    if (!target) {
                        fprintf(stderr, "pscalasm:%d: unknown label '%s'.\n",
                                inst->asm_line_number, label_name);
                        free(offsets);
                        return 0;
                    }
                    int target_offset = offsets[target->instruction_index];
                    int origin_after_inst = offsets[i] + 3;
                    int delta = target_offset - origin_after_inst;
                    if (delta < INT16_MIN || delta > INT16_MAX) {
                        fprintf(stderr, "pscalasm:%d: jump to label '%s' out of int16 range.\n",
                                inst->asm_line_number, label_name);
                        free(offsets);
                        return 0;
                    }
                    distance = (int16_t)delta;
                }
            } else {
                fprintf(stderr, "pscalasm:%d: jump opcode requires 1 label/offset operand or 2 raw bytes.\n",
                        inst->asm_line_number);
                free(offsets);
                return 0;
            }

            uint16_t encoded = (uint16_t)distance;
            if (!appendCodeByte(program, (uint8_t)((encoded >> 8) & 0xFF), inst->line) ||
                !appendCodeByte(program, (uint8_t)(encoded & 0xFF), inst->line)) {
                free(offsets);
                return 0;
            }
            continue;
        }

        for (int opi = 0; opi < inst->operand_count; ++opi) {
            uint8_t byte = 0;
            if (!parseByteExact(inst->operands[opi], &byte)) {
                fprintf(stderr, "pscalasm:%d: operand '%s' is not a byte value.\n",
                        inst->asm_line_number, inst->operands[opi]);
                free(offsets);
                return 0;
            }
            if (!appendCodeByte(program, byte, inst->line)) {
                free(offsets);
                return 0;
            }
        }
    }

    free(offsets);
    return 1;
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int appendRawByte(uint8_t **buffer, size_t *count, size_t *capacity, uint8_t value) {
    if (*count >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4096u : (*capacity * 2u);
        uint8_t *new_buffer = (uint8_t *)realloc(*buffer, new_capacity);
        if (!new_buffer) {
            return 0;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    (*buffer)[(*count)++] = value;
    return 1;
}

static int parseLegacyPscalasmBlock(const char *input_text, uint8_t **out_bytes, size_t *out_len) {
    if (!input_text) {
        return 0;
    }

    char *copy = strdup(input_text);
    if (!copy) {
        return 0;
    }

    int in_block = 0;
    int in_hex = 0;
    int found_block = 0;
    long long expected_bytes = -1;
    uint8_t *bytes = NULL;
    size_t count = 0;
    size_t capacity = 0;

    char *saveptr = NULL;
    for (char *line = strtok_r(copy, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        if (!in_block) {
            if (strstr(line, "== PSCALASM BEGIN v1 ==") != NULL) {
                in_block = 1;
                found_block = 1;
            }
            continue;
        }

        if (strstr(line, "== PSCALASM END ==") != NULL) {
            break;
        }

        if (strncmp(line, "bytes:", 6) == 0) {
            const char *p = line + 6;
            while (*p != '\0' && isspace((unsigned char)*p)) {
                ++p;
            }
            if (*p != '\0') {
                char *end = NULL;
                errno = 0;
                long long parsed = strtoll(p, &end, 10);
                if (errno == 0 && end != p && parsed >= 0) {
                    expected_bytes = parsed;
                }
            }
            continue;
        }

        if (strncmp(line, "hex:", 4) == 0) {
            in_hex = 1;
            continue;
        }

        if (!in_hex) {
            continue;
        }

        for (const char *p = line; *p != '\0'; ) {
            while (*p != '\0' && !isxdigit((unsigned char)*p)) {
                ++p;
            }
            if (*p == '\0') {
                break;
            }

            int hi = hex_nibble(*p++);
            while (*p != '\0' && !isxdigit((unsigned char)*p)) {
                ++p;
            }
            if (*p == '\0') {
                break;
            }
            int lo = hex_nibble(*p++);

            if (hi < 0 || lo < 0) {
                continue;
            }
            if (!appendRawByte(&bytes, &count, &capacity, (uint8_t)((hi << 4) | lo))) {
                free(bytes);
                free(copy);
                return 0;
            }
        }
    }

    free(copy);

    if (!found_block) {
        return 0;
    }
    if (expected_bytes >= 0 && (size_t)expected_bytes != count) {
        fprintf(stderr,
                "pscalasm: byte count mismatch (header=%lld parsed=%zu).\n",
                expected_bytes, count);
        free(bytes);
        return -1;
    }

    *out_bytes = bytes;
    *out_len = count;
    return 1;
}

static int parseShellFunctionPointerPayload(const char *asm_text, Value *value_out) {
    if (!asm_text || !value_out) {
        return 0;
    }

    ParsedAsmProgram nested;
    initParsedAsmProgram(&nested);
    int parse_status = parsePscalasm2(asm_text, &nested);
    if (parse_status <= 0) {
        freeParsedAsmProgram(&nested);
        return 0;
    }

    char tmp_path[] = "/tmp/pscalasm-shellfn-XXXXXX";
    int fd = mkstemp(tmp_path);
    if (fd < 0) {
        freeParsedAsmProgram(&nested);
        return 0;
    }
    close(fd);

    int ok = assembleAndWritePscalasm2(&nested, "<inline-shellfn>", tmp_path);
    freeParsedAsmProgram(&nested);
    if (!ok) {
        unlink(tmp_path);
        return 0;
    }

    if (!ensureAssemblerSymbolTables()) {
        unlink(tmp_path);
        return 0;
    }

    ShellCompiledFunction *compiled =
        (ShellCompiledFunction *)calloc(1, sizeof(ShellCompiledFunction));
    if (!compiled) {
        cleanupAssemblerSymbolTables();
        unlink(tmp_path);
        return 0;
    }

    compiled->magic = SHELL_COMPILED_FUNCTION_MAGIC;
    initBytecodeChunk(&compiled->chunk);
    if (!loadBytecodeFromFile(tmp_path, &compiled->chunk)) {
        free(compiled);
        cleanupAssemblerSymbolTables();
        unlink(tmp_path);
        return 0;
    }

    cleanupAssemblerSymbolTables();
    unlink(tmp_path);

    value_out->type = TYPE_POINTER;
    value_out->ptr_val = (Value *)compiled;
    value_out->base_type_node = SHELL_FUNCTION_PTR_SENTINEL;
    return 1;
}

static int parseAsmConstantValue(VarType type, const char **cursor, Value *value_out) {
    Value v = {0};
    v.type = type;

    if (type == TYPE_ARRAY) {
        char keyword[32];
        long long dims_ll = 0;
        long long elem_type_ll = 0;
        long long declared_total_ll = 0;

        if (!parseWordToken(cursor, keyword, sizeof(keyword)) || strcmp(keyword, "dims") != 0 ||
            !parseLongLongToken(cursor, &dims_ll) || dims_ll <= 0 || dims_ll > 32 ||
            !parseWordToken(cursor, keyword, sizeof(keyword)) || strcmp(keyword, "elem") != 0 ||
            !parseLongLongToken(cursor, &elem_type_ll) ||
            elem_type_ll < TYPE_UNKNOWN || elem_type_ll > TYPE_THREAD ||
            !parseWordToken(cursor, keyword, sizeof(keyword)) || strcmp(keyword, "bounds") != 0) {
            return 0;
        }

        int dims = (int)dims_ll;
        VarType elem_type = (VarType)elem_type_ll;
        int *lower_bounds = (int *)calloc((size_t)dims, sizeof(int));
        int *upper_bounds = (int *)calloc((size_t)dims, sizeof(int));
        if (!lower_bounds || !upper_bounds) {
            free(lower_bounds);
            free(upper_bounds);
            return 0;
        }
        for (int i = 0; i < dims; ++i) {
            long long lb = 0;
            long long ub = 0;
            if (!parseLongLongToken(cursor, &lb) || !parseLongLongToken(cursor, &ub) ||
                lb < INT32_MIN || lb > INT32_MAX || ub < INT32_MIN || ub > INT32_MAX ||
                ub < lb) {
                free(lower_bounds);
                free(upper_bounds);
                return 0;
            }
            lower_bounds[i] = (int)lb;
            upper_bounds[i] = (int)ub;
        }

        if (!parseWordToken(cursor, keyword, sizeof(keyword)) || strcmp(keyword, "values") != 0 ||
            !parseLongLongToken(cursor, &declared_total_ll) ||
            declared_total_ll < 0 || declared_total_ll > INT32_MAX) {
            free(lower_bounds);
            free(upper_bounds);
            return 0;
        }

        Value arr = makeEmptyArray(elem_type, NULL);
        arr.dimensions = dims;
        arr.lower_bounds = lower_bounds;
        arr.upper_bounds = upper_bounds;
        arr.lower_bound = lower_bounds[0];
        arr.upper_bound = upper_bounds[0];
        arr.array_is_packed = isPackedByteElementType(elem_type);

        int total = calculateArrayTotalSize(&arr);
        if (total < 0 || declared_total_ll != total) {
            freeValue(&arr);
            return 0;
        }

        if (total > 0) {
            if (arr.array_is_packed) {
                arr.array_raw = (uint8_t *)calloc((size_t)total, sizeof(uint8_t));
                if (!arr.array_raw) {
                    freeValue(&arr);
                    return 0;
                }
                for (int i = 0; i < total; ++i) {
                    long long n = 0;
                    if (!parseLongLongToken(cursor, &n) || n < 0 || n > 255) {
                        freeValue(&arr);
                        return 0;
                    }
                    arr.array_raw[i] = (uint8_t)n;
                }
            } else {
                arr.array_val = (Value *)calloc((size_t)total, sizeof(Value));
                if (!arr.array_val) {
                    freeValue(&arr);
                    return 0;
                }
                for (int i = 0; i < total; ++i) {
                    Value elem = {0};
                    elem.type = elem_type;
                    switch (elem_type) {
                        case TYPE_INT32:
                        case TYPE_WORD:
                        case TYPE_BYTE:
                        case TYPE_BOOLEAN:
                        case TYPE_INT8:
                        case TYPE_INT16:
                        case TYPE_INT64: {
                            long long n = 0;
                            if (!parseLongLongToken(cursor, &n)) {
                                freeValue(&arr);
                                return 0;
                            }
                            SET_INT_VALUE(&elem, n);
                            break;
                        }
                        case TYPE_UINT8:
                        case TYPE_UINT16:
                        case TYPE_UINT32:
                        case TYPE_UINT64: {
                            long long n = 0;
                            if (!parseLongLongToken(cursor, &n) || n < 0) {
                                freeValue(&arr);
                                return 0;
                            }
                            elem.u_val = (unsigned long long)n;
                            elem.i_val = (long long)elem.u_val;
                            break;
                        }
                        case TYPE_FLOAT:
                        case TYPE_DOUBLE:
                        case TYPE_LONG_DOUBLE: {
                            long double n = 0.0;
                            if (!parseLongDoubleToken(cursor, &n)) {
                                freeValue(&arr);
                                return 0;
                            }
                            SET_REAL_VALUE(&elem, n);
                            break;
                        }
                        case TYPE_STRING: {
                            char *text = NULL;
                            if (!parseQuotedStringToken(cursor, &text)) {
                                freeValue(&arr);
                                return 0;
                            }
                            elem.s_val = text;
                            break;
                        }
                        case TYPE_CHAR: {
                            long long n = 0;
                            if (!parseLongLongToken(cursor, &n) || n < 0 || n > 255) {
                                freeValue(&arr);
                                return 0;
                            }
                            elem.c_val = (int)n;
                            SET_INT_VALUE(&elem, elem.c_val);
                            break;
                        }
                        case TYPE_NIL: {
                            char nil_word[16];
                            if (!parseWordToken(cursor, nil_word, sizeof(nil_word)) ||
                                strcmp(nil_word, "nil") != 0) {
                                freeValue(&arr);
                                return 0;
                            }
                            elem = makeNil();
                            break;
                        }
                        default:
                            freeValue(&arr);
                            return 0;
                    }
                    arr.array_val[i] = elem;
                }
            }
        }

        *value_out = arr;
        return 1;
    }

    switch (type) {
        case TYPE_INT32:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT64: {
            long long n = 0;
            if (!parseLongLongToken(cursor, &n)) {
                return 0;
            }
            SET_INT_VALUE(&v, n);
            break;
        }
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64: {
            long long n = 0;
            if (!parseLongLongToken(cursor, &n) || n < 0) {
                return 0;
            }
            v.u_val = (unsigned long long)n;
            v.i_val = (long long)v.u_val;
            break;
        }
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE: {
            long double n = 0.0;
            if (!parseLongDoubleToken(cursor, &n)) {
                return 0;
            }
            SET_REAL_VALUE(&v, n);
            break;
        }
        case TYPE_STRING: {
            char *text = NULL;
            if (!parseQuotedStringToken(cursor, &text)) {
                return 0;
            }
            v.s_val = text;
            break;
        }
        case TYPE_CHAR: {
            long long n = 0;
            if (!parseLongLongToken(cursor, &n) || n < 0 || n > 255) {
                return 0;
            }
            v.c_val = (int)n;
            SET_INT_VALUE(&v, v.c_val);
            break;
        }
        case TYPE_NIL:
            break;
        case TYPE_ENUM: {
            char *enum_name = NULL;
            long long ordinal = 0;
            if (!parseQuotedStringToken(cursor, &enum_name) ||
                !parseLongLongToken(cursor, &ordinal) ||
                ordinal < INT32_MIN || ordinal > INT32_MAX) {
                free(enum_name);
                return 0;
            }
            v = makeEnum(enum_name, (int)ordinal);
            free(enum_name);
            break;
        }
        case TYPE_SET: {
            long long set_size = 0;
            if (!parseLongLongToken(cursor, &set_size) ||
                set_size < 0 || set_size > INT32_MAX) {
                return 0;
            }
            v.set_val.set_size = (int)set_size;
            if (v.set_val.set_size > 0) {
                v.set_val.set_values =
                    (long long *)calloc((size_t)v.set_val.set_size, sizeof(long long));
                if (!v.set_val.set_values) {
                    return 0;
                }
                for (int i = 0; i < v.set_val.set_size; ++i) {
                    if (!parseLongLongToken(cursor, &v.set_val.set_values[i])) {
                        free(v.set_val.set_values);
                        v.set_val.set_values = NULL;
                        v.set_val.set_size = 0;
                        return 0;
                    }
                }
            }
            break;
        }
        case TYPE_POINTER: {
            char keyword[32];
            if (!parseWordToken(cursor, keyword, sizeof(keyword))) {
                return 0;
            }
            if (strcmp(keyword, "null") == 0) {
                v.ptr_val = NULL;
                v.base_type_node = NULL;
                break;
            }
            if (strcmp(keyword, "shellfn_asm") == 0) {
                char *nested_asm = NULL;
                if (!parseQuotedStringToken(cursor, &nested_asm)) {
                    return 0;
                }
                int ok = parseShellFunctionPointerPayload(nested_asm, &v);
                free(nested_asm);
                if (!ok) {
                    return 0;
                }
                break;
            }
            if (strcmp(keyword, "charptr") == 0) {
                char *text = NULL;
                if (!parseQuotedStringToken(cursor, &text)) {
                    return 0;
                }
                v.ptr_val = (Value *)text;
                v.base_type_node = SERIALIZED_CHAR_PTR_SENTINEL;
                break;
            }
            if (strcmp(keyword, "opaque_addr") == 0) {
                char addr_token[64];
                if (!parseWordToken(cursor, addr_token, sizeof(addr_token))) {
                    return 0;
                }
                char *end = NULL;
                errno = 0;
                unsigned long long addr = strtoull(addr_token, &end, 0);
                if (errno != 0 || end == addr_token || *end != '\0') {
                    return 0;
                }
                v.ptr_val = (Value *)(uintptr_t)addr;
                v.base_type_node = OPAQUE_POINTER_SENTINEL;
                break;
            }
            return 0;
        }
        default:
            return 0;
    }

    *value_out = v;
    return 1;
}

static int parsePscalasm2(const char *input_text, ParsedAsmProgram *program) {
    if (!input_text || !program) {
        return -1;
    }

    char *copy = strdup(input_text);
    if (!copy) {
        return -1;
    }

    int line_number = 0;
    char *saveptr = NULL;
    ParsedInstruction *instructions = NULL;
    int instruction_count = 0;
    int instruction_capacity = 0;
    ParsedLabel *labels = NULL;
    int label_count = 0;
    int label_capacity = 0;

    for (char *line = strtok_r(copy, "\n", &saveptr); line; line = strtok_r(NULL, "\n", &saveptr)) {
        line_number++;
        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] == '\r') {
            line[line_len - 1] = '\0';
        }

        char *trimmed = trimInPlace(line);
        if (*trimmed == '\0' || *trimmed == ';' || *trimmed == '#') {
            continue;
        }

        if (!program->has_header) {
            if (strcmp(trimmed, "PSCALASM2") != 0) {
                free(copy);
                return 0;
            }
            program->has_header = true;
            continue;
        }

        const char *cursor = trimmed;
        char directive[64];
        if (!parseWordToken(&cursor, directive, sizeof(directive))) {
            continue;
        }

        if (strcmp(directive, "version") == 0) {
            long long v = 0;
            if (!parseLongLongToken(&cursor, &v) || v < 0 || v > UINT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid version directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->version = (uint32_t)v;
            program->has_version = true;
            continue;
        }

        if (strcmp(directive, "constants") == 0) {
            long long count = 0;
            if (!parseLongLongToken(&cursor, &count) || count < 0 || count > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid constants directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (program->has_constants) {
                fprintf(stderr, "pscalasm:%d: duplicate constants directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->has_constants = true;
            program->constants_count = (int)count;
            if (program->constants_count > 0) {
                program->constants = (Value *)calloc((size_t)program->constants_count, sizeof(Value));
                program->constant_set = (uint8_t *)calloc((size_t)program->constants_count, sizeof(uint8_t));
                if (!program->constants || !program->constant_set) {
                    fprintf(stderr, "pscalasm:%d: out of memory allocating constants.\n", line_number);
                    free(copy);
                    return -1;
                }
            }
            continue;
        }

        if (strcmp(directive, "const") == 0) {
            long long idx_ll = -1;
            long long type_ll = -1;
            if (!program->has_constants) {
                fprintf(stderr, "pscalasm:%d: const before constants directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (!parseLongLongToken(&cursor, &idx_ll) ||
                !parseLongLongToken(&cursor, &type_ll) ||
                idx_ll < 0 || idx_ll >= program->constants_count ||
                type_ll < TYPE_UNKNOWN || type_ll > TYPE_THREAD) {
                fprintf(stderr, "pscalasm:%d: invalid const directive header.\n", line_number);
                free(copy);
                return -1;
            }

            int idx = (int)idx_ll;
            if (program->constant_set[idx]) {
                fprintf(stderr, "pscalasm:%d: duplicate const index %d.\n", line_number, idx);
                free(copy);
                return -1;
            }

            VarType type = (VarType)type_ll;
            Value parsed = {0};
            if (!parseAsmConstantValue(type, &cursor, &parsed)) {
                fprintf(stderr, "pscalasm:%d: invalid constant payload for type %s.\n",
                        line_number, varTypeToString(type));
                free(copy);
                return -1;
            }

            skipSpaces(&cursor);
            if (*cursor != '\0') {
                freeValue(&parsed);
                fprintf(stderr, "pscalasm:%d: trailing text in const directive.\n", line_number);
                free(copy);
                return -1;
            }

            program->constants[idx] = parsed;
            program->constant_set[idx] = 1;
            continue;
        }

        if (strcmp(directive, "builtin_map") == 0) {
            long long count = 0;
            if (!parseLongLongToken(&cursor, &count) || count < 0 || count > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid builtin_map directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (program->has_builtin_map) {
                fprintf(stderr, "pscalasm:%d: duplicate builtin_map directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->has_builtin_map = true;
            program->expected_builtin_entries = (int)count;
            continue;
        }

        if (strcmp(directive, "builtin") == 0) {
            long long original = -1;
            long long lower = -1;
            if (!program->has_builtin_map) {
                fprintf(stderr, "pscalasm:%d: builtin before builtin_map.\n", line_number);
                free(copy);
                return -1;
            }
            if (!parseLongLongToken(&cursor, &original) ||
                !parseLongLongToken(&cursor, &lower) ||
                original < 0 || lower < 0 ||
                original > INT32_MAX || lower > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid builtin directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (!appendBuiltinEntry(program, (int)original, (int)lower)) {
                fprintf(stderr, "pscalasm:%d: out of memory adding builtin map entry.\n", line_number);
                free(copy);
                return -1;
            }
            continue;
        }

        if (strcmp(directive, "const_symbols") == 0) {
            long long count = 0;
            if (!parseLongLongToken(&cursor, &count) || count < 0 || count > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid const_symbols directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (program->has_const_symbols) {
                fprintf(stderr, "pscalasm:%d: duplicate const_symbols directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->has_const_symbols = true;
            program->expected_const_symbol_count = (int)count;
            continue;
        }

        if (strcmp(directive, "const_symbol") == 0) {
            char *name = NULL;
            long long type_ll = -1;
            Value parsed_value = {0};
            if (!program->has_const_symbols) {
                fprintf(stderr, "pscalasm:%d: const_symbol before const_symbols.\n", line_number);
                free(copy);
                return -1;
            }
            if (!parseQuotedStringToken(&cursor, &name) ||
                !parseLongLongToken(&cursor, &type_ll) ||
                type_ll < TYPE_UNKNOWN || type_ll > TYPE_THREAD) {
                free(name);
                fprintf(stderr, "pscalasm:%d: invalid const_symbol header.\n", line_number);
                free(copy);
                return -1;
            }

            VarType type = (VarType)type_ll;
            if (!parseAsmConstantValue(type, &cursor, &parsed_value)) {
                free(name);
                fprintf(stderr, "pscalasm:%d: invalid const_symbol payload.\n", line_number);
                free(copy);
                return -1;
            }

            if (!appendConstSymbol(program, name, type, &parsed_value)) {
                freeValue(&parsed_value);
                free(name);
                fprintf(stderr, "pscalasm:%d: out of memory appending const_symbol.\n", line_number);
                free(copy);
                return -1;
            }

            freeValue(&parsed_value);
            free(name);
            continue;
        }

        if (strcmp(directive, "types") == 0) {
            long long count = 0;
            if (!parseLongLongToken(&cursor, &count) || count < 0 || count > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid types directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (program->has_types) {
                fprintf(stderr, "pscalasm:%d: duplicate types directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->has_types = true;
            program->expected_type_count = (int)count;
            continue;
        }

        if (strcmp(directive, "type") == 0) {
            char *name = NULL;
            char *json = NULL;
            if (!program->has_types) {
                fprintf(stderr, "pscalasm:%d: type before types directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (!parseQuotedStringToken(&cursor, &name) ||
                !parseQuotedStringToken(&cursor, &json)) {
                free(name);
                free(json);
                fprintf(stderr, "pscalasm:%d: invalid type directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (!appendTypeEntry(program, name, json)) {
                free(name);
                free(json);
                fprintf(stderr, "pscalasm:%d: out of memory appending type entry.\n", line_number);
                free(copy);
                return -1;
            }
            skipSpaces(&cursor);
            if (*cursor != '\0') {
                free(name);
                free(json);
                fprintf(stderr, "pscalasm:%d: trailing text in type directive.\n", line_number);
                free(copy);
                return -1;
            }
            free(name);
            free(json);
            continue;
        }

        if (strcmp(directive, "procedures") == 0) {
            long long count = 0;
            if (!parseLongLongToken(&cursor, &count) || count < 0 || count > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid procedures directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (program->has_procedures) {
                fprintf(stderr, "pscalasm:%d: duplicate procedures directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->has_procedures = true;
            program->procedures_count = (int)count;
            if (program->procedures_count > 0) {
                program->procedures =
                    (ParsedProcedure *)calloc((size_t)program->procedures_count, sizeof(ParsedProcedure));
                program->procedure_set = (uint8_t *)calloc((size_t)program->procedures_count, sizeof(uint8_t));
                if (!program->procedures || !program->procedure_set) {
                    fprintf(stderr, "pscalasm:%d: out of memory allocating procedures.\n", line_number);
                    free(copy);
                    return -1;
                }
            }
            continue;
        }

        if (strcmp(directive, "proc") == 0) {
            long long idx_ll = -1;
            long long addr = 0;
            long long locals = 0;
            long long upvalues = 0;
            long long type_ll = 0;
            long long arity = 0;
            long long enclosing = -1;
            char *name = NULL;

            if (!program->has_procedures) {
                fprintf(stderr, "pscalasm:%d: proc before procedures directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (!parseLongLongToken(&cursor, &idx_ll) ||
                idx_ll < 0 || idx_ll >= program->procedures_count) {
                fprintf(stderr, "pscalasm:%d: invalid proc index.\n", line_number);
                free(copy);
                return -1;
            }
            int idx = (int)idx_ll;
            if (program->procedure_set[idx]) {
                fprintf(stderr, "pscalasm:%d: duplicate proc index %d.\n", line_number, idx);
                free(copy);
                return -1;
            }

            if (!parseQuotedStringToken(&cursor, &name) ||
                !parseLongLongToken(&cursor, &addr) ||
                !parseLongLongToken(&cursor, &locals) ||
                !parseLongLongToken(&cursor, &upvalues) ||
                !parseLongLongToken(&cursor, &type_ll) ||
                !parseLongLongToken(&cursor, &arity) ||
                !parseLongLongToken(&cursor, &enclosing)) {
                free(name);
                fprintf(stderr, "pscalasm:%d: invalid proc directive.\n", line_number);
                free(copy);
                return -1;
            }

            if (locals < 0 || locals > UINT16_MAX ||
                upvalues < 0 || upvalues > 255 ||
                type_ll < TYPE_UNKNOWN || type_ll > TYPE_THREAD ||
                arity < 0 || arity > 255 ||
                enclosing < -1 || enclosing >= program->procedures_count) {
                free(name);
                fprintf(stderr, "pscalasm:%d: proc values out of range.\n", line_number);
                free(copy);
                return -1;
            }

            ParsedProcedure *proc = &program->procedures[idx];
            proc->name = name;
            proc->bytecode_address = (int)addr;
            proc->locals_count = (uint16_t)locals;
            proc->upvalue_count = (uint8_t)upvalues;
            proc->type = (VarType)type_ll;
            proc->arity = (uint8_t)arity;
            proc->enclosing_index = (int)enclosing;
            proc->is_set = true;
            program->procedure_set[idx] = 1;
            continue;
        }

        if (strcmp(directive, "upvalue") == 0) {
            long long proc_idx = -1;
            long long uv_idx = -1;
            long long slot_idx = -1;
            long long is_local = -1;
            long long is_ref = -1;
            if (!parseLongLongToken(&cursor, &proc_idx) ||
                !parseLongLongToken(&cursor, &uv_idx) ||
                !parseLongLongToken(&cursor, &slot_idx) ||
                !parseLongLongToken(&cursor, &is_local) ||
                !parseLongLongToken(&cursor, &is_ref)) {
                fprintf(stderr, "pscalasm:%d: invalid upvalue directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (proc_idx < 0 || proc_idx >= program->procedures_count) {
                fprintf(stderr, "pscalasm:%d: upvalue proc index out of range.\n", line_number);
                free(copy);
                return -1;
            }
            ParsedProcedure *proc = &program->procedures[proc_idx];
            if (!proc->is_set || uv_idx < 0 || uv_idx >= proc->upvalue_count || uv_idx >= 256 ||
                slot_idx < 0 || slot_idx > 255 ||
                (is_local != 0 && is_local != 1) ||
                (is_ref != 0 && is_ref != 1)) {
                fprintf(stderr, "pscalasm:%d: upvalue values out of range.\n", line_number);
                free(copy);
                return -1;
            }
            ParsedUpvalue *uv = &proc->upvalues[uv_idx];
            uv->index = (uint8_t)slot_idx;
            uv->is_local = is_local != 0;
            uv->is_ref = is_ref != 0;
            uv->is_set = true;
            continue;
        }

        if (strcmp(directive, "code") == 0) {
            long long count = 0;
            if (!parseLongLongToken(&cursor, &count) || count < 0 || count > INT32_MAX) {
                fprintf(stderr, "pscalasm:%d: invalid code directive.\n", line_number);
                free(copy);
                return -1;
            }
            program->has_code = true;
            program->expected_code_count = (int)count;
            continue;
        }

        if (strcmp(directive, "label") == 0) {
            char label_name[256];
            if (!program->has_code) {
                fprintf(stderr, "pscalasm:%d: label before code directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (!parseWordToken(&cursor, label_name, sizeof(label_name))) {
                fprintf(stderr, "pscalasm:%d: invalid label directive.\n", line_number);
                free(copy);
                return -1;
            }
            if (findLabelByName(labels, label_count, label_name)) {
                fprintf(stderr, "pscalasm:%d: duplicate label '%s'.\n", line_number, label_name);
                free(copy);
                return -1;
            }
            if (!appendLabel(&labels, &label_count, &label_capacity,
                             label_name, line_number, instruction_count)) {
                fprintf(stderr, "pscalasm:%d: out of memory adding label.\n", line_number);
                free(copy);
                return -1;
            }
            continue;
        }

        if (strcmp(directive, "inst") == 0) {
            long long line_no = 0;
            char mnemonic[64];
            ParsedInstruction inst;
            memset(&inst, 0, sizeof(inst));
            inst.asm_line_number = line_number;
            if (!parseLongLongToken(&cursor, &line_no) ||
                !parseWordToken(&cursor, mnemonic, sizeof(mnemonic))) {
                fprintf(stderr, "pscalasm:%d: invalid inst directive header.\n", line_number);
                free(copy);
                return -1;
            }
            int opcode = pscalOpcodeFromName(mnemonic);
            if (opcode < 0 || opcode > UINT8_MAX) {
                fprintf(stderr, "pscalasm:%d: unknown opcode '%s'.\n", line_number, mnemonic);
                free(copy);
                return -1;
            }
            inst.line = (int)line_no;
            inst.opcode = (uint8_t)opcode;

            for (;;) {
                char operand_token[256];
                const char *before = cursor;
                if (!parseWordToken(&cursor, operand_token, sizeof(operand_token))) {
                    cursor = before;
                    break;
                }
                if (!appendInstructionOperand(&inst, operand_token)) {
                    fprintf(stderr, "pscalasm:%d: out of memory appending operand token.\n", line_number);
                    freeParsedInstruction(&inst);
                    free(copy);
                    return -1;
                }
            }

            if (!appendInstruction(&instructions, &instruction_count, &instruction_capacity, &inst)) {
                fprintf(stderr, "pscalasm:%d: out of memory appending instruction.\n", line_number);
                freeParsedInstruction(&inst);
                free(copy);
                return -1;
            }
            continue;
        }

        if (strcmp(directive, "end") == 0) {
            break;
        }

        fprintf(stderr, "pscalasm:%d: unknown directive '%s'.\n", line_number, directive);
        free(copy);
        return -1;
    }

    free(copy);

    if (!program->has_header) {
        return 0;
    }

    if (!program->has_constants || !program->has_procedures || !program->has_code) {
        fprintf(stderr, "pscalasm: PSCALASM2 input is missing required directives.\n");
        return -1;
    }

    for (int i = 0; i < program->constants_count; ++i) {
        if (!program->constant_set[i]) {
            fprintf(stderr, "pscalasm: missing const entry for index %d.\n", i);
            return -1;
        }
    }
    for (int i = 0; i < program->procedures_count; ++i) {
        if (!program->procedure_set[i]) {
            fprintf(stderr, "pscalasm: missing proc entry for index %d.\n", i);
            return -1;
        }
        for (int uv = 0; uv < program->procedures[i].upvalue_count; ++uv) {
            if (!program->procedures[i].upvalues[uv].is_set) {
                fprintf(stderr, "pscalasm: missing upvalue %d for proc index %d.\n", uv, i);
                return -1;
            }
        }
    }
    if (program->has_builtin_map &&
        program->builtin_entry_count != program->expected_builtin_entries) {
        fprintf(stderr, "pscalasm: builtin entry count mismatch (declared=%d actual=%d).\n",
                program->expected_builtin_entries, program->builtin_entry_count);
        for (int i = 0; i < instruction_count; ++i) {
            freeParsedInstruction(&instructions[i]);
        }
        free(instructions);
        freeLabels(labels, label_count);
        return -1;
    }
    if (program->has_const_symbols &&
        program->const_symbol_count != program->expected_const_symbol_count) {
        fprintf(stderr, "pscalasm: const_symbol entry count mismatch (declared=%d actual=%d).\n",
                program->expected_const_symbol_count, program->const_symbol_count);
        for (int i = 0; i < instruction_count; ++i) {
            freeParsedInstruction(&instructions[i]);
        }
        free(instructions);
        freeLabels(labels, label_count);
        return -1;
    }
    if (program->has_types &&
        program->type_count != program->expected_type_count) {
        fprintf(stderr, "pscalasm: type entry count mismatch (declared=%d actual=%d).\n",
                program->expected_type_count, program->type_count);
        for (int i = 0; i < instruction_count; ++i) {
            freeParsedInstruction(&instructions[i]);
        }
        free(instructions);
        freeLabels(labels, label_count);
        return -1;
    }

    if (!buildCodeFromInstructions(program, instructions, instruction_count, labels, label_count)) {
        for (int i = 0; i < instruction_count; ++i) {
            freeParsedInstruction(&instructions[i]);
        }
        free(instructions);
        freeLabels(labels, label_count);
        return -1;
    }

    for (int i = 0; i < instruction_count; ++i) {
        freeParsedInstruction(&instructions[i]);
    }
    free(instructions);
    freeLabels(labels, label_count);

    if (program->expected_code_count != program->code_count) {
        fprintf(stderr, "pscalasm: code byte count mismatch (declared=%d actual=%d).\n",
                program->expected_code_count, program->code_count);
        return -1;
    }
    return 1;
}

static int writeOutputFile(const char *path, const uint8_t *bytes, size_t len) {
    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "pscalasm: cannot open output '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (len > 0 && fwrite(bytes, 1, len, out) != len) {
        fprintf(stderr, "pscalasm: short write to '%s'.\n", path);
        fclose(out);
        return 0;
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "pscalasm: failed to close '%s': %s\n", path, strerror(errno));
        return 0;
    }
    return 1;
}

static int readAllBytes(FILE *in, char **out_data, size_t *out_len) {
    char *data = NULL;
    size_t count = 0;
    size_t capacity = 0;
    char buffer[8192];

    while (!feof(in)) {
        size_t n = fread(buffer, 1, sizeof(buffer), in);
        if (n > 0) {
            if (count + n + 1 > capacity) {
                size_t new_capacity = capacity == 0 ? 16384u : capacity;
                while (count + n + 1 > new_capacity) {
                    new_capacity *= 2u;
                }
                char *new_data = (char *)realloc(data, new_capacity);
                if (!new_data) {
                    free(data);
                    return 0;
                }
                data = new_data;
                capacity = new_capacity;
            }
            memcpy(data + count, buffer, n);
            count += n;
        }
        if (ferror(in)) {
            free(data);
            return 0;
        }
    }

    if (!data) {
        data = (char *)malloc(1);
        if (!data) {
            return 0;
        }
    }
    data[count] = '\0';
    *out_data = data;
    *out_len = count;
    return 1;
}

static int ensureAssemblerSymbolTables(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
    return globalSymbols && constGlobalSymbols && procedure_table;
}

static void cleanupAssemblerSymbolTables(void) {
    if (globalSymbols) {
        freeHashTable(globalSymbols);
        globalSymbols = NULL;
    }
    if (constGlobalSymbols) {
        freeHashTable(constGlobalSymbols);
        constGlobalSymbols = NULL;
    }
    if (procedure_table) {
        freeHashTable(procedure_table);
        procedure_table = NULL;
    }
    current_procedure_table = NULL;
}

static Symbol *makeProcedureSymbol(const ParsedProcedure *parsed) {
    if (!parsed || !parsed->name) {
        return NULL;
    }
    Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
    if (!sym) {
        return NULL;
    }

    sym->name = strdup(parsed->name);
    if (!sym->name) {
        free(sym);
        return NULL;
    }
    toLowerString(sym->name);
    sym->type = parsed->type;
    sym->is_defined = true;
    sym->bytecode_address = parsed->bytecode_address;
    sym->locals_count = parsed->locals_count;
    sym->upvalue_count = parsed->upvalue_count;
    sym->arity = parsed->arity;
    for (int i = 0; i < parsed->upvalue_count; ++i) {
        sym->upvalues[i].index = parsed->upvalues[i].index;
        sym->upvalues[i].isLocal = parsed->upvalues[i].is_local;
        sym->upvalues[i].is_ref = parsed->upvalues[i].is_ref;
    }
    return sym;
}

static int validateProcedureEnclosingGraph(const ParsedAsmProgram *program) {
    if (!program || program->procedures_count <= 0) {
        return 1;
    }

    for (int i = 0; i < program->procedures_count; ++i) {
        int current = i;
        int hop_count = 0;
        while (current >= 0) {
            int enclosing = program->procedures[current].enclosing_index;
            if (enclosing < 0) {
                break;
            }
            if (enclosing >= program->procedures_count) {
                fprintf(stderr,
                        "pscalasm: proc %d has out-of-range enclosing index %d.\n",
                        i, enclosing);
                return 0;
            }
            if (enclosing == current) {
                fprintf(stderr,
                        "pscalasm: proc %d cannot enclose itself.\n",
                        i);
                return 0;
            }
            current = enclosing;
            hop_count++;
            if (hop_count > program->procedures_count) {
                fprintf(stderr,
                        "pscalasm: cyclic enclosing relationship detected at proc %d.\n",
                        i);
                return 0;
            }
        }
    }

    return 1;
}

static HashTable *ensureProcedureScopeTable(Symbol *parent) {
    if (!parent) {
        return NULL;
    }
    if (!parent->type_def) {
        parent->type_def = newASTNode(AST_PROCEDURE_DECL, NULL);
        if (!parent->type_def) {
            return NULL;
        }
    }
    if (!parent->type_def->symbol_table) {
        HashTable *nested = createHashTable();
        if (!nested) {
            return NULL;
        }
        parent->type_def->symbol_table = (Symbol *)nested;
    }
    return (HashTable *)parent->type_def->symbol_table;
}

static int assembleAndWritePscalasm2(const ParsedAsmProgram *program,
                                     const char *source_hint,
                                     const char *output_path) {
    if (!program || !output_path || !source_hint) {
        return 0;
    }

    int ok = 0;
    Symbol **symbols_by_index = NULL;

    if (!ensureAssemblerSymbolTables()) {
        fprintf(stderr, "pscalasm: failed to initialize symbol tables.\n");
        cleanupAssemblerSymbolTables();
        return 0;
    }

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    chunk.version = program->has_version ? program->version : pscal_vm_version();

    if (program->constants_count > 0) {
        chunk.constants = (Value *)calloc((size_t)program->constants_count, sizeof(Value));
        chunk.builtin_lowercase_indices = (int *)malloc(sizeof(int) * (size_t)program->constants_count);
        chunk.global_symbol_cache =
            (Symbol **)calloc((size_t)program->constants_count, sizeof(Symbol *));
        if (!chunk.constants || !chunk.builtin_lowercase_indices || !chunk.global_symbol_cache) {
            fprintf(stderr, "pscalasm: out of memory allocating constants.\n");
            goto cleanup;
        }
        chunk.constants_count = program->constants_count;
        chunk.constants_capacity = program->constants_count;
        for (int i = 0; i < program->constants_count; ++i) {
            chunk.constants[i] = makeCopyOfValue(&program->constants[i]);
            chunk.builtin_lowercase_indices[i] = -1;
            chunk.global_symbol_cache[i] = NULL;
        }
    }

    for (int i = 0; i < program->builtin_entry_count; ++i) {
        int original_idx = program->builtin_entries[i].original_idx;
        int lower_idx = program->builtin_entries[i].lowercase_idx;
        if (original_idx < 0 || original_idx >= chunk.constants_count ||
            lower_idx < 0 || lower_idx >= chunk.constants_count) {
            fprintf(stderr, "pscalasm: builtin map index out of range (%d -> %d).\n",
                    original_idx, lower_idx);
            goto cleanup;
        }
        setBuiltinLowercaseIndex(&chunk, original_idx, lower_idx);
    }

    for (int i = 0; i < program->const_symbol_count; ++i) {
        const ParsedConstSymbol *cs = &program->const_symbols[i];
        insertGlobalSymbol(cs->name, cs->type, NULL);
        Symbol *sym = lookupGlobalSymbol(cs->name);
        if (!sym || !sym->value) {
            fprintf(stderr, "pscalasm: failed to materialize const symbol '%s'.\n", cs->name);
            goto cleanup;
        }
        freeValue(sym->value);
        *(sym->value) = makeCopyOfValue(&cs->value);
        sym->type = cs->type;
        sym->is_const = true;
        insertConstGlobalSymbol(cs->name, cs->value);
    }

    for (int i = 0; i < program->type_count; ++i) {
        const ParsedTypeEntry *te = &program->types[i];
        AST *type_ast = loadASTFromJSON(te->json);
        if (!type_ast) {
            fprintf(stderr, "pscalasm: failed to parse type JSON for '%s'.\n", te->name);
            goto cleanup;
        }
        insertType(te->name, type_ast);
        freeAST(type_ast);
    }

    for (int i = 0; i < program->code_count; ++i) {
        writeBytecodeChunk(&chunk, program->code[i], program->lines[i]);
    }

    if (program->procedures_count > 0) {
        if (!validateProcedureEnclosingGraph(program)) {
            goto cleanup;
        }

        symbols_by_index = (Symbol **)calloc((size_t)program->procedures_count, sizeof(Symbol *));
        if (!symbols_by_index) {
            fprintf(stderr, "pscalasm: out of memory allocating procedure index map.\n");
            goto cleanup;
        }

        for (int i = 0; i < program->procedures_count; ++i) {
            Symbol *sym = makeProcedureSymbol(&program->procedures[i]);
            if (!sym) {
                fprintf(stderr, "pscalasm: failed to allocate procedure symbol %d.\n", i);
                goto cleanup;
            }
            symbols_by_index[i] = sym;
        }

        for (int i = 0; i < program->procedures_count; ++i) {
            int enclosing_idx = program->procedures[i].enclosing_index;
            HashTable *scope = procedure_table;
            if (enclosing_idx >= 0) {
                Symbol *parent = symbols_by_index[enclosing_idx];
                scope = ensureProcedureScopeTable(parent);
                if (!scope) {
                    fprintf(stderr, "pscalasm: failed to create procedure scope for index %d.\n",
                            enclosing_idx);
                    goto cleanup;
                }
                symbols_by_index[i]->enclosing = parent;
            }
            hashTableInsert(scope, symbols_by_index[i]);
        }
    }

    ok = saveBytecodeToFile(output_path, source_hint, &chunk);
    if (!ok) {
        fprintf(stderr, "pscalasm: failed to write assembled bytecode to '%s'.\n", output_path);
    }

cleanup:
    free(symbols_by_index);
    freeBytecodeChunk(&chunk);
    cleanupAssemblerSymbolTables();
    return ok;
}

int pscalasm_main(int argc, char **argv) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
#define PSCALASM_RETURN(value)         \
    do {                                \
        int __pscalasm_rc = (value);    \
        frontendPopKind(previousKind);  \
        return __pscalasm_rc;           \
    } while (0)

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("%s", PSCALASM_USAGE);
        PSCALASM_RETURN(EXIT_SUCCESS);
    }
    if (argc != 3) {
        fprintf(stderr, "%s", PSCALASM_USAGE);
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];

    FILE *in = NULL;
    if (strcmp(input_path, "-") == 0) {
        in = stdin;
    } else {
        in = fopen(input_path, "rb");
        if (!in) {
            fprintf(stderr, "pscalasm: cannot open input '%s': %s\n", input_path, strerror(errno));
            PSCALASM_RETURN(EXIT_FAILURE);
        }
    }

    char *input_text = NULL;
    size_t input_len = 0;
    if (!readAllBytes(in, &input_text, &input_len)) {
        if (in != stdin) {
            fclose(in);
        }
        fprintf(stderr, "pscalasm: failed reading input.\n");
        PSCALASM_RETURN(EXIT_FAILURE);
    }
    (void)input_len;

    if (in != stdin) {
        fclose(in);
    }

    ParsedAsmProgram parsed;
    initParsedAsmProgram(&parsed);
    int parse_status = parsePscalasm2(input_text, &parsed);
    if (parse_status < 0) {
        freeParsedAsmProgram(&parsed);
        free(input_text);
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    if (parse_status > 0) {
        const char *source_hint = (strcmp(input_path, "-") == 0) ? "<stdin-pscalasm2>" : input_path;
        int ok = assembleAndWritePscalasm2(&parsed, source_hint, output_path);
        freeParsedAsmProgram(&parsed);
        free(input_text);
        if (!ok) {
            PSCALASM_RETURN(EXIT_FAILURE);
        }
        PSCALASM_RETURN(EXIT_SUCCESS);
    }

    uint8_t *bytes = NULL;
    size_t len = 0;
    int legacy_status = parseLegacyPscalasmBlock(input_text, &bytes, &len);
    free(input_text);
    if (legacy_status <= 0) {
        fprintf(stderr, "pscalasm: input is neither PSCALASM2 nor legacy PSCALASM block.\n");
        free(bytes);
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    int ok = writeOutputFile(output_path, bytes, len);
    free(bytes);
    if (!ok) {
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    PSCALASM_RETURN(EXIT_SUCCESS);
}
#undef PSCALASM_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char **argv) {
    return pscalasm_main(argc, argv);
}
#endif
