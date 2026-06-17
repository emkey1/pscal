/*
 * translate.c - lower the CLike front-end AST (ASTNodeClike) into the shared
 * PSCAL AST (AST), so CLike can use the common bytecode compiler + VM instead
 * of its own codegen. Mirrors how Rea drives the shared pipeline:
 *   parse -> (this) translate -> annotateTypes -> compileASTToBytecode.
 *
 * Design notes:
 *  - The CLike parser already desugars ++/-- and += style compound assignments
 *    into plain TCAST_ASSIGN(x, x <op> 1), so this pass never sees them.
 *  - CLike `switch` already auto-breaks (no fall-through), so it maps directly
 *    onto the shared AST_CASE.
 *  - C `for(init;cond;post) body` is lowered to { init; while(cond){ body; post; } }.
 *  - C `do body while(cond)` is lowered to AST_REPEAT body until !(cond).
 *  - Field access through a pointer (`p->f`) is lowered to (*p).f by inserting
 *    an AST_DEREFERENCE when the receiver is typed TYPE_POINTER.
 */

#include "clike/translate.h"
#include "clike/ast.h"
#include "clike/builtins.h"
#include "clike/lexer.h"
#include "clike/parser.h"
#include "clike/semantics.h"
#include "ast/ast.h"
#include "core/types.h"
#include "core/utils.h"
#include <pscal_paths.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

/* Build a shared AST node from a freshly-made transient token. newASTNode()
 * deep-copies the token, so we own and must free the transient afterwards. */
static AST *mkNode(ASTNodeType type, TokenType tt, const char *value, int line) {
    Token *tok = newToken(tt, value ? value : "", line, 0);
    AST *n = newASTNode(type, tok);
    if (tok) freeToken(tok);
    return n;
}

/* Node with no token. */
static AST *mkBare(ASTNodeType type) {
    return newASTNode(type, NULL);
}

/* Duplicate a CLike token's raw lexeme as a NUL-terminated C string. */
static char *lexToStr(ClikeToken t) {
    char *s = (char *)malloc((size_t)t.length + 1);
    if (!s) return NULL;
    if (t.length > 0 && t.lexeme) memcpy(s, t.lexeme, (size_t)t.length);
    s[t.length] = '\0';
    return s;
}

/* Cook a CLike string-literal lexeme, interpreting the same escape set the
 * legacy codegen honored (\n \r \t \\ \"). */
static char *cookString(ClikeToken t) {
    char *s = (char *)malloc((size_t)t.length + 1);
    if (!s) return NULL;
    int j = 0;
    for (int i = 0; i < t.length; i++) {
        char c = t.lexeme[i];
        if (c == '\\' && i + 1 < t.length) {
            char next = t.lexeme[++i];
            switch (next) {
                case 'n': s[j++] = '\n'; break;
                case 'r': s[j++] = '\r'; break;
                case 't': s[j++] = '\t'; break;
                case '\\': s[j++] = '\\'; break;
                case '"': s[j++] = '"'; break;
                default: s[j++] = next; break;
            }
        } else {
            s[j++] = c;
        }
    }
    s[j] = '\0';
    return s;
}

static int isIntlike(VarType vt) {
    switch (vt) {
        case TYPE_INT8: case TYPE_UINT8:
        case TYPE_INT16: case TYPE_UINT16:
        case TYPE_INT32: case TYPE_UINT32:
        case TYPE_INT64: case TYPE_UINT64:
        case TYPE_BYTE: case TYPE_WORD:
        case TYPE_BOOLEAN: case TYPE_CHAR:
            return 1;
        default:
            return 0;
    }
}

/* Forward declarations. */
static AST *xExpr(ASTNodeClike *n);
static AST *xStmt(ASTNodeClike *n);
static AST *xTypeNode(ASTNodeClike *decl, int line);

/* ------------------------------------------------------------------ */
/* Type nodes                                                         */
/* ------------------------------------------------------------------ */

static AST *scalarTypeNode(VarType vt, ASTNodeClike *typeIdent, int line) {
    if (vt == TYPE_RECORD && typeIdent && typeIdent->type == TCAST_IDENTIFIER) {
        char *name = lexToStr(typeIdent->token);
        AST *t = mkNode(AST_TYPE_REFERENCE, TOKEN_IDENTIFIER, name, line);
        free(name);
        t->var_type = TYPE_RECORD;
        return t;
    }
    const char *spelling = varTypeToString(vt);
    AST *t = mkNode(AST_TYPE_IDENTIFIER, TOKEN_IDENTIFIER, spelling, line);
    t->var_type = vt;
    return t;
}

