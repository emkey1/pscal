#include "rea/parser.h"
#include "ast/ast.h"
#include "core/types.h"
#include "symbol/symbol.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "core/list.h"

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);
// Provided by front-end stubs for Rea
void insertType(const char* name, AST* typeDef);

typedef struct {
    ReaLexer lexer;
    ReaToken current;
    VarType currentFunctionType;
    bool hadError;
    const char* currentClassName; // non-owning pointer to current class name while parsing class body
    const char* currentParentClassName; // non-owning pointer to current parent class name while in class body
} ReaParser;

static void reaAdvance(ReaParser *p) { p->current = reaNextToken(&p->lexer); }

static AST *parseExpression(ReaParser *p);
static AST *parseAssignment(ReaParser *p);
static AST *parseEquality(ReaParser *p);
static AST *parseComparison(ReaParser *p);
static AST *parseAdditive(ReaParser *p);
static AST *parseTerm(ReaParser *p);
static AST *parseFactor(ReaParser *p);
static AST *parseLogicalAnd(ReaParser *p);
static AST *parseLogicalOr(ReaParser *p);
static AST *parseStatement(ReaParser *p);
static AST *parseVarDecl(ReaParser *p);
static AST *parseReturn(ReaParser *p);
static AST *parseIf(ReaParser *p);
static AST *parseBlock(ReaParser *p);
static AST *parseFunctionDecl(ReaParser *p, Token *nameTok, AST *typeNode, VarType vtype);
static AST *parseWhile(ReaParser *p);
static AST *parseDoWhile(ReaParser *p);
static AST *parseFor(ReaParser *p);
static AST *parseSwitch(ReaParser *p);
static AST *parseConstDecl(ReaParser *p);
static AST *parseImport(ReaParser *p);

// Access to global type table provided by Pascal front end
AST *lookupType(const char* name);

// Helper to rewrite 'continue' statements to 'post; continue' inside for-loop bodies
static AST *rewriteContinueWithPost(AST *node, AST *postStmt) {
    if (!node) return NULL;
    if (node->type == AST_CONTINUE) {
        AST *comp = newASTNode(AST_COMPOUND, NULL);
        addChild(comp, copyAST(postStmt));
        addChild(comp, newASTNode(AST_CONTINUE, NULL));
        return comp;
    }
    // Recurse
    node->left = rewriteContinueWithPost(node->left, postStmt);
    node->right = rewriteContinueWithPost(node->right, postStmt);
    node->extra = rewriteContinueWithPost(node->extra, postStmt);
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) node->children[i] = rewriteContinueWithPost(node->children[i], postStmt);
    }
    return node;
}

// Unescape standard escape sequences within a string literal segment
static char *reaUnescapeString(const char *src, size_t len, size_t *out_len) {
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        char c = src[i];
        if (c == '\\' && i + 1 < len) {
            char n = src[++i];
            switch (n) {
                case 'n': buf[j++] = '\n'; break;
                case 'r': buf[j++] = '\r'; break;
                case 't': buf[j++] = '\t'; break;
                case '\\': buf[j++] = '\\'; break;
                case 0x27: buf[j++] = 0x27; break;
                case '"': buf[j++] = '"'; break;
                default: buf[j++] = n; break;
            }
        } else {
            buf[j++] = c;
        }
    }
    buf[j] = '\0';
    if (out_len) *out_len = j;
    return buf;
}

