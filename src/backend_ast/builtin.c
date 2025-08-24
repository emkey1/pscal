#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#ifdef SDL
#include "backend_ast/sdl.h"
#include "backend_ast/audio.h"
#endif
#include "globals.h"                  // Assuming globals.h is directly in src/
#include "backend_ast/builtin_network_api.h"
#include "vm/vm.h"

// Standard library includes remain the same
#include <math.h>
#include <termios.h> // For tcgetattr, tcsetattr, etc. (Terminal I/O)
#include <signal.h>  // For signal handling (SIGINT)
#include <unistd.h>  // For read, write, STDIN_FILENO, STDOUT_FILENO, isatty
#include <ctype.h>   // For isdigit
#include <errno.h>   // For errno
#include <sys/ioctl.h> // For ioctl, FIONREAD (Terminal I/O)
#include <stdint.h>  // For fixed-width integer types like uint8_t
#include <stdbool.h> // For bool, true, false (IMPORTANT - GCC needs this for 'bool')
#include <string.h>  // For strlen, strdup
#include <strings.h> // For strcasecmp
#include <dirent.h>  // For directory traversal
#include <sys/stat.h> // For file attributes
#include <stdlib.h>  // For system(), getenv, malloc
#include <time.h>    // For date/time functions
#include <sys/time.h> // For gettimeofday
#include <stdio.h>   // For printf, fprintf

static DIR* dos_dir = NULL; // Used by dos_findfirst/findnext

// Terminal cursor helper
static int getCursorPosition(int *row, int *col);

// The new dispatch table for the VM - MUST be defined before the function that uses it
// This list MUST BE SORTED ALPHABETICALLY BY NAME (lowercase).
static const VmBuiltinMapping vmBuiltinDispatchTable[] = {
    {"abs", vmBuiltinAbs},
    {"api_receive", vmBuiltinApiReceive},
    {"api_send", vmBuiltinApiSend},
    {"assign", vmBuiltinAssign},
    {"beep", vmBuiltinBeep},
    {"biblinktext", vmBuiltinBlinktext},
    {"biboldtext", vmBuiltinBoldtext},
    {"biclrscr", vmBuiltinClrscr},
    {"bilowvideo", vmBuiltinLowvideo},
    {"binormvideo", vmBuiltinNormvideo},
    {"biunderlinetext", vmBuiltinUnderlinetext},
    {"biwherex", vmBuiltinWherex},
    {"biwherey", vmBuiltinWherey},
    {"blinktext", vmBuiltinBlinktext},
    {"boldtext", vmBuiltinBoldtext},
    {"chr", vmBuiltinChr},
#ifdef SDL
    {"cleardevice", vmBuiltinCleardevice},
#endif
    {"clreol", vmBuiltinClreol},
    {"clrscr", vmBuiltinClrscr},
    {"close", vmBuiltinClose},
#ifdef SDL
    {"closegraph", vmBuiltinClosegraph},
#endif
    {"copy", vmBuiltinCopy},
    {"cos", vmBuiltinCos},
    {"cursoroff", vmBuiltinCursoroff},
    {"cursoron", vmBuiltinCursoron},
#ifdef SDL
    {"createtargettexture", vmBuiltinCreatetargettexture}, // Moved
    {"createtexture", vmBuiltinCreatetexture}, // Moved
#endif
    {"dec", vmBuiltinDec},
    {"delay", vmBuiltinDelay},
    {"deline", vmBuiltinDeline},
#ifdef SDL
    {"destroytexture", vmBuiltinDestroytexture},
#endif
    {"dispose", vmBuiltinDispose},
    {"dos_exec", vmBuiltinDosExec},
    {"dos_findfirst", vmBuiltinDosFindfirst},
    {"dos_findnext", vmBuiltinDosFindnext},
    {"dos_getdate", vmBuiltinDosGetdate},
    {"dos_getenv", vmBuiltinDosGetenv},
    {"dos_getfattr", vmBuiltinDosGetfattr},
    {"dos_gettime", vmBuiltinDosGettime},
    {"dos_mkdir", vmBuiltinDosMkdir},
    {"dos_rmdir", vmBuiltinDosRmdir},
#ifdef SDL
    {"drawcircle", vmBuiltinDrawcircle}, // Moved
    {"drawline", vmBuiltinDrawline}, // Moved
    {"drawpolygon", vmBuiltinDrawpolygon}, // Moved
    {"drawrect", vmBuiltinDrawrect}, // Moved
#endif
    {"eof", vmBuiltinEof},
    {"exit", vmBuiltinExit},
    {"exp", vmBuiltinExp},
#ifdef SDL
    {"fillcircle", vmBuiltinFillcircle},
    {"fillrect", vmBuiltinFillrect},
    {"freesound", vmBuiltinFreesound},
#endif
    {"getdate", vmBuiltinDosGetdate},
    {"getenv", vmBuiltinGetenv},
    {"getenvint", vmBuiltinGetenvint},
#ifdef SDL
    {"getmaxx", vmBuiltinGetmaxx},
    {"getmaxy", vmBuiltinGetmaxy},
    {"getmousestate", vmBuiltinGetmousestate},
    {"getpixelcolor", vmBuiltinGetpixelcolor}, // Moved
    {"gettextsize", vmBuiltinGettextsize},
    {"getticks", vmBuiltinGetticks},
#endif
    {"gettime", vmBuiltinDosGettime},
#ifdef SDL
    {"graphloop", vmBuiltinGraphloop},
#endif
    {"gotoxy", vmBuiltinGotoxy},
    {"halt", vmBuiltinHalt},
    {"hidecursor", vmBuiltinHidecursor},
    {"high", vmBuiltinHigh},
    {"highvideo", vmBuiltinHighvideo},
    {"inc", vmBuiltinInc},
#ifdef SDL
    {"initgraph", vmBuiltinInitgraph},
#endif
#ifdef SDL
    {"initsoundsystem", vmBuiltinInitsoundsystem},
    {"inittextsystem", vmBuiltinInittextsystem},
#endif
    {"insline", vmBuiltinInsline},
    {"inttostr", vmBuiltinInttostr},
    {"invertcolors", vmBuiltinInvertcolors},
    {"ioresult", vmBuiltinIoresult},
#ifdef SDL
    {"issoundplaying", vmBuiltinIssoundplaying}, // Moved
#endif
    {"keypressed", vmBuiltinKeypressed},
    {"length", vmBuiltinLength},
    {"ln", vmBuiltinLn},
#ifdef SDL
    {"loadimagetotexture", vmBuiltinLoadimagetotexture}, // Moved
    {"loadsound", vmBuiltinLoadsound},
#endif
    {"low", vmBuiltinLow},
    {"lowvideo", vmBuiltinLowvideo},
    {"mstreamcreate", vmBuiltinMstreamcreate},
    {"mstreamfree", vmBuiltinMstreamfree},
    {"mstreamloadfromfile", vmBuiltinMstreamloadfromfile},
    {"mstreamsavetofile", vmBuiltinMstreamsavetofile},
    {"mstreambuffer", vmBuiltinMstreambuffer},
    {"new", vmBuiltinNew},
    {"normalcolors", vmBuiltinNormalcolors},
    {"normvideo", vmBuiltinNormvideo},
    {"ord", vmBuiltinOrd},
#ifdef SDL
    {"outtextxy", vmBuiltinOuttextxy}, // Moved
#endif
    {"paramcount", vmBuiltinParamcount},
    {"paramstr", vmBuiltinParamstr},
#ifdef SDL
    {"playsound", vmBuiltinPlaysound},
    {"pollkey", vmBuiltinPollkey},
#endif
    {"popscreen", vmBuiltinPopscreen},
    {"pos", vmBuiltinPos},
    {"pushscreen", vmBuiltinPushscreen},
#ifdef SDL
    {"putpixel", vmBuiltinPutpixel},
#endif
    {"quitrequested", vmBuiltinQuitrequested},
#ifdef SDL
    {"quitsoundsystem", vmBuiltinQuitsoundsystem},
    {"quittextsystem", vmBuiltinQuittextsystem},
#endif
    {"random", vmBuiltinRandom},
    {"randomize", vmBuiltinRandomize},
    {"read", vmBuiltinRead},
    {"readkey", vmBuiltinReadkey},
    {"readln", vmBuiltinReadln},
    {"real", vmBuiltinReal},
    {"realtostr", vmBuiltinRealtostr},
#ifdef SDL
    {"rendercopy", vmBuiltinRendercopy}, // Moved
    {"rendercopyex", vmBuiltinRendercopyex}, // Moved
    {"rendercopyrect", vmBuiltinRendercopyrect},
    {"rendertexttotexture", vmBuiltinRendertexttotexture},
#endif
    {"reset", vmBuiltinReset},
    {"restorecursor", vmBuiltinRestorecursor},
    {"rewrite", vmBuiltinRewrite},
    {"round", vmBuiltinRound},
    {"savecursor", vmBuiltinSavecursor},
    {"screencols", vmBuiltinScreencols},
    {"screenrows", vmBuiltinScreenrows},
    {"setlength", vmBuiltinSetlength},
#ifdef SDL
    {"setalphablend", vmBuiltinSetalphablend},
    {"setcolor", vmBuiltinSetcolor}, // Moved
    {"setrendertarget", vmBuiltinSetrendertarget}, // Moved
#endif
#ifdef SDL
    {"setrgbcolor", vmBuiltinSetrgbcolor},
#endif
    {"showcursor", vmBuiltinShowcursor},
    {"sin", vmBuiltinSin},
    {"sqr", vmBuiltinSqr},
    {"sqrt", vmBuiltinSqrt},
    {"succ", vmBuiltinSucc},
    {"tan", vmBuiltinTan},
    {"textbackground", vmBuiltinTextbackground},
    {"textbackgrounde", vmBuiltinTextbackgrounde},
    {"textcolor", vmBuiltinTextcolor},
    {"textcolore", vmBuiltinTextcolore},
    {"trunc", vmBuiltinTrunc},
    {"underlinetext", vmBuiltinUnderlinetext},
    {"upcase", vmBuiltinUpcase},
 #ifdef SDL
    {"updatescreen", vmBuiltinUpdatescreen},
    {"updatetexture", vmBuiltinUpdatetexture},
#endif
    {"val", vmBuiltinVal},
    {"valreal", vmBuiltinValreal},
#ifdef SDL
    {"waitkeyevent", vmBuiltinWaitkeyevent}, // Moved
#endif
    {"window", vmBuiltinWindow},
    {"wherex", vmBuiltinWherex},
    {"wherey", vmBuiltinWherey},
};

