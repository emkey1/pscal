// src/vm/vm.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h> // For bool, true, false
#include <ctype.h>
#include <pthread.h>
#include <limits.h>
#include <stdint.h>
#include <stddef.h>

#include "vm/vm.h"
#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For runtimeError, printValueToStream, makeNil, freeValue, Type helper macros
#include "symbol/symbol.h" // For HashTable, createHashTable, hashTableLookup, hashTableInsert
#include "Pascal/globals.h"
#include "backend_ast/audio.h"
#include "Pascal/parser.h"
#include "ast/ast.h"
#include "vm/string_sentinels.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "backend_ast/builtin.h"


// --- VM Helper Functions ---
static void resetStack(VM* vm) {
    vm->stackTop = vm->stack;
}

// Resolve a value to its underlying record by chasing pointer chains.
// Returns NULL if a nil pointer is encountered.  If the original value is
// neither a pointer nor a record, *invalid_type is set to true.
static void push(VM* vm, Value value);
static Value copyValueForStack(const Value* src);

static Value* resolveRecord(Value* base, bool* invalid_type) {
    if (invalid_type) *invalid_type = false;
    if (base->type != TYPE_POINTER && base->type != TYPE_RECORD) {
        if (invalid_type) *invalid_type = true;
        return NULL;
    }
    Value* current = base;
    while (current && current->type == TYPE_POINTER) {
        current = current->ptr_val;
    }
    return current;
}

static Value* resolveRecordForField(VM* vm, Value* base_val_ptr) {
    bool invalid_type = false;
    Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
    if (invalid_type) {
        runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
        return NULL;
    }
    if (record_struct_ptr == NULL) {
        runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
        return NULL;
    }
    if (record_struct_ptr->type != TYPE_RECORD) {
        runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
        return NULL;
    }
    return record_struct_ptr;
}

static bool pushFieldValueByOffset(VM* vm, Value* base_val_ptr, uint16_t field_index) {
    Value* record_struct_ptr = resolveRecordForField(vm, base_val_ptr);
    if (!record_struct_ptr) {
        return false;
    }

    FieldValue* current = record_struct_ptr->record_val;
    for (uint16_t i = 0; i < field_index && current; i++) {
        current = current->next;
    }
    if (!current) {
        runtimeError(vm, "VM Error: Field index out of range.");
        return false;
    }

    push(vm, copyValueForStack(&current->value));
    return true;
}

static bool pushFieldValueByName(VM* vm, Value* base_val_ptr, const char* field_name) {
    if (!field_name) {
        runtimeError(vm, "VM Error: Field name constant is invalid or NULL.");
        return false;
    }

    Value* record_struct_ptr = resolveRecordForField(vm, base_val_ptr);
    if (!record_struct_ptr) {
        return false;
    }

    FieldValue* current = record_struct_ptr->record_val;
    while (current) {
        if (current->name && strcmp(current->name, field_name) == 0) {
            push(vm, copyValueForStack(&current->value));
            return true;
        }
        current = current->next;
    }

    runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
    return false;
}

static bool coerceValueToBoolean(const Value* value, bool* out_truth) {
    if (!value || !out_truth) {
        return false;
    }
    if (IS_BOOLEAN(*value)) {
        *out_truth = AS_BOOLEAN(*value);
        return true;
    }
    if (IS_INTLIKE(*value)) {
        *out_truth = AS_INTEGER(*value) != 0;
        return true;
    }
    if (IS_REAL(*value)) {
        *out_truth = AS_REAL(*value) != 0.0;
        return true;
    }
    if (IS_CHAR(*value)) {
        *out_truth = AS_CHAR(*value) != '\0';
        return true;
    }
    if (value->type == TYPE_NIL) {
        *out_truth = false;
        return true;
    }
    return false;
}

static bool adjustLocalByDelta(VM* vm, Value* slot, long long delta, const char* opcode_name) {
    if (!slot) {
        runtimeError(vm, "VM Error: %s encountered a null local slot pointer.", opcode_name);
        return false;
    }

    bool is_enum_slot = (slot->type == TYPE_ENUM);
    if (!is_enum_slot && !IS_INTLIKE(*slot)) {
        runtimeError(vm, "VM Error: %s requires an ordinal local, got %s.",
                     opcode_name, varTypeToString(slot->type));
        return false;
    }

    if (is_enum_slot) {
        long long new_ord = (long long)slot->enum_val.ordinal + delta;
        slot->enum_val.ordinal = (int)new_ord;
        slot->i_val = (long long)slot->enum_val.ordinal;
        slot->u_val = (unsigned long long)slot->enum_val.ordinal;
        return true;
    }

    long long new_val = AS_INTEGER(*slot) + delta;
    switch (slot->type) {
        case TYPE_BOOLEAN:
            slot->i_val = (new_val != 0);
            slot->u_val = (unsigned long long)slot->i_val;
            break;
        case TYPE_CHAR:
            slot->c_val = (int)new_val;
            SET_INT_VALUE(slot, slot->c_val);
            break;
        case TYPE_UINT8:
        case TYPE_BYTE:
        case TYPE_UINT16:
        case TYPE_WORD:
        case TYPE_UINT32:
        case TYPE_UINT64:
            slot->u_val = (unsigned long long)new_val;
            slot->i_val = (long long)slot->u_val;
            break;
        default:
            SET_INT_VALUE(slot, new_val);
            break;
    }
    return true;
}

// --- Class method registration helpers ---
void vmRegisterClassMethod(VM* vm, const char* className, uint16_t methodIndex, Symbol* methodSymbol) {
    if (!vm || !vm->procedureTable || !className || !methodSymbol) return;
    char key[256];
    snprintf(key, sizeof(key), "%s::%u", className, methodIndex);
    Symbol* alias = (Symbol*)malloc(sizeof(Symbol));
    if (!alias) return;
    *alias = *methodSymbol;
    alias->name = strdup(key);
    alias->is_alias = true;
    alias->real_symbol = methodSymbol;
    alias->next = NULL;
    hashTableInsert(vm->procedureTable, alias);
}

Symbol* vmFindClassMethod(VM* vm, const char* className, uint16_t methodIndex) {
    if (!vm || !vm->procedureTable || !className) return NULL;
    char key[256];
    snprintf(key, sizeof(key), "%s::%u", className, methodIndex);
    Symbol* sym = hashTableLookup(vm->procedureTable, key);
    if (sym && sym->is_alias && sym->real_symbol) return sym->real_symbol;
    return sym;
}

// --- Threading Helpers ---
typedef struct {
    Thread* thread;
    uint16_t entry;
    int argc;
    Value args[8]; // up to 8 arguments supported
} ThreadStartArgs;

// Forward declarations for helpers used by threadStart.
static void push(VM* vm, Value value);
static Symbol* findProcedureByAddress(HashTable* table, uint16_t address);

static void* threadStart(void* arg) {
    ThreadStartArgs* args = (ThreadStartArgs*)arg;
    Thread* thread = args->thread;
    VM* vm = thread->vm;
    uint16_t entry = args->entry;
    int argc = args->argc;
    Value local_args[8];
    for (int i = 0; i < argc && i < 8; i++) local_args[i] = args->args[i];
    free(args);

    Symbol* proc_symbol = findProcedureByAddress(vm->procedureTable, entry);
    if (proc_symbol && proc_symbol->is_alias) proc_symbol = proc_symbol->real_symbol;

    CallFrame* frame = &vm->frames[vm->frameCount++];
    frame->return_address = NULL;
    frame->slots = vm->stack;
    frame->function_symbol = proc_symbol;
    frame->slotCount = 0;
    frame->locals_count = proc_symbol ? proc_symbol->locals_count : 0;
    frame->upvalue_count = proc_symbol ? proc_symbol->upvalue_count : 0;
    frame->upvalues = NULL;
    frame->discard_result_on_return = false;
    frame->vtable = NULL;

    if (proc_symbol && proc_symbol->upvalue_count > 0) {
        frame->upvalues = calloc(proc_symbol->upvalue_count, sizeof(Value*));
        if (frame->upvalues) {
            for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                frame->upvalues[i] = NULL;
            }
        }
    }

    // If the callee expects parameters, push the provided start_arg as the first argument.
    // Push up to the number of expected parameters; coerce basic types as needed.
    int expected = proc_symbol && proc_symbol->type_def ? proc_symbol->type_def->child_count : argc;
    int pushed_args = 0;
    for (int i = 0; i < expected && i < argc && i < 8; i++) {
        Value v = local_args[i];
        if (proc_symbol && proc_symbol->type_def) {
            AST* param_ast = proc_symbol->type_def->children[i];
            if (param_ast) {
                if (isRealType(param_ast->var_type) && isIntlikeType(v.type)) {
                    long double tmp = asLd(v);
                    setTypeValue(&v, param_ast->var_type);
                    SET_REAL_VALUE(&v, tmp);
                }
            }
        }
        push(vm, v);
        pushed_args++;
    }

    for (int i = 0; proc_symbol && i < proc_symbol->locals_count; i++) {
        push(vm, makeNil());
    }

    if (proc_symbol) {
        frame->slotCount = (uint16_t)(pushed_args + proc_symbol->locals_count);
    } else {
        frame->slotCount = (uint16_t)pushed_args;
    }

    interpretBytecode(vm, vm->chunk, vm->vmGlobalSymbols, vm->vmConstGlobalSymbols, vm->procedureTable, entry);
    thread->active = false;
    return NULL;
}

static int createThreadWithArgs(VM* vm, uint16_t entry, int argc, Value* argv) {
    int id = -1;
    for (int i = 1; i < VM_MAX_THREADS; i++) {
        if (!vm->threads[i].active && vm->threads[i].vm == NULL) {
            id = i;
            break;
        }
    }
    if (id == -1) return -1;

    Thread* t = &vm->threads[id];
    t->vm = malloc(sizeof(VM));
    if (!t->vm) return -1;
    initVM(t->vm);
    t->vm->vmGlobalSymbols = vm->vmGlobalSymbols;
    t->vm->vmConstGlobalSymbols = vm->vmConstGlobalSymbols;
    t->vm->procedureTable = vm->procedureTable;
    memcpy(t->vm->host_functions, vm->host_functions, sizeof(vm->host_functions));
    t->vm->chunk = vm->chunk;
    t->vm->mutexOwner = vm->mutexOwner ? vm->mutexOwner : vm;
    t->vm->mutexCount = t->vm->mutexOwner->mutexCount;
    t->vm->trace_head_instructions = vm->trace_head_instructions;
    t->vm->trace_executed = 0;

    ThreadStartArgs* args = malloc(sizeof(ThreadStartArgs));
    if (!args) {
        free(t->vm);
        t->vm = NULL;
        return -1;
    }
    args->thread = t;
    args->entry = entry;
    args->argc = (argc > 8) ? 8 : argc;
    for (int i = 0; i < args->argc; i++) args->args[i] = argv[i];
    t->active = true;
    if (pthread_create(&t->handle, NULL, threadStart, args) != 0) {
        free(args);
        free(t->vm);
        t->vm = NULL;
        t->active = false;
        return -1;
    }

    if (id >= vm->threadCount) {
        vm->threadCount = id + 1;
    }
    return id;
}

// Backward-compatible helper: no argument provided, pass NIL
static int createThread(VM* vm, uint16_t entry) {
    return createThreadWithArgs(vm, entry, 0, NULL);
}

static void joinThread(VM* vm, int id) {
    // Thread IDs start at 1. ID 0 represents the main thread and cannot be
    // joined through this helper. Only negative IDs or those beyond the
    // current thread count are invalid.
    if (id <= 0 || id >= vm->threadCount) return;
    Thread* t = &vm->threads[id];
    if (t->active) {
        pthread_join(t->handle, NULL);
        t->active = false;
    }
    if (t->vm) {
        freeVM(t->vm);
        free(t->vm);
        t->vm = NULL;
    }
    while (vm->threadCount > 1 &&
           !vm->threads[vm->threadCount - 1].active &&
           vm->threads[vm->threadCount - 1].vm == NULL) {
        vm->threadCount--;
    }
}

// --- Mutex Helpers ---
static int createMutex(VM* vm, bool recursive) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    int id = -1;
    // Look for an inactive slot to reuse.

    for (int i = 0; i < owner->mutexCount; i++) {
        if (!owner->mutexes[i].active) {
            id = i;
            break;
        }
    }
    // If none found, append a new mutex if capacity allows.
    if (id == -1) {
        if (owner->mutexCount >= VM_MAX_MUTEXES) {
            pthread_mutex_unlock(&owner->mutexRegistryLock);
            return -1;
        }
        id = owner->mutexCount;
        owner->mutexCount++;
    }
    Mutex* m = &owner->mutexes[id];
    pthread_mutexattr_t attr;
    pthread_mutexattr_t* attr_ptr = NULL;
    if (recursive) {
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        attr_ptr = &attr;
    }
    if (pthread_mutex_init(&m->handle, attr_ptr) != 0) {
        if (attr_ptr) pthread_mutexattr_destroy(&attr);
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return -1;
    }
    if (attr_ptr) pthread_mutexattr_destroy(&attr);
    m->active = true;
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return id;
}

static bool lockMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    Mutex* m = &owner->mutexes[id];
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return pthread_mutex_lock(&m->handle) == 0;
}

static bool unlockMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    Mutex* m = &owner->mutexes[id];
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return pthread_mutex_unlock(&m->handle) == 0;
}

// Permanently frees a mutex created by mutex()/rcmutex(), making its ID unusable.
static bool destroyMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    pthread_mutex_lock(&owner->mutexRegistryLock);
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    if (pthread_mutex_destroy(&owner->mutexes[id].handle) != 0) {
        pthread_mutex_unlock(&owner->mutexRegistryLock);
        return false;
    }
    owner->mutexes[id].active = false;

    // If this was the highest-index mutex, shrink the count so new mutexes can reuse slots.
    while (owner->mutexCount > 0 && !owner->mutexes[owner->mutexCount - 1].active) {
        owner->mutexCount--;

    }
    pthread_mutex_unlock(&owner->mutexRegistryLock);
    return true;
}

// Internal function shared by stack dump helpers
static void vmDumpStackInternal(VM* vm, bool detailed) {
    if (!vm) return;

    if (detailed) {
        for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
            fprintf(stderr, "  [ ");
            printValueToStream(*slot, stderr);
            fprintf(stderr, " ]\n");
        }
    } else {
        for (Value* slot = vm->stack; slot < vm->stackTop; slot++) {
            fprintf(stderr, "[");
            printValueToStream(*slot, stderr);
            fprintf(stderr, "] ");
        }
        fprintf(stderr, "\n");
    }
}

