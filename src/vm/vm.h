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
#include <pthread.h>
#include <stdint.h>
#include <stddef.h>
#include <time.h>
#include <sys/resource.h>
#include <stdatomic.h>

// --- VM Configuration ---
#define VM_STACK_MAX 8192       // Maximum number of Values on the operand stack
#define VM_GLOBALS_MAX 4096     // Maximum number of global variables (for simple array storage)

#define MAX_HOST_FUNCTIONS 4096

#define VM_CALL_STACK_MAX 4096
#define VM_MAX_THREADS 16
#define VM_MAX_MUTEXES 64

#ifndef THREAD_NAME_MAX
#define THREAD_NAME_MAX 64
#endif

#ifndef VM_MAX_WORKERS
#define VM_MAX_WORKERS (VM_MAX_THREADS - 1)
#endif

// Flags for the VM write/writeln builtin.
#define VM_WRITE_FLAG_NEWLINE           0x1
#define VM_WRITE_FLAG_SUPPRESS_SPACING  0x2

// Forward declaration
struct VM_s;

// Host function pointer type
typedef Value (*HostFn)(struct VM_s* vm);

// Enum to identify specific host functions
typedef enum {
    HOST_FN_QUIT_REQUESTED,
    HOST_FN_CREATE_THREAD_ADDR,
    HOST_FN_WAIT_THREAD,
    HOST_FN_PRINTF,
    HOST_FN_SHELL_LAST_STATUS,
    HOST_FN_SHELL_LOOP_CHECK_CONDITION,
    HOST_FN_SHELL_LOOP_CHECK_BODY,
    HOST_FN_SHELL_LOOP_EXEC_BODY,
    HOST_FN_SHELL_LOOP_ADVANCE,
    HOST_FN_SHELL_POLL_JOBS,
    HOST_FN_SHELL_LOOP_IS_READY,
    HOST_FN_CREATE_CLOSURE,
    HOST_FN_BOX_INTERFACE,
    HOST_FN_INTERFACE_LOOKUP,
    HOST_FN_INTERFACE_ASSERT,
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
                                // Note: Storing Symbol* is one way; alternatively, CALL could carry locals_count,
                                // or RETURN could be generic if stack is always reset to frame->slots.
    uint16_t slotCount;         // Total slots (arguments + locals) reserved for this frame
    uint8_t locals_count;       // Number of local variables (excluding params)
    uint8_t upvalue_count;
    Value** upvalues;
    bool owns_upvalues;
    ClosureEnvPayload* closureEnv;
    bool discard_result_on_return; // If true, drop any function result on return
    Value* vtable;               // Reference to class V-table when executing a method
} CallFrame;

typedef struct ThreadJob ThreadJob;

typedef struct {
    bool valid;
    struct timespec cpuTime;    // CPU time reading for the worker
    struct rusage usage;        // Resource usage snapshot
    size_t rssBytes;            // Resident set size snapshot (bytes)
} ThreadMetricsSample;

typedef struct {
    ThreadMetricsSample start;
    ThreadMetricsSample end;
} ThreadMetrics;

typedef struct {
    pthread_t handle;           // OS-level thread handle
    struct VM_s* vm;            // Pointer to the VM executing on this thread
    bool active;                // Whether this thread is running

    // Result hand-off state for builtin jobs.
    bool statusReady;           // True when a worker has published a status/result
    bool statusFlag;            // Worker-reported success flag
    bool statusConsumed;        // Tracks whether readers consumed the status
    bool resultReady;           // True when resultValue holds a pending result
    bool resultConsumed;        // Tracks whether the pending result has been taken
    Value resultValue;          // Stored builtin result value

    pthread_mutex_t resultMutex; // Protects result/status hand-off
    pthread_cond_t resultCond;   // Notifies waiters when hand-off is ready
    bool syncInitialized;        // True once mutex/cond initialised

    // Cooperative scheduling controls & identity
    char name[THREAD_NAME_MAX];
    atomic_bool paused;             // Worker paused flag
    atomic_bool cancelRequested;    // Cancellation requested flag
    atomic_bool killRequested;      // Hard termination requested

    // Pool ownership bookkeeping
    bool inPool;                    // True when thread participates in worker pool
    bool idle;                      // True when waiting for a job
    bool shouldExit;                // Signals worker loop shutdown
    bool ownsVm;                    // Tracks whether vm pointer should be destroyed
    int poolGeneration;             // Bumps when recycled so handles stay unique
    bool poolWorker;                // True when thread should be reported as part of the shared pool
    ThreadJob* currentJob;          // Currently executing job (if any)
    bool awaitingReuse;             // True once job finished but not yet released
    bool readyForReuse;             // Set by consumers to allow thread to return to pool

    struct timespec queuedAt;       // Timestamp when job was queued
    struct timespec startedAt;      // Timestamp when job began execution
    struct timespec finishedAt;     // Timestamp when job finished execution

    ThreadMetrics metrics;        // Metrics captured for last executed job

    pthread_mutex_t stateMutex;    // Protects paused/cancel flags and state transitions
    pthread_cond_t stateCond;      // Signals state changes (pause/resume/kill)
    bool stateSyncInitialized;     // True once state mutex/cond initialised
} Thread;

