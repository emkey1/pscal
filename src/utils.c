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
        case TYPE_ARRAY:        return "ARRAY";
        case TYPE_BOOLEAN:      return "BOOLEAN";
        case TYPE_MEMORYSTREAM: return "MEMORY_STREAM";
        case TYPE_ENUM:         return "ENUM";
        case TYPE_SET:          return "SET";
        default:                return "UNKNOWN_VAR_TYPE";
    }
}

const char *tokenTypeToString(TokenType type) {
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
        default:
            return "INVALID_TOKEN";
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
        case AST_BREAK:          return "BREAK";
        case AST_ARRAY_LITERAL:  return "ARRAY_LITERAL";
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

        new_field->value = makeCopyOfValue(&curr->value);

        new_field->next = NULL;
        *ptr = new_field;
        ptr = &new_field->next;
    }
    return new_head;
}

FieldValue *createEmptyRecord(AST *recordType) {
    // Resolve type references if necessary
    if (recordType && recordType->type == AST_TYPE_REFERENCE) {
        recordType = recordType->right; // Follow the link
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
    while (fv) {
        FieldValue *next = fv->next;
        free(fv->name);
        if (fv->value.type == TYPE_STRING)
            free(fv->value.s_val);
        free(fv);
        fv = next;
    }
}

// Value constructors
Value makeInt(long long val) {
    Value v;
    v.type = TYPE_INTEGER;
    v.i_val = val;
    return v;
}

Value makeReal(double val) {
    Value v;
    v.type = TYPE_REAL;
    v.r_val = val;
    return v;
}

Value makeByte(unsigned char val) {
    Value v;
    v.type = TYPE_BYTE;
    v.i_val = val;  // Store the byte in the integer field.
    return v;
}

Value makeWord(unsigned int val) {
    Value v;
    v.type = TYPE_WORD;
    v.i_val = val;  // Store the word value in the same field (ensure it stays within 16 bits if needed)
    return v;
}

Value makeString(const char *val) {
    Value v;
    v.type = TYPE_STRING;
    v.max_length = -1; // Indicate dynamic string (no fixed limit relevant here)

    if (val != NULL) {
        size_t len = strlen(val);
        v.s_val = malloc(len + 1); // Allocate exact size needed + null terminator
        if (!v.s_val) {
            fprintf(stderr, "FATAL: Memory allocation failed in makeString (dynamic alloc)\n");
            EXIT_FAILURE_HANDLER();
        }
        strcpy(v.s_val, val); // Copy the whole string
    } else {
        // Handle NULL input -> create an empty string
        v.s_val = malloc(1);
        if (!v.s_val) {
            fprintf(stderr, "FATAL: Memory allocation failed in makeString (null input)\n");
            EXIT_FAILURE_HANDLER();
        }
        v.s_val[0] = '\0';
    }

#ifdef DEBUG
    // Optional: Keep debug print if useful
    // DEBUG_PRINT("[DEBUG] makeString: input='%s', allocated_len=%zu, value='%s'\n",
    //             val ? val : "NULL", v.s_val ? strlen(v.s_val) : 0, v.s_val ? v.s_val : "NULL");
#endif

    return v;
}

Value makeChar(char c) {
    Value v;
    v.type = TYPE_CHAR;
    v.c_val = c;
    v.max_length = 1;

#ifdef DEBUG
    DEBUG_PRINT("[DEBUG] makeChar: char='%c'\n", c);
#endif

    return v;
}

Value makeBoolean(int b) {
    Value v;
    v.type = TYPE_BOOLEAN;
    v.i_val = b ? 1 : 0;
    return v;
}

Value makeFile(FILE *f) {
    Value v;
    v.type = TYPE_FILE;
    v.f_val = f;
    v.filename = NULL;
    return v;
}

Value makeRecord(FieldValue *rec) {
    Value v;
    v.type = TYPE_RECORD;
    v.record_val = rec;
    return v;
}

Value makeArrayND(int dimensions, int *lower_bounds, int *upper_bounds, VarType element_type, AST *type_def) {
    Value v;
    v.type = TYPE_ARRAY;
    v.dimensions = dimensions;
    v.lower_bounds = malloc(sizeof(int) * dimensions);
    v.upper_bounds = malloc(sizeof(int) * dimensions);
    v.element_type = element_type;

    int total_size = 1;

#ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG] makeArrayND: Creating %d-D array, element_type=%s\n",
                dimensions, varTypeToString(element_type));
    }