static AST *parseFactor(ReaParser *p) {
    if (p->current.type == REA_TOKEN_SUPER) {
        // super(...) or super.method(...)
        ReaToken supTok = p->current;
        reaAdvance(p); // consume 'super'
        if (!p->currentParentClassName) {
            // Outside of a subclass context; treat as error-like no-op
            return NULL;
        }
        // Case: super(args)
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p);
            AST *args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(args, arg);
                if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
            // Build call to parent constructor alias: ParentName(this, ...)
            Token *ctorTok = newToken(TOKEN_IDENTIFIER, p->currentParentClassName, supTok.line, 0);
            AST *call = newASTNode(AST_PROCEDURE_CALL, ctorTok);
            // Prepend implicit 'this'
            Token *thisTok = newToken(TOKEN_IDENTIFIER, "this", supTok.line, 0);
            AST *thisVar = newASTNode(AST_VARIABLE, thisTok);
            setTypeAST(thisVar, TYPE_POINTER);
            addChild(call, thisVar);
            if (args && args->child_count > 0) {
                for (int i = 0; i < args->child_count; i++) {
                    addChild(call, args->children[i]);
                    args->children[i] = NULL;
                }
                args->child_count = 0;
            }
            if (args) freeAST(args);
            setTypeAST(call, TYPE_UNKNOWN);
            return call;
        }
        // Case: super.method(args)
        if (p->current.type == REA_TOKEN_DOT) {
            reaAdvance(p);
            if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
            char *mlex = (char*)malloc(p->current.length + 1);
            if (!mlex) return NULL;
            memcpy(mlex, p->current.start, p->current.length);
            mlex[p->current.length] = '\0';
            reaAdvance(p); // consume method name
            // Optional args
            AST *args = NULL;
            if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                reaAdvance(p);
                args = newASTNode(AST_COMPOUND, NULL);
                while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                    AST *arg = parseExpression(p);
                    if (!arg) break;
                    addChild(args, arg);
                    if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                }
                if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
            }
            // Build mangled name Parent_Method
            size_t ln = strlen(p->currentParentClassName) + 1 + strlen(mlex) + 1;
            char *mangled = (char*)malloc(ln);
            if (!mangled) { free(mlex); return NULL; }
            snprintf(mangled, ln, "%s_%s", p->currentParentClassName, mlex);
            free(mlex);
            Token *nameTok = newToken(TOKEN_IDENTIFIER, mangled, supTok.line, 0);
            free(mangled);
            AST *call = newASTNode(AST_PROCEDURE_CALL, nameTok);
            // Prepend implicit 'this'
            Token *thisTok = newToken(TOKEN_IDENTIFIER, "this", supTok.line, 0);
            AST *thisVar = newASTNode(AST_VARIABLE, thisTok);
            setTypeAST(thisVar, TYPE_POINTER);
            addChild(call, thisVar);
            if (args && args->child_count > 0) {
                for (int i = 0; i < args->child_count; i++) {
                    addChild(call, args->children[i]);
                    args->children[i] = NULL;
                }
                args->child_count = 0;
            }
            if (args) freeAST(args);
            setTypeAST(call, TYPE_UNKNOWN);
            return call;
        }
        return NULL;
    }
    if (p->current.type == REA_TOKEN_NEW) {
        // new ClassName(args) -> AST_NEW(token=ClassName, children=args)
        reaAdvance(p); // consume 'new'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *clsTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume class name
        AST *args = NULL;
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p);
            args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(args, arg);
                if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
        }
        AST *node = newASTNode(AST_NEW, clsTok);
        if (args && args->child_count > 0) {
            node->children = args->children;
            node->child_count = args->child_count;
            node->child_capacity = args->child_capacity;
            for (int i = 0; i < node->child_count; i++) if (node->children[i]) node->children[i]->parent = node;
            args->children = NULL; args->child_count = 0; args->child_capacity = 0;
        }
        if (args) freeAST(args);
        setTypeAST(node, TYPE_POINTER);
        return node;
    }
    if (p->current.type == REA_TOKEN_NUMBER) {
        size_t len = p->current.length;
        const char *start = p->current.start;
        TokenType ttype = TOKEN_INTEGER_CONST;
        VarType vtype = TYPE_INT64;

        bool is_hex = false;
        if (len > 2 && start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) {
            is_hex = true;
            start += 2;
            len -= 2;
            ttype = TOKEN_HEX_CONST;
        } else {
            for (size_t i = 0; i < len; i++) {
                if (start[i] == '.' || start[i] == 'e' || start[i] == 'E') {
                    ttype = TOKEN_REAL_CONST;
                    vtype = TYPE_DOUBLE;
                    break;
                }
            }
        }

        char *lex = (char *)malloc(len + 1);
        if (!lex) return NULL;
        memcpy(lex, start, len);
        lex[len] = '\0';

        Token *tok = newToken(ttype, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_NUMBER, tok);
        setTypeAST(node, vtype);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_STRING) {
        size_t len = p->current.length;
        if (len < 2) return NULL;
        size_t inner_len = len - 2;
        size_t unesc_len = 0;
        char *lex = reaUnescapeString(p->current.start + 1, inner_len, &unesc_len);
        if (!lex) return NULL;
        Token *tok = newToken(TOKEN_STRING_CONST, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_STRING, tok);
        VarType vtype = (p->current.start[0] == '\'' && unesc_len == 1) ? TYPE_CHAR : TYPE_STRING;
        setTypeAST(node, vtype);
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_TRUE || p->current.type == REA_TOKEN_FALSE) {
        TokenType tt = (p->current.type == REA_TOKEN_TRUE) ? TOKEN_TRUE : TOKEN_FALSE;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(tt, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_BOOLEAN, tok);
        setTypeAST(node, TYPE_BOOLEAN);
        node->i_val = (tt == TOKEN_TRUE) ? 1 : 0;
        reaAdvance(p);
        return node;
    } else if (p->current.type == REA_TOKEN_IDENTIFIER || p->current.type == REA_TOKEN_THIS) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        if (p->current.type == REA_TOKEN_THIS) {
            strcpy(lex, "this");
        } else {
            memcpy(lex, p->current.start, p->current.length);
            lex[p->current.length] = '\0';
        }
        Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume identifier

        AST *call_args = NULL;
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            reaAdvance(p); // consume '('
            call_args = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                AST *arg = parseExpression(p);
                if (!arg) break;
                addChild(call_args, arg);
                if (p->current.type == REA_TOKEN_COMMA) {
                    reaAdvance(p);
                } else {
                    break;
                }
            }
            if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
                reaAdvance(p);
            }
            // Syntactic sugar: map writeln/write to dedicated AST nodes for direct builtin handling
            AST *call = NULL;
            if (tok && tok->value && strcasecmp(tok->value, "writeln") == 0) {
                call = newASTNode(AST_WRITELN, NULL);
            } else if (tok && tok->value && strcasecmp(tok->value, "write") == 0) {
                call = newASTNode(AST_WRITE, NULL);
            } else {
                call = newASTNode(AST_PROCEDURE_CALL, tok);
            }
            if (call_args && call_args->child_count > 0) {
                call->children = call_args->children;
                call->child_count = call_args->child_count;
                call->child_capacity = call_args->child_capacity;
                for (int i = 0; i < call->child_count; i++) {
                    if (call->children[i]) call->children[i]->parent = call;
                }
                call_args->children = NULL;
                call_args->child_count = 0;
                call_args->child_capacity = 0;
            }
            if (call_args) freeAST(call_args);
            setTypeAST(call, TYPE_UNKNOWN);
            // Support member access chaining after call
            AST *node = call;
            while (p->current.type == REA_TOKEN_DOT) {
                reaAdvance(p);
                if (p->current.type != REA_TOKEN_IDENTIFIER) break;
                // Capture method/field name
                char *f = (char*)malloc(p->current.length + 1);
                if (!f) break;
                memcpy(f, p->current.start, p->current.length);
                f[p->current.length] = '\0';
                Token *nameTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
                free(f);
                reaAdvance(p);
                // If this is a qualified method call like receiver.method(...), build a call
                if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                    // Parse call args
                    reaAdvance(p); // consume '('
                    AST *args = newASTNode(AST_COMPOUND, NULL);
                    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                        AST *arg = parseExpression(p);
                        if (!arg) break;
                        addChild(args, arg);
                        if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                    }
                    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
                    // Build call node and prepend receiver as first arg
                    // Potentially mangle if receiver is 'this' or freshly constructed 'new Class'
                    const char* cls = NULL;
                    if (node->type == AST_VARIABLE && node->token && node->token->value && strcasecmp(node->token->value, "this") == 0) {
                        cls = p->currentClassName;
                    } else if (node->type == AST_NEW && node->token && node->token->value) {
                        cls = node->token->value;
                    }
                    if (cls) {
                        size_t ln = strlen(cls) + 1 + strlen(nameTok->value) + 1;
                        char *m = (char*)malloc(ln);
                        if (m) {
                            snprintf(m, ln, "%s_%s", cls, nameTok->value);
                            free(nameTok->value);
                            nameTok->value = m;
                        }
                    }
                    AST *call = newASTNode(AST_PROCEDURE_CALL, nameTok);
                    // Receiver as qualifier and also as first argument
                    setLeft(call, node);
                    addChild(call, node);
                    if (args && args->child_count > 0) {
                        for (int i = 0; i < args->child_count; i++) {
                            addChild(call, args->children[i]);
                            args->children[i] = NULL;
                        }
                        args->child_count = 0;
                    }
                    if (args) freeAST(args);
                    node = call;
                } else {
                    // Simple field access
                    AST *fieldVar = newASTNode(AST_VARIABLE, nameTok);
                    AST *fa = newASTNode(AST_FIELD_ACCESS, nameTok);
                    setLeft(fa, node);
                    setRight(fa, fieldVar);
                    node = fa;
                }
            }
            return node;
        } else {
            AST *node = newASTNode(AST_VARIABLE, tok);
            setTypeAST(node, TYPE_UNKNOWN);
            while (p->current.type == REA_TOKEN_DOT) {
                reaAdvance(p);
                if (p->current.type != REA_TOKEN_IDENTIFIER) break;
                char *f = (char*)malloc(p->current.length + 1);
                if (!f) break;
                memcpy(f, p->current.start, p->current.length);
                f[p->current.length] = '\0';
                Token *fieldTok = newToken(TOKEN_IDENTIFIER, f, p->current.line, 0);
                free(f);
                reaAdvance(p);
                if (p->current.type == REA_TOKEN_LEFT_PAREN) {
                    // Method call on arbitrary receiver: parse args and build call node with receiver first
                    reaAdvance(p); // consume '('
                    AST *args = newASTNode(AST_COMPOUND, NULL);
                    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
                        AST *arg = parseExpression(p);
                        if (!arg) break;
                        addChild(args, arg);
                        if (p->current.type == REA_TOKEN_COMMA) reaAdvance(p); else break;
                    }
                    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
                    AST *call = newASTNode(AST_PROCEDURE_CALL, fieldTok);
                    setLeft(call, node);
                    addChild(call, node);
                    if (args && args->child_count > 0) {
                        for (int i = 0; i < args->child_count; i++) {
                            addChild(call, args->children[i]);
                            args->children[i] = NULL;
                        }
                        args->child_count = 0;
                    }
                    if (args) freeAST(args);
                    node = call;
                } else {
                    AST *fieldVar = newASTNode(AST_VARIABLE, fieldTok);
                    AST *fa = newASTNode(AST_FIELD_ACCESS, fieldTok);
                    setLeft(fa, node);
                    setRight(fa, fieldVar);
                    node = fa;
                }
            }
            return node;
        }
    } else if (p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_MINUS, "-", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, right->var_type);
        return node;
    } else if (p->current.type == REA_TOKEN_BANG) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_NOT, "!", op.line, 0);
        AST *node = newASTNode(AST_UNARY_OP, tok);
        setLeft(node, right);
        setTypeAST(node, TYPE_BOOLEAN);
        return node;
    } else if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
        AST *expr = parseExpression(p);
        if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
            reaAdvance(p);
        }
        return expr;
    }
    return NULL;
}

