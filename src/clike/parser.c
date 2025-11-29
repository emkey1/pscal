#include "clike/parser.h"
#include "clike/errors.h"
#include "clike/opt.h"
#include "ast/ast.h"
#include "Pascal/type_registry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

VarType clikeTokenTypeToVarType(ClikeTokenType t) {
    switch (t) {
        case CLIKE_TOKEN_INT:        return TYPE_INT32;
        case CLIKE_TOKEN_LONG:       return TYPE_INT64;
        case CLIKE_TOKEN_LONG_LONG:  return TYPE_INT64;
        case CLIKE_TOKEN_FLOAT:      return TYPE_FLOAT;
        case CLIKE_TOKEN_DOUBLE:     return TYPE_DOUBLE;
        case CLIKE_TOKEN_LONG_DOUBLE:return TYPE_LONG_DOUBLE;
        case CLIKE_TOKEN_STR:        return TYPE_STRING;
        case CLIKE_TOKEN_TEXT:       return TYPE_FILE;
        case CLIKE_TOKEN_MSTREAM:    return TYPE_MEMORYSTREAM;
        case CLIKE_TOKEN_VOID:       return TYPE_VOID;
        case CLIKE_TOKEN_CHAR:       return TYPE_CHAR;
        case CLIKE_TOKEN_BYTE:       return TYPE_BYTE;
        default:                     return TYPE_UNKNOWN;
    }
}

const char *clikeTokenTypeToTypeName(ClikeTokenType t) {
    switch (t) {
        case CLIKE_TOKEN_INT:    return "int";
        case CLIKE_TOKEN_LONG:   return "long";
        case CLIKE_TOKEN_LONG_LONG: return "long long";
        case CLIKE_TOKEN_FLOAT:  return "float";
        case CLIKE_TOKEN_DOUBLE: return "double";
        case CLIKE_TOKEN_LONG_DOUBLE: return "long double";
        case CLIKE_TOKEN_STR:    return "string";
        case CLIKE_TOKEN_TEXT:   return "text";
        case CLIKE_TOKEN_MSTREAM:return "mstream";
        case CLIKE_TOKEN_CHAR:   return "char";
        case CLIKE_TOKEN_BYTE:   return "byte";
        case CLIKE_TOKEN_VOID:   return "void";
        default: return NULL;
    }
}

static VarType literalTokenToVarType(ClikeTokenType t) {
    switch (t) {
        case CLIKE_TOKEN_FLOAT_LITERAL: return TYPE_DOUBLE;
        case CLIKE_TOKEN_CHAR_LITERAL:  return TYPE_CHAR;
        default:                        return TYPE_INT32;
    }
}

