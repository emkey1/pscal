#include "builtin.h"
#include "parser.h"
#include "utils.h"
#include "interpreter.h"
#include "symbol.h"
#include "globals.h"
#include <math.h>
#include <termios.h>
#include <unistd.h>
#include <unistd.h>     // For read, write, STDIN_FILENO, STDOUT_FILENO, isatty
#include <ctype.h>      // For isdigit
#include <errno.h>
#include <sys/ioctl.h> // For ioctl, FIONREAD

// Comparison function for bsearch (case-insensitive)
static int compareBuiltinMappings(const void *key, const void *element) {
    const char *target_name = (const char *)key;
    const BuiltinMapping *mapping = (const BuiltinMapping *)element;
    return strcasecmp(target_name, mapping->name);
}

// Define the dispatch table - MUST BE SORTED ALPHABETICALLY BY NAME (lowercase)
static const BuiltinMapping builtin_dispatch_table[] = {
    {"abs",       executeBuiltinAbs},
    {"api_receive", executeBuiltinAPIReceive}, // From builtin_network_api.c
    {"api_send",  executeBuiltinAPISend},      // From builtin_network_api.c
    {"assign",    executeBuiltinAssign},
    {"chr",       executeBuiltinChr},
    {"close",     executeBuiltinClose},
    {"copy",      executeBuiltinCopy},
    {"cos",       executeBuiltinCos},
    {"dec",       executeBuiltinDec},         // Include Dec
    {"delay",     executeBuiltinDelay},
    {"eof",       executeBuiltinEOF},
    {"exp",       executeBuiltinExp},
    {"halt",      executeBuiltinHalt},
    {"high",      executeBuiltinHigh},
    {"inc",       executeBuiltinInc},
    {"inttostr",  executeBuiltinIntToStr},
    {"ioresult",  executeBuiltinIOResult},
    {"keypressed", executeBuiltinKeyPressed},
    {"length",    executeBuiltinLength},
    {"ln",        executeBuiltinLn},
    {"low",       executeBuiltinLow},
    {"mstreamcreate", executeBuiltinMstreamCreate},
    {"mstreamfree", executeBuiltinMstreamFree},
    {"mstreamloadfromfile", executeBuiltinMstreamLoadFromFile},
    {"mstreamsavetofile", executeBuiltinMstreamSaveToFile}, // Corrected name based on registration
    {"ord",       executeBuiltinOrd},
    {"paramcount", executeBuiltinParamcount},
    {"paramstr",  executeBuiltinParamstr},
    {"pos",       executeBuiltinPos},
    {"random",    executeBuiltinRandom},
    {"randomize", executeBuiltinRandomize},
    {"readkey",   executeBuiltinReadKey},
    {"reset",     executeBuiltinReset},
    // {"result",    executeBuiltinResult}, // 'result' is special, handled differently? Let's assume not dispatched here.
    {"rewrite",   executeBuiltinRewrite},
    {"screencols", executeBuiltinScreenCols},
    {"screenrows", executeBuiltinScreenRows},
    {"sin",       executeBuiltinSin},
    {"sqrt",      executeBuiltinSqrt},
    {"succ",      executeBuiltinSucc},        // Include Succ
    {"tan",       executeBuiltinTan},         // Include Tan
    {"trunc",     executeBuiltinTrunc},
    {"upcase",    executeBuiltinUpcase},
    {"wherex",    executeBuiltinWhereX},
    {"wherey",    executeBuiltinWhereY}
    // Add Write/Writeln/Read/Readln if you want them dispatched here,
    // but they are currently handled directly in the interpreter's main switch.
};

// Calculate the number of entries in the table
static const size_t num_builtins = sizeof(builtin_dispatch_table) / sizeof(builtin_dispatch_table[0]);

void assignValueToLValue(AST *lvalueNode, Value newValue) {
    if (!lvalueNode) {
        fprintf(stderr, "Runtime error: Cannot assign to NULL lvalue node.\n");
        EXIT_FAILURE_HANDLER();
    }

    if (lvalueNode->type == AST_VARIABLE) {
        // Simple variable assignment - use updateSymbol which handles everything
        if (!lvalueNode->token || !lvalueNode->token->value) {
            fprintf(stderr, "Runtime error: Invalid AST_VARIABLE node in assignValueToLValue.\n"); EXIT_FAILURE_HANDLER();
        }
        updateSymbol(lvalueNode->token->value, newValue);

    } else if (lvalueNode->type == AST_FIELD_ACCESS) {
        // Record field assignment
        // 1. Find the base record symbol
        AST* baseVarNode = lvalueNode->left;
        while(baseVarNode && baseVarNode->type != AST_VARIABLE) { // Simple traversal - enhance if needed
             if (baseVarNode->left) baseVarNode = baseVarNode->left;
             else { fprintf(stderr,"Runtime error: Cannot find base var for field assign in assignValueToLValue\n"); EXIT_FAILURE_HANDLER(); }
        }
         if (!baseVarNode || baseVarNode->type != AST_VARIABLE || !baseVarNode->token) { fprintf(stderr,"Runtime error: Invalid base variable node for field assign in assignValueToLValue\n"); EXIT_FAILURE_HANDLER();}
         Symbol *recSym = lookupSymbol(baseVarNode->token->value);
         if (!recSym || !recSym->value || recSym->value->type != TYPE_RECORD) { fprintf(stderr,"Runtime error: Base variable '%s' is not a record in assignValueToLValue\n", baseVarNode->token ? baseVarNode->token->value : "?"); EXIT_FAILURE_HANDLER(); }
         if (recSym->is_const) { fprintf(stderr,"Runtime error: Cannot assign to field of constant '%s'\n", recSym->name); EXIT_FAILURE_HANDLER(); }

        // 2. Find the specific field *within the symbol's actual value*
        FieldValue *field = recSym->value->record_val;
        const char *targetFieldName = lvalueNode->token ? lvalueNode->token->value : NULL;
        if (!targetFieldName) { fprintf(stderr,"Runtime error: Invalid FIELD_ACCESS node (missing token) in assignValueToLValue\n"); EXIT_FAILURE_HANDLER();}
        while (field) {
            if (field->name && strcmp(field->name, targetFieldName) == 0) {
                 // Found the field to update

                 // 3. Check type compatibility (optional but recommended)
                 if (field->value.type != newValue.type) {
                       bool compatible = false;
                       if (field->value.type == TYPE_REAL && newValue.type == TYPE_INTEGER) compatible = true;
                       else if (field->value.type == TYPE_STRING && newValue.type == TYPE_CHAR) compatible = true;
                       // ... add others ...

                       if (!compatible && typeWarn) {
                           fprintf(stderr, "Warning: Type mismatch assigning to field '%s.%s'. Expected %s, got %s.\n",
                                   recSym->name, targetFieldName, varTypeToString(field->value.type), varTypeToString(newValue.type));
                       }
                       // Perform promotion on newValue if needed
                       if (field->value.type == TYPE_REAL && newValue.type == TYPE_INTEGER) {
                           newValue.r_val = (double)newValue.i_val; newValue.type = TYPE_REAL;
                       }
                       // Add other necessary promotions here...
                 }

                 // 4. Free the *current* value stored in the field
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG ASSIGN_LVAL] Freeing old value for field '%s'\n", field->name);
                 #endif
                 freeValue(&field->value); // Frees heap data held by the current field value

                 // 5. Assign a DEEP COPY of the newValue into the field
                 #ifdef DEBUG
                 fprintf(stderr, "[DEBUG ASSIGN_LVAL] Assigning new value (type %s) to field '%s'\n", varTypeToString(newValue.type), field->name);
                 #endif
                 field->value = makeCopyOfValue(&newValue); // makeCopyOfValue performs deep copy

                 return; // Assignment done
            }
            field = field->next;
        }
        fprintf(stderr, "Runtime error: Field '%s' not found in record '%s' for assignment.\n", targetFieldName, recSym->name);
        EXIT_FAILURE_HANDLER();

    } else if (lvalueNode->type == AST_ARRAY_ACCESS) {
         // Array element assignment
         // 1. Find the base array/string symbol
         AST* baseVarNode = lvalueNode->left;
         while(baseVarNode && baseVarNode->type != AST_VARIABLE) { /* traversal */ if (baseVarNode->left) baseVarNode=baseVarNode->left; else { /* error */ } }
         if (!baseVarNode || baseVarNode->type != AST_VARIABLE || !baseVarNode->token) { /* Error */ }
         Symbol *arrSym = lookupSymbol(baseVarNode->token->value);
         if (!arrSym || !arrSym->value || (arrSym->value->type != TYPE_ARRAY && arrSym->value->type != TYPE_STRING)) { /* Error */ }
         if (arrSym->is_const) { /* Error */ }

         // Handle string element assignment
         if (arrSym->value->type == TYPE_STRING) {
              if (lvalueNode->child_count != 1) { fprintf(stderr, "Runtime error: String assignment requires exactly one index\n"); EXIT_FAILURE_HANDLER(); }
              if (newValue.type != TYPE_CHAR && !(newValue.type == TYPE_STRING && newValue.s_val && strlen(newValue.s_val)==1) ) {
                    fprintf(stderr, "Runtime error: Assignment to string index requires char or single-char string.\n"); EXIT_FAILURE_HANDLER();
              }
              Value indexVal = eval(lvalueNode->children[0]);
              if(indexVal.type != TYPE_INTEGER) { fprintf(stderr, "Runtime error: String index must be an integer.\n"); EXIT_FAILURE_HANDLER(); }
              long long idx = indexVal.i_val;
              int len = arrSym->value->s_val ? (int)strlen(arrSym->value->s_val) : 0;
              if (idx < 1 || idx > len) { fprintf(stderr, "Runtime error: String index %lld out of bounds [1..%d] for assignment.\n", idx, len); EXIT_FAILURE_HANDLER(); }

              char char_to_assign = (newValue.type == TYPE_CHAR) ? newValue.c_val : newValue.s_val[0];
               if (!arrSym->value->s_val) { /* Should not happen */ } else { arrSym->value->s_val[idx - 1] = char_to_assign; }
         }
         // Handle array element assignment
         else if (arrSym->value->type == TYPE_ARRAY) {
             if (!arrSym->value->array_val) { fprintf(stderr, "Runtime error: Array '%s' not initialized before assignment.\n", arrSym->name); EXIT_FAILURE_HANDLER(); }
             if (lvalueNode->child_count != arrSym->value->dimensions) { fprintf(stderr, "Runtime error: Incorrect number of indices for array '%s'.\n", arrSym->name); EXIT_FAILURE_HANDLER(); }

             // Calculate indices and offset
             int *indices = malloc(sizeof(int) * arrSym->value->dimensions);
             if (!indices) { fprintf(stderr,"FATAL: Malloc failed for indices array\n"); EXIT_FAILURE_HANDLER(); }
             for (int i = 0; i < lvalueNode->child_count; i++) {
                  Value idxVal = eval(lvalueNode->children[i]);
                  if (idxVal.type != TYPE_INTEGER) { fprintf(stderr,"Runtime error: Array index must be integer\n"); free(indices); EXIT_FAILURE_HANDLER(); }
                  indices[i] = (int)idxVal.i_val;
             }
             int offset = computeFlatOffset(arrSym->value, indices);
             // Bounds check offset... (You need to calculate total_size based on bounds)
             int total_size = 1;
             for(int d=0; d<arrSym->value->dimensions; ++d) total_size *= (arrSym->value->upper_bounds[d] - arrSym->value->lower_bounds[d] + 1);
             if (offset < 0 || offset >= total_size) { fprintf(stderr, "Runtime error: Array index out of bounds (offset %d, size %d).\n", offset, total_size); free(indices); EXIT_FAILURE_HANDLER(); }

             // Check type compatibility
             VarType elementType = arrSym->value->element_type;
             if (elementType != newValue.type) {
                  // Add compatibility checks/promotions for newValue if needed
                   bool compatible = false;
                   if(elementType == TYPE_REAL && newValue.type == TYPE_INTEGER) compatible = true;
                   // ... other compatible types ...
                   if(!compatible && typeWarn) { fprintf(stderr, "Warning: Type mismatch assigning to array '%s' element.\n", arrSym->name); }
                   // Perform promotion if necessary
                   if (elementType == TYPE_REAL && newValue.type == TYPE_INTEGER) { newValue.r_val = (double)newValue.i_val; newValue.type = TYPE_REAL; }
             }

             // Find target element
             Value *targetElement = &(arrSym->value->array_val[offset]);

             // Free existing element value
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG ASSIGN_LVAL] Freeing old value for array element at offset %d\n", offset);
             #endif
             freeValue(targetElement);

             // Assign deep copy of new value
             #ifdef DEBUG
             fprintf(stderr, "[DEBUG ASSIGN_LVAL] Assigning new value (type %s) to array element at offset %d\n", varTypeToString(newValue.type), offset);
             #endif
             *targetElement = makeCopyOfValue(&newValue);

             free(indices);
         }
    } else {
        fprintf(stderr, "Runtime error: Cannot assign to the given expression type (%s).\n", astTypeToString(lvalueNode->type));
        EXIT_FAILURE_HANDLER();
    }
}

