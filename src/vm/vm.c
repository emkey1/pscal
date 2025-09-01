// src/vm/vm.c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h> // For bool, true, false
#include <pthread.h>
#include <limits.h>
#include <stdint.h>

#include "vm/vm.h"
#include "compiler/bytecode.h"
#include "core/types.h"
#include "core/utils.h"    // For runtimeError, printValueToStream, makeNil, freeValue, Type helper macros
#include "symbol/symbol.h" // For HashTable, createHashTable, hashTableLookup, hashTableInsert
#include "Pascal/globals.h"
#include "backend_ast/audio.h"
#include "Pascal/parser.h"
#include "Pascal/ast.h"
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "backend_ast/builtin.h"

#define MAX_WRITELN_ARGS_VM 32

// Special sentinel values used in pointer.base_type_node to signal
// non-standard dereference behavior in OP_GET_INDIRECT.
#define STRING_CHAR_PTR_SENTINEL   ((AST*)0xDEADBEEF)
#define STRING_LENGTH_SENTINEL     ((AST*)0xFEEDBEEF)

// --- VM Helper Functions ---
static void resetStack(VM* vm) {
    vm->stackTop = vm->stack;
}

// --- Threading Helpers ---
typedef struct {
    Thread* thread;
    uint16_t entry;
} ThreadStartArgs;

static void* threadStart(void* arg) {
    ThreadStartArgs* args = (ThreadStartArgs*)arg;
    Thread* thread = args->thread;
    VM* vm = thread->vm;
    uint16_t entry = args->entry;
    free(args);

    interpretBytecode(vm, vm->chunk, vm->vmGlobalSymbols, vm->vmConstGlobalSymbols, vm->procedureTable, entry);
    thread->active = false;
    return NULL;
}

static int createThread(VM* vm, uint16_t entry) {
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

    ThreadStartArgs* args = malloc(sizeof(ThreadStartArgs));
    if (!args) {
        free(t->vm);
        t->vm = NULL;
        return -1;
    }
    args->thread = t;
    args->entry = entry;
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
        if (owner->mutexCount >= VM_MAX_MUTEXES) return -1;
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
        return -1;
    }
    if (attr_ptr) pthread_mutexattr_destroy(&attr);
    m->active = true;
    return id;
}

static bool lockMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) return false;
    return pthread_mutex_lock(&owner->mutexes[id].handle) == 0;
}

static bool unlockMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) return false;
    return pthread_mutex_unlock(&owner->mutexes[id].handle) == 0;
}