static TokenType mapOp(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_PLUS: return TOKEN_PLUS;
        case REA_TOKEN_MINUS: return TOKEN_MINUS;
        case REA_TOKEN_STAR: return TOKEN_MUL;
        case REA_TOKEN_SLASH: return TOKEN_SLASH;
        case REA_TOKEN_PERCENT: return TOKEN_MOD;
        case REA_TOKEN_EQUAL_EQUAL: return TOKEN_EQUAL;
        case REA_TOKEN_BANG_EQUAL: return TOKEN_NOT_EQUAL;
        case REA_TOKEN_GREATER: return TOKEN_GREATER;
        case REA_TOKEN_GREATER_EQUAL: return TOKEN_GREATER_EQUAL;
        case REA_TOKEN_LESS: return TOKEN_LESS;
        case REA_TOKEN_LESS_EQUAL: return TOKEN_LESS_EQUAL;
        default: return TOKEN_UNKNOWN;
    }
}

static const char *opLexeme(TokenType t) {
    switch (t) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_MUL: return "*";
        case TOKEN_SLASH: return "/";
        case TOKEN_MOD: return "%";
        case TOKEN_EQUAL: return "==";
        case TOKEN_NOT_EQUAL: return "!=";
        case TOKEN_GREATER: return ">";
        case TOKEN_GREATER_EQUAL: return ">=";
        case TOKEN_LESS: return "<";
        case TOKEN_LESS_EQUAL: return "<=";
        default: return "";
    }
}

