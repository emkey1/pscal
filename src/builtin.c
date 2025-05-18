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
    {"api_receive", executeBuiltinAPIReceive},
    {"api_send",  executeBuiltinAPISend},
    {"assign",    executeBuiltinAssign},
    {"chr",       executeBuiltinChr},
    {"cleardevice", executeBuiltinClearDevice},
    {"close",     executeBuiltinClose},
    {"closegraph", executeBuiltinCloseGraph},
    {"copy",      executeBuiltinCopy},
    {"cos",       executeBuiltinCos},
    {"createtargettexture", executeBuiltinCreateTargetTexture},
    {"createtexture", executeBuiltinCreateTexture},
    {"dec",       executeBuiltinDec},         // Include Dec
    {"delay",     executeBuiltinDelay},
    {"destroytexture", executeBuiltinDestroyTexture},
    {"dispose",   executeBuiltinDispose},
    {"drawcircle", executeBuiltinDrawCircle},
    {"drawline", executeBuiltinDrawLine},
    {"drawpolygon", executeBuiltinDrawPolygon},
    {"drawrect",  executeBuiltinDrawRect},
    {"eof",       executeBuiltinEOF},
    {"exp",       executeBuiltinExp},
    {"fillcircle", executeBuiltinFillCircle},
    {"fillrect", executeBuiltinFillRect},
    {"getmaxx",   executeBuiltinGetMaxX},
    {"getmaxy",   executeBuiltinGetMaxY},
    {"getmousestate", executeBuiltinGetMouseState},
    {"getpixelcolor", executeBuiltinGetPixelColor},
    {"gettextsize", executeBuiltinGetTextSize},
    {"getticks", executeBuiltinGetTicks},
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
    {"loadimagetotexture", executeBuiltinLoadImageToTexture},
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
    {"real",      executeBuiltinReal},
    {"realtostr", executeBuiltinRealToStr},
    {"rendercopy", executeBuiltinRenderCopy},
    {"rendercopyex", executeBuiltinRenderCopyEx},
    {"rendercopyrect", executeBuiltinRenderCopyRect},
    {"reset",     executeBuiltinReset},
    // {"result",    executeBuiltinResult}, // 'result' is special, handled differently? Let's assume not dispatched here.
    {"rewrite",   executeBuiltinRewrite},
    {"round",     executeBuiltinRound},
    {"screencols", executeBuiltinScreenCols},
    {"screenrows", executeBuiltinScreenRows},
    {"setalphablend", executeBuiltinSetAlphaBlend},
    {"setcolor",  executeBuiltinSetColor},
    {"setrendertarget",  executeBuiltinSetRenderTarget},
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