// Attempts to get the current cursor position using ANSI DSR query.
// Returns 0 on success, -1 on failure.
// Stores results in *row and *col.
int getCursorPosition(int *row, int *col) {
    struct termios oldt, newt;
    char buf[32];       // Buffer for response: ESC[<row>;<col>R
    int i = 0;
    char ch;
    int ret_status = -1; // Default to critical failure
    int read_errno = 0; // Store errno from read() operation

    // Default row/col in case of non-critical failure
    *row = 1;
    *col = 1;

    // --- Check if Input is a Terminal ---
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "Warning: Cannot get cursor position (stdin is not a TTY).\n");
        return 0; // Treat as non-critical failure, return default 1,1
    }

    // --- Save Current Terminal Settings ---
    if (tcgetattr(STDIN_FILENO, &oldt) < 0) {
        perror("getCursorPosition: tcgetattr failed");
        return -1; // Critical failure
    }

    // --- Prepare and Set Raw Mode ---
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    newt.c_cc[VMIN] = 0;              // Non-blocking read
    newt.c_cc[VTIME] = 2;             // Timeout 0.2 seconds (adjust if needed)

    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0) {
        int setup_errno = errno;
        perror("getCursorPosition: tcsetattr (set raw) failed");
        // Attempt to restore original settings even if setting new ones failed
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Best effort restore
        errno = setup_errno; // Restore errno for accurate reporting
        return -1; // Critical failure
    }

    // --- Write DSR Query ---
    const char *dsr_query = "\x1B[6n"; // ANSI Device Status Report for cursor position
    if (write(STDOUT_FILENO, dsr_query, strlen(dsr_query)) == -1) {
        int write_errno = errno;
        perror("getCursorPosition: write DSR query failed");
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore terminal settings
        errno = write_errno;
        return -1; // Critical failure
    }
    // Ensure the query is sent immediately
    // fflush(stdout); // Usually not needed for STDOUT if line-buffered, but can add if experiencing delays

    // --- Read Response ---
    memset(buf, 0, sizeof(buf));
    i = 0;
    while (i < sizeof(buf) - 1) {
        errno = 0; // Clear errno before read
        ssize_t bytes_read = read(STDIN_FILENO, &ch, 1);
        read_errno = errno; // Store errno immediately after read

        if (bytes_read < 0) { // Read error
             // Check if it was just a timeout (EAGAIN/EWOULDBLOCK) or a real error
             if (read_errno == EAGAIN || read_errno == EWOULDBLOCK) {
                 fprintf(stderr, "Warning: Timeout waiting for cursor position response.\n");
             } else {
                 perror("getCursorPosition: read failed");
             }
             break; // Exit loop on any read error or timeout
        }
        if (bytes_read == 0) { // Should not happen with VTIME > 0 unless EOF
             fprintf(stderr, "Warning: Read 0 bytes waiting for cursor position (EOF?).\n");
             break;
        }

        // Store character and check for terminator 'R'
        buf[i++] = ch;
        if (ch == 'R') {
            break; // End of response sequence found
        }
    }
    buf[i] = '\0'; // Null-terminate the buffer

    // --- Restore Original Terminal Settings ---
    if (tcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0) {
        perror("getCursorPosition: tcsetattr (restore) failed - Terminal state may be unstable!");
        // Continue processing, but be aware terminal might be left in raw mode
    }

    // --- Parse Response ---
    // Expected format: \x1B[<row>;<col>R
    int parsed_row = 0, parsed_col = 0;
    if (i > 0 && buf[0] == '\x1B' && buf[1] == '[' && buf[i-1] == 'R') {
        // Attempt to parse using sscanf
        if (sscanf(buf, "\x1B[%d;%dR", &parsed_row, &parsed_col) == 2) {
            *row = parsed_row;
            *col = parsed_col;
            ret_status = 0; // Success!
#ifdef DEBUG
            if (dumpExec) fprintf(stderr, "[DEBUG] getCursorPosition: Parsed Row=%d, Col=%d from response '%s'\n", *row, *col, buf);
#endif
        } else {
#ifdef DEBUG
             if (dumpExec) fprintf(stderr, "Warning: Failed to parse cursor position response values: '%s'\n", buf);
#endif
             ret_status = 0; // Non-critical failure, return default 1,1
        }
    } else {
#ifdef DEBUG
         if (dumpExec) fprintf(stderr, "Warning: Invalid or incomplete cursor position response format: '%s'\n", buf);
#endif
         ret_status = 0; // Non-critical failure, return default 1,1
    }

    return ret_status; // 0 for success or non-critical error, -1 for critical error
}

// Math Functions
Value executeBuiltinCos(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: cos expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    double x = (arg.type == TYPE_INTEGER ? arg.i_val : arg.r_val);
    return makeReal(cos(x));
}

Value executeBuiltinSin(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: sin expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    double x = (arg.type == TYPE_INTEGER ? arg.i_val : arg.r_val);
    return makeReal(sin(x));
}

Value executeBuiltinTan(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: tan expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    double x = (arg.type == TYPE_INTEGER ? arg.i_val : arg.r_val);
    return makeReal(tan(x));
}   

Value executeBuiltinSqrt(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: sqrt expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    double x = (arg.type == TYPE_INTEGER ? (double)arg.i_val : arg.r_val); // Promote int to double
    if (x < 0) {
        fprintf(stderr, "Runtime error: sqrt expects a non-negative argument.\n");
        freeValue(&arg); // Free evaluated arg before exit
        EXIT_FAILURE_HANDLER();
    }
    Value result = makeReal(sqrt(x));

    // --- ADDED: Free the evaluated argument ---
    freeValue(&arg);

    return result;
}

Value executeBuiltinLn(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: ln expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    double x = (arg.type == TYPE_INTEGER ? arg.i_val : arg.r_val);
    if (x <= 0) { fprintf(stderr, "Runtime error: ln expects a positive argument.\n"); EXIT_FAILURE_HANDLER(); }
    return makeReal(log(x));
}

Value executeBuiltinExp(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: exp expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    double x = (arg.type == TYPE_INTEGER ? arg.i_val : arg.r_val);
    return makeReal(exp(x));
}

Value executeBuiltinAbs(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: abs expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    Value result = makeInt(0); // Declare result value

    if (arg.type == TYPE_INTEGER)
        result = makeInt(llabs(arg.i_val));
    else if (arg.type == TYPE_REAL) // Assume numeric if not integer
        result = makeReal(fabs(arg.r_val));
    else {
        fprintf(stderr, "Runtime error: abs expects a numeric argument.\n");
        freeValue(&arg); // Free evaluated arg before exit
        EXIT_FAILURE_HANDLER();
    }

    // --- ADDED: Free the evaluated argument ---
    freeValue(&arg);

    return result;
}

Value executeBuiltinTrunc(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: trunc expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value arg = eval(node->children[0]);
    if (arg.type == TYPE_INTEGER) {
        return makeInt(arg.i_val);
    } else if (arg.type == TYPE_REAL) {
        return makeInt((int)arg.r_val);
    } else {
        fprintf(stderr, "Runtime error: trunc argument must be a numeric type.\n");
        EXIT_FAILURE_HANDLER();
    }
    return makeValueForType(TYPE_INTEGER, NULL);

}

