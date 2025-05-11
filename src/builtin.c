#include "builtin.h"
#include "parser.h"
#include "utils.h"
#include "interpreter.h"
#include "symbol.h"
#include "sdl.h"
#include "audio.h"
#include "sdl.h"
#include "audio.h"
#include "globals.h"
#include <math.h>
#include <termios.h>
#include <unistd.h>     // For read, write, STDIN_FILENO, STDOUT_FILENO, isatty
#include <ctype.h>      // For isdigit
#include <errno.h>
#include <sys/ioctl.h> // For ioctl, FIONREAD
#include <stdint.h>

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
    {"cleardevice", executeBuiltinClearDevice},
    {"close",     executeBuiltinClose},
    {"closegraph", executeBuiltinCloseGraph},
    {"copy",      executeBuiltinCopy},
    {"cos",       executeBuiltinCos},
    {"createtexture", executeBuiltinCreateTexture},
    {"dec",       executeBuiltinDec},         // Include Dec
    {"delay",     executeBuiltinDelay},
    {"destroytexture", executeBuiltinDestroyTexture},
    {"dispose",   executeBuiltinDispose},
    {"drawcircle", executeBuiltinDrawCircle},
    {"drawline", executeBuiltinDrawLine},
    {"drawrect",  executeBuiltinDrawRect},
    {"eof",       executeBuiltinEOF},
    {"exp",       executeBuiltinExp},
    {"fillcircle", executeBuiltinFillCircle},
    {"fillrect", executeBuiltinFillRect},
    {"getmaxx",   executeBuiltinGetMaxX},
    {"getmaxy",   executeBuiltinGetMaxY},
    {"getmousestate", executeBuiltinGetMouseState},
    {"graphloop", executeBuiltinGraphLoop},
    {"halt",      executeBuiltinHalt},
    {"high",      executeBuiltinHigh},
    {"inc",       executeBuiltinInc},
    {"initgraph", executeBuiltinInitGraph},
    {"initsoundsystem", executeBuiltinInitSoundSystem},
    {"inittextsystem", executeBuiltinInitTextSystem},
    {"inttostr",  executeBuiltinIntToStr},
    {"ioresult",  executeBuiltinIOResult},
    {"issoundplaying", executeBuiltinIsSoundPlaying},
    {"keypressed", executeBuiltinKeyPressed},
    {"length",    executeBuiltinLength},
    {"ln",        executeBuiltinLn},
    {"loadsound", executeBuiltinLoadSound},
    {"low",       executeBuiltinLow},
    {"mstreamcreate", executeBuiltinMstreamCreate},
    {"mstreamfree", executeBuiltinMstreamFree},
    {"mstreamloadfromfile", executeBuiltinMstreamLoadFromFile},
    {"mstreamsavetofile", executeBuiltinMstreamSaveToFile}, // Corrected name based on registration
    {"new",       executeBuiltinNew},
    {"ord",       executeBuiltinOrd},
    {"outtextxy", executeBuiltinOutTextXY},
    {"paramcount", executeBuiltinParamcount},
    {"paramstr",  executeBuiltinParamstr},
    {"playsound", executeBuiltinPlaySound},
    {"pos",       executeBuiltinPos},
    {"putpixel",  executeBuiltinPutPixel},
    {"quitrequested", executeBuiltinQuitRequested},
    {"quitsoundsystem", executeBuiltinQuitSoundSystem},
    {"quittextsystem", executeBuiltinQuitTextSystem},
    {"random",    executeBuiltinRandom},
    {"randomize", executeBuiltinRandomize},
    {"readkey",   executeBuiltinReadKey},
    {"rendercopy", executeBuiltinRenderCopy},
    {"rendercopyrect", executeBuiltinRenderCopyRect},
    {"reset",     executeBuiltinReset},
    // {"result",    executeBuiltinResult}, // 'result' is special, handled differently? Let's assume not dispatched here.
    {"rewrite",   executeBuiltinRewrite},
    {"screencols", executeBuiltinScreenCols},
    {"screenrows", executeBuiltinScreenRows},
    {"setcolor",  executeBuiltinSetColor},
    {"setrgbcolor", executeBuiltinSetRGBColor},
    {"sin",       executeBuiltinSin},
    {"sqr",       executeBuiltinSqr},
    {"sqrt",      executeBuiltinSqrt},
    {"succ",      executeBuiltinSucc},
    {"tan",       executeBuiltinTan},
    {"textbackground", executeBuiltinTextBackground},
    {"textbackgrounde", executeBuiltinTextBackgroundE},
    {"textcolor", executeBuiltinTextColor},
    {"textcolore", executeBuiltinTextColorE},
    {"trunc",     executeBuiltinTrunc},
    {"upcase",    executeBuiltinUpcase},
    {"updatescreen", executeBuiltinUpdateScreen},
    {"updatetexture", executeBuiltinUpdateTexture},
    {"waitkeyevent", executeBuiltinWaitKeyEvent},
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
    // Check argument count
    if (node->child_count != 3) {
        fprintf(stderr, "Runtime error: copy expects 3 arguments.\n");
        EXIT_FAILURE_HANDLER(); // Exit considered acceptable for fatal argument errors
    }

    // Evaluate arguments
    Value sourceVal = eval(node->children[0]);
    Value startVal  = eval(node->children[1]);
    Value countVal  = eval(node->children[2]);
    Value result = makeString(""); // Default return value on error or empty result

    // Buffer for potential char source conversion
    char char_source_buf[2];
    const char *src_ptr = NULL;
    size_t src_len = 0;

    // --- Type checks and Source Preparation ---
    if (startVal.type != TYPE_INTEGER || countVal.type != TYPE_INTEGER) {
        fprintf(stderr, "Runtime error: copy requires integer start index and count.\n");
        goto cleanup; // Go to cleanup to free evaluated values
    }

    if (sourceVal.type == TYPE_STRING) {
        src_ptr = sourceVal.s_val ? sourceVal.s_val : ""; // Handle NULL string pointer safely
        src_len = strlen(src_ptr);
    } else if (sourceVal.type == TYPE_CHAR) {
        char_source_buf[0] = sourceVal.c_val;
        char_source_buf[1] = '\0';
        src_ptr = char_source_buf; // Point src_ptr to the temporary buffer
        src_len = 1; // Source length is 1 for a char
    } else {
        fprintf(stderr, "Runtime error: copy requires a string or char source argument.\n");
        goto cleanup; // Go to cleanup
    }

    // --- Get and validate start/count values ---
    long long start_ll = startVal.i_val;
    long long count_ll = countVal.i_val;

    // Use size_t for indices and counts where appropriate after validation
    if (start_ll < 1 || count_ll < 0) {
        fprintf(stderr, "Runtime error: copy: invalid start index (%lld) or count (%lld).\n", start_ll, count_ll);
        goto cleanup; // Go to cleanup
    }
    size_t start = (size_t)start_ll; // Convert after validation
    size_t count = (size_t)count_ll; // Convert after validation


    // --- Bounds checks and count adjustment ---
    if (start > src_len) {
        // Start is past the end, result is empty string (already set as default)
        goto cleanup; // Go to cleanup
    }
    // Adjust count if it exceeds available characters from start position
    // Note: start is 1-based, src_ptr is 0-based
    if (start - 1 + count > src_len) {
        count = src_len - (start - 1);
    }

    // --- Copy Substring ---
    if (count > 0) { // Only proceed if there's something to copy
        char *substr = malloc(count + 1); // Use size_t count
        if (!substr) {
            fprintf(stderr, "Memory allocation error in copy().\n");
            // No EXIT here, allow cleanup
            goto cleanup; // Go to cleanup, result will be empty string
        }

        // Copy the substring
        strncpy(substr, src_ptr + (start - 1), count); // Use size_t count
        substr[count] = '\0'; // Ensure null termination

        // Create result Value (free previous default empty string first)
        freeValue(&result); // Free the "" allocated initially
        result = makeString(substr); // makeString copies substr

        // Free the temporary buffer
        free(substr);
    }
    // If count is 0, the default empty string in 'result' is correct

