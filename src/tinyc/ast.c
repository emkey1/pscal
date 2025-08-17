#include "tinyc/ast.h"
#include <stdlib.h>

ASTNodeTinyC *newASTNodeTinyC(ASTNodeTypeTinyC type, TinyCToken token) {
    ASTNodeTinyC *node = (ASTNodeTinyC*)calloc(1, sizeof(ASTNodeTinyC));
    if (!node) return NULL;
    node->type = type;
    node->token = token;
    node->left = node->right = node->third = NULL;
    node->children = NULL;
    node->child_count = 0;
    return node;
}

void addChildTinyC(ASTNodeTinyC *parent, ASTNodeTinyC *child) {
    if (!parent || !child) return;
    parent->children = (ASTNodeTinyC**)realloc(parent->children, sizeof(ASTNodeTinyC*) * (parent->child_count + 1));
    parent->children[parent->child_count++] = child;
}

void freeASTTinyC(ASTNodeTinyC *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; ++i) freeASTTinyC(node->children[i]);
    if (node->left) freeASTTinyC(node->left);
    if (node->right) freeASTTinyC(node->right);
    if (node->third) freeASTTinyC(node->third);
    free(node->children);
    free(node);
}
