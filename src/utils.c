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

void cleanupLocalEnv(void) {
    Symbol *sym;
    while (localSymbols != NULL) {
        sym = localSymbols;
        localSymbols = localSymbols->next;
        free(sym->name);
        free(sym);
    }
}

MemoryStream *createMemoryStream(void) {
    MemoryStream *ms = malloc(sizeof(MemoryStream));
    if (!ms) {
        fprintf(stderr, "Memory allocation error in create_memory_stream\n");
        EXIT_FAILURE_HANDLER();
    }
    ms->buffer = NULL;
    ms->size = 0;
    ms->capacity = 0;
    return ms;
}

void freeMemoryStream(MemoryStream *ms) { // Is this used?  Should it be?
    if (ms) {
        free(ms->buffer);
        free(ms);
    }
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

        // *** FIX: Use makeCopyOfValue for deep copy of the field's value ***
        new_field->value = makeCopyOfValue(&curr->value);

        // *** REMOVE redundant string copy (now handled by makeCopyOfValue) ***
        // if (new_field->value.type == TYPE_STRING && curr->value.s_val != NULL)
        //     new_field->value.s_val = strdup(curr->value.s_val); // This is no longer needed

        new_field->next = NULL;
        *ptr = new_field;
        ptr = &new_field->next;
    }
    return new_head;
}