cleanup:
    // Free evaluated arguments - ALWAYS do this before returning
    freeValue(&sourceVal);
    freeValue(&startVal);
    freeValue(&countVal);

    return result; // Return the actual result or the default empty string on error/empty case
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

Value executeBuiltinSqr(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Sqr expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value arg = eval(node->children[0]);
    Value result = makeVoid(); // Default error

    if (arg.type == TYPE_INTEGER) {
        long long i_val = arg.i_val;
        // Check for potential overflow before multiplication, though long long has a large range.
        // For simplicity here, we'll just do the multiplication.
        // A more robust solution might check if i_val > sqrt(MAX_LONGLONG)
        result = makeInt(i_val * i_val);
    } else if (arg.type == TYPE_REAL) {
        double r_val = arg.r_val;
        result = makeReal(r_val * r_val);
    } else {
        fprintf(stderr, "Runtime error: Sqr expects an Integer or Real argument. Got %s.\n", varTypeToString(arg.type));
        freeValue(&arg); // Free evaluated arg before exit
        EXIT_FAILURE_HANDLER();
    }

    freeValue(&arg); // Free the evaluated argument
    return result;
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
    else if (arg.type == TYPE_BYTE) {
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
        
    } else if (strcasecmp(name, "createtexture") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); /* check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_ct_w"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER);
        Token* p2n = newToken(TOKEN_IDENTIFIER, "_ct_h"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;
        // Set return type for the function
        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok); setTypeAST(retNode, TYPE_INTEGER);
        setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
        
    } else if (strcasecmp(name, "sqr") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }

        AST* param = newASTNode(AST_VAR_DECL, NULL);
        // We can make the dummy param type REAL, as Sqr(Integer) also works
        // and the C implementation will handle both INTEGER and REAL inputs.
        setTypeAST(param, TYPE_REAL); // Or TYPE_INTEGER, C code handles either
        Token* paramNameToken = newToken(TOKEN_IDENTIFIER, "_sqr_arg");
        AST* varNode = newASTNode(AST_VARIABLE, paramNameToken);
        freeToken(paramNameToken);
        addChild(param, varNode);
        dummy->children[0] = param;
        dummy->child_count = 1;

        // Return type depends on argument type.
        // The dummy AST's var_type here is a hint; actual eval handles it.
        // Let's set it to REAL as that's a common use.
        // Alternatively, set to TYPE_VOID and let eval determine precise return.
        // For functions, it's better to be more specific if possible.
        // Since it can return Int or Real, let's mark it as Real for the dummy.
        Token* retTok = newToken(TOKEN_IDENTIFIER, "real"); // Or "integer" - depends on convention
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL); // Or TYPE_INTEGER
        setRight(dummy, retNode);
        dummy->var_type = TYPE_REAL; // Or TYPE_INTEGER. The C impl will return correct type.
    } else if (strcasecmp(name, "destroytexture") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); /* check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_dt_id"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
        dummy->child_count = 1; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "updatetexture") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); /* check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); // TextureID
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_ut_id"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_ARRAY); // PixelData (ARRAY OF Byte)
        // We don't specify element type here in dummy AST; runtime will check.
        // Or you could create a dummy AST_ARRAY_TYPE node for p2->right if needed for stricter parsing.
        Token* p2n = newToken(TOKEN_IDENTIFIER, "_ut_data"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "rendercopy") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); /* check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_rc_id"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
        dummy->child_count = 1; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "rendercopyrect") == 0) {
        dummy->child_capacity = 5; dummy->children = malloc(sizeof(AST*) * 5); /* check */
        const char* pnames[] = {"_rcr_id", "_rcr_dx", "_rcr_dy", "_rcr_dw", "_rcr_dh"};
        for(int i=0; i<5; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER);
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 5; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "fillcircle") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); /* check */
        const char* pnames[] = {"_fc_cx", "_fc_cy", "_fc_r"};
        for(int i=0; i<3; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER);
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 3; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "setrgbcolor") == 0) { // New built-in
        dummy->child_capacity = 3;
        dummy->children = malloc(sizeof(AST*) * 3);
        if (!dummy->children) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }

        // Param 1: R (Byte or Integer)
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_BYTE); // Or TYPE_INTEGER
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_rgb_r"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1);
        dummy->children[0] = p1;

        // Param 2: G (Byte or Integer)
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_BYTE); // Or TYPE_INTEGER
        Token* p2n = newToken(TOKEN_IDENTIFIER, "_rgb_g"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2);
        dummy->children[1] = p2;

        // Param 3: B (Byte or Integer)
        AST* p3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3, TYPE_BYTE); // Or TYPE_INTEGER
        Token* p3n = newToken(TOKEN_IDENTIFIER, "_rgb_b"); AST* v3 = newASTNode(AST_VARIABLE, p3n); freeToken(p3n); addChild(p3,v3);
        dummy->children[2] = p3;

        dummy->child_count = 3;
        dummy->var_type = TYPE_VOID; // It's a procedure
    } else if (strcasecmp(name, "drawline") == 0) {
        dummy->child_capacity = 4; dummy->children = malloc(sizeof(AST*) * 4); /* check */
        const char* pnames[] = {"_dl_x1", "_dl_y1", "_dl_x2", "_dl_y2"};
        for(int i=0; i<4; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER);
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 4; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "fillrect") == 0) {
        dummy->child_capacity = 4; dummy->children = malloc(sizeof(AST*) * 4); /* check */
        const char* pnames[] = {"_fr_x1", "_fr_y1", "_fr_x2", "_fr_y2"}; // or x,y,w,h depending on your preference
        for(int i=0; i<4; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER);
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 4; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "drawcircle") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); /* check */
        const char* pnames[] = {"_dc_cx", "_dc_cy", "_dc_r"};
        for(int i=0; i<3; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER);
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 3; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "inittextsystem") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); /* check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_its_font"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1);
        dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER);
        Token* p2n = newToken(TOKEN_IDENTIFIER, "_its_size"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2);
        dummy->children[1] = p2;
        dummy->child_count = 2; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "outtextxy") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); /* check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_ot_x"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1);
        dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER);
        Token* p2n = newToken(TOKEN_IDENTIFIER, "_ot_y"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2);
        dummy->children[1] = p2;
        AST* p3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3, TYPE_STRING);
        Token* p3n = newToken(TOKEN_IDENTIFIER, "_ot_s"); AST* v3 = newASTNode(AST_VARIABLE, p3n); freeToken(p3n); addChild(p3,v3);
        dummy->children[2] = p3;
        dummy->child_count = 3; dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "getmousestate") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); /* check */
        const char* pnames[] = {"_gms_x", "_gms_y", "_gms_b"};
        for(int i=0; i<3; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER); p->by_ref = 1; // VAR parameters
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 3; dummy->var_type = TYPE_VOID;
    } else if (strcmp(name, "textcolore") == 0 || strcmp(name, "textbackgrounde") == 0 ) {
        // Param: byte/integer
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST *)); if (!dummy->children) { /* Error */ }
        AST *p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_BYTE); // Expect byte
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_tc_color"); AST *v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;
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
 } else if (strcasecmp(name, "mstreamcreate") == 0) {
     Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "mstream"); // Use the type name recognized by the parser
     AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken);
     freeToken(retTypeNameToken); // Free temp token
     setTypeAST(retTypeNode, TYPE_MEMORYSTREAM); // Set the VarType enum
     setRight(dummy, retTypeNode); // Link the type node as the return type
     dummy->var_type = TYPE_MEMORYSTREAM; // Set the function node's return type hint
 } else if (strcasecmp(name, "initsoundsystem") == 0) { // Procedure, 0 args
     dummy->child_count = 0;
     dummy->var_type = TYPE_VOID;
 } else if (strcasecmp(name, "loadsound") == 0) { // Function, 1 string arg
     dummy->child_capacity = 1;
     dummy->children = malloc(sizeof(AST*));
     if (!dummy->children) { /* check malloc */ EXIT_FAILURE_HANDLER(); }
     AST* p1 = newASTNode(AST_VAR_DECL, NULL); // Dummy VAR_DECL for the parameter
     setTypeAST(p1, TYPE_STRING); // Parameter type is String
     Token* p1n = newToken(TOKEN_IDENTIFIER, "_ls_filename"); // Dummy parameter name
     AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); // Dummy variable node
     addChild(p1, v1); // Add dummy var to dummy VAR_DECL
     dummy->children[0] = p1; // Add dummy VAR_DECL to dummy procedure node's children
     dummy->child_count = 1; // <<< Set child_count to 1
     // Set return type for the function (already exists for loadsound)

 } else if (strcasecmp(name, "playsound") == 0) { // Procedure, 1 integer arg
     dummy->child_capacity = 1;
     dummy->children = malloc(sizeof(AST*));
      if (!dummy->children) { /* check malloc */ EXIT_FAILURE_HANDLER(); }
     AST* p1 = newASTNode(AST_VAR_DECL, NULL); // Dummy VAR_DECL
     setTypeAST(p1, TYPE_INTEGER); // Parameter type is Integer (SoundID)
     Token* p1n = newToken(TOKEN_IDENTIFIER, "_ps_soundid"); // Dummy parameter name
     AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n);
     addChild(p1, v1);
     dummy->children[0] = p1;
     dummy->child_count = 1; // <<< Set child_count to 1
     dummy->var_type = TYPE_VOID; // It's a procedure

 } else if (strcasecmp(name, "quitsoundsystem") == 0) { // Procedure, 0 args
     dummy->child_count = 0;
     dummy->var_type = TYPE_VOID;
 } else if (strcasecmp(name, "freesound") == 0) { // Procedure, 1 integer arg
     dummy->child_capacity = 1;
     dummy->children = malloc(sizeof(AST*));
      if (!dummy->children) { /* check malloc */ EXIT_FAILURE_HANDLER(); }
     AST* p1 = newASTNode(AST_VAR_DECL, NULL); // Dummy VAR_DECL
     setTypeAST(p1, TYPE_INTEGER); // Parameter type is Integer (SoundID)
     Token* p1n = newToken(TOKEN_IDENTIFIER, "_fs_soundid"); // Dummy parameter name
     AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n);
     addChild(p1, v1);
     dummy->children[0] = p1;
     dummy->child_count = 1; // <<< Set child_count to 1
     dummy->var_type = TYPE_VOID; // It's a procedure
 } else if (strcasecmp(name, "issoundplaying") == 0) { // Function, 0 args, returns Boolean
     dummy->child_count = 0; // Explicitly confirm 0 args (default is already 0)

     // Set return type for the function (Boolean)
     Token* retTypeNameToken = newToken(TOKEN_IDENTIFIER, "boolean"); // Create a temporary token for the type name "boolean"
     AST *retTypeNode = newASTNode(AST_VARIABLE, retTypeNameToken); // Create a dummy AST node representing the Boolean type
     freeToken(retTypeNameToken); // Free the temporary token after newASTNode copies it

     setTypeAST(retTypeNode, TYPE_BOOLEAN); // Set the VarType enum for the return type node

     setRight(dummy, retTypeNode); // Link the return type node to the dummy function declaration node's 'right' child
     dummy->var_type = TYPE_BOOLEAN; // Set the function declaration node's VarType hint to Boolean

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
    
    freeAST(dummy); // This will free the dummy node and its tree
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
    if (argNode->type != AST_VARIABLE || !argNode->token) { // Check token too
         fprintf(stderr, "Runtime error: Low argument must be a valid type identifier. Got AST type %s\n", astTypeToString(argNode->type));
         EXIT_FAILURE_HANDLER();
    }

    const char* typeName = argNode->token->value; // Get the type name string

    // --- ADDED: Handle Built-in Types Directly ---
    if (strcasecmp(typeName, "integer") == 0 || strcasecmp(typeName, "longint") == 0 || strcasecmp(typeName, "cardinal") == 0) {
        // Assuming 32-bit signed integer minimum for simplicity, adjust if needed
        return makeInt(-2147483648); // Or MIN_INT if defined, or 0 for Cardinal? TP used 0 for cardinal Low. Let's use 0 for Cardinal.
        // For simplicity let's return 0 for now as MIN_INT isn't defined
        // return makeInt(0);
    } else if (strcasecmp(typeName, "char") == 0) {
        return makeChar((char)0); // Low(Char) is ASCII 0
    } else if (strcasecmp(typeName, "boolean") == 0) {
        return makeBoolean(0); // Low(Boolean) is False (ordinal 0)
    } else if (strcasecmp(typeName, "byte") == 0) {
        return makeInt(0); // Low(Byte) is 0
    } else if (strcasecmp(typeName, "word") == 0) {
        return makeInt(0); // Low(Word) is 0
    }
    // --- END ADDED ---

    // --- If not a built-in, assume user-defined type and lookup ---
    AST* typeDef = lookupType(typeName);

    if (!typeDef) {
        // Check again if it *looks* like a basic type that wasn't handled above (shouldn't happen now)
        fprintf(stderr, "Runtime error: Type '%s' not found or not an ordinal type in Low().\n", typeName);
        EXIT_FAILURE_HANDLER();
    }

    // Resolve type reference if necessary
    if (typeDef->type == AST_TYPE_REFERENCE) {
        typeDef = typeDef->right; // Assuming right points to the actual definition
         if (!typeDef) {
              fprintf(stderr, "Runtime error: Could not resolve type reference '%s' in Low().\n", typeName);
              EXIT_FAILURE_HANDLER();
         }
    }

    // We have the type definition AST node (typeDef)
    VarType actualType = typeDef->var_type; // Get the VarType from the definition node

    switch (actualType) {
        // Remove cases handled above (Integer, Char, Boolean, Byte, Word)
        case TYPE_ENUM:
        {
            if (typeDef->type != AST_ENUM_TYPE) {
                 fprintf(stderr, "Runtime error: Type definition for '%s' is not an Enum type for Low().\n", typeName);
                 EXIT_FAILURE_HANDLER();
            }
            // Lowest ordinal is 0
            const char* enumTypeName = typeDef->token ? typeDef->token->value : typeName; // Use original name if possible
            Value lowEnum = makeEnum(enumTypeName, 0);
            // Free the value returned by makeEnum before returning a copy or reassigning
            Value result = makeCopyOfValue(&lowEnum);
            freeValue(&lowEnum); // Free the temporary enum created by makeEnum
            return result; // Return the copy
        }
        // Keep default for unsupported types
        default:
            fprintf(stderr, "Runtime error: Low() not supported for user-defined type %s ('%s').\n", varTypeToString(actualType), typeName);
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
     if (argNode->type != AST_VARIABLE || !argNode->token) { // Check token too
         fprintf(stderr, "Runtime error: High argument must be a valid type identifier. Got AST type %s\n", astTypeToString(argNode->type));
         EXIT_FAILURE_HANDLER();
    }

    const char* typeName = argNode->token->value;

    // --- ADDED: Handle Built-in Types Directly ---
    if (strcasecmp(typeName, "integer") == 0 || strcasecmp(typeName, "longint") == 0) {
        // Assuming 32-bit signed integer maximum for simplicity, adjust if needed
        return makeInt(2147483647); // Or MAX_INT if defined
    } else if (strcasecmp(typeName, "cardinal") == 0) {
        // Assuming 32-bit unsigned integer maximum for simplicity
        return makeInt(4294967295); // Or MAX_CARDINAL if defined
    } else if (strcasecmp(typeName, "char") == 0) {
        return makeChar((char)255); // High(Char) is ASCII 255 (assuming 8-bit char)
    } else if (strcasecmp(typeName, "boolean") == 0) {
        return makeBoolean(1); // High(Boolean) is True (ordinal 1)
    } else if (strcasecmp(typeName, "byte") == 0) {
        return makeInt(255); // High(Byte) is 255
    } else if (strcasecmp(typeName, "word") == 0) {
        return makeInt(65535); // High(Word) is 65535
    }
    // --- END ADDED ---

    // --- If not a built-in, assume user-defined type and lookup ---
    AST* typeDef = lookupType(typeName);

    if (!typeDef) {
        fprintf(stderr, "Runtime error: Type '%s' not found or not an ordinal type in High().\n", typeName);
        EXIT_FAILURE_HANDLER();
    }

    // Resolve type reference if necessary
    if (typeDef->type == AST_TYPE_REFERENCE) {
        typeDef = typeDef->right; // Assuming right points to the actual definition
         if (!typeDef) {
              fprintf(stderr, "Runtime error: Could not resolve type reference '%s' in High().\n", typeName);
              EXIT_FAILURE_HANDLER();
         }
    }

    VarType actualType = typeDef->var_type;

    switch (actualType) {
        // Remove cases handled above (Integer, Char, Boolean, Byte, Word)
        case TYPE_ENUM:
        {
            if (typeDef->type != AST_ENUM_TYPE) {
                fprintf(stderr, "Runtime error: Type definition for '%s' is not an Enum type for High().\n", typeName);
                EXIT_FAILURE_HANDLER();
            }
            // Highest ordinal is number of members - 1
            int highOrdinal = typeDef->child_count - 1;
            if (highOrdinal < 0) highOrdinal = 0; // Handle empty enum?

            const char* enumTypeName = typeDef->token ? typeDef->token->value : typeName;
            Value highEnum = makeEnum(enumTypeName, highOrdinal);
            // Free the value returned by makeEnum before returning a copy or reassigning
            Value result = makeCopyOfValue(&highEnum);
            freeValue(&highEnum); // Free the temporary enum created by makeEnum
            return result; // Return the copy
        }
        // Keep default for unsupported types
        default:
            fprintf(stderr, "Runtime error: High() not supported for user-defined type %s ('%s').\n", varTypeToString(actualType), typeName);
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

    Value argVal = eval(node->children[0]); // Evaluate the argument
    long long currentOrdinal;
    long long maxOrdinal = -1;
    bool checkMax = false;
    Value result = makeVoid();
    VarType effectiveType = argVal.type; // Start with the evaluated type

    // --- ADDED: Check for single-char string and treat as CHAR ---
    if (argVal.type == TYPE_STRING && argVal.s_val != NULL && strlen(argVal.s_val) == 1) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG Succ] Treating single-char string '%s' as CHAR.\n", argVal.s_val);
        #endif
        effectiveType = TYPE_CHAR; // Change effective type for the switch
        currentOrdinal = argVal.s_val[0]; // Get ordinal from the single char
    }
    // --- END ADDED CHECK ---

    // Use effectiveType in the switch, handle original argVal types inside
    switch (effectiveType) {
        case TYPE_INTEGER:
            currentOrdinal = argVal.i_val;
            result = makeInt(currentOrdinal + 1);
            break;
        case TYPE_CHAR: // Now handles actual TYPE_CHAR and single-char TYPE_STRING
             // If it was originally a string, currentOrdinal was set above.
             // If it was originally TYPE_CHAR, set currentOrdinal now.
             if (argVal.type == TYPE_CHAR) {
                 currentOrdinal = argVal.c_val;
             }
            maxOrdinal = 255;
            checkMax = true;
            if (currentOrdinal >= maxOrdinal && checkMax) { goto succ_overflow; }
            result = makeChar((char)(currentOrdinal + 1));
            break;
        case TYPE_BOOLEAN:
            currentOrdinal = argVal.i_val;
            maxOrdinal = 1;
            checkMax = true;
            if (currentOrdinal >= maxOrdinal && checkMax) { goto succ_overflow; }
            result = makeBoolean((int)(currentOrdinal + 1));
            break;
        case TYPE_ENUM:
            { // Keep existing enum logic
                 currentOrdinal = argVal.enum_val.ordinal;
                 AST* typeDef = lookupType(argVal.enum_val.enum_name);
                 if (!typeDef || (typeDef->type == AST_TYPE_REFERENCE && !(typeDef = typeDef->right))) {
                     fprintf(stderr, "Runtime warning: Cannot determine enum definition for Succ() bounds check on type '%s'.\n", argVal.enum_val.enum_name ? argVal.enum_val.enum_name : "?");
                     checkMax = false;
                 } else if (typeDef->type == AST_ENUM_TYPE) {
                      maxOrdinal = typeDef->child_count - 1;
                      checkMax = true;
                 } else {
                      fprintf(stderr, "Runtime warning: Invalid type definition found for enum '%s' during Succ().\n", argVal.enum_val.enum_name ? argVal.enum_val.enum_name : "?");
                      checkMax = false;
                 }
                 if (currentOrdinal >= maxOrdinal && checkMax) { goto succ_overflow; }
                 Value nextEnum = makeEnum(argVal.enum_val.enum_name, (int)(currentOrdinal + 1));
                 result = makeCopyOfValue(&nextEnum);
                 freeValue(&nextEnum);
            }
            break;
         case TYPE_BYTE:
             currentOrdinal = argVal.i_val;
             maxOrdinal = 255; checkMax = true;
             if (currentOrdinal >= maxOrdinal && checkMax) { goto succ_overflow; }
             result = makeInt(currentOrdinal + 1); result.type = TYPE_BYTE;
             break;
         case TYPE_WORD:
             currentOrdinal = argVal.i_val;
             maxOrdinal = 65535; checkMax = true;
             if (currentOrdinal >= maxOrdinal && checkMax) { goto succ_overflow; }
             result = makeInt(currentOrdinal + 1); result.type = TYPE_WORD;
             break;
        // *** REMOVED STRING case if added previously ***
        // case TYPE_STRING: // This case should no longer be needed here
        //     break;
        default: // Handles types that are not ordinal *after* the string check
            fprintf(stderr, "Runtime error: Succ() requires an ordinal type argument. Got %s.\n", varTypeToString(argVal.type)); // Use original argVal.type for error msg
            freeValue(&argVal);
            EXIT_FAILURE_HANDLER();
    }

    freeValue(&argVal);
    return result;

succ_overflow:
    fprintf(stderr, "Runtime error: Succ argument out of range (Overflow on type %s).\n", varTypeToString(argVal.type)); // Use original argVal.type
    freeValue(&argVal);
    EXIT_FAILURE_HANDLER();
    return makeVoid();
}