// File I/O
Value executeBuiltinAssign(AST *node) {
    if (node->child_count != 2) { fprintf(stderr, "Runtime error: assign expects 2 arguments.\n"); EXIT_FAILURE_HANDLER(); }

    // Evaluate arguments
    Value fileVal = eval(node->children[0]);
    Value nameVal = eval(node->children[1]);

    // Type checks
    if (fileVal.type != TYPE_FILE) {
        fprintf(stderr, "Runtime error: first parameter to assign must be a file variable.\n");
        freeValue(&fileVal); // Free potentially allocated value
        freeValue(&nameVal); // Free potentially allocated value
        EXIT_FAILURE_HANDLER();
    }
    if (nameVal.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: second parameter to assign must be a string.\n");
        freeValue(&fileVal); // Free potentially allocated value
        freeValue(&nameVal); // Free potentially allocated value
        EXIT_FAILURE_HANDLER();
    }

    // Find symbol and assign filename
    // Ensure the LValue node (children[0]) is a variable
    if (node->children[0]->type != AST_VARIABLE || !node->children[0]->token) {
        fprintf(stderr, "Runtime error: file variable parameter to assign must be a simple variable.\n");
        freeValue(&fileVal);
        freeValue(&nameVal);
        EXIT_FAILURE_HANDLER();
    }
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName); // lookupSymbol handles not found error

    if (!sym || !sym->value || sym->value->type != TYPE_FILE) { // Check if symbol is actually a FILE type
        fprintf(stderr, "Runtime error: Symbol '%s' is not a file variable.\n", fileVarName);
        freeValue(&fileVal);
        freeValue(&nameVal);
        EXIT_FAILURE_HANDLER();
    }

    // Free old filename if exists and assign new one
    if (sym->value->filename) free(sym->value->filename);
    sym->value->filename = nameVal.s_val ? strdup(nameVal.s_val) : NULL;
    if (nameVal.s_val && !sym->value->filename) { // Check strdup success
         fprintf(stderr, "Memory allocation error assigning filename.\n");
         freeValue(&fileVal);
         freeValue(&nameVal);
         EXIT_FAILURE_HANDLER();
    }

    // --- ADDED: Free the evaluated values ---
    freeValue(&fileVal); // fileVal itself doesn't hold heap data for TYPE_FILE
    freeValue(&nameVal); // Free the string evaluated for the filename

    return makeVoid(); // Return void value
}


Value executeBuiltinClose(AST *node) { // Return Value
    if (node->child_count != 1) { /* ... error handling ... */ }
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) { /* ... error handling ... */ }
    if (!fileVal.f_val) { /* ... error handling ... */ }

    // Existing core logic
    fclose(fileVal.f_val);
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName);
    // Make robust: Check if sym and sym->value exist before dereferencing
    if (sym && sym->value && sym->value->filename) {
        free(sym->value->filename);
        sym->value->filename = NULL;
        // Also nullify the FILE* pointer in the symbol table
        sym->value->f_val = NULL;
    }
     // --- ADDED ---
     freeValue(&fileVal); // Free value returned by eval

    return makeVoid(); // Return void value for procedures
}

Value executeBuiltinReset(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: reset expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }

    // Evaluate argument
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) {
        fprintf(stderr, "Runtime error: reset parameter must be a file variable.\n");
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }

    // Find symbol
    if (node->children[0]->type != AST_VARIABLE || !node->children[0]->token) {
        fprintf(stderr, "Runtime error: file variable parameter to reset must be a simple variable.\n");
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName); // Handles not found
    if (!sym || !sym->value || sym->value->type != TYPE_FILE) { // Check if symbol is actually a FILE type
        fprintf(stderr, "Runtime error: Symbol '%s' is not a file variable.\n", fileVarName);
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }
    if (sym->value->filename == NULL) {
        fprintf(stderr, "Runtime error: file variable '%s' not assigned a filename before reset.\n", fileVarName);
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }

    // Close existing file handle if open
    if (sym->value->f_val) {
        fclose(sym->value->f_val);
        sym->value->f_val = NULL;
    }

    // Open file for reading
    FILE *f = fopen(sym->value->filename, "r");
    if (f == NULL) {
        last_io_error = errno ? errno : 1; // Store system error or generic error 1
        // Don't exit, allow IOResult check
    } else {
        sym->value->f_val = f; // Assign the new FILE handle
        last_io_error = 0;
    }

    // --- ADDED: Free the evaluated value ---
    freeValue(&fileVal); // fileVal itself doesn't hold heap data for TYPE_FILE

    return makeVoid(); // Return void value
}

Value executeBuiltinRewrite(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: rewrite expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }

    // Evaluate argument
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) {
        fprintf(stderr, "Runtime error: rewrite parameter must be a file variable.\n");
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }

    // Find symbol
    if (node->children[0]->type != AST_VARIABLE || !node->children[0]->token) {
        fprintf(stderr, "Runtime error: file variable parameter to rewrite must be a simple variable.\n");
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName); // Handles not found
    if (!sym || !sym->value || sym->value->type != TYPE_FILE) { // Check if symbol is actually a FILE type
        fprintf(stderr, "Runtime error: Symbol '%s' is not a file variable.\n", fileVarName);
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }
    if (sym->value->filename == NULL) {
        fprintf(stderr, "Runtime error: file variable '%s' not assigned a filename before rewrite.\n", fileVarName);
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }

    // Close existing file handle if open
    if (sym->value->f_val) {
        fclose(sym->value->f_val);
        sym->value->f_val = NULL;
    }

    // Open file for writing
    FILE *f = fopen(sym->value->filename, "w"); // Use "w" for rewrite
    if (!f) {
        last_io_error = errno ? errno : 1;
        fprintf(stderr, "Runtime error: could not open file '%s' for writing. IOResult=%d\n", sym->value->filename, last_io_error);
        // Don't exit, allow IOResult check (though Rewrite usually aborts on error)
        // EXIT_FAILURE_HANDLER(); // Or enable this for stricter Turbo Pascal compatibility
    } else {
        sym->value->f_val = f; // Assign the new FILE handle
        last_io_error = 0;
    }

    // --- ADDED: Free the evaluated value ---
    freeValue(&fileVal); // fileVal itself doesn't hold heap data for TYPE_FILE

    return makeVoid(); // Return void value
}


Value executeBuiltinEOF(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: eof expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) {
        fprintf(stderr, "Runtime error: eof argument must be a file variable.\n");
        EXIT_FAILURE_HANDLER();
    }
    if (fileVal.f_val == NULL) {
        fprintf(stderr, "Runtime error: file is not open.\n");
        EXIT_FAILURE_HANDLER();
    }
    int is_eof = feof(fileVal.f_val);
    return makeInt(is_eof);
}

Value executeBuiltinIOResult(AST *node) {
    if (node->child_count != 0) { fprintf(stderr, "Runtime error: IOResult expects no arguments.\n"); EXIT_FAILURE_HANDLER(); }
    int err = last_io_error;
    last_io_error = 0;
    return makeInt(err);
}

Value executeBuiltinLength(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: length expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value arg = eval(node->children[0]);
    if (arg.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: length argument must be a string. Got %s\n", varTypeToString(arg.type));
        freeValue(&arg); // Free evaluated arg before exit
        EXIT_FAILURE_HANDLER();
    }
    // Handle potential NULL string defensively
    int len = (arg.s_val) ? (int)strlen(arg.s_val) : 0;
    Value result = makeInt(len);

    // --- ADDED: Free the evaluated argument ---
    freeValue(&arg); // Frees the string content

    return result;
}

// Strings
Value executeBuiltinCopy(AST *node) {
    if (node->child_count != 3) { fprintf(stderr, "Runtime error: copy expects 3 arguments.\n"); EXIT_FAILURE_HANDLER(); }
    Value sourceVal = eval(node->children[0]);
    Value startVal  = eval(node->children[1]);
    Value countVal  = eval(node->children[2]);
    if (sourceVal.type != TYPE_STRING || startVal.type != TYPE_INTEGER || countVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: copy requires a string, an integer, and an integer.\n");
        EXIT_FAILURE_HANDLER();
    }
    long long start = startVal.i_val;
    long long count = countVal.i_val;
    if (start < 1 || count < 0) { fprintf(stderr, "Runtime error: copy: invalid start index or count.\n"); EXIT_FAILURE_HANDLER(); }
    const char *src = sourceVal.s_val;
    int src_len = (int)strlen(src);
    if (start > src_len)
        return makeString("");
    if (start - 1 + count > src_len)
        count = src_len - (start - 1);
    char *substr = malloc(count + 1);
    if (!substr) { fprintf(stderr, "Memory allocation error in copy().\n"); EXIT_FAILURE_HANDLER(); }
    strncpy(substr, src + start - 1, count);
    substr[count] = '\0';
    Value retVal = makeString(substr);
    free(substr);
    return retVal;
}

Value executeBuiltinPos(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: pos expects 2 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value substr = eval(node->children[0]);
    Value s = eval(node->children[1]);

    if (s.type != TYPE_STRING || s.s_val == NULL) {
        fprintf(stderr, "Runtime error: pos second argument must be a valid string.\n");
        EXIT_FAILURE_HANDLER();
    }

    const char *needle = NULL;
    char single_char_buf[2];

    if (substr.type == TYPE_CHAR) {
        // Wrap single char into temporary string for strstr
        single_char_buf[0] = substr.c_val;
        single_char_buf[1] = '\0';
        needle = single_char_buf;
    } else if (substr.type == TYPE_STRING) {
        if (substr.s_val == NULL) {
            fprintf(stderr, "Runtime error: pos first argument is a null string.\n");
            EXIT_FAILURE_HANDLER();
        }
        needle = substr.s_val;
    } else {
        fprintf(stderr, "Runtime error: pos first argument must be a CHAR or STRING.\n");
        EXIT_FAILURE_HANDLER();
    }

    const char *found = strstr(s.s_val, needle);
    if (!found) {
        return makeInt(0);
    } else {
        return makeInt((int)(found - s.s_val) + 1);
    }
}


