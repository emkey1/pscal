#include "clike/semantics.h"
#include "core/utils.h"
#include "clike/errors.h"
#include "clike/builtins.h"
#include "clike/parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static VarType builtinReturnType(const char* name) {
    if (!name) return TYPE_VOID;

    if (strcasecmp(name, "chr")  == 0) return TYPE_CHAR;
    if (strcasecmp(name, "ord")  == 0) return TYPE_INT32;

    if (strcasecmp(name, "sqrt") == 0) return TYPE_LONG_DOUBLE;

    if (strcasecmp(name, "cos")  == 0 ||
        strcasecmp(name, "sin")  == 0 ||
        strcasecmp(name, "tan")  == 0 ||
        strcasecmp(name, "ln")   == 0 ||
        strcasecmp(name, "exp")  == 0 ||
        strcasecmp(name, "real") == 0) {
        return TYPE_DOUBLE;
    }

    if (strcasecmp(name, "chudnovsky") == 0) return TYPE_LONG_DOUBLE;

    if (strcasecmp(name, "paramcount") == 0) {
        /* Return a wide integer to match the builtin implementation. */
        return TYPE_INT64;
    }

    if (strcasecmp(name, "round")     == 0 ||
        strcasecmp(name, "trunc")     == 0 ||
        strcasecmp(name, "random")    == 0 ||
        strcasecmp(name, "ioresult")  == 0 ||
        strcasecmp(name, "length")    == 0 ||
        strcasecmp(name, "strlen")    == 0 ||
        strcasecmp(name, "pos")       == 0 ||
        strcasecmp(name, "screencols")== 0 ||
        strcasecmp(name, "screenrows")== 0 ||
        strcasecmp(name, "wherex")    == 0 ||
        strcasecmp(name, "wherey")    == 0 ||
        strcasecmp(name, "getmaxx")   == 0 ||
        strcasecmp(name, "getmaxy")   == 0) {
        return TYPE_INT32;
    }

    if (strcasecmp(name, "inttostr")  == 0 ||
        strcasecmp(name, "realtostr") == 0 ||
        strcasecmp(name, "paramstr")  == 0 ||
        strcasecmp(name, "copy")      == 0) {
        return TYPE_STRING;
    }

    if (strcasecmp(name, "readkey") == 0 ||
        strcasecmp(name, "upcase")  == 0) {
        return TYPE_CHAR;
    }

    if (strcasecmp(name, "mstreamloadfromfile") == 0) {
        return TYPE_BOOLEAN;
    }

    if (strcasecmp(name, "createtexture") == 0 ||
        strcasecmp(name, "createtargettexture") == 0 ||
        strcasecmp(name, "loadimagetotexture") == 0 ||
        strcasecmp(name, "loadsound") == 0 ||
        strcasecmp(name, "getticks") == 0 ||
        strcasecmp(name, "pollkey") == 0) {
        return TYPE_INT32;
    }

    if (strcasecmp(name, "keypressed") == 0 ||
        strcasecmp(name, "issoundplaying") == 0) {
        return TYPE_BOOLEAN;
    }

    return TYPE_VOID;
}

static size_t varTypeSize(VarType t) {
    switch (t) {
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_BYTE:
            return 1;
        case TYPE_INT16:
        case TYPE_UINT16:
            return 2;
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_FLOAT:
            return 4;
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_DOUBLE:
        case TYPE_POINTER:
            return 8;
        case TYPE_LONG_DOUBLE:
            return sizeof(long double);
        case TYPE_CHAR:
            return 1;
        default:
            return 0;
    }
}

typedef struct {
    char *name;
    VarType type;
    ASTNodeClike *decl;
} VarEntry;

typedef struct {
    VarEntry entries[256];
    int count;
} VarTable;

typedef struct {
    VarTable scopes[256];
    int depth;
} ScopeStack;

// Holds global variable declarations so that functions can reference them.
static VarTable globalVars = {0};

static void vt_add(VarTable *t, const char *name, VarType type, ASTNodeClike *decl) {
    t->entries[t->count].name = strdup(name);
    t->entries[t->count].type = type;
    t->entries[t->count].decl = decl;
    t->count++;
}