static AST *parseTerm(ReaParser *p) {
    AST *node = parseFactor(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_STAR ||
           p->current.type == REA_TOKEN_SLASH ||
           p->current.type == REA_TOKEN_PERCENT) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseFactor(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (tt == TOKEN_SLASH || lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) {
            res = TYPE_DOUBLE;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseAdditive(ReaParser *p) {
    AST *node = parseTerm(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_PLUS || p->current.type == REA_TOKEN_MINUS) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseTerm(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        VarType lt = node->var_type;
        VarType rt = right->var_type;
        VarType res;
        if (lt == TYPE_DOUBLE || rt == TYPE_DOUBLE) {
            res = TYPE_DOUBLE;
        } else if (lt == TYPE_INT64 || rt == TYPE_INT64) {
            res = TYPE_INT64;
        } else {
            res = TYPE_INT32;
        }
        setTypeAST(bin, res);
        node = bin;
    }
    return node;
}

static AST *parseComparison(ReaParser *p) {
    AST *node = parseAdditive(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_GREATER || p->current.type == REA_TOKEN_GREATER_EQUAL ||
           p->current.type == REA_TOKEN_LESS || p->current.type == REA_TOKEN_LESS_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseAdditive(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseEquality(ReaParser *p) {
    AST *node = parseComparison(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_EQUAL_EQUAL || p->current.type == REA_TOKEN_BANG_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseComparison(p);
        if (!right) return NULL;
        TokenType tt = mapOp(op.type);
        Token *tok = newToken(tt, opLexeme(tt), op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseAssignment(ReaParser *p) {
    // Highest precedence: handle assignment right-associatively
    AST *left = parseLogicalOr(p);
    if (!left) return NULL;
    if ((left->type == AST_VARIABLE || left->type == AST_FIELD_ACCESS) && p->current.type == REA_TOKEN_EQUAL) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *value = parseAssignment(p);
        if (!value) return NULL;
        Token *tok = newToken(TOKEN_ASSIGN, "=", op.line, 0);
            AST *node = newASTNode(AST_ASSIGN, tok);
            setLeft(node, left);
            setRight(node, value);
            setTypeAST(node, left->var_type);
            return node;
    }
    return left;
}

static AST *parseExpression(ReaParser *p) {
    return parseAssignment(p);
}

static AST *parseLogicalAnd(ReaParser *p) {
    AST *node = parseEquality(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_AND_AND) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseEquality(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_AND, "&&", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static AST *parseLogicalOr(ReaParser *p) {
    AST *node = parseLogicalAnd(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_OR_OR) {
        ReaToken op = p->current;
        reaAdvance(p);
        AST *right = parseLogicalAnd(p);
        if (!right) return NULL;
        Token *tok = newToken(TOKEN_OR, "||", op.line, 0);
        AST *bin = newASTNode(AST_BINARY_OP, tok);
        setLeft(bin, node);
        setRight(bin, right);
        setTypeAST(bin, TYPE_BOOLEAN);
        node = bin;
    }
    return node;
}

static VarType mapType(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT:
        case REA_TOKEN_INT64: return TYPE_INT64;
        case REA_TOKEN_INT32: return TYPE_INT32;
        case REA_TOKEN_INT16: return TYPE_INT16;
        case REA_TOKEN_INT8:  return TYPE_INT8;
        case REA_TOKEN_FLOAT: return TYPE_DOUBLE;
        case REA_TOKEN_FLOAT32: return TYPE_FLOAT;
        case REA_TOKEN_LONG_DOUBLE: return TYPE_LONG_DOUBLE;
        case REA_TOKEN_CHAR: return TYPE_CHAR;
        case REA_TOKEN_BYTE: return TYPE_BYTE;
        case REA_TOKEN_STR: return TYPE_STRING;
        case REA_TOKEN_TEXT: return TYPE_FILE;
        case REA_TOKEN_MSTREAM: return TYPE_MEMORYSTREAM;
        case REA_TOKEN_BOOL: return TYPE_BOOLEAN;
        case REA_TOKEN_VOID: return TYPE_VOID;
        default: return TYPE_VOID;
    }
}

static const char *typeName(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT: return "int";
        case REA_TOKEN_INT64: return "int64";
        case REA_TOKEN_INT32: return "int32";
        case REA_TOKEN_INT16: return "int16";
        case REA_TOKEN_INT8: return "int8";
        case REA_TOKEN_FLOAT: return "float";
        case REA_TOKEN_FLOAT32: return "float32";
        case REA_TOKEN_LONG_DOUBLE: return "longdouble";
        case REA_TOKEN_CHAR: return "char";
        case REA_TOKEN_BYTE: return "byte";
        case REA_TOKEN_STR: return "str";
        case REA_TOKEN_TEXT: return "text";
        case REA_TOKEN_MSTREAM: return "mstream";
        case REA_TOKEN_BOOL: return "bool";
        case REA_TOKEN_VOID: return "void";
        default: return "";
    }
}

static AST *parseVarDecl(ReaParser *p) {
    // Allow constructor shorthand: inside a class, the constructor may omit
    // the return type and start directly with the class name, e.g.:
    //   ClassName(arg1, arg2) { ... }
    // Detect this before normal type parsing by peeking at the next
    // non-whitespace character. If it is an opening parenthesis and the
    // identifier matches the current class name, treat it as a constructor
    // with an implicit void return type.
    if (p->current.type == REA_TOKEN_IDENTIFIER && p->currentClassName) {
        size_t len = p->current.length;
        if (strlen(p->currentClassName) == len &&
            strncasecmp(p->current.start, p->currentClassName, len) == 0) {
            size_t look = p->lexer.pos;
            const char *src = p->lexer.source;
            while (src[look] == ' ' || src[look] == '\t' ||
                   src[look] == '\r' || src[look] == '\n') {
                look++;
            }
            if (src[look] == '(') {
                char *lex = (char *)malloc(len + 1);
                if (!lex) return NULL;
                memcpy(lex, p->current.start, len);
                lex[len] = '\0';
                Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
                free(lex);
                reaAdvance(p); // consume constructor name
                return parseFunctionDecl(p, nameTok, NULL, TYPE_VOID);
            }
        }
    }

    ReaTokenType typeTok = p->current.type;
    VarType vtype = mapType(typeTok);

    AST *typeNode = NULL;
    if (typeTok == REA_TOKEN_VOID) {
        // Procedures: void name(...)
        reaAdvance(p); // consume 'void'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume identifier
        if (p->current.type == REA_TOKEN_LEFT_PAREN) {
            return parseFunctionDecl(p, nameTok, NULL, TYPE_VOID);
        } else {
            fprintf(stderr, "L%d: Variable cannot have type void.\n", p->current.line);
            p->hadError = true;
            return NULL;
        }
    } else if (vtype != TYPE_VOID) {
        // Built-in type keyword
        const char *tname = typeName(typeTok);
        Token *typeToken = newToken(TOKEN_IDENTIFIER, tname, p->current.line, 0);
        typeNode = newASTNode(AST_TYPE_IDENTIFIER, typeToken);
        setTypeAST(typeNode, vtype);
        reaAdvance(p); // consume type keyword
    } else if (p->current.type == REA_TOKEN_IDENTIFIER) {
        // User-defined type (e.g., class name). Treat vars as POINTER to that type.
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *typeRefTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        typeNode = newASTNode(AST_TYPE_REFERENCE, typeRefTok);
        setTypeAST(typeNode, TYPE_RECORD);
        vtype = TYPE_POINTER; // store pointers to class instances by default
        reaAdvance(p); // consume type identifier
    } else {
        return NULL;
    }

    if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;

    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);

    reaAdvance(p); // consume identifier

    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        return parseFunctionDecl(p, nameTok, typeNode, vtype);
    }

    AST *var = newASTNode(AST_VARIABLE, nameTok);
    setTypeAST(var, vtype);

    AST *init = NULL;
    if (p->current.type == REA_TOKEN_EQUAL) {
        reaAdvance(p);
        init = parseExpression(p);
    }

    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }

    AST *decl = newASTNode(AST_VAR_DECL, NULL);
    addChild(decl, var);
    setLeft(decl, init);
    setRight(decl, typeNode);
    setTypeAST(decl, vtype);
    return decl;
}

static AST *parseFunctionDecl(ReaParser *p, Token *nameTok, AST *typeNode, VarType vtype) {
    VarType prevType = p->currentFunctionType;
    p->currentFunctionType = vtype;

    // If inside a class, mangle function name to ClassName_method
    if (p->currentClassName && nameTok && nameTok->value) {
        size_t ln = strlen(p->currentClassName) + 1 + strlen(nameTok->value) + 1;
        char *m = (char*)malloc(ln);
        if (m) {
            snprintf(m, ln, "%s_%s", p->currentClassName, nameTok->value);
            free(nameTok->value);
            nameTok->value = m;
        }
    }

    // Parse parameter list
    reaAdvance(p); // consume '('
    AST *params = newASTNode(AST_COMPOUND, NULL);
    // Inject implicit 'this' parameter as first parameter when inside a class
    if (p->currentClassName) {
        Token *ptypeTok = newToken(TOKEN_IDENTIFIER, p->currentClassName, p->current.line, 0);
        AST *ptypeNode = newASTNode(AST_TYPE_REFERENCE, ptypeTok);
        setTypeAST(ptypeNode, TYPE_RECORD);
        Token *thisTok = newToken(TOKEN_IDENTIFIER, "this", p->current.line, 0);
        AST *thisVar = newASTNode(AST_VARIABLE, thisTok);
        setTypeAST(thisVar, TYPE_POINTER);
        AST *thisDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(thisDecl, thisVar);
        setRight(thisDecl, ptypeNode);
        setTypeAST(thisDecl, TYPE_POINTER);
        addChild(params, thisDecl);
    }
    while (p->current.type != REA_TOKEN_RIGHT_PAREN && p->current.type != REA_TOKEN_EOF) {
        ReaTokenType paramTypeTok = p->current.type;
        VarType pvtype = mapType(paramTypeTok);
        const char *ptname = typeName(paramTypeTok);
        Token *ptypeTok = newToken(TOKEN_IDENTIFIER, ptname, p->current.line, 0);
        AST *ptypeNode = newASTNode(AST_TYPE_IDENTIFIER, ptypeTok);
        setTypeAST(ptypeNode, pvtype);
        reaAdvance(p); // consume param type

        if (p->current.type != REA_TOKEN_IDENTIFIER) break;
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) break;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *paramNameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        AST *paramVar = newASTNode(AST_VARIABLE, paramNameTok);
        setTypeAST(paramVar, pvtype);
        reaAdvance(p); // consume param name

        AST *paramDecl = newASTNode(AST_VAR_DECL, NULL);
        addChild(paramDecl, paramVar);
        setRight(paramDecl, ptypeNode);
        setTypeAST(paramDecl, pvtype);
        addChild(params, paramDecl);

        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
        } else {
            break;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    // Parse function body
    AST *block = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        reaAdvance(p); // consume '{'
        AST *decls = newASTNode(AST_COMPOUND, NULL);
        AST *stmts = newASTNode(AST_COMPOUND, NULL);
        while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
            AST *stmt = parseStatement(p);
            if (!stmt) break;
            if (stmt->type == AST_VAR_DECL) {
                addChild(decls, stmt);
            } else {
                addChild(stmts, stmt);
            }
        }
        if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
            reaAdvance(p);
        }
        block = newASTNode(AST_BLOCK, NULL);
        addChild(block, decls);
        addChild(block, stmts);
    }

    AST *func = (vtype == TYPE_VOID) ? newASTNode(AST_PROCEDURE_DECL, nameTok)
                                     : newASTNode(AST_FUNCTION_DECL, nameTok);
    if (params->child_count > 0) {
        func->children = params->children;
        func->child_count = params->child_count;
        func->child_capacity = params->child_capacity;
        for (int i = 0; i < func->child_count; i++) {
            if (func->children[i]) func->children[i]->parent = func;
        }
        params->children = NULL;
        params->child_count = 0;
        params->child_capacity = 0;
    }
    freeAST(params);
    if (vtype == TYPE_VOID) {
        // Procedures carry body in right; no return type node
        setRight(func, block);
    } else {
        setRight(func, typeNode);
        setExtra(func, block);
    }
    setTypeAST(func, vtype);

    // Register function in procedure table
    Symbol *sym = (Symbol*)malloc(sizeof(Symbol));
    if (sym) {
        memset(sym, 0, sizeof(Symbol));
        sym->name = strdup(nameTok->value);
        if (sym->name) {
            for (int i = 0; sym->name[i]; i++) {
                sym->name[i] = tolower((unsigned char)sym->name[i]);
            }
        }
        sym->type = vtype;
        sym->type_def = copyAST(func);
        hashTableInsert(procedure_table, sym);
    }

    // If inside a class, also add a bare-name alias so 'obj.method(...)' can resolve.
    if (p->currentClassName && sym && sym->name) {
        const char* us2 = strchr(sym->name, '_');
        const char* bare = NULL;
        if (us2 && *(us2+1)) bare = us2 + 1;
        if (bare) {
            Symbol* alias2 = (Symbol*)malloc(sizeof(Symbol));
            if (alias2) {
                memset(alias2, 0, sizeof(Symbol));
                alias2->name = strdup(bare);
                // already lower case since sym->name is lower
                alias2->is_alias = true;
                alias2->real_symbol = sym;
                alias2->type = vtype;
                alias2->type_def = copyAST(sym->type_def);
                hashTableInsert(procedure_table, alias2);
            }
        }
    }

    // If this is a constructor (method name equals class name), add alias 'ClassName' -> real symbol
    if (p->currentClassName && nameTok && nameTok->value) {
        const char* us = strchr(nameTok->value, '_');
        if (us) {
            // Check if name is ClassName_ClassName
            size_t cls_len = (size_t)(us - nameTok->value);
            if (strlen(p->currentClassName) == cls_len && strncasecmp(nameTok->value, p->currentClassName, cls_len) == 0) {
                const char* after = us + 1;
                if (strncasecmp(after, p->currentClassName, cls_len) == 0 && after[cls_len] == '\0') {
                    Symbol* alias = (Symbol*)malloc(sizeof(Symbol));
                    if (alias) {
                        memset(alias, 0, sizeof(Symbol));
                        alias->name = strdup(p->currentClassName);
                        if (alias->name) {
                            for (int i = 0; alias->name[i]; i++) alias->name[i] = tolower((unsigned char)alias->name[i]);
                        }
                        alias->is_alias = true;
                        alias->real_symbol = sym;
                        alias->type = vtype;
                        alias->type_def = sym ? copyAST(sym->type_def) : NULL;
                        hashTableInsert(procedure_table, alias);
                    }
                }
            }
        }
    }

    p->currentFunctionType = prevType;
    return func;
}

static AST *parseBlock(ReaParser *p) {
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    reaAdvance(p); // consume '{'
    AST *block = newASTNode(AST_COMPOUND, NULL);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(p);
        if (!stmt) break;
        addChild(block, stmt);
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) {
        reaAdvance(p);
    }
    return block;
}

static AST *parseReturn(ReaParser *p) {
    ReaToken ret = p->current;
    reaAdvance(p); // consume 'return'
    AST *value = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON) {
        value = parseExpression(p);
    } else if (p->currentFunctionType != TYPE_VOID) {
        fprintf(stderr, "L%d: return requires a value.\n", ret.line);
        p->hadError = true;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) {
        reaAdvance(p);
    }
    if (p->hadError) return NULL;
    Token *retTok = newToken(TOKEN_RETURN, "return", ret.line, 0);
    AST *node = newASTNode(AST_RETURN, retTok);
    setLeft(node, value);
    if (value) setTypeAST(node, value->var_type); else setTypeAST(node, TYPE_VOID);
    return node;
}

