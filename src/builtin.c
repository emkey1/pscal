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

void assignValueToLValue(AST *lvalueNode, Value newValue) {
    if (!lvalueNode) {
        fprintf(stderr, "Runtime error: Cannot assign to NULL lvalue node.\n");
        EXIT_FAILURE_HANDLER();
    }

    if (lvalueNode->type == AST_VARIABLE) {
        // Simple variable assignment
        Symbol *sym = lookupSymbol(lvalueNode->token->value); // Re-lookup symbol
        if (!sym) {
            fprintf(stderr, "Runtime error: Variable '%s' not found for assignment.\n", lvalueNode->token->value);
            EXIT_FAILURE_HANDLER();
        }
         // Check type compatibility (INTEGER is expected for Inc/Dec)
         if (sym->type != TYPE_INTEGER && sym->type != TYPE_ENUM && sym->type != TYPE_CHAR && sym->type != TYPE_BOOLEAN && sym->type != TYPE_BYTE && sym->type != TYPE_WORD) { // Allow related ordinal types
             fprintf(stderr, "Runtime error: Type mismatch assigning to '%s'. Expected ordinal type, got %s during Inc/Dec.\n", sym->name, varTypeToString(newValue.type));
             EXIT_FAILURE_HANDLER();
         }
        updateSymbol(sym->name, newValue); // Use existing updateSymbol
    }
    else if (lvalueNode->type == AST_FIELD_ACCESS) {
        // Record field assignment
        // 1. Evaluate the base record structure (e.g., `newHead` from `newHead.x`)
        //    We need the *original* symbol holding the record, not just eval(lvalueNode->left) which might be a copy.
        //    This requires traversing up if the left side is complex, or assuming left is AST_VARIABLE for now.
        AST* baseVarNode = lvalueNode->left;
        while(baseVarNode && baseVarNode->type != AST_VARIABLE) {
             // This is simplistic, might fail for nested records a[i].b.c
             if (baseVarNode->left) {
                  baseVarNode = baseVarNode->left;
             } else {
                  fprintf(stderr, "Runtime error: Cannot find base variable for field access in Inc/Dec.\n");
                  dumpASTFromRoot(lvalueNode);
                  EXIT_FAILURE_HANDLER();
                  return; // Added return
             }
        }
         if (!baseVarNode || baseVarNode->type != AST_VARIABLE) {
             fprintf(stderr, "Runtime error: Could not determine base variable for field access in Inc/Dec.\n");
             dumpASTFromRoot(lvalueNode);
              EXIT_FAILURE_HANDLER();
             return; // Added return
         }

        Symbol *recSym = lookupSymbol(baseVarNode->token->value);
        if (!recSym || recSym->value->type != TYPE_RECORD) {
            fprintf(stderr, "Runtime error: Base variable '%s' is not a record for field assignment in Inc/Dec.\n", baseVarNode->token->value);
            EXIT_FAILURE_HANDLER();
             return; // Added return
        }

        // 2. Find the specific field within the *symbol's* record value
        FieldValue *field = recSym->value->record_val;
        const char *targetFieldName = lvalueNode->token->value;
        while (field) {
            if (strcmp(field->name, targetFieldName) == 0) {
                 // Check type compatibility
                 if (field->value.type != TYPE_INTEGER && field->value.type != TYPE_ENUM && field->value.type != TYPE_CHAR && field->value.type != TYPE_BOOLEAN && field->value.type != TYPE_BYTE && field->value.type != TYPE_WORD) {
                      fprintf(stderr, "Runtime error: Type mismatch assigning to field '%s'. Expected ordinal type, got %s during Inc/Dec.\n", targetFieldName, varTypeToString(newValue.type));
                      EXIT_FAILURE_HANDLER();
                 }
                 // Assign the new value directly to the field within the symbol's record
                 field->value = newValue; // Assuming integer value copy is sufficient
                return;
            }
            field = field->next;
        }
        fprintf(stderr, "Runtime error: Field '%s' not found in record '%s' for assignment in Inc/Dec.\n", targetFieldName, recSym->name);
        EXIT_FAILURE_HANDLER();

    }
     else if (lvalueNode->type == AST_ARRAY_ACCESS) {
         // Array element assignment
         // 1. Find the base array symbol
         AST* baseVarNode = lvalueNode->left;
         // Simplified traversal (like field access) - might need improvement for complex bases
         while(baseVarNode && baseVarNode->type != AST_VARIABLE) {
              if (baseVarNode->left) baseVarNode = baseVarNode->left;
              else { /* Error */ fprintf(stderr,"Cannot find base var for array access\n"); EXIT_FAILURE_HANDLER(); return; }
         }
          if (!baseVarNode || baseVarNode->type != AST_VARIABLE) { /* Error */ fprintf(stderr,"Cannot find base var for array access\n"); EXIT_FAILURE_HANDLER(); return; }

         Symbol *arrSym = lookupSymbol(baseVarNode->token->value);
         if (!arrSym || (arrSym->value->type != TYPE_ARRAY && arrSym->value->type != TYPE_STRING)) { // Allow string indexing too
             fprintf(stderr, "Runtime error: Base variable '%s' is not an array/string for assignment in Inc/Dec.\n", baseVarNode->token->value);
             EXIT_FAILURE_HANDLER();
             return; // Added return
         }

         // Handle string indexing separately (Inc/Dec likely invalid here, but check)
         if (arrSym->value->type == TYPE_STRING) {
              fprintf(stderr, "Runtime error: Cannot use Inc/Dec on string elements.\n");
              EXIT_FAILURE_HANDLER();
              return; // Added return
         }

         // 2. Evaluate indices
         if (lvalueNode->child_count != arrSym->value->dimensions) {
              fprintf(stderr, "Runtime error: Incorrect number of indices for array '%s' in Inc/Dec. Expected %d, got %d.\n", arrSym->name, arrSym->value->dimensions, lvalueNode->child_count);
              EXIT_FAILURE_HANDLER();
               return; // Added return
         }
         int *indices = malloc(sizeof(int) * arrSym->value->dimensions);
         if (!indices) { /* Mem error */ EXIT_FAILURE_HANDLER(); return; } // Added return
         for (int i = 0; i < lvalueNode->child_count; i++) {
             Value idxVal = eval(lvalueNode->children[i]);
             if (idxVal.type != TYPE_INTEGER) { /* Type error */ free(indices); EXIT_FAILURE_HANDLER(); return; } // Added return
             indices[i] = (int)idxVal.i_val;
         }

         // 3. Calculate flat offset
         int offset = computeFlatOffset(arrSym->value, indices);

          // Bounds check for offset (copied from eval array access)
         int total_size = 1;
         for (int i = 0; i < arrSym->value->dimensions; i++) {
             total_size *= (arrSym->value->upper_bounds[i] - arrSym->value->lower_bounds[i] + 1);
         }
         if (offset < 0 || offset >= total_size) {
             fprintf(stderr, "Runtime error: Array index out of bounds during Inc/Dec assignment (offset %d, size %d).\n", offset, total_size);
             free(indices);
             EXIT_FAILURE_HANDLER();
              return; // Added return
         }


         // 4. Assign new value to the element in the *symbol's* array
         Value *targetElement = &(arrSym->value->array_val[offset]);
          // Check type compatibility
         if (targetElement->type != TYPE_INTEGER && targetElement->type != TYPE_ENUM && targetElement->type != TYPE_CHAR && targetElement->type != TYPE_BOOLEAN && targetElement->type != TYPE_BYTE && targetElement->type != TYPE_WORD) {
             fprintf(stderr, "Runtime error: Type mismatch assigning to array element of type %s during Inc/Dec.\n", varTypeToString(targetElement->type));
             free(indices);
             EXIT_FAILURE_HANDLER();
              return; // Added return
         }
         *targetElement = newValue; // Assuming integer value copy is sufficient

         free(indices);
     }
    else {
        fprintf(stderr, "Runtime error: Cannot apply Inc/Dec to the given expression type (%s).\n", astTypeToString(lvalueNode->type));
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
    double x = (arg.type == TYPE_INTEGER ? arg.i_val : arg.r_val);
    if (x < 0) { fprintf(stderr, "Runtime error: sqrt expects a non-negative argument.\n"); EXIT_FAILURE_HANDLER(); }
    return makeReal(sqrt(x));
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
    if (arg.type == TYPE_INTEGER)
        return makeInt(llabs(arg.i_val));
    else
        return makeReal(fabs(arg.r_val));
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
void executeBuiltinAssign(AST *node) {
    if (node->child_count != 2) { fprintf(stderr, "Runtime error: assign expects 2 arguments.\n"); EXIT_FAILURE_HANDLER(); }
    Value fileVal = eval(node->children[0]);
    Value nameVal = eval(node->children[1]);
    if (fileVal.type != TYPE_FILE) { fprintf(stderr, "Runtime error: first parameter to assign must be a file variable.\n"); EXIT_FAILURE_HANDLER(); }
    if (nameVal.type != TYPE_STRING) { fprintf(stderr, "Runtime error: second parameter to assign must be a string.\n"); EXIT_FAILURE_HANDLER(); }
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName);
    if (!sym) { fprintf(stderr, "Runtime error: file variable '%s' not declared.\n", fileVarName); EXIT_FAILURE_HANDLER(); }
    if (sym->value->filename) free(sym->value->filename);
    sym->value->filename = strdup(nameVal.s_val);
}

void executeBuiltinClose(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: close expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) { fprintf(stderr, "Runtime error: close parameter must be a file variable.\n"); EXIT_FAILURE_HANDLER(); }
    if (!fileVal.f_val) { fprintf(stderr, "Runtime error: file is not open.\n"); EXIT_FAILURE_HANDLER(); }
    fclose(fileVal.f_val);
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName);
    if (sym->value->filename) {
        free(sym->value->filename);
        sym->value->filename = NULL;
    }
}

