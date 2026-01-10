#pragma once

#include "ast/ast.h"
#include "core/types.h"

AST* sharedLookupType(const char* name);
Value sharedEvaluateCompileTimeValue(AST* node);
void sharedInsertType(const char* name, AST* typeDef);
AST* sharedNewASTNode(ASTNodeType type, Token* token);
void sharedSetTypeAST(AST* node, VarType type);
void sharedSetRight(AST* parent, AST* child);
void sharedAddChild(AST* parent, AST* child);
void sharedFreeAST(AST* node);
void sharedDumpAST(AST* node, int indent);