static AST *parseIf(ReaParser *p) {
    reaAdvance(p); // consume 'if'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) {
        reaAdvance(p);
    }
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) {
        reaAdvance(p);
    }

    AST *thenBranch = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        thenBranch = parseBlock(p);
    } else {
        thenBranch = parseStatement(p);
    }

    AST *elseBranch = NULL;
    if (p->current.type == REA_TOKEN_ELSE) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            elseBranch = parseBlock(p);
        } else {
            elseBranch = parseStatement(p);
        }
    }

    AST *node = newASTNode(AST_IF, NULL);
    setLeft(node, condition);
    setRight(node, thenBranch);
    setExtra(node, elseBranch);
    return node;
}

static AST *parseWhile(ReaParser *p) {
    reaAdvance(p); // consume 'while'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    AST *condition = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) body = parseBlock(p);
    else body = parseStatement(p);

    AST *node = newASTNode(AST_WHILE, NULL);
    setLeft(node, condition);
    setRight(node, body);
    return node;
}

static AST *parseDoWhile(ReaParser *p) {
    reaAdvance(p); // consume 'do'
    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) body = parseBlock(p);
    else body = parseStatement(p);
    if (p->current.type == REA_TOKEN_WHILE) reaAdvance(p);
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    AST *cond = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    // Translate do { body } while (cond);  ==>  REPEAT body UNTIL !cond
    Token *notTok = newToken(TOKEN_NOT, "!", cond ? (cond->token ? cond->token->line : 0) : 0, 0);
    AST *notCond = newASTNode(AST_UNARY_OP, notTok);
    setLeft(notCond, cond);
    setTypeAST(notCond, TYPE_BOOLEAN);
    AST *node = newASTNode(AST_REPEAT, NULL);
    setLeft(node, body);
    setRight(node, notCond);
    return node;
}