/* Build the shared type node for a CLike declaration (the `right` slot of an
 * AST_VAR_DECL / the return-type slot of a function). */
static AST *xTypeNode(ASTNodeClike *decl, int line) {
    if (!decl) return scalarTypeNode(TYPE_VOID, NULL, line);

    if (decl->is_array) {
        AST *arr = mkBare(AST_ARRAY_TYPE);
        arr->var_type = TYPE_ARRAY;
        /* element type in `right` */
        setRight(arr, scalarTypeNode(decl->element_type, decl->right, line));
        int dims = decl->dim_count > 0 ? decl->dim_count : 1;
        for (int d = 0; d < dims; d++) {
            int hi = (decl->array_dims && decl->array_dims[d] > 0)
                         ? decl->array_dims[d] - 1
                         : 0;
            AST *range = mkBare(AST_SUBRANGE);
            AST *lo = mkNode(AST_NUMBER, TOKEN_INTEGER_CONST, "0", line);
            lo->var_type = TYPE_INT64; lo->i_val = 0;
            char buf[32]; snprintf(buf, sizeof(buf), "%d", hi);
            AST *hin = mkNode(AST_NUMBER, TOKEN_INTEGER_CONST, buf, line);
            hin->var_type = TYPE_INT64; hin->i_val = hi;
            setLeft(range, lo);
            setRight(range, hin);
            addChild(arr, range);
        }
        return arr;
    }

    if (decl->var_type == TYPE_POINTER) {
        AST *ptr = mkBare(AST_POINTER_TYPE);
        ptr->var_type = TYPE_POINTER;
        /* base type in `right`: a named record if we have one, else element. */
        VarType base = decl->element_type != TYPE_UNKNOWN ? decl->element_type : TYPE_VOID;
        setRight(ptr, scalarTypeNode(base, decl->right, line));
        return ptr;
    }

    return scalarTypeNode(decl->var_type, decl->right, line);
}

/* ------------------------------------------------------------------ */
/* Expressions                                                        */
/* ------------------------------------------------------------------ */

/* Map a CLike binary operator token to a shared operator TokenType. The SLASH
 * int-vs-real choice is made by the caller (needs operand types). */
static TokenType binOpToken(ClikeTokenType t) {
    switch (t) {
        case CLIKE_TOKEN_PLUS: return TOKEN_PLUS;
        case CLIKE_TOKEN_MINUS: return TOKEN_MINUS;
        case CLIKE_TOKEN_STAR: return TOKEN_MUL;
        case CLIKE_TOKEN_PERCENT: return TOKEN_MOD;
        case CLIKE_TOKEN_GREATER: return TOKEN_GREATER;
        case CLIKE_TOKEN_GREATER_EQUAL: return TOKEN_GREATER_EQUAL;
        case CLIKE_TOKEN_LESS: return TOKEN_LESS;
        case CLIKE_TOKEN_LESS_EQUAL: return TOKEN_LESS_EQUAL;
        case CLIKE_TOKEN_EQUAL_EQUAL: return TOKEN_EQUAL;
        case CLIKE_TOKEN_BANG_EQUAL: return TOKEN_NOT_EQUAL;
        case CLIKE_TOKEN_AND_AND: return TOKEN_AND;
        case CLIKE_TOKEN_OR_OR: return TOKEN_OR;
        case CLIKE_TOKEN_BIT_AND: return TOKEN_AND;
        case CLIKE_TOKEN_BIT_OR: return TOKEN_OR;
        case CLIKE_TOKEN_BIT_XOR: return TOKEN_XOR;
        case CLIKE_TOKEN_SHL: return TOKEN_SHL;
        case CLIKE_TOKEN_SHR: return TOKEN_SHR;
        default: return TOKEN_PLUS;
    }
}

