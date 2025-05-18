//
//  compiler.h
//  Pscal
//
//  Created by Michael Miller on 5/18/25.
//
#ifndef PSCAL_COMPILER_H
#define PSCAL_COMPILER_H

#include "frontend/ast.h"   // For AST* type
#include "bytecode.h"     // For BytecodeChunk struct

// Main function to compile an AST into a bytecode chunk.
// Returns true on success, false on failure.
// The BytecodeChunk outputChunk should be initialized by the caller before passing.
bool compileASTToBytecode(AST* rootNode, BytecodeChunk* outputChunk);

// You might also want a function to compile from a JSON file if that's your intermediate step:
// bool compileJSONASTFileToBytecode(const char* jsonFilePath, BytecodeChunk* outputChunk);
// For now, let's focus on compiling an in-memory AST.

#endif // PSCAL_COMPILER_H