static void advanceParser(ParserClike *p) {
    p->current = p->next;
    p->next = clikeNextToken(&p->lexer);
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

static ClikeToken parseTypeToken(ParserClike *p) {
    ClikeToken type_tok = p->current;
    if (type_tok.type == CLIKE_TOKEN_LONG && p->next.type == CLIKE_TOKEN_LONG) {
        advanceParser(p);
        advanceParser(p);
        type_tok.type = CLIKE_TOKEN_LONG_LONG;
        type_tok.lexeme = "long long";
        type_tok.length = 9;
    } else if (type_tok.type == CLIKE_TOKEN_LONG && p->next.type == CLIKE_TOKEN_DOUBLE) {
        advanceParser(p);
        advanceParser(p);
        type_tok.type = CLIKE_TOKEN_LONG_DOUBLE;
        type_tok.lexeme = "long double";
        type_tok.length = 11;
    } else {
        advanceParser(p);
    }
    return type_tok;
}

static int isTypeToken(ClikeTokenType t);
static ASTNodeClike* funDeclaration(ParserClike *p, ClikeToken type_token, ClikeToken ident, int isPointer);
static ASTNodeClike* structDeclaration(ParserClike *p, ClikeToken nameTok);
static ASTNodeClike* varDeclarationNoSemi(ParserClike *p, ClikeToken type_token, ClikeToken ident, int isPointer);
static ASTNodeClike* structVarDeclarationNoSemi(ParserClike *p, ClikeToken nameTok, ClikeToken ident, int isPointer);
static ASTNodeClike* structFunDeclaration(ParserClike *p, ClikeToken nameTok, ClikeToken ident, int isPointer);
static ASTNodeClike* params(ParserClike *p);
static ASTNodeClike* param(ParserClike *p);
static ASTNodeClike* compoundStmt(ParserClike *p);
static ASTNodeClike* statement(ParserClike *p);
static ASTNodeClike* expression(ParserClike *p);
static ASTNodeClike* assignment(ParserClike *p);
static ASTNodeClike* conditional(ParserClike *p);
static ASTNodeClike* logicalOr(ParserClike *p);
static ASTNodeClike* logicalAnd(ParserClike *p);
static ASTNodeClike* bitwiseOr(ParserClike *p);
static ASTNodeClike* bitwiseXor(ParserClike *p);
static ASTNodeClike* bitwiseAnd(ParserClike *p);
static ASTNodeClike* equality(ParserClike *p);
static ASTNodeClike* relational(ParserClike *p);
static ASTNodeClike* shift(ParserClike *p);
static ASTNodeClike* additive(ParserClike *p);
static ASTNodeClike* term(ParserClike *p);
static ASTNodeClike* unary(ParserClike *p);
static ASTNodeClike* factor(ParserClike *p);
static ASTNodeClike* postfix(ParserClike *p, ASTNodeClike *node);
static ASTNodeClike* call(ParserClike *p, ClikeToken ident);
static ASTNodeClike* expressionStatement(ParserClike *p);
static ASTNodeClike* ifStatement(ParserClike *p);
static ASTNodeClike* whileStatement(ParserClike *p);
static ASTNodeClike* forStatement(ParserClike *p);
static ASTNodeClike* doWhileStatement(ParserClike *p);
static ASTNodeClike* breakStatement(ParserClike *p);
static ASTNodeClike* continueStatement(ParserClike *p);
static ASTNodeClike* returnStatement(ParserClike *p);
static ASTNodeClike* switchStatement(ParserClike *p);
ASTNodeClike* clikeSpawnStatement(ParserClike *p);
ASTNodeClike* clikeJoinStatement(ParserClike *p);
static ClikeToken parseStringLiteral(ParserClike *p);

char **clike_imports = NULL;
int clike_import_count = 0;
static int clike_import_capacity = 0;

typedef struct {
    char *name;
    AST *ast; // core AST representing struct layout
} ClikeStructDef;

static ClikeStructDef *clike_structs = NULL;
static int clike_struct_count = 0;
static int clike_struct_capacity = 0;

typedef struct { char *name; long long value; } ConstEntry;
static ConstEntry const_table[256];
static int const_count = 0;

static char* copyName(const char *s, size_t len) {
    char *out = (char*)malloc(len + 1);
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

static void addConst(const char *name, size_t len, long long value) {
    if (const_count >= 256) return;
    const_table[const_count].name = copyName(name, len);
    const_table[const_count].value = value;
    const_count++;
}

// Parse one or more adjacent string literals, concatenating their contents.
static ClikeToken parseStringLiteral(ParserClike *p) {
    ClikeToken str = p->current;
    if (str.type != CLIKE_TOKEN_STRING) {
        expectToken(p, CLIKE_TOKEN_STRING, "string literal");
        return str;
    }
    advanceParser(p);
    if (p->current.type != CLIKE_TOKEN_STRING) return str;
    size_t total = str.length;
    char *buf = copyName(str.lexeme, str.length);
    while (p->current.type == CLIKE_TOKEN_STRING) {
        char *newbuf = (char*)realloc(buf, total + p->current.length + 1);
        if (!newbuf) break;
        buf = newbuf;
        memcpy(buf + total, p->current.lexeme, p->current.length);
        total += p->current.length;
        buf[total] = '\0';
        advanceParser(p);
    }
    str.lexeme = buf;
    str.length = (int)total;
    return str;
}

static int getConst(const char *name, size_t len, long long *out) {
    for (int i = 0; i < const_count; ++i) {
        if (strncmp(const_table[i].name, name, len) == 0 && const_table[i].name[len] == '\0') {
            if (out) *out = const_table[i].value;
            return 1;
        }
    }
    return 0;
}

static void freeConstTable(void) {
    for (int i = 0; i < const_count; ++i) free(const_table[i].name);
    const_count = 0;
}

static inline int isIntlikeTypeLocal(VarType t) {
    switch (t) {
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
        case TYPE_BOOLEAN:
            return 1;
        default:
            return 0;
    }
}

static long long evalConstExpr(ASTNodeClike* node, int *ok) {
    if (!node) { *ok = 0; return 0; }
    switch (node->type) {
        case TCAST_NUMBER:
            if (isIntlikeTypeLocal(node->var_type)) { *ok = 1; return node->token.int_val; }
            *ok = 0; return 0;
        case TCAST_IDENTIFIER: {
            long long val;
            if (getConst(node->token.lexeme, node->token.length, &val)) { *ok = 1; return val; }
            *ok = 0; return 0;
        }
        case TCAST_BINOP: {
            long long lv = evalConstExpr(node->left, ok);
            if (!*ok) return 0;
            long long rv = evalConstExpr(node->right, ok);
            if (!*ok) return 0;
            switch (node->token.type) {
                case CLIKE_TOKEN_PLUS:  return lv + rv;
                case CLIKE_TOKEN_MINUS: return lv - rv;
                case CLIKE_TOKEN_STAR:  return lv * rv;
                case CLIKE_TOKEN_SLASH: if (rv != 0) return lv / rv; else { *ok = 0; return 0; }
                default: *ok = 0; return 0;
            }
        }
        case TCAST_UNOP: {
            long long v = evalConstExpr(node->left, ok);
            if (!*ok) return 0;
            switch (node->token.type) {
                case CLIKE_TOKEN_MINUS: return -v;
                case CLIKE_TOKEN_PLUS:  return v;
                default: *ok = 0; return 0;
            }
        }
        default:
            *ok = 0; return 0;
    }
}

AST* clikeLookupStruct(const char *name) {
    for (int i = 0; i < clike_struct_count; ++i) {
        if (strcmp(clike_structs[i].name, name) == 0) return clike_structs[i].ast;
    }
    return NULL;
}

void clikeRegisterStruct(const char *name, AST *ast) {
    for (int i = 0; i < clike_struct_count; ++i) {
        if (strcmp(clike_structs[i].name, name) == 0) {
            clike_structs[i].ast = ast;
            insertType(name, ast);
            return;
        }
    }
    if (clike_struct_count >= clike_struct_capacity) {
        int new_cap = clike_struct_capacity ? clike_struct_capacity * 2 : 4;
        ClikeStructDef *new_structs =
            (ClikeStructDef*)realloc(clike_structs, sizeof(ClikeStructDef) * new_cap);
        if (!new_structs) return;
        clike_structs = new_structs;
        clike_struct_capacity = new_cap;
    }
    clike_structs[clike_struct_count].name = strdup(name);
    clike_structs[clike_struct_count].ast = ast;
    clike_struct_count++;
    insertType(name, ast);
}

void clikeFreeStructs(void) {
    for (int i = 0; i < clike_struct_count; ++i) free(clike_structs[i].name);
    free(clike_structs);
    clike_structs = NULL;
    clike_struct_count = 0;
    clike_struct_capacity = 0;
}

static Token* makeIdentToken(const char *s) {
    Token *t = (Token*)malloc(sizeof(Token));
    t->type = TOKEN_IDENTIFIER;
    t->value = strdup(s);
    t->length = t->value ? strlen(t->value) : 0;
    t->line = 0;
    t->column = 0;
    t->is_char_code = false;
    return t;
}

static char* clikeTokenToCString(ClikeToken t) {
    char *s = (char*)malloc(t.length + 1);
    memcpy(s, t.lexeme, t.length);
    s[t.length] = '\0';
    return s;
}

static AST* makeBuiltinTypeAST(ClikeToken t) {
    const char *name = clikeTokenTypeToTypeName(t.type);
    VarType vt = clikeTokenTypeToVarType(t.type);
    if (!name) { name = "integer"; vt = TYPE_INT64; }
    Token *tok = makeIdentToken(name);
    AST *node = newASTNode(AST_VARIABLE, tok);
    setTypeAST(node, vt);
    return node;
}

static void queueImportPath(ParserClike *p, ClikeToken tok) {
    char *path = (char*)malloc(tok.length + 1);
    if (!path) return;
    memcpy(path, tok.lexeme, tok.length);
    path[tok.length] = '\0';
    for (int i = 0; i < clike_import_count; ++i) {
        if (strcmp(clike_imports[i], path) == 0) {
            free(path);
            return;
        }
    }
    if (clike_import_count >= clike_import_capacity) {
        int new_cap = clike_import_capacity ? clike_import_capacity * 2 : 4;
        char **new_imports = (char**)realloc(clike_imports, sizeof(char*) * new_cap);
        if (!new_imports) { free(path); return; }
        clike_imports = new_imports;
        clike_import_capacity = new_cap;
    }
    clike_imports[clike_import_count++] = path;
    if (p) {
        if (p->import_count >= p->import_capacity) {
            int new_cap = p->import_capacity ? p->import_capacity * 2 : 4;
            char **new_arr = (char**)realloc(p->imports, sizeof(char*) * new_cap);
            if (!new_arr) return; // path stored globally, so still freed later
            p->imports = new_arr;
            p->import_capacity = new_cap;
        }
        p->imports[p->import_count++] = path;
    }
}

void initParserClike(ParserClike *parser, const char *source) {
    clikeInitLexer(&parser->lexer, source);
    parser->current = clikeNextToken(&parser->lexer);
    parser->next = clikeNextToken(&parser->lexer);
    parser->imports = NULL;
    parser->import_count = 0;
    parser->import_capacity = 0;
    const_count = 0;
}

void freeParserClike(ParserClike *parser) {
    if (!parser) return;
    free(parser->imports);
    parser->imports = NULL;
    parser->import_count = 0;
    parser->import_capacity = 0;
    freeConstTable();
}

ASTNodeClike* parseProgramClike(ParserClike *p) {
    ASTNodeClike *prog = newASTNodeClike(TCAST_PROGRAM, p->current);
    while (p->current.type != CLIKE_TOKEN_EOF) {
        if (p->current.type == CLIKE_TOKEN_IMPORT) {
            advanceParser(p);
            ClikeToken pathTok = parseStringLiteral(p);
            queueImportPath(p, pathTok);
            expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
            continue;
        } else if (p->current.type == CLIKE_TOKEN_STRUCT) {
            advanceParser(p);
            ClikeToken nameTok = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "struct name");
            if (p->current.type == CLIKE_TOKEN_LBRACE) {
                ASTNodeClike *decl = structDeclaration(p, nameTok);
                addChildClike(prog, decl);
            } else {
                int isPtr = 0;
                if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
                ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                if (p->current.type == CLIKE_TOKEN_LPAREN) {
                    ASTNodeClike *decl = structFunDeclaration(p, nameTok, ident, isPtr);
                    addChildClike(prog, decl);
                } else {
                    ASTNodeClike *decl = structVarDeclarationNoSemi(p, nameTok, ident, isPtr);
                    addChildClike(prog, decl);
                    while (matchToken(p, CLIKE_TOKEN_COMMA)) {
                        int ptr = 0;
                        if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); ptr = 1; }
                        ClikeToken id = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                        ASTNodeClike *d = structVarDeclarationNoSemi(p, nameTok, id, ptr);
                        addChildClike(prog, d);
                    }
                    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
                }
            }
        } else if (p->current.type == CLIKE_TOKEN_CONST || isTypeToken(p->current.type)) {
            int isConst = 0;
            if (p->current.type == CLIKE_TOKEN_CONST) { advanceParser(p); isConst = 1; }
            if (!isTypeToken(p->current.type)) {
                fprintf(stderr, "Parse error at line %d, column %d: expected type after const\n",
                        p->current.line, p->current.column);
                clike_error_count++;
                break;
            }
            ClikeToken type_tok = parseTypeToken(p);
            int isPtr = 0;
            if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
            ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
            if (p->current.type == CLIKE_TOKEN_LPAREN) {
                ASTNodeClike *decl = funDeclaration(p, type_tok, ident, isPtr);
                addChildClike(prog, decl);
            } else {
                ASTNodeClike *decl = varDeclarationNoSemi(p, type_tok, ident, isPtr);
                decl->is_const = isConst;
                addChildClike(prog, decl);
                while (matchToken(p, CLIKE_TOKEN_COMMA)) {
                    int ptr = 0;
                    if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); ptr = 1; }
                    ClikeToken id = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                    ASTNodeClike *d = varDeclarationNoSemi(p, type_tok, id, ptr);
                    d->is_const = isConst;
                    addChildClike(prog, d);
                }
                expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
            }
        } else {
            fprintf(stderr, "Unexpected token %s at line %d, column %d\n",
                    clikeTokenTypeToString(p->current.type),
                    p->current.line, p->current.column);
            clike_error_count++;
            advanceParser(p);
        }
    }
    return prog;
}

