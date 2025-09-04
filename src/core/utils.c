#include "utils.h"
#include "Pascal/globals.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <ctype.h>
#include "ast/ast.h"
#include "Pascal/documented_units.h"
#include "compiler/compiler.h"
#include <stdio.h>
#include "symbol.h"
#include "types.h"
#include "builtin.h"
#include <sys/ioctl.h> // Make sure this is included
#include <unistd.h>    // For STDOUT_FILENO
#include <sys/stat.h>  // For stat
#include <limits.h>    // For PATH_MAX


const char *varTypeToString(VarType type) {
    switch (type) {
        case TYPE_VOID:         return "VOID";
        case TYPE_INT32:        return "INTEGER";
        case TYPE_DOUBLE:       return "REAL";
        case TYPE_STRING:       return "STRING";
        case TYPE_CHAR:         return "CHAR";
        case TYPE_RECORD:       return "RECORD";
        case TYPE_FILE:         return "FILE";
        case TYPE_BYTE:         return "BYTE";
        case TYPE_WORD:         return "WORD";
        case TYPE_ENUM:         return "ENUM";
        case TYPE_ARRAY:        return "ARRAY";
        case TYPE_BOOLEAN:      return "BOOLEAN";
        case TYPE_MEMORYSTREAM: return "MEMORY_STREAM";
        case TYPE_SET:          return "SET";
        case TYPE_POINTER:      return "POINTER";
        case TYPE_INT8:         return "INT8";
        case TYPE_UINT8:        return "UINT8";
        case TYPE_INT16:        return "INT16";
        case TYPE_UINT16:       return "UINT16";
        case TYPE_UINT32:       return "UINT32";
        case TYPE_INT64:        return "INT64";
        case TYPE_UINT64:       return "UINT64";
        case TYPE_FLOAT:        return "REAL";
        case TYPE_LONG_DOUBLE:  return "LONG_DOUBLE";
        case TYPE_NIL:          return "NIL";
        case TYPE_THREAD:       return "THREAD";
        default:                return "UNKNOWN_VAR_TYPE";
    }
}

const char *tokenTypeToString(TokenType type) {
    static char unknown_buf[32];
    switch (type) {
        case TOKEN_PROGRAM:       return "PROGRAM";
        case TOKEN_VAR:           return "VAR";
        case TOKEN_BEGIN:         return "BEGIN";
        case TOKEN_END:           return "END";
        case TOKEN_IF:            return "IF";
        case TOKEN_THEN:          return "THEN";
        case TOKEN_ELSE:          return "ELSE";
        case TOKEN_WHILE:         return "WHILE";
        case TOKEN_DO:            return "DO";
        case TOKEN_FOR:           return "FOR";
        case TOKEN_TO:            return "TO";
        case TOKEN_DOWNTO:        return "DOWNTO";
        case TOKEN_REPEAT:        return "REPEAT";
        case TOKEN_UNTIL:         return "UNTIL";
        case TOKEN_PROCEDURE:     return "PROCEDURE";
        case TOKEN_FUNCTION:      return "FUNCTION";
        case TOKEN_CONST:         return "CONST";
        case TOKEN_TYPE:          return "TYPE";
        case TOKEN_WRITE:         return "WRITE";
        case TOKEN_WRITELN:       return "WRITELN";
        case TOKEN_READ:          return "READ";
        case TOKEN_READLN:        return "READLN";
        case TOKEN_INT_DIV:       return "DIV";
        case TOKEN_MOD:           return "MOD";
        case TOKEN_RECORD:        return "RECORD";
        case TOKEN_IDENTIFIER:    return "IDENTIFIER";
        case TOKEN_INTEGER_CONST: return "INTEGER_CONST";
        case TOKEN_REAL_CONST:    return "REAL_CONST";
        case TOKEN_STRING_CONST:  return "STRING_CONST";
        case TOKEN_SEMICOLON:     return "SEMICOLON";
        case TOKEN_GREATER:       return "GREATER";
        case TOKEN_GREATER_EQUAL: return "GREATER_EQUAL";
        case TOKEN_EQUAL:         return "EQUAL";
        case TOKEN_NOT_EQUAL:     return "NOT_EQUAL";
        case TOKEN_LESS_EQUAL:    return "LESS_EQUAL";
        case TOKEN_LESS:          return "LESS";
        case TOKEN_COLON:         return "COLON";
        case TOKEN_COMMA:         return "COMMA";
        case TOKEN_PERIOD:        return "PERIOD";
        case TOKEN_ASSIGN:        return "ASSIGN";
        case TOKEN_PLUS:          return "PLUS";
        case TOKEN_MINUS:         return "MINUS";
        case TOKEN_MUL:           return "MUL";
        case TOKEN_SLASH:         return "SLASH";
        case TOKEN_LPAREN:        return "LPAREN";
        case TOKEN_RPAREN:        return "RPAREN";
        case TOKEN_LBRACKET:      return "LBRACKET";
        case TOKEN_RBRACKET:      return "RBRACKET";
        case TOKEN_DOTDOT:        return "DOTDOT";
        case TOKEN_ARRAY:         return "ARRAY";
        case TOKEN_OF:            return "OF";
        case TOKEN_AND:           return "AND";
        case TOKEN_OR:            return "OR";
        case TOKEN_SHL:           return "SHL";
        case TOKEN_SHR:           return "SHR";
        case TOKEN_TRUE:          return "TRUE";
        case TOKEN_FALSE:         return "FALSE";
        case TOKEN_NOT:           return "NOT";
        case TOKEN_CASE:          return "CASE";
        case TOKEN_USES:          return "USES";
        case TOKEN_EOF:           return "EOF";
        case TOKEN_HEX_CONST:     return "HEX_CONST";
        case TOKEN_UNKNOWN:       return "UNKNOWN";
        case TOKEN_UNIT:          return "UNIT";
        case TOKEN_INTERFACE:     return "INTERFACE";
        case TOKEN_IMPLEMENTATION:return "IMPLEMENTATION";
        case TOKEN_INITIALIZATION:return "INITIALIZATION";
        case TOKEN_IN:            return "IN";
        case TOKEN_BREAK:         return "BREAK";
        case TOKEN_OUT:           return "OUT";
        case TOKEN_SET:           return "SET";
        case TOKEN_CARET:         return "CARET";
        case TOKEN_NIL:           return "NIL";
        case TOKEN_INLINE:       return "INLINE";
        case TOKEN_SPAWN:        return "SPAWN";
        case TOKEN_JOIN:         return "JOIN";
        case TOKEN_AT:           return "AT";
        default:
            // Create a small buffer to handle potentially large unknown enum values
            // Although, this function should ideally cover all defined TokenType values.
            // If an unknown value appears, it indicates a potential issue elsewhere.
            snprintf(unknown_buf, sizeof(unknown_buf), "INVALID_TOKEN (%d)", type);
            return unknown_buf;
    }
}

const char *astTypeToString(ASTNodeType type) {
    switch (type) {
        case AST_NOOP:           return "NOOP";
        case AST_PROGRAM:        return "PROGRAM";
        case AST_BLOCK:          return "BLOCK";
        case AST_CONST_DECL:     return "CONST_DECL";
        case AST_TYPE_DECL:      return "TYPE_DECL";
        case AST_VAR_DECL:       return "VAR_DECL";
        case AST_ASSIGN:         return "ASSIGN";
        case AST_BINARY_OP:      return "BINARY_OP";
        case AST_UNARY_OP:       return "UNARY_OP";
        case AST_NUMBER:         return "NUMBER";
        case AST_STRING:         return "STRING";
        case AST_VARIABLE:       return "VARIABLE";
        case AST_COMPOUND:       return "COMPOUND";
        case AST_IF:             return "IF";
        case AST_WHILE:          return "WHILE";
        case AST_REPEAT:         return "REPEAT";
        case AST_FOR_TO:         return "FOR_TO";
        case AST_FOR_DOWNTO:     return "FOR_DOWNTO";
        case AST_WRITELN:        return "WRITELN";
        case AST_WRITE:          return "WRITE";
        case AST_READLN:         return "READLN";
        case AST_READ:           return "READ";
        case AST_PROCEDURE_DECL: return "PROCEDURE_DECL";
        case AST_PROCEDURE_CALL: return "PROCEDURE_CALL";
        case AST_FUNCTION_DECL:  return "FUNCTION_DECL";
        case AST_CASE:           return "CASE";
        case AST_CASE_BRANCH:    return "CASE_BRANCH";
        case AST_RECORD_TYPE:    return "RECORD_TYPE";
        case AST_FIELD_ACCESS:   return "FIELD_ACCESS";
        case AST_ARRAY_TYPE:     return "ARRAY_TYPE";
        case AST_ARRAY_ACCESS:   return "ARRAY_ACCESS";
        case AST_BOOLEAN:        return "BOOLEAN";
        case AST_FORMATTED_EXPR: return "FORMATTED_EXPR";
        case AST_TYPE_REFERENCE: return "TYPE_REFERENCE";
        case AST_SUBRANGE:       return "SUBRANGE";
        case AST_USES_CLAUSE:    return "USES_CLAUSE";
        case AST_UNIT:           return "UNIT";
        case AST_INTERFACE:      return "INTERFACE";
        case AST_IMPLEMENTATION: return "IMPLEMENTATION";
        case AST_INITIALIZATION: return "INITIALIZATION";
        case AST_LIST:           return "LIST";
        case AST_ENUM_TYPE:      return "TYPE_ENUM";
        case AST_ENUM_VALUE:     return "ENUM_VALUE";
        case AST_SET:            return "SET";
        case AST_ARRAY_LITERAL:  return "ARRAY_LITERAL";
        case AST_BREAK:          return "BREAK";
        case AST_THREAD_SPAWN:   return "THREAD_SPAWN";
        case AST_THREAD_JOIN:    return "THREAD_JOIN";
        case AST_POINTER_TYPE:   return "POINTER_TYPE";
        case AST_PROC_PTR_TYPE:  return "PROC_PTR_TYPE";
        case AST_DEREFERENCE:    return "DEREFERENCE";
        case AST_ADDR_OF:        return "ADDR_OF";
        case AST_NIL:            return "NIL";
        default:                 return "UNKNOWN_AST_TYPE";
    }
}

MStream *createMStream(void) {
    MStream *ms = malloc(sizeof(MStream));
    if (!ms) {
        fprintf(stderr, "Memory allocation error in create_memory_stream\n");
        EXIT_FAILURE_HANDLER();
    }
    ms->buffer = NULL;
    ms->size = 0;
    ms->capacity = 0;
    return ms;
}