BuiltinRoutineType getBuiltinType(const char *name) {
    // List known FUNCTIONS (return values) - case-insensitive compare
    const char *functions[] = {
        "paramcount", "paramstr", "length", "pos", "ord", "chr",
        "abs", "sqrt", "cos", "sin", "tan", "ln", "exp", "trunc",
        "random", "wherex", "wherey", "ioresult", "eof", "copy",
        "upcase", "low", "high", "succ", "pred", // Added Pred assuming it might exist
        "inttostr", "api_send", "api_receive", "screencols", "screenrows",
        "keypressed", "mstreamcreate", "quitrequested", "loadsound",
        
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
         "randomize", "mstreamfree", "textcolore", "textbackgrounde",
        "initsoundsystem", "playsound", "quitsoundsystem","issoundplaying"
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

Value executeBuiltinTextColorE(AST *node) {
    fflush(stderr); // Flush immediately
    if (node->child_count != 1) { /* Error handling */ EXIT_FAILURE_HANDLER(); }
    Value colorVal = eval(node->children[0]);
    if (colorVal.type != TYPE_INTEGER && colorVal.type != TYPE_BYTE) { /* Error handling */ freeValue(&colorVal); EXIT_FAILURE_HANDLER(); }
    long long colorCode = colorVal.i_val;
    freeValue(&colorVal);

    // Store 0-255 index
    gCurrentTextColor = (colorCode >= 0 && colorCode <= 255) ? (int)colorCode : 7; // Default to 7 if out of range
    gCurrentTextBold = false; // Extended colors don't usually use the bold flag for intensity
    gCurrentColorIsExt = true; // Mark as extended 256-color mode

    // DO NOT PRINT ANYTHING HERE
    return makeVoid();
}

Value executeBuiltinTextBackgroundE(AST *node) {
    if (node->child_count != 1) { /* Error handling */ EXIT_FAILURE_HANDLER(); }
    Value colorVal = eval(node->children[0]);
    if (colorVal.type != TYPE_INTEGER && colorVal.type != TYPE_BYTE) { /* Error handling */ freeValue(&colorVal); EXIT_FAILURE_HANDLER(); }
    long long colorCode = colorVal.i_val;
    freeValue(&colorVal);

    // Store 0-255 index
    gCurrentTextBackground = (colorCode >= 0 && colorCode <= 255) ? (int)colorCode : 0; // Default to 0 if out of range
    gCurrentBgIsExt = true; // Mark as extended 256-color mode

    // DO NOT PRINT ANYTHING HERE
    return makeVoid();
}

Value executeBuiltinTextColor(AST *node) {
    if (node->child_count != 1) { /* Error handling */ EXIT_FAILURE_HANDLER(); }
    Value colorVal = eval(node->children[0]);
    if (colorVal.type != TYPE_INTEGER && colorVal.type != TYPE_BYTE) { /* Error handling */ freeValue(&colorVal); EXIT_FAILURE_HANDLER(); }
    long long colorCode = colorVal.i_val;
    freeValue(&colorVal);

    gCurrentTextColor = (int)(colorCode % 16); // Store 0-15 index
    gCurrentTextBold = (colorCode >= 8 && colorCode <= 15); // Set bold for high-intensity 8-15
    gCurrentColorIsExt = false; // Mark as standard 16-color mode

    // DO NOT PRINT ANYTHING HERE
    return makeVoid();
}

// --- MODIFIED TextBackground ---
Value executeBuiltinTextBackground(AST *node) {
    if (node->child_count != 1) { /* Error handling */ EXIT_FAILURE_HANDLER(); }
    Value colorVal = eval(node->children[0]);
    if (colorVal.type != TYPE_INTEGER && colorVal.type != TYPE_BYTE) { /* Error handling */ freeValue(&colorVal); EXIT_FAILURE_HANDLER(); }
    long long colorCode = colorVal.i_val;
    freeValue(&colorVal);

    gCurrentTextBackground = (int)(colorCode % 8); // Store 0-7 index (standard BG range)
    gCurrentBgIsExt = false; // Mark as standard 16-color mode (only 8 for BG used)

    // DO NOT PRINT ANYTHING HERE
    return makeVoid();
}

// --- Implementation for new(pointer_variable) ---
// Pascal: procedure new(var P: Pointer);
// Allocates memory for the variable pointed to by P and makes P point to it.
// The type of the allocated memory is determined by the base type of P.
Value executeBuiltinNew(AST *node) {
    // Check that exactly one argument was provided.
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: new() expects exactly one argument (a pointer variable).\n");
        EXIT_FAILURE_HANDLER();
    }

    // The argument must be an L-value (a variable, field, or array element).
    // It should resolve to a pointer to the Value struct of the pointer variable itself.
    AST *lvalueNode = node->children[0];
    // resolveLValueToPtr finds the memory location of the pointer variable's Value struct.
    Value *pointerVarValuePtr = resolveLValueToPtr(lvalueNode);

    // Check if the lvalue resolved successfully.
    if (!pointerVarValuePtr) {
        fprintf(stderr, "Runtime error: Argument to new() could not be resolved to a memory location.\n");
        EXIT_FAILURE_HANDLER();
    }

    // Check if the variable resolved by lvalue is indeed of pointer type.
    if (pointerVarValuePtr->type != TYPE_POINTER) {
        fprintf(stderr, "Runtime error: Argument to new() must be of pointer type. Got %s.\n", varTypeToString(pointerVarValuePtr->type));
        EXIT_FAILURE_HANDLER();
    }

    // Get the base type information from the pointer variable's Value struct.
    // This link (base_type_node) was set when the pointer variable was declared and initialized (makeValueForType).
    AST *baseTypeNode = pointerVarValuePtr->base_type_node;

    // Check if the base type information is available. Without it, we don't know what to allocate/initialize.
    if (!baseTypeNode) {
        fprintf(stderr, "Runtime error: Cannot determine base type for pointer variable in new(). Missing type definition link.\n");
        EXIT_FAILURE_HANDLER();
    }

    // Determine the actual VarType enum and the AST node definition of the base type being pointed to.
    // This involves potentially resolving type references.
    VarType baseVarType = TYPE_VOID;
    AST* actualBaseTypeDef = baseTypeNode; // Start with the node linked by the pointer variable.

    // Logic to get baseVarType and actualBaseTypeDef (copied from previous implementation)
    if (actualBaseTypeDef->type == AST_VARIABLE && actualBaseTypeDef->token) {
        const char* typeName = actualBaseTypeDef->token->value;
        // Check against built-in types first.
        if (strcasecmp(typeName, "integer")==0) { baseVarType=TYPE_INTEGER; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "real")==0) { baseVarType=TYPE_REAL; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "char")==0) { baseVarType=TYPE_CHAR; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "string")==0) { baseVarType=TYPE_STRING; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "boolean")==0) { baseVarType=TYPE_BOOLEAN; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "byte")==0) { baseVarType=TYPE_BYTE; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "word")==0) { baseVarType=TYPE_WORD; actualBaseTypeDef = NULL; }
        else {
            // If not a built-in type name, look it up in the type table.
            AST* lookedUpType = lookupType(typeName); // Assumes lookupType is defined (parser.h/parser.c)
            if (!lookedUpType) {
                 fprintf(stderr, "Runtime error: Cannot resolve base type identifier '%s' during new(). Type not found.\n", typeName);
                 EXIT_FAILURE_HANDLER();
            }
            actualBaseTypeDef = lookedUpType; // Use the looked-up type definition node.
            baseVarType = actualBaseTypeDef->var_type; // Get the VarType from the definition.
        }
    } else {
         // If the base type node is not a TYPE_REFERENCE or simple VARIABLE node,
         // assume its var_type is already set (e.g., for anonymous records/arrays).
         baseVarType = actualBaseTypeDef->var_type;
    }

    // Final check to ensure a valid base type was determined.
    if (baseVarType == TYPE_VOID) {
        fprintf(stderr, "Runtime error: Cannot determine valid base type VarType during new(). AST Node type was %s\n",
                actualBaseTypeDef ? astTypeToString(actualBaseTypeDef->type) : (baseTypeNode ? astTypeToString(baseTypeNode->type) : "NULL")); // Assumes astTypeToString is defined.
        EXIT_FAILURE_HANDLER();
    }

    // --- Allocate memory for the new Value structure on the heap ---
    // This is the memory block that the pointer variable will point TO.
    // We allocate a Value struct because this is our standard container for runtime data.
    Value *allocated_memory = malloc(sizeof(Value)); // Use malloc to allocate on the heap.
    if (!allocated_memory) {
        fprintf(stderr, "Memory allocation failed for new value structure in new().\n");
        EXIT_FAILURE_HANDLER();
    }

    // --- Initialize the allocated memory based on the base type ---
    // Initialize the Value structure located at the allocated_memory address with default values for its type.
    // makeValueForType creates a Value and initializes its contents (e.g., NULL string, empty record).
    *(allocated_memory) = makeValueForType(baseVarType, actualBaseTypeDef); // Use assignment to copy the returned Value.

    #ifdef DEBUG // Debug print to confirm allocation and initialization.
    fprintf(stderr, "[DEBUG new] Allocated memory for pointed-to Value* at %p for base type %s. Initialized content.\n",
            (void*)allocated_memory, varTypeToString(baseVarType)); // Assumes varTypeToString is defined.
    fflush(stderr);
    #endif

    // --- Create a Value struct representing the pointer TO this newly allocated memory ---
    // This is the source Value that we will assign TO the pointer variable (lvalueNode).
    // Use the makePointer function to create this Value.
    // Pass the address of the allocated memory and the base type definition node link.
    Value pointerValueToAssign = makePointer(allocated_memory, baseTypeNode); // <<< Use makePointer (defined in utils.c)

    #ifdef DEBUG // Debug print to show the pointer Value being assigned.
    fprintf(stderr, "[DEBUG NEW] Created Value struct to assign to pointer variable: type=%s, ptr_val=%p, base_type_node=%p\n",
            varTypeToString(pointerValueToAssign.type), (void*)pointerValueToAssign.ptr_val, (void*)pointerValueToAssign.base_type_node);
    fflush(stderr);
    #endif


    // --- Update the pointer variable (lvalueNode) to hold this new pointer value ---
    // Use the standard assignment helper function assignValueToLValue.
    // assignValueToLValue will handle freeing the pointer variable's old Value contents (if necessary, though for TYPE_POINTER it likely doesn't free the pointed-to memory)
    // and copying the new pointer value (ptr_val and base_type_node) into the pointer variable's Value struct.
    assignValueToLValue(lvalueNode, pointerValueToAssign); // <<< Use assignValueToLValue (defined in builtin.c or interpreter.h)

    // --- The temporary Value struct 'pointerValueToAssign' is on the stack ---
    // Its contents (ptr_val, base_type_node) are copied into the symbol's Value by assignValueToLValue.
    // No dynamic memory owned *by* pointerValueToAssign needs freeing here; it's just a stack variable holding pointers/data by value.
    // freeValue(&pointerValueToAssign); // This call is NOT needed for a simple pointer Value struct on the stack.


    return makeVoid(); // new() is a procedure, it does not return a value on the Pascal stack.
}