Value executeBuiltinUpcase(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: upcase expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value arg = eval(node->children[0]);

    char ch = '\0'; // Prevent uninitialized warning

    if (arg.type == TYPE_CHAR) {
        ch = (char)arg.c_val;
    } else if (arg.type == TYPE_STRING) {
        if (!arg.s_val || strlen(arg.s_val) != 1) {
            fprintf(stderr, "Runtime error: upcase expects a single-character string.\n");
            EXIT_FAILURE_HANDLER();
        }
        ch = arg.s_val[0];
    } else {
        fprintf(stderr, "Runtime error: upcase expects a CHAR or STRING argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    char up = toupper((unsigned char)ch);
    return makeChar(up);
}

#include <termios.h> // Make sure these headers are included at the top of builtin.c
#include <unistd.h>
#include <stdio.h>

Value executeBuiltinReadKey(AST *node) {
    // --- Add necessary declarations ---
    struct termios oldt, newt;
    char ch_read;        // Buffer for the character read
    ssize_t bytes_read;  // To check read() return value
    // --- End declarations ---

    // --- Diagnostic: Check if stdin is a terminal ---
    if (!isatty(STDIN_FILENO)) {
        fprintf(stderr, "ReadKey Error: Standard input is not a terminal.\n");
        return makeString(""); // Return empty string on error
    }

    // --- Get current terminal settings ---
    if (tcgetattr(STDIN_FILENO, &oldt) < 0) {
         perror("ReadKey Error: tcgetattr failed");
         return makeString("");
    }
    newt = oldt;

    // --- Modify terminal settings ---
    newt.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode, echo
    newt.c_cc[VMIN] = 1;              // Wait for 1 char
    newt.c_cc[VTIME] = 0;             // No timeout

    // --- Apply new settings ---
    // Use TCSANOW to apply changes immediately
    if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0) {
        perror("ReadKey Error: tcsetattr (set raw) failed");
        // Attempt to restore original settings *before* returning
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Best effort restore
        return makeString("");
    }

    // --- Flush the input and output buffer (TCIOFLUSH attempt) ---
 //   if (tcflush(STDIN_FILENO, TCOFLUSH) < 0) {
  //      perror("ReadKey Warning: tcflush(TCOFLUSH) failed");
 //   }
    tcdrain(STDOUT_FILENO);

    // --- Read a single character using read() ---
    errno = 0; // Clear errno before read
    bytes_read = read(STDIN_FILENO, &ch_read, 1); // Read 1 byte

    // --- Restore original terminal settings ---
    // It's crucial to restore settings *regardless* of read success/failure
    if (tcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0) {
        perror("ReadKey CRITICAL ERROR: tcsetattr (restore) failed");
        // Terminal state is likely messed up now. Might need to abort.
        // EXIT_FAILURE_HANDLER(); // Consider uncommenting if this happens
    }

    // --- Handle read result ---
    if (bytes_read < 0) {
        perror("ReadKey Error: read failed");
        return makeString("");
    } else if (bytes_read == 0) {
        // Should not happen with VMIN=1, VTIME=0 unless EOF was reached *before* read
        fprintf(stderr, "Warning: ReadKey read 0 bytes (EOF?).\n");
        return makeString("");
    } else {
        // Success: bytes_read should be 1
        char buf[2];
        buf[0] = ch_read;
        buf[1] = '\0';
        // Return as single-char string (or makeChar if preferred)
        return makeString(buf);
    }
}

// ord() implementation
Value executeBuiltinOrd(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: ord expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value arg = eval(node->children[0]); // Evaluate the argument

    // Handle TYPE_CHAR correctly
    if (arg.type == TYPE_CHAR) {
        // Ord(Char) returns the integer ordinal value (ASCII)
        return makeInt((int)arg.c_val);
    }
    // Handle single-character TYPE_STRING (add NULL check)
    else if (arg.type == TYPE_STRING && arg.s_val != NULL && strlen(arg.s_val) == 1) {
        return makeInt((int)arg.s_val[0]);
    }
    // Handle TYPE_ENUM
    else if (arg.type == TYPE_ENUM) {
        // Ord(Enum) returns the integer ordinal value
        return makeInt((int)arg.enum_val.ordinal);
    }
    // Handle TYPE_BOOLEAN (Ord(False)=0, Ord(True)=1) - Stored in i_val
    else if (arg.type == TYPE_BOOLEAN) {
        return makeInt(arg.i_val); // i_val is 0 or 1 for boolean
    }
    // Handle TYPE_INTEGER (Ord(Integer) returns the integer itself)
    else if (arg.type == TYPE_INTEGER) {
        return makeInt(arg.i_val); // Ordinal of an integer is itself
    }
    // Handle other ordinal types if you add them (e.g., Byte, Word)
    // else if (arg.type == TYPE_BYTE) { ... }

    else {
        // Argument is not an ordinal type
        fprintf(stderr, "Runtime error: ord expects an ordinal type argument (Char, Boolean, Enum, Integer, etc.). Got %s.\n",
                varTypeToString(arg.type));
        EXIT_FAILURE_HANDLER();
    }
    
    return(makeInt(0));
    // Should be unreachable
}

// chr() implementation
Value executeBuiltinChr(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: chr expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value arg = eval(node->children[0]);
    
    if (arg.type == TYPE_INTEGER) {
        // Create single-character string
        char buf[2] = {(char)arg.i_val, '\0'};
        return makeString(buf);
    } else {
        fprintf(stderr, "Runtime error: chr expects an integer argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    return makeValueForType(TYPE_INTEGER, NULL);
}

// System
Value executeBuiltinHalt(AST *node) {
    long long code = 0;
    Value arg; // Declare outside conditional block
    arg.type = TYPE_VOID; // Initialize type

    // Optionally allow one argument (the exit code)
    if (node->child_count == 1) {
        arg = eval(node->children[0]);
        if (arg.type != TYPE_INTEGER) {
            fprintf(stderr, "Runtime error: halt expects an integer argument.\n");
            freeValue(&arg); // Free if eval allocated something
            EXIT_FAILURE_HANDLER();
        }
        code = arg.i_val;
        freeValue(&arg); // Free the evaluated integer value
    } else if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: halt expects 0 or 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    // --- Cleanup before exit ---
    // Consider freeing global resources if necessary, e.g., symbol tables, type table AST nodes.
    // freeProcedureTable(); // Example
    // freeTypeTableASTNodes(); // Example
    // freeTypeTable(); // Example
    // You might want a dedicated cleanup function.

    exit((int)code); // Exit the program

    // This line is technically unreachable due to exit()
    return makeVoid();
}

Value executeBuiltinIntToStr(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: IntToStr expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value arg = eval(node->children[0]); // Evaluate the argument

    if (arg.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: IntToStr expects an integer argument. Got %s.\n",
                varTypeToString(arg.type));
        EXIT_FAILURE_HANDLER();
    }

    // --- MODIFICATION START: Dynamic Allocation ---
    // 1. Determine required size using snprintf with size 0
    //    (Returns number of chars needed *excluding* null terminator)
    int required_size = snprintf(NULL, 0, "%lld", arg.i_val);
    if (required_size < 0) {
         fprintf(stderr, "Runtime error: snprintf failed to determine size in IntToStr.\n");
         return makeString(""); // Return empty string on error
    }

    // 2. Allocate buffer on the heap (+1 for null terminator)
    char *buffer = malloc(required_size + 1);
    if (!buffer) {
         fprintf(stderr, "Runtime error: Memory allocation failed for buffer in IntToStr.\n");
         // No EXIT_FAILURE_HANDLER needed here, just return empty string? Or maybe exit is safer.
         // EXIT_FAILURE_HANDLER();
         return makeString(""); // Let's return empty for now
    }

    // 3. Perform the actual formatting into the allocated buffer
    //    Pass the allocated size (required_size + 1) to snprintf.
    int chars_written = snprintf(buffer, required_size + 1, "%lld", arg.i_val);

    if (chars_written < 0 || chars_written >= (required_size + 1)) {
        // Handle potential snprintf error (shouldn't happen if size calculation was correct)
        fprintf(stderr, "Runtime error: Failed to convert integer to string in IntToStr (step 2).\n");
        free(buffer); // Free the allocated buffer before returning
        return makeString(""); // Return empty string on error
    }

    // 4. Create the Value using makeString (which copies the buffer content to new heap memory)
    Value result = makeString(buffer);

    // 5. Free the dynamically allocated buffer used for formatting
    free(buffer);
    // --- MODIFICATION END ---

    return result; // Return the string Value (contains its own heap copy)
}

Value executeBuiltinInc(AST *node) {
    if (node->child_count < 1 || node->child_count > 2) {
        fprintf(stderr, "Runtime error: Inc expects 1 or 2 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    AST *lvalueNode = node->children[0];
    // Check if lvalueNode is assignable
    if (lvalueNode->type != AST_VARIABLE && lvalueNode->type != AST_FIELD_ACCESS && lvalueNode->type != AST_ARRAY_ACCESS) {
         fprintf(stderr, "Runtime error: First argument to Inc must be a variable, field, or array element.\n");
         EXIT_FAILURE_HANDLER();
    }

    Value currentVal = eval(lvalueNode); // Get current value
    long long current_iVal = -1;
    VarType originalType = currentVal.type;

    // Determine current ordinal value (logic remains the same)
    if (originalType == TYPE_INTEGER || originalType == TYPE_BOOLEAN || originalType == TYPE_BYTE || originalType == TYPE_WORD) current_iVal = currentVal.i_val;
    else if (originalType == TYPE_CHAR) current_iVal = currentVal.c_val;
    else if (originalType == TYPE_ENUM) current_iVal = currentVal.enum_val.ordinal;
    else {
        fprintf(stderr, "Runtime error: inc can only operate on ordinal types. Got %s\n", varTypeToString(originalType));
        freeValue(&currentVal); // Free the evaluated value
        EXIT_FAILURE_HANDLER();
    }

    long long increment = 1;
    Value incrVal; // Declare outside conditional
    incrVal.type = TYPE_VOID; // Initialize

    if (node->child_count == 2) {
        incrVal = eval(node->children[1]);
        if (incrVal.type != TYPE_INTEGER) {
            fprintf(stderr, "Runtime error: Inc step amount (second argument) must be an integer. Got %s\n", varTypeToString(incrVal.type));
            freeValue(&currentVal);
            freeValue(&incrVal); // Free the evaluated step value
            EXIT_FAILURE_HANDLER();
        }
        increment = incrVal.i_val;
        freeValue(&incrVal); // Free the evaluated step value after use
    }

    long long new_iVal = current_iVal + increment;
    Value newValue = makeInt(0); // Placeholder initialization

    // Create the correct type of value for the result (switch logic remains the same)
    switch (originalType) {
        case TYPE_INTEGER: newValue = makeInt(new_iVal); break;
        case TYPE_BOOLEAN:
             if (new_iVal > 1 || new_iVal < 0) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
             newValue = makeBoolean((int)new_iVal); break;
        case TYPE_CHAR:
             if (new_iVal > 255 || new_iVal < 0) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
             newValue = makeChar((char)new_iVal); break;
        case TYPE_BYTE:
             if (new_iVal > 255 || new_iVal < 0) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
             newValue = makeInt(new_iVal); newValue.type = TYPE_BYTE; break;
        case TYPE_WORD:
             if (new_iVal > 65535 || new_iVal < 0) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
             newValue = makeInt(new_iVal); newValue.type = TYPE_WORD; break;
        case TYPE_ENUM:
             { // Block scope needed
                 AST* typeDef = currentVal.enum_val.enum_name ? lookupType(currentVal.enum_val.enum_name) : NULL; // Lookup type def
                 if (typeDef && typeDef->type == AST_TYPE_REFERENCE) typeDef = typeDef->right; // Resolve reference
                 long long maxOrdinal = -1;
                 if (typeDef && typeDef->type == AST_ENUM_TYPE) { maxOrdinal = typeDef->child_count - 1; }
                 else { fprintf(stderr, "Warning: Could not find enum definition for '%s' during Inc.\n", currentVal.enum_val.enum_name ? currentVal.enum_val.enum_name : "?");}

                 if (maxOrdinal != -1 && new_iVal > maxOrdinal) { /* Overflow error */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
                 if (new_iVal < 0) { /* Underflow error */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
                 newValue = makeEnum(currentVal.enum_val.enum_name, (int)new_iVal); // makeEnum strdups the name
             }
             break;
        default: freeValue(&currentVal); EXIT_FAILURE_HANDLER(); break;
    }

    // Assign the new value back using the helper
    assignValueToLValue(lvalueNode, newValue);

    // --- ADDED: Free temporary values ---
    freeValue(&currentVal); // Free the value obtained from the initial eval
    freeValue(&newValue);   // Free the temporary newValue (assignValueToLValue made its own copy)

    return makeVoid();
}

Value executeBuiltinDec(AST *node) {
    if (node->child_count < 1 || node->child_count > 2) {
        fprintf(stderr, "Runtime error: Dec expects 1 or 2 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    AST *lvalueNode = node->children[0];
    // Check if lvalueNode is assignable
    if (lvalueNode->type != AST_VARIABLE && lvalueNode->type != AST_FIELD_ACCESS && lvalueNode->type != AST_ARRAY_ACCESS) {
         fprintf(stderr, "Runtime error: First argument to Dec must be a variable, field, or array element.\n");
         EXIT_FAILURE_HANDLER();
    }

    Value currentVal = eval(lvalueNode); // Get current value
    long long current_iVal = -1;
    VarType originalType = currentVal.type;

    // Determine current ordinal value (logic remains the same)
    if (originalType == TYPE_INTEGER || originalType == TYPE_BOOLEAN || originalType == TYPE_BYTE || originalType == TYPE_WORD) current_iVal = currentVal.i_val;
    else if (originalType == TYPE_CHAR) current_iVal = currentVal.c_val;
    else if (originalType == TYPE_ENUM) current_iVal = currentVal.enum_val.ordinal;
    else {
        fprintf(stderr, "Runtime error: dec can only operate on ordinal types. Got %s\n", varTypeToString(originalType));
        freeValue(&currentVal); // Free the evaluated value
        EXIT_FAILURE_HANDLER();
    }

    long long decrement = 1;
    Value decrVal; // Declare outside conditional
    decrVal.type = TYPE_VOID; // Initialize

    if (node->child_count == 2) {
        decrVal = eval(node->children[1]);
        if (decrVal.type != TYPE_INTEGER) {
            fprintf(stderr, "Runtime error: Dec step amount (second argument) must be an integer. Got %s\n", varTypeToString(decrVal.type));
            freeValue(&currentVal);
            freeValue(&decrVal); // Free the evaluated step value
            EXIT_FAILURE_HANDLER();
        }
        decrement = decrVal.i_val;
        freeValue(&decrVal); // Free the evaluated step value after use
    }

    long long new_iVal = current_iVal - decrement;
    Value newValue = makeInt(0); // Placeholder initialization

    // Create the correct type of value for the result (switch logic remains the same)
    switch (originalType) {
         case TYPE_INTEGER: newValue = makeInt(new_iVal); break;
         case TYPE_BOOLEAN:
             if (new_iVal < 0 || new_iVal > 1) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
             newValue = makeBoolean((int)new_iVal); break;
         case TYPE_CHAR:
             if (new_iVal < 0 || new_iVal > 255) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
             newValue = makeChar((char)new_iVal); break;
         case TYPE_BYTE:
              if (new_iVal < 0 || new_iVal > 255) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
              newValue = makeInt(new_iVal); newValue.type = TYPE_BYTE; break;
         case TYPE_WORD:
              if (new_iVal < 0 || new_iVal > 65535) { /* Error handling */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
              newValue = makeInt(new_iVal); newValue.type = TYPE_WORD; break;
         case TYPE_ENUM:
             { // Block scope needed
                  // No upper bound check needed for standard Dec(X, 1)
                 if (new_iVal < 0) { /* Underflow error */ freeValue(&currentVal); EXIT_FAILURE_HANDLER(); }
                 newValue = makeEnum(currentVal.enum_val.enum_name, (int)new_iVal); // makeEnum strdups the name
             }
             break;
         default: freeValue(&currentVal); EXIT_FAILURE_HANDLER(); break;
    }

    // Assign the new value back using the helper
    assignValueToLValue(lvalueNode, newValue);

    // --- ADDED: Free temporary values ---
    freeValue(&currentVal); // Free the value obtained from the initial eval
    freeValue(&newValue);   // Free the temporary newValue (assignValueToLValue made its own copy)

    return makeVoid();
}

Value executeBuiltinScreenCols(AST *node) {
    // ... arg check ...
    int rows = 0, cols = 0; // Initialize
    int result = getTerminalSize(&rows, &cols);
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_SIZE] getTerminalSize returned %d. rows=%d, cols=%d\n", result, rows, cols); // DEBUG
#endif
    if (result == 0) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_SIZE] Returning ScreenCols: %d\n", cols); // DEBUG
#endif
        return makeInt(cols);
    } else {
#ifdef DEBUG
        fprintf(stderr, "Warning: Using default screen width (80) due to error.\n");
        fprintf(stderr, "[DEBUG_SIZE] Returning ScreenCols (default): 80\n"); // DEBUG
#endif
        return makeInt(80);
    }
}

Value executeBuiltinScreenRows(AST *node) {
    // ... arg check ...
    int rows = 0, cols = 0; // Initialize
    int result = getTerminalSize(&rows, &cols);
#ifdef DEBUG
    fprintf(stderr, "[DEBUG_SIZE] getTerminalSize returned %d. rows=%d, cols=%d\n", result, rows, cols); // DEBUG
#endif
    if (result == 0) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG_SIZE] Returning ScreenRows: %d\n", rows); // DEBUG
#endif
        return makeInt(rows);
    } else {
#ifdef DEBUG
        fprintf(stderr, "Warning: Using default screen height (24) due to error.\n");
        fprintf(stderr, "[DEBUG_SIZE] Returning ScreenRows (default): 24\n"); // DEBUG
#endif
        return makeInt(24);
    }
}

Value executeBuiltinRandomize(AST *node) {
    if (node->child_count != 0) { fprintf(stderr, "Runtime error: Randomize expects no arguments.\n"); EXIT_FAILURE_HANDLER(); }
    srand((unsigned int)time(NULL));
    return makeVoid(); // Return void value
}

Value executeBuiltinRandom(AST *node) {
    if (node->child_count == 0) {
        double r = (double)rand() / ((double)RAND_MAX + 1.0);
        return makeReal(r);
    } else if (node->child_count == 1) {
        Value arg = eval(node->children[0]);
        if (arg.type == TYPE_INTEGER) {
            long long n = arg.i_val;
            if (n <= 0) { fprintf(stderr, "Runtime error: Random argument must be > 0.\n"); EXIT_FAILURE_HANDLER(); }
            int r = rand() % n;
#ifdef DEBUG
            if(dumpExec) fprintf(stderr, "[DEBUG_RANDOM] Random(%lld) calculated r=%d\n", n, r);
#endif
            return makeInt(r);
        } else if (arg.type == TYPE_REAL) {
            double n = arg.r_val;
            if (n <= 0.0) { fprintf(stderr, "Runtime error: Random argument must be > 0.\n"); EXIT_FAILURE_HANDLER(); }
            double r = (double)rand() / ((double)RAND_MAX + 1.0);
            return makeReal(n * r);
        } else {
            fprintf(stderr, "Runtime error: Random argument must be integer or real.\n");
            EXIT_FAILURE_HANDLER();
        }
    } else {
        fprintf(stderr, "Runtime error: Random expects 0 or 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    return makeValueForType(TYPE_INTEGER, NULL);
}

Value executeBuiltinDelay(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Delay expects 1 argument (milliseconds).\n");
        EXIT_FAILURE_HANDLER();
    }

    Value msVal = eval(node->children[0]);
    if (msVal.type != TYPE_INTEGER && msVal.type != TYPE_WORD) {
         fprintf(stderr, "Runtime error: Delay argument must be an integer or word type. Got %s\n", varTypeToString(msVal.type));
         freeValue(&msVal); // Free evaluated value
         EXIT_FAILURE_HANDLER();
    }

    long long ms = msVal.i_val;
    if (ms < 0) ms = 0; // Treat negative delay as 0

    useconds_t usec = (useconds_t)ms * 1000;
    usleep(usec);

    // --- ADDED: Free the evaluated value ---
    freeValue(&msVal);

    return makeVoid(); // Return void value
}

// Memory Streams
Value executeBuiltinMstreamCreate(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: TMemoryStream.Create expects no arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    MStream *ms = malloc(sizeof(MStream));
    if (!ms) {
        fprintf(stderr, "Memory allocation error in TMemoryStream.Create.\n");
        EXIT_FAILURE_HANDLER();
    }
    ms->buffer = NULL;
    ms->size = 0;
    ms->capacity = 0;
    return makeMStream(ms);
}

Value executeBuiltinMstreamLoadFromFile(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: TMemoryStream.LoadFromFile expects 2 arguments (a memory stream and a filename).\n");
        EXIT_FAILURE_HANDLER();
    }
    // First argument must be a memory stream
    Value msVal = eval(node->children[0]);
    if (msVal.type != TYPE_MEMORYSTREAM) {
        fprintf(stderr, "Runtime error: first parameter of LoadFromFile must be a TMemoryStream.\n");
        EXIT_FAILURE_HANDLER();
    }
    // Second argument must be a string (the filename)
    Value fileNameVal = eval(node->children[1]);
    if (fileNameVal.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: second parameter of LoadFromFile must be a string.\n");
        EXIT_FAILURE_HANDLER();
    }
    FILE *f = fopen(fileNameVal.s_val, "rb");
    if (!f) {
        fprintf(stderr, "Runtime error: cannot open file '%s' for reading.\n", fileNameVal.s_val);
        EXIT_FAILURE_HANDLER();
    }
    // Determine file size
    fseek(f, 0, SEEK_END);
    int size = (int)ftell(f);
    rewind(f);
    char *buffer = malloc(size);
    if (!buffer) {
        fclose(f);
        fprintf(stderr, "Memory allocation error in LoadFromFile.\n");
        EXIT_FAILURE_HANDLER();
    }
    fread(buffer, 1, size, f);
    fclose(f);
    msVal.mstream->buffer = (unsigned char *)buffer;
    msVal.mstream->size = size;
    return makeMStream(msVal.mstream);
}

Value executeBuiltinMstreamSaveToFile(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: TMemoryStream.SaveToFile expects 2 arguments (a memory stream and a filename).\n");
        EXIT_FAILURE_HANDLER();
    }
    Value msVal = eval(node->children[0]);
    if (msVal.type != TYPE_MEMORYSTREAM) {
        fprintf(stderr, "Runtime error: first parameter of SaveToFile must be a Ttype MStream.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value fileNameVal = eval(node->children[1]);
    if (fileNameVal.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: second parameter of SaveToFile must be a string.\n");
        EXIT_FAILURE_HANDLER();
    }
    FILE *f = fopen(fileNameVal.s_val, "wb");
    if (!f) {
        fprintf(stderr, "Runtime error: cannot open file '%s' for writing.\n", fileNameVal.s_val);
        EXIT_FAILURE_HANDLER();
    }
    fwrite(msVal.mstream->buffer, 1, msVal.mstream->size, f);
    fclose(f);
    return makeMStream(msVal.mstream);
}

Value executeBuiltinMstreamFree(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: TMemoryStream.Free expects 1 argument (a memory stream).\n");
        EXIT_FAILURE_HANDLER();
    }

    // --- Evaluate and Type Check ---
    Value msVal = eval(node->children[0]);
    if (msVal.type != TYPE_MEMORYSTREAM) {
        fprintf(stderr, "Runtime error: parameter of MStreamFree must be a Type MStream.\n");
        freeValue(&msVal); // Free potentially allocated value
        EXIT_FAILURE_HANDLER();
    }

    // --- Find Symbol and Update ---
    // We need to NULL the pointer in the symbol table after freeing
    if (node->children[0]->type != AST_VARIABLE || !node->children[0]->token) {
        fprintf(stderr, "Runtime error: Memory stream parameter to Free must be a simple variable.\n");
        freeValue(&msVal);
        EXIT_FAILURE_HANDLER();
    }
    const char *msVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(msVarName); // Handles not found
    if (!sym || !sym->value || sym->value->type != TYPE_MEMORYSTREAM) { // Check if symbol is actually a MSTREAM
        fprintf(stderr, "Runtime error: Symbol '%s' is not a memory stream variable.\n", msVarName);
        freeValue(&msVal);
        EXIT_FAILURE_HANDLER();
    }

    // --- Free Memory Stream Contents ---
    // Ensure we are freeing the stream pointed to by the *symbol*,
    // as msVal might be a temporary copy (though less likely for MStream).
    if (sym->value->mstream) {
        if (sym->value->mstream->buffer) {
            free(sym->value->mstream->buffer);
            sym->value->mstream->buffer = NULL;
        }
        free(sym->value->mstream);
        sym->value->mstream = NULL; // Set symbol's pointer to NULL
    }

    // --- ADDED: Free the evaluated value ---
    // msVal itself doesn't hold heap data here, the MStream* was shallow copied.
    // Do NOT call freeValue(&msVal) if msVal.mstream points to the same memory
    // as sym->value->mstream which we just freed. Setting sym->value->mstream = NULL
    // prevents double-free if msVal was somehow a copy.

    return makeVoid(); // Return void value
}

// Special
Value executeBuiltinResult(AST *node) {
    if (node->child_count != 0) { fprintf(stderr, "Runtime error: result expects no arguments.\n"); EXIT_FAILURE_HANDLER(); }
    if (current_function_symbol == NULL) { fprintf(stderr, "Runtime error: result called outside a function.\n"); EXIT_FAILURE_HANDLER(); }
    return *(current_function_symbol->value);
}

Value executeBuiltinProcedure(AST *node) {
    if (!node || !node->token || !node->token->value) {
        fprintf(stderr, "Internal Error: Invalid AST node passed to executeBuiltinProcedure.\n");
        EXIT_FAILURE_HANDLER();
    }

    const char *original_name = node->token->value;

    // Use a temporary buffer for lowercase conversion if needed,
    // or ensure lookup uses case-insensitive compare.
    // We use strcasecmp in the comparison function, so no need to lowercase here.

#ifdef DEBUG
    fprintf(stderr, "[DEBUG DISPATCH] Looking up built-in: '%s'\n", original_name);
#endif

    // Use bsearch to find the handler
    BuiltinMapping *found = (BuiltinMapping *)bsearch(
        original_name,                      // Key to search for
        builtin_dispatch_table,             // Array to search in
        num_builtins,                       // Number of elements in the array
        sizeof(BuiltinMapping),             // Size of each element
        compareBuiltinMappings              // Comparison function
    );

    if (found) {
#ifdef DEBUG
        fprintf(stderr, "[DEBUG DISPATCH] Found handler for '%s'. Calling function at %p.\n", original_name, (void*)found->handler);
#endif
        // Call the found handler function
        return found->handler(node);
    } else {
        // This should ideally not happen if isBuiltin() was checked beforehand,
        // but handle it defensively.
        fprintf(stderr, "Runtime error: Built-in procedure/function '%s' not found in dispatch table (but isBuiltin returned true?).\n", original_name);
        // Maybe check Write/Writeln/Read/Readln here if they aren't in the table?
        // For now, treat as an internal inconsistency.
        EXIT_FAILURE_HANDLER();
        // return makeVoid(); // Or return void if exiting is too harsh
    }
    return(makeInt(0));
}

void registerBuiltinFunction(const char *name, ASTNodeType declType) {
    char *lowerName = strdup(name);
    if (!lowerName) {
        fprintf(stderr, "Memory allocation error in register_builtin_function\n");
        EXIT_FAILURE_HANDLER();
    }
    // Convert the duplicated name to lowercase for consistent storage/lookup
    for (int i = 0; lowerName[i] != '\0'; i++) {
        lowerName[i] = tolower((unsigned char)lowerName[i]);
    }

    // Create the initial token for the function name.
    // This token is TEMPORARY, only used to pass the name to newASTNode.
    Token *funcNameToken = newToken(TOKEN_IDENTIFIER, lowerName);
    if (!funcNameToken) { // Check if newToken failed
        fprintf(stderr, "Memory allocation error creating token in registerBuiltinFunction\n");
        free(lowerName);
        EXIT_FAILURE_HANDLER();
    }

    // Create the main dummy AST node for the built-in routine.
    // newASTNode makes its *own internal copy* of funcNameToken (struct and value).
    AST *dummy = newASTNode(declType, funcNameToken);
    if (!dummy) { // Check if newASTNode failed
         fprintf(stderr, "Memory allocation error creating AST node in registerBuiltinFunction\n");
         freeToken(funcNameToken); // Free the original temporary token
         free(lowerName);
         EXIT_FAILURE_HANDLER();
    }

    // Free the ORIGINAL temporary function name token.
    // Its content has been copied into dummy->token by newASTNode.
    freeToken(funcNameToken);
    // The lowerName string itself can now be freed as funcNameToken owned its copy.
    free(lowerName); // Free the lowercased string

    // Initialize basic AST node fields
    dummy->child_count = 0;
    dummy->child_capacity = 0;
    setLeft(dummy, NULL);
    setRight(dummy, NULL);
    setExtra(dummy, NULL);

    // --- Logic to set return types and parameter info based on function name ---
    // For nodes created below (retTypeNode, param, var), newASTNode makes
    // its own copy of the temporary tokens used (typeNameToken, paramNameToken).
    // We free the temporary token *immediately after* newASTNode uses it.
    // We DO NOT free the token *inside* the resulting AST node (e.g., retTypeNode->token).

    // -- Functions with Return Types --
    if (strcmp(name, "api_send") == 0) { // Use original name 'name' for clarity in conditions
        Token* typeNameToken = newToken(TOKEN_IDENTIFIER, "mstream");
        AST *retTypeNode = newASTNode(AST_VARIABLE, typeNameToken);
        freeToken(typeNameToken); // Free temp token
        setTypeAST(retTypeNode, TYPE_MEMORYSTREAM);
        setRight(dummy, retTypeNode);
        dummy->var_type = TYPE_MEMORYSTREAM; // Set function's return type
    } else if (strcmp(name, "api_receive") == 0) {
        Token* typeNameToken = newToken(TOKEN_IDENTIFIER, "string");
        AST *retTypeNode = newASTNode(AST_VARIABLE, typeNameToken);
        freeToken(typeNameToken); // Free temp token
        setTypeAST(retTypeNode, TYPE_STRING);
        setRight(dummy, retTypeNode);
        dummy->var_type = TYPE_STRING;
    } else if (strcmp(name, "chr") == 0) {
        // Define parameter (integer)
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST *));
        if (!dummy->children) { /* Malloc error check */ EXIT_FAILURE_HANDLER(); }
        AST *param = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(param, TYPE_INTEGER);
        Token* paramNameToken = newToken(TOKEN_IDENTIFIER, "_chr_arg");
        AST *var = newASTNode(AST_VARIABLE, paramNameToken);
        freeToken(paramNameToken); // Free temp token
        addChild(param, var);
        dummy->children[0] = param;
        dummy->child_count = 1;
        // Define return type (char)
        Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "char");
        AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
        freeToken(retTypeNameToken); // Free temp token
        setTypeAST(retTypeNode, TYPE_CHAR);
        setRight(dummy, retTypeNode);
        dummy->var_type = TYPE_CHAR;
    } else if (strcmp(name, "ord") == 0) {
        // Define parameter (ordinal type, represented as CHAR here for simplicity)
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST *));
        if (!dummy->children) { /* Malloc error check */ EXIT_FAILURE_HANDLER(); }
        AST *param = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(param, TYPE_CHAR); // Example parameter type
        Token* paramNameToken = newToken(TOKEN_IDENTIFIER, "_ord_arg");
        AST *var = newASTNode(AST_VARIABLE, paramNameToken);
        freeToken(paramNameToken); // Free temp token
        addChild(param, var);
        dummy->children[0] = param;
        dummy->child_count = 1;
        // Define return type (integer)
        Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "integer");
        AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
        freeToken(retTypeNameToken); // Free temp token
        setTypeAST(retTypeNode, TYPE_INTEGER);
        setRight(dummy, retTypeNode);
        dummy->var_type = TYPE_INTEGER;
    } else if (strcmp(name, "wherex") == 0) {
        Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "integer");
        AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
        freeToken(retTypeNameToken); // Free temp token
        setTypeAST(retTypeNode, TYPE_INTEGER);
        setRight(dummy, retTypeNode);
        dummy->var_type = TYPE_INTEGER;
   } else if (strcmp(name, "wherey") == 0) {
        Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "integer");
        AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
        freeToken(retTypeNameToken); // Free temp token
        setTypeAST(retTypeNode, TYPE_INTEGER);
        setRight(dummy, retTypeNode);
        dummy->var_type = TYPE_INTEGER;
   } else if (strcasecmp(name, "keypressed") == 0) { // Use strcasecmp if needed
       Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "boolean");
       AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
       freeToken(retTypeNameToken); // Free temp token
       setTypeAST(retTypeNode, TYPE_BOOLEAN);
       setRight(dummy, retTypeNode);
       dummy->var_type = TYPE_BOOLEAN;
  } else if (strcmp(name, "inttostr") == 0) {
      Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "string");
      AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
      freeToken(retTypeNameToken); // Free temp token
      setTypeAST(retTypeNode, TYPE_STRING);
      setRight(dummy, retTypeNode);
      dummy->var_type = TYPE_STRING;
  } else if (strcasecmp(name, "screencols") == 0) {
      Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "integer");
      AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
      freeToken(retTypeNameToken); // Free temp token
      setTypeAST(retTypeNode, TYPE_INTEGER);
      setRight(dummy, retTypeNode);
      dummy->var_type = TYPE_INTEGER;
 } else if (strcasecmp(name, "screenrows") == 0) {
      Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "integer");
      AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
      freeToken(retTypeNameToken); // Free temp token
      setTypeAST(retTypeNode, TYPE_INTEGER);
      setRight(dummy, retTypeNode);
      dummy->var_type = TYPE_INTEGER;
 } else if (strcmp(name, "length") == 0) { // Example: length function
      // Define parameter (string)
      dummy->child_capacity = 1;
      dummy->children = malloc(sizeof(AST *));
      if (!dummy->children) { /* Malloc error check */ EXIT_FAILURE_HANDLER(); }
      AST *param = newASTNode(AST_VAR_DECL, NULL);
      setTypeAST(param, TYPE_STRING);
      Token* paramNameToken = newToken(TOKEN_IDENTIFIER, "_len_arg");
      AST *var = newASTNode(AST_VARIABLE, paramNameToken);
      freeToken(paramNameToken); // Free temp token
      addChild(param, var);
      dummy->children[0] = param;
      dummy->child_count = 1;
      // Define return type (integer)
      Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "integer");
      AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
      freeToken(retTypeNameToken); // Free temp token
      setTypeAST(retTypeNode, TYPE_INTEGER);
      setRight(dummy, retTypeNode);
      dummy->var_type = TYPE_INTEGER;
 } else if (strcmp(name, "copy") == 0) { // Example: copy function
      // Define parameters (string, integer, integer) - simplified, assumes order
      dummy->child_capacity = 3;
      dummy->children = malloc(sizeof(AST *) * 3);
      if (!dummy->children) { /* Malloc error check */ EXIT_FAILURE_HANDLER(); }
      // Param 1 (string)
      AST *param1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(param1, TYPE_STRING);
      Token* p1Name = newToken(TOKEN_IDENTIFIER, "_cpy_s"); AST *v1 = newASTNode(AST_VARIABLE, p1Name); freeToken(p1Name); addChild(param1, v1);
      dummy->children[0] = param1;
      // Param 2 (integer)
      AST *param2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(param2, TYPE_INTEGER);
      Token* p2Name = newToken(TOKEN_IDENTIFIER, "_cpy_idx"); AST *v2 = newASTNode(AST_VARIABLE, p2Name); freeToken(p2Name); addChild(param2, v2);
      dummy->children[1] = param2;
      // Param 3 (integer)
      AST *param3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(param3, TYPE_INTEGER);
      Token* p3Name = newToken(TOKEN_IDENTIFIER, "_cpy_cnt"); AST *v3 = newASTNode(AST_VARIABLE, p3Name); freeToken(p3Name); addChild(param3, v3);
      dummy->children[2] = param3;
      dummy->child_count = 3;
      // Define return type (string)
      Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "string");
      AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
      freeToken(retTypeNameToken); // Free temp token
      setTypeAST(retTypeNode, TYPE_STRING);
      setRight(dummy, retTypeNode);
      dummy->var_type = TYPE_STRING;
 }
    // --- Add similar blocks for ALL other built-in functions ---
    // --- that require specific return type or parameter setup. ---
    // --- Ensure the pattern is followed: ---
    // --- 1. Create temp token with newToken ---
    // --- 2. Create AST node with newASTNode using temp token ---
    // --- 3. Free temp token with freeToken ---
    // --- 4. DO NOT free token inside the created AST node ---

    // -- Procedures (no return type to set on 'dummy' itself, var_type remains VOID) --
    // Procedures might still have parameter definitions added to dummy->children if needed
    // for type checking later, but the basic registration doesn't require setting dummy->right.

    // else if (strcmp(name, "writeln") == 0) {
    //     // Writeln is variadic, parameter definition is complex/skipped here
    //     dummy->var_type = TYPE_VOID;
    // }
    // ... add parameter definitions for other procedures if strict checking is desired ...


    // Add the fully prepared dummy AST node to the procedure table
    addProcedure(dummy);

    // Lowercased name string `lowerName` was freed earlier after funcNameToken was created.
}

