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
Value executeBuiltinGotoXY(AST *node);
Value executeBuiltinKeyPressed(AST *node);
Value executeBuiltinLow(AST *node);
Value executeBuiltinHigh(AST *node);
Value executeBuiltinSucc(AST *node);
Value executeBuiltinTextColorE(AST *node);
Value executeBuiltinTextBackgroundE(AST *node);
Value executeBuiltinTextColor(AST *node);
Value executeBuiltinTextBackground(AST *node);
Value executeBuiltinBoldText(AST *node);
Value executeBuiltinUnderlineText(AST *node);
Value executeBuiltinBlinkText(AST *node);
Value executeBuiltinNormVideo(AST *node);
Value executeBuiltinLowVideo(AST *node);
Value executeBuiltinClrScr(AST *node);
Value executeBuiltinNew(AST *node);
Value executeBuiltinDispose(AST *node);
Value executeBuiltinRealToStr(AST *node);
int getBuiltinIDForCompiler(const char *name);

#ifdef SDL
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
Value vmBuiltinLoadimagetotexture(struct VM_s* vm, int arg_count, Value* args);
Value vmBuiltinWaitkeyevent(struct VM_s* vm, int arg_count, Value* args);

// Prototypes from audio.c (via audio.h)
Value executeBuiltinInitSoundSystem(AST *node);
Value executeBuiltinLoadSound(AST *node);
Value executeBuiltinPlaySound(AST *node);
Value executeBuiltinQuitSoundSystem(AST *node);
Value executeBuiltinIsSoundPlaying(AST *node);
#endif

// Prototypes from builtin_network_api.c (via its .h)
Value executeBuiltinAPISend(AST *node);
Value executeBuiltinAPIReceive(AST *node);

// General helper prototypes
void nullifyPointerAliasesByAddrValue(HashTable* table, uintptr_t disposedAddrValue);
int getCursorPosition(int *row, int *col);

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
