#ifndef REA_AST_H
#define REA_AST_H

#include "rea/lexer.h"
#include <stdio.h>

typedef enum {
    REA_AST_PROGRAM,
    REA_AST_NUMBER,
    REA_AST_BINARY
} ReaASTNodeType;

typedef struct ReaAST {
    ReaASTNodeType type;
    ReaToken token;            // Used by literal and operator nodes
    struct ReaAST **children;
    int child_count;
    int child_capacity;
} ReaAST;

ReaAST *reaNewASTNode(ReaASTNodeType type);
void reaAddChild(ReaAST *parent, ReaAST *child);
void reaFreeAST(ReaAST *node);
void reaDumpASTJSON(ReaAST *node, FILE *out);

const char *reaTokenTypeToString(ReaTokenType type);
const char *reaASTNodeTypeToString(ReaASTNodeType type);

#endif // REA_AST_H
