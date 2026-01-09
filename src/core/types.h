// src/core/types.h
#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <stdint.h>
#include "list.h"
#include <stdbool.h>
#include "common/frontend_symbol_aliases.h"
#if defined(PSCAL_TARGET_IOS)
#include "ios/vproc.h"
#include "ios/vproc_stdio_shim.h"
#endif

// Default record size used for untyped files when RESET/REWRITE omit an
// explicit size. Turbo Pascal historically defaults to 128 bytes; mirror that
// so existing code that relies on the legacy behaviour keeps working.
#define PSCAL_DEFAULT_FILE_RECORD_SIZE 128

// Forward declaration of AST struct, as TypeEntry will use AST*
struct AST;

// Define TypeEntry here
typedef struct TypeEntry_s { // Use a named tag for robustness
    char *name;
    struct AST *typeAST; // Uses forward-declared struct AST
    struct TypeEntry_s *next;
} TypeEntry;

struct ValueStruct;
typedef struct FieldValue FieldValue;
struct Symbol_s;

typedef struct ClosureEnvPayload {
    uint32_t refcount;
    uint16_t slot_count;
    struct Symbol_s *symbol;
    struct ValueStruct **slots;
} ClosureEnvPayload;

typedef enum {
    TYPE_UNKNOWN = 0,
    TYPE_VOID,
    TYPE_INT32,
    TYPE_DOUBLE,
    TYPE_STRING,
    TYPE_CHAR,
    TYPE_RECORD,
    TYPE_FILE,
    TYPE_BYTE,
    TYPE_WORD,
    TYPE_ENUM,
    TYPE_ARRAY,
    TYPE_BOOLEAN,
    TYPE_MEMORYSTREAM,
    TYPE_SET,
    TYPE_POINTER,
    TYPE_INTERFACE,
    TYPE_CLOSURE,
    /* Extended integer and floating-point types */
    TYPE_INT8,
    TYPE_UINT8,
    TYPE_INT16,
    TYPE_UINT16,
    TYPE_UINT32,
    TYPE_INT64,
    TYPE_UINT64,
    TYPE_FLOAT,
    TYPE_LONG_DOUBLE,
    TYPE_NIL,
    TYPE_THREAD
} VarType;

/*
 * Backwards compatibility aliases.
 *
 * Pascal traditionally exposes INTEGER and REAL as its fundamental numeric
 * types.  The VM has been moving toward a more explicit naming scheme where
 * the underlying sizes are part of the type name (e.g. INT32 and DOUBLE).
 *
 * To avoid a massive churn throughout the existing front‑ends we simply map
 * the old identifiers to the new ones via macros.  This allows legacy code
 * that still uses TYPE_INTEGER/TYPE_REAL to compile unchanged while the rest
 * of the system can reason about the new INT32/DOUBLE symbols.
 */
#define TYPE_INTEGER TYPE_INT32
#define TYPE_REAL    TYPE_DOUBLE

typedef struct MStream {
    unsigned char *buffer;
    int size;
    int capacity;
    int refcount;
} MStream;

// Definition of Type struct for enum metadata
typedef struct EnumType {
    char *name;         // Name of the enum type
    char **members;     // Array of member names
    int member_count;   // Number of members
} Type;

// Forward declaration of AST
typedef struct AST AST;

typedef struct RealValue { float f32_val; double d_val; long double r_val; } RealValue;

typedef struct ValueStruct {
    VarType type;
    Type *enum_meta;
    long long i_val;
    unsigned long long u_val;
    RealValue real;
    union {
        char *s_val;
        int c_val;
        FieldValue *record_val;
        FILE *f_val;
        struct ValueStruct *array_val;
        MStream *mstream;
        struct {
            char *enum_name; // Name of the enumerated type
            int ordinal;     // Ordinal value
        } enum_val;
        struct ValueStruct *ptr_val; // Pointer to another Value (for heap data)
        struct {
            uint32_t entry_offset;
            struct Symbol_s *symbol;
            ClosureEnvPayload *env;
        } closure;
        struct {
            struct AST *type_def;
            ClosureEnvPayload *payload;
        } interface;
    };
    uint8_t *array_raw;
    bool array_is_packed;
    AST *base_type_node; // AST node defining the type this pointer points to
                         // Needed for new(), dispose(), dereferencing type checks.

    char *filename;
    int record_size;      // Active record size for untyped file operations
    bool record_size_explicit; // Whether the record size was explicitly requested
    int lower_bound;    // For single-dimensional arrays
    int upper_bound;    // For single-dimensional arrays
    int max_length;     // For fixed length strings (text: string[100];)
    VarType element_type;
    int dimensions;       // number of dimensions (e.g. 2 for [1..2, 1..3])
    int *lower_bounds;    // array of lower bounds for each dimension
    int *upper_bounds;    // array of upper bounds for each dimension
    AST *element_type_def; // AST node defining the element type
    struct {
        int set_size;
        long long *set_values; // Store ordinal values (int/char)
        // Potentially add base_type if needed later VarType set_base_type;
    } set_val;
} Value;

/* Helpers to initialise numeric fields consistently. */
#define SET_INT_VALUE(dest, val) \
    do { (dest)->i_val = (long long)(val); (dest)->u_val = (unsigned long long)(val); } while(0)
#define SET_REAL_VALUE(dest, val) \
    do { (dest)->real.r_val = (long double)(val); (dest)->real.d_val = (double)(dest)->real.r_val; \
         (dest)->real.f32_val = (float)(dest)->real.r_val; } while(0)

typedef struct FieldValue {
    char *name;
    struct ValueStruct value;
    struct FieldValue *next;
} FieldValue;

