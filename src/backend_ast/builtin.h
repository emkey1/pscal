#ifndef BUILTIN_H
#define BUILTIN_H

#include "core/types.h"
#include "frontend/ast.h"
#include "globals.h"

struct VM_s;

typedef Value (*VmBuiltinFn)(struct VM_s* vm, int arg_count, Value* args);

typedef struct {
    const char* name;
    VmBuiltinFn handler;
} VmBuiltinMapping;

VmBuiltinFn getVmBuiltinHandler(const char* name);
void registerVmBuiltin(const char *name, VmBuiltinFn handler);

/* Optional hook for externally linked built-ins.  The weak
 * definition in builtin.c does nothing unless overridden. */
void registerExtendedBuiltins(void);

/* VM-native general built-ins */
Value vmBuiltinInttostr(struct VM_s* vm, int arg_count, Value* args);
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
Value vmBuiltinReal(struct VM_s* vm, int arg_count, Value* args);

/* VM-native file I/O */
Value vmBuiltinAssign(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReset(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRewrite(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRead(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReadln(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinEof(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinIoresult(struct VM_s* vm, int arg_count, Value* args);

/* VM-native memory stream functions */
Value vmBuiltinMstreamcreate(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreamloadfromfile(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreamsavetofile(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreamfree(struct VM_s* vm, int arg_count, Value* args);

/* VM-native math functions */
Value vmBuiltinSqrt(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinExp(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLn(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCos(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTan(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTrunc(struct VM_s* vm, int arg_count, Value* args);

/* VM-native random functions */
Value vmBuiltinRandomize(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRandom(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinVal(struct VM_s* vm, int arg_count, Value* args);

/* VM-native DOS/OS functions */
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

int getBuiltinIDForCompiler(const char *name);

typedef enum {
    BUILTIN_TYPE_NONE,
    BUILTIN_TYPE_PROCEDURE,
    BUILTIN_TYPE_FUNCTION
} BuiltinRoutineType;

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc);
int isBuiltin(const char *name);
BuiltinRoutineType getBuiltinType(const char *name);

/* Register all built-in routines with the compiler/VM registry. */
void registerAllBuiltins(void);

#endif // BUILTIN_H