// Memory
FieldValue *createEmptyRecord(AST *recordType) {
    // Resolve type references
    if (recordType && recordType->type == AST_TYPE_REFERENCE) {
        recordType = recordType->right;
    }

    FieldValue *head = NULL, **ptr = &head;
    
    if (!recordType || recordType->type != AST_RECORD_TYPE) {
        return NULL;  // Invalid record type
    }

    for (int i = 0; i < recordType->child_count; i++) {
        AST *fieldDecl = recordType->children[i];
        VarType fieldType = fieldDecl->var_type;
        AST *fieldTypeDef = fieldDecl->right;

        for (int j = 0; j < fieldDecl->child_count; j++) {
            AST *varNode = fieldDecl->children[j];
            FieldValue *fv = malloc(sizeof(FieldValue));
            fv->name = strdup(varNode->token->value);
            fv->value = makeValueForType(fieldType, fieldTypeDef);
            fv->next = NULL;
            
            *ptr = fv;
            ptr = &fv->next;
        }
    }
    return head;
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
    v.type = type;
    
    switch(type) {
        case TYPE_INTEGER:
            v.i_val = 0;
            break;
        case TYPE_REAL:
            v.r_val = 0.0;
            break;
        case TYPE_STRING:
            v.s_val = strdup("");
            v.max_length = -1;  // Default to dynamic string
            break;
        case TYPE_CHAR:
            v.c_val = ' '; // Default char value
            break;
        case TYPE_BOOLEAN:
            v.i_val = 0;
            break;
        case TYPE_FILE:
            v.f_val = NULL;
            v.filename = NULL;
            break;
        case TYPE_RECORD:
            if (type_def && type_def->type == AST_TYPE_REFERENCE) {
                type_def = type_def->right;
            }
            v.record_val = createEmptyRecord(type_def);
            break;
        case TYPE_ARRAY:
            v.dimensions = 0;
            v.lower_bounds = NULL;
            v.upper_bounds = NULL;
            v.array_val = NULL;
            break;
        case TYPE_MEMORYSTREAM:
            v.mstream = createMemoryStream();
            break;
        case TYPE_ENUM:
            v.enum_val.enum_name = (type_def && type_def->token) ? strdup(type_def->token->value) : strdup("");
            v.enum_val.ordinal = -1;  // Default to "unset"
            break;
        case TYPE_BYTE:
            v.i_val = 0;
            break;
        case TYPE_WORD:
            v.i_val = 0;
            break;
        case TYPE_VOID:
            // No data to assign; leave everything zeroed
            break;
        default:
            fprintf(stderr, "Error creating default value for type %s\n", varTypeToString(type));
            EXIT_FAILURE_HANDLER();
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
Token *copyToken(const Token *orig) {
    if (!orig) return NULL;
    return newToken(orig->type, orig->value);
}


/* Free a token */
void freeToken(Token *token) {
    if (token) {
        free(token->value);
        free(token);
    }
}

void freeProcedureTable(void) {
    Procedure *proc = procedure_table;
    while (proc) {
        Procedure *next = proc->next;
        free(proc->name);
        // --- DO NOT FREE THE AST NODE HERE ---
        // freeAST(proc->proc_decl); // This line causes the double-free/use-after-free
        // --- CHANGE END ---
        free(proc); // Free the Procedure struct itself
        proc = next;
    }
    procedure_table = NULL;
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

Value makeMemoryStream(MemoryStream *ms) {
    Value v;
    v.type = TYPE_MEMORYSTREAM;
    v.mstream = ms;
    return v;
}

Symbol *findInScope(const char *name, Symbol *scope) {
    Symbol *cur = scope;
    while (cur) {
        if (strcmp(cur->name, name) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

void freeValue(Value *v) {
    if (!v) return;

    // *** ADD DEBUG PRINT ***
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_FREE] freeValue called for Value* at %p, type=%s\n",
            (void*)v, varTypeToString(v->type));
#endif
    // *** END DEBUG PRINT ***

    switch (v->type) {
        case TYPE_VOID:
        case TYPE_INTEGER:
        case TYPE_REAL:
        case TYPE_BOOLEAN:
        case TYPE_CHAR:
        case TYPE_ENUM: // Enum name is usually shared/static, ordinal is inline
        case TYPE_BYTE:
        case TYPE_WORD:
            // No heap data associated with the Value struct itself for these
#ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]   No heap data to free for type %s\n", varTypeToString(v->type));
#endif
            break;

        case TYPE_STRING:
            if (v->s_val) {
                // *** ADD DEBUG PRINT ***
#ifdef DEBUG
                fprintf(stderr, "[DEBUG_FREE]   Freeing string content '%s' at %p\n", v->s_val, (void*)v->s_val);
#endif
                // *** END DEBUG PRINT ***
                free(v->s_val);
                v->s_val = NULL;
            } else {
#ifdef DEBUG
                fprintf(stderr, "[DEBUG_FREE]   String content is NULL\n");
#endif
            }
            break;

        case TYPE_RECORD: {
            FieldValue *f = v->record_val;
            // *** ADD DEBUG PRINT ***
#ifdef DEBUG
            fprintf(stderr, "[DEBUG_FREE]   Processing record fields for Value* at %p (record_val=%p)\n", (void*)v, (void*)f);
#endif
            // *** END DEBUG PRINT ***
            while (f) {
                FieldValue *next = f->next;
                // *** ADD DEBUG PRINT ***
#ifdef DEBUG
                fprintf(stderr, "[DEBUG_FREE]     Freeing FieldValue* at %p (name='%s' @ %p)\n",
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
             fprintf(stderr, "[DEBUG_FREE]   Processing array for Value* at %p (array_val=%p)\n", (void*)v, (void*)v->array_val);
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
                   fprintf(stderr, "[DEBUG_FREE]   Warning: Array bounds missing or zero dimensions.\n");
#endif
                 }

                 for (int i = 0; i < total; i++) {
#ifdef DEBUG
                    fprintf(stderr, "[DEBUG_FREE]     Freeing array element %d\n", i); // Added print
#endif
                    freeValue(&v->array_val[i]); // Frees contents of each element
                 }
#ifdef DEBUG
                 fprintf(stderr, "[DEBUG_FREE]   Freeing array data buffer at %p\n", (void*)v->array_val); // Added print
#endif
                 free(v->array_val); // Frees the array of Value structs itself
             }
#ifdef DEBUG
             fprintf(stderr, "[DEBUG_FREE]   Freeing array bounds at %p and %p\n", (void*)v->lower_bounds, (void*)v->upper_bounds); // Added print
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
             fprintf(stderr, "[DEBUG_FREE]   Unhandled type %s in freeValue\n", varTypeToString(v->type));
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
                printf("MemoryStream (size: %d)", sym->value->mstream->size);
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
    base_path = "/Users/mke/Pscal/lib";
#else
    base_path = "/Users/mke/Pscal/lib";
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
                updateSymbol(unit_symbol->name, makeMemoryStream(unit_symbol->value->mstream));
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

int areValuesEqual(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
        case TYPE_INTEGER:   return a.i_val == b.i_val;
        case TYPE_REAL:      return a.r_val == b.r_val;
        case TYPE_STRING:    return strcmp(a.s_val, b.s_val) == 0;
        case TYPE_CHAR:     return a.c_val == b.c_val;
        case TYPE_BOOLEAN:   return a.i_val == b.i_val;
        case TYPE_BYTE:      return a.i_val == b.i_val;
        case TYPE_WORD:      return a.i_val == b.i_val;
        case TYPE_ENUM:      return (strcmp(a.enum_val.enum_name, b.enum_val.enum_name) == 0) && (a.enum_val.ordinal == b.enum_val.ordinal);
        // Add comparisons for other types as needed.
        default:             return 0;
    }
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