Value executeBuiltinParamcount(AST *node) {
    // No arguments expected.
    return makeInt(gParamCount);
}
        
Value executeBuiltinParamstr(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: ParamStr expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value indexVal = eval(node->children[0]);
    if (indexVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: ParamStr argument must be an integer.\n");
        EXIT_FAILURE_HANDLER();
    }
    long long idx = indexVal.i_val;
    if (idx < 1 || idx > gParamCount) {
        fprintf(stderr, "Runtime error: ParamStr index out of range.\n");
        EXIT_FAILURE_HANDLER();
    }
    return makeString(gParamValues[idx - 1]);
}

Value executeBuiltinWhereX(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: WhereX expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(c); // Return column
    } else {
        // Handle failure - perhaps return 1 or raise a specific error?
        fprintf(stderr, "Runtime warning: Failed to get cursor position for WhereX.\n");
        return makeInt(1); // Default to 1 on error
    }
}

Value executeBuiltinWhereY(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: WhereY expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(r); // Return row
    } else {
        // Handle failure
        fprintf(stderr, "Runtime warning: Failed to get cursor position for WhereY.\n");
        return makeInt(1); // Default to 1 on error
    }
}

Value executeBuiltinKeyPressed(AST *node) {
    if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: KeyPressed expects 0 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }

    struct termios oldt, newt;
    int bytes_available = 0;
    bool key_is_pressed = false;
    int stdin_fd = STDIN_FILENO;

    // --- Check if stdin is a terminal ---
    if (!isatty(stdin_fd)) {
         // If not a TTY, cannot check for keys in this way.
         // Standard Pascal KeyPressed might return true if EOF reached on redirected input.
         // We'll return false for simplicity here.
         return makeBoolean(false);
    }

    // --- Get current terminal settings ---
    if (tcgetattr(stdin_fd, &oldt) < 0) {
        perror("KeyPressed Error: tcgetattr failed");
        // Return false as we couldn't check
        return makeBoolean(false);
    }
    newt = oldt;

    // --- Set non-canonical, non-blocking mode ---
    // Disable canonical mode (line buffering) and echo
    newt.c_lflag &= ~(ICANON | ECHO);
    // Set VMIN=0, VTIME=0 for a truly non-blocking check
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;

    // --- Apply new settings ---
    if (tcsetattr(stdin_fd, TCSANOW, &newt) < 0) {
        perror("KeyPressed Error: tcsetattr (set non-blocking) failed");
        tcsetattr(stdin_fd, TCSANOW, &oldt); // Attempt restore
        return makeBoolean(false);
    }

    // --- Check for available bytes using ioctl(FIONREAD) ---
    if (ioctl(stdin_fd, FIONREAD, &bytes_available) < 0) {
         perror("KeyPressed Error: ioctl(FIONREAD) failed");
         key_is_pressed = false; // Assume no key if ioctl fails
    } else {
         key_is_pressed = (bytes_available > 0);
    }

    // --- CRITICAL: Restore original terminal settings ---
    // Use TCSANOW to restore immediately
    if (tcsetattr(stdin_fd, TCSANOW, &oldt) < 0) {
        perror("KeyPressed CRITICAL ERROR: tcsetattr (restore) failed");
        // Terminal state might be broken now!
        // EXIT_FAILURE_HANDLER(); // Consider exiting if restore fails
    }

    // --- Return result ---
    return makeBoolean(key_is_pressed);
}