void executeBuiltinReset(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: reset expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) { fprintf(stderr, "Runtime error: reset parameter must be a file variable.\n"); EXIT_FAILURE_HANDLER(); }
    if (fileVal.filename == NULL) { fprintf(stderr, "Runtime error: file variable not assigned a filename.\n"); EXIT_FAILURE_HANDLER(); }
    FILE *f = fopen(fileVal.filename, "r");
    if (f == NULL) { last_io_error = 1; return; }
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName);
    sym->value->f_val = f;
    last_io_error = 0;
}

void executeBuiltinRewrite(AST *node) {
    if (node->child_count != 1) { fprintf(stderr, "Runtime error: rewrite expects 1 argument.\n"); EXIT_FAILURE_HANDLER(); }
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) { fprintf(stderr, "Runtime error: rewrite parameter must be a file variable.\n"); EXIT_FAILURE_HANDLER(); }
    if (fileVal.filename == NULL) { fprintf(stderr, "Runtime error: file variable not assigned a filename.\n"); EXIT_FAILURE_HANDLER(); }
    FILE *f = fopen(fileVal.filename, "w");
    if (!f) { fprintf(stderr, "Runtime error: could not open file '%s' for writing.\n", fileVal.filename); EXIT_FAILURE_HANDLER(); }
    const char *fileVarName = node->children[0]->token->value;
    Symbol *sym = lookupSymbol(fileVarName);
    sym->value->f_val = f;
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
    if (arg.type != TYPE_STRING) { fprintf(stderr, "Runtime error: length argument must be a string.\n"); EXIT_FAILURE_HANDLER(); }
    int len = (int)strlen(arg.s_val);
    return makeInt(len);
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
void executeBuiltinHalt(AST *node) {
    long long code = 0;
    // Optionally allow one argument (the exit code)
    if (node->child_count == 1) {
        Value arg = eval(node->children[0]);
        if (arg.type != TYPE_INTEGER) {
            fprintf(stderr, "Runtime error: halt expects an integer argument.\n");
            EXIT_FAILURE_HANDLER();
        }
        code = arg.i_val;
    } else if (node->child_count != 0) {
        fprintf(stderr, "Runtime error: halt expects 0 or 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    exit((int)code);
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

void executeBuiltinInc(AST *node) {
    if (node->child_count < 1 || node->child_count > 2) {
        fprintf(stderr, "Runtime error: Inc expects 1 or 2 arguments.\n"); // Corrected error message
        EXIT_FAILURE_HANDLER();
    }
    AST *lvalueNode = node->children[0];
    // Check if lvalueNode is assignable (Variable, Field Access, Array Access)
    if (lvalueNode->type != AST_VARIABLE && lvalueNode->type != AST_FIELD_ACCESS && lvalueNode->type != AST_ARRAY_ACCESS) {
         fprintf(stderr, "Runtime error: First argument to Inc must be a variable, field, or array element.\n");
         EXIT_FAILURE_HANDLER();
    }

    Value currentVal = eval(lvalueNode);
    long long current_iVal = -1; // Placeholder
    VarType originalType = currentVal.type;

    // Determine current ordinal value based on type
    // (Keep existing logic for determining current_iVal based on originalType)
    if (originalType == TYPE_INTEGER || originalType == TYPE_BOOLEAN || originalType == TYPE_BYTE || originalType == TYPE_WORD) {
        current_iVal = currentVal.i_val;
    } else if (originalType == TYPE_CHAR) {
        current_iVal = currentVal.c_val;
    } else if (originalType == TYPE_ENUM) {
        current_iVal = currentVal.enum_val.ordinal;
    } else {
        fprintf(stderr, "Runtime error: inc can only operate on ordinal types. Got %s\n", varTypeToString(originalType));
        EXIT_FAILURE_HANDLER();
    }


    long long increment = 1; // Default increment
    // *** FIX: Evaluate the second argument if present ***
    if (node->child_count == 2) {
        Value incrVal = eval(node->children[1]); // Evaluate the second argument
        if (incrVal.type != TYPE_INTEGER) {
            fprintf(stderr, "Runtime error: Inc step amount (second argument) must be an integer. Got %s\n", varTypeToString(incrVal.type));
            EXIT_FAILURE_HANDLER();
        }
        increment = incrVal.i_val; // Use the provided integer value
        if (increment < 0) { // Standard Pascal Inc usually requires positive step
             fprintf(stderr, "Warning: Inc called with negative step %lld.\n", increment);
             // Consider making this an error depending on desired compatibility
        }
    }
    // *** END FIX ***

    long long new_iVal = current_iVal + increment;
    Value newValue = makeInt(0); // Value to assign back

    // Create the correct type of value for the result
    // (Keep existing switch statement for creating newValue based on originalType)
    switch (originalType) {
         case TYPE_INTEGER: newValue = makeInt(new_iVal); break;
         case TYPE_BOOLEAN:
             if (new_iVal > 1 || new_iVal < 0) { fprintf(stderr, "Runtime error: Boolean range error on Inc.\n"); EXIT_FAILURE_HANDLER(); }
             newValue = makeBoolean((int)new_iVal); break;
         case TYPE_CHAR:
             if (new_iVal > 255 || new_iVal < 0) { fprintf(stderr, "Runtime error: Char range error on Inc.\n"); EXIT_FAILURE_HANDLER(); }
             newValue = makeChar((char)new_iVal); break;
         case TYPE_BYTE:
              if (new_iVal > 255 || new_iVal < 0) { fprintf(stderr, "Runtime error: Byte range error on Inc.\n"); EXIT_FAILURE_HANDLER(); }
              newValue = makeInt(new_iVal); newValue.type = TYPE_BYTE; break;
         case TYPE_WORD:
              if (new_iVal > 65535 || new_iVal < 0) { fprintf(stderr, "Runtime error: Word range error on Inc.\n"); EXIT_FAILURE_HANDLER(); }
              newValue = makeInt(new_iVal); newValue.type = TYPE_WORD; break;
         case TYPE_ENUM:
             {
                 AST* typeDef = lookupType(currentVal.enum_val.enum_name);
                 long long maxOrdinal = -1;
                 if (typeDef && typeDef->type == AST_ENUM_TYPE) { maxOrdinal = typeDef->child_count - 1; }
                 if (maxOrdinal != -1 && new_iVal > maxOrdinal) { fprintf(stderr, "Runtime error: Enum overflow on Inc for type '%s'.\n", currentVal.enum_val.enum_name ? currentVal.enum_val.enum_name : "?"); EXIT_FAILURE_HANDLER(); }
                 if (new_iVal < 0) { fprintf(stderr, "Runtime error: Enum underflow on Inc for type '%s'.\n", currentVal.enum_val.enum_name ? currentVal.enum_val.enum_name : "?"); EXIT_FAILURE_HANDLER(); } // Check lower bound too
                 newValue = makeEnum(currentVal.enum_val.enum_name, (int)new_iVal);
             }
             break;
         default: EXIT_FAILURE_HANDLER(); break;
    }

    // Assign the new value back
    assignValueToLValue(lvalueNode, newValue);

    if (newValue.type == TYPE_ENUM && newValue.enum_val.enum_name) {
         // If makeEnum strdup'd the name, free the copy in newValue now that it's assigned
         // free(newValue.enum_val.enum_name); // Be cautious with ownership here
    }
}

void executeBuiltinDec(AST *node) {
     if (node->child_count < 1 || node->child_count > 2) {
         fprintf(stderr, "Runtime error: Dec expects 1 or 2 arguments.\n"); // Corrected error message
         EXIT_FAILURE_HANDLER();
     }
     AST *lvalueNode = node->children[0];
     // Check if lvalueNode is assignable
    if (lvalueNode->type != AST_VARIABLE && lvalueNode->type != AST_FIELD_ACCESS && lvalueNode->type != AST_ARRAY_ACCESS) {
         fprintf(stderr, "Runtime error: First argument to Dec must be a variable, field, or array element.\n");
         EXIT_FAILURE_HANDLER();
    }


    Value currentVal = eval(lvalueNode);
    long long current_iVal = -1;
    VarType originalType = currentVal.type;

    // Determine current ordinal value based on type
    // (Keep existing logic for determining current_iVal based on originalType)
    if (originalType == TYPE_INTEGER || originalType == TYPE_BOOLEAN || originalType == TYPE_BYTE || originalType == TYPE_WORD) current_iVal = currentVal.i_val;
    else if (originalType == TYPE_CHAR) current_iVal = currentVal.c_val;
    else if (originalType == TYPE_ENUM) current_iVal = currentVal.enum_val.ordinal;
    else { fprintf(stderr, "Runtime error: dec can only operate on ordinal types. Got %s\n", varTypeToString(originalType)); EXIT_FAILURE_HANDLER(); }


    long long decrement = 1; // Default decrement
    // *** FIX: Evaluate the second argument if present ***
    if (node->child_count == 2) {
        Value decrVal = eval(node->children[1]); // Evaluate the second argument
        if (decrVal.type != TYPE_INTEGER) {
            fprintf(stderr, "Runtime error: Dec step amount (second argument) must be an integer. Got %s\n", varTypeToString(decrVal.type));
            EXIT_FAILURE_HANDLER();
        }
        decrement = decrVal.i_val; // Use the provided integer value
        if (decrement < 0) { // Standard Pascal Dec usually requires positive step
             fprintf(stderr, "Warning: Dec called with negative step %lld.\n", decrement);
             // Consider making this an error
        }
    }
    // *** END FIX ***


    long long new_iVal = current_iVal - decrement;
    Value newValue = makeInt(0);

    // Create the correct type of value for the result
    // (Keep existing switch statement for creating newValue based on originalType)
     switch (originalType) {
         case TYPE_INTEGER: newValue = makeInt(new_iVal); break;
         case TYPE_BOOLEAN:
             if (new_iVal < 0 || new_iVal > 1) { fprintf(stderr, "Runtime error: Boolean range error on Dec.\n"); EXIT_FAILURE_HANDLER(); }
             newValue = makeBoolean((int)new_iVal); break;
         case TYPE_CHAR:
             if (new_iVal < 0 || new_iVal > 255) { fprintf(stderr, "Runtime error: Char range error on Dec.\n"); EXIT_FAILURE_HANDLER(); }
             newValue = makeChar((char)new_iVal); break;
          case TYPE_BYTE:
              if (new_iVal < 0 || new_iVal > 255) { fprintf(stderr, "Runtime error: Byte range error on Dec.\n"); EXIT_FAILURE_HANDLER(); }
              newValue = makeInt(new_iVal); newValue.type = TYPE_BYTE; break;
         case TYPE_WORD:
              if (new_iVal < 0 || new_iVal > 65535) { fprintf(stderr, "Runtime error: Word range error on Dec.\n"); EXIT_FAILURE_HANDLER(); }
              newValue = makeInt(new_iVal); newValue.type = TYPE_WORD; break;
         case TYPE_ENUM:
             {
                 if (new_iVal < 0) { fprintf(stderr, "Runtime error: Enum underflow on Dec for type '%s'.\n", currentVal.enum_val.enum_name ? currentVal.enum_val.enum_name : "?"); EXIT_FAILURE_HANDLER(); }
                 // Need High check? Only necessary if decrement could be negative/large. Standard Dec won't overflow High.
                 newValue = makeEnum(currentVal.enum_val.enum_name, (int)new_iVal);
             }
             break;
         default: EXIT_FAILURE_HANDLER(); break;
    }


    assignValueToLValue(lvalueNode, newValue);

    if (newValue.type == TYPE_ENUM && newValue.enum_val.enum_name) {
        // If makeEnum strdup'd the name, free the copy in newValue now that it's assigned
        // free(newValue.enum_val.enum_name); // Be cautious with ownership here
    }
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

void executeBuiltinRandomize(AST *node) {
    if (node->child_count != 0) { fprintf(stderr, "Runtime error: Randomize expects no arguments.\n"); EXIT_FAILURE_HANDLER(); }
    srand((unsigned int)time(NULL));
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

void executeBuiltinDelay(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Delay expects 1 argument (milliseconds).\n");
        EXIT_FAILURE_HANDLER();
    }

    Value msVal = eval(node->children[0]);

    // Pascal 'word' is typically unsigned 0-65535. We'll accept INTEGER type.
    if (msVal.type != TYPE_INTEGER && msVal.type != TYPE_WORD) { // Allow TYPE_WORD if you added it
         fprintf(stderr, "Runtime error: Delay argument must be an integer or word type. Got %s\n", varTypeToString(msVal.type));
         EXIT_FAILURE_HANDLER();
    }

    long long ms = msVal.i_val;

    if (ms < 0) {
        // Negative delay doesn't make sense, treat as 0 or error.
        // Turbo Pascal likely treated it as 0 or wrapped around for 'word'.
        // We'll just do nothing for negative values for simplicity.
        ms = 0;
    }

    // usleep expects microseconds (millionths of a second)
    // Convert milliseconds (thousandths) to microseconds
    useconds_t usec = (useconds_t)ms * 1000;

    // Call usleep
    usleep(usec);

    // Delay is a procedure, doesn't return a value to Pascal side
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

void executeBuiltinMstreamFree(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: TMemoryStream.Free expects 1 argument (a memory stream).\n");
        EXIT_FAILURE_HANDLER();
    }
    Value msVal = eval(node->children[0]);
    if (msVal.type != TYPE_MEMORYSTREAM) {
        fprintf(stderr, "Runtime error: parameter of MStreamFree must be a Type MStream.\n");
        EXIT_FAILURE_HANDLER();
    }
    if (msVal.mstream->buffer)
        free(msVal.mstream->buffer);
    free(msVal.mstream);
}

// Special
Value executeBuiltinResult(AST *node) {
    if (node->child_count != 0) { fprintf(stderr, "Runtime error: result expects no arguments.\n"); EXIT_FAILURE_HANDLER(); }
    if (current_function_symbol == NULL) { fprintf(stderr, "Runtime error: result called outside a function.\n"); EXIT_FAILURE_HANDLER(); }
    return *(current_function_symbol->value);
}

Value executeBuiltinProcedure(AST *node) {
    if (strcmp(node->token->value, "cos") == 0)
        return executeBuiltinCos(node);
    if (strcmp(node->token->value, "sin") == 0)
        return executeBuiltinSin(node);
    if (strcmp(node->token->value, "tan") == 0)
        return executeBuiltinTan(node);
    if (strcmp(node->token->value, "sqrt") == 0)
        return executeBuiltinSqrt(node);
    if (strcmp(node->token->value, "ln") == 0)
        return executeBuiltinLn(node);
    if (strcmp(node->token->value, "exp") == 0)
        return executeBuiltinExp(node);
    if (strcmp(node->token->value, "abs") == 0)
        return executeBuiltinAbs(node);
    if (strcmp(node->token->value, "eof") == 0)
        return executeBuiltinEOF(node);
    if (strcmp(node->token->value, "pos") == 0)
        return executeBuiltinPos(node);
    if (strcmp(node->token->value, "close") == 0) {
        executeBuiltinClose(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "halt") == 0) {
         executeBuiltinHalt(node); // Call the function that exits
         // Note: exit() should prevent the code below from running,
         // but returning makeVoid() is formally correct for a procedure.
         return makeVoid();
    }
    if (strcmp(node->token->value, "low") == 0) // Use lowercase
        return executeBuiltinLow(node);
    if (strcmp(node->token->value, "high") == 0) // Use lowercase
        return executeBuiltinHigh(node);
    if (strcmp(node->token->value, "succ") == 0) // Use lowercase
        return executeBuiltinSucc(node);
    if (strcasecmp(node->token->value, "keypressed") == 0) { // Use strcasecmp if Pascal is case-insensitive
         return executeBuiltinKeyPressed(node);
    }
    if (strcmp(node->token->value, "assign") == 0) {
        executeBuiltinAssign(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "copy") == 0)
        return executeBuiltinCopy(node);
    if (strcmp(node->token->value, "mstreamloadfromfile") == 0)
        return executeBuiltinMstreamLoadFromFile(node);
    if (strcmp(node->token->value, "mstreamsavetofile") == 0)
        return executeBuiltinMstreamSaveToFile(node);
    if (strcmp(node->token->value, "mstreamfree") == 0) {
        executeBuiltinMstreamFree(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "mstreamcreate") == 0)
        return executeBuiltinMstreamCreate(node);
    if (strcmp(node->token->value, "inc") == 0) {
        executeBuiltinInc(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "dec") == 0) {
        executeBuiltinDec(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "ioresult") == 0)
        return executeBuiltinIOResult(node);
    if (strcmp(node->token->value, "result") == 0)
        return executeBuiltinResult(node);
    if (strcmp(node->token->value, "length") == 0)
        return executeBuiltinLength(node);
    if (strcmp(node->token->value, "randomize") == 0) {
        executeBuiltinRandomize(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "random") == 0)
        return executeBuiltinRandom(node);
    if (strcmp(node->token->value, "reset") == 0) {
        executeBuiltinReset(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "delay") == 0) { // Check for "delay"
        executeBuiltinDelay(node);                  // Call the C implementation
        return makeVoid();                          // Procedures return void
    }
    if (strcasecmp(node->token->value, "screencols") == 0) // Use strcasecmp for case-insensitivity
        return executeBuiltinScreenCols(node);
    if (strcasecmp(node->token->value, "screenrows") == 0)
        return executeBuiltinScreenRows(node);
    if (strcmp(node->token->value, "rewrite") == 0) {
        executeBuiltinRewrite(node);
        return makeVoid();
    }
    if (strcmp(node->token->value, "trunc") == 0)
        return executeBuiltinTrunc(node);
    if (strcmp(node->token->value, "upcase") == 0)
        return executeBuiltinUpcase(node);
    if (strcmp(node->token->value, "ord") == 0)
        return executeBuiltinOrd(node);
    if (strcmp(node->token->value, "chr") == 0)
        return executeBuiltinChr(node);
    if (strcmp(node->token->value, "api_send") == 0)
        return executeBuiltinAPISend(node);
    if (strcmp(node->token->value, "api_receive") == 0)
        return executeBuiltinAPIReceive(node);
    if (strcmp(node->token->value, "paramstr") == 0)
        return executeBuiltinParamstr(node);
    if (strcmp(node->token->value, "paramcount") == 0)
        return executeBuiltinParamcount(node);
    if (strcmp(node->token->value, "readkey") == 0)
        return executeBuiltinReadKey(node);
    if (strcasecmp(node->token->value, "wherex") == 0)
        return executeBuiltinWhereX(node);
    if (strcasecmp(node->token->value, "wherey") == 0)
        return executeBuiltinWhereY(node);
    if (strcmp(node->token->value, "inttostr") == 0)
        return executeBuiltinIntToStr(node);
  //  if (strcmp(node->token->value, "trystrtofloat") == 0)
   //     return execute_builtin_trystrtofloat(node);
    else
        return makeVoid();
}

void registerBuiltinFunction(const char *name, ASTNodeType declType) {
    char *lowerName = strdup(name);
    if (!lowerName) {
        fprintf(stderr, "Memory allocation error in register_builtin_function\n");
        EXIT_FAILURE_HANDLER();
    }
    for (int i = 0; lowerName[i] != '\0'; i++) {
        lowerName[i] = tolower((unsigned char)lowerName[i]);
    }

    Token *token = newToken(TOKEN_IDENTIFIER, lowerName);
    AST *dummy = newASTNode(declType, token);
    dummy->child_count = 0;
    dummy->child_capacity = 0;
    setLeft(dummy, NULL);
    setRight(dummy, NULL);
    setExtra(dummy, NULL);

    if (strcmp(lowerName, "api_send") == 0) {
        AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "mstream"));
        setTypeAST(retTypeNode, TYPE_MEMORYSTREAM);
        setRight(dummy, retTypeNode);
    } else if (strcmp(lowerName, "api_receive") == 0) {
        AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "string"));
        setTypeAST(retTypeNode, TYPE_STRING);
        setRight(dummy, retTypeNode);
    } else if (strcmp(lowerName, "chr") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST *));
        if (!dummy->children) {
            fprintf(stderr, "Memory allocation error in register_builtin_function for chr.\n");
            EXIT_FAILURE_HANDLER();
        }
        AST *param = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(param, TYPE_INTEGER);
        AST *var = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "_chr_arg"));
        addChild(param, var);
        dummy->children[0] = param;
        dummy->child_count = 1;

        AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "char"));
        setTypeAST(retTypeNode, TYPE_CHAR);
        setRight(dummy, retTypeNode);
    } else if (strcmp(lowerName, "ord") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST *));
        if (!dummy->children) {
            fprintf(stderr, "Memory allocation error in register_builtin_function for ord.\n");
            EXIT_FAILURE_HANDLER();
        }
        AST *param = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(param, TYPE_CHAR);
        AST *var = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "_ord_arg"));
        addChild(param, var);
        dummy->children[0] = param;
        dummy->child_count = 1;

        AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "integer"));
        setTypeAST(retTypeNode, TYPE_INTEGER);
        setRight(dummy, retTypeNode);
    } else if (strcmp(lowerName, "wherex") == 0) {
        // No parameters needed (child_count already 0)
        // Set return type to Integer
        AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "integer"));
        setTypeAST(retTypeNode, TYPE_INTEGER);
        setRight(dummy, retTypeNode); // Assign return type node
        dummy->var_type = TYPE_INTEGER; // Set type on the FUNCTION_DECL node itself
   } else if (strcmp(lowerName, "wherey") == 0) {
        // No parameters needed
        // Set return type to Integer
        AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "integer"));
        setTypeAST(retTypeNode, TYPE_INTEGER);
        setRight(dummy, retTypeNode); // Assign return type node
        dummy->var_type = TYPE_INTEGER; // Set type on the FUNCTION_DECL node itself
   } else if (strcasecmp(lowerName, "keypressed") == 0) {
       AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "boolean"));
       setTypeAST(retTypeNode, TYPE_BOOLEAN);
       setRight(dummy, retTypeNode); // Assign return type node
       dummy->var_type = TYPE_BOOLEAN; // Set type on the FUNCTION_DECL node itself
  } else if (strcmp(lowerName, "inttostr") == 0) {
      AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "string"));
      setTypeAST(retTypeNode, TYPE_STRING);
      setRight(dummy, retTypeNode); // dummy is the AST_FUNCTION_DECL node
      dummy->var_type = TYPE_STRING; // Set type on the FUNCTION_DECL node itself
  } else if (strcasecmp(lowerName, "screencols") == 0) { // Use strcasecmp for consistency if needed
      AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "integer"));
      setTypeAST(retTypeNode, TYPE_INTEGER);
      setRight(dummy, retTypeNode); // Link type node as right child
      dummy->var_type = TYPE_INTEGER; // Set function's return type
 } else if (strcasecmp(lowerName, "screenrows") == 0) { // Use strcasecmp for consistency if needed
      AST *retTypeNode = newASTNode(AST_VARIABLE, newToken(TOKEN_IDENTIFIER, "integer"));
      setTypeAST(retTypeNode, TYPE_INTEGER);
      setRight(dummy, retTypeNode); // Link type node as right child
      dummy->var_type = TYPE_INTEGER; // Set function's return type
 }
    addProcedure(dummy);
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
    // List all built-in procedure names (lowercase).
    const char *builtins[] = {
        "cos","sin","tan","sqrt","ln","exp","abs",
        "trunc","assign","close","reset","rewrite",
        "eof","ioresult","copy","length","pos",
        "upcase","halt","inc","randomize","random",
        "mstreamcreate","mstreamloadfromfile",
        "mstream_savetofile","mstream_free","ord",
        "chr","api_send","api_receive","paramstr",
        "paramcount","readkey", "dec", "wherex",
        "wherey", "delay", "keypressed",
        "low", "high", "succ", "inttostr", "screencols",
        "screenrows"

    };

    int num_builtins = sizeof(builtins) / sizeof(builtins[0]);
    for (int i = 0; i < num_builtins; i++) {
        if (strcasecmp(name, builtins[i]) == 0)
            return 1;
    }
    return 0;
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
