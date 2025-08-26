#ifndef UTILS_H
#define UTILS_H
#include "parser.h"
#include "symbol.h"
#include "vm.h"

#define PASCAL_DEFAULT_FLOAT_PRECISION 6
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

// Bytecode related stuff
// Make sure Value and VarType are defined before these.
#define IS_BOOLEAN(value) ((value).type == TYPE_BOOLEAN)
#define AS_BOOLEAN(value) ((value).i_val != 0) // Assumes i_val stores 0 for false, 1 for true

// Also useful:
#define IS_INTEGER(value) (is_intlike_type((value).type))
#define AS_INTEGER(value) ((value).i_val)
#define IS_REAL(value)    (is_real_type((value).type))
#define AS_REAL(value)    ((value).r_val)
#define IS_STRING(value)  ((value).type == TYPE_STRING)
#define AS_STRING(value)  ((value).s_val)
#define IS_CHAR(value)    ((value).type == TYPE_CHAR)
#define AS_CHAR(value)    ((value).c_val)

// Helper array to map Pscal color codes 0-7 to ANSI base numbers (30-37 or 40-47)
// Pscal: 0=Black, 1=Blue,  2=Green, 3=Cyan, 4=Red, 5=Magenta, 6=Brown(Yellow), 7=LightGray(White)
// ANSI:  x0=Black,x1=Red, x2=Green,x3=Yellow,x4=Blue,x5=Magenta,x6=Cyan,x7=White
static const int pscalToAnsiBase[8] = {
    0, // Pscal Black   -> ANSI Black (30/40 + 0)
    4, // Pscal Blue    -> ANSI Blue  (30/40 + 4)
    2, // Pscal Green   -> ANSI Green (30/40 + 2)
    6, // Pscal Cyan    -> ANSI Cyan  (30/40 + 6)
    1, // Pscal Red     -> ANSI Red   (30/40 + 1)
    5, // Pscal Magenta -> ANSI Magenta(30/40 + 5)
    3, // Pscal Brown   -> ANSI Yellow(30/40 + 3) (Brown is often dark yellow)
    7  // Pscal LtGray  -> ANSI White (30/40 + 7)
};

static inline bool is_intlike_type(VarType t) {
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
            return true;
        default:
            return false;
    }
}

static inline bool is_real_type(VarType t) {
    switch (t) {
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            return true;
        default:
            return false;
    }
}

static inline bool is_ordinal_type(VarType t) {
    // Pascal ordinals: integer subranges, enumerations, char, boolean.
    // Here we treat INTEGER/BYTE/WORD/CHAR/ENUM (BOOLEAN optional) as ordinal.
    return is_intlike_type(t) || t == TYPE_CHAR || t == TYPE_ENUM /*|| t == TYPE_BOOLEAN*/;
}

static inline long long coerce_to_i64(const Value* v, VM* vm, const char* who) {
    switch (v->type) {
        case TYPE_UINT64:
        case TYPE_UINT32:
        case TYPE_UINT16:
        case TYPE_UINT8:
        case TYPE_WORD:
        case TYPE_BYTE:
            return (long long)v->u_val;
        case TYPE_INT64:
        case TYPE_INT32:
        case TYPE_INT16:
        case TYPE_INT8:
        case TYPE_BOOLEAN:
            return v->i_val;
        case TYPE_CHAR:
            return (unsigned char)v->c_val;
        default:
            runtimeError(vm, "Argument error: %s delta must be an ordinal, got %s.",
                         who, varTypeToString(v->type));
            return 0;
    }
}

#define IS_INTLIKE(v) (is_intlike_type((v).type))
#define IS_NUMERIC(v) (IS_INTLIKE(v) || is_real_type((v).type))

// Accessors (use your existing Value layout: i_val for INTEGER/BYTE/WORD/BOOLEAN)
static inline long long as_i64(Value v) {
    switch (v.type) {
        case TYPE_UINT64:
        case TYPE_UINT32:
        case TYPE_UINT16:
        case TYPE_UINT8:
        case TYPE_WORD:
        case TYPE_BYTE:
            return (long long)v.u_val;
        default:
            return v.i_val;
    }
}
static inline long double as_ld(Value v) {
    if (is_real_type(v.type)) {
        switch (v.type) {
            case TYPE_FLOAT:       return v.f32_val;
            case TYPE_DOUBLE:      return v.d_val;
            case TYPE_LONG_DOUBLE: return v.r_val;
            default:               return v.r_val;
        }
    }
    return (long double)as_i64(v);
}

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
Value makeReal(long double val);
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
Value makeValueForType(VarType type, AST *type_def, Symbol* context_symbol);

// Token
Token *newToken(TokenType type, const char *value, int line, int column);
Token *copyToken(const Token *orig_token);
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
void printValueToStream(Value v, FILE *stream);
int calculateArrayTotalSize(const Value* array_val);

// Unit Stuff
char *findUnitFile(const char *unit_name);
void linkUnit(AST *unit_ast, int recursion_depth);
Symbol *buildUnitSymbolTable(AST *interface_ast);
void freeUnitSymbolTable(Symbol *symbol_table);
bool isUnitDocumented(const char *unit_name);

// General helpers
// Helper function to map 0-15 to ANSI FG codes
int map16FgColorToAnsi(int pscalColorCode, bool isBold);
// Helper function to map 0-7 to ANSI BG codes
int map16BgColorToAnsi(int pscalColorCode);
bool applyCurrentTextAttributes(FILE* stream);
void resetTextAttributes(FILE* stream);

// Arrays
Value makeArrayND(int dimensions, int *lower_bounds, int *upper_bounds, VarType element_type, AST *type_def);
int computeFlatOffset(Value *array, int *indices);
Value makeCopyOfValue(const Value *src);

// Set operations
Value setUnion(Value setA, Value setB);
Value setDifference(Value setA, Value setB);
Value setIntersection(Value setA, Value setB);

#endif // UTILS_H
