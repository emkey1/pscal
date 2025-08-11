// src/backend_ast/builtin.c
#include "backend_ast/builtin.h"
#include "frontend/parser.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#ifdef SDL
#include "backend_ast/sdl.h"
#include "backend_ast/audio.h"
#endif
#include "globals.h"                  // Assuming globals.h is directly in src/
#include "backend_ast/builtin_network_api.h"
#include "vm/vm.h"

// Forward declaration for eval from interpreter
Value eval(AST *node);

// Standard library includes remain the same
#include <math.h>
#include <termios.h> // For tcgetattr, tcsetattr, etc. (Terminal I/O)
#include <unistd.h>  // For read, write, STDIN_FILENO, STDOUT_FILENO, isatty
#include <ctype.h>   // For isdigit
#include <errno.h>   // For errno
#include <sys/ioctl.h> // For ioctl, FIONREAD (Terminal I/O)
#include <stdint.h>  // For fixed-width integer types like uint8_t
#include <stdbool.h> // For bool, true, false (IMPORTANT - GCC needs this for 'bool')

// Comparison function for bsearch (case-insensitive) - MUST be defined before the table that uses it
static int compareVmBuiltinMappings(const void *key, const void *element) {
    const char *target_name = (const char *)key;
    const VmBuiltinMapping *mapping = (const VmBuiltinMapping *)element;
    return strcasecmp(target_name, mapping->name);
}

// The new dispatch table for the VM - MUST be defined before the function that uses it
// This list MUST BE SORTED ALPHABETICALLY BY NAME (lowercase).
static const VmBuiltinMapping vm_builtin_dispatch_table[] = {
    {"abs", vm_builtin_abs},
    {"api_receive", vm_builtin_api_receive},
    {"api_send", vm_builtin_api_send},
    {"assign", vm_builtin_assign},
    {"chr", vm_builtin_chr},
#ifdef SDL
    {"cleardevice", vm_builtin_cleardevice},
#endif
    {"close", vm_builtin_close},
#ifdef SDL
    {"closegraph", vm_builtin_closegraph},
#endif
    {"copy", vm_builtin_copy},
    {"cos", vm_builtin_cos},
#ifdef SDL
    {"createtargettexture", vm_builtin_createtargettexture}, // Moved
    {"createtexture", vm_builtin_createtexture}, // Moved
#endif
    {"dec", vm_builtin_dec},
    {"delay", vm_builtin_delay},
#ifdef SDL
    {"destroytexture", vm_builtin_destroytexture},
#endif
    {"dispose", vm_builtin_dispose},
#ifdef SDL
    {"drawcircle", vm_builtin_drawcircle}, // Moved
    {"drawline", vm_builtin_drawline}, // Moved
    {"drawpolygon", vm_builtin_drawpolygon}, // Moved
    {"drawrect", vm_builtin_drawrect}, // Moved
#endif
    {"eof", vm_builtin_eof},
    {"exit", vm_builtin_exit},
    {"exp", vm_builtin_exp},
#ifdef SDL
    {"fillcircle", vm_builtin_fillcircle},
    {"fillrect", vm_builtin_fillrect},
    {"getmaxx", vm_builtin_getmaxx},
    {"getmaxy", vm_builtin_getmaxy},
    {"getmousestate", vm_builtin_getmousestate},
    {"getpixelcolor", vm_builtin_getpixelcolor}, // Moved
    {"gettextsize", vm_builtin_gettextsize},
    {"getticks", vm_builtin_getticks},
    {"graphloop", vm_builtin_graphloop},
#endif
    {"gotoxy", vm_builtin_gotoxy},
    {"halt", vm_builtin_halt},
    {"high", vm_builtin_high},
    {"inc", vm_builtin_inc},
#ifdef SDL
    {"initgraph", vm_builtin_initgraph},
    {"initsoundsystem", vm_builtin_initsoundsystem},
    {"inittextsystem", vm_builtin_inittextsystem},
#endif
    {"inttostr", vm_builtin_inttostr},
    {"ioresult", vm_builtin_ioresult},
#ifdef SDL
    {"issoundplaying", vm_builtin_issoundplaying}, // Moved
    {"keypressed", vm_builtin_keypressed},
#endif
    {"length", vm_builtin_length},
    {"ln", vm_builtin_ln},
#ifdef SDL
    {"loadimagetotexture", vm_builtin_loadimagetotexture}, // Moved
    {"loadsound", vm_builtin_loadsound},
#endif
    {"low", vm_builtin_low},
    {"mstreamcreate", vm_builtin_mstreamcreate},
    {"mstreamfree", vm_builtin_mstreamfree},
    {"mstreamloadfromfile", vm_builtin_mstreamloadfromfile},
    {"mstreamsavetofile", vm_builtin_mstreamsavetofile},
    {"new", vm_builtin_new},
    {"ord", vm_builtin_ord},
#ifdef SDL
    {"outtextxy", vm_builtin_outtextxy}, // Moved
#endif
    {"paramcount", vm_builtin_paramcount},
    {"paramstr", vm_builtin_paramstr},
#ifdef SDL
    {"playsound", vm_builtin_playsound},
#endif
    {"pos", vm_builtin_pos},
#ifdef SDL
    {"putpixel", vm_builtin_putpixel},
#endif
    {"quitrequested", vm_builtin_quitrequested},
#ifdef SDL
    {"quitsoundsystem", vm_builtin_quitsoundsystem},
    {"quittextsystem", vm_builtin_quittextsystem},
#endif
    {"random", vm_builtin_random},
    {"randomize", vm_builtin_randomize},
    {"readkey", vm_builtin_readkey},
    {"readln", vm_builtin_readln},
    {"real", vm_builtin_real},
    {"realtostr", vm_builtin_realtostr},
#ifdef SDL
    {"rendercopy", vm_builtin_rendercopy}, // Moved
    {"rendercopyex", vm_builtin_rendercopyex}, // Moved
    {"rendercopyrect", vm_builtin_rendercopyrect},
    {"rendertexttotexture", vm_builtin_rendertexttotexture},
#endif
    {"reset", vm_builtin_reset},
    {"rewrite", vm_builtin_rewrite},
    {"round", vm_builtin_round},
    {"screencols", vm_builtin_screencols},
    {"screenrows", vm_builtin_screenrows},
#ifdef SDL
    {"setalphablend", vm_builtin_setalphablend},
    {"setcolor", vm_builtin_setcolor}, // Moved
    {"setrendertarget", vm_builtin_setrendertarget}, // Moved
    {"setrgbcolor", vm_builtin_setrgbcolor},
#endif
    {"sin", vm_builtin_sin},
    {"sqr", vm_builtin_sqr},
    {"sqrt", vm_builtin_sqrt},
    {"succ", vm_builtin_succ},
    {"tan", vm_builtin_tan},
    {"textbackground", vm_builtin_textbackground},
    {"textbackgrounde", vm_builtin_textbackgrounde},
    {"textcolor", vm_builtin_textcolor},
    {"textcolore", vm_builtin_textcolore},
    {"trunc", vm_builtin_trunc},
    {"upcase", vm_builtin_upcase},
#ifdef SDL
    {"updatescreen", vm_builtin_updatescreen},
    {"updatetexture", vm_builtin_updatetexture},
    {"waitkeyevent", vm_builtin_waitkeyevent}, // Moved
#endif
    {"wherex", vm_builtin_wherex},
    {"wherey", vm_builtin_wherey},
};

static const size_t num_vm_builtins = sizeof(vm_builtin_dispatch_table) / sizeof(vm_builtin_dispatch_table[0]);

// This function now comes AFTER the table and comparison function it uses.
VmBuiltinFn getVmBuiltinHandler(const char *name) {
    if (!name) return NULL;
    VmBuiltinMapping *found = (VmBuiltinMapping *)bsearch(
        name,
        vm_builtin_dispatch_table,
        num_vm_builtins,
        sizeof(VmBuiltinMapping),
        compareVmBuiltinMappings
    );
    return found ? found->handler : NULL;
}

Value vm_builtin_sqr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Sqr expects 1 argument.");
        return makeInt(0);
    }
    Value arg = args[0];
    if (arg.type == TYPE_INTEGER) {
        return makeInt(arg.i_val * arg.i_val);
    } else if (arg.type == TYPE_REAL) {
        return makeReal(arg.r_val * arg.r_val);
    }
    runtimeError(vm, "Sqr expects an Integer or Real argument. Got %s.", varTypeToString(arg.type));
    return makeInt(0);
}

Value vm_builtin_chr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "Chr expects 1 integer argument.");
        return makeChar('\0');
    }
    return makeChar((char)args[0].i_val);
}

Value vm_builtin_succ(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Succ expects 1 argument.");
        return makeVoid();
    }
    Value arg = args[0];
    switch(arg.type) {
        case TYPE_INTEGER: return makeInt(arg.i_val + 1);
        case TYPE_CHAR:    return makeChar(arg.c_val + 1);
        case TYPE_BOOLEAN: return makeBoolean(arg.i_val + 1 > 1 ? 1 : arg.i_val + 1);
        case TYPE_ENUM: {
            int ordinal = arg.enum_val.ordinal;
            if (arg.enum_meta && ordinal + 1 >= arg.enum_meta->member_count) {
                runtimeError(vm, "Succ enum overflow.");
                return makeVoid();
            }
            Value result = makeEnum(arg.enum_val.enum_name, ordinal + 1);
            result.enum_meta = arg.enum_meta;
            result.base_type_node = arg.base_type_node;
            return result;
        }
        default:
            runtimeError(vm, "Succ requires an ordinal type argument. Got %s.", varTypeToString(arg.type));
            return makeVoid();
    }
}

Value vm_builtin_upcase(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_CHAR) {
        runtimeError(vm, "Upcase expects 1 char argument.");
        return makeChar('\0');
    }
    return makeChar(toupper(args[0].c_val));
}