void clikeResetParserState(void) {
    if (clike_imports) {
        for (int i = 0; i < clike_import_count; ++i) {
            free(clike_imports[i]);
        }
        free(clike_imports);
        clike_imports = NULL;
    }
    clike_import_count = 0;
    clike_import_capacity = 0;
}

static int isTypeToken(ClikeTokenType t) {
    return t == CLIKE_TOKEN_INT || t == CLIKE_TOKEN_LONG ||
           t == CLIKE_TOKEN_LONG_LONG || t == CLIKE_TOKEN_VOID ||
           t == CLIKE_TOKEN_FLOAT || t == CLIKE_TOKEN_DOUBLE ||
           t == CLIKE_TOKEN_LONG_DOUBLE || t == CLIKE_TOKEN_STR ||
           t == CLIKE_TOKEN_TEXT || t == CLIKE_TOKEN_MSTREAM ||
           t == CLIKE_TOKEN_CHAR || t == CLIKE_TOKEN_BYTE;
}


static ASTNodeClike* structFunDeclaration(ParserClike *p, ClikeToken nameTok, ClikeToken ident, int isPointer) {
    (void)nameTok;
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *paramsNode = params(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *body = compoundStmt(p);
    ASTNodeClike *node = newASTNodeClike(TCAST_FUN_DECL, ident);
    node->var_type = isPointer ? TYPE_POINTER : TYPE_RECORD;
    node->element_type = isPointer ? TYPE_RECORD : TYPE_UNKNOWN;
    setLeftClike(node, paramsNode);
    setRightClike(node, body);
    return node;
}

static ASTNodeClike* parseArrayDim(ParserClike *p) {
    ASTNodeClike *expr = expression(p);
    return optimizeClikeAST(expr);
}

static ASTNodeClike* varDeclarationNoSemi(ParserClike *p, ClikeToken type_token, ClikeToken ident, int isPointer) {
    ASTNodeClike *node = newASTNodeClike(TCAST_VAR_DECL, ident);
    node->var_type = isPointer ? TYPE_POINTER : clikeTokenTypeToVarType(type_token.type);
    node->element_type = isPointer ? clikeTokenTypeToVarType(type_token.type) : TYPE_UNKNOWN;
    setRightClike(node, newASTNodeClike(TCAST_IDENTIFIER, type_token));
    node->right->var_type = node->var_type;
    if (matchToken(p, CLIKE_TOKEN_LBRACKET)) {
        int capacity = 4;
        int count = 0;
        int *dims = (int*)malloc(sizeof(int) * capacity);
        ASTNodeClike **dim_exprs = (ASTNodeClike**)malloc(sizeof(ASTNodeClike*) * capacity);
        do {
            if (count >= capacity) {
                capacity *= 2;
                dims = (int*)realloc(dims, sizeof(int) * capacity);
                dim_exprs = (ASTNodeClike**)realloc(dim_exprs, sizeof(ASTNodeClike*) * capacity);
            }
            ASTNodeClike *dimExpr = NULL;
            if (p->current.type != CLIKE_TOKEN_RBRACKET) {
                dimExpr = parseArrayDim(p);
                int ok; long long val = evalConstExpr(dimExpr, &ok);
                dims[count] = ok ? (int)val : 0;
                dim_exprs[count] = dimExpr;
                if (dimExpr) dimExpr->parent = node;
            } else {
                dims[count] = 0;
                dim_exprs[count] = NULL;
            }
            count++;
            expectToken(p, CLIKE_TOKEN_RBRACKET, "]");
        } while (matchToken(p, CLIKE_TOKEN_LBRACKET));

        node->is_array = 1;
        node->array_size = (count > 0 && dims[0] != 0) ? dims[0] : 0;
        node->array_dims = dims;
        node->array_dim_exprs = dim_exprs;
        node->dim_count = count;
        node->element_type = node->var_type;
        node->var_type = TYPE_ARRAY;
    }
    if (matchToken(p, CLIKE_TOKEN_EQUAL)) {
        setLeftClike(node, expression(p));
    }

    if (node->is_array && node->dim_count > 0 && node->array_dims[0] == 0 &&
        node->element_type == TYPE_CHAR && node->left &&
        node->left->type == TCAST_STRING) {
        node->array_dims[0] = node->left->token.length + 1;
        node->array_size = node->array_dims[0];
    }

    if (node->left) {
        setLeftClike(node, optimizeClikeAST(node->left));
        int ok;
        long long val = evalConstExpr(node->left, &ok);
        if (ok) {
            addConst(ident.lexeme, ident.length, val);
        }
    }
    return node;
}

static ASTNodeClike* structVarDeclarationNoSemi(ParserClike *p, ClikeToken nameTok, ClikeToken ident, int isPointer) {
    ASTNodeClike *node = newASTNodeClike(TCAST_VAR_DECL, ident);
    node->var_type = isPointer ? TYPE_POINTER : TYPE_RECORD;
    node->element_type = isPointer ? TYPE_RECORD : TYPE_UNKNOWN;
    setRightClike(node, newASTNodeClike(TCAST_IDENTIFIER, nameTok));
    node->right->var_type = node->var_type;
    if (matchToken(p, CLIKE_TOKEN_EQUAL)) {
        setLeftClike(node, expression(p));
    }
    return node;
}

static ASTNodeClike* structDeclaration(ParserClike *p, ClikeToken nameTok) {
    ASTNodeClike *node = newASTNodeClike(TCAST_STRUCT_DECL, nameTok);
    AST *recordAst = newASTNode(AST_RECORD_TYPE, NULL);
    setTypeAST(recordAst, TYPE_RECORD);
    char *name = clikeTokenToCString(nameTok);
    int duplicate = clikeLookupStruct(name) != NULL;

    expectToken(p, CLIKE_TOKEN_LBRACE, "{");
    while (p->current.type != CLIKE_TOKEN_RBRACE && p->current.type != CLIKE_TOKEN_EOF) {
        ClikeToken typeTok = parseTypeToken(p);
        ClikeToken structTypeTok; structTypeTok.lexeme = NULL; structTypeTok.length = 0;
        if (typeTok.type == CLIKE_TOKEN_STRUCT) {
            structTypeTok = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "struct name");
        }
        int isPtr = 0;
        if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
        ClikeToken fieldName = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "field name");
        expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");

        ASTNodeClike *fieldDecl = NULL;
        if (typeTok.type == CLIKE_TOKEN_STRUCT) {
            fieldDecl = newASTNodeClike(TCAST_VAR_DECL, fieldName);
            fieldDecl->var_type = isPtr ? TYPE_POINTER : TYPE_RECORD;
            fieldDecl->element_type = isPtr ? TYPE_RECORD : TYPE_UNKNOWN;
            setRightClike(fieldDecl, newASTNodeClike(TCAST_IDENTIFIER, structTypeTok));
            fieldDecl->right->var_type = fieldDecl->var_type;
        } else {
            fieldDecl = newASTNodeClike(TCAST_VAR_DECL, fieldName);
            fieldDecl->var_type = isPtr ? TYPE_POINTER : clikeTokenTypeToVarType(typeTok.type);
            fieldDecl->element_type = isPtr ? clikeTokenTypeToVarType(typeTok.type) : TYPE_UNKNOWN;
            setRightClike(fieldDecl, newASTNodeClike(TCAST_IDENTIFIER, typeTok));
            fieldDecl->right->var_type = fieldDecl->var_type;
        }
        addChildClike(node, fieldDecl);

        // Build core AST field declaration
        AST *fieldAst = newASTNode(AST_VAR_DECL, NULL);
        char *fnameStr = clikeTokenToCString(fieldName);
        Token *fnameTok = makeIdentToken(fnameStr);
        free(fnameStr);
        AST *varNode = newASTNode(AST_VARIABLE, fnameTok);
        addChild(fieldAst, varNode);

        AST *typeAst = NULL;
        if (typeTok.type == CLIKE_TOKEN_STRUCT) {
            AST *base = NULL;
            char *stype = clikeTokenToCString(structTypeTok);
            if (strncmp(stype, nameTok.lexeme, nameTok.length) == 0 && strlen(stype) == (size_t)nameTok.length) {
                base = recordAst;
            } else {
                base = clikeLookupStruct(stype);
            }
            if (isPtr) {
                AST *ptrAst = newASTNode(AST_POINTER_TYPE, NULL);
                setRight(ptrAst, base);
                setTypeAST(ptrAst, TYPE_POINTER);
                typeAst = ptrAst;
            } else {
                typeAst = base;
            }
            free(stype);
        } else {
            AST *base = makeBuiltinTypeAST(typeTok);
            if (isPtr) {
                AST *ptrAst = newASTNode(AST_POINTER_TYPE, NULL);
                setRight(ptrAst, base);
                setTypeAST(ptrAst, TYPE_POINTER);
                typeAst = ptrAst;
            } else {
                typeAst = base;
            }
        }
        setRight(fieldAst, typeAst);
        setTypeAST(fieldAst, typeAst ? typeAst->var_type : TYPE_UNKNOWN);
        addChild(recordAst, fieldAst);
    }
    expectToken(p, CLIKE_TOKEN_RBRACE, "}");
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");

    if (duplicate) {
        fprintf(stderr,
                "Parse error: struct '%s' redefinition at line %d, column %d\n",
                name,
                nameTok.line,
                nameTok.column);
        clike_error_count++;
        freeAST(recordAst);
    } else {
        clikeRegisterStruct(name, recordAst);
    }
    free(name);
    return node;
}