int isBuiltin(const char *name) {
    if (!name) return 0;
    
    // Use bsearch to check if the name exists in the dispatch table
    BuiltinMapping *found = (BuiltinMapping *)bsearch(
                                                      name,                               // Key (function name)
                                                      builtin_dispatch_table,             // Table to search
                                                      num_builtins,                       // Number of elements
                                                      sizeof(BuiltinMapping),             // Size of elements
                                                      compareBuiltinMappings              // Comparison function
                                                      );
    
    // Additionally check for Write/Writeln/Read/Readln if they are handled
    // directly in the interpreter and not in the dispatch table.
    if (!found) {
        if (strcasecmp(name, "write") == 0 || strcasecmp(name, "writeln") == 0 ||
            strcasecmp(name, "read") == 0 || strcasecmp(name, "readln") == 0) {
            return 1; // Treat these as built-in even if not dispatched
        }
    }
    
    
    return (found != NULL); // Return 1 if found, 0 otherwise
}

// --- Low(X) ---
// Argument X: An expression evaluating to an ordinal type, OR more simply,
//             an AST_VARIABLE node whose type is ordinal. We use the latter.
Value executeBuiltinLow(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Low expects 1 argument (a type identifier).\n");
        EXIT_FAILURE_HANDLER();
    }

    AST *argNode = node->children[0];
    // Expecting the argument to be the identifier for the type
    if (argNode->type != AST_VARIABLE) { // Type names are parsed as identifiers
         fprintf(stderr, "Runtime error: Low argument must be a type identifier. Got AST type %s\n", astTypeToString(argNode->type));
         EXIT_FAILURE_HANDLER();
    }

    const char* typeName = argNode->token->value; // Get the type name (e.g., "tcolor")
    AST* typeDef = lookupType(typeName);          // <<< Use lookupType, not lookupSymbol

    if (!typeDef) {
        fprintf(stderr, "Runtime error: Type '%s' not found in Low().\n", typeName);
        EXIT_FAILURE_HANDLER();
    }

    // We have the type definition AST node (typeDef)
    // Note: typeDef might be an AST_TYPE_REFERENCE itself, but lookupType should ideally return
    // the ultimate definition node. If not, we might need to resolve it here.
    // Assuming lookupType returns the actual definition node (e.g., AST_ENUM_TYPE):

    VarType actualType = typeDef->var_type; // Get the VarType from the definition node

    switch (actualType) {
        case TYPE_INTEGER:
            return makeInt(0); // Or MIN_INT if defined
        case TYPE_CHAR:
            return makeChar((char)0);
        case TYPE_BOOLEAN:
            return makeBoolean(0); // False
        case TYPE_ENUM:
        {
            // Lowest ordinal is 0
            const char* enumTypeName = typeDef->token ? typeDef->token->value : typeName; // Use original name if possible
            Value lowEnum = makeEnum(enumTypeName, 0);
            return lowEnum;
        }
         case TYPE_BYTE:
             return makeInt(0); // Or makeByte(0)
         case TYPE_WORD:
             return makeInt(0); // Or makeWord(0)
        default:
            fprintf(stderr, "Runtime error: Low() not supported for type %s ('%s').\n", varTypeToString(actualType), typeName);
            EXIT_FAILURE_HANDLER();
    }
     return makeVoid(); // Should not be reached
}