// Permanently frees a mutex created by mutex()/rcmutex(), making its ID unusable.
static bool destroyMutex(VM* vm, int id) {
    VM* owner = vm->mutexOwner ? vm->mutexOwner : vm;
    if (id < 0 || id >= owner->mutexCount || !owner->mutexes[id].active) return false;
    if (pthread_mutex_destroy(&owner->mutexes[id].handle) != 0) return false;
    owner->mutexes[id].active = false;
    // If this was the highest-index mutex, shrink the count so new mutexes can reuse slots.
    while (owner->mutexCount > 0 && !owner->mutexes[owner->mutexCount - 1].active) {
        owner->mutexCount--;
    }
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

    // Get approximate instruction offset and line for the error
    size_t instruction_offset = 0;
    int error_line = 0;
    if (vm && vm->chunk && vm->ip && vm->chunk->code && vm->chunk->lines) {
        // The instruction that *caused* the error is usually the one *before* vm->ip
        if (vm->ip > vm->chunk->code) {
            instruction_offset = (vm->ip - vm->chunk->code) - 1;
            if (instruction_offset < (size_t)vm->chunk->count) {
                error_line = vm->chunk->lines[instruction_offset];
            }
        } else if (vm->chunk->count > 0) { // Special case: error on the very first byte
            instruction_offset = 0;
            error_line = vm->chunk->lines[0];
        }
    }
    fprintf(stderr, "[Error Location] Offset: %zu, Line: %d\n", instruction_offset, error_line);

    // --- NEW: Dump crash context (instructions and full stack) ---
    fprintf(stderr, "\n--- VM Crash Context ---\n");
    fprintf(stderr, "Instruction Pointer (IP): %p\n", (void*)vm->ip);
    fprintf(stderr, "Code Base: %p\n", (void*)vm->chunk->code);

    // Dump current instruction (at vm->ip)
    fprintf(stderr, "Current Instruction (at IP, might be the instruction that IP tried to fetch/decode):\n");
    if (vm->ip >= vm->chunk->code && (vm->ip - vm->chunk->code) < vm->chunk->count) {
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
    for (int offset = start_dump_offset; offset < (int)instruction_offset; ) {
        offset = disassembleInstruction(vm->chunk, offset, vm->procedureTable);
    }
    if (start_dump_offset == (int)instruction_offset) {
        fprintf(stderr, "  (No preceding instructions in buffer to display)\n");
    }

    // Dump full stack contents
    vmDumpStackInfoDetailed(vm, "Full Stack at Crash");

    // --- END NEW DUMP ---

    // resetStack(vm); // Keep this commented out for post-mortem analysis purposes if you want to inspect stack in debugger
    // ... (rest of runtimeError function, calls EXIT_FAILURE_HANDLER()) ...
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
        safeReturnValue = makeCopyOfValue(&returnValue);
        freeValue(&returnValue);
    }

    for (Value* slot = currentFrame->slots; slot < vm->stackTop; slot++) {
        freeValue(slot);
    }

    vm->ip = currentFrame->return_address;
    vm->stackTop = currentFrame->slots;

    if (currentFrame->upvalues) {
        free(currentFrame->upvalues);
        currentFrame->upvalues = NULL;
    }
    vm->frameCount--;

    if (has_result) {
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

    sym->is_alias = false;
    sym->is_const = false; // Constants handled at compile time won't use OP_DEFINE_GLOBAL
                           // If VM needs to know about them, another mechanism or flag is needed.
    sym->is_local_var = false;
    sym->is_inline = false;
    sym->next = NULL;
    sym->enclosing = NULL;
    sym->upvalue_count = 0;
    return sym;
}

// Shared logic for OP_DEFINE_GLOBAL and OP_DEFINE_GLOBAL16.
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
            type_def_node = lookupType(typeNameVal.s_val);
            if (declaredType == TYPE_ENUM && type_def_node == NULL) {
                runtimeError(vm, "VM Error: Enum type '%s' not found for global '%s'.", typeNameVal.s_val, varNameVal.s_val);
                return INTERPRET_RUNTIME_ERROR;
            }
        }

        if (varNameVal.type != TYPE_STRING || !varNameVal.s_val) {
            runtimeError(vm, "VM Error: Invalid variable name for OP_DEFINE_GLOBAL.");
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

    // Establish a base call frame for the main program.  This allows inline
    // routines at the top level to utilize local variable opcodes without
    // triggering stack underflows due to the absence of an active frame.
    CallFrame* baseFrame = &vm->frames[vm->frameCount++];
    baseFrame->return_address = NULL;
    baseFrame->slots = vm->stack;
    baseFrame->function_symbol = NULL;
    baseFrame->locals_count = 0;
    baseFrame->upvalue_count = 0;
    baseFrame->upvalues = NULL;

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
        /* String/char concatenation for OP_ADD */ \
        if (current_instruction_code == OP_ADD) { \
            while (a_val_popped.type == TYPE_POINTER && a_val_popped.ptr_val) { \
                Value tmp = makeCopyOfValue(a_val_popped.ptr_val); \
                freeValue(&a_val_popped); \
                a_val_popped = tmp; \
            } \
            while (b_val_popped.type == TYPE_POINTER && b_val_popped.ptr_val) { \
                Value tmp = makeCopyOfValue(b_val_popped.ptr_val); \
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
            if (current_instruction_code == OP_ADD || current_instruction_code == OP_SUBTRACT) { \
                bool a_enum_b_int = (a_val_popped.type == TYPE_ENUM && IS_INTLIKE(b_val_popped)); \
                bool a_int_b_enum = (IS_INTLIKE(a_val_popped) && b_val_popped.type == TYPE_ENUM); \
                if (a_enum_b_int || a_int_b_enum) { \
                    Value enum_val = a_enum_b_int ? a_val_popped : b_val_popped; \
                    Value int_val  = a_enum_b_int ? b_val_popped : a_val_popped; \
                    long long delta = asI64(int_val); \
                    int new_ord = enum_val.enum_val.ordinal + \
                        ((current_instruction_code == OP_ADD) ? (int)delta : -(int)delta); \
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
                    case OP_ADD: \
                        result_val = setUnion(a_val_popped, b_val_popped); \
                        break; \
                    case OP_SUBTRACT: \
                        result_val = setDifference(a_val_popped, b_val_popped); \
                        break; \
                    case OP_MULTIPLY: \
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
                    if (current_instruction_code == OP_DIVIDE && fb == 0.0L) { \
                        runtimeError(vm, "Runtime Error: Division by zero."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    int useLong = (a_val_popped.type == TYPE_LONG_DOUBLE || b_val_popped.type == TYPE_LONG_DOUBLE); \
                    switch (current_instruction_code) { \
                        case OP_ADD:      result_val = useLong ? makeLongDouble(fa + fb) : makeReal(fa + fb); break; \
                        case OP_SUBTRACT: result_val = useLong ? makeLongDouble(fa - fb) : makeReal(fa - fb); break; \
                        case OP_MULTIPLY: result_val = useLong ? makeLongDouble(fa * fb) : makeReal(fa * fb); break; \
                        case OP_DIVIDE:   result_val = useLong ? makeLongDouble(fa / fb) : makeReal(fa / fb); break; \
                        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for real numbers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                } else { \
                    long long ia = asI64(a_val_popped); \
                    long long ib = asI64(b_val_popped); \
                    if (current_instruction_code == OP_DIVIDE && ib == 0) { \
                        runtimeError(vm, "Runtime Error: Division by zero (integer)."); \
                        freeValue(&a_val_popped); freeValue(&b_val_popped); \
                        return INTERPRET_RUNTIME_ERROR; \
                    } \
                    long long iresult = 0; \
                    bool overflow = false; \
                    switch (current_instruction_code) { \
                        case OP_ADD: \
                            overflow = __builtin_add_overflow(ia, ib, &iresult); \
                            break; \
                        case OP_SUBTRACT: \
                            overflow = __builtin_sub_overflow(ia, ib, &iresult); \
                            break; \
                        case OP_MULTIPLY: \
                            overflow = __builtin_mul_overflow(ia, ib, &iresult); \
                            break; \
                        case OP_DIVIDE: \
                            result_val = makeReal((long double)ia / (long double)ib); \
                            break; \
                        case OP_MOD: \
                            iresult = ib == 0 ? 0 : ia % ib; \
                            break; \
        default: \
                            runtimeError(vm, "Runtime Error: Invalid arithmetic opcode %d for integers.", current_instruction_code); \
                            freeValue(&a_val_popped); freeValue(&b_val_popped); \
                            return INTERPRET_RUNTIME_ERROR; \
                    } \
                    if (current_instruction_code == OP_DIVIDE) { \
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
        switch (instruction_val) {
            case OP_RETURN: {
                bool halted = false;
                InterpretResult res = returnFromCall(vm, &halted);
                if (res != INTERPRET_OK) return res;
                if (halted) return INTERPRET_OK;
                break;
            }
            case OP_EXIT: {
                bool halted = false;
                InterpretResult res = returnFromCall(vm, &halted);
                if (res != INTERPRET_OK) return res;
                if (halted) return INTERPRET_OK;
                break;
            }
            case OP_CONSTANT: {
                Value constant = READ_CONSTANT();
                push(vm, makeCopyOfValue(&constant));
                break;
            }
                
            case OP_CONSTANT16: {
                uint16_t idx = READ_SHORT(vm);
                if (idx >= vm->chunk->constants_count) {
                    runtimeError(vm, "VM Error: Constant index %u out of bounds for OP_CONSTANT16.", idx);
                   return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeCopyOfValue(&vm->chunk->constants[idx]));
                break;
            }

            case OP_GET_CHAR_ADDRESS: {
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
            case OP_GET_GLOBAL_ADDRESS: {
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
            case OP_GET_GLOBAL_ADDRESS16: {
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
            case OP_GET_LOCAL_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                push(vm, makePointer(&frame->slots[slot], NULL));
                break;
            }
            case OP_ADD:      BINARY_OP("+", instruction_val); break;
            case OP_SUBTRACT: BINARY_OP("-", instruction_val); break;
            case OP_MULTIPLY: BINARY_OP("*", instruction_val); break;
            case OP_DIVIDE:   BINARY_OP("/", instruction_val); break;

            case OP_NEGATE: {
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
            case OP_NOT: {
                Value val_popped = pop(vm);
                bool condition_truth = false;
                bool value_valid = true;

                if (IS_BOOLEAN(val_popped)) {
                    condition_truth = AS_BOOLEAN(val_popped);
                } else if (IS_INTLIKE(val_popped)) {
                    condition_truth = AS_INTEGER(val_popped) != 0;
                } else if (IS_REAL(val_popped)) {
                    condition_truth = AS_REAL(val_popped) != 0.0;
                } else if (IS_CHAR(val_popped)) {
                    condition_truth = AS_CHAR(val_popped) != '\0';
                } else if (val_popped.type == TYPE_NIL) {
                    condition_truth = false;
                } else {
                    value_valid = false;
                }

                if (!value_valid) {
                    runtimeError(vm, "Runtime Error: Operand for NOT must be boolean or numeric.");
                    freeValue(&val_popped);
                    return INTERPRET_RUNTIME_ERROR;
                }

                push(vm, makeBoolean(!condition_truth));
                freeValue(&val_popped);
                break;
            }
            case OP_SWAP: {
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
            case OP_DUP: {
                if (vm->stackTop == vm->stack) {
                    runtimeError(vm, "VM Error: Stack underflow (dup from empty stack).");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeCopyOfValue(&vm->stackTop[-1]));
                break;
            }
            case OP_AND:
            case OP_OR: {
                Value b_val = pop(vm);
                Value a_val = pop(vm);
                Value result_val;

                if (IS_BOOLEAN(a_val) && IS_BOOLEAN(b_val)) {
                    bool ba = AS_BOOLEAN(a_val);
                    bool bb = AS_BOOLEAN(b_val);
                    if (instruction_val == OP_AND) {
                        result_val = makeBoolean(ba && bb);
                    } else {
                        result_val = makeBoolean(ba || bb);
                    }
                } else if (IS_INTLIKE(a_val) && IS_INTLIKE(b_val))  {
                    long long ia = AS_INTEGER(a_val);
                    long long ib = AS_INTEGER(b_val);
                    if (instruction_val == OP_AND) {
                        result_val = makeInt(ia & ib);
                    } else {
                        result_val = makeInt(ia | ib);
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
            case OP_MOD: {
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
            case OP_SHL:
            case OP_SHR: {
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
                    if (instruction_val == OP_SHL) {
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

                // Handle explicit NIL-to-NIL comparisons first.  Pointer/NIL
                // comparisons are handled in the pointer block below.
                if (a_val.type == TYPE_NIL && b_val.type == TYPE_NIL) {
                    if (instruction_val == OP_EQUAL) {
                        result_val = makeBoolean(true);
                    } else if (instruction_val == OP_NOT_EQUAL) {
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
                            case OP_EQUAL:         result_val = makeBoolean(fa == fb); break;
                            case OP_NOT_EQUAL:     result_val = makeBoolean(fa != fb); break;
                            case OP_GREATER:       result_val = makeBoolean(fa >  fb); break;
                            case OP_GREATER_EQUAL: result_val = makeBoolean(fa >= fb); break;
                            case OP_LESS:          result_val = makeBoolean(fa <  fb); break;
                            case OP_LESS_EQUAL:    result_val = makeBoolean(fa <= fb); break;
                            default: goto comparison_error_label;
                        }
                    } else {
                        long long ia = asI64(a_val);
                        long long ib = asI64(b_val);
                        switch (instruction_val) {
                            case OP_EQUAL:         result_val = makeBoolean(ia == ib); break;
                            case OP_NOT_EQUAL:     result_val = makeBoolean(ia != ib); break;
                            case OP_GREATER:       result_val = makeBoolean(ia >  ib); break;
                            case OP_GREATER_EQUAL: result_val = makeBoolean(ia >= ib); break;
                            case OP_LESS:          result_val = makeBoolean(ia <  ib); break;
                            case OP_LESS_EQUAL:    result_val = makeBoolean(ia <= ib); break;
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
                        result_val = makeBoolean(instruction_val == OP_EQUAL);
                    } else {
                        result_val = makeBoolean(instruction_val != OP_EQUAL);
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
                } else if ((IS_CHAR(a_val) && IS_INTEGER(b_val)) || (IS_INTEGER(a_val) && IS_CHAR(b_val))) {
                    int char_val = IS_CHAR(a_val) ? AS_CHAR(a_val) : AS_CHAR(b_val);
                    long long int_val = IS_INTEGER(a_val) ? AS_INTEGER(a_val) : AS_INTEGER(b_val);

                    switch (instruction_val) {
                        case OP_EQUAL:         result_val = makeBoolean((long long)char_val == int_val); break;
                        case OP_NOT_EQUAL:     result_val = makeBoolean((long long)char_val != int_val); break;
                        case OP_GREATER:       result_val = makeBoolean((long long)char_val > int_val);  break;
                        case OP_GREATER_EQUAL: result_val = makeBoolean((long long)char_val >= int_val); break;
                        case OP_LESS:          result_val = makeBoolean((long long)char_val < int_val);  break;
                        case OP_LESS_EQUAL:    result_val = makeBoolean((long long)char_val <= int_val); break;
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
                        case OP_EQUAL:         result_val = makeBoolean(eq); break;
                        case OP_NOT_EQUAL:     result_val = makeBoolean(!eq); break;
                        case OP_GREATER:
                        case OP_GREATER_EQUAL:
                        case OP_LESS:
                        case OP_LESS_EQUAL:
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
                    const char *name_a = a_val.enum_val.enum_name;
                    const char *name_b = b_val.enum_val.enum_name;
                    bool types_match = (name_a && name_b && strcmp(name_a, name_b) == 0);
                    int ord_a = a_val.enum_val.ordinal;
                    int ord_b = b_val.enum_val.ordinal;

                    if (instruction_val == OP_EQUAL) {
                        result_val = makeBoolean(types_match && (ord_a == ord_b));
                    } else if (instruction_val == OP_NOT_EQUAL) {
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
                    } else {
                        ptrs_equal = (a_val.ptr_val == b_val.ptr_val);
                    }

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

comparison_error_label:
                if (comparison_succeeded) {
                    push(vm, result_val);
                } else {
                    const char* op_str = "unknown_comparison_op";
                    switch (instruction_val) {
                        case OP_EQUAL:         op_str = "="; break;
                        case OP_NOT_EQUAL:     op_str = "<>"; break;
                        case OP_GREATER:       op_str = ">";  break;
                        case OP_GREATER_EQUAL: op_str = ">="; break;
                        case OP_LESS:          op_str = "<";  break;
                        case OP_LESS_EQUAL:    op_str = "<="; break;
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
            case OP_GET_FIELD_ADDRESS: {
                uint8_t field_name_idx = READ_BYTE();
                Value* base_val_ptr = vm->stackTop - 1;

                Value* record_struct_ptr = NULL;

                if (base_val_ptr->type == TYPE_POINTER) {
                    if (base_val_ptr->ptr_val == NULL) {
                        runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    record_struct_ptr = base_val_ptr->ptr_val;
                } else if (base_val_ptr->type == TYPE_RECORD) {
                    record_struct_ptr = base_val_ptr;
                } else {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
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
            case OP_GET_FIELD_ADDRESS16: {
                uint16_t field_name_idx = READ_SHORT(vm);
                Value* base_val_ptr = vm->stackTop - 1;

                Value* record_struct_ptr = NULL;

                if (base_val_ptr->type == TYPE_POINTER) {
                    if (base_val_ptr->ptr_val == NULL) {
                        runtimeError(vm, "VM Error: Cannot access field on a nil pointer.");
                        return INTERPRET_RUNTIME_ERROR;
                    }
                    record_struct_ptr = base_val_ptr->ptr_val;
                } else if (base_val_ptr->type == TYPE_RECORD) {
                    record_struct_ptr = base_val_ptr;
                } else {
                    runtimeError(vm, "VM Error: Cannot access field on a non-record/non-pointer type.");
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
            case OP_GET_ELEMENT_ADDRESS: {
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
                    if (operand.type == TYPE_POINTER) freeValue(&operand);
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
            case OP_SET_INDIRECT: {
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
            case OP_IN: {
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

            case OP_GET_INDIRECT: {
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
                    push(vm, makeCopyOfValue(target_lvalue_ptr));
                }
                freeValue(&pointer_val);
                break;
            }

            case OP_GET_CHAR_FROM_STRING: {
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
            case OP_DEFINE_GLOBAL: {
                Value varNameVal = READ_CONSTANT();
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobal(vm, varNameVal);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            case OP_DEFINE_GLOBAL16: {
                Value varNameVal = READ_CONSTANT16();
                pthread_mutex_lock(&globals_mutex);
                InterpretResult r = handleDefineGlobal(vm, varNameVal);
                pthread_mutex_unlock(&globals_mutex);
                if (r != INTERPRET_OK) return r;
                break;
            }
            case OP_GET_GLOBAL: {
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
                        push(vm, makeCopyOfValue(sym->value));
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

                push(vm, makeCopyOfValue(sym->value));
                break;
            }
            case OP_GET_GLOBAL16: {
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
                        push(vm, makeCopyOfValue(sym->value));
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

                push(vm, makeCopyOfValue(sym->value));
                break;
            }
            case OP_SET_GLOBAL: {
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
            case OP_SET_GLOBAL16: {
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
            case OP_GET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                push(vm, makeCopyOfValue(&frame->slots[slot]));
                break;
            }
            case OP_SET_LOCAL: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
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
            case OP_GET_UPVALUE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeCopyOfValue(frame->upvalues[slot]));
                break;
            }
            case OP_SET_UPVALUE: {
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
            case OP_GET_UPVALUE_ADDRESS: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                if (slot >= frame->upvalue_count) {
                    runtimeError(vm, "VM Error: Upvalue index out of range.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makePointer(frame->upvalues[slot], NULL));
                break;
            }
            case OP_INIT_LOCAL_ARRAY: {
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
            case OP_INIT_LOCAL_FILE: {
                uint8_t slot = READ_BYTE();
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                *target_slot = makeValueForType(TYPE_FILE, NULL, NULL);
                break;
            }
            case OP_INIT_LOCAL_POINTER: {
                uint8_t slot = READ_BYTE();
                uint16_t type_name_idx = READ_SHORT(vm);
                AST* type_def = NULL;
                Value type_name_val = vm->chunk->constants[type_name_idx];
                if (type_name_val.type == TYPE_STRING && type_name_val.s_val && type_name_val.s_val[0] != '\0') {
                    type_def = lookupType(type_name_val.s_val);
                }
                CallFrame* frame = &vm->frames[vm->frameCount - 1];
                Value* target_slot = &frame->slots[slot];
                freeValue(target_slot);
                *target_slot = makeValueForType(TYPE_POINTER, type_def, NULL);
                break;
            }
            case OP_JUMP_IF_FALSE: {
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
            case OP_JUMP: {
                uint16_t offset = READ_SHORT(vm);
                vm->ip += (int16_t)offset;
                break;
            }
            case OP_WRITE_LN:
            case OP_WRITE: {
                uint8_t argCount = READ_BYTE();
                if (argCount > MAX_WRITELN_ARGS_VM) {
                    runtimeError(vm, "VM Error: Too many arguments for WRITE/WRITELN (max %d).", MAX_WRITELN_ARGS_VM);
                    return INTERPRET_RUNTIME_ERROR;
                }

                FILE *output_stream = stdout;
                int start_index = 0;

                // First arg may be FILE or ^FILE; route output accordingly.
                if (argCount > 0) {
                    Value first = vm->stackTop[-argCount];
                    const Value* f = &first;
                    if (first.type == TYPE_POINTER && first.ptr_val) f = (const Value*)first.ptr_val;
                    if (f->type == TYPE_FILE) {
                        if (!f->f_val) {
                            runtimeError(vm, "File not open for writing.");
                            // pop & clean args then bail...
                            for (int i = 0; i < argCount; i++) freeValue(&vm->stackTop[-i-1]);
                            vm->stackTop -= argCount;
                            return INTERPRET_RUNTIME_ERROR;
                        }
                        output_stream = f->f_val;
                        start_index = 1;
                    }
                }

                Value args_to_print[MAX_WRITELN_ARGS_VM];
                int print_arg_count = argCount - start_index;

                // Pop the printable arguments into a temporary array.
                for (int i = 0; i < print_arg_count; i++) {
                    args_to_print[print_arg_count - 1 - i] = pop(vm);
                }
                if (start_index > 0) {
                    // DO NOT free the file here; ownership stays with the variable.
                    Value file_arg = pop(vm);
                    if (file_arg.type != TYPE_FILE) freeValue(&file_arg);
                }

                bool color_was_applied = false; // Flag to track if we change the color

                // Apply console colors only if writing to stdout
                if (output_stream == stdout) {
                    color_was_applied = applyCurrentTextAttributes(output_stream);
                }

                // Print the arguments (strings as full buffers; chars as a single byte)
                for (int i = 0; i < print_arg_count; i++) {
                    Value val = args_to_print[i];
                    if (val.type == TYPE_STRING) {
                        if (output_stream == stdout) {
                            fputs(val.s_val ? val.s_val : "", output_stream);
                        } else {
                            size_t len = val.s_val ? strlen(val.s_val) : 0;
                            fwrite(val.s_val ? val.s_val : "", 1, len, output_stream);
                        }
                        freeValue(&val);
                    } else if (val.type == TYPE_CHAR) {
                        fputc(val.c_val, output_stream);
                        freeValue(&val);
                    } else {
                        printValueToStream(val, output_stream);
                        freeValue(&val);
                    }
                }

                if (instruction_val == OP_WRITE_LN) {
                    fprintf(output_stream, "\n");
                }

                // Reset console colors only if they were applied in this call.
                if (color_was_applied) {
                    resetTextAttributes(output_stream);
                }

                fflush(output_stream);
                break;
            }
            case OP_POP: {
                Value popped_val = pop(vm);
                freeValue(&popped_val);
                break;
            }
            case OP_CALL_BUILTIN: {
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
                        if (args[i].type != TYPE_ARRAY && args[i].type != TYPE_POINTER) {
                            // The actual Value structs are on the stack and will be overwritten,
                            // but we need to free any heap data they point to (like strings).
                            freeValue(&args[i]);
                        }
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
                        if (args[i].type != TYPE_ARRAY && args[i].type != TYPE_POINTER) {
                            freeValue(&args[i]);
                        }
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
            case OP_CALL: {
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

                vm->ip = vm->chunk->code + target_address;
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
                pthread_mutex_lock(&globals_mutex);
                Value result = func(vm);
                pthread_mutex_unlock(&globals_mutex);
                push(vm, result);
                break;
            }
            case OP_THREAD_CREATE: {
                uint16_t entry = READ_SHORT(vm);
                int id = createThread(vm, entry);
                if (id < 0) {
                    runtimeError(vm, "Thread limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case OP_THREAD_JOIN: {
                Value tidVal = peek(vm, 0);
                if (!IS_INTLIKE(tidVal)) {
                    runtimeError(vm, "Thread id must be integer.");
                    Value popped_tid = pop(vm);
                    freeValue(&popped_tid);
                    return INTERPRET_RUNTIME_ERROR;
                }
                int tid = (int)tidVal.i_val;
                joinThread(vm, tid);
                Value popped_tid = pop(vm);
                freeValue(&popped_tid);
                break;
            }
            case OP_MUTEX_CREATE: {
                int id = createMutex(vm, false);
                if (id < 0) {
                    runtimeError(vm, "Mutex limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case OP_RCMUTEX_CREATE: {
                int id = createMutex(vm, true);
                if (id < 0) {
                    runtimeError(vm, "Mutex limit exceeded.");
                    return INTERPRET_RUNTIME_ERROR;
                }
                push(vm, makeInt(id));
                break;
            }
            case OP_MUTEX_LOCK: {
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
            case OP_MUTEX_UNLOCK: {
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
            case OP_MUTEX_DESTROY: {
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
            case OP_FORMAT_VALUE: {
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