FieldValue *copyRecord(FieldValue *orig) {
    if (!orig) return NULL;
    FieldValue *new_head = NULL, **ptr = &new_head;
    for (FieldValue *curr = orig; curr != NULL; curr = curr->next) {
        FieldValue *new_field = malloc(sizeof(FieldValue));
        if (!new_field) {
            fprintf(stderr, "Memory allocation error in copyRecord for new_field\n");
            freeFieldValue(new_head); // Free any previously allocated nodes
            EXIT_FAILURE_HANDLER();
            return NULL; // In case EXIT_FAILURE_HANDLER returns
        }
        new_field->name = strdup(curr->name);
        if (!new_field->name) {
            fprintf(stderr, "Memory allocation error in copyRecord for new_field->name\n");
            free(new_field);
            freeFieldValue(new_head);
            EXIT_FAILURE_HANDLER();
            return NULL;
        }

        // --- Recursively copy the field's value ---
        new_field->value = makeCopyOfValue(&curr->value); // Use makeCopyOfValue

        new_field->next = NULL;
        *ptr = new_field;
        ptr = &new_field->next;
    }
    return new_head;
}

FieldValue *createEmptyRecord(AST *recordType) {
    // Resolve type references if necessary
    if (recordType && recordType->type == AST_TYPE_REFERENCE) {
        // Look up the referenced type definition
        AST* resolvedType = lookupType(recordType->token->value);
        if (!resolvedType) {
             fprintf(stderr, "Error in createEmptyRecord: Could not resolve type reference '%s'.\n", recordType->token->value);
             return NULL;
        }
        recordType = resolvedType; // Use the resolved definition node
    }

    // Check if we have a valid RECORD_TYPE node
    if (!recordType || recordType->type != AST_RECORD_TYPE) {
        fprintf(stderr, "Error in createEmptyRecord: Invalid or NULL recordType node provided (Type: %s).\n",
                recordType ? astTypeToString(recordType->type) : "NULL");
        return NULL; // Return NULL explicitly on error
    }

    FieldValue *head = NULL, **ptr = &head; // Use pointer-to-pointer for easy list building

    // Iterate through the children of the RECORD_TYPE node (these should be VAR_DECLs for fields)
    for (int i = 0; i < recordType->child_count; i++) {
        AST *fieldDecl = recordType->children[i]; // Should be VAR_DECL for the field group

        // --- Robustness Check: Ensure fieldDecl is a valid VAR_DECL node ---
        if (!fieldDecl) {
             fprintf(stderr, "Warning: NULL field declaration node at index %d in createEmptyRecord.\n", i);
             continue; // Skip this invalid entry
        }
        if (fieldDecl->type != AST_VAR_DECL) {
             fprintf(stderr, "Warning: Expected VAR_DECL for field group at index %d in createEmptyRecord, found %s.\n",
                     i, astTypeToString(fieldDecl->type));
             continue; // Skip invalid entry
        }
        // ---

        VarType fieldType = fieldDecl->var_type; // Get the type enum for the field(s)
        AST *fieldTypeDef = fieldDecl->right; // Get the AST node defining the field's type

        // Iterate through the children of the VAR_DECL (these are the VARIABLE nodes for field names)
        for (int j = 0; j < fieldDecl->child_count; j++) {
            AST *varNode = fieldDecl->children[j]; // Should be VARIABLE node for the field name

            // --- Robustness Check: Ensure varNode and its token are valid ---
            if (!varNode || varNode->type != AST_VARIABLE || !varNode->token || !varNode->token->value) {
                 fprintf(stderr, "Warning: Invalid field variable node or token at index %d,%d in createEmptyRecord.\n", i, j);
                 continue; // Skip this invalid field name
            }
            // ---

            // Allocate memory for the FieldValue struct (holds name + value)
            FieldValue *fv = malloc(sizeof(FieldValue));
            if (!fv) {
                 fprintf(stderr, "FATAL: malloc failed for FieldValue in createEmptyRecord for field '%s'\n", varNode->token->value);
                 freeFieldValue(head); // Free any partially built list
                 EXIT_FAILURE_HANDLER();
            }

            // Duplicate the field name
            fv->name = strdup(varNode->token->value);
            if (!fv->name) {
                 fprintf(stderr, "FATAL: strdup failed for FieldValue name in createEmptyRecord for field '%s'\n", varNode->token->value);
                 free(fv); // Free the FieldValue struct itself
                 freeFieldValue(head);
                 EXIT_FAILURE_HANDLER();
            }

            // Recursively create the default value for this field's type
            fv->value = makeValueForType(fieldType, fieldTypeDef, NULL); // Relies on makeValueForType checks
            fv->next = NULL; // Initialize next pointer

            // Link this new FieldValue struct into the list
            *ptr = fv;
            ptr = &fv->next; // Advance the tail pointer
        }
    }
    return head; // Return the head of the linked list of fields
}


void freeFieldValue(FieldValue *fv) {
    FieldValue *current = fv;
    while (current) {
        FieldValue *next = current->next; // Store next pointer before freeing current node's contents
        if (current->name) {
            free(current->name); // Free the duplicated field name
        }
        // Recursively free the value stored in the field
        freeValue(&current->value);
        // Free the FieldValue struct itself
        free(current);
        current = next; // Move to the next node
    }
}

