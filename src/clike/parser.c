#include "clike/parser.h"
#include "clike/errors.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static VarType tokenTypeToVarType(ClikeTokenType t) {
    switch (t) {
        case CLIKE_TOKEN_INT: return TYPE_INTEGER;
        case CLIKE_TOKEN_FLOAT: return TYPE_REAL;
        case CLIKE_TOKEN_STR: return TYPE_STRING;
        case CLIKE_TOKEN_TEXT: return TYPE_FILE;
        case CLIKE_TOKEN_VOID: return TYPE_VOID;
        default: return TYPE_UNKNOWN;
    }
}

static VarType literalTokenToVarType(ClikeTokenType t) {
    switch (t) {
        case CLIKE_TOKEN_FLOAT_LITERAL: return TYPE_REAL;
        case CLIKE_TOKEN_CHAR: return TYPE_CHAR;
        default: return TYPE_INTEGER;
    }
}

static void advanceParser(ParserClike *p) {
    p->current = p->next;
    p->next = clike_nextToken(&p->lexer);
}

static int matchToken(ParserClike *p, ClikeTokenType type) {
    if (p->current.type == type) { advanceParser(p); return 1; }
    return 0;
}

static void expectToken(ParserClike *p, ClikeTokenType type, const char *msg) {
    if (!matchToken(p, type)) {
        fprintf(stderr,
                "Parse error at line %d, column %d: expected %s (%s), got '%.*s' (%s)\n",
                p->current.line,
                p->current.column,
                msg,
                clikeTokenTypeToString(type),
                p->current.length,
                p->current.lexeme,
                clikeTokenTypeToString(p->current.type));
        clike_error_count++;
    }
}

static ASTNodeClike* declaration(ParserClike *p);
static ASTNodeClike* varDeclaration(ParserClike *p, ClikeToken type_token, ClikeToken ident);
static ASTNodeClike* funDeclaration(ParserClike *p, ClikeToken type_token, ClikeToken ident);
static ASTNodeClike* params(ParserClike *p);
static ASTNodeClike* param(ParserClike *p);
static ASTNodeClike* compoundStmt(ParserClike *p);
static ASTNodeClike* statement(ParserClike *p);
static ASTNodeClike* expression(ParserClike *p);
static ASTNodeClike* assignment(ParserClike *p);
static ASTNodeClike* logicalOr(ParserClike *p);
static ASTNodeClike* logicalAnd(ParserClike *p);
static ASTNodeClike* equality(ParserClike *p);
static ASTNodeClike* relational(ParserClike *p);
static ASTNodeClike* additive(ParserClike *p);
static ASTNodeClike* term(ParserClike *p);
static ASTNodeClike* unary(ParserClike *p);
static ASTNodeClike* factor(ParserClike *p);
static ASTNodeClike* call(ParserClike *p, ClikeToken ident);
static ASTNodeClike* expressionStatement(ParserClike *p);
static ASTNodeClike* ifStatement(ParserClike *p);
static ASTNodeClike* whileStatement(ParserClike *p);
static ASTNodeClike* forStatement(ParserClike *p);
static ASTNodeClike* doWhileStatement(ParserClike *p);
static ASTNodeClike* breakStatement(ParserClike *p);
static ASTNodeClike* continueStatement(ParserClike *p);
static ASTNodeClike* returnStatement(ParserClike *p);

char **clike_imports = NULL;
int clike_import_count = 0;
static int clike_import_capacity = 0;

static void queueImportPath(ParserClike *p, ClikeToken tok) {
    char *path = (char*)malloc(tok.length + 1);
    memcpy(path, tok.lexeme, tok.length);
    path[tok.length] = '\0';
    for (int i = 0; i < clike_import_count; ++i) {
        if (strcmp(clike_imports[i], path) == 0) {
            free(path);
            return;
        }
    }
    if (clike_import_count >= clike_import_capacity) {
        clike_import_capacity = clike_import_capacity ? clike_import_capacity * 2 : 4;
        clike_imports = (char**)realloc(clike_imports, sizeof(char*) * clike_import_capacity);
    }
    clike_imports[clike_import_count++] = path;
    if (p) {
        if (p->import_count >= p->import_capacity) {
            p->import_capacity = p->import_capacity ? p->import_capacity * 2 : 4;
            p->imports = (char**)realloc(p->imports, sizeof(char*) * p->import_capacity);
        }
        p->imports[p->import_count++] = path;
    }
}

