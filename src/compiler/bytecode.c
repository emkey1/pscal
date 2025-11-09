// src/compiler/bytecode.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memcpy
#include <stdint.h>

#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For freeValue, varTypeToString
#include "symbol/symbol.h" // For Symbol struct, HashTable, lookupSymbolIn
#include "vm/vm.h"         // For HostFunctionID type (used in CALL_HOST cast)
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"
#include "core/version.h"

// initBytecodeChunk, freeBytecodeChunk, reallocate, writeBytecodeChunk,
// addConstantToChunk, emitShort, patchShort from your provided file.

void initBytecodeChunk(BytecodeChunk* chunk) { // From all.txt
    chunk->version = pscal_vm_version();
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->constants_count = 0;
    chunk->constants_capacity = 0;
    chunk->constants = NULL;
    chunk->builtin_lowercase_indices = NULL;
    chunk->global_symbol_cache = NULL;
  //  chunk->lines = 0;
}

void freeBytecodeChunk(BytecodeChunk* chunk) { // From all.txt
    free(chunk->code);
    free(chunk->lines);
    for (int i = 0; i < chunk->constants_count; i++) {
        freeValue(&chunk->constants[i]);
    }
    free(chunk->constants);
    free(chunk->builtin_lowercase_indices);
    free(chunk->global_symbol_cache);
    initBytecodeChunk(chunk);
}

const char* bytecodeDisplayNameForPath(const char* path) {
    if (!path) {
        return NULL;
    }

    const char* trimmed = path;
    const char* tests_sub = strstr(path, "/Tests/");
    if (tests_sub) {
        trimmed = tests_sub + 7; // skip "/Tests/"
    } else {
        const char* tests_back_sub = strstr(path, "\\Tests\\");
        if (tests_back_sub) {
            trimmed = tests_back_sub + 7; // skip "\\Tests\\"
        } else if (strncmp(path, "Tests/", 6) == 0) {
            trimmed = path + 6;
        } else if (strncmp(path, "Tests\\", 6) == 0) {
            trimmed = path + 6;
        }
    }

    while (*trimmed == '/' || *trimmed == '\\') {
        ++trimmed;
    }

    if (*trimmed == '\0') {
        return path;
    }
    return trimmed;
}

static void* reallocate(void* pointer, size_t oldSize, size_t newSize) { // From all.txt
    (void)oldSize;
    if (newSize == 0) {
        free(pointer);
        return NULL;
    }
    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        fprintf(stderr, "Memory allocation error (realloc failed)\n");
        EXIT_FAILURE_HANDLER();
        return NULL;
    }
    return result;
}

static const char* formatInlineCachePointer(uintptr_t cached, char* buffer, size_t bufferSize) {
    if (cached == (uintptr_t)0) {
        return "0x0";
    }

    if (bufferSize == 0) {
        return "";
    }

    snprintf(buffer, bufferSize, "%p", (void*)cached);
    buffer[bufferSize - 1] = '\0';
    return buffer;
}