static const size_t num_vm_builtins = sizeof(vmBuiltinDispatchTable) / sizeof(vmBuiltinDispatchTable[0]);

/* Dynamic registry for user-supplied VM built-ins. */
static VmBuiltinMapping *extra_vm_builtins = NULL;
static size_t num_extra_vm_builtins = 0;

void registerVmBuiltin(const char *name, VmBuiltinFn handler) {
    if (!name || !handler) return;
    VmBuiltinMapping *new_table = realloc(extra_vm_builtins,
        sizeof(VmBuiltinMapping) * (num_extra_vm_builtins + 1));
    if (!new_table) return;
    extra_vm_builtins = new_table;
    extra_vm_builtins[num_extra_vm_builtins].name = strdup(name);
    extra_vm_builtins[num_extra_vm_builtins].handler = handler;
    num_extra_vm_builtins++;
}

/* Weak hook that external modules can override to register additional
 * built-ins.  The default implementation does nothing. */
__attribute__((weak)) void registerExtendedBuiltins(void) {}

// This function now comes AFTER the table and comparison function it uses.
VmBuiltinFn getVmBuiltinHandler(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < num_vm_builtins; i++) {
        if (strcasecmp(name, vmBuiltinDispatchTable[i].name) == 0) {
            return vmBuiltinDispatchTable[i].handler;
        }
    }
    for (size_t i = 0; i < num_extra_vm_builtins; i++) {
        if (strcasecmp(name, extra_vm_builtins[i].name) == 0) {
            return extra_vm_builtins[i].handler;
        }
    }
    return NULL;
}

Value vmBuiltinSqr(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinChr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "Chr expects 1 integer argument.");
        return makeChar('\0');
    }
    return makeChar((char)args[0].i_val);
}

Value vmBuiltinSucc(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinUpcase(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_CHAR) {
        runtimeError(vm, "Upcase expects 1 char argument.");
        return makeChar('\0');
    }
    return makeChar(toupper(args[0].c_val));
}

Value vmBuiltinPos(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinCopy(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinSetlength(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_POINTER || args[1].type != TYPE_INTEGER) {
        runtimeError(vm, "SetLength expects (var string, integer).");
        return makeVoid();
    }

    Value* target = (Value*)args[0].ptr_val;
    if (!target) {
        runtimeError(vm, "SetLength received a nil pointer.");
        return makeVoid();
    }

    long long new_len = AS_INTEGER(args[1]);
    if (new_len < 0) new_len = 0;

    if (target->type != TYPE_STRING) {
        freeValue(target);
        target->type = TYPE_STRING;
        target->s_val = NULL;
        target->max_length = -1;
    }

    char* new_buf = (char*)malloc((size_t)new_len + 1);
    if (!new_buf) {
        runtimeError(vm, "SetLength: memory allocation failed.");
        return makeVoid();
    }

    size_t copy_len = 0;
    if (target->s_val) {
        copy_len = strlen(target->s_val);
        if (copy_len > (size_t)new_len) copy_len = (size_t)new_len;
        memcpy(new_buf, target->s_val, copy_len);
        free(target->s_val);
    }

    if ((size_t)new_len > copy_len) {
        memset(new_buf + copy_len, 0, (size_t)new_len - copy_len);
    }
    new_buf[new_len] = '\0';

    target->s_val = new_buf;
    target->max_length = -1;
    return makeVoid();
}

Value vmBuiltinRealtostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_REAL) {
        runtimeError(vm, "RealToStr expects 1 real argument.");
        return makeString("");
    }
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%f", AS_REAL(args[0]));
    return makeString(buffer);
}