Value vm_builtin_pos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { // Changed this to a general error
        runtimeError(vm, "Pos expects 2 arguments.");
        return makeInt(0);
    }
    // Allow the first argument to be a char
    if (args[0].type != TYPE_STRING && args[0].type != TYPE_CHAR) {
        runtimeError(vm, "Pos first argument must be a string or char.");
        return makeInt(0);
    }
    if (args[1].type != TYPE_STRING) {
        runtimeError(vm, "Pos second argument must be a string.");
        return makeInt(0);
    }

    const char* needle = NULL;
    char needle_buf[2] = {0};
    if (args[0].type == TYPE_CHAR) {
        needle_buf[0] = AS_CHAR(args[0]);
        needle = needle_buf;
    } else {
        needle = AS_STRING(args[0]);
    }
    const char* haystack = AS_STRING(args[1]);
    if (!needle || !haystack) return makeInt(0);

    const char* found = strstr(haystack, needle);
    if (!found) {
        return makeInt(0);
    }
    return makeInt((long long)(found - haystack) + 1);
}

Value vm_builtin_copy(VM* vm, int arg_count, Value* args) {
    // Allow the first argument to be a char
    if (arg_count != 3 || (args[0].type != TYPE_STRING && args[0].type != TYPE_CHAR) || args[1].type != TYPE_INTEGER || args[2].type != TYPE_INTEGER) {
        runtimeError(vm, "Copy expects (String/Char, Integer, Integer).");
        return makeString("");
    }
    const char* source = NULL;
    char source_buf[2] = {0};
    if (args[0].type == TYPE_CHAR) {
        source_buf[0] = AS_CHAR(args[0]);
        source = source_buf;
    } else {
        source = AS_STRING(args[0]);
    }
    // ... (rest of the function is the same)
    long long start_idx = AS_INTEGER(args[1]);
    long long count = AS_INTEGER(args[2]);

    if (!source || start_idx < 1 || count < 0) return makeString("");

    size_t source_len = strlen(source);
    if ((size_t)start_idx > source_len) return makeString("");

    size_t start_0based = start_idx - 1;
    size_t len_to_copy = count;
    if (start_0based + len_to_copy > source_len) {
        len_to_copy = source_len - start_0based;
    }

    char* new_str = malloc(len_to_copy + 1);
    if (!new_str) {
        runtimeError(vm, "Malloc failed in copy().");
        return makeString("");
    }
    strncpy(new_str, source + start_0based, len_to_copy);
    new_str[len_to_copy] = '\0';

    Value result = makeString(new_str);
    free(new_str);
    return result;
}

Value vm_builtin_realtostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_REAL) {
        runtimeError(vm, "RealToStr expects 1 real argument.");
        return makeString("");
    }
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%f", AS_REAL(args[0]));
    return makeString(buffer);
}

Value vm_builtin_paramcount(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ParamCount expects 0 arguments.");
        return makeInt(0);
    }
    return makeInt(gParamCount);
}

Value vm_builtin_paramstr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "ParamStr expects 1 integer argument.");
        return makeString("");
    }
    long long idx = AS_INTEGER(args[0]);
    if (idx < 0 || idx > gParamCount) {
         return makeString("");
    }
    if (idx == 0) {
        // ParamStr(0) returns the program name, which we don't store yet.
        return makeString("");
    }
    return makeString(gParamValues[idx -1]);
}

Value vm_builtin_wherex(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "WhereX expects 0 arguments.");
        return makeInt(1);
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(c);
    }
    return makeInt(1); // Default on error
}

Value vm_builtin_wherey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "WhereY expects 0 arguments.");
        return makeInt(1);
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(r);
    }
    return makeInt(1); // Default on error
}

Value vm_builtin_keypressed(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "KeyPressed expects 0 arguments.");
        return makeBoolean(false);
    }
    // Logic from executeBuiltinKeyPressed
    struct termios oldt, newt;
    int bytes_available = 0;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    newt.c_cc[VMIN] = 0;
    newt.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    ioctl(STDIN_FILENO, FIONREAD, &bytes_available);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return makeBoolean(bytes_available > 0);
}

Value vm_builtin_readkey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ReadKey expects 0 arguments.");
        return makeChar('\0');
    }
    // Logic from executeBuiltinReadKey
    struct termios oldt, newt;
    char c;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    read(STDIN_FILENO, &c, 1);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return makeChar(c);
}

Value vm_builtin_quitrequested(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "QuitRequested expects 0 arguments.");
        // Return a default value in case of error
        return makeBoolean(false);
    }
    // Access the global flag and return it as a Pscal boolean
    return makeBoolean(break_requested != 0);
}

Value vm_builtin_gotoxy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER) {
        runtimeError(vm, "GotoXY expects 2 integer arguments.");
        return makeVoid();
    }
    long long x = AS_INTEGER(args[0]);
    long long y = AS_INTEGER(args[1]);
    printf("\x1B[%lld;%lldH", y, x);
    fflush(stdout);
    return makeVoid();
}

Value vm_builtin_textcolor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "TextColor expects 1 integer argument.");
        return makeVoid();
    }
    long long colorCode = AS_INTEGER(args[0]);
    gCurrentTextColor = (int)(colorCode % 16);
    gCurrentTextBold = (colorCode >= 8 && colorCode <= 15);
    gCurrentColorIsExt = false;
    return makeVoid();
}

Value vm_builtin_textbackground(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "TextBackground expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)(AS_INTEGER(args[0]) % 8);
    gCurrentBgIsExt = false;
    return makeVoid();
}

Value vm_builtin_textcolore(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || (args[0].type != TYPE_INTEGER && args[0].type != TYPE_BYTE && args[0].type != TYPE_WORD)) { // <<< MODIFIED LINE
        runtimeError(vm, "TextColorE expects an integer-compatible argument (Integer, Word, Byte)."); // Changed error message
        return makeVoid();
    }
    gCurrentTextColor = (int)AS_INTEGER(args[0]);
    gCurrentTextBold = false;
    gCurrentColorIsExt = true;
    return makeVoid();
}

Value vm_builtin_textbackgrounde(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "TextBackgroundE expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)AS_INTEGER(args[0]);
    gCurrentBgIsExt = true;
    return makeVoid();
}

Value vm_builtin_rewrite(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Rewrite requires 1 argument."); return makeVoid(); }

    if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
        runtimeError(vm, "Rewrite: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)args[0].ptr_val; // Dereference the pointer

    if (fileVarLValue->type != TYPE_FILE) { runtimeError(vm, "Argument to Rewrite must be a file variable."); return makeVoid(); }
    if (fileVarLValue->filename == NULL) { runtimeError(vm, "File variable not assigned a name before Rewrite."); return makeVoid(); }
    if (fileVarLValue->f_val) fclose(fileVarLValue->f_val);

    FILE* f = fopen(fileVarLValue->filename, "w");
    if (f == NULL) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
    fileVarLValue->f_val = f;
    return makeVoid();
}

Value vm_builtin_sqrt(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sqrt expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    if (x < 0) { runtimeError(vm, "sqrt expects a non-negative argument."); return makeReal(0.0); }
    return makeReal(sqrt(x));
}

Value vm_builtin_exp(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "exp expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(exp(x));
}

Value vm_builtin_ln(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ln expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    if (x <= 0) { runtimeError(vm, "ln expects a positive argument."); return makeReal(0.0); }
    return makeReal(log(x));
}

Value vm_builtin_cos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cos expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(cos(x));
}

Value vm_builtin_sin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sin expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(sin(x));
}

Value vm_builtin_tan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "tan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(tan(x));
}

Value vm_builtin_trunc(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "trunc expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (arg.type == TYPE_INTEGER) return makeInt(arg.i_val);
    if (arg.type == TYPE_REAL) return makeInt((long long)arg.r_val);
    runtimeError(vm, "trunc expects a numeric argument.");
    return makeInt(0);
}

static inline bool isOrdinalDelta(const Value* v) {
    return v->type == TYPE_INTEGER || v->type == TYPE_BYTE || v->type == TYPE_WORD || v->type == TYPE_CHAR /* || v->type == TYPE_BOOLEAN */;
}

static inline long long coerceDeltaToI64(const Value* v) {
    switch (v->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            return v->i_val;
        case TYPE_CHAR:
            return (unsigned char)v->c_val;
        default:
            return 0;
    }
}

Value vm_builtin_ord(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ord expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (arg.type == TYPE_CHAR) return makeInt((unsigned char)arg.c_val);
    if (arg.type == TYPE_BOOLEAN) return makeInt(arg.i_val);
    if (arg.type == TYPE_ENUM) return makeInt(arg.enum_val.ordinal);
    if (arg.type == TYPE_INTEGER) return makeInt(arg.i_val);
    runtimeError(vm, "ord expects an ordinal type argument.");
    return makeInt(0);
}

Value vm_builtin_inc(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "Inc expects 1 or 2 arguments.");
        return makeVoid();
    }
    if (args[0].type != TYPE_POINTER || args[0].ptr_val == NULL) {
        runtimeError(vm, "First argument to Inc must be a variable (pointer).");
        return makeVoid();
    }

    Value* target = (Value*)args[0].ptr_val;

    long long delta = 1;
    if (arg_count == 2) {
        if (!isOrdinalDelta(&args[1])) {
            runtimeError(vm, "Inc amount must be an ordinal (integer/byte/word/char).");
            return makeVoid();
        }
        delta = coerceDeltaToI64(&args[1]);
    }

    switch (target->type) {
        case TYPE_INTEGER:
            target->i_val += delta;
            break;

        case TYPE_BYTE: {
            long long next = target->i_val + delta;
            if (next < 0 || next > 255) {
                runtimeError(vm, "Warning: Range check error incrementing BYTE to %lld.", next);
            }
            target->i_val = (next & 0xFF);
            break;
        }

        case TYPE_WORD: {
            long long next = target->i_val + delta;
            if (next < 0 || next > 65535) {
                runtimeError(vm, "Warning: Range check error incrementing WORD to %lld.", next);
            }
            target->i_val = (next & 0xFFFF);
            break;
        }

        case TYPE_CHAR:
            target->c_val = (char)((unsigned char)target->c_val + (unsigned long long)delta);
            break;

        case TYPE_ENUM:
            target->enum_val.ordinal += (int)delta;
            break;

        default:
            runtimeError(vm, "Cannot Inc a non-ordinal type.");
            break;
    }

    return makeVoid(); // procedure
}

