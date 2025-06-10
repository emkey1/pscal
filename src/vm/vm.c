// src/vm/vm.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h> // For bool, true, false

#include "vm/vm.h"
#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For runtimeError, printValueToStream, makeNil, freeValue, Type helper macros
#include "symbol/symbol.h" // For HashTable, createHashTable, hashTableLookup, hashTableInsert
#include "globals.h"
#include "backend_ast/interpreter.h" // For makeCopyOfValue (If still needed, consider moving to utils.c)
#include "backend_ast/audio.h"
#include "frontend/parser.h"
#include "frontend/ast.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "backend_ast/builtin.h"
#include "backend_ast/audio.h"  

// --- VM Helper Functions ---

static void resetStack(VM* vm) {
    vm->stackTop = vm->stack;
}

// Scans all global symbols and the entire VM value stack to find and nullify
// any pointers that are aliases of a memory address that is being disposed.
void vm_nullifyAliases(VM* vm, uintptr_t disposedAddrValue) {
    // 1. Scan global symbols using the existing hash table helper
    if (vm->vmGlobalSymbols) {
        nullifyPointerAliasesByAddrValue(vm->vmGlobalSymbols, disposedAddrValue);
    }

    // 2. Scan the entire VM value stack for local variables and parameters
    //    across all active call frames.
    for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
        if (slot->type == TYPE_POINTER && (uintptr_t)slot->ptr_val == disposedAddrValue) {
            slot->ptr_val = NULL; // This is an alias, set it to nil.
        }
    }
}

// runtimeError - Assuming your existing one is fine.
void runtimeError(VM* vm, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);

    if (vm && vm->chunk && vm->ip && vm->chunk->code && vm->chunk->lines) {
        if (vm->ip > vm->chunk->code) { // Ensure ip has moved past the start
            size_t instruction_offset = (vm->ip - vm->chunk->code) - 1;
            if (instruction_offset < (size_t)vm->chunk->count) {
                int line = vm->chunk->lines[instruction_offset];
                fprintf(stderr, "[line %d] in script (approx.)\n", line);
            }
        } else if (vm->chunk->count > 0) { // If error on the very first byte
             int line = vm->chunk->lines[0];
             fprintf(stderr, "[line %d] in script (approx.)\n", line);
        }
    }
    resetStack(vm);
}

static void push(VM* vm, Value value) { // Using your original name 'push'
    if (vm->stackTop - vm->stack >= VM_STACK_MAX) {
        runtimeError(vm, "VM Error: Stack overflow.");
        return;
    }
    *vm->stackTop = value;
    vm->stackTop++;
}

static Value pop(VM* vm) { // Using your original name 'pop'
    if (vm->stackTop == vm->stack) {
        runtimeError(vm, "VM Error: Stack underflow (pop from empty stack).");
        return makeNil();
    }
    vm->stackTop--;
    return *vm->stackTop;
}

static Value peek(VM* vm, int distance) { // Using your original name 'peek'
    if (vm->stackTop - vm->stack < distance + 1) {
        runtimeError(vm, "VM Error: Stack underflow (peek too deep).");
        return makeNil();
    }
    return vm->stackTop[-(distance + 1)];
}

// --- Host Function C Implementations ---
static Value vm_host_quit_requested(VM* vm) {
    // break_requested is extern int from globals.h, defined in globals.c
    // makeBoolean is from core/utils.h
    return makeBoolean(break_requested);
}

// --- Host Function Registration ---
bool register_host_function(VM* vm, HostFunctionID id, HostFn fn) {
    if (!vm) return false;
    if (id >= HOST_FN_COUNT || id < 0) {
        fprintf(stderr, "VM Error: HostFunctionID %d out of bounds during registration.\n", id);
        return false;
    }
    vm->host_functions[id] = fn;
    return true;
}

// --- VM Initialization and Cleanup ---
void initVM(VM* vm) { // As in all.txt, with frameCount
    if (!vm) return;
    resetStack(vm);
    vm->chunk = NULL;
    vm->ip = NULL;
    vm->vmGlobalSymbols = NULL;              // Will be set by interpretBytecode
    vm->procedureTable = NULL;
    
    vm->frameCount = 0; // <--- INITIALIZE frameCount

    for (int i = 0; i < MAX_HOST_FUNCTIONS; i++) {
        vm->host_functions[i] = NULL;
    }
    if (!register_host_function(vm, HOST_FN_QUIT_REQUESTED, vm_host_quit_requested)) { // from all.txt
        fprintf(stderr, "Fatal VM Error: Could not register HOST_FN_QUIT_REQUESTED.\n");
        exit(EXIT_FAILURE);
    }
}

void freeVM(VM* vm) {
    if (!vm) return;
    if (vm->vmGlobalSymbols) {
        // Assuming freeHashTable also frees the Symbols and their Values correctly.
        // If not, you might need to iterate and free Values if they contain heap data
        // not managed by freeValue calls during Symbol freeing.
        freeHashTable(vm->vmGlobalSymbols); //
        vm->vmGlobalSymbols = NULL;
    }
    // No explicit freeing of vm->host_functions array itself as it's part of VM struct.
    // If HostFn structs themselves allocated memory, that would need handling.
}

// --- Bytecode Reading Macros ---
// Your existing READ_BYTE and READ_CONSTANT macros are fine as they implicitly use 'vm' from runVM scope
#define READ_BYTE() (*vm->ip++)
#define READ_CONSTANT() (vm->chunk->constants[READ_BYTE()])

// Helper function to read a 16-bit short. It will use the READ_BYTE() macro.
static inline uint16_t READ_SHORT(VM* vm_param) { // Pass vm explicitly here
    uint8_t msb = (*vm_param->ip++); // Explicitly use vm_param
    uint8_t lsb = (*vm_param->ip++); // Explicitly use vm_param
    return (uint16_t)(msb << 8) | lsb;
}

#define READ_HOST_ID() ((HostFunctionID)READ_BYTE())
// This assumes that your HostFunctionID values will fit within a uint8_t.
// If you anticipate having more than 255 host functions, you'd use READ_SHORT(vm)
// and the compiler would need to emit two bytes for the ID. For now, one byte is fine.
// Note: READ_BYTE() implicitly uses 'vm' because it's used inside interpretBytecode
// where 'vm' is in scope and points to the current VM instance.

