// src/backend_ast/builtin.h
#ifndef BUILTIN_H
#define BUILTIN_H

#include "core/types.h"
#include "frontend/ast.h"
#include "globals.h"

// Forward declare the VM struct to break circular include dependencies
struct VM_s;

// New signature for VM-native built-in functions
typedef Value (*VmBuiltinFn)(struct VM_s* vm, int arg_count, Value* args);

// Struct for the VM's built-in dispatch table
typedef struct {
    const char* name;
    VmBuiltinFn handler;
} VmBuiltinMapping;

// Function to get a handler from the VM dispatch table
VmBuiltinFn getVmBuiltinHandler(const char* name);

// --- VM-NATIVE GENERAL BUILT-INS ---
Value vm_builtin_inttostr(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_length(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_abs(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_round(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_halt(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_delay(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_new(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_dispose(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_exit(struct VM_s* vm, int arg_count, Value* args);

// --- VM-NATIVE FILE I/O ---
Value vm_builtin_assign(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_reset(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_close(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_readln(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_eof(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_ioresult(struct VM_s* vm, int arg_count, Value* args);

// --- VM-NATIVE RANDOM FUNCTIONS ---
Value vm_builtin_randomize(struct VM_s* vm, int arg_count, Value* args);
Value vm_builtin_random(struct VM_s* vm, int arg_count, Value* args);

// --- AST-BASED BUILT-INS (for AST interpreter) ---

// Prototypes from builtin.c
Value executeBuiltinExit(AST *node);
Value executeBuiltinCos(AST *node);
Value executeBuiltinSin(AST *node);
Value executeBuiltinTan(AST *node);
Value executeBuiltinSqrt(AST *node);
Value executeBuiltinLn(AST *node);
Value executeBuiltinExp(AST *node);
Value executeBuiltinAbs(AST *node);
Value executeBuiltinTrunc(AST *node);
Value executeBuiltinRound(AST *node);
Value executeBuiltinAssign(AST *node);
Value executeBuiltinClose(AST *node);
Value executeBuiltinReset(AST *node);
Value executeBuiltinRewrite(AST *node);
Value executeBuiltinEOF(AST *node);
Value executeBuiltinIOResult(AST *node);
Value executeBuiltinLength(AST *node);
Value executeBuiltinCopy(AST *node);
Value executeBuiltinPos(AST *node);
Value executeBuiltinSqr(AST *node);
Value executeBuiltinUpcase(AST *node);
Value executeBuiltinReadKey(AST *node);
Value executeBuiltinOrd(AST *node);
Value executeBuiltinChr(AST *node);
Value executeBuiltinHalt(AST *node);
Value executeBuiltinIntToStr(AST *node);
Value executeBuiltinReal(AST *node);
Value executeBuiltinInc(AST *node);
Value executeBuiltinDec(AST *node);
Value executeBuiltinScreenCols(AST *node);
Value executeBuiltinScreenRows(AST *node);
Value executeBuiltinRandomize(AST *node);
Value executeBuiltinRandom(AST *node);
Value executeBuiltinDelay(AST *node);
Value executeBuiltinMstreamCreate(AST *node);
Value executeBuiltinMstreamLoadFromFile(AST *node);
Value executeBuiltinMstreamSaveToFile(AST *node);
Value executeBuiltinMstreamFree(AST *node);
Value executeBuiltinResult(AST *node);
Value executeBuiltinParamcount(AST *node);
Value executeBuiltinParamstr(AST *node);
Value executeBuiltinWhereX(AST *node);
Value executeBuiltinWhereY(AST *node);
Value executeBuiltinKeyPressed(AST *node);
Value executeBuiltinLow(AST *node);
Value executeBuiltinHigh(AST *node);
Value executeBuiltinSucc(AST *node);
Value executeBuiltinTextColorE(AST *node);
Value executeBuiltinTextBackgroundE(AST *node);
Value executeBuiltinTextColor(AST *node);
Value executeBuiltinTextBackground(AST *node);
Value executeBuiltinNew(AST *node);
Value executeBuiltinDispose(AST *node);
Value executeBuiltinRealToStr(AST *node);
int getBuiltinIDForCompiler(const char *name);


// Prototypes from sdl.c (via sdl.h)
Value executeBuiltinInitGraph(AST *node);
Value executeBuiltinCloseGraph(AST *node);
Value executeBuiltinGraphLoop(AST *node);
Value executeBuiltinUpdateScreen(AST *node);
Value executeBuiltinClearDevice(AST *node);
Value executeBuiltinWaitKeyEvent(AST *node);
Value executeBuiltinGetMaxX(AST *node);
Value executeBuiltinGetMaxY(AST *node);
Value executeBuiltinGetTicks(AST *node);
Value executeBuiltinGetMouseState(AST *node);
Value executeBuiltinQuitRequested(AST *node);
Value executeBuiltinSetColor(AST *node);
Value executeBuiltinSetRGBColor(AST *node);
Value executeBuiltinPutPixel(AST *node);
Value executeBuiltinDrawLine(AST *node);
Value executeBuiltinDrawRect(AST *node);
Value executeBuiltinFillRect(AST *node);
Value executeBuiltinDrawCircle(AST *node);
Value executeBuiltinFillCircle(AST *node);
Value executeBuiltinDrawPolygon(AST *node);
Value executeBuiltinGetPixelColor(AST *node);
Value executeBuiltinInitTextSystem(AST *node);
Value executeBuiltinQuitTextSystem(AST *node);
Value executeBuiltinOutTextXY(AST *node);
Value executeBuiltinGetTextSize(AST *node);
Value executeBuiltinCreateTexture(AST *node);
Value executeBuiltinCreateTargetTexture(AST *node);
Value executeBuiltinDestroyTexture(AST *node);
Value executeBuiltinUpdateTexture(AST *node);
Value executeBuiltinSetRenderTarget(AST *node);
Value executeBuiltinRenderCopy(AST *node);
Value executeBuiltinRenderCopyRect(AST *node);
Value executeBuiltinRenderCopyEx(AST *node);
Value executeBuiltinLoadImageToTexture(AST *node);
Value executeBuiltinRenderTextToTexture(AST *node);
Value executeBuiltinSetAlphaBlend(AST *node);

// Prototypes from audio.c (via audio.h)
Value executeBuiltinInitSoundSystem(AST *node);
Value executeBuiltinLoadSound(AST *node);
Value executeBuiltinPlaySound(AST *node);
Value executeBuiltinQuitSoundSystem(AST *node);
Value executeBuiltinIsSoundPlaying(AST *node);

// Prototypes from builtin_network_api.c (via its .h)
Value executeBuiltinAPISend(AST *node);
Value executeBuiltinAPIReceive(AST *node);


// General helper prototypes
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue);

typedef enum {
    BUILTIN_TYPE_NONE,
    BUILTIN_TYPE_PROCEDURE,
    BUILTIN_TYPE_FUNCTION
} BuiltinRoutineType;

BuiltinRoutineType getBuiltinType(const char *name);
void assignValueToLValue(AST *lvalueNode, Value newValue);
BuiltinHandler getBuiltinHandler(const char *name);

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc);
int isBuiltin(const char *name);
Value executeBuiltinProcedure(AST *node);

#endif // BUILTIN_H