static void assignRealToIntChecked(VM* vm, Value* dest, long double real_val) {
    bool range_error = false;
    switch (dest->type) {
        case TYPE_BOOLEAN: {
            long long tmp = (real_val != 0.0L) ? 1 : 0;
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_CHAR: {
            int tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0;
            } else if (real_val > (long double)UCHAR_MAX) {
                range_error = true;
                tmp = UCHAR_MAX;
            } else {
                tmp = (int)real_val;
            }
            dest->c_val = tmp;
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT8:
        case TYPE_BYTE: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT8_MAX) {
                range_error = true;
                tmp = UINT8_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT8: {
            long long tmp;
            if (real_val < (long double)INT8_MIN) {
                range_error = true;
                tmp = INT8_MIN;
            } else if (real_val > (long double)INT8_MAX) {
                range_error = true;
                tmp = INT8_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT16:
        case TYPE_WORD: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT16_MAX) {
                range_error = true;
                tmp = UINT16_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT16: {
            long long tmp;
            if (real_val < (long double)INT16_MIN) {
                range_error = true;
                tmp = INT16_MIN;
            } else if (real_val > (long double)INT16_MAX) {
                range_error = true;
                tmp = INT16_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT32: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT32_MAX) {
                range_error = true;
                tmp = UINT32_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_INT32: {
            long long tmp;
            if (real_val < (long double)INT32_MIN) {
                range_error = true;
                tmp = INT32_MIN;
            } else if (real_val > (long double)INT32_MAX) {
                range_error = true;
                tmp = INT32_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        case TYPE_UINT64: {
            unsigned long long tmp;
            if (real_val < 0.0L) {
                range_error = true;
                tmp = 0ULL;
            } else if (real_val > (long double)UINT64_MAX) {
                range_error = true;
                tmp = UINT64_MAX;
            } else {
                tmp = (unsigned long long)real_val;
            }
            dest->u_val = tmp;
            dest->i_val = (tmp <= (unsigned long long)LLONG_MAX) ? (long long)tmp : LLONG_MAX;
            break;
        }
        case TYPE_INT64: {
            long long tmp;
            if (real_val < (long double)LLONG_MIN) {
                range_error = true;
                tmp = LLONG_MIN;
            } else if (real_val > (long double)LLONG_MAX) {
                range_error = true;
                tmp = LLONG_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
        default: {
            long long tmp;
            if (real_val < (long double)LLONG_MIN) {
                range_error = true;
                tmp = LLONG_MIN;
            } else if (real_val > (long double)LLONG_MAX) {
                range_error = true;
                tmp = LLONG_MAX;
            } else {
                tmp = (long long)real_val;
            }
            SET_INT_VALUE(dest, tmp);
            break;
        }
    }
    if (range_error) {
        runtimeError(vm, "Warning: Range check error assigning REAL %Lf to %s.",
                    real_val, varTypeToString(dest->type));
    }
}

void vmDumpStackInfoDetailed(VM* vm, const char* context_message) {
    if (!vm) return; // Safety check

    fprintf(stderr, "\n--- VM State Dump (%s) ---\n", context_message ? context_message : "Runtime Context");
    fprintf(stderr, "Stack Size: %ld, Frame Count: %d\n", vm->stackTop - vm->stack, vm->frameCount);
    fprintf(stderr, "Stack Contents (bottom to top):\n");
    vmDumpStackInternal(vm, true);
    fprintf(stderr, "--------------------------\n");
}

// --- Helper function to dump stack and frame info ---
void vmDumpStackInfo(VM* vm) {
    long current_offset = vm->ip - vm->chunk->code;
    int line = (current_offset > 0 && current_offset <= vm->chunk->count) ? vm->chunk->lines[current_offset - 1] : 0;

    fprintf(stderr, "[VM_DEBUG] Offset: %04ld, Line: %4d, Stack Size: %ld, Frame Count: %d\n",
            current_offset, line, vm->stackTop - vm->stack, vm->frameCount);

    // Disassemble and print the current instruction
    if (current_offset < vm->chunk->count) {
        disassembleInstruction(vm->chunk, current_offset, vm->procedureTable);
    } else {
        fprintf(stderr, "         (End of bytecode or invalid offset)\n");
    }

    // Print stack contents for more detailed debugging:
    fprintf(stderr, "[VM_DEBUG] Stack Contents: ");
    vmDumpStackInternal(vm, false);
}

static bool vmSetContains(const Value* setVal, const Value* itemVal) {
    if (!setVal || setVal->type != TYPE_SET || !itemVal) {
        return false;
    }

    long long item_ord;
    bool item_is_ordinal = false;

    // Get ordinal value of the item
    switch (itemVal->type) {
        case TYPE_INTEGER:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_BOOLEAN:
            item_ord = itemVal->i_val;
            item_is_ordinal = true;
            break;
        case TYPE_CHAR:
            item_ord = (long long)itemVal->c_val;
            item_is_ordinal = true;
            break;
        case TYPE_ENUM:
            item_ord = (long long)itemVal->enum_val.ordinal;
            item_is_ordinal = true;
            break;
        default:
            item_is_ordinal = false;
            break;
    }

    if (!item_is_ordinal) return false; // Item is not of a type that can be in a set

    // Search for the ordinal value in the set's values array
    if (!setVal->set_val.set_values) return false;
    for (int i = 0; i < setVal->set_val.set_size; i++) {
        if (setVal->set_val.set_values[i] == item_ord) {
            return true;
        }
    }
    return false;
}

// Scans all global symbols and the entire VM value stack to find and nullify
// any pointers that are aliases of a memory address that is being disposed.
//
// The caller must hold `globals_mutex` before invoking this function to ensure
// thread-safe access to global interpreter state.
void vmNullifyAliases(VM* vm, uintptr_t disposedAddrValue) {
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

    // Get precise instruction offset and line for the error.
    // vm->lastInstruction points at the start of the instruction that ran last
    // (the one that triggered the runtime error).
    size_t instruction_offset = 0;
    int error_line = 0;
    if (vm && vm->chunk && vm->lastInstruction && vm->chunk->code && vm->chunk->lines) {
        if (vm->lastInstruction >= vm->chunk->code) {
            instruction_offset = (size_t)(vm->lastInstruction - vm->chunk->code);
            if (instruction_offset < (size_t)vm->chunk->count) {
                error_line = vm->chunk->lines[instruction_offset];
            }
        }
    } else if (vm && vm->chunk && vm->chunk->count > 0) {
        // Special case: error on the very first instruction
        instruction_offset = 0;
        error_line = vm->chunk->lines[0];
    }
    fprintf(stderr, "[Error Location] Offset: %zu, Line: %d\n", instruction_offset, error_line);

    // --- NEW: Dump crash context (instructions and full stack) ---
    fprintf(stderr, "\n--- VM Crash Context ---\n");
    if (vm) {
        fprintf(stderr, "Instruction Pointer (IP): %p\n", (void*)vm->ip);
        fprintf(stderr, "Code Base: %p\n", vm && vm->chunk ? (void*)vm->chunk->code : (void*)NULL);
    }

    // Dump current instruction (at vm->ip)
    fprintf(stderr, "Current Instruction (at IP, might be the instruction that IP tried to fetch/decode):\n");
    if (vm && vm->chunk && vm->ip >= vm->chunk->code && (vm->ip - vm->chunk->code) < vm->chunk->count) {
        // disassembleInstruction will advance vm->ip temporarily.
        // We want to disassemble at vm->ip's current position.
        // Temporarily store vm->ip and restore it if disassembleInstruction modifies it.
        // However, disassembleInstruction itself returns the new offset, so safer to pass the current IP's offset.
        disassembleInstruction(vm->chunk, (int)(vm->ip - vm->chunk->code), vm->procedureTable);
    } else {
        fprintf(stderr, "  (IP is out of bytecode bounds: %p)\n", (void*)vm->ip);
    }

    // Dump preceding instructions for context
    int start_dump_offset = (int)instruction_offset - 10; // Dump last 10 instructions
    if (start_dump_offset < 0) start_dump_offset = 0;

    fprintf(stderr, "\nLast Instructions executed (leading to crash, up to %d bytes before error point):\n", (int)instruction_offset - start_dump_offset);
    if (vm && vm->chunk) {
        for (int offset = start_dump_offset; offset < (int)instruction_offset; ) {
            offset = disassembleInstruction(vm->chunk, offset, vm->procedureTable);
        }
    }
    if (start_dump_offset == (int)instruction_offset) {
        fprintf(stderr, "  (No preceding instructions in buffer to display)\n");
    }

    // Dump full stack contents
    if (vm) vmDumpStackInfoDetailed(vm, "Full Stack at Crash");

    // --- END NEW DUMP ---

    // resetStack(vm); // Keep this commented out for post-mortem analysis purposes if you want to inspect stack in debugger
    // ... (rest of runtimeError function, calls EXIT_FAILURE_HANDLER()) ...
}

static Value copyValueForStack(const Value* src) {
    if (!src) {
        return makeNil();
    }

    if (src->type == TYPE_MEMORYSTREAM) {
        Value alias = *src;
        if (alias.mstream) {
            retainMStream(alias.mstream);
        }
        return alias;
    }

    return makeCopyOfValue(src);
}

static void push(VM* vm, Value value) { // Using your original name 'push'
    if (vm->stackTop - vm->stack >= VM_STACK_MAX) {
        runtimeError(vm, "VM Error: Stack overflow.");
        return;
    }
    *vm->stackTop = value;
    vm->stackTop++;
}

static Symbol* findProcedureByAddress(HashTable* table, uint16_t address) {
    if (!table) return NULL;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (s->is_defined && s->bytecode_address == address) {
                return s;
            }
            if (s->type_def && s->type_def->symbol_table) {
                Symbol* nested = findProcedureByAddress((HashTable*)s->type_def->symbol_table, address);
                if (nested) return nested;
            }
        }
    }
    return NULL;
}

static Symbol* resolveProcedureAlias(Symbol* symbol) {
    if (symbol && symbol->is_alias && symbol->real_symbol) {
        return symbol->real_symbol;
    }
    return symbol;
}

static bool procedureVisibleFromFrames(VM* vm, Symbol* symbol) {
    if (!symbol) return false;
    if (!symbol->enclosing) return true;
    if (!vm) return false;

    for (int fi = vm->frameCount - 1; fi >= 0; fi--) {
        Symbol* frame_symbol = vm->frames[fi].function_symbol;
        while (frame_symbol) {
            if (frame_symbol == symbol->enclosing) {
                return true;
            }
            frame_symbol = frame_symbol->enclosing;
        }
    }
    return false;
}

static Symbol* findProcedureByName(HashTable* table, const char* lookup_name, VM* vm) {
    if (!table || !lookup_name) return NULL;

    Symbol* sym = resolveProcedureAlias(hashTableLookup(table, lookup_name));
    if (sym && procedureVisibleFromFrames(vm, sym)) {
        return sym;
    }

    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (Symbol* entry = table->buckets[i]; entry; entry = entry->next) {
            if (entry->type_def && entry->type_def->symbol_table) {
                Symbol* nested = findProcedureByName((HashTable*)entry->type_def->symbol_table, lookup_name, vm);
                if (nested) {
                    return nested;
                }
            }
        }
    }

    return NULL;
}

static Value pop(VM* vm) {
    if (vm->stackTop == vm->stack) {
        runtimeError(vm, "VM Error: Stack underflow (pop from empty stack).");
        return makeNil();
    }
    vm->stackTop--;
    Value result = *vm->stackTop; // Make a copy of the value to return.

    // Overwrite the just-popped slot with a safe NIL value to invalidate it
    // and prevent dangling pointers if the returned copy's contents are freed.
    *vm->stackTop = makeNil();

    return result; // Return the copy, which the caller is now responsible for.
}
static Value peek(VM* vm, int distance) { // Using your original name 'peek'
    if (vm->stackTop - vm->stack < distance + 1) {
        runtimeError(vm, "VM Error: Stack underflow (peek too deep).");
        return makeNil();
    }
    return vm->stackTop[-(distance + 1)];
}

// --- Host Function C Implementations ---
static Value vmHostQuitRequested(VM* vm) {
    // break_requested is extern int from globals.h, defined in globals.c
    // makeBoolean is from core/utils.h
    return makeBoolean(break_requested);
}

static Value vmHostCreateThreadAddr(VM* vm) {
    // New layout: [addr, arg0, arg1, ..., argc] — argc on top.
    Value argcVal = pop(vm);
    if (IS_INTLIKE(argcVal)) {
        int argc = (int)AS_INTEGER(argcVal);
        if (argc < 0) argc = 0;
        int totalArgs = argc;
        Value args[8];
        if (argc > 8) argc = 8;
        for (int i = totalArgs - 1; i >= 0; i--) {
            Value v = pop(vm);
            if (i < 8) {
                args[i] = v;
            } else {
                freeValue(&v);
            }
        }
        Value addrVal = pop(vm);
        uint16_t entry = 0;
        if (IS_INTLIKE(addrVal)) entry = (uint16_t)AS_INTEGER(addrVal);
        freeValue(&addrVal);
        int id = createThreadWithArgs(vm, entry, argc, args);
        return makeInt(id < 0 ? -1 : id);
    } else {
        // Backwards-compatible path: [addr, arg]
        Value argVal = argcVal; // already popped
        Value addrVal = pop(vm);
        uint16_t entry = 0;
        if (IS_INTLIKE(addrVal)) entry = (uint16_t)AS_INTEGER(addrVal);
        freeValue(&addrVal);
        int id = createThreadWithArgs(vm, entry, 1, &argVal);
        return makeInt(id < 0 ? -1 : id);
    }
}

static Value vmHostWaitThread(VM* vm) {
    // Expects top of stack: integer thread id
    Value tidVal = pop(vm);
    if (tidVal.type == TYPE_THREAD) {
        int id = (int)AS_INTEGER(tidVal);
        joinThread(vm, id);
    } else if (IS_INTLIKE(tidVal)) {
        int id = (int)AS_INTEGER(tidVal);
        joinThread(vm, id);
    }
    freeValue(&tidVal);
    return makeInt(0);
}

static Value vmHostPrintf(VM* vm) {
    Value countVal = pop(vm);
    int arg_count = 0;
    if (IS_INTLIKE(countVal)) arg_count = (int)AS_INTEGER(countVal);
    freeValue(&countVal);
    if (arg_count <= 0) return makeInt(0);

    Value* args = (Value*)malloc(sizeof(Value) * arg_count);
    if (!args) return makeInt(0);
    for (int i = 0; i < arg_count; ++i) {
        args[arg_count - 1 - i] = pop(vm);
    }

    const char* fmt = (args[0].type == TYPE_STRING && args[0].s_val) ? args[0].s_val : "";
    int arg_index = 1;
    size_t flen = strlen(fmt);
    for (size_t i = 0; i < flen; ++i) {
        if (fmt[i] == '%' && i + 1 < flen) {
            if (fmt[i + 1] == '%') {
                fputc('%', stdout);
                i++;
            } else if (arg_index < arg_count) {
                size_t j = i + 1;
                // Parse minimal flags: support '0' for zero-padding
                bool zero_pad = false;
                while (j < flen) {
                    if (fmt[j] == '0') { zero_pad = true; j++; continue; }
                    // Ignore other flags for now ('-', '+', ' ', '#')
                    break;
                }
                int width = 0;
                int precision = -1;
                while (j < flen && isdigit((unsigned char)fmt[j])) { width = width * 10 + (fmt[j]-'0'); j++; }
                if (j < flen && fmt[j] == '.') {
                    j++;
                    precision = 0;
                    while (j < flen && isdigit((unsigned char)fmt[j])) { precision = precision * 10 + (fmt[j]-'0'); j++; }
                }
                const char* length_mods = "hlLjzt";
                size_t mod_start = j;
                while (j < flen && strchr(length_mods, fmt[j]) != NULL) j++;
                char lenmod[4] = {0};
                size_t mod_len = (j > mod_start) ? (j - mod_start) : 0;
                if (mod_len > 0) {
                    if (mod_len > sizeof(lenmod) - 1) mod_len = sizeof(lenmod) - 1;
                    memcpy(lenmod, fmt + mod_start, mod_len);
                    lenmod[mod_len] = '\0';
                }
                char spec = (j < flen) ? fmt[j] : '\0';

                char fmtbuf[32];
                char buf[DEFAULT_STRING_CAPACITY];
                Value v = args[arg_index++];
                switch (spec) {
                    case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': {
                        unsigned long long u = 0ULL; long long s = 0LL;
                        if (isIntlikeType(v.type) || v.type == TYPE_BOOLEAN || v.type == TYPE_CHAR) {
                            s = AS_INTEGER(v);
                            u = (unsigned long long)AS_INTEGER(v);
                        }
                        bool is_unsigned = (spec=='u'||spec=='o'||spec=='x'||spec=='X');
                if (precision >= 0 && width > 0)
                    snprintf(fmtbuf, sizeof(fmtbuf), "%%%d.%d%s%c", width, precision, lenmod, spec);
                else if (precision >= 0)
                    snprintf(fmtbuf, sizeof(fmtbuf), "%%.%d%s%c", precision, lenmod, spec);
                else if (width > 0)
                    snprintf(fmtbuf, sizeof(fmtbuf), zero_pad ? "%%0%d%s%c" : "%%%d%s%c", width, lenmod, spec);
                else
                    snprintf(fmtbuf, sizeof(fmtbuf), "%%%s%c", lenmod, spec);

                        // Cast to the correct type expected by the format length modifier
                        if (is_unsigned) {
                            if (strcmp(lenmod, "ll") == 0) {
                                unsigned long long val = (unsigned long long)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "l") == 0) {
                                unsigned long val = (unsigned long)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "j") == 0) {
                                uintmax_t val = (uintmax_t)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "z") == 0) {
                                size_t val = (size_t)u;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else {
                                unsigned int val = (unsigned int)u; // includes h, hh, and default
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            }
                        } else {
                            if (strcmp(lenmod, "ll") == 0) {
                                long long val = (long long)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "l") == 0) {
                                long val = (long)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "j") == 0) {
                                intmax_t val = (intmax_t)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else if (strcmp(lenmod, "t") == 0) {
                                ptrdiff_t val = (ptrdiff_t)s;
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            } else {
                                int val = (int)s; // includes h, hh, and default
                                snprintf(buf, sizeof(buf), fmtbuf, val);
                            }
                        }
                        fputs(buf, stdout);
                        break;
                    }
                    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                        long double rv = isRealType(v.type) ? AS_REAL(v) : (long double)AS_INTEGER(v);
                        if (precision >= 0 && width > 0)
                            snprintf(fmtbuf, sizeof(fmtbuf), "%%%d.%d%s%c", width, precision, lenmod, spec);
                        else if (precision >= 0)
                            snprintf(fmtbuf, sizeof(fmtbuf), "%%.%d%s%c", precision, lenmod, spec);
                        else if (width > 0)
                            snprintf(fmtbuf, sizeof(fmtbuf), zero_pad ? "%%0%d%s%c" : "%%%d%s%c", width, lenmod, spec);
                        else
                            snprintf(fmtbuf, sizeof(fmtbuf), "%%%s%c", lenmod, spec);

                        // If 'L' modifier is present, pass a long double; otherwise pass double
                        if (strcmp(lenmod, "L") == 0) {
                            // Print directly with long double argument
                            // Use a temporary buffer via vsnprintf-like call by delegating to snprintf
                            // Note: snprintf supports %Lf on platforms where long double is distinct.
                            // We still format into buf to keep existing behavior (collect into string before fputs)
                            // Casting to long double explicitly for clarity.
                            snprintf(buf, sizeof(buf), fmtbuf, (long double)rv);
                        } else {
                            snprintf(buf, sizeof(buf), fmtbuf, (double)rv);
                        }
                        fputs(buf, stdout);
                        break;
                    }
                    case 'c': {
                        char ch = (v.type == TYPE_CHAR) ? v.c_val : (char)AS_INTEGER(v);
                        if (width > 0)
                            snprintf(fmtbuf, sizeof(fmtbuf), "%%%dc", width);
                        else
                            strcpy(fmtbuf, "%c");
                        snprintf(buf, sizeof(buf), fmtbuf, ch);
                        fputs(buf, stdout);
                        break;
                    }
                    case 's': {
                        const char* sv = (v.type == TYPE_STRING && v.s_val) ? v.s_val : "";
                        if (precision >= 0)
                            snprintf(fmtbuf, sizeof(fmtbuf), "%%%d.%ds", width, precision);
                        else if (width > 0)
                            snprintf(fmtbuf, sizeof(fmtbuf), "%%%ds", width);
                        else
                            strcpy(fmtbuf, "%s");
                        snprintf(buf, sizeof(buf), fmtbuf, sv);
                        fputs(buf, stdout);
                        break;
                    }
                    default:
                        printValueToStream(v, stdout);
                        break;
                }
                freeValue(&v);
                i = j;
            } else {
                fputc('%', stdout);
            }
        } else {
            fputc(fmt[i], stdout);
        }
    }

    for (int k = arg_index; k < arg_count; ++k) {
        freeValue(&args[k]);
    }
    freeValue(&args[0]);
    free(args);
    fflush(stdout);
    return makeInt(0);
}

// --- Host Function Registration ---
bool registerHostFunction(VM* vm, HostFunctionID id, HostFn fn) {
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
    vm->lastInstruction = NULL;
    vm->vmGlobalSymbols = NULL;              // Will be set by interpretBytecode
    vm->vmConstGlobalSymbols = NULL;
    vm->procedureTable = NULL;

    vm->frameCount = 0; // <--- INITIALIZE frameCount

    vm->exit_requested = false;

    vm->threadCount = 1; // main thread occupies index 0
    for (int i = 0; i < VM_MAX_THREADS; i++) {
        vm->threads[i].active = false;
        vm->threads[i].vm = NULL;
    }

    vm->mutexCount = 0;
    pthread_mutex_init(&vm->mutexRegistryLock, NULL);
    vm->mutexOwner = vm;
    for (int i = 0; i < VM_MAX_MUTEXES; i++) {
        vm->mutexes[i].active = false;
    }

    for (int i = 0; i < MAX_HOST_FUNCTIONS; i++) {
        vm->host_functions[i] = NULL;
    }
    if (!registerHostFunction(vm, HOST_FN_QUIT_REQUESTED, vmHostQuitRequested)) { // from all.txt
        fprintf(stderr, "Fatal VM Error: Could not register HOST_FN_QUIT_REQUESTED.\n");
        EXIT_FAILURE_HANDLER();
    }
    registerHostFunction(vm, HOST_FN_CREATE_THREAD_ADDR, vmHostCreateThreadAddr);
    registerHostFunction(vm, HOST_FN_WAIT_THREAD, vmHostWaitThread);
    registerHostFunction(vm, HOST_FN_PRINTF, vmHostPrintf);

    // Default: tracing disabled
    vm->trace_head_instructions = 0;
    vm->trace_executed = 0;
}

void freeVM(VM* vm) {
    if (!vm) return;
    // The VM holds references to global symbol tables that are owned and
    // managed by the caller (e.g. vm_main.c). Freeing them here would lead to
    // double-free errors when the caller performs its own cleanup. Simply
    // clear the pointer to signal that the VM no longer uses it.
    if (vm->vmGlobalSymbols) {
        vm->vmGlobalSymbols = NULL;
    }
    if (vm->vmConstGlobalSymbols) {
        vm->vmConstGlobalSymbols = NULL;
    }

    for (int i = 1; i < vm->threadCount; i++) {
        if (vm->threads[i].active) {
            pthread_join(vm->threads[i].handle, NULL);
            vm->threads[i].active = false;
        }
        if (vm->threads[i].vm) {
            freeVM(vm->threads[i].vm);
            free(vm->threads[i].vm);
            vm->threads[i].vm = NULL;
        }
    }

    if (vm->mutexOwner == vm) {
        for (int i = 0; i < vm->mutexCount; i++) {
            if (vm->mutexes[i].active) {
                pthread_mutex_destroy(&vm->mutexes[i].handle);
                vm->mutexes[i].active = false;
            }
        }
    }
    pthread_mutex_destroy(&vm->mutexRegistryLock);
    // No explicit freeing of vm->host_functions array itself as it's part of
    // the VM struct. If HostFn entries allocated memory, that would require
    // additional handling.
}

// Unwind the current call frame. If there are no more frames, the VM should halt.
// The 'halted' flag is set to true when the VM has returned from the top-level frame.
static InterpretResult returnFromCall(VM* vm, bool* halted) {
    if (vm->frameCount == 0) {
        if (vm->stackTop > vm->stack) {
            Value final_return_val = pop(vm);
            freeValue(&final_return_val);
        }
        if (halted) *halted = true;
        return INTERPRET_OK;
    }

    CallFrame* currentFrame = &vm->frames[vm->frameCount - 1];
    bool has_result = (currentFrame->function_symbol != NULL) &&
                      (currentFrame->function_symbol->type != TYPE_VOID);

    Value safeReturnValue = makeVoid();
    if (has_result) {
        if (vm->stackTop <= currentFrame->slots) {
            runtimeError(vm, "Stack underflow on function return.");
            if (halted) *halted = true;
            return INTERPRET_RUNTIME_ERROR;
        }
        Value returnValue = pop(vm);
        safeReturnValue = copyValueForStack(&returnValue);
        freeValue(&returnValue);
    }

    for (Value* slot = currentFrame->slots; slot < vm->stackTop; slot++) {
        freeValue(slot);
    }

    vm->ip = currentFrame->return_address;
    vm->stackTop = currentFrame->slots;
    currentFrame->slotCount = 0;

    if (currentFrame->upvalues) {
        free(currentFrame->upvalues);
        currentFrame->upvalues = NULL;
    }
    vm->frameCount--;

    if (has_result && !currentFrame->discard_result_on_return) {
        push(vm, safeReturnValue);
    } else {
        freeValue(&safeReturnValue);
    }

    // Signal halt when we've popped the last call frame so the caller can
    // terminate execution gracefully.
    if (halted) {
        *halted = (vm->frameCount == 0);
    }
    return INTERPRET_OK;
}

// --- Bytecode Reading Macros ---
// Your existing READ_BYTE and READ_CONSTANT macros are fine as they implicitly use 'vm' from runVM scope
#define READ_BYTE() (*vm->ip++)
#define READ_CONSTANT() (vm->chunk->constants[READ_BYTE()])
#define READ_CONSTANT16() (vm->chunk->constants[READ_SHORT(vm)])

// Helper function to read a 16-bit short. It will use the READ_BYTE() macro.
static inline uint16_t READ_SHORT(VM* vm_param) { // Pass vm explicitly here
    uint8_t msb = (*vm_param->ip++); // Explicitly use vm_param
    uint8_t lsb = (*vm_param->ip++); // Explicitly use vm_param
    return (uint16_t)(msb << 8) | lsb;
}

static inline uint32_t READ_UINT32(VM* vm_param) {
    uint32_t b1 = (uint32_t)(*vm_param->ip++);
    uint32_t b2 = (uint32_t)(*vm_param->ip++);
    uint32_t b3 = (uint32_t)(*vm_param->ip++);
    uint32_t b4 = (uint32_t)(*vm_param->ip++);
    return (b1 << 24) | (b2 << 16) | (b3 << 8) | b4;
}

#define READ_HOST_ID() ((HostFunctionID)READ_BYTE())

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
    // (debug logging removed)

    sym->is_alias = false;
    sym->is_const = false; // Constants handled at compile time won't use DEFINE_GLOBAL
                           // If VM needs to know about them, another mechanism or flag is needed.
    sym->is_local_var = false;
    sym->is_inline = false;
    sym->next = NULL;
    sym->enclosing = NULL;
    sym->upvalue_count = 0;
    return sym;
}