// --- High(X) ---
// Argument X: An expression evaluating to an ordinal type, OR more simply,
//             an AST_VARIABLE node whose type is ordinal. We use the latter.
Value executeBuiltinHigh(AST *node) {
     if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: High expects 1 argument (a type identifier).\n");
        EXIT_FAILURE_HANDLER();
    }

    AST *argNode = node->children[0];
    if (argNode->type != AST_VARIABLE) {
         fprintf(stderr, "Runtime error: High argument must be a type identifier. Got AST type %s\n", astTypeToString(argNode->type));
         EXIT_FAILURE_HANDLER();
    }

    const char* typeName = argNode->token->value; // Get the type name (e.g., "tcolor")
    AST* typeDef = lookupType(typeName);          // <<< Use lookupType, not lookupSymbol

    if (!typeDef) {
        fprintf(stderr, "Runtime error: Type '%s' not found in High().\n", typeName);
        EXIT_FAILURE_HANDLER();
    }

    // We have the type definition AST node (typeDef)
    VarType actualType = typeDef->var_type;

    switch (actualType) {
        case TYPE_INTEGER:
            return makeInt(2147483647); // Or MAX_INT if defined
        case TYPE_CHAR:
            return makeChar((char)255);
        case TYPE_BOOLEAN:
            return makeBoolean(1); // True
        case TYPE_ENUM:
        {
            if (typeDef->type != AST_ENUM_TYPE) { // Ensure it's really an enum definition node
                fprintf(stderr, "Runtime error: Type definition for '%s' is not an Enum type for High().\n", typeName);
                EXIT_FAILURE_HANDLER();
            }
            // Highest ordinal is number of members - 1
            int highOrdinal = typeDef->child_count - 1;
            if (highOrdinal < 0) highOrdinal = 0; // Handle empty enum?
            
            const char* enumTypeName = typeDef->token ? typeDef->token->value : typeName;
            Value highEnum = makeEnum(enumTypeName, highOrdinal);
            return highEnum;
        }
         case TYPE_BYTE:
             return makeInt(255); // Or makeByte(255)
         case TYPE_WORD:
             return makeInt(65535); // Or makeWord(65535)
        default:
            fprintf(stderr, "Runtime error: High() not supported for type %s ('%s').\n", varTypeToString(actualType), typeName);
            EXIT_FAILURE_HANDLER();
    }
     return makeVoid(); // Should not be reached
}