static VarType vt_get_type(VarTable *t, const char *name) {
    for (int i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return t->entries[i].type;
        }
    }
    return TYPE_UNKNOWN;
}

static ASTNodeClike* vt_get_decl(VarTable *t, const char *name) {
    for (int i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return t->entries[i].decl;
        }
    }
    return NULL;
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

static void ss_add(ScopeStack *s, const char *name, VarType type, ASTNodeClike *decl) {
    vt_add(&s->scopes[s->depth - 1], name, type, decl);
}

static VarType ss_get(ScopeStack *s, const char *name) {
    for (int i = s->depth - 1; i >= 0; --i) {
        VarType t = vt_get_type(&s->scopes[i], name);
        if (t != TYPE_UNKNOWN) return t;
    }
    return TYPE_UNKNOWN;
}

static ASTNodeClike* ss_get_decl(ScopeStack *s, const char *name) {
    for (int i = s->depth - 1; i >= 0; --i) {
        ASTNodeClike *d = vt_get_decl(&s->scopes[i], name);
        if (d) return d;
    }
    return NULL;
}

typedef struct {
    char *name;
    VarType type;
} FuncEntry;

static FuncEntry functions[256];
static int functionCount = 0;

static void registerBuiltinFunctions(void) {
    functions[functionCount].name = strdup("printf");
    functions[functionCount].type = TYPE_INT32;
    functionCount++;
    functions[functionCount].name = strdup("scanf");
    functions[functionCount].type = TYPE_INT32;
    functionCount++;
    functions[functionCount].name = strdup("strlen");
    functions[functionCount].type = TYPE_INT32;
    functionCount++;
    functions[functionCount].name = strdup("itoa");
    functions[functionCount].type = TYPE_VOID;
    functionCount++;
    // `exit` behaves like C's exit, terminating the program with an optional code.
    functions[functionCount].name = strdup("exit");
    functions[functionCount].type = TYPE_VOID;
    functionCount++;
    functions[functionCount].name = strdup("mstreamcreate");
    functions[functionCount].type = TYPE_MEMORYSTREAM;
    functionCount++;
    functions[functionCount].name = strdup("mstreamloadfromfile");
    functions[functionCount].type = TYPE_BOOLEAN;
    functionCount++;
    functions[functionCount].name = strdup("mstreamsavetofile");
    functions[functionCount].type = TYPE_VOID;
    functionCount++;
    functions[functionCount].name = strdup("mstreamfree");
    functions[functionCount].type = TYPE_VOID;
    functionCount++;
    functions[functionCount].name = strdup("mstreambuffer");
    functions[functionCount].type = TYPE_STRING;
    functionCount++;
}

