//
//  vm.h
//  Pscal
//
//  Created by Michael Miller on 5/19/25.
//
#ifndef PSCAL_VM_H
#define PSCAL_VM_H

#include "compiler/bytecode.h" // For BytecodeChunk and Value (via its include of types.h)
#include "core/types.h"        // For Value explicitly, though bytecode.h should bring it in
#include "symbol/symbol.h"     // For HashTable, if VM manages globals using it directly

// --- VM Configuration ---
#define VM_STACK_MAX 256       // Maximum number of Values on the operand stack
#define VM_GLOBALS_MAX 256     // Maximum number of global variables (for simple array storage)
// #define VM_CALL_STACK_MAX 64 // For function calls later

// --- Interpret Result ---
typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR, // Should be caught before VM runs
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

// --- Virtual Machine Structure ---
typedef struct {
    BytecodeChunk* chunk;     // The chunk of bytecode to execute
    uint8_t* ip;              // Instruction Pointer: points to the *next* byte to be read

    Value stack[VM_STACK_MAX]; // The operand stack
    Value* stackTop;          // Pointer to the element just above the top of the stack
                              // (i.e., where the next pushed item will go)

    // Global variable storage:
    // Option 1: Simple array if globals are mapped to indices 0..N by compiler
    // Value globals[VM_GLOBALS_MAX];

    // Option 2: Use your existing HashTable for globals (more flexible, handles names)
    HashTable* vmGlobalSymbols; // VM's own symbol table for runtime global variable storage

    // Future: Call frames for function calls
    // CallFrame frames[VM_CALL_STACK_MAX];
    // int frameCount;

} VM;

// --- Public VM Interface ---
void initVM(VM* vm);    // Initialize a new VM instance
void freeVM(VM* vm);    // Free resources associated with a VM instance

// Main function to interpret a chunk of bytecode
// Takes a BytecodeChunk that was successfully compiled.
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk);

// Stack operations (can be static helpers in vm.c or part of public API if needed elsewhere)
// void push(VM* vm, Value value);
// Value pop(VM* vm);
// Value peek(VM* vm, int distance); // Peek 'distance' items down the stack (0 is top)

#endif // PSCAL_VM_H
