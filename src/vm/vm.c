// src/vm/vm.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "vm.h"
#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"
#include "symbol/symbol.h"     // For HashTable, createHashTable, hashTableLookup, hashTableInsert
#include "globals.h"
// #include "backend_ast/builtin.h" // Only if VM directly calls executeBuiltinProcedure
                                   // For WriteLn, we'll do a direct printf for now.
#include "backend_ast/interpreter.h" // <<< ADDED for makeCopyOfValue

// --- Forward Declarations for VM-specific Symbol Table Helpers (stubs) ---
#ifdef DEBUG // Only if dumpSymbolValue is used by debug prints in this file
static void dumpSymbolValue(Value* v); // Assuming this is in utils.c or here for debug
#endif

// --- VM Helper Functions ---

static void resetStack(VM* vm) {
    vm->stackTop = vm->stack;
}

static void runtimeError(VM* vm, const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);

    // To get line number:
    if (vm && vm->chunk && vm->ip && vm->chunk->code && vm->chunk->lines) {
        // vm->ip points to the *next* instruction. The error occurred at the previous one.
        // Ensure vm->ip > vm->chunk->code to prevent underflow if error is at first byte.
        if (vm->ip > vm->chunk->code) {
            size_t instruction_offset = (vm->ip - vm->chunk->code) - 1;
            if (instruction_offset < (size_t)vm->chunk->count) { // Check bounds
                 int line = vm->chunk->lines[instruction_offset];
                 fprintf(stderr, "[line %d] in script (approx.)\n", line);
            }
        }
    }
    resetStack(vm);
}

static void push(VM* vm, Value value) {
    if (vm->stackTop - vm->stack >= VM_STACK_MAX) {
        runtimeError(vm, "VM Error: Stack overflow.");
        // Consider how to propagate this error to stop interpretBytecode
        return;
    }
    *vm->stackTop = value;
    vm->stackTop++;
}

static Value pop(VM* vm) {
    if (vm->stackTop == vm->stack) {
        runtimeError(vm, "VM Error: Stack underflow (pop from empty stack).");
        return makeNil();
    }
    vm->stackTop--;
    // Before returning, if the popped value is a string, record, or array that was deep copied onto stack,
    // its content is now owned by the caller of pop().
    // If we are not careful, this can lead to memory leaks if the caller doesn't freeValue().
    // For now, we return a direct copy of the struct.
    return *vm->stackTop;
}