#endif

    for (int i = 0; i < dimensions; i++) {
        v.lower_bounds[i] = lower_bounds[i];
        v.upper_bounds[i] = upper_bounds[i];
        int size_i = (upper_bounds[i] - lower_bounds[i] + 1);
        total_size *= size_i;

#ifdef DEBUG
        if (dumpExec) {
            fprintf(stderr,
                    "[DEBUG] makeArrayND: Dimension %d => lower_bound=%d, upper_bound=%d, size=%d\n",
                    i+1, lower_bounds[i], upper_bounds[i], size_i);
        }
#endif
    }

    v.array_val = malloc(sizeof(Value) * total_size);
    if (!v.array_val) {
        fprintf(stderr, "Memory allocation error in makeArrayND.\n");
        EXIT_FAILURE_HANDLER();
    }

#ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG] makeArrayND: total_size=%d\n", total_size);
    }
#endif

    for (int i = 0; i < total_size; i++) {
        switch (element_type) {
            case TYPE_INTEGER:
                v.array_val[i] = makeInt(0);
                break;
            case TYPE_REAL:
                v.array_val[i] = makeReal(0.0);
                break;
            case TYPE_STRING:
                v.array_val[i] = makeString("");
                break;
            case TYPE_RECORD:
                v.array_val[i] = makeValueForType(TYPE_RECORD, type_def);
                break;
            case TYPE_FILE:
                v.array_val[i] = makeFile(NULL);
                break;
            case TYPE_BOOLEAN:
                v.array_val[i] = makeBoolean(0);
                break;
            default:
                v.array_val[i] = makeValueForType(element_type, NULL);
                break;
        }
    }

#ifdef DEBUG
    if (dumpExec) {
        fprintf(stderr, "[DEBUG] makeArrayND: Finished initializing array.\n");
    }
#endif

    return v;
}

Value makeVoid(void) {
    Value v;
    v.type = TYPE_VOID;
    return v;
}

