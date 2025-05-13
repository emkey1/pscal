#include "utils.h"
#include "globals.h"
#include <stdlib.h>
#include <string.h>
#include "parser.h"  // so that Procedure and TypeEntry are defined
#include "interpreter.h"  // so that Procedure and TypeEntry are defined
#include <stdio.h>
#include <stdlib.h>
#include "globals.h"
#include "symbol.h"
#include "types.h"
#include "builtin.h"
#include <sys/ioctl.h> // Make sure this is included
#include <unistd.h>    // For STDOUT_FILENO


const char *varTypeToString(VarType type) {
    switch (type) {
        case TYPE_VOID:         return "VOID";
        case TYPE_INTEGER:      return "INTEGER";
        case TYPE_REAL:         return "REAL";
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
        case TYPE_NIL:          return "NIL";
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
        case TOKEN_CARET:         return "CARET";   // <<< ADDED
        case TOKEN_NIL:           return "NIL";     // <<< ADDED
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
        case AST_LIST:           return "LIST";
        case AST_ENUM_TYPE:      return "TYPE_ENUM";
        case AST_ENUM_VALUE:     return "ENUM_VALUE";
        case AST_SET:            return "SET";
        case AST_ARRAY_LITERAL:  return "ARRAY_LITERAL";
        case AST_BREAK:          return "BREAK";
        case AST_POINTER_TYPE:   return "POINTER_TYPE";
        case AST_DEREFERENCE:    return "DEREFERENCE";
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
        if (!new_field) { // Added null check for malloc result
             fprintf(stderr, "Memory allocation error in copyRecord for new_field\n");
             // Consider freeing already allocated parts of new_head before exiting
             EXIT_FAILURE_HANDLER();
        }
        new_field->name = strdup(curr->name);
        if (!new_field->name) { // Added null check for strdup result
             fprintf(stderr, "Memory allocation error in copyRecord for new_field->name\n");
             free(new_field);
             // Consider freeing already allocated parts of new_head before exiting
             EXIT_FAILURE_HANDLER();
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
            if (!fv) { // Check malloc
                 fprintf(stderr, "FATAL: malloc failed for FieldValue in createEmptyRecord for field '%s'\n", varNode->token->value);
                 // Hard to clean up partially built list, exiting is safest
                 EXIT_FAILURE_HANDLER();
            }

            // Duplicate the field name
            fv->name = strdup(varNode->token->value);
            if (!fv->name) { // Check strdup
                 fprintf(stderr, "FATAL: strdup failed for FieldValue name in createEmptyRecord for field '%s'\n", varNode->token->value);
                 free(fv); // Free the FieldValue struct itself
                 EXIT_FAILURE_HANDLER();
            }

            // Recursively create the default value for this field's type
            fv->value = makeValueForType(fieldType, fieldTypeDef); // Relies on makeValueForType checks
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
    v.type = TYPE_INTEGER;
    v.i_val = val;
    return v;
}

Value makeReal(double val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_REAL;
    v.r_val = val;
    return v;
}

Value makeByte(unsigned char val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_BYTE;
    v.i_val = val;  // Store the byte in the integer field.
    return v;
}

Value makeWord(unsigned int val) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_WORD;
    // Use i_val, ensuring it handles potential size differences if long long > unsigned int
    v.i_val = val;
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

Value makeChar(char c) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = TYPE_CHAR;
    v.c_val = c;
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
        v.array_val[i] = makeValueForType(element_type, type_def);
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

Value makeValueForType(VarType type, AST *type_def_param) {
    Value v;
    memset(&v, 0, sizeof(Value));
    v.type = type;
    v.base_type_node = NULL; // Initialize

    // Directly use the passed-in AST node which should be from the
    // copied structure linked to the VAR_DECL.
    AST* node_to_inspect = type_def_param;
    AST* actual_type_def = node_to_inspect; // Use this for type-specific details after resolving reference

    // --- Set base_type_node specifically for pointers ---
    if (type == TYPE_POINTER) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG makeValueForType] Setting base type for POINTER. Processing structure starting at %p (Type: %s)\n",
                (void*)node_to_inspect, node_to_inspect ? astTypeToString(node_to_inspect->type) : "NULL");
        fflush(stderr);
        #endif

        AST* pointer_type_node = node_to_inspect; // Start with the node from VAR_DECL

        // If it's a TYPE_REFERENCE, follow its 'right' link ONE level
        // to get the actual POINTER_TYPE node within the *same copied structure*.
        // DO NOT call lookupType here.
        if (pointer_type_node && pointer_type_node->type == AST_TYPE_REFERENCE) {
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG makeValueForType] Passed node is TYPE_REFERENCE ('%s'), following its right pointer (%p)\n",
                    pointer_type_node->token ? pointer_type_node->token->value : "?", (void*)pointer_type_node->right);
            fflush(stderr);
            #endif
            pointer_type_node = pointer_type_node->right; // Get the node linked from the TYPE_REFERENCE
        }

        // Now, check if we correctly landed on a POINTER_TYPE node
        if (pointer_type_node && pointer_type_node->type == AST_POINTER_TYPE) {
            // Get the base type from the 'right' child of *this* POINTER_TYPE node
            v.base_type_node = pointer_type_node->right;
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG makeValueForType] -> Base type node set to %p (Type: %s, Token: '%s') from node %p\n",
                     (void*)v.base_type_node,
                     v.base_type_node ? astTypeToString(v.base_type_node->type) : "NULL",
                     (v.base_type_node && v.base_type_node->token) ? v.base_type_node->token->value : "N/A",
                     (void*)pointer_type_node);
             fflush(stderr);
             #endif
        } else {
            // If pointer_type_node is NULL or not POINTER_TYPE after resolving reference (if any)
             fprintf(stderr, "Warning: Failed to find POINTER_TYPE definition node when initializing pointer Value. Structure trace started from VAR_DECL->right at %p. Final node checked was %p (Type: %s).\n",
                     (void*)type_def_param, // Log the original param
                     (void*)pointer_type_node,
                     pointer_type_node ? astTypeToString(pointer_type_node->type) : "NULL");
             v.base_type_node = NULL; // Ensure it remains NULL if logic fails
        }
    } // End if (type == TYPE_POINTER)

    // --- Initialize Value based on Type ---
    // The rest of the switch uses 'type' and 'type_def_param' (as actual_type_def)
    // Needs careful check to ensure 'actual_type_def' isn't needed where 'type_def_param' should be used
    // For consistency let's rename actual_type_def back to type_def_param inside the switch
    // where it makes sense (e.g., for record, array, fixed string).

    switch(type) {
        case TYPE_INTEGER: v.i_val = 0; break;
        case TYPE_REAL:    v.r_val = 0.0; break;
        case TYPE_STRING: { // Use braces for local variable scope
            v.s_val = NULL;
            v.max_length = -1; // Default dynamic
            long long parsed_len = -1; // Flag to check if length was determined

            // Check if the type definition is string[N]
            // Use 'actual_type_def' here as we need the potentially resolved definition
            if (actual_type_def && actual_type_def->type == AST_VARIABLE && actual_type_def->token &&
                strcasecmp(actual_type_def->token->value, "string") == 0 && actual_type_def->right)
            {
                 AST* lenNode = actual_type_def->right;

                 // --- MODIFICATION START: Handle Constant Integer Literal OR Constant Identifier ---
                 if (lenNode->type == AST_NUMBER && lenNode->token && lenNode->token->type == TOKEN_INTEGER_CONST) {
                     // Case 1: Length is a literal integer (e.g., string[10])
                     parsed_len = atoll(lenNode->token->value);
                 }
                 else if (lenNode->type == AST_VARIABLE && lenNode->token && lenNode->token->value) {
                     // Case 2: Length is an identifier (e.g., string[MAX_LEN])
                     const char *const_name = lenNode->token->value;
                     #ifdef DEBUG
                     fprintf(stderr, "[DEBUG makeValueForType] String length specified by identifier '%s'. Looking up constant...\n", const_name);
                     #endif
                     Symbol *constSym = lookupSymbol(const_name); // Lookup the constant symbol
                     // NOTE: Assumes constant is already defined globally or locally *before* this type is used.

                     if (constSym && constSym->is_const && constSym->value && constSym->value->type == TYPE_INTEGER) {
                          parsed_len = constSym->value->i_val; // Use the constant's integer value
                          #ifdef DEBUG
                          fprintf(stderr, "[DEBUG makeValueForType] Found constant '%s' with value %lld.\n", const_name, parsed_len);
                          #endif
                     } else {
                          fprintf(stderr, "Warning: Identifier '%s' used for string length is not a defined integer constant. Using dynamic.\n", const_name);
                          // parsed_len remains -1, will default to dynamic
                     }
                 }
                 else {
                     // Case 3: Length expression is neither an integer literal nor a constant identifier
                     fprintf(stderr, "Warning: Fixed string length not constant integer or identifier. Using dynamic.\n");
                     // parsed_len remains -1, will default to dynamic
                 }
                 // --- MODIFICATION END ---


                 // --- Process the parsed length (if valid) ---
                 if (parsed_len != -1) { // Check if a valid length was found
                      if (parsed_len > 0 && parsed_len <= 255) { // Standard Pascal max length
                          v.max_length = (int)parsed_len;
                          v.s_val = calloc(v.max_length + 1, 1); // Allocate and zero-fill
                          if (!v.s_val) { fprintf(stderr, "FATAL: calloc failed for fixed string\n"); EXIT_FAILURE_HANDLER(); }
                          #ifdef DEBUG
                          fprintf(stderr, "[DEBUG makeValueForType] Allocated fixed string (max_length=%d).\n", v.max_length);
                          #endif
                      } else {
                          fprintf(stderr, "Warning: Fixed string length %lld invalid or too large. Using dynamic.\n", parsed_len);
                          // Fall through to dynamic allocation below
                          v.max_length = -1; // Ensure dynamic if length was invalid
                      }
                 }
                 // --- End processing parsed length ---

            } // End if (actual_type_def is string[...])

            // Allocate if still dynamic (max_length is -1) and s_val hasn't been allocated
            if (v.max_length == -1 && !v.s_val) {
                 v.s_val = strdup(""); // Initialize dynamic/default string
                 if (!v.s_val) { fprintf(stderr, "FATAL: strdup failed for dynamic string\n"); EXIT_FAILURE_HANDLER(); }
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG makeValueForType] Allocated dynamic string.\n");
                 #endif
            }
            break; // End case TYPE_STRING
        } // End brace for TYPE_STRING case scope
        case TYPE_CHAR:    v.c_val = '\0'; v.max_length = 1; break;
        case TYPE_BOOLEAN: v.i_val = 0; break; // False
        case TYPE_FILE:    v.f_val = NULL; v.filename = NULL; break;
        case TYPE_RECORD:
             // Pass type_def_param (copied RECORD_TYPE node)
             v.record_val = createEmptyRecord(type_def_param);
             break;
        case TYPE_ARRAY: {
            // Initialize defaults for the Value struct 'v'
            v.dimensions = 0;
            v.lower_bounds = NULL;
            v.upper_bounds = NULL;
            v.array_val = NULL;
            v.element_type = TYPE_VOID;
            v.element_type_def = NULL;

            // This is the AST node passed in, could be AST_TYPE_REFERENCE or AST_ARRAY_TYPE
            AST* definition_node_for_array = type_def_param;

            // If it's a named type (AST_TYPE_REFERENCE), resolve it
            if (definition_node_for_array && definition_node_for_array->type == AST_TYPE_REFERENCE) {
                #ifdef DEBUG
                fprintf(stderr, "[DEBUG makeValueForType ARRAY] type_def_param is TYPE_REFERENCE ('%s'). Looking up actual type.\n",
                        definition_node_for_array->token ? definition_node_for_array->token->value : "?");
                #endif
                AST* resolved_type_ast = lookupType(definition_node_for_array->token->value);
                if (!resolved_type_ast) {
                     fprintf(stderr, "Error: Could not resolve array type reference '%s' in makeValueForType for array initialization.\n",
                             definition_node_for_array->token ? definition_node_for_array->token->value : "?");
                     // Let definition_node_for_array remain the unresolved reference;
                     // the next check will likely fail, leading to the warning.
                } else {
                    definition_node_for_array = resolved_type_ast; // Use the looked-up definition
                }
            }

            // Now, definition_node_for_array should point to the actual AST_ARRAY_TYPE node
            if (definition_node_for_array && definition_node_for_array->type == AST_ARRAY_TYPE) {
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG makeValueForType] Initializing ARRAY from (resolved) AST_ARRAY_TYPE node %p.\n", (void*)definition_node_for_array);
                 #endif

                 int dims = definition_node_for_array->child_count; // Number of subrange children in AST_ARRAY_TYPE
                 AST* elemTypeDefNode = definition_node_for_array->right; // Element type AST from AST_ARRAY_TYPE
                 VarType elemType = TYPE_VOID;

                 // Determine element VarType
                 if(elemTypeDefNode) {
                     elemType = elemTypeDefNode->var_type; // Relies on prior type annotation
                       if (elemType == TYPE_VOID) { // Fallback if not annotated
                             if (elemTypeDefNode->type == AST_VARIABLE && elemTypeDefNode->token) {
                                 const char *tn = elemTypeDefNode->token->value;
                                 if (strcasecmp(tn, "integer") == 0) elemType = TYPE_INTEGER;
                                 else if (strcasecmp(tn, "real") == 0) elemType = TYPE_REAL;
                                 else if (strcasecmp(tn, "char") == 0) elemType = TYPE_CHAR;
                                 else if (strcasecmp(tn, "boolean") == 0) elemType = TYPE_BOOLEAN;
                                 else if (strcasecmp(tn, "byte") == 0) elemType = TYPE_BYTE;
                                 else if (strcasecmp(tn, "word") == 0) elemType = TYPE_WORD;
                                 else if (strcasecmp(tn, "string") == 0) elemType = TYPE_STRING;
                                 else { // User-defined type
                                     AST* userTypeDef = lookupType(tn);
                                     if (userTypeDef) elemType = userTypeDef->var_type;
                                     // Ensure elemTypeDefNode points to the actual definition for makeArrayND
                                     if (userTypeDef) elemTypeDefNode = userTypeDef;
                                 }
                             } else if (elemTypeDefNode->type == AST_RECORD_TYPE) { // Array of anonymous records
                                elemType = TYPE_RECORD;
                             } else if (elemTypeDefNode->type == AST_ARRAY_TYPE) { // Array of anonymous arrays
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
                     // Extract bounds from the children of definition_node_for_array (which are AST_SUBRANGE)
                     for (int i = 0; i < dims; i++) {
                         AST *subrange = definition_node_for_array->children[i];
                         if (!subrange || subrange->type != AST_SUBRANGE || !subrange->left || !subrange->right) {
                             bounds_ok = false; break;
                         }
                         // Evaluate bounds - assuming they are constant integer expressions
                         Value low_val = eval(subrange->left);
                         Value high_val = eval(subrange->right);
                         // For now, strictly expect integer bounds for simplicity
                         if (low_val.type == TYPE_INTEGER && high_val.type == TYPE_INTEGER) {
                             lbs[i] = (int)low_val.i_val;
                             ubs[i] = (int)high_val.i_val;
                         } else {
                             // TODO: Support char or other ordinal bounds if necessary by converting to integer ordinals
                             fprintf(stderr, "Runtime error: Array bounds must be integer constants for now in makeValueForType. Dim %d has types %s..%s\n", i, varTypeToString(low_val.type), varTypeToString(high_val.type));
                             bounds_ok = false;
                         }
                         freeValue(&low_val);
                         freeValue(&high_val);
                         if (!bounds_ok || lbs[i] > ubs[i]) {
                             bounds_ok = false; break;
                         }
                     }

                     if (bounds_ok) {
                         // Pass the element type definition node (elemTypeDefNode) to makeArrayND.
                         // This is important for initializing elements that are themselves complex types (records, other arrays).
                         v = makeArrayND(dims, lbs, ubs, elemType, elemTypeDefNode);
                     } else {
                         fprintf(stderr, "Error: Failed to initialize array in makeValueForType due to invalid or non-integer bounds.\n");
                         // v will retain dimensions = 0
                     }
                     free(lbs);
                     free(ubs);
                 } else {
                     fprintf(stderr, "Warning: Invalid dimension count (%d) or element type (%s) for array in makeValueForType.\n", dims, varTypeToString(elemType));
                     // v will retain dimensions = 0
                 }
            } else {
                 // This warning will now include the type of the node that was expected to be AST_ARRAY_TYPE
                 fprintf(stderr, "Warning: Cannot initialize array value. Type definition missing, not an array type, or could not be resolved. (Actual node type for definition: %s)\n",
                         definition_node_for_array ? astTypeToString(definition_node_for_array->type) : "NULL");
                 // v will have dimensions = 0 as initialized if this path is taken
            }

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG makeValueForType - ARRAY CASE EXIT] Returning Value: type=%s, dimensions=%d\n", varTypeToString(v.type), v.dimensions);
            #endif
            break;
        } // End TYPE_ARRAY case
        case TYPE_MEMORYSTREAM: v.mstream = createMStream(); break;
        case TYPE_ENUM:
             v.enum_val.ordinal = 0;
             // Use type_def_param (copied ENUM_TYPE node) for the name
             v.enum_val.enum_name = (type_def_param && type_def_param->token && type_def_param->token->value) ? strdup(type_def_param->token->value) : strdup("<unknown_enum>");
             if (!v.enum_val.enum_name) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }
             break;
        case TYPE_BYTE:    v.i_val = 0; break;
        case TYPE_WORD:    v.i_val = 0; break;
        case TYPE_SET:     v.set_val.set_size = 0; v.set_val.set_values = NULL; v.max_length = 0; break; // Init max_length
        case TYPE_POINTER:
            v.ptr_val = NULL; // Initialize pointer to nil
            // v.base_type_node was set above
            break;
        case TYPE_VOID:    /* No value needed */ break;
        default:
            fprintf(stderr, "Warning: makeValueForType called with unhandled type %d (%s)\n", type, varTypeToString(type));
            break;
    }

    #ifdef DEBUG
    // (Keep final debug print)
    #endif

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
Token *newToken(TokenType type, const char *value) {
    Token *token = malloc(sizeof(Token));
    if (!token) {
        fprintf(stderr, "Memory allocation error in newToken\n");
        EXIT_FAILURE_HANDLER();
    }
    token->type = type;
    token->value = value ? strdup(value) : NULL;  // Deep copy the string!
    if (value && !token->value) { // Check strdup result
        fprintf(stderr, "Memory allocation error (strdup value) in newToken\n");
        free(token);
        EXIT_FAILURE_HANDLER();
    }
    return token;
}

