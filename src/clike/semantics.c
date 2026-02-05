#include "clike/semantics.h"
#include "core/utils.h"
#include "clike/errors.h"
#include "clike/builtins.h"
#include "clike/parser.h"
#include "backend_ast/builtin.h"
#include "pscal_paths.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static int builtinMatches(const char *name, const char *const *candidates, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (strcasecmp(name, candidates[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static VarType builtinReturnType(const char* name) {
    if (!name) return TYPE_VOID;

    static const char *const charFuncs[] = {
        "chr", "readkey", "upcase", "toupper", "char", "tochar"
    };
    if (builtinMatches(name, charFuncs, sizeof(charFuncs) / sizeof(charFuncs[0]))) {
        return TYPE_CHAR;
    }

    static const char *const booleanFuncs[] = {
        "bool", "tobool", "keypressed", "issoundplaying", "quitrequested",
        "eof", "mstreamloadfromfile"
    };
    if (builtinMatches(name, booleanFuncs, sizeof(booleanFuncs) / sizeof(booleanFuncs[0]))) {
        return TYPE_BOOLEAN;
    }

    static const char *const stringFuncs[] = {
        "inttostr", "realtostr", "formatfloat", "paramstr", "copy", "getenv", "dosgetenv",
        "findfirst", "findnext", "dosfindfirst", "dosfindnext", "mstreambuffer",
        "dnslookup", "apireceive", "jsonget", "httpgetheader", "socketpeeraddr",
        "httpgetlastheaders", "httplasterror"
    };
    if (builtinMatches(name, stringFuncs, sizeof(stringFuncs) / sizeof(stringFuncs[0]))) {
        return TYPE_STRING;
    }

    static const char *const memoryStreamFuncs[] = {
        "apisend", "socketreceive", "mstreamcreate"
    };
    if (builtinMatches(name, memoryStreamFuncs, sizeof(memoryStreamFuncs) / sizeof(memoryStreamFuncs[0]))) {
        return TYPE_MEMORYSTREAM;
    }

    static const char *const pointerFuncs[] = { "newobj" };
    if (builtinMatches(name, pointerFuncs, sizeof(pointerFuncs) / sizeof(pointerFuncs[0]))) {
        return TYPE_POINTER;
    }

    static const char *const fileFuncs[] = { "fopen" };
    if (builtinMatches(name, fileFuncs, sizeof(fileFuncs) / sizeof(fileFuncs[0]))) {
        return TYPE_FILE;
    }

    static const char *const byteFuncs[] = { "byte", "tobyte" };
    if (builtinMatches(name, byteFuncs, sizeof(byteFuncs) / sizeof(byteFuncs[0]))) {
        return TYPE_BYTE;
    }

    static const char *const floatFuncs[] = { "float", "tofloat" };
    if (builtinMatches(name, floatFuncs, sizeof(floatFuncs) / sizeof(floatFuncs[0]))) {
        return TYPE_FLOAT;
    }

    static const char *const doubleFuncs[] = {
        "cos", "sin", "tan", "ln", "exp", "real", "cosh", "sinh", "tanh",
        "cotan", "arccos", "arcsin", "arctan", "atan2", "double", "todouble",
        "realtimeclock"
    };
    if (builtinMatches(name, doubleFuncs, sizeof(doubleFuncs) / sizeof(doubleFuncs[0]))) {
        return TYPE_DOUBLE;
    }

    static const char *const longDoubleFuncs[] = { "sqrt", "chudnovsky" };
    if (builtinMatches(name, longDoubleFuncs, sizeof(longDoubleFuncs) / sizeof(longDoubleFuncs[0]))) {
        return TYPE_LONG_DOUBLE;
    }

    static const char *const int64Funcs[] = { "paramcount" };
    if (builtinMatches(name, int64Funcs, sizeof(int64Funcs) / sizeof(int64Funcs[0]))) {
        /* Return a wide integer to match the builtin implementation. */
        return TYPE_INT64;
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
    int is_const;
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

static void vtAdd(VarTable *t, const char *name, VarType type, ASTNodeClike *decl, int is_const) {
    t->entries[t->count].name = strdup(name);
    t->entries[t->count].type = type;
    t->entries[t->count].decl = decl;
    t->entries[t->count].is_const = is_const;
    t->count++;
}

static int vtContains(VarTable *t, const char *name) {
    for (int i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return 1;
        }
    }
    return 0;
}

