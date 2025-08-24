#include "clike/ast.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

ASTNodeClike *newASTNodeClike(ASTNodeTypeClike type, ClikeToken token) {
    ASTNodeClike *node = (ASTNodeClike*)calloc(1, sizeof(ASTNodeClike));
    if (!node) return NULL;
    node->type = type;
    node->token = token;
    node->var_type = TYPE_UNKNOWN;
    node->is_array = 0;
    node->array_size = 0;
    node->array_dims = NULL;
    node->dim_count = 0;
    node->element_type = TYPE_UNKNOWN;
    node->left = node->right = node->third = NULL;
    node->children = NULL;
    node->child_count = 0;
    node->parent = NULL;
    return node;
}

void addChildClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent || !child) return;
    ASTNodeClike **new_children = (ASTNodeClike**)realloc(
        parent->children, sizeof(ASTNodeClike*) * (parent->child_count + 1));
    if (!new_children) return; // allocation failed, leave structure unchanged
    parent->children = new_children;
    parent->children[parent->child_count++] = child;
    child->parent = parent;
}

void setLeftClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent) return;
    parent->left = child;
    if (child) child->parent = parent;
}

void setRightClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent) return;
    parent->right = child;
    if (child) child->parent = parent;
}

void setThirdClike(ASTNodeClike *parent, ASTNodeClike *child) {
    if (!parent) return;
    parent->third = child;
    if (child) child->parent = parent;
}

ASTNodeClike *cloneASTClike(ASTNodeClike *node) {
    if (!node) return NULL;
    ASTNodeClike *copy = newASTNodeClike(node->type, node->token);
    if (!copy) return NULL;
    copy->var_type = node->var_type;
    copy->is_array = node->is_array;
    copy->array_size = node->array_size;
    copy->dim_count = node->dim_count;
    copy->element_type = node->element_type;
    if (node->dim_count > 0 && node->array_dims) {
        copy->array_dims = (int*)malloc(sizeof(int) * node->dim_count);
        memcpy(copy->array_dims, node->array_dims, sizeof(int) * node->dim_count);
    }
    setLeftClike(copy, cloneASTClike(node->left));
    setRightClike(copy, cloneASTClike(node->right));
    setThirdClike(copy, cloneASTClike(node->third));
    for (int i = 0; i < node->child_count; ++i) {
        addChildClike(copy, cloneASTClike(node->children[i]));
    }
    return copy;
}

void freeASTClike(ASTNodeClike *node) {
    if (!node) return;
    for (int i = 0; i < node->child_count; ++i) freeASTClike(node->children[i]);
    if (node->left) freeASTClike(node->left);
    if (node->right) freeASTClike(node->right);
    if (node->third) freeASTClike(node->third);
    free(node->children);
    free(node->array_dims);
    free(node);
}

static void indent(FILE *out, int level) {
    for (int i = 0; i < level; ++i) {
        fputc(' ', out);
    }
}

static void escapeString(FILE *out, const char *str, int len) {
    for (int i = 0; i < len; ++i) {
        char c = str[i];
        switch (c) {
            case '\\': fputs("\\\\", out); break;
            case '"': fputs("\\\"", out); break;
            case '\n': fputs("\\n", out); break;
            case '\r': fputs("\\r", out); break;
            case '\t': fputs("\\t", out); break;
            default: fputc(c, out); break;
        }
    }
}

static const char* nodeTypeToString(ASTNodeTypeClike type) {
    switch (type) {
        case TCAST_PROGRAM: return "PROGRAM";
        case TCAST_VAR_DECL: return "VAR_DECL";
        case TCAST_FUN_DECL: return "FUN_DECL";
        case TCAST_PARAM: return "PARAM";
        case TCAST_COMPOUND: return "COMPOUND";
        case TCAST_IF: return "IF";
        case TCAST_WHILE: return "WHILE";
        case TCAST_FOR: return "FOR";
        case TCAST_DO_WHILE: return "DO_WHILE";
        case TCAST_SWITCH: return "SWITCH";
        case TCAST_CASE: return "CASE";
        case TCAST_BREAK: return "BREAK";
        case TCAST_CONTINUE: return "CONTINUE";
        case TCAST_RETURN: return "RETURN";
        case TCAST_EXPR_STMT: return "EXPR_STMT";
        case TCAST_ASSIGN: return "ASSIGN";
        case TCAST_BINOP: return "BINOP";
        case TCAST_UNOP: return "UNOP";
        case TCAST_TERNARY: return "TERNARY";
        case TCAST_NUMBER: return "NUMBER";
        case TCAST_STRING: return "STRING";
        case TCAST_IDENTIFIER: return "IDENTIFIER";
        case TCAST_ARRAY_ACCESS: return "ARRAY_ACCESS";
        case TCAST_CALL: return "CALL";
        default: return "UNKNOWN";
    }
}

static void dumpASTClikeJSONRecursive(ASTNodeClike *node, FILE *out, int level) {
    if (!node) {
        indent(out, level);
        fputs("null", out);
        return;
    }

    indent(out, level);
    fputs("{\n", out);

    indent(out, level + 2);
    fprintf(out, "\"type\": \"%s\"", nodeTypeToString(node->type));

    if (node->token.type != CLIKE_TOKEN_UNKNOWN) {
        fputs(",\n", out);
        indent(out, level + 2);
        fprintf(out, "\"tokenType\": \"%s\"", clikeTokenTypeToString(node->token.type));
    }
    if (node->token.lexeme && node->token.length > 0) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"token\": \"", out);
        escapeString(out, node->token.lexeme, node->token.length);
        fputc('"', out);
    }

    if (node->left) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"left\": \n", out);
        dumpASTClikeJSONRecursive(node->left, out, level + 4);
    }
    if (node->right) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"right\": \n", out);
        dumpASTClikeJSONRecursive(node->right, out, level + 4);
    }
    if (node->third) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"third\": \n", out);
        dumpASTClikeJSONRecursive(node->third, out, level + 4);
    }
    if (node->child_count > 0) {
        fputs(",\n", out);
        indent(out, level + 2);
        fputs("\"children\": [\n", out);
        for (int i = 0; i < node->child_count; ++i) {
            dumpASTClikeJSONRecursive(node->children[i], out, level + 4);
            if (i < node->child_count - 1) {
                fputs(",\n", out);
            } else {
                fputc('\n', out);
            }
        }
        indent(out, level + 2);
        fputs("]", out);
    }

    fputc('\n', out);
    indent(out, level);
    fputc('}', out);
}

void dumpASTClikeJSON(ASTNodeClike *node, FILE *out) {
    dumpASTClikeJSONRecursive(node, out, 0);
}

bool verifyASTClikeLinks(ASTNodeClike *node, ASTNodeClike *expectedParent) {
    if (!node) return true;
    bool links_ok = true;
    if (node->parent != expectedParent) {
        fprintf(stderr,
                "[VERIFY] Node %p has parent %p but expected %p\n",
                (void*)node, (void*)node->parent, (void*)expectedParent);
        links_ok = false;
    }
    if (!verifyASTClikeLinks(node->left, node)) links_ok = false;
    if (!verifyASTClikeLinks(node->right, node)) links_ok = false;
    if (!verifyASTClikeLinks(node->third, node)) links_ok = false;
    for (int i = 0; i < node->child_count; ++i) {
        if (!verifyASTClikeLinks(node->children[i], node)) links_ok = false;
    }
    return links_ok;
}