static ASTNodeClike* funDeclaration(ParserClike *p, ClikeToken type_token, ClikeToken ident, int isPointer) {
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *paramsNode = params(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *node = newASTNodeClike(TCAST_FUN_DECL, ident);
    node->var_type = isPointer ? TYPE_POINTER : clikeTokenTypeToVarType(type_token.type);
    if (isPointer) node->element_type = clikeTokenTypeToVarType(type_token.type);
    setLeftClike(node, paramsNode);
    if (p->current.type == CLIKE_TOKEN_SEMICOLON) {
        advanceParser(p);
        return node;
    }
    ASTNodeClike *body = compoundStmt(p);
    setRightClike(node, body);
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
    int isConst = 0;
    if (p->current.type == CLIKE_TOKEN_CONST) { advanceParser(p); isConst = 1; }
    if (p->current.type == CLIKE_TOKEN_STRUCT) {
        advanceParser(p);
        ClikeToken nameTok = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "struct name");
        int isPtr = 0;
        if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
        ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "param name");
        ASTNodeClike *node = newASTNodeClike(TCAST_PARAM, ident);
        node->var_type = isPtr ? TYPE_POINTER : TYPE_RECORD;
        node->element_type = isPtr ? TYPE_RECORD : TYPE_UNKNOWN;
        ASTNodeClike *typeNode = newASTNodeClike(TCAST_IDENTIFIER, nameTok);
        typeNode->var_type = node->var_type;
        setLeftClike(node, typeNode);
        node->is_const = isConst;
        return node;
    } else {
        ClikeToken type_tok = parseTypeToken(p);
        int isPtr = 0;
        if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
        ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "param name");
        ASTNodeClike *node = newASTNodeClike(TCAST_PARAM, ident);
        node->var_type = isPtr ? TYPE_POINTER : clikeTokenTypeToVarType(type_tok.type);
        node->element_type = isPtr ? clikeTokenTypeToVarType(type_tok.type) : TYPE_UNKNOWN;
        setLeftClike(node, newASTNodeClike(TCAST_IDENTIFIER, type_tok));
        node->left->var_type = node->var_type;
        node->is_const = isConst;
        return node;
    }
}