static AST *parseFor(ReaParser *p) {
    reaAdvance(p); // consume 'for'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    // init (may be var decl or expr) and ends with ';'
    AST *init = NULL;
    if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_INT64 ||
        p->current.type == REA_TOKEN_INT32 || p->current.type == REA_TOKEN_INT16 ||
        p->current.type == REA_TOKEN_INT8 || p->current.type == REA_TOKEN_FLOAT ||
        p->current.type == REA_TOKEN_FLOAT32 || p->current.type == REA_TOKEN_LONG_DOUBLE ||
        p->current.type == REA_TOKEN_CHAR || p->current.type == REA_TOKEN_BYTE ||
        p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_TEXT ||
        p->current.type == REA_TOKEN_MSTREAM || p->current.type == REA_TOKEN_BOOL) {
        init = parseVarDecl(p);
    } else if (p->current.type != REA_TOKEN_SEMICOLON) {
        AST *ie = parseExpression(p);
        if (ie) {
            AST *stmt = newASTNode(AST_EXPR_STMT, ie->token);
            setLeft(stmt, ie);
            init = stmt;
        }
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);

    // condition (optional)
    AST *cond = NULL;
    if (p->current.type != REA_TOKEN_SEMICOLON) {
        cond = parseExpression(p);
    } else {
        Token *tt = newToken(TOKEN_TRUE, "true", p->current.line, 0);
        AST *b = newASTNode(AST_BOOLEAN, tt);
        b->i_val = 1;
        setTypeAST(b, TYPE_BOOLEAN);
        cond = b;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);

    // post (optional) until ')'
    AST *post = NULL;
    if (p->current.type != REA_TOKEN_RIGHT_PAREN) {
        AST *pe = parseExpression(p);
        if (pe) {
            AST *stmt = newASTNode(AST_EXPR_STMT, pe->token);
            setLeft(stmt, pe);
            post = stmt;
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);

    AST *body = NULL;
    if (p->current.type == REA_TOKEN_LEFT_BRACE) body = parseBlock(p);
    else body = parseStatement(p);

    if (post) {
        body = rewriteContinueWithPost(body, post);
    }

    AST *whileBody = NULL;
    if (post) {
        whileBody = newASTNode(AST_COMPOUND, NULL);
        addChild(whileBody, body);
        addChild(whileBody, post);
    } else {
        whileBody = body;
    }
    AST *whileNode = newASTNode(AST_WHILE, NULL);
    setLeft(whileNode, cond);
    setRight(whileNode, whileBody);

    if (init) {
        AST *outer = newASTNode(AST_COMPOUND, NULL);
        addChild(outer, init);
        addChild(outer, whileNode);
        return outer;
    } else {
        return whileNode;
    }
}

