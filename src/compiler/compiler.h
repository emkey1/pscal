#ifndef PSCAL_COMPILER_H // Ensure include guards
#define PSCAL_COMPILER_H

#include "Pascal/ast.h"
#include "compiler/bytecode.h"
// Assuming MAX_COMPILER_CONSTANTS is defined here or in another included header like types.h/globals.h
// If not, define it here. For example: #define MAX_COMPILER_CONSTANTS 128

typedef struct {
    char* name;
    Value value;
} CompilerConstant;

#define MAX_COMPILER_CONSTANTS 128 // Adjust as needed

extern CompilerConstant compilerConstants[MAX_COMPILER_CONSTANTS];
extern int compilerConstantCount;

void resetCompilerConstants(void);
void addCompilerConstant(const char* name_original_case, const Value* value, int line);
Value* findCompilerConstant(const char* name_original_case);
Value evaluateCompileTimeValue(AST* node); // For parser to evaluate const expressions

bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk);
void compileUnitImplementation(AST* unit_ast, BytecodeChunk* outputChunk);

void finalizeBytecode(BytecodeChunk* chunk);

#endif // PSCAL_COMPILER_H