Value makeValueForType(VarType type, AST *type_def) {
    Value v;
    memset(&v, 0, sizeof(Value)); // Initialize struct to zero/NULL
    v.type = type;

    switch(type) {
        case TYPE_INTEGER:
            v.i_val = 0;
            break;
        case TYPE_REAL:
            v.r_val = 0.0;
            break;
        case TYPE_STRING: {
            v.s_val = NULL;      // Initialize pointer to NULL
            v.max_length = -1; // Default to dynamic string (-1 or 0 can indicate dynamic)

            // Check if a type definition AST node was provided
            if (type_def != NULL) {
                AST* actualTypeDef = type_def; // Start with the provided node

                // Resolve the type if it's a reference to another defined type
                if (actualTypeDef->type == AST_TYPE_REFERENCE) {
                    // Assuming lookupType finds the actual definition AST
                    actualTypeDef = lookupType(actualTypeDef->token->value);
                     if (!actualTypeDef) {
                          // Handle error: referenced type not found. Default to dynamic string.
                          fprintf(stderr, "Warning: Could not resolve type reference '%s' during string initialization. Defaulting to dynamic.\n",
                                   type_def->token ? type_def->token->value : "<unknown>");
                          actualTypeDef = NULL; // Prevent using invalid pointer below
                     }
                }

                // Now check if the resolved definition represents 'string[N]'
                // This relies on the parser creating an AST_VARIABLE node for 'string'
                // and putting the length AST (e.g., AST_NUMBER) in its 'right' child.
                if (actualTypeDef &&
                    actualTypeDef->type == AST_VARIABLE &&
                    actualTypeDef->token &&
                    strcasecmp(actualTypeDef->token->value, "string") == 0 &&
                    actualTypeDef->right != NULL)
                {
                    AST* lenNode = actualTypeDef->right; // Node representing the length
                    Value lenVal = makeInt(0);          // Temp Value to hold evaluated length
                    bool length_ok = false;            // Flag to track if length was valid

                    // Currently, only support simple integer constants for length
                    // (Evaluating complex constant expressions here can be risky)
                    if (lenNode->type == AST_NUMBER && lenNode->token && lenNode->token->type == TOKEN_INTEGER_CONST) {
                        // Use atoll for safety, although length shouldn't exceed int range typically
                        long long parsed_len = atoll(lenNode->token->value);
                        if (parsed_len > 0 && parsed_len <= 2147483647) { // Check positive and reasonable upper bound
                            lenVal = makeInt(parsed_len);
                            length_ok = true;
                        } else {
                             fprintf(stderr, "Warning: Fixed string length constant %lld is out of valid range (1..%d). Defaulting to dynamic string.\n", parsed_len, 2147483647);
                        }
                    } else {
                         fprintf(stderr, "Warning: Fixed string length is not a simple integer constant. Defaulting to dynamic string.\n");
                    }

                    // If we got a valid positive length, set up the fixed-size string
                    if (length_ok) {
                        v.max_length = (int)lenVal.i_val; // Store the fixed length
                        #ifdef DEBUG
                        fprintf(stderr, "[DEBUG makeValueForType] Setting fixed string length to %d\n", v.max_length);
                        #endif

                        // Allocate buffer for fixed size + null terminator
                        // Use calloc for zero-initialization (ensures empty string initially)
                        v.s_val = calloc(v.max_length + 1, 1);
                        if (!v.s_val) {
                            fprintf(stderr, "FATAL: calloc failed for fixed string buffer in makeValueForType (size %d)\n", v.max_length + 1);
                            EXIT_FAILURE_HANDLER();
                        }
                    }
                    // If length wasn't valid (not const int, <=0), v.max_length remains -1 (dynamic)
                }
                 // If actualTypeDef wasn't 'string[N]', v.max_length remains -1
            } // End if(type_def != NULL)

            // If, after all checks, it's still determined to be dynamic (max_length == -1),
            // allocate storage for an empty dynamic string.
            if (v.max_length == -1) {
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG makeValueForType] Initializing as dynamic empty string.\n");
                 #endif
                 v.s_val = strdup(""); // Use strdup for empty string
                 if (!v.s_val) {
                     fprintf(stderr, "FATAL: strdup(\"\") failed for dynamic string in makeValueForType\n");
                     EXIT_FAILURE_HANDLER();
                 }
            }
            break; // End case TYPE_STRING
        } // End block for case TYPE_STRING
        case TYPE_CHAR:
            v.c_val = '\0'; // Default char value (null char)
            break;
        case TYPE_BOOLEAN:
            v.i_val = 0; // false
            break;
        case TYPE_FILE:
            v.f_val = NULL;
            v.filename = NULL;
            break;
        case TYPE_RECORD:
            // Resolve type references if necessary
            if (type_def && type_def->type == AST_TYPE_REFERENCE) {
                type_def = type_def->right; // Follow the link to the actual definition
            }
            // Create the initial structure for the record fields
            v.record_val = createEmptyRecord(type_def);
            // Check if createEmptyRecord failed (it returns NULL on error now)
            if (!v.record_val && type_def != NULL && type_def->type == AST_RECORD_TYPE) {
                 fprintf(stderr, "Error: createEmptyRecord returned NULL unexpectedly in makeValueForType.\n");
                 // EXIT_FAILURE_HANDLER(); // Exit or allow NULL record_val? Allowing NULL for now.
            }
            break;
        case TYPE_ARRAY:
            // Initialize array fields to NULL/0 - actual allocation happens later if needed
            v.dimensions = 0;
            v.lower_bounds = NULL;
            v.upper_bounds = NULL;
            v.array_val = NULL;
            v.element_type = TYPE_VOID; // Mark as uninitialized element type
            v.element_type_def = NULL; // No definition link yet
            break;
        case TYPE_MEMORYSTREAM:
            v.mstream = createMStream(); // Assumes createMStream handles its errors
            break;
        case TYPE_ENUM:
             // Initialize enum fields
             v.enum_val.ordinal = 0; // Default to the first ordinal value (usually 0)
             // Get the type name from the definition node if possible
             v.enum_val.enum_name = (type_def && type_def->token && type_def->token->value) ? strdup(type_def->token->value) : strdup("<unknown_enum>");
              // <<< ADD DEBUG PRINT HERE >>>
              #ifdef DEBUG
              fprintf(stderr, "[DEBUG MAKEVAL ENUM] Initializing enum. TypeDef Token: '%s'. strdup result: '%s' (addr=%p)\n",
                      (type_def && type_def->token && type_def->token->value) ? type_def->token->value : "<NO_TYPEDEF_TOKEN>",
                      v.enum_val.enum_name ? v.enum_val.enum_name : "<NULL>",
                      (void*)v.enum_val.enum_name);
              #endif
              // <<< END DEBUG PRINT >>>
             if (!v.enum_val.enum_name) { // Check strdup
                  fprintf(stderr, "FATAL: strdup failed for enum_name in makeValueForType\n");
                  EXIT_FAILURE_HANDLER();
             }
             break;
        case TYPE_BYTE:
            v.i_val = 0;
            break;
        case TYPE_WORD:
            v.i_val = 0;
            break;
        case TYPE_SET:
            v.set_val.set_size = 0;
            v.set_val.set_values = NULL;
             break;
        case TYPE_VOID:
            // No data to assign; memset already handled
            break;
        default:
            fprintf(stderr, "Error creating default value for unhandled type %s\n", varTypeToString(type));
            // EXIT_FAILURE_HANDLER(); // Exit might be too harsh, allows program to continue potentially
            break; // Ensure switch is exited
    }
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
    return token;
}