// Value constructors
Value makeInt(long long val) {
    Value v;
    memset(&v, 0, sizeof(Value)); // Initialize all fields to 0/NULL
    v.type = TYPE_INT32;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeReal(long double val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_DOUBLE;
    SET_REAL_VALUE(&v, val);
    return v;
}

Value makeFloat(float val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_FLOAT;
    SET_REAL_VALUE(&v, val);
    return v;
}

Value makeDouble(double val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_DOUBLE;
    SET_REAL_VALUE(&v, val);
    return v;
}

Value makeLongDouble(long double val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_LONG_DOUBLE;
    SET_REAL_VALUE(&v, val);
    return v;
}

Value makeInt8(int8_t val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_INT8;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeUInt8(uint8_t val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_UINT8;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeInt16(int16_t val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_INT16;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeUInt16(uint16_t val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_UINT16;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeUInt32(uint32_t val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_UINT32;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeInt64(long long val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_INT64;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeUInt64(unsigned long long val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_UINT64;
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeByte(unsigned char val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_BYTE;
    SET_INT_VALUE(&v, val);  // Store the byte in the integer field.
    return v;
}

Value makeWord(unsigned int val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_WORD;
    // Use i_val, ensuring it handles potential size differences if long long > unsigned int
    SET_INT_VALUE(&v, val);
    return v;
}

Value makeString(const char *val) {
    Value v;
    memset(&v, 0, sizeof(Value)); // Initialize all fields
    v.type = TYPE_STRING;
    v.max_length = -1; // Indicate dynamic string (no fixed limit relevant here)

    if (val != NULL) {
        v.s_val = strdup(val); // Use strdup for clean duplication
        if (!v.s_val) {
            fprintf(stderr, "FATAL: Memory allocation failed in makeString (strdup)\n");
            EXIT_FAILURE_HANDLER();
        }
    } else {
        // Handle NULL input -> create an empty string
        v.s_val = strdup("");
        if (!v.s_val) {
            fprintf(stderr, "FATAL: Memory allocation failed in makeString (strdup empty)\n");
            EXIT_FAILURE_HANDLER();
        }
    }
    return v;
}

Value makeChar(int c) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_CHAR;
    v.c_val = c;
    SET_INT_VALUE(&v, c); // Keep numeric fields in sync for ordinal ops
    v.max_length = 1; // Character has a fixed length of 1
    return v;
}

Value makeBoolean(int b) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_BOOLEAN;
    v.i_val = b ? 1 : 0; // Store as 0 or 1
    return v;
}

Value makeFile(FILE *f) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_FILE;
    v.f_val = f;
    v.filename = NULL; // Filename is associated via assign()
    return v;
}

Value makeRecord(FieldValue *rec) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_RECORD;
    v.record_val = rec; // Takes ownership of the FieldValue list
    return v;
}

Value makeArrayND(int dimensions, int *lower_bounds, int *upper_bounds, VarType element_type, AST *type_def) {
    Value v;
    memset(&v, 0, sizeof(Value)); // Initialize all fields
    v.type = TYPE_ARRAY;
    v.dimensions = dimensions;
    v.lower_bounds = NULL; // Allocate below
    v.upper_bounds = NULL; // Allocate below
    v.array_val = NULL;    // Allocate below
    v.element_type = element_type;
    v.element_type_def = type_def; // Store link to element type definition

    if (dimensions <= 0) {
         fprintf(stderr, "Warning: makeArrayND called with zero or negative dimensions.\n");
         return v; // Return initialized empty array struct
    }

    // Allocate bounds arrays
    v.lower_bounds = malloc(sizeof(int) * dimensions);
    v.upper_bounds = malloc(sizeof(int) * dimensions);
    if (!v.lower_bounds || !v.upper_bounds) {
        fprintf(stderr, "Memory allocation error for bounds in makeArrayND.\n");
        free(v.lower_bounds); // Free potentially allocated lower bounds
        EXIT_FAILURE_HANDLER();
    }

    // Calculate total size and copy bounds
    int total_size = 1;
    for (int i = 0; i < dimensions; i++) {
        v.lower_bounds[i] = lower_bounds[i];
        v.upper_bounds[i] = upper_bounds[i];
        int size_i = (upper_bounds[i] - lower_bounds[i] + 1);
        if (size_i <= 0) {
             fprintf(stderr, "Error: Invalid array dimension size (%d..%d) in makeArrayND.\n", lower_bounds[i], upper_bounds[i]);
             free(v.lower_bounds); free(v.upper_bounds);
             EXIT_FAILURE_HANDLER();
        }
        // Check for potential integer overflow when calculating total_size
        if (__builtin_mul_overflow(total_size, size_i, &total_size)) {
             fprintf(stderr, "Error: Array size exceeds limits in makeArrayND.\n");
             free(v.lower_bounds); free(v.upper_bounds);
             EXIT_FAILURE_HANDLER();
        }
    }

    // Allocate array for Value elements
    v.array_val = malloc(sizeof(Value) * total_size);
    if (!v.array_val) {
        fprintf(stderr, "Memory allocation error for array data in makeArrayND.\n");
        free(v.lower_bounds); free(v.upper_bounds);
        EXIT_FAILURE_HANDLER();
    }

    // Initialize each element with its default value
    for (int i = 0; i < total_size; i++) {
        // Pass the element type definition node for complex types like records
        v.array_val[i] = makeValueForType(element_type, type_def, NULL);
    }

    return v;
}

// Value constructor for the 'nil' literal.
// Creates a Value of type TYPE_NIL with a NULL pointer value.
Value makeNil(void) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_NIL; // <<< Set type to TYPE_NIL
    v.ptr_val = NULL; // A nil pointer's value is NULL
    v.base_type_node = NULL; // A nil pointer doesn't point to a specific base type definition node
    return v;
}

Value makeVoid(void) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_VOID;
    return v;
}

Value makeValueForType(VarType type, AST *type_def_param, Symbol* context_symbol) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = type;
    v.base_type_node = NULL; // Initialize

    // --- MODIFICATION: Use context_symbol to find type definition if not passed directly ---
    AST* node_to_inspect = type_def_param;
    if (!node_to_inspect && context_symbol) {
        node_to_inspect = context_symbol->type_def;
    }
    if (node_to_inspect && node_to_inspect->type == AST_TYPE_REFERENCE && node_to_inspect->right) {
        node_to_inspect = node_to_inspect->right;
    }
    // --- END MODIFICATION ---

    AST* actual_type_def = node_to_inspect;

    // If the resolved type definition is an enum, ensure the value type reflects that
    // and remember the enum's definition node for later metadata access.
    if (actual_type_def && actual_type_def->type == AST_ENUM_TYPE) {
        if (type != TYPE_ENUM) {
            type = TYPE_ENUM;
            v.type = TYPE_ENUM;
        }
        v.base_type_node = actual_type_def;
    }

    if (type == TYPE_POINTER) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG makeValueForType] Setting base type for POINTER. Processing structure starting at %p (Type: %s)\n",
                (void*)node_to_inspect, node_to_inspect ? astTypeToString(node_to_inspect->type) : "NULL");
        fflush(stderr);
        #endif

        AST* pointer_type_node = node_to_inspect;

        if (pointer_type_node && pointer_type_node->type == AST_TYPE_REFERENCE) {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG makeValueForType] Passed node is TYPE_REFERENCE ('%s'), following its right pointer (%p)\n",
                    pointer_type_node->token ? pointer_type_node->token->value : "?", (void*)pointer_type_node->right);
            fflush(stderr);
            #endif
            pointer_type_node = pointer_type_node->right;
        }

        if (pointer_type_node && pointer_type_node->type == AST_POINTER_TYPE) {
            v.base_type_node = pointer_type_node->right;
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG makeValueForType] -> Base type node set to %p (Type: %s, Token: '%s') from node %p\n",
                     (void*)v.base_type_node,
                     v.base_type_node ? astTypeToString(v.base_type_node->type) : "NULL",
                     (v.base_type_node && v.base_type_node->token) ? v.base_type_node->token->value : "N/A",
                     (void*)pointer_type_node);
             fflush(stderr);
             #endif
        } else if (pointer_type_node && pointer_type_node->type == AST_PROC_PTR_TYPE) {
             // Procedure pointer types: treat as generic pointer with unknown base; no warning.
             v.base_type_node = NULL;
        } else if (pointer_type_node) {
             // If a non-pointer AST node is provided (e.g., a simple type identifier),
             // treat it as the base type directly.
             v.base_type_node = pointer_type_node;
        } else {
             // Unknown pointer type shape; log only in debug builds to avoid noisy stderr in tests
             #ifdef DEBUG
             fprintf(stderr, "Warning: Failed to find POINTER_TYPE definition node when initializing pointer Value. Structure trace started from VAR_DECL->right at %p. Final node checked was %p (Type: %s).\n",
                     (void*)type_def_param,
                     (void*)pointer_type_node,
                     pointer_type_node ? astTypeToString(pointer_type_node->type) : "NULL");
             #endif
             v.base_type_node = NULL;
        }
    }

    switch(type) {
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_INT32:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
            SET_INT_VALUE(&v, 0);
            break;
        case TYPE_FLOAT:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            SET_REAL_VALUE(&v, 0.0L);
            break;
        case TYPE_STRING: {
            v.s_val = NULL;
            v.max_length = -1;
            long long parsed_len = -1;

            if (actual_type_def && actual_type_def->type == AST_VARIABLE && actual_type_def->token &&
                strcasecmp(actual_type_def->token->value, "string") == 0 && actual_type_def->right)
            {
                 AST* lenNode = actual_type_def->right;

                 if (lenNode->type == AST_NUMBER && lenNode->token && lenNode->token->type == TOKEN_INTEGER_CONST) {
                     parsed_len = atoll(lenNode->token->value);
                 }
                 else if (lenNode->type == AST_VARIABLE && lenNode->token && lenNode->token->value) {
                     const char *const_name = lenNode->token->value;
                     #ifdef DEBUG
                     fprintf(stderr, "[DEBUG makeValueForType] String length specified by identifier '%s'. Looking up constant...\n", const_name);
                     #endif
                     Symbol *constSym = lookupSymbol(const_name);

                    if (constSym && constSym->is_const && constSym->value && constSym->value->type == TYPE_INT32) {
                          parsed_len = constSym->value->i_val;
                          #ifdef DEBUG
                          fprintf(stderr, "[DEBUG makeValueForType] Found constant '%s' with value %lld.\n", const_name, parsed_len);
                          #endif
                     } else {
                          fprintf(stderr, "Warning: Identifier '%s' used for string length is not a defined integer constant. Using dynamic.\n", const_name);
                     }
                 }
                 else {
                     fprintf(stderr, "Warning: Fixed string length not constant integer or identifier. Using dynamic.\n");
                 }

                 if (parsed_len != -1) {
                      if (parsed_len > 0 && parsed_len <= 255) {
                          v.max_length = (int)parsed_len;
                          v.s_val = calloc(v.max_length + 1, 1);
                          if (!v.s_val) { fprintf(stderr, "FATAL: calloc failed for fixed string\n"); EXIT_FAILURE_HANDLER(); }
                          #ifdef DEBUG
                          fprintf(stderr, "[DEBUG makeValueForType] Allocated fixed string (max_length=%d).\n", v.max_length);
                          #endif
                      } else {
                          fprintf(stderr, "Warning: Fixed string length %lld invalid or too large. Using dynamic.\n", parsed_len);
                          v.max_length = -1;
                      }
                 }
            }

            if (v.max_length == -1 && !v.s_val) {
                 v.s_val = strdup("");
                 if (!v.s_val) { fprintf(stderr, "FATAL: strdup failed for dynamic string\n"); EXIT_FAILURE_HANDLER(); }
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG makeValueForType] Allocated dynamic string.\n");
                 #endif
            }
            break;
        }
        case TYPE_CHAR:    v.c_val = '\0'; v.max_length = 1; break;
        case TYPE_BOOLEAN: v.i_val = 0; break;
        case TYPE_FILE:    v.f_val = NULL; v.filename = NULL; break;
        case TYPE_RECORD:
             v.record_val = createEmptyRecord(node_to_inspect);
             break;
        case TYPE_ARRAY: {
            v.dimensions = 0;
            v.lower_bounds = NULL;
            v.upper_bounds = NULL;
            v.array_val = NULL;
            v.element_type = TYPE_VOID;
            v.element_type_def = NULL;

            AST* definition_node_for_array = node_to_inspect;

            if (definition_node_for_array && definition_node_for_array->type == AST_TYPE_REFERENCE) {
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG makeValueForType ARRAY] type_def_param is TYPE_REFERENCE ('%s'). Looking up actual type.\n",
                        definition_node_for_array->token ? definition_node_for_array->token->value : "?");
                #endif
                AST* resolved_type_ast = lookupType(definition_node_for_array->token->value);
                if (!resolved_type_ast) {
                     fprintf(stderr, "Error: Could not resolve array type reference '%s' in makeValueForType for array initialization.\n",
                             definition_node_for_array->token ? definition_node_for_array->token->value : "?");
                } else {
                    definition_node_for_array = resolved_type_ast;
                }
            }

            if (definition_node_for_array && definition_node_for_array->type == AST_ARRAY_TYPE) {
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG makeValueForType] Initializing ARRAY from (resolved) AST_ARRAY_TYPE node %p.\n", (void*)definition_node_for_array);
                 #endif

                 int dims = definition_node_for_array->child_count;
                 AST* elemTypeDefNode = definition_node_for_array->right;
                 VarType elemType = TYPE_VOID;

                 if(elemTypeDefNode) {
                     elemType = elemTypeDefNode->var_type;
                       if (elemType == TYPE_VOID) {
                             if (elemTypeDefNode->type == AST_VARIABLE && elemTypeDefNode->token) {
                                 const char *tn = elemTypeDefNode->token->value;
                                 if (strcasecmp(tn, "integer") == 0) elemType = TYPE_INT32;
                                 else if (strcasecmp(tn, "real") == 0) elemType = TYPE_DOUBLE;
                                 else if (strcasecmp(tn, "char") == 0) elemType = TYPE_CHAR;
                                 else if (strcasecmp(tn, "boolean") == 0) elemType = TYPE_BOOLEAN;
                                 else if (strcasecmp(tn, "byte") == 0) elemType = TYPE_BYTE;
                                 else if (strcasecmp(tn, "word") == 0) elemType = TYPE_WORD;
                                 else if (strcasecmp(tn, "string") == 0) elemType = TYPE_STRING;
                                 else {
                                     AST* userTypeDef = lookupType(tn);
                                     if (userTypeDef) elemType = userTypeDef->var_type;
                                     if (userTypeDef) elemTypeDefNode = userTypeDef;
                                 }
                             } else if (elemTypeDefNode->type == AST_RECORD_TYPE) {
                                elemType = TYPE_RECORD;
                             } else if (elemTypeDefNode->type == AST_ARRAY_TYPE) {
                                elemType = TYPE_ARRAY;
                             }
                       }
                 }

                 if (dims > 0 && elemType != TYPE_VOID) {
                     int *lbs = (int*)malloc(sizeof(int) * dims);
                     int *ubs = (int*)malloc(sizeof(int) * dims);
                     if (!lbs || !ubs) {
                         fprintf(stderr, "Memory allocation error for bounds in makeValueForType.\n");
                         if(lbs) free(lbs);
                         if(ubs) free(ubs);
                         EXIT_FAILURE_HANDLER();
                     }

                     bool bounds_ok = true;
                     for (int i = 0; i < dims; i++) {
                         AST *subrange = definition_node_for_array->children[i];
                         if (!subrange || subrange->type != AST_SUBRANGE || !subrange->left || !subrange->right) {
                             bounds_ok = false; break;
                         }

                         // --- MODIFICATION: Use evaluateCompileTimeValue instead of eval ---
                         Value low_val = evaluateCompileTimeValue(subrange->left);
                         Value high_val = evaluateCompileTimeValue(subrange->right);

                        if (low_val.type == TYPE_INT32 && high_val.type == TYPE_INT32) {
                             lbs[i] = (int)low_val.i_val;
                             ubs[i] = (int)high_val.i_val;
                         } else {
                             fprintf(stderr, "Runtime error: Array bounds must be integer constants for now. Dim %d has types %s..%s\n", i, varTypeToString(low_val.type), varTypeToString(high_val.type));
                             bounds_ok = false;
                         }
                         freeValue(&low_val);
                         freeValue(&high_val);
                         if (!bounds_ok || lbs[i] > ubs[i]) {
                             bounds_ok = false; break;
                         }
                     }

                     if (bounds_ok) {
                         v = makeArrayND(dims, lbs, ubs, elemType, elemTypeDefNode);
                     } else {
                         fprintf(stderr, "Error: Failed to initialize array in makeValueForType due to invalid or non-integer bounds.\n");
                     }
                     free(lbs);
                     free(ubs);
                 } else {
                     fprintf(stderr, "Warning: Invalid dimension count (%d) or element type (%s) for array in makeValueForType.\n", dims, varTypeToString(elemType));
                 }
            } else {
                 fprintf(stderr, "Warning: Cannot initialize array value. Type definition missing, not an array type, or could not be resolved. (Actual node type for definition: %s)\n",
                         definition_node_for_array ? astTypeToString(definition_node_for_array->type) : "NULL");
            }

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG makeValueForType - ARRAY CASE EXIT] Returning Value: type=%s, dimensions=%d\n", varTypeToString(v.type), v.dimensions);
            #endif
            break;
        }
        case TYPE_MEMORYSTREAM: v.mstream = createMStream(); break;
        case TYPE_ENUM:
             v.enum_val.ordinal = 0;
             v.enum_val.enum_name = (actual_type_def && actual_type_def->token && actual_type_def->token->value)
                                      ? strdup(actual_type_def->token->value)
                                      : strdup("<unknown_enum>");
             if (!v.enum_val.enum_name) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
             v.base_type_node = actual_type_def;
             break;
        case TYPE_SET:     v.set_val.set_size = 0; v.set_val.set_values = NULL; v.max_length = 0; break;
        case TYPE_POINTER:
            v.ptr_val = NULL;
            break;
        case TYPE_VOID:
            break;
        default:
            fprintf(stderr, "Warning: makeValueForType called with unhandled type %d (%s)\n", type, varTypeToString(type));
            break;
    }

    return v;
}