typedef enum {
    TOKEN_PROGRAM, TOKEN_VAR, TOKEN_BEGIN, TOKEN_END, TOKEN_IF, TOKEN_THEN,
    TOKEN_ELSE, TOKEN_WHILE, TOKEN_DO, TOKEN_FOR, TOKEN_TO, TOKEN_DOWNTO,
    TOKEN_REPEAT, TOKEN_UNTIL, TOKEN_PROCEDURE, TOKEN_FUNCTION, TOKEN_CONST,
    TOKEN_TYPE, TOKEN_WRITE, TOKEN_WRITELN, TOKEN_READ, TOKEN_READLN,
    TOKEN_INT_DIV, TOKEN_MOD, TOKEN_RECORD, TOKEN_IDENTIFIER, TOKEN_INTEGER_CONST,
    TOKEN_REAL_CONST, TOKEN_STRING_CONST, TOKEN_SEMICOLON, TOKEN_GREATER,
    TOKEN_GREATER_EQUAL, TOKEN_EQUAL, TOKEN_NOT_EQUAL, TOKEN_LESS_EQUAL,
    TOKEN_LESS, TOKEN_COLON, TOKEN_QUESTION, TOKEN_COMMA, TOKEN_PERIOD, TOKEN_ASSIGN,
    TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_MUL, TOKEN_SLASH, TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACKET, TOKEN_RBRACKET, TOKEN_DOTDOT, TOKEN_ARRAY, TOKEN_AS, TOKEN_OF,
    TOKEN_AND, TOKEN_OR, TOKEN_TRUE, TOKEN_FALSE, TOKEN_NOT, TOKEN_CASE,
    TOKEN_USES, TOKEN_EOF, TOKEN_HEX_CONST, TOKEN_UNKNOWN, TOKEN_UNIT,
    TOKEN_INTERFACE, TOKEN_IMPLEMENTATION, TOKEN_INITIALIZATION, TOKEN_ENUM,
    TOKEN_IN, TOKEN_IS, TOKEN_XOR, TOKEN_BREAK, TOKEN_RETURN, TOKEN_OUT, TOKEN_SHL, TOKEN_SHR,
    TOKEN_SET, TOKEN_POINTER, TOKEN_CARET, TOKEN_NIL, TOKEN_INLINE, TOKEN_FORWARD, TOKEN_SPAWN, TOKEN_JOIN,
    TOKEN_AT, TOKEN_LABEL, TOKEN_GOTO
} TokenType;

typedef struct {
    TokenType type;
    char *value;
    size_t length;
    int line;
    int column;
    bool is_char_code;
} Token;

/* =======================
   AST DEFINITIONS & HELPERS
   ======================= */
typedef enum {
    AST_NOOP,
    AST_PROGRAM,
    AST_BLOCK,
    AST_CONST_DECL,
    AST_TYPE_DECL,
    AST_VAR_DECL,
    AST_ASSIGN,
    AST_BINARY_OP,
    AST_UNARY_OP,
    AST_TERNARY,
    AST_NUMBER,
    AST_STRING,
    AST_VARIABLE,
    AST_COMPOUND,
    AST_IF,
    AST_WHILE,
    AST_REPEAT,
    AST_FOR_TO,
    AST_FOR_DOWNTO,
    AST_WRITELN,
    AST_WRITE,
    AST_READLN,
    AST_READ,
    AST_RETURN,
    AST_EXPR_STMT,
    AST_PROCEDURE_DECL,
    AST_PROCEDURE_CALL,
    AST_FUNCTION_DECL,
    AST_CASE,
    AST_CASE_BRANCH,
    AST_RECORD_TYPE,
    AST_FIELD_ACCESS,
    AST_ARRAY_TYPE,
    AST_ARRAY_ACCESS,
    AST_BOOLEAN,
    AST_FORMATTED_EXPR,
    AST_TYPE_REFERENCE,
    AST_TYPE_IDENTIFIER, // Added: Represents a simple type identifier like "integer" or "MyCustomType"
    AST_TYPE_ASSERT,
    AST_SUBRANGE,
    AST_USES_CLAUSE,
    AST_IMPORT,
    AST_UNIT,
    AST_MODULE,
    AST_INTERFACE,
    AST_IMPLEMENTATION,
    AST_INITIALIZATION,
    AST_LIST,
    AST_ENUM_TYPE,
    AST_ENUM_VALUE,
    AST_SET,
    AST_ARRAY_LITERAL,
    AST_BREAK,
    AST_CONTINUE,
    AST_THREAD_SPAWN,
    AST_THREAD_JOIN,
    AST_POINTER_TYPE,
    AST_PROC_PTR_TYPE,
    AST_DEREFERENCE,
    AST_ADDR_OF,
    AST_NIL,
    AST_NEW,
    AST_MATCH,
    AST_MATCH_BRANCH,
    AST_PATTERN_BINDING,
    AST_TRY,
    AST_CATCH,
    AST_THROW,
    AST_LABEL_DECL,
    AST_LABEL,
    AST_GOTO
} ASTNodeType;

// Define the function pointer type for built-in handlers
typedef Value (*BuiltinHandler)(AST *node);

// Structure to map built-in names to handlers
typedef struct {
    const char *name;       // Lowercase name of the built-in
    BuiltinHandler handler; // Pointer to the C function implementation
} BuiltinMapping;

const char *varTypeToString(VarType type);
const char *tokenTypeToString(TokenType type);
const char *astTypeToString(ASTNodeType type);

// Function prototypes
void setTypeValue(Value *val, VarType type);
VarType inferBinaryOpType(VarType left, VarType right);

#endif // TYPES_H