/* Copy a token */
Token *copyToken(const Token *token) {
    if (!token) return NULL;
    Token *new_token = malloc(sizeof(Token));
    if (!new_token) { fprintf(stderr, "Memory allocation error in copyToken (Token struct)\n"); EXIT_FAILURE_HANDLER(); }
    *new_token = *token; // Copy basic fields
    // Deep copy the string value if it exists
    new_token->value = token->value ? strdup(token->value) : NULL; // Use strdup, check for NULL
    if (token->value && !new_token->value) { // Check if strdup failed
         fprintf(stderr, "Memory allocation error (strdup value) in copyToken\n");
         free(new_token); // Free the partially allocated token
         EXIT_FAILURE_HANDLER();
    }
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
    if (!procedure_table) { // procedure_table is now HashTable*
        return;
    }
    DEBUG_PRINT("[DEBUG SYMBOL] Freeing Procedure HashTable at %p.\n", (void*)procedure_table);

    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        // The bucket stores Symbol* which are actually Procedure*
        Procedure *current_proc = (Procedure*)procedure_table->buckets[i];
        while (current_proc) {
            Procedure *next_proc = current_proc->next_in_bucket; // Use the correct 'next' field

            #ifdef DEBUG
            fprintf(stderr, "[DEBUG FREE_PROC_TABLE] Freeing Procedure '%s' (AST @ %p).\n",
                    current_proc->name ? current_proc->name : "?", (void*)current_proc->proc_decl);
            #endif
            
            if (current_proc->name) {
                free(current_proc->name);
            }
            if (current_proc->proc_decl) {
                // The proc_decl AST node is a deep copy owned by this Procedure struct.
                // It needs to be freed.
                freeAST(current_proc->proc_decl);
            }
            free(current_proc); // Free the Procedure struct itself
            
            current_proc = next_proc;
        }
        procedure_table->buckets[i] = NULL;
    }
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

