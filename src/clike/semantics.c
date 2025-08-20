#include "clike/semantics.h"
#include "core/utils.h"
#include "clike/errors.h"
#include "clike/builtins.h"
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

typedef struct {
    VarTable scopes[256];
    int depth;
} ScopeStack;

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
    t->count = 0;
}

static void ss_push(ScopeStack *s) {
    s->scopes[s->depth].count = 0;
    s->depth++;
}

static void ss_pop(ScopeStack *s) {
    if (s->depth <= 0) return;
    vt_free(&s->scopes[s->depth - 1]);
    s->depth--;
}

static void ss_add(ScopeStack *s, const char *name, VarType type) {
    vt_add(&s->scopes[s->depth - 1], name, type);
}

static VarType ss_get(ScopeStack *s, const char *name) {
    for (int i = s->depth - 1; i >= 0; --i) {
        VarType t = vt_get(&s->scopes[i], name);
        if (t != TYPE_UNKNOWN) return t;
    }
    return TYPE_UNKNOWN;
}

typedef struct {
    char *name;
    VarType type;
} FuncEntry;

static FuncEntry functions[256];
static int functionCount = 0;

static void registerBuiltinFunctions(void) {
    functions[functionCount].name = strdup("printf");
    functions[functionCount].type = TYPE_INTEGER;
    functionCount++;
    functions[functionCount].name = strdup("scanf");
    functions[functionCount].type = TYPE_INTEGER;
    functionCount++;
}

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

static VarType analyzeExpr(ASTNodeClike *node, ScopeStack *scopes);