Value vmBuiltinParamcount(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ParamCount expects 0 arguments.");
        return makeInt(0);
    }
    return makeInt(gParamCount);
}

Value vmBuiltinParamstr(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinWherex(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "WhereX expects 0 arguments.");
        return makeInt(1);
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(c - gWindowLeft + 1);
    }
    return makeInt(1); // Default on error
}

Value vmBuiltinWherey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "WhereY expects 0 arguments.");
        return makeInt(1);
    }
    int r, c;
    if (getCursorPosition(&r, &c) == 0) {
        return makeInt(r - gWindowTop + 1);
    }
    return makeInt(1); // Default on error
}

// --- Terminal helper for VM input routines ---
static struct termios vm_orig_termios;
static int vm_raw_mode = 0;
static int vm_termios_saved = 0;
static int vm_restore_registered = 0;
static int vm_alt_screen_depth = 0; // Track nested alternate screen buffers

typedef struct {
    char fg[32];
    char bg[32];
    int valid;
} VmColorState;

#define VM_COLOR_STACK_MAX 16
static VmColorState vm_color_stack[VM_COLOR_STACK_MAX];
static int vm_color_stack_depth = 0;

static void vmEnableRawMode(void); // Forward declaration
static void vmSetupTermHandlers(void);
static void vmPushColorState(void);
static void vmPopColorState(void);
static void vmRestoreColorState(void);
static int vmQueryColor(const char *query, char *dest, size_t dest_size);

static void vmRestoreTerminal(void) {
    if (vm_termios_saved && vm_raw_mode) {
        tcsetattr(STDIN_FILENO, TCSANOW, &vm_orig_termios);
        vm_raw_mode = 0;
    }
}

// Query terminal for current color (OSC 10/11) and store result in dest
static int vmQueryColor(const char *query, char *dest, size_t dest_size) {
    struct termios oldt, raw;
    char buf[64];
    size_t i = 0;
    char ch;

    if (!isatty(STDIN_FILENO))
        return -1;

    if (tcgetattr(STDIN_FILENO, &oldt) < 0)
        return -1;

    raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 5; // 0.5s timeout

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return -1;
    }

    if (write(STDOUT_FILENO, query, strlen(query)) == -1) {
        tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return -1;
    }

    while (i < sizeof(buf) - 1) {
        ssize_t n = read(STDIN_FILENO, &ch, 1);
        if (n <= 0)
            break;
        if (ch == '\a')
            break; // BEL terminator
        if (ch == '\x1B') {
            ssize_t n2 = read(STDIN_FILENO, &ch, 1);
            if (n2 <= 0)
                break;
            if (ch == '\\')
                break; // ESC \ terminator
            buf[i++] = '\x1B';
        }
        buf[i++] = ch;
    }
    buf[i] = '\0';

    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);

    char *p = strchr(buf, ';');
    if (!p)
        return -1;
    p++;
    strncpy(dest, p, dest_size - 1);
    dest[dest_size - 1] = '\0';
    return 0;
}

// Save current terminal foreground and background colors
static void vmPushColorState(void) {
    if (vm_color_stack_depth >= VM_COLOR_STACK_MAX)
        return;
    VmColorState *cs = &vm_color_stack[vm_color_stack_depth];
    cs->valid = 0;
    if (vmQueryColor("\x1B]10;?\x07", cs->fg, sizeof(cs->fg)) == 0 &&
        vmQueryColor("\x1B]11;?\x07", cs->bg, sizeof(cs->bg)) == 0) {
        cs->valid = 1;
    }
    vm_color_stack_depth++;
}

static void vmPopColorState(void) {
    if (vm_color_stack_depth > 1)
        vm_color_stack_depth--;
}

// Restore terminal colors from the top of the stack
static void vmRestoreColorState(void) {
    if (vm_color_stack_depth == 0)
        return;
    VmColorState *cs = &vm_color_stack[vm_color_stack_depth - 1];
    if (!cs->valid)
        return;
    char seq[64];
    int len = snprintf(seq, sizeof(seq), "\x1B]10;%s\x07", cs->fg);
    if (len > 0)
        write(STDOUT_FILENO, seq, len);
    len = snprintf(seq, sizeof(seq), "\x1B]11;%s\x07", cs->bg);
    if (len > 0)
        write(STDOUT_FILENO, seq, len);
}

// atexit handler: restore terminal settings and ensure cursor visibility
static void vmAtExitCleanup(void) {
    vmRestoreTerminal();
    if (isatty(STDOUT_FILENO)) {
        const char show_cursor[] = "\x1B[?25h"; // Ensure cursor is visible
        write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
        if (vm_color_stack_depth > 0)
            vm_color_stack_depth = 1; // Restore base screen colors
        vmRestoreColorState();
    }
}

// Signal handler to ensure terminal state is restored on interrupts.
static void vmSignalHandler(int signum) {
    if (vm_raw_mode || vm_alt_screen_depth > 0)
        vmAtExitCleanup();
    _exit(128 + signum);
}

static void vmSetupTermHandlers(void) {
    if (!vm_termios_saved) {
        if (tcgetattr(STDIN_FILENO, &vm_orig_termios) == 0) {
            vm_termios_saved = 1;
        }
    }
    if (!vm_restore_registered) {
        atexit(vmAtExitCleanup);
        struct sigaction sa;
        sa.sa_handler = vmSignalHandler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGINT, &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGQUIT, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        sigaction(SIGSEGV, &sa, NULL);
        vm_restore_registered = 1;
    }
}

void vmInitTerminalState(void) {
    vmSetupTermHandlers();
    vmPushColorState();
    vmEnableRawMode();
}

/*
// Pause to allow the user to read messages before the VM exits.  The pause
// occurs before any terminal cleanup is performed.
void vmPauseBeforeExit(void) {
    // Only pause when running interactively.
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO))
        return;

    sleep(10);

    fprintf(stderr, "Press any key to exit");
    fflush(stderr);

    tcflush(STDIN_FILENO, TCIFLUSH); // Discard any pending input
    vmEnableRawMode();               // Ensure we can read single key presses
    const char show_cursor[] = "\x1B[?25h";
    write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);

    char ch;
    while (read(STDIN_FILENO, &ch, 1) < 0) {
        if (errno != EINTR) {
            break; // Ignore other read errors and continue with cleanup
        }
    }
}
 */