static AST *parseSwitch(ReaParser *p) {
    reaAdvance(p); // consume 'switch'
    if (p->current.type == REA_TOKEN_LEFT_PAREN) reaAdvance(p);
    AST *expr = parseExpression(p);
    if (p->current.type == REA_TOKEN_RIGHT_PAREN) reaAdvance(p);
    if (p->current.type != REA_TOKEN_LEFT_BRACE) return NULL;
    reaAdvance(p); // consume '{'
    AST *node = newASTNode(AST_CASE, NULL);
    setLeft(node, expr);
    while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
        if (p->current.type == REA_TOKEN_CASE) {
            reaAdvance(p);
            AST *labels = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_COLON && p->current.type != REA_TOKEN_EOF) {
                AST *lbl = parseExpression(p);
                if (!lbl) break;
                addChild(labels, lbl);
                if (p->current.type == REA_TOKEN_COMMA) { reaAdvance(p); continue; }
                else break;
            }
            if (p->current.type == REA_TOKEN_COLON) reaAdvance(p);
            AST *body = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_BRACE &&
                   p->current.type != REA_TOKEN_CASE &&
                   p->current.type != REA_TOKEN_DEFAULT &&
                   p->current.type != REA_TOKEN_EOF) {
                if (p->current.type == REA_TOKEN_BREAK) {
                    reaAdvance(p);
                    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
                    break;
                }
                AST *s = parseStatement(p);
                if (!s) break;
                addChild(body, s);
            }
            AST *branch = newASTNode(AST_CASE_BRANCH, NULL);
            if (labels->child_count == 1) {
                AST *only = labels->children[0];
                only->parent = NULL;
                free(labels->children);
                free(labels);
                setLeft(branch, only);
            } else {
                setLeft(branch, labels);
            }
            setRight(branch, body);
            addChild(node, branch);
        } else if (p->current.type == REA_TOKEN_DEFAULT) {
            reaAdvance(p);
            if (p->current.type == REA_TOKEN_COLON) reaAdvance(p);
            AST *body = newASTNode(AST_COMPOUND, NULL);
            while (p->current.type != REA_TOKEN_RIGHT_BRACE &&
                   p->current.type != REA_TOKEN_CASE &&
                   p->current.type != REA_TOKEN_DEFAULT &&
                   p->current.type != REA_TOKEN_EOF) {
                if (p->current.type == REA_TOKEN_BREAK) {
                    reaAdvance(p);
                    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
                    break;
                }
                AST *s = parseStatement(p);
                if (!s) break;
                addChild(body, s);
            }
            setExtra(node, body);
        } else {
            reaAdvance(p);
        }
    }
    if (p->current.type == REA_TOKEN_RIGHT_BRACE) reaAdvance(p);
    return node;
}

static AST *parseConstDecl(ReaParser *p) {
    reaAdvance(p); // consume 'const'
    if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *nameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);
    reaAdvance(p);
    if (p->current.type != REA_TOKEN_EQUAL) return NULL;
    reaAdvance(p);
    AST *value = parseExpression(p);
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    AST *node = newASTNode(AST_CONST_DECL, nameTok);
    setLeft(node, value);
    if (value) setTypeAST(node, value->var_type);
    return node;
}

static AST *parseImport(ReaParser *p) {
    // Parse: #import Identifier[, Identifier]* ;
    // or:    #import "UnitName"[, "UnitName"]* ;
    reaAdvance(p); // consume '#import'
    AST *uses = newASTNode(AST_USES_CLAUSE, NULL);
    uses->unit_list = createList();

    while (p->current.type != REA_TOKEN_EOF) {
        char *name = NULL;
        if (p->current.type == REA_TOKEN_IDENTIFIER) {
            name = (char*)malloc(p->current.length + 1);
            if (!name) break;
            memcpy(name, p->current.start, p->current.length);
            name[p->current.length] = '\0';
            reaAdvance(p);
        } else if (p->current.type == REA_TOKEN_STRING) {
            size_t len = p->current.length;
            if (len >= 2) {
                name = (char*)malloc(len - 1);
                if (!name) break;
                memcpy(name, p->current.start + 1, len - 2);
                name[len - 2] = '\0';
            }
            reaAdvance(p);
        } else {
            break;
        }

        if (name) {
            // Append a copy into the list; list implementation duplicates the pointer value
            listAppend(uses->unit_list, name);
            // We keep ownership consistent with Pascal parser which frees after use; for now, leave as is
        }
        if (p->current.type == REA_TOKEN_COMMA) {
            reaAdvance(p);
            continue;
        }
        break;
    }
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    return uses;
}