static AST *xBinop(ASTNodeClike *n) {
    TokenType tt;
    const char *spell = "+";
    if (n->token.type == CLIKE_TOKEN_SLASH) {
        int bothInt = n->left && n->right &&
                      isIntlike(n->left->var_type) && isIntlike(n->right->var_type) &&
                      isIntlike(n->var_type);
        tt = bothInt ? TOKEN_INT_DIV : TOKEN_SLASH;
        spell = bothInt ? "div" : "/";
    } else {
        tt = binOpToken(n->token.type);
        spell = "op";
    }
    AST *b = mkNode(AST_BINARY_OP, tt, spell, n->token.line);
    setLeft(b, xExpr(n->left));
    setRight(b, xExpr(n->right));
    b->var_type = n->var_type;
    /* C `&&`/`||` are always logical and short-circuit. The shared compiler
     * only short-circuits when the node's type is boolean (otherwise it treats
     * integer operands as bitwise), so force a boolean result type here. */
    if (n->token.type == CLIKE_TOKEN_AND_AND || n->token.type == CLIKE_TOKEN_OR_OR) {
        b->var_type = TYPE_BOOLEAN;
    }
    return b;
}

static AST *xExpr(ASTNodeClike *n) {
    if (!n) return NULL;
    int line = n->token.line;

    switch (n->type) {
        case TCAST_NUMBER: {
            if (n->token.type == CLIKE_TOKEN_FLOAT_LITERAL) {
                char buf[64];
                snprintf(buf, sizeof(buf), "%.17g", n->token.float_val);
                AST *node = mkNode(AST_NUMBER, TOKEN_REAL_CONST, buf, line);
                node->var_type = TYPE_DOUBLE;
                return node;
            }
            if (n->token.type == CLIKE_TOKEN_CHAR_LITERAL) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%lld", n->token.int_val);
                AST *node = mkNode(AST_NUMBER, TOKEN_INTEGER_CONST, buf, line);
                node->var_type = TYPE_CHAR;
                node->i_val = n->token.int_val;
                return node;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", n->token.int_val);
            AST *node = mkNode(AST_NUMBER, TOKEN_INTEGER_CONST, buf, line);
            node->var_type = isIntlike(n->var_type) ? n->var_type : TYPE_INT64;
            node->i_val = n->token.int_val;
            return node;
        }
        case TCAST_SIZEOF: {
            char buf[32];
            snprintf(buf, sizeof(buf), "%lld", n->token.int_val);
            AST *node = mkNode(AST_NUMBER, TOKEN_INTEGER_CONST, buf, line);
            node->var_type = TYPE_INT64;
            node->i_val = n->token.int_val;
            return node;
        }
        case TCAST_STRING: {
            char *cooked = cookString(n->token);
            if (n->var_type == TYPE_CHAR && cooked) {
                /* single-char literal typed as char */
                AST *node = mkNode(AST_STRING, TOKEN_STRING_CONST, cooked, line);
                node->var_type = TYPE_CHAR;
                node->i_val = (int)strlen(cooked);
                free(cooked);
                return node;
            }
            AST *node = mkNode(AST_STRING, TOKEN_STRING_CONST, cooked ? cooked : "", line);
            node->var_type = TYPE_STRING;
            node->i_val = cooked ? (int)strlen(cooked) : 0;
            free(cooked);
            return node;
        }
        case TCAST_IDENTIFIER: {
            char *name = lexToStr(n->token);
            if (name && strcasecmp(name, "NULL") == 0) {
                free(name);
                AST *nil = mkBare(AST_NIL);
                nil->var_type = TYPE_NIL;
                return nil;
            }
            AST *v = mkNode(AST_VARIABLE, TOKEN_IDENTIFIER, name, line);
            free(name);
            return v;
        }
        case TCAST_BINOP:
            return xBinop(n);
        case TCAST_UNOP: {
            TokenType tt = TOKEN_MINUS;
            const char *spell = "-";
            if (n->token.type == CLIKE_TOKEN_BANG) { tt = TOKEN_NOT; spell = "not"; }
            else if (n->token.type == CLIKE_TOKEN_TILDE) { tt = TOKEN_NOT; spell = "not"; }
            AST *u = mkNode(AST_UNARY_OP, tt, spell, line);
            setLeft(u, xExpr(n->left));
            u->var_type = n->var_type;
            return u;
        }
        case TCAST_TERNARY: {
            AST *t = mkBare(AST_TERNARY);
            setLeft(t, xExpr(n->left));
            setRight(t, xExpr(n->right));
            setExtra(t, xExpr(n->third));
            t->var_type = n->var_type;
            return t;
        }
        case TCAST_ASSIGN: {
            AST *a = mkNode(AST_ASSIGN, TOKEN_ASSIGN, "=", line);
            setLeft(a, xExpr(n->left));
            setRight(a, xExpr(n->right));
            if (n->left) a->var_type = n->left->var_type;
            return a;
        }
        case TCAST_ARRAY_ACCESS: {
            AST *arr = mkBare(AST_ARRAY_ACCESS);
            setLeft(arr, xExpr(n->left));
            for (int i = 0; i < n->child_count; i++) {
                addChild(arr, xExpr(n->children[i]));
            }
            arr->var_type = n->var_type;
            return arr;
        }
        case TCAST_MEMBER: {
            /* receiver; auto-deref when it is a pointer (`p->field`). */
            AST *recv = xExpr(n->left);
            if (n->left && n->left->var_type == TYPE_POINTER) {
                AST *deref = mkBare(AST_DEREFERENCE);
                setLeft(deref, recv);
                recv = deref;
            }
            char *fname = (n->right && n->right->type == TCAST_IDENTIFIER)
                              ? lexToStr(n->right->token) : strdup("");
            AST *fa = mkNode(AST_FIELD_ACCESS, TOKEN_IDENTIFIER, fname, line);
            setLeft(fa, recv);
            setRight(fa, mkNode(AST_VARIABLE, TOKEN_IDENTIFIER, fname, line));
            free(fname);
            fa->var_type = n->var_type;
            return fa;
        }
        case TCAST_ADDR: {
            AST *a = mkBare(AST_ADDR_OF);
            setLeft(a, xExpr(n->left));
            a->var_type = TYPE_POINTER;
            return a;
        }
        case TCAST_DEREF: {
            AST *d = mkBare(AST_DEREFERENCE);
            setLeft(d, xExpr(n->left));
            d->var_type = n->var_type;
            return d;
        }
        case TCAST_CALL: {
            char *raw = lexToStr(n->token);
            /* Map C-like builtin spellings to their shared counterparts
             * (strlen->length, itoa->str, exit->halt, remove->erase, ...).
             * scanf has no shared runtime; lower it to readln, which reads
             * into its variable arguments (matches the legacy codegen). */
            const char *canon = (raw && strcasecmp(raw, "scanf") == 0)
                                    ? "readln"
                                    : clikeCanonicalBuiltinName(raw);
            AST *call = mkNode(AST_PROCEDURE_CALL, TOKEN_IDENTIFIER, canon, line);
            /* new()/dispose() take a by-reference variable; C passes &var, so
             * strip the address-of to hand the shared compiler an l-value. */
            int byref = raw && (
                strcasecmp(raw, "new") == 0 || strcasecmp(raw, "dispose") == 0 ||
                strcasecmp(raw, "assign") == 0 || strcasecmp(raw, "reset") == 0 ||
                strcasecmp(raw, "rewrite") == 0 || strcasecmp(raw, "append") == 0 ||
                strcasecmp(raw, "close") == 0 || strcasecmp(raw, "rename") == 0 ||
                strcasecmp(raw, "erase") == 0 || strcasecmp(raw, "setlength") == 0 ||
                strcasecmp(raw, "mstreamloadfromfile") == 0 ||
                strcasecmp(raw, "mstreamsavetofile") == 0 ||
                strcasecmp(raw, "mstreamfree") == 0);
            for (int i = 0; i < n->child_count; i++) {
                ASTNodeClike *arg = n->children[i];
                if (byref && i == 0 && arg && arg->type == TCAST_ADDR && arg->left) {
                    addChild(call, xExpr(arg->left)); /* by-ref param: pass l-value */
                } else {
                    addChild(call, xExpr(arg));
                }
            }
            free(raw);
            call->var_type = n->var_type;
            return call;
        }
        case TCAST_THREAD_SPAWN: {
            AST *inner = xExpr(n->left);
            return newThreadSpawn(inner);
        }
        case TCAST_THREAD_JOIN: {
            AST *inner = xExpr(n->left);
            return newThreadJoin(inner);
        }
        default:
            return NULL;
    }
}

