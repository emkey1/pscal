#ifndef PARSER_H
#define PARSER_H

#include "types.h"
#include "lexer.h"
#include "ast/ast.h"
#include "core/list.h"
#include <stdbool.h>
#include "compiler/bytecode.h"

typedef struct {
    Lexer *lexer;
    Token *current_token;
    const char *current_unit_name_context;
    List *dependency_paths;
} Parser;

AST *parsePointerType(Parser *parser);

void addProcedure(Parser *parser, AST *proc_decl, const char* unit_context_name, HashTable *proc_table);

// Full parser API
AST *buildProgramAST(Parser *parser, BytecodeChunk* chunk);
AST *block(Parser *parser);
AST *declarations(Parser *parser, bool in_interface);
AST *constDeclaration(Parser *parser);
AST *typeSpecifier(Parser *parser, int allowAnonymous);
AST *typeDeclaration(Parser *parser);
AST *variable(Parser *parser);
AST *varDeclaration(Parser *parser, bool isGlobal);
AST *procedureDeclaration(Parser *parser, bool in_interface);
AST *functionDeclaration(Parser *parser, bool in_interface);
AST *paramList(Parser *parser);
AST *compoundStatement(Parser *parser);
AST *statementList(Parser *parser);
AST *statement(Parser *parser);
AST *assignmentStatement(Parser *parser, AST *parsedLValue); 
AST *lvalue(Parser *parser);
AST *procedureCall(Parser *parser);
AST *ifStatement(Parser *parser);
AST *whileStatement(Parser *parser);
AST *caseStatement(Parser *parser);
AST *repeatStatement(Parser *parser);
AST *forStatement(Parser *parser);
AST *writelnStatement(Parser *parser);
AST *writeStatement(Parser *parser);
AST *readStatement(Parser *parser);
AST *readlnStatement(Parser *parser);
AST *exprList(Parser *parser);
AST *expr(Parser *parser);
AST *pascalTerm(Parser *parser);
AST *unitParser(Parser *parser, int recursion_depth, const char* unit_name_being_parsed, BytecodeChunk* chunk);
AST *enumDeclaration(Parser *parser);
AST *spawnStatement(Parser *parser);
AST *joinStatement(Parser *parser);

// Other stuff
void errorParser(Parser *parser, const char *msg);
AST *lookupType(const char *name);
void insertType(const char *name, AST *typeAST);
void eatInternal(Parser *parser, TokenType type);
AST *parseArrayInitializer(Parser *parser);
Token *peekToken(Parser *parser);

AST *expression(Parser *parser);        // New top-level expression parser
AST *simpleExpression(Parser *parser);  // Handles additive ops
AST *pascalTerm(Parser *parser);              // Handles multiplicative ops (modified)
AST *factor(Parser *parser);            // Handles primaries, NOT, parens (modified)
AST *parseSetConstructor(Parser *parser);
AST *parseWriteArguments(Parser *parser);

#ifdef DEBUG

// Declaration for the debug wrapper (defined in parser.c)
// Not strictly needed here if only called via macro below, but good practice
void eatDebugWrapper(Parser *parser_ptr, TokenType expected_token_type, const char* func_name);

// Redefine 'eat' macro to call the debug wrapper
#define eat(parser_ptr, expected_token_type) eatDebugWrapper(parser_ptr, expected_token_type, __func__)

#else // If DEBUG is NOT defined

// Define 'eat' macro to call the internal implementation directly
#define eat(parser_ptr, expected_token_type) eatInternal(parser_ptr, expected_token_type) // <<< Use eatInternal

#endif // DEBUG

#endif
