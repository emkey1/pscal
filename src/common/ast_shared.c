#include "common/ast_shared.h"
#include "core/utils.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

AST* sharedLookupType(const char* name) {
    if (!name) return NULL;
    AST* type = sharedNewASTNode(AST_VARIABLE, NULL);
    if (!type) return NULL;
    if      (strcasecmp(name, "integer") == 0) sharedSetTypeAST(type, TYPE_INTEGER);
    else if (strcasecmp(name, "real")    == 0) sharedSetTypeAST(type, TYPE_REAL);
    else if (strcasecmp(name, "char")    == 0) sharedSetTypeAST(type, TYPE_CHAR);
    else if (strcasecmp(name, "string")  == 0) sharedSetTypeAST(type, TYPE_STRING);
    else if (strcasecmp(name, "boolean") == 0) sharedSetTypeAST(type, TYPE_BOOLEAN);
    else if (strcasecmp(name, "byte")    == 0) sharedSetTypeAST(type, TYPE_BYTE);
    else if (strcasecmp(name, "word")    == 0) sharedSetTypeAST(type, TYPE_WORD);
    else {
        sharedFreeAST(type);
        return NULL;
    }
    return type;
}

Value sharedEvaluateCompileTimeValue(AST* node) {
    (void)node;
    return makeNil();
}

void sharedInsertType(const char* name, AST* typeDef) {
    (void)name;
    (void)typeDef;
}

AST* sharedNewASTNode(ASTNodeType type, Token* token) {
    (void)token;
    AST* node = (AST*)calloc(1, sizeof(AST));
    if (!node) return NULL;
    node->type = type;
    return node;
}

void sharedSetTypeAST(AST* node, VarType type) {
    if (node) node->var_type = type;
}

void sharedSetRight(AST* parent, AST* child) {
    if (!parent) return;
    parent->right = child;
    if (child) child->parent = parent;
}

void sharedAddChild(AST* parent, AST* child) {
    if (!parent || !child) return;
    if (parent->child_capacity <= parent->child_count) {
        int newcap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        AST** new_children = (AST**)realloc(parent->children, sizeof(AST*) * newcap);
        if (!new_children) return;
        parent->children = new_children;
        parent->child_capacity = newcap;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void sharedFreeAST(AST* node) {
    (void)node;
}

void sharedDumpAST(AST* node, int indent) {
    (void)node;
    (void)indent;
}