/* Wrap an expression as an expression-statement-ish node. The shared compiler
 * accepts AST_PROCEDURE_CALL / AST_ASSIGN directly in statement position. */
static AST *exprAsStmt(ASTNodeClike *e) {
    return xExpr(e);
}

/* ------------------------------------------------------------------ */
/* Statements                                                         */
/* ------------------------------------------------------------------ */

static AST *xVarDecl(ASTNodeClike *n, int isGlobal) {
    AST *decl = mkBare(AST_VAR_DECL);
    char *name = lexToStr(n->token);
    AST *nameVar = mkNode(AST_VARIABLE, TOKEN_IDENTIFIER, name, n->token.line);
    nameVar->var_type = n->var_type;
    free(name);
    addChild(decl, nameVar);
    setRight(decl, xTypeNode(n, n->token.line));
    if (n->left) setLeft(decl, xExpr(n->left)); /* initializer */
    decl->var_type = n->var_type;
    if (isGlobal) decl->is_global_scope = 1;
    return decl;
}

static AST *xCompound(ASTNodeClike *n) {
    AST *c = mkBare(AST_COMPOUND);
    if (n) {
        for (int i = 0; i < n->child_count; i++) {
            AST *s = xStmt(n->children[i]);
            if (s) addChild(c, s);
        }
    }
    return c;
}