/* Copy a token */
Token *copyToken(const Token *token) {
    if (!token) return NULL;
    Token *new_token = malloc(sizeof(Token));
    if (!new_token) { fprintf(stderr, "Memory allocation error\n"); EXIT_FAILURE_HANDLER(); }
    *new_token = *token; // Copy basic fields
    // Deep copy the string value if it exists
    new_token->value = token->value ? strdup(token->value) : NULL; // Use strdup, check for NULL
    if (token->value && !new_token->value) { // Check if strdup failed
         fprintf(stderr, "Memory allocation error (strdup in copyToken)\n");
         free(new_token); // Free the partially allocated token
         EXIT_FAILURE_HANDLER();
    }
    return new_token;
}

/* Free a token */
void freeToken(Token *token) {
    if (!token) return;
    // --- ADD THIS CHECK AND FREE ---
    if (token->value) {
#ifdef DEBUG // Optional: Use a separate define for free-specific logs
         fprintf(stderr, "[DEBUG] Freeing token value '%s' at %p\n", token->value, (void*)token->value);
#endif
        free(token->value); // Free the duplicated string
        token->value = NULL; // Prevent double-free
    }
    // --- END ADDITION ---
#ifdef DEBUG // Optional: Use a separate define for free-specific logs
     fprintf(stderr, "[DEBUG] Freeing token struct at %p\n", (void*)token);
#endif
    free(token); // Free the token struct itself
}

void freeProcedureTable(void) {
    Procedure *proc = procedure_table;
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_FREE] Starting freeProcedureTable.\n");
#endif
    while (proc) {
        Procedure *next = proc->next;

#ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]  Processing procedure entry: '%s' (AST Node: %p)\n",
                proc->name ? proc->name : "NULL", (void*)proc->proc_decl);
#endif

        // --- ADDED CHECK FOR BUILT-INS ---
        // Only free the AST node if it's a dummy node for a built-in.
        // User-defined procedure ASTs are part of the main tree and freed there.
        if (proc->name && isBuiltin(proc->name)) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]   -> '%s' is a built-in. Freeing associated dummy AST node %p.\n",
                    proc->name, (void*)proc->proc_decl);
#endif
            freeAST(proc->proc_decl); // Free the dummy AST node
        } else {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]   -> '%s' is user-defined or name is NULL. AST node %p will be freed with main tree.\n",
                    proc->name ? proc->name : "NULL", (void*)proc->proc_decl);
#endif
            // Do not free proc->proc_decl here for user-defined procedures
        }
        // --- END ADDED CHECK ---

        // Free the procedure name and the Procedure struct itself
        if (proc->name) {
#ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]   Freeing procedure name '%s' at %p.\n", proc->name, (void*)proc->name);
#endif
            free(proc->name);
        }
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_FREE]   Freeing Procedure struct itself at %p.\n", (void*)proc);
#endif
        free(proc);
        proc = next;
    }
    procedure_table = NULL;
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_FREE] Finished freeProcedureTable.\n");
#endif
}