void initParserClike(ParserClike *parser, const char *source) {
    clike_initLexer(&parser->lexer, source);
    parser->current = clike_nextToken(&parser->lexer);
    parser->next = clike_nextToken(&parser->lexer);
    parser->imports = NULL;
    parser->import_count = 0;
    parser->import_capacity = 0;
}

ASTNodeClike* parseProgramClike(ParserClike *parser) {
    ASTNodeClike *prog = newASTNodeClike(TCAST_PROGRAM, parser->current);
    while (parser->current.type != CLIKE_TOKEN_EOF) {
        if (parser->current.type == CLIKE_TOKEN_IMPORT) {
            advanceParser(parser);
            ClikeToken pathTok = parser->current;
            expectToken(parser, CLIKE_TOKEN_STRING, "module path");
            queueImportPath(parser, pathTok);
            expectToken(parser, CLIKE_TOKEN_SEMICOLON, ";");
            continue;
        }
        ASTNodeClike *decl = declaration(parser);
        if (decl) addChildClike(prog, decl);
    }
    return prog;
}

static int isTypeToken(ClikeTokenType t) {
    return t == CLIKE_TOKEN_INT || t == CLIKE_TOKEN_VOID ||
           t == CLIKE_TOKEN_FLOAT || t == CLIKE_TOKEN_STR ||
           t == CLIKE_TOKEN_TEXT;
}

static ASTNodeClike* declaration(ParserClike *p) {
    if (isTypeToken(p->current.type)) {
        ClikeToken type_tok = p->current; advanceParser(p);
        ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
        if (p->current.type == CLIKE_TOKEN_LPAREN) {
            return funDeclaration(p, type_tok, ident);
        } else {
            return varDeclaration(p, type_tok, ident);
        }
    }
    return NULL;
}

static ASTNodeClike* varDeclaration(ParserClike *p, ClikeToken type_token, ClikeToken ident) {
    ASTNodeClike *node = newASTNodeClike(TCAST_VAR_DECL, ident);
    node->var_type = tokenTypeToVarType(type_token.type);
    node->right = newASTNodeClike(TCAST_IDENTIFIER, type_token);
    node->right->var_type = node->var_type;
    if (matchToken(p, CLIKE_TOKEN_LBRACKET)) {
        ClikeToken num = p->current; expectToken(p, CLIKE_TOKEN_NUMBER, "array size");
        node->left = newASTNodeClike(TCAST_NUMBER, num);
        node->left->var_type = TYPE_INTEGER;
        node->is_array = 1;
        node->array_size = (int)num.int_val;
        node->element_type = node->var_type;
        node->var_type = TYPE_ARRAY;
        expectToken(p, CLIKE_TOKEN_RBRACKET, "]");
    }
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    return node;
}

static ASTNodeClike* funDeclaration(ParserClike *p, ClikeToken type_token, ClikeToken ident) {
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *paramsNode = params(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *body = compoundStmt(p);
    ASTNodeClike *node = newASTNodeClike(TCAST_FUN_DECL, ident);
    node->var_type = tokenTypeToVarType(type_token.type);
    node->left = paramsNode;
    node->right = body;
    return node;
}

static ASTNodeClike* params(ParserClike *p) {
    /* allow both "void" and empty parameter lists */
    if (p->current.type == CLIKE_TOKEN_VOID) { advanceParser(p); return NULL; }
    if (p->current.type == CLIKE_TOKEN_RPAREN) { return NULL; }
    ASTNodeClike *paramList = newASTNodeClike(TCAST_PARAM, p->current);
    ASTNodeClike *first = param(p);
    addChildClike(paramList, first);
    while (matchToken(p, CLIKE_TOKEN_COMMA)) {
        ASTNodeClike *pr = param(p);
        addChildClike(paramList, pr);
    }
    return paramList;
}

static ASTNodeClike* param(ParserClike *p) {
    ClikeToken type_tok = p->current; advanceParser(p);
    ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "param name");
    ASTNodeClike *node = newASTNodeClike(TCAST_PARAM, ident);
    node->var_type = tokenTypeToVarType(type_tok.type);
    node->left = newASTNodeClike(TCAST_IDENTIFIER, type_tok);
    node->left->var_type = node->var_type;
    return node;
}

