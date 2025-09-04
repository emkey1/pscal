#include "rea/parser.h"
#include "ast/ast.h"
#include "core/types.h"
#include <stdlib.h>
#include <string.h>

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);

typedef struct {
    ReaLexer lexer;
    ReaToken current;
} ReaParser;

static void reaAdvance(ReaParser *p) { p->current = reaNextToken(&p->lexer); }

static AST *parseExpression(ReaParser *p);
static AST *parseTerm(ReaParser *p);
static AST *parseFactor(ReaParser *p);

static AST *parseFactor(ReaParser *p) {
    if (p->current.type == REA_TOKEN_NUMBER) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';

        TokenType ttype = TOKEN_INTEGER_CONST;
        VarType vtype = TYPE_INT64;
        bool is_real = false;
        for (size_t i = 0; i < p->current.length; i++) {
            if (lex[i] == '.' || lex[i] == 'e' || lex[i] == 'E') {
                is_real = true;
                break;
            }
        }
        if (is_real) {
            ttype = TOKEN_REAL_CONST;
            vtype = TYPE_DOUBLE;
        }

        Token *tok = newToken(ttype, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_NUMBER, tok);
        setTypeAST(node, vtype);
        reaAdvance(p);
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
        default: return TOKEN_UNKNOWN;
    }
}

static const char *opLexeme(TokenType t) {
    switch (t) {
        case TOKEN_PLUS: return "+";
        case TOKEN_MINUS: return "-";
        case TOKEN_MUL: return "*";
        case TOKEN_SLASH: return "/";
        default: return "";
    }
}

static AST *parseTerm(ReaParser *p) {
    AST *node = parseFactor(p);
    if (!node) return NULL;
    while (p->current.type == REA_TOKEN_STAR || p->current.type == REA_TOKEN_SLASH) {
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

static AST *parseExpression(ReaParser *p) {
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

AST *parseRea(const char *source) {
    ReaParser p;
    reaInitLexer(&p.lexer, source);
    reaAdvance(&p);

    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);

    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    AST *expr = parseExpression(&p);
    if (expr) {
        AST *stmt = newASTNode(AST_EXPR_STMT, expr->token);
        setLeft(stmt, expr);
        addChild(stmts, stmt);
    }

    return program;
}