int vmExitWithCleanup(int status) {
    // vmPauseBeforeExit();
    vmAtExitCleanup();
    return status;
}

static void vmEnableRawMode(void) {
    vmSetupTermHandlers();
    if (vm_raw_mode)
        return;

    struct termios raw = vm_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        vm_raw_mode = 1;
    }
}

// Restore the terminal to a canonical, line-buffered state suitable for
// Read()/ReadLn().  This undoes any prior ReadKey-induced raw mode,
// discards leftover input, ensures echoing, and makes the cursor visible.
static void vmPrepareCanonicalInput(void) {
    vmRestoreTerminal();
    tcflush(STDIN_FILENO, TCIFLUSH);
    const char show_cursor[] = "\x1B[?25h";
    write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1);
    fflush(stdout);
}

// Attempts to get the current cursor position using ANSI DSR query.
// Returns 0 on success, -1 on failure.
// Stores results in *row and *col.
static int getCursorPosition(int *row, int *col) {
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

Value vmBuiltinKeypressed(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "KeyPressed expects 0 arguments.");
        return makeBoolean(false);
    }
    vmEnableRawMode();

    int bytes_available = 0;
    ioctl(STDIN_FILENO, FIONREAD, &bytes_available);
    return makeBoolean(bytes_available > 0);
}

Value vmBuiltinReadkey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0 && arg_count != 1) {
        runtimeError(vm, "ReadKey expects 0 or 1 argument.");
        return makeChar('\0');
    }
    vmEnableRawMode();

    char c;
    if (read(STDIN_FILENO, &c, 1) != 1) {
        c = '\0';
    }

    if (arg_count == 1) {
        if (args[0].type != TYPE_POINTER || args[0].ptr_val == NULL) {
            runtimeError(vm, "ReadKey argument must be a VAR char.");
        } else {
            Value* dst = (Value*)args[0].ptr_val;
            if (dst->type == TYPE_CHAR) {
                dst->c_val = c;
            } else {
                runtimeError(vm, "ReadKey argument must be of type CHAR.");
            }
        }
    }

    return makeChar(c);
}

Value vmBuiltinQuitrequested(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "QuitRequested expects 0 arguments.");
        // Return a default value in case of error
        return makeBoolean(false);
    }
    // Access the global flag and return it as a Pscal boolean
    return makeBoolean(break_requested != 0);
}

Value vmBuiltinGotoxy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER) {
        runtimeError(vm, "GotoXY expects 2 integer arguments.");
        return makeVoid();
    }
    long long x = AS_INTEGER(args[0]);
    long long y = AS_INTEGER(args[1]);
    long long absX = gWindowLeft + x - 1;
    long long absY = gWindowTop + y - 1;
    printf("\x1B[%lld;%lldH", absY, absX);
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinTextcolor(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinTextbackground(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "TextBackground expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)(AS_INTEGER(args[0]) % 8);
    gCurrentBgIsExt = false;
    return makeVoid();
}
Value vmBuiltinTextcolore(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || (args[0].type != TYPE_INTEGER && args[0].type != TYPE_BYTE && args[0].type != TYPE_WORD)) { // <<< MODIFIED LINE
        runtimeError(vm, "TextColorE expects an integer-compatible argument (Integer, Word, Byte)."); // Changed error message
        return makeVoid();
    }
    gCurrentTextColor = (int)AS_INTEGER(args[0]);
    gCurrentTextBold = false;
    gCurrentColorIsExt = true;
    return makeVoid();
}

Value vmBuiltinTextbackgrounde(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_INTEGER) {
        runtimeError(vm, "TextBackgroundE expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)AS_INTEGER(args[0]);
    gCurrentBgIsExt = true;
    return makeVoid();
}

Value vmBuiltinBoldtext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "BoldText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBold = true;
    return makeVoid();
}

Value vmBuiltinUnderlinetext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "UnderlineText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextUnderline = true;
    return makeVoid();
}

Value vmBuiltinBlinktext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "BlinkText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBlink = true;
    return makeVoid();
}

Value vmBuiltinLowvideo(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "LowVideo expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBold = false;
    return makeVoid();
}

Value vmBuiltinNormvideo(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "NormVideo expects no arguments.");
        return makeVoid();
    }
    gCurrentTextColor = 7;
    gCurrentTextBackground = 0;
    gCurrentTextBold = false;
    gCurrentColorIsExt = false;
    gCurrentBgIsExt = false;
    gCurrentTextUnderline = false;
    gCurrentTextBlink = false;
    printf("\x1B[0m");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinClrscr(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ClrScr expects no arguments.");
        return makeVoid();
    }

    bool color_was_applied = applyCurrentTextAttributes(stdout);
    printf("\x1B[2J\x1B[H");
    if (color_was_applied) {
        resetTextAttributes(stdout);
    }
    printf("\x1B[%d;%dH", gWindowTop, gWindowLeft);
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinClreol(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ClrEol expects no arguments.");
        return makeVoid();
    }
    bool color_was_applied = applyCurrentTextAttributes(stdout);
    printf("\x1B[K");
    if (color_was_applied) {
        resetTextAttributes(stdout);
    }
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinHidecursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "HideCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[?25l");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinShowcursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ShowCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[?25h");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinCursoroff(VM* vm, int arg_count, Value* args) {
    return vmBuiltinHidecursor(vm, arg_count, args);
}

Value vmBuiltinCursoron(VM* vm, int arg_count, Value* args) {
    return vmBuiltinShowcursor(vm, arg_count, args);
}

Value vmBuiltinDeline(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "DelLine expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[M");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinInsline(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "InsLine expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[L");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinInvertcolors(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "InvertColors expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[7m");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinNormalcolors(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "NormalColors expects no arguments.");
        return makeVoid();
    }
    gCurrentTextColor = 7;
    gCurrentTextBackground = 0;
    gCurrentTextBold = false;
    gCurrentColorIsExt = false;
    gCurrentBgIsExt = false;
    gCurrentTextUnderline = false;
    gCurrentTextBlink = false;
    printf("\x1B[0m");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinBeep(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "Beep expects no arguments.");
        return makeVoid();
    }
    fputc('\a', stdout);
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinSavecursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "SaveCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[s");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinRestorecursor(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "RestoreCursor expects no arguments.");
        return makeVoid();
    }
    printf("\x1B[u");
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinPushscreen(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "PushScreen expects no arguments.");
        return makeVoid();
    }
    if (isatty(STDOUT_FILENO)) {
        vmPushColorState();
        if (vm_alt_screen_depth == 0) {
            const char enter_alt[] = "\x1B[?1049h";
            write(STDOUT_FILENO, enter_alt, sizeof(enter_alt) - 1);
        }
        vm_alt_screen_depth++;
        vmRestoreColorState();
        fflush(stdout);
    }
    return makeVoid();
}