static ASTNodeClike* compoundStmt(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_LBRACE, "{");
    ASTNodeClike *node = newASTNodeClike(TCAST_COMPOUND, p->current);
    while (isTypeToken(p->current.type)) {
        ClikeToken type_tok = p->current; advanceParser(p);
        ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
        ASTNodeClike *decl = varDeclaration(p, type_tok, ident);
        addChildClike(node, decl);
    }
    while (p->current.type != CLIKE_TOKEN_RBRACE && p->current.type != CLIKE_TOKEN_EOF) {
        ASTNodeClike *stmt = statement(p);
        if (stmt) addChildClike(node, stmt);
    }
    expectToken(p, CLIKE_TOKEN_RBRACE, "}");
    return node;
}

static ASTNodeClike* statement(ParserClike *p) {
    switch (p->current.type) {
        case CLIKE_TOKEN_IF: return ifStatement(p);
        case CLIKE_TOKEN_WHILE: return whileStatement(p);
        case CLIKE_TOKEN_FOR: return forStatement(p);
        case CLIKE_TOKEN_DO: return doWhileStatement(p);
        case CLIKE_TOKEN_BREAK: return breakStatement(p);
        case CLIKE_TOKEN_CONTINUE: return continueStatement(p);
        case CLIKE_TOKEN_RETURN: return returnStatement(p);
        case CLIKE_TOKEN_LBRACE: return compoundStmt(p);
        default: return expressionStatement(p);
    }
}

static ASTNodeClike* ifStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_IF, "if");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *cond = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *thenBranch = statement(p);
    ASTNodeClike *elseBranch = NULL;
    if (matchToken(p, CLIKE_TOKEN_ELSE)) {
        elseBranch = statement(p);
    }
    ASTNodeClike *node = newASTNodeClike(TCAST_IF, p->current);
    node->left = cond;
    node->right = thenBranch;
    node->third = elseBranch;
    return node;
}

static ASTNodeClike* whileStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_WHILE, "while");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *cond = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *body = statement(p);
    ASTNodeClike *node = newASTNodeClike(TCAST_WHILE, p->current);
    node->left = cond;
    node->right = body;
    return node;
}

static ASTNodeClike* forStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_FOR, "for");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *init = NULL;
    if (p->current.type != CLIKE_TOKEN_SEMICOLON) init = expression(p);
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *cond = NULL;
    if (p->current.type != CLIKE_TOKEN_SEMICOLON) cond = expression(p);
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *post = NULL;
    if (p->current.type != CLIKE_TOKEN_RPAREN) post = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *body = statement(p);
    ASTNodeClike *node = newASTNodeClike(TCAST_FOR, p->current);
    node->left = init;
    node->right = cond;
    node->third = post;
    if (body) addChildClike(node, body);
    return node;
}

static ASTNodeClike* doWhileStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_DO, "do");
    ASTNodeClike *body = statement(p);
    expectToken(p, CLIKE_TOKEN_WHILE, "while");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *cond = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *node = newASTNodeClike(TCAST_DO_WHILE, p->current);
    node->left = cond;
    node->right = body;
    return node;
}

static ASTNodeClike* breakStatement(ParserClike *p) {
    ClikeToken tok = p->current;
    expectToken(p, CLIKE_TOKEN_BREAK, "break");
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    return newASTNodeClike(TCAST_BREAK, tok);
}

static ASTNodeClike* continueStatement(ParserClike *p) {
    ClikeToken tok = p->current;
    expectToken(p, CLIKE_TOKEN_CONTINUE, "continue");
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    return newASTNodeClike(TCAST_CONTINUE, tok);
}

static ASTNodeClike* returnStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_RETURN, "return");
    ASTNodeClike *expr = NULL;
    if (p->current.type != CLIKE_TOKEN_SEMICOLON) {
        expr = expression(p);
    }
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *node = newASTNodeClike(TCAST_RETURN, p->current);
    node->left = expr;
    return node;
}

static ASTNodeClike* expressionStatement(ParserClike *p) {
    if (p->current.type == CLIKE_TOKEN_SEMICOLON) {
        advanceParser(p);
        return newASTNodeClike(TCAST_EXPR_STMT, p->current);
    }
    ASTNodeClike *expr = expression(p);
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *node = newASTNodeClike(TCAST_EXPR_STMT, p->current);
    node->left = expr;
    return node;
}

static ASTNodeClike* expression(ParserClike *p) { return assignment(p); }

static ASTNodeClike* assignment(ParserClike *p) {
    ASTNodeClike *node = logicalOr(p);
    if (p->current.type == CLIKE_TOKEN_EQUAL) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = assignment(p);
        ASTNodeClike *assign = newASTNodeClike(TCAST_ASSIGN, op);
        assign->left = node;
        assign->right = right;
        return assign;
    }
    return node;
}