static AST *parseStatement(ReaParser *p) {
    if (p->current.type == REA_TOKEN_CLASS) {
        // class Name { field declarations ; ... }
        reaAdvance(p); // consume 'class'
        if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;
        // class name token
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *classNameTok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        reaAdvance(p); // consume class name

        // Optional 'extends' <Identifier>
        AST *parentRef = NULL;
        if (p->current.type == REA_TOKEN_EXTENDS) {
            reaAdvance(p); // consume extends
            if (p->current.type == REA_TOKEN_IDENTIFIER) {
                char *plex = (char *)malloc(p->current.length + 1);
                if (plex) {
                    memcpy(plex, p->current.start, p->current.length);
                    plex[p->current.length] = '\0';
                    Token *ptypeTok = newToken(TOKEN_IDENTIFIER, plex, p->current.line, 0);
                    free(plex);
                    parentRef = newASTNode(AST_TYPE_REFERENCE, ptypeTok);
                }
                reaAdvance(p);
            }
        }

        // Parse class body
        AST *recordAst = newASTNode(AST_RECORD_TYPE, NULL);
        if (parentRef) setExtra(recordAst, parentRef);
        AST *methods = newASTNode(AST_COMPOUND, NULL);
        const char* prevClass = p->currentClassName;
        const char* prevParent = p->currentParentClassName;
        p->currentClassName = classNameTok->value;
        p->currentParentClassName = parentRef && parentRef->token ? parentRef->token->value : NULL;
        if (p->current.type == REA_TOKEN_LEFT_BRACE) {
            reaAdvance(p); // consume '{'
            while (p->current.type != REA_TOKEN_RIGHT_BRACE && p->current.type != REA_TOKEN_EOF) {
                // Field or method: both start with a type keyword
                if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_INT64 ||
                    p->current.type == REA_TOKEN_INT32 || p->current.type == REA_TOKEN_INT16 ||
                    p->current.type == REA_TOKEN_INT8 || p->current.type == REA_TOKEN_FLOAT ||
                    p->current.type == REA_TOKEN_FLOAT32 || p->current.type == REA_TOKEN_LONG_DOUBLE ||
                    p->current.type == REA_TOKEN_CHAR || p->current.type == REA_TOKEN_BYTE ||
                    p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_TEXT ||
                    p->current.type == REA_TOKEN_MSTREAM || p->current.type == REA_TOKEN_BOOL ||
                    p->current.type == REA_TOKEN_VOID ||
                    p->current.type == REA_TOKEN_IDENTIFIER) {
                    // Consume type and identifier into a fake varDecl to reuse existing parser
                    // Consume type and identifier into a fake varDecl to reuse existing parser
                    AST *decl = parseVarDecl(p);
                    if (decl) {
                        if (decl->type == AST_FUNCTION_DECL) {
                            addChild(methods, decl);
                        } else if (decl->type == AST_PROCEDURE_DECL) {
                            addChild(methods, decl);
                        } else {
                            addChild(recordAst, decl);
                        }
                    } else {
                        // Recovery: advance one token to avoid infinite loop
                        reaAdvance(p);
                    }
                } else {
                    // Skip unsupported constructs for now
                    reaAdvance(p);
                }
                // optional semicolons are consumed by parseVarDecl; any extras will be skipped
            }
            if (p->current.type == REA_TOKEN_RIGHT_BRACE) reaAdvance(p);
        }
        p->currentClassName = prevClass;
        p->currentParentClassName = prevParent;

        // Build TYPE_DECL(Name = <recordAst>) and register type
        AST *typeDecl = newASTNode(AST_TYPE_DECL, classNameTok);
        setLeft(typeDecl, recordAst);

        // Register the type into the global type table
        if (classNameTok && classNameTok->value) {
            insertType(classNameTok->value, recordAst);
        }
        // Return both type and methods in a compound so top-level gets both
        AST *bundle = newASTNode(AST_COMPOUND, NULL);
        addChild(bundle, typeDecl);
        // append methods
        if (methods && methods->child_count > 0) {
            for (int mi = 0; mi < methods->child_count; mi++) {
                addChild(bundle, methods->children[mi]);
                methods->children[mi] = NULL;
            }
            methods->child_count = 0;
        }
        freeAST(methods);
        return bundle;
    }
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        return parseBlock(p);
    }
    if (p->current.type == REA_TOKEN_IF) {
        return parseIf(p);
    }
    if (p->current.type == REA_TOKEN_WHILE) {
        return parseWhile(p);
    }
    if (p->current.type == REA_TOKEN_DO) {
        return parseDoWhile(p);
    }
    if (p->current.type == REA_TOKEN_FOR) {
        return parseFor(p);
    }
    if (p->current.type == REA_TOKEN_SWITCH) {
        return parseSwitch(p);
    }
    if (p->current.type == REA_TOKEN_CONTINUE) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
        return newASTNode(AST_CONTINUE, NULL);
    }
    if (p->current.type == REA_TOKEN_BREAK) {
        reaAdvance(p);
        if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
        AST *br = newASTNode(AST_BREAK, NULL);
        return br;
    }
    if (p->current.type == REA_TOKEN_CONST) {
        return parseConstDecl(p);
    }
    if (p->current.type == REA_TOKEN_IMPORT) {
        return parseImport(p);
    }
    if (p->current.type == REA_TOKEN_RETURN) {
        return parseReturn(p);
    }
    if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_INT64 ||
        p->current.type == REA_TOKEN_INT32 || p->current.type == REA_TOKEN_INT16 ||
        p->current.type == REA_TOKEN_INT8 || p->current.type == REA_TOKEN_FLOAT ||
        p->current.type == REA_TOKEN_FLOAT32 || p->current.type == REA_TOKEN_LONG_DOUBLE ||
        p->current.type == REA_TOKEN_CHAR || p->current.type == REA_TOKEN_BYTE ||
        p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_TEXT ||
        p->current.type == REA_TOKEN_MSTREAM || p->current.type == REA_TOKEN_BOOL ||
        p->current.type == REA_TOKEN_VOID) {
        return parseVarDecl(p);
    }
    // Allow declarations that start with user-defined type identifiers
    if (p->current.type == REA_TOKEN_IDENTIFIER) {
        // Peek the identifier text and check type table
        char namebuf[256];
        size_t n = p->current.length < sizeof(namebuf)-1 ? p->current.length : sizeof(namebuf)-1;
        memcpy(namebuf, p->current.start, n);
        namebuf[n] = '\0';
        AST *tdef = lookupType(namebuf);
        if (tdef) {
            return parseVarDecl(p);
        }
    }
    AST *expr = parseExpression(p);
    if (!expr) return NULL;
    if (p->current.type == REA_TOKEN_SEMICOLON) reaAdvance(p);
    if (expr->type == AST_ASSIGN) {
        return expr; // assignments act as statements directly
    }
    AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
    setLeft(stmt, expr);
    return stmt;
}

AST *parseRea(const char *source) {
    ReaParser p;
    reaInitLexer(&p.lexer, source);
    p.currentFunctionType = TYPE_VOID;
    p.hadError = false;
    p.currentClassName = NULL;
    p.currentParentClassName = NULL;
    reaAdvance(&p);

    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);

    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    while (p.current.type != REA_TOKEN_EOF && !p.hadError) {
        AST *stmt = parseStatement(&p);
        if (!stmt) break;
        if (stmt->type == AST_VAR_DECL || stmt->type == AST_FUNCTION_DECL || stmt->type == AST_PROCEDURE_DECL ||
            stmt->type == AST_TYPE_DECL || stmt->type == AST_CONST_DECL) {
            addChild(decls, stmt);
        } else {
            addChild(stmts, stmt);
        }
    }

    if (p.hadError) {
        freeAST(program);
        return NULL;
    }

    return program;
}