#ifdef DEBUG
    fprintf(stderr, "[DEBUG] freeValue called for Value* at %p, type=%s\n",
            (void*)v, varTypeToString(v->type));
#endif
    switch (v->type) {
        case TYPE_VOID:
        case TYPE_INTEGER:
        case TYPE_REAL:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
        case TYPE_BYTE:
        case TYPE_WORD:
            // No heap data associated with the Value struct itself for these
#ifdef DEBUG
            fprintf(stderr, "[DEBUG]   No heap data to free for type %s\n", varTypeToString(v->type));
#endif
            break;
        case TYPE_ENUM:
            if (v->enum_val.enum_name) {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]   Attempting to free enum name '%s' at %p\n",
                        v->enum_val.enum_name, (void*)v->enum_val.enum_name);
#endif
                free(v->enum_val.enum_name);
                v->enum_val.enum_name = NULL;
            } else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]   Enum name pointer is NULL, nothing to free.\n");
#endif
            }
            break;
        case TYPE_POINTER:
            // freeValue for a pointer variable's Value struct should ONLY reset the
            // address it holds (ptr_val), NOT the base_type_node.
            // The base_type_node represents the declared type and persists even if ptr_val is nil.
            // Freeing the memory *pointed to* is the job of dispose/FreeMem.
            #ifdef DEBUG
            fprintf(stderr, "[DEBUG]   Resetting ptr_val for POINTER Value* at %p. Base type node (%p) is preserved.\n",
                    (void*)v, (void*)v->base_type_node);
            #endif
            v->ptr_val = NULL;
            // DO NOT NULLIFY v->base_type_node HERE.
            break;

        case TYPE_STRING:
            if (v->s_val) {
#ifdef DEBUG
                // Check if pointer seems valid before attempting to print content
                // This is primarily to avoid crashing *in the debug print itself*
                // if v->s_val points to garbage or already freed memory.
                fprintf(stderr, "[DEBUG]   Attempting to free string content at %p (value was '%s')\n",
                        (void*)v->s_val, v->s_val ? v->s_val : "<INVALID_PTR_OR_FREED>");
#endif
                // Original free logic
                free(v->s_val);
                v->s_val = NULL;
            } else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]   String content pointer is NULL, nothing to free.\n");