Value makeMStream(MStream *ms) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_MEMORYSTREAM;
    v.mstream = ms; // Takes ownership or shares pointer based on usage context
    return v;
}

// Value constructor for creating a Value representing a general pointer.
// Used by the 'new' builtin after memory allocation.
// Creates a Value of type TYPE_POINTER with a given memory address and base type link.
// @param address        The memory address the pointer points to (e.g., allocated by malloc).
// @param base_type_node The AST node defining the type being pointed to (e.g., the Integer node in ^Integer).
Value makePointer(void* address, AST* base_type_node) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_POINTER; // The type of the value is POINTER
    v.ptr_val = address;     // The actual memory address it points to
    v.base_type_node = base_type_node; // Link to the definition of the type being pointed to
    return v;
}


// Token
/* Create a new token */
Token *newToken(TokenType type, const char *value, int line, int column) { // Added line, column
    Token *token = malloc(sizeof(Token));
    if (!token) {
        fprintf(stderr, "Memory allocation error in newToken\n");
        EXIT_FAILURE_HANDLER();
    }
    token->type = type;
    token->value = value ? strdup(value) : NULL;
    if (value && !token->value) {
        fprintf(stderr, "Memory allocation error (strdup value) in newToken\n");
        free(token);
        EXIT_FAILURE_HANDLER();
    }
    token->line = line;     // <<< SET LINE
    token->column = column; // <<< SET COLUMN
    return token;
}

/* Copy a token */
Token *copyToken(const Token *orig_token) { // Renamed parameter to avoid conflict if any
    if (!orig_token) return NULL;
    Token *new_token = malloc(sizeof(Token));
    if (!new_token) { fprintf(stderr, "Memory allocation error in copyToken (Token struct)\n"); EXIT_FAILURE_HANDLER(); }
    
    new_token->type = orig_token->type;
    new_token->value = orig_token->value ? strdup(orig_token->value) : NULL;
    if (orig_token->value && !new_token->value) {
         fprintf(stderr, "Memory allocation error (strdup value) in copyToken\n");
         free(new_token);
         EXIT_FAILURE_HANDLER();
    }
    new_token->line = orig_token->line;
    new_token->column = orig_token->column; 
    return new_token;
}

/* Free a token */
void freeToken(Token *token) {
    if (!token) return;
    if (token->value) {
        free(token->value); // Free the duplicated string
        token->value = NULL; // Prevent double-free
    }
    free(token); // Free the token struct itself
}

void freeProcedureTable(void) {
    if (!procedure_table) {
        return;
    }
    DEBUG_PRINT("[DEBUG SYMBOL] Freeing Procedure HashTable at %p.\n", (void*)procedure_table);

    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        Symbol *current_sym = procedure_table->buckets[i]; // current_sym is Symbol*
        while (current_sym) {
            Symbol *next_sym = current_sym->next; // Use Symbol's 'next' field

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG FREE_PROC_TABLE] Freeing Symbol (routine) '%s' (AST @ %p type_def).\n",
                    current_sym->name ? current_sym->name : "?", (void*)current_sym->type_def);
            #endif
            
            if (current_sym->name) {
                free(current_sym->name);
                current_sym->name = NULL;
            }

            // The AST declaration is stored in type_def for Symbols in procedure_table
            if (current_sym->type_def) {
                // This AST node is a deep copy owned by this Symbol struct.
                freeAST(current_sym->type_def);
                current_sym->type_def = NULL;
            }
            
            // Note: current_sym->value should be NULL for procedure/function symbols
            // as they don't have a "value" in the variable sense. If it could be non-NULL,
            // it would need freeing: if (current_sym->value) { freeValue(current_sym->value); free(current_sym->value); }

            free(current_sym); // Free the Symbol struct itself
            
            current_sym = next_sym;
        }
        procedure_table->buckets[i] = NULL;
    }
    // free(procedure_table->buckets); // The buckets array is part of HashTable struct, not separately allocated
    free(procedure_table); // Free the HashTable struct itself
    procedure_table = NULL;
}

void freeTypeTable(void) {
    TypeEntry *entry = type_table;
    while (entry) {
        TypeEntry *next = entry->next;
        free(entry->name); // Free the duplicated type name
        // AST node (entry->typeAST) is freed separately by freeTypeTableASTNodes
        free(entry); // Free the TypeEntry struct itself
        entry = next;
    }
    type_table = NULL;
}

void freeValue(Value *v) {
    if (!v) return;

//#ifdef DEBUG
//    fprintf(stderr, "[DEBUG] freeValue called for Value* at %p, type=%s\n",
 //           (void*)v, varTypeToString(v->type));
//    fflush(stderr); // Ensure debug message is printed immediately
//#endif
    switch (v->type) {
        case TYPE_VOID:
        case TYPE_INT32:
        case TYPE_DOUBLE:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_NIL: // <<<< ADDED TYPE_NIL HERE
            // No heap data associated with the Value struct itself for these simple types.
            // For TYPE_POINTER, ptr_val itself is an address, not heap data owned by this Value.
            // The memory pointed *to* by a TYPE_POINTER is managed by new/dispose.
//#ifdef DEBUG
 //           fprintf(stderr, "[DEBUG]   No heap data to free for type %s directly within Value struct.\n", varTypeToString(v->type));
  //          fflush(stderr);
//#endif
            break;
        case TYPE_ENUM:
            if (v->enum_val.enum_name) {
//#ifdef DEBUG
 //               fprintf(stderr, "[DEBUG]   Attempting to free enum name '%s' at %p for Value* %p\n",
   //                     v->enum_val.enum_name, (void*)v->enum_val.enum_name, (void*)v);
  //              fflush(stderr);
//#endif
                free(v->enum_val.enum_name);
                v->enum_val.enum_name = NULL;
            } else {
//#ifdef DEBUG
 //               fprintf(stderr, "[DEBUG]   Enum name pointer is NULL for Value* %p, nothing to free.\n", (void*)v);
  //              fflush(stderr);
//#endif
            }
            break;
        case TYPE_POINTER:
            // For a Value struct of TYPE_POINTER, freeValue should NOT free the memory
            // that v->ptr_val points to. That's the job of dispose/FreeMem.
            // We only nullify the pointer here to indicate this Value struct no longer points.
            // The base_type_node is also part of the type definition, not data to be freed here.
#ifdef DEBUG
            fprintf(stderr, "[DEBUG]   Resetting ptr_val for POINTER Value* at %p. Base type node (%p) is preserved. Pointed-to memory NOT freed by this call.\n",
                    (void*)v, (void*)v->base_type_node);
            fflush(stderr);
#endif
            v->ptr_val = NULL;
            // v->base_type_node = NULL; // Generally, base_type_node should persist as it defines the pointer's *type*
            break;

        case TYPE_STRING:
            if (v->s_val) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]   Attempting to free string content at %p (value was '%s') for Value* %p\n",
                        (void*)v->s_val, v->s_val ? v->s_val : "<INVALID_OR_FREED>", (void*)v);
                fflush(stderr);
#endif
                free(v->s_val);
                v->s_val = NULL;
            } else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]   String content pointer is NULL for Value* %p, nothing to free.\n", (void*)v);
                fflush(stderr);
#endif
            }
            break;

        case TYPE_RECORD: {
            FieldValue *f = v->record_val;
#ifdef DEBUG
            fprintf(stderr, "[DEBUG]   Processing record fields for Value* at %p (record_val=%p)\n", (void*)v, (void*)f);
            fflush(stderr);
#endif
            while (f) {
                FieldValue *next = f->next;
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]     Freeing FieldValue* at %p (name='%s' @ %p) within Value* %p\n",
                        (void*)f, f->name ? f->name : "NULL", (void*)f->name, (void*)v);
                fflush(stderr);
#endif
                if (f->name) free(f->name);
                freeValue(&f->value); // Recursive call
                free(f);              // Free the FieldValue struct itself
                f = next;
            }
            v->record_val = NULL;
            break;
        }
        case TYPE_ARRAY: {
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]   Processing array for Value* at %p (array_val=%p)\n", (void*)v, (void*)v->array_val);
             fflush(stderr);
#endif
             if (v->array_val) {
                 int total = 1;
                 if(v->dimensions > 0 && v->lower_bounds && v->upper_bounds) {
                   for (int i = 0; i < v->dimensions; i++)
                       total *= (v->upper_bounds[i] - v->lower_bounds[i] + 1);
                 } else {
                   total = 0;
#ifdef DEBUG
                   fprintf(stderr, "[DEBUG]     Warning: Array bounds missing or zero dimensions for Value* %p.\n", (void*)v);
                   fflush(stderr);
#endif
                 }

                 for (int i = 0; i < total; i++) {
#ifdef DEBUG
                    fprintf(stderr, "[DEBUG]     Freeing array element %d for Value* %p\n", i, (void*)v);
                    fflush(stderr);
#endif
                    freeValue(&v->array_val[i]);
                 }
#ifdef DEBUG
                 fprintf(stderr, "[DEBUG]   Freeing array data buffer at %p for Value* %p\n", (void*)v->array_val, (void*)v);
                 fflush(stderr);
#endif
                 free(v->array_val);
             }
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]   Freeing array bounds at %p and %p for Value* %p\n", (void*)v->lower_bounds, (void*)v->upper_bounds, (void*)v);
             fflush(stderr);
