#ifndef CLIKE_AST_H
#define CLIKE_AST_H

#include "clike/lexer.h"
#include "core/types.h"
#include <stdio.h>
#include <stdbool.h>

typedef enum {
    TCAST_PROGRAM,
    TCAST_VAR_DECL,
    TCAST_FUN_DECL,
    TCAST_PARAM,
    TCAST_COMPOUND,
    TCAST_IF,
    TCAST_WHILE,
    TCAST_FOR,
    TCAST_DO_WHILE,
    TCAST_SWITCH,
    TCAST_CASE,
    TCAST_BREAK,
    TCAST_CONTINUE,
    TCAST_RETURN,
    TCAST_EXPR_STMT,
    TCAST_ASSIGN,
    TCAST_BINOP,
    TCAST_UNOP,
    TCAST_TERNARY,
    TCAST_NUMBER,
    TCAST_STRING,
    TCAST_IDENTIFIER,
    TCAST_ARRAY_ACCESS,
    TCAST_MEMBER,
    TCAST_ADDR,
    TCAST_DEREF,
    TCAST_SIZEOF,
    TCAST_CALL,
    TCAST_STRUCT_DECL,
    TCAST_THREAD_SPAWN,
    TCAST_THREAD_JOIN
} ASTNodeTypeClike;

typedef struct ASTNodeClike {
    ASTNodeTypeClike type;
    ClikeToken token; // For identifier or operator token
    VarType var_type; // Inferred or declared type
    int is_array;           // Non-zero if this declaration is an array
    int array_size;         // Size of array for single-dimensional arrays
    int *array_dims;        // Sizes for each dimension in multi-dimensional arrays
    struct ASTNodeClike **array_dim_exprs; // Expressions for dimension sizes (optional)
    int dim_count;          // Number of dimensions if this node represents an array
    VarType element_type;   // Element type if this node represents an array
    int is_const;           // Non-zero if this declaration is const-qualified
    int is_forward_decl;    // Non-zero if this function node is a prototype without a body
    struct ASTNodeClike *left;
    struct ASTNodeClike *right;
    struct ASTNodeClike *third; // else branch or additional pointer
    struct ASTNodeClike **children;
    int child_count;
    struct ASTNodeClike *parent;
} ASTNodeClike;

ASTNodeClike *newASTNodeClike(ASTNodeTypeClike type, ClikeToken token);
ASTNodeClike *cloneASTClike(ASTNodeClike *node);
void addChildClike(ASTNodeClike *parent, ASTNodeClike *child);
void setLeftClike(ASTNodeClike *parent, ASTNodeClike *child);
void setRightClike(ASTNodeClike *parent, ASTNodeClike *child);
void setThirdClike(ASTNodeClike *parent, ASTNodeClike *child);
bool verifyASTClikeLinks(ASTNodeClike *node, ASTNodeClike *expectedParent);
void freeASTClike(ASTNodeClike *node);
void dumpASTClikeJSON(ASTNodeClike *node, FILE *out);

// Threading helpers
ASTNodeClike *newThreadSpawnClike(ASTNodeClike *call);
ASTNodeClike *newThreadJoinClike(ASTNodeClike *expr);

#endif
