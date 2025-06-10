#ifndef BUILTIN_H
#define BUILTIN_H

#include "core/types.h"
#include "frontend/ast.h"
#include "globals.h"

// --- START: MODIFIED VM BUILT-IN SECTION ---

// Forward declare the VM struct to break circular include dependencies.
struct VM_s;

// New signature for VM-native built-in functions, using the struct tag.
typedef Value (*VmBuiltinFn)(struct VM_s* vm, int arg_count, Value* args);

// New struct for the VM's built-in dispatch table.
typedef struct {
    const char* name;
    VmBuiltinFn handler;
} VmBuiltinMapping;

// Function to get a handler from the new VM dispatch table.
VmBuiltinFn getVmBuiltinHandler(const char* name);

// Prototypes for VM-native general-purpose built-in handlers
Value vm_builtin_inttostr(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_length(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_abs(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_round(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_halt(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_delay(struct VM_s* vm, int arg_count, Value* args);

// --- END: MODIFIED VM BUILT-IN SECTION ---


// --- AST-based built-in handlers (for AST interpreter path) ---

// Math Functions
Value executeBuiltinCos(AST *node);
Value executeBuiltinSin(AST *node);
Value executeBuiltinTan(AST *node);
Value executeBuiltinSqrt(AST *node);
Value executeBuiltinLn(AST *node);
Value executeBuiltinExp(AST *node);
Value executeBuiltinAbs(AST *node);
Value executeBuiltinTrunc(AST *node);
Value executeBuiltinSqr(AST *node);
Value executeBuiltinRound(AST *node);

// File I/O
Value executeBuiltinAssign(AST *node);
Value executeBuiltinClose(AST *node);
Value executeBuiltinReset(AST *node);
Value executeBuiltinRewrite(AST *node);
Value executeBuiltinEOF(AST *node);
Value executeBuiltinIOResult(AST *node);

// Strings & char
Value executeBuiltinCopy(AST *node);
Value executeBuiltinLength(AST *node);
Value executeBuiltinPos(AST *node);
Value executeBuiltinUpcase(AST *node);
Value executeBuiltinOrd(AST *node);
Value executeBuiltinChr(AST *node);
Value executeBuiltinIntToStr(AST *node);
Value executeBuiltinReal(AST *node);
Value executeBuiltinRealToStr(AST *node);

// System
Value executeBuiltinHalt(AST *node);
Value executeBuiltinInc(AST *node);
Value executeBuiltinRandomize(AST *node);
Value executeBuiltinRandom(AST *node);
Value executeBuiltinDelay(AST *node);
Value executeBuiltinDec(AST *node);
Value executeBuiltinReadKey(AST *node);

// Memory Streams
Value executeBuiltinMstreamCreate(AST *node);
Value executeBuiltinMstreamLoadFromFile(AST *node);
Value executeBuiltinMstreamSaveToFile(AST *node);
Value executeBuiltinMstreamFree(AST *node);

// Support
Value executeBuiltinResult(AST *node);
Value executeBuiltinProcedure(AST *node);
void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc);
int isBuiltin(const char *name);
int getBuiltinIDForCompiler(const char *name);

// Networking
Value executeBuiltinAPISend(AST *node);
Value executeBuiltinAPIReceive(AST *node);

// Command line parsing
Value executeBuiltinParamcount(AST *node);
Value executeBuiltinParamstr(AST *node);

// Terminal IO
Value executeBuiltinWhereX(AST *node);
Value executeBuiltinWhereY(AST *node);
Value executeBuiltinKeyPressed(AST *node);
Value executeBuiltinScreenCols(AST *node);
Value executeBuiltinScreenRows(AST *node);

// Ordinal Functions (Low, High, Succ)
Value executeBuiltinLow(AST *node);
Value executeBuiltinHigh(AST *node);
Value executeBuiltinSucc(AST *node);

// Terminal Extended Color stuff
Value executeBuiltinTextColor(AST *node);
Value executeBuiltinTextBackground(AST *node);
Value executeBuiltinTextColorE(AST *node);
Value executeBuiltinTextBackgroundE(AST *node);

// Pointers/Memory Management
Value executeBuiltinNew(AST *node);
Value executeBuiltinDispose(AST *node);
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue);

typedef enum {
    BUILTIN_TYPE_NONE,      // Not a built-in routine
    BUILTIN_TYPE_PROCEDURE, // Built-in, does not return a value usable in expressions
    BUILTIN_TYPE_FUNCTION   // Built-in, returns a value usable in expressions
} BuiltinRoutineType;

// The rest
BuiltinRoutineType getBuiltinType(const char *name);
void assignValueToLValue(AST *lvalueNode, Value newValue);
BuiltinHandler getBuiltinHandler(const char *name);

#endif // BUILTIN_H