#endif
             free(v->lower_bounds);
             free(v->upper_bounds);
             v->array_val = NULL;
             v->lower_bounds = NULL;
             v->upper_bounds = NULL;
             v->dimensions = 0; // Reset dimensions
             break;
        }
        case TYPE_FILE:
            if (v->f_val) {
                // This is a file handle. Close it if it's not NULL.
                fclose(v->f_val);
                // Set the pointer to NULL after closing to prevent accidental reuse.
                v->f_val = NULL;
            }
            break; // Break from the switch statement
        case TYPE_MEMORYSTREAM:
              if (v->mstream) {
#ifdef DEBUG
                  fprintf(stderr, "[DEBUG freeValue] Freeing MStream structure and its buffer for Value* %p. MStream* %p, Buffer* %p\n",
                          (void*)v, (void*)v->mstream, (void*)(v->mstream ? v->mstream->buffer : NULL));
                  fflush(stderr);
#endif
                  if (v->mstream->buffer) {
                      free(v->mstream->buffer);
                      v->mstream->buffer = NULL;
                  }
                  free(v->mstream);
                  v->mstream = NULL;
              } else {
#ifdef DEBUG
                  fprintf(stderr, "[DEBUG freeValue] MStream pointer is NULL for Value* %p, nothing to free.\n", (void*)v);
                  fflush(stderr);
#endif
              }
              break;
        case TYPE_SET: // Added case for freeing set values
            if (v->set_val.set_values) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]   Freeing set_val.set_values at %p for Value* %p\n", (void*)v->set_val.set_values, (void*)v);
                fflush(stderr);
#endif
                free(v->set_val.set_values);
                v->set_val.set_values = NULL;
            }
            v->set_val.set_size = 0;
            // v.max_length for sets was used for capacity tracking by addOrdinalToResultSet,
            // not a dynamically allocated string, so no free needed for max_length itself.
            break;
        // Add other types if they allocate memory pointed to by Value struct members
        default:
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]   Unhandled type %s in freeValue for Value* %p\n", varTypeToString(v->type), (void*)v);
             fflush(stderr);
#endif
             break;
    }
    // Optionally mark type as VOID after freeing contents, but this might mask issues
    // if the Value struct is reused without proper reinitialization.
    // v->type = TYPE_VOID;
}

void dumpSymbol(Symbol *sym) {
    if (!sym) return;

    printf("Name: %s, Type: %s", sym->name, varTypeToString(sym->type));

    if (sym->value) {
        printf(", Value: ");
        switch (sym->type) {
            case TYPE_INT32:
                printf("%lld", sym->value->i_val);
                break;
            case TYPE_FLOAT:
                printf("%f", sym->value->real.f32_val);
                break;
            case TYPE_DOUBLE:
                printf("%f", sym->value->real.d_val);
                break;
            case TYPE_LONG_DOUBLE:
                printf("%Lf", sym->value->real.r_val);
                break;
            case TYPE_STRING:
                printf("\"%s\"", sym->value->s_val ? sym->value->s_val : "(null)");
                break;
            case TYPE_CHAR:
                printf("'%c'", sym->value->c_val);
                break;
            case TYPE_BOOLEAN:
                printf("%s", sym->value->i_val ? "true" : "false");
                break;
            case TYPE_BYTE:
                printf("Byte %lld", sym->value->i_val);
                break;
            case TYPE_WORD:
                printf("Word %u", (unsigned int)sym->value->i_val);
                break;
            case TYPE_ENUM:
                printf("Enumerated Type '%s', Ordinal: %d", sym->value->enum_val.enum_name, sym->value->enum_val.ordinal);
                break;
            case TYPE_ARRAY: {
                printf("Array[");
                for (int i = 0; i < sym->value->dimensions; i++) {
                    printf("%d..%d", sym->value->lower_bounds[i], sym->value->upper_bounds[i]);
                    if (i < sym->value->dimensions - 1) {
                        printf(", ");
                    }
                }
                printf("] of %s", varTypeToString(sym->value->element_type));
                break;
            }
            case TYPE_RECORD: {
                printf("Record { ");
                FieldValue *fv = sym->value->record_val;
                while (fv) {
                    printf("%s: %s", fv->name, varTypeToString(fv->value.type));
                    if (fv->value.type == TYPE_ENUM) {
                        printf(" ('%s', Ordinal: %d)", fv->value.enum_val.enum_name, fv->value.enum_val.ordinal);
                    } else if (fv->value.type == TYPE_STRING) {
                        printf(" (\"%s\")", fv->value.s_val ? fv->value.s_val : "(null)");
                    }
                    fv = fv->next;
                    if (fv) {
                        printf(", ");
                    }
                }
                printf(" }");
                break;
            }
            case TYPE_FILE:
                printf("File (handle: %p)", (void *)sym->value->f_val);
                break;
            case TYPE_MEMORYSTREAM:
                printf("MStream (size: %d)", sym->value->mstream->size);
                break;
            case TYPE_NIL:
                 // A TYPE_NIL Value struct represents the absence of a pointer.
                 // It does not own any heap data itself (the ptr_val field is NULL).
                 // Therefore, there is nothing specific to free for a TYPE_NIL value.
                 // Just break and let the Value struct container potentially be freed by the caller.
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG]   Handling TYPE_NIL in freeValue - no heap data to free.\n");
                 #endif
                 break; // No dynamic memory specific to the NIL type to free
            default:
                printf("(not printed)");
                break;
        }
    } else {
        printf(", Value: (null)");
    }

    printf("\n");
}

/*
 * debug_ast - A simple wrapper that begins dumping at the root with zero indent.
 */
void debugASTFile(AST *node) {
    dumpAST(node, 0);
}

bool isUnitDocumented(const char *unit_name) {
    for (size_t i = 0; i < documented_units_count; i++) {
        if (strcmp(unit_name, documented_units[i]) == 0) {
            return true;
        }
    }
    return false;
}

char *findUnitFile(const char *unit_name) {
    // 1. Determine the library search path in a safe, writable buffer.
    char lib_path[PATH_MAX];
    const char *env_path = getenv("PASCAL_LIB_DIR");

    if (env_path != NULL && *env_path != '\0') {
        // An override path was provided, so use it.
        strncpy(lib_path, env_path, PATH_MAX - 1);
        lib_path[PATH_MAX - 1] = '\0'; // Ensure null-termination
    } else {
        // Fall back to the default hard-coded path.
        strncpy(lib_path, "/usr/local/pscal/pascal/lib", PATH_MAX - 1);
        lib_path[PATH_MAX - 1] = '\0';
    }

    // 2. Check if the resolved library directory actually exists.
    struct stat dir_info;
    if (stat(lib_path, &dir_info) != 0 || !S_ISDIR(dir_info.st_mode)) {
        fprintf(stderr, "Error: Pascal library directory not found. Searched path: %s\n", lib_path);
        EXIT_FAILURE_HANDLER();
    }

    // 3. Allocate enough space for the full file path.
    //    (path + '/' + unit_name + ".pl" + null)
    size_t required_len = strlen(lib_path) + 1 + strlen(unit_name) + 3 + 1;
    char *file_name = malloc(required_len);
    if (!file_name) {
        fprintf(stderr, "Memory allocation error in findUnitFile\n");
        EXIT_FAILURE_HANDLER();
    }

    // 4. Compose the full path using the resolved base path.
    snprintf(file_name, required_len, "%s/%s.pl", lib_path, unit_name);

    // 5. Check if the file exists and return it, otherwise clean up.
    if (access(file_name, F_OK) == 0) {
        return file_name; // Success: Caller takes ownership and must free() this memory.
    }

    free(file_name);
    return NULL; // Not found.
}

