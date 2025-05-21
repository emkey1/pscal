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

// --- VM Initialization and Cleanup ---
void initVM(VM* vm) {
    if (!vm) return;
    resetStack(vm);
    vm->chunk = NULL;
    vm->ip = NULL;
    vm->vmGlobalSymbols = createHashTable();
    if (!vm->vmGlobalSymbols) {
        fprintf(stderr, "VM Error: Failed to create VM's global symbol table.\n");
        EXIT_FAILURE_HANDLER();
    }
}

void freeVM(VM* vm) {
    if (!vm) return;
    if (vm->vmGlobalSymbols) {
        freeHashTable(vm->vmGlobalSymbols);
        vm->vmGlobalSymbols = NULL;
    }
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
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk) {
    if (!vm || !chunk) return INTERPRET_RUNTIME_ERROR;

    vm->chunk = chunk;
    vm->ip = vm->chunk->code;

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
            disassembleInstruction(vm->chunk, (int)(vm->ip - vm->chunk->code));
        }
        #endif

        instruction_val = READ_BYTE(); // Uses the macro which uses vm->ip from runVM
        switch (instruction_val) {
            case OP_RETURN: {
                return INTERPRET_OK;
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
            case OP_EQUAL:
            case OP_NOT_EQUAL:
            case OP_GREATER:
            case OP_GREATER_EQUAL:
            case OP_LESS:
            case OP_LESS_EQUAL: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;
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
                        default:
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
                    // Check if enum_name members are comparable (both NULL or both non-NULL and equal)
                    if (a_val.enum_val.enum_name == NULL && b_val.enum_val.enum_name == NULL) {
                        types_match = true; // Both are anonymous enums, compare ordinals
                    } else if (a_val.enum_val.enum_name != NULL && b_val.enum_val.enum_name != NULL &&
                               strcmp(a_val.enum_val.enum_name, b_val.enum_val.enum_name) == 0) {
                        types_match = true; // Names match
                    }

                    if (instruction_val == OP_EQUAL) {
                        result_val = makeBoolean(types_match && (a_val.enum_val.ordinal == b_val.enum_val.ordinal));
                    } else if (instruction_val == OP_NOT_EQUAL) {
                        result_val = makeBoolean(!types_match || (a_val.enum_val.ordinal != b_val.enum_val.ordinal));
                    } else { // For >, >=, <, <=, types MUST match
                        if (!types_match) {
                            runtimeError(vm, "Runtime Error: Cannot compare different ENUM types ('%s' vs '%s') with opcode %d.",
                                         a_val.enum_val.enum_name ? a_val.enum_val.enum_name : "<anon>",
                                         b_val.enum_val.enum_name ? b_val.enum_val.enum_name : "<anon>",
                                         instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                        // Types match, compare ordinals
                        int ord_a = a_val.enum_val.ordinal;
                        int ord_b = b_val.enum_val.ordinal;
                        switch (instruction_val) {
                            case OP_GREATER:       result_val = makeBoolean(ord_a > ord_b);  break;
                            case OP_GREATER_EQUAL: result_val = makeBoolean(ord_a >= ord_b); break;
                            case OP_LESS:          result_val = makeBoolean(ord_a < ord_b);  break;
                            case OP_LESS_EQUAL:    result_val = makeBoolean(ord_a <= ord_b); break;
                            default: // Should not happen
                                runtimeError(vm, "VM Error: Unexpected enum comparison opcode %d.", instruction_val);
                                freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    comparison_succeeded = true;
                }

                if (comparison_succeeded) {
                    push(vm, result_val);
                } else {
                    runtimeError(vm, "Runtime Error: Operands must be comparable for opcode %d. Got %s and %s.", instruction_val, varTypeToString(a_val.type), varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            } // End block for comparison opcodes            i
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
                     return INTERPRET_RUNTIME_ERROR; // Exit on error
                 }
                 Value builtinNameVal = vm->chunk->constants[name_const_idx];

                 if (builtinNameVal.type != TYPE_STRING || !builtinNameVal.s_val) {
                     runtimeError(vm, "VM Error: Invalid built-in name constant for OP_CALL_BUILTIN (not a string).");
                     return INTERPRET_RUNTIME_ERROR; // Exit on error
                 }
                 const char* builtin_name = builtinNameVal.s_val;

                 Value actual_args[256];
                 if (vm->stackTop - vm->stack < arg_count) {
                     runtimeError(vm, "VM Error: Stack underflow preparing arguments for built-in %s. Expected %d, have %ld.",
                                  builtin_name, arg_count, (long)(vm->stackTop - vm->stack));
                     return INTERPRET_RUNTIME_ERROR; // Exit on error
                 }

                 for (int i = 0; i < arg_count; i++) {
                     actual_args[arg_count - 1 - i] = pop(vm);
                 }

                 Value result_val = makeNil();
                 bool is_function_that_succeeded = false;

                 // --- Dispatch to C implementation for the built-in ---
                 if (strcasecmp(builtin_name, "abs") == 0) {
                     if (arg_count != 1) { runtimeError(vm, "VM: abs expects 1 arg."); goto op_call_builtin_error_cleanup; }
                     Value arg = actual_args[0];
                     if (IS_INTEGER(arg)) result_val = makeInt(llabs(AS_INTEGER(arg)));
                     else if (IS_REAL(arg)) result_val = makeReal(fabs(AS_REAL(arg)));
                     else { runtimeError(vm, "VM: abs arg must be number."); goto op_call_builtin_error_cleanup; }
                     is_function_that_succeeded = true;
                 } else if (strcasecmp(builtin_name, "length") == 0) {
                     if (arg_count != 1) { runtimeError(vm, "VM: length expects 1 arg."); goto op_call_builtin_error_cleanup; }
                     Value arg = actual_args[0];
                     if (!IS_STRING(arg)) { runtimeError(vm, "VM: length arg must be string."); goto op_call_builtin_error_cleanup; }
                     result_val = makeInt(AS_STRING(arg) ? strlen(AS_STRING(arg)) : 0);
                     is_function_that_succeeded = true;
                 } else if (strcasecmp(builtin_name, "ord") == 0) {
                     if (arg_count != 1) { runtimeError(vm, "VM: ord expects 1 arg."); goto op_call_builtin_error_cleanup; }
                     Value arg = actual_args[0];
                     if (IS_CHAR(arg)) {
                         result_val = makeInt((long long)AS_CHAR(arg));
                     } else if (IS_BOOLEAN(arg)) {
                         result_val = makeInt(AS_BOOLEAN(arg) ? 1 : 0);
                     } else if (IS_STRING(arg) && AS_STRING(arg) != NULL && strlen(AS_STRING(arg)) == 1) { // <<< ADDED THIS
                         result_val = makeInt((long long)AS_STRING(arg)[0]);
                     } else if (IS_INTEGER(arg)) { // Pascal's Ord(Integer) is often the integer itself
                         result_val = makeInt(AS_INTEGER(arg));
                     }
                     // Add TYPE_BYTE, TYPE_WORD if Ord should support them directly
                     // else if (arg.type == TYPE_ENUM) {
                     //    result_val = makeInt((long long)arg.enum_val.ordinal);
                     // }
                     else {
                         runtimeError(vm, "VM: ord expects char, boolean, single-char string, or integer. Got %s", varTypeToString(arg.type));
                         goto op_call_builtin_error_cleanup;
                     }
                     is_function_that_succeeded = true;
                 }  else if (strcasecmp(builtin_name, "chr") == 0) {
                     if (arg_count != 1) { runtimeError(vm, "VM: chr expects 1 arg."); goto op_call_builtin_error_cleanup; }
                     Value arg = actual_args[0];
                     if (!IS_INTEGER(arg)) { runtimeError(vm, "VM: chr expects integer arg."); goto op_call_builtin_error_cleanup; }
                     result_val = makeChar((char)AS_INTEGER(arg)); // makeChar returns TYPE_CHAR
                     is_function_that_succeeded = true;
                 }
                 // --- Add more built-in handlers here ---
                 else {
                     runtimeError(vm, "VM Error: Built-in function/procedure '%s' not yet implemented in VM.", builtin_name);
                     goto op_call_builtin_error_cleanup;
                 }

                 // If it was a function and execution was successful, push its result
                 if (is_function_that_succeeded) {
                     push(vm, result_val);
                 } else {
                     // If it was a procedure (is_function_that_succeeded is false)
                     // or an error occurred before result_val was properly set for a function.
                     // The default makeNil() doesn't need freeing for its contents.
                     // If result_val was modified from makeNil for a procedure that later errored,
                     // freeValue here would be important. But procedures usually don't set result_val.
                     freeValue(&result_val); // Safe to call on makeNil()
                 }

             // Common cleanup for actual_args (always execute this path before successful break)
             op_call_builtin_success_cleanup:
                 for (int i = 0; i < arg_count; i++) {
                     freeValue(&actual_args[i]);
                 }
                 break; // Break from OP_CALL_BUILTIN switch case on success

             op_call_builtin_error_cleanup: // Jump here if runtimeError was called within built-in logic
                 for (int i = 0; i < arg_count; i++) {
                     freeValue(&actual_args[i]); // Free any popped arguments
                 }
                 // Note: result_val might be uninitialized or a default makeNil if error happened early.
                 // It's not pushed on error. If it was partially formed with heap data, freeValue would be needed.
                 // For simplicity, assume built-in logic doesn't half-allocate result_val on error.
                 // freeValue(&result_val); // Free the default makeNil() or partially formed result.
                 return INTERPRET_RUNTIME_ERROR; // Propagate error to stop VM

             } // End OP_CALL_BUILTIN
            case OP_HALT:
                return INTERPRET_OK;

            default:
                runtimeError(vm, "VM Error: Unknown opcode %d.", instruction_val);
                return INTERPRET_RUNTIME_ERROR;
        }
    }
    return INTERPRET_OK;
}

#ifdef DEBUG
static void dumpSymbolValue(Value* v) {
    if (!v) { printf("NULL_Value_Ptr"); return; }
    printValueToStream(*v, stderr);
}
#endif
