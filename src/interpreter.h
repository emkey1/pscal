#ifndef INTERPRETER_H
#define INTERPRETER_H

#include "types.h"
#include "parser.h"
#include "ast.h"

// Execution
Value eval(AST *node);
Value executeProcedureCall(AST *node);
//ExecStatus executeWithScope(AST *node, bool is_global_scope);
void executeWithScope(AST *node, bool is_global_scope);


Value makeCopyOfValue(Value *src);
// Memory
FieldValue *copyRecord(FieldValue *orig);
FieldValue *createEmptyRecord(AST *recordType);

// Arrays
int computeFlatOffset(Value *array, int *indices);

// Program flow
typedef enum {
    EXEC_NORMAL,    // Continue execution normally
    EXEC_BREAK,     // A 'break' statement was encountered
    EXEC_CONTINUE,  // (Future) A 'continue' statement was encountered
    EXEC_EXIT       // (Future) An 'exit' (procedure exit) statement was encountered
    // Add EXEC_HALT if you want to handle halt this way too
} ExecStatus;

extern AST *GlobalASTRoot;

#endif // INTERPRETER_H
