#include "clike/opt.h"
#include "clike/lexer.h"
#include "core/types.h"
#include <stdlib.h>

static int isConst(ASTNodeClike* n, double* out, int* is_float) {
    if (!n || n->type != TCAST_NUMBER) return 0;
    if (n->var_type == TYPE_REAL) {
        if (out) *out = n->token.float_val;
        if (is_float) *is_float = 1;
    } else {
        if (out) *out = (double)n->token.int_val;
        if (is_float) *is_float = 0;
    }
    return 1;
}

static ASTNodeClike* foldBinary(ASTNodeClike* node) {
    double lv, rv; int lf, rf;
    if (!isConst(node->left, &lv, &lf) || !isConst(node->right, &rv, &rf))
        return node;
    int result_is_float = lf || rf;
    double res = 0;
    switch (node->token.type) {
        case CLIKE_TOKEN_PLUS: res = lv + rv; break;
        case CLIKE_TOKEN_MINUS: res = lv - rv; break;
        case CLIKE_TOKEN_STAR: res = lv * rv; break;
        case CLIKE_TOKEN_SLASH: res = lv / rv; result_is_float = 1; break;
        case CLIKE_TOKEN_EQUAL_EQUAL: res = (lv == rv); result_is_float = 0; break;
        case CLIKE_TOKEN_BANG_EQUAL: res = (lv != rv); result_is_float = 0; break;
        case CLIKE_TOKEN_LESS: res = (lv < rv); result_is_float = 0; break;
        case CLIKE_TOKEN_LESS_EQUAL: res = (lv <= rv); result_is_float = 0; break;
        case CLIKE_TOKEN_GREATER: res = (lv > rv); result_is_float = 0; break;
        case CLIKE_TOKEN_GREATER_EQUAL: res = (lv >= rv); result_is_float = 0; break;
        case CLIKE_TOKEN_AND_AND: res = (lv != 0 && rv != 0); result_is_float = 0; break;
        case CLIKE_TOKEN_OR_OR: res = (lv != 0 || rv != 0); result_is_float = 0; break;
        default: return node;
    }
    ClikeToken t = {0};
    if (result_is_float) {
        t.type = CLIKE_TOKEN_FLOAT_LITERAL;
        t.float_val = res;
    } else {
        t.type = CLIKE_TOKEN_NUMBER;
        t.int_val = (int)res;
        t.float_val = res;
    }
    ASTNodeClike* newNode = newASTNodeClike(TCAST_NUMBER, t);
    newNode->var_type = result_is_float ? TYPE_REAL : TYPE_INTEGER;
    freeASTClike(node);
    return newNode;
}

static ASTNodeClike* foldUnary(ASTNodeClike* node) {
    double v; int vf;
    if (!isConst(node->left, &v, &vf)) return node;
    double res = 0; int is_float = vf;
    switch (node->token.type) {
        case CLIKE_TOKEN_MINUS: res = -v; break;
        case CLIKE_TOKEN_BANG: res = !(v != 0); is_float = 0; break;
        default: return node;
    }
    ClikeToken t = {0};
    if (is_float) {
        t.type = CLIKE_TOKEN_FLOAT_LITERAL; t.float_val = res;
    } else {
        t.type = CLIKE_TOKEN_NUMBER; t.int_val = (int)res; t.float_val = res;
    }
    ASTNodeClike* newNode = newASTNodeClike(TCAST_NUMBER, t);
    newNode->var_type = is_float ? TYPE_REAL : TYPE_INTEGER;
    freeASTClike(node);
    return newNode;
}

static ASTNodeClike* optimizeNode(ASTNodeClike* node) {
    if (!node) return NULL;
    setLeftClike(node, optimizeNode(node->left));
    setRightClike(node, optimizeNode(node->right));
    setThirdClike(node, optimizeNode(node->third));
    int j = 0;
    for (int i = 0; i < node->child_count; ++i) {
        ASTNodeClike *child = optimizeNode(node->children[i]);
        if (child) {
            child->parent = node;
            node->children[j++] = child;
        }
    }
    node->child_count = j;

    switch (node->type) {
        case TCAST_BINOP: return foldBinary(node);
        case TCAST_UNOP: return foldUnary(node);
        case TCAST_IF: {
            double cond; int cf;
            if (isConst(node->left, &cond, &cf)) {
                if (cond != 0) {
                    ASTNodeClike* taken = node->right;
                    freeASTClike(node->left);
                    freeASTClike(node->third);
                    free(node);
                    return taken;
                } else {
                    ASTNodeClike* taken = node->third;
                    freeASTClike(node->left);
                    freeASTClike(node->right);
                    free(node);
                    return taken;
                }
            }
            break;
        }
        default: break;
    }
    return node;
}

ASTNodeClike* optimizeClikeAST(ASTNodeClike* node) {
    return optimizeNode(node);
}

