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
        if ((IS_INTEGER(a_val_popped) || IS_REAL(a_val_popped)) && (IS_INTEGER(b_val_popped) || IS_REAL(b_val_popped))) { \
            if (IS_REAL(a_val_popped) || IS_REAL(b_val_popped)) { \
                double fa = IS_REAL(a_val_popped) ? AS_REAL(a_val_popped) : (double)AS_INTEGER(a_val_popped); \
                double fb = IS_REAL(b_val_popped) ? AS_REAL(b_val_popped) : (double)AS_INTEGER(b_val_popped); \
                if (current_instruction_code == OP_DIVIDE && fb == 0.0) { \
                    runtimeError(vm, "Runtime Error: Division by zero."); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    return INTERPRET_RUNTIME_ERROR; \
                } \
                switch(current_instruction_code) { \
                    case OP_ADD: result_val = makeReal(fa + fb); break; \
                    case OP_SUBTRACT: result_val = makeReal(fa - fb); break; \
                    case OP_MULTIPLY: result_val = makeReal(fa * fb); break; \
                    case OP_DIVIDE: result_val = makeReal(fa / fb); break; \
                    default: result_val = makeNil(); break; \
                } \
            } else { /* Both are integers */ \
                long long ia = AS_INTEGER(a_val_popped); \
                long long ib = AS_INTEGER(b_val_popped); \
                if (current_instruction_code == OP_DIVIDE && ib == 0) { \
                    runtimeError(vm, "Runtime Error: Division by zero (integer)."); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    return INTERPRET_RUNTIME_ERROR; \
                } \
                switch(current_instruction_code) { \
                    case OP_ADD: result_val = makeInt(ia + ib); break; \
                    case OP_SUBTRACT: result_val = makeInt(ia - ib); break; \
                    case OP_MULTIPLY: result_val = makeInt(ia * ib); break; \
                    case OP_DIVIDE: result_val = makeReal((double)ia / (double)ib); break; \
                    default: result_val = makeNil(); break; \
                } \
            } \
        } else { \
            runtimeError(vm, "Runtime Error: Operands must be numbers for arithmetic operation '%s'.", op_char_for_error_msg); \
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
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) { /* error */ return INTERPRET_RUNTIME_ERROR; }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym) {
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", varNameVal.s_val);
                    Value temp_popped_val = pop(vm); freeValue(&temp_popped_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if(!sym->value) { /* Malloc error */ Value temp_popped_val = pop(vm); freeValue(&temp_popped_val); return INTERPRET_RUNTIME_ERROR; }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def);
                }
                Value value_to_set_from_stack = peek(vm, 0);
                freeValue(sym->value);
                *(sym->value) = makeCopyOfValue(&value_to_set_from_stack);
                Value popped_val = pop(vm);
                freeValue(&popped_val);
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