// --- Symbol Management (VM specific) ---
static Symbol* createSymbolForVM(const char* name, VarType type, AST* type_def_for_value_init) {
    if (!name || name[0] == '\0') { /* ... */ return NULL; }
    Symbol *sym = (Symbol*)malloc(sizeof(Symbol));
    if (!sym) { /* ... */ return NULL; }
    sym->name = strdup(name);
    if (!sym->name) { /* ... */ free(sym); return NULL; }
    toLowerString(sym->name);
    
    sym->type = type;
    sym->type_def = type_def_for_value_init; // Store the provided type definition AST
    sym->value = (Value*)malloc(sizeof(Value));
    if (!sym->value) { /* ... */ free(sym->name); free(sym); return NULL; }

    // Call makeValueForType with the (now potentially non-NULL) type_def_for_value_init
    *(sym->value) = makeValueForType(type, type_def_for_value_init, sym);
    
    sym->is_alias = false;
    sym->is_const = false; // Constants handled at compile time won't use OP_DEFINE_GLOBAL
                           // If VM needs to know about them, another mechanism or flag is needed.
    sym->is_local_var = false;
    sym->next = NULL;
    return sym;
}

// Ensure these are defined or included (e.g., from utils.h or types.h)
// If they are macros, they will use 'value' as their argument.
#ifndef IS_BOOLEAN
    #define IS_BOOLEAN(val) ((val).type == TYPE_BOOLEAN)
    #define AS_BOOLEAN(val) ((val).i_val != 0) // Assumes 0 is false, non-0 is true for i_val storage
    #define IS_INTEGER(val) ((val).type == TYPE_INTEGER)
    #define AS_INTEGER(val) ((val).i_val)
    #define IS_REAL(val)    ((val).type == TYPE_REAL)
    #define AS_REAL(val)    ((val).r_val)
    #define IS_STRING(val)  ((val).type == TYPE_STRING)
    #define AS_STRING(val)  ((val).s_val)
    #define IS_CHAR(val)    ((val).type == TYPE_CHAR)
    #define AS_CHAR(val)    ((val).c_val)
#endif