Value vm_builtin_dec(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "Dec expects 1 or 2 arguments.");
        return makeVoid();
    }
    if (args[0].type != TYPE_POINTER || args[0].ptr_val == NULL) {
        runtimeError(vm, "First argument to Dec must be a variable (pointer).");
        return makeVoid();
    }

    Value* target = (Value*)args[0].ptr_val;

    long long delta = 1;
    if (arg_count == 2) {
        if (!isOrdinalDelta(&args[1])) {
            runtimeError(vm, "Dec amount must be an ordinal (integer/byte/word/char).");
            return makeVoid();
        }
        delta = coerceDeltaToI64(&args[1]);
    }

    switch (target->type) {
        case TYPE_INTEGER:
            target->i_val -= delta;
            break;

        case TYPE_BYTE: {
            long long next = target->i_val - delta;
            if (next < 0 || next > 255) {
                runtimeError(vm, "Warning: Range check error decrementing BYTE to %lld.", next);
            }
            target->i_val = (next & 0xFF);
            break;
        }

        case TYPE_WORD: {
            long long next = target->i_val - delta;
            if (next < 0 || next > 65535) {
                runtimeError(vm, "Warning: Range check error decrementing WORD to %lld.", next);
            }
            target->i_val = (next & 0xFFFF);
            break;
        }

        case TYPE_CHAR:
            target->c_val = (char)((unsigned char)target->c_val - (unsigned long long)delta);
            break;

        case TYPE_ENUM:
            target->enum_val.ordinal -= (int)delta;
            break;

        default:
            runtimeError(vm, "Cannot Dec a non-ordinal type.");
            break;
    }

    return makeVoid(); // procedure
}

Value vm_builtin_low(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Low() expects a single type identifier.");
        return makeInt(0);
    }

    Value arg = args[0];
    const char* typeName = NULL;
    AST* typeDef = NULL;
    VarType t = TYPE_UNKNOWN;

    // Extract type name or type information from the argument
    if (arg.type == TYPE_STRING) {
        typeName = AS_STRING(arg);
    } else if (arg.type == TYPE_ENUM) {
        typeName = arg.enum_val.enum_name;
        t = TYPE_ENUM;
    } else {
        t = arg.type;
    }

    // Map the provided name to a VarType if we haven't yet
    if (t == TYPE_UNKNOWN && typeName) {
        if (strcasecmp(typeName, "integer") == 0)      t = TYPE_INTEGER;
        else if (strcasecmp(typeName, "char") == 0)    t = TYPE_CHAR;
        else if (strcasecmp(typeName, "boolean") == 0) t = TYPE_BOOLEAN;
        else if (strcasecmp(typeName, "byte") == 0)    t = TYPE_BYTE;
        else if (strcasecmp(typeName, "word") == 0)    t = TYPE_WORD;
        else {
            typeDef = lookupType(typeName);
            if (typeDef) t = typeDef->var_type;
        }
    } else if (t == TYPE_ENUM && typeName) {
        typeDef = lookupType(typeName);
    }

    switch (t) {
        case TYPE_INTEGER: return makeInt(-2147483648);
        case TYPE_CHAR:    return makeChar(0);
        case TYPE_BOOLEAN: return makeBoolean(false);
        case TYPE_BYTE:    return makeInt(0);
        case TYPE_WORD:    return makeInt(0);
        case TYPE_ENUM: {
            if (typeDef && typeDef->var_type == TYPE_ENUM && typeName) {
                return makeEnum(typeName, 0);
            }
            break;
        }
        default:
            break;
    }

    if (typeName)
        runtimeError(vm, "Low() not supported for type '%s'.", typeName);
    else
        runtimeError(vm, "Low() not supported for provided type.");
    return makeInt(0);
}

Value vm_builtin_high(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "High() expects a single type identifier.");
        return makeInt(0);
    }

    Value arg = args[0];
    const char* typeName = NULL;
    AST* typeDef = NULL;
    VarType t = TYPE_UNKNOWN;

    if (arg.type == TYPE_STRING) {
        typeName = AS_STRING(arg);
    } else if (arg.type == TYPE_ENUM) {
        typeName = arg.enum_val.enum_name;
        t = TYPE_ENUM;
    } else {
        t = arg.type;
    }

    if (t == TYPE_UNKNOWN && typeName) {
        if (strcasecmp(typeName, "integer") == 0)      t = TYPE_INTEGER;
        else if (strcasecmp(typeName, "char") == 0)    t = TYPE_CHAR;
        else if (strcasecmp(typeName, "boolean") == 0) t = TYPE_BOOLEAN;
        else if (strcasecmp(typeName, "byte") == 0)    t = TYPE_BYTE;
        else if (strcasecmp(typeName, "word") == 0)    t = TYPE_WORD;
        else {
            typeDef = lookupType(typeName);
            if (typeDef) t = typeDef->var_type;
        }
    } else if (t == TYPE_ENUM && typeName) {
        typeDef = lookupType(typeName);
    }

    switch (t) {
        case TYPE_INTEGER: return makeInt(2147483647);
        case TYPE_CHAR:    return makeChar((unsigned char)255);
        case TYPE_BOOLEAN: return makeBoolean(true);
        case TYPE_BYTE:    return makeInt(255);
        case TYPE_WORD:    return makeInt(65535);
        case TYPE_ENUM: {
            if (typeDef && typeDef->var_type == TYPE_ENUM && typeName) {
                return makeEnum(typeName, typeDef->child_count - 1);
            }
            break;
        }
        default:
            break;
    }

    if (typeName)
        runtimeError(vm, "High() not supported for type '%s'.", typeName);
    else
        runtimeError(vm, "High() not supported for provided type.");
    return makeInt(0);
}

Value vm_builtin_new(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_POINTER) {
        runtimeError(vm, "new() expects a single pointer variable argument.");
        return makeVoid();
    }
    
    // The argument on the stack is a pointer TO the pointer variable's Value struct
    Value* pointerVarValuePtr = (Value*)args[0].ptr_val;
    if (!pointerVarValuePtr) {
        runtimeError(vm, "VM internal error: new() received a null LValue pointer.");
        return makeVoid();
    }
    if (pointerVarValuePtr->type != TYPE_POINTER) {
        runtimeError(vm, "Argument to new() must be of pointer type. Got %s.", varTypeToString(pointerVarValuePtr->type));
        return makeVoid();
    }

    AST *baseTypeNode = pointerVarValuePtr->base_type_node;
    if (!baseTypeNode) {
        runtimeError(vm, "Cannot determine base type for pointer variable in new().");
        return makeVoid();
    }

    // This logic is similar to the AST version's
    VarType baseVarType = TYPE_VOID;
    AST* actualBaseTypeDef = baseTypeNode;

    if (actualBaseTypeDef->type == AST_VARIABLE && actualBaseTypeDef->token) {
        const char* typeName = actualBaseTypeDef->token->value;
        if (strcasecmp(typeName, "integer")==0) { baseVarType=TYPE_INTEGER; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "real")==0) { baseVarType=TYPE_REAL; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "char")==0) { baseVarType=TYPE_CHAR; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "string")==0) { baseVarType=TYPE_STRING; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "boolean")==0) { baseVarType=TYPE_BOOLEAN; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "byte")==0) { baseVarType=TYPE_BYTE; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "word")==0) { baseVarType=TYPE_WORD; actualBaseTypeDef = NULL; }
        else {
            AST* lookedUpType = lookupType(typeName);
            if (!lookedUpType) { runtimeError(vm, "Cannot resolve base type '%s' in new().", typeName); return makeVoid(); }
            actualBaseTypeDef = lookedUpType;
            baseVarType = actualBaseTypeDef->var_type;
        }
    } else {
         baseVarType = actualBaseTypeDef->var_type;
    }

    if (baseVarType == TYPE_VOID) { runtimeError(vm, "Cannot determine valid base type in new()."); return makeVoid(); }
    
    Value* allocated_memory = malloc(sizeof(Value));
    if (!allocated_memory) { runtimeError(vm, "Memory allocation failed in new()."); return makeVoid(); }
    
    *(allocated_memory) = makeValueForType(baseVarType, actualBaseTypeDef, NULL);
    
    // Update the pointer variable that was passed by reference
    pointerVarValuePtr->ptr_val = allocated_memory;
    pointerVarValuePtr->type = TYPE_POINTER;

    return makeVoid();
}

Value vm_builtin_exit(VM* vm, int arg_count, Value* args) {
    if (arg_count > 0) {
        runtimeError(vm, "exit does not take any arguments.");
        return makeVoid();
    }
    // Signal the VM to unwind the current call frame on return from the builtin
    vm->exit_requested = true;
    return makeVoid();
}

Value vm_builtin_dispose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_POINTER) {
        runtimeError(vm, "dispose() expects a single pointer variable argument.");
        return makeVoid();
    }
    
    Value* pointerVarValuePtr = (Value*)args[0].ptr_val;
    if (!pointerVarValuePtr) {
        runtimeError(vm, "VM internal error: dispose() received a null LValue pointer.");
        return makeVoid();
    }
    if (pointerVarValuePtr->type != TYPE_POINTER) {
        runtimeError(vm, "Argument to dispose() must be a pointer.");
        return makeVoid();
    }

    Value* valueToDispose = pointerVarValuePtr->ptr_val;
    if (valueToDispose == NULL) {
        // Disposing a nil pointer is a safe no-op.
        return makeVoid();
    }
    
    // Get the address value BEFORE freeing the memory
    uintptr_t disposedAddrValue = (uintptr_t)valueToDispose;
    
    // Free the memory and the Value struct itself
    freeValue(valueToDispose);
    free(valueToDispose);
    
    // Set the original pointer variable to nil
    pointerVarValuePtr->ptr_val = NULL;
    
    // Call the new helper to find and nullify any dangling aliases
    vm_nullifyAliases(vm, disposedAddrValue);
    
    return makeVoid();
}

