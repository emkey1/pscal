#include "common/ast_shared.h"

AST* lookupType(const char* name) { return sharedLookupType(name); }
Value evaluateCompileTimeValue(AST* node) { return sharedEvaluateCompileTimeValue(node); }
void insertType(const char* name, AST* typeDef) { sharedInsertType(name, typeDef); }
AST* newASTNode(ASTNodeType type, Token* token) { return sharedNewASTNode(type, token); }
void setTypeAST(AST* node, VarType type) { sharedSetTypeAST(node, type); }
void setRight(AST* parent, AST* child) { sharedSetRight(parent, child); }
void addChild(AST* parent, AST* child) { sharedAddChild(parent, child); }
void freeAST(AST* node) { sharedFreeAST(node); }
void dumpAST(AST* node, int indent) { sharedDumpAST(node, indent); }
AST* copyAST(AST* node) { (void)node; return NULL; }
