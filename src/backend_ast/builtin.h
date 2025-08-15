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
Value vmBuiltinInttostr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinApiReceive(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinApiSend(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLength(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinAbs(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRound(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHalt(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDelay(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinNew(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDispose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinExit(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinOrd(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinInc(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDec(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLow(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHigh(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinScreencols(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinScreenrows(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSqr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinChr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSucc(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinUpcase(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPos(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCopy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSetlength(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRealtostr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinParamcount(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinParamstr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinWherex(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinWherey(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGotoxy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinKeypressed(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReadkey(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTextcolor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTextbackground(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTextcolore(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTextbackgrounde(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinBoldtext(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinUnderlinetext(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinBlinktext(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinNormvideo(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLowvideo(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClrscr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinQuitrequested(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReal(struct VM_s* vm, int arg_count, Value* args); // ADDED

// --- VM-NATIVE FILE I/O ---
Value vmBuiltinAssign(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReset(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRewrite(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRead(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReadln(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinEof(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinIoresult(struct VM_s* vm, int arg_count, Value* args);

// --- VM-NATIVE MEMORY STREAM FUNCTIONS ---
Value vmBuiltinMstreamcreate(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vmBuiltinMstreamloadfromfile(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vmBuiltinMstreamsavetofile(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vmBuiltinMstreamfree(struct VM_s* vm, int arg_count, Value* args); // ADDED

// --- VM-NATIVE MATHY STUFF ---
Value vmBuiltinSqrt(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinExp(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLn(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCos(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTan(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTrunc(struct VM_s* vm, int arg_count, Value* args);

// --- VM-NATIVE RANDOM FUNCTIONS ---
Value vmBuiltinRandomize(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRandom(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinVal(struct VM_s* vm, int arg_count, Value* args);

// --- VM-NATIVE DOS/OS FUNCTIONS ---
Value vmBuiltinDosGetenv(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetenv(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosExec(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosMkdir(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosRmdir(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosFindfirst(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosFindnext(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosGetdate(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosGettime(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDosGetfattr(struct VM_s* vm, int arg_count, Value* args);
// General helper prototypes
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue);
int getCursorPosition(int *row, int *col);

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc);
int isBuiltin(const char *name);

#ifdef SDL
Value vmBuiltinLoadimagetotexture(struct VM_s* vm, int arg_count, Value* args); // ADDED
Value vmBuiltinWaitkeyevent(struct VM_s* vm, int arg_count, Value* args); // ADDED
#endif

#endif // BUILTIN_H