Value vmBuiltinPopscreen(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "PopScreen expects no arguments.");
        return makeVoid();
    }
    if (vm_alt_screen_depth > 0) {
        vm_alt_screen_depth--;
        vmPopColorState();
        if (isatty(STDOUT_FILENO)) {
            if (vm_alt_screen_depth == 0) {
                const char exit_alt[] = "\x1B[?1049l";
                write(STDOUT_FILENO, exit_alt, sizeof(exit_alt) - 1);
            }
            vmRestoreColorState();
            fflush(stdout);
        }
    }
    return makeVoid();
}

Value vmBuiltinHighvideo(VM* vm, int arg_count, Value* args) {
    return vmBuiltinBoldtext(vm, arg_count, args);
}

Value vmBuiltinWindow(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4 ||
        args[0].type != TYPE_INTEGER || args[1].type != TYPE_INTEGER ||
        args[2].type != TYPE_INTEGER || args[3].type != TYPE_INTEGER) {
        runtimeError(vm, "Window expects 4 integer arguments.");
        return makeVoid();
    }
    gWindowLeft = (int)AS_INTEGER(args[0]);
    gWindowTop = (int)AS_INTEGER(args[1]);
    gWindowRight = (int)AS_INTEGER(args[2]);
    gWindowBottom = (int)AS_INTEGER(args[3]);
    printf("\x1B[%d;%dr", gWindowTop, gWindowBottom);
    printf("\x1B[%d;%dH", gWindowTop, gWindowLeft);
    fflush(stdout);
    return makeVoid();
}

Value vmBuiltinRewrite(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinSqrt(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sqrt expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    if (x < 0) { runtimeError(vm, "sqrt expects a non-negative argument."); return makeReal(0.0); }
    return makeReal(sqrt(x));
}

Value vmBuiltinExp(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "exp expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(exp(x));
}

Value vmBuiltinLn(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ln expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    if (x <= 0) { runtimeError(vm, "ln expects a positive argument."); return makeReal(0.0); }
    return makeReal(log(x));
}

Value vmBuiltinCos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cos expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(cos(x));
}

Value vmBuiltinSin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sin expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(sin(x));
}

Value vmBuiltinTan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "tan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = (arg.type == TYPE_INTEGER) ? (double)arg.i_val : arg.r_val;
    return makeReal(tan(x));
}

