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

// --- VM Helper Functions ---

static void resetStack(VM* vm) {
    vm->stackTop = vm->stack;
}

// runtimeError - Assuming your existing one is fine.
static void runtimeError(VM* vm, const char* format, ...) {
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
    // vm->vmGlobalSymbols = createHashTable(); // createHashTable is in symbol.c, this was in all.txt
                                             // but interpretBytecode receives globals now.
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
    // ... (your existing implementation is likely fine here) ...
    if (!name || name[0] == '\0') {
        fprintf(stderr, "VM Internal Error: Invalid name for createSymbolForVM.\n");
        return NULL;
    }
    Symbol *sym = (Symbol*)malloc(sizeof(Symbol));
    if (!sym) {
        fprintf(stderr, "VM Internal Error: Malloc failed for Symbol in createSymbolForVM for '%s'.\n", name);
        return NULL;
    }
    sym->name = strdup(name);
    if (!sym->name) {
        fprintf(stderr, "VM Internal Error: Malloc failed for Symbol name in createSymbolForVM for '%s'.\n", name);
        free(sym);
        return NULL;
    }
    sym->type = type;
    sym->type_def = type_def_for_value_init;
    sym->value = (Value*)malloc(sizeof(Value));
    if (!sym->value) {
        fprintf(stderr, "VM Internal Error: Malloc failed for Value in createSymbolForVM for '%s'.\n", name);
        free(sym->name);
        free(sym);
        return NULL;
    }
    *(sym->value) = makeValueForType(type, type_def_for_value_init);
    sym->is_alias = false;
    sym->is_const = false;
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
        /* Special handling for OP_ADD if operands are strings or char */ \
        if (current_instruction_code == OP_ADD) { \
            if ((IS_STRING(a_val_popped) || IS_CHAR(a_val_popped)) && \
                (IS_STRING(b_val_popped) || IS_CHAR(b_val_popped))) { \
                \
                char a_buffer[2] = {0}; /* For char to string conversion */ \
                char b_buffer[2] = {0}; \
                const char* s_a = NULL; \
                const char* s_b = NULL; \
                \
                if (IS_STRING(a_val_popped)) { \
                    s_a = AS_STRING(a_val_popped) ? AS_STRING(a_val_popped) : ""; \
                } else { /* IS_CHAR */ \
                    a_buffer[0] = AS_CHAR(a_val_popped); \
                    s_a = a_buffer; \
                } \
                \
                if (IS_STRING(b_val_popped)) { \
                    s_b = AS_STRING(b_val_popped) ? AS_STRING(b_val_popped) : ""; \
                } else { /* IS_CHAR */ \
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
                result_val = makeString(concat_buffer); /* makeString duplicates concat_buffer */ \
                free(concat_buffer); \
                op_is_handled = true; \
            } \
        } \
        \
        /* If not handled by string concatenation (or not OP_ADD), try numeric */ \
        if (!op_is_handled) { \
            if ((IS_INTEGER(a_val_popped) || IS_REAL(a_val_popped)) && \
                (IS_INTEGER(b_val_popped) || IS_REAL(b_val_popped))) { \
                \
                if (IS_REAL(a_val_popped) || IS_REAL(b_val_popped)) { /* Numeric promotion to Real */ \
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
                } else { /* Both are integers */ \
                    long long ia = AS_INTEGER(a_val_popped); \
                    long long ib = AS_INTEGER(b_val_popped); \
                    if (current_instruction_code == OP_DIVIDE && ib == 0) { /* Note: OP_DIVIDE with ints produces REAL */ \
                        runtimeError(vm, "Runtime Error: Division by zero (integer)."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    switch(current_instruction_code) { \
                        case OP_ADD:      result_val = makeInt(ia + ib); break; \
                        case OP_SUBTRACT: result_val = makeInt(ia - ib); break; \
                        case OP_MULTIPLY: result_val = makeInt(ia * ib); break; \
                        case OP_DIVIDE:   result_val = makeReal((double)ia / (double)ib); break; /* Integer division result is Real */ \
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

    uint8_t instruction_val; // Used for the current instruction in the loop
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

        instruction_val = READ_BYTE(); // Uses the macro which uses vm->ip from runVM
        switch (instruction_val) {
            case OP_RETURN: { // EXECUTION logic for OP_RETURN
                 if (vm->frameCount == 0) {
                     runtimeError(vm, "VM Error: Cannot return from top-level script (no call frame).");
                     return INTERPRET_RUNTIME_ERROR;
                 }

                 Value returnValue = pop(vm); // Pop the function's result (or a dummy value for procedures)

                 vm->frameCount--; // Go back to the caller's frame index
                 
                 CallFrame* frame = &vm->frames[vm->frameCount]; // Get the frame we are returning TO (caller)
                                                                 // Note: frameCount was already decremented.
                 
                 vm->stackTop = frame->slots;    // Reset stack top to where the caller's frame started,
                                                 // effectively popping all of callee's slots (args + locals).
                 push(vm, returnValue);          // Push the function's actual result back onto the caller's stack.

                 if (vm->frameCount == 0) { // If we just returned from the main script body
                     return INTERPRET_OK; // Program finished
                 }
                 
                 vm->ip = frame->return_address; // Jump back to the caller's next instruction.
                 break;
             }

            case OP_CONSTANT: {
                Value constant = READ_CONSTANT(); // Uses the macro
                push(vm, makeCopyOfValue(&constant));
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
            case OP_DEFINE_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                VarType declaredType = (VarType)READ_BYTE();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) { /* error */ return INTERPRET_RUNTIME_ERROR; }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (sym == NULL) {
                    sym = createSymbolForVM(varNameVal.s_val, declaredType, NULL);
                    if (!sym) { /* error */ return INTERPRET_RUNTIME_ERROR; }
                    hashTableInsert(vm->vmGlobalSymbols, sym);
                    #ifdef DEBUG
                        if(dumpExec) fprintf(stderr, "VM: Defined global '%s' type %s\n", varNameVal.s_val, varTypeToString(declaredType));
                    #endif
                } else { /* error or warning for redefinition */ }
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
                    // Value is peeked, so no pop needed before error return
                    return INTERPRET_RUNTIME_ERROR;
                }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym) {
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", varNameVal.s_val);
                    // Value is peeked, so no pop needed before error return
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!sym->value) { // Should have been initialized by OP_DEFINE_GLOBAL
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        runtimeError(vm, "VM Error: Malloc failed for symbol value in SET_GLOBAL.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def); // type_def might be NULL
                }

                Value value_from_stack = peek(vm, 0); // Value to be assigned is on top of stack
                Value value_to_assign; // This will hold the (potentially coerced) value to store

                // Perform type coercion
                if (sym->type == TYPE_CHAR && value_from_stack.type == TYPE_STRING) {
                    if (value_from_stack.s_val && strlen(value_from_stack.s_val) == 1) {
                        value_to_assign = makeChar(value_from_stack.s_val[0]);
                        // No need to free value_from_stack.s_val here, it's owned by the constant pool or another Value
                    } else {
                        runtimeError(vm, "Runtime Error: Cannot assign multi-character string or null string to CHAR variable '%s'.", sym->name);
                        // Don't pop yet, the error will stop execution.
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (sym->type == TYPE_REAL && value_from_stack.type == TYPE_INTEGER) {
                    value_to_assign = makeReal((double)value_from_stack.i_val);
                } else if (sym->type == TYPE_INTEGER && value_from_stack.type == TYPE_REAL) {
                    // Standard Pascal usually requires explicit Trunc/Round for Real->Integer.
                    // For now, let's allow direct assignment with truncation for VM simplicity, or error.
                    // runtimeError(vm, "Runtime Error: Cannot implicitly assign REAL to INTEGER for '%s'. Use Trunc() or Round().", sym->name);
                    // return INTERPRET_RUNTIME_ERROR;
                    value_to_assign = makeInt((long long)value_from_stack.r_val); // Implicit truncation
                }
                // Add other coercions like Integer to Byte/Word with range checks if desired
                else {
                    // No coercion needed, or types are incompatible (makeCopyOfValue will handle copy)
                    // If types are truly incompatible and not handled by makeCopyOfValue implicitly,
                    // an error should be raised here after checking sym->type vs value_from_stack.type
                    if (sym->type != value_from_stack.type && typeWarn) { // Basic check
                         // More sophisticated checks needed for assign-compatibility like array types, record types etc.
                         // For now, if not char/real/int coercion, rely on direct copy or error later if problematic
                        // fprintf(stderr, "VM Warning: Potential type mismatch assigning %s to %s for '%s'\n",
                        //        varTypeToString(value_from_stack.type), varTypeToString(sym->type), sym->name);
                    }
                    value_to_assign = value_from_stack; // Use the stack value directly for makeCopyOfValue
                }

                freeValue(sym->value); // Free existing contents of the symbol's value
                *(sym->value) = makeCopyOfValue(&value_to_assign); // Assign a deep copy

                // If value_to_assign was a new temporary Value created by makeChar/makeReal,
                // and if makeCopyOfValue did not consume its *contents*, it would need freeing.
                // However, makeChar/makeReal for primitives don't allocate contents that makeCopyOfValue wouldn't handle.
                // If value_to_assign refers to value_from_stack, makeCopyOfValue handles its contents.
                if (value_to_assign.type != value_from_stack.type && value_to_assign.s_val != value_from_stack.s_val) {
                    // This condition means value_to_assign is a new temporary (e.g. from makeChar for string->char coercion)
                    // and makeCopyOfValue would have duplicated its primitive data.
                    // If makeChar itself allocated something for s_val (it doesn't for char), it'd be freed here.
                    // Since makeChar just sets c_val, and makeReal sets r_val, freeValue is a no-op for their *contents*.
                    freeValue(&value_to_assign);
                }


                Value popped_val_after_assign = pop(vm); // Now pop the original value from stack
                freeValue(&popped_val_after_assign);    // And free its contents

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
                uint16_t offset_val = READ_SHORT(vm); // Use READ_SHORT(vm) to pass vm
                vm->ip += offset_val;
                break;
            }
            case OP_WRITE_LN: {
                #define MAX_WRITELN_ARGS_VM 32
                uint8_t argCount = READ_BYTE();
                Value args_for_writeln[MAX_WRITELN_ARGS_VM];
                if (argCount > MAX_WRITELN_ARGS_VM) { /* error */ return INTERPRET_RUNTIME_ERROR; }

                for (int i = 0; i < argCount; i++) {
                    args_for_writeln[argCount - 1 - i] = pop(vm);
                }

                for (int i = 0; i < argCount; i++) {
                    Value val = args_for_writeln[i];
                    printValueToStream(val, stdout);
                    if (i < argCount - 1) {
                        printf(" ");
                    }
                    freeValue(&val);
                }
                printf("\n");
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

                if (name_const_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Invalid constant index for built-in name.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value builtinNameVal = vm->chunk->constants[name_const_idx];

                if (builtinNameVal.type != TYPE_STRING || !builtinNameVal.s_val) {
                    runtimeError(vm, "VM Error: Invalid built-in name constant for OP_CALL_BUILTIN (not a string).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                const char* builtin_name = builtinNameVal.s_val;

                Value actual_args[256];
                if (vm->stackTop - vm->stack < arg_count) {
                    runtimeError(vm, "VM Error: Stack underflow preparing arguments for built-in %s. Expected %d, have %ld.",
                                 builtin_name, arg_count, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < arg_count; i++) {
                    actual_args[arg_count - 1 - i] = pop(vm);
                }

                Value result_val = makeNil();
                bool is_function_that_succeeded = false;

                // --- Dispatch to C implementation for the built-in ---
                if (strcasecmp(builtin_name, "abs") == 0) {
                    // ... (abs logic - already provided) ...
                } else if (strcasecmp(builtin_name, "length") == 0) {
                    // ... (length logic - already provided) ...
                } else if (strcasecmp(builtin_name, "ord") == 0) {
                    // ... (ord logic - already provided and updated) ...
                } else if (strcasecmp(builtin_name, "chr") == 0) {
                    // ... (chr logic - already provided) ...
                } else if (strcasecmp(builtin_name, "randomize") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: Randomize expects 0 arguments."); goto op_call_builtin_error_cleanup; }
                    srand((unsigned int)time(NULL));
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "inittextsystem") == 0) {
                    if (arg_count != 2) { runtimeError(vm, "VM: InitTextSystem expects 2 args (FontFileName: String, FontSize: Integer)."); goto op_call_builtin_error_cleanup; }
                    Value fontNameVal = actual_args[0];
                    Value fontSizeVal = actual_args[1];
                    if (!IS_STRING(fontNameVal) || !AS_STRING(fontNameVal) || !IS_INTEGER(fontSizeVal)) {
                        runtimeError(vm, "VM: InitTextSystem argument type mismatch."); goto op_call_builtin_error_cleanup;
                    }
                    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "VM: Core SDL Graphics not initialized before InitTextSystem."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlTtfInitialized) {
                        if (TTF_Init() == -1) { runtimeError(vm, "VM: SDL_ttf system initialization failed: %s", TTF_GetError()); goto op_call_builtin_error_cleanup; }
                        gSdlTtfInitialized = true;
                    }
                    const char* font_path = AS_STRING(fontNameVal);
                    int font_size = (int)AS_INTEGER(fontSizeVal);
                    if (gSdlFont) { TTF_CloseFont(gSdlFont); gSdlFont = NULL; }
                    gSdlFont = TTF_OpenFont(font_path, font_size);
                    if (!gSdlFont) { runtimeError(vm, "VM: Failed to load font '%s': %s", font_path, TTF_GetError()); goto op_call_builtin_error_cleanup; }
                    gSdlFontSize = font_size;
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "initsoundsystem") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: InitSoundSystem expects 0 arguments."); goto op_call_builtin_error_cleanup; }
                    audioInitSystem(); // Call the C function from audio.c
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "loadsound") == 0) {
                    if (arg_count != 1) { runtimeError(vm, "VM: LoadSound expects 1 arg (FileName: String)."); goto op_call_builtin_error_cleanup; }
                    Value fileNameVal = actual_args[0]; // This is the Value struct for the filename argument
                    if (!IS_STRING(fileNameVal) || !AS_STRING(fileNameVal)) {
                        runtimeError(vm, "VM: LoadSound argument must be a valid String.");
                        goto op_call_builtin_error_cleanup;
                    }

                    const char* original_filename = AS_STRING(fileNameVal);
                    char full_path[512]; // Buffer for the full path
                    const char* filename_to_pass = original_filename;

                    // Prepend default sound directory if the path is not absolute or relative
                    if (original_filename && original_filename[0] != '.' && original_filename[0] != '/') {
                        const char* default_sound_dir = "/usr/local/Pscal/lib/sounds/"; // As per your ls output
                        int chars_written = snprintf(full_path, sizeof(full_path), "%s%s", default_sound_dir, original_filename);
                        if (chars_written < 0 || (size_t)chars_written >= sizeof(full_path)) {
                            runtimeError(vm, "VM: Constructed sound file path too long for '%s'.", original_filename);
                            goto op_call_builtin_error_cleanup;
                        }
                        filename_to_pass = full_path;
                    }
                    
                    #ifdef DEBUG
                    fprintf(stderr, "VM LoadSound: Attempting to load from path: '%s'\n", filename_to_pass);
                    #endif

                    result_val = makeInt(audioLoadSound(filename_to_pass)); // audioLoadSound takes const char*
                    is_function_that_succeeded = true;
                } else if (strcasecmp(builtin_name, "playsound") == 0) {
                    if (arg_count != 1) { runtimeError(vm, "VM: PlaySound expects 1 arg (SoundID: Integer)."); goto op_call_builtin_error_cleanup; }
                    Value soundIDVal = actual_args[0];
                    if (!IS_INTEGER(soundIDVal)) {
                        runtimeError(vm, "VM: PlaySound SoundID must be an integer."); goto op_call_builtin_error_cleanup;
                    }
                    audioPlaySound((int)AS_INTEGER(soundIDVal));
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "audiofreesound") == 0) { // Match your renamed function if Pascal calls this
                    if (arg_count != 1) { runtimeError(vm, "VM: audioFreeSound expects 1 arg (SoundID: Integer)."); goto op_call_builtin_error_cleanup; }
                    Value soundIDVal = actual_args[0];
                    if (!IS_INTEGER(soundIDVal)) {
                        runtimeError(vm, "VM: audioFreeSound SoundID must be an integer."); goto op_call_builtin_error_cleanup;
                    }
                    audioFreeSound((int)AS_INTEGER(soundIDVal));
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "quitsoundsystem") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: QuitSoundSystem expects 0 arguments."); goto op_call_builtin_error_cleanup; }
                    audioQuitSystem();
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "quitrequested") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: QuitRequested expects 0 arguments."); goto op_call_builtin_error_cleanup; }
                    result_val = makeBoolean(break_requested != 0); // break_requested is global
                    is_function_that_succeeded = true;
                } else if (strcasecmp(builtin_name, "random") == 0) {
                    if (arg_count == 0) {
                        result_val = makeReal((double)rand() / ((double)RAND_MAX + 1.0));
                    } else if (arg_count == 1) {
                        Value arg = actual_args[0];
                        if (IS_INTEGER(arg)) {
                            long long n = AS_INTEGER(arg);
                            if (n <= 0) { runtimeError(vm, "VM: Random(N) N must be > 0."); goto op_call_builtin_error_cleanup;}
                            result_val = makeInt(rand() % n);
                        } else { runtimeError(vm, "VM: Random(N) N must be integer."); goto op_call_builtin_error_cleanup;}
                    } else { runtimeError(vm, "VM: Random expects 0 or 1 argument."); goto op_call_builtin_error_cleanup; }
                    is_function_that_succeeded = true;
                } else if (strcasecmp(builtin_name, "inttostr") == 0) {
                    if (arg_count != 1) { runtimeError(vm, "VM: IntToStr expects 1 arg."); goto op_call_builtin_error_cleanup; }
                    Value arg = actual_args[0];
                    long long val_to_convert;
                    // Allow Integer, Byte, Word, Boolean, Char for IntToStr
                    if (IS_INTEGER(arg) || arg.type == TYPE_BYTE || arg.type == TYPE_WORD || IS_BOOLEAN(arg)) val_to_convert = AS_INTEGER(arg);
                    else if (IS_CHAR(arg)) val_to_convert = (long long)AS_CHAR(arg);
                    else { runtimeError(vm, "VM: IntToStr expects Integer compatible arg. Got %s", varTypeToString(arg.type)); goto op_call_builtin_error_cleanup; }
                    char buffer[64]; // Sufficient for long long
                    snprintf(buffer, sizeof(buffer), "%lld", val_to_convert);
                    result_val = makeString(buffer);
                    is_function_that_succeeded = true;
                } else if (strcasecmp(builtin_name, "cleardevice") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: ClearDevice expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "VM: Graphics not initialized for ClearDevice."); goto op_call_builtin_error_cleanup; }
                    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255); // Assuming clear to black
                    SDL_RenderClear(gSdlRenderer);
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "setrgbcolor") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: SetRGBColor expects 3 args (R,G,B: Byte/Int)."); goto op_call_builtin_error_cleanup; }
                    Value rVal = actual_args[0], gVal = actual_args[1], bVal = actual_args[2];
                    if (! ( (IS_INTEGER(rVal) || rVal.type == TYPE_BYTE) &&
                            (IS_INTEGER(gVal) || gVal.type == TYPE_BYTE) &&
                            (IS_INTEGER(bVal) || bVal.type == TYPE_BYTE) ) ) {
                        runtimeError(vm, "VM: SetRGBColor args must be Byte/Integer."); goto op_call_builtin_error_cleanup;
                    }
                    gSdlCurrentColor.r = (Uint8)(AS_INTEGER(rVal) & 0xFF);
                    gSdlCurrentColor.g = (Uint8)(AS_INTEGER(gVal) & 0xFF);
                    gSdlCurrentColor.b = (Uint8)(AS_INTEGER(bVal) & 0xFF);
                    gSdlCurrentColor.a = 255;
                    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "fillrect") == 0) {
                    if (arg_count != 4) { runtimeError(vm, "VM: FillRect expects 4 args (x1,y1,x2,y2: Int)."); goto op_call_builtin_error_cleanup; }
                    Value x1Val = actual_args[0], y1Val = actual_args[1], x2Val = actual_args[2], y2Val = actual_args[3];
                    if (!IS_INTEGER(x1Val) || !IS_INTEGER(y1Val) || !IS_INTEGER(x2Val) || !IS_INTEGER(y2Val)) {
                        runtimeError(vm, "VM: FillRect args must be Integer."); goto op_call_builtin_error_cleanup;
                    }
                    SDL_Rect rect;
                    rect.x = (int)AS_INTEGER(x1Val);
                    rect.y = (int)AS_INTEGER(y1Val);
                    rect.w = (int)AS_INTEGER(x2Val) - rect.x + 1; // Assuming x2,y2 are inclusive bottom-right
                    rect.h = (int)AS_INTEGER(y2Val) - rect.y + 1;
                    if (rect.w < 0) { rect.x += rect.w -1; rect.w = -rect.w +2; } // Adjust if w/h is negative
                    if (rect.h < 0) { rect.y += rect.h -1; rect.h = -rect.h +2; }
                    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
                    SDL_RenderFillRect(gSdlRenderer, &rect);
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "outtextxy") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: OutTextXY expects 3 args (X,Y:Int; Text:Str)."); goto op_call_builtin_error_cleanup; }
                    Value xVal=actual_args[0], yVal=actual_args[1], textVal=actual_args[2];
                    if (!IS_INTEGER(xVal) || !IS_INTEGER(yVal) || !IS_STRING(textVal)) {
                        runtimeError(vm, "VM: OutTextXY arg type mismatch."); goto op_call_builtin_error_cleanup;
                    }
                    if (!gSdlTtfInitialized || !gSdlFont) { runtimeError(vm, "VM: Text system not ready for OutTextXY."); goto op_call_builtin_error_cleanup; }
                    const char* text_to_render = AS_STRING(textVal) ? AS_STRING(textVal) : "";
                    SDL_Surface* surf = TTF_RenderUTF8_Solid(gSdlFont, text_to_render, gSdlCurrentColor);
                    if (!surf) { runtimeError(vm, "VM: TTF_RenderUTF8_Solid failed: %s", TTF_GetError()); goto op_call_builtin_error_cleanup; }
                    SDL_Texture* tex = SDL_CreateTextureFromSurface(gSdlRenderer, surf);
                    if (!tex) { SDL_FreeSurface(surf); runtimeError(vm, "VM: CreateTextureFromSurface failed: %s", SDL_GetError()); goto op_call_builtin_error_cleanup; }
                    SDL_Rect dst = {(int)AS_INTEGER(xVal), (int)AS_INTEGER(yVal), surf->w, surf->h};
                    SDL_RenderCopy(gSdlRenderer, tex, NULL, &dst);
                    SDL_DestroyTexture(tex);
                    SDL_FreeSurface(surf);
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "updatescreen") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: UpdateScreen expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "VM: Graphics not initialized for UpdateScreen."); goto op_call_builtin_error_cleanup; }
                    SDL_RenderPresent(gSdlRenderer);
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "graphloop") == 0) {
                    if (arg_count != 1) { runtimeError(vm, "VM: GraphLoop expects 1 arg (ms:Int)."); goto op_call_builtin_error_cleanup; }
                    Value msVal = actual_args[0];
                    if (!IS_INTEGER(msVal)) { runtimeError(vm, "VM: GraphLoop arg must be Integer."); goto op_call_builtin_error_cleanup; }
                    long long ms = AS_INTEGER(msVal);
                    if (ms < 0) ms = 0;
                    if (gSdlInitialized && gSdlWindow && gSdlRenderer) {
                        Uint32 startT = SDL_GetTicks(); Uint32 endT = startT + (Uint32)ms; SDL_Event ev;
                        while(SDL_GetTicks() < endT) { while(SDL_PollEvent(&ev)) { if(ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q) ) { break_requested=1; goto graphloop_exit;}}}
                        graphloop_exit:;
                    }
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "waitkeyevent") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: WaitKeyEvent expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlInitialized || !gSdlWindow) {is_function_that_succeeded = false; break;} // Do nothing if no graphics
                    SDL_Event event; int waiting = 1; while(waiting){ if(SDL_WaitEvent(&event)){ if(event.type == SDL_QUIT || event.type == SDL_KEYDOWN) waiting=0;} else {waiting=0;}}
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "quittextsystem") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: QuitTextSystem expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (gSdlFont) { TTF_CloseFont(gSdlFont); gSdlFont = NULL; }
                    if (gSdlTtfInitialized) { TTF_Quit(); gSdlTtfInitialized = false; }
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "closegraph") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: CloseGraph expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
                    if (gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
                    // gSdlInitialized remains true, SDL_Quit() is for program exit.
                    is_function_that_succeeded = false;
                }
                // GetMouseState is tricky due to VAR parameters.
                // For now, a simple stub to avoid "not implemented" if compiler generates call.
                else if (strcasecmp(builtin_name, "getmousestate") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: GetMouseState expects 3 args."); goto op_call_builtin_error_cleanup; }
                    // This VM implementation currently cannot easily write back to VAR parameters
                    // passed by value. A more advanced mechanism is needed (e.g. passing pointers/indices).
                    // For now, just consume args and do nothing to prevent crash.
                    #ifdef DEBUG
                    fprintf(stderr, "VM Warning: GetMouseState called, but VM cannot modify VAR params with current OP_CALL_BUILTIN.\n");
                    #endif
                    is_function_that_succeeded = false;
                } else if (strcasecmp(builtin_name, "initgraph") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: InitGraph expects 3 args (Width, Height, Title)."); goto op_call_builtin_error_cleanup; }
                    Value widthVal = actual_args[0];
                    Value heightVal = actual_args[1];
                    Value titleVal = actual_args[2];

                    if (!IS_INTEGER(widthVal) || !IS_INTEGER(heightVal) || !IS_STRING(titleVal)) {
                        runtimeError(vm, "VM: InitGraph argument type mismatch. Expected (Int, Int, String).");
                        goto op_call_builtin_error_cleanup;
                    }

                    int w = (int)AS_INTEGER(widthVal);
                    int h = (int)AS_INTEGER(heightVal);
                    const char* title = AS_STRING(titleVal) ? AS_STRING(titleVal) : "Pscal VM Graphics";

                    if (w <= 0 || h <= 0) {
                        runtimeError(vm, "VM: InitGraph width and height must be positive.");
                        goto op_call_builtin_error_cleanup;
                    }
                    
                    if (!gSdlInitialized) {
                        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) < 0) {
                            runtimeError(vm, "VM: SDL_Init failed in InitGraph: %s", SDL_GetError());
                            goto op_call_builtin_error_cleanup;
                        }
                        gSdlInitialized = true;
                    }
                    if (gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
                    if (gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }

                    gSdlWindow = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_SHOWN);
                    if (!gSdlWindow) {
                        runtimeError(vm, "VM: SDL_CreateWindow failed: %s", SDL_GetError());
                        goto op_call_builtin_error_cleanup;
                    }
                    gSdlWidth = w; gSdlHeight = h;
                    gSdlRenderer = SDL_CreateRenderer(gSdlWindow, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
                    if (!gSdlRenderer) {
                        SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL;
                        runtimeError(vm, "VM: SDL_CreateRenderer failed: %s", SDL_GetError());
                        goto op_call_builtin_error_cleanup;
                    }
                    InitializeTextureSystem(); // From sdl.c/sdl.h
                    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255);
                    SDL_RenderClear(gSdlRenderer);
                    SDL_RenderPresent(gSdlRenderer);
                    gSdlCurrentColor = (SDL_Color){255, 255, 255, 255}; // Default draw white

                    is_function_that_succeeded = false; // InitGraph is a procedure
                
                } else if (strcasecmp(builtin_name, "quitrequested") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: QuitRequested expects 0 arguments."); goto op_call_builtin_error_cleanup; }
                    result_val = makeBoolean(break_requested != 0);
                    is_function_that_succeeded = true;
                
                } else if (strcasecmp(builtin_name, "getmousestate") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: GetMouseState expects 3 arguments (X, Y, Buttons)."); goto op_call_builtin_error_cleanup; }
                    // Args actual_args[0], actual_args[1], actual_args[2] are the *names* of the
                    // global variables X, Y, Buttons, passed as string constants by the compiler.
                    // This is a deviation from the typical "value" passing of OP_CALL_BUILTIN.
                    // The compiler needs to be adjusted to push string constants for these var names.
                    // For now, this implementation assumes this convention.

                    Value xNameVal = actual_args[0];
                    Value yNameVal = actual_args[1];
                    Value buttonsNameVal = actual_args[2];

                    if (!IS_STRING(xNameVal) || !IS_STRING(yNameVal) || !IS_STRING(buttonsNameVal)) {
                        runtimeError(vm, "VM: GetMouseState expects string variable names for VAR parameters.");
                        goto op_call_builtin_error_cleanup;
                    }

                    int mse_x, mse_y;
                    Uint32 sdl_buttons_state = SDL_GetMouseState(&mse_x, &mse_y);
                    int pscal_buttons = 0;
                    if (sdl_buttons_state & SDL_BUTTON_LMASK) pscal_buttons |= 1;
                    if (sdl_buttons_state & SDL_BUTTON_MMASK) pscal_buttons |= 2;
                    if (sdl_buttons_state & SDL_BUTTON_RMASK) pscal_buttons |= 4;

                    Symbol* symX = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(xNameVal));
                    Symbol* symY = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(yNameVal));
                    Symbol* symButtons = hashTableLookup(vm->vmGlobalSymbols, AS_STRING(buttonsNameVal));

                    if (!symX || !symY || !symButtons) {
                        runtimeError(vm, "VM: One or more VAR parameters for GetMouseState not found in global symbols.");
                        goto op_call_builtin_error_cleanup;
                    }
                    // Type checking for symbols can be added here if desired.
                    // Assuming they are INTEGER.

                    freeValue(symX->value); *(symX->value) = makeInt(mse_x);
                    freeValue(symY->value); *(symY->value) = makeInt(mse_y);
                    freeValue(symButtons->value); *(symButtons->value) = makeInt(pscal_buttons);
                    
                    is_function_that_succeeded = false; // Procedure
                
                } else if (strcasecmp(builtin_name, "cleardevice") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: ClearDevice expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "VM: Graphics not initialized for ClearDevice."); goto op_call_builtin_error_cleanup; }
                    SDL_SetRenderDrawColor(gSdlRenderer, 0, 0, 0, 255); // Clear to black
                    SDL_RenderClear(gSdlRenderer);
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "setrgbcolor") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: SetRGBColor expects 3 args (R,G,B: Byte/Int)."); goto op_call_builtin_error_cleanup; }
                    Value rVal = actual_args[0], gVal = actual_args[1], bVal = actual_args[2];
                    if (! ( (IS_INTEGER(rVal) || rVal.type == TYPE_BYTE) &&
                            (IS_INTEGER(gVal) || gVal.type == TYPE_BYTE) &&
                            (IS_INTEGER(bVal) || bVal.type == TYPE_BYTE) ) ) {
                        runtimeError(vm, "VM: SetRGBColor args must be Byte/Integer."); goto op_call_builtin_error_cleanup;
                    }
                    gSdlCurrentColor.r = (Uint8)(AS_INTEGER(rVal) & 0xFF);
                    gSdlCurrentColor.g = (Uint8)(AS_INTEGER(gVal) & 0xFF);
                    gSdlCurrentColor.b = (Uint8)(AS_INTEGER(bVal) & 0xFF);
                    gSdlCurrentColor.a = 255;
                    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "fillrect") == 0) {
                    if (arg_count != 4) { runtimeError(vm, "VM: FillRect expects 4 args (x1,y1,x2,y2: Int)."); goto op_call_builtin_error_cleanup; }
                    Value x1Val = actual_args[0], y1Val = actual_args[1], x2Val = actual_args[2], y2Val = actual_args[3];
                    if (!IS_INTEGER(x1Val) || !IS_INTEGER(y1Val) || !IS_INTEGER(x2Val) || !IS_INTEGER(y2Val)) {
                        runtimeError(vm, "VM: FillRect args must be Integer."); goto op_call_builtin_error_cleanup;
                    }
                    SDL_Rect rect;
                    rect.x = (int)AS_INTEGER(x1Val);
                    rect.y = (int)AS_INTEGER(y1Val);
                    // Pascal's FillRect often takes (x1,y1,x2,y2) as corners.
                    // SDL_RenderFillRect takes (x,y,w,h).
                    int x1 = rect.x;
                    int y1 = rect.y;
                    int x2 = (int)AS_INTEGER(x2Val);
                    int y2 = (int)AS_INTEGER(y2Val);
                    rect.x = (x1 < x2) ? x1 : x2;
                    rect.y = (y1 < y2) ? y1 : y2;
                    rect.w = abs(x2 - x1) + 1;
                    rect.h = abs(y2 - y1) + 1;

                    SDL_SetRenderDrawColor(gSdlRenderer, gSdlCurrentColor.r, gSdlCurrentColor.g, gSdlCurrentColor.b, gSdlCurrentColor.a);
                    SDL_RenderFillRect(gSdlRenderer, &rect);
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "outtextxy") == 0) {
                    if (arg_count != 3) { runtimeError(vm, "VM: OutTextXY expects 3 args (X,Y:Int; Text:Str)."); goto op_call_builtin_error_cleanup; }
                    Value xVal=actual_args[0], yVal=actual_args[1], textVal=actual_args[2];
                    if (!IS_INTEGER(xVal) || !IS_INTEGER(yVal) || !IS_STRING(textVal)) {
                        runtimeError(vm, "VM: OutTextXY arg type mismatch."); goto op_call_builtin_error_cleanup;
                    }
                    if (!gSdlTtfInitialized || !gSdlFont) { runtimeError(vm, "VM: Text system not ready for OutTextXY."); goto op_call_builtin_error_cleanup; }
                    
                    const char* text_to_render = AS_STRING(textVal) ? AS_STRING(textVal) : "";
                    SDL_Surface* surf = TTF_RenderUTF8_Solid(gSdlFont, text_to_render, gSdlCurrentColor); // Uses global gSdlCurrentColor
                    if (!surf) { runtimeError(vm, "VM: TTF_RenderUTF8_Solid failed: %s", TTF_GetError()); goto op_call_builtin_error_cleanup; }
                    SDL_Texture* tex = SDL_CreateTextureFromSurface(gSdlRenderer, surf);
                    if (!tex) { SDL_FreeSurface(surf); runtimeError(vm, "VM: CreateTextureFromSurface failed: %s", SDL_GetError()); goto op_call_builtin_error_cleanup; }
                    SDL_Rect dstRect = {(int)AS_INTEGER(xVal), (int)AS_INTEGER(yVal), surf->w, surf->h};
                    SDL_RenderCopy(gSdlRenderer, tex, NULL, &dstRect);
                    SDL_DestroyTexture(tex);
                    SDL_FreeSurface(surf);
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "updatescreen") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: UpdateScreen expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlInitialized || !gSdlRenderer) { runtimeError(vm, "VM: Graphics not initialized for UpdateScreen."); goto op_call_builtin_error_cleanup; }
                    SDL_Event event; while (SDL_PollEvent(&event)) { if (event.type == SDL_QUIT) break_requested = 1; } // Basic event pump
                    SDL_RenderPresent(gSdlRenderer);
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "graphloop") == 0) {
                    if (arg_count != 1) { runtimeError(vm, "VM: GraphLoop expects 1 arg (ms:Int)."); goto op_call_builtin_error_cleanup; }
                    Value msVal = actual_args[0];
                    if (!IS_INTEGER(msVal)) { runtimeError(vm, "VM: GraphLoop arg must be Integer."); goto op_call_builtin_error_cleanup; }
                    long long ms = AS_INTEGER(msVal);
                    if (ms < 0) ms = 0;
                    if (gSdlInitialized && gSdlWindow && gSdlRenderer) {
                        Uint32 startT = SDL_GetTicks(); Uint32 endT = startT + (Uint32)ms; SDL_Event ev;
                        while(SDL_GetTicks() < endT && !break_requested) {
                            while(SDL_PollEvent(&ev)) {
                                if(ev.type == SDL_QUIT || (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_q) ) {
                                    break_requested=1;
                                    break;
                                }
                            }
                            if (break_requested) break;
                            SDL_Delay(1); // Prevent 100% CPU
                        }
                    }
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "waitkeyevent") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: WaitKeyEvent expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (!gSdlInitialized || !gSdlWindow) {is_function_that_succeeded = false; break;}
                    SDL_Event event; int waiting = 1;
                    while(waiting){
                        if(SDL_WaitEvent(&event)){
                            if(event.type == SDL_QUIT || event.type == SDL_KEYDOWN || event.type == SDL_MOUSEBUTTONDOWN) waiting=0;
                        } else { waiting=0; /* Error in WaitEvent */ }
                    }
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "quittextsystem") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: QuitTextSystem expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (gSdlFont) { TTF_CloseFont(gSdlFont); gSdlFont = NULL; }
                    if (gSdlTtfInitialized) { TTF_Quit(); gSdlTtfInitialized = false; }
                    is_function_that_succeeded = false;

                } else if (strcasecmp(builtin_name, "closegraph") == 0) {
                    if (arg_count != 0) { runtimeError(vm, "VM: CloseGraph expects 0 args."); goto op_call_builtin_error_cleanup; }
                    if (gSdlRenderer) { SDL_DestroyRenderer(gSdlRenderer); gSdlRenderer = NULL; }
                    if (gSdlWindow) { SDL_DestroyWindow(gSdlWindow); gSdlWindow = NULL; }
                    is_function_that_succeeded = false;
                } else { // Fallback for other Pong built-ins not yet added above
                    runtimeError(vm, "VM Error: Built-in function/procedure '%s' still needs full VM implementation.", builtin_name);
                    goto op_call_builtin_error_cleanup;
                }
                // --- Add more built-in handlers here ---

                if (is_function_that_succeeded) {
                    push(vm, result_val);
                } else {
                    freeValue(&result_val);
                }

            // Common cleanup for actual_args (always execute this path before successful break)
            // op_call_builtin_success_cleanup: // <<< REMOVE THIS UNUSED LABEL
                for (int i = 0; i < arg_count; i++) {
                    freeValue(&actual_args[i]);
                }
                break; // Break from OP_CALL_BUILTIN switch case on success

            op_call_builtin_error_cleanup:
                for (int i = 0; i < arg_count; i++) {
                    freeValue(&actual_args[i]);
                }
                return INTERPRET_RUNTIME_ERROR;

            } // End OP_CALL_BUILTIN
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
            case OP_CALL: { // EXECUTION logic for OP_CALL
                  if (vm->frameCount >= VM_CALL_STACK_MAX) {
                      runtimeError(vm, "VM Error: Call stack overflow.");
                      return INTERPRET_RUNTIME_ERROR;
                  }

                  uint16_t target_address = READ_SHORT(vm); // Read 2-byte address from bytecode stream
                  uint8_t declared_arity = READ_BYTE();   // Read 1-byte declared arity from bytecode stream

                  #ifdef DEBUG
                  if (dumpExec) {
                      // Note: For execution, we don't need to find/print the name here.
                      // The disassembler trace (called above) would have printed the name.
                      fprintf(stderr, "VM EXEC: OP_CALL to address %04X (declared_arity: %d), current args on stack: %ld\n",
                              target_address, declared_arity, (long)(vm->stackTop - vm->stack));
                  }
                  #endif
                  
                  // Check if enough arguments are actually on the stack (pushed by caller)
                  if (vm->stackTop - vm->stack < declared_arity) {
                      runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %d, have %ld.",
                                   declared_arity, (long)(vm->stackTop - vm->stack));
                      return INTERPRET_RUNTIME_ERROR;
                  }

                  CallFrame* frame = &vm->frames[vm->frameCount++];
                  frame->return_address = vm->ip; // Current vm->ip is *after* OP_CALL's operands
                  frame->slots = vm->stackTop - declared_arity; // Arguments are already on stack; new frame starts there

                  // Lookup symbol to get locals_count for stack setup
                  Symbol* proc_symbol = NULL;
                  if(vm->procedureTable) { // vm->procedureTable was set at the start of interpretBytecode
                      // This search can be slow. In a more optimized VM, locals_count might be an
                      // operand to OP_CALL or stored at the function's entry point in bytecode.
                      for (int i = 0; i < HASHTABLE_SIZE; i++) {
                          Symbol* s = vm->procedureTable->buckets[i];
                          while (s) {
                              if (s->is_defined && s->bytecode_address == target_address) {
                                  proc_symbol = s;
                                  break;
                              }
                              s = s->next;
                          }
                          if (proc_symbol) break;
                      }
                  }
                  if (!proc_symbol) {
                      runtimeError(vm, "VM Error: Could not retrieve procedure symbol for called address %04X.", target_address);
                      vm->frameCount--; // Revert frame increment
                      return INTERPRET_RUNTIME_ERROR;
                  }
                  frame->locals_count = proc_symbol->locals_count;

                  // Reserve space on stack for local variables by pushing nils
                  for (int i = 0; i < frame->locals_count; i++) {
                      push(vm, makeNil());
                  }
                  
                  vm->ip = vm->chunk->code + target_address; // Jump to the function/procedure
                  break;
              }


            default:
                runtimeError(vm, "VM Error: Unknown opcode %d.", instruction_val);
                return INTERPRET_RUNTIME_ERROR;
        }
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