void linkUnit(AST *unit_ast, int recursion_depth) {
    if (!unit_ast) return;

    // The unit parser should have built a temporary (unit-scoped) symbol list.
    if (!unit_ast->symbol_table) {
        fprintf(stderr, "Error: Symbol table for unit is missing.\n");
        EXIT_FAILURE_HANDLER();
    }

    // Walk the unit's symbol list and merge ONLY variables/constants into globals.
    Symbol *unit_symbol = unit_ast->symbol_table;
    while (unit_symbol) {

        // Skip procedures/functions (these live in procedure_table and are handled elsewhere).
        bool is_routine_symbol =
            (unit_symbol->type_def &&
            (unit_symbol->type_def->type == AST_PROCEDURE_DECL ||
             unit_symbol->type_def->type == AST_FUNCTION_DECL));
        if (is_routine_symbol) {
            DEBUG_PRINT("[DEBUG] linkUnit: Skipping routine symbol '%s' (type %s) from unit interface.\n",
                        unit_symbol->name, varTypeToString(unit_symbol->type));
            unit_symbol = unit_symbol->next;
            continue;
        }

        // Already present in globals?
        Symbol *existing_global = lookupGlobalSymbol(unit_symbol->name);
        if (existing_global) {
            DEBUG_PRINT("[DEBUG] linkUnit: '%s' already exists globally.\n", unit_symbol->name);

            // If the unit provided a constant value, update the existing global
            // using a DEEP COPY so updateSymbol can free its temp safely.
            if (unit_symbol->is_const && unit_symbol->value) {
                DEBUG_PRINT("[DEBUG] linkUnit: Updating existing global const '%s' from unit.\n",
                            unit_symbol->name);
                Value dup = makeCopyOfValue(unit_symbol->value);  // deep copy
                updateSymbol(unit_symbol->name, dup);             // updateSymbol will free dup
                existing_global->is_const = true;
            }

            unit_symbol = unit_symbol->next;
            continue;
        }

        // Insert a fresh global (Value is default-initialized inside insertGlobalSymbol).
        DEBUG_PRINT("[DEBUG] linkUnit: Inserting global '%s' (type %s) from unit.\n",
                    unit_symbol->name, varTypeToString(unit_symbol->type));
        insertGlobalSymbol(unit_symbol->name, unit_symbol->type, unit_symbol->type_def);

        Symbol *g = lookupGlobalSymbol(unit_symbol->name);
        if (!g) {
            fprintf(stderr, "Internal Error: Failed to find global '%s' after insertion.\n",
                    unit_symbol->name);
            EXIT_FAILURE_HANDLER();
        }
        DEBUG_PRINT("[DEBUG] linkUnit: Successfully inserted '%s'.\n", g->name);

        // If the unit symbol is a constant with a value, copy that value into the global now.
        if (unit_symbol->is_const && unit_symbol->value) {
            DEBUG_PRINT("[DEBUG] linkUnit: Copying constant value for '%s'.\n", unit_symbol->name);
            Value dup = makeCopyOfValue(unit_symbol->value);  // deep copy
            updateSymbol(unit_symbol->name, dup);             // updateSymbol will free dup
            g->is_const = true;
        }
        // If the unit symbol is an initialized array *variable* in the interface (rare),
        // copy its initial value too (deep copy). Constants were handled above already.
        else if (unit_symbol->type == TYPE_ARRAY && unit_symbol->value) {
            DEBUG_PRINT("[DEBUG] linkUnit: Copying initial array value for '%s'.\n",
                        unit_symbol->name);
            Value dup = makeCopyOfValue(unit_symbol->value);  // deep copy
            updateSymbol(unit_symbol->name, dup);             // updateSymbol will free dup
        }

        // NOTE:
        // We intentionally do NOT perform additional manual per-type copying here.
        // updateSymbol(...) already handles all supported types (ENUM, SET, POINTER, etc.)
        // and takes ownership of the temporary deep-copied Value safely. Doing manual
        // re-copies after updateSymbol risks double-frees and is redundant.

        unit_symbol = unit_symbol->next;
    }

    // Done merging: free the temporary unit symbol list. Its Values are still owned by the unit
    // list and will be freed here; globals now own their own deep copies, so this is safe.
    if (unit_ast->symbol_table) {
        DEBUG_PRINT("[DEBUG] linkUnit: Freeing unit symbol table for '%s' at %p\n",
                    unit_ast->token ? unit_ast->token->value : "NULL",
                    (void*)unit_ast->symbol_table);
        freeUnitSymbolTable(unit_ast->symbol_table);
        unit_ast->symbol_table = NULL; // prevent double free when freeing the AST later
    }

    // Register types declared in the unit's interface (these ASTs are managed by the type table).
    AST *type_decl = unit_ast->right;
    while (type_decl && type_decl->type == AST_TYPE_DECL) {
        insertType(type_decl->token->value, type_decl->left);
        type_decl = type_decl->right;
    }

    // Add unqualified aliases for interface routines (procedure_table logic unchanged).
    AST *interface_compound_node = unit_ast->left;
    if (interface_compound_node && interface_compound_node->type == AST_COMPOUND) {
        DEBUG_PRINT("[DEBUG] linkUnit: Adding unqualified aliases for interface routines of unit '%s'.\n",
                    unit_ast->token ? unit_ast->token->value : "UNKNOWN_UNIT");

        for (int i = 0; i < interface_compound_node->child_count; i++) {
            AST *interface_decl_node = interface_compound_node->children[i];

            if (interface_decl_node && interface_decl_node->token &&
                (interface_decl_node->type == AST_PROCEDURE_DECL ||
                 interface_decl_node->type == AST_FUNCTION_DECL)) {

                const char* unq_orig = interface_decl_node->token->value;
                char unq_lower[MAX_ID_LENGTH + 1];
                strncpy(unq_lower, unq_orig, MAX_ID_LENGTH);
                unq_lower[MAX_ID_LENGTH] = '\0';
                toLowerString(unq_lower);

                const char* unit_name = unit_ast->token ? unit_ast->token->value : NULL;
                if (!unit_name) {
                    fprintf(stderr, "[ERROR] linkUnit: Cannot determine unit name for aliasing '%s'.\n", unq_orig);
                    continue;
                }

                char qualified_lower[MAX_ID_LENGTH * 2 + 2];
                snprintf(qualified_lower, sizeof(qualified_lower), "%s.%s", unit_name, unq_orig);
                toLowerString(qualified_lower);

                Symbol* qualified_proc_symbol = hashTableLookup(procedure_table, qualified_lower);
                if (qualified_proc_symbol && qualified_proc_symbol->type_def &&
                    qualified_proc_symbol->type_def != (AST*)0x1) {

                    Symbol* existing_unq = hashTableLookup(procedure_table, unq_lower);
                    if (!existing_unq) {
                        DEBUG_PRINT("[DEBUG] linkUnit: Adding unqualified alias '%s' -> '%s'.\n",
                                    unq_lower, qualified_lower);
                        Symbol* alias_sym = (Symbol*)calloc(1, sizeof(Symbol));
                        if (!alias_sym) { EXIT_FAILURE_HANDLER(); }
                        alias_sym->name = strdup(unq_lower);
                        alias_sym->is_alias = true;
                        alias_sym->real_symbol = qualified_proc_symbol;
                        /* Copy metadata so the alias behaves like the real symbol during compilation */
                        alias_sym->type = qualified_proc_symbol->type;
                        alias_sym->arity = qualified_proc_symbol->arity;
                        alias_sym->locals_count = qualified_proc_symbol->locals_count;
                        alias_sym->bytecode_address = qualified_proc_symbol->bytecode_address;
                        alias_sym->is_defined = qualified_proc_symbol->is_defined;
                        hashTableInsert(procedure_table, alias_sym);
                    } else {
                        /* Update existing placeholder with real implementation details */
                        existing_unq->is_alias = true;
                        existing_unq->real_symbol = qualified_proc_symbol;
                        existing_unq->type = qualified_proc_symbol->type;
                        existing_unq->arity = qualified_proc_symbol->arity;
                        existing_unq->locals_count = qualified_proc_symbol->locals_count;
                        existing_unq->bytecode_address = qualified_proc_symbol->bytecode_address;
                        existing_unq->is_defined = qualified_proc_symbol->is_defined;
                    }
                } else {
                    DEBUG_PRINT("[WARN] linkUnit: No implementation for '%s'; cannot alias '%s'.\n",
                                qualified_lower, unq_lower);
                }
            }
        }
    }
}


// buildUnitSymbolTable traverses the interface AST node and creates a symbol table
// containing all exported symbols (variables, procedures, functions, types) for the unit.
// buildUnitSymbolTable traverses the unit's interface AST node and builds a linked list
// of Symbols for all exported constants, variables, and procedures/functions.
Symbol *buildUnitSymbolTable(AST *interface_ast) {
    if (!interface_ast || interface_ast->type != AST_COMPOUND) return NULL;

    Symbol *unitSymbols = NULL;
    Symbol **tail = &unitSymbols; // Pointer-to-pointer for efficient list appending

    // Iterate over all declarations in the interface.
    for (int i = 0; i < interface_ast->child_count; i++) {
        AST *decl = interface_ast->children[i];
        if (!decl) continue;

        Symbol *sym = NULL; // Symbol to potentially add

        switch(decl->type) {
            case AST_CONST_DECL: {
                if (!decl->token) break;
                Value v = evaluateCompileTimeValue(decl->left); // evaluated constant expression
                sym = malloc(sizeof(Symbol)); /* null check */
                if (!sym) { fprintf(stderr, "Malloc failed (Symbol) in buildUnitSymbolTable\n"); freeValue(&v); EXIT_FAILURE_HANDLER(); }

                sym->name = strdup(decl->token->value); /* null check */
                if (!sym->name) { fprintf(stderr, "Malloc failed (name) in buildUnitSymbolTable\n"); free(sym); freeValue(&v); EXIT_FAILURE_HANDLER(); }

                sym->value = malloc(sizeof(Value)); /* null check */
                if (!sym->value) { fprintf(stderr, "Malloc failed (Value) in buildUnitSymbolTable\n"); free(sym->name); free(sym); freeValue(&v); EXIT_FAILURE_HANDLER(); }

                *sym->value = makeCopyOfValue(&v); // deep copy the evaluated value
                sym->type = v.type;                // Use evaluated value's type
                sym->type_def = decl->right;       // Link to type node if present
                sym->is_const = true;              // Mark as constant
                sym->is_alias = false;
                sym->is_local_var = false;
                sym->is_inline = false;
                sym->next = NULL;
                sym->enclosing = NULL;
                freeValue(&v); // Free the temporary value from eval
                break;
            }
            case AST_VAR_DECL: {
                 // Interface VARs typically represent external linkage in other systems.
                 // Here, we can add them to the unit's symbol table, but they won't
                 // have actual storage allocated unless the implementation part defines them.
                 // The main purpose here is to make their name and type known.
                 for (int j = 0; j < decl->child_count; j++) {
                     AST *varNode = decl->children[j];
                     if (!varNode || !varNode->token) continue;
                     DEBUG_PRINT("[DEBUG BUILD_UNIT_SYM] Adding interface VAR '%s' (type %s)\n", varNode->token->value, varTypeToString(decl->var_type));
                     Symbol *varSym = malloc(sizeof(Symbol)); /* null check */
                     if (!varSym) { /* error */ }
                     varSym->name = strdup(varNode->token->value); /* null check */
                      if (!varSym->name) { /* error */ }
                     varSym->type = decl->var_type;
                     varSym->type_def = decl->right; // Store type def link
                     varSym->value = NULL; // Interface VARs don't have values initially
                     varSym->is_const = false;
                     varSym->is_alias = false;
                     varSym->is_local_var = false; // Not local to the unit's execution scope yet
                     varSym->is_inline = false;
                     varSym->next = NULL;
                     varSym->enclosing = NULL;

                     // Append to list
                     *tail = varSym;
                     tail = &varSym->next;
                 }
                 // Skip adding to list via 'sym' below
                 continue; // Process next declaration
             }
            case AST_PROCEDURE_DECL:
            case AST_FUNCTION_DECL: {
                if (!decl->token) break;
                sym = malloc(sizeof(Symbol)); /* null check */
                 if (!sym) { /* error */ }
                sym->name = strdup(decl->token->value); /* null check */
                 if (!sym->name) { /* error */ }

                // Determine type (return type for functions, VOID for procedures)
                if (decl->type == AST_FUNCTION_DECL && decl->right) {
                    sym->type = decl->right->var_type; // Use pre-annotated type
                    sym->type_def = decl->right;      // Link to return type node
                } else {
                    sym->type = TYPE_VOID;
                    sym->type_def = NULL;
                }
                sym->value = NULL; // Procedures/functions don't have a 'value' in this context
                sym->is_const = false;
                sym->is_alias = false;
                sym->is_local_var = false;
                sym->is_inline = decl->is_inline;
                sym->next = NULL;
                sym->enclosing = NULL;
                break;
            }
            default:
                // Skip other declaration types (e.g. TYPE_DECL)
                break;
        } // End switch

        // Append the created symbol (if any) to the list
        if (sym) {
            *tail = sym;
            tail = &sym->next;
        }
    } // End for loop

    return unitSymbols;
}

