#ifndef TYPES_H
#define TYPES_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include "list.h"
#include "types.h"
#include <stdbool.h>

typedef struct FieldValue FieldValue;

typedef enum {
    TYPE_VOID,
    TYPE_INTEGER,
    TYPE_REAL,
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
    TYPE_NIL
} VarType;

typedef struct MStream {
    unsigned char *buffer;
    int size;
    int capacity;
} MStream;

// Definition of Type struct for enum metadata
typedef struct EnumType {
    char *name;         // Name of the enum type
    char **members;     // Array of member names
    int member_count;   // Number of members
} Type;

// Forward declaration of AST
typedef struct AST AST;

typedef struct ValueStruct {
    VarType type;
    Type *enum_meta; 
    union {
        long long i_val;
        double r_val;
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
    };
    AST *base_type_node; // AST node defining the type this pointer points to
                         // Needed for new(), dispose(), dereferencing type checks.

    char *filename;
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
    TOKEN_LESS, TOKEN_COLON, TOKEN_COMMA, TOKEN_PERIOD, TOKEN_ASSIGN,
    TOKEN_PLUS, TOKEN_MINUS, TOKEN_MUL, TOKEN_SLASH, TOKEN_LPAREN, TOKEN_RPAREN,
    TOKEN_LBRACKET, TOKEN_RBRACKET, TOKEN_DOTDOT, TOKEN_ARRAY, TOKEN_OF,
    TOKEN_AND, TOKEN_OR, TOKEN_TRUE, TOKEN_FALSE, TOKEN_NOT, TOKEN_CASE,
    TOKEN_USES, TOKEN_EOF, TOKEN_HEX_CONST, TOKEN_UNKNOWN, TOKEN_UNIT,
    TOKEN_INTERFACE, TOKEN_IMPLEMENTATION, TOKEN_INITIALIZATION, TOKEN_ENUM,
    TOKEN_IN, TOKEN_XOR, TOKEN_BREAK, TOKEN_OUT, TOKEN_SHL, TOKEN_SHR,
    TOKEN_SET,TOKEN_CARET, TOKEN_NIL
} TokenType;

typedef struct {
    TokenType type;
    char *value;
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
    AST_SUBRANGE,
    AST_USES_CLAUSE,
    AST_UNIT,
    AST_INTERFACE,
    AST_IMPLEMENTATION,
    AST_INITIALIZATION,
    AST_LIST,
    AST_ENUM_TYPE,
    AST_ENUM_VALUE,
    AST_SET,
    AST_ARRAY_LITERAL,
    AST_BREAK,
    AST_POINTER_TYPE, // <<< Represents a pointer type definition (^TypeName)
    AST_DEREFERENCE,  // <<< Represents dereferencing a pointer (ptr^)
    AST_NIL           // <<< Represents the 'nil' literal
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
