#ifndef AST_H
#define AST_H
#include "types.h"
#include "symbol.h"

typedef struct AST {
    ASTNodeType type;
    Token *token;            /* For names, field names, etc. */
    List *unit_list;         // List of unit names (for 'uses' clause)
    Symbol *symbol_table;    // Symbol table for the unit (if applicable)
    int var_type;            /* For declarations */
    int by_ref;              /* 1 if parameter passed by reference */
    struct AST *left;
    struct AST *right;
    struct AST *extra;
    struct AST **children;
    struct AST *parent;
    int child_count;
    int child_capacity;
    int i_val;
    bool is_global_scope;
} AST;

AST *newASTNode(ASTNodeType type, Token *token);
void addChild(AST *parent, AST *child);
void freeAST(AST *node);
void setLeft(AST *parent, AST *child);
void setRight(AST *parent, AST *child);
void setExtra(AST *parent, AST *child);
void dumpAST(AST *node, int indent);
void dumpASTFromRoot(AST *node);
void debugAST(AST *node, int indent);
void setTypeAST(AST *node, VarType type);
void annotateTypes(AST *node, AST *currentScopeNode, AST *globalProgramNode);
AST *copyAST(AST *node);
bool verifyASTLinks(AST *node, AST *expectedParent);
#endif // AST_H