static AST *xStmt(ASTNodeClike *n) {
    if (!n) return NULL;
    int line = n->token.line;

    switch (n->type) {
        case TCAST_COMPOUND:
            return xCompound(n);
        case TCAST_VAR_DECL:
            return xVarDecl(n, 0);
        case TCAST_EXPR_STMT:
            return exprAsStmt(n->left);
        case TCAST_IF: {
            AST *node = mkBare(AST_IF);
            setLeft(node, xExpr(n->left));
            setRight(node, xStmt(n->right));
            if (n->third) setExtra(node, xStmt(n->third));
            return node;
        }
        case TCAST_WHILE: {
            AST *node = mkBare(AST_WHILE);
            setLeft(node, xExpr(n->left));
            setRight(node, xStmt(n->right));
            return node;
        }
        case TCAST_DO_WHILE: {
            /* do body while(cond)  ==>  repeat body until !(cond) */
            AST *node = mkBare(AST_REPEAT);
            setLeft(node, xStmt(n->right)); /* body */
            AST *notc = mkNode(AST_UNARY_OP, TOKEN_NOT, "not", line);
            setLeft(notc, xExpr(n->left));  /* cond */
            notc->var_type = TYPE_BOOLEAN;
            setRight(node, notc);
            return node;
        }
        case TCAST_FOR: {
            /* for(init; cond; post) body  ==>  { init; while(cond){ body; post; } } */
            AST *outer = mkBare(AST_COMPOUND);
            if (n->left) {
                if (n->left->type == TCAST_COMPOUND) {
                    for (int i = 0; i < n->left->child_count; i++) {
                        AST *s = xStmt(n->left->children[i]);
                        if (s) addChild(outer, s);
                    }
                } else if (n->left->type == TCAST_VAR_DECL) {
                    addChild(outer, xStmt(n->left));
                } else {
                    AST *s = xExpr(n->left);
                    if (s) addChild(outer, s);
                }
            }
            AST *w = mkBare(AST_WHILE);
            if (n->right) {
                setLeft(w, xExpr(n->right));
            } else {
                AST *t = mkNode(AST_BOOLEAN, TOKEN_TRUE, "true", line);
                t->var_type = TYPE_BOOLEAN; t->i_val = 1;
                setLeft(w, t);
            }
            AST *body = mkBare(AST_COMPOUND);
            ASTNodeClike *clikeBody = n->child_count > 0 ? n->children[0] : NULL;
            if (clikeBody) {
                AST *bs = xStmt(clikeBody);
                if (bs) addChild(body, bs);
            }
            if (n->third) {
                AST *post = xExpr(n->third);
                if (post) addChild(body, post);
            }
            setRight(w, body);
            addChild(outer, w);
            return outer;
        }
        case TCAST_SWITCH: {
            AST *node = mkBare(AST_CASE);
            setLeft(node, xExpr(n->left));
            for (int i = 0; i < n->child_count; i++) {
                ASTNodeClike *br = n->children[i];
                AST *branch = mkBare(AST_CASE_BRANCH);
                setLeft(branch, xExpr(br->left)); /* label */
                AST *body = mkBare(AST_COMPOUND);
                for (int j = 0; j < br->child_count; j++) {
                    /* AST_CASE auto-breaks, so drop the C break that exits it. */
                    if (br->children[j] && br->children[j]->type == TCAST_BREAK) continue;
                    AST *s = xStmt(br->children[j]);
                    if (s) addChild(body, s);
                }
                setRight(branch, body);
                addChild(node, branch);
            }
            if (n->right) {
                if (n->right->type == TCAST_COMPOUND) {
                    AST *def = mkBare(AST_COMPOUND);
                    for (int j = 0; j < n->right->child_count; j++) {
                        if (n->right->children[j] &&
                            n->right->children[j]->type == TCAST_BREAK) continue;
                        AST *s = xStmt(n->right->children[j]);
                        if (s) addChild(def, s);
                    }
                    setExtra(node, def); /* default */
                } else if (n->right->type != TCAST_BREAK) {
                    setExtra(node, xStmt(n->right)); /* default */
                }
            }
            return node;
        }
        case TCAST_BREAK:
            return mkNode(AST_BREAK, TOKEN_BREAK, "break", line);
        case TCAST_CONTINUE:
            return mkNode(AST_CONTINUE, TOKEN_CONTINUE, "continue", line);
        case TCAST_RETURN: {
            AST *node = mkNode(AST_RETURN, TOKEN_RETURN, "return", line);
            if (n->left) setLeft(node, xExpr(n->left));
            return node;
        }
        case TCAST_THREAD_JOIN:
            return xExpr(n);
        default:
            /* Any other node used in statement position: translate as expr. */
            return xExpr(n);
    }
}

