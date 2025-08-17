#include "clike/ast.h"
#include <stdlib.h>

ASTNodeClike *newASTNodeClike(ASTNodeTypeClike type, ClikeToken token) {
    ASTNodeClike *node = (ASTNodeClike*)calloc(1, sizeof(ASTNodeClike));
    if (!node) return NULL;
    node->type = type;
    node->token = token;
    node->left = node->right = node->third = NULL;
    node->children = NULL;
    node->child_count = 0;
    return node;
}

void addChildClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent || !child) return;
    parent->children = (ASTNodeClike**)realloc(parent->children, sizeof(ASTNodeClike*) * (parent->child_count + 1));
    parent->children[parent->child_count++] = child;
}

void freeASTClike(ASTNodeClike *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; ++i) freeASTClike(node->children[i]);
    if (node->left) freeASTClike(node->left);
    if (node->right) freeASTClike(node->right);
    if (node->third) freeASTClike(node->third);
    free(node->children);
    free(node);
}