Value executeBuiltinRound(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Round expects 1 argument (Real).\n");
        EXIT_FAILURE_HANDLER();
    }

    Value arg = eval(node->children[0]);
    Value result;

    if (arg.type == TYPE_REAL) {
        // C round() returns a double, convert to long long for Pscal Integer
        result = makeInt((long long)round(arg.r_val));
    } else if (arg.type == TYPE_INTEGER || arg.type == TYPE_BYTE || arg.type == TYPE_WORD) {
        // Rounding an integer just returns the integer itself
        result = makeInt(arg.i_val);
    }
    else {
        fprintf(stderr, "Runtime error: Round argument must be a Real or Integer type. Got %s.\n", varTypeToString(arg.type));
        freeValue(&arg); // Free evaluated arg before exit
        result = makeInt(0);
        EXIT_FAILURE_HANDLER();
    }

    freeValue(&arg); // Free the evaluated argument
    return result;
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

    long long value_to_convert = 0; // Use long long to hold the value

    // <<< MODIFICATION START: Accept Byte, Word, Boolean, Char as well as Integer >>>
    switch (arg.type) {
        case TYPE_INTEGER:
        case TYPE_WORD:    // Word is also essentially an integer
        case TYPE_BYTE:    // Byte is also essentially an integer
        case TYPE_BOOLEAN: // Boolean (0 or 1) can be converted
            value_to_convert = arg.i_val;
            break;
        case TYPE_CHAR:    // Convert char to its ordinal value
            value_to_convert = (long long)arg.c_val;
            break;
        default:
            fprintf(stderr, "Runtime error: IntToStr expects an integer-compatible argument (Integer, Byte, Word, Boolean, Char). Got %s.\n",
                    varTypeToString(arg.type));
            freeValue(&arg); // Free the evaluated argument before exiting
            EXIT_FAILURE_HANDLER();
    }
    // <<< MODIFICATION END >>>

    // Dynamic Allocation for the string buffer
    int required_size = snprintf(NULL, 0, "%lld", value_to_convert);
    if (required_size < 0) {
         fprintf(stderr, "Runtime error: snprintf failed to determine size in IntToStr.\n");
         freeValue(&arg);
         return makeString(""); // Return empty string on error
    }

    char *buffer = malloc(required_size + 1);
    if (!buffer) {
         fprintf(stderr, "Runtime error: Memory allocation failed for buffer in IntToStr.\n");
         freeValue(&arg);
         return makeString("");
    }

    int chars_written = snprintf(buffer, required_size + 1, "%lld", value_to_convert);

    if (chars_written < 0 || chars_written >= (required_size + 1)) {
        fprintf(stderr, "Runtime error: Failed to convert integer to string in IntToStr (step 2).\n");
        free(buffer);
        freeValue(&arg);
        return makeString("");
    }

    Value result = makeString(buffer);
    free(buffer);
    freeValue(&arg); // Free the evaluated argument AFTER its value has been used

    return result;
}

