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
static AST *parseAssignment(ReaParser *p);
static AST *parseEquality(ReaParser *p);
static AST *parseComparison(ReaParser *p);
static AST *parseAdditive(ReaParser *p);
static AST *parseTerm(ReaParser *p);
static AST *parseFactor(ReaParser *p);
static AST *parseStatement(ReaParser *p);
static AST *parseVarDecl(ReaParser *p);
static AST *parseIf(ReaParser *p);
static AST *parseBlock(ReaParser *p);

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
    } else if (p->current.type == REA_TOKEN_STRING) {
        size_t len = p->current.length;
        if (len < 2) return NULL;
        char *lex = (char *)malloc(len - 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start + 1, len - 2);
        lex[len - 2] = '\0';
        Token *tok = newToken(TOKEN_STRING_CONST, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_STRING, tok);
        setTypeAST(node, TYPE_STRING);
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
    } else if (p->current.type == REA_TOKEN_IDENTIFIER) {
        char *lex = (char *)malloc(p->current.length + 1);
        if (!lex) return NULL;
        memcpy(lex, p->current.start, p->current.length);
        lex[p->current.length] = '\0';
        Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
        free(lex);
        AST *node = newASTNode(AST_VARIABLE, tok);
        setTypeAST(node, TYPE_UNKNOWN);
        reaAdvance(p);
        return node;
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
    AST *left = parseEquality(p);
    if (!left) return NULL;
    if (left->type == AST_VARIABLE && p->current.type == REA_TOKEN_EQUAL) {
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

static VarType mapType(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT: return TYPE_INT64;
        case REA_TOKEN_FLOAT: return TYPE_DOUBLE;
        case REA_TOKEN_STR: return TYPE_STRING;
        case REA_TOKEN_BOOL: return TYPE_BOOLEAN;
        default: return TYPE_VOID;
    }
}

static const char *typeName(ReaTokenType t) {
    switch (t) {
        case REA_TOKEN_INT: return "int";
        case REA_TOKEN_FLOAT: return "float";
        case REA_TOKEN_STR: return "str";
        case REA_TOKEN_BOOL: return "bool";
        default: return "";
    }
}

static AST *parseVarDecl(ReaParser *p) {
    ReaTokenType typeTok = p->current.type;
    VarType vtype = mapType(typeTok);

    // Build a type identifier node for the declaration's type.
    const char *tname = typeName(typeTok);
    Token *typeToken = newToken(TOKEN_IDENTIFIER, tname, p->current.line, 0);
    AST *typeNode = newASTNode(AST_TYPE_IDENTIFIER, typeToken);
    setTypeAST(typeNode, vtype);

    reaAdvance(p); // consume type keyword

    if (p->current.type != REA_TOKEN_IDENTIFIER) return NULL;

    char *lex = (char *)malloc(p->current.length + 1);
    if (!lex) return NULL;
    memcpy(lex, p->current.start, p->current.length);
    lex[p->current.length] = '\0';
    Token *tok = newToken(TOKEN_IDENTIFIER, lex, p->current.line, 0);
    free(lex);

    AST *var = newASTNode(AST_VARIABLE, tok);
    setTypeAST(var, vtype);

    reaAdvance(p); // consume identifier

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

static AST *parseStatement(ReaParser *p) {
    if (p->current.type == REA_TOKEN_LEFT_BRACE) {
        return parseBlock(p);
    }
    if (p->current.type == REA_TOKEN_IF) {
        return parseIf(p);
    }
    if (p->current.type == REA_TOKEN_INT || p->current.type == REA_TOKEN_FLOAT ||
        p->current.type == REA_TOKEN_STR || p->current.type == REA_TOKEN_BOOL) {
        return parseVarDecl(p);
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
    reaAdvance(&p);

    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);

    AST *decls = newASTNode(AST_COMPOUND, NULL);
    AST *stmts = newASTNode(AST_COMPOUND, NULL);
    addChild(block, decls);
    addChild(block, stmts);

    while (p.current.type != REA_TOKEN_EOF) {
        AST *stmt = parseStatement(&p);
        if (!stmt) break;
        if (stmt->type == AST_VAR_DECL) {
            addChild(decls, stmt);
        } else {
            addChild(stmts, stmt);
        }
    }

    return program;
}