void freeTypeTable(void) {
    TypeEntry *entry = type_table;
    while (entry) {
        TypeEntry *next = entry->next;
        free(entry->name); // Free the duplicated type name

        free(entry); // Free the TypeEntry struct itself
        entry = next;
    }
    type_table = NULL;
}

Value makeMStream(MStream *ms) {
    Value v;
    v.type = TYPE_MEMORYSTREAM;
    v.mstream = ms;
    return v;
}

void freeValue(Value *v) {
    if (!v) return;

    // *** ADD DEBUG PRINT ***
#ifdef DEBUG
    fprintf(stderr, "[DEBUG] freeValue called for Value* at %p, type=%s\n",
            (void*)v, varTypeToString(v->type));
#endif
    // *** END DEBUG PRINT ***

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
            // *** ADD DEBUG PRINT ***
#ifdef DEBUG
            fprintf(stderr, "[DEBUG]   Processing record fields for Value* at %p (record_val=%p)\n", (void*)v, (void*)f);
#endif
            // *** END DEBUG PRINT ***
            while (f) {
                FieldValue *next = f->next;
                // *** ADD DEBUG PRINT ***
#ifdef DEBUG
                fprintf(stderr, "[DEBUG]     Freeing FieldValue* at %p (name='%s' @ %p)\n",
                        (void*)f, f->name ? f->name : "NULL", (void*)f->name);
#endif
                // *** END DEBUG PRINT ***
                if (f->name) free(f->name);
                freeValue(&f->value); // Recursive call
                free(f);              // Free the FieldValue struct itself
                f = next;
            }
            v->record_val = NULL;
            break;
        }
        case TYPE_ARRAY: {
            // *** ADD DEBUG PRINT ***
#ifdef DEBUG
             fprintf(stderr, "[DEBUG]   Processing array for Value* at %p (array_val=%p)\n", (void*)v, (void*)v->array_val);
#endif
            // *** END DEBUG PRINT ***
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

static void dumpSymbol(Symbol *sym) {
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
void dumpSymbolTable(void) {
    printf("--- Symbol Table Dump ---\n");

    printf("Global Symbols:\n");
    Symbol *sym = globalSymbols;
    if (!sym) {
        printf("  (none)\n");
    }
    while (sym) {
        dumpSymbol(sym);
        sym = sym->next;
    }

    printf("Local Symbols:\n");
    sym = localSymbols;
    if (!sym) {
        printf("  (none)\n");
    }
    while (sym) {
        dumpSymbol(sym);
        sym = sym->next;
    }

    printf("--- End of Symbol Table Dump ---\n");
}

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

    // Ensure that the unit_ast has a symbol table
    if (!unit_ast->symbol_table) {
        fprintf(stderr, "Error: Symbol table for unit is missing.\n");
        EXIT_FAILURE_HANDLER();
    }

    // Iterate through the unit's symbol table and add symbols to the global symbol table
    Symbol *unit_symbol = unit_ast->symbol_table;
    while (unit_symbol) {
        // Check for name conflicts
        Symbol *existing = lookupGlobalSymbol(unit_symbol->name);
        if (existing) {
            DEBUG_PRINT("[DEBUG] Skipping already-defined global '%s' during linkUnit()\n", unit_symbol->name);
            unit_symbol = unit_symbol->next;
            continue;
        }

        // For procedures (which have TYPE_VOID) we assume they are already
        // registered in the procedure table and do not have an associated value.
        if (unit_symbol->type == TYPE_VOID) {
            unit_symbol = unit_symbol->next;
            continue;
        }
        DEBUG_PRINT("[DEBUG] linkUnit: Attempting to insert global symbol '%s' (type %s) from unit.\n",
                    unit_symbol->name, varTypeToString(unit_symbol->type));
        // Insert the symbol into the global symbol table
        insertGlobalSymbol(unit_symbol->name, unit_symbol->type, NULL);
        
        Symbol* inserted_check = lookupGlobalSymbol(unit_symbol->name);
        if (inserted_check) {
             DEBUG_PRINT("[DEBUG] linkUnit: Successfully inserted/found global symbol '%s'.\n", inserted_check->name);
        } else {
             DEBUG_PRINT("[DEBUG] linkUnit: FAILED to find global symbol '%s' immediately after insertion!\n", unit_symbol->name);
        }

        // Retrieve the symbol we just inserted
        Symbol *inserted = lookupGlobalSymbol(unit_symbol->name);
        if (!inserted || !inserted->value || !unit_symbol->value) {
            DEBUG_PRINT("[DEBUG] Skipping update: '%s' has no allocated value\n", unit_symbol->name);
            unit_symbol = unit_symbol->next;
            continue;
        }

        // Deep copy the value to avoid issues with memory management
        switch (unit_symbol->type) {
            case TYPE_INTEGER:
                updateSymbol(unit_symbol->name, makeInt(unit_symbol->value->i_val));
                break;
            case TYPE_BYTE:
                updateSymbol(unit_symbol->name, makeByte(unit_symbol->value->i_val));
                break;
            case TYPE_WORD:
                updateSymbol(unit_symbol->name, makeWord((int)unit_symbol->value->i_val));
                break;
            case TYPE_REAL:
                updateSymbol(unit_symbol->name, makeReal(unit_symbol->value->r_val));
                break;
            case TYPE_STRING:
                updateSymbol(unit_symbol->name, makeString(unit_symbol->value->s_val));
                break;
            case TYPE_CHAR:
                updateSymbol(unit_symbol->name, makeChar(unit_symbol->value->c_val));
                break;
            case TYPE_BOOLEAN:
                updateSymbol(unit_symbol->name, makeBoolean((int)unit_symbol->value->i_val));
                break;
            case TYPE_FILE:
                updateSymbol(unit_symbol->name, makeFile(unit_symbol->value->f_val));
                break;
            case TYPE_RECORD:
                updateSymbol(unit_symbol->name, makeRecord(copyRecord(unit_symbol->value->record_val)));
                break;
            case TYPE_ARRAY:
                // Handle array types appropriately
                break;
            case TYPE_MEMORYSTREAM:
                updateSymbol(unit_symbol->name, makeMStream(unit_symbol->value->mstream));
                break;
            default:
                fprintf(stderr, "Error: Unsupported type %s in unit symbol table.\n", varTypeToString(unit_symbol->type));
                EXIT_FAILURE_HANDLER();
        }

        unit_symbol = unit_symbol->next;
    }

    // Handle type definitions...
    AST *type_decl = unit_ast->right;
    while (type_decl && type_decl->type == AST_TYPE_DECL) {
        insertType(type_decl->token->value, type_decl->left);
        type_decl = type_decl->right;
    }

    // Handle nested uses clauses
    if (unit_ast->left && unit_ast->left->type == AST_USES_CLAUSE) {
        AST *uses_clause = unit_ast->left;
        List *unit_list = uses_clause->unit_list;
        for (int i = 0; i < listSize(unit_list); i++) {
            char *unit_name = listGet(unit_list, i);
            char *unit_path = findUnitFile(unit_name);
            if (unit_path == NULL) {
                fprintf(stderr, "Error: Unit '%s' not found.\n", unit_name);
                EXIT_FAILURE_HANDLER();
            }
            Lexer lexer;
            initLexer(&lexer, unit_path);
            Parser unit_parser_instance;
            unit_parser_instance.lexer = &lexer;
            unit_parser_instance.current_token = getNextToken(&lexer);
            AST *linked_unit_ast = unitParser(&unit_parser_instance, recursion_depth + 1);
            linkUnit(linked_unit_ast, recursion_depth);
        }
    }
}

// buildUnitSymbolTable traverses the interface AST node and creates a symbol table
// containing all exported symbols (variables, procedures, functions, types) for the unit.
// buildUnitSymbolTable traverses the unit's interface AST node and builds a linked list
// of Symbols for all exported constants, variables, and procedures/functions.
Symbol *buildUnitSymbolTable(AST *interface_ast) {
    if (!interface_ast) return NULL;
    
    Symbol *unitSymbols = NULL;
    
    // Iterate over all declarations in the interface.
    for (int i = 0; i < interface_ast->child_count; i++) {
        AST *decl = interface_ast->children[i];
        if (!decl) continue;
        
        switch(decl->type) {
            case AST_CONST_DECL: {
                if (!decl->token) break;

                Value v = eval(decl->left);  // evaluated constant expression

                Symbol *sym = malloc(sizeof(Symbol));
                if (!sym) {
                    fprintf(stderr, "Memory allocation error in buildUnitSymbolTable\n");
                    EXIT_FAILURE_HANDLER();
                }

                sym->name = strdup(decl->token->value);
                sym->value = malloc(sizeof(Value));
                if (!sym->value) {
                    fprintf(stderr, "Memory allocation error in buildUnitSymbolTable\n");
                    EXIT_FAILURE_HANDLER();
                }

                *sym->value = makeCopyOfValue(&v);      // deep copy the evaluated value
                setTypeValue(sym->value, v.type);       // ensure type tag is set correctly
                sym->type = v.type;

                if (v.type == TYPE_ENUM) {
                    sym->value->enum_meta = v.enum_meta;
                }

                sym->next = unitSymbols;
                unitSymbols = sym;
                break;
            }
            case AST_VAR_DECL: {
                 for (int j = 0; j < decl->child_count; j++) {
                     AST *varNode = decl->children[j];
                     if (!varNode || !varNode->token) continue;
                     // --- ADD DEBUG PRINT ---
                     DEBUG_PRINT("[DEBUG] buildUnitSymbolTable: Found VAR '%s' with type %s in interface.\n",
                                 varNode->token->value, varTypeToString(decl->var_type));
                     // --- END DEBUG PRINT ---
                     Symbol *sym = malloc(sizeof(Symbol)); /* ... null check ... */
                     sym->name = strdup(varNode->token->value);
                     sym->type = decl->var_type;
                     sym->type_def = decl->right; // <<< Store type def link
                     sym->value = malloc(sizeof(Value)); /* ... null check ... */
                     *sym->value = makeValueForType(sym->type, sym->type_def); // Pass type_def
                     sym->next = unitSymbols;
                     unitSymbols = sym;
                 }
                 break;
             }
            case AST_PROCEDURE_DECL:
            case AST_FUNCTION_DECL: {
                // For routines, use the declaration's token as the name.
                if (!decl->token) break;
                Symbol *sym = malloc(sizeof(Symbol));
                if (!sym) {
                    fprintf(stderr, "Memory allocation error in buildUnitSymbolTable\n");
                    EXIT_FAILURE_HANDLER();
                }
                sym->name = strdup(decl->token->value);
                if (decl->type == AST_FUNCTION_DECL && decl->right) {
                    // decl->right holds the return type node.
                    sym->type = decl->right->var_type;
                } else {
                    // For procedures, use TYPE_VOID (or a dummy type as desired).
                    sym->type = TYPE_VOID;
                }
                sym->value = malloc(sizeof(Value));
                if (!sym->value) {
                    fprintf(stderr, "Memory allocation error in buildUnitSymbolTable\n");
                    EXIT_FAILURE_HANDLER();
                }
                *sym->value = makeValueForType(sym->type, NULL);
                sym->next = unitSymbols;
                unitSymbols = sym;
                break;
            }
            default:
                // Skip other declaration types (e.g. AST_TYPE_DECL is handled separately in linkUnit())
                break;
        }
    }
    
    return unitSymbols;
}

Value makeEnum(const char *enum_name, int ordinal) {
    Value v;
    v.type = TYPE_ENUM;
    v.enum_val.enum_name = enum_name ? strdup(enum_name) : NULL;
    v.enum_val.ordinal = ordinal;
    return v;
}

// Helper function to get terminal dimensions
// Returns 0 on success, -1 on error. Fills rows and cols.
int getTerminalSize(int *rows, int *cols) {
    // Default values in case of error or non-TTY
    *rows = 24; // Default height
    *cols = 80; // Default width

    // Check if stdout is a terminal
    if (!isatty(STDOUT_FILENO)) {
        fprintf(stderr, "Warning: Cannot get terminal size (stdout is not a TTY).\n");
        return 0; // Return default size for non-TTY
    }

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        perror("getTerminalSize: ioctl(TIOCGWINSZ) failed");
        return -1; // Indicate an error occurred
    }

    // Check for valid size values
    if (ws.ws_row > 0 && ws.ws_col > 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
    } else {
        // ioctl succeeded but returned 0 size, use defaults
        fprintf(stderr, "Warning: ioctl(TIOCGWINSZ) returned zero size, using defaults.\n");
    }

    return 0; // Success
}