Value vm_builtin_assign(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "Assign requires 2 arguments.");
        return makeVoid();
    }

    // --- FIX: Arguments are in reverse order from the stack ---
    // For assign(f, filename), filename is at args[1] and a pointer to f is at args[0].
    Value fileVarPtr  = args[0];
    Value fileNameVal = args[1];

    if (fileVarPtr.type != TYPE_POINTER || !fileVarPtr.ptr_val) {
        runtimeError(vm, "Assign: First argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)fileVarPtr.ptr_val;

    if (fileVarLValue->type != TYPE_FILE) {
        runtimeError(vm, "First arg to Assign must be a file variable.");
        return makeVoid();
    }
    if (fileNameVal.type != TYPE_STRING) {
        runtimeError(vm, "Second arg to Assign must be a string. Got type %s.", varTypeToString(fileNameVal.type));
        return makeVoid();
    }

    if (fileVarLValue->filename) {
        free(fileVarLValue->filename);
    }

    // Use strdup to create a persistent copy of the filename
    fileVarLValue->filename = fileNameVal.s_val ? strdup(fileNameVal.s_val) : NULL;
    if (fileNameVal.s_val && !fileVarLValue->filename) {
        runtimeError(vm, "Memory allocation failed for filename in assign.");
    }

    return makeVoid();
}

Value vm_builtin_reset(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Reset requires 1 argument."); return makeVoid(); }
    
    if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
        runtimeError(vm, "Reset: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)args[0].ptr_val; // Dereference the pointer

    if (fileVarLValue->type != TYPE_FILE) { runtimeError(vm, "Argument to Reset must be a file variable."); return makeVoid(); }
    if (fileVarLValue->filename == NULL) { runtimeError(vm, "File variable not assigned a name before Reset."); return makeVoid(); }
    if (fileVarLValue->f_val) fclose(fileVarLValue->f_val);

    FILE* f = fopen(fileVarLValue->filename, "r");
    if (f == NULL) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
    fileVarLValue->f_val = f;
    return makeVoid();
}

Value vm_builtin_close(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Close requires 1 argument."); return makeVoid(); }
    
    if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
        runtimeError(vm, "Close: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)args[0].ptr_val; // Dereference the pointer

    if (fileVarLValue->type != TYPE_FILE) { runtimeError(vm, "Argument to Close must be a file variable."); return makeVoid(); }
    if (fileVarLValue->f_val) {
        fclose(fileVarLValue->f_val);
        fileVarLValue->f_val = NULL;
    }
    // Standard Pascal does not de-assign the filename on Close.
    return makeVoid();
}

Value vm_builtin_eof(VM* vm, int arg_count, Value* args) {
    FILE* stream = stdin;

    if (arg_count == 0) {
        if (vm->vmGlobalSymbols) {
            Symbol* inputSym = hashTableLookup(vm->vmGlobalSymbols, "input");
            if (inputSym && inputSym->value &&
                inputSym->value->type == TYPE_FILE &&
                inputSym->value->f_val) {
                stream = inputSym->value->f_val;
            }
        }
    } else if (arg_count == 1) {
        if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
            runtimeError(vm, "Eof: Argument must be a VAR file parameter.");
            return makeBoolean(true);
        }
        Value* fileVarLValue = (Value*)args[0].ptr_val; // Dereference the pointer
        if (fileVarLValue->type != TYPE_FILE) {
            runtimeError(vm, "Argument to Eof must be a file variable.");
            return makeBoolean(true);
        }
        if (!fileVarLValue->f_val) {
            return makeBoolean(true); // Closed file is treated as EOF
        }
        stream = fileVarLValue->f_val;
    } else {
        runtimeError(vm, "Eof expects 0 or 1 arguments.");
        return makeBoolean(true);
    }

    int c = fgetc(stream);
    if (c == EOF) {
        return makeBoolean(true);
    }
    ungetc(c, stream); // Push character back
    return makeBoolean(false);
}

Value vm_builtin_readln(VM* vm, int arg_count, Value* args) {
    FILE* input_stream = stdin;
    int var_start_index = 0;
    bool first_arg_is_file_by_value = false;   // ***NEW***

    // 1) Determine input stream: allow FILE or ^FILE
    if (arg_count > 0) {
        const Value* a0 = &args[0];
        if (a0->type == TYPE_POINTER && a0->ptr_val) a0 = (const Value*)a0->ptr_val;
        if (a0->type == TYPE_FILE) {
            if (!a0->f_val) { runtimeError(vm, "File not open for Readln."); last_io_error = 1; return makeVoid(); }
            input_stream = a0->f_val;
            var_start_index = 1;
            // If the actual arg0 on the stack is a FILE by value (not a pointer),
            // the VM will free it after we return, which would fclose() the stream.
            // Prevent that by neutering the stack value before we return.
            if (args[0].type == TYPE_FILE) first_arg_is_file_by_value = true;   // ***NEW***
        }
    }

    // 2) Read full line
    char line_buffer[1024];
    if (fgets(line_buffer, sizeof(line_buffer), input_stream) == NULL) {
        last_io_error = feof(input_stream) ? 0 : 1;
        // ***NEW***: prevent VM cleanup from closing the stream we used
        if (first_arg_is_file_by_value) { args[0].type = TYPE_NIL; args[0].f_val = NULL; }  // ***NEW***
        return makeVoid();
    }
    line_buffer[strcspn(line_buffer, "\r\n")] = '\0';

    // 3) Parse vars from buffer
    const char* p = line_buffer;
    for (int i = var_start_index; i < arg_count; i++) {
        if (args[i].type != TYPE_POINTER || !args[i].ptr_val) { runtimeError(vm, "Readln requires VAR parameters to read into."); last_io_error = 1; break; }
        Value* dst = (Value*)args[i].ptr_val;

        while (isspace((unsigned char)*p)) p++;

        // If the destination has not been initialized, default it to an empty
        // string so that Readln can populate it.  Variables declared as
        // strings start out with the TYPE_NIL tag which caused the builtin to
        // reject them.  Treating TYPE_NIL as a string matches Pascal's
        // semantics where uninitialised strings can be read into directly.
        if (dst->type == TYPE_NIL) {
            dst->type = TYPE_STRING;
            dst->s_val = NULL;
        }

        switch (dst->type) {
            case TYPE_INTEGER:
            case TYPE_WORD:
            case TYPE_BYTE: {
                errno = 0;
                char* endp = NULL;
                long long v = strtoll(p, &endp, 10);
                if (endp == p || errno == ERANGE) { last_io_error = 1; v = 0; }
                dst->i_val = v;
                p = endp ? endp : p;
                break;
            }
            case TYPE_REAL: {
                errno = 0;
                char* endp = NULL;
                double v = strtod(p, &endp);
                if (endp == p || errno == ERANGE) { last_io_error = 1; v = 0.0; }
                dst->r_val = v;
                p = endp ? endp : p;
                break;
            }
            case TYPE_CHAR:
                if (*p) { dst->c_val = *p++; } else { dst->c_val = '\0'; last_io_error = 1; }
                break;

            case TYPE_STRING: {
                char* tmp = strdup(p);
                if (!tmp) { runtimeError(vm, "Out of memory in Readln."); last_io_error = 1; break; }
                freeValue(dst);
                dst->type = TYPE_STRING;
                dst->s_val = tmp;
                i = arg_count; // consume the line; ignore trailing params
                break;
            }

            default:
                runtimeError(vm, "Cannot Readln into a variable of type %s.", varTypeToString(dst->type));
                last_io_error = 1;
                i = arg_count;
                break;
        }
    }

    if (!last_io_error && ferror(input_stream)) last_io_error = 1;
    else if (last_io_error != 1) last_io_error = 0;

    // ***NEW***: neuter FILE-by-value arg so VM cleanup won’t fclose()
    if (first_arg_is_file_by_value) { args[0].type = TYPE_NIL; args[0].f_val = NULL; }  // ***NEW***

    return makeVoid();
}

Value vm_builtin_ioresult(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "IOResult requires 0 arguments."); return makeInt(0); }
    int err = last_io_error;
    last_io_error = 0;
    return makeInt(err);
}

// --- VM BUILT-IN IMPLEMENTATIONS: RANDOM ---

Value vm_builtin_randomize(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "Randomize requires 0 arguments."); return makeVoid(); }
    srand((unsigned int)time(NULL));
    return makeVoid();
}

Value vm_builtin_random(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) {
        return makeReal((double)rand() / ((double)RAND_MAX + 1.0));
    }
    if (arg_count == 1 && args[0].type == TYPE_INTEGER) {
        long long n = args[0].i_val;
        if (n <= 0) { runtimeError(vm, "Random argument must be > 0."); return makeInt(0); }
        return makeInt(rand() % n);
    }
    runtimeError(vm, "Random requires 0 arguments, or 1 integer argument.");
    return makeVoid();
}

Value vm_builtin_screencols(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ScreenCols expects 0 arguments.");
        return makeInt(80);
    }
    int rows, cols;
    if (getTerminalSize(&rows, &cols) == 0) {
        return makeInt(cols);
    }
    return makeInt(80); // Default on error
}

Value vm_builtin_screenrows(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ScreenRows expects 0 arguments.");
        return makeInt(24);
    }
    int rows, cols;
    if (getTerminalSize(&rows, &cols) == 0) {
        return makeInt(rows);
    }
    return makeInt(24); // Default on error
}

// --- VM-NATIVE MEMORY STREAM FUNCTIONS ---
Value vm_builtin_mstreamcreate(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "MStreamCreate expects no arguments.");
        return makeVoid();
    }
    MStream *ms = malloc(sizeof(MStream));
    if (!ms) {
        runtimeError(vm, "Memory allocation error for MStream structure in MStreamCreate.");
        return makeVoid();
    }
    ms->buffer = NULL;
    ms->size = 0;
    ms->capacity = 0;
    return makeMStream(ms);
}

Value vm_builtin_mstreamloadfromfile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "MStreamLoadFromFile expects 2 arguments (MStreamVar, Filename).");
        return makeVoid();
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (args[0].type != TYPE_POINTER) {
        runtimeError(vm, "MStreamLoadFromFile: First argument must be a VAR MStream.");
        return makeVoid();
    }
    Value* ms_value_ptr = (Value*)args[0].ptr_val;
    if (!ms_value_ptr || ms_value_ptr->type != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamLoadFromFile: First argument is not a valid MStream variable.");
        return makeVoid();
    }
    MStream* ms = ms_value_ptr->mstream;
    if (!ms) {
        runtimeError(vm, "MStreamLoadFromFile: MStream variable not initialized.");
        return makeVoid();
    }

    // Argument 1 is the filename string
    if (args[1].type != TYPE_STRING || args[1].s_val == NULL) {
        runtimeError(vm, "MStreamLoadFromFile: Second argument must be a string filename.");
        return makeVoid(); // No need to free args[1] here, vm stack manages
    }
    const char* filename = args[1].s_val;

    FILE* f = fopen(filename, "rb");
    if (!f) {
        runtimeError(vm, "MStreamLoadFromFile: Cannot open file '%s' for reading.", filename);
        return makeVoid();
    }

    fseek(f, 0, SEEK_END);
    int size = (int)ftell(f);
    rewind(f);

    unsigned char* buffer = malloc(size + 1); // +1 for null terminator for safety
    if (!buffer) {
        fclose(f);
        runtimeError(vm, "MStreamLoadFromFile: Memory allocation error for file buffer.");
        return makeVoid();
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0'; // Null-terminate the buffer
    fclose(f);

    // Free existing buffer in MStream if any
    if (ms->buffer) free(ms->buffer);

    ms->buffer = buffer;
    ms->size = size;
    ms->capacity = size + 1; // Capacity is now exactly what's needed + null

    return makeVoid();
}