#endif
            }
            break;

        case TYPE_RECORD: {
            FieldValue *f = v->record_val;
#ifdef DEBUG
            fprintf(stderr, "[DEBUG]   Processing record fields for Value* at %p (record_val=%p)\n", (void*)v, (void*)f);
#endif
            while (f) {
                FieldValue *next = f->next;
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]     Freeing FieldValue* at %p (name='%s' @ %p)\n",
                        (void*)f, f->name ? f->name : "NULL", (void*)f->name);
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
#endif
             if (v->array_val) {
                 int total = 1;
                 if(v->dimensions > 0 && v->lower_bounds && v->upper_bounds) { // Add null checks
                   for (int i = 0; i < v->dimensions; i++)
                       total *= (v->upper_bounds[i] - v->lower_bounds[i] + 1);
                 } else {
                   total = 0; // Prevent calculation if bounds are missing
#ifdef DEBUG
                   fprintf(stderr, "[DEBUG]   Warning: Array bounds missing or zero dimensions.\n");
#endif
                 }

                 for (int i = 0; i < total; i++) {
#ifdef DEBUG
                    fprintf(stderr, "[DEBUG]     Freeing array element %d\n", i); // Added print
#endif
                    freeValue(&v->array_val[i]); // Frees contents of each element
                 }
#ifdef DEBUG
                 fprintf(stderr, "[DEBUG]   Freeing array data buffer at %p\n", (void*)v->array_val); // Added print
#endif
                 free(v->array_val); // Frees the array of Value structs itself
             }
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]   Freeing array bounds at %p and %p\n", (void*)v->lower_bounds, (void*)v->upper_bounds); // Added print
#endif
             free(v->lower_bounds);
             free(v->upper_bounds);
             v->array_val = NULL;
             v->lower_bounds = NULL;
             v->upper_bounds = NULL;
             break;
        }
        case TYPE_MEMORYSTREAM:
             if (v->mstream) {
         #ifdef DEBUG
                 fprintf(stderr, "[DEBUG]   Attempting to free MStream content for Value* at %p (mstream=%p)\n", (void*)v, (void*)v->mstream);
         #endif
                 if (v->mstream->buffer) {
         #ifdef DEBUG
                     fprintf(stderr, "[DEBUG]     Freeing MStream buffer at %p (size=%d)\n", (void*)v->mstream->buffer, v->mstream->size);
         #endif
                     free(v->mstream->buffer);
                     v->mstream->buffer = NULL;
                 }
         #ifdef DEBUG
                 fprintf(stderr, "[DEBUG]     Freeing MStream struct itself at %p\n", (void*)v->mstream);
         #endif
                 free(v->mstream);
                 v->mstream = NULL;
             } else {
         #ifdef DEBUG
                 fprintf(stderr, "[DEBUG]   MStream pointer is NULL, nothing to free for MStream.\n");
         #endif
             }
             break;
        // Add other types if they allocate memory pointed to by Value struct members
        default:
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]   Unhandled type %s in freeValue\n", varTypeToString(v->type));
#endif
             break; // Or handle error
    }
    // Optionally mark type as VOID after freeing contents?
    // v->type = TYPE_VOID;
}