static ASTNodeClike* logicalOr(ParserClike *p) {
    ASTNodeClike *node = logicalAnd(p);
    while (p->current.type == CLIKE_TOKEN_OR_OR) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = logicalAnd(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeClike* logicalAnd(ParserClike *p) {
    ASTNodeClike *node = equality(p);
    while (p->current.type == CLIKE_TOKEN_AND_AND) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = equality(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeClike* equality(ParserClike *p) {
    ASTNodeClike *node = relational(p);
    while (p->current.type == CLIKE_TOKEN_EQUAL_EQUAL || p->current.type == CLIKE_TOKEN_BANG_EQUAL) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = relational(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeClike* relational(ParserClike *p) {
    ASTNodeClike *node = additive(p);
    while (p->current.type == CLIKE_TOKEN_LESS || p->current.type == CLIKE_TOKEN_LESS_EQUAL ||
           p->current.type == CLIKE_TOKEN_GREATER || p->current.type == CLIKE_TOKEN_GREATER_EQUAL) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = additive(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeClike* additive(ParserClike *p) {
    ASTNodeClike *node = term(p);
    while (p->current.type == CLIKE_TOKEN_PLUS || p->current.type == CLIKE_TOKEN_MINUS) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = term(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeClike* term(ParserClike *p) {
    ASTNodeClike *node = unary(p);
    while (p->current.type == CLIKE_TOKEN_STAR || p->current.type == CLIKE_TOKEN_SLASH) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = unary(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        bin->left = node; bin->right = rhs; node = bin;
    }
    return node;
}

static ASTNodeClike* unary(ParserClike *p) {
    if (p->current.type == CLIKE_TOKEN_MINUS || p->current.type == CLIKE_TOKEN_BANG) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = unary(p);
        ASTNodeClike *node = newASTNodeClike(TCAST_UNOP, op);
        node->left = right;
        return node;
    }
    return factor(p);
}

static ASTNodeClike* factor(ParserClike *p) {
    if (matchToken(p, CLIKE_TOKEN_LPAREN)) {
        ASTNodeClike *expr = expression(p);
        expectToken(p, CLIKE_TOKEN_RPAREN, ")");
        return expr;
    }
    if (p->current.type == CLIKE_TOKEN_NUMBER || p->current.type == CLIKE_TOKEN_FLOAT_LITERAL || p->current.type == CLIKE_TOKEN_CHAR) {
        ClikeToken num = p->current; advanceParser(p);
        ASTNodeClike *n = newASTNodeClike(TCAST_NUMBER, num);
        n->var_type = literalTokenToVarType(num.type);
        return n;
    }
    if (p->current.type == CLIKE_TOKEN_STRING) {
        ClikeToken str = p->current; advanceParser(p);
        ASTNodeClike *n = newASTNodeClike(TCAST_STRING, str);
        n->var_type = TYPE_STRING;
        return n;
    }
    if (p->current.type == CLIKE_TOKEN_IDENTIFIER) {
        ClikeToken ident = p->current; advanceParser(p);
        if (p->current.type == CLIKE_TOKEN_LPAREN) {
            return call(p, ident);
        }
        if (p->current.type == CLIKE_TOKEN_LBRACKET) {
            advanceParser(p);
            ASTNodeClike *index = expression(p);
            expectToken(p, CLIKE_TOKEN_RBRACKET, "]");
            ASTNodeClike *idNode = newASTNodeClike(TCAST_IDENTIFIER, ident);
            ASTNodeClike *access = newASTNodeClike(TCAST_ARRAY_ACCESS, ident);
            access->left = idNode;
            access->right = index;
            return access;
        }
        return newASTNodeClike(TCAST_IDENTIFIER, ident);
    }
    fprintf(stderr, "Unexpected token %s at line %d\n", clikeTokenTypeToString(p->current.type), p->current.line);
    advanceParser(p);
    return newASTNodeClike(TCAST_NUMBER, p->current); // error node
}

static ASTNodeClike* call(ParserClike *p, ClikeToken ident) {
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *node = newASTNodeClike(TCAST_CALL, ident);
    if (p->current.type != CLIKE_TOKEN_RPAREN) {
        ASTNodeClike *arg = expression(p);
        addChildClike(node, arg);
        while (matchToken(p, CLIKE_TOKEN_COMMA)) {
            ASTNodeClike *argn = expression(p);
            addChildClike(node, argn);
        }
    }
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    return node;
}