// --- Implementation for dispose(pointer_variable) ---
Value executeBuiltinDispose(AST *node) {
    // ... (argument checking as before) ...

    AST *lvalueNode = node->children[0];
    Value *pointerVarValuePtr = resolveLValueToPtr(lvalueNode);
    // ... (checks for pointerVarValuePtr and its type) ...

    Value *valueToDispose = pointerVarValuePtr->ptr_val;

    if (valueToDispose == NULL) {
        #ifdef DEBUG
        fprintf(stderr, "[DEBUG DISPOSE] Attempted to dispose a nil pointer. Doing nothing.\n");
        #endif
        return makeVoid();
    }

    #ifdef DEBUG
    fprintf(stderr, "[DEBUG DISPOSE] Disposing Value* at address %p (pointed to by variable '%s').\n",
            (void*)valueToDispose, lvalueNode->token ? lvalueNode->token->value : "?");
    #endif

    // Store the address VALUE as an integer type BEFORE freeing
     uintptr_t disposedAddrValue = (uintptr_t)valueToDispose;

     // Free the pointed-to Value struct and its contents
     freeValue(valueToDispose); // Free contents (strings, records, etc.)
     free(valueToDispose);      // Free the Value struct itself

     // Set the original pointer variable back to nil
     pointerVarValuePtr->ptr_val = NULL;

     // --- Nullify Aliases using the stored integer address value ---
     #ifdef DEBUG
     // Use the stored integer address value for printing (using %lx for hex representation)
     fprintf(stderr, "[DEBUG DISPOSE] Nullifying aliases pointing to address 0x%lx.\n", disposedAddrValue);
     #endif

     // Pass the integer address value to the (renamed) helper functions
     nullifyPointerAliasesByAddrValue(globalSymbols, disposedAddrValue);
     nullifyPointerAliasesByAddrValue(localSymbols, disposedAddrValue);
    
    return makeVoid();
}