// Shared logic for DEFINE_GLOBAL and DEFINE_GLOBAL16.
// Assumes the name has already been read (as Value) and the IP is positioned
// at the declared type byte.
static InterpretResult handleDefineGlobal(VM* vm, Value varNameVal) {
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
            if (lower_bounds) free(lower_bounds);
            if (upper_bounds) free(upper_bounds);
            return INTERPRET_RUNTIME_ERROR;
        }

        for (int i = 0; i < dimension_count; i++) {
            uint16_t lower_idx = READ_SHORT(vm);
            uint16_t upper_idx = READ_SHORT(vm);
            if (lower_idx >= vm->chunk->constants_count || upper_idx >= vm->chunk->constants_count) {
                runtimeError(vm, "VM Error: Array bound constant index out of range for '%s'.", varNameVal.s_val);
                free(lower_bounds); free(upper_bounds);
                return INTERPRET_RUNTIME_ERROR;
            }
            Value lower_val = vm->chunk->constants[lower_idx];
            Value upper_val = vm->chunk->constants[upper_idx];
            if (!isIntlikeType(lower_val.type) || !isIntlikeType(upper_val.type)) {
                runtimeError(vm, "VM Error: Invalid constant types for array bounds of '%s'.", varNameVal.s_val);
                free(lower_bounds); free(upper_bounds);
                return INTERPRET_RUNTIME_ERROR;
            }
            lower_bounds[i] = (int)lower_val.i_val;
            upper_bounds[i] = (int)upper_val.i_val;
        }

        VarType elem_var_type = (VarType)READ_BYTE();
        uint8_t elem_name_idx = READ_BYTE();
        Value elem_name_val = vm->chunk->constants[elem_name_idx];
        AST* elem_type_def = NULL;
        if (elem_name_val.type == TYPE_STRING && elem_name_val.s_val && elem_name_val.s_val[0] != '\0') {
            elem_type_def = lookupType(elem_name_val.s_val);
        }

        Value array_val = makeArrayND(dimension_count, lower_bounds, upper_bounds,
                                      elem_var_type, elem_type_def);
        free(lower_bounds);
        free(upper_bounds);

        Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
        if (sym == NULL) {
            sym = (Symbol*)malloc(sizeof(Symbol));
            if (!sym) {
                runtimeError(vm, "VM Error: Malloc failed for Symbol struct for global array '%s'.", varNameVal.s_val);
                freeValue(&array_val);
                return INTERPRET_RUNTIME_ERROR;
            }
            sym->name = strdup(varNameVal.s_val);
            if (!sym->name) {
                runtimeError(vm, "VM Error: Malloc failed for symbol name for global array '%s'.", varNameVal.s_val);
                free(sym); freeValue(&array_val);
                return INTERPRET_RUNTIME_ERROR;
            }
            toLowerString(sym->name);
            sym->type = declaredType;
            sym->type_def = NULL;
            sym->value = (Value*)malloc(sizeof(Value));
            if (!sym->value) {
                runtimeError(vm, "VM Error: Malloc failed for Value struct for global array '%s'.", varNameVal.s_val);
                free(sym->name); free(sym); freeValue(&array_val);
                return INTERPRET_RUNTIME_ERROR;
            }
            *(sym->value) = array_val;
            sym->is_alias = false;
            sym->is_const = false;
            sym->is_local_var = false;
            sym->is_inline = false;
            sym->next = NULL;
            sym->enclosing = NULL;
            sym->upvalue_count = 0;
            hashTableInsert(vm->vmGlobalSymbols, sym);
        } else {
            runtimeError(vm, "VM Warning: Global variable '%s' redefined.", varNameVal.s_val);
            freeValue(sym->value);
            *(sym->value) = array_val;
        }
    } else {
        uint16_t type_name_idx = READ_SHORT(vm);
        int str_len = 0;
        uint16_t len_idx = 0;
        if (declaredType == TYPE_STRING) {
            len_idx = READ_SHORT(vm);
            Value len_val = vm->chunk->constants[len_idx];
            if (len_val.type == TYPE_INTEGER) {
                str_len = (int)len_val.i_val;
            }
        }
        Value typeNameVal = vm->chunk->constants[type_name_idx];
        // (debug logging removed)
        AST* type_def_node = NULL;
        if (declaredType == TYPE_STRING && str_len > 0) {
            Token* strTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0);
            type_def_node = newASTNode(AST_VARIABLE, strTok);
            setTypeAST(type_def_node, TYPE_STRING);
            freeToken(strTok);
            char buf[32];
            snprintf(buf, sizeof(buf), "%d", str_len);
            Token* lenTok = newToken(TOKEN_INTEGER_CONST, buf, 0, 0);
            AST* lenNode = newASTNode(AST_NUMBER, lenTok);
            setTypeAST(lenNode, TYPE_INTEGER);
            freeToken(lenTok);
            setRight(type_def_node, lenNode);
        } else if (typeNameVal.type == TYPE_STRING && typeNameVal.s_val) {
            // Prefer user-defined type resolution if available
            AST* looked = lookupType(typeNameVal.s_val);
            if (declaredType == TYPE_POINTER && looked) {
                type_def_node = looked; // Will be a POINTER_TYPE or TYPE_REFERENCE
            } else if (declaredType == TYPE_POINTER) {
                // Fall back to simple base types mapping
                Token* baseTok = newToken(TOKEN_IDENTIFIER, typeNameVal.s_val, 0, 0);
                type_def_node = newASTNode(AST_VARIABLE, baseTok);
                const char* tn = typeNameVal.s_val;
                if      (strcasecmp(tn, "integer") == 0 || strcasecmp(tn, "int") == 0) setTypeAST(type_def_node, TYPE_INT32);
                else if (strcasecmp(tn, "real")    == 0 || strcasecmp(tn, "double") == 0) setTypeAST(type_def_node, TYPE_DOUBLE);
                else if (strcasecmp(tn, "single")  == 0 || strcasecmp(tn, "float")  == 0) setTypeAST(type_def_node, TYPE_FLOAT);
                else if (strcasecmp(tn, "char")    == 0) setTypeAST(type_def_node, TYPE_CHAR);
                else if (strcasecmp(tn, "boolean") == 0 || strcasecmp(tn, "bool") == 0) setTypeAST(type_def_node, TYPE_BOOLEAN);
                else if (strcasecmp(tn, "byte")    == 0) setTypeAST(type_def_node, TYPE_BYTE);
                else if (strcasecmp(tn, "word")    == 0) setTypeAST(type_def_node, TYPE_WORD);
                else if (strcasecmp(tn, "int64")   == 0 || strcasecmp(tn, "longint") == 0) setTypeAST(type_def_node, TYPE_INT64);
                else if (strcasecmp(tn, "cardinal")== 0) setTypeAST(type_def_node, TYPE_UINT32);
                else setTypeAST(type_def_node, TYPE_VOID);
                freeToken(baseTok);
            } else {
                type_def_node = looked;
                if (declaredType == TYPE_ENUM && type_def_node == NULL) {
                    runtimeError(vm, "VM Error: Enum type '%s' not found for global '%s'.", typeNameVal.s_val, varNameVal.s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
            }
        }

        if (declaredType == TYPE_POINTER && type_def_node == NULL) {
            // Final safety: default pointer base to integer if not provided
            Token* baseTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0);
            type_def_node = newASTNode(AST_VARIABLE, baseTok);
            setTypeAST(type_def_node, TYPE_INT32);
            freeToken(baseTok);
        }

        if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
            runtimeError(vm, "VM Error: Invalid variable name for DEFINE_GLOBAL.");
            return INTERPRET_RUNTIME_ERROR;
        }

        Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, varNameVal.s_val);
        if (sym == NULL) {
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

    return INTERPRET_OK;
}

// Determine if a core VM builtin requires access to global interpreter
// structures protected by globals_mutex. Builtins that do not touch such
// structures can execute without acquiring the global lock.
static bool builtinUsesGlobalStructures(const char* name) {
    if (!name) return false;

    /*
     * Builtins listed here read or modify interpreter globals declared in
     * Pascal/globals.c (symbol tables, IO state, CRT state, etc.). They must
     * execute while holding globals_mutex to avoid races with other
     * interpreter threads touching the same shared state.
     */
    static const char* const needs_lock[] = {
        "append",         "assign",        "biblinktext",   "biboldtext",
        "biclrscr",       "bilowvideo",    "binormvideo",   "biunderlinetext",
        "biwherex",       "biwherey",      "blinktext",     "boldtext",
        "close",          "clreol",        "clrscr",        "cursoroff",
        "cursoron",       "deline",        "dispose",       "eof",
        "erase",          "gotoxy",        "hidecursor",    "highvideo",
        "ioresult",       "insline",       "invertcolors",  "lowvideo",
        "normvideo",      "normalcolors",  "paramcount",    "paramstr",
        "quitrequested",  "read",          "readln",        "rename",
        "reset",          "rewrite",       "screenrows",    "screencols",
        "showcursor",     "textbackground", "textbackgrounde","textcolor",
        "textcolore",     "underlinetext", "window",        "wherex",
        "wherey", "pollkey", "waitkeyevent", "graphloop", 
    };

    for (size_t i = 0; i < sizeof(needs_lock)/sizeof(needs_lock[0]); i++) {
        if (strcmp(name, needs_lock[i]) == 0) return true;
    }
    return false;
}