/* ------------------------------------------------------------------ */
/* Declarations                                                       */
/* ------------------------------------------------------------------ */

/* A CLike parameter stores its type-name identifier in `left` (a var decl uses
 * `right`), so it needs its own type-node builder. */
static AST *paramTypeNode(ASTNodeClike *p) {
    int line = p->token.line;
    ASTNodeClike *typeIdent = p->left;
    if (p->var_type == TYPE_POINTER) {
        AST *ptr = mkBare(AST_POINTER_TYPE);
        ptr->var_type = TYPE_POINTER;
        VarType base = p->element_type != TYPE_UNKNOWN ? p->element_type : TYPE_VOID;
        setRight(ptr, scalarTypeNode(base, typeIdent, line));
        return ptr;
    }
    return scalarTypeNode(p->var_type, typeIdent, line);
}

static AST *xFunction(ASTNodeClike *f) {
    int isVoid = (f->var_type == TYPE_VOID);
    char *name = lexToStr(f->token);
    AST *fn = mkNode(isVoid ? AST_PROCEDURE_DECL : AST_FUNCTION_DECL,
                     TOKEN_IDENTIFIER, name, f->token.line);
    free(name);
    fn->var_type = f->var_type;

    /* Parameters -> children[] */
    if (f->left) {
        for (int i = 0; i < f->left->child_count; i++) {
            ASTNodeClike *p = f->left->children[i];
            AST *pd = mkBare(AST_VAR_DECL);
            char *pname = lexToStr(p->token);
            AST *pv = mkNode(AST_VARIABLE, TOKEN_IDENTIFIER, pname, p->token.line);
            pv->var_type = p->var_type;
            free(pname);
            addChild(pd, pv);
            setRight(pd, paramTypeNode(p));
            pd->var_type = p->var_type;
            addChild(fn, pd);
        }
    }

    AST *body = xStmt(f->right);
    if (isVoid) {
        setRight(fn, body);
    } else {
        setRight(fn, xTypeNode(f, f->token.line)); /* return type */
        setExtra(fn, body);
    }
    return fn;
}

/* struct S { ... }  ->  AST_TYPE_DECL(name, AST_RECORD_TYPE{fields}) */
static AST *xStruct(ASTNodeClike *s) {
    char *name = lexToStr(s->token);
    AST *td = mkNode(AST_TYPE_DECL, TOKEN_IDENTIFIER, name, s->token.line);
    free(name);
    AST *rec = mkBare(AST_RECORD_TYPE);
    rec->var_type = TYPE_RECORD;
    for (int i = 0; i < s->child_count; i++) {
        ASTNodeClike *f = s->children[i];
        if (!f || f->type != TCAST_VAR_DECL) continue;
        AST *fd = mkBare(AST_VAR_DECL);
        char *fn = lexToStr(f->token);
        AST *fv = mkNode(AST_VARIABLE, TOKEN_IDENTIFIER, fn, f->token.line);
        fv->var_type = f->var_type;
        free(fn);
        addChild(fd, fv);
        setRight(fd, xTypeNode(f, f->token.line));
        fd->var_type = f->var_type;
        addChild(rec, fd);
    }
    setLeft(td, rec);
    td->var_type = TYPE_RECORD;
    return td;
}

