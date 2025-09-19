#include "Pascal/opt.h"
#include "core/types.h"
#include "core/utils.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int isConst(AST* n, double* out, int* is_float) {
    if (!n) return 0;
    if (n->type == AST_NUMBER && n->token) {
        if (n->var_type == TYPE_REAL || n->token->type == TOKEN_REAL_CONST) {
            if (out) *out = strtod(n->token->value, NULL);
            if (is_float) *is_float = 1;
        } else {
            long long v;
            if (n->token->type == TOKEN_HEX_CONST)
                v = strtoll(n->token->value, NULL, 16);
            else
                v = strtoll(n->token->value, NULL, 10);
            if (out) *out = (double)v;
            if (is_float) *is_float = 0;
        }
        return 1;
    } else if (n->type == AST_BOOLEAN) {
        if (out) *out = (double)(n->i_val != 0);
        if (is_float) *is_float = 0;
        return 1;
    }
    return 0;
}

static AST* foldBinary(AST* node) {
    double lv, rv; int lf, rf;
    if (!isConst(node->left, &lv, &lf) || !isConst(node->right, &rv, &rf))
        return node;
    if ((lf && !rf) || (!lf && rf))
        return node; // Avoid folding mixed int/real expressions
    double res = 0;
    int result_is_float = lf || rf;
    int result_is_bool = 0;
    switch (node->token->type) {
        case TOKEN_PLUS: res = lv + rv; break;
        case TOKEN_MINUS: res = lv - rv; break;
        case TOKEN_MUL: res = lv * rv; break;
        case TOKEN_SLASH: res = lv / rv; result_is_float = 1; break;
        case TOKEN_INT_DIV:
            if (lf || rf) return node;
            res = (double)((long long)lv / (long long)rv);
            result_is_float = 0;
            break;
        case TOKEN_MOD:
            if (lf || rf) return node;
            res = (double)((long long)lv % (long long)rv);
            result_is_float = 0;
            break;
        case TOKEN_EQUAL: res = (lv == rv); result_is_bool = 1; break;
        case TOKEN_NOT_EQUAL: res = (lv != rv); result_is_bool = 1; break;
        case TOKEN_LESS: res = (lv < rv); result_is_bool = 1; break;
        case TOKEN_LESS_EQUAL: res = (lv <= rv); result_is_bool = 1; break;
        case TOKEN_GREATER: res = (lv > rv); result_is_bool = 1; break;
        case TOKEN_GREATER_EQUAL: res = (lv >= rv); result_is_bool = 1; break;
        case TOKEN_AND: res = ((lv != 0) && (rv != 0)); result_is_bool = 1; break;
        case TOKEN_OR: res = ((lv != 0) || (rv != 0)); result_is_bool = 1; break;
        case TOKEN_XOR: {
            if (lf || rf) return node; // No folding for real operands
            int left_is_bool = node->left &&
                (node->left->type == AST_BOOLEAN || node->left->var_type == TYPE_BOOLEAN);
            int right_is_bool = node->right &&
                (node->right->type == AST_BOOLEAN || node->right->var_type == TYPE_BOOLEAN);
            if (left_is_bool && right_is_bool) {
                res = ((lv != 0) != (rv != 0));
                result_is_bool = 1;
            } else if (!left_is_bool && !right_is_bool) {
                long long li = (long long)lv;
                long long ri = (long long)rv;
                res = (double)(li ^ ri);
                result_is_float = 0;
            } else {
                return node; // Mixed boolean/integer types; leave for runtime
            }
            break;
        }
        default: return node;
    }

    int line = node->token ? node->token->line : 0;
    int col = node->token ? node->token->column : 0;
    Token *t;
    AST *newNode;
    if (result_is_bool) {
        t = newToken(res ? TOKEN_TRUE : TOKEN_FALSE, res ? "true" : "false", line, col);
        newNode = newASTNode(AST_BOOLEAN, t);
        freeToken(t);
        setTypeAST(newNode, TYPE_BOOLEAN);
        newNode->i_val = (int)res;
    } else if (result_is_float) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", res);
        t = newToken(TOKEN_REAL_CONST, buf, line, col);
        newNode = newASTNode(AST_NUMBER, t);
        freeToken(t);
        setTypeAST(newNode, TYPE_REAL);
    } else {
        char buf[64];
        long long iv = (long long)res;
        snprintf(buf, sizeof(buf), "%lld", iv);
        t = newToken(TOKEN_INTEGER_CONST, buf, line, col);
        newNode = newASTNode(AST_NUMBER, t);
        freeToken(t);
        setTypeAST(newNode, TYPE_INTEGER);
        newNode->i_val = iv;
    }
    freeAST(node);
    return newNode;
}

static AST* foldUnary(AST* node) {
    double v; int vf;
    if (!isConst(node->left, &v, &vf)) return node;
    double res = v;
    int is_float = vf;
    int is_bool = 0;
    switch (node->token->type) {
        case TOKEN_MINUS: res = -v; break;
        case TOKEN_PLUS: res = v; break;
        case TOKEN_NOT: res = !(v != 0); is_float = 0; is_bool = 1; break;
        default: return node;
    }

    int line = node->token ? node->token->line : 0;
    int col = node->token ? node->token->column : 0;
    Token *t;
    AST *newNode;
    if (is_bool) {
        t = newToken(res ? TOKEN_TRUE : TOKEN_FALSE, res ? "true" : "false", line, col);
        newNode = newASTNode(AST_BOOLEAN, t);
        freeToken(t);
        setTypeAST(newNode, TYPE_BOOLEAN);
        newNode->i_val = (int)res;
    } else if (is_float) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%g", res);
        t = newToken(TOKEN_REAL_CONST, buf, line, col);
        newNode = newASTNode(AST_NUMBER, t);
        freeToken(t);
        setTypeAST(newNode, TYPE_REAL);
    } else {
        char buf[64];
        long long iv = (long long)res;
        snprintf(buf, sizeof(buf), "%lld", iv);
        t = newToken(TOKEN_INTEGER_CONST, buf, line, col);
        newNode = newASTNode(AST_NUMBER, t);
        freeToken(t);
        setTypeAST(newNode, TYPE_INTEGER);
        newNode->i_val = iv;
    }
    freeAST(node);
    return newNode;
}

static AST* optimizeNode(AST* node) {
    if (!node) return NULL;
    setLeft(node, optimizeNode(node->left));
    setRight(node, optimizeNode(node->right));
    setExtra(node, optimizeNode(node->extra));
    int j = 0;
    for (int i = 0; i < node->child_count; ++i) {
        AST* child = optimizeNode(node->children[i]);
        if (child) {
            child->parent = node;
            node->children[j++] = child;
        }
    }
    node->child_count = j;

    switch (node->type) {
        case AST_BINARY_OP: return foldBinary(node);
        case AST_UNARY_OP: return foldUnary(node);
        case AST_IF: {
            double cond; int cf;
            if (isConst(node->left, &cond, &cf)) {
                AST* taken = (cond != 0) ? node->right : node->extra;
                AST* discard = (cond != 0) ? node->extra : node->right;
                freeAST(node->left);
                if (discard) freeAST(discard);
                if (node->token) freeToken(node->token);
                free(node);
                return taken;
            }
            break;
        }
        case AST_THREAD_SPAWN:
        case AST_THREAD_JOIN:
            break;
        default: break;
    }
    return node;
}

AST* optimizePascalAST(AST* node) {
    return optimizeNode(node);
}

