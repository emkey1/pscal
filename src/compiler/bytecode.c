// src/compiler/bytecode.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memcpy

#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For freeValue, varTypeToString
#include "symbol/symbol.h" // For Symbol struct, HashTable, lookupSymbolIn
#include "vm/vm.h"         // For HostFunctionID type (used in OP_CALL_HOST cast)
#include "backend_ast/interpreter.h" 

// initBytecodeChunk, freeBytecodeChunk, reallocate, writeBytecodeChunk,
// addConstantToChunk, emitShort, patchShort from your provided file.

void initBytecodeChunk(BytecodeChunk* chunk) { // From all.txt
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->constants_count = 0;
    chunk->constants_capacity = 0;
    chunk->constants = NULL;
  //  chunk->lines = 0;
}

void freeBytecodeChunk(BytecodeChunk* chunk) { // From all.txt
    free(chunk->code);
    free(chunk->lines);
    for (int i = 0; i < chunk->constants_count; i++) {
        freeValue(&chunk->constants[i]);
    }
    free(chunk->constants);
    initBytecodeChunk(chunk);
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
        exit(1);
    }
    return result;
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
            if (existing->type == TYPE_REAL && existing->r_val == value->r_val) return i;
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
    }
    
    // Perform a deep copy from the provided pointer.
    chunk->constants[chunk->constants_count] = makeCopyOfValue(value);

    // The function NO LONGER frees the incoming value. The caller is responsible.
    
    return chunk->constants_count++;
}

void emitShort(BytecodeChunk* chunk, uint16_t value, int line) { // From all.txt
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

// Corrected helper function to find procedure/function name by its bytecode address
static const char* findProcedureNameByAddress(HashTable* procedureTable, uint16_t address) {
    if (!procedureTable) return NULL;
    for (int i = 0; i < HASHTABLE_SIZE; i++) { // Using HASHTABLE_SIZE from symbol.h
        Symbol* symbol = procedureTable->buckets[i]; // Using 'buckets' from symbol.h
        while (symbol != NULL) {
            if (symbol->type_def &&
                (symbol->type_def->type == AST_PROCEDURE_DECL || symbol->type_def->type == AST_FUNCTION_DECL) &&
                symbol->is_defined &&
                symbol->bytecode_address == address) {
                return symbol->name;
            }
            symbol = symbol->next; // Using 'next' from symbol.h
        }
    }
    return NULL;
}
// New helper function to get the length of an instruction at a given offset.
int getInstructionLength(BytecodeChunk* chunk, int offset) {
    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_CONSTANT:
        case OP_GET_GLOBAL:
        case OP_SET_GLOBAL:
        case OP_GET_LOCAL:
        case OP_SET_LOCAL:
        case OP_GET_GLOBAL_ADDRESS:
        case OP_GET_LOCAL_ADDRESS:
        case OP_GET_FIELD_ADDRESS:
        case OP_GET_ELEMENT_ADDRESS:
        case OP_GET_CHAR_ADDRESS:
        case OP_WRITE:
        case OP_WRITE_LN:
        case OP_INIT_LOCAL_FILE:
            return 2; // 1-byte opcode + 1-byte operand
        case OP_INIT_LOCAL_POINTER:
            return 4; // opcode + slot byte + 2-byte type name index
        case OP_INIT_LOCAL_ARRAY: {
            int current_pos = offset + 1; // after opcode
            current_pos++; // slot
            if (current_pos >= chunk->count) return 1;
            uint8_t dimension_count = chunk->code[current_pos++];
            current_pos += dimension_count * 4; // bounds indices (two 16-bit indices per dimension)
            current_pos += 2; // elem type and elem type name index
            return current_pos - offset;
        }
        case OP_CONSTANT16:
        case OP_GET_FIELD_ADDRESS16:
        case OP_GET_GLOBAL16:
        case OP_SET_GLOBAL16:
        case OP_GET_GLOBAL_ADDRESS16:
            return 3; // 1 byte opcode + 2-byte operand
        case OP_JUMP:
        case OP_JUMP_IF_FALSE:
        case OP_CALL_BUILTIN:
        case OP_FORMAT_VALUE:
            return 3; // 1-byte opcode + 2-byte operand
        case OP_CALL:
            return 5; // 1-byte opcode + 1-byte name_idx + 2-byte addr + 1-byte arity
        case OP_DEFINE_GLOBAL: {
            // This instruction has a variable length.
            int current_pos = offset + 1; // Position after the opcode
            if (current_pos + 1 >= chunk->count) return 1; // Safeguard for incomplete instruction

            VarType declaredType = (VarType)chunk->code[offset + 2];
            current_pos = offset + 3; // Position after the type byte

            if (declaredType == TYPE_ARRAY) {
                if (current_pos < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_pos++];
                    current_pos += dimension_count * 4; // Skip over all the bounds indices
                    current_pos += 2; // Skip element VarType and element type name index
                }
            } else {
                // Simple types store a 16-bit type name index and, for strings,
                // an extra 16-bit length constant.
                current_pos += 2; // type name index (16-bit)
                if (declaredType == TYPE_STRING) {
                    current_pos += 2; // length constant index (16-bit)
                }
            }
            return (current_pos - offset); // Return the total calculated length
        }
        case OP_DEFINE_GLOBAL16: {
            // Similar to OP_DEFINE_GLOBAL but with a 16-bit name index.
            int current_pos = offset + 1; // after opcode
            if (current_pos + 2 >= chunk->count) return 1; // ensure enough bytes for name and type
            VarType declaredType = (VarType)chunk->code[offset + 3];
            current_pos = offset + 4; // position after type byte

            if (declaredType == TYPE_ARRAY) {
                if (current_pos < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_pos++];
                    current_pos += dimension_count * 4; // bounds indices
                    current_pos += 2; // element var type and element type name index
                }
            } else {
                current_pos += 2; // type name index (16-bit)
                if (declaredType == TYPE_STRING) {
                    current_pos += 2; // length constant index (16-bit)
                }
            }
            return (current_pos - offset);
        }
        default:
            return 1; // All other opcodes are 1 byte.
    }
}