/* Append a translated top-level declaration list to `decls`. */
static void translateModuleDecls(ASTNodeClike *mod, AST *decls) {
    if (!mod) return;
    for (int j = 0; j < mod->child_count; j++) {
        ASTNodeClike *d = mod->children[j];
        if (!d) continue;
        if (d->type == TCAST_STRUCT_DECL) addChild(decls, xStruct(d));
        else if (d->type == TCAST_VAR_DECL) addChild(decls, xVarDecl(d, 1));
        else if (d->type == TCAST_FUN_DECL) addChild(decls, xFunction(d));
    }
}

/* Load `import "..."` modules (parsed + analyzed) and fold their declarations
 * into the shared decl list, mirroring the legacy codegen's module loading. */
static void translateImports(AST *decls) {
    for (int i = 0; i < clike_import_count; i++) {
        const char *orig = clike_imports[i];
        if (!orig) continue;
        char *alloc = NULL;
        FILE *f = fopen(orig, "rb");
        if (!f) {
            const char *libdir = getenv("CLIKE_LIB_DIR");
            if (libdir && *libdir) {
                size_t len = strlen(libdir) + 1 + strlen(orig) + 1;
                alloc = (char *)malloc(len);
                if (alloc) {
                    snprintf(alloc, len, "%s/%s", libdir, orig);
                    f = fopen(alloc, "rb");
                    if (!f) { free(alloc); alloc = NULL; }
                }
            }
        }
        if (!f) {
            const char *def = PSCAL_CLIKE_LIB_DIR;
            size_t len = strlen(def) + 1 + strlen(orig) + 1;
            alloc = (char *)malloc(len);
            if (alloc) {
                snprintf(alloc, len, "%s/%s", def, orig);
                f = fopen(alloc, "rb");
                if (!f) { free(alloc); alloc = NULL; }
            }
        }
        if (!f) {
            fprintf(stderr, "Could not open import '%s'\n", orig);
            continue;
        }
        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        rewind(f);
        char *src = (len >= 0) ? (char *)malloc((size_t)len + 1) : NULL;
        if (!src) { fclose(f); if (alloc) free(alloc); continue; }
        size_t rd = fread(src, 1, (size_t)len, f);
        fclose(f);
        if (rd != (size_t)len) { free(src); if (alloc) free(alloc); continue; }
        src[len] = '\0';

        ParserClike pp;
        initParserClike(&pp, src);
        ASTNodeClike *mod = parseProgramClike(&pp);
        freeParserClike(&pp);
        analyzeSemanticsClike(mod, orig);
        translateModuleDecls(mod, decls);
        /* mod/src/alloc intentionally leaked: one-shot compilation. */
    }
}

/* ------------------------------------------------------------------ */
/* Program root                                                       */
/* ------------------------------------------------------------------ */

AST *translateClikeToShared(ASTNodeClike *program) {
    if (!program) return NULL;

    AST *root = mkNode(AST_PROGRAM, TOKEN_IDENTIFIER, "program", 0);
    AST *block = mkBare(AST_BLOCK);
    AST *decls = mkBare(AST_COMPOUND);
    AST *stmts = mkBare(AST_COMPOUND);
    setRight(root, block);
    addChild(block, decls);
    addChild(block, stmts);

    /* Imported modules contribute their globals/functions first. */
    translateImports(decls);

    /* Type declarations first so later decls/bodies can resolve them. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNodeClike *d = program->children[i];
        if (d->type == TCAST_STRUCT_DECL) {
            addChild(decls, xStruct(d));
        }
    }
    /* Globals next (so initializers run before main), then functions. */
    for (int i = 0; i < program->child_count; i++) {
        ASTNodeClike *d = program->children[i];
        if (d->type == TCAST_VAR_DECL) {
            addChild(decls, xVarDecl(d, 1));
        }
    }
    for (int i = 0; i < program->child_count; i++) {
        ASTNodeClike *d = program->children[i];
        if (d->type == TCAST_FUN_DECL) {
            addChild(decls, xFunction(d));
        }
    }

    /* Program body: call main(). */
    AST *callMain = mkNode(AST_PROCEDURE_CALL, TOKEN_IDENTIFIER, "main", 0);
    addChild(stmts, callMain);

    return root;
}
