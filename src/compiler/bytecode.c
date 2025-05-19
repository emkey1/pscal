//
//  bytecode.c
//  Pscal
//
//  Created by Michael Miller on 5/18/25.
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // For memcpy

#include "bytecode.h" // Our new header
#include "core/types.h"   // For Value struct
#include "core/utils.h"   // For freeValue, etc. (might be needed by Value)
// #include "memory.h" // If you had a separate memory manager for realloc etc.

void initBytecodeChunk(BytecodeChunk* chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL; // Initialize lines array

    chunk->constants_count = 0;
    chunk->constants_capacity = 0;
    chunk->constants = NULL;
}

void freeBytecodeChunk(BytecodeChunk* chunk) {
    // Free the bytecode array and line numbers array
    free(chunk->code);
    free(chunk->lines);

    // Free the constants array (needs to free each Value within it too)
    for (int i = 0; i < chunk->constants_count; i++) {
        freeValue(&chunk->constants[i]); // Assuming freeValue handles your Value struct properly
    }
    free(chunk->constants);

    // Re-initialize to a clean state (optional, but good practice)
    initBytecodeChunk(chunk);
}

// Helper to grow dynamic arrays (could be generic)
static void* reallocate(void* pointer, size_t oldSize, size_t newSize) {
    if (newSize == 0) {
        free(pointer);
        return NULL;
    }
    void* result = realloc(pointer, newSize);
    if (result == NULL) {
        fprintf(stderr, "Memory allocation error (realloc failed)\n");
        exit(1); // Or a more graceful error handling
    }
    return result;
}

void writeBytecodeChunk(BytecodeChunk* chunk, uint8_t byte, int line) {
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
    chunk->lines[chunk->count] = line; // Store corresponding source line
    chunk->count++;
}

int addConstantToChunk(BytecodeChunk* chunk, Value value) {
    // TODO: Check if constant already exists to avoid duplicates (optional optimization)
    // For now, always add.

    if (chunk->constants_capacity < chunk->constants_count + 1) {
        int oldCapacity = chunk->constants_capacity;
        chunk->constants_capacity = oldCapacity < 8 ? 8 : oldCapacity * 2;
        chunk->constants = (Value*)reallocate(chunk->constants,
                                       sizeof(Value) * oldCapacity,
                                       sizeof(Value) * chunk->constants_capacity);
    }

    // We assume 'value' is a temporary that can be moved or copied.
    // The Value struct itself is copied here. If 'value' contains pointers
    // to heap data (like strings), makeCopyOfValue should be used if the original
    // 'value' is going out of scope and its contents would be freed.
    // For now, direct copy. The compiler will create Value structs for constants.
    chunk->constants[chunk->constants_count] = value; // Direct struct copy
    // If 'value' came from an AST node's token that will be freed, ensure string data is duplicated if necessary
    // e.g., if value is a string literal from AST, its s_val might point to token->value.
    // The compiler should ensure constants are self-contained Values.
    // For example, if adding a string constant:
    // Value str_const_val = makeString(ast_node->token->value); // makeString strdups
    // addConstantToChunk(chunk, str_const_val); // Now str_const_val can be freed by caller if it was temporary
    // The copy in chunk->constants now owns its (duplicated) string.

    return chunk->constants_count++;
}

// --- Bytecode Disassembler ---

