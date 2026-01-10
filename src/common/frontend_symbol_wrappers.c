#include "Pascal/globals.h"
#include "Pascal/parser.h"
#include "compiler/compiler.h"
#include "ast/ast.h"
#include "core/types.h"

AST* rea_lookupType(const char* name) {
    return lookupType(name);
}

void rea_insertType(const char* name, AST* typeDef) {
    insertType(name, typeDef);
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