Value vm_builtin_mstreamsavetofile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "MStreamSaveToFile expects 2 arguments (MStreamVar, Filename).");
        return makeVoid();
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (args[0].type != TYPE_POINTER) {
        runtimeError(vm, "MStreamSaveToFile: First argument must be a VAR MStream.");
        return makeVoid();
    }
    Value* ms_value_ptr = (Value*)args[0].ptr_val;
    if (!ms_value_ptr || ms_value_ptr->type != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamSaveToFile: First argument is not a valid MStream variable.");
        return makeVoid();
    }
    MStream* ms = ms_value_ptr->mstream;
    if (!ms) {
        runtimeError(vm, "MStreamSaveToFile: MStream variable not initialized.");
        return makeVoid();
    }

    // Argument 1 is the filename string
    if (args[1].type != TYPE_STRING || args[1].s_val == NULL) {
        runtimeError(vm, "MStreamSaveToFile: Second argument must be a string filename.");
        return makeVoid();
    }
    const char* filename = args[1].s_val;

    FILE* f = fopen(filename, "wb");
    if (!f) {
        runtimeError(vm, "MStreamSaveToFile: Cannot open file '%s' for writing.", filename);
        return makeVoid();
    }

    if (ms->buffer && ms->size > 0) {
        fwrite(ms->buffer, 1, ms->size, f);
    }
    fclose(f);

    return makeVoid();
}

Value vm_builtin_mstreamfree(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "MStreamFree expects 1 argument (MStreamVar).");
        return makeVoid();
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (args[0].type != TYPE_POINTER) {
        runtimeError(vm, "MStreamFree: First argument must be a VAR MStream.");
        return makeVoid();
    }
    Value* ms_value_ptr = (Value*)args[0].ptr_val;
    if (!ms_value_ptr || ms_value_ptr->type != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamFree: First argument is not a valid MStream variable.");
        return makeVoid();
    }
    MStream* ms = ms_value_ptr->mstream;

    if (ms) { // Only free if MStream struct itself exists
        if (ms->buffer) {
            free(ms->buffer);
            ms->buffer = NULL;
        }
        free(ms); // Free the MStream struct
        ms_value_ptr->mstream = NULL; // Crucial: Set the MStream pointer in the variable's Value struct to NULL
    }
    // If ms was NULL, it's a no-op, which is fine.

    return makeVoid();
}

Value vm_builtin_real(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Real() expects 1 argument.");
        return makeReal(0.0);
    }
    Value arg = args[0];
    switch (arg.type) {
        case TYPE_INTEGER:
        case TYPE_BYTE:
        case TYPE_WORD:
        case TYPE_BOOLEAN:
            return makeReal((double)arg.i_val);
        case TYPE_CHAR:
            return makeReal((double)arg.c_val);
        case TYPE_REAL:
            // Return a copy of the real value itself, it's already a real.
            return makeReal(arg.r_val);
        default:
            runtimeError(vm, "Real() argument must be an Integer, Ordinal, or Real type. Got %s.", varTypeToString(arg.type));
            return makeReal(0.0);
    }
}


// Comparison function for bsearch (case-insensitive)
static int compareBuiltinMappings(const void *key, const void *element) {
    const char *target_name = (const char *)key;
    const BuiltinMapping *mapping = (const BuiltinMapping *)element;
    return strcasecmp(target_name, mapping->name);
}

static const BuiltinMapping builtin_dispatch_table[] = {
    {"abs",       executeBuiltinAbs},
    {"api_receive", executeBuiltinAPIReceive},
    {"api_send",  executeBuiltinAPISend},
    {"assign",    executeBuiltinAssign},
    {"chr",       executeBuiltinChr},
#ifdef SDL
    {"cleardevice", executeBuiltinClearDevice},
#endif
    {"close",     executeBuiltinClose},
#ifdef SDL
    {"closegraph", executeBuiltinCloseGraph},
#endif
    {"copy",      executeBuiltinCopy},
    {"cos",       executeBuiltinCos},
#ifdef SDL
    {"createtargettexture", executeBuiltinCreateTargetTexture},
    {"createtexture", executeBuiltinCreateTexture},
#endif
    {"dec",       executeBuiltinDec},
    {"delay",     executeBuiltinDelay},
#ifdef SDL
    {"destroytexture", executeBuiltinDestroyTexture},
#endif
    {"dispose",   executeBuiltinDispose},
#ifdef SDL
    {"drawcircle", executeBuiltinDrawCircle},
    {"drawline", executeBuiltinDrawLine},
    {"drawpolygon", executeBuiltinDrawPolygon},
    {"drawrect",  executeBuiltinDrawRect},
#endif
    {"eof",       executeBuiltinEOF},
    {"exit",      executeBuiltinExit},
    {"exp",       executeBuiltinExp},
#ifdef SDL
    {"fillcircle", executeBuiltinFillCircle},
    {"fillrect",  executeBuiltinFillRect},
    {"getmaxx",   executeBuiltinGetMaxX},
    {"getmaxy",   executeBuiltinGetMaxY},
    {"getmousestate", executeBuiltinGetMouseState},
    {"getpixelcolor", executeBuiltinGetPixelColor},
    {"gettextsize", executeBuiltinGetTextSize},
    {"getticks", executeBuiltinGetTicks},
    {"graphloop", executeBuiltinGraphLoop},
#endif
    {"gotoxy", executeBuiltinGotoXY},
    {"halt",      executeBuiltinHalt},
    {"high",      executeBuiltinHigh},
    {"inc",       executeBuiltinInc},
#ifdef SDL
    {"initgraph", executeBuiltinInitGraph},
    {"initsoundsystem", executeBuiltinInitSoundSystem},
    {"inittextsystem", executeBuiltinInitTextSystem},
#endif
    {"inttostr",  executeBuiltinIntToStr},
    {"ioresult",  executeBuiltinIOResult},
#ifdef SDL
    {"issoundplaying", executeBuiltinIsSoundPlaying},
#endif
    {"keypressed", executeBuiltinKeyPressed},
    {"length",    executeBuiltinLength},
    {"ln",        executeBuiltinLn},
#ifdef SDL
    {"loadimagetotexture", executeBuiltinLoadImageToTexture},
    {"loadsound", executeBuiltinLoadSound},
#endif
    {"low",       executeBuiltinLow},
    {"mstreamcreate", executeBuiltinMstreamCreate},
    {"mstreamfree", executeBuiltinMstreamFree},
    {"mstreamloadfromfile", executeBuiltinMstreamLoadFromFile},
    {"mstreamsavetofile", executeBuiltinMstreamSaveToFile},
    {"new",       executeBuiltinNew},
    {"ord",       executeBuiltinOrd},
#ifdef SDL
    {"outtextxy", executeBuiltinOutTextXY},
#endif
    {"paramcount", executeBuiltinParamcount},
    {"paramstr",  executeBuiltinParamstr},
#ifdef SDL
    {"playsound", executeBuiltinPlaySound},
#endif
    {"pos",       executeBuiltinPos},
#ifdef SDL
    {"putpixel",  executeBuiltinPutPixel},
    {"quitrequested", executeBuiltinQuitRequested},
    {"quitsoundsystem", executeBuiltinQuitSoundSystem},
    {"quittextsystem", executeBuiltinQuitTextSystem},
#endif
    {"random",    executeBuiltinRandom},
    {"randomize", executeBuiltinRandomize},
    {"readkey",   executeBuiltinReadKey},
    {"real",      executeBuiltinReal},
    {"realtostr", executeBuiltinRealToStr},
#ifdef SDL
    {"rendercopy", executeBuiltinRenderCopy},
    {"rendercopyex", executeBuiltinRenderCopyEx},
    {"rendercopyrect", executeBuiltinRenderCopyRect},
    {"rendertexttotexture", executeBuiltinRenderTextToTexture},
#endif
    {"reset",     executeBuiltinReset},
    {"rewrite",   executeBuiltinRewrite},
    {"round",     executeBuiltinRound},
    {"screencols", executeBuiltinScreenCols},
    {"screenrows", executeBuiltinScreenRows},
#ifdef SDL
    {"setalphablend", executeBuiltinSetAlphaBlend},
    {"setcolor",  executeBuiltinSetColor},
    {"setrendertarget",  executeBuiltinSetRenderTarget},
    {"setrgbcolor", executeBuiltinSetRGBColor},
#endif
    {"sin",       executeBuiltinSin},
    {"sqr",       executeBuiltinSqr},
    {"sqrt",      executeBuiltinSqrt},
    {"succ",      executeBuiltinSucc},
    {"tan",       executeBuiltinTan},
    {"gotoxy",    executeBuiltinGotoXY},
    {"textbackground", executeBuiltinTextBackground},
    {"textbackgrounde", executeBuiltinTextBackgroundE},
    {"textcolor", executeBuiltinTextColor},
    {"textcolore", executeBuiltinTextColorE},
    {"trunc",     executeBuiltinTrunc},
    {"upcase",    executeBuiltinUpcase},
#ifdef SDL
    {"updatescreen", executeBuiltinUpdateScreen},
    {"updatetexture", executeBuiltinUpdateTexture},
    {"waitkeyevent", executeBuiltinWaitKeyEvent},
#endif
    {"wherex",    executeBuiltinWhereX},
    {"wherey",    executeBuiltinWhereY}
};

// Calculate the number of entries in the table
static const size_t num_builtins = sizeof(builtin_dispatch_table) / sizeof(builtin_dispatch_table[0]);

