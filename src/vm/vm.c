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
static Symbol* insertSymbolIntoHashTable(HashTable* table, const char* name, VarType type, AST* type_def, bool is_var_decl_for_local_scope_pop);
static bool updateSymbolInHashTable(HashTable* table, const char* name, Value newValue);
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
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
                    runtimeError(vm, "Runtime Error: Global variable name not a string for OP_DEFINE_GLOBAL.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (sym == NULL) {
                    // For Pscal, type is known at compile time. VM should allocate with correct type.
                    // This requires compiler to pass type info for OP_DEFINE_GLOBAL.
                    // For now, default-initialize to integer 0, assignment will set type.
                    // The type_def AST node is not easily available here unless passed somehow.
                    sym = insertSymbolIntoHashTable(vm->vmGlobalSymbols, varNameVal.s_val, TYPE_INTEGER, NULL, false);
                    if (sym && sym->value) {
                         *(sym->value) = makeInt(0);
                    } else if (!sym) {
                        runtimeError(vm, "Runtime Error: Could not define global '%s' in VM.", varNameVal.s_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                }
                // No value is pushed or popped by OP_DEFINE_GLOBAL itself.
                // freeValue(&varNameVal); // No, varNameVal is from constant pool, not a heap copy
                break;
            }
            case OP_GET_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
                    runtimeError(vm, "Runtime Error: Global variable name not a string for OP_GET_GLOBAL.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", varNameVal.s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeCopyOfValue(sym->value));
                // freeValue(&varNameVal); // No, from constant pool
                break;
            }
            case OP_SET_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
                    runtimeError(vm, "Runtime Error: Global variable name not a string for OP_SET_GLOBAL.");
                     return INTERPRET_RUNTIME_ERROR;
                }
                
                // Value to be set is on top of the stack.
                // Peek it first, because updateSymbolInHashTable might free the old value in the symbol.
                Value value_to_set_on_stack = peek(vm, 0);
                
                if (!updateSymbolInHashTable(vm->vmGlobalSymbols, varNameVal.s_val, value_to_set_on_stack)) {
                    // updateSymbolInHashTable calls runtimeError if needed
                    return INTERPRET_RUNTIME_ERROR;
                }
                // The value on stack was deep copied by updateSymbolInHashTable into the symbol.
                // Now pop the original from stack and free its contents if it was complex.
                Value popped_val = pop(vm);
                freeValue(&popped_val);
                // freeValue(&varNameVal); // No, from constant pool
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
        // Add other types as needed
        default: printf("%s", varTypeToString(v->type)); break;
    }
}
#endif

// --- VM-specific Symbol Table Helper Stubs ---
// These need to be robustly implemented, likely by calling your existing
// symbol.c functions but passing vm->vmGlobalSymbols.
// For now, basic stubs.

static Symbol* insertSymbolIntoHashTable(HashTable* table, const char* name, VarType type, AST* type_def, bool is_var_decl_for_local_scope_pop) {
    (void)is_var_decl_for_local_scope_pop; // Unused for global VM table for now
    if (!table || !name) {
        fprintf(stderr, "VM Internal Error: Null table or name for insertSymbolIntoHashTable.\n");
        return NULL;
    }

    Symbol* sym = hashTableLookup(table, name);
    if (sym) {
        // Symbol already exists. For OP_DEFINE_GLOBAL, this might mean it's already defined.
        // Depending on Pscal rules (allow re-declaration at same level?), this could be an error or no-op.
        // For now, let's assume it's okay and the VM might just ensure its value area is ready.
        // But a proper DEFINE should only happen once for a given scope.
        // If this is just ensuring it exists for GET/SET, that's different.
        // The compiler's OP_DEFINE_GLOBAL ensures it is called for each var in `var` section.
        return sym;
    }

    sym = (Symbol*)malloc(sizeof(Symbol));
    if (!sym) {
        fprintf(stderr, "VM Internal Error: Malloc failed for Symbol in insertSymbolIntoHashTable.\n");
        return NULL;
    }
    sym->name = strdup(name);
    if (!sym->name) {
        fprintf(stderr, "VM Internal Error: Malloc failed for Symbol name in insertSymbolIntoHashTable.\n");
        free(sym);
        return NULL; /* Corrected: was missing a semicolon and not returning */
    } // <<< CORRECTED: Added semicolon and return NULL

    sym->type = type; // Initial type (might be generic like INTEGER if not fully known)
    sym->type_def = type_def;
    sym->value = (Value*)malloc(sizeof(Value));
    if (!sym->value) {
        fprintf(stderr, "VM Internal Error: Malloc failed for Value in insertSymbolIntoHashTable.\n");
        free(sym->name);
        free(sym);
        return NULL;
    }
    
    *(sym->value) = makeValueForType(type, type_def); // Initialize with default for type

    sym->is_alias = false;
    sym->is_const = false;
    sym->is_local_var = false;
    sym->next = NULL;

    hashTableInsert(table, sym); // Assumes hashTableInsert is from your symbol.c
    return sym;
}

static bool updateSymbolInHashTable(HashTable* table, const char* name, Value newValue) {
    if (!table || !name) {
        fprintf(stderr, "VM Internal Error: Null table or name for updateSymbolInHashTable.\n");
        return false;
    }
    Symbol* sym = hashTableLookup(table, name);
    if (!sym) {
        runtimeError(NULL, "VM Runtime Error: Global variable '%s' not defined before assignment.", name); // VM* is null for global error here
        return false;
    }
    if (sym->is_const) {
        runtimeError(NULL, "VM Runtime Error: Cannot assign to constant global '%s'.", name);
        return false;
    }
    if (!sym->value) {
        // This should ideally not happen if insertSymbolIntoHashTable initializes it
        sym->value = (Value*)malloc(sizeof(Value));
        if(!sym->value) {
            fprintf(stderr, "VM Internal Error: Malloc failed for Value in updateSymbolInHashTable for '%s'.\n", name);
            return false;
        }
        memset(sym->value, 0, sizeof(Value));
        // Type will be set by makeCopyOfValue
    }

    // TODO: VM-side type checking for assignment: sym->type vs newValue.type
    // For now, assume types are compatible or promote (e.g. int to real)
    // Example: if (sym->type == TYPE_REAL && newValue.type == TYPE_INTEGER) { newValue = makeReal(newValue.i_val); }

    freeValue(sym->value); // Free old contents of the symbol's Value
    *(sym->value) = makeCopyOfValue(&newValue); // Deep copy the new value
    sym->type = sym->value->type; // Ensure symbol's declared type matches the new value's type after copy
                                  // Or, sym->type should remain its declared type, and we check compatibility.
                                  // Let's keep sym->type as declared, assignment check needed.
    return true;
}
