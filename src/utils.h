#ifndef UTILS_H
#define UTILS_H
#include "parser.h"
#include "symbol.h"

/* =======================
   DEBUG MACROS & GLOBALS
   ======================= */
#ifdef DEBUG
    // In DEBUG builds, use the fprintf logic if dumpExec is enabled
    extern int dumpExec;   /* Global flag for execution debug dump */
    #define DEBUG_PRINT(...) do { if(dumpExec) fprintf(stderr, __VA_ARGS__); } while(0)

    // DEBUG_DUMP_AST will call the debugAST function
    #define DEBUG_DUMP_AST(node, indent) debugAST((node), (indent))
#else
    // In non-DEBUG builds, define DEBUG_PRINT to expand to nothing.
    // The compiler will simply remove any line like DEBUG_PRINT(...)
    #define DEBUG_PRINT(...)

    // DEBUG_DUMP_AST should still be defined as a no-op that takes the arguments
    #define DEBUG_DUMP_AST(node, indent) ((void)0)
#endif

#include "types.h"

const char *varTypeToString(VarType type);
const char *tokenTypeToString(TokenType type);
const char *astTypeToString(ASTNodeType type);

// Symbol table debugging/dumping functions.
void dumpSymbolTable(void);
void dumpSymbol(Symbol *sym);


MStream *createMStream(void);
FieldValue *copyRecord(FieldValue *orig);
FieldValue *createEmptyRecord(AST *recordType);
void freeFieldValue(FieldValue *fv);

// Value constructors
Value makeInt(long long val);
Value makeReal(double val);
Value makeByte(unsigned char val);
Value makeWord(unsigned int val);
Value makeNil(void);
// Value constructor for creating a Value representing a general pointer.
// Used by the 'new' builtin.
Value makePointer(void* address, AST* base_type_node); // <<< ADD THIS PROTOTYPE >>>
Value makeString(const char *val);
Value makeChar(char c);
Value makeBoolean(int b);
Value makeFile(FILE *f);
Value makeRecord(FieldValue *rec);
Value makeMStream(MStream *ms);
Value makeVoid(void);
Value makeValueForType(VarType type, AST *type_def); // This simplifies the code.  Transitioning to it

// Token
Token *newToken(TokenType type, const char *value);
Token *copyToken(const Token *orig);
void freeToken(Token *token);

// Misc
void freeProcedureTable(void);
void freeTypeTable(void);
int getTerminalSize(int *rows, int *cols);
void toLowerString(char *str);
void parseError(Parser *parser, const char *message);
void debugASTFile(AST *node);
Value makeEnum(const char *enum_name, int ordinal);
void freeValue(Value *v);

// Unit Stuff
char *findUnitFile(const char *unit_name);
void linkUnit(AST *unit_ast, int recursion_depth);
Symbol *buildUnitSymbolTable(AST *interface_ast);
void freeUnitSymbolTable(Symbol *symbol_table);

// Arrays
Value makeArrayND(int dimensions, int *lower_bounds, int *upper_bounds, VarType element_type, AST *type_def);




#endif // UTILS_H