// --- Main Interpretation Loop ---
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* const_globals, HashTable* procedures, uint16_t entry) {
    if (!vm || !chunk) return INTERPRET_RUNTIME_ERROR;

    vm->chunk = chunk;
    vm->ip = vm->chunk->code + entry;
    vm->lastInstruction = vm->ip;

    vm->vmGlobalSymbols = globals;    // Store globals table (ensure this is the intended one)
    vm->vmConstGlobalSymbols = const_globals; // Table of constant globals (no locking)
    vm->procedureTable = procedures; // <--- STORED procedureTable

    // Initialize default file variables if present but not yet opened.
    if (vm->vmGlobalSymbols) {
        pthread_mutex_lock(&globals_mutex);
        Symbol* inputSym = hashTableLookup(vm->vmGlobalSymbols, "input");
        if (inputSym && inputSym->value &&
            inputSym->value->type == TYPE_FILE &&
            inputSym->value->f_val == NULL) {
            inputSym->value->f_val = stdin;
        }

        Symbol* outputSym = hashTableLookup(vm->vmGlobalSymbols, "output");
        if (outputSym && outputSym->value &&
            outputSym->value->type == TYPE_FILE &&
            outputSym->value->f_val == NULL) {
            outputSym->value->f_val = stdout;
        }
        pthread_mutex_unlock(&globals_mutex);
    }

    // Establish a base call frame for the main program if none has been
    // installed yet. Threads that set up their own initial frame prior to
    // invoking the interpreter can skip this.
    if (vm->frameCount == 0) {
        CallFrame* baseFrame = &vm->frames[vm->frameCount++];
        baseFrame->return_address = NULL;
        baseFrame->slots = vm->stack;
        baseFrame->function_symbol = NULL;
        baseFrame->slotCount = 0;
        baseFrame->locals_count = 0;
        baseFrame->upvalue_count = 0;
        baseFrame->upvalues = NULL;
        baseFrame->vtable = NULL;
    }

    #ifdef DEBUG
    if (dumpExec) { // from all.txt
        printf("\n--- VM Initial State ---\n");
        printf("IP: %p (offset 0)\n", (void*)vm->ip);
        printf("Stack top: %p (empty)\n", (void*)vm->stackTop);
        printf("Chunk code: %p, Chunk constants: %p\n", (void*)vm->chunk->code, (void*)vm->chunk->constants);
        printf("Global symbol table (for VM): %p\n", (void*)vm->vmGlobalSymbols);
        printf("Const global symbol table: %p\n", (void*)vm->vmConstGlobalSymbols);
        printf("Procedure table (for disassembly): %p\n", (void*)vm->procedureTable); // Debug print
        printf("------------------------\n");
    }
    #endif

#define BINARY_OP(op_char_for_error_msg, current_instruction_code) \
    do { \
        Value b_val_popped = pop(vm); \
        Value a_val_popped = pop(vm); \
        Value result_val; \
        bool op_is_handled = false; \
        \
        /* String/char concatenation for ADD */ \
        if (current_instruction_code == ADD) { \
            while (a_val_popped.type == TYPE_POINTER && a_val_popped.ptr_val) { \
                Value tmp = copyValueForStack(a_val_popped.ptr_val); \
                freeValue(&a_val_popped); \
                a_val_popped = tmp; \
            } \
            while (b_val_popped.type == TYPE_POINTER && b_val_popped.ptr_val) { \
                Value tmp = copyValueForStack(b_val_popped.ptr_val); \
                freeValue(&b_val_popped); \
                b_val_popped = tmp; \
            } \
            if ((IS_STRING(a_val_popped) || IS_CHAR(a_val_popped)) && \
                (IS_STRING(b_val_popped) || IS_CHAR(b_val_popped))) { \
                char a_buffer[2] = {0}; \
                char b_buffer[2] = {0}; \
                const char* s_a = NULL; \
                const char* s_b = NULL; \
                if (IS_STRING(a_val_popped)) { \
                    s_a = AS_STRING(a_val_popped) ? AS_STRING(a_val_popped) : ""; \
                } else { \
                    a_buffer[0] = AS_CHAR(a_val_popped); \
                    s_a = a_buffer; \
                } \
                if (IS_STRING(b_val_popped)) { \
                    s_b = AS_STRING(b_val_popped) ? AS_STRING(b_val_popped) : ""; \
                } else { \
                    b_buffer[0] = AS_CHAR(b_val_popped); \
                    s_b = b_buffer; \
                } \
                size_t len_a = strlen(s_a); \
                size_t len_b = strlen(s_b); \
                size_t total_len = len_a + len_b; \
                char* temp_concat_buffer = (char*)malloc(total_len + 1); \
                if (!temp_concat_buffer) { \
                    runtimeError(vm, "Runtime Error: Malloc failed for string concatenation buffer."); \
                    freeValue(&a_val_popped); freeValue(&b_val_popped); \
                    return INTERPRET_RUNTIME_ERROR; \
                } \
                memcpy(temp_concat_buffer, s_a, len_a); \
                memcpy(temp_concat_buffer + len_a, s_b, len_b); \
                temp_concat_buffer[total_len] = '\0'; \
                result_val = makeString(temp_concat_buffer); \
                free(temp_concat_buffer); \
                op_is_handled = true; \
            } \
        } \
        \
        /* Char +/- intlike handled as numeric ordinal operations */ \
\
        /* Enum +/- intlike */ \
        if (!op_is_handled) { \
            if (current_instruction_code == ADD || current_instruction_code == SUBTRACT) { \
                bool a_enum_b_int = (a_val_popped.type == TYPE_ENUM && IS_INTLIKE(b_val_popped)); \
                bool a_int_b_enum = (IS_INTLIKE(a_val_popped) && b_val_popped.type == TYPE_ENUM); \
                if (a_enum_b_int || a_int_b_enum) { \
                    Value enum_val = a_enum_b_int ? a_val_popped : b_val_popped; \
                    Value int_val  = a_enum_b_int ? b_val_popped : a_val_popped; \
                    long long delta = asI64(int_val); \
                    int new_ord = enum_val.enum_val.ordinal + \
                        ((current_instruction_code == ADD) ? (int)delta : -(int)delta); \
                    if (enum_val.enum_meta && \
                        (new_ord < 0 || new_ord >= enum_val.enum_meta->member_count)) { \
                        runtimeError(vm, "Runtime Error: Enum '%s' out of range.", \
                                     enum_val.enum_val.enum_name ? enum_val.enum_val.enum_name : "<anon>"); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    result_val = makeEnum(enum_val.enum_val.enum_name, new_ord); \
                    result_val.enum_meta = enum_val.enum_meta; \
                    result_val.base_type_node = enum_val.base_type_node; \
                    op_is_handled = true; \
                } \
            } \
        } \
        \
        /* Set union/difference/intersection */ \
        if (!op_is_handled) { \
            if (a_val_popped.type == TYPE_SET && b_val_popped.type == TYPE_SET) { \
                switch (current_instruction_code) { \
                    case ADD: \
                        result_val = setUnion(a_val_popped, b_val_popped); \
                        break; \
                    case SUBTRACT: \
                        result_val = setDifference(a_val_popped, b_val_popped); \
                        break; \
                    case MULTIPLY: \
                        result_val = setIntersection(a_val_popped, b_val_popped); \
                        break; \
                    default: \
                        runtimeError(vm, "Runtime Error: Unsupported set operation '%s'.", op_char_for_error_msg); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                } \
                op_is_handled = true; \
            } \
        } \
        \
        /* Numeric arithmetic (INTEGER/BYTE/WORD/REAL) */ \
        if (!op_is_handled) { \
            if (IS_NUMERIC(a_val_popped) && IS_NUMERIC(b_val_popped)) { \
                bool a_real = IS_REAL(a_val_popped); \
                bool b_real = IS_REAL(b_val_popped); \
                if (a_real || b_real) { \
                    /*
                     * When an integer participates in real arithmetic, operate on
                     * temporary copies so the original integer Value retains its
                     * type.  This prevents implicit widening of integer operands.
                     */ \
                    Value a_tmp = makeCopyOfValue(&a_val_popped); \
                    Value b_tmp = makeCopyOfValue(&b_val_popped); \
                    long double fa = asLd(a_tmp); \
                    long double fb = asLd(b_tmp); \
                    freeValue(&a_tmp); \
                    freeValue(&b_tmp); \
                    if (current_instruction_code == DIVIDE && fb == 0.0L) { \
                        runtimeError(vm, "Runtime Error: Division by zero."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    int useLong = (a_val_popped.type == TYPE_LONG_DOUBLE || b_val_popped.type == TYPE_LONG_DOUBLE); \
                    switch (current_instruction_code) { \
                        case ADD:      result_val = useLong ? makeLongDouble(fa + fb) : makeReal(fa + fb); break; \
                        case SUBTRACT: result_val = useLong ? makeLongDouble(fa - fb) : makeReal(fa - fb); break; \
                        case MULTIPLY: result_val = useLong ? makeLongDouble(fa * fb) : makeReal(fa * fb); break; \
                        case DIVIDE:   result_val = useLong ? makeLongDouble(fa / fb) : makeReal(fa / fb); break; \
                        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for real numbers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                } else { \
                    long long ia = asI64(a_val_popped); \
                    long long ib = asI64(b_val_popped); \
                    if (current_instruction_code == DIVIDE && ib == 0) { \
                        runtimeError(vm, "Runtime Error: Division by zero (integer)."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    long long iresult = 0; \
                    bool overflow = false; \
                    switch (current_instruction_code) { \
                        case ADD: \
                            overflow = __builtin_add_overflow(ia, ib, &iresult); \
                            break; \
                        case SUBTRACT: \
                            overflow = __builtin_sub_overflow(ia, ib, &iresult); \
                            break; \
                        case MULTIPLY: \
                            overflow = __builtin_mul_overflow(ia, ib, &iresult); \
                            break; \
                        case DIVIDE: \
                            result_val = makeReal((long double)ia / (long double)ib); \
                            break; \
                        case MOD: \
                            iresult = ib == 0 ? 0 : ia % ib; \
                            break; \
        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for integers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                    if (current_instruction_code == DIVIDE) { \
                        /* result_val already set for division */ \
                    } else if (overflow) { \
                        runtimeError(vm, "Runtime Error: Integer overflow."); \
                        freeValue(&a_val_popped); \
                        freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } else { \
                        result_val = makeInt(iresult); \
                    } \
                } \
                op_is_handled = true; \
            } \
        } \
        \
        if (!op_is_handled) { \
            runtimeError(vm, "Runtime Error: Operands must be numbers for arithmetic operation '%s' (or strings/chars for '+'). Got %s and %s", \
                         op_char_for_error_msg, varTypeToString(a_val_popped.type), varTypeToString(b_val_popped.type)); \
            freeValue(&a_val_popped); \
            freeValue(&b_val_popped); \
            return INTERPRET_RUNTIME_ERROR; \
        } \
        push(vm, result_val); \
        freeValue(&a_val_popped); \
        freeValue(&b_val_popped); \
    } while (false)

    uint8_t instruction_val;
    for (;;) {
        vm->lastInstruction = vm->ip;
/* #ifdef DEBUG
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
        #endif */
        //vmDumpStackInfo(vm); // Call new helper at the start of each instruction

        instruction_val = READ_BYTE();
        if (vm->trace_head_instructions > 0 && vm->trace_executed < vm->trace_head_instructions) {
            int offset = (int)(vm->ip - vm->chunk->code) - 1;
            long stacksz = (long)(vm->stackTop - vm->stack);
            fprintf(stderr, "[VM-TRACE] IP=%04d OPC=%u STACK=%ld\n", offset, (unsigned)instruction_val, stacksz);
            vm->trace_executed++;
        }
        switch (instruction_val) {
            case RETURN: {
                bool halted = false;
                InterpretResult res = returnFromCall(vm, &halted);
                if (res != INTERPRET_OK) return res;
                if (halted) return INTERPRET_OK;
                break;
            }
            case EXIT: {
                bool halted = false;
                InterpretResult res = returnFromCall(vm, &halted);
                if (res != INTERPRET_OK) return res;
                if (halted) return INTERPRET_OK;
                break;
            }
            case CONSTANT: {
                Value constant = READ_CONSTANT();
                push(vm, copyValueForStack(&constant));
                break;
            }
                
            case CONSTANT16: {
                uint16_t idx = READ_SHORT(vm);
                if (idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Constant index %u out of bounds for CONSTANT16.", idx);
                   return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(&vm->chunk->constants[idx]));
                break;
            }

            case GET_CHAR_ADDRESS: {
                Value index_val = pop(vm);
                Value* string_ptr_val = vm->stackTop - 1; // Peek at the string pointer

                if (string_ptr_val->type != TYPE_POINTER || !string_ptr_val->ptr_val || ((Value*)string_ptr_val->ptr_val)->type != TYPE_STRING) {
                    runtimeError(vm, "VM Error: Base for character index is not a pointer to a string.");
                    freeValue(&index_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (!isIntlikeType(index_val.type)) {
                    runtimeError(vm, "VM Error: String index must be an integer.");
                    freeValue(&index_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                long long pscal_index = index_val.i_val;
                freeValue(&index_val);

                Value* string_val = (Value*)string_ptr_val->ptr_val;
                const char* str = string_val->s_val ? string_val->s_val : "";
                size_t len = strlen(str);

                if (pscal_index < 1 || (size_t)pscal_index > len) {
                    runtimeError(vm, "Runtime Error: String index (%lld) out of bounds [1..%zu].", pscal_index, len);
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Pop the old string address pointer from the stack
                Value popped_string_ptr = pop(vm);
                freeValue(&popped_string_ptr);

                // Push a new pointer directly to the character's memory location
                // We use a special marker in base_type_node to identify this as a char pointer
                push(vm, makePointer(&string_val->s_val[pscal_index - 1], STRING_CHAR_PTR_SENTINEL));
                break;
            }
            case GET_GLOBAL_ADDRESS: {
                uint8_t name_idx = READ_BYTE();
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL_ADDRESS.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "Runtime Error: Invalid global name for address lookup.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* sym = NULL;
                if (vm->vmConstGlobalSymbols) {
                    sym = hashTableLookup(vm->vmConstGlobalSymbols, name_val->s_val);
                    if (sym && sym->value) {
                        push(vm, makePointer(sym->value, NULL));
                        break;
                    }
                }
                pthread_mutex_lock(&globals_mutex);
                sym = hashTableLookup(vm->vmGlobalSymbols, name_val->s_val);
                pthread_mutex_unlock(&globals_mutex);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Global '%s' not found in symbol table.", name_val->s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makePointer(sym->value, NULL));
                break;
            }
            case GET_GLOBAL_ADDRESS16: {
                uint16_t name_idx = READ_SHORT(vm);
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL_ADDRESS16.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "Runtime Error: Invalid global name for address lookup.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* sym = NULL;
                if (vm->vmConstGlobalSymbols) {
                    sym = hashTableLookup(vm->vmConstGlobalSymbols, name_val->s_val);
                    if (sym && sym->value) {
                        push(vm, makePointer(sym->value, NULL));
                        break;
                    }
                }
                pthread_mutex_lock(&globals_mutex);
                sym = hashTableLookup(vm->vmGlobalSymbols, name_val->s_val);
                pthread_mutex_unlock(&globals_mutex);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Global '%s' not found in symbol table.", name_val->s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makePointer(sym->value, NULL));
                break;
            }
            case GET_LOCAL_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makePointer(&frame->slots[slot], NULL));
                break;
            }
            case ADD:      BINARY_OP("+", instruction_val); break;
            case SUBTRACT: BINARY_OP("-", instruction_val); break;
            case MULTIPLY: BINARY_OP("*", instruction_val); break;
            case DIVIDE:   BINARY_OP("/", instruction_val); break;

            case NEGATE: {
                Value val_popped = pop(vm);
                Value result_val;
                if (IS_INTEGER(val_popped)) result_val = makeInt(-AS_INTEGER(val_popped));
                else if (IS_REAL(val_popped)) {
                    if (val_popped.type == TYPE_LONG_DOUBLE) result_val = makeLongDouble(-AS_REAL(val_popped));
                    else result_val = makeReal(-AS_REAL(val_popped));
                }
                else {
                    runtimeError(vm, "Runtime Error: Operand for negate must be a number.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result_val);
                freeValue(&val_popped);
                break;
            }
            case NOT: {
                Value val_popped = pop(vm);
                bool condition_truth = false;
                if (!coerceValueToBoolean(&val_popped, &condition_truth)) {
                    runtimeError(vm, "Runtime Error: Operand for boolean conversion must be boolean or numeric.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeBoolean(!condition_truth));
                freeValue(&val_popped);
                break;
            }
            case TO_BOOL: {
                Value val_popped = pop(vm);
                bool condition_truth = false;
                if (!coerceValueToBoolean(&val_popped, &condition_truth)) {
                    runtimeError(vm, "Runtime Error: Operand for boolean conversion must be boolean or numeric.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeBoolean(condition_truth));
                freeValue(&val_popped);
                break;
            }
            case SWAP: {
                if (vm->stackTop - vm->stack < 2) {
                    runtimeError(vm, "VM Error: Not enough values on stack to swap.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                // Perform an in-place swap of the top two Value structs on the stack.
                Value temp = vm->stackTop[-1];
                vm->stackTop[-1] = vm->stackTop[-2];
                vm->stackTop[-2] = temp;
                break;
            }
            case DUP: {
                if (vm->stackTop == vm->stack) {
                    runtimeError(vm, "VM Error: Stack underflow (dup from empty stack).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(&vm->stackTop[-1]));
                break;
            }
            case AND:
            case OR:
            case XOR: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;

                if (IS_BOOLEAN(a_val) && IS_BOOLEAN(b_val)) {
                    bool ba = AS_BOOLEAN(a_val);
                    bool bb = AS_BOOLEAN(b_val);
                    if (instruction_val == AND) {
                        result_val = makeBoolean(ba && bb);
                    } else if (instruction_val == OR) {
                        result_val = makeBoolean(ba || bb);
                    } else {
                        result_val = makeBoolean(ba ^ bb);
                    }
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val))  {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (instruction_val == AND) {
                        result_val = makeInt(ia & ib);
                    } else if (instruction_val == OR) {
                        result_val = makeInt(ia | ib);
                    } else {
                        result_val = makeInt(ia ^ ib);
                    }
                } else {
                    runtimeError(vm, "Runtime Error: Operands for AND/OR/XOR must be both Boolean or both Integer. Got %s and %s.",
                                 varTypeToString(a_val.type), varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, result_val);
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case INT_DIV: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib == 0) {
                        runtimeError(vm, "Runtime Error: Integer division by zero.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (ia == LLONG_MIN && ib == -1) {
                        runtimeError(vm, "Runtime Error: Integer overflow.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeInt(ia / ib));
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
            case MOD: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
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
            case SHL:
            case SHR: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val)) {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (ib < 0) {
                        runtimeError(vm, "Runtime Error: Shift amount cannot be negative.");
                        freeValue(&a_val); freeValue(&b_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    if (instruction_val == SHL) {
                        push(vm, makeInt(ia << ib));
                    } else {
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
            case EQUAL:
            case NOT_EQUAL:
            case GREATER:
            case GREATER_EQUAL:
            case LESS:
            case LESS_EQUAL: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;
                bool comparison_succeeded = false;

                // Handle explicit NIL-to-NIL comparisons first.  Pointer/NIL
                // comparisons are handled in the pointer block below.
                if (a_val.type == TYPE_NIL && b_val.type == TYPE_NIL) {
                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(true);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(false);
                    } else {
                        goto comparison_error_label;
                    }
                    comparison_succeeded = true;
                }
                // Numeric comparison (Integers and Reals)
                else if (IS_NUMERIC(a_val) && IS_NUMERIC(b_val)) {
                    bool a_real = isRealType(a_val.type);
                    bool b_real = isRealType(b_val.type);

                    if (a_real || b_real) {
                        long double fa = asLd(a_val);
                        long double fb = asLd(b_val);
                        switch (instruction_val) {
                            case EQUAL:         result_val = makeBoolean(fa == fb); break;
                            case NOT_EQUAL:     result_val = makeBoolean(fa != fb); break;
                            case GREATER:       result_val = makeBoolean(fa >  fb); break;
                            case GREATER_EQUAL: result_val = makeBoolean(fa >= fb); break;
                            case LESS:          result_val = makeBoolean(fa <  fb); break;
                            case LESS_EQUAL:    result_val = makeBoolean(fa <= fb); break;
                            default: goto comparison_error_label;
                        }
                    } else {
                        long long ia = asI64(a_val);
                        long long ib = asI64(b_val);
                        switch (instruction_val) {
                            case EQUAL:         result_val = makeBoolean(ia == ib); break;
                            case NOT_EQUAL:     result_val = makeBoolean(ia != ib); break;
                            case GREATER:       result_val = makeBoolean(ia >  ib); break;
                            case GREATER_EQUAL: result_val = makeBoolean(ia >= ib); break;
                            case LESS:          result_val = makeBoolean(ia <  ib); break;
                            case LESS_EQUAL:    result_val = makeBoolean(ia <= ib); break;
                            default: goto comparison_error_label;
                        }
                    }
                    comparison_succeeded = true;
                } else if ((IS_CHAR(a_val) && IS_STRING(b_val)) || (IS_STRING(a_val) && IS_CHAR(b_val))) {
                    char char_val;
                    const char* str_val;

                    if (IS_CHAR(a_val)) {
                        char_val = AS_CHAR(a_val);
                        str_val = AS_STRING(b_val);
                    } else {
                        char_val = AS_CHAR(b_val);
                        str_val = AS_STRING(a_val);
                    }

                    if (strlen(str_val) == 1 && str_val[0] == char_val) {
                        result_val = makeBoolean(instruction_val == EQUAL);
                    } else {
                        result_val = makeBoolean(instruction_val != EQUAL);
                    }
                    comparison_succeeded = true;
                }
                // String comparison
                else if (IS_STRING(a_val) && IS_STRING(b_val)) {
                    const char* sa = AS_STRING(a_val) ? AS_STRING(a_val) : "";
                    const char* sb = AS_STRING(b_val) ? AS_STRING(b_val) : "";
                    int cmp = strcmp(sa, sb);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(cmp == 0); break;
                        case NOT_EQUAL:     result_val = makeBoolean(cmp != 0); break;
                        case GREATER:       result_val = makeBoolean(cmp > 0);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(cmp >= 0); break;
                        case LESS:          result_val = makeBoolean(cmp < 0);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(cmp <= 0); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected string comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                } else if ((IS_CHAR(a_val) && IS_INTEGER(b_val)) || (IS_INTEGER(a_val) && IS_CHAR(b_val))) {
                    int char_val = IS_CHAR(a_val) ? AS_CHAR(a_val) : AS_CHAR(b_val);
                    long long int_val = IS_INTEGER(a_val) ? AS_INTEGER(a_val) : AS_INTEGER(b_val);

                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean((long long)char_val == int_val); break;
                        case NOT_EQUAL:     result_val = makeBoolean((long long)char_val != int_val); break;
                        case GREATER:       result_val = makeBoolean((long long)char_val > int_val);  break;
                        case GREATER_EQUAL: result_val = makeBoolean((long long)char_val >= int_val); break;
                        case LESS:          result_val = makeBoolean((long long)char_val < int_val);  break;
                        case LESS_EQUAL:    result_val = makeBoolean((long long)char_val <= int_val); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected char/integer comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Char comparison
                else if (IS_CHAR(a_val) && IS_CHAR(b_val)) {
                    int ca = AS_CHAR(a_val);
                    int cb = AS_CHAR(b_val);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(ca == cb); break;
                        case NOT_EQUAL:     result_val = makeBoolean(ca != cb); break;
                        case GREATER:       result_val = makeBoolean(ca > cb);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(ca >= cb); break;
                        case LESS:          result_val = makeBoolean(ca < cb);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(ca <= cb); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected char comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                } else if ((IS_CHAR(a_val) && IS_STRING(b_val)) || (IS_STRING(a_val) && IS_CHAR(b_val))) {
                    char char_val;
                    const char* str_val;
                    if (IS_CHAR(a_val)) { char_val = AS_CHAR(a_val); str_val = AS_STRING(b_val); }
                    else { char_val = AS_CHAR(b_val); str_val = AS_STRING(a_val); }

                    size_t slen = str_val ? strlen(str_val) : 0;

                    bool eq;
                    if (slen == 1) {
                        eq = (str_val[0] == char_val);
                    } else if (slen == 0) {
                        // Treat empty string as #0 char for equality tests
                        eq = (char_val == '\0');
                    } else {
                        eq = false;
                    }

                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(eq); break;
                        case NOT_EQUAL:     result_val = makeBoolean(!eq); break;
                        case GREATER:
                        case GREATER_EQUAL:
                        case LESS:
                        case LESS_EQUAL:
                            runtimeError(vm, "Runtime Error: Relational comparison between CHAR and STRING is not supported.");
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        default:
                            runtimeError(vm, "VM Error: Unexpected char/string comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // Boolean comparison
                else if ((IS_BOOLEAN(a_val) && (IS_BOOLEAN(b_val) || IS_INTEGER(b_val))) ||
                         (IS_INTEGER(a_val) && IS_BOOLEAN(b_val))) {
                          bool ba = IS_BOOLEAN(a_val) ? AS_BOOLEAN(a_val) : (AS_INTEGER(a_val) != 0);
                          bool bb = IS_BOOLEAN(b_val) ? AS_BOOLEAN(b_val) : (AS_INTEGER(b_val) != 0);
                    switch (instruction_val) {
                        case EQUAL:         result_val = makeBoolean(ba == bb); break;
                        case NOT_EQUAL:     result_val = makeBoolean(ba != bb); break;
                        case GREATER:       result_val = makeBoolean(ba > bb);  break;
                        case GREATER_EQUAL: result_val = makeBoolean(ba >= bb); break;
                        case LESS:          result_val = makeBoolean(ba < bb);  break;
                        case LESS_EQUAL:    result_val = makeBoolean(ba <= bb); break;
                        default:
                            runtimeError(vm, "VM Error: Unexpected boolean comparison opcode %d.", instruction_val);
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }
                // ENUM comparison
                else if (a_val.type == TYPE_ENUM && b_val.type == TYPE_ENUM) {
                    const char *name_a = a_val.enum_val.enum_name;
                    const char *name_b = b_val.enum_val.enum_name;
                    bool types_match = (name_a && name_b && strcmp(name_a, name_b) == 0);
                    int ord_a = a_val.enum_val.ordinal;
                    int ord_b = b_val.enum_val.ordinal;

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(types_match && (ord_a == ord_b));
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!types_match || (ord_a != ord_b));
                    } else {
                        if (!types_match) {
                            runtimeError(vm,
                                         "Runtime Error: Cannot compare different ENUM types ('%s' vs '%s') with relational operator.",
                                         name_a ? name_a : "<anon>",
                                         name_b ? name_b : "<anon>");
                            freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                        switch (instruction_val) {
                            case GREATER:       result_val = makeBoolean(ord_a > ord_b);  break;
                            case GREATER_EQUAL: result_val = makeBoolean(ord_a >= ord_b); break;
                            case LESS:          result_val = makeBoolean(ord_a < ord_b);  break;
                            case LESS_EQUAL:    result_val = makeBoolean(ord_a <= ord_b); break;
                            default:
                                runtimeError(vm, "VM Error: Unexpected enum comparison opcode %d.", instruction_val);
                                freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                        }
                    }
                    comparison_succeeded = true;
                }
                // Memory stream and NIL comparison
                else if ((a_val.type == TYPE_MEMORYSTREAM || a_val.type == TYPE_NIL) &&
                         (b_val.type == TYPE_MEMORYSTREAM || b_val.type == TYPE_NIL)) {
                    MStream* ms_a = (a_val.type == TYPE_MEMORYSTREAM) ? a_val.mstream : NULL;
                    MStream* ms_b = (b_val.type == TYPE_MEMORYSTREAM) ? b_val.mstream : NULL;
                    bool streams_equal = false;
                    if (a_val.type == TYPE_NIL && b_val.type == TYPE_NIL) {
                        streams_equal = true;
                    } else if (a_val.type == TYPE_NIL) {
                        streams_equal = (ms_b == NULL);
                    } else if (b_val.type == TYPE_NIL) {
                        streams_equal = (ms_a == NULL);
                    } else {
                        streams_equal = (ms_a == ms_b);
                    }

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(streams_equal);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!streams_equal);
                    } else {
                        runtimeError(vm,
                                     "Runtime Error: Invalid operator for memory stream comparison. Only '=' and '<>' are allowed. Got opcode %d.",
                                     instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
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
                    } else {
                        ptrs_equal = (a_val.ptr_val == b_val.ptr_val);
                    }

                    if (instruction_val == EQUAL) {
                        result_val = makeBoolean(ptrs_equal);
                    } else if (instruction_val == NOT_EQUAL) {
                        result_val = makeBoolean(!ptrs_equal);
                    } else {
                        runtimeError(vm, "Runtime Error: Invalid operator for pointer comparison. Only '=' and '<>' are allowed. Got opcode %d.", instruction_val);
                        freeValue(&a_val); freeValue(&b_val); return INTERPRET_RUNTIME_ERROR;
                    }
                    comparison_succeeded = true;
                }

comparison_error_label:
                if (comparison_succeeded) {
                    push(vm, result_val);
                } else {
                    const char* op_str = "unknown_comparison_op";
                    switch (instruction_val) {
                        case EQUAL:         op_str = "="; break;
                        case NOT_EQUAL:     op_str = "<>"; break;
                        case GREATER:       op_str = ">";  break;
                        case GREATER_EQUAL: op_str = ">="; break;
                        case LESS:          op_str = "<";  break;
                        case LESS_EQUAL:    op_str = "<="; break;
                        default: op_str = "unknown_comparison_op_code"; break;
                    }

                    runtimeError(vm, "Runtime Error: Operands not comparable for operator '%s'. Left operand: %s, Right operand: %s.",
                                                 op_str,
                                                 varTypeToString(a_val.type),
                                                 varTypeToString(b_val.type));
                    freeValue(&a_val); freeValue(&b_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&a_val);
                freeValue(&b_val);
                break;
            }
            case ALLOC_OBJECT: {
                uint8_t field_count = READ_BYTE();
                FieldValue* fields = calloc(field_count, sizeof(FieldValue));
                if (!fields) {
                    runtimeError(vm, "VM Error: Out of memory allocating object.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                for (uint16_t i = 0; i < field_count; i++) {
                    fields[i].name = NULL;
                    fields[i].value = makeNil();
                    fields[i].next = (i + 1 < field_count) ? &fields[i + 1] : NULL;
                }
                Value* obj = malloc(sizeof(Value));
                if (!obj) {
                    free(fields);
                    runtimeError(vm, "VM Error: Out of memory allocating object value.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                *obj = makeRecord(fields);
                push(vm, makePointer(obj, NULL));
                break;
            }
            case ALLOC_OBJECT16: {
                uint16_t field_count = READ_SHORT(vm);
                FieldValue* fields = calloc(field_count, sizeof(FieldValue));
                if (!fields) {
                    runtimeError(vm, "VM Error: Out of memory allocating object.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                for (uint16_t i = 0; i < field_count; i++) {
                    fields[i].name = NULL;
                    fields[i].value = makeNil();
                    fields[i].next = (i + 1 < field_count) ? &fields[i + 1] : NULL;
                }
                Value* obj = malloc(sizeof(Value));
                if (!obj) {
                    free(fields);
                    runtimeError(vm, "VM Error: Out of memory allocating object value.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                *obj = makeRecord(fields);
                push(vm, makePointer(obj, NULL));
                break;
            }
            case GET_FIELD_OFFSET: {
                uint8_t field_index = READ_BYTE();
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }


                FieldValue* current = record_struct_ptr->record_val;
                for (uint16_t i = 0; i < field_index && current; i++) {
                    current = current->next;
                }
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_base_val = pop(vm);
                freeValue(&popped_base_val);
                push(vm, makePointer(&current->value, NULL));
                break;
            }
            case GET_FIELD_OFFSET16: {
                uint16_t field_index = READ_SHORT(vm);
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                FieldValue* current = record_struct_ptr->record_val;
                for (uint16_t i = 0; i < field_index && current; i++) {
                    current = current->next;
                }
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range.");

                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_base_val = pop(vm);
                freeValue(&popped_base_val);
                push(vm, makePointer(&current->value, NULL));
                break;
            }
            case LOAD_FIELD_VALUE: {
                uint8_t field_index = READ_BYTE();
                Value base_val = pop(vm);
                bool ok = pushFieldValueByOffset(vm, &base_val, field_index);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case LOAD_FIELD_VALUE16: {
                uint16_t field_index = READ_SHORT(vm);
                Value base_val = pop(vm);
                bool ok = pushFieldValueByOffset(vm, &base_val, field_index);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case GET_FIELD_ADDRESS: {
                uint8_t field_name_idx = READ_BYTE();
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = record_struct_ptr->record_val;
                while (current) {
                    if (strcmp(current->name, field_name) == 0) {
                        Value popped_base_val = pop(vm);
                        freeValue(&popped_base_val);
                        push(vm, makePointer(&current->value, NULL));
                        goto next_instruction;
                    }
                    current = current->next;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case GET_FIELD_ADDRESS16: {
                uint16_t field_name_idx = READ_SHORT(vm);
                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Internal - expected to resolve to a record for field access.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* field_name = AS_STRING(vm->chunk->constants[field_name_idx]);
                FieldValue* current = record_struct_ptr->record_val;
                while (current) {
                    if (strcmp(current->name, field_name) == 0) {
                        Value popped_base_val = pop(vm);
                        freeValue(&popped_base_val);
                        push(vm, makePointer(&current->value, NULL));
                        goto next_instruction;
                    }
                    current = current->next;
                }

                runtimeError(vm, "VM Error: Field '%s' not found in record.", field_name);
                return INTERPRET_RUNTIME_ERROR;
            }
            case LOAD_FIELD_VALUE_BY_NAME: {
                uint8_t field_name_idx = READ_BYTE();
                Value base_val = pop(vm);
                const Value* name_val = (field_name_idx < vm->chunk->constants_count)
                                        ? &vm->chunk->constants[field_name_idx]
                                        : NULL;
                const char* field_name = (name_val && name_val->type == TYPE_STRING)
                                          ? AS_STRING(*name_val)
                                          : NULL;
                bool ok = pushFieldValueByName(vm, &base_val, field_name);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case LOAD_FIELD_VALUE_BY_NAME16: {
                uint16_t field_name_idx = READ_SHORT(vm);
                Value base_val = pop(vm);
                const Value* name_val = (field_name_idx < vm->chunk->constants_count)
                                        ? &vm->chunk->constants[field_name_idx]
                                        : NULL;
                const char* field_name = (name_val && name_val->type == TYPE_STRING)
                                          ? AS_STRING(*name_val)
                                          : NULL;
                bool ok = pushFieldValueByName(vm, &base_val, field_name);
                freeValue(&base_val);
                if (!ok) return INTERPRET_RUNTIME_ERROR;
                break;
            }
            case GET_ELEMENT_ADDRESS: {
                uint8_t dimension_count = READ_BYTE();

                // Pop the base operand first so type checking does not
                // depend on stack offsets.
                Value operand = pop(vm);

                // Special handling for strings when there is exactly one
                // index.  We avoid referencing stackTop-2 by working with the
                // popped operand directly and reusing it if it represents a
                // string.
                if (dimension_count == 1 && operand.type == TYPE_POINTER) {
                    Value* base_val = (Value*)operand.ptr_val;
                    if (base_val && base_val->type == TYPE_STRING) {
                        Value index_val = pop(vm);
                        if (!isIntlikeType(index_val.type)) {
                            runtimeError(vm, "VM Error: String index must be an integer.");
                            freeValue(&index_val);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        long long pscal_index = index_val.i_val;
                        freeValue(&index_val);

                        size_t len = base_val->s_val ? strlen(base_val->s_val) : 0;

                        if (pscal_index == 0) {
                            // Special case: element 0 returns the string length.
                            push(vm, makePointer(base_val, STRING_LENGTH_SENTINEL));
                            freeValue(&operand);
                            break; // Exit the case
                        }

                        if (pscal_index < 0 || (size_t)pscal_index > len) {
                            runtimeError(vm,
                                         "Runtime Error: String index (%lld) out of bounds for string of length %zu.",
                                         pscal_index, len);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        // Push a special pointer to the character's memory location
                        push(vm, makePointer(&base_val->s_val[pscal_index - 1], STRING_CHAR_PTR_SENTINEL));
                        freeValue(&operand);
                        break; // Exit the case
                    }
                }

                int* indices = malloc(sizeof(int) * dimension_count);
                if (!indices) { runtimeError(vm, "VM Error: Malloc failed for array indices."); freeValue(&operand); return INTERPRET_RUNTIME_ERROR; }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (isIntlikeType(index_val.type)) {
                        indices[dimension_count - 1 - i] = (int)index_val.i_val;
                    } else if (isRealType(index_val.type)) {
                        indices[dimension_count - 1 - i] = (int)AS_REAL(index_val);
                    } else {
                        runtimeError(vm, "VM Error: Array index must be an integer.");
                        free(indices); freeValue(&index_val); freeValue(&operand); return INTERPRET_RUNTIME_ERROR;
                    }
                    freeValue(&index_val);
                }

                Value* array_val_ptr = NULL;
                Value temp_wrapper;
                bool using_wrapper = false;

                if (operand.type == TYPE_POINTER) {
                    Value* candidate = (Value*)operand.ptr_val;
                    if (candidate && candidate->type == TYPE_ARRAY) {
                        array_val_ptr = candidate;
                    } else if (operand.base_type_node && operand.base_type_node->type == AST_ARRAY_TYPE) {
                        AST* arrayType = operand.base_type_node;
                        int dims = arrayType->child_count;
                        temp_wrapper.type = TYPE_ARRAY;
                        temp_wrapper.dimensions = dims;
                        temp_wrapper.array_val = (Value*)operand.ptr_val;
                        temp_wrapper.lower_bounds = malloc(sizeof(int) * dims);
                        temp_wrapper.upper_bounds = malloc(sizeof(int) * dims);
                        if (!temp_wrapper.lower_bounds || !temp_wrapper.upper_bounds) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            if (temp_wrapper.lower_bounds) free(temp_wrapper.lower_bounds);
                            if (temp_wrapper.upper_bounds) free(temp_wrapper.upper_bounds);
                            free(indices);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && sub->type == AST_SUBRANGE && sub->left && sub->right) {
                                lb = sub->left->i_val;
                                ub = sub->right->i_val;
                            }
                            temp_wrapper.lower_bounds[i] = lb;
                            temp_wrapper.upper_bounds[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (operand.type == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    free(indices);
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int offset = computeFlatOffset(array_val_ptr, indices);
                free(indices);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (offset < 0 || offset >= total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    freeValue(&operand);
                    if (using_wrapper) {
                        free(temp_wrapper.lower_bounds);
                        free(temp_wrapper.upper_bounds);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makePointer(&array_val_ptr->array_val[offset], NULL));

                if (operand.type == TYPE_POINTER) {
                    freeValue(&operand);
                }
                if (using_wrapper) {
                    free(temp_wrapper.lower_bounds);
                    free(temp_wrapper.upper_bounds);
                }

                break;
            }
            case GET_ELEMENT_ADDRESS_CONST: {
                uint32_t flat_offset = READ_UINT32(vm);
                Value operand = pop(vm);

                Value* array_val_ptr = NULL;
                Value temp_wrapper;
                temp_wrapper.lower_bounds = NULL;
                temp_wrapper.upper_bounds = NULL;
                bool using_wrapper = false;

                if (operand.type == TYPE_POINTER) {
                    Value* candidate = (Value*)operand.ptr_val;
                    if (candidate && candidate->type == TYPE_ARRAY) {
                        array_val_ptr = candidate;
                    } else if (operand.base_type_node && operand.base_type_node->type == AST_ARRAY_TYPE) {
                        AST* arrayType = operand.base_type_node;
                        int dims = arrayType->child_count;
                        temp_wrapper.type = TYPE_ARRAY;
                        temp_wrapper.dimensions = dims;
                        temp_wrapper.array_val = (Value*)operand.ptr_val;
                        temp_wrapper.lower_bounds = malloc(sizeof(int) * dims);
                        temp_wrapper.upper_bounds = malloc(sizeof(int) * dims);
                        if (!temp_wrapper.lower_bounds || !temp_wrapper.upper_bounds) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            if (temp_wrapper.lower_bounds) free(temp_wrapper.lower_bounds);
                            if (temp_wrapper.upper_bounds) free(temp_wrapper.upper_bounds);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && sub->type == AST_SUBRANGE && sub->left && sub->right) {
                                lb = sub->left->i_val;
                                ub = sub->right->i_val;
                            }
                            temp_wrapper.lower_bounds[i] = lb;
                            temp_wrapper.upper_bounds[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (operand.type == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (flat_offset >= (uint32_t)total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    if (operand.type == TYPE_POINTER) {
                        freeValue(&operand);
                    }
                    if (using_wrapper) {
                        free(temp_wrapper.lower_bounds);
                        free(temp_wrapper.upper_bounds);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makePointer(&array_val_ptr->array_val[flat_offset], NULL));

                if (operand.type == TYPE_POINTER) {
                    freeValue(&operand);
                }
                if (using_wrapper) {
                    free(temp_wrapper.lower_bounds);
                    free(temp_wrapper.upper_bounds);
                }

                break;
            }
            case LOAD_ELEMENT_VALUE: {
                uint8_t dimension_count = READ_BYTE();

                Value operand = pop(vm);

                if (dimension_count == 1 && operand.type == TYPE_POINTER) {
                    Value* base_val = (Value*)operand.ptr_val;
                    if (base_val && base_val->type == TYPE_STRING) {
                        Value index_val = pop(vm);
                        if (!isIntlikeType(index_val.type)) {
                            runtimeError(vm, "VM Error: String index must be an integer.");
                            freeValue(&index_val);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        long long pscal_index = index_val.i_val;
                        freeValue(&index_val);

                        size_t len = base_val->s_val ? strlen(base_val->s_val) : 0;
                        if (pscal_index == 0) {
                            push(vm, makeInt((long long)len));
                            freeValue(&operand);
                            break;
                        }
                        if (pscal_index < 1 || (size_t)pscal_index > len) {
                            runtimeError(vm,
                                         "Runtime Error: String index (%lld) out of bounds for string of length %zu.",
                                         pscal_index, len);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }

                        char ch = base_val->s_val ? base_val->s_val[pscal_index - 1] : '\0';
                        push(vm, makeChar(ch));
                        freeValue(&operand);
                        break;
                    }
                }

                int* indices = malloc(sizeof(int) * dimension_count);
                if (!indices) {
                    runtimeError(vm, "VM Error: Malloc failed for array indices.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < dimension_count; i++) {
                    Value index_val = pop(vm);
                    if (isIntlikeType(index_val.type)) {
                        indices[dimension_count - 1 - i] = (int)index_val.i_val;
                    } else if (isRealType(index_val.type)) {
                        indices[dimension_count - 1 - i] = (int)AS_REAL(index_val);
                    } else {
                        runtimeError(vm, "VM Error: Array index must be an integer.");
                        free(indices);
                        freeValue(&index_val);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    freeValue(&index_val);
                }

                Value* array_val_ptr = NULL;
                Value temp_wrapper;
                temp_wrapper.lower_bounds = NULL;
                temp_wrapper.upper_bounds = NULL;
                bool using_wrapper = false;

                if (operand.type == TYPE_POINTER) {
                    Value* candidate = (Value*)operand.ptr_val;
                    if (candidate && candidate->type == TYPE_ARRAY) {
                        array_val_ptr = candidate;
                    } else if (operand.base_type_node && operand.base_type_node->type == AST_ARRAY_TYPE) {
                        AST* arrayType = operand.base_type_node;
                        int dims = arrayType->child_count;
                        temp_wrapper.type = TYPE_ARRAY;
                        temp_wrapper.dimensions = dims;
                        temp_wrapper.array_val = (Value*)operand.ptr_val;
                        temp_wrapper.lower_bounds = malloc(sizeof(int) * dims);
                        temp_wrapper.upper_bounds = malloc(sizeof(int) * dims);
                        if (!temp_wrapper.lower_bounds || !temp_wrapper.upper_bounds) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            if (temp_wrapper.lower_bounds) free(temp_wrapper.lower_bounds);
                            if (temp_wrapper.upper_bounds) free(temp_wrapper.upper_bounds);
                            free(indices);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && sub->type == AST_SUBRANGE && sub->left && sub->right) {
                                lb = sub->left->i_val;
                                ub = sub->right->i_val;
                            }
                            temp_wrapper.lower_bounds[i] = lb;
                            temp_wrapper.upper_bounds[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        free(indices);
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (operand.type == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    free(indices);
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int offset = computeFlatOffset(array_val_ptr, indices);
                free(indices);

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (offset < 0 || offset >= total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    if (operand.type == TYPE_POINTER) freeValue(&operand);
                    if (using_wrapper) {
                        free(temp_wrapper.lower_bounds);
                        free(temp_wrapper.upper_bounds);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, copyValueForStack(&array_val_ptr->array_val[offset]));

                freeValue(&operand);
                if (using_wrapper) {
                    free(temp_wrapper.lower_bounds);
                    free(temp_wrapper.upper_bounds);
                }

                break;
            }
            case LOAD_ELEMENT_VALUE_CONST: {
                uint32_t flat_offset = READ_UINT32(vm);

                Value operand = pop(vm);

                Value* array_val_ptr = NULL;
                Value temp_wrapper;
                temp_wrapper.lower_bounds = NULL;
                temp_wrapper.upper_bounds = NULL;
                bool using_wrapper = false;

                if (operand.type == TYPE_POINTER) {
                    Value* candidate = (Value*)operand.ptr_val;
                    if (candidate && candidate->type == TYPE_ARRAY) {
                        array_val_ptr = candidate;
                    } else if (operand.base_type_node && operand.base_type_node->type == AST_ARRAY_TYPE) {
                        AST* arrayType = operand.base_type_node;
                        int dims = arrayType->child_count;
                        temp_wrapper.type = TYPE_ARRAY;
                        temp_wrapper.dimensions = dims;
                        temp_wrapper.array_val = (Value*)operand.ptr_val;
                        temp_wrapper.lower_bounds = malloc(sizeof(int) * dims);
                        temp_wrapper.upper_bounds = malloc(sizeof(int) * dims);
                        if (!temp_wrapper.lower_bounds || !temp_wrapper.upper_bounds) {
                            runtimeError(vm, "VM Error: Malloc failed for temporary array wrapper bounds.");
                            if (temp_wrapper.lower_bounds) free(temp_wrapper.lower_bounds);
                            if (temp_wrapper.upper_bounds) free(temp_wrapper.upper_bounds);
                            freeValue(&operand);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        for (int i = 0; i < dims; i++) {
                            int lb = 0, ub = -1;
                            AST* sub = arrayType->children[i];
                            if (sub && sub->type == AST_SUBRANGE && sub->left && sub->right) {
                                lb = sub->left->i_val;
                                ub = sub->right->i_val;
                            }
                            temp_wrapper.lower_bounds[i] = lb;
                            temp_wrapper.upper_bounds[i] = ub;
                        }
                        array_val_ptr = &temp_wrapper;
                        using_wrapper = true;
                    } else {
                        runtimeError(vm, "VM Error: Pointer does not point to an array for element access.");
                        freeValue(&operand);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (operand.type == TYPE_ARRAY) {
                    array_val_ptr = &operand;
                } else {
                    runtimeError(vm, "VM Error: Expected a pointer to an array for element access.");
                    freeValue(&operand);
                    return INTERPRET_RUNTIME_ERROR;
                }

                int total_size = calculateArrayTotalSize(array_val_ptr);
                if (flat_offset >= (uint32_t)total_size) {
                    runtimeError(vm, "VM Error: Array element index out of bounds.");
                    freeValue(&operand);
                    if (using_wrapper) {
                        free(temp_wrapper.lower_bounds);
                        free(temp_wrapper.upper_bounds);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, copyValueForStack(&array_val_ptr->array_val[flat_offset]));

                freeValue(&operand);
                if (using_wrapper) {
                    free(temp_wrapper.lower_bounds);
                    free(temp_wrapper.upper_bounds);
                }

                break;
            }
            case SET_INDIRECT: {
                Value value_to_set = pop(vm);
                Value pointer_to_lvalue = pop(vm);

                if (pointer_to_lvalue.type != TYPE_POINTER) {
                    runtimeError(vm, "VM Error: SET_INDIRECT requires an address on the stack.");
                    freeValue(&value_to_set);
                    freeValue(&pointer_to_lvalue);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (pointer_to_lvalue.base_type_node == STRING_CHAR_PTR_SENTINEL) {
                    char* char_target_addr = (char*)pointer_to_lvalue.ptr_val;
                    if (char_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to assign to a NULL character address.");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    if (value_to_set.type == TYPE_CHAR) {
                        *char_target_addr = value_to_set.c_val;
                    } else if (value_to_set.type == TYPE_STRING) {
                        if (value_to_set.s_val && strlen(value_to_set.s_val) == 1) {
                            *char_target_addr = value_to_set.s_val[0];
                        } else {
                            runtimeError(vm, "VM Error: Cannot assign multi-character or empty string to a single character location.");
                            freeValue(&value_to_set);
                            freeValue(&pointer_to_lvalue);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                    } else {
                        runtimeError(vm, "VM Error: Type mismatch for character assignment. Expected CHAR or single-char STRING, got %s.", varTypeToString(value_to_set.type));
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (pointer_to_lvalue.base_type_node == STRING_LENGTH_SENTINEL) {
                    runtimeError(vm, "VM Error: Cannot assign to string length.");
                    freeValue(&value_to_set);
                    freeValue(&pointer_to_lvalue);
                    return INTERPRET_RUNTIME_ERROR;
                } else { // This is the start of your existing logic for other types
                    Value* target_lvalue_ptr = (Value*)pointer_to_lvalue.ptr_val;
                    if (!target_lvalue_ptr) {
                        runtimeError(vm, "VM Error: SET_INDIRECT called with a nil LValue pointer.");
                        freeValue(&value_to_set);
                        freeValue(&pointer_to_lvalue);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    // (Your existing logic for handling fixed-length strings, pointers, reals, etc. goes here)
                    if (target_lvalue_ptr->type == TYPE_STRING && target_lvalue_ptr->max_length <= 0) {
                        if (value_to_set.type == TYPE_CHAR) {
                            freeValue(target_lvalue_ptr);
                            target_lvalue_ptr->s_val = (char*)malloc(2);
                            if (!target_lvalue_ptr->s_val) {
                                runtimeError(vm, "VM Error: Malloc failed for CHAR to STRING assignment.");
                            } else {
                                target_lvalue_ptr->s_val[0] = value_to_set.c_val;
                                target_lvalue_ptr->s_val[1] = '\0';
                            }
                            target_lvalue_ptr->type = TYPE_STRING;
                            target_lvalue_ptr->max_length = -1;
                        } else if (value_to_set.type == TYPE_STRING && value_to_set.s_val) {
                            freeValue(target_lvalue_ptr);
                            target_lvalue_ptr->s_val = strdup(value_to_set.s_val);
                            if (!target_lvalue_ptr->s_val) {
                                runtimeError(vm, "VM Error: strdup failed for string assignment.");
                                target_lvalue_ptr->s_val = NULL;
                            }
                            target_lvalue_ptr->type = TYPE_STRING;
                            target_lvalue_ptr->max_length = -1;
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign this type to a dynamic string.");
                        }
                    }
                    else if (target_lvalue_ptr->type == TYPE_STRING && target_lvalue_ptr->max_length > 0) {
                        if (value_to_set.type == TYPE_STRING && value_to_set.s_val) {
                            strncpy(target_lvalue_ptr->s_val, value_to_set.s_val, target_lvalue_ptr->max_length);
                            target_lvalue_ptr->s_val[target_lvalue_ptr->max_length] = '\0'; // Ensure null termination
                        } else if (value_to_set.type == TYPE_CHAR) {
                            target_lvalue_ptr->s_val[0] = value_to_set.c_val;
                            target_lvalue_ptr->s_val[1] = '\0';
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign this type to a fixed-length string.");
                        }
                    }
                    else if (target_lvalue_ptr->type == TYPE_POINTER && (value_to_set.type == TYPE_POINTER || value_to_set.type == TYPE_NIL)) {
                        if (value_to_set.type == TYPE_NIL) {
                            // Preserve base type when assigning nil
                            target_lvalue_ptr->ptr_val = NULL;
                        } else {
                            target_lvalue_ptr->ptr_val = value_to_set.ptr_val;
                            if (value_to_set.base_type_node) {
                                target_lvalue_ptr->base_type_node = value_to_set.base_type_node;
                            }
                        }
                    }
                    else if (isRealType(target_lvalue_ptr->type) && isRealType(value_to_set.type)) {
                        long double tmp = AS_REAL(value_to_set);
                        SET_REAL_VALUE(target_lvalue_ptr, tmp);
                    }
                    else if (isRealType(target_lvalue_ptr->type) && isIntlikeType(value_to_set.type)) {
                        long double tmp = asLd(value_to_set);
                        SET_REAL_VALUE(target_lvalue_ptr, tmp);
                    }
                    else if (isIntlikeType(target_lvalue_ptr->type) && isRealType(value_to_set.type)) {
                        assignRealToIntChecked(vm, target_lvalue_ptr, AS_REAL(value_to_set));
                    }
                    else if (target_lvalue_ptr->type == TYPE_BYTE && value_to_set.type == TYPE_INTEGER) {
                        if (value_to_set.i_val < 0 || value_to_set.i_val > 255) {
                            runtimeError(vm, "Warning: Range check error assigning INTEGER %lld to BYTE.", value_to_set.i_val);
                        }
                        SET_INT_VALUE(target_lvalue_ptr, value_to_set.i_val & 0xFF);
                    }
                    else if (target_lvalue_ptr->type == TYPE_WORD && value_to_set.type == TYPE_INTEGER) {
                        if (value_to_set.i_val < 0 || value_to_set.i_val > 65535) {
                            runtimeError(vm, "Warning: Range check error assigning INTEGER %lld to WORD.", value_to_set.i_val);
                        }
                        SET_INT_VALUE(target_lvalue_ptr, value_to_set.i_val & 0xFFFF);
                    }
                    else if (target_lvalue_ptr->type == TYPE_INTEGER &&
                             (value_to_set.type == TYPE_BYTE || value_to_set.type == TYPE_WORD || value_to_set.type == TYPE_BOOLEAN)) {
                        SET_INT_VALUE(target_lvalue_ptr, value_to_set.i_val);
                    }
                    else if (target_lvalue_ptr->type == TYPE_INTEGER && value_to_set.type == TYPE_CHAR) {
                        SET_INT_VALUE(target_lvalue_ptr, (long long)value_to_set.c_val);
                    }
                    else if (target_lvalue_ptr->type == TYPE_CHAR) {
                        if (value_to_set.type == TYPE_CHAR) {
                            target_lvalue_ptr->c_val = value_to_set.c_val;
                        } else if (value_to_set.type == TYPE_STRING && value_to_set.s_val) {
                            size_t len = strlen(value_to_set.s_val);
                            if (len == 1) {
                                target_lvalue_ptr->c_val = (unsigned char)value_to_set.s_val[0];
                            } else if (len == 0) {
                                target_lvalue_ptr->c_val = '\0';
                            } else {
                                runtimeError(vm, "Type mismatch: Cannot assign multi-character string to CHAR.");
                            }
                        } else if (value_to_set.type == TYPE_INTEGER) {
                            target_lvalue_ptr->c_val = (int)value_to_set.i_val;
                        } else {
                            runtimeError(vm, "Type mismatch: Cannot assign %s to CHAR.", varTypeToString(value_to_set.type));
                        }
                        SET_INT_VALUE(target_lvalue_ptr, target_lvalue_ptr->c_val);
                    }
                    else {
                        freeValue(target_lvalue_ptr);
                        if (value_to_set.type == TYPE_MEMORYSTREAM) {
                            /* Transfer ownership of the MStream pointer without freeing it
                             * when the temporary value is cleaned up below. */
                            *target_lvalue_ptr = value_to_set;
                            value_to_set.mstream = NULL;
                        } else {
                            *target_lvalue_ptr = makeCopyOfValue(&value_to_set);
                        }
                    }
                }

                /*
                 * In Pascal, assignments are statements and do not yield a
                 * value.  The previous implementation pushed a copy of the
                 * assigned value onto the stack, mirroring C-like semantics.
                 * This resulted in stray values accumulating on the VM stack
                 * for every indirect assignment (e.g. array or record field
                 * writes).  Large programs such as the SDL multi bouncing balls
                 * demo perform many such assignments each frame, eventually
                 * exhausting the stack and triggering a runtime stack overflow.
                 *
                 * To restore correct Pascal semantics and prevent the leak,
                 * simply discard the temporary value after storing it.
                 */
                freeValue(&value_to_set);
                freeValue(&pointer_to_lvalue);
                break;
            }
            case IN: {
                Value set_val = pop(vm);
                Value item_val = pop(vm);

                if (set_val.type != TYPE_SET) {
                    runtimeError(vm, "Right operand of IN must be a set.");
                    freeValue(&item_val);
                    freeValue(&set_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                bool result = vmSetContains(&set_val, &item_val);

                freeValue(&item_val);
                freeValue(&set_val);

                push(vm, makeBoolean(result));
                break;
            }

            case GET_INDIRECT: {
                Value pointer_val = pop(vm);
                if (pointer_val.type != TYPE_POINTER) {
                    runtimeError(vm, "VM Error: GET_INDIRECT requires an address on the stack.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (pointer_val.base_type_node == STRING_CHAR_PTR_SENTINEL) {
                    // Special case: pointer into a string's character buffer.
                    char* char_target_addr = (char*)pointer_val.ptr_val;
                    if (char_target_addr == NULL) {
                        runtimeError(vm, "VM Error: Attempting to dereference a NULL character address.");
                        freeValue(&pointer_val);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, makeChar(*char_target_addr));
                } else if (pointer_val.base_type_node == STRING_LENGTH_SENTINEL) {
                    // Special case: request for string length via element 0.
                    Value* str_val = (Value*)pointer_val.ptr_val;
                    size_t len = (str_val && str_val->s_val) ? strlen(str_val->s_val) : 0;
                    push(vm, makeInt((long long)len));
                } else {
                    Value* target_lvalue_ptr = (Value*)pointer_val.ptr_val;
                    if (target_lvalue_ptr == NULL) {
                        runtimeError(vm, "VM Error: GET_INDIRECT on a nil pointer.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    push(vm, copyValueForStack(target_lvalue_ptr));
                }
                freeValue(&pointer_val);
                break;
            }

            case GET_CHAR_FROM_STRING: {
                 Value index_val = pop(vm);
                 Value base_val = pop(vm); // Can be string or char

                 if (!isIntlikeType(index_val.type)) {
                     runtimeError(vm, "VM Error: String/Char index must be an integer.");
                     freeValue(&index_val); freeValue(&base_val);
                     return INTERPRET_RUNTIME_ERROR;
                 }

                 long long pscal_index = index_val.i_val;
                 char result_char;

                 if (base_val.type == TYPE_STRING) {
                     const char* str = base_val.s_val ? base_val.s_val : "";
                     size_t len = strlen(str);
                     if (pscal_index < 1 || (size_t)pscal_index > len) {
                         runtimeError(vm, "Runtime Error: String index (%lld) out of bounds [1..%zu].", pscal_index, len);
                         freeValue(&index_val); freeValue(&base_val);
                         return INTERPRET_RUNTIME_ERROR;
                     }
                     result_char = str[pscal_index - 1];
                 } else if (base_val.type == TYPE_CHAR) {
                     if (pscal_index != 1) {
                         runtimeError(vm, "Runtime Error: Index for a CHAR type must be 1, got %lld.", pscal_index);
                         freeValue(&index_val); freeValue(&base_val);
                         return INTERPRET_RUNTIME_ERROR;
                     }
                     result_char = base_val.c_val;
                 } else {
                     runtimeError(vm, "VM Error: Base for character index is not a string or char. Got %s", varTypeToString(base_val.type));
                     freeValue(&index_val); freeValue(&base_val);
                     return INTERPRET_RUNTIME_ERROR;
                 }

                 push(vm, makeChar(result_char));

                 freeValue(&index_val);
                 freeValue(&base_val);
                 break;
             }
            case DEFINE_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobal(vm, varNameVal);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            case DEFINE_GLOBAL16: {
                Value varNameVal = READ_CONSTANT16();
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobal(vm, varNameVal);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            case GET_GLOBAL: {
                uint8_t name_idx = READ_BYTE();
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "Runtime Error: Invalid global name.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* sym = NULL;
                if (vm->vmConstGlobalSymbols) {
                    sym = hashTableLookup(vm->vmConstGlobalSymbols, name_val->s_val);
                    if (sym && sym->value) {
                        push(vm, copyValueForStack(sym->value));
                        break;
                    }
                }

                pthread_mutex_lock(&globals_mutex);
                sym = hashTableLookup(vm->vmGlobalSymbols, name_val->s_val);
                pthread_mutex_unlock(&globals_mutex);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", name_val->s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, copyValueForStack(sym->value));
                break;
            }
            case GET_GLOBAL16: {
                uint16_t name_idx = READ_SHORT(vm);
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for GET_GLOBAL16.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "Runtime Error: Invalid global name.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* sym = NULL;
                if (vm->vmConstGlobalSymbols) {
                    sym = hashTableLookup(vm->vmConstGlobalSymbols, name_val->s_val);
                    if (sym && sym->value) {
                        push(vm, copyValueForStack(sym->value));
                        break;
                    }
                }

                pthread_mutex_lock(&globals_mutex);
                sym = hashTableLookup(vm->vmGlobalSymbols, name_val->s_val);
                pthread_mutex_unlock(&globals_mutex);
                if (!sym || !sym->value) {
                    runtimeError(vm, "Runtime Error: Undefined global variable '%s'.", name_val->s_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, copyValueForStack(sym->value));
                break;
            }
            case SET_GLOBAL: {
                uint8_t name_idx = READ_BYTE();
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for SET_GLOBAL.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "Runtime Error: Invalid global variable name.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                pthread_mutex_lock(&globals_mutex);
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, name_val->s_val);
                if (!sym) {
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", name_val->s_val);
                    pthread_mutex_unlock(&globals_mutex);
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

                Value value_from_stack = pop(vm);
                updateSymbol(name_val->s_val, value_from_stack);
                pthread_mutex_unlock(&globals_mutex);
                break;
            }
            case SET_GLOBAL16: {
                uint16_t name_idx = READ_SHORT(vm);
                if (name_idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Name constant index %u out of bounds for SET_GLOBAL16.", name_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_idx];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "Runtime Error: Invalid global variable name.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                pthread_mutex_lock(&globals_mutex);
                Symbol* sym = hashTableLookup(vm->vmGlobalSymbols, name_val->s_val);
                if (!sym) {
                    runtimeError(vm, "Runtime Error: Global variable '%s' not defined for assignment.", name_val->s_val);
                    pthread_mutex_unlock(&globals_mutex);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!sym->value) {
                    sym->value = (Value*)malloc(sizeof(Value));
                    if (!sym->value) {
                        runtimeError(vm, "VM Error: Malloc failed for symbol value in SET_GLOBAL16.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    *(sym->value) = makeValueForType(sym->type, sym->type_def, sym);
                }

                Value value_from_stack = pop(vm);
                updateSymbol(name_val->s_val, value_from_stack);
                pthread_mutex_unlock(&globals_mutex);
                break;
            }
            case GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(&frame->slots[slot]));
                break;
            }
            case SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                Value value_from_stack = pop(vm);

                // --- START CORRECTED LOGIC ---
                if (target_slot->type == TYPE_POINTER && value_from_stack.type == TYPE_NIL) {
                    // Assigning nil to a pointer variable preserves its base type and type
                    target_slot->ptr_val = NULL;
                    // type and base_type_node remain unchanged
                } else if (target_slot->type == TYPE_STRING && target_slot->max_length > 0) {
                    // Special case: Assignment to a fixed-length string.
                    const char* source_str = "";
                    char char_buf[2] = {0};

                    if (value_from_stack.type == TYPE_STRING && value_from_stack.s_val) {
                        source_str = value_from_stack.s_val;
                    } else if (value_from_stack.type == TYPE_CHAR) {
                        char_buf[0] = value_from_stack.c_val;
                        source_str = char_buf;
                    }
                    
                    strncpy(target_slot->s_val, source_str, target_slot->max_length);
                    target_slot->s_val[target_slot->max_length] = '\0';

                } else if (isRealType(target_slot->type)) {
                    if (isRealType(value_from_stack.type)) {
                        long double tmp = AS_REAL(value_from_stack);
                        SET_REAL_VALUE(target_slot, tmp);
                    } else if (isIntlikeType(value_from_stack.type)) {
                        long double tmp = asLd(value_from_stack);
                        SET_REAL_VALUE(target_slot, tmp);
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to real.", varTypeToString(value_from_stack.type));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (isIntlikeType(target_slot->type)) {
                    if (IS_NUMERIC(value_from_stack)) {
                        if (isRealType(value_from_stack.type)) {
                            assignRealToIntChecked(vm, target_slot, AS_REAL(value_from_stack));
                        } else {
                            long long tmp = asI64(value_from_stack);
                            if (target_slot->type == TYPE_BOOLEAN) tmp = (tmp != 0) ? 1 : 0;
                            SET_INT_VALUE(target_slot, tmp);
                            if (target_slot->type == TYPE_CHAR) target_slot->c_val = (int)tmp;
                        }
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to integer.",
                                     varTypeToString(value_from_stack.type));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    // This is the logic for all other types, including dynamic strings,
                    // numbers, records, etc., which requires a deep copy.
                    AST* preserved_base = target_slot->base_type_node;
                    freeValue(target_slot);
                    *target_slot = makeCopyOfValue(&value_from_stack);
                    if (target_slot->type == TYPE_POINTER && target_slot->base_type_node == NULL) {
                        target_slot->base_type_node = preserved_base;
                    }
                }
                // --- END CORRECTED LOGIC ---
                #ifdef DEBUG
                if (target_slot->type == TYPE_POINTER) {
                    fprintf(stderr,
                            "[DEBUG set_local] slot %u ptr=%p base=%p (%s) val=%p\n",
                            (unsigned)slot,
                            (void*)target_slot,
                            (void*)target_slot->base_type_node,
                            target_slot->base_type_node ? astTypeToString(target_slot->base_type_node->type) : "NULL",
                            target_slot->ptr_val);
                }
                #endif

                // Free the temporary value that was popped from the stack.
                freeValue(&value_from_stack);
                break;
            }
            case INC_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                if (!adjustLocalByDelta(vm, target_slot, 1, "INC_LOCAL")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case DEC_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                size_t declared_window = frame->slotCount;
                size_t live_window = (size_t)(vm->stackTop - frame->slots);
                size_t frame_window = declared_window ? declared_window : live_window;
                if (slot >= frame_window) {
                    runtimeError(vm, "VM Error: Local slot index %u out of range (declared window=%zu, live window=%zu).",
                                 slot, declared_window, live_window);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = &frame->slots[slot];
                if (!adjustLocalByDelta(vm, target_slot, -1, "DEC_LOCAL")) {
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, copyValueForStack(frame->upvalues[slot]));
                break;
            }
            case SET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value* target_slot = frame->upvalues[slot];
                Value value_from_stack = pop(vm);

                if (target_slot->type == TYPE_POINTER && value_from_stack.type == TYPE_NIL) {
                    // Preserve pointer metadata when assigning NIL.
                    target_slot->ptr_val = NULL;
                } else if (target_slot->type == TYPE_STRING && target_slot->max_length > 0) {
                    const char* source_str = "";
                    char char_buf[2] = {0};
                    if (value_from_stack.type == TYPE_STRING && value_from_stack.s_val) {
                        source_str = value_from_stack.s_val;
                    } else if (value_from_stack.type == TYPE_CHAR) {
                        char_buf[0] = value_from_stack.c_val;
                        source_str = char_buf;
                    }
                    strncpy(target_slot->s_val, source_str, target_slot->max_length);
                    target_slot->s_val[target_slot->max_length] = '\0';
                } else if (isRealType(target_slot->type)) {
                    if (IS_NUMERIC(value_from_stack)) {
                        long double tmp = asLd(value_from_stack);
                        SET_REAL_VALUE(target_slot, tmp);
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to real.", varTypeToString(value_from_stack.type));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else if (isIntlikeType(target_slot->type)) {
                    if (IS_NUMERIC(value_from_stack)) {
                        if (isRealType(value_from_stack.type)) {
                            assignRealToIntChecked(vm, target_slot, AS_REAL(value_from_stack));
                        } else {
                            long long tmp = asI64(value_from_stack);
                            if (target_slot->type == TYPE_BOOLEAN) tmp = (tmp != 0) ? 1 : 0;
                            SET_INT_VALUE(target_slot, tmp);
                            if (target_slot->type == TYPE_CHAR) target_slot->c_val = (int)tmp;
                        }
                    } else {
                        runtimeError(vm, "Type mismatch: Cannot assign %s to integer.",
                                     varTypeToString(value_from_stack.type));
                        freeValue(&value_from_stack);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                } else {
                    AST* preserved_base = target_slot->base_type_node;
                    freeValue(target_slot);
                    *target_slot = makeCopyOfValue(&value_from_stack);
                    if (target_slot->type == TYPE_POINTER && target_slot->base_type_node == NULL) {
                        target_slot->base_type_node = preserved_base;
                    }
                }
                freeValue(&value_from_stack);
                break;
            }
            case GET_UPVALUE_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makePointer(frame->upvalues[slot], NULL));
                break;
            }
            case INIT_LOCAL_ARRAY: {
                uint8_t slot = READ_BYTE();
                uint8_t dimension_count = READ_BYTE();
                if (dimension_count == 0) {
                    runtimeError(vm, "VM Error: Array defined with zero dimensions for local variable.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                uint16_t *lower_idx = malloc(sizeof(uint16_t) * dimension_count);
                uint16_t *upper_idx = malloc(sizeof(uint16_t) * dimension_count);
                if (!lower_idx || !upper_idx) {
                    runtimeError(vm, "VM Error: Malloc failed for array bound indices.");
                    if (lower_idx) free(lower_idx);
                    if (upper_idx) free(upper_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < dimension_count; i++) {
                    lower_idx[i] = READ_SHORT(vm);
                    upper_idx[i] = READ_SHORT(vm);
                }

                VarType elem_var_type = (VarType)READ_BYTE();
                uint8_t elem_name_idx = READ_BYTE();
                Value elem_name_val = vm->chunk->constants[elem_name_idx];
                AST* elem_type_def = NULL;
                if (elem_name_val.type == TYPE_STRING && elem_name_val.s_val && elem_name_val.s_val[0] != '\0') {
                    elem_type_def = lookupType(elem_name_val.s_val);
                }

                int* lower_bounds = malloc(sizeof(int) * dimension_count);
                int* upper_bounds = malloc(sizeof(int) * dimension_count);
                if (!lower_bounds || !upper_bounds) {
                    runtimeError(vm, "VM Error: Malloc failed for array bounds construction.");
                    free(lower_idx); free(upper_idx);
                    if (lower_bounds) free(lower_bounds);
                    if (upper_bounds) free(upper_bounds);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = dimension_count - 1; i >= 0; i--) {
                    if (lower_idx[i] == 0xFFFF && upper_idx[i] == 0xFFFF) {
                        Value size_val = pop(vm);
                        if (!isIntlikeType(size_val.type)) {
                            runtimeError(vm, "VM Error: Array size expression did not evaluate to an integer.");
                            free(lower_bounds); free(upper_bounds);
                            free(lower_idx); free(upper_idx);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        lower_bounds[i] = 0;
                        upper_bounds[i] = (int)size_val.i_val - 1;
                        freeValue(&size_val);
                    } else {
                        if (lower_idx[i] >= vm->chunk->constants_count || upper_idx[i] >= vm->chunk->constants_count) {
                            runtimeError(vm, "VM Error: Array bound constant index out of range.");
                            free(lower_bounds); free(upper_bounds);
                            free(lower_idx); free(upper_idx);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        Value lower_val = vm->chunk->constants[lower_idx[i]];
                        Value upper_val = vm->chunk->constants[upper_idx[i]];
                        if (!isIntlikeType(lower_val.type) || !isIntlikeType(upper_val.type)) {
                            runtimeError(vm, "VM Error: Invalid constant types for array bounds.");
                            free(lower_bounds); free(upper_bounds);
                            free(lower_idx); free(upper_idx);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        lower_bounds[i] = (int)lower_val.i_val;
                        upper_bounds[i] = (int)upper_val.i_val;
                    }
                }

                free(lower_idx);
                free(upper_idx);

                Value array_val = makeArrayND(dimension_count, lower_bounds, upper_bounds, elem_var_type, elem_type_def);

                free(lower_bounds);
                free(upper_bounds);

                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                *target_slot = array_val;
                break;
            }
            case INIT_FIELD_ARRAY: {
                uint8_t field_index = READ_BYTE();
                uint8_t dimension_count = READ_BYTE();
                if (dimension_count == 0) {
                    runtimeError(vm, "VM Error: Array defined with zero dimensions for object field.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                uint16_t *lower_idx = malloc(sizeof(uint16_t) * dimension_count);
                uint16_t *upper_idx = malloc(sizeof(uint16_t) * dimension_count);
                if (!lower_idx || !upper_idx) {
                    runtimeError(vm, "VM Error: Malloc failed for array bound indices.");
                    if (lower_idx) free(lower_idx);
                    if (upper_idx) free(upper_idx);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = 0; i < dimension_count; i++) {
                    lower_idx[i] = READ_SHORT(vm);
                    upper_idx[i] = READ_SHORT(vm);
                }

                VarType elem_var_type = (VarType)READ_BYTE();
                uint8_t elem_name_idx = READ_BYTE();
                Value elem_name_val = vm->chunk->constants[elem_name_idx];
                AST* elem_type_def = NULL;
                if (elem_name_val.type == TYPE_STRING && elem_name_val.s_val && elem_name_val.s_val[0] != '\0') {
                    elem_type_def = lookupType(elem_name_val.s_val);
                }

                int* lower_bounds = malloc(sizeof(int) * dimension_count);
                int* upper_bounds = malloc(sizeof(int) * dimension_count);
                if (!lower_bounds || !upper_bounds) {
                    runtimeError(vm, "VM Error: Malloc failed for array bounds construction.");
                    free(lower_idx); free(upper_idx);
                    if (lower_bounds) free(lower_bounds);
                    if (upper_bounds) free(upper_bounds);
                    return INTERPRET_RUNTIME_ERROR;
                }

                for (int i = dimension_count - 1; i >= 0; i--) {
                    if (lower_idx[i] == 0xFFFF && upper_idx[i] == 0xFFFF) {
                        Value size_val = pop(vm);
                        if (!isIntlikeType(size_val.type)) {
                            runtimeError(vm, "VM Error: Array size expression did not evaluate to an integer.");
                            free(lower_bounds); free(upper_bounds);
                            free(lower_idx); free(upper_idx);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        lower_bounds[i] = 0;
                        upper_bounds[i] = (int)size_val.i_val - 1;
                        freeValue(&size_val);
                    } else {
                        if (lower_idx[i] >= vm->chunk->constants_count || upper_idx[i] >= vm->chunk->constants_count) {
                            runtimeError(vm, "VM Error: Array bound constant index out of range.");
                            free(lower_bounds); free(upper_bounds);
                            free(lower_idx); free(upper_idx);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        Value lower_val = vm->chunk->constants[lower_idx[i]];
                        Value upper_val = vm->chunk->constants[upper_idx[i]];
                        if (!isIntlikeType(lower_val.type) || !isIntlikeType(upper_val.type)) {
                            runtimeError(vm, "VM Error: Invalid constant types for array bounds.");
                            free(lower_bounds); free(upper_bounds);
                            free(lower_idx); free(upper_idx);
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        lower_bounds[i] = (int)lower_val.i_val;
                        upper_bounds[i] = (int)upper_val.i_val;
                    }
                }

                free(lower_idx);
                free(upper_idx);

                Value array_val = makeArrayND(dimension_count, lower_bounds, upper_bounds, elem_var_type, elem_type_def);

                free(lower_bounds);
                free(upper_bounds);

                Value* base_val_ptr = vm->stackTop - 1;
                bool invalid_type = false;
                Value* record_struct_ptr = resolveRecord(base_val_ptr, &invalid_type);
                if (invalid_type) {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
                    freeValue(&array_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (record_struct_ptr == NULL || record_struct_ptr->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Cannot access field on a nil pointer or non-record value.");
                    freeValue(&array_val);
                    return INTERPRET_RUNTIME_ERROR;
                }

                FieldValue* current = record_struct_ptr->record_val;
                for (uint16_t i = 0; i < field_index && current; i++) {
                    current = current->next;
                }
                if (!current) {
                    runtimeError(vm, "VM Error: Field index out of range for INIT_FIELD_ARRAY.");
                    freeValue(&array_val);
                    return INTERPRET_RUNTIME_ERROR;
                }
                freeValue(&current->value);
                current->value = array_val;
                break;
            }
            case INIT_LOCAL_FILE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                *target_slot = makeValueForType(TYPE_FILE, NULL, NULL);
                break;
            }
            case INIT_LOCAL_POINTER: {
                uint8_t slot = READ_BYTE();
                uint16_t type_name_idx = READ_SHORT(vm);
                AST* type_def = NULL;
                Value type_name_val = vm->chunk->constants[type_name_idx];
                if (type_name_val.type == TYPE_STRING && type_name_val.s_val && type_name_val.s_val[0] != '\0') {
                    // Prefer a named type if available (e.g., pointer to record type)
                    AST* looked = lookupType(type_name_val.s_val);
                    if (looked) {
                        type_def = looked;
                    } else {
                        // Build a simple base type node from the provided name
                        Token* baseTok = newToken(TOKEN_IDENTIFIER, type_name_val.s_val, 0, 0);
                        type_def = newASTNode(AST_VARIABLE, baseTok);
                        const char* tn = type_name_val.s_val;
                        if      (strcasecmp(tn, "integer") == 0 || strcasecmp(tn, "int") == 0) setTypeAST(type_def, TYPE_INT32);
                        else if (strcasecmp(tn, "real")    == 0 || strcasecmp(tn, "double") == 0) setTypeAST(type_def, TYPE_DOUBLE);
                        else if (strcasecmp(tn, "single")  == 0 || strcasecmp(tn, "float")  == 0) setTypeAST(type_def, TYPE_FLOAT);
                        else if (strcasecmp(tn, "char")    == 0) setTypeAST(type_def, TYPE_CHAR);
                        else if (strcasecmp(tn, "boolean") == 0 || strcasecmp(tn, "bool") == 0) setTypeAST(type_def, TYPE_BOOLEAN);
                        else if (strcasecmp(tn, "byte")    == 0) setTypeAST(type_def, TYPE_BYTE);
                        else if (strcasecmp(tn, "word")    == 0) setTypeAST(type_def, TYPE_WORD);
                        else if (strcasecmp(tn, "int64")   == 0 || strcasecmp(tn, "longint") == 0) setTypeAST(type_def, TYPE_INT64);
                        else if (strcasecmp(tn, "cardinal")== 0) setTypeAST(type_def, TYPE_UINT32);
                        else setTypeAST(type_def, TYPE_VOID);
                        freeToken(baseTok);
                    }
                }
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                // Initialize pointer with a sensible base_type_node. If type_def is a pointer type
                // use its right child; for basic types, use the type_def directly.
                Value ptr = makeValueForType(TYPE_POINTER, NULL, NULL);
                if (type_def) {
                    AST* resolved = type_def;
                    if (resolved->type == AST_TYPE_REFERENCE && resolved->right) {
                        resolved = resolved->right;
                    }
                    if (resolved->type == AST_POINTER_TYPE && resolved->right) {
                        ptr.base_type_node = resolved->right;
                    } else if (resolved->type == AST_VARIABLE || resolved->type == AST_TYPE_IDENTIFIER) {
                        ptr.base_type_node = resolved;
                    } else {
                        ptr.base_type_node = resolved;
                    }
                }
                *target_slot = ptr;
                break;
            }
            case INIT_LOCAL_STRING: {
                uint8_t slot = READ_BYTE();
                uint8_t length = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                target_slot->type = TYPE_STRING;
                target_slot->max_length = length;
                target_slot->s_val = (char*)calloc(length + 1, 1);
                if (!target_slot->s_val) {
                    runtimeError(vm, "VM Error: Malloc failed for fixed-length string initialization.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                break;
            }
            case JUMP_IF_FALSE: {
                uint16_t offset_val = READ_SHORT(vm);
                Value condition_value = pop(vm);
                bool condition_truth = false;
                bool value_valid = true;

                if (IS_BOOLEAN(condition_value)) {
                    condition_truth = AS_BOOLEAN(condition_value);
                } else if (IS_INTLIKE(condition_value)) {
                    condition_truth = AS_INTEGER(condition_value) != 0;
                } else if (IS_REAL(condition_value)) {
                    condition_truth = AS_REAL(condition_value) != 0.0;
                } else if (IS_CHAR(condition_value)) {
                    condition_truth = AS_CHAR(condition_value) != '\0';
                } else if (condition_value.type == TYPE_NIL) {
                    condition_truth = false;
                } else {
                    value_valid = false;
                }

                if (!value_valid) {
                    runtimeError(vm, "VM Error: IF condition must be a Boolean or numeric value.");
                    freeValue(&condition_value);
                    return INTERPRET_RUNTIME_ERROR;
                }

                freeValue(&condition_value);

                if (!condition_truth) {
                    vm->ip += (int16_t)offset_val;
                }
                break;
            }
            case JUMP: {
                uint16_t offset = READ_SHORT(vm);
                vm->ip += (int16_t)offset;
                break;
            }
            case POP: {
                Value popped_val = pop(vm);
                freeValue(&popped_val);
                break;
            }
            case CALL_BUILTIN_PROC: {
                uint16_t builtin_id = READ_SHORT(vm);
                uint16_t name_const_idx = READ_SHORT(vm);
                uint8_t arg_count = READ_BYTE();

                if (vm->stackTop - vm->stack < arg_count) {
                    runtimeError(vm, "VM Error: Stack underflow for built-in arguments.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* encoded_name = NULL;
                if (name_const_idx < vm->chunk->constants_count) {
                    Value* name_val = &vm->chunk->constants[name_const_idx];
                    if (name_val->type == TYPE_STRING && name_val->s_val) {
                        encoded_name = name_val->s_val;
                    }
                }

                Value* args = vm->stackTop - arg_count;
                const char* builtin_name = getVmBuiltinNameById((int)builtin_id);
                VmBuiltinFn handler = getVmBuiltinHandlerById((int)builtin_id);

                char lookup_name[MAX_ID_LENGTH + 1];
                lookup_name[0] = '\0';
                if (encoded_name && *encoded_name) {
                    strncpy(lookup_name, encoded_name, MAX_ID_LENGTH);
                    lookup_name[MAX_ID_LENGTH] = '\0';
                    toLowerString(lookup_name);
                } else if (builtin_name) {
                    strncpy(lookup_name, builtin_name, MAX_ID_LENGTH);
                    lookup_name[MAX_ID_LENGTH] = '\0';
                    toLowerString(lookup_name);
                }

                if (!handler && lookup_name[0] != '\0') {
                    handler = getVmBuiltinHandler(lookup_name);
                    if (!builtin_name && encoded_name && *encoded_name) {
                        builtin_name = encoded_name;
                    }
                }

                const char* effective_name = builtin_name;
                if (!effective_name && encoded_name && *encoded_name) {
                    effective_name = encoded_name;
                } else if (!effective_name && lookup_name[0] != '\0') {
                    effective_name = lookup_name;
                }

                if (!handler) {
                    if (effective_name && *effective_name) {
                        runtimeError(vm, "VM Error: Unimplemented or unknown built-in '%s' (id %u) called.",
                                     effective_name, builtin_id);
                    } else {
                        runtimeError(vm, "VM Error: Unknown built-in id %u called.", builtin_id);
                    }
                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        if (args[i].type == TYPE_POINTER) {
                            continue;
                        }
                        freeValue(&args[i]);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }

                bool needs_lock = (lookup_name[0] != '\0') ? builtinUsesGlobalStructures(lookup_name) : false;
                if (needs_lock) pthread_mutex_lock(&globals_mutex);
                Value result = handler(vm, arg_count, args);
                if (needs_lock) pthread_mutex_unlock(&globals_mutex);

                vm->stackTop -= arg_count;
                for (int i = 0; i < arg_count; i++) {
                    if (args[i].type == TYPE_POINTER) {
                        continue;
                    }
                    freeValue(&args[i]);
                }

                BuiltinRoutineType builtin_type = effective_name ? getBuiltinType(effective_name) : BUILTIN_TYPE_NONE;
                if (builtin_type == BUILTIN_TYPE_FUNCTION) {
                    push(vm, result);
                } else {
                    freeValue(&result);
                }

                if (vm->exit_requested) {
                    vm->exit_requested = false;
                    bool halted = false;
                    InterpretResult res = returnFromCall(vm, &halted);
                    if (res != INTERPRET_OK) return res;
                    if (halted) return INTERPRET_OK;
                }
                break;
            }
            case CALL_BUILTIN: {
                uint16_t name_const_idx = READ_SHORT(vm);
                uint8_t arg_count = READ_BYTE();

                if (vm->stackTop - vm->stack < arg_count) {
                    runtimeError(vm, "VM Stack underflow for built-in arguments.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* args = vm->stackTop - arg_count;
                const char* builtin_name_original_case = AS_STRING(vm->chunk->constants[name_const_idx]);

                // Convert builtin_name to lowercase for lookup in the dispatch table
                char builtin_name_lower[MAX_ID_LENGTH + 1]; // Use a buffer large enough for builtin names
                strncpy(builtin_name_lower, builtin_name_original_case, MAX_ID_LENGTH);
                builtin_name_lower[MAX_ID_LENGTH] = '\0'; // Ensure null termination
                toLowerString(builtin_name_lower); // toLowerString is in utils.h/c

                VmBuiltinFn handler = getVmBuiltinHandler(builtin_name_lower); // Pass the lowercase name

                if (handler) {
                    bool needs_lock = builtinUsesGlobalStructures(builtin_name_lower);
                    if (needs_lock) pthread_mutex_lock(&globals_mutex);
                    Value result = handler(vm, arg_count, args);
                    if (needs_lock) pthread_mutex_unlock(&globals_mutex);

                    // Pop arguments from the stack and free their contents when safe.
                    // Arrays and pointers reference caller-managed memory, so avoid freeing
                    // the underlying data to prevent invalidating VAR arguments.
                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        if (args[i].type == TYPE_POINTER) {
                            // Pointer arguments reference caller-managed memory; do not dereference here.
                            continue;
                        }

                        // Arrays are copied when arguments are evaluated, so the VM owns the
                        // temporary buffer on the stack.  Free it now to avoid leaking large
                        // allocations (e.g., texture pixel buffers) on every builtin call.
                        freeValue(&args[i]);
                    }

                    if (getBuiltinType(builtin_name_original_case) == BUILTIN_TYPE_FUNCTION) {
                        push(vm, result);
                    } else {
                        freeValue(&result);
                    }
                } else {
                    runtimeError(vm, "VM Error: Unimplemented or unknown built-in '%s' called.", builtin_name_original_case);
                    // Cleanup stack even on error, preserving caller-owned arrays/pointers
                    vm->stackTop -= arg_count;
                    for (int i = 0; i < arg_count; i++) {
                        if (args[i].type == TYPE_POINTER) {
                            continue;
                        }
                        freeValue(&args[i]);
                    }
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->exit_requested) {
                    vm->exit_requested = false;
                    bool halted = false;
                    InterpretResult res = returnFromCall(vm, &halted);
                    if (res != INTERPRET_OK) return res;
                    if (halted) return INTERPRET_OK;
                }
                break;
            }
            case CALL_USER_PROC: {
                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                uint16_t name_index = READ_SHORT(vm);
                uint8_t declared_arity = READ_BYTE();

                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %u, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!vm->chunk || name_index >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Invalid procedure name index %u for CALL_USER_PROC.", name_index);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* name_val = &vm->chunk->constants[name_index];
                if (name_val->type != TYPE_STRING || !name_val->s_val) {
                    runtimeError(vm, "VM Error: CALL_USER_PROC requires string constant for callee name (index %u).", name_index);
                    return INTERPRET_RUNTIME_ERROR;
                }

                const char* proc_name = name_val->s_val;
                char lookup_name[MAX_SYMBOL_LENGTH + 1];
                strncpy(lookup_name, proc_name, MAX_SYMBOL_LENGTH);
                lookup_name[MAX_SYMBOL_LENGTH] = '\0';
                toLowerString(lookup_name);

                if (!vm->procedureTable) {
                    runtimeError(vm, "VM Error: Procedure table not initialized when calling '%s'.", proc_name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                Symbol* proc_symbol = findProcedureByName(vm->procedureTable, lookup_name, vm);
                if (!proc_symbol) {
                    runtimeError(vm, "VM Error: Procedure '%s' not found for CALL_USER_PROC.", proc_name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (!proc_symbol->is_defined || proc_symbol->bytecode_address < 0) {
                    runtimeError(vm, "VM Error: Procedure '%s' has no compiled body.",
                                 proc_symbol->name ? proc_symbol->name : proc_name);
                    return INTERPRET_RUNTIME_ERROR;
                }

                size_t target_address = (size_t)proc_symbol->bytecode_address;
                if (!vm->chunk || target_address >= (size_t)vm->chunk->count) {
                    runtimeError(vm, "VM Error: Procedure '%s' bytecode address %d out of range.",
                                 proc_symbol->name ? proc_symbol->name : proc_name,
                                 proc_symbol->bytecode_address);
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(arg_val->type)) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->discard_result_on_return = false;
                frame->vtable = NULL;

                if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }

                    if (!parent_frame) {
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case CALL: {
                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Operands: name_idx (2 bytes), target_address (2 bytes), declared_arity (1 byte)
                uint16_t name_idx_ignored = READ_SHORT(vm); // Read and discard the name index
                (void)name_idx_ignored; // Suppress unused variable warning
                uint16_t target_address = READ_SHORT(vm);
                uint8_t declared_arity = READ_BYTE();

                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                Symbol* proc_symbol = findProcedureByAddress(vm->procedureTable, target_address);
                if (proc_symbol && proc_symbol->is_alias) proc_symbol = proc_symbol->real_symbol;
                if (!proc_symbol) {
                    runtimeError(vm, "VM Error: Could not retrieve procedure symbol for called address %04d.", target_address);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(arg_val->type)) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->discard_result_on_return = false;
                frame->vtable = NULL;
                frame->vtable = NULL;

                if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }

                    if (!parent_frame) {
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case CALL_INDIRECT: {
                uint8_t declared_arity = READ_BYTE();
                // Stack layout expected: [... args] [addr]
                Value addrVal = pop(vm);
                if (!IS_INTLIKE(addrVal)) {
                    freeValue(&addrVal);
                    runtimeError(vm, "VM Error: Indirect call requires integer address on stack.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                uint16_t target_address = (uint16_t)AS_INTEGER(addrVal);
                freeValue(&addrVal);

                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for indirect call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                Symbol* proc_symbol = findProcedureByAddress(vm->procedureTable, target_address);
                if (proc_symbol && proc_symbol->is_alias) proc_symbol = proc_symbol->real_symbol;
                if (!proc_symbol) {
                    runtimeError(vm, "VM Error: No procedure found at address %04d for indirect call.", target_address);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                // Coerce numeric argument types to match formal parameter real/integer expectations
                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(arg_val->type)) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->discard_result_on_return = false;

                if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }

                    if (!parent_frame) {
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        return INTERPRET_RUNTIME_ERROR;
                    }

                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case CALL_METHOD: {
                uint8_t method_index = READ_BYTE();
                uint8_t declared_arity = READ_BYTE();
                if (vm->stackTop - vm->stack < declared_arity + 1) {
                    runtimeError(vm, "VM Error: Stack underflow for method call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value receiverVal = vm->stackTop[-declared_arity - 1];
                if (receiverVal.type != TYPE_POINTER || receiverVal.ptr_val == NULL) {
                    runtimeError(vm, "VM Error: Method call receiver must be an object pointer.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                Value* objVal = receiverVal.ptr_val;
                if (objVal->type != TYPE_RECORD) {
                    runtimeError(vm, "VM Error: Method call receiver must be an object record.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                FieldValue* current = objVal->record_val;
                Value* vtable_arr = NULL;
                while (current) {
                    if (strcmp(current->name, "__vtable") == 0) {
                        if (current->value.type == TYPE_ARRAY) {
                            vtable_arr = current->value.array_val;
                        }
                        break;
                    }
                    current = current->next;
                }

                if (!vtable_arr) {
                    runtimeError(vm, "VM Error: Object missing V-table.");
                    return INTERPRET_RUNTIME_ERROR;
                }

                uint16_t target_address = (uint16_t)vtable_arr[method_index].u_val;
                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity - 1;
                frame->vtable = vtable_arr;
                frame->slotCount = 0;

                Symbol* method_symbol = NULL;
                const char* className = NULL;
                if (objVal->base_type_node && objVal->base_type_node->token) {
                    className = objVal->base_type_node->token->value;
                }
                if (className) {
                    method_symbol = vmFindClassMethod(vm, className, method_index);
                }
                if (!method_symbol) {
                    method_symbol = findProcedureByAddress(vm->procedureTable, target_address);
                }
                if (!method_symbol) {
                    runtimeError(vm, "VM Error: Method not found for index %d.", method_index);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (method_symbol->type_def && method_symbol->type_def->child_count >= declared_arity + 1) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = method_symbol->type_def->children[i + 1];
                        Value* arg_val = frame->slots + 1 + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(arg_val->type)) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = method_symbol;
                frame->locals_count = method_symbol->locals_count;
                frame->upvalue_count = method_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->discard_result_on_return = false;

                if (method_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * method_symbol->upvalue_count);
                    CallFrame* parent_frame = NULL;
                    if (method_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == method_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }
                    if (!parent_frame) {
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", method_symbol->name);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    for (int i = 0; i < method_symbol->upvalue_count; i++) {
                        if (method_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + method_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[method_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < method_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + 1 + method_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;
                break;
            }
            case PROC_CALL_INDIRECT: {
                uint8_t declared_arity = READ_BYTE();
                // Reuse CALL_INDIRECT machinery by rewinding ip to interpret the common path,
                // but we need to know when to discard a return value. Implement inline duplication instead.

                Value addrVal = pop(vm);
                if (!IS_INTLIKE(addrVal)) {
                    freeValue(&addrVal);
                    runtimeError(vm, "VM Error: Indirect call requires integer address on stack.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                uint16_t target_address = (uint16_t)AS_INTEGER(addrVal);
                freeValue(&addrVal);

                if (vm->frameCount >= VM_CALL_STACK_MAX) {
                    runtimeError(vm, "VM Error: Call stack overflow.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                if (vm->stackTop - vm->stack < declared_arity) {
                    runtimeError(vm, "VM Error: Stack underflow for indirect call arguments. Expected %d, have %ld.",
                                 declared_arity, (long)(vm->stackTop - vm->stack));
                    return INTERPRET_RUNTIME_ERROR;
                }

                CallFrame* frame = &vm->frames[vm->frameCount++];
                frame->return_address = vm->ip;
                frame->slots = vm->stackTop - declared_arity;
                frame->slotCount = 0;

                Symbol* proc_symbol = findProcedureByAddress(vm->procedureTable, target_address);
                if (proc_symbol && proc_symbol->is_alias) proc_symbol = proc_symbol->real_symbol;
                if (!proc_symbol) {
                    runtimeError(vm, "VM Error: No procedure found at address %04d for indirect call.", target_address);
                    vm->frameCount--;
                    return INTERPRET_RUNTIME_ERROR;
                }

                if (proc_symbol->type_def && proc_symbol->type_def->child_count >= declared_arity) {
                    for (int i = 0; i < declared_arity; i++) {
                        AST* param_ast = proc_symbol->type_def->children[i];
                        Value* arg_val = frame->slots + i;
                        if (isRealType(param_ast->var_type) && isIntlikeType(arg_val->type)) {
                            long double tmp = asLd(*arg_val);
                            setTypeValue(arg_val, param_ast->var_type);
                            SET_REAL_VALUE(arg_val, tmp);
                        }
                    }
                }

                frame->function_symbol = proc_symbol;
                frame->locals_count = proc_symbol->locals_count;
                frame->upvalue_count = proc_symbol->upvalue_count;
                frame->upvalues = NULL;
                frame->discard_result_on_return = true;
                frame->vtable = NULL;

                if (proc_symbol->upvalue_count > 0) {
                    frame->upvalues = malloc(sizeof(Value*) * proc_symbol->upvalue_count);
                    CallFrame* parent_frame = NULL;
                    if (proc_symbol->enclosing) {
                        for (int fi = vm->frameCount - 2; fi >= 0; fi--) {
                            if (vm->frames[fi].function_symbol == proc_symbol->enclosing) {
                                parent_frame = &vm->frames[fi];
                                break;
                            }
                        }
                    } else if (vm->frameCount >= 2) {
                        parent_frame = &vm->frames[vm->frameCount - 2];
                    }
                    if (!parent_frame) {
                        runtimeError(vm, "VM Error: Enclosing frame not found for '%s'.", proc_symbol->name);
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    for (int i = 0; i < proc_symbol->upvalue_count; i++) {
                        if (proc_symbol->upvalues[i].isLocal) {
                            frame->upvalues[i] = parent_frame->slots + proc_symbol->upvalues[i].index;
                        } else {
                            frame->upvalues[i] = parent_frame->upvalues[proc_symbol->upvalues[i].index];
                        }
                    }
                }

                for (int i = 0; i < proc_symbol->locals_count; i++) {
                    push(vm, makeNil());
                }

                frame->slotCount = (uint16_t)(declared_arity + proc_symbol->locals_count);

                vm->ip = vm->chunk->code + target_address;

                // After the callee returns, if it is a function, its result will be on the stack.
                // Since this opcode is for statement context, discard it to keep the stack balanced.
                // This block will run when the frame unwinds back here.
                // Note: actual popping occurs after the callee returns to this frame.
                // The main interpreter loop continues; no action needed here now.
                break;
            }

            case HALT:
                return INTERPRET_OK;
            case CALL_HOST: {
                HostFunctionID host_id = READ_HOST_ID();
                if (host_id >= HOST_FN_COUNT || vm->host_functions[host_id] == NULL) {
                    runtimeError(vm, "Invalid host function ID %d or function not registered.", host_id);
                    return INTERPRET_RUNTIME_ERROR;
                }
                HostFn func = vm->host_functions[host_id];
                // Do not hold globals_mutex around host calls that may block (e.g., thread waits),
                // or that start threads which immediately need to access globals during VM init.
                // Individual host functions should lock as needed.
                Value result = func(vm);
                push(vm, result);
                break;
            }
            case THREAD_CREATE: {
                uint16_t entry = READ_SHORT(vm);
                int id = createThread(vm, entry);
                if (id < 0) {
                    runtimeError(vm, "Thread limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case THREAD_JOIN: {
                Value tidVal = peek(vm, 0);
                int tid_ok = 0;
                int tid = 0;
                if (tidVal.type == TYPE_THREAD) {
                    tid = (int)AS_INTEGER(tidVal);
                    tid_ok = 1;
                } else if (IS_INTLIKE(tidVal)) {
                    tid = (int)AS_INTEGER(tidVal);
                    tid_ok = 1;
                }
                if (!tid_ok) {
                    runtimeError(vm, "Thread id must be integer.");
                    Value popped_tid = pop(vm);
                    freeValue(&popped_tid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                joinThread(vm, tid);
                Value popped_tid = pop(vm);
                freeValue(&popped_tid);
                break;
            }
            case MUTEX_CREATE: {
                int id = createMutex(vm, false);
                if (id < 0) {
                    runtimeError(vm, "Mutex limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case RCMUTEX_CREATE: {
                int id = createMutex(vm, true);
                if (id < 0) {
                    runtimeError(vm, "Mutex limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case MUTEX_LOCK: {
                Value midVal = peek(vm, 0);
                if (!IS_INTLIKE(midVal)) {
                    runtimeError(vm, "Mutex id must be integer.");
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int mid = (int)midVal.i_val;
                if (!lockMutex(vm, mid)) {
                    runtimeError(vm, "Invalid mutex id %d.", mid);
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_mid = pop(vm);
                freeValue(&popped_mid);
                break;
            }
            case MUTEX_UNLOCK: {
                Value midVal = peek(vm, 0);
                if (!IS_INTLIKE(midVal)) {
                    runtimeError(vm, "Mutex id must be integer.");
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int mid = (int)midVal.i_val;
                if (!unlockMutex(vm, mid)) {
                    runtimeError(vm, "Invalid mutex id %d.", mid);
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_mid = pop(vm);
                freeValue(&popped_mid);
                break;
            }
            case MUTEX_DESTROY: {
                Value midVal = peek(vm, 0);
                if (!IS_INTLIKE(midVal)) {
                    runtimeError(vm, "Mutex id must be integer.");
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int mid = (int)midVal.i_val;
                if (!destroyMutex(vm, mid)) {
                    runtimeError(vm, "Invalid mutex id %d.", mid);
                    Value popped_mid = pop(vm);
                    freeValue(&popped_mid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                Value popped_mid = pop(vm);
                freeValue(&popped_mid);
                break;
            }
            case FORMAT_VALUE: {
                uint8_t width = READ_BYTE();
                uint8_t precision_raw = READ_BYTE();
                int precision = (precision_raw == 0xFF) ? -1 : precision_raw;

                Value raw_val = pop(vm);

                char buf[DEFAULT_STRING_CAPACITY];
                buf[0] = '\0';

                if (isRealType(raw_val.type)) {
                    long double rv = AS_REAL(raw_val);
                    if (precision >= 0) {
                        snprintf(buf, sizeof(buf), "%*.*Lf", width, precision, rv);
                    } else {
                        snprintf(buf, sizeof(buf), "%*.*LE", width, PASCAL_DEFAULT_FLOAT_PRECISION, rv);
                    }
                } else if (raw_val.type == TYPE_CHAR) {
                    snprintf(buf, sizeof(buf), "%*c", width, raw_val.c_val);
                } else if (raw_val.type == TYPE_BOOLEAN) {
                    const char* bool_str = raw_val.i_val ? "TRUE" : "FALSE";
                    snprintf(buf, sizeof(buf), "%*s", width, bool_str);
                } else if (isIntlikeType(raw_val.type)) {
                    if (raw_val.type == TYPE_UINT64 || raw_val.type == TYPE_UINT32 ||
                        raw_val.type == TYPE_UINT16 || raw_val.type == TYPE_UINT8 ||
                        raw_val.type == TYPE_WORD   || raw_val.type == TYPE_BYTE) {
                        unsigned long long u = raw_val.u_val;
                        if (raw_val.type == TYPE_BYTE || raw_val.type == TYPE_UINT8)   u &= 0xFFULL;
                        if (raw_val.type == TYPE_WORD || raw_val.type == TYPE_UINT16) u &= 0xFFFFULL;
                        if (raw_val.type == TYPE_UINT32) u &= 0xFFFFFFFFULL;
                        snprintf(buf, sizeof(buf), "%*llu", width, u);
                    } else {
                        long long s = raw_val.i_val;
                        if (raw_val.type == TYPE_INT8)  s = (int8_t)s;
                        if (raw_val.type == TYPE_INT16) s = (int16_t)s;
                        snprintf(buf, sizeof(buf), "%*lld", width, s);
                    }
                } else if (raw_val.type == TYPE_STRING) {
                    const char* source_str = raw_val.s_val ? raw_val.s_val : "";
                    size_t len = strlen(source_str);
                    int prec = (width > 0 && (size_t)width < len) ? width : (int)len;
                    snprintf(buf, sizeof(buf), "%*.*s", width, prec, source_str);
                } else {
                    snprintf(buf, sizeof(buf), "%*s", width, "?");
                }

                freeValue(&raw_val);

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