void assignValueToLValue(AST *lvalueNode, Value newValue) {
    if (!lvalueNode) {
        fprintf(stderr, "Runtime error: Cannot assign to NULL lvalue node.\n");
        freeValue(&newValue);
        EXIT_FAILURE_HANDLER();
    }

    Value* target_ptr = resolveLValueToPtr(lvalueNode);
    if (!target_ptr) {
        fprintf(stderr, "Runtime error: LValue could not be resolved for assignment.\n");
        freeValue(&newValue);
        EXIT_FAILURE_HANDLER();
    }

    if (target_ptr->type == TYPE_STRING && target_ptr->max_length > 0) {
        const char* source_str = "";
        char char_buf[2];

        if (newValue.type == TYPE_STRING) {
            source_str = newValue.s_val ? newValue.s_val : "";
        } else if (newValue.type == TYPE_CHAR) {
            char_buf[0] = newValue.c_val;
            char_buf[1] = '\0';
            source_str = char_buf;
        } else {
             fprintf(stderr, "Runtime error: Cannot assign type %s to a fixed-length string.\n", varTypeToString(newValue.type));
             freeValue(&newValue);
             EXIT_FAILURE_HANDLER();
        }

        strncpy(target_ptr->s_val, source_str, target_ptr->max_length);
        target_ptr->s_val[target_ptr->max_length] = '\0'; // Ensure null-termination.

        freeValue(&newValue); // Free the incoming value's contents
        return; // Assignment is done
    }

    // For all other types, use the original logic.
    freeValue(target_ptr);
    *target_ptr = makeCopyOfValue(&newValue);
    // No need to free newValue, its contents were moved or copied
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
    while (i < (int)sizeof(buf) - 1) {
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
    return makeValueForType(TYPE_INTEGER, NULL, NULL);

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

Value executeBuiltinClose(AST *node) {
    if (node->child_count != 1) {
        fprintf(stderr, "Runtime error: Close expects 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value fileVal = eval(node->children[0]);
    if (fileVal.type != TYPE_FILE) {
        fprintf(stderr, "Runtime error: Argument to Close must be a file variable.\n");
        freeValue(&fileVal);
        EXIT_FAILURE_HANDLER();
    }

    // Only close if the file handle is actually open
    if (fileVal.f_val != NULL) {
        fclose(fileVal.f_val);

        // Find the symbol in the symbol table to nullify its file handle
        const char *fileVarName = node->children[0]->token->value;
        Symbol *sym = lookupSymbol(fileVarName);
        if (sym && sym->value && sym->value->type == TYPE_FILE) {
            sym->value->f_val = NULL; // Set the file handle to NULL
        }
    }
    
    // Free the temporary Value struct returned by eval
    freeValue(&fileVal);

    return makeVoid();
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
    FILE *f = stdin;

    if (node->child_count == 0) {
        Symbol* inputSym = lookupSymbol("input");
        if (inputSym && inputSym->value &&
            inputSym->value->type == TYPE_FILE &&
            inputSym->value->f_val) {
            f = inputSym->value->f_val;
        }
    } else if (node->child_count == 1) {
        Value fileVal = eval(node->children[0]);
        if (fileVal.type != TYPE_FILE) {
            fprintf(stderr, "Runtime error: eof argument must be a file variable.\n");
            EXIT_FAILURE_HANDLER();
        }
        if (fileVal.f_val == NULL) {
            fprintf(stderr, "Runtime error: file is not open.\n");
            EXIT_FAILURE_HANDLER();
        }
        f = fileVal.f_val;
        freeValue(&fileVal);
    } else {
        fprintf(stderr, "Runtime error: eof expects 0 or 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }

    int is_eof = feof(f);
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
    Value result = makeVoid(); // Declare result value

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
        // Terminal state is likely messed up now!
        // EXIT_FAILURE_HANDLER(); // Consider uncommenting if this happens
    }

    // --- Handle read result ---
    if (bytes_read < 0) {
        perror("ReadKey Error: read failed");
        return makeString("");
    }
    else if (bytes_read == 0) {
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
    return makeValueForType(TYPE_INTEGER, NULL, NULL);
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
            fprintf(stderr, "Runtime error: Random expects 0 or 1 argument.\n");
            EXIT_FAILURE_HANDLER();
        }
    } else {
        fprintf(stderr, "Runtime error: Random expects 0 or 1 argument.\n");
        EXIT_FAILURE_HANDLER();
    }
    return makeValueForType(TYPE_INTEGER, NULL, NULL);
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
    freeValue(&msVal); // Free evaluated value

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
        fprintf(stderr, "Runtime error: first parameter of SaveToFile must be a Type MStream.\n");
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

    // --- Functions Returning REAL ---
    if (strcasecmp(name, "cos") == 0 ||
        strcasecmp(name, "sin") == 0 ||
        strcasecmp(name, "tan") == 0 ||
        strcasecmp(name, "sqrt") == 0 ||
        strcasecmp(name, "ln") == 0 ||
        strcasecmp(name, "exp") == 0 ||
        strcasecmp(name, "real") == 0) {

        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_REAL);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_param1_real", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "real", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL); setRight(dummy, retNode); dummy->var_type = TYPE_REAL;
    }
    // --- Functions Returning INTEGER ---
    else if (strcasecmp(name, "abs") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_REAL);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_abs_arg", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "pos") == 0) {
        dummy->child_capacity = 2;
        dummy->children = malloc(sizeof(AST*) * 2);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_pos_substr", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_STRING); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_pos_str", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "ioresult") == 0 ||
             strcasecmp(name, "paramcount") == 0 ||
             strcasecmp(name, "getmaxx") == 0 ||
             strcasecmp(name, "getmaxy") == 0 ||
             strcasecmp(name, "screencols") == 0 ||
             strcasecmp(name, "screenrows") == 0 ||
             strcasecmp(name, "ord") == 0 ||
             strcasecmp(name, "round") == 0 ||
             strcasecmp(name, "trunc") == 0 ||
             strcasecmp(name, "length") == 0 ||
             strcasecmp(name, "wherex") == 0 ||
             strcasecmp(name, "wherey") == 0 ||
             strcasecmp(name, "createtexture") == 0 ||
             strcasecmp(name, "loadsound") == 0 ) {

        if (strcasecmp(name, "ord") == 0 || strcasecmp(name, "round") == 0 || strcasecmp(name, "trunc") == 0 || strcasecmp(name, "length") == 0 || strcasecmp(name, "loadsound") == 0) {
            dummy->child_capacity = 1;
            dummy->children = malloc(sizeof(AST*));
            if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
            AST* p1 = newASTNode(AST_VAR_DECL, NULL);
            if (strcasecmp(name, "length") == 0 || strcasecmp(name, "loadsound") == 0) setTypeAST(p1, TYPE_STRING);
            else if (strcasecmp(name, "round") == 0 || strcasecmp(name, "trunc") == 0) setTypeAST(p1, TYPE_REAL);
            else setTypeAST(p1, TYPE_CHAR);

            Token* pn1 = newToken(TOKEN_IDENTIFIER, "_param1", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
            dummy->children[0] = p1; dummy->child_count = 1;
        } else if (strcasecmp(name, "createtexture") == 0) {
             dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
             AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* p1n = newToken(TOKEN_IDENTIFIER, "_ct_w", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
             AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* p2n = newToken(TOKEN_IDENTIFIER, "_ct_h", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2); dummy->children[1] = p2;
             dummy->child_count = 2;
        }
        else {
            dummy->child_count = 0;
        }

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    // --- Functions Returning CHAR ---
    else if (strcasecmp(name, "upcase") == 0 ||
             strcasecmp(name, "chr") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, (strcasecmp(name, "chr") == 0 ? TYPE_INTEGER : TYPE_CHAR) );
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_param1_char_int", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "char", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_CHAR); setRight(dummy, retNode); dummy->var_type = TYPE_CHAR;
    }
    // --- Functions Returning STRING ---
    else if (strcasecmp(name, "paramstr") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_paramstr_idx", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcasecmp(name, "readkey") == 0) {
        dummy->child_count = 0;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcasecmp(name, "copy") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_cpy_s", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_cpy_idx", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        AST* p3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3, TYPE_INTEGER); Token* pn3 = newToken(TOKEN_IDENTIFIER, "_cpy_cnt", 0, 0); AST* v3 = newASTNode(AST_VARIABLE, pn3); freeToken(pn3); addChild(p3,v3); dummy->children[2] = p3;
        dummy->child_count = 3;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcasecmp(name, "inttostr") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_itos_val", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    else if (strcmp(name, "api_receive") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_MEMORYSTREAM); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_apirecv_ms", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "string", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING); setRight(dummy, retNode); dummy->var_type = TYPE_STRING;
    }
    // --- Functions Returning BOOLEAN ---
    else if (strcasecmp(name, "keypressed") == 0 ||
             strcasecmp(name, "quitrequested") == 0 ||
             strcasecmp(name, "issoundplaying") == 0 ||
             strcasecmp(name, "eof") == 0 ) {
        dummy->child_count = 0;
        if (strcasecmp(name, "eof") == 0) {
            dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*)); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
            AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_FILE);
            p1->by_ref = 1; // eof allows optional VAR file parameter
            Token* pn1 = newToken(TOKEN_IDENTIFIER, "_eof_fvar", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
            dummy->children[0] = p1; dummy->child_count = 1;
            dummy->i_val = 0; // zero required parameters
        }
        Token* retTok = newToken(TOKEN_IDENTIFIER, "boolean", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_BOOLEAN); setRight(dummy, retNode); dummy->var_type = TYPE_BOOLEAN;
    }
    // --- Functions Returning MEMORYSTREAM ---
    else if (strcasecmp(name, "api_send") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_apisend_url", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_STRING); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_apisend_body", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "mstream", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_MEMORYSTREAM); setRight(dummy, retNode); dummy->var_type = TYPE_MEMORYSTREAM;
    }
    else if (strcasecmp(name, "mstreamcreate") == 0) {
        dummy->child_count = 0;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "mstream", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_MEMORYSTREAM); setRight(dummy, retNode); dummy->var_type = TYPE_MEMORYSTREAM;
    }
    // --- Procedures with VAR parameters ---
    else if (strcasecmp(name, "mstreamloadfromfile") == 0 ||
             strcasecmp(name, "mstreamsavetofile") == 0 ||
             strcasecmp(name, "mstreamfree") == 0) {
        dummy->child_capacity = 2;
        dummy->children = malloc(sizeof(AST*) * dummy->child_capacity);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }

        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, TYPE_MEMORYSTREAM);
        p1->by_ref = 1; // **FIX**: MStream argument is VAR
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_ms_var", 0, 0);
        AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1);
        addChild(p1,v1);
        dummy->children[0] = p1;
        dummy->child_count = 1;

        if (strcasecmp(name, "mstreamloadfromfile") == 0 ||
            strcasecmp(name, "mstreamsavetofile") == 0) {
            AST* p2 = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(p2, TYPE_STRING);
            Token* pn2 = newToken(TOKEN_IDENTIFIER, "_filename_str", 0, 0);
            AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2);
            addChild(p2,v2);
            dummy->children[1] = p2;
            dummy->child_count = 2;
        }
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "assign") == 0 || strcasecmp(name, "reset") == 0 || strcasecmp(name, "rewrite") == 0 || strcasecmp(name, "close") == 0) {
        // **FIX**: Handle file procedures requiring VAR parameters
        dummy->child_capacity = 2; // Max for assign
        dummy->children = malloc(sizeof(AST*) * 2);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }

        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, TYPE_FILE);
        p1->by_ref = 1; // Mark as VAR
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_file_var", 0, 0);
        AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1);
        addChild(p1, v1);
        dummy->children[0] = p1;

        if (strcasecmp(name, "assign") == 0) {
            AST* p2 = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(p2, TYPE_STRING);
            Token* pn2 = newToken(TOKEN_IDENTIFIER, "_filename", 0, 0);
            AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2);
            addChild(p2, v2);
            dummy->children[1] = p2;
            dummy->child_count = 2;
        } else {
            dummy->child_count = 1;
        }
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "new") == 0 || strcasecmp(name, "dispose") == 0 || strcasecmp(name, "inc") == 0 || strcasecmp(name, "dec") == 0) {
        // **FIX**: Handle new/dispose/inc/dec requiring VAR parameters
        dummy->child_capacity = 2;
        dummy->children = malloc(sizeof(AST*) * 2);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }

        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        if (strcasecmp(name, "new") == 0 || strcasecmp(name, "dispose") == 0) {
            setTypeAST(p1, TYPE_POINTER);
            AST* ptrType = newASTNode(AST_POINTER_TYPE, NULL); // Generic pointer (no subtype)
            setTypeAST(ptrType, TYPE_POINTER);
            p1->type_def = ptrType;
            setRight(p1, ptrType);
        } else {
            setTypeAST(p1, TYPE_INTEGER); // Placeholder for any ordinal
        }
        p1->by_ref = 1; // Mark as VAR
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_var_to_modify", 0, 0);
        AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1);
        addChild(p1, v1);
        dummy->children[0] = p1;

        if (strcasecmp(name, "inc") == 0 || strcasecmp(name, "dec") == 0) {
            AST* p2 = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(p2, TYPE_INTEGER);
            Token* pn2 = newToken(TOKEN_IDENTIFIER, "_amount", 0, 0);
            AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2);
            addChild(p2, v2);
            dummy->children[1] = p2;
        }
        // Only the first parameter is required; the second is optional.
        dummy->child_count = 1;
        dummy->var_type = TYPE_VOID;
    }
    // --- Ordinal functions (Low, High, Succ) ---
    else if (strcasecmp(name, "low") == 0 || strcasecmp(name, "high") == 0 || strcasecmp(name, "succ") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_ord_type_arg", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1; dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0);
        AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    // --- Procedures with specific params (getmousestate, etc.) ---
    else if (strcasecmp(name, "getmousestate") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        const char* pnames[] = {"_gms_x", "_gms_y", "_gms_b"};
        for(int i=0; i<3; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p, TYPE_INTEGER); p->by_ref = 1; // VAR params
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i], 0, 0); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 3;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "textcolor") == 0 ||
             strcasecmp(name, "textbackground") == 0 ||
             strcasecmp(name, "textcolore") == 0 ||
             strcasecmp(name, "textbackgrounde") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, TYPE_INTEGER);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_color", 0, 0);
        AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1);
        addChild(p1, v1);
        dummy->children[0] = p1;
        dummy->child_count = 1;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "gotoxy") == 0) {
        dummy->child_capacity = 2;
        dummy->children = malloc(sizeof(AST*) * 2);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_x", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1, v1);
        dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER);
        Token* pn2 = newToken(TOKEN_IDENTIFIER, "_y", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2, v2);
        dummy->children[1] = p2;
        dummy->child_count = 2;
        dummy->var_type = TYPE_VOID;
    }
    // --- Special case functions (Random, Sqr) ---
    else if (strcasecmp(name, "random") == 0) {
        dummy->child_count = 0;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "real", 0, 0);
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_REAL;
    }
    else if (strcasecmp(name, "sqr") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL);
        setTypeAST(p1, TYPE_REAL);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_sqr_arg", 0, 0);
        AST* v1 = newASTNode(AST_VARIABLE, pn1);
        freeToken(pn1);
        addChild(p1, v1);
        dummy->children[0] = p1;
        dummy->child_count = 1;

        Token* retTok = newToken(TOKEN_IDENTIFIER, "real", 0, 0);
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_REAL);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_REAL;
    }
