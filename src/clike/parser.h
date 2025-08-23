#ifndef CLIKE_PARSER_H
#define CLIKE_PARSER_H

#include "clike/lexer.h"
#include "clike/ast.h"

// Forward declaration of core AST for struct type metadata
typedef struct AST AST;

typedef struct {
    ClikeLexer lexer;
    ClikeToken current;
    ClikeToken next;
    char **imports;
    int import_count;
    int import_capacity;
} ParserClike;

void initParserClike(ParserClike *parser, const char *source);
void freeParserClike(ParserClike *parser);
ASTNodeClike* parseProgramClike(ParserClike *parser);

extern char **clike_imports;
extern int clike_import_count;

// Struct type registration and lookup
AST* clike_lookup_struct(const char *name);
void clike_register_struct(const char *name, AST *ast);
void clike_free_structs(void);

VarType clike_tokenTypeToVarType(ClikeTokenType t);
const char* clike_tokenTypeToTypeName(ClikeTokenType t);

#endif