// Helper to print a single instruction and its operands
static int disassembleInstruction(BytecodeChunk* chunk, int offset) {
    printf("%04d ", offset); // Print the offset (address) of the instruction
    if (offset > 0 && chunk->lines[offset] == chunk->lines[offset - 1]) {
        printf("   | "); // Same line as previous instruction
    } else {
        printf("%4d ", chunk->lines[offset]); // Print source line number
    }

    uint8_t instruction = chunk->code[offset];
    switch (instruction) {
        case OP_RETURN:
            printf("OP_RETURN\n");
            return offset + 1;
        case OP_CONSTANT: {
            uint8_t constant_index = chunk->code[offset + 1];
            printf("OP_CONSTANT     %4d '", constant_index);
            // Print the constant value (requires access to Value printing logic)
            Value constantValue = chunk->constants[constant_index];
            switch(constantValue.type) {
                case TYPE_INTEGER: printf("%lld", constantValue.i_val); break;
                case TYPE_REAL:    printf("%f", constantValue.r_val); break;
                case TYPE_STRING:  printf("%s", constantValue.s_val ? constantValue.s_val : "NULL"); break;
                case TYPE_CHAR:    printf("%c", constantValue.c_val); break;
                case TYPE_BOOLEAN: printf("%s", constantValue.i_val ? "true" : "false"); break;
                default: printf("Value type %s", varTypeToString(constantValue.type)); break;
            }
            printf("'\n");
            return offset + 2; // Opcode + 1-byte operand
        }
        case OP_ADD:
            printf("OP_ADD\n");
            return offset + 1;
        case OP_SUBTRACT:
            printf("OP_SUBTRACT\n");
            return offset + 1;
        case OP_MULTIPLY:
            printf("OP_MULTIPLY\n");
            return offset + 1;
        case OP_DIVIDE:
            printf("OP_DIVIDE\n");
            return offset + 1;
        case OP_NEGATE:
            printf("OP_NEGATE\n");
            return offset + 1;
        case OP_NOT:
            printf("OP_NOT\n");
            return offset + 1;
        case OP_DEFINE_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("OP_DEFINE_GLOBAL %4d '", name_index);
            if (name_index < chunk->constants_count && chunk->constants[name_index].type == TYPE_STRING) {
                printf("%s", chunk->constants[name_index].s_val);
            } else {
                printf("INVALID_NAME_INDEX");
            }
            printf("'\n");
            return offset + 2; // Opcode + 1-byte operand
        }
        case OP_GET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("OP_GET_GLOBAL   %4d '", name_index);
             if (name_index < chunk->constants_count && chunk->constants[name_index].type == TYPE_STRING) {
                printf("%s", chunk->constants[name_index].s_val);
            } else {
                printf("INVALID_NAME_INDEX");
            }
            printf("'\n");
            return offset + 2; // Opcode + 1-byte operand
        }
        case OP_SET_GLOBAL: {
            uint8_t name_index = chunk->code[offset + 1];
            printf("OP_SET_GLOBAL   %4d '", name_index);
            if (name_index < chunk->constants_count && chunk->constants[name_index].type == TYPE_STRING) {
                printf("%s", chunk->constants[name_index].s_val);
            } else {
                printf("INVALID_NAME_INDEX");
            }
            printf("'\n");
            return offset + 2; // Opcode + 1-byte operand
        }
        case OP_WRITE_LN: {
            uint8_t arg_count = chunk->code[offset + 1];
            printf("OP_WRITE_LN     %4d (arg_count)\n", arg_count);
            return offset + 2; // Opcode + 1-byte operand
        }
        case OP_POP:
            printf("OP_POP\n");
            return offset + 1;
        case OP_HALT:
            printf("OP_HALT\n");
            return offset + 1;
        // Add OP_CALL_BUILTIN case when you implement it fully
        // case OP_CALL_BUILTIN: {
        //     uint8_t builtin_index = chunk->code[offset + 1];
        //     uint8_t num_args = chunk->code[offset + 2];
        //     printf("OP_CALL_BUILTIN %4d (idx) %3d (args)\n", builtin_index, num_args);
        //     return offset + 3; // Opcode + 2 operands
        // }
        default:
            printf("Unknown opcode %d\n", instruction);
            return offset + 1;
    }
}

void disassembleBytecodeChunk(BytecodeChunk* chunk, const char* name) {
    printf("== Disassembly: %s ==\n", name);
    printf("Offset Line Opcode    Operand  Value\n");
    printf("------ ---- --------- -------- ----------\n");
    for (int offset = 0; offset < chunk->count; ) {
        offset = disassembleInstruction(chunk, offset);
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
                case TYPE_STRING:  printf("STR   \"%s\"\n", constantValue.s_val ? constantValue.s_val : "NULL"); break;
                case TYPE_CHAR:    printf("CHAR  '%c'\n", constantValue.c_val); break;
                case TYPE_BOOLEAN: printf("BOOL  %s\n", constantValue.i_val ? "true" : "false"); break;
                default: printf("Value type %s\n", varTypeToString(constantValue.type)); break;
            }
        }
        printf("\n");
    }
}
