#ifndef TINYC_AST_H
#define TINYC_AST_H

#include "tinyc/lexer.h"

typedef enum {
    TCAST_PROGRAM,
    TCAST_VAR_DECL,
    TCAST_FUN_DECL,
    TCAST_PARAM,
    TCAST_COMPOUND,
    TCAST_IF,
    TCAST_WHILE,
    TCAST_RETURN,
    TCAST_EXPR_STMT,
    TCAST_ASSIGN,
    TCAST_BINOP,
    TCAST_UNOP,
    TCAST_NUMBER,
    TCAST_STRING,
    TCAST_IDENTIFIER,
    TCAST_CALL
} ASTNodeTypeTinyC;

typedef struct ASTNodeTinyC {
    ASTNodeTypeTinyC type;
    TinyCToken token; // For identifier or operator token
    struct ASTNodeTinyC *left;
    struct ASTNodeTinyC *right;
    struct ASTNodeTinyC *third; // else branch or additional pointer
    struct ASTNodeTinyC **children;
    int child_count;
} ASTNodeTinyC;

ASTNodeTinyC *newASTNodeTinyC(ASTNodeTypeTinyC type, TinyCToken token);
void addChildTinyC(ASTNodeTinyC *parent, ASTNodeTinyC *child);
void freeASTTinyC(ASTNodeTinyC *node);

#endif