static VarType getFunctionType(const char *name) {
    for (int i = 0; i < functionCount; ++i) {
        if (strcasecmp(functions[i].name, name) == 0) return functions[i].type;
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
            if (strcasecmp(name, "NULL") == 0) {
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
            if (is_real_type(lt) && is_intlike_type(rt)) {
                node->var_type = lt;
            } else if (is_real_type(rt) && is_intlike_type(lt)) {
                node->var_type = rt;
            } else if (is_real_type(lt) && is_real_type(rt)) {
                if (lt == TYPE_LONG_DOUBLE || rt == TYPE_LONG_DOUBLE) node->var_type = TYPE_LONG_DOUBLE;
                else if (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) node->var_type = TYPE_DOUBLE;
                else node->var_type = TYPE_FLOAT;
            } else if (lt == TYPE_STRING || rt == TYPE_STRING) {
                node->var_type = TYPE_STRING;
            } else {
                node->var_type = lt != TYPE_UNKNOWN ? lt : rt;
            }
            return node->var_type;
        }
        case TCAST_UNOP:
            node->var_type = analyzeExpr(node->left, scopes);
            return node->var_type;
        case TCAST_TERNARY: {
            analyzeExpr(node->left, scopes);
            VarType rt = analyzeExpr(node->right, scopes);
            VarType ft = analyzeExpr(node->third, scopes);
            if (is_real_type(rt) && is_intlike_type(ft)) {
                node->var_type = rt;
            } else if (is_real_type(ft) && is_intlike_type(rt)) {
                node->var_type = ft;
            } else if (is_real_type(rt) && is_real_type(ft)) {
                if (rt == TYPE_LONG_DOUBLE || ft == TYPE_LONG_DOUBLE) node->var_type = TYPE_LONG_DOUBLE;
                else if (rt == TYPE_DOUBLE || ft == TYPE_DOUBLE) node->var_type = TYPE_DOUBLE;
                else node->var_type = TYPE_FLOAT;
            } else if (rt == TYPE_STRING || ft == TYPE_STRING) {
                node->var_type = TYPE_STRING;
            } else {
                node->var_type = rt != TYPE_UNKNOWN ? rt : ft;
            }
            return node->var_type;
        }
        case TCAST_ADDR:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_POINTER;
            return TYPE_POINTER;
        case TCAST_DEREF:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_UNKNOWN;
            return TYPE_UNKNOWN;
        case TCAST_SIZEOF: {
            size_t size = 0;
            if (node->left) {
                ASTNodeClike *operand = node->left;
                VarType tokenType = clike_tokenTypeToVarType(operand->token.type);
                if (operand->type == TCAST_IDENTIFIER && tokenType != TYPE_UNKNOWN && operand->token.type != CLIKE_TOKEN_IDENTIFIER) {
                    size = varTypeSize(tokenType);
                } else {
                    VarType t = analyzeExpr(operand, scopes);
                    if (operand->type == TCAST_IDENTIFIER) {
                        char *name = tokenToCString(operand->token);
                        ASTNodeClike *decl = ss_get_decl(scopes, name);
                        free(name);
                        if (decl && decl->is_array) {
                            size = varTypeSize(decl->element_type);
                            for (int i = 0; i < decl->dim_count; ++i) size *= decl->array_dims[i];
                        } else {
                            size = varTypeSize(t);
                        }
                    } else if (operand->is_array) {
                        size = varTypeSize(operand->element_type);
                        for (int i = 0; i < operand->dim_count; ++i) size *= operand->array_dims[i];
                    } else {
                        size = varTypeSize(t);
                    }
                }
            }
            node->token.int_val = (long long)size;
            node->var_type = TYPE_INT64;
            return TYPE_INT64;
        }
        case TCAST_ASSIGN: {
            VarType lt = analyzeExpr(node->left, scopes);
            VarType rt = analyzeExpr(node->right, scopes);
            if (lt != TYPE_UNKNOWN && rt != TYPE_UNKNOWN) {
            if (lt != rt &&
                !(is_real_type(lt) && is_real_type(rt)) &&
                !(is_real_type(lt) && is_intlike_type(rt)) &&
                !(lt == TYPE_STRING && rt == TYPE_CHAR) &&
                !(is_intlike_type(lt) && is_intlike_type(rt))) {
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
            // `exit` behaves like C's exit: allow 0 or 1 integer argument.
            if (strcasecmp(name, "exit") == 0) {
                if (node->child_count > 1) {
                    fprintf(stderr,
                            "Type error: exit expects at most 1 argument at line %d, column %d\n",
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                }
                if (node->child_count == 1) {
                    VarType at = analyzeExpr(node->children[0], scopes);
                    if (!is_intlike_type(at)) {
                        fprintf(stderr,
                                "Type error: exit argument must be an integer at line %d, column %d\n",
                                node->token.line,
                                node->token.column);
                        clike_error_count++;
                    }
                }
                free(name);
                node->var_type = TYPE_VOID;
                return TYPE_VOID;
            }

            if (strcasecmp(name, "mutex") == 0 || strcasecmp(name, "rcmutex") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr,
                            "Type error: %s expects no arguments at line %d, column %d\n",
                            name, node->token.line, node->token.column);
                    clike_error_count++;
                }
                free(name);
                node->var_type = TYPE_INT32;
                return TYPE_INT32;
            }
            if (strcasecmp(name, "lock") == 0 || strcasecmp(name, "unlock") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr,
                            "Type error: %s expects 1 argument at line %d, column %d\n",
                            name, node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    VarType at = analyzeExpr(node->children[0], scopes);
                    if (!is_intlike_type(at)) {
                        fprintf(stderr,
                                "Type error: %s argument must be integer at line %d, column %d\n",
                                name, node->token.line, node->token.column);
                        clike_error_count++;
                    }
                }
                free(name);
                node->var_type = TYPE_VOID;
                return TYPE_VOID;
            }

            VarType t = getFunctionType(name);
            if (t == TYPE_UNKNOWN) {
                int bid = clike_get_builtin_id(name);
                if (bid != -1) {
                    t = builtinReturnType(name);
                } else {
                    fprintf(stderr,
                            "Type error: call to undefined function '%s' at line %d, column %d\n",
                            name,
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                }
            }
            for (int i = 0; i < node->child_count; ++i) {
                analyzeExpr(node->children[i], scopes);
            }
            if (strcasecmp(name, "mstreamcreate") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr,
                            "Type error: mstreamcreate expects no arguments at line %d, column %d\n",
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                }
            } else if (strcasecmp(name, "mstreamloadfromfile") == 0 ||
                       strcasecmp(name, "mstreamsavetofile") == 0) {
                if (node->child_count != 2) {
                    fprintf(stderr,
                            "Type error: %s expects 2 arguments at line %d, column %d\n",
                            name,
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                } else {
                    if (node->children[0]->var_type != TYPE_POINTER) {
                        fprintf(stderr,
                                "Type error: first argument to %s must be a pointer at line %d, column %d\n",
                                name,
                                node->token.line,
                                node->token.column);
                        clike_error_count++;
                    }
                    if (node->children[1]->var_type != TYPE_STRING) {
                        fprintf(stderr,
                                "Type error: second argument to %s must be a string at line %d, column %d\n",
                                name,
                                node->token.line,
                                node->token.column);
                        clike_error_count++;
                    }
                }
            } else if (strcasecmp(name, "mstreamfree") == 0) {
                if (node->child_count != 1 || node->children[0]->var_type != TYPE_POINTER) {
                    fprintf(stderr,
                            "Type error: mstreamfree expects a pointer argument at line %d, column %d\n",
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                }
            } else if (strcasecmp(name, "mstreambuffer") == 0) {
                if (node->child_count != 1 || node->children[0]->var_type != TYPE_MEMORYSTREAM) {
                    fprintf(stderr,
                            "Type error: mstreambuffer expects an mstream argument at line %d, column %d\n",
                            node->token.line,
                            node->token.column);
                    clike_error_count++;
                }
            }
            free(name);
            node->var_type = t;
            return t;
        }
        case TCAST_ARRAY_ACCESS: {
            ASTNodeClike *arrDecl = NULL;
            if (node->left) {
                analyzeExpr(node->left, scopes);
                if (node->left->type == TCAST_IDENTIFIER) {
                    char *name = tokenToCString(node->left->token);
                    arrDecl = ss_get_decl(scopes, name);
                    free(name);
                } else if (node->left->is_array) {
                    arrDecl = node->left;
                }
            }
            for (int i = 0; i < node->child_count; ++i) {
                VarType idxType = analyzeExpr(node->children[i], scopes);
                if (!is_intlike_type(idxType)) {
                    fprintf(stderr,
                            "Type error: array index must be integer at line %d, column %d\n",
                            node->children[i]->token.line,
                            node->children[i]->token.column);
                    clike_error_count++;
                }
            }
            if (arrDecl && arrDecl->is_array) {
                if (arrDecl->dim_count > node->child_count) {
                    node->is_array = 1;
                    node->dim_count = arrDecl->dim_count - node->child_count;
                    node->element_type = arrDecl->element_type;
                    node->var_type = TYPE_ARRAY;
                    if (arrDecl->array_dims) {
                        node->array_dims = (int*)malloc(sizeof(int) * node->dim_count);
                        for (int i = 0; i < node->dim_count; ++i) {
                            node->array_dims[i] = arrDecl->array_dims[i + node->child_count];
                        }
                    }
                } else {
                    node->var_type = arrDecl->element_type;
                }
            } else {
                node->var_type = TYPE_UNKNOWN;
            }
            return node->var_type;
        }
        case TCAST_MEMBER:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_UNKNOWN;
            return TYPE_UNKNOWN;
        case TCAST_THREAD_SPAWN: {
            if (node->left) {
                VarType ct = analyzeExpr(node->left, scopes);
                if (node->left->type != TCAST_CALL) {
                    fprintf(stderr,
                            "Type error: spawn expects function call at line %d, column %d\n",
                            node->left->token.line, node->left->token.column);
                    clike_error_count++;
                } else {
                    if (ct != TYPE_VOID && ct != TYPE_UNKNOWN) {
                        fprintf(stderr,
                                "Type error: spawned function must return void at line %d, column %d\n",
                                node->left->token.line, node->left->token.column);
                        clike_error_count++;
                    }
                    if (node->left->child_count > 0) {
                        fprintf(stderr,
                                "Type warning: arguments to spawned function ignored at line %d, column %d\n",
                                node->left->token.line, node->left->token.column);
                    }
                }
            }
            node->var_type = TYPE_INT32;
            return TYPE_INT32;
        }
        default:
            return TYPE_UNKNOWN;
    }
}

static void analyzeStmt(ASTNodeClike *node, ScopeStack *scopes, VarType retType) {
    if (!node) return;
    switch (node->type) {
        case TCAST_VAR_DECL: {
            char *name = tokenToCString(node->token);
            ss_add(scopes, name, node->var_type, node);
            free(name);
            if (node->left) {
                VarType initType = analyzeExpr(node->left, scopes);
                VarType declType = node->var_type;
                if (declType == TYPE_ARRAY && node->element_type == TYPE_CHAR &&
                    node->left->type == TCAST_STRING) {
                    initType = declType;
                }
                if (declType != TYPE_UNKNOWN && initType != TYPE_UNKNOWN) {
                    if (declType != initType &&
                        !(is_real_type(declType) && is_real_type(initType)) &&
                        !(declType == TYPE_STRING && initType == TYPE_CHAR) &&
                        !(is_intlike_type(declType) && is_intlike_type(initType))) {
                        fprintf(stderr,
                                "Type error: cannot assign %s to %s at line %d, column %d\n",
                                varTypeToString(initType), varTypeToString(declType),
                                node->left->token.line, node->left->token.column);
                        clike_error_count++;
                    }
                }
            }
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
            ss_push(scopes);
            if (node->left) {
                if (node->left->type == TCAST_VAR_DECL) {
                    analyzeStmt(node->left, scopes, retType);
                } else if (node->left->type == TCAST_COMPOUND) {
                    for (int i = 0; i < node->left->child_count; ++i) {
                        analyzeStmt(node->left->children[i], scopes, retType);
                    }
                } else {
                    analyzeExpr(node->left, scopes);
                }
            }
            if (node->right) analyzeExpr(node->right, scopes);
            if (node->third) analyzeExpr(node->third, scopes);
            if (node->child_count > 0) analyzeStmt(node->children[0], scopes, retType);
            ss_pop(scopes);
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
            } else if (t != TYPE_UNKNOWN && t != retType &&
                       !(is_real_type(t) && is_real_type(retType)) &&
                       !(is_intlike_type(t) && is_intlike_type(retType))) {
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
        case TCAST_THREAD_JOIN: {
            if (node->left) {
                VarType t = analyzeExpr(node->left, scopes);
                if (!is_intlike_type(t)) {
                    fprintf(stderr,
                            "Type error: join expects integer thread id at line %d, column %d\n",
                            node->left->token.line, node->left->token.column);
                    clike_error_count++;
                }
            }
            break;
        }
        default:
            if (node->type == TCAST_ASSIGN) {
                analyzeExpr(node, scopes);
            }
            break;
    }
}

static void analyzeFunction(ASTNodeClike *func) {
    ScopeStack scopes = {0};

    // Global scope available to all functions
    ss_push(&scopes);
    for (int i = 0; i < globalVars.count; ++i) {
        ss_add(&scopes, globalVars.entries[i].name, globalVars.entries[i].type, globalVars.entries[i].decl);
    }

    // Function scope for parameters/local variables
    ss_push(&scopes);
    if (func->left) {
        for (int i = 0; i < func->left->child_count; ++i) {
            ASTNodeClike *p = func->left->children[i];
            char *name = tokenToCString(p->token);
            ss_add(&scopes, name, p->var_type, p);
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

    typedef struct {
        ASTNodeClike *prog;
        char *source;
        char *allocated_path;
    } ImportModule;

    ImportModule *modules = NULL;
    if (clike_import_count > 0) {
        modules = (ImportModule *)malloc(sizeof(ImportModule) * clike_import_count);
    }

    for (int i = 0; i < clike_import_count; ++i) {
        const char *orig_path = clike_imports[i];
        const char *path = orig_path;
        char *allocated_path = NULL;
        FILE *f = fopen(path, "rb");
        if (!f) {
            const char *lib_dir = getenv("CLIKE_LIB_DIR");
            if (lib_dir && *lib_dir) {
                size_t len = strlen(lib_dir) + 1 + strlen(orig_path) + 1;
                allocated_path = (char *)malloc(len);
                snprintf(allocated_path, len, "%s/%s", lib_dir, orig_path);
                f = fopen(allocated_path, "rb");
                if (f) path = allocated_path; else { free(allocated_path); allocated_path = NULL; }
            }
        }
        if (!f) {
            const char *default_dir = "/usr/local/pscal/clike/lib";
            size_t len = strlen(default_dir) + 1 + strlen(orig_path) + 1;
            allocated_path = (char *)malloc(len);
            snprintf(allocated_path, len, "%s/%s", default_dir, orig_path);
            f = fopen(allocated_path, "rb");
            if (f) path = allocated_path; else { free(allocated_path); allocated_path = NULL; }
        }
        if (!f) {
            fprintf(stderr, "Could not open import '%s'\n", orig_path);
            modules[i].prog = NULL;
            modules[i].source = NULL;
            modules[i].allocated_path = NULL;
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        rewind(f);
        char *src = (char *)malloc(len + 1);
        if (!src) {
            fclose(f);
            free(allocated_path);
            modules[i].prog = NULL;
            modules[i].source = NULL;
            modules[i].allocated_path = NULL;
            continue;
        }
        size_t bytes_read = fread(src, 1, len, f);
        if (bytes_read != (size_t)len) {
            fprintf(stderr, "Error reading module '%s'\n", path);
            free(src);
            fclose(f);
            free(allocated_path);
            modules[i].prog = NULL;
            modules[i].source = NULL;
            modules[i].allocated_path = NULL;
            continue;
        }
        src[len] = '\0';
        fclose(f);

        ParserClike p; initParserClike(&p, src);
        ASTNodeClike *modProg = parseProgramClike(&p);
        freeParserClike(&p);

        modules[i].prog = modProg;
        modules[i].source = src;
        modules[i].allocated_path = allocated_path;
    }

    for (int i = 0; i < clike_import_count; ++i) {
        if (!modules[i].prog) continue;
        for (int j = 0; j < modules[i].prog->child_count; ++j) {
            ASTNodeClike *decl = modules[i].prog->children[j];
            if (decl->type == TCAST_FUN_DECL) {
                char *name = tokenToCString(decl->token);
                functions[functionCount].name = name;
                functions[functionCount].type = decl->var_type;
                functionCount++;
            }
        }
    }

    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_FUN_DECL) {
            char *name = tokenToCString(decl->token);
            functions[functionCount].name = name;
            functions[functionCount].type = decl->var_type;
            functionCount++;
        }
    }

    // Process global variable declarations so functions can reference them.
    globalVars.count = 0;
    ScopeStack globalsScope = {0};
    ss_push(&globalsScope);
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_VAR_DECL) {
            char *name = tokenToCString(decl->token);
            ss_add(&globalsScope, name, decl->var_type, decl);
            vt_add(&globalVars, name, decl->var_type, decl);
            if (decl->left) analyzeExpr(decl->left, &globalsScope);
            free(name);
        }
    }
    ss_pop(&globalsScope);

    for (int i = 0; i < clike_import_count; ++i) {
        if (!modules[i].prog) continue;
        for (int j = 0; j < modules[i].prog->child_count; ++j) {
            ASTNodeClike *decl = modules[i].prog->children[j];
            if (decl->type == TCAST_FUN_DECL) analyzeFunction(decl);
        }
    }

    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_FUN_DECL) analyzeFunction(decl);
    }

    for (int i = 0; i < clike_import_count; ++i) {
        if (modules[i].prog) freeASTClike(modules[i].prog);
        if (modules[i].source) free(modules[i].source);
        if (modules[i].allocated_path) free(modules[i].allocated_path);
    }
    free(modules);

    for (int i = 0; i < functionCount; ++i) free(functions[i].name);
}