void writeBytecodeChunk(BytecodeChunk* chunk, uint8_t byte, int line) { // From all.txt
    if (chunk->capacity < chunk->count + 1) {
        int oldCapacity = chunk->capacity;
        chunk->capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->code = (uint8_t*)reallocate(chunk->code,
                                         sizeof(uint8_t) * oldCapacity,
                                         sizeof(uint8_t) * chunk->capacity);
        chunk->lines = (int*)reallocate(chunk->lines,
                                      sizeof(int) * oldCapacity,
                                      sizeof(int) * chunk->capacity);
    }
    chunk->code[chunk->count] = byte;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int addConstantToChunk(BytecodeChunk* chunk, const Value* value) {
#ifdef DEBUG
    fprintf(stderr, "[DEBUG addConstantToChunk] ENTER. Adding value type %s. chunk ptr: %p\n", varTypeToString(value->type), (void*)chunk);
    if (value->type == TYPE_STRING) {
        fprintf(stderr, "[DEBUG addConstantToChunk] String value to add: '%s'\n", value->s_val ? value->s_val : "NULL_SVAL");
    }
    fflush(stderr);
#endif

    // First, check if an identical constant already exists to avoid duplicates.
    for (int i = 0; i < chunk->constants_count; i++) {
        Value* existing = &chunk->constants[i];
        if (existing->type == value->type) {
            if (existing->type == TYPE_INTEGER && existing->i_val == value->i_val) return i;
            if (isRealType(existing->type) && AS_REAL(*existing) == AS_REAL(*value)) return i;
            if (existing->type == TYPE_STRING && existing->s_val && value->s_val && strcmp(existing->s_val, value->s_val) == 0) return i;
            if (existing->type == TYPE_CHAR && existing->c_val == value->c_val) return i;
        }
    }

    if (chunk->constants_capacity < chunk->constants_count + 1) {
        int oldCapacity = chunk->constants_capacity;
        chunk->constants_capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->constants = (Value*)reallocate(chunk->constants,
                                             sizeof(Value) * oldCapacity,
                                             sizeof(Value) * chunk->constants_capacity);
        chunk->builtin_lowercase_indices = (int*)reallocate(chunk->builtin_lowercase_indices,
                                                           sizeof(int) * oldCapacity,
                                                           sizeof(int) * chunk->constants_capacity);
        chunk->global_symbol_cache = (Symbol**)reallocate(chunk->global_symbol_cache,
                                                          sizeof(Symbol*) * oldCapacity,
                                                          sizeof(Symbol*) * chunk->constants_capacity);
        for (int i = oldCapacity; i < chunk->constants_capacity; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
            chunk->global_symbol_cache[i] = NULL;
        }
    } else if (!chunk->builtin_lowercase_indices && chunk->constants_capacity > 0) {
        chunk->builtin_lowercase_indices = (int*)reallocate(NULL,
                                                           0,
                                                           sizeof(int) * chunk->constants_capacity);
        for (int i = 0; i < chunk->constants_capacity; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
        }
    }
    if (!chunk->global_symbol_cache && chunk->constants_capacity > 0) {
        chunk->global_symbol_cache = (Symbol**)reallocate(NULL,
                                                          0,
                                                          sizeof(Symbol*) * chunk->constants_capacity);
        for (int i = 0; i < chunk->constants_capacity; ++i) {
            chunk->global_symbol_cache[i] = NULL;
        }
    }

    // Perform a deep copy from the provided pointer.
    int index = chunk->constants_count;
    chunk->constants[index] = makeCopyOfValue(value);
    if (chunk->builtin_lowercase_indices) {
        chunk->builtin_lowercase_indices[index] = -1;
    }
    if (chunk->global_symbol_cache) {
        chunk->global_symbol_cache[index] = NULL;
    }

    // The function NO LONGER frees the incoming value. The caller is responsible.

    return chunk->constants_count++;
}

void setBuiltinLowercaseIndex(BytecodeChunk* chunk, int original_idx, int lowercase_idx) {
    if (!chunk || original_idx < 0) {
        return;
    }
    if (!chunk->builtin_lowercase_indices) {
        int capacity = chunk->constants_capacity > 0 ? chunk->constants_capacity : chunk->constants_count;
        if (capacity <= 0) {
            return;
        }
        chunk->builtin_lowercase_indices = (int*)reallocate(NULL, 0, sizeof(int) * capacity);
        for (int i = 0; i < capacity; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
        }
    }
    if (original_idx >= chunk->constants_capacity) {
        return;
    }
    chunk->builtin_lowercase_indices[original_idx] = lowercase_idx;
}

int getBuiltinLowercaseIndex(const BytecodeChunk* chunk, int original_idx) {
    if (!chunk || !chunk->builtin_lowercase_indices) {
        return -1;
    }
    if (original_idx < 0 || original_idx >= chunk->constants_count) {
        return -1;
    }
    return chunk->builtin_lowercase_indices[original_idx];
}

void emitShort(BytecodeChunk* chunk, uint16_t value, int line) { // From all.txt
    writeBytecodeChunk(chunk, (uint8_t)((value >> 8) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)(value & 0xFF), line);
}

void emitInt32(BytecodeChunk* chunk, uint32_t value, int line) {
    writeBytecodeChunk(chunk, (uint8_t)((value >> 24) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)((value >> 16) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)((value >> 8) & 0xFF), line);
    writeBytecodeChunk(chunk, (uint8_t)(value & 0xFF), line);
}

void patchShort(BytecodeChunk* chunk, int offset_in_code, uint16_t value) {
    if (offset_in_code < 0 || (offset_in_code + 1) >= chunk->count) {
        fprintf(stderr, "Error: patchShort out of bounds. Offset: %d, Chunk count: %d.\n",
                offset_in_code, chunk->count);
        return;
    }
    chunk->code[offset_in_code]     = (uint8_t)((value >> 8) & 0xFF);
    chunk->code[offset_in_code + 1] = (uint8_t)(value & 0xFF);
}

void writeInlineCacheSlot(BytecodeChunk* chunk, int line) {
    if (!chunk) {
        return;
    }
    for (int i = 0; i < GLOBAL_INLINE_CACHE_SLOT_SIZE; ++i) {
        writeBytecodeChunk(chunk, 0, line);
    }
}

// Corrected helper function to find procedure/function name by its bytecode address
static const char* findProcedureNameByAddress(HashTable* procedureTable, uint16_t address) {
    if (!procedureTable) return NULL;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol* symbol = procedureTable->buckets[i];
        while (symbol) {
            if (symbol->is_defined && symbol->bytecode_address == address) {
                return symbol->name;
            }
            symbol = symbol->next;
        }
    }
    return NULL;
}
// New helper function to get the length of an instruction at a given offset.
int getInstructionLength(BytecodeChunk* chunk, int offset) {
    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case CONSTANT:
        case GET_LOCAL:
        case SET_LOCAL:
        case INC_LOCAL:
        case DEC_LOCAL:
        case GET_GLOBAL_ADDRESS:
        case GET_LOCAL_ADDRESS:
        case GET_UPVALUE:
        case SET_UPVALUE:
        case GET_UPVALUE_ADDRESS:
            return 2; // opcode + 1-byte operand
        case GET_GLOBAL:
        case SET_GLOBAL:
        case GET_GLOBAL_CACHED:
        case SET_GLOBAL_CACHED:
            return 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        case GET_FIELD_ADDRESS:
        case GET_FIELD_OFFSET:
        case LOAD_FIELD_VALUE:
        case LOAD_FIELD_VALUE_BY_NAME:
        case ALLOC_OBJECT:
            return 2; // opcode + 1-byte operand
        case INIT_LOCAL_FILE:
            return 5; // opcode + slot + element type + 2-byte type name index
        case GET_ELEMENT_ADDRESS:
        case LOAD_ELEMENT_VALUE:
            return 2; // opcode + operand byte
        case GET_CHAR_ADDRESS:
            return 1; // opcode only
        case GET_ELEMENT_ADDRESS_CONST:
        case LOAD_ELEMENT_VALUE_CONST:
            return 5; // opcode + 4-byte flat offset
        case INIT_LOCAL_STRING:
            return 3; // opcode + slot + length
        case INIT_LOCAL_POINTER:
            return 4; // opcode + slot byte + 2-byte type name index
        case INIT_FIELD_ARRAY: {
            int current_pos = offset + 1; // after opcode
            current_pos++; // field index
            if (current_pos >= chunk->count) return 1;
            uint8_t dimension_count = chunk->code[current_pos++];
            current_pos += dimension_count * 4; // bounds indices
            current_pos += 3; // elem type and 2-byte elem type name index
            return current_pos - offset;
        }
        case INIT_LOCAL_ARRAY: {
            int current_pos = offset + 1; // after opcode
            current_pos++; // slot
            if (current_pos >= chunk->count) return 1;
            uint8_t dimension_count = chunk->code[current_pos++];
            current_pos += dimension_count * 4; // bounds indices (two 16-bit indices per dimension)
            current_pos += 3; // elem type and 2-byte elem type name index
            return current_pos - offset;
        }
        case CONSTANT16:
        case GET_FIELD_ADDRESS16:
        case GET_FIELD_OFFSET16:
        case LOAD_FIELD_VALUE16:
        case LOAD_FIELD_VALUE_BY_NAME16:
        case ALLOC_OBJECT16:
        case GET_GLOBAL_ADDRESS16:
            return 3; // 1 byte opcode + 2-byte operand
        case GET_GLOBAL16:
        case SET_GLOBAL16:
        case GET_GLOBAL16_CACHED:
        case SET_GLOBAL16_CACHED:
            return 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        case PUSH_IMMEDIATE_INT8:
            return 2; // opcode + immediate byte
        case CONST_0:
        case CONST_1:
        case CONST_TRUE:
        case CONST_FALSE:
            return 1;
        case JUMP:
        case JUMP_IF_FALSE:
        case FORMAT_VALUE:
            return 3; // 1-byte opcode + 2-byte operand
        case CALL_BUILTIN:
            return 4; // 1-byte opcode + 2-byte name_idx + 1-byte arg count
        case CALL_BUILTIN_PROC:
            return 6; // 1-byte opcode + 2-byte builtin id + 2-byte name idx + 1-byte arg count
        case CALL_USER_PROC:
            return 4; // 1-byte opcode + 2-byte name_idx + 1-byte arg count
        case CALL:
            return 6; // 1-byte opcode + 2-byte name_idx + 2-byte addr + 1-byte arity
        case CALL_INDIRECT:
        case PROC_CALL_INDIRECT:
        case CALL_HOST:
            return 2; // opcode + 1-byte operand
        case CALL_METHOD:
            return 3; // opcode + method index + arity
        case EXIT:
            return 1;
        case THREAD_CREATE:
            return 3; // opcode + 2-byte entry offset
        case DEFINE_GLOBAL: {
            // This instruction has a variable length.
            int current_pos = offset + 1; // Position after the opcode
            if (current_pos + 1 >= chunk->count) return 1; // Safeguard for incomplete instruction

            VarType declaredType = (VarType)chunk->code[offset + 2];
            current_pos = offset + 3; // Position after the type byte

            if (declaredType == TYPE_ARRAY) {
                if (current_pos < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_pos++];
                    current_pos += dimension_count * 4; // Skip over all the bounds indices
                    current_pos += 3; // Skip element VarType and 2-byte element type name index
                }
            } else {
                // Simple types store a 16-bit type name index. Strings add an
                // extra 16-bit length constant and files carry element metadata.
                current_pos += 2; // type name index (16-bit)
                if (declaredType == TYPE_STRING) {
                    current_pos += 2; // length constant index (16-bit)
                } else if (declaredType == TYPE_FILE) {
                    current_pos += 3; // element VarType byte + 2-byte element type name index
                }
            }
            return (current_pos - offset); // Return the total calculated length
        }
        case DEFINE_GLOBAL16: {
            // Similar to DEFINE_GLOBAL but with a 16-bit name index.
            int current_pos = offset + 1; // after opcode
            if (current_pos + 2 >= chunk->count) return 1; // ensure enough bytes for name and type
            VarType declaredType = (VarType)chunk->code[offset + 3];
            current_pos = offset + 4; // position after type byte

            if (declaredType == TYPE_ARRAY) {
                if (current_pos < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_pos++];
                    current_pos += dimension_count * 4; // bounds indices
                    current_pos += 3; // element var type and 2-byte element type name index
                }
            } else {
                current_pos += 2; // type name index (16-bit)
                if (declaredType == TYPE_STRING) {
                    current_pos += 2; // length constant index (16-bit)
                } else if (declaredType == TYPE_FILE) {
                    current_pos += 3; // element VarType byte + 2-byte element type name index
                }
            }
            return (current_pos - offset);
        }
        default:
            return 1; // All other opcodes are 1 byte.
    }
}