Value makeEnum(const char *enum_name, int ordinal) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_ENUM;
    v.enum_val.enum_name = enum_name ? strdup(enum_name) : NULL; // Duplicate the name
     if (enum_name && !v.enum_val.enum_name) { // Check strdup result
         fprintf(stderr, "FATAL: strdup failed for enum_name in makeEnum\n");
         EXIT_FAILURE_HANDLER();
     }
    v.enum_val.ordinal = ordinal;
    return v;
}


// getTerminalSize remains the same
int getTerminalSize(int *rows, int *cols) {
    // Default values in case of error or non-TTY
    *rows = 24; // Default height
    *cols = 80; // Default width

    // Check if stdout is a terminal
    if (!isatty(STDOUT_FILENO)) {
       // fprintf(stderr, "Warning: Cannot get terminal size (stdout is not a TTY).\n");
        return 0; // Return default size for non-TTY
    }

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        // perror("getTerminalSize: ioctl(TIOCGWINSZ) failed"); // Suppress error message?
        return -1; // Indicate an error occurred
    }

    // Check for valid size values
    if (ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        // ioctl succeeded but returned 0 size, use defaults
       // fprintf(stderr, "Warning: ioctl(TIOCGWINSZ) returned zero size, using defaults.\n");
    }

    return 0; // Success
}

void freeUnitSymbolTable(Symbol *symbol_table) {
    Symbol *current = symbol_table;
    while (current) {
        Symbol *next = current->next;
        if (current->name) {
            free(current->name);
        }
        // Only free the value if it's not NULL (i.e., for constants built here)
        if (current->value) {
            freeValue(current->value); // Free the deep-copied value content
            free(current->value);      // Free the Value struct itself
        }
        free(current); // Free the Symbol struct
        current = next;
    }
}

void toLowerString(char *str) {
    if (!str) return;
    for (int i = 0; str[i]; i++) {
        str[i] = tolower(str[i]);
    }
}

void printValueToStream(Value v, FILE *stream) {
    if (!stream) {
        stream = stdout;
    }

    switch (v.type) {
        case TYPE_INT8:
            fprintf(stream, "%hhd", (int8_t)v.i_val);
            break;
        case TYPE_UINT8:
            fprintf(stream, "%hhu", (uint8_t)v.u_val);
            break;
        case TYPE_INT16:
            fprintf(stream, "%hd", (int16_t)v.i_val);
            break;
        case TYPE_UINT16:
            fprintf(stream, "%hu", (uint16_t)v.u_val);
            break;
        case TYPE_INT32:
            fprintf(stream, "%lld", v.i_val);
            break;
        case TYPE_UINT32:
            fprintf(stream, "%u", (uint32_t)v.u_val);
            break;
        case TYPE_INT64:
            fprintf(stream, "%lld", v.i_val);
            break;
        case TYPE_UINT64:
            fprintf(stream, "%llu", v.u_val);
            break;
        case TYPE_FLOAT:
            fprintf(stream, "%f", v.real.f32_val);
            break;
        case TYPE_DOUBLE:
            fprintf(stream, "%f", v.real.d_val);
            break;
        case TYPE_LONG_DOUBLE:
            fprintf(stream, "%Lf", v.real.r_val);
            break;
        case TYPE_BOOLEAN:
            fprintf(stream, "%s", v.i_val ? "TRUE" : "FALSE"); // Boolean still uses i_val
            break;
        case TYPE_CHAR:
            fprintf(stream, "%c", v.c_val); // Assuming c_val is 'char' or int holding char ASCII
            break;
        case TYPE_STRING:
            if (v.s_val) {
                fprintf(stream, "%s", v.s_val);
            } else {
                fprintf(stream, "(null string)");
            }
            break;
        case TYPE_NIL:
            fprintf(stream, "NIL");
            break;
        case TYPE_POINTER:
            fprintf(stream, "POINTER(@%p -> ", (void*)v.ptr_val);
            if (v.ptr_val) { // If it's not a nil pointer, try to print what it points to
                printValueToStream(*(v.ptr_val), stream); // Recursive call
            } else {
                fprintf(stream, "NIL_TARGET");
            }
            fprintf(stream, ")");
            break;
        case TYPE_ARRAY:
            // Your `v.array_val` is a `Value*` pointing to the first element.
            // The other array metadata (dimensions, bounds, element_type) is directly in `v`.
            fprintf(stream, "ARRAY(dims:%d, base_type:%s, elements_at:%p)",
                    v.dimensions,
                    varTypeToString(v.element_type), // Using v.element_type directly
                    (void*)v.array_val); // v.array_val is the pointer to elements
            // For a more detailed print, you'd iterate based on dimensions and bounds.
            break;
        case TYPE_RECORD:
            fprintf(stream, "RECORD{");
            FieldValue *field = v.record_val;
            bool first_field = true;
            while (field) {
                if (!first_field) {
                    fprintf(stream, "; ");
                }
                fprintf(stream, "%s: ", field->name ? field->name : "?");
                printValueToStream(field->value, stream);
                first_field = false;
                field = field->next;
            }
            fprintf(stream, "}");
            break;
        case TYPE_ENUM: {
            const char *type_name = v.enum_val.enum_name ?
                v.enum_val.enum_name : (v.enum_meta ? v.enum_meta->name : NULL);
            const char *member_name = NULL;
            AST *enum_ast = v.base_type_node;
            if (!enum_ast && type_name) {
                enum_ast = lookupType(type_name);
            }
            if (enum_ast && enum_ast->type == AST_ENUM_TYPE &&
                v.enum_val.ordinal >= 0 &&
                v.enum_val.ordinal < enum_ast->child_count) {
                AST *val_node = enum_ast->children[v.enum_val.ordinal];
                if (val_node && val_node->token && val_node->token->value) {
                    member_name = val_node->token->value;
                }
            }
            if (member_name) {
                fprintf(stream, "%s", member_name);
            } else {
                fprintf(stream, "ENUM(%s, ord: %d)",
                        type_name ? type_name : "<type_unknown>",
                        v.enum_val.ordinal);
            }
            break;
        }
        case TYPE_SET:
            // Corrected access to set_val and its members
            fprintf(stream, "SET(size:%d, values:[", v.set_val.set_size);
            for(int i = 0; i < v.set_val.set_size; ++i) {
                fprintf(stream, "%lld%s", v.set_val.set_values[i], (i == v.set_val.set_size - 1) ? "" : ", ");
            }
            fprintf(stream, "])");
            break;
        case TYPE_FILE:
            if (v.filename) {
                fprintf(stream, "FILE(%s, handle: %p)", v.filename, (void*)v.f_val);
            } else {
                fprintf(stream, "FILE(UNNAMED, handle: %p)", (void*)v.f_val);
            }
            break;
        case TYPE_MEMORYSTREAM:
            if (v.mstream) {
                // Corrected format specifiers for int members of MStream
                fprintf(stream, "MSTREAM(size:%d, cap:%d, data:%p)",
                        v.mstream->size,
                        v.mstream->capacity,
                        (void*)v.mstream->buffer);
            } else {
                fprintf(stream, "MSTREAM(NULL)");
            }
            break;
        case TYPE_BYTE:
            fprintf(stream, "%lld", v.i_val & 0xFF);
            break;
        case TYPE_WORD:
            fprintf(stream, "%lld", v.i_val & 0xFFFF);
            break;
        case TYPE_VOID:
            fprintf(stream, "<VOID_TYPE>");
            break;
        default:
            fprintf(stream, "<UnknownType:%s>", varTypeToString(v.type));
            break;
    }
}
Value makeCopyOfValue(const Value *src) {
    Value v;
    v = *src;  // shallow copy to start

    switch (src->type) {
        case TYPE_STRING:
            if (src->max_length > 0) {
                v.s_val = malloc(src->max_length + 1);
                if (!v.s_val) {
                    fprintf(stderr, "Memory allocation failed in makeCopyOfValue (string)\n");
                    EXIT_FAILURE_HANDLER();
                }
                if (src->s_val) {
                    strncpy(v.s_val, src->s_val, src->max_length);
                    v.s_val[src->max_length] = '\0';
                } else {
                    v.s_val[0] = '\0';
                }
                v.max_length = src->max_length;
            } else if (src->s_val) {
                size_t len = strlen(src->s_val);
                v.s_val = malloc(len + 1);
                if (!v.s_val) {
                    fprintf(stderr, "Memory allocation failed in makeCopyOfValue (string)\n");
                    EXIT_FAILURE_HANDLER();
                }
                strcpy(v.s_val, src->s_val);
            } else {
                v.s_val = NULL;
            }
            break;
        case TYPE_ENUM:
            v.enum_val.enum_name = src->enum_val.enum_name ? strdup(src->enum_val.enum_name) : NULL;
            if (src->enum_val.enum_name && !v.enum_val.enum_name) {
                 fprintf(stderr, "Memory allocation failed in makeCopyOfValue (enum name strdup)\n");
                 EXIT_FAILURE_HANDLER();
            }
            break;
        case TYPE_RECORD: {
            FieldValue *head = NULL, *tail = NULL;
            for (FieldValue *cur = src->record_val; cur; cur = cur->next) {
                FieldValue *copy = malloc(sizeof(FieldValue));
                copy->name = strdup(cur->name);
                copy->value = makeCopyOfValue(&cur->value);
                copy->next = NULL;
                if (tail)
                    tail->next = copy;
                else
                    head = copy;
                tail = copy;
            }
            v.record_val = head;
            break;
        }
        case TYPE_ARRAY: {
            int total = 1;
            v.dimensions = src->dimensions;

            if (v.dimensions > 0 && src->lower_bounds && src->upper_bounds) {
                v.lower_bounds = malloc(sizeof(int) * src->dimensions);
                v.upper_bounds = malloc(sizeof(int) * src->dimensions);
                if (!v.lower_bounds || !v.upper_bounds) { /* Handle error */ }

                for (int i = 0; i < src->dimensions; i++) {
                    v.lower_bounds[i] = src->lower_bounds[i];
                    v.upper_bounds[i] = src->upper_bounds[i];
                    int size_i = (v.upper_bounds[i] - v.lower_bounds[i] + 1);
                    if (size_i <= 0) { total = 0; break; }
                    if (__builtin_mul_overflow(total, size_i, &total)) { total = -1; break; }
                }
            } else {
                total = 0;
                v.lower_bounds = NULL;
                v.upper_bounds = NULL;
            }

            v.array_val = NULL;
            if (total > 0 && src->array_val) {
                 v.array_val = malloc(sizeof(Value) * total);
                 if (!v.array_val) { /* Handle error */ }
                 for (int i = 0; i < total; i++) {
                     v.array_val[i] = makeCopyOfValue(&src->array_val[i]);
                 }
            } else if (total < 0) {
                 fprintf(stderr, "Error: Array size overflow during copy.\n");
                 v.dimensions = 0;
                 free(v.lower_bounds); v.lower_bounds = NULL;
                 free(v.upper_bounds); v.upper_bounds = NULL;
            }

            v.element_type_def = src->element_type_def;
            v.element_type = src->element_type;

            break;
        }
        case TYPE_CHAR:
            v.c_val = src->c_val;
            v.max_length = 1;
            break;
        case TYPE_MEMORYSTREAM:
            v.mstream = NULL;
            if (src->mstream) {
                v.mstream = malloc(sizeof(MStream));
                if (!v.mstream) {
                    fprintf(stderr, "Memory allocation failed in makeCopyOfValue (mstream)\n");
                    EXIT_FAILURE_HANDLER();
                }
                v.mstream->size = src->mstream->size;
                if (src->mstream->buffer && src->mstream->size >= 0) {
                    size_t copy_size = (size_t)src->mstream->size + 1;
                    v.mstream->capacity = copy_size;
                    v.mstream->buffer = malloc(copy_size);
                    if (!v.mstream->buffer) {
                        free(v.mstream);
                        fprintf(stderr, "Memory allocation failed in makeCopyOfValue (mstream buffer)\n");
                        EXIT_FAILURE_HANDLER();
                    }
                    memcpy(v.mstream->buffer, src->mstream->buffer, copy_size);
                } else {
                    v.mstream->buffer = NULL;
                    v.mstream->capacity = 0;
                }
                }
            break;
        case TYPE_SET:
            v.set_val.set_values = NULL;
            v.set_val.set_size = 0;

            if (src->set_val.set_size > 0 && src->set_val.set_values != NULL) {
                size_t array_size_bytes = sizeof(long long) * src->set_val.set_size;
                v.set_val.set_values = malloc(array_size_bytes);
                if (!v.set_val.set_values) {
                    freeValue(&v);
                    fprintf(stderr,
                            "Memory allocation failed in makeCopyOfValue (set)\n");
                    EXIT_FAILURE_HANDLER();
                }
                if (!v.set_val.set_values) {
                    fprintf(stderr, "Memory allocation failed in makeCopyOfValue (set values)\n");
                    EXIT_FAILURE_HANDLER();
                }
                memcpy(v.set_val.set_values, src->set_val.set_values, array_size_bytes);
                v.set_val.set_size = src->set_val.set_size;
            }
            break;
        default:
            break;
    }

    return v;
}

