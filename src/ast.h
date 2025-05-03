#ifndef AST_H
#define AST_H
#include "types.h"
#include "symbol.h"

typedef struct AST {
    ASTNodeType type;
    Token *token;            /* For names, field names, type names, nil, ^, etc. */
    List *unit_list;         // List of unit names (for 'uses' clause)
    Symbol *symbol_table;    // Symbol table for the unit (if applicable)
    VarType var_type;        // <<< RENAMED from int to VarType for clarity >>>
    int by_ref;              /* 1 if parameter passed by reference */
    struct AST *left;        // Left child or operand
    struct AST *right;       // Right child, type node, return type, base type (for POINTER_TYPE)
    struct AST *extra;       // Else branch (IF), body (FOR), implementation decls (UNIT), function block
    struct AST **children;   // Children nodes (compound statements, params, args, array indices, record fields)
    struct AST *parent;      // Pointer to parent node
    int child_count;
    int child_capacity;
    int i_val;               // Used for enum ordinal value storage in AST_ENUM_VALUE
    bool is_global_scope;    // Flag for block nodes
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
void freeTypeTableASTNodes(void);
AST* findDeclarationInScope(const char* varName, AST* currentScopeNode);
AST* findStaticDeclarationInAST(const char* varName, AST* currentScopeNode, AST* globalProgramNode);
VarType getBuiltinReturnType(const char* name);
#endif // AST_H