// This is the function declared in bytecode.h and called by disassembleBytecodeChunk
// It was already non-static in your provided bytecode.c
int disassembleInstruction(BytecodeChunk* chunk, int offset, HashTable* procedureTable) {
    printf("%04d ", offset);
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | ");
    } else {
        printf("%4d ", chunk->lines[offset]);
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_RETURN:
            printf("OP_RETURN\n");
            return offset + 1;

        case OP_CONSTANT: {
            uint8_t constant_index = chunk->code[offset + 1];
            printf("%-16s %4u ", "OP_CONSTANT", (unsigned)constant_index);
            if (constant_index >= chunk->constants_count) {
                printf("<INVALID CONST IDX %u>\n", (unsigned)constant_index);
                return offset + 2;
            }
            printf("'");
            Value constantValue = chunk->constants[constant_index];
            switch(constantValue.type) {
                case TYPE_INTEGER: printf("%lld", constantValue.i_val); break;
                case TYPE_REAL:    printf("%f", constantValue.r_val); break;
                case TYPE_STRING:  printf("%s", constantValue.s_val ? constantValue.s_val : "NULL_STR"); break;
                case TYPE_CHAR:    printf("%c", constantValue.c_val); break;
                case TYPE_BOOLEAN: printf("%s", constantValue.i_val ? "true" : "false"); break;
                case TYPE_NIL:     printf("nil"); break;
                default:           printf("Value type %s", varTypeToString(constantValue.type)); break;
            }
            printf("'\n");
            return offset + 2;
        }
        case OP_CONSTANT16: {
            uint16_t constant_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            printf("%-16s %4u ", "OP_CONSTANT16", (unsigned)constant_index);
            if (constant_index >= chunk->constants_count) {
                printf("<INVALID CONST IDX %u>\n", (unsigned)constant_index);
                return offset + 3;
            }
            printf("'");
            Value constantValue = chunk->constants[constant_index];
            switch(constantValue.type) {
                case TYPE_INTEGER: printf("%lld", constantValue.i_val); break;
                case TYPE_REAL:    printf("%f", constantValue.r_val); break;
                case TYPE_STRING:  printf("%s", constantValue.s_val ? constantValue.s_val : "NULL_STR"); break;
                case TYPE_CHAR:    printf("%c", constantValue.c_val); break;
                case TYPE_BOOLEAN: printf("%s", constantValue.i_val ? "true" : "false"); break;
                case TYPE_NIL:     printf("nil"); break;
                default:           printf("Value type %s", varTypeToString(constantValue.type)); break;
            }
            printf("'\n");
            return offset + 3;
        }
        case OP_ADD:           printf("OP_ADD\n"); return offset + 1;
        case OP_SUBTRACT:      printf("OP_SUBTRACT\n"); return offset + 1;
        case OP_MULTIPLY:      printf("OP_MULTIPLY\n"); return offset + 1;
        case OP_DIVIDE:        printf("OP_DIVIDE\n"); return offset + 1;
        case OP_NEGATE:        printf("OP_NEGATE\n"); return offset + 1;
        case OP_NOT:           printf("OP_NOT\n"); return offset + 1;
        case OP_EQUAL:         printf("OP_EQUAL\n"); return offset + 1;
        case OP_NOT_EQUAL:     printf("OP_NOT_EQUAL\n"); return offset + 1;
        case OP_GREATER:       printf("OP_GREATER\n"); return offset + 1;
        case OP_GREATER_EQUAL: printf("OP_GREATER_EQUAL\n"); return offset + 1;
        case OP_LESS:          printf("OP_LESS\n"); return offset + 1;
        case OP_LESS_EQUAL:    printf("OP_LESS_EQUAL\n"); return offset + 1;
        case OP_INT_DIV:       printf("OP_INT_DIV\n"); return offset + 1;
        case OP_MOD:           printf("OP_MOD\n"); return offset + 1;
        case OP_AND:           printf("OP_AND\n"); return offset + 1;
        case OP_OR:            printf("OP_OR\n"); return offset + 1;
        case OP_SHL:           printf("OP_SHL\n"); return offset + 1;
        case OP_SHR:           printf("OP_SHR\n"); return offset + 1;

        case OP_JUMP_IF_FALSE: {
            uint16_t jump_operand = (uint16_t)(chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int target_addr = offset + 3 + (int16_t)jump_operand;
            const char* targetName = findProcedureNameByAddress(procedureTable, target_addr);
            printf("%-16s %4d (to %04X)", "OP_JUMP_IF_FALSE", (int16_t)jump_operand, target_addr);
            if (targetName) {
                printf(" -> %s", targetName);
            }
            printf("\n");
            return offset + 3;
        }
        case OP_JUMP: {
            uint16_t jump_operand_uint = (uint16_t)(chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            int16_t jump_operand_sint = (int16_t)jump_operand_uint;
            int target_addr = offset + 3 + jump_operand_sint;
            const char* targetName = findProcedureNameByAddress(procedureTable, target_addr);
            printf("%-16s %4d (to %04X)", "OP_JUMP", jump_operand_sint, target_addr);
            if (targetName) {
                printf(" -> %s", targetName);
            }
            printf("\n");
            return offset + 3;
        }
        case OP_SWAP: printf("OP_SWAP\n"); return offset + 1;
        case OP_DUP:  printf("OP_DUP\n"); return offset + 1;

        case OP_DEFINE_GLOBAL: {
            uint8_t name_idx = chunk->code[offset + 1];
            VarType declaredType = (VarType)chunk->code[offset + 2];
            printf("%-16s NameIdx:%-3d ", "OP_DEFINE_GLOBAL", name_idx);
            if (name_idx < chunk->constants_count && chunk->constants[name_idx].type == TYPE_STRING) {
                printf("'%s' ", chunk->constants[name_idx].s_val);
            } else {
                printf("INVALID_NAME_IDX ");
            }
            printf("Type:%s ", varTypeToString(declaredType));
            int current_offset = offset + 3;
            if (declaredType == TYPE_ARRAY) {
                if (current_offset < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_offset++];
                    printf("Dims:%d [", dimension_count);
                    for (int i=0; i < dimension_count; i++) {
                        if (current_offset + 3 < chunk->count) {
                            uint16_t lower_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            uint16_t upper_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            printf("%lld..%lld%s", chunk->constants[lower_idx].i_val, chunk->constants[upper_idx].i_val,
                                   (i == dimension_count - 1) ? "" : ", ");
                        }
                    }
                    printf("] of ");
                    if (current_offset < chunk->count) {
                        VarType elem_var_type = (VarType)chunk->code[current_offset++];
                        printf("%s ", varTypeToString(elem_var_type));
                        if (current_offset < chunk->count) {
                            uint8_t elem_name_idx = chunk->code[current_offset++];
                            if (elem_name_idx < chunk->constants_count && chunk->constants[elem_name_idx].type == TYPE_STRING) {
                                printf("('%s')", chunk->constants[elem_name_idx].s_val);
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
                        printf("('%s')", chunk->constants[type_name_idx].s_val);
                    }
                    if (declaredType == TYPE_STRING && current_offset + 1 < chunk->count) {
                        uint16_t len_idx =
                            (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                        current_offset += 2;
                        if (len_idx < chunk->constants_count && chunk->constants[len_idx].type == TYPE_INTEGER) {
                            printf(" len=%lld", chunk->constants[len_idx].i_val);
                        }
                    }
                }
            }
            printf("\n");
            return current_offset;
        }
        case OP_DEFINE_GLOBAL16: {
            // Variable or constant definition using a 16-bit name index.
            uint16_t name_idx = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            VarType declaredType = (VarType)chunk->code[offset + 3];
            printf("%-16s NameIdx:%-3d ", "OP_DEFINE_GLOBAL16", name_idx);
            if (name_idx < chunk->constants_count && chunk->constants[name_idx].type == TYPE_STRING) {
                printf("'%s' ", chunk->constants[name_idx].s_val);
            } else {
                printf("INVALID_NAME_IDX ");
            }
            printf("Type:%s ", varTypeToString(declaredType));
            int current_offset = offset + 4;
            if (declaredType == TYPE_ARRAY) {
                if (current_offset < chunk->count) {
                    uint8_t dimension_count = chunk->code[current_offset++];
                    printf("Dims:%d [", dimension_count);
                    for (int i=0; i < dimension_count; i++) {
                        if (current_offset + 3 < chunk->count) {
                            uint16_t lower_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            uint16_t upper_idx = (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                            current_offset += 2;
                            printf("%lld..%lld%s", chunk->constants[lower_idx].i_val, chunk->constants[upper_idx].i_val,
                                   (i == dimension_count - 1) ? "" : ", ");
                        }
                    }
                    printf("] of ");
                    if (current_offset < chunk->count) {
                        VarType elem_var_type = (VarType)chunk->code[current_offset++];
                        printf("%s ", varTypeToString(elem_var_type));
                        if (current_offset < chunk->count) {
                            uint8_t elem_name_idx = chunk->code[current_offset++];
                            if (elem_name_idx < chunk->constants_count && chunk->constants[elem_name_idx].type == TYPE_STRING) {
                                printf("('%s')", chunk->constants[elem_name_idx].s_val);
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
                        printf("('%s')", chunk->constants[type_name_idx].s_val);
                    }
                    if (declaredType == TYPE_STRING && current_offset + 1 < chunk->count) {
                        uint16_t len_idx =
                            (uint16_t)((chunk->code[current_offset] << 8) | chunk->code[current_offset + 1]);
                        current_offset += 2;
                        if (len_idx < chunk->constants_count && chunk->constants[len_idx].type == TYPE_INTEGER) {
                            printf(" len=%lld", chunk->constants[len_idx].i_val);
                        }
                    }
                }
            }
            printf("\n");
            return current_offset;
        }
        case OP_GET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("%-16s %4d '%s'\n", "OP_GET_GLOBAL", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 2;
        }
        case OP_SET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("%-16s %4d '%s'\n", "OP_SET_GLOBAL", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 2;
        }
        case OP_GET_GLOBAL_ADDRESS: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("%-16s %4d '%s'\n", "OP_GET_GLOBAL_ADDRESS", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 2;
        }
        case OP_GET_GLOBAL16: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            printf("%-16s %4d '%s'\n", "OP_GET_GLOBAL16", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 3;
        }
        case OP_SET_GLOBAL16: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            printf("%-16s %4d '%s'\n", "OP_SET_GLOBAL16", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 3;
        }
        case OP_GET_GLOBAL_ADDRESS16: {
            uint16_t name_index = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
            printf("%-16s %4d '%s'\n", "OP_GET_GLOBAL_ADDRESS16", name_index, AS_STRING(chunk->constants[name_index]));
            return offset + 3;
        }
        case OP_GET_LOCAL: {
            uint8_t slot = chunk->code[offset + 1];
            printf("%-16s %4d (slot)\n", "OP_GET_LOCAL", slot);
            return offset + 2;
        }
        case OP_SET_LOCAL: {
            uint8_t slot = chunk->code[offset + 1];
            printf("%-16s %4d (slot)\n", "OP_SET_LOCAL", slot);
            return offset + 2;
        }
        case OP_INIT_LOCAL_ARRAY: {
            uint8_t slot = chunk->code[offset + 1];
            uint8_t dim_count = chunk->code[offset + 2];
            printf("%-16s Slot:%d Dims:%d\n", "OP_INIT_LOCAL_ARRAY", slot, dim_count);
            return offset + 5 + dim_count * 4;
        }
        case OP_INIT_LOCAL_FILE: {
            uint8_t slot = chunk->code[offset + 1];
            printf("%-16s %4d (slot)\n", "OP_INIT_LOCAL_FILE", slot);
            return offset + 2;
        }
        case OP_INIT_LOCAL_POINTER: {
            uint8_t slot = chunk->code[offset + 1];
            uint16_t name_idx = (uint16_t)(chunk->code[offset + 2] << 8) | chunk->code[offset + 3];
            printf("%-16s %4d (slot) %4d", "OP_INIT_LOCAL_POINTER", slot, name_idx);
            if (name_idx < chunk->constants_count &&
                chunk->constants[name_idx].type == TYPE_STRING) {
                printf(" '%s'", chunk->constants[name_idx].s_val);
            }
            printf("\n");
            return offset + 4;
        }
        case OP_GET_LOCAL_ADDRESS: {
            uint8_t slot = chunk->code[offset + 1];
            printf("%-16s %4d (slot)\n", "OP_GET_LOCAL_ADDRESS", slot);
            return offset + 2;
        }
        case OP_GET_FIELD_ADDRESS: {
            uint8_t const_idx = chunk->code[offset + 1];
            printf("%-16s %4d ", "OP_GET_FIELD_ADDRESS", const_idx);
            if (const_idx < chunk->constants_count &&
                chunk->constants[const_idx].type == TYPE_STRING) {
                printf("'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                printf("<INVALID FIELD CONST>\n");
            }
            return offset + 2;
        }
        case OP_GET_FIELD_ADDRESS16: {
            uint16_t const_idx = (uint16_t)(chunk->code[offset + 1] << 8) |
                                 chunk->code[offset + 2];
            printf("%-16s %4d ", "OP_GET_FIELD_ADDRESS16", const_idx);
            if (const_idx < chunk->constants_count &&
                chunk->constants[const_idx].type == TYPE_STRING) {
                printf("'%s'\n", AS_STRING(chunk->constants[const_idx]));
            } else {
                printf("<INVALID FIELD CONST>\n");
            }
            return offset + 3;
        }
        case OP_GET_ELEMENT_ADDRESS: {
            uint8_t dims = chunk->code[offset + 1];
            printf("%-16s %4d (dims)\n", "OP_GET_ELEMENT_ADDRESS", dims);
            return offset + 2;
        }
        case OP_GET_CHAR_ADDRESS:
            printf("OP_GET_CHAR_ADDRESS\n"); // <-- ADDED MISSING CASE
            return offset + 1;
        case OP_SET_INDIRECT:
            printf("OP_SET_INDIRECT\n");
            return offset + 1;
        case OP_GET_INDIRECT:
            printf("OP_GET_INDIRECT\n");
            return offset + 1;
        case OP_IN:
            printf("OP_IN\n");
            return offset + 1;
        case OP_GET_CHAR_FROM_STRING:
            printf("OP_GET_CHAR_FROM_STRING\n");
            return offset + 1;

        case OP_CALL_BUILTIN: {
            uint8_t name_index = chunk->code[offset + 1];
            uint8_t arg_count = chunk->code[offset + 2];
            printf("%-16s %3d '%s' (%d args)\n", "OP_CALL_BUILTIN", name_index, AS_STRING(chunk->constants[name_index]), arg_count);
            return offset + 3;
        }
        
        // These are not currently used in your compiler but are in the enum
        case OP_CALL_BUILTIN_PROC:
             printf("%-16s (not fully impl.)\n", "OP_CALL_BUILTIN_PROC");
             return offset + 3;
        case OP_CALL_USER_PROC:
             printf("%-16s (not fully impl.)\n", "OP_CALL_USER_PROC");
             return offset + 3;

        case OP_WRITE_LN: {
            uint8_t arg_count = chunk->code[offset + 1];
            printf("%-16s %4d (args)\n", "OP_WRITE_LN", arg_count);
            return offset + 2;
        }
        case OP_WRITE: {
            uint8_t arg_count = chunk->code[offset + 1];
            printf("%-16s %4d (args)\n", "OP_WRITE", arg_count);
            return offset + 2;
        }
        case OP_CALL_HOST: {
            uint8_t host_fn_id_val = chunk->code[offset + 1];
            printf("%-16s %4d (ID: %d)\n", "OP_CALL_HOST", host_fn_id_val, host_fn_id_val);
            return offset + 2;
        }
        case OP_POP:
            printf("OP_POP\n");
            return offset + 1;
        case OP_CALL: {
            uint8_t name_index = chunk->code[offset + 1];
            uint16_t address = (uint16_t)((chunk->code[offset + 2] << 8) | chunk->code[offset + 3]);
            uint8_t declared_arity = chunk->code[offset + 4];
            const char* targetProcName = AS_STRING(chunk->constants[name_index]);
            printf("%-16s %04X (%s) (%d args)\n", "OP_CALL", address, targetProcName, declared_arity);
            return offset + 5;
        }
        case OP_HALT:
            printf("OP_HALT\n");
            return offset + 1;
        case OP_FORMAT_VALUE: {
            uint8_t width = chunk->code[offset+1];
            uint8_t precision = chunk->code[offset+2];
            printf("%-16s width:%d prec:%d\n", "OP_FORMAT_VALUE", width, (int8_t)precision);
            return offset + 3;
        }
        // NOTE: There is no OP_BREAK in your bytecode.h enum, so it cannot be disassembled.
        // The AST_BREAK node is handled by the compiler generating jump instructions.

        default:
            printf("Unknown opcode %02X\n", instruction);
            return offset + 1;
    }
}

void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name, HashTable* procedureTable) {
    printf("== Disassembly: %s ==\n", name);
    printf("Offset Line Opcode           Operand  Value / Target (Args)\n");
    printf("------ ---- ---------------- -------- --------------------------\n");

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
            printf("\n");
            printf("--- %s %s (at %04X) ---\n", routineTypeStr, procNameAtOffset, offset);
        }
        // Call the public, enhanced disassembleInstruction
        offset = disassembleInstruction(chunk, offset, procedureTable);
    }
    printf("== End Disassembly: %s ==\n\n", name);

    if (chunk->constants_count > 0) {
        printf("Constants (%d):\n", chunk->constants_count);
        for (int i = 0; i < chunk->constants_count; i++) {
            printf("  %04d: ", i);
            Value constantValue = chunk->constants[i];
            switch(constantValue.type) {
                case TYPE_INTEGER: printf("INT   %lld\n", constantValue.i_val); break;
                case TYPE_REAL:    printf("REAL  %f\n", constantValue.r_val); break;
                case TYPE_STRING:  printf("STR   \"%s\"\n", constantValue.s_val ? constantValue.s_val : "NULL_STR"); break;
                case TYPE_CHAR:    printf("CHAR  '%c'\n", constantValue.c_val); break;
                case TYPE_BOOLEAN: printf("BOOL  %s\n", constantValue.i_val ? "true" : "false"); break;
                case TYPE_NIL:     printf("NIL\n"); break;
                default: printf("Value type %s\n", varTypeToString(constantValue.type)); break;
            }
        }
        printf("\n");
    }
}