int calculateArrayTotalSize(const Value* array_val) {
    if (!array_val || array_val->type != TYPE_ARRAY || array_val->dimensions == 0) {
        return 0;
    }
    int total_size = 1;
    for (int i = 0; i < array_val->dimensions; i++) {
        total_size *= (array_val->upper_bounds[i] - array_val->lower_bounds[i] + 1);
    }
    return total_size;
}

int computeFlatOffset(Value *array, int *indices) {
    int offset = 0;
    int multiplier = 1;

    for (int i = array->dimensions - 1; i >= 0; i--) {
        // Bounds check for the current dimension
        if (indices[i] < array->lower_bounds[i] || indices[i] > array->upper_bounds[i]) {
            fprintf(stderr, "Runtime error: Index %d out of bounds [%d..%d] in dimension %d.\n",
                    indices[i], array->lower_bounds[i], array->upper_bounds[i], i + 1);
            EXIT_FAILURE_HANDLER();
        }
        
        // Add the contribution of the current dimension to the total offset
        offset += (indices[i] - array->lower_bounds[i]) * multiplier;
        
        // Update the multiplier for the next (more significant) dimension
        multiplier *= (array->upper_bounds[i] - array->lower_bounds[i] + 1);
    }
    return offset;
}

// --- Set utility helpers (internal) ---
static bool setContainsOrdinalUtil(const Value* setVal, long long ordinal) {
    if (!setVal || setVal->type != TYPE_SET || !setVal->set_val.set_values) {
        return false;
    }
    for (int i = 0; i < setVal->set_val.set_size; i++) {
        if (setVal->set_val.set_values[i] == ordinal) {
            return true;
        }
    }
    return false;
}

static void addOrdinalToResultSetUtil(Value* resultVal, long long ordinal) {
    if (!resultVal || resultVal->type != TYPE_SET) return;

    if (setContainsOrdinalUtil(resultVal, ordinal)) {
        return;
    }

    if (resultVal->set_val.set_size >= resultVal->max_length) {
        int new_capacity = (resultVal->max_length == 0) ? 8 : resultVal->max_length * 2;
        long long* new_values = realloc(resultVal->set_val.set_values, sizeof(long long) * new_capacity);
        if (!new_values) {
            fprintf(stderr, "FATAL: realloc failed in addOrdinalToResultSetUtil\n");
            EXIT_FAILURE_HANDLER();
        }
        resultVal->set_val.set_values = new_values;
        resultVal->max_length = new_capacity;
    }

    resultVal->set_val.set_values[resultVal->set_val.set_size] = ordinal;
    resultVal->set_val.set_size++;
}

// --- Set operations exported via utils.h ---
Value setUnion(Value setA, Value setB) {
    if (setA.type != TYPE_SET || setB.type != TYPE_SET) {
        fprintf(stderr, "Internal Error: Non-set type passed to setUnion.\n");
        return makeVoid();
    }

    Value result = makeValueForType(TYPE_SET, NULL, NULL);
    result.max_length = setA.set_val.set_size + setB.set_val.set_size;
    if (result.max_length > 0) {
        result.set_val.set_values = malloc(sizeof(long long) * result.max_length);
        if (!result.set_val.set_values) {
            fprintf(stderr, "Malloc failed for set union result\n");
            result.max_length = 0;
            EXIT_FAILURE_HANDLER();
        }
    } else {
        result.set_val.set_values = NULL;
    }
    result.set_val.set_size = 0;

    if (setA.set_val.set_values) {
        for (int i = 0; i < setA.set_val.set_size; i++) {
            addOrdinalToResultSetUtil(&result, setA.set_val.set_values[i]);
        }
    }
    if (setB.set_val.set_values) {
        for (int i = 0; i < setB.set_val.set_size; i++) {
            addOrdinalToResultSetUtil(&result, setB.set_val.set_values[i]);
        }
    }

    return result;
}

Value setDifference(Value setA, Value setB) {
    if (setA.type != TYPE_SET || setB.type != TYPE_SET) {
        return makeVoid();
    }

    Value result = makeValueForType(TYPE_SET, NULL, NULL);
    result.max_length = setA.set_val.set_size;
    if (result.max_length > 0) {
        result.set_val.set_values = malloc(sizeof(long long) * result.max_length);
        if (!result.set_val.set_values) {
            result.max_length = 0;
            EXIT_FAILURE_HANDLER();
        }
    } else {
        result.set_val.set_values = NULL;
    }
    result.set_val.set_size = 0;

    if (setA.set_val.set_values) {
        for (int i = 0; i < setA.set_val.set_size; i++) {
            if (!setContainsOrdinalUtil(&setB, setA.set_val.set_values[i])) {
                addOrdinalToResultSetUtil(&result, setA.set_val.set_values[i]);
            }
        }
    }
    return result;
}

Value setIntersection(Value setA, Value setB) {
    if (setA.type != TYPE_SET || setB.type != TYPE_SET) {
        return makeVoid();
    }

    Value result = makeValueForType(TYPE_SET, NULL, NULL);
    result.max_length = (setA.set_val.set_size < setB.set_val.set_size) ? setA.set_val.set_size : setB.set_val.set_size;
    if (result.max_length > 0) {
        result.set_val.set_values = malloc(sizeof(long long) * result.max_length);
        if (!result.set_val.set_values) {
            result.max_length = 0;
            EXIT_FAILURE_HANDLER();
        }
    } else {
        result.set_val.set_values = NULL;
    }
    result.set_val.set_size = 0;

    if (setA.set_val.set_values) {
        for (int i = 0; i < setA.set_val.set_size; i++) {
            if (setContainsOrdinalUtil(&setB, setA.set_val.set_values[i])) {
                addOrdinalToResultSetUtil(&result, setA.set_val.set_values[i]);
            }
        }
    }
    return result;
}

// Helper function to map 0-15 to ANSI FG codes
int map16FgColorToAnsi(int pscalColorCode, bool isBold) {
    int basePscalColor = pscalColorCode % 8;
    bool isBright = isBold || (pscalColorCode >= 8);
    int ansiBaseOffset = pscalToAnsiBase[basePscalColor];
    return (isBright ? 90 : 30) + ansiBaseOffset;
}

// Helper function to map 0-7 to ANSI BG codes
int map16BgColorToAnsi(int pscalColorCode) {
    int basePscalColor = pscalColorCode % 8;
    return 40 + pscalToAnsiBase[basePscalColor];
}

bool applyCurrentTextAttributes(FILE* stream) {
    bool is_default_state = (gCurrentTextColor == 7 && gCurrentTextBackground == 0 &&
                             !gCurrentTextBold && !gCurrentTextUnderline &&
                             !gCurrentTextBlink && !gCurrentColorIsExt &&
                             !gCurrentBgIsExt);

    if (is_default_state) return false;

    char escape_sequence[64] = "\x1B[";
    char code_str[64];
    bool first_attr = true;

    if (gCurrentTextBold) { strcat(escape_sequence, "1"); first_attr = false; }
    if (gCurrentTextUnderline) {
        if (!first_attr) strcat(escape_sequence, ";");
        strcat(escape_sequence, "4");
        first_attr = false;
    }
    if (gCurrentTextBlink) {
        if (!first_attr) strcat(escape_sequence, ";");
        strcat(escape_sequence, "5");
        first_attr = false;
    }
    if (gCurrentColorIsExt) {
        if (!first_attr) strcat(escape_sequence, ";");
        snprintf(code_str, sizeof(code_str), "38;5;%d", gCurrentTextColor);
    } else {
        if (!first_attr) strcat(escape_sequence, ";");
        snprintf(code_str, sizeof(code_str), "%d",
                 map16FgColorToAnsi(gCurrentTextColor, gCurrentTextBold));
    }
    strcat(escape_sequence, code_str);
    first_attr = false;
    strcat(escape_sequence, ";");
    if (gCurrentBgIsExt) {
        snprintf(code_str, sizeof(code_str), "48;5;%d", gCurrentTextBackground);
    } else {
        snprintf(code_str, sizeof(code_str), "%d",
                 map16BgColorToAnsi(gCurrentTextBackground));
    }
    strcat(escape_sequence, code_str);
    strcat(escape_sequence, "m");
    fprintf(stream, "%s", escape_sequence);
    return true;
}

void resetTextAttributes(FILE* stream) {
    fprintf(stream, "\x1B[0m");
}