Value vmBuiltinTrunc(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinOrd(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ord expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (arg.type == TYPE_CHAR) return makeInt((unsigned char)arg.c_val);
    if (arg.type == TYPE_BOOLEAN) return makeInt(arg.i_val);
    if (arg.type == TYPE_ENUM) return makeInt(arg.enum_val.ordinal);
    if (arg.type == TYPE_INTEGER) return makeInt(arg.i_val);
    runtimeError(vm, "ord expects an ordinal type argument.");
    return makeInt(0);
}

Value vmBuiltinInc(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinDec(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinLow(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinHigh(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinNew(VM* vm, int arg_count, Value* args) {
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
#ifdef DEBUG
    fprintf(stderr, "[DEBUG new] ptrVar=%p type=%s ptr_val=%p base=%p (%s)\n",
            (void*)pointerVarValuePtr,
            varTypeToString(pointerVarValuePtr->type),
            pointerVarValuePtr->ptr_val,
            (void*)baseTypeNode,
            baseTypeNode ? astTypeToString(baseTypeNode->type) : "NULL");
#endif
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

Value vmBuiltinExit(VM* vm, int arg_count, Value* args) {
    if (arg_count > 0) {
        runtimeError(vm, "exit does not take any arguments.");
        return makeVoid();
    }
    // Signal the VM to unwind the current call frame on return from the builtin
    vm->exit_requested = true;
    return makeVoid();
}

Value vmBuiltinDispose(VM* vm, int arg_count, Value* args) {
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
    vmNullifyAliases(vm, disposedAddrValue);
    
    return makeVoid();
}

Value vmBuiltinAssign(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinReset(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinClose(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinEof(VM* vm, int arg_count, Value* args) {
    FILE* stream = NULL;

    if (arg_count == 0) {
        if (vm->vmGlobalSymbols) {
            Symbol* inputSym = hashTableLookup(vm->vmGlobalSymbols, "input");
            if (inputSym && inputSym->value &&
                inputSym->value->type == TYPE_FILE) {
                stream = inputSym->value->f_val;
            }
        }
        if (!stream) {
            // No default input file has been opened; treat as EOF
            return makeBoolean(true);
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

Value vmBuiltinRead(VM* vm, int arg_count, Value* args) {
    FILE* input_stream = stdin;
    int var_start_index = 0;
    bool first_arg_is_file_by_value = false;
    last_io_error = 0;

    // Determine input stream: allow FILE or ^FILE
    if (arg_count > 0) {
        const Value* a0 = &args[0];
        if (a0->type == TYPE_POINTER && a0->ptr_val) a0 = (const Value*)a0->ptr_val;
        if (a0->type == TYPE_FILE) {
            if (!a0->f_val) { runtimeError(vm, "File not open for Read."); last_io_error = 1; return makeVoid(); }
            input_stream = a0->f_val;
            var_start_index = 1;
            if (args[0].type == TYPE_FILE) first_arg_is_file_by_value = true;
        }
    }

    if (input_stream == stdin) {
        vmPrepareCanonicalInput();
    }

    for (int i = var_start_index; i < arg_count; i++) {
        if (args[i].type != TYPE_POINTER || !args[i].ptr_val) {
            runtimeError(vm, "Read requires VAR parameters to read into.");
            last_io_error = 1;
            break;
        }
        Value* dst = (Value*)args[i].ptr_val;

        if (dst->type == TYPE_CHAR) {
            int ch = fgetc(input_stream);
            if (ch == EOF) { last_io_error = feof(input_stream) ? 0 : 1; break; }
            dst->c_val = (char)ch;
            continue;
        }

        char buffer[1024];
        if (fscanf(input_stream, "%1023s", buffer) != 1) {
            last_io_error = feof(input_stream) ? 0 : 1;
            break;
        }

        switch (dst->type) {
            case TYPE_INTEGER:
            case TYPE_WORD:
            case TYPE_BYTE: {
                errno = 0;
                long long v = strtoll(buffer, NULL, 10);
                if (errno == ERANGE) { last_io_error = 1; v = 0; }
                dst->i_val = v;
                break;
            }
            case TYPE_REAL: {
                errno = 0;
                double v = strtod(buffer, NULL);
                if (errno == ERANGE) { last_io_error = 1; v = 0.0; }
                dst->r_val = v;
                break;
            }
            case TYPE_BOOLEAN: {
                if (strcasecmp(buffer, "true") == 0 || strcmp(buffer, "1") == 0) {
                    dst->i_val = 1;
                } else if (strcasecmp(buffer, "false") == 0 || strcmp(buffer, "0") == 0) {
                    dst->i_val = 0;
                } else {
                    dst->i_val = 0;
                    last_io_error = 1;
                }
                break;
            }
            case TYPE_STRING:
            case TYPE_NIL: {
                dst->type = TYPE_STRING;
                if (dst->s_val) { free(dst->s_val); }
                dst->s_val = strdup(buffer);
                if (!dst->s_val) { last_io_error = 1; }
                break;
            }
            default:
                runtimeError(vm, "Cannot Read into a variable of type %s.", varTypeToString(dst->type));
                last_io_error = 1;
                i = arg_count;
                break;
        }
    }

    if (!last_io_error && ferror(input_stream)) last_io_error = 1;
    else if (last_io_error != 1) last_io_error = 0;

    if (first_arg_is_file_by_value) { args[0].type = TYPE_NIL; args[0].f_val = NULL; }

    if (input_stream == stdin) {
        vmEnableRawMode();
    }

    return makeVoid();
}

Value vmBuiltinReadln(VM* vm, int arg_count, Value* args) {
    FILE* input_stream = stdin;
    int var_start_index = 0;
    bool first_arg_is_file_by_value = false;
    // Clear any previous IO error so a successful read won't report stale
    // failures from earlier operations (e.g. failed parse on a prior call).
    last_io_error = 0;

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

    if (input_stream == stdin) {
        vmPrepareCanonicalInput();
    }

    // 2) Read full line
    char line_buffer[1024];
    if (fgets(line_buffer, sizeof(line_buffer), input_stream) == NULL) {
        last_io_error = feof(input_stream) ? 0 : 1;
        // ***NEW***: prevent VM cleanup from closing the stream we used
        if (first_arg_is_file_by_value) { args[0].type = TYPE_NIL; args[0].f_val = NULL; }  // ***NEW***
        if (input_stream == stdin) vmEnableRawMode();
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
                if (!tmp) {
                    runtimeError(vm, "Out of memory in Readln.");
                    last_io_error = 1;
                    break;
                }
                if (dst->type == TYPE_STRING && dst->s_val) {
                    free(dst->s_val);
                }
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

    if (input_stream == stdin) {
        vmEnableRawMode();
    }

    return makeVoid();
}


Value vmBuiltinIoresult(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "IOResult requires 0 arguments."); return makeInt(0); }
    int err = last_io_error;
    last_io_error = 0;
    return makeInt(err);
}

// --- VM BUILT-IN IMPLEMENTATIONS: RANDOM ---

Value vmBuiltinRandomize(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) { runtimeError(vm, "Randomize requires 0 arguments."); return makeVoid(); }
    srand((unsigned int)time(NULL));
    return makeVoid();
}

Value vmBuiltinRandom(VM* vm, int arg_count, Value* args) {
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

// --- VM BUILT-IN IMPLEMENTATIONS: DOS/OS ---

Value vmBuiltinDosGetenv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dos_getenv expects 1 string argument.");
        return makeString("");
    }
    const char* val = getenv(AS_STRING(args[0]));
    if (!val) val = "";
    return makeString(val);
}

// Expose getenv without the DOS_ prefix for portability
Value vmBuiltinGetenv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "getenv expects 1 string argument.");
        return makeString("");
    }
    const char* val = getenv(AS_STRING(args[0]));
    if (!val) val = "";
    return makeString(val);
}

Value vmBuiltinGetenvint(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_STRING ||
        (args[1].type != TYPE_INTEGER && args[1].type != TYPE_BYTE && args[1].type != TYPE_WORD)) {
        runtimeError(vm, "GetEnvInt expects (string, integer).");
        return makeInt(0);
    }
    const char* name = AS_STRING(args[0]);
    long long defVal = AS_INTEGER(args[1]);
    const char* val = getenv(name);
    if (!val || *val == '\0') return makeInt(defVal);
    char* endptr;
    long long parsed = strtoll(val, &endptr, 10);
    if (endptr == val || *endptr != '\0') return makeInt(defVal);
    return makeInt(parsed);
}

// Parse string to numeric value similar to Turbo Pascal's Val procedure
Value vmBuiltinVal(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3) {
        runtimeError(vm, "Val expects 3 arguments.");
        return makeVoid();
    }

    Value src = args[0];
    Value dstPtr = args[1];
    Value codePtr = args[2];
    if (src.type != TYPE_STRING || dstPtr.type != TYPE_POINTER || codePtr.type != TYPE_POINTER) {
        runtimeError(vm, "Val expects (string, var numeric, var integer).");
        return makeVoid();
    }

    Value* dst = (Value*)dstPtr.ptr_val;
    Value* code = (Value*)codePtr.ptr_val;
    const char* s = src.s_val ? src.s_val : "";

    char* endptr = NULL;
    errno = 0;
    if (dst->type == TYPE_REAL) {
        double r = strtod(s, &endptr);
        if (errno != 0 || (endptr && *endptr != '\0')) {
            *code = makeInt((int)((endptr ? endptr : s) - s) + 1);
        } else {
            dst->r_val = r;
            *code = makeInt(0);
        }
    } else {
        long long n = strtoll(s, &endptr, 10);
        if (errno != 0 || (endptr && *endptr != '\0')) {
            *code = makeInt((int)((endptr ? endptr : s) - s) + 1);
        } else {
            dst->i_val = n;
            *code = makeInt(0);
        }
    }
    return makeVoid();
}

Value vmBuiltinValreal(VM* vm, int arg_count, Value* args) {
    return vmBuiltinVal(vm, arg_count, args);
}

Value vmBuiltinDosExec(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
        runtimeError(vm, "dos_exec expects 2 string arguments.");
        return makeInt(-1);
    }
    const char* path = AS_STRING(args[0]);
    const char* cmdline = AS_STRING(args[1]);
    size_t len = strlen(path) + strlen(cmdline) + 2;
    char* cmd = malloc(len);
    if (!cmd) {
        runtimeError(vm, "dos_exec memory allocation failed.");
        return makeInt(-1);
    }
    snprintf(cmd, len, "%s %s", path, cmdline);
    int res = system(cmd);
    free(cmd);
    return makeInt(res);
}

Value vmBuiltinDosMkdir(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dos_mkdir expects 1 string argument.");
        return makeInt(errno);
    }
    int rc = mkdir(AS_STRING(args[0]), 0777);
    return makeInt(rc == 0 ? 0 : errno);
}

Value vmBuiltinDosRmdir(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dos_rmdir expects 1 string argument.");
        return makeInt(errno);
    }
    int rc = rmdir(AS_STRING(args[0]));
    return makeInt(rc == 0 ? 0 : errno);
}

Value vmBuiltinDosFindfirst(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dos_findfirst expects 1 string argument.");
        return makeString("");
    }
    if (dos_dir) { closedir(dos_dir); dos_dir = NULL; }
    dos_dir = opendir(AS_STRING(args[0]));
    if (!dos_dir) return makeString("");
    struct dirent* ent;
    while ((ent = readdir(dos_dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            return makeString(ent->d_name);
        }
    }
    closedir(dos_dir); dos_dir = NULL;
    return makeString("");
}

Value vmBuiltinDosFindnext(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "dos_findnext expects 0 arguments.");
        return makeString("");
    }
    if (!dos_dir) return makeString("");
    struct dirent* ent;
    while ((ent = readdir(dos_dir)) != NULL) {
        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0) {
            return makeString(ent->d_name);
        }
    }
    closedir(dos_dir); dos_dir = NULL;
    return makeString("");
}