#ifdef SDL
    // --- SDL/Graphics functions and procedures ---
    else if (strcasecmp(name, "loadimagetotexture") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*));
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_filePath", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "gettextsize") == 0) {
        dummy->child_capacity = 3; dummy->children = malloc(sizeof(AST*) * 3);
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_STRING); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_text", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); p2->by_ref = 1; Token* pn2 = newToken(TOKEN_IDENTIFIER, "_width", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        AST* p3 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3, TYPE_INTEGER); p3->by_ref = 1; Token* pn3 = newToken(TOKEN_IDENTIFIER, "_height", 0, 0); AST* v3 = newASTNode(AST_VARIABLE, pn3); freeToken(pn3); addChild(p3,v3); dummy->children[2] = p3;
        dummy->child_count = 3;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "setrendertarget") == 0) {
        dummy->child_capacity = 1; dummy->children = malloc(sizeof(AST*));
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_textureID", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1; dummy->child_count = 1;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "drawpolygon") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2);
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_ARRAY); Token* pn1 = newToken(TOKEN_IDENTIFIER, "_points", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* pn2 = newToken(TOKEN_IDENTIFIER, "_numPoints", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, pn2); freeToken(pn2); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "getpixelcolor") == 0) {
        dummy->child_capacity = 6; dummy->children = malloc(sizeof(AST*) * 6);
        const char* pnames[] = {"_x", "_y", "_r", "_g", "_b", "_a"};
        for(int i=0; i<6; ++i) {
            AST* p = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(p, (i < 2) ? TYPE_INTEGER : TYPE_BYTE);
            if (i >= 2) p->by_ref = 1; // R,G,B,A are VAR
            Token* pn = newToken(TOKEN_IDENTIFIER, pnames[i], 0, 0); AST* v = newASTNode(AST_VARIABLE, pn); freeToken(pn); addChild(p,v);
            dummy->children[i] = p;
        }
        dummy->child_count = 6;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "createtargettexture") == 0) {
        dummy->child_capacity = 2; dummy->children = malloc(sizeof(AST*) * 2); if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_INTEGER); Token* p1n = newToken(TOKEN_IDENTIFIER, "_ctt_w", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n); addChild(p1,v1); dummy->children[0] = p1;
        AST* p2 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2, TYPE_INTEGER); Token* p2n = newToken(TOKEN_IDENTIFIER, "_ctt_h", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n); addChild(p2,v2); dummy->children[1] = p2;
        dummy->child_count = 2;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0); AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER); setRight(dummy, retNode); dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "setalphablend") == 0) {
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1 = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1, TYPE_BOOLEAN);
        Token* pn1 = newToken(TOKEN_IDENTIFIER, "_enable", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, pn1); freeToken(pn1); addChild(p1,v1);
        dummy->children[0] = p1;
        dummy->child_count = 1;
        dummy->var_type = TYPE_VOID;
    }
    else if (strcasecmp(name, "rendercopyex") == 0) {
        dummy->child_capacity = 13;
        dummy->children = malloc(sizeof(AST*) * 13);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        const char* pnames[] = { "_textureID", "_srcX", "_srcY", "_srcW", "_srcH", "_dstX", "_dstY", "_dstW", "_dstH", "_angle", "_rotationCenterX", "_rotationCenterY", "_flipMode" };
        VarType ptypes[] = { TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER, TYPE_REAL, TYPE_INTEGER, TYPE_INTEGER, TYPE_INTEGER };
        for(int k=0; k < 13; ++k) {
            AST* param_var_decl_node = newASTNode(AST_VAR_DECL, NULL);
            setTypeAST(param_var_decl_node, ptypes[k]);
            Token* param_name_token = newToken(TOKEN_IDENTIFIER, pnames[k], 0, 0);
            AST* param_var_name_node = newASTNode(AST_VARIABLE, param_name_token);
            setTypeAST(param_var_name_node, ptypes[k]);
            freeToken(param_name_token);
            addChild(param_var_decl_node, param_var_name_node);
            dummy->children[k] = param_var_decl_node;
        }
        dummy->child_count = 13;
        dummy->var_type = TYPE_VOID;
    }
#endif
    else if (strcasecmp(name, "getticks") == 0) {
        dummy->child_count = 0;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "cardinal_or_integer", 0, 0);
        AST* retNode = newASTNode(AST_VARIABLE, retTok);
        freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_INTEGER;
    }
    else if (strcasecmp(name, "realtostr") == 0) {
        dummy->type = AST_FUNCTION_DECL;
        dummy->child_capacity = 1;
        dummy->children = malloc(sizeof(AST*));
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1_decl = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1_decl, TYPE_REAL);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_valueR", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n);
        setTypeAST(v1, TYPE_REAL); addChild(p1_decl, v1);
        dummy->children[0] = p1_decl;
        dummy->child_count = 1;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "string_result", 0, 0);
        AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_STRING);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_STRING;
    }