static VarType vtGetType(VarTable *t, const char *name) {
    for (int i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return t->entries[i].type;
        }
    }
    return TYPE_UNKNOWN;
}

static ASTNodeClike* vtGetDecl(VarTable *t, const char *name) {
    for (int i = 0; i < t->count; ++i) {
        if (strcmp(t->entries[i].name, name) == 0) {
            return t->entries[i].decl;
        }
    }
    return NULL;
}

static void vtFree(VarTable *t) {
    for (int i = 0; i < t->count; ++i) free(t->entries[i].name);
    t->count = 0;
}

static void ssPush(ScopeStack *s) {
    s->scopes[s->depth].count = 0;
    s->depth++;
}

static void ssPop(ScopeStack *s) {
    if (s->depth <= 0) return;
    vtFree(&s->scopes[s->depth - 1]);
    s->depth--;
}

static int ssAdd(ScopeStack *s, const char *name, VarType type, ASTNodeClike *decl, int is_const) {
    if (s->depth <= 0) return 0;
    VarTable *current = &s->scopes[s->depth - 1];
    if (vtContains(current, name)) {
        const char *kind = "declaration";
        if (decl && decl->type == TCAST_PARAM) kind = "parameter";
        int line = decl ? decl->token.line : 0;
        int column = decl ? decl->token.column : 0;
        fprintf(stderr,
                "Scope error: duplicate %s '%s' at line %d, column %d\n",
                kind,
                name,
                line,
                column);
        clike_error_count++;
        return 0;
    }
    vtAdd(current, name, type, decl, is_const);
    return 1;
}

static VarType ssGet(ScopeStack *s, const char *name) {
    for (int i = s->depth - 1; i >= 0; --i) {
        VarType t = vtGetType(&s->scopes[i], name);
        if (t != TYPE_UNKNOWN) return t;
    }
    return TYPE_UNKNOWN;
}

static ASTNodeClike* ssGetDecl(ScopeStack *s, const char *name) {
    for (int i = s->depth - 1; i >= 0; --i) {
        ASTNodeClike *d = vtGetDecl(&s->scopes[i], name);
        if (d) return d;
    }
    return NULL;
}

typedef struct {
    char *name;
    VarType type;
    int has_definition;
    int defined_line;
    int defined_column;
} FuncEntry;

static FuncEntry functions[256];
static int functionCount = 0;

static void registerFunctionSignature(char *name, VarType type,
                                      int has_definition, int line, int column) {
    if (!name) return;
    for (int i = 0; i < functionCount; ++i) {
        if (strcasecmp(functions[i].name, name) == 0) {
            if (has_definition) {
                if (functions[i].has_definition) {
                    fprintf(stderr,
                            "Scope error: duplicate function definition '%s' at line %d, column %d\n",
                            name,
                            line,
                            column);
                    clike_error_count++;
                } else {
                    functions[i].has_definition = 1;
                    functions[i].defined_line = line;
                    functions[i].defined_column = column;
                }
            }
            functions[i].type = type;
            free(name);
            return;
        }
    }
    functions[functionCount].name = name;
    functions[functionCount].type = type;
    functions[functionCount].has_definition = has_definition;
    functions[functionCount].defined_line = has_definition ? line : 0;
    functions[functionCount].defined_column = has_definition ? column : 0;
    functionCount++;
}