static ASTNodeClike* compoundStmt(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_LBRACE, "{");
    ASTNodeClike *node = newASTNodeClike(TCAST_COMPOUND, p->current);
    while (p->current.type != CLIKE_TOKEN_RBRACE && p->current.type != CLIKE_TOKEN_EOF) {
        if (p->current.type == CLIKE_TOKEN_STRUCT) {
            advanceParser(p);
            ClikeToken nameTok = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "struct name");
            if (p->current.type == CLIKE_TOKEN_LBRACE) {
                ASTNodeClike *decl = structDeclaration(p, nameTok);
                addChildClike(node, decl);
            } else {
                int isPtr = 0;
                if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
                ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                ASTNodeClike *decl = structVarDeclarationNoSemi(p, nameTok, ident, isPtr);
                addChildClike(node, decl);
                while (matchToken(p, CLIKE_TOKEN_COMMA)) {
                    int ptr = 0;
                    if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); ptr = 1; }
                    ClikeToken id = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                    ASTNodeClike *d = structVarDeclarationNoSemi(p, nameTok, id, ptr);
                    addChildClike(node, d);
                }
                expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
            }
        } else if (p->current.type == CLIKE_TOKEN_CONST || isTypeToken(p->current.type)) {
            int isConst = 0;
            if (p->current.type == CLIKE_TOKEN_CONST) { advanceParser(p); isConst = 1; }
            if (!isTypeToken(p->current.type)) {
                fprintf(stderr, "Parse error at line %d, column %d: expected type after const\n",
                        p->current.line, p->current.column);
                clike_error_count++;
                break;
            }
            ClikeToken type_tok = parseTypeToken(p);
            int isPtr = 0;
            if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
            ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
            ASTNodeClike *decl = varDeclarationNoSemi(p, type_tok, ident, isPtr);
            decl->is_const = isConst;
            addChildClike(node, decl);
            while (matchToken(p, CLIKE_TOKEN_COMMA)) {
                int ptr = 0;
                if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); ptr = 1; }
                ClikeToken id = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                ASTNodeClike *d = varDeclarationNoSemi(p, type_tok, id, ptr);
                d->is_const = isConst;
                addChildClike(node, d);
            }
            expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
        } else {
            ASTNodeClike *stmt = statement(p);
            if (stmt) addChildClike(node, stmt);
        }
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
        case CLIKE_TOKEN_SWITCH: return switchStatement(p);
        case CLIKE_TOKEN_BREAK: return breakStatement(p);
        case CLIKE_TOKEN_CONTINUE: return continueStatement(p);
        case CLIKE_TOKEN_RETURN: return returnStatement(p);
        case CLIKE_TOKEN_JOIN: return clikeJoinStatement(p);
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
    setLeftClike(node, cond);
    setRightClike(node, thenBranch);
    setThirdClike(node, elseBranch);
    return node;
}

static ASTNodeClike* whileStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_WHILE, "while");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *cond = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *body = statement(p);
    ASTNodeClike *node = newASTNodeClike(TCAST_WHILE, p->current);
    setLeftClike(node, cond);
    setRightClike(node, body);
    return node;
}

static ASTNodeClike* forStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_FOR, "for");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *init = NULL;
    if (p->current.type != CLIKE_TOKEN_SEMICOLON) {
        if (p->current.type == CLIKE_TOKEN_STRUCT) {
            advanceParser(p);
            ClikeToken nameTok = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "struct name");
            int isPtr = 0;
            if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
            ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
            init = structVarDeclarationNoSemi(p, nameTok, ident, isPtr);
            if (matchToken(p, CLIKE_TOKEN_COMMA)) {
                ASTNodeClike *comp = newASTNodeClike(TCAST_COMPOUND, ident);
                addChildClike(comp, init);
                do {
                    int ptr = 0;
                    if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); ptr = 1; }
                    ClikeToken id = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                    ASTNodeClike *d = structVarDeclarationNoSemi(p, nameTok, id, ptr);
                    addChildClike(comp, d);
                } while (matchToken(p, CLIKE_TOKEN_COMMA));
                init = comp;
            }
        } else if (p->current.type == CLIKE_TOKEN_CONST || isTypeToken(p->current.type)) {
            int isConst = 0;
            if (p->current.type == CLIKE_TOKEN_CONST) { advanceParser(p); isConst = 1; }
            if (!isTypeToken(p->current.type)) {
                fprintf(stderr, "Parse error at line %d, column %d: expected type after const\n",
                        p->current.line, p->current.column);
                clike_error_count++;
            }
            ClikeToken type_tok = parseTypeToken(p);
            int isPtr = 0;
            if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); isPtr = 1; }
            ClikeToken ident = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
            init = varDeclarationNoSemi(p, type_tok, ident, isPtr);
            init->is_const = isConst;
            if (matchToken(p, CLIKE_TOKEN_COMMA)) {
                ASTNodeClike *comp = newASTNodeClike(TCAST_COMPOUND, ident);
                addChildClike(comp, init);
                do {
                    int ptr = 0;
                    if (p->current.type == CLIKE_TOKEN_STAR) { advanceParser(p); ptr = 1; }
                    ClikeToken id = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
                    ASTNodeClike *d = varDeclarationNoSemi(p, type_tok, id, ptr);
                    d->is_const = isConst;
                    addChildClike(comp, d);
                } while (matchToken(p, CLIKE_TOKEN_COMMA));
                init = comp;
            }
        } else {
            init = expression(p);
        }
    }
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *cond = NULL;
    if (p->current.type != CLIKE_TOKEN_SEMICOLON) cond = expression(p);
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *post = NULL;
    if (p->current.type != CLIKE_TOKEN_RPAREN) post = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *body = statement(p);
    ASTNodeClike *node = newASTNodeClike(TCAST_FOR, p->current);
    setLeftClike(node, init);
    setRightClike(node, cond);
    setThirdClike(node, post);
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
    setLeftClike(node, cond);
    setRightClike(node, body);
    return node;
}

