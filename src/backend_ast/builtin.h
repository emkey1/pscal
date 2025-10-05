#ifndef BUILTIN_H
#define BUILTIN_H

#include "core/types.h"
#include "ast/ast.h"
#include "Pascal/globals.h"
#include <stdbool.h>
#include <stddef.h>

struct VM_s;

typedef Value (*VmBuiltinFn)(struct VM_s* vm, int arg_count, Value* args);

typedef enum {
    BUILTIN_TYPE_NONE,
    BUILTIN_TYPE_PROCEDURE,
    BUILTIN_TYPE_FUNCTION
} BuiltinRoutineType;

typedef struct {
    const char* name;
    VmBuiltinFn handler;
} VmBuiltinMapping;

VmBuiltinFn getVmBuiltinHandler(const char* name);
VmBuiltinFn getVmBuiltinHandlerById(int id);
const char* getVmBuiltinNameById(int id);
void registerVmBuiltin(const char *vm_name, VmBuiltinFn handler,
                       BuiltinRoutineType type, const char *display_name);

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
Value vmBuiltinNewObj(struct VM_s* vm, int arg_count, Value* args);
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
Value vmBuiltinPrintf(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFprintf(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFopen(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFclose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCopy(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSetlength(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRealtostr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFormatfloat(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinStr(struct VM_s* vm, int arg_count, Value* args);
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
Value vmBuiltinClreol(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHidecursor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShowcursor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCursoroff(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCursoron(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinDeline(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinInsline(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinInvertcolors(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinNormalcolors(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinBeep(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSavecursor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRestorecursor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPushscreen(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPopscreen(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinHighvideo(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinGetenvint(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinValreal(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinWindow(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinQuitrequested(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReal(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinVMVersion(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinBytecodeVersion(struct VM_s* vm, int arg_count, Value* args);

/* Shell builtins */
Value vmBuiltinShellExec(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellPipeline(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellAnd(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellOr(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellSubshell(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellLoop(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellLoopEnd(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellIf(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellCase(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellCaseClause(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellCaseEnd(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellDefineFunction(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellCd(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellPwd(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellSource(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellEval(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellExit(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellRead(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellShift(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellSetenv(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellExport(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellUnset(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellUnsetenv(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellSet(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellTrap(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellLocal(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellBreak(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellContinue(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellAlias(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellHistory(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellJobs(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellFg(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellBg(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellWait(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellBuiltin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellColon(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellFinger(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinShellReturn(struct VM_s* vm, int arg_count, Value* args);
Value vmHostShellLastStatus(struct VM_s* vm);
Value vmHostShellLoopAdvance(struct VM_s* vm);
Value vmHostShellLoopIsReady(struct VM_s* vm);
Value vmHostShellPollJobs(struct VM_s* vm);
bool shellRuntimeConsumeExitRequested(void);
int shellRuntimeLastStatus(void);
void shellRuntimeRecordHistory(const char *line);
void shellRuntimeSetArg0(const char *name);
const char *shellRuntimeGetArg0(void);
void shellRuntimeInitJobControl(void);
void shellRuntimeInitSignals(void);
void shellRuntimeProcessPendingSignals(void);
size_t shellRuntimeHistoryCount(void);
bool shellRuntimeHistoryGetEntry(size_t reverse_index, char **out_line);
bool shellRuntimeExpandHistoryReference(const char *input,
                                        char **out_line,
                                        bool *out_did_expand,
                                        char **out_error_token);

/* VM-native file I/O */
Value vmBuiltinAssign(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReset(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRewrite(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinAppend(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinClose(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRename(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinErase(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinRead(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinReadln(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinWrite(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinEof(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinIoresult(struct VM_s* vm, int arg_count, Value* args);

/* VM-native memory stream functions */
Value vmBuiltinMstreamcreate(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreamloadfromfile(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreamsavetofile(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreamfree(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMstreambuffer(struct VM_s* vm, int arg_count, Value* args);

/* VM-native math functions */
Value vmBuiltinSqrt(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinExp(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLn(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCos(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTan(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinArctan(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinArcsin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinArccos(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCotan(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinPower(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinLog10(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinSinh(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCosh(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinTanh(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMax(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinMin(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinFloor(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinCeil(struct VM_s* vm, int arg_count, Value* args);
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

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc);
int isBuiltin(const char *name);
BuiltinRoutineType getBuiltinType(const char *name);

/* Register all built-in routines with the compiler/VM registry. */
void registerAllBuiltins(void);

/* Save and restore terminal state for the VM. */
void vmInitTerminalState(void);

/* Pause for ten seconds and require a key press before exit when running
 * interactively. */
void vmPauseBeforeExit(void);

/* Exit the VM after pausing and then restoring the terminal state. */
int vmExitWithCleanup(int status);

#endif // BUILTIN_H