#ifdef SDL
    else if (strcasecmp(name, "rendertexttotexture") == 0) {
        dummy->type = AST_FUNCTION_DECL;
        dummy->child_capacity = 4;
        dummy->children = malloc(sizeof(AST*) * 4);
        if (!dummy->children) { EXIT_FAILURE_HANDLER(); }
        AST* p1_decl = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p1_decl, TYPE_STRING);
        Token* p1n = newToken(TOKEN_IDENTIFIER, "_textToRender", 0, 0); AST* v1 = newASTNode(AST_VARIABLE, p1n); freeToken(p1n);
        setTypeAST(v1, TYPE_STRING); addChild(p1_decl, v1);
        dummy->children[0] = p1_decl;
        AST* p2_decl = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p2_decl, TYPE_BYTE);
        Token* p2n = newToken(TOKEN_IDENTIFIER, "_red", 0, 0); AST* v2 = newASTNode(AST_VARIABLE, p2n); freeToken(p2n);
        setTypeAST(v2, TYPE_BYTE); addChild(p2_decl, v2);
        dummy->children[1] = p2_decl;
        AST* p3_decl = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p3_decl, TYPE_BYTE);
        Token* p3n = newToken(TOKEN_IDENTIFIER, "_green", 0, 0); AST* v3 = newASTNode(AST_VARIABLE, p3n); freeToken(p3n);
        setTypeAST(v3, TYPE_BYTE); addChild(p3_decl, v3);
        dummy->children[2] = p3_decl;
        AST* p4_decl = newASTNode(AST_VAR_DECL, NULL); setTypeAST(p4_decl, TYPE_BYTE);
        Token* p4n = newToken(TOKEN_IDENTIFIER, "_blue", 0, 0); AST* v4 = newASTNode(AST_VARIABLE, p4n); freeToken(p4n);
        setTypeAST(v4, TYPE_BYTE); addChild(p4_decl, v4);
        dummy->children[3] = p4_decl;
        dummy->child_count = 4;
        Token* retTok = newToken(TOKEN_IDENTIFIER, "_textureID_result", 0, 0);
        AST* retNode = newASTNode(AST_VARIABLE, retTok); freeToken(retTok);
        setTypeAST(retNode, TYPE_INTEGER);
        setRight(dummy, retNode);
        dummy->var_type = TYPE_INTEGER;
    }
#endif
    // --- Default / Unhandled ---
    else {
        if (dummy->type == AST_FUNCTION_DECL) {
            // This is a function we haven't explicitly configured.
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

    Token *funcNameToken = newToken(TOKEN_IDENTIFIER, lowerNameCopy, 0, 0); // Token value is lowercase
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
    addProcedure(dummy, unit_context_name_param_for_addproc, procedure_table);
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
    if (argNode->type != AST_VARIABLE || !argNode->token) {
        fprintf(stderr, "Runtime error: Low argument must be a valid type identifier. Got AST type %s\n", astTypeToString(argNode->type));
        EXIT_FAILURE_HANDLER();
    }

    Value arg = makeString(argNode->token->value);
    Value result = vm_builtin_low(NULL, 1, &arg);
    freeValue(&arg);
    return result;
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
    if (argNode->type != AST_VARIABLE || !argNode->token) {
        fprintf(stderr, "Runtime error: High argument must be a valid type identifier. Got AST type %s\n", astTypeToString(argNode->type));
        EXIT_FAILURE_HANDLER();
    }

    Value arg = makeString(argNode->token->value);
    Value result = vm_builtin_high(NULL, 1, &arg);
    freeValue(&arg);
    return result;
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
            {
                 currentOrdinal = argVal.enum_val.ordinal;
                 if (argVal.enum_meta) {
                     maxOrdinal = argVal.enum_meta->member_count - 1;
                     checkMax = true;
                 } else {
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
                 }
                 if (currentOrdinal >= maxOrdinal && checkMax) { goto succ_overflow; }
                 Value nextEnum = makeEnum(argVal.enum_val.enum_name, (int)(currentOrdinal + 1));
                 nextEnum.enum_meta = argVal.enum_meta;
                 nextEnum.base_type_node = argVal.base_type_node;
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
        "upcase", "low", "high", "succ", "pred", "round",
        "inttostr", "api_send", "api_receive", "screencols", "screenrows",
        "keypressed", "mstreamcreate", "quitrequested", "loadsound",
        "real", "readkey", "getmaxx", "getmaxy", "getticks", "sqr",
        "realtostr", "createtexture", "createtargettexture",
        "loadimagetotexture", "rendertexttotexture"
        
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
         // Existing procedures
        "assign", "cleardevice", "close", "closegraph", "dec", "delay",
        "destroytexture", "dispose", "drawcircle", "drawline",
        "drawpolygon", "drawrect", "exit", "fillcircle", "fillrect",
        "getmousestate", "gettextsize", "gotoxy", "graphloop", "halt", "inc",
        "initgraph", "initsoundsystem", "inittextsystem", "mstreamfree",
        "mstreamloadfromfile", "mstreamsavetofile", "new", "outtextxy",
        "playsound", "putpixel", "quitsoundsystem", "quittextsystem",
        "randomize", "read", "readln", "rendercopy", "rendercopyex", "rendercopyrect", "reset", "rewrite", "setalphablend",
         "setcolor", "setrendertarget", "setrgbcolor",
         "textbackground", "textbackgrounde", "textcolor",
         "textcolore", "updatescreen", "updatetexture", "waitkeyevent",
         "write", "writeln",
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

Value executeBuiltinGotoXY(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: GotoXY expects 2 arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value xVal = eval(node->children[0]);
    Value yVal = eval(node->children[1]);
    if ((xVal.type != TYPE_INTEGER && xVal.type != TYPE_BYTE) ||
        (yVal.type != TYPE_INTEGER && yVal.type != TYPE_BYTE)) {
        freeValue(&xVal);
        freeValue(&yVal);
        EXIT_FAILURE_HANDLER();
    }
    long long x = xVal.i_val;
    long long y = yVal.i_val;
    freeValue(&xVal);
    freeValue(&yVal);
    printf("\x1B[%lld;%lldH", y, x);
    fflush(stdout);
    return makeVoid();
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
    *(allocated_memory) = makeValueForType(baseVarType, actualBaseTypeDef, NULL); // Use assignment to copy the returned Value.

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

     // Set the original pointer variable to nil
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

    if (chars_written < 0 || (size_t)chars_written >= sizeof(buffer)) {
        fprintf(stderr, "Runtime error: Failed to convert real to string in RealToStr (snprintf error or buffer too small).\n");
        freeValue(&arg); // Free the evaluated argument
        return makeString("Error:RealToStrConversion");
    }

    Value result = makeString(buffer); // makeString copies the buffer
    freeValue(&arg);                   // Free the evaluated argument

    return result;
}

// --- ADDED: Definition for getBuiltinIDForCompiler ---
// Returns the index in builtin_dispatch_table or -1 if not found.
// This function needs to be declared in builtin.h as well.
int getBuiltinIDForCompiler(const char *name) {
    if (!name) return -1;

    // The builtin_dispatch_table is static const in this file.
    // num_builtins is also static const.
    for (size_t i = 0; i < num_builtins; i++) {
        // Assuming BuiltinMapping struct has a 'name' field.
        // Using strcasecmp for case-insensitive comparison, matching compareBuiltinMappings.
        if (strcasecmp(name, builtin_dispatch_table[i].name) == 0) {
            return (int)i; // Found, return index
        }
    }
    return -1; // Not found
}
Value executeBuiltinExit(AST *node) {
    // Mark that the current procedure should return early
    if (node->child_count > 0) {
        fprintf(stderr, "Runtime error: exit does not take arguments.\n");
        EXIT_FAILURE_HANDLER();
    }
    // In the AST interpreter we simply note the exit; the interpreter loop
    // is responsible for unwinding appropriately.
    return makeVoid();
}

// VM Versions

Value vm_builtin_inttostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "IntToStr requires 1 argument."); return makeString(""); }
    Value arg = args[0];
    long long value_to_convert = 0;
    switch (arg.type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            value_to_convert = arg.i_val;
            break;
        case TYPE_CHAR:
            value_to_convert = (long long)arg.c_val;
            break;
        default: runtimeError(vm, "IntToStr requires an integer-compatible argument."); return makeString("");
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%lld", value_to_convert);
    return makeString(buffer);
}

Value vm_builtin_length(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) { runtimeError(vm, "Length requires 1 string argument."); return makeInt(0); }
    return makeInt(args[0].s_val ? strlen(args[0].s_val) : 0);
}

Value vm_builtin_abs(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "abs expects 1 argument."); return makeInt(0); }
    if (args[0].type == TYPE_INTEGER) return makeInt(llabs(args[0].i_val));
    if (args[0].type == TYPE_REAL) return makeReal(fabs(args[0].r_val));
    runtimeError(vm, "abs expects a numeric argument.");
    return makeInt(0);
}

Value vm_builtin_round(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Round expects 1 argument."); return makeInt(0); }
    if (args[0].type == TYPE_REAL) return makeInt((long long)round(args[0].r_val));
    if (args[0].type == TYPE_INTEGER) return makeInt(args[0].i_val);
    runtimeError(vm, "Round expects a numeric argument.");
    return makeInt(0);
}

Value vm_builtin_halt(VM* vm, int arg_count, Value* args) {
    long long code = 0;
    if (arg_count == 1 && args[0].type == TYPE_INTEGER) {
        code = args[0].i_val;
    } else if (arg_count > 1) {
        runtimeError(vm, "Halt expects 0 or 1 integer argument.");
    }
    exit((int)code);
    return makeVoid(); // Unreachable
}

Value vm_builtin_delay(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || (args[0].type != TYPE_INTEGER && args[0].type != TYPE_WORD)) {
        runtimeError(vm, "Delay requires an integer or word argument.");
        return makeVoid();
    }
    long long ms = args[0].i_val;
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return makeVoid();
}

// Looks up a built-in by name and returns its C function handler.
BuiltinHandler getBuiltinHandler(const char *name) {
    if (!name) return NULL;

    BuiltinMapping *found = (BuiltinMapping *)bsearch(
        name,
        builtin_dispatch_table,
        num_builtins,
        sizeof(BuiltinMapping),
        compareBuiltinMappings
    );

    if (found) {
        return found->handler;
    }
    return NULL;
}
