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
#include <stdbool.h>

// --- VM Configuration ---
#define VM_STACK_MAX 8192       // Maximum number of Values on the operand stack
#define VM_GLOBALS_MAX 4096     // Maximum number of global variables (for simple array storage)

#define MAX_HOST_FUNCTIONS 4096

#define VM_CALL_STACK_MAX 4096

// Forward declaration
struct VM_s;

// Host function pointer type
typedef Value (*HostFn)(struct VM_s* vm);

// Enum to identify specific host functions
typedef enum {
    HOST_FN_QUIT_REQUESTED,
    // ... add other host function IDs here ...
    HOST_FN_COUNT
} HostFunctionID;

// --- Interpret Result ---
typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR, // Should be caught before VM runs
    INTERPRET_RUNTIME_ERROR
} InterpretResult;

typedef struct {
    uint8_t* return_address;    // IP in the caller to return to
    Value* slots;               // Pointer to this frame's window on the VM value stack
    Symbol* function_symbol;    // Pointer to the Symbol of the function being called (for arity/locals_count)
                                // Note: Storing Symbol* is one way; alternatively, OP_CALL could carry locals_count,
                                // or OP_RETURN could be generic if stack is always reset to frame->slots.
    uint8_t locals_count;       // Number of local variables (excluding params)
    uint8_t upvalue_count;
    Value** upvalues;
} CallFrame;

// --- Virtual Machine Structure ---
typedef struct VM_s { 
    BytecodeChunk* chunk;     // The chunk of bytecode to execute
    uint8_t* ip;              // Instruction Pointer: points to the *next* byte to be read

    Value stack[VM_STACK_MAX]; // The operand stack
    Value* stackTop;          // Pointer to the element just above the top of the stack
                              // (i.e., where the next pushed item will go)

    HashTable* vmGlobalSymbols; // VM's own symbol table for runtime global variable storage
    HashTable* procedureTable; // store procedure table for disassembly
    
    HostFn host_functions[MAX_HOST_FUNCTIONS];

    CallFrame frames[VM_CALL_STACK_MAX];
    int frameCount;

    bool exit_requested;      // Indicates a builtin requested early exit from the current frame

} VM;

// --- Public VM Interface ---
void initVM(VM* vm);    // Initialize a new VM instance
void freeVM(VM* vm);    // Free resources associated with a VM instance

// Main function to interpret a chunk of bytecode
// Takes a BytecodeChunk that was successfully compiled.
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* procedures);
void vm_nullifyAliases(VM* vm, uintptr_t disposedAddrValue);

void runtimeError(VM* vm, const char* format, ...);
void vm_dump_stack_info(VM* vm);
void vm_dump_stack_info_detailed(VM* vm, const char* context_message);

#endif // PSCAL_VM_H
