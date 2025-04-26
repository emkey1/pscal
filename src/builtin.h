#ifndef BUILTIN_H
#define BUILTIN_H



#include "types.h"
#include "ast.h"

// Math Functions
Value executeBuiltinCos(AST *node);
Value executeBuiltinSin(AST *node);
Value executeBuiltinTan(AST *node);
Value executeBuiltinSqrt(AST *node);
Value executeBuiltinLn(AST *node);
Value executeBuiltinExp(AST *node);
Value executeBuiltinAbs(AST *node);
Value executeBuiltinTrunc(AST *node);

// File I/O
void executeBuiltinAssign(AST *node);
void executeBuiltinClose(AST *node);
void executeBuiltinReset(AST *node);
void executeBuiltinRewrite(AST *node);
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

// System
void executeBuiltinHalt(AST *node);
void executeBuiltinInc(AST *node);
void executeBuiltinRandomize(AST *node);
Value executeBuiltinRandom(AST *node);
void executeBuiltinDelay(AST *node);

// Memory Streams
Value executeBuiltinMstreamCreate(AST *node);
Value executeBuiltinMstreamLoadFromFile(AST *node);
Value executeBuiltinMstreamSaveToFile(AST *node);
void executeBuiltinMstreamFree(AST *node);

// Support
Value executeBuiltinResult(AST *node);
Value executeBuiltinProcedure(AST *node);
void registerBuiltinFunction(const char *name, ASTNodeType declType);
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

typedef enum {
    BUILTIN_TYPE_NONE,      // Not a built-in routine
    BUILTIN_TYPE_PROCEDURE, // Built-in, does not return a value usable in expressions
    BUILTIN_TYPE_FUNCTION   // Built-in, returns a value usable in expressions
} BuiltinRoutineType;

// The rest
BuiltinRoutineType getBuiltinType(const char *name);
void assignValueToLValue(AST *lvalueNode, Value newValue);


#endif