static VarType analyzeExpr(ASTNodeClike *node, ScopeStack *scopes) {
    if (!node) return TYPE_UNKNOWN;
    switch (node->type) {
        case TCAST_NUMBER:
        case TCAST_STRING:
            return node->var_type;
        case TCAST_IDENTIFIER: {
            char *name = tokenToCString(node->token);
            if (strcmp(name, "NULL") == 0) {
                free(name);
                node->var_type = TYPE_POINTER;
                return TYPE_POINTER;
            }
            VarType t = ss_get(scopes, name);
            node->var_type = t;
            if (t == TYPE_UNKNOWN) {
                fprintf(stderr,
                        "Type error: undefined variable '%s' at line %d, column %d\n",
                        name,
                        node->token.line,
                        node->token.column);
                clike_error_count++;
            }
            free(name);
            return t;
        }
        case TCAST_BINOP: {
            VarType lt = analyzeExpr(node->left, scopes);
            VarType rt = analyzeExpr(node->right, scopes);
            if (lt == TYPE_REAL || rt == TYPE_REAL) node->var_type = TYPE_REAL;
            else if (lt == TYPE_STRING || rt == TYPE_STRING) node->var_type = TYPE_STRING;
            else node->var_type = lt != TYPE_UNKNOWN ? lt : rt;
            return node->var_type;
        }
        case TCAST_UNOP:
            node->var_type = analyzeExpr(node->left, scopes);
            return node->var_type;
        case TCAST_ADDR:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_POINTER;
            return TYPE_POINTER;
        case TCAST_DEREF:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_UNKNOWN;
            return TYPE_UNKNOWN;
        case TCAST_ASSIGN: {
            VarType lt = analyzeExpr(node->left, scopes);
            VarType rt = analyzeExpr(node->right, scopes);
            if (lt != TYPE_UNKNOWN && rt != TYPE_UNKNOWN) {
                if (!(lt == TYPE_REAL && rt == TYPE_INTEGER) && lt != rt) {
                    fprintf(stderr,
                            "Type error: cannot assign %s to %s at line %d, column %d\n",
                            varTypeToString(rt), varTypeToString(lt),
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
            }
            node->var_type = lt;
            return lt;
        }
        case TCAST_CALL: {
            char *name = tokenToCString(node->token);
            VarType t = getFunctionType(name);
            if (t == TYPE_UNKNOWN) {
                if (clike_get_builtin_id(name) != -1) {
                    t = TYPE_INTEGER;
                } else {
                    fprintf(stderr,
                            "Type error: call to undefined function '%s' at line %d, column %d\n",
                            name,
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                }
            }
            free(name);
            for (int i = 0; i < node->child_count; ++i) {
                analyzeExpr(node->children[i], scopes);
            }
            node->var_type = t;
            return t;
        }
        case TCAST_ARRAY_ACCESS: {
            analyzeExpr(node->left, scopes);
            for (int i = 0; i < node->child_count; ++i) {
                analyzeExpr(node->children[i], scopes);
            }
            node->var_type = TYPE_UNKNOWN;
            return TYPE_UNKNOWN;
        }
        case TCAST_MEMBER:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_UNKNOWN;
            return TYPE_UNKNOWN;
        default:
            return TYPE_UNKNOWN;
    }
}

static void analyzeStmt(ASTNodeClike *node, ScopeStack *scopes, VarType retType) {
    if (!node) return;
    switch (node->type) {
        case TCAST_VAR_DECL: {
            char *name = tokenToCString(node->token);
            ss_add(scopes, name, node->var_type);
            free(name);
            if (node->left) analyzeExpr(node->left, scopes);
            break;
        }
        case TCAST_STRUCT_DECL:
            break;
        case TCAST_COMPOUND:
            ss_push(scopes);
            for (int i = 0; i < node->child_count; ++i) {
                analyzeStmt(node->children[i], scopes, retType);
            }
            ss_pop(scopes);
            break;
        case TCAST_IF:
            analyzeExpr(node->left, scopes);
            analyzeStmt(node->right, scopes, retType);
            analyzeStmt(node->third, scopes, retType);
            break;
        case TCAST_WHILE:
            analyzeExpr(node->left, scopes);
            analyzeStmt(node->right, scopes, retType);
            break;
        case TCAST_FOR:
            if (node->left) analyzeExpr(node->left, scopes);
            if (node->right) analyzeExpr(node->right, scopes);
            if (node->third) analyzeExpr(node->third, scopes);
            if (node->child_count > 0) analyzeStmt(node->children[0], scopes, retType);
            break;
        case TCAST_DO_WHILE:
            analyzeStmt(node->right, scopes, retType);
            analyzeExpr(node->left, scopes);
            break;
        case TCAST_SWITCH:
            analyzeExpr(node->left, scopes);
            for (int i = 0; i < node->child_count; ++i) {
                ASTNodeClike *c = node->children[i];
                analyzeExpr(c->left, scopes);
                for (int j = 0; j < c->child_count; ++j) {
                    analyzeStmt(c->children[j], scopes, retType);
                }
            }
            if (node->right) analyzeStmt(node->right, scopes, retType);
            break;
        case TCAST_BREAK:
        case TCAST_CONTINUE:
            break;
        case TCAST_RETURN: {
            VarType t = TYPE_VOID;
            if (node->left) t = analyzeExpr(node->left, scopes);
            if (retType == TYPE_VOID) {
                if (t != TYPE_VOID && t != TYPE_UNKNOWN) {
                    fprintf(stderr,
                            "Type error: returning value from void function at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
            } else if (t != TYPE_UNKNOWN && t != retType) {
                fprintf(stderr,
                        "Type error: return type %s does not match %s at line %d, column %d\n",
                        varTypeToString(t), varTypeToString(retType),
                        node->token.line, node->token.column);
                clike_error_count++;
            }
            break;
        }
        case TCAST_EXPR_STMT:
            if (node->left) analyzeExpr(node->left, scopes);
            break;
        default:
            if (node->type == TCAST_ASSIGN) {
                analyzeExpr(node, scopes);
            }
            break;
    }
}

static void analyzeFunction(ASTNodeClike *func) {
    ScopeStack scopes = {0};
    ss_push(&scopes); // function scope for parameters
    if (func->left) {
        for (int i = 0; i < func->left->child_count; ++i) {
            ASTNodeClike *p = func->left->children[i];
            char *name = tokenToCString(p->token);
            ss_add(&scopes, name, p->var_type);
            free(name);
        }
    }
    analyzeStmt(func->right, &scopes, func->var_type);
    while (scopes.depth > 0) ss_pop(&scopes);
}

void analyzeSemanticsClike(ASTNodeClike *program) {
    if (!program) return;
    functionCount = 0;
    registerBuiltinFunctions();
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
