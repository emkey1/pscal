#ifndef INTERPRETER_H
#define INTERPRETER_H

#include "types.h"
#include "parser.h"
#include "ast.h"

#define PASCAL_DEFAULT_FLOAT_PRECISION 6

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

Value* resolveLValueToPtr(AST* lvalueNode); // Returns pointer to the Value struct to be modified

// Program flow
typedef enum {
    EXEC_NORMAL,    // Continue execution normally
    EXEC_BREAK,     // A 'break' statement was encountered
    EXEC_CONTINUE,  // (Future) A 'continue' statement was encountered
    EXEC_EXIT       // (Future) An 'exit' (procedure exit) statement was encountered
    // Add EXEC_HALT if you want to handle halt this way too
} ExecStatus;

// Various set related stuff
Value setUnion(Value setA, Value setB);
Value setDifference(Value setA, Value setB);
Value setIntersection(Value setA, Value setB);

#endif // INTERPRETER_H