Value vmBuiltinDosGetfattr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dos_getfattr expects 1 string argument.");
        return makeInt(0);
    }
    struct stat st;
    if (stat(AS_STRING(args[0]), &st) != 0) {
        return makeInt(0);
    }
    int attr = 0;
    if (S_ISDIR(st.st_mode)) attr |= 16;
    if (!(st.st_mode & S_IWUSR)) attr |= 1;
    return makeInt(attr);
}

Value vmBuiltinDosGetdate(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "dos_getdate expects 4 var arguments.");
        return makeVoid();
    }
    time_t t = time(NULL);
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    Value* year = (Value*)args[0].ptr_val;
    Value* month = (Value*)args[1].ptr_val;
    Value* day = (Value*)args[2].ptr_val;
    Value* dow = (Value*)args[3].ptr_val;
    if (year) year->i_val = tm_info.tm_year + 1900;
    if (month) month->i_val = tm_info.tm_mon + 1;
    if (day) day->i_val = tm_info.tm_mday;
    if (dow) dow->i_val = tm_info.tm_wday;
    return makeVoid();
}

Value vmBuiltinDosGettime(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "dos_gettime expects 4 var arguments.");
        return makeVoid();
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
    localtime_r(&tv.tv_sec, &tm_info);
    Value* hour = (Value*)args[0].ptr_val;
    Value* min = (Value*)args[1].ptr_val;
    Value* sec = (Value*)args[2].ptr_val;
    Value* sec100 = (Value*)args[3].ptr_val;
    if (hour) hour->i_val = tm_info.tm_hour;
    if (min) min->i_val = tm_info.tm_min;
    if (sec) sec->i_val = tm_info.tm_sec;
    if (sec100) sec100->i_val = (int)(tv.tv_usec / 10000);
    return makeVoid();
}

Value vmBuiltinScreencols(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinScreenrows(VM* vm, int arg_count, Value* args) {
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
Value vmBuiltinMstreamcreate(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinMstreamloadfromfile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "MStreamLoadFromFile expects 2 arguments (MStreamVar, Filename).");
        return makeBoolean(0);
    }

    // Argument 0 is pointer to the Value holding the MStream*
    if (args[0].type != TYPE_POINTER) {
        runtimeError(vm, "MStreamLoadFromFile: First argument must be a VAR MStream.");
        return makeBoolean(0);
    }
    Value* ms_value_ptr = (Value*)args[0].ptr_val;
    if (!ms_value_ptr || ms_value_ptr->type != TYPE_MEMORYSTREAM) {
        runtimeError(vm, "MStreamLoadFromFile: First argument is not a valid MStream variable.");
        return makeBoolean(0);
    }
    MStream* ms = ms_value_ptr->mstream;
    if (!ms) {
        runtimeError(vm, "MStreamLoadFromFile: MStream variable not initialized.");
        return makeBoolean(0);
    }

    // Argument 1 is the filename string
    if (args[1].type != TYPE_STRING || args[1].s_val == NULL) {
        runtimeError(vm, "MStreamLoadFromFile: Second argument must be a string filename.");
        return makeBoolean(0); // No need to free args[1] here, vm stack manages
    }
    const char* filename = args[1].s_val;

    FILE* f = fopen(filename, "rb");
    if (!f) {
        runtimeError(vm, "MStreamLoadFromFile: Cannot open file '%s' for reading.", filename);
        return makeBoolean(0);
    }

    fseek(f, 0, SEEK_END);
    int size = (int)ftell(f);
    rewind(f);

    unsigned char* buffer = malloc(size + 1); // +1 for null terminator for safety
    if (!buffer) {
        fclose(f);
        runtimeError(vm, "MStreamLoadFromFile: Memory allocation error for file buffer.");
        return makeBoolean(0);
    }
    fread(buffer, 1, size, f);
    buffer[size] = '\0'; // Null-terminate the buffer
    fclose(f);

    // Free existing buffer in MStream if any
    if (ms->buffer) free(ms->buffer);

    ms->buffer = buffer;
    ms->size = size;
    ms->capacity = size + 1; // Capacity is now exactly what's needed + null

    return makeBoolean(1);
}

Value vmBuiltinMstreamsavetofile(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinMstreamfree(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinMstreambuffer(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "MStreamBuffer expects 1 argument (MStream).");
        return makeVoid();
    }
    if (args[0].type != TYPE_MEMORYSTREAM || args[0].mstream == NULL) {
        runtimeError(vm, "MStreamBuffer: Argument is not a valid MStream.");
        return makeVoid();
    }
    MStream* mstream = args[0].mstream;
    const char* buffer_content = mstream->buffer ? (char*)mstream->buffer : "";
    return makeString(buffer_content);
}

Value vmBuiltinReal(VM* vm, int arg_count, Value* args) {
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


Value vmBuiltinInttostr(VM* vm, int arg_count, Value* args) {
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

Value vmBuiltinLength(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Length expects 1 argument.");
        return makeInt(0);
    }

    if (args[0].type == TYPE_STRING) {
        return makeInt(args[0].s_val ? (long long)strlen(args[0].s_val) : 0);
    }

    if (args[0].type == TYPE_ARRAY) {
        long long len = 0;
        if (args[0].dimensions > 0 && args[0].upper_bounds && args[0].lower_bounds) {
            len = args[0].upper_bounds[0] - args[0].lower_bounds[0] + 1;
        } else {
            len = args[0].upper_bound - args[0].lower_bound + 1;
        }
        return makeInt(len);
    }

    runtimeError(vm, "Length expects a string or array argument.");
    return makeInt(0);
}


Value vmBuiltinAbs(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "abs expects 1 argument."); return makeInt(0); }
    if (args[0].type == TYPE_INTEGER) return makeInt(llabs(args[0].i_val));
    if (args[0].type == TYPE_REAL) return makeReal(fabs(args[0].r_val));
    runtimeError(vm, "abs expects a numeric argument.");
    return makeInt(0);
}