// --- Main Interpretation Loop ---
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* procedures) {
    if (!vm || !chunk) return INTERPRET_RUNTIME_ERROR;

    vm->chunk = chunk;
    vm->ip = vm->chunk->code;
    
    vm->vmGlobalSymbols = globals;    // Store globals table (ensure this is the intended one)
    vm->procedureTable = procedures; // <--- STORED procedureTable

    #ifdef DEBUG
    if (dumpExec) { // from all.txt [cite: 1391]
        printf("\n--- VM Initial State ---\n");
        printf("IP: %p (offset 0)\n", (void*)vm->ip);
        printf("Stack top: %p (empty)\n", (void*)vm->stackTop);
        printf("Chunk code: %p, Chunk constants: %p\n", (void*)vm->chunk->code, (void*)vm->chunk->constants);
        printf("Global symbol table (for VM): %p\n", (void*)vm->vmGlobalSymbols);
        printf("Procedure table (for disassembly): %p\n", (void*)vm->procedureTable); // Debug print
        printf("------------------------\n");
    }
    #endif
    
#define READ_BYTE() (*vm->ip++)
#define READ_CONSTANT() (vm->chunk->constants[READ_BYTE()])
#define READ_HOST_ID() ((HostFunctionID)READ_BYTE())
// Corrected BINARY_OP to use the passed instruction_val
#define BINARY_OP(op_char_for_error_msg, current_instruction_code) \
    do { \
        Value b_val_popped = pop(vm); \
        Value a_val_popped = pop(vm); \
        Value result_val; \
        bool op_is_handled = false; \
        \
        if (current_instruction_code == OP_ADD) { \
            if ((IS_STRING(a_val_popped) || IS_CHAR(a_val_popped)) && \
                (IS_STRING(b_val_popped) || IS_CHAR(b_val_popped))) { \
                \
                char a_buffer[2] = {0}; \
                char b_buffer[2] = {0}; \
                const char* s_a = NULL; \
                const char* s_b = NULL; \
                \
                if (IS_STRING(a_val_popped)) { \
                    s_a = AS_STRING(a_val_popped) ? AS_STRING(a_val_popped) : ""; \
                } else { \
                    a_buffer[0] = AS_CHAR(a_val_popped); \
                    s_a = a_buffer; \
                } \
                \
                if (IS_STRING(b_val_popped)) { \
                    s_b = AS_STRING(b_val_popped) ? AS_STRING(b_val_popped) : ""; \
                } else { \
                    b_buffer[0] = AS_CHAR(b_val_popped); \
                    s_b = b_buffer; \
                } \
                \
                size_t len_a = strlen(s_a); \
                size_t len_b = strlen(s_b); \
                char* concat_buffer = (char*)malloc(len_a + len_b + 1); \
                if (!concat_buffer) { \
                    runtimeError(vm, "Runtime Error: Malloc failed for string concatenation buffer."); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    return INTERPRET_RUNTIME_ERROR; \
                } \
                strcpy(concat_buffer, s_a); \
                strcat(concat_buffer, s_b); \
                result_val = makeString(concat_buffer); \
                free(concat_buffer); \
                op_is_handled = true; \
            } \
        } \
        \
        if (!op_is_handled) { \
            if ((IS_INTEGER(a_val_popped) || IS_REAL(a_val_popped)) && \
                (IS_INTEGER(b_val_popped) || IS_REAL(b_val_popped))) { \
                \
                if (IS_REAL(a_val_popped) || IS_REAL(b_val_popped)) { \
                    double fa = IS_REAL(a_val_popped) ? AS_REAL(a_val_popped) : (double)AS_INTEGER(a_val_popped); \
                    double fb = IS_REAL(b_val_popped) ? AS_REAL(b_val_popped) : (double)AS_INTEGER(b_val_popped); \
                    if (current_instruction_code == OP_DIVIDE && fb == 0.0) { \
                        runtimeError(vm, "Runtime Error: Division by zero."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    switch(current_instruction_code) { \
                        case OP_ADD:      result_val = makeReal(fa + fb); break; \
                        case OP_SUBTRACT: result_val = makeReal(fa - fb); break; \
                        case OP_MULTIPLY: result_val = makeReal(fa * fb); break; \
                        case OP_DIVIDE:   result_val = makeReal(fa / fb); break; \
                        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for real numbers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                } else { \
                    long long ia = AS_INTEGER(a_val_popped); \
                    long long ib = AS_INTEGER(b_val_popped); \
                    if (current_instruction_code == OP_DIVIDE && ib == 0) { \
                        runtimeError(vm, "Runtime Error: Division by zero (integer)."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    switch(current_instruction_code) { \
                        case OP_ADD:      result_val = makeInt(ia + ib); break; \
                        case OP_SUBTRACT: result_val = makeInt(ia - ib); break; \
                        case OP_MULTIPLY: result_val = makeInt(ia * ib); break; \
                        case OP_DIVIDE:   result_val = makeReal((double)ia / (double)ib); break; \
                        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for integers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                } \
                op_is_handled = true; \
            } \
        } \
        \
        if (!op_is_handled) { \
            runtimeError(vm, "Runtime Error: Operands must be numbers for arithmetic operation '%s' (or strings/chars for '+'). Got %s and %s", op_char_for_error_msg, varTypeToString(a_val_popped.type), varTypeToString(b_val_popped.type)); \
            freeValue(&a_val_popped); freeValue(&b_val_popped); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        push(vm, result_val); \
        freeValue(&a_val_popped); \
        freeValue(&b_val_popped); \
    } while (false)

    uint8_t instruction_val;
    for (;;) {
        #ifdef DEBUG
        if (dumpExec) {
            fprintf(stderr,"VM Stack: ");
            for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
                fprintf(stderr,"[ ");
                printValueToStream(*slot, stderr);
                fprintf(stderr," ]");
            }
            fprintf(stderr,"\n");
            disassembleInstruction(vm->chunk, (int)(vm->ip - vm->chunk->code), vm->procedureTable);
        }
        #endif

        instruction_val = READ_BYTE();
        switch (instruction_val) {
            case OP_RETURN: {
                 if (vm->frameCount == 0) {
                     // Returning from the top-level script, program is finished.
                     #ifdef DEBUG
                     if (dumpExec) printf("--- Returning from top-level script. VM shutting down. ---\n");
                     #endif
                     if (vm->stackTop > vm->stack) {
                         pop(vm); // Pop final implicit value if any
                     }
                     return INTERPRET_OK;
                 }

                 // Pop the result that the callee placed on top of its stack.
                 Value returnValue = pop(vm);

                 // Get the frame we are returning FROM (the callee's frame).
                 CallFrame* calleeFrame = &vm->frames[vm->frameCount - 1];

                 // The instruction pointer should be restored from the callee's frame.
                 vm->ip = calleeFrame->return_address;

                 // Reset the stack top to the beginning of the callee's frame.
                 // This erases all of the callee's locals and arguments from the stack.
                 vm->stackTop = calleeFrame->slots;
                 
                 // Finally, decrement the frame count.
                 vm->frameCount--;
                 
                 // Push the return value. It now sits on top of the caller's stack, ready for use.
                 push(vm, returnValue);

                 break;
            }
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT(); // Uses the macro
                push(vm, makeCopyOfValue(&constant));
                break;
            }
            case OP_GET_GLOBAL_ADDRESS: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) { /* error */ return INTERPRET_RUNTIME_ERROR; }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", varNameVal.s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makePointer(sym->value, NULL)); // Push a pointer to the symbol's Value struct
                break;
            }
            case OP_GET_LOCAL_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                push(vm, makePointer(&frame->slots[slot], NULL)); // Push a pointer to the Value struct on the stack
                break;
            }
            // Pass instruction_val to the macro
            case OP_ADD:      BINARY_OP("+", instruction_val); break;
            case OP_SUBTRACT: BINARY_OP("-", instruction_val); break;
            case OP_MULTIPLY: BINARY_OP("*", instruction_val); break;
            case OP_DIVIDE:   BINARY_OP("/", instruction_val); break;
            
            case OP_NEGATE: {
                Value val_popped = pop(vm);
                Value result_val;
                if (IS_INTEGER(val_popped)) result_val = makeInt(-AS_INTEGER(val_popped));
                else if (IS_REAL(val_popped)) result_val = makeReal(-AS_REAL(val_popped));
                else {
                    runtimeError(vm, "Runtime Error: Operand for negate must be a number.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result_val);
                freeValue(&val_popped);
                break;
            }
            case OP_NOT: {
                Value val_popped = pop(vm);
                if (!IS_BOOLEAN(val_popped) && !IS_INTEGER(val_popped)) {
                    runtimeError(vm, "Runtime Error: Operand for NOT must be boolean or integer.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                bool bool_val = IS_BOOLEAN(val_popped) ? AS_BOOLEAN(val_popped) : (AS_INTEGER(val_popped) != 0);
                push(vm, makeBoolean(!bool_val));
                freeValue(&val_popped);
                break;
            }
            case OP_SWAP: {
                if (vm->stackTop - vm->stack < 2) {
                    runtimeError(vm, "VM Error: Not enough values on stack to swap.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value a = pop(vm);
                Value b = pop(vm);
                push(vm, a);
                push(vm, b);
                break;
            }
            case OP_AND:
            case OP_OR: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;

                // Pascal's AND/OR can be logical for Booleans or bitwise for Integers
                if (IS_BOOLEAN(a_val) && IS_BOOLEAN(b_val)) {
                    bool ba = AS_BOOLEAN(a_val);
                    bool bb = AS_BOOLEAN(b_val);
                    if (instruction_val == OP_AND) {
                        result_val = makeBoolean(ba && bb);
                    } else { // OP_OR
                        result_val = makeBoolean(ba || bb);
                    }
                } else if (IS_INTEGER(a_val) && IS_INTEGER(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (instruction_val == OP_AND) {
                        result_val = makeInt(ia & ib); // Bitwise AND
                    } else { // OP_OR
                        result_val = makeInt(ia | ib); // Bitwise OR
                    }
                } else {
                    runtimeError(vm, "Runtime Error: Operands for AND/OR must be both Boolean or both Integer. Got %s and %s.",
                                 varTypeToString(a_val.type), varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result_val);
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case OP_INT_DIV: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (IS_INTEGER(a_val) && IS_INTEGER(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Integer division by zero.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeInt(ia / ib)); // C integer division truncates towards zero
                } else {
                    runtimeError(vm, "Runtime Error: Operands for 'div' must be integers. Got %s and %s.",
                                 varTypeToString(a_val.type), varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case OP_MOD: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (IS_INTEGER(a_val) && IS_INTEGER(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Modulo by zero.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeInt(ia % ib));
                } else {
                    runtimeError(vm, "Runtime Error: Operands for 'mod' must be integers. Got %s and %s.",
                                 varTypeToString(a_val.type), varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case OP_SHL:
            case OP_SHR: {
                Value b_val = pop(vm); // Shift amount
                Value a_val = pop(vm); // Value to shift
                if (IS_INTEGER(a_val) && IS_INTEGER(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib < 0) {
                        runtimeError(vm, "Runtime Error: Shift amount cannot be negative.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (instruction_val == OP_SHL) {
                        push(vm, makeInt(ia << ib));
                    } else { // OP_SHR
                        push(vm, makeInt(ia >> ib));
                    }
                } else {
                    runtimeError(vm, "Runtime Error: Operands for 'shl' and 'shr' must be integers. Got %s and %s.",
                                 varTypeToString(a_val.type), varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case OP_EQUAL:
            case OP_NOT_EQUAL:
            case OP_GREATER:
            case OP_GREATER_EQUAL:
            case OP_LESS:
            case OP_LESS_EQUAL: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val; // Default initialization is not strictly needed if comparison_succeeded ensures it's set.
                                  // However, to be safe, you could do result_val = makeNil(); if it might be used uninitialized.
                bool comparison_succeeded = false;

                // Numeric comparison (Integers and Reals)
                if ((IS_INTEGER(a_val) || IS_REAL(a_val)) && (IS_INTEGER(b_val) || IS_REAL(b_val))) {
                    double fa, fb;
                    if (IS_REAL(a_val) || IS_REAL(b_val)) { // Promote to real if either is real
                        fa = IS_REAL(a_val) ? AS_REAL(a_val) : (double)AS_INTEGER(a_val);
                        fb = IS_REAL(b_val) ? AS_REAL(b_val) : (double)AS_INTEGER(b_val);
                    } else { // Both are integers, treat as reals for comparison to avoid separate logic paths
                        fa = (double)AS_INTEGER(a_val);
                        fb = (double)AS_INTEGER(b_val);
                    }
                    switch (instruction_val) {
                        case OP_EQUAL:         result_val = makeBoolean(fa == fb); break;
                        case OP_NOT_EQUAL:     result_val = makeBoolean(fa != fb); break;
                        case OP_GREATER:       result_val = makeBoolean(fa > fb);  break;
                        case OP_GREATER_EQUAL: result_val = makeBoolean(fa >= fb); break;
                        case OP_LESS:          result_val = makeBoolean(fa < fb);  break;
                        case OP_LESS_EQUAL:    result_val = makeBoolean(fa <= fb); break;
                        default: // Should not be reached if instruction_val is one of these opcodes
                            runtimeError(vm, "VM Error: Unexpected numeric comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // String comparison
                else if (IS_STRING(a_val) && IS_STRING(b_val)) {
                    const char* sa = AS_STRING(a_val) ? AS_STRING(a_val) : "";
                    const char* sb = AS_STRING(b_val) ? AS_STRING(b_val) : "";
                    int cmp = strcmp(sa, sb);
                    switch (instruction_val) {
                        case OP_EQUAL:         result_val = makeBoolean(cmp == 0); break;
                        case OP_NOT_EQUAL:     result_val = makeBoolean(cmp != 0); break;
                        case OP_GREATER:       result_val = makeBoolean(cmp > 0);  break;
                        case OP_GREATER_EQUAL: result_val = makeBoolean(cmp >= 0); break;
                        case OP_LESS:          result_val = makeBoolean(cmp < 0);  break;
                        case OP_LESS_EQUAL:    result_val = makeBoolean(cmp <= 0); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected string comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Char comparison
                else if (IS_CHAR(a_val) && IS_CHAR(b_val)) {
                    char ca = AS_CHAR(a_val);
                    char cb = AS_CHAR(b_val);
                    switch (instruction_val) {
                        case OP_EQUAL:         result_val = makeBoolean(ca == cb); break;
                        case OP_NOT_EQUAL:     result_val = makeBoolean(ca != cb); break;
                        case OP_GREATER:       result_val = makeBoolean(ca > cb);  break;
                        case OP_GREATER_EQUAL: result_val = makeBoolean(ca >= cb); break;
                        case OP_LESS:          result_val = makeBoolean(ca < cb);  break;
                        case OP_LESS_EQUAL:    result_val = makeBoolean(ca <= cb); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected char comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Boolean comparison
                else if (IS_BOOLEAN(a_val) && IS_BOOLEAN(b_val)) {
                    bool ba = AS_BOOLEAN(a_val);
                    bool bb = AS_BOOLEAN(b_val);
                     switch (instruction_val) {
                        case OP_EQUAL:         result_val = makeBoolean(ba == bb); break;
                        case OP_NOT_EQUAL:     result_val = makeBoolean(ba != bb); break;
                        // For Booleans, >, >=, <, <= are sometimes defined as (True > False)
                        // Or disallowed. Standard Pascal typically allows comparison.
                        // Let's assume True=1, False=0 for these.
                        case OP_GREATER:       result_val = makeBoolean(ba > bb);  break;
                        case OP_GREATER_EQUAL: result_val = makeBoolean(ba >= bb); break;
                        case OP_LESS:          result_val = makeBoolean(ba < bb);  break;
                        case OP_LESS_EQUAL:    result_val = makeBoolean(ba <= bb); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected boolean comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // ENUM comparison
                else if (a_val.type == TYPE_ENUM && b_val.type == TYPE_ENUM) {
                    bool types_match = false;
                    if (a_val.enum_val.enum_name == NULL && b_val.enum_val.enum_name == NULL) {
                        types_match = true;
                    } else if (a_val.enum_val.enum_name != NULL && b_val.enum_val.enum_name != NULL &&
                               strcmp(a_val.enum_val.enum_name, b_val.enum_val.enum_name) == 0) {
                        types_match = true;
                    }

                    if (instruction_val == OP_EQUAL) {
                        result_val = makeBoolean(types_match && (a_val.enum_val.ordinal == b_val.enum_val.ordinal));
                    } else if (instruction_val == OP_NOT_EQUAL) {
                        result_val = makeBoolean(!types_match || (a_val.enum_val.ordinal != b_val.enum_val.ordinal));
                    } else {
                        if (!types_match) {
                            runtimeError(vm, "Runtime Error: Cannot compare different ENUM types ('%s' vs '%s') with relational operator.",
                                         a_val.enum_val.enum_name ? a_val.enum_val.enum_name : "<anon>",
                                         b_val.enum_val.enum_name ? b_val.enum_val.enum_name : "<anon>");
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                        int ord_a = a_val.enum_val.ordinal;
                        int ord_b = b_val.enum_val.ordinal;
                        switch (instruction_val) {
                            case OP_GREATER:       result_val = makeBoolean(ord_a > ord_b);  break;
                            case OP_GREATER_EQUAL: result_val = makeBoolean(ord_a >= ord_b); break;
                            case OP_LESS:          result_val = makeBoolean(ord_a < ord_b);  break;
                            case OP_LESS_EQUAL:    result_val = makeBoolean(ord_a <= ord_b); break;
                            default:
                                runtimeError(vm, "VM Error: Unexpected enum comparison opcode %d.", instruction_val);
                                freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    comparison_succeeded = true;
                }
                // Pointer and NIL comparison
                else if ((a_val.type == TYPE_POINTER || a_val.type == TYPE_NIL) &&
                         (b_val.type == TYPE_POINTER || b_val.type == TYPE_NIL)) {
                    bool ptrs_equal = false;
                    if (a_val.type == TYPE_NIL && b_val.type == TYPE_NIL) {
                        ptrs_equal = true;
                    } else if (a_val.type == TYPE_NIL && b_val.type == TYPE_POINTER) {
                        ptrs_equal = (b_val.ptr_val == NULL);
                    } else if (a_val.type == TYPE_POINTER && b_val.type == TYPE_NIL) {
                        ptrs_equal = (a_val.ptr_val == NULL);
                    } else { // Both are TYPE_POINTER
                        ptrs_equal = (a_val.ptr_val == b_val.ptr_val);
                    }

                    // Only OP_EQUAL and OP_NOT_EQUAL are valid for pointers
                    if (instruction_val == OP_EQUAL) {
                        result_val = makeBoolean(ptrs_equal);
                    } else if (instruction_val == OP_NOT_EQUAL) {
                        result_val = makeBoolean(!ptrs_equal);
                    } else {
                        runtimeError(vm, "Runtime Error: Invalid operator for pointer comparison. Only '=' and '<>' are allowed. Got opcode %d.", instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }


                if (comparison_succeeded) {
                    push(vm, result_val);
                } else {
                    // Expanded error message
                    const char* op_str = "unknown_comparison_op";
                    char a_repr[128];
                    char b_repr[128];
                    switch (instruction_val) {
                        case OP_EQUAL:         op_str = "="; break;
                        case OP_NOT_EQUAL:     op_str = "<>"; break;
                        case OP_GREATER:       op_str = ">";  break;
                        case OP_GREATER_EQUAL: op_str = ">="; break; // Opcode 10
                        case OP_LESS:          op_str = "<";  break;
                        case OP_LESS_EQUAL:    op_str = "<="; break; // Opcode 11
                        // Add other comparison opcodes if they exist and are handled by this block
                        default: op_str = "unknown_comparison_op_code"; break; // Should not happen if instruction_val is one of the above
                    }
                    // Buffer for string representations of values, if you choose to include them.
                    // char a_val_buffer[128] = "N/A";
                    // char b_val_buffer[128] = "N/A";
                    // You would need a function like: void valueToString(Value v, char* buffer, size_t buffer_len);
                    // For now, sticking to types.
                    runtimeError(vm, "Runtime Error: Operands not comparable for operator '%s'. Left operand: %s (type %s), Right operand: %s (type %s).",
                                                 op_str,
                                                 a_repr, varTypeToString(a_val.type),
                                                 b_repr, varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case OP_GET_FIELD_ADDRESS: {
                uint8_t field_name_idx = READ_BYTE();
                Value* record_val_ptr = vm->stackTop - 1; // Peek, don't pop

                if (record_val_ptr->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record type.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = record_val_ptr->record_val;
                while (current) {
                    if (strcmp(current->name, field_name) == 0) {
                        // Pop the record value, push a pointer to the field's value
                        pop(vm);
                        push(vm, makePointer(&current->value, NULL));
                        goto next_instruction;
                    }
                    current = current->next;
                }
                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case OP_GET_ELEMENT_ADDRESS: {
                uint8_t dimension_count = READ_BYTE();
                int* indices = malloc(sizeof(int) * dimension_count);
                if (!indices) { /* Malloc error */ return INTERPRET_RUNTIME_ERROR; }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (index_val.type != TYPE_INTEGER) { /* Error */ }
                    indices[dimension_count - 1 - i] = (int)index_val.i_val;
                    freeValue(&index_val);
                }

                Value* array_val_ptr = vm->stackTop - 1; // Peek at the array
                if (array_val_ptr->type != TYPE_ARRAY) { /* Error */ }

                int offset = computeFlatOffset(array_val_ptr, indices);
                free(indices);
                if (offset < 0 || offset >= calculateArrayTotalSize(array_val_ptr)) { /* Bounds error */ }

                // Pop the array value, push a pointer to the element's value
                pop(vm);
                push(vm, makePointer(&array_val_ptr->array_val[offset], NULL));
                break;
            }
            case OP_SET_INDIRECT: {
                Value value_to_set = pop(vm);
                Value pointer_to_lvalue = pop(vm);

                if (pointer_to_lvalue.type != TYPE_POINTER) {
                    runtimeError(vm, "VM Error: SET_INDIRECT requires an address on the stack.");
                    freeValue(&value_to_set); // Clean up popped value
                    freeValue(&pointer_to_lvalue);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* target_lvalue_ptr = (Value*)pointer_to_lvalue.ptr_val;
                if (!target_lvalue_ptr) {
                    runtimeError(vm, "VM Error: SET_INDIRECT called with a nil LValue pointer.");
                    freeValue(&value_to_set);
                    freeValue(&pointer_to_lvalue);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // --- NEW LOGIC: Special handling for pointer assignment ---
                if (target_lvalue_ptr->type == TYPE_POINTER &&
                    (value_to_set.type == TYPE_POINTER || value_to_set.type == TYPE_NIL)) {
                    
                    // This is a pointer assignment (e.g., p1 := p2 or p1 := nil).
                    // We only update the internal pointer value, not the type of the variable itself.
                    target_lvalue_ptr->ptr_val = value_to_set.ptr_val;
                    
                    // Also copy the base type node if the source is a non-nil pointer.
                    if (value_to_set.type == TYPE_POINTER) {
                         target_lvalue_ptr->base_type_node = value_to_set.base_type_node;
                    }
                    // The type of the target L-Value remains TYPE_POINTER.

                } else {
                    // This is a regular assignment to a non-pointer (e.g., p1^ := 5),
                    // or the types are otherwise incompatible for direct pointer assignment.
                    freeValue(target_lvalue_ptr); // Free the old value at the target location
                    *target_lvalue_ptr = makeCopyOfValue(&value_to_set); // Copy new value into place
                }

                // Clean up the temporary values that were on the stack
                freeValue(&value_to_set);
                freeValue(&pointer_to_lvalue);
                break;
            }
            case OP_GET_INDIRECT: {
                Value pointer_val = pop(vm);
                if (pointer_val.type != TYPE_POINTER) {
                    runtimeError(vm, "VM Error: GET_INDIRECT requires an address on the stack.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_lvalue_ptr = (Value*)pointer_val.ptr_val;
                if (target_lvalue_ptr == NULL) {
                    runtimeError(vm, "VM Error: GET_INDIRECT on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Push a copy of the value at the address
                push(vm, makeCopyOfValue(target_lvalue_ptr));
                freeValue(&pointer_val); // Clean up the temporary pointer value
                break;
            }

            case OP_DEFINE_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                VarType declaredType = (VarType)READ_BYTE();

                if (declaredType == TYPE_ARRAY) {
                    uint8_t dimension_count = READ_BYTE();
                    if (dimension_count == 0) {
                        runtimeError(vm, "VM Error: Array defined with zero dimensions for '%s'.", varNameVal.s_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    int* lower_bounds = malloc(sizeof(int) * dimension_count);
                    int* upper_bounds = malloc(sizeof(int) * dimension_count);
                    if (!lower_bounds || !upper_bounds) {
                        runtimeError(vm, "VM Error: Malloc failed for array bounds construction.");
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for(int i = 0; i < dimension_count; i++) {
                        uint8_t lower_idx = READ_BYTE();
                        uint8_t upper_idx = READ_BYTE();
                        Value lower_val = vm->chunk->constants[lower_idx];
                        Value upper_val = vm->chunk->constants[upper_idx];
                        if (lower_val.type != TYPE_INTEGER || upper_val.type != TYPE_INTEGER) {
                             runtimeError(vm, "VM Error: Invalid constant types for array bounds of '%s'.", varNameVal.s_val);
                             free(lower_bounds); free(upper_bounds);
                             return INTERPRET_RUNTIME_ERROR;
                        }
                        lower_bounds[i] = (int)lower_val.i_val;
                        upper_bounds[i] = (int)upper_val.i_val;
                    }

                    VarType elem_var_type = (VarType)READ_BYTE();

                    // Read the element type name from the constant pool.
                    uint8_t elem_name_idx = READ_BYTE();
                    Value elem_name_val = vm->chunk->constants[elem_name_idx];
                    AST* elem_type_def = NULL;

                    // If a type name was provided, look up its AST definition.
                    // This is crucial for creating arrays of records or other named types.
                    if (elem_name_val.type == TYPE_STRING && elem_name_val.s_val && elem_name_val.s_val[0] != '\0') {
                        elem_type_def = lookupType(elem_name_val.s_val);
                    }

                    // Pass the found type definition to makeArrayND.
                    Value array_val = makeArrayND(dimension_count, lower_bounds, upper_bounds, elem_var_type, elem_type_def);
                   

                    Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                    if (sym == NULL) {
                        // Replace the call to createSymbolForVM with manual symbol creation
                        // to avoid the unnecessary and problematic call to makeValueForType.
                        sym = (Symbol*)malloc(sizeof(Symbol));
                        if (!sym) {
                            runtimeError(vm, "VM Error: Malloc failed for Symbol struct for global array '%s'.", varNameVal.s_val);
                            freeValue(&array_val); free(lower_bounds); free(upper_bounds); return INTERPRET_RUNTIME_ERROR;
                        }
                        sym->name = strdup(varNameVal.s_val);
                        if (!sym->name) {
                            runtimeError(vm, "VM Error: Malloc failed for symbol name for global array '%s'.", varNameVal.s_val);
                            free(sym); freeValue(&array_val); free(lower_bounds); free(upper_bounds); return INTERPRET_RUNTIME_ERROR;
                        }
                        toLowerString(sym->name);
                        
                        sym->type = declaredType; // This is TYPE_ARRAY
                        sym->type_def = NULL;     // We don't have the AST node in the VM
                        sym->value = (Value*)malloc(sizeof(Value));
                        if (!sym->value) {
                             runtimeError(vm, "VM Error: Malloc failed for Value struct for global array '%s'.", varNameVal.s_val);
                             free(sym->name); free(sym); freeValue(&array_val); free(lower_bounds); free(upper_bounds); return INTERPRET_RUNTIME_ERROR;
                        }

                        // Directly assign the correctly-created array value, bypassing makeValueForType(TYPE_ARRAY, NULL).
                        *(sym->value) = array_val;

                        sym->is_alias = false;
                        sym->is_const = false;
                        sym->is_local_var = false;
                        sym->next = NULL;

                        hashTableInsert(vm->vmGlobalSymbols, sym);
                    } else {
                        runtimeError(vm, "VM Warning: Global variable '%s' redefined.", varNameVal.s_val);
                        freeValue(sym->value);
                        *(sym->value) = array_val;
                    }

                    free(lower_bounds);
                    free(upper_bounds);
                } else {
                    // This is for non-array types (Integer, Pointer, Record, etc.)
                    // The compiler provides the type name's index in the constant pool.
                    uint8_t type_name_idx = READ_BYTE();
                    Value typeNameVal = vm->chunk->constants[type_name_idx];
                    AST* type_def_node = NULL;

                    // If a type name was provided (e.g., for PInt = ^Integer, it's 'pint'),
                    // look up its full definition AST node from the type table created by the parser.
                    if (typeNameVal.type == TYPE_STRING && typeNameVal.s_val) {
                        type_def_node = lookupType(typeNameVal.s_val);
                    }

                    if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
                         runtimeError(vm, "VM Error: Invalid variable name for OP_DEFINE_GLOBAL.");
                         return INTERPRET_RUNTIME_ERROR;
                    }

                    Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                    if (sym == NULL) {
                        // Pass the looked-up AST node to the symbol creation function.
                        // This node contains the necessary metadata for pointer base types.
                        sym = createSymbolForVM(varNameVal.s_val, declaredType, type_def_node);
                        if (!sym) {
                             runtimeError(vm, "VM Error: Failed to create symbol for global '%s'.", varNameVal.s_val);
                             return INTERPRET_RUNTIME_ERROR;
                        }
                        hashTableInsert(vm->vmGlobalSymbols, sym);
                    } else {
                         runtimeError(vm, "VM Warning: Global variable '%s' redefined.", varNameVal.s_val);
                    }
                }
                break;
            }
            case OP_GET_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) { /* error */ return INTERPRET_RUNTIME_ERROR; }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", varNameVal.s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeCopyOfValue(sym->value));
                break;
            }
            case OP_SET_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
                    runtimeError(vm, "VM Error: Invalid var name constant for SET_GLOBAL.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym) {
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", varNameVal.s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        runtimeError(vm, "VM Error: Malloc failed for symbol value in SET_GLOBAL.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def, sym);
                }

                // Pop the value from the stack first.
                Value value_from_stack = pop(vm);
                Value value_to_assign;

                // --- Perform type coercion (this logic can be simplified now) ---
                if (sym->type == TYPE_CHAR && value_from_stack.type == TYPE_STRING) {
                    if (value_from_stack.s_val && strlen(value_from_stack.s_val) == 1) {
                        value_to_assign = makeChar(value_from_stack.s_val[0]);
                    } else {
                        runtimeError(vm, "Runtime Error: Cannot assign multi-character string or null string to CHAR variable '%s'.", sym->name);
                        freeValue(&value_from_stack); // Free the popped value before exiting
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (sym->type == TYPE_REAL && value_from_stack.type == TYPE_INTEGER) {
                    value_to_assign = makeReal((double)value_from_stack.i_val);
                } else if (sym->type == TYPE_INTEGER && value_from_stack.type == TYPE_REAL) {
                    value_to_assign = makeInt((long long)value_from_stack.r_val);
                }
                else {
                    // No coercion needed, use the popped value directly for the copy
                    value_to_assign = value_from_stack;
                }

                freeValue(sym->value); // Free existing contents of the symbol's value
                *(sym->value) = makeCopyOfValue(&value_to_assign); // Assign a deep copy

                // If a new temporary value was created for coercion, free it
                if (value_to_assign.type != value_from_stack.type) {
                    freeValue(&value_to_assign);
                }

                // Free the original value popped from the stack
                freeValue(&value_from_stack);
                
                break;
            }
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                push(vm, makeCopyOfValue(&frame->slots[slot]));
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                // Free the old value at the slot before overwriting, if it's a complex type
                freeValue(&frame->slots[slot]);
                // POP the value from the top of the stack and make a deep copy into the slot
                Value value_to_set = pop(vm); // Changed from peek(vm, 0)
                frame->slots[slot] = makeCopyOfValue(&value_to_set);
                // Free the temporary value that was popped from the stack
                freeValue(&value_to_set);
                break;
            }
            case OP_JUMP_IF_FALSE: {
                uint16_t offset_val = READ_SHORT(vm); // Use READ_SHORT(vm) to pass vm
                Value condition_value = pop(vm);
                bool jump_condition_met = false;
                if (IS_BOOLEAN(condition_value)) {
                    jump_condition_met = !AS_BOOLEAN(condition_value);
                } else {
                    runtimeError(vm, "VM Error: IF condition must be a Boolean.");
                    freeValue(&condition_value);
                    return INTERPRET_RUNTIME_ERROR;
                }
                // freeValue(&condition_value); // Only if makeCopyOfValue was used on stack for the boolean. Primitives usually don't need this.

                if (jump_condition_met) {
                    vm->ip += offset_val;
                }
                break;
            }
            case OP_JUMP: {
                uint16_t offset = READ_SHORT(vm);
                vm->ip += (int16_t)offset;
                break;
            }
            case OP_WRITE_LN: {
                #define MAX_WRITELN_ARGS_VM 32 // Keep this define or ensure it's globally accessible
                uint8_t argCount = READ_BYTE();
                Value args_for_writeln[MAX_WRITELN_ARGS_VM]; // Still using a temporary array to reverse popped args

                if (argCount > MAX_WRITELN_ARGS_VM) {
                    runtimeError(vm, "VM Error: Too many arguments for OP_WRITE_LN (max %d).", MAX_WRITELN_ARGS_VM);
                    // Note: If actual_args was dynamic, it would need freeing here on error.
                    // Since args_for_writeln is stack-based, it's okay.
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Pop arguments from VM stack into the temporary C array to get them in correct order
                for (int i = 0; i < argCount; i++) {
                    if (vm->stackTop == vm->stack) { // Check for underflow before each pop
                        runtimeError(vm, "VM Error: Stack underflow preparing arguments for OP_WRITE_LN. Expected %d, premature empty.", argCount);
                        // Clean up already popped arguments in args_for_writeln if any
                        for (int k = 0; k < i; ++k) {
                            freeValue(&args_for_writeln[argCount - 1 - k]);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    args_for_writeln[argCount - 1 - i] = pop(vm);
                }

                // Print arguments using logic similar to the AST interpreter's WriteLn
                for (int i = 0; i < argCount; i++) {
                    Value val = args_for_writeln[i]; // Get the argument in the correct order

                    // Note: The AST interpreter checks argNode->type == AST_FORMATTED_EXPR.
                    // The VM bytecode for formatted expressions would likely be different
                    // (e.g., a dedicated OP_WRITE_FORMATTED or the formatting done before OP_WRITE_LN,
                    // resulting in a pre-formatted string on the stack).
                    // For now, we'll assume 'val' is the raw value to be printed.
                    // If formatting opcodes are added later, this logic might need adjustment
                    // or OP_WRITE_LN would expect already formatted strings.

                    // Replicate printing logic from AST interpreter for consistency:
                    if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                        fprintf(stdout, "%lld", val.i_val);
                    } else if (val.type == TYPE_REAL) {
                        fprintf(stdout, "%f", val.r_val);
                    } else if (val.type == TYPE_BOOLEAN) {
                        // AST interpreter's WriteLn prints booleans as TRUE/FALSE words.
                        // The specific `fprintf` for boolean in AST was:
                        // else if (val.type == TYPE_BOOLEAN) fprintf(output, "%s", (val.i_val != 0) ? "true" : "false");
                        // Let's use that for consistency:
                        fprintf(stdout, "%s", (val.i_val != 0) ? "TRUE" : "FALSE"); // Standard Pascal output
                    } else if (val.type == TYPE_STRING) {
                        // Print string content directly without adding extra quotes
                        fprintf(stdout, "%s", val.s_val ? val.s_val : "");
                    } else if (val.type == TYPE_CHAR) {
                        // Print char directly without quotes
                        fputc(val.c_val, stdout);
                    } else if (val.type == TYPE_ENUM) {
                        fprintf(stdout, "%s", val.enum_val.enum_name ? val.enum_val.enum_name : "?");
                    }
                    // Other types like NIL, POINTER, RECORD, ARRAY, SET, FILE are typically not directly printable
                    // by a simple WriteLn argument in standard Pascal without formatting or specific handling.
                    // The AST interpreter had:
                    // else if (val.type != TYPE_FILE) fprintf(output, "[unprintable_type_%d]", val.type);
                    // We can adopt a similar placeholder for unhandled types.
                    else if (val.type == TYPE_NIL) {
                        fprintf(stdout, "NIL"); // Consistent with AST version of printValueToStream
                    }
                    // Add other types as needed, or a default placeholder:
                    else if (val.type != TYPE_FILE && val.type != TYPE_MEMORYSTREAM && val.type != TYPE_POINTER && val.type != TYPE_RECORD && val.type != TYPE_ARRAY && val.type != TYPE_SET) {
                         // This condition is getting complex. A helper function or a switch might be cleaner.
                         // For now, if it's not one of the above and not a known complex unprintable:
                         fprintf(stdout, "<VM_PRINT_TYPE_%s>", varTypeToString(val.type));
                    }


                    // Free the Value struct's contents (e.g., s_val if it was a string)
                    // This 'val' is a copy from args_for_writeln, which itself was a copy from stack.
                    freeValue(&val);

                    if (i < argCount - 1) {
                        // Standard Pascal WriteLn typically does not add spaces between arguments unless
                        // they are formatted with field widths. If you want spaces, add printf(" ");
                        // AST interpreter's WriteLn does not add spaces by default.
                        // For now, no space to match typical WriteLn behavior for unformatted args.
                    }
                }
                printf("\n"); // The "Ln" part
                fflush(stdout); // Ensure output is flushed
                break;
            }
            case OP_WRITE: {
                uint8_t argCount = READ_BYTE();
                Value args_for_write[MAX_WRITELN_ARGS_VM];

                if (argCount > MAX_WRITELN_ARGS_VM) {
                    runtimeError(vm, "VM Error: Too many arguments for OP_WRITE (max %d).", MAX_WRITELN_ARGS_VM);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < argCount; i++) {
                    if (vm->stackTop == vm->stack) {
                        runtimeError(vm, "VM Error: Stack underflow preparing arguments for OP_WRITE.");
                        for (int k = 0; k < i; ++k) {
                            freeValue(&args_for_write[argCount - 1 - k]);
                        }
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    args_for_write[argCount - 1 - i] = pop(vm);
                }

                for (int i = 0; i < argCount; i++) {
                    Value val = args_for_write[i];
                    if (val.type == TYPE_INTEGER || val.type == TYPE_BYTE || val.type == TYPE_WORD) {
                        fprintf(stdout, "%lld", val.i_val);
                    } else if (val.type == TYPE_REAL) {
                        fprintf(stdout, "%f", val.r_val);
                    } else if (val.type == TYPE_BOOLEAN) {
                        fprintf(stdout, "%s", (val.i_val != 0) ? "TRUE" : "FALSE");
                    } else if (val.type == TYPE_STRING) {
                        fprintf(stdout, "%s", val.s_val ? val.s_val : "");
                    } else if (val.type == TYPE_CHAR) {
                        fputc(val.c_val, stdout);
                    } else if (val.type == TYPE_ENUM) {
                        fprintf(stdout, "%s", val.enum_val.enum_name ? val.enum_val.enum_name : "?");
                    } else if (val.type == TYPE_NIL) {
                        fprintf(stdout, "NIL");
                    }
                    freeValue(&val);
                }
                fflush(stdout);
                break;
            }
            case OP_POP: {
                Value popped_val = pop(vm);
                freeValue(&popped_val);
                break;
            }
            case OP_CALL_BUILTIN: {
                uint8_t name_const_idx = READ_BYTE();
                uint8_t arg_count = READ_BYTE();

                if (vm->stackTop - vm->stack < arg_count) {
                    runtimeError(vm, "VM Stack underflow for built-in arguments.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* args = vm->stackTop - arg_count;
                const char* builtin_name = AS_STRING(vm->chunk->constants[name_const_idx]);
                
                VmBuiltinFn handler = getVmBuiltinHandler(builtin_name);

                if (handler) {
                    Value result = handler(vm, arg_count, args);
                    
                    vm->stackTop -= arg_count; // Pop arguments

                    if (getBuiltinType(builtin_name) == BUILTIN_TYPE_FUNCTION) {
                        push(vm, result);
                    } else {
                        freeValue(&result);
                    }
                } else {
                    runtimeError(vm, "VM Error: Unimplemented or unknown built-in '%s' called.", builtin_name);
                    vm->stackTop -= arg_count; // Pop args to clean stack
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case OP_CALL: { // EXECUTION logic for OP_CALL
                  if (vm->frameCount >= VM_CALL_STACK_MAX) {
                      runtimeError(vm, "VM Error: Call stack overflow.");
                      return INTERPRET_RUNTIME_ERROR;
                  }

                  uint16_t target_address = READ_SHORT(vm); // Read 2-byte address
                  uint8_t declared_arity = READ_BYTE();   // Read 1-byte declared arity

                  if (vm->stackTop - vm->stack < declared_arity) {
                      runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %d, have %ld.",
                                   declared_arity, (long)(vm->stackTop - vm->stack));
                      return INTERPRET_RUNTIME_ERROR;
                  }

                  CallFrame* frame = &vm->frames[vm->frameCount++];
                  frame->return_address = vm->ip; // Current ip is *after* OP_CALL's operands
                  frame->slots = vm->stackTop - declared_arity; // New frame starts where args are on stack

                  // Find the procedure's symbol to get locals_count
                  Symbol* proc_symbol = NULL;
                  if(vm->procedureTable) {
                      for (int i = 0; i < HASHTABLE_SIZE; i++) {
                          for (Symbol* s = vm->procedureTable->buckets[i]; s; s = s->next) {
                              if (s->is_defined && s->bytecode_address == target_address) {
                                  proc_symbol = s;
                                  break;
                              }
                          }
                          if (proc_symbol) break;
                      }
                  }
                  if (!proc_symbol) {
                      runtimeError(vm, "VM Error: Could not retrieve procedure symbol for called address %04X.", target_address);
                      vm->frameCount--; // Revert frame increment
                      return INTERPRET_RUNTIME_ERROR;
                  }
                  frame->function_symbol = proc_symbol;
                  frame->locals_count = proc_symbol->locals_count;

                  // Reserve space on stack for local variables by pushing nils
                  for (int i = 0; i < frame->locals_count; i++) {
                      push(vm, makeNil());
                  }
                  
                  vm->ip = vm->chunk->code + target_address; // Jump to the function/procedure
                  break;
            }
            case OP_HALT:
                return INTERPRET_OK;
            case OP_CALL_HOST: {
                HostFunctionID host_id = READ_HOST_ID();
                if (host_id >= HOST_FN_COUNT || vm->host_functions[host_id] == NULL) {
                    runtimeError(vm, "Invalid host function ID %d or function not registered.", host_id);
                    return INTERPRET_RUNTIME_ERROR;
                }
                HostFn func = vm->host_functions[host_id];
                Value result = func(vm);
                push(vm, result);
                break;
            }
            case OP_FORMAT_VALUE: {
                // Read operands from the bytecode stream
                uint8_t width = READ_BYTE();
                uint8_t precision_raw = READ_BYTE();
                int precision = (precision_raw == 0xFF) ? -1 : precision_raw; // Convert 0xFF back to -1

                // Pop the raw value to be formatted
                Value raw_val = pop(vm);

                char buf[DEFAULT_STRING_CAPACITY]; // From globals.h
                buf[0] = '\0';

                // This logic is borrowed from the working AST interpreter's eval function
                if (raw_val.type == TYPE_REAL) {
                    if (precision >= 0) {
                        snprintf(buf, sizeof(buf), "%*.*f", width, precision, raw_val.r_val);
                    } else {
                        snprintf(buf, sizeof(buf), "%*.*E", width, PASCAL_DEFAULT_FLOAT_PRECISION, raw_val.r_val);
                    }
                } else if (raw_val.type == TYPE_INTEGER || raw_val.type == TYPE_BYTE || raw_val.type == TYPE_WORD) {
                    snprintf(buf, sizeof(buf), "%*lld", width, raw_val.i_val);
                } else if (raw_val.type == TYPE_STRING) {
                    const char* source_str = raw_val.s_val ? raw_val.s_val : "";
                    snprintf(buf, sizeof(buf), "%*.*s", width, (int)strlen(source_str), source_str);
                } else if (raw_val.type == TYPE_BOOLEAN) {
                    const char* bool_str = raw_val.i_val ? "TRUE" : "FALSE";
                    snprintf(buf, sizeof(buf), "%*s", width, bool_str);
                } else if (raw_val.type == TYPE_CHAR) {
                    snprintf(buf, sizeof(buf), "%*c", width, raw_val.c_val);
                } else {
                    snprintf(buf, sizeof(buf), "%*s", width, "?");
                }

                // Free the raw value that was popped
                freeValue(&raw_val);
                
                // Push the newly formatted string value back onto the stack
                push(vm, makeString(buf));
                break;
            }
  
            default:
                runtimeError(vm, "VM Error: Unknown opcode %d.", instruction_val);
                return INTERPRET_RUNTIME_ERROR;
        }
        next_instruction:; 
    }
    return INTERPRET_OK;
}

/*
 #ifdef DEBUG
 static void dumpSymbolValue(Value* v) {
 if (!v) { printf("NULL_Value_Ptr"); return; }
 printValueToStream(*v, stderr);
 }
 #endif
 */
