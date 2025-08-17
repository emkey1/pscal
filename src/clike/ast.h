#ifndef CLIKE_AST_H
#define CLIKE_AST_H

#include "clike/lexer.h"
#include "core/types.h"
#include <stdio.h>

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
    TCAST_BREAK,
    TCAST_CONTINUE,
    TCAST_RETURN,
    TCAST_EXPR_STMT,
    TCAST_ASSIGN,
    TCAST_BINOP,
    TCAST_UNOP,
    TCAST_NUMBER,
    TCAST_STRING,
    TCAST_IDENTIFIER,
    TCAST_CALL
} ASTNodeTypeClike;

typedef struct ASTNodeClike {
    ASTNodeTypeClike type;
    ClikeToken token; // For identifier or operator token
    VarType var_type; // Inferred or declared type
    struct ASTNodeClike *left;
    struct ASTNodeClike *right;
    struct ASTNodeClike *third; // else branch or additional pointer
    struct ASTNodeClike **children;
    int child_count;
} ASTNodeClike;

ASTNodeClike *newASTNodeClike(ASTNodeTypeClike type, ClikeToken token);
void addChildClike(ASTNodeClike *parent, ASTNodeClike *child);
void freeASTClike(ASTNodeClike *node);
void dumpASTClikeJSON(ASTNodeClike *node, FILE *out);

#endif