static ASTNodeClike* switchStatement(ParserClike *p) {
    expectToken(p, CLIKE_TOKEN_SWITCH, "switch");
    expectToken(p, CLIKE_TOKEN_LPAREN, "(");
    ASTNodeClike *expr = expression(p);
    expectToken(p, CLIKE_TOKEN_RPAREN, ")");
    ASTNodeClike *node = newASTNodeClike(TCAST_SWITCH, p->current);
    setLeftClike(node, expr);
    expectToken(p, CLIKE_TOKEN_LBRACE, "{");
    while (p->current.type == CLIKE_TOKEN_CASE) {
        advanceParser(p);
        ASTNodeClike *val = expression(p);
        expectToken(p, CLIKE_TOKEN_COLON, ":");
        ASTNodeClike *br = newASTNodeClike(TCAST_CASE, val->token);
        setLeftClike(br, val);
        while (p->current.type != CLIKE_TOKEN_CASE &&
               p->current.type != CLIKE_TOKEN_DEFAULT &&
               p->current.type != CLIKE_TOKEN_RBRACE &&
               p->current.type != CLIKE_TOKEN_EOF) {
            ASTNodeClike *stmt = statement(p);
            if (stmt) addChildClike(br, stmt);
        }
        addChildClike(node, br);
    }
    if (p->current.type == CLIKE_TOKEN_DEFAULT) {
        advanceParser(p);
        expectToken(p, CLIKE_TOKEN_COLON, ":");
        ASTNodeClike *defBlock = newASTNodeClike(TCAST_COMPOUND, p->current);
        while (p->current.type != CLIKE_TOKEN_RBRACE &&
               p->current.type != CLIKE_TOKEN_EOF) {
            ASTNodeClike *stmt = statement(p);
            if (stmt) addChildClike(defBlock, stmt);
        }
        setRightClike(node, defBlock);
    }
    expectToken(p, CLIKE_TOKEN_RBRACE, "}");
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
    setLeftClike(node, expr);
    return node;
}

ASTNodeClike* clikeJoinStatement(ParserClike *p) {
    ClikeToken tok = p->current;
    expectToken(p, CLIKE_TOKEN_JOIN, "join");
    ASTNodeClike *expr = expression(p);
    expectToken(p, CLIKE_TOKEN_SEMICOLON, ";");
    ASTNodeClike *node = newThreadJoinClike(expr);
    node->token = tok;
    return node;
}

ASTNodeClike* clikeSpawnStatement(ParserClike *p) {
    ClikeToken tok = p->current;
    expectToken(p, CLIKE_TOKEN_SPAWN, "spawn");
    ClikeToken ident = p->current;
    expectToken(p, CLIKE_TOKEN_IDENTIFIER, "identifier");
    ASTNodeClike *callNode = call(p, ident);
    ASTNodeClike *node = newThreadSpawnClike(callNode);
    node->token = tok;
    node->var_type = TYPE_INT32;
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
    setLeftClike(node, expr);
    return node;
}

static ASTNodeClike* expression(ParserClike *p) { return assignment(p); }

static ASTNodeClike* assignment(ParserClike *p) {
    ASTNodeClike *node = conditional(p);
    ClikeTokenType t = p->current.type;
    if (t == CLIKE_TOKEN_EQUAL) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = assignment(p);
        ASTNodeClike *assign = newASTNodeClike(TCAST_ASSIGN, op);
        setLeftClike(assign, node);
        setRightClike(assign, right);
        return assign;
    } else if (
        t == CLIKE_TOKEN_PLUS_EQUAL || t == CLIKE_TOKEN_MINUS_EQUAL ||
        t == CLIKE_TOKEN_STAR_EQUAL || t == CLIKE_TOKEN_SLASH_EQUAL ||
        t == CLIKE_TOKEN_PERCENT_EQUAL || t == CLIKE_TOKEN_BIT_AND_EQUAL ||
        t == CLIKE_TOKEN_BIT_OR_EQUAL || t == CLIKE_TOKEN_BIT_XOR_EQUAL ||
        t == CLIKE_TOKEN_SHL_EQUAL || t == CLIKE_TOKEN_SHR_EQUAL
    ) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = assignment(p);
        ClikeToken binTok = op;
        switch (t) {
            case CLIKE_TOKEN_PLUS_EQUAL:
                binTok.type = CLIKE_TOKEN_PLUS; binTok.lexeme = "+"; binTok.length = 1; break;
            case CLIKE_TOKEN_MINUS_EQUAL:
                binTok.type = CLIKE_TOKEN_MINUS; binTok.lexeme = "-"; binTok.length = 1; break;
            case CLIKE_TOKEN_STAR_EQUAL:
                binTok.type = CLIKE_TOKEN_STAR; binTok.lexeme = "*"; binTok.length = 1; break;
            case CLIKE_TOKEN_SLASH_EQUAL:
                binTok.type = CLIKE_TOKEN_SLASH; binTok.lexeme = "/"; binTok.length = 1; break;
            case CLIKE_TOKEN_PERCENT_EQUAL:
                binTok.type = CLIKE_TOKEN_PERCENT; binTok.lexeme = "%"; binTok.length = 1; break;
            case CLIKE_TOKEN_BIT_AND_EQUAL:
                binTok.type = CLIKE_TOKEN_BIT_AND; binTok.lexeme = "&"; binTok.length = 1; break;
            case CLIKE_TOKEN_BIT_OR_EQUAL:
                binTok.type = CLIKE_TOKEN_BIT_OR; binTok.lexeme = "|"; binTok.length = 1; break;
            case CLIKE_TOKEN_BIT_XOR_EQUAL:
                binTok.type = CLIKE_TOKEN_BIT_XOR; binTok.lexeme = "^"; binTok.length = 1; break;
            case CLIKE_TOKEN_SHL_EQUAL:
                binTok.type = CLIKE_TOKEN_SHL; binTok.lexeme = "<<"; binTok.length = 2; break;
            case CLIKE_TOKEN_SHR_EQUAL:
                binTok.type = CLIKE_TOKEN_SHR; binTok.lexeme = ">>"; binTok.length = 2; break;
            default: break; // unreachable
        }
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, binTok);
        setLeftClike(bin, cloneASTClike(node));
        setRightClike(bin, right);
        ClikeToken eqTok = op; eqTok.type = CLIKE_TOKEN_EQUAL; eqTok.lexeme = "="; eqTok.length = 1;
        ASTNodeClike *assign = newASTNodeClike(TCAST_ASSIGN, eqTok);
        setLeftClike(assign, node);
        setRightClike(assign, bin);
        return assign;
    }
    return node;
}