Value executeBuiltinReal(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Real() expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    Value arg = eval(node->children[0]);
    Value result = makeInt(0);

    switch (arg.type) {
        case TYPE_INTEGER:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_BOOLEAN: // Booleans (0 or 1) can be promoted to Real (0.0 or 1.0)
            result = makeReal((double)arg.i_val);
            break;
        case TYPE_CHAR:
            result = makeReal((double)arg.c_val); // Use ASCII value
            break;
        case TYPE_REAL:
            result = makeCopyOfValue(&arg); // Already Real, just return a copy
            break;
        default:
            fprintf(stderr, "Runtime error: Real() argument must be an Integer, Ordinal, or Real type. Got %s.\n", varTypeToString(arg.type));
            freeValue(&arg); // Free evaluated arg before exit
            EXIT_FAILURE_HANDLER();
    }

    freeValue(&arg); // Free the original evaluated argument
    return result;
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

static void configureBuiltinDummyAST(AST *dummy, const char *name) {
    // Ensure dummy is not NULL before proceeding
    if (!dummy) {
        fprintf(stderr, "Error: configureBuiltinDummyAST called with NULL dummy AST node for name: %s\n", name ? name : "NULL_NAME");
        return; // Or EXIT_FAILURE_HANDLER();
    }

    // Note: 'name' here is the original case-sensitive name passed to registerBuiltinFunction.
    // The strcasecmp will handle case-insensitivity.

    // --- Functions Returning REAL ---
    if (strcasecmp(name, "cos") == 0 ||
        strcasecmp(name, "sin") == 0 ||
        strcasecmp(name, "tan") == 0 ||
        strcasecmp(name, "sqrt") == 0 ||
        strcasecmp(name, "ln") == 0 ||
        strcasecmp(name, "exp") == 0 ||
        strcasecmp(name, "real") == 0) { // Added "real" here

        // All these take one numeric argument (can be represented as REAL for dummy) and return REAL.
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_REAL); // Dummy param type
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_param1_real"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "real"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL); setRight(dummy, retNode); dummy->var_type = TYPE_REAL;
    }
    // --- Functions Returning INTEGER ---
    else if (strcasecmp(name, "abs") == 0) { // abs can return INT or REAL, C code handles it. Dummy can be INT.
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_REAL); // Param can be Real or Int
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_abs_arg"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "pos") == 0) {
        dummy->child_capacity = 2;
        dummy->children = malloc(sizeof(AST*) * 2);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_pos_substr"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_STRING); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_pos_str"); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "ioresult") == 0 ||
             strcasecmp(name, "paramcount") == 0 ||
             strcasecmp(name, "getmaxx") == 0 ||
             strcasecmp(name, "getmaxy") == 0 ||
             strcasecmp(name, "screencols") == 0 ||    // Added from previous list
             strcasecmp(name, "screenrows") == 0 ||    // Added
             strcasecmp(name, "ord") == 0 ||           // Added
             strcasecmp(name, "round") == 0 ||         // Added
             strcasecmp(name, "trunc") == 0 ||
             strcasecmp(name, "length") == 0 ||        // Added
             strcasecmp(name, "wherex") == 0 ||        // Added from builtin.c
             strcasecmp(name, "wherey") == 0 ||        // Added from builtin.c
             strcasecmp(name, "createtexture") == 0 || // Added
             strcasecmp(name, "loadsound") == 0 ) {    // Added

        // These functions take 0 or 1 argument (ord, round, trunc, length, loadsound) and return INTEGER.
        // For simplicity, we'll set up a generic single parameter if applicable, or none.
        // The C execution will handle exact parameter requirements.

        if (strcasecmp(name, "ord") == 0 || strcasecmp(name, "round") == 0 || strcasecmp(name, "trunc") == 0 || strcasecmp(name, "length") == 0 || strcasecmp(name, "loadsound") == 0) {
            dummy->child_capacity = 1;
            dummy->children = malloc(sizeof(AST*));
            if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
            AST* p1 = newASTNode(AST_VAR_DECL, NULL);
            // Determine appropriate dummy param type
            if (strcasecmp(name, "length") == 0 || strcasecmp(name, "loadsound") == 0) setTypeAST(p1, TYPE_STRING);
            else if (strcasecmp(name, "round") == 0 || strcasecmp(name, "trunc") == 0) setTypeAST(p1, TYPE_REAL);
            else setTypeAST(p1, TYPE_CHAR); // for ord (can be other ordinals too)

            Token* pn1 = newToken(TOKEN_IDENTIFIER, "_param1"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
            dummy->children[0] = p1; dummy->child_count = 1;
        } else if (strcasecmp(name, "createtexture") == 0) {
             dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
             AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* p1n = newToken(TOKEN_IDENTIFIER, "_ct_w"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
             AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* p2n = newToken(TOKEN_IDENTIFIER, "_ct_h"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2); dummy->children[1] = p2;
             dummy->child_count = 2;
        }
        else { // 0 parameters
            dummy->child_count = 0;
        }

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    // --- Functions Returning CHAR ---
    else if (strcasecmp(name, "upcase") == 0 ||
             strcasecmp(name, "chr") == 0) {    // Added "chr"
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, (strcasecmp(name, "chr") == 0 ? TYPE_INTEGER : TYPE_CHAR) ); // chr takes int, upcase char
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_param1_char_int"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "char"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_CHAR); setRight(dummy, retNode); dummy->var_type = TYPE_CHAR;
    }
    // --- Functions Returning STRING ---
    else if (strcasecmp(name, "paramstr") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_paramstr_idx"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "string"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcasecmp(name, "readkey") == 0) {
        dummy->child_count = 0;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcasecmp(name, "copy") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_cpy_s"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_cpy_idx"); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        AST* p3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3, TYPE_INTEGER); Token* pn3 = newToken(TOKEN_IDENTIFIER, "_cpy_cnt"); AST* v3 = newASTNode(AST_VARIABLE, pn3); freeToken(pn3); addChild(p3,v3); dummy->children[2] = p3;
        dummy->child_count = 3;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcasecmp(name, "inttostr") == 0) { // Added
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_itos_val"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcmp(name, "api_receive") == 0) { // Already had this
        // Param: MStream
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_MEMORYSTREAM); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_apirecv_ms"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "string"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    // --- Functions Returning BOOLEAN ---
    else if (strcasecmp(name, "keypressed") == 0 ||
             strcasecmp(name, "quitrequested") == 0 ||
             strcasecmp(name, "issoundplaying") == 0 || // Added from previous list
             strcasecmp(name, "eof") == 0 ) {           // Added
        dummy->child_count = 0; // keypressed, quitrequested, issoundplaying take 0 args. eof takes 1 file arg.
        if (strcasecmp(name, "eof") == 0) {
            dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
            AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_FILE); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_eof_fvar"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
            dummy->children[0] = p1; dummy->child_count = 1;
        }
        Token* retTok = newToken(TOKEN_IDENTIFIER, "boolean"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_BOOLEAN); setRight(dummy, retNode); dummy->var_type = TYPE_BOOLEAN;
    }
    // --- Functions Returning MEMORYSTREAM ---
    else if (strcasecmp(name, "api_send") == 0) { // Already had this
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_apisend_url"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_STRING); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_apisend_body"); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2; // Body can also be MStream, C code handles
        dummy->child_count = 2;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "mstream"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_MEMORYSTREAM); setRight(dummy, retNode); dummy->var_type = TYPE_MEMORYSTREAM;
    }
    else if (strcasecmp(name, "mstreamcreate") == 0) { // Already had this
        dummy->child_count = 0;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "mstream"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_MEMORYSTREAM); setRight(dummy, retNode); dummy->var_type = TYPE_MEMORYSTREAM;
    }
    // --- Ordinal functions (Low, High, Succ) ---
    // These are special as their return type matches their argument's base type,
    // and their argument is a type identifier rather than a value.
    // For dummy setup, we can make the return type INTEGER as a placeholder.
    // The actual C execution will determine the correct type.
    else if (strcasecmp(name, "low") == 0 || strcasecmp(name, "high") == 0 || strcasecmp(name, "succ") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); // Param is type identifier, dummy type
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_ord_type_arg"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); // Placeholder return type
        AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    // --- Procedures (TYPE_VOID return type) ---
    // Most procedures listed in the original long if/else chain don't strictly need
    // specific dummy AST setup here unless you want to define their parameter list
    // for very early (parser/annotation time) arity checks.
    // If they are AST_PROCEDURE_DECL, dummy->var_type will default to TYPE_VOID
    // from newASTNode, which is correct.
    // Example for a procedure with specific params:
    else if (strcasecmp(name, "getmousestate") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        const char* pnames[] = {"_gms_x", "_gms_y", "_gms_b"};
        for(int i=0; i<3; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER); p->by_ref = 1; // VAR params
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 3;
        dummy->var_type = TYPE_VOID; // Explicitly VOID for procedures
    } else if (strcasecmp(name, "random") == 0) {
        // Random can be called with 0 or 1 (integer) argument.
        // The C execution `executeBuiltinRandom` handles both.
        // For the dummy AST, we can define it as taking an optional integer
        // or simply define the 0-argument version which returns REAL.
        // Let's set up for the 0-argument version primarily for return type.
        dummy->child_count = 0; // For Random()
        // If you wanted to represent the optional Random(N):
        // dummy->child_capacity = 1;
        // dummy->children = malloc(sizeof(AST*));
        // if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        // AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        // Token* pn1 = newToken(TOKEN_IDENTIFIER, "_random_n_arg"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        // dummy->children[0] = p1;
        // dummy->child_count = 1; // Or 0 if primarily defining Random()

        // Return: REAL (for Random() without arguments)
        // If Random(N) is called, executeBuiltinRandom returns INTEGER.
        // The dummy->var_type here helps if annotation needs a hint for the no-arg case.
        Token* retTok = newToken(TOKEN_IDENTIFIER, "real");
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_REAL; // For Random()
    } else if (strcasecmp(name, "sqr") == 0) {
        // Parameter: Integer or Real. C code executeBuiltinSqr handles both.
        // For dummy AST, we can specify REAL as the parameter type.
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, TYPE_REAL); // Dummy parameter type
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_sqr_arg");
        AST* v1 = newASTNode(AST_VARIABLE, pn1);
        freeToken(pn1);
        addChild(p1, v1);
        dummy->children[0] = p1;
        dummy->child_count = 1;

        // Return type: Matches argument type (Integer or Real).
        // For the dummy AST, we can set it to REAL, as Sqr(Integer) can be promoted.
        // The C execution `executeBuiltinSqr` will return the correctly typed Value.
        Token* retTok = newToken(TOKEN_IDENTIFIER, "real");
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_REAL;
    } else if (strcasecmp(name, "loadimagetotexture") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); /* Malloc check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_filePath"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    } else if (strcasecmp(name, "gettextsize") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); /* Malloc check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_text"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); p2->by_ref = 1; Token* pn2 = newToken(TOKEN_IDENTIFIER, "_width"); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        AST* p3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3, TYPE_INTEGER); p3->by_ref = 1; Token* pn3 = newToken(TOKEN_IDENTIFIER, "_height"); AST* v3 = newASTNode(AST_VARIABLE, pn3); freeToken(pn3); addChild(p3,v3); dummy->children[2] = p3;
        dummy->child_count = 3;
        dummy->var_type = TYPE_VOID; // It's a procedure
    } else if (strcasecmp(name, "setrendertarget") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); /* Malloc check */
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_textureID"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;
        dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "drawpolygon") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); /* Malloc check */
        // Represent ARRAY OF PointRecord parameter simply as ARRAY for dummy AST
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_ARRAY); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_points"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_numPoints"); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;
        dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "getpixelcolor") == 0) {
        dummy->child_capacity = 6; dummy->children = malloc(sizeof(AST*) * 6); /* Malloc check */
        const char* pnames[] = {"_x", "_y", "_r", "_g", "_b", "_a"};
        for(int i=0; i<6; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(p, TYPE_INTEGER); // X, Y are Integer
            if (i >= 2) setTypeAST(p, TYPE_BYTE); // R,G,B,A are Byte
            if (i >= 2) p->by_ref = 1; // R,G,B,A are VAR params
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i]); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 6;
        dummy->var_type = TYPE_VOID;
    } else if (strcasecmp(name, "createtargettexture") == 0) { // <<< NEW
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* p1n = newToken(TOKEN_IDENTIFIER, "_ctt_w"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* p2n = newToken(TOKEN_IDENTIFIER, "_ctt_h"); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer"); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    } else if (strcasecmp(name, "setalphablend") == 0) { // <<< NEW
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_BOOLEAN);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_enable"); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1;
        dummy->child_count = 1;
        dummy->var_type = TYPE_VOID; // It's a procedure
    }      else if (strcasecmp(name, "rendercopyex") == 0) {
        dummy->child_capacity = 13;
        dummy->children = malloc(sizeof(AST*) * 13);
        if (!dummy->children) { /* Malloc error */ EXIT_FAILURE_HANDLER(); }

        // Parameters for RenderCopyEx:
        // TextureID: Integer
        // SrcX, SrcY, SrcW, SrcH: Integer (Source Rect)
        // DstX, DstY, DstW, DstH: Integer (Destination Rect)
        // Angle: Real
        // RotationCenterX, RotationCenterY: Integer (Point for rotation center)
        // FlipMode: Integer (SDL_RendererFlip enum: 0=none, 1=horizontal, 2=vertical)
        const char* pnames[] = {
            "_textureID",       // 0
            "_srcX",            // 1
            "_srcY",            // 2
            "_srcW",            // 3
            "_srcH",            // 4
            "_dstX",            // 5
            "_dstY",            // 6
            "_dstW",            // 7
            "_dstH",            // 8
            "_angle",           // 9 <<< ANGLE HERE
            "_rotationCenterX", // 10
            "_rotationCenterY", // 11
            "_flipMode"         // 12
        };
        VarType ptypes[] = {
            TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER,
            TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER,
            TYPE_REAL,    // _angle (index 9)
            TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER
        };

        for(int k=0; k < 13; ++k) {
            AST* param_var_decl_node = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(param_var_decl_node, ptypes[k]);

            Token* param_name_token = newToken(TOKEN_IDENTIFIER, pnames[k]);
            AST* param_var_name_node = newASTNode(AST_VARIABLE, param_name_token);
            setTypeAST(param_var_name_node, ptypes[k]); // Also set type on the variable name AST node
            freeToken(param_name_token);

            addChild(param_var_decl_node, param_var_name_node);
            dummy->children[k] = param_var_decl_node;
        }
        dummy->child_count = 13;
        dummy->var_type = TYPE_VOID; // RenderCopyEx is a Procedure
    } else if (strcasecmp(name, "getticks") == 0) { // <<< NEW
        dummy->child_capacity = 0; // No parameters
        dummy->children = NULL;
        dummy->child_count = 0;
        // Return type is Cardinal (unsigned integer). We'll represent it as INTEGER in the dummy AST's type system
        // Your Pscal type system should ideally have a TYPE_CARDINAL or handle this appropriately.
        // For the dummy AST, TYPE_INTEGER is often used as a stand-in for various integer-like types.
        Token* retTok = newToken(TOKEN_IDENTIFIER, "cardinal_or_integer"); // Name doesn't strictly matter here
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); // Or TYPE_CARDINAL if you add it
        setRight(dummy, retNode);          // For functions, ->right points to the return type AST node
        dummy->var_type = TYPE_INTEGER;    // Set the function's own var_type to its return type
                                           // (or TYPE_CARDINAL)
    } else if (strcasecmp(name, "realtostr") == 0) {
        dummy->type = AST_FUNCTION_DECL;
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }

        // Param 1: Value (Real)
        AST* p1_decl = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1_decl, TYPE_REAL);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_valueR"); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n);
        setTypeAST(v1, TYPE_REAL); addChild(p1_decl, v1);
        dummy->children[0] = p1_decl;
        dummy->child_count = 1;

        // Return type: String
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string_result");
        AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING);
        setRight(dummy, retNode);      // For functions, ->right points to the return type AST node
        dummy->var_type = TYPE_STRING; // Set the function's own var_type to its return type
    }
    // ... add other specific procedures if parameter list checking via dummy AST is desired ...
    else {
        // If it's an AST_FUNCTION_DECL and not handled above, it will retain TYPE_VOID
        // from newASTNode, which will trigger the warning in addProcedure.
        // If it's an AST_PROCEDURE_DECL, TYPE_VOID is appropriate.
        if (dummy->type == AST_FUNCTION_DECL) {
            // This is a function we haven't explicitly configured.
            // It will get TYPE_VOID by default from newASTNode and trigger the warning.
            // To fix, it needs its own 'else if' block above.
            // No action needed here, the warning will guide.
        } else { // AST_PROCEDURE_DECL
            dummy->var_type = TYPE_VOID; // Ensure procedures are typed VOID
        }
    }
}

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc) {
    // --- Basic setup for the dummy AST node ---
    char *lowerNameCopy = strdup(name); // Use original 'name' for strcmp in helper, but lowercase for token value
    if (!lowerNameCopy) {
        fprintf(stderr, "Memory allocation error (strdup lowerNameCopy) in registerBuiltinFunction\n");
        EXIT_FAILURE_HANDLER();
    }
    for (int i = 0; lowerNameCopy[i] != '\0'; i++) {
        lowerNameCopy[i] = tolower((unsigned char)lowerNameCopy[i]);
    }

    Token *funcNameToken = newToken(TOKEN_IDENTIFIER, lowerNameCopy); // Token value is lowercase
    if (!funcNameToken) {
        fprintf(stderr, "Memory allocation error creating token in registerBuiltinFunction\n");
        free(lowerNameCopy);
        EXIT_FAILURE_HANDLER();
    }
    free(lowerNameCopy); // Free the strdup'd lowercase name, token has its own copy

    AST *dummy = newASTNode(declType, funcNameToken); // newASTNode copies funcNameToken
    if (!dummy) {
        fprintf(stderr, "Memory allocation error creating AST node in registerBuiltinFunction\n");
        freeToken(funcNameToken);
        EXIT_FAILURE_HANDLER();
    }
    freeToken(funcNameToken); // Free the initial token, dummy AST has its own copy

    // Initialize basic AST node fields - newASTNode sets var_type to TYPE_VOID by default
    dummy->child_count = 0;
    dummy->child_capacity = 0;
    setLeft(dummy, NULL);
    setRight(dummy, NULL); // Helper will set this for functions
    setExtra(dummy, NULL);
    // dummy->var_type is already TYPE_VOID from newASTNode; configureBuiltinDummyAST will set it for functions.

    // --- Call helper to configure parameters and return type based on 'name' ---
    // Pass the original 'name' (not lowercased) because configureBuiltinDummyAST uses strcasecmp.
    configureBuiltinDummyAST(dummy, name);

    // --- Add to procedure table and free dummy AST ---
    addProcedure(dummy, unit_context_name_param_for_addproc);
    freeAST(dummy); // This will free the dummy node and its tree (params, return type node)
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
    long long currentOrdinal = 0;
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
        "upcase", "low", "high", "succ", "pred", "round", // Added Pred assuming it might exist
        "inttostr", "api_send", "api_receive", "screencols", "screenrows",
        "keypressed", "mstreamcreate", "quitrequested", "loadsound",
        "real"
        
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
        "initsoundsystem", "playsound", "quitsoundsystem",
        "issoundplaying", "rendercopyex"
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

// Pscal: FUNCTION RealToStr(Value: Real): String;
Value executeBuiltinRealToStr(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: RealToStr expects 1 argument (Value: Real).\n");
        // Consider EXIT_FAILURE_HANDLER() or return an error string
        return makeString("Error:RealToStrArgCount");
    }

    Value arg = eval(node->children[0]); // Evaluate the Real argument

    if (arg.type != TYPE_REAL) {
        // Optionally, allow integer to be promoted to real for RealToStr
        if (arg.type == TYPE_INTEGER || arg.type == TYPE_BYTE || arg.type == TYPE_WORD) {
            // Promote to real for string conversion
            arg.r_val = (double)arg.i_val;
            arg.type = TYPE_REAL; // Treat as real for formatting
        } else {
            fprintf(stderr, "Runtime error: RealToStr expects a Real or Integer compatible argument. Got %s.\n",
                    varTypeToString(arg.type));
            freeValue(&arg);
            return makeString("Error:RealToStrArgType");
        }
    }

    // Determine buffer size for snprintf
    // Standard %f can produce many digits. %g is often more compact.
    // A generous buffer size: sign, up to ~308 digits for mantissa, decimal point, exponent (e.g., e+308), null.
    // Or use a fixed precision for now for simplicity.
    char buffer[128]; // Should be sufficient for typical doubles with reasonable precision.

    // Use snprintf to convert double to string.
    // %g uses the shorter of %e or %f.
    // You can specify precision, e.g., "%.6g" for up to 6 significant digits.
    // For simplicity, let's use a default precision that snprintf's %f or %g chooses.
    // Standard Pascal often has a default width/precision for reals.
    int chars_written = snprintf(buffer, sizeof(buffer), "%f", arg.r_val); // Or "%g"

    if (chars_written < 0 || chars_written >= sizeof(buffer)) {
        fprintf(stderr, "Runtime error: Failed to convert real to string in RealToStr (snprintf error or buffer too small).\n");
        freeValue(&arg); // Free the evaluated argument
        return makeString("Error:RealToStrConversion");
    }

    Value result = makeString(buffer); // makeString copies the buffer
    freeValue(&arg);                   // Free the evaluated argument

    return result;
}