// Utility helpers to print strings and characters with escape sequences so
// they don't introduce newlines in the disassembly output.
static void printEscapedString(const char* str) {
    if (!str) {
        fprintf(stderr, "NULL_STR");
        return;
    }
    for (const char* p = str; *p; p++) {
        switch (*p) {
            case '\n': fprintf(stderr, "\\n"); break;
            case '\r': fprintf(stderr, "\\r"); break;
            case '\t': fprintf(stderr, "\\t"); break;
            case '\\': fprintf(stderr, "\\\\"); break;
            case '\'': fputc('\'', stderr); break;
            default: fputc(*p, stderr); break;
        }
    }
}

static void printEscapedChar(char c) {
    switch (c) {
        case '\n': fprintf(stderr, "\\n"); break;
        case '\r': fprintf(stderr, "\\r"); break;
        case '\t': fprintf(stderr, "\\t"); break;
        case '\\': fprintf(stderr, "\\\\"); break;
        case '\'': fputc('\'', stderr); break;
        default: fprintf(stderr, "%c", c); break;
    }
}

static void printConstantValue(const Value* value) {
    if (!value) {
        fprintf(stderr, "<NULL>");
        return;
    }

    switch (value->type) {
        case TYPE_INTEGER:
            fprintf(stderr, "%lld", value->i_val);
            break;
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            fprintf(stderr, "%Lf", AS_REAL(*value));
            break;
        case TYPE_STRING:
            if (value->s_val) {
                printEscapedString(value->s_val);
            } else {
                fprintf(stderr, "NULL_STR");
            }
            break;
        case TYPE_CHAR:
            printEscapedChar(value->c_val);
            break;
        case TYPE_BOOLEAN:
            fprintf(stderr, "%s", value->i_val ? "true" : "false");
            break;
        case TYPE_NIL:
            fprintf(stderr, "nil");
            break;
        case TYPE_CLOSURE: {
            fprintf(stderr, "closure(entry=%u", value->closure.entry_offset);
            if (value->closure.symbol && value->closure.symbol->name) {
                fprintf(stderr, ", symbol=%s", value->closure.symbol->name);
            }
            if (value->closure.env) {
                fprintf(stderr, ", env=%p, slots=%u, ref=%u)",
                        (void*)value->closure.env,
                        (unsigned)value->closure.env->slot_count,
                        (unsigned)value->closure.env->refcount);
            } else {
                fprintf(stderr, ", env=NULL)");
            }
            break;
        }
        default:
            fprintf(stderr, "Value type %s", varTypeToString(value->type));
            break;
    }
}

static uintptr_t readInlineCachePtr(const BytecodeChunk* chunk, int offset) {
    uintptr_t value = 0;
    size_t to_copy = sizeof(value) < (size_t)GLOBAL_INLINE_CACHE_SLOT_SIZE
                         ? sizeof(value)
                         : (size_t)GLOBAL_INLINE_CACHE_SLOT_SIZE;
    memcpy(&value, chunk->code + offset, to_copy);
    return value;
}

