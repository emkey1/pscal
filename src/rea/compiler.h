#ifndef REA_COMPILER_H
#define REA_COMPILER_H

#include "rea/ast.h"
#include "ast/ast.h"

// Convert a Rea AST into the core AST used by the existing
// PSCAL backend.  The current implementation recognises a very
// small subset of the language – a single number literal – and
// wraps it in a "writeln" call so it can be executed by the VM.
//
// On success an AST_PROGRAM node is returned.  NULL indicates the
// Rea tree could not be translated.
AST *reaConvertToAST(ReaAST *root);

#endif // REA_COMPILER_H

