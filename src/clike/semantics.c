#include "clike/semantics.h"
#include "core/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *name;
    VarType type;
} VarEntry;

typedef struct {
    VarEntry entries[256];
    int count;
} VarTable;

static void vt_add(VarTable *t, const char *name, VarType type) {
    t->entries[t->count].name = strdup(name);
    t->entries[t->count].type = type;
    t->count++;
}

static VarType vt_get(VarTable *t, const char *name) {
    for (int i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return t->entries[i].type;
        }
    }
    return TYPE_UNKNOWN;
}

static void vt_free(VarTable *t) {
    for (int i = 0; i < t->count; ++i) free(t->entries[i].name);
}

typedef struct {
    char *name;
    VarType type;
} FuncEntry;

static FuncEntry functions[256];
static int functionCount = 0;

static VarType getFunctionType(const char *name) {
    for (int i = 0; i < functionCount; ++i) {
        if (strcmp(functions[i].name, name) == 0) return functions[i].type;
    }
    return TYPE_UNKNOWN;
}

static char *tokenToCString(ClikeToken t) {
    char *s = (char *)malloc(t.length + 1);
    memcpy(s, t.lexeme, t.length);
    s[t.length] = '\0';
    return s;
}

static VarType analyzeExpr(ASTNodeClike *node, VarTable *vt);

static VarType analyzeExpr(ASTNodeClike *node, VarTable *vt) {
    if (!node) return TYPE_UNKNOWN;
    switch (node->type) {
        case TCAST_NUMBER:
        case TCAST_STRING:
            return node->var_type;
        case TCAST_IDENTIFIER: {
            char *name = tokenToCString(node->token);
            VarType t = vt_get(vt, name);
            node->var_type = t;
            free(name);
            return t;
        }
        case TCAST_BINOP: {
            VarType lt = analyzeExpr(node->left, vt);
            VarType rt = analyzeExpr(node->right, vt);
            if (lt == TYPE_REAL || rt == TYPE_REAL) node->var_type = TYPE_REAL;
            else if (lt == TYPE_STRING || rt == TYPE_STRING) node->var_type = TYPE_STRING;
            else node->var_type = lt != TYPE_UNKNOWN ? lt : rt;
            return node->var_type;
        }
        case TCAST_UNOP:
            node->var_type = analyzeExpr(node->left, vt);
            return node->var_type;
        case TCAST_ASSIGN: {
            VarType lt = analyzeExpr(node->left, vt);
            VarType rt = analyzeExpr(node->right, vt);
            if (lt != TYPE_UNKNOWN && rt != TYPE_UNKNOWN && lt != rt) {
                fprintf(stderr, "Type error: cannot assign %s to %s at line %d\n",
                        varTypeToString(rt), varTypeToString(lt), node->token.line);
            }
            node->var_type = lt;
            return lt;
        }
        case TCAST_CALL: {
            char *name = tokenToCString(node->token);
            VarType t = getFunctionType(name);
            free(name);
            node->var_type = t;
            return t;
        }
        default:
            return TYPE_UNKNOWN;
    }
}

static void analyzeStmt(ASTNodeClike *node, VarTable *vt, VarType retType) {
    if (!node) return;
    switch (node->type) {
        case TCAST_COMPOUND:
            for (int i = 0; i < node->child_count; ++i) {
                analyzeStmt(node->children[i], vt, retType);
            }
            break;
        case TCAST_IF:
            analyzeExpr(node->left, vt);
            analyzeStmt(node->right, vt, retType);
            analyzeStmt(node->third, vt, retType);
            break;
        case TCAST_WHILE:
            analyzeExpr(node->left, vt);
            analyzeStmt(node->right, vt, retType);
            break;
        case TCAST_RETURN: {
            VarType t = TYPE_VOID;
            if (node->left) t = analyzeExpr(node->left, vt);
            if (retType == TYPE_VOID) {
                if (t != TYPE_VOID && t != TYPE_UNKNOWN) {
                    fprintf(stderr, "Type error: returning value from void function at line %d\n", node->token.line);
                }
            } else if (t != TYPE_UNKNOWN && t != retType) {
                fprintf(stderr, "Type error: return type %s does not match %s at line %d\n",
                        varTypeToString(t), varTypeToString(retType), node->token.line);
            }
            break;
        }
        case TCAST_EXPR_STMT:
            if (node->left) analyzeExpr(node->left, vt);
            break;
        default:
            if (node->type == TCAST_ASSIGN) {
                analyzeExpr(node, vt);
            }
            break;
    }
}

static void gatherDecls(ASTNodeClike *node, VarTable *vt) {
    if (!node) return;
    if (node->type == TCAST_VAR_DECL) {
        char *name = tokenToCString(node->token);
        vt_add(vt, name, node->var_type);
        free(name);
        return;
    }
    if (node->left) gatherDecls(node->left, vt);
    if (node->right) gatherDecls(node->right, vt);
    if (node->third) gatherDecls(node->third, vt);
    for (int i = 0; i < node->child_count; ++i) {
        gatherDecls(node->children[i], vt);
    }
}

static void analyzeFunction(ASTNodeClike *func) {
    VarTable vt = {0};
    // parameters
    if (func->left) {
        for (int i = 0; i < func->left->child_count; ++i) {
            ASTNodeClike *p = func->left->children[i];
            char *name = tokenToCString(p->token);
            vt_add(&vt, name, p->var_type);
            free(name);
        }
    }
    gatherDecls(func->right, &vt);
    analyzeStmt(func->right, &vt, func->var_type);
    vt_free(&vt);
}

void analyzeSemanticsClike(ASTNodeClike *program) {
    if (!program) return;
    functionCount = 0;
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_FUN_DECL) {
            char *name = tokenToCString(decl->token);
            functions[functionCount].name = name;
            functions[functionCount].type = decl->var_type;
            functionCount++;
        }
    }
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_FUN_DECL) analyzeFunction(decl);
    }
    for (int i = 0; i < functionCount; ++i) free(functions[i].name);
}
