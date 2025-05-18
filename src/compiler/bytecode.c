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