static void registerBuiltinFunctions(void) {
    registerFunctionSignature(strdup("printf"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("scanf"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("strlen"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("itoa"), TYPE_VOID, 0, 0, 0);
    registerFunctionSignature(strdup("exit"), TYPE_VOID, 0, 0, 0);
    registerFunctionSignature(strdup("mstreamcreate"), TYPE_MEMORYSTREAM, 0, 0, 0);
    registerFunctionSignature(strdup("mstreamloadfromfile"), TYPE_BOOLEAN, 0, 0, 0);
    registerFunctionSignature(strdup("mstreamsavetofile"), TYPE_VOID, 0, 0, 0);
    registerFunctionSignature(strdup("mstreamfree"), TYPE_VOID, 0, 0, 0);
    registerFunctionSignature(strdup("mstreambuffer"), TYPE_STRING, 0, 0, 0);
    registerFunctionSignature(strdup("hasextbuiltin"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltincategorycount"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltincategoryname"), TYPE_STRING, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltinfunctioncount"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltinfunctionname"), TYPE_STRING, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltingroupcount"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltingroupname"), TYPE_STRING, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltingroupfunctioncount"), TYPE_INT32, 0, 0, 0);
    registerFunctionSignature(strdup("extbuiltingroupfunctionname"), TYPE_STRING, 0, 0, 0);

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
static void analyzeStmt(ASTNodeClike *node, ScopeStack *scopes, VarType retType);

static void analyzeScopedStmt(ASTNodeClike *node, ScopeStack *scopes, VarType retType) {
    if (!node) return;
    if (node->type == TCAST_COMPOUND) {
        analyzeStmt(node, scopes, retType);
        return;
    }
    ssPush(scopes);
    analyzeStmt(node, scopes, retType);
    ssPop(scopes);
}

static int isCharPointerDecl(const ASTNodeClike *decl) {
    return decl && decl->var_type == TYPE_POINTER && decl->element_type == TYPE_CHAR;
}

static int canAssignToType(VarType target, VarType value, int allowStringToCharPointer) {
    if (target == TYPE_UNKNOWN || value == TYPE_UNKNOWN) {
        return 1;
    }
    if (target == value) {
        return 1;
    }
    if (isRealType(target) && isRealType(value)) {
        return 1;
    }
    if (isRealType(target) && isIntlikeType(value)) {
        return 1;
    }
    if (target == TYPE_STRING && value == TYPE_CHAR) {
        return 1;
    }
    if (target == TYPE_STRING && isIntlikeType(value)) {
        return 1;
    }
    if (isIntlikeType(target) && isIntlikeType(value)) {
        return 1;
    }
    if (isIntlikeType(target) && value == TYPE_POINTER) {
        return 1;
    }
    if (target == TYPE_POINTER && value == TYPE_STRING && allowStringToCharPointer) {
        return 1;
    }
    return 0;
}

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
            VarType t = ssGet(scopes, name);
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
            if (isRealType(lt) && isIntlikeType(rt)) {
                node->var_type = lt;
            } else if (isRealType(rt) && isIntlikeType(lt)) {
                node->var_type = rt;
            } else if (isRealType(lt) && isRealType(rt)) {
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
            if (rt == TYPE_POINTER || ft == TYPE_POINTER) {
                node->var_type = TYPE_POINTER;
                node->element_type = TYPE_UNKNOWN;
                if (rt == TYPE_POINTER && node->right) {
                    node->element_type = node->right->element_type;
                }
                if (ft == TYPE_POINTER && node->third) {
                    if (node->element_type == TYPE_UNKNOWN) {
                        node->element_type = node->third->element_type;
                    } else if (node->third->element_type != TYPE_UNKNOWN &&
                               node->element_type != node->third->element_type) {
                        node->element_type = TYPE_UNKNOWN;
                    }
                }
            } else if (isRealType(rt) && isIntlikeType(ft)) {
                node->var_type = rt;
            } else if (isRealType(ft) && isIntlikeType(rt)) {
                node->var_type = ft;
            } else if (isRealType(rt) && isRealType(ft)) {
                if (rt == TYPE_LONG_DOUBLE || ft == TYPE_LONG_DOUBLE) node->var_type = TYPE_LONG_DOUBLE;
                else if (rt == TYPE_DOUBLE || ft == TYPE_DOUBLE) node->var_type = TYPE_DOUBLE;
                else node->var_type = TYPE_FLOAT;
            } else if (rt == TYPE_STRING || ft == TYPE_STRING) {
                node->var_type = TYPE_STRING;
            } else if (rt == TYPE_BOOLEAN && ft == TYPE_BOOLEAN) {
                node->var_type = TYPE_BOOLEAN;
            } else {
                node->var_type = rt != TYPE_UNKNOWN ? rt : ft;
            }
            return node->var_type;
        }
        case TCAST_ADDR: {
            // Address-of: allow &var and &func
            if (node->left && node->left->type == TCAST_IDENTIFIER) {
                char *name = tokenToCString(node->left->token);
                VarType ft = getFunctionType(name);
                free(name);
                if (ft != TYPE_UNKNOWN) {
                    node->var_type = TYPE_POINTER;
                    return TYPE_POINTER;
                }
            }
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_POINTER;
            return TYPE_POINTER;
        }
        case TCAST_DEREF:
            analyzeExpr(node->left, scopes);
            node->var_type = TYPE_UNKNOWN;
            return TYPE_UNKNOWN;
        case TCAST_SIZEOF: {
            size_t size = 0;
            if (node->left) {
                ASTNodeClike *operand = node->left;
                VarType tokenType = clikeTokenTypeToVarType(operand->token.type);
                if (operand->type == TCAST_IDENTIFIER && tokenType != TYPE_UNKNOWN && operand->token.type != CLIKE_TOKEN_IDENTIFIER) {
                    size = varTypeSize(tokenType);
                } else {
                    VarType t = analyzeExpr(operand, scopes);
                    if (operand->type == TCAST_IDENTIFIER) {
                        char *name = tokenToCString(operand->token);
                        ASTNodeClike *decl = ssGetDecl(scopes, name);
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
            ASTNodeClike *lhsDecl = NULL;
            ASTNodeClike *base = node->left;
            while (base && base->type == TCAST_ARRAY_ACCESS) base = base->left;
            if (base && base->type == TCAST_MEMBER) base = base->left;
            char *lhsName = NULL;
            if (base && base->type == TCAST_IDENTIFIER) {
                lhsName = tokenToCString(base->token);
                lhsDecl = ssGetDecl(scopes, lhsName);
            }
            if (lhsDecl && lhsDecl->is_const) {
                const char *display = lhsName ? lhsName : "<const>";
                int line = base ? base->token.line : node->token.line;
                int column = base ? base->token.column : node->token.column;
                fprintf(stderr,
                        "Type error: cannot assign to const variable '%s' at line %d, column %d\n",
                        display,
                        line,
                        column);
                clike_error_count++;
            }
            free(lhsName);
            int allowStringToCharPointer = 0;
            if (lt == TYPE_POINTER && rt == TYPE_STRING) {
                ASTNodeClike *lhsDeclForPtr = NULL;
                if (node->left && node->left->type == TCAST_IDENTIFIER) {
                    char *lhsName = tokenToCString(node->left->token);
                    lhsDeclForPtr = ssGetDecl(scopes, lhsName);
                    free(lhsName);
                }
                if (isCharPointerDecl(lhsDeclForPtr)) {
                    allowStringToCharPointer = 1;
                }
            }
            if (lt != TYPE_UNKNOWN && rt != TYPE_UNKNOWN) {
            if (!canAssignToType(lt, rt, allowStringToCharPointer)) {
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
                    if (!isIntlikeType(at)) {
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
            if (strcasecmp(name, "lock") == 0 || strcasecmp(name, "unlock") == 0 || strcasecmp(name, "destroy") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr,
                            "Type error: %s expects 1 argument at line %d, column %d\n",
                            name, node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    VarType at = analyzeExpr(node->children[0], scopes);
                    if (!isIntlikeType(at)) {
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
            if (strcasecmp(name, "destroymutex") == 0) {
                if (node->child_count != 1) {
                    fprintf(stderr,
                            "Type error: %s expects 1 argument at line %d, column %d\n",
                            name, node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    VarType at = analyzeExpr(node->children[0], scopes);
                    if (!isIntlikeType(at)) {
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
            BuiltinRoutineType builtinKind = BUILTIN_TYPE_NONE;
            if (t == TYPE_UNKNOWN) {
                int bid = clikeGetBuiltinID(name);
                if (bid != -1) {
                    t = builtinReturnType(name);
                    builtinKind = getBuiltinType(name);
                    if ((t == TYPE_VOID || t == TYPE_UNKNOWN) && builtinKind == BUILTIN_TYPE_FUNCTION) {
                        t = TYPE_INT32;
                    }
                } else {
                    // Known VM builtins not in clike's local map (HTTP helpers):
                    if (strcasecmp(name, "httpsession") == 0 || strcasecmp(name, "httprequest") == 0) {
                        t = TYPE_INT32;
                    } else if (strcasecmp(name, "getpid") == 0) {
                        t = TYPE_INT32;
                    } else {
                        // Allow indirect calls through variables (function pointers): if a variable
                        // with this name exists in any visible scope, treat the call as indirect.
                        VarType vt = TYPE_UNKNOWN;
                        for (int i = scopes->depth - 1; i >= 0; --i) {
                            vt = vtGetType(&scopes->scopes[i], name);
                            if (vt != TYPE_UNKNOWN) break;
                        }
                        if (vt == TYPE_UNKNOWN) {
                            vt = vtGetType(&globalVars, name);
                        }
                        if (vt != TYPE_UNKNOWN) {
                            // Indirect function pointer call: conservatively assume int return
                            t = TYPE_INT32;
                        } else {
                            fprintf(stderr,
                                    "Type error: call to undefined function '%s' at line %d, column %d\n",
                                    name,
                                    node->token.line,
                                    node->token.column);
                            clike_error_count++;
                        }
                    }
                }
            }
            // Final fallback for runtime-registered builtins that need explicit validation
            if (strcasecmp(name, "realtimeclock") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr,
                            "Type error: realtimeclock expects no arguments at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_DOUBLE;
            } else if (strcasecmp(name, "httpsession") == 0) {
                if (node->child_count != 0) {
                    fprintf(stderr,
                            "Type error: httpsession expects no arguments at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_INT32;
            } else if (strcasecmp(name, "httpclose") == 0) {
                if (node->child_count != 1 || !isIntlikeType(analyzeExpr(node->children[0], scopes))) {
                    fprintf(stderr,
                            "Type error: httpclose expects (session:int) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_VOID;
            } else if (strcasecmp(name, "httpsetheader") == 0) {
                if (node->child_count != 3) {
                    fprintf(stderr,
                            "Type error: httpsetheader expects (session:int, name:string, value:string) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    if (!isIntlikeType(analyzeExpr(node->children[0], scopes)) ||
                        analyzeExpr(node->children[1], scopes) != TYPE_STRING ||
                        analyzeExpr(node->children[2], scopes) != TYPE_STRING) {
                        fprintf(stderr,
                                "Type error: httpsetheader argument types are (int, string, string) at line %d, column %d\n",
                                node->token.line, node->token.column);
                        clike_error_count++;
                    }
                }
                t = TYPE_VOID;
            } else if (strcasecmp(name, "httpclearheaders") == 0) {
                if (node->child_count != 1 || !isIntlikeType(analyzeExpr(node->children[0], scopes))) {
                    fprintf(stderr,
                            "Type error: httpclearheaders expects (session:int) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_VOID;
            } else if (strcasecmp(name, "httpsetoption") == 0) {
                if (node->child_count != 3) {
                    fprintf(stderr,
                            "Type error: httpsetoption expects (session:int, key:string, value:int|string) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    VarType a0 = analyzeExpr(node->children[0], scopes);
                    VarType a1 = analyzeExpr(node->children[1], scopes);
                    VarType a2 = analyzeExpr(node->children[2], scopes);
                    if (!isIntlikeType(a0) || a1 != TYPE_STRING || !(isIntlikeType(a2) || a2 == TYPE_STRING)) {
                        fprintf(stderr,
                                "Type error: httpsetoption expects (int, string, int|string) at line %d, column %d\n",
                                node->token.line, node->token.column);
                        clike_error_count++;
                    }
                }
                t = TYPE_VOID;
            } else if (strcasecmp(name, "httpgetlastheaders") == 0) {
                if (node->child_count != 1 || !isIntlikeType(analyzeExpr(node->children[0], scopes))) {
                    fprintf(stderr,
                            "Type error: httpgetlastheaders expects (session:int) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_STRING;
            } else if (strcasecmp(name, "httpgetheader") == 0) {
                if (node->child_count != 2) {
                    fprintf(stderr,
                            "Type error: httpgetheader expects (session:int, name:string) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    VarType a0 = analyzeExpr(node->children[0], scopes);
                    VarType a1 = analyzeExpr(node->children[1], scopes);
                    if (!isIntlikeType(a0) || a1 != TYPE_STRING) {
                        fprintf(stderr,
                                "Type error: httpgetheader expects (int, string) at line %d, column %d\n",
                                node->token.line, node->token.column);
                        clike_error_count++;
                    }
                }
                t = TYPE_STRING;
            } else if (strcasecmp(name, "httperrorcode") == 0) {
                if (node->child_count != 1 || !isIntlikeType(analyzeExpr(node->children[0], scopes))) {
                    fprintf(stderr,
                            "Type error: httperrorcode expects (session:int) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_INT32;
            } else if (strcasecmp(name, "httplasterror") == 0) {
                if (node->child_count != 1 || !isIntlikeType(analyzeExpr(node->children[0], scopes))) {
                    fprintf(stderr,
                            "Type error: httplasterror expects (session:int) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_STRING;
            } else if ((strcasecmp(name, "httprequest") == 0)) {
                // We leave flexible checking for now; ensure it returns int
                t = TYPE_INT32;
            } else if ((strcasecmp(name, "httprequesttofile") == 0)) {
                // httprequesttofile(session:int, method:string, url:string, body:string|mstream|nil, out:string)
                if (node->child_count != 5) {
                    fprintf(stderr,
                            "Type error: httprequesttofile expects 5 arguments at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                } else {
                    VarType a0 = analyzeExpr(node->children[0], scopes);
                    VarType a1 = analyzeExpr(node->children[1], scopes);
                    VarType a2 = analyzeExpr(node->children[2], scopes);
                    VarType a3 = analyzeExpr(node->children[3], scopes);
                    VarType a4 = analyzeExpr(node->children[4], scopes);
                    if (!isIntlikeType(a0) || a1 != TYPE_STRING || a2 != TYPE_STRING || !(a3 == TYPE_STRING || a3 == TYPE_MEMORYSTREAM || a3 == TYPE_NIL) || a4 != TYPE_STRING) {
                        fprintf(stderr,
                                "Type error: httprequesttofile expects (int, string, string, string|mstream|nil, string) at line %d, column %d\n",
                                node->token.line, node->token.column);
                        clike_error_count++;
                    }
                }
                t = TYPE_INT32;
            } else if ((strcasecmp(name, "httprequestasync") == 0)) {
                if (node->child_count != 4) {
                    fprintf(stderr,
                            "Type error: httprequestasync expects 4 arguments at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_INT32;
            } else if ((strcasecmp(name, "httprequestasynctofile") == 0)) {
                if (node->child_count != 5) {
                    fprintf(stderr,
                            "Type error: httprequestasynctofile expects 5 arguments at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_INT32;
            } else if (strcasecmp(name, "httptryawait") == 0) {
                if (node->child_count != 2) {
                    fprintf(stderr,
                            "Type error: httptryawait expects (id:int, out:mstream) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_INT32;
            } else if (strcasecmp(name, "httpisdone") == 0) {
                if (node->child_count != 1 || !isIntlikeType(analyzeExpr(node->children[0], scopes))) {
                    fprintf(stderr,
                            "Type error: httpisdone expects (id:int) at line %d, column %d\n",
                            node->token.line, node->token.column);
                    clike_error_count++;
                }
                t = TYPE_INT32;
            } else if ((strcasecmp(name, "httpsession") == 0 || strcasecmp(name, "httprequest") == 0) &&
                       (t == TYPE_UNKNOWN || t == TYPE_VOID)) {
                t = TYPE_INT32;
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
                    arrDecl = ssGetDecl(scopes, name);
                    free(name);
                } else if (node->left->is_array) {
                    arrDecl = node->left;
                }
            }
            for (int i = 0; i < node->child_count; ++i) {
                VarType idxType = analyzeExpr(node->children[i], scopes);
                if (!isIntlikeType(idxType)) {
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
            ssAdd(scopes, name, node->var_type, node, node->is_const);
            free(name);
            if (node->left) {
                VarType initType = analyzeExpr(node->left, scopes);
                VarType declType = node->var_type;
                if (declType == TYPE_ARRAY && node->element_type == TYPE_CHAR &&
                    node->left->type == TCAST_STRING) {
                    initType = declType;
                }
                if (!canAssignToType(declType,
                                     initType,
                                     isCharPointerDecl(node))) {
                    fprintf(stderr,
                            "Type error: cannot assign %s to %s at line %d, column %d\n",
                            varTypeToString(initType), varTypeToString(declType),
                            node->left->token.line, node->left->token.column);
                    clike_error_count++;
                }
            }
            break;
        }
        case TCAST_STRUCT_DECL:
            break;
        case TCAST_COMPOUND:
            ssPush(scopes);
            for (int i = 0; i < node->child_count; ++i) {
                analyzeStmt(node->children[i], scopes, retType);
            }
            ssPop(scopes);
            break;
        case TCAST_IF:
            analyzeExpr(node->left, scopes);
            analyzeScopedStmt(node->right, scopes, retType);
            analyzeScopedStmt(node->third, scopes, retType);
            break;
        case TCAST_WHILE:
            analyzeExpr(node->left, scopes);
            analyzeScopedStmt(node->right, scopes, retType);
            break;
        case TCAST_FOR:
            ssPush(scopes);
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
            ssPop(scopes);
            break;
        case TCAST_DO_WHILE:
            analyzeScopedStmt(node->right, scopes, retType);
            analyzeExpr(node->left, scopes);
            break;
        case TCAST_SWITCH:
            analyzeExpr(node->left, scopes);
            for (int i = 0; i < node->child_count; ++i) {
                ASTNodeClike *c = node->children[i];
                ssPush(scopes);
                analyzeExpr(c->left, scopes);
                for (int j = 0; j < c->child_count; ++j) {
                    analyzeStmt(c->children[j], scopes, retType);
                }
                ssPop(scopes);
            }
            analyzeScopedStmt(node->right, scopes, retType);
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
            } else if (!canAssignToType(retType, t, 0)) {
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
                if (!isIntlikeType(t)) {
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
    if (!func || !func->right) return;
    ScopeStack *scopes = (ScopeStack *)calloc(1, sizeof(ScopeStack));
    if (!scopes) {
        return;
    }

    // Global scope available to all functions
    ssPush(scopes);
    for (int i = 0; i < globalVars.count; ++i) {
        ssAdd(scopes,
              globalVars.entries[i].name,
              globalVars.entries[i].type,
              globalVars.entries[i].decl,
              globalVars.entries[i].is_const);
    }

    // Function scope for parameters/local variables
    ssPush(scopes);
    if (func->left) {
        for (int i = 0; i < func->left->child_count; ++i) {
            ASTNodeClike *p = func->left->children[i];
            char *name = tokenToCString(p->token);
            ssAdd(scopes, name, p->var_type, p, p->is_const);
            free(name);
        }
    }
    analyzeStmt(func->right, scopes, func->var_type);
    while (scopes->depth > 0) ssPop(scopes);
    free(scopes);
}

void analyzeSemanticsClike(ASTNodeClike *program, const char *current_path) {
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
        if (current_path && strcmp(orig_path, current_path) == 0) {
            modules[i].prog = NULL;
            modules[i].source = NULL;
            modules[i].allocated_path = NULL;
            continue;
        }
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
            const char *default_dir = PSCAL_CLIKE_LIB_DIR;
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
                int has_body = decl->right != NULL;
                registerFunctionSignature(name, decl->var_type, has_body,
                                          decl->token.line, decl->token.column);
            }
        }
    }

    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_FUN_DECL) {
            char *name = tokenToCString(decl->token);
            int has_body = decl->right != NULL;
            registerFunctionSignature(name, decl->var_type, has_body,
                                      decl->token.line, decl->token.column);
        }
    }

    // Process global variable declarations so functions can reference them.
    globalVars.count = 0;
    ScopeStack *globalsScope = (ScopeStack *)calloc(1, sizeof(ScopeStack));
    if (!globalsScope) {
        if (modules) {
            for (int i = 0; i < clike_import_count; ++i) {
                if (modules[i].prog) freeASTClike(modules[i].prog);
                if (modules[i].source) free(modules[i].source);
                if (modules[i].allocated_path) free(modules[i].allocated_path);
            }
            free(modules);
        }
        return;
    }
    ssPush(globalsScope);
    for (int i = 0; i < clike_import_count; ++i) {
        if (!modules[i].prog) continue;
        for (int j = 0; j < modules[i].prog->child_count; ++j) {
            ASTNodeClike *decl = modules[i].prog->children[j];
            if (decl->type == TCAST_VAR_DECL) {
                char *name = tokenToCString(decl->token);
                if (ssAdd(globalsScope, name, decl->var_type, decl, decl->is_const)) {
                    vtAdd(&globalVars, name, decl->var_type, decl, decl->is_const);
                }
                if (decl->left) analyzeExpr(decl->left, globalsScope);
                free(name);
            }
        }
    }
    for (int i = 0; i < program->child_count; ++i) {
        ASTNodeClike *decl = program->children[i];
        if (decl->type == TCAST_VAR_DECL) {
            char *name = tokenToCString(decl->token);
            if (ssAdd(globalsScope, name, decl->var_type, decl, decl->is_const)) {
                vtAdd(&globalVars, name, decl->var_type, decl, decl->is_const);
            }
            if (decl->left) analyzeExpr(decl->left, globalsScope);
            free(name);
        }
    }
    ssPop(globalsScope);

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
    free(globalsScope);

    for (int i = 0; i < functionCount; ++i) {
        free(functions[i].name);
        functions[i].name = NULL;
    }
}

void clikeResetSemanticsState(void) {
    vtFree(&globalVars);
    for (int i = 0; i < functionCount; ++i) {
        if (functions[i].name) {
            free(functions[i].name);
            functions[i].name = NULL;
        }
        functions[i].type = TYPE_UNKNOWN;
        functions[i].has_definition = 0;
        functions[i].defined_line = 0;
        functions[i].defined_column = 0;
    }
    functionCount = 0;
}