// This is the function declared in bytecode.h and called by disassembleBytecodeChunk
// It was already non-static in your provided bytecode.c
int disassembleInstruction(BytecodeChunk* chunk, int offset, HashTable* procedureTable) {
    fprintf(stderr, "%04d ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        fprintf(stderr, "   | ");
    } else {
        fprintf(stderr, "%4d ", chunk->lines[offset]);
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case RETURN:
            fprintf(stderr, "RETURN\n");
            return offset + 1;

        case CONSTANT: {
            uint8_t constant_index = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4u ", "CONSTANT", (unsigned)constant_index);
            if (constant_index >= chunk->constants_count) {
                fprintf(stderr, "<INVALID CONST IDX %u>\n", (unsigned)constant_index);
                return offset + 2;
            }
            fprintf(stderr, "'");
            Value constantValue = chunk->constants[constant_index];
            printConstantValue(&constantValue);
            fprintf(stderr, "'\n");
            return offset + 2;
        }
        case CONSTANT16: {
            uint16_t constant_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            fprintf(stderr, "%-16s %4u ", "CONSTANT16", (unsigned)constant_index);
            if (constant_index >= chunk->constants_count) {
                fprintf(stderr, "<INVALID CONST IDX %u>\n", (unsigned)constant_index);
                return offset + 3;
            }
            fprintf(stderr, "'");
            Value constantValue = chunk->constants[constant_index];
            printConstantValue(&constantValue);
            fprintf(stderr, "'\n");
            return offset + 3;
        }
        case CONST_0:
            fprintf(stderr, "CONST_0\n");
            return offset + 1;
        case CONST_1:
            fprintf(stderr, "CONST_1\n");
            return offset + 1;
        case CONST_TRUE:
            fprintf(stderr, "CONST_TRUE\n");
            return offset + 1;
        case CONST_FALSE:
            fprintf(stderr, "CONST_FALSE\n");
            return offset + 1;
        case PUSH_IMMEDIATE_INT8: {
            uint8_t raw = chunk->code[offset + 1];
            int imm = (raw <= 0x7F) ? (int)raw : ((int)raw - 0x100);
            fprintf(stderr, "%-16s %4d\n", "PUSH_IMM_I8", imm);
            return offset + 2;
        }
        case ADD:           fprintf(stderr, "ADD\n"); return offset + 1;
        case SUBTRACT:      fprintf(stderr, "SUBTRACT\n"); return offset + 1;
        case MULTIPLY:      fprintf(stderr, "MULTIPLY\n"); return offset + 1;
        case DIVIDE:        fprintf(stderr, "DIVIDE\n"); return offset + 1;
        case NEGATE:        fprintf(stderr, "NEGATE\n"); return offset + 1;
        case NOT:           fprintf(stderr, "NOT\n"); return offset + 1;
        case TO_BOOL:       fprintf(stderr, "TO_BOOL\n"); return offset + 1;
        case EQUAL:         fprintf(stderr, "EQUAL\n"); return offset + 1;
        case NOT_EQUAL:     fprintf(stderr, "NOT_EQUAL\n"); return offset + 1;
        case GREATER:       fprintf(stderr, "GREATER\n"); return offset + 1;
        case GREATER_EQUAL: fprintf(stderr, "GREATER_EQUAL\n"); return offset + 1;
        case LESS:          fprintf(stderr, "LESS\n"); return offset + 1;
        case LESS_EQUAL:    fprintf(stderr, "LESS_EQUAL\n"); return offset + 1;
        case INT_DIV:       fprintf(stderr, "INT_DIV\n"); return offset + 1;
        case MOD:           fprintf(stderr, "MOD\n"); return offset + 1;
        case AND:           fprintf(stderr, "AND\n"); return offset + 1;
        case OR:            fprintf(stderr, "OR\n"); return offset + 1;
        case XOR:           fprintf(stderr, "XOR\n"); return offset + 1;
        case SHL:           fprintf(stderr, "SHL\n"); return offset + 1;
        case SHR:           fprintf(stderr, "SHR\n"); return offset + 1;

        case JUMP_IF_FALSE: {
            uint16_t jump_operand = (uint16_t)(chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int target_addr = offset + 3 + (int16_t)jump_operand;
            const char* targetName = findProcedureNameByAddress(procedureTable, target_addr);
            fprintf(stderr, "%-16s %4d (to %04d)", "JUMP_IF_FALSE", (int16_t)jump_operand, target_addr);
            if (targetName) {
                fprintf(stderr, " -> %s", targetName);
            }
            fprintf(stderr, "\n");
            return offset + 3;
        }
        case JUMP: {
            uint16_t jump_operand_uint = (uint16_t)(chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int16_t jump_operand_sint = (int16_t)jump_operand_uint;
            int target_addr = offset + 3 + jump_operand_sint;
            const char* targetName = findProcedureNameByAddress(procedureTable, target_addr);
            fprintf(stderr, "%-16s %4d (to %04d)", "JUMP", jump_operand_sint, target_addr);
            if (targetName) {
                fprintf(stderr, " -> %s", targetName);
            }
            fprintf(stderr, "\n");
            return offset + 3;
        }
        case SWAP: fprintf(stderr, "SWAP\n"); return offset + 1;
        case DUP:  fprintf(stderr, "DUP\n"); return offset + 1;

        case DEFINE_GLOBAL: {
            uint8_t name_idx = chunk->code[offset + 1];
            VarType declaredType = (VarType)chunk->code[offset + 2];
            fprintf(stderr, "%-16s NameIdx:%-3d ", "DEFINE_GLOBAL", name_idx);
            if (name_idx < chunk->constants_count && chunk->constants[name_idx].type == TYPE_STRING) {
                fprintf(stderr, "'%s' ", chunk->constants[name_idx].s_val);
            } else {
                fprintf(stderr, "INVALID_NAME_IDX ");
            }
            fprintf(stderr, "Type:%s ", varTypeToString(declaredType));
            int current_offset = offset + 3;
            if (declaredType == TYPE_ARRAY) {
                if (current_offset < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_offset++];
                    fprintf(stderr, "Dims:%d [", dimension_count);
                    for (int i=0; i < dimension_count; i++) {
                        if (current_offset + 3 < chunk->count) {
                            uint16_t lower_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            uint16_t upper_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            fprintf(stderr, "%lld..%lld%s", chunk->constants[lower_idx].i_val, chunk->constants[upper_idx].i_val,
                                   (i == dimension_count - 1) ? "" : ", ");
                        }
                    }
                    fprintf(stderr, "] of ");
                    if (current_offset < chunk->count) {
                        VarType elem_var_type = (VarType)chunk->code[current_offset++];
                        fprintf(stderr, "%s ", varTypeToString(elem_var_type));
                        if (current_offset + 1 < chunk->count) {
                            uint16_t elem_name_idx =
                                (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            if (elem_name_idx < chunk->constants_count &&
                                chunk->constants[elem_name_idx].type == TYPE_STRING) {
                                fprintf(stderr, "('%s')", chunk->constants[elem_name_idx].s_val);
                            }
                        }
                    }
                }
            } else {
                if (current_offset + 1 < chunk->count) {
                    uint16_t type_name_idx =
                        (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                    current_offset += 2;
                    if (type_name_idx > 0 && type_name_idx < chunk->constants_count &&
                        chunk->constants[type_name_idx].type == TYPE_STRING) {
                        fprintf(stderr, "('%s')", chunk->constants[type_name_idx].s_val);
                    }
                    if (declaredType == TYPE_STRING && current_offset + 1 < chunk->count) {
                        uint16_t len_idx =
                            (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                        current_offset += 2;
                        if (len_idx < chunk->constants_count && chunk->constants[len_idx].type == TYPE_INTEGER) {
                            fprintf(stderr, " len=%lld", chunk->constants[len_idx].i_val);
                        }
                    } else if (declaredType == TYPE_FILE && current_offset + 2 < chunk->count) {
                        VarType elem_type = (VarType)chunk->code[current_offset++];
                        uint16_t elem_name_idx =
                            (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                        current_offset += 2;
                        fprintf(stderr, " elem=%s", varTypeToString(elem_type));
                        if (elem_name_idx != 0xFFFF && elem_name_idx < chunk->constants_count &&
                            chunk->constants[elem_name_idx].type == TYPE_STRING) {
                            fprintf(stderr, " ('%s')", chunk->constants[elem_name_idx].s_val);
                        }
                    }
                }
            }
            fprintf(stderr, "\n");
            return current_offset;
        }
        case DEFINE_GLOBAL16: {
            // Variable or constant definition using a 16-bit name index.
            uint16_t name_idx = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            VarType declaredType = (VarType)chunk->code[offset + 3];
            fprintf(stderr, "%-16s NameIdx:%-3d ", "DEFINE_GLOBAL16", name_idx);
            if (name_idx < chunk->constants_count && chunk->constants[name_idx].type == TYPE_STRING) {
                fprintf(stderr, "'%s' ", chunk->constants[name_idx].s_val);
            } else {
                fprintf(stderr, "INVALID_NAME_IDX ");
            }
            fprintf(stderr, "Type:%s ", varTypeToString(declaredType));
            int current_offset = offset + 4;
            if (declaredType == TYPE_ARRAY) {
                if (current_offset < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_offset++];
                    fprintf(stderr, "Dims:%d [", dimension_count);
                    for (int i=0; i < dimension_count; i++) {
                        if (current_offset + 3 < chunk->count) {
                            uint16_t lower_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            uint16_t upper_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            fprintf(stderr, "%lld..%lld%s", chunk->constants[lower_idx].i_val, chunk->constants[upper_idx].i_val,
                                   (i == dimension_count - 1) ? "" : ", ");
                        }
                    }
                    fprintf(stderr, "] of ");
                    if (current_offset < chunk->count) {
                        VarType elem_var_type = (VarType)chunk->code[current_offset++];
                        fprintf(stderr, "%s ", varTypeToString(elem_var_type));
                        if (current_offset + 1 < chunk->count) {
                            uint16_t elem_name_idx =
                                (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            if (elem_name_idx < chunk->constants_count &&
                                chunk->constants[elem_name_idx].type == TYPE_STRING) {
                                fprintf(stderr, "('%s')", chunk->constants[elem_name_idx].s_val);
                            }
                        }
                    }
                }
            } else {
                if (current_offset + 1 < chunk->count) {
                    uint16_t type_name_idx =
                        (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                    current_offset += 2;
                    if (type_name_idx > 0 && type_name_idx < chunk->constants_count &&
                        chunk->constants[type_name_idx].type == TYPE_STRING) {
                        fprintf(stderr, "('%s')", chunk->constants[type_name_idx].s_val);
                    }
                    if (declaredType == TYPE_STRING && current_offset + 1 < chunk->count) {
                        uint16_t len_idx =
                            (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                        current_offset += 2;
                        if (len_idx < chunk->constants_count && chunk->constants[len_idx].type == TYPE_INTEGER) {
                            fprintf(stderr, " len=%lld", chunk->constants[len_idx].i_val);
                        }
                    }
                }
            }
            fprintf(stderr, "\n");
            return current_offset;
        }
        case GET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            const char* name = (name_index < chunk->constants_count &&
                                chunk->constants[name_index].type == TYPE_STRING &&
                                chunk->constants[name_index].s_val)
                                   ? chunk->constants[name_index].s_val
                                   : "<invalid>";
            uintptr_t cached = readInlineCachePtr(chunk, offset + 2);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u '%s' cache=%s\n", "GET_GLOBAL",
                    (unsigned)name_index, name, cache_string);
            return offset + 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case SET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            const char* name = (name_index < chunk->constants_count &&
                                chunk->constants[name_index].type == TYPE_STRING &&
                                chunk->constants[name_index].s_val)
                                   ? chunk->constants[name_index].s_val
                                   : "<invalid>";
            uintptr_t cached = readInlineCachePtr(chunk, offset + 2);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u '%s' cache=%s\n", "SET_GLOBAL",
                    (unsigned)name_index, name, cache_string);
            return offset + 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL_CACHED: {
            uint8_t name_index = chunk->code[offset + 1];
            uintptr_t cached = readInlineCachePtr(chunk, offset + 2);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u cache=%s\n", "GET_GLOBAL_CACHED",
                    (unsigned)name_index, cache_string);
            return offset + 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case SET_GLOBAL_CACHED: {
            uint8_t name_index = chunk->code[offset + 1];
            uintptr_t cached = readInlineCachePtr(chunk, offset + 2);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u cache=%s\n", "SET_GLOBAL_CACHED",
                    (unsigned)name_index, cache_string);
            return offset + 2 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL_ADDRESS: {
            uint8_t name_index = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d '%s'\n", "GET_GLOBAL_ADDRESS", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 2;
        }
        case GET_GLOBAL16: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            const char* name = (name_index < chunk->constants_count &&
                                chunk->constants[name_index].type == TYPE_STRING &&
                                chunk->constants[name_index].s_val)
                                   ? chunk->constants[name_index].s_val
                                   : "<invalid>";
            uintptr_t cached = readInlineCachePtr(chunk, offset + 3);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u '%s' cache=%s\n", "GET_GLOBAL16",
                    (unsigned)name_index, name, cache_string);
            return offset + 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case SET_GLOBAL16: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            const char* name = (name_index < chunk->constants_count &&
                                chunk->constants[name_index].type == TYPE_STRING &&
                                chunk->constants[name_index].s_val)
                                   ? chunk->constants[name_index].s_val
                                   : "<invalid>";
            uintptr_t cached = readInlineCachePtr(chunk, offset + 3);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u '%s' cache=%s\n", "SET_GLOBAL16",
                    (unsigned)name_index, name, cache_string);
            return offset + 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL16_CACHED: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            uintptr_t cached = readInlineCachePtr(chunk, offset + 3);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u cache=%s\n", "GET_GLOBAL16_CACHED",
                    (unsigned)name_index, cache_string);
            return offset + 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case SET_GLOBAL16_CACHED: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            uintptr_t cached = readInlineCachePtr(chunk, offset + 3);
            char cache_buffer[32];
            const char* cache_string = formatInlineCachePointer(cached, cache_buffer, sizeof(cache_buffer));
            fprintf(stderr, "%-16s %4u cache=%s\n", "SET_GLOBAL16_CACHED",
                    (unsigned)name_index, cache_string);
            return offset + 3 + GLOBAL_INLINE_CACHE_SLOT_SIZE;
        }
        case GET_GLOBAL_ADDRESS16: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            fprintf(stderr, "%-16s %4d '%s'\n", "GET_GLOBAL_ADDRESS16", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 3;
        }
        case GET_LOCAL: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "GET_LOCAL", slot);
            return offset + 2;
        }
        case SET_LOCAL: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "SET_LOCAL", slot);
            return offset + 2;
        }
        case INC_LOCAL: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "INC_LOCAL", slot);
            return offset + 2;
        }
        case DEC_LOCAL: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "DEC_LOCAL", slot);
            return offset + 2;
        }
        case GET_UPVALUE: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "GET_UPVALUE", slot);
            return offset + 2;
        }
        case SET_UPVALUE: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "SET_UPVALUE", slot);
            return offset + 2;
        }
        case GET_UPVALUE_ADDRESS: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "GET_UPVALUE_ADDRESS", slot);
            return offset + 2;
        }
        case INIT_FIELD_ARRAY: {
            uint8_t field = chunk->code[offset + 1];
            uint8_t dim_count = chunk->code[offset + 2];
            fprintf(stderr, "%-16s Field:%d Dims:%d", "INIT_FIELD_ARRAY", field, dim_count);
            int current_offset = offset + 3 + dim_count * 4;
            int next_offset = offset + 6 + dim_count * 4;
            if (current_offset < chunk->count) {
                VarType elem_type = (VarType)chunk->code[current_offset++];
                fprintf(stderr, " Elem:%s", varTypeToString(elem_type));
                if (current_offset + 1 < chunk->count) {
                    uint16_t elem_name_idx =
                        (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                    current_offset += 2;
                    if (elem_name_idx != 0xFFFF &&
                        elem_name_idx < chunk->constants_count &&
                        chunk->constants[elem_name_idx].type == TYPE_STRING &&
                        chunk->constants[elem_name_idx].s_val) {
                        fprintf(stderr, " ('%s')", chunk->constants[elem_name_idx].s_val);
                    } else if (elem_name_idx != 0xFFFF) {
                        fprintf(stderr, " idx=%u", elem_name_idx);
                    }
                }
            }
            fprintf(stderr, "\n");
            return next_offset;
        }
        case INIT_LOCAL_ARRAY: {
            uint8_t slot = chunk->code[offset + 1];
            uint8_t dim_count = chunk->code[offset + 2];
            fprintf(stderr, "%-16s Slot:%d Dims:%d", "INIT_LOCAL_ARRAY", slot, dim_count);
            int current_offset = offset + 3 + dim_count * 4;
            int next_offset = offset + 6 + dim_count * 4;
            if (current_offset < chunk->count) {
                VarType elem_type = (VarType)chunk->code[current_offset++];
                fprintf(stderr, " Elem:%s", varTypeToString(elem_type));
                if (current_offset + 1 < chunk->count) {
                    uint16_t elem_name_idx =
                        (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                    current_offset += 2;
                    if (elem_name_idx != 0xFFFF &&
                        elem_name_idx < chunk->constants_count &&
                        chunk->constants[elem_name_idx].type == TYPE_STRING &&
                        chunk->constants[elem_name_idx].s_val) {
                        fprintf(stderr, " ('%s')", chunk->constants[elem_name_idx].s_val);
                    } else if (elem_name_idx != 0xFFFF) {
                        fprintf(stderr, " idx=%u", elem_name_idx);
                    }
                }
            }
            fprintf(stderr, "\n");
            return next_offset;
        }
        case INIT_LOCAL_FILE: {
            uint8_t slot = chunk->code[offset + 1];
            VarType elem_type = (VarType)chunk->code[offset + 2];
            uint16_t name_idx = (uint16_t)(chunk->code[offset + 3] << 8) | chunk->code[offset + 4];
            fprintf(stderr, "%-16s %4d (slot) %-8s", "INIT_LOCAL_FILE", slot, varTypeToString(elem_type));
            if (name_idx != 0xFFFF) {
                fprintf(stderr, " idx=%u", name_idx);
                if (name_idx < chunk->constants_count && chunk->constants[name_idx].type == TYPE_STRING &&
                    chunk->constants[name_idx].s_val) {
                    fprintf(stderr, " '%s'", chunk->constants[name_idx].s_val);
                }
            }
            fprintf(stderr, "\n");
            return offset + 5;
        }
        case INIT_LOCAL_STRING: {
            uint8_t slot = chunk->code[offset + 1];
            uint8_t length = chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d (slot) %4d (len)\n", "INIT_LOCAL_STRING", slot, length);
            return offset + 3;
        }
        case INIT_LOCAL_POINTER: {
            uint8_t slot = chunk->code[offset + 1];
            uint16_t name_idx = (uint16_t)(chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
            fprintf(stderr, "%-16s %4d (slot) %4d", "INIT_LOCAL_POINTER", slot, name_idx);
            if (name_idx < chunk->constants_count &&
                chunk->constants[name_idx].type == TYPE_STRING) {
                fprintf(stderr, " '%s'", chunk->constants[name_idx].s_val);
            }
            fprintf(stderr, "\n");
            return offset + 4;
        }
        case GET_LOCAL_ADDRESS: {
            uint8_t slot = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (slot)\n", "GET_LOCAL_ADDRESS", slot);
            return offset + 2;
        }
        case GET_FIELD_ADDRESS: {
            uint8_t const_idx = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d ", "GET_FIELD_ADDRESS", const_idx);
            if (const_idx < chunk->constants_count &&
                chunk->constants[const_idx].type == TYPE_STRING) {
                fprintf(stderr, "'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                fprintf(stderr, "<INVALID FIELD CONST>\n");
            }
            return offset + 2;
        }
        case GET_FIELD_ADDRESS16: {
            uint16_t const_idx = (uint16_t)(chunk->code[offset + 1] << 8) |
                                 chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d ", "GET_FIELD_ADDRESS16", const_idx);
            if (const_idx < chunk->constants_count &&
                chunk->constants[const_idx].type == TYPE_STRING) {
                fprintf(stderr, "'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                fprintf(stderr, "<INVALID FIELD CONST>\n");
            }
            return offset + 3;
        }
        case LOAD_FIELD_VALUE_BY_NAME: {
            uint8_t const_idx = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d ", "LOAD_FIELD_VALUE_BY_NAME", const_idx);
            if (const_idx < chunk->constants_count &&
                chunk->constants[const_idx].type == TYPE_STRING) {
                fprintf(stderr, "'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                fprintf(stderr, "<INVALID FIELD CONST>\n");
            }
            return offset + 2;
        }
        case LOAD_FIELD_VALUE_BY_NAME16: {
            uint16_t const_idx = (uint16_t)(chunk->code[offset + 1] << 8) |
                                 chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d ", "LOAD_FIELD_VALUE_BY_NAME16", const_idx);
            if (const_idx < chunk->constants_count &&
                chunk->constants[const_idx].type == TYPE_STRING) {
                fprintf(stderr, "'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                fprintf(stderr, "<INVALID FIELD CONST>\n");
            }
            return offset + 3;
        }
        case ALLOC_OBJECT: {
            uint8_t fields = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (fields)\n", "ALLOC_OBJECT", fields);
            return offset + 2;
        }
        case ALLOC_OBJECT16: {
            uint16_t fields = (uint16_t)(chunk->code[offset + 1] << 8) |
                              chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d (fields)\n", "ALLOC_OBJECT16", fields);
            return offset + 3;
        }
        case GET_FIELD_OFFSET: {
            uint8_t idx = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (index)\n", "GET_FIELD_OFFSET", idx);
            return offset + 2;
        }
        case GET_FIELD_OFFSET16: {
            uint16_t idx = (uint16_t)(chunk->code[offset + 1] << 8) |
                            chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d (index)\n", "GET_FIELD_OFFSET16", idx);
            return offset + 3;
        }
        case LOAD_FIELD_VALUE: {
            uint8_t idx = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (index)\n", "LOAD_FIELD_VALUE", idx);
            return offset + 2;
        }
        case LOAD_FIELD_VALUE16: {
            uint16_t idx = (uint16_t)(chunk->code[offset + 1] << 8) |
                            chunk->code[offset + 2];
            fprintf(stderr, "%-16s %4d (index)\n", "LOAD_FIELD_VALUE16", idx);
            return offset + 3;
        }
        case GET_ELEMENT_ADDRESS: {
            uint8_t dims = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (dims)\n", "GET_ELEMENT_ADDRESS", dims);
            return offset + 2;
        }
        case GET_ELEMENT_ADDRESS_CONST: {
            uint32_t flat = ((uint32_t)chunk->code[offset + 1] << 24) |
                            ((uint32_t)chunk->code[offset + 2] << 16) |
                            ((uint32_t)chunk->code[offset + 3] << 8) |
                            (uint32_t)chunk->code[offset + 4];
            fprintf(stderr, "%-16s %10u (flat offset)\n", "GET_ELEMENT_ADDRESS_CONST", flat);
            return offset + 5;
        }
        case LOAD_ELEMENT_VALUE: {
            uint8_t dims = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (dims)\n", "LOAD_ELEMENT_VALUE", dims);
            return offset + 2;
        }
        case LOAD_ELEMENT_VALUE_CONST: {
            uint32_t flat = ((uint32_t)chunk->code[offset + 1] << 24) |
                            ((uint32_t)chunk->code[offset + 2] << 16) |
                            ((uint32_t)chunk->code[offset + 3] << 8) |
                            (uint32_t)chunk->code[offset + 4];
            fprintf(stderr, "%-16s %10u (flat offset)\n", "LOAD_ELEMENT_VALUE_CONST", flat);
            return offset + 5;
        }
        case GET_CHAR_ADDRESS:
            fprintf(stderr, "GET_CHAR_ADDRESS\n"); // <-- ADDED MISSING CASE
            return offset + 1;
        case SET_INDIRECT:
            fprintf(stderr, "SET_INDIRECT\n");
            return offset + 1;
        case GET_INDIRECT:
            fprintf(stderr, "GET_INDIRECT\n");
            return offset + 1;
        case IN:
            fprintf(stderr, "IN\n");
            return offset + 1;
        case GET_CHAR_FROM_STRING:
            fprintf(stderr, "GET_CHAR_FROM_STRING\n");
            return offset + 1;

        case CALL_BUILTIN: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) |
                                             chunk->code[offset + 2]);
            uint8_t arg_count = chunk->code[offset + 3];
            const char* name = "<INVALID>";
            if (name_index < chunk->constants_count &&
                chunk->constants[name_index].type == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                name = AS_STRING(chunk->constants[name_index]);
            }
            const char* lower_name = NULL;
            int lower_idx = getBuiltinLowercaseIndex(chunk, (int)name_index);
            if (lower_idx >= 0 && lower_idx < chunk->constants_count &&
                chunk->constants[lower_idx].type == TYPE_STRING &&
                AS_STRING(chunk->constants[lower_idx])) {
                lower_name = AS_STRING(chunk->constants[lower_idx]);
            }
            if (lower_name && name && strcmp(name, lower_name) != 0) {
                fprintf(stderr, "%-16s %5d '%s' (lower='%s') (%d args)\n",
                        "CALL_BUILTIN", name_index, name, lower_name, arg_count);
            } else {
                fprintf(stderr, "%-16s %5d '%s' (%d args)\n",
                        "CALL_BUILTIN", name_index, name, arg_count);
            }
            return offset + 4;
        }

        // These are not currently used in your compiler but are in the enum
        case CALL_BUILTIN_PROC: {
            uint16_t builtin_id = (uint16_t)((chunk->code[offset + 1] << 8) |
                                             chunk->code[offset + 2]);
            uint16_t name_index = (uint16_t)((chunk->code[offset + 3] << 8) |
                                             chunk->code[offset + 4]);
            uint8_t arg_count = chunk->code[offset + 5];
            const char* name = NULL;
            if (name_index < chunk->constants_count &&
                chunk->constants[name_index].type == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                name = AS_STRING(chunk->constants[name_index]);
            }
            if (!name) {
                name = getVmBuiltinNameById((int)builtin_id);
            }
            if (!name) name = "<UNKNOWN>";
            fprintf(stderr, "%-16s %5u '%s' (%u args)\n",
                    "CALL_BUILTIN_PROC", builtin_id, name, arg_count);
            return offset + 6;
        }
        case CALL_USER_PROC: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) |
                                             chunk->code[offset + 2]);
            uint8_t arg_count = chunk->code[offset + 3];
            const char* name = NULL;
            if (name_index < chunk->constants_count &&
                chunk->constants[name_index].type == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                name = AS_STRING(chunk->constants[name_index]);
            }

            const char* display_name = name ? name : "<INVALID>";
            int entry_address = -1;
            if (procedureTable && name && *name) {
                char lookup_name[MAX_SYMBOL_LENGTH + 1];
                strncpy(lookup_name, name, MAX_SYMBOL_LENGTH);
                lookup_name[MAX_SYMBOL_LENGTH] = '\0';
                toLowerString(lookup_name);
                Symbol* sym = hashTableLookup(procedureTable, lookup_name);
                sym = resolveSymbolAlias(sym);
                if (sym && sym->is_defined) {
                    entry_address = sym->bytecode_address;
                }
            }

            if (entry_address >= 0) {
                fprintf(stderr, "%-16s %5u '%s' @%04d (%u args)\n",
                        "CALL_USER_PROC", name_index, display_name,
                        (uint16_t)entry_address, arg_count);
            } else {
                fprintf(stderr, "%-16s %5u '%s' (%u args)\n",
                        "CALL_USER_PROC", name_index, display_name, arg_count);
            }
            return offset + 4;
        }

        case CALL_HOST: {
            uint8_t host_fn_id_val = chunk->code[offset + 1];
            fprintf(stderr, "%-16s %4d (ID: %d)\n", "CALL_HOST", host_fn_id_val, host_fn_id_val);
            return offset + 2;
        }
        case POP:
            fprintf(stderr, "POP\n");
            return offset + 1;
        case CALL: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) |
                                             chunk->code[offset + 2]);
            uint16_t address = (uint16_t)((chunk->code[offset + 3] << 8) |
                                          chunk->code[offset + 4]);
            uint8_t declared_arity = chunk->code[offset + 5];
            const char* targetProcName = "<INVALID>";
            if (name_index < chunk->constants_count &&
                chunk->constants[name_index].type == TYPE_STRING &&
                AS_STRING(chunk->constants[name_index])) {
                targetProcName = AS_STRING(chunk->constants[name_index]);
            }
            fprintf(stderr, "%-16s %04d (%s) (%d args)\n",
                    "CALL", address, targetProcName, declared_arity);
            return offset + 6;
        }
        case CALL_INDIRECT: {
            uint8_t arg_count = chunk->code[offset + 1];
            fprintf(stderr, "%-16s (args=%d)\n", "CALL_INDIRECT", arg_count);
            return offset + 2;
        }
        case PROC_CALL_INDIRECT: {
            uint8_t arg_count = chunk->code[offset + 1];
            fprintf(stderr, "%-16s (args=%d)\n", "PROC_CALL_INDIRECT", arg_count);
            return offset + 2;
        }
        case HALT:
            fprintf(stderr, "HALT\n");
            return offset + 1;
        case EXIT:
            fprintf(stderr, "EXIT\n");
            return offset + 1;
        case FORMAT_VALUE: {
            uint8_t width = chunk->code[offset+1];
            uint8_t precision = chunk->code[offset+2];
            fprintf(stderr, "%-16s width:%d prec:%d\n", "FORMAT_VALUE", width, (int8_t)precision);
            return offset + 3;
        }
        case THREAD_CREATE: {
            uint16_t entry = (uint16_t)((chunk->code[offset + 1] << 8) |
                                       chunk->code[offset + 2]);
            fprintf(stderr, "%-16s %04d\n", "THREAD_CREATE", entry);
            return offset + 3;
        }
        case THREAD_JOIN:
            fprintf(stderr, "THREAD_JOIN\n");
            return offset + 1;
        case MUTEX_CREATE:
            fprintf(stderr, "MUTEX_CREATE\n");
            return offset + 1;
        case RCMUTEX_CREATE:
            fprintf(stderr, "RCMUTEX_CREATE\n");
            return offset + 1;
        case MUTEX_LOCK:
            fprintf(stderr, "MUTEX_LOCK\n");
            return offset + 1;
        case MUTEX_UNLOCK:
            fprintf(stderr, "MUTEX_UNLOCK\n");
            return offset + 1;
        case MUTEX_DESTROY:
            fprintf(stderr, "MUTEX_DESTROY\n");
            return offset + 1;
        // NOTE: There is no BREAK in your bytecode.h enum, so it cannot be disassembled.
        // The AST_BREAK node is handled by the compiler generating jump instructions.

        default:
            fprintf(stderr, "Unknown opcode %d\n", instruction);
            return offset + 1;
    }
}