static ASTNodeClike* conditional(ParserClike *p) {
    ASTNodeClike *node = logicalOr(p);
    if (p->current.type == CLIKE_TOKEN_QUESTION) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *thenBranch = assignment(p);
        expectToken(p, CLIKE_TOKEN_COLON, ":");
        ASTNodeClike *elseBranch = assignment(p);
        ASTNodeClike *cond = newASTNodeClike(TCAST_TERNARY, op);
        setLeftClike(cond, node);
        setRightClike(cond, thenBranch);
        setThirdClike(cond, elseBranch);
        return cond;
    }
    return node;
}

static ASTNodeClike* logicalOr(ParserClike *p) {
    ASTNodeClike *node = logicalAnd(p);
    while (p->current.type == CLIKE_TOKEN_OR_OR) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = logicalAnd(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* logicalAnd(ParserClike *p) {
    ASTNodeClike *node = bitwiseOr(p);
    while (p->current.type == CLIKE_TOKEN_AND_AND) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = bitwiseOr(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* bitwiseOr(ParserClike *p) {
    ASTNodeClike *node = bitwiseXor(p);
    while (p->current.type == CLIKE_TOKEN_BIT_OR) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = bitwiseXor(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* bitwiseXor(ParserClike *p) {
    ASTNodeClike *node = bitwiseAnd(p);
    while (p->current.type == CLIKE_TOKEN_BIT_XOR) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = bitwiseAnd(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* bitwiseAnd(ParserClike *p) {
    ASTNodeClike *node = equality(p);
    while (p->current.type == CLIKE_TOKEN_BIT_AND) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = equality(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* equality(ParserClike *p) {
    ASTNodeClike *node = relational(p);
    while (p->current.type == CLIKE_TOKEN_EQUAL_EQUAL || p->current.type == CLIKE_TOKEN_BANG_EQUAL) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = relational(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* relational(ParserClike *p) {
    ASTNodeClike *node = shift(p);
    while (p->current.type == CLIKE_TOKEN_LESS || p->current.type == CLIKE_TOKEN_LESS_EQUAL ||
           p->current.type == CLIKE_TOKEN_GREATER || p->current.type == CLIKE_TOKEN_GREATER_EQUAL) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = shift(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

// Shift expressions: handles '<<' and '>>' with lower precedence than additive
static ASTNodeClike* shift(ParserClike *p) {
    ASTNodeClike *node = additive(p);
    while (p->current.type == CLIKE_TOKEN_SHL || p->current.type == CLIKE_TOKEN_SHR) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = additive(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* additive(ParserClike *p) {
    ASTNodeClike *node = term(p);
    while (p->current.type == CLIKE_TOKEN_PLUS || p->current.type == CLIKE_TOKEN_MINUS) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = term(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* term(ParserClike *p) {
    ASTNodeClike *node = unary(p);
    while (p->current.type == CLIKE_TOKEN_STAR || p->current.type == CLIKE_TOKEN_SLASH || p->current.type == CLIKE_TOKEN_PERCENT) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *rhs = unary(p);
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, op);
        setLeftClike(bin, node);
        setRightClike(bin, rhs);
        node = bin;
    }
    return node;
}

static ASTNodeClike* unary(ParserClike *p) {
    if (p->current.type == CLIKE_TOKEN_MINUS || p->current.type == CLIKE_TOKEN_BANG || p->current.type == CLIKE_TOKEN_TILDE) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = unary(p);
        ASTNodeClike *node = newASTNodeClike(TCAST_UNOP, op);
        setLeftClike(node, right);
        return node;
    }
    if (p->current.type == CLIKE_TOKEN_STAR) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = unary(p);
        ASTNodeClike *node = newASTNodeClike(TCAST_DEREF, op);
        setLeftClike(node, right);
        return node;
    }
    if (p->current.type == CLIKE_TOKEN_BIT_AND) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *right = unary(p);
        ASTNodeClike *node = newASTNodeClike(TCAST_ADDR, op);
        setLeftClike(node, right);
        return node;
    }
    if (p->current.type == CLIKE_TOKEN_PLUS_PLUS || p->current.type == CLIKE_TOKEN_MINUS_MINUS) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *operand = unary(p);
        ClikeToken oneTok = op; oneTok.type = CLIKE_TOKEN_NUMBER; oneTok.lexeme = "1"; oneTok.length = 1; oneTok.int_val = 1LL;
        ASTNodeClike *one = newASTNodeClike(TCAST_NUMBER, oneTok);
        one->var_type = TYPE_INT32;
        ClikeToken opTok = op; opTok.type = (op.type == CLIKE_TOKEN_PLUS_PLUS) ? CLIKE_TOKEN_PLUS : CLIKE_TOKEN_MINUS; opTok.lexeme = (op.type == CLIKE_TOKEN_PLUS_PLUS)?"+":"-"; opTok.length = 1;
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, opTok);
        setLeftClike(bin, cloneASTClike(operand));
        setRightClike(bin, one);
        ClikeToken eqTok = op; eqTok.type = CLIKE_TOKEN_EQUAL; eqTok.lexeme = "="; eqTok.length = 1;
        ASTNodeClike *assign = newASTNodeClike(TCAST_ASSIGN, eqTok);
        setLeftClike(assign, operand);
        setRightClike(assign, bin);
        return assign;
    }
    if (p->current.type == CLIKE_TOKEN_SIZEOF) {
        ClikeToken op = p->current; advanceParser(p);
        ASTNodeClike *operand = NULL;
        if (p->current.type == CLIKE_TOKEN_LPAREN) {
            advanceParser(p);
            if (isTypeToken(p->current.type)) {
                ClikeToken type_tok = parseTypeToken(p);
                expectToken(p, CLIKE_TOKEN_RPAREN, ")");
                operand = newASTNodeClike(TCAST_IDENTIFIER, type_tok);
                operand->var_type = clikeTokenTypeToVarType(type_tok.type);
            } else {
                operand = expression(p);
                expectToken(p, CLIKE_TOKEN_RPAREN, ")");
            }
        } else {
            operand = unary(p);
        }
        ASTNodeClike *node = newASTNodeClike(TCAST_SIZEOF, op);
        setLeftClike(node, operand);
        return node;
    }
    return factor(p);
}

static ASTNodeClike* factor(ParserClike *p) {
    if (p->current.type == CLIKE_TOKEN_SPAWN) {
        return clikeSpawnStatement(p);
    }
    if (matchToken(p, CLIKE_TOKEN_LPAREN)) {
        if (isTypeToken(p->current.type)) {
            ClikeToken type_tok = parseTypeToken(p);
            expectToken(p, CLIKE_TOKEN_RPAREN, ")");
            ASTNodeClike *expr = unary(p);
            const char *fname = NULL;
            switch (type_tok.type) {
                case CLIKE_TOKEN_DOUBLE:
                case CLIKE_TOKEN_FLOAT:
                    fname = "real";
                    break;
                case CLIKE_TOKEN_INT:
                case CLIKE_TOKEN_LONG:
                case CLIKE_TOKEN_LONG_LONG:
                    fname = "trunc";
                    break;
                case CLIKE_TOKEN_CHAR:
                    fname = "chr";
                    break;
                default:
                    break;
            }
            if (fname) {
                ClikeToken callTok = type_tok;
                callTok.type = CLIKE_TOKEN_IDENTIFIER;
                callTok.lexeme = fname;
                callTok.length = (int)strlen(fname);
                ASTNodeClike *callNode = newASTNodeClike(TCAST_CALL, callTok);
                addChildClike(callNode, expr);
                return postfix(p, callNode);
            }
            return postfix(p, expr);
        }
        ASTNodeClike *expr = expression(p);
        expectToken(p, CLIKE_TOKEN_RPAREN, ")");
        return postfix(p, expr);
    }
    if (p->current.type == CLIKE_TOKEN_NUMBER || p->current.type == CLIKE_TOKEN_FLOAT_LITERAL || p->current.type == CLIKE_TOKEN_CHAR_LITERAL) {
        ClikeToken num = p->current; advanceParser(p);
        ASTNodeClike *n = newASTNodeClike(TCAST_NUMBER, num);
        n->var_type = literalTokenToVarType(num.type);
        return n;
    }
    if (p->current.type == CLIKE_TOKEN_STRING) {
        ClikeToken str = parseStringLiteral(p);
        ASTNodeClike *n = newASTNodeClike(TCAST_STRING, str);
        n->var_type = TYPE_STRING;
        return n;
    }
    if (p->current.type == CLIKE_TOKEN_IDENTIFIER) {
        ClikeToken ident = p->current; advanceParser(p);
        if (p->current.type == CLIKE_TOKEN_LPAREN) {
            return call(p, ident);
        }
        ASTNodeClike *idNode = newASTNodeClike(TCAST_IDENTIFIER, ident);
        return postfix(p, idNode);
    }
    fprintf(stderr, "Unexpected token %s at line %d\n", clikeTokenTypeToString(p->current.type), p->current.line);
    advanceParser(p);
    return newASTNodeClike(TCAST_NUMBER, p->current); // error node
}

static ASTNodeClike* postfix(ParserClike *p, ASTNodeClike *node) {
    if (!node) return NULL;
    while (1) {
        if (p->current.type == CLIKE_TOKEN_LBRACKET) {
            ClikeToken tok = p->current;
            ASTNodeClike *access = newASTNodeClike(TCAST_ARRAY_ACCESS, tok);
            setLeftClike(access, node);
            do {
                advanceParser(p);
                ASTNodeClike *index = expression(p);
                expectToken(p, CLIKE_TOKEN_RBRACKET, "]");
                addChildClike(access, index);
            } while (p->current.type == CLIKE_TOKEN_LBRACKET);
            node = access;
            continue;
        }
        if (p->current.type == CLIKE_TOKEN_ARROW) {
            ClikeToken arrow = p->current; advanceParser(p);
            ClikeToken field = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "field");
            ASTNodeClike *fieldId = newASTNodeClike(TCAST_IDENTIFIER, field);
            ASTNodeClike *member = newASTNodeClike(TCAST_MEMBER, arrow);
            setLeftClike(member, node);
            setRightClike(member, fieldId);
            node = member;
            while (p->current.type == CLIKE_TOKEN_LBRACKET) {
                ClikeToken tok = p->current;
                ASTNodeClike *access = newASTNodeClike(TCAST_ARRAY_ACCESS, tok);
                setLeftClike(access, node);
                do {
                    advanceParser(p);
                    ASTNodeClike *index = expression(p);
                    expectToken(p, CLIKE_TOKEN_RBRACKET, "]");
                    addChildClike(access, index);
                } while (p->current.type == CLIKE_TOKEN_LBRACKET);
                node = access;
            }
            continue;
        }
        if (p->current.type == CLIKE_TOKEN_DOT) {
            ClikeToken dot = p->current; advanceParser(p);
            ClikeToken field = p->current; expectToken(p, CLIKE_TOKEN_IDENTIFIER, "field");
            ASTNodeClike *fieldId = newASTNodeClike(TCAST_IDENTIFIER, field);
            ASTNodeClike *member = newASTNodeClike(TCAST_MEMBER, dot);
            setLeftClike(member, node);
            setRightClike(member, fieldId);
            node = member;
            while (p->current.type == CLIKE_TOKEN_LBRACKET) {
                ClikeToken tok = p->current;
                ASTNodeClike *access = newASTNodeClike(TCAST_ARRAY_ACCESS, tok);
                setLeftClike(access, node);
                do {
                    advanceParser(p);
                    ASTNodeClike *index = expression(p);
                    expectToken(p, CLIKE_TOKEN_RBRACKET, "]");
                    addChildClike(access, index);
                } while (p->current.type == CLIKE_TOKEN_LBRACKET);
                node = access;
            }
            continue;
        }
        break;
    }
    if (p->current.type == CLIKE_TOKEN_PLUS_PLUS || p->current.type == CLIKE_TOKEN_MINUS_MINUS) {
        ClikeToken op = p->current; advanceParser(p);
        ClikeToken oneTok = op; oneTok.type = CLIKE_TOKEN_NUMBER; oneTok.lexeme = "1"; oneTok.length = 1; oneTok.int_val = 1LL;
        ASTNodeClike *one = newASTNodeClike(TCAST_NUMBER, oneTok);
        one->var_type = TYPE_INT32;
        ClikeToken opTok = op; opTok.type = (op.type == CLIKE_TOKEN_PLUS_PLUS)?CLIKE_TOKEN_PLUS:CLIKE_TOKEN_MINUS; opTok.lexeme = (op.type==CLIKE_TOKEN_PLUS_PLUS)?"+":"-"; opTok.length = 1;
        ASTNodeClike *bin = newASTNodeClike(TCAST_BINOP, opTok);
        setLeftClike(bin, cloneASTClike(node));
        setRightClike(bin, one);
        ClikeToken eqTok = op; eqTok.type = CLIKE_TOKEN_EQUAL; eqTok.lexeme = "="; eqTok.length = 1;
        ASTNodeClike *assign = newASTNodeClike(TCAST_ASSIGN, eqTok);
        setLeftClike(assign, node);
        setRightClike(assign, bin);
        node = assign;
    }
    return node;
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
