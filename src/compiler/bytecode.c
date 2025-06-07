// src/compiler/bytecode.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memcpy

#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For freeValue, varTypeToString
#include "symbol/symbol.h" // For Symbol struct, HashTable, lookupSymbolIn
#include "vm/vm.h"         // For HostFunctionID type (used in OP_CALL_HOST cast)

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
    chunk->lines = 0;
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

/*
int addConstantToChunk(BytecodeChunk* chunk, Value value) { // From all.txt
    if (chunk->constants_capacity < chunk->constants_count + 1) {
        int oldCapacity = chunk->constants_capacity;
        chunk->constants_capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->constants = (Value*)reallocate(chunk->constants,
                                             sizeof(Value) * oldCapacity,
                                             sizeof(Value) * chunk->constants_capacity);
    }
    chunk->constants[chunk->constants_count] = value;
    return chunk->constants_count++;
}
 */

int addConstantToChunk(BytecodeChunk* chunk, Value value) {
    fprintf(stderr, "[DEBUG addConstantToChunk] ENTER. Adding value type %s. chunk ptr: %p\n", varTypeToString(value.type), (void*)chunk);
    if (value.type == TYPE_STRING) {
        fprintf(stderr, "[DEBUG addConstantToChunk] String value to add: '%s'\n", value.s_val ? value.s_val : "NULL_SVAL");
    }
    fflush(stderr);

    // Perform reallocation if needed (keep this block as is)
    if (chunk->constants_capacity < chunk->constants_count + 1) {
        fprintf(stderr, "[DEBUG addConstantToChunk] Reallocating constants. Old cap: %d, Old count: %d\n", chunk->constants_capacity, chunk->constants_count);
        fflush(stderr);
        int oldCapacity = chunk->constants_capacity;
        chunk->constants_capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->constants = (Value*)reallocate(chunk->constants,
                                             sizeof(Value) * oldCapacity,
                                             sizeof(Value) * chunk->constants_capacity);
        fprintf(stderr, "[DEBUG addConstantToChunk] Reallocated. New cap: %d. chunk->constants ptr: %p\n", chunk->constants_capacity, (void*)chunk->constants);
        fflush(stderr);
    }

    fprintf(stderr, "[DEBUG addConstantToChunk] BEFORE assignment: chunk->constants_count = %d. Dest addr: %p. Value.s_val addr: %p\n",
            chunk->constants_count,
            (void*)&(chunk->constants[chunk->constants_count]),
            (void*)value.s_val);
    fflush(stderr);

    // The problematic line:
    chunk->constants[chunk->constants_count] = value; // bytecode.c:67

    fprintf(stderr, "[DEBUG addConstantToChunk] AFTER assignment. Copied value type %s to index %d.\n",
            varTypeToString(chunk->constants[chunk->constants_count].type), chunk->constants_count);
    if (chunk->constants[chunk->constants_count].type == TYPE_STRING) {
         fprintf(stderr, "[DEBUG addConstantToChunk] Copied string: '%s'\n",
                 chunk->constants[chunk->constants_count].s_val ? chunk->constants[chunk->constants_count].s_val : "NULL_SVAL_COPIED");
    }
    fflush(stderr);

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

// --- Bytecode Disassembler ---

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
            printf("%-16s %4d '", "OP_CONSTANT", constant_index);
            Value constantValue = chunk->constants[constant_index];
            switch(constantValue.type) {
                case TYPE_INTEGER: printf("%lld", constantValue.i_val); break;
                case TYPE_REAL:    printf("%f", constantValue.r_val); break;
                case TYPE_STRING:  printf("%s", constantValue.s_val ? constantValue.s_val : "NULL_STR"); break;
                case TYPE_CHAR:    printf("%c", constantValue.c_val); break;
                case TYPE_BOOLEAN: printf("%s", constantValue.i_val ? "true" : "false"); break;
                case TYPE_NIL:     printf("nil"); break; // Assuming you might add TYPE_NIL to Value printing
                default: printf("Value type %s", varTypeToString(constantValue.type)); break;
            }
            printf("'\n");
            return offset + 2;
        }
        case OP_ADD:      printf("OP_ADD\n"); return offset + 1;
        case OP_SUBTRACT: printf("OP_SUBTRACT\n"); return offset + 1;
        case OP_MULTIPLY: printf("OP_MULTIPLY\n"); return offset + 1;
        case OP_DIVIDE:   printf("OP_DIVIDE\n"); return offset + 1;
        case OP_NEGATE:   printf("OP_NEGATE\n"); return offset + 1;
        case OP_NOT:      printf("OP_NOT\n"); return offset + 1;

        case OP_EQUAL:         printf("OP_EQUAL\n"); return offset + 1;
        case OP_NOT_EQUAL:     printf("OP_NOT_EQUAL\n"); return offset + 1;
        case OP_GREATER:       printf("OP_GREATER\n"); return offset + 1;
        case OP_GREATER_EQUAL: printf("OP_GREATER_EQUAL\n"); return offset + 1;
        case OP_LESS:          printf("OP_LESS\n"); return offset + 1;
        case OP_LESS_EQUAL:    printf("OP_LESS_EQUAL\n"); return offset + 1;
        case OP_INT_DIV:  printf("OP_INT_DIV\n"); return offset + 1;
        case OP_AND:      printf("OP_AND\n"); return offset + 1;
        case OP_OR:       printf("OP_OR\n"); return offset + 1;

        case OP_JUMP_IF_FALSE: {
            uint16_t jump_operand = (uint16_t)(chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf("%-16s %4d (to %04X)\n", "OP_JUMP_IF_FALSE", jump_operand, offset + 3 + jump_operand);
            return offset + 3;
        }
        case OP_JUMP: {
            uint16_t jump_operand = (uint16_t)(chunk->code[offset + 1] << 8) | chunk->code[offset + 2];
            printf("%-16s %4d (to %04X)\n", "OP_JUMP", jump_operand, offset + 3 + jump_operand);
            return offset + 3;
        }
        case OP_DEFINE_GLOBAL: {
            // --- MODIFICATION START ---
            uint8_t name_idx = chunk->code[offset + 1];
            uint8_t type_name_idx = chunk->code[offset + 2]; // New operand
            uint8_t var_type_enum = chunk->code[offset + 3]; // New operand

            printf("OP_DEFINE_GLOBAL NameIdx:%-3d ", name_idx);
            if (name_idx < chunk->constants_count && chunk->constants[name_idx].type == TYPE_STRING) {
                printf("'%s' ", chunk->constants[name_idx].s_val);
            } else {
                printf("INVALID_NAME_IDX ");
            }
            printf("TypeNameIdx:%-3d ", type_name_idx);
            if (type_name_idx > 0 && type_name_idx < chunk->constants_count && chunk->constants[type_name_idx].type == TYPE_STRING) {
                 printf("('%s') ", chunk->constants[type_name_idx].s_val);
            } else if (type_name_idx == 0) {
                 printf("(simple/anon) ");
            } else {
                 printf("INVALID_TYPE_NAME_IDX ");
            }
            printf("VarType:%s (%d)\n", varTypeToString((VarType)var_type_enum), var_type_enum);
            return offset + 4; // Opcode + 3 bytes for operands
            // --- MODIFICATION END ---
        }
        case OP_GET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("%-16s %4d '", "OP_GET_GLOBAL", name_index);
            if (name_index < chunk->constants_count && chunk->constants[name_index].type == TYPE_STRING) {
                printf("%s", chunk->constants[name_index].s_val);
            } else {
                printf("INVALID_NAME_INDEX");
            }
            printf("'\n");
            return offset + 2;
        }
        case OP_SET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("%-16s %4d '", "OP_SET_GLOBAL", name_index);
            if (name_index < chunk->constants_count && chunk->constants[name_index].type == TYPE_STRING) {
                printf("%s", chunk->constants[name_index].s_val);
            } else {
                printf("INVALID_NAME_INDEX");
            }
            printf("'\n");
            return offset + 2;
        }
        case OP_CALL_BUILTIN: {
            uint8_t name_index = chunk->code[offset + 1];
            uint8_t arg_count = chunk->code[offset + 2];
            printf("%-16s %3d '%s' (%d args)\n", "OP_CALL_BUILTIN",
                   name_index,
                   (name_index < chunk->constants_count && chunk->constants[name_index].type == TYPE_STRING)
                       ? chunk->constants[name_index].s_val
                       : "INVALID_IDX",
                   arg_count);
            return offset + 3;
        }
        case OP_WRITE_LN: {
            uint8_t arg_count = chunk->code[offset + 1];
            printf("%-16s %4d (args)\n", "OP_WRITE_LN", arg_count);
            return offset + 2;
        }
        case OP_CALL_HOST: {
            if (offset + 1 >= chunk->count) {
                 printf("OP_CALL_HOST (operand out of bounds)\n");
                 return offset + 1;
            }
            uint8_t host_fn_id_val = chunk->code[offset + 1];
            // Since hostFunctionIDToString might not exist yet in your utils.c,
            // we'll print the numeric ID. You can enhance this later.
            // const char* host_name = hostFunctionIDToString((HostFunctionID)host_fn_id_val); // If you implement it
            // For now:
            printf("%-16s %4d (ID: %d)\n", "OP_CALL_HOST", host_fn_id_val, host_fn_id_val);
            return offset + 2;
        }
        case OP_CALL: {
                   // Operands: 2-byte address, 1-byte arity
                   if (offset + 3 >= chunk->count) { // Check for 3 operand bytes
                       printf("OP_CALL (operands out of bounds)\n");
                       return offset + 1; // Advance by 1 for the opcode itself
                   }
                   uint16_t address = (uint16_t)((chunk->code[offset + 1] << 8) | chunk->code[offset + 2]);
                   uint8_t declared_arity = chunk->code[offset + 3];
                   const char* targetProcName = findProcedureNameByAddress(procedureTable, address);

                   printf("%-16s %04X", "OP_CALL", address);
                   if (targetProcName) {
                       printf(" (%s)", targetProcName);
                   } else {
                       printf(" (Unknown@%04X)", address);
                   }
                   printf(" (%d args)\n", declared_arity);
                   return offset + 4; // Opcode (1) + Address (2) + Arity (1)
               }
        case OP_POP:
            printf("OP_POP\n");
            return offset + 1;
        case OP_HALT:
            printf("OP_HALT\n");
            return offset + 1;
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
