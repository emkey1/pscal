#ifndef BUILTIN_H
#define BUILTIN_H



#include "types.h"
#include "ast.h"
#include "globals.h"

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
Value executeBuiltinExit(AST *node); // Exit current routine only (use halt to terminate program)
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
Value executeBuiltinTextColor(AST *node); // Existing 16-color
Value executeBuiltinTextBackground(AST *node); // Existing 16-color
Value executeBuiltinTextColorE(AST *node); // <--- ADD for 256 foreground
Value executeBuiltinTextBackgroundE(AST *node); // <--- ADD for 256 background

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


#endif