// --- Succ(X) ---
// Argument X: An expression evaluating to an ordinal type.
Value executeBuiltinSucc(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Succ expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value argVal = eval(node->children[0]);
    long long currentOrdinal;
    long long maxOrdinal = -1; // Used for bounds check, -1 means no check needed

    switch (argVal.type) {
        case TYPE_INTEGER:
            currentOrdinal = argVal.i_val;
            // Potentially check against MAX_INT here
            return makeInt(currentOrdinal + 1);
        case TYPE_CHAR:
            currentOrdinal = argVal.c_val;
            maxOrdinal = 255; // Assuming 8-bit char
            if (currentOrdinal >= maxOrdinal) {
                 fprintf(stderr, "Runtime error: Succ argument out of range (Char overflow).\n");
                 EXIT_FAILURE_HANDLER();
            }
            return makeChar((char)(currentOrdinal + 1));
        case TYPE_BOOLEAN:
            currentOrdinal = argVal.i_val; // 0 or 1
            maxOrdinal = 1;
             if (currentOrdinal >= maxOrdinal) {
                 fprintf(stderr, "Runtime error: Succ argument out of range (Boolean overflow).\n");
                 EXIT_FAILURE_HANDLER();
            }
            return makeBoolean((int)(currentOrdinal + 1)); // Succ(False) = True
        case TYPE_ENUM:
            { // Need block scope for variables
                 currentOrdinal = argVal.enum_val.ordinal;
                 // Need to find the High value for this specific enum type
                 AST* typeDef = lookupType(argVal.enum_val.enum_name); // Find type def by name
                 if (!typeDef || typeDef->type != AST_ENUM_TYPE) {
                     fprintf(stderr, "Runtime error: Cannot determine enum definition for Succ() check on type '%s'.\n", argVal.enum_val.enum_name ? argVal.enum_val.enum_name : "?");
                     // Cannot perform bounds check, proceed with caution or error out
                     // EXIT_FAILURE_HANDLER(); // Option: Error if type def not found
                     maxOrdinal = currentOrdinal + 1; // Skip check if type unknown
                 } else {
                      maxOrdinal = typeDef->child_count - 1; // High ordinal
                 }

                 if (currentOrdinal >= maxOrdinal) {
                     fprintf(stderr, "Runtime error: Succ argument out of range (Enum '%s' overflow).\n", argVal.enum_val.enum_name ? argVal.enum_val.enum_name : "?");
                     EXIT_FAILURE_HANDLER();
                 }
                 // Create new enum value with incremented ordinal and same name
                 Value nextEnum = makeEnum(argVal.enum_val.enum_name, (int)(currentOrdinal + 1));
                 return nextEnum;
            }
         case TYPE_BYTE:
             currentOrdinal = argVal.i_val;
             maxOrdinal = 255;
              if (currentOrdinal >= maxOrdinal) { /* Overflow error */ EXIT_FAILURE_HANDLER(); }
             return makeInt(currentOrdinal + 1); // Or makeByte
         case TYPE_WORD:
              currentOrdinal = argVal.i_val;
             maxOrdinal = 65535;
              if (currentOrdinal >= maxOrdinal) { /* Overflow error */ EXIT_FAILURE_HANDLER(); }
             return makeInt(currentOrdinal + 1); // Or makeWord
        // Add other ordinal types if needed
        default:
            fprintf(stderr, "Runtime error: Succ() requires an ordinal type argument. Got %s.\n", varTypeToString(argVal.type));
            EXIT_FAILURE_HANDLER();
    }
     return makeVoid(); // Should not be reached
}

BuiltinRoutineType getBuiltinType(const char *name) {
    // List known FUNCTIONS (return values) - case-insensitive compare
    const char *functions[] = {
        "paramcount", "paramstr", "length", "pos", "ord", "chr",
        "abs", "sqrt", "cos", "sin", "tan", "ln", "exp", "trunc",
        "random", "wherex", "wherey", "ioresult", "eof", "copy",
        "upcase", "low", "high", "succ", "pred", // Added Pred assuming it might exist
        "inttostr", "api_send", "api_receive", "screencols", "screenrows",
        "keypressed", "mstreamcreate"
         // Add others like TryStrToInt, TryStrToFloat if implemented
    };
    int num_functions = sizeof(functions) / sizeof(functions[0]);
    for (int i = 0; i < num_functions; i++) {
        if (strcasecmp(name, functions[i]) == 0) {
            return BUILTIN_TYPE_FUNCTION;
        }
    }

    // List known PROCEDURES (no return value) - case-insensitive compare
    const char *procedures[] = {
         "writeln", "write", "readln", "read", "reset", "rewrite",
         "close", "assign", "halt", "inc", "dec", "delay",
         "randomize", "mstreamfree"
         // Add others like clrscr, gotoxy, assert if implemented
    };
    int num_procedures = sizeof(procedures) / sizeof(procedures[0]);
    for (int i = 0; i < num_procedures; i++) {
        if (strcasecmp(name, procedures[i]) == 0) {
            return BUILTIN_TYPE_PROCEDURE;
        }
    }

    // If not found in either list
    return BUILTIN_TYPE_NONE;
}