typedef void (*VMThreadCallback)(struct VM_s* threadVm, void* user_data);
typedef void (*VMThreadCleanup)(void* user_data);

typedef struct {
    pthread_mutex_t handle;
    bool active;
} Mutex;

// --- Virtual Machine Structure ---
typedef struct VM_s {
    BytecodeChunk* chunk;     // The chunk of bytecode to execute
    uint8_t* ip;              // Instruction Pointer: points to the *next* byte to be read
    uint8_t* lastInstruction; // Start of the last executed instruction

    Value stack[VM_STACK_MAX]; // The operand stack
    Value* stackTop;          // Pointer to the element just above the top of the stack
                              // (i.e., where the next pushed item will go)

    HashTable* vmGlobalSymbols;      // VM's own symbol table for runtime global variable storage
    HashTable* vmConstGlobalSymbols; // Separate table for constant globals (read-only, no mutex)
    HashTable* procedureTable;      // store procedure table for disassembly
    Symbol** procedureByAddress;    // Cache mapping bytecode offsets to procedure symbols
    size_t procedureByAddressSize;  // Number of cached entries
    
    HostFn host_functions[MAX_HOST_FUNCTIONS];

    CallFrame frames[VM_CALL_STACK_MAX];
    int frameCount;

    bool exit_requested;      // Indicates a builtin requested early exit from the current frame
    bool abort_requested;     // Raised when a builtin requests an immediate interpreter abort
    const char* current_builtin_name; // Tracks the name of the builtin currently executing (for diagnostics)

    // Threading support
    Thread threads[VM_MAX_THREADS];
    int threadCount;
    struct VM_s* threadOwner;

    pthread_mutex_t threadRegistryLock; // Protects worker pool state
    struct ThreadJobQueue* jobQueue;    // Shared job queue for worker reuse
    int workerCount;                    // Number of worker threads allocated
    int availableWorkers;               // Number of idle workers in pool
    atomic_bool shuttingDownWorkers;    // Signals pool shutdown

    // Mutex support
    Mutex mutexes[VM_MAX_MUTEXES];
    int mutexCount;
    pthread_mutex_t mutexRegistryLock; // Protects mutex registry updates
    struct VM_s* mutexOwner; // VM that owns the mutex registry

    Thread* owningThread;        // Non-NULL when running inside a worker slot
    int threadId;                // Slot index for owningThread (0 for main VM)

    /* Frontend-specific context; e.g., exsh per-VM shell state. */
    void* frontendContext;
    /* String indexing mode: true for shell-style (0-based), false for Pascal/REA (1-based). */
    bool shellIndexing;

    // Optional tracing: when >0, print execution of first N instructions
    int trace_head_instructions;
    int trace_executed;

} VM;

// --- Public VM Interface ---
void initVM(VM* vm);    // Initialize a new VM instance
void freeVM(VM* vm);    // Free resources associated with a VM instance
void vmResetExecutionState(VM* vm); // Reset stack/frames so a VM can be reused

// Main function to interpret a chunk of bytecode
// Takes a BytecodeChunk that was successfully compiled.
InterpretResult interpretBytecode(VM* vm, BytecodeChunk* chunk, HashTable* globals, HashTable* const_globals, HashTable* procedures, uint16_t entry);
void vmNullifyAliases(VM* vm, uintptr_t disposedAddrValue);
int vmSpawnCallbackThread(VM* vm, VMThreadCallback callback, void* user_data, VMThreadCleanup cleanup);
int vmSpawnBuiltinThread(VM* vm, int builtinId, const char* builtinName, int argCount,
                         const Value* args, bool submitOnly, const char* threadName);
void vmThreadStoreResult(VM* vm, const Value* result, bool success);
bool vmThreadTakeResult(VM* vm, int threadId, Value* outResult, bool takeValue, bool* outStatus, bool takeStatus);
bool vmJoinThreadById(struct VM_s* vm, int id);
bool vmThreadAssignName(struct VM_s* vm, int threadId, const char* name);
int vmThreadFindIdByName(struct VM_s* vm, const char* name);
bool vmThreadPause(struct VM_s* vm, int threadId);
bool vmThreadResume(struct VM_s* vm, int threadId);
bool vmThreadCancel(struct VM_s* vm, int threadId);
bool vmThreadKill(struct VM_s* vm, int threadId);
size_t vmSnapshotWorkerUsage(struct VM_s* vm, ThreadMetrics* outMetrics, size_t capacity);

// Register and lookup class methods in the VM's procedure table
void vmRegisterClassMethod(VM* vm, const char* className, uint16_t methodIndex, Symbol* methodSymbol);
Symbol* vmFindClassMethod(VM* vm, const char* className, uint16_t methodIndex);

void runtimeError(VM* vm, const char* format, ...);
void runtimeWarning(VM* vm, const char* format, ...);
void vmDumpStackInfo(VM* vm);
void vmDumpStackInfoDetailed(VM* vm, const char* context_message);
void vmSetSuppressStateDump(bool suppress);
void vmSetVerboseErrors(bool enabled);
void vmOpcodeProfileDump(void);
bool vmOpcodeProfileIsEnabled(void);
void vmProfileShellBuiltin(const char *name);

#endif // PSCAL_VM_H