void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name, HashTable* procedureTable) {
    fprintf(stderr, "== Disassembly: %s ==\n", name);
    fprintf(stderr, "Offset Line Opcode           Operand  Value / Target (Args)\n");
    fprintf(stderr, "------ ---- ---------------- -------- --------------------------\n");

    for (int offset = 0; offset < chunk->count; ) {
        const char* procNameAtOffset = findProcedureNameByAddress(procedureTable, offset);
        if (procNameAtOffset) {
            const char* routineTypeStr = "Routine"; // Default
            if (procedureTable) {
                Symbol* sym = lookupSymbolIn(procedureTable, procNameAtOffset); // Assumes lookupSymbolIn is robust
                if (sym && sym->type_def) {
                    if (sym->type_def->type == AST_FUNCTION_DECL) {
                        routineTypeStr = "Function";
                    } else if (sym->type_def->type == AST_PROCEDURE_DECL) {
                        routineTypeStr = "Procedure";
                    }
                }
            }
            fprintf(stderr, "\n");
            fprintf(stderr, "--- %s %s (at %04d) ---\n", routineTypeStr, procNameAtOffset, offset);
        }
        // Call the public, enhanced disassembleInstruction
        offset = disassembleInstruction(chunk, offset, procedureTable);
    }
    fprintf(stderr, "== End Disassembly: %s ==\n\n", name);

    if (chunk->constants_count > 0) {
        fprintf(stderr, "Constants (%d):\\n", chunk->constants_count);
        for (int i = 0; i < chunk->constants_count; i++) {
            fprintf(stderr, "  %04d: ", i);
            Value constantValue = chunk->constants[i];
            switch(constantValue.type) {
                case TYPE_INTEGER:
                    fprintf(stderr, "INT   %lld\n", constantValue.i_val);
                    break;
                case TYPE_FLOAT:
                case TYPE_DOUBLE:
                case TYPE_LONG_DOUBLE:
                    fprintf(stderr, "REAL  %Lf\n", AS_REAL(constantValue));
                    break;
                case TYPE_STRING:
                    fprintf(stderr, "STR   \"");
                    if (constantValue.s_val) {
                        printEscapedString(constantValue.s_val);
                    } else {
                    fprintf(stderr, "NULL_STR");
                    }
                    fprintf(stderr, "\"");
                    int lower_idx = getBuiltinLowercaseIndex(chunk, i);
                    if (lower_idx >= 0 && lower_idx < chunk->constants_count &&
                        chunk->constants[lower_idx].type == TYPE_STRING &&
                        chunk->constants[lower_idx].s_val &&
                        (!constantValue.s_val || strcmp(constantValue.s_val, chunk->constants[lower_idx].s_val) != 0)) {
                        fprintf(stderr, " (lower -> %04d: \"", lower_idx);
                        printEscapedString(chunk->constants[lower_idx].s_val);
                        fprintf(stderr, "\"");
                    }
                    fprintf(stderr, "\n");
                    break;
                case TYPE_CHAR:
                    fprintf(stderr, "CHAR  '");
                    printEscapedChar(constantValue.c_val);
                    fprintf(stderr, "'\n");
                    break;
                case TYPE_BOOLEAN:
                    fprintf(stderr, "BOOL  %s\n", constantValue.i_val ? "true" : "false");
                    break;
                case TYPE_CLOSURE:
                    fprintf(stderr, "CLOS  ");
                    printConstantValue(&constantValue);
                    fprintf(stderr, "\n");
                    break;
                case TYPE_NIL:
                    fprintf(stderr, "NIL\n");
                    break;
                default:
                    fprintf(stderr, "Value type %s\n", varTypeToString(constantValue.type));
                    break;
            }
        }
        fprintf(stderr, "\n");
    }
}