static Value peek(VM* vm, int distance) {
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

// Forward declaration if this function is defined after its first use in OP_DEFINE_GLOBAL
static Symbol* createSymbolForVM(const char* name, VarType type, AST* type_def_for_value_init);

// --- Main Interpretation Loop ---
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk) {
    if (!vm || !chunk) return INTERPRET_RUNTIME_ERROR;

    vm->chunk = chunk;
    vm->ip = vm->chunk->code;
    // resetStack(vm); // initVM already does this. If interpretBytecode can be re-entered, then yes.

    #define READ_BYTE() (*vm->ip++)
    #define READ_CONSTANT() (vm->chunk->constants[READ_BYTE()])
    
    #define BINARY_OP(value_type, op, op_name_str) \
        do { \
            Value b_val_popped = pop(vm); \
            Value a_val_popped = pop(vm); \
            Value result_val; \
            if (a_val_popped.type == TYPE_INTEGER && b_val_popped.type == TYPE_INTEGER) { \
                result_val = makeInt(a_val_popped.i_val op b_val_popped.i_val); \
            } else if ( (a_val_popped.type == TYPE_REAL || a_val_popped.type == TYPE_INTEGER) && \
                        (b_val_popped.type == TYPE_REAL || b_val_popped.type == TYPE_INTEGER) ) { \
                double fa = (a_val_popped.type == TYPE_REAL) ? a_val_popped.r_val : (double)a_val_popped.i_val; \
                double fb = (b_val_popped.type == TYPE_REAL) ? b_val_popped.r_val : (double)b_val_popped.i_val; \
                if (strcmp(op_name_str, "/") == 0 && fb == 0.0) { \
                    runtimeError(vm, "Runtime Error: Division by zero."); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    return INTERPRET_RUNTIME_ERROR; \
                } \
                result_val = makeReal(fa op fb); \
            } else { \
                runtimeError(vm, "Runtime Error: Operands must be numbers for " op_name_str "."); \
                freeValue(&a_val_popped); freeValue(&b_val_popped); \
                return INTERPRET_RUNTIME_ERROR; \
            } \
            push(vm, result_val); /* result_val is a new Value, push copies its struct */ \
            freeValue(&a_val_popped); /* Free contents of popped values */ \
            freeValue(&b_val_popped); \
        } while (false)


    for (;;) {
        uint8_t instruction;
        #ifdef DEBUG
        if (dumpExec) {
            fprintf(stderr,"VM Stack: ");
            for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
                fprintf(stderr,"[ ");
                dumpSymbolValue(slot);
                fprintf(stderr," ]");
            }
            fprintf(stderr,"\n");
            disassembleInstruction(vm->chunk, (int)(vm->ip - vm->chunk->code));
        }
        #endif

        instruction = READ_BYTE();
        switch (instruction) {
            case OP_RETURN: {
                return INTERPRET_OK;
            }
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                // makeCopyOfValue is crucial if constant is string/record/array
                // to ensure stack has its own copy of dynamic data.
                push(vm, makeCopyOfValue(&constant));
                break;
            }
            case OP_ADD:      BINARY_OP(Value, +, "+"); break;
            case OP_SUBTRACT: BINARY_OP(Value, -, "-"); break;
            case OP_MULTIPLY: BINARY_OP(Value, *, "*"); break;
            case OP_DIVIDE:   BINARY_OP(Value, /, "/"); break;
            
            case OP_NEGATE: {
                Value val_popped = pop(vm);
                Value result_val;
                if (val_popped.type == TYPE_INTEGER) result_val = makeInt(-val_popped.i_val);
                else if (val_popped.type == TYPE_REAL) result_val = makeReal(-val_popped.r_val);
                else {
                    runtimeError(vm, "Runtime Error: Operand for negate must be a number.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result_val);
                freeValue(&val_popped);
                break;
            }
            case OP_NOT: { // Assuming boolean is stored in i_val (0 for false, 1 for true)
                Value val_popped = pop(vm);
                if (val_popped.type != TYPE_BOOLEAN && val_popped.type != TYPE_INTEGER) { // Allow NOT on integer 0/1
                    runtimeError(vm, "Runtime Error: Operand for NOT must be boolean or integer.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeBoolean(val_popped.i_val == 0));
                freeValue(&val_popped);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                 Value varNameVal = READ_CONSTANT(); // Operand 1: name string index
                 VarType declaredType = (VarType)READ_BYTE(); // Operand 2: VarType enum value
                 
                 if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
                     runtimeError(vm, "Runtime Error: Global variable name not a string for OP_DEFINE_GLOBAL.");
                     return INTERPRET_RUNTIME_ERROR;
                 }

                 Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                 if (sym == NULL) {
                     // Create the symbol. The type_def AST node isn't directly available from bytecode for basic types.
                     // We pass NULL for type_def_for_value_init if only basic types are handled by makeValueForType without it.
                     // If makeValueForType needs an AST for structured types, the bytecode/compiler would need to provide that info.
                     sym = createSymbolForVM(varNameVal.s_val, declaredType, NULL);
                                         // For simple types from SimpleMath.p, NULL for type_def_for_value_init is fine.
                                         // When you have records/arrays, OP_DEFINE_GLOBAL might need to carry
                                         // an index to a serialized type definition that the VM can use.

                     if (!sym) {
                         // createSymbolForVM would have printed an error
                         runtimeError(vm, "Runtime Error: Could not create symbol structure for global '%s' in VM.", varNameVal.s_val);
                         return INTERPRET_RUNTIME_ERROR;
                     }
                     hashTableInsert(vm->vmGlobalSymbols, sym);
                     #ifdef DEBUG
                         if(dumpExec) fprintf(stderr, "VM: Defined global '%s' type %s\n", varNameVal.s_val, varTypeToString(declaredType));
                     #endif
                 } else {
                     // Symbol already defined. Check type consistency or warn.
                     if (sym->type != declaredType) {
                          runtimeError(vm, "VM Runtime Warning: Global '%s' re-defined or already exists with different type (%s vs %s).",
                                       varNameVal.s_val, varTypeToString(sym->type), varTypeToString(declaredType));
                     }
                 }
                 break;
             }
            case OP_GET_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) { /* error handling */ return INTERPRET_RUNTIME_ERROR; }
                
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", varNameVal.s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeCopyOfValue(sym->value)); // Push a deep copy of the global's value
                break;
            }
            case OP_SET_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) { /* error handling */ return INTERPRET_RUNTIME_ERROR; }
                
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym) {
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", varNameVal.s_val);
                    // Value on stack still needs to be popped and freed.
                    Value temp_popped_val = pop(vm); freeValue(&temp_popped_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (sym->is_const) {
                    runtimeError(vm, "Runtime Error: Cannot assign to constant global '%s'.", varNameVal.s_val);
                    Value temp_popped_val = pop(vm); freeValue(&temp_popped_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!sym->value) { // Should have been initialized by OP_DEFINE_GLOBAL
                     sym->value = (Value*)malloc(sizeof(Value));
                     if(!sym->value) { /* Malloc error */ Value temp_popped_val = pop(vm); freeValue(&temp_popped_val); return INTERPRET_RUNTIME_ERROR; }
                     // Initialize if it was somehow NULL
                     *(sym->value) = makeValueForType(sym->type, sym->type_def);
                }
                
                Value value_to_set_from_stack = peek(vm, 0);
                
                // TODO: Type checking for Pscal assignment (e.g., int to real promotion, error on string to int)
                // if (!typesAreAssignmentCompatible(sym->type, value_to_set_from_stack.type)) {
                //    runtimeError(vm, "Type mismatch assigning to global '%s'", varNameVal.s_val);
                //    return INTERPRET_RUNTIME_ERROR;
                // }

                freeValue(sym->value); // Free the old contents of the symbol's value
                *(sym->value) = makeCopyOfValue(&value_to_set_from_stack); // Store a deep copy
                // After copy, the symbol's type should ideally remain its declared type,
                // and the value assigned should be compatible or coerced.
                // For now, if makeCopyOfValue sets the type (it does by struct copy), let it be.
                // sym->type = sym->value->type; // Or ensure this reflects declared type and value is compatible
                
                Value popped_val = pop(vm); // Pop the value that was on stack
                freeValue(&popped_val);    // And free its (now redundant) contents
                break;
            }
            case OP_WRITE_LN: {
                #define MAX_WRITELN_ARGS 32 // Define a reasonable max
                // ...
                uint8_t argCount = READ_BYTE();
                Value args_for_writeln[MAX_WRITELN_ARGS];
                if (argCount > MAX_WRITELN_ARGS) {
                    runtimeError(vm, "Too many arguments for WriteLn (max %d).", MAX_WRITELN_ARGS);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < argCount; i++) {
                    args_for_writeln[argCount - 1 - i] = pop(vm); // Pop into array in correct order
                }

                for (int i = 0; i < argCount; i++) {
                    Value val = args_for_writeln[i];
                    // Basic printing logic (should be expanded like your AST interpreter's WriteLn)
                    switch(val.type) {
                        case TYPE_INTEGER: printf("%lld", val.i_val); break;
                        case TYPE_REAL:    printf("%f", val.r_val); break;
                        case TYPE_STRING:  printf("%s", val.s_val ? val.s_val : ""); break;
                        case TYPE_CHAR:    putchar(val.c_val); break;
                        case TYPE_BOOLEAN: printf("%s", val.i_val ? "TRUE" : "FALSE"); break;
                        default: printf("<VM:WriteLn_UnsupportedType:%s>", varTypeToString(val.type)); break;
                    }
                    freeValue(&val); // Free the content of the value popped from stack
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
                runtimeError(vm, "VM Error: Unknown opcode %d.", instruction);
                return INTERPRET_RUNTIME_ERROR;
        }
    }
    return INTERPRET_OK;
}

#ifdef DEBUG
// Simple Value printer for VM stack debugging
void dumpSymbolValue(Value* v) { // Made static as it's a local helper for vm.c's debug
    if (!v) { printf("NULL_Value_Ptr"); return; }
    switch(v->type) {
        case TYPE_INTEGER: printf("INT:%lld", v->i_val); break;
        case TYPE_REAL:    printf("REAL:%.2f", v->r_val); break;
        case TYPE_STRING:  printf("STR:\"%s\"", v->s_val ? v->s_val : "nil"); break;
        case TYPE_CHAR:    printf("CHAR:'%c'", v->c_val); break;
        case TYPE_BOOLEAN: printf("BOOL:%s", v->i_val ? "T" : "F"); break;
        case TYPE_NIL:     printf("NIL"); break;
        case TYPE_MEMORYSTREAM: printf("MSTREAM:%p", (void*)v->mstream); break;
        default: printf("%s", varTypeToString(v->type)); break;
    }
}
#endif
    
    static Symbol* createSymbolForVM(const char* name, VarType type, AST* type_def_for_value_init) {
        if (!name || name[0] == '\0') {
            // runtimeError(vm, "VM Internal Error: Invalid name for createSymbolForVM."); // vm not available here
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
        sym->type_def = type_def_for_value_init; // Store the AST type definition if provided (for complex types)
                                                 // For simple types from bytecode, this might be NULL.
        sym->value = (Value*)malloc(sizeof(Value));
        if (!sym->value) {
            fprintf(stderr, "VM Internal Error: Malloc failed for Value in createSymbolForVM for '%s'.\n", name);
            free(sym->name);
            free(sym);
            return NULL;
        }

        // Initialize the Value struct using makeValueForType
        *(sym->value) = makeValueForType(type, type_def_for_value_init);

        sym->is_alias = false;
        sym->is_const = false; // Globals defined by VAR are not const
        sym->is_local_var = false; // These are VM globals
        sym->next = NULL; // Will be handled by hashTableInsert

        return sym;
    }