Value vmBuiltinRound(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Round expects 1 argument."); return makeInt(0); }
    if (args[0].type == TYPE_REAL) return makeInt((long long)round(args[0].r_val));
    if (args[0].type == TYPE_INTEGER) return makeInt(args[0].i_val);
    runtimeError(vm, "Round expects a numeric argument.");
    return makeInt(0);
}

Value vmBuiltinHalt(VM* vm, int arg_count, Value* args) {
    long long code = 0;
    if (arg_count == 0) {
        // No exit code supplied, default to 0.
    } else if (arg_count == 1 && args[0].type == TYPE_INTEGER) {
        code = args[0].i_val;
    } else {
        runtimeError(vm, "Halt expects 0 or 1 integer argument.");
    }
    exit(vmExitWithCleanup((int)code));
    return makeVoid(); // Unreachable
}

Value vmBuiltinDelay(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || (args[0].type != TYPE_INTEGER && args[0].type != TYPE_WORD)) {
        runtimeError(vm, "Delay requires an integer or word argument.");
        return makeVoid();
    }
    long long ms = args[0].i_val;
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return makeVoid();
}

int getBuiltinIDForCompiler(const char *name) {
    if (!name) return -1;
    for (size_t i = 0; i < num_vm_builtins; ++i) {
        if (strcasecmp(name, vmBuiltinDispatchTable[i].name) == 0) {
            return (int)i;
        }
    }
    for (size_t i = 0; i < num_extra_vm_builtins; ++i) {
        if (strcasecmp(name, extra_vm_builtins[i].name) == 0) {
            return (int)(num_vm_builtins + i);
        }
    }
    return -1;
}

typedef struct {
    char *name;
    BuiltinRoutineType type;
} RegisteredBuiltin;

static RegisteredBuiltin builtin_registry[256];
static int builtin_registry_count = 0;

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc) {
    (void)unit_context_name_param_for_addproc;
    for (int i = 0; i < builtin_registry_count; ++i) {
        if (strcasecmp(name, builtin_registry[i].name) == 0) {
            builtin_registry[i].type = (declType == AST_FUNCTION_DECL) ?
                BUILTIN_TYPE_FUNCTION : BUILTIN_TYPE_PROCEDURE;
            return;
        }
    }
    if (builtin_registry_count >= 256) return;
    builtin_registry[builtin_registry_count].name = strdup(name);
    builtin_registry[builtin_registry_count].type = (declType == AST_FUNCTION_DECL) ?
        BUILTIN_TYPE_FUNCTION : BUILTIN_TYPE_PROCEDURE;
    builtin_registry_count++;
}

int isBuiltin(const char *name) {
    return getBuiltinIDForCompiler(name) != -1;
}

BuiltinRoutineType getBuiltinType(const char *name) {
    for (int i = 0; i < builtin_registry_count; ++i) {
        if (strcasecmp(name, builtin_registry[i].name) == 0) {
            return builtin_registry[i].type;
        }
    }
    return BUILTIN_TYPE_NONE;
}

void registerAllBuiltins(void) {
    /* Graphics stubs (usable even without SDL) */
    registerBuiltinFunction("ClearDevice", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("CloseGraph", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("FillCircle", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("GetMaxX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("GetMaxY", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("GraphLoop", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("InitGraph", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("SetRGBColor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("UpdateScreen", AST_PROCEDURE_DECL, NULL);

#ifdef SDL
    /* Additional SDL graphics and sound built-ins */
    registerBuiltinFunction("CreateTargetTexture", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("CreateTexture", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("DestroyTexture", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("DrawCircle", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("DrawLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("DrawPolygon", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("DrawRect", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("FillRect", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("FreeSound", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("GetMouseState", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("GetPixelColor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("GetTextSize", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("GetTicks", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("InitSoundSystem", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("InitTextSystem", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("IsSoundPlaying", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("LoadImageToTexture", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("LoadSound", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("OutTextXY", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("PlaySound", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("PollKey", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("PutPixel", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("QuitSoundSystem", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("QuitTextSystem", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("RenderCopy", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("RenderCopyEx", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("RenderCopyRect", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("RenderTextToTexture", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("SetAlphaBlend", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("SetColor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("SetRenderTarget", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("UpdateTexture", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("WaitKeyEvent", AST_PROCEDURE_DECL, NULL);
#endif

    /* General built-in functions and procedures */
    registerBuiltinFunction("Abs", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("api_receive", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("api_send", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Assign", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Beep", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Chr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Close", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("ClrEol", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Copy", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Cos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("CursorOff", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("CursorOn", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Dec", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Delay", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("DelLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Dispose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("dos_exec", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_findfirst", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_findnext", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_getenv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_getfattr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_mkdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_rmdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("dos_getdate", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("dos_gettime", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("EOF", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Exit", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Exp", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("GetDate", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("GetEnv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("GetEnvInt", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("GetTime", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Halt", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("HideCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("High", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("HighVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Inc", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("InsLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("IntToStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("InvertColors", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("IOResult", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("KeyPressed", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Length", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Ln", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Low", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("MStreamCreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("MStreamFree", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("MStreamLoadFromFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("MStreamSaveToFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("MStreamBuffer", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("New", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("NormalColors", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Ord", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("ParamCount", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("ParamStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("PopScreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Pos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("PushScreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("QuitRequested", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Random", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Randomize", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("ReadKey", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Real", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("RealToStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Reset", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("RestoreCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Rewrite", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Round", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("SaveCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("ScreenCols", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("ScreenRows", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("ShowCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Sin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Sqr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Sqrt", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Succ", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Tan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("GotoXY", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BoldText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BIBoldText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BlinkText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BIBlinkText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("UnderlineText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BIUnderlineText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("LowVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BILowVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("NormVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BINormVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("ClrScr", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("BIClrScr", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("TermBackground", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("TextBackground", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("TextBackgroundE", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("TextColor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("TextColorE", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Trunc", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("UpCase", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("Val", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("ValReal", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("Window", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunction("WhereX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("BIWhereX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("WhereY", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("BIWhereY", AST_FUNCTION_DECL, NULL);

    /* Allow externally linked modules to add more builtins. */
    registerExtendedBuiltins();
}