void dumpSymbol(Symbol *sym) {
    if (!sym) return;

    printf("Name: %s, Type: %s", sym->name, varTypeToString(sym->type));

    if (sym->value) {
        printf(", Value: ");
        switch (sym->type) {
            case TYPE_INTEGER:
                printf("%lld", sym->value->i_val);
                break;
            case TYPE_REAL:
                printf("%f", sym->value->r_val);
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

/* Dump the global and local symbol tables. */
void dumpSymbolTable(void);

/*
 * debug_ast - A simple wrapper that begins dumping at the root with zero indent.
 */
void debugASTFile(AST *node) {
    dumpAST(node, 0);
}

char *findUnitFile(const char *unit_name) {
    const char *base_path;
#ifdef DEBUG
    base_path = "/usr/local/Pscal/lib";
#else
    base_path = "/usr/local/Pscal/lib";
#endif

    // Allocate enough space: path + '/' + unit name + ".pl" + null terminator
    size_t max_path_len = strlen(base_path) + 1 + strlen(unit_name) + 3 + 1;
    char *file_name = malloc(max_path_len);
    if (!file_name) {
        fprintf(stderr, "Memory allocation error in findUnitFile\n");
        EXIT_FAILURE_HANDLER();
    }

    // Format full path safely
    snprintf(file_name, max_path_len, "%s/%s.pl", base_path, unit_name);

    return file_name;
}

void linkUnit(AST *unit_ast, int recursion_depth) {
    if (!unit_ast) return;

    // Ensure that the unit_ast has a symbol table (built by unitParser)
    if (!unit_ast->symbol_table) {
        fprintf(stderr, "Error: Symbol table for unit is missing.\n");
        EXIT_FAILURE_HANDLER();
    }

    // Iterate through the unit's symbol table and add ONLY VARIABLES and CONSTANTS
    // to the global symbol table. Procedures and Functions are handled by the procedure_table.
    Symbol *unit_symbol = unit_ast->symbol_table;
    while (unit_symbol) {

        // ADDED: Check if the symbol represents a procedure or function declaration
        // If it is, skip it as it's already added to the procedure_table during parsing.
        // We identify procedures/functions by checking if their type_def links back
        // to a PROCEDURE_DECL or FUNCTION_DECL node in the AST.
        bool is_routine_symbol = (unit_symbol->type_def &&
                                  (unit_symbol->type_def->type == AST_PROCEDURE_DECL ||
                                   unit_symbol->type_def->type == AST_FUNCTION_DECL));

        if (is_routine_symbol) {
             DEBUG_PRINT("[DEBUG] linkUnit: Skipping routine symbol '%s' (type %s) from unit interface, handled by procedure_table.\n",
                         unit_symbol->name, varTypeToString(unit_symbol->type));
             unit_symbol = unit_symbol->next;
             continue; // Skip to the next symbol
        }
        // The original check 'if (unit_symbol->type == TYPE_VOID)' was insufficient
        // because functions have a non-VOID return type.


        // Check for name conflicts (only for variables/constants we are about to insert)
        Symbol *existing = lookupGlobalSymbol(unit_symbol->name);
        if (existing) {
            DEBUG_PRINT("[DEBUG] Skipping already-defined global variable/constant '%s' during linkUnit()\n", unit_symbol->name);
            unit_symbol = unit_symbol->next;
            continue;
        }

        DEBUG_PRINT("[DEBUG] linkUnit: Attempting to insert global variable/constant '%s' (type %s) from unit.\n",
                    unit_symbol->name, varTypeToString(unit_symbol->type));
        // Insert the variable or constant symbol into the global symbol table.
        // insertGlobalSymbol allocates the Value struct and its default content.
        insertGlobalSymbol(unit_symbol->name, unit_symbol->type, unit_symbol->type_def); // NOTE: Your code had NULL here, using unit_symbol->type_def is likely correct.

        Symbol* inserted_check = lookupGlobalSymbol(unit_symbol->name);
        if (inserted_check) {
            DEBUG_PRINT("[DEBUG] linkUnit: Successfully inserted/found global symbol '%s'.\n", inserted_check->name);
        } else {
            fprintf(stderr, "Internal Error: FAILED to find global symbol '%s' immediately after insertion!\n", unit_symbol->name);
            // This is a critical error, maybe EXIT?
            EXIT_FAILURE_HANDLER();
        }

        // If the unit symbol table entry represents a constant and has a value (constants built here do),
        // copy that constant value over the default value created by insertGlobalSymbol.
        // Variables in the interface table have NULL values in the unit's symbol table.
        if (unit_symbol->is_const && unit_symbol->value) {
            // Retrieve the symbol we just inserted (this is the one we will update)
            Symbol *global_sym_entry = lookupGlobalSymbol(unit_symbol->name);
            if (global_sym_entry && global_sym_entry->value) {
                // updateSymbol handles freeing the default value in global_sym_entry->value
                // and copies the content from unit_symbol->value.
                DEBUG_PRINT("[DEBUG] linkUnit: Copying constant value for '%s'.\n", unit_symbol->name);
                updateSymbol(unit_symbol->name, *(unit_symbol->value)); // Pass the Value struct by value
                global_sym_entry->is_const = true; // Ensure it's marked as const globally
            } else {
                 fprintf(stderr, "Internal Error: Could not locate global symbol entry for constant '%s' after insertion.\n", unit_symbol->name);
                 EXIT_FAILURE_HANDLER();
            }
        }
        // ADDED: Copy array value if the unit symbol is an array.
        // Your original code had a case for TYPE_ARRAY but it was empty.
        // This is where you would add the logic to copy array values.
        // If unit_symbol->type == TYPE_ARRAY, the symbol has been inserted globally
        // with a default-initialized array value by insertGlobalSymbol.
        // If unit_symbol->value is also an initialized array (from a unit constant array),
        // you would copy it here.
        else if (unit_symbol->type == TYPE_ARRAY && unit_symbol->value) {
             Symbol *global_sym_entry = lookupGlobalSymbol(unit_symbol->name);
             if (global_sym_entry && global_sym_entry->value) {
                 DEBUG_PRINT("[DEBUG] linkUnit: Copying array value for '%s'.\n", unit_symbol->name);
                 updateSymbol(unit_symbol->name, makeCopyOfValue(unit_symbol->value)); // updateSymbol frees old, copies new
             } else {
                  fprintf(stderr, "Internal Error: Could not locate global symbol entry for array '%s' after insertion.\n", unit_symbol->name);
                  EXIT_FAILURE_HANDLER();
             }
        }
        // Note: Your original code had a switch statement here for other types.
        // This switch statement is relevant for copying the *value* of the symbol
        // from the unit's symbol table entry to the global symbol table entry.
        // It seems you intended to copy values for various types here.
        // Let's include that logic based on your original code structure, but
        // ensure it only runs if unit_symbol->value is not NULL (i.e., for constants).

        // ADDED: Start of the switch statement to copy constant values by type
        switch (unit_symbol->type) {
             case TYPE_INTEGER: /* handled above */ break;
             case TYPE_BYTE: /* handled above */ break;
             case TYPE_WORD: /* handled above */ break;
             case TYPE_REAL: /* handled above */ break;
             case TYPE_STRING: /* handled above */ break;
             case TYPE_CHAR: /* handled above */ break;
             case TYPE_BOOLEAN: /* handled above */ break;
             case TYPE_FILE: /* handled above */ break; // File variables are not typically assigned directly like this
             case TYPE_RECORD: /* handled above */ break;
             case TYPE_ARRAY: /* handled above */ break;
             case TYPE_MEMORYSTREAM: /* handled above */ break; // Similar to File/Record, requires careful copy semantic

             case TYPE_ENUM:
                  // ADDED: Copy enum value if it's a constant
                  if (unit_symbol->is_const && unit_symbol->value) {
                       Symbol *global_sym_entry = lookupGlobalSymbol(unit_symbol->name);
                       if (global_sym_entry && global_sym_entry->value) {
                            DEBUG_PRINT("[DEBUG] linkUnit: Copying enum value for '%s'.\n", unit_symbol->name);
                            // Free the default enum name string created by insertGlobalSymbol
                            if(global_sym_entry->value->enum_val.enum_name) free(global_sym_entry->value->enum_val.enum_name);
                            // Copy the unit's enum value content
                            global_sym_entry->value->enum_val.enum_name = unit_symbol->value->enum_val.enum_name ? strdup(unit_symbol->value->enum_val.enum_name) : NULL;
                            global_sym_entry->value->enum_val.ordinal = unit_symbol->value->enum_val.ordinal;
                            global_sym_entry->value->type = TYPE_ENUM; // Ensure type is set
                            global_sym_entry->is_const = true;
                       } else {
                            fprintf(stderr, "Internal Error: Could not locate global symbol entry for enum constant '%s'.\n", unit_symbol->name);
                            EXIT_FAILURE_HANDLER();
                       }
                  }
                  break;

             case TYPE_SET:
                   // ADDED: Copy set value if it's a constant
                  if (unit_symbol->is_const && unit_symbol->value) {
                       Symbol *global_sym_entry = lookupGlobalSymbol(unit_symbol->name);
                       if (global_sym_entry && global_sym_entry->value) {
                            DEBUG_PRINT("[DEBUG] linkUnit: Copying set value for '%s'.\n", unit_symbol->name);
                            // Free the default set allocation by insertGlobalSymbol
                            freeValue(global_sym_entry->value);
                            // Copy the unit's set value (deep copy)
                            *(global_sym_entry->value) = makeCopyOfValue(unit_symbol->value);
                            global_sym_entry->is_const = true;
                       } else {
                             fprintf(stderr, "Internal Error: Could not locate global symbol entry for set constant '%s'.\n", unit_symbol->name);
                             EXIT_FAILURE_HANDLER();
                       }
                  }
                 break;

             case TYPE_POINTER:
                  // ADDED: Copy pointer value (the address and base type node) if it's a constant
                  if (unit_symbol->is_const && unit_symbol->value) {
                       Symbol *global_sym_entry = lookupGlobalSymbol(unit_symbol->name);
                       if (global_sym_entry && global_sym_entry->value) {
                            DEBUG_PRINT("[DEBUG] linkUnit: Copying pointer value for '%s'.\n", unit_symbol->name);
                            // Pointer value is just the ptr_val and base_type_node (shallow copy of node pointer)
                            global_sym_entry->value->ptr_val = unit_symbol->value->ptr_val;
                            // The base_type_node should ideally come from the *global* symbol's type definition
                            // which was linked by insertGlobalSymbol using unit_symbol->type_def.
                            // We should NOT overwrite the global sym's base_type_node with the unit sym's.
                            // The base_type_node is part of the type definition AST structure, not the Value data itself.
                            // So only copy the ptr_val.
                            // global_sym_entry->value->base_type_node = unit_symbol->value->base_type_node; // DO NOT DO THIS
                            global_sym_entry->is_const = true;
                       } else {
                            fprintf(stderr, "Internal Error: Could not locate global symbol entry for pointer constant '%s'.\n", unit_symbol->name);
                            EXIT_FAILURE_HANDLER();
                       }
                  }
                 break;

             default:
                 // Any other types that were not explicitly handled above, and are constants with values
                 if (unit_symbol->is_const && unit_symbol->value) {
                      fprintf(stderr, "Warning: Unhandled constant type %s for copying value during unit linking.\n", varTypeToString(unit_symbol->type));
                 }
                 break;
        }
        // ADDED: End of the switch statement


        unit_symbol = unit_symbol->next;
    }

    // ADDED: Free the temporary unit symbol table after processing.
    // This list was created by buildUnitSymbolTable in unitParser and attached to unit_ast->symbol_table.
    // Its contents (for variables/constants) have now been copied into the global symbol table.
    // The Symbol structs and their contents (for constants) are no longer needed.
    if (unit_ast->symbol_table) {
        DEBUG_PRINT("[DEBUG] linkUnit: Freeing unit symbol table for '%s' at %p\n",
                    unit_ast->token ? unit_ast->token->value : "NULL",
                    (void*)unit_ast->symbol_table);
        freeUnitSymbolTable(unit_ast->symbol_table);
        unit_ast->symbol_table = NULL; // Crucial: set to NULL to prevent double free attempts via AST freeing
    }


    // Handle type definitions...
    // Type definitions themselves are added to the global type_table by insertType in unitParser.
    // Their AST nodes are owned by the type_table and freed by freeTypeTableASTNodes.
    // The loop below iterates over declaration nodes, which are part of the unit_ast.
    // These nodes will be freed when unit_ast is freed (see changes below).
    AST *type_decl = unit_ast->right; // Assuming right contains decls in some AST structure
    while (type_decl && type_decl->type == AST_TYPE_DECL) {
        // This loop seems correct for its purpose (registering types globally),
        // no memory leak here as the AST nodes are linked into type_table
        // and freed by freeTypeTableASTNodes.
        insertType(type_decl->token->value, type_decl->left);
        type_decl = type_decl->right; // Move to next sibling? Or structure is different?
    }


    // Handle nested uses clauses
    // The logic here uses unitParser to get a new AST for the nested unit
    // and then recursively calls linkUnit on that new AST.
    if (unit_ast->left && unit_ast->left->type == AST_USES_CLAUSE) {
        AST *uses_clause = unit_ast->left;
        List *unit_list = uses_clause->unit_list; // List owns the unit name strings
        for (int i = 0; i < listSize(unit_list); i++) {
            char *unit_name = listGet(unit_list, i); // listGet does not copy name
            char *unit_path = findUnitFile(unit_name); // findUnitFile allocates memory
            if (unit_path == NULL) {
                fprintf(stderr, "Error: Unit '%s' not found.\n", unit_name);
                EXIT_FAILURE_HANDLER(); // Exit if a used unit is not found
            }
             // ADDED: Code to read the file into a buffer named nested_source.
             char *nested_source = NULL;
             FILE *nested_file = fopen(unit_path, "r");
             if (!nested_file) {
                  char error_msg[512];
                  snprintf(error_msg, sizeof(error_msg), "Could not open unit file '%s' for unit '%s'", unit_path, unit_name);
                  perror(error_msg);
                  free(unit_path);
                  EXIT_FAILURE_HANDLER();
             }
             fseek(nested_file, 0, SEEK_END);
             long nested_fsize = ftell(nested_file);
             rewind(nested_file);
             nested_source = malloc(nested_fsize + 1);
             if (!nested_source) {
                 fprintf(stderr, "Memory allocation error reading unit '%s'\n", unit_name);
                 fclose(nested_file); free(unit_path);
                 EXIT_FAILURE_HANDLER();
             }
             fread(nested_source, 1, nested_fsize, nested_file);
             nested_source[nested_fsize] = '\0';
             fclose(nested_file);
             free(unit_path); // Free the path string after reading


            Lexer lexer; // Local lexer instance
            initLexer(&lexer, nested_source); // CORRECTED: Use the source buffer

            Parser unit_parser_instance; // Local parser instance
            unit_parser_instance.lexer = &lexer;
            unit_parser_instance.current_token = getNextToken(&lexer); // getNextToken allocates the first token

            AST *linked_unit_ast = unitParser(&unit_parser_instance, recursion_depth + 1, unit_name);

            // ADDED: Free the allocated nested_source buffer if it was allocated
            if (nested_source) free(nested_source);

            // ADDED: Free the final token owned by the nested parser instance
            // This token might be the EOF token or an error token if parsing failed/ended early.
            if (unit_parser_instance.current_token) {
                freeToken(unit_parser_instance.current_token);
                unit_parser_instance.current_token = NULL;
            }


            if (!linked_unit_ast) { /* unitParser already reported error, continue to next unit */ continue; }

            // Recursively call linkUnit on the parsed nested unit AST.
            // This call will process the nested unit's symbols (variables/constants),
            // free its symbol_table (using the logic above), and free the nested_unit_ast itself
            // at the end of *that* call (using the logic at the very end of this function).
            linkUnit(linked_unit_ast, recursion_depth + 1);

            // No need to free linked_unit_ast here, the recursive call handles it.
        } // End for loop
    } // End if uses_clause

    // ADDED: Free the unit_ast itself after its contents (symbols/types/uses) are linked.
    // The main Program AST (parsed in main.c) is the root and is freed by freeAST(GlobalAST) in main.
    // AST nodes created by unitParser *within the 'uses' loop* are of type AST_UNIT
    // and are not part of the main Program AST structure. They are temporary and need to be freed here.
    if (unit_ast->type == AST_UNIT) {
        DEBUG_PRINT("[DEBUG] linkUnit: Freeing unit_ast for '%s' at %p\n",
                    unit_ast->token ? unit_ast->token->value : "NULL",
                    (void*)unit_ast);
        freeAST(unit_ast); // This will recursively free its children, token, etc.
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
                Value v = eval(decl->left); // evaluated constant expression
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
                sym->next = NULL;
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
                     varSym->next = NULL;

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
                sym->next = NULL;
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
