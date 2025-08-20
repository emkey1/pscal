#include "frontend/ast.h"
#include "symbol/symbol.h"
#include "core/types.h"
#include "core/utils.h"
#include "clike/parser.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

AST* lookupType(const char* name) {
    AST* base = clike_lookup_struct(name);
    if (!base) {
        base = newASTNode(AST_VARIABLE, NULL);
        if (!base) return NULL;

        if      (strcasecmp(name, "integer") == 0) setTypeAST(base, TYPE_INTEGER);
        else if (strcasecmp(name, "real")    == 0) setTypeAST(base, TYPE_REAL);
        else if (strcasecmp(name, "char")    == 0) setTypeAST(base, TYPE_CHAR);
        else if (strcasecmp(name, "string")  == 0) setTypeAST(base, TYPE_STRING);
        else if (strcasecmp(name, "boolean") == 0) setTypeAST(base, TYPE_BOOLEAN);
        else if (strcasecmp(name, "byte")    == 0) setTypeAST(base, TYPE_BYTE);
        else if (strcasecmp(name, "word")    == 0) setTypeAST(base, TYPE_WORD);
        else { freeAST(base); return NULL; }
    }

    AST* ptr = newASTNode(AST_POINTER_TYPE, NULL);
    if (!ptr) { freeAST(base); return NULL; }
    setRight(ptr, base);
    setTypeAST(ptr, TYPE_POINTER);
    return ptr;
}

Value evaluateCompileTimeValue(AST* node) { (void)node; return makeNil(); }
void insertType(const char* name, AST* typeDef) { (void)name; (void)typeDef; }

AST* newASTNode(ASTNodeType type, Token* token) {
    AST* node = (AST*)calloc(1, sizeof(AST));
    node->type = type;
    if (token) {
        node->token = (Token*)malloc(sizeof(Token));
        node->token->type = token->type;
        node->token->line = token->line;
        node->token->column = token->column;
        node->token->value = token->value ? strdup(token->value) : NULL;
    }
    return node;
}

void setTypeAST(AST* node, VarType type) { if (node) node->var_type = type; }

void setRight(AST* parent, AST* child) {
    if (parent) {
        parent->right = child;
        if (child) child->parent = parent;
    }
}

void addChild(AST* parent, AST* child) {
    if (!parent || !child) return;
    if (parent->child_capacity <= parent->child_count) {
        int newcap = parent->child_capacity ? parent->child_capacity * 2 : 4;
        parent->children = (AST**)realloc(parent->children, sizeof(AST*) * newcap);
        parent->child_capacity = newcap;
    }
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void freeAST(AST* node) { (void)node; }
void dumpAST(AST* node, int indent) { (void)node; (void)indent; }
AST* copyAST(AST* node) { (void)node; return NULL; }
