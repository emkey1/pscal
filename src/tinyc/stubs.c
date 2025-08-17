#include "frontend/ast.h"
#include "symbol/symbol.h"
#include "core/types.h"
#include "core/utils.h"

AST* lookupType(const char* name) { (void)name; return NULL; }
Value evaluateCompileTimeValue(AST* node) { (void)node; return makeNil(); }
void freeAST(AST* node) { (void)node; }
void dumpAST(AST* node, int indent) { (void)node; (void)indent; }
void insertType(const char* name, AST* typeDef) { (void)name; (void)typeDef; }
AST* newASTNode(ASTNodeType type, Token* token) { (void)type; (void)token; return NULL; }
void setTypeAST(AST* node, VarType type) { (void)node; (void)type; }
void setRight(AST* parent, AST* child) { (void)parent; (void)child; }
AST* copyAST(AST* node) { (void)node; return NULL; }
