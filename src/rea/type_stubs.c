#include "ast/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include "Pascal/globals.h"
#include <strings.h>

#ifdef FRONTEND_REA
#undef lookupType
#undef insertType
#undef newASTNode
#undef setTypeAST
#undef setRight
#undef addChild
#undef freeAST
#undef dumpAST
#undef copyAST
#undef evaluateCompileTimeValue
#endif

AST* newASTNode(ASTNodeType type, Token* token);
void setTypeAST(AST* node, VarType type);
void setRight(AST* parent, AST* child);
void addChild(AST* parent, AST* child);
void freeAST(AST* node);
AST* copyAST(AST* node);
Value evaluateCompileTimeValue(AST* node);

// Simple type table integration for the Rea front end.
//
// The Pascal front end maintains a global linked list of TypeEntry records
// (see TypeEntry in core/types.h).  Rea reuses this table so that user-defined
// classes can be referenced later in the program.  These helpers mirror the
// minimal functionality required by the parser: inserting newly declared types
// and looking them up by name.

AST* rea_lookupType(const char* name) {
    // First search any user-defined types that have been registered via
    // insertType().  The table lives in globals.c and is shared across the
    // front ends.
    for (TypeEntry* entry = type_table; entry; entry = entry->next) {
        if (entry->name && name && strcasecmp(entry->name, name) == 0) {
            return entry->typeAST;
        }
    }

    // If no user-defined type matches, fall back to a small set of builtin
    // names.  We construct a transient AST node with the appropriate VarType so
    // that later stages (such as code generation) can reason about the type.
    if (!name) return NULL;

    AST* node = newASTNode(AST_VARIABLE, NULL);
    if (!node) return NULL;

    if      (strcasecmp(name, "int")      == 0 ||
             strcasecmp(name, "int64")    == 0) setTypeAST(node, TYPE_INT64);
    else if (strcasecmp(name, "int32")    == 0) setTypeAST(node, TYPE_INT32);
    else if (strcasecmp(name, "int16")    == 0) setTypeAST(node, TYPE_INT16);
    else if (strcasecmp(name, "int8")     == 0) setTypeAST(node, TYPE_INT8);
    else if (strcasecmp(name, "float")    == 0) setTypeAST(node, TYPE_DOUBLE);
    else if (strcasecmp(name, "float32")  == 0) setTypeAST(node, TYPE_FLOAT);
    else if (strcasecmp(name, "long double") == 0)
        setTypeAST(node, TYPE_LONG_DOUBLE);
    else if (strcasecmp(name, "char")     == 0) setTypeAST(node, TYPE_CHAR);
    else if (strcasecmp(name, "byte")     == 0) setTypeAST(node, TYPE_BYTE);
    else if (strcasecmp(name, "str")      == 0 ||
             strcasecmp(name, "text")     == 0)
        setTypeAST(node, TYPE_STRING);
    else if (strcasecmp(name, "mstream")  == 0)
        setTypeAST(node, TYPE_MEMORYSTREAM);
    else if (strcasecmp(name, "bool")     == 0) setTypeAST(node, TYPE_BOOLEAN);
    else if (strcasecmp(name, "void")     == 0) setTypeAST(node, TYPE_VOID);
    else {
        freeAST(node);
        return NULL;
    }

    return node;
}

void rea_insertType(const char* name, AST* typeDef) {
    if (!name || !typeDef) return;

    TypeEntry* entry = (TypeEntry*)malloc(sizeof(TypeEntry));
    if (!entry) return;
    entry->name = strdup(name);
    entry->typeAST = copyAST(typeDef);
    entry->next = type_table;
    type_table = entry;
}

AST* rea_newASTNode(ASTNodeType type, Token* token) {
    return newASTNode(type, token);
}

void rea_setTypeAST(AST* node, VarType type) {
    setTypeAST(node, type);
}

void rea_setRight(AST* parent, AST* child) {
    setRight(parent, child);
}

void rea_addChild(AST* parent, AST* child) {
    addChild(parent, child);
}

void rea_freeAST(AST* node) {
    freeAST(node);
}

AST* rea_copyAST(AST* node) {
    return copyAST(node);
}

Value rea_evaluateCompileTimeValue(AST* node) {
    return evaluateCompileTimeValue(node);
}
