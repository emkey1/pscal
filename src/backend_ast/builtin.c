#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "core/version.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"                  // Assuming globals.h is directly in src/
#include "backend_ast/builtin_network_api.h"
#include "vm/vm.h"
#include "vm/string_sentinels.h"

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
#include <limits.h>
#include <time.h>    // For date/time functions
#include <sys/time.h> // For gettimeofday
#include <stdio.h>   // For printf, fprintf
#include <pthread.h>

#if defined(__APPLE__)
extern void shellRuntimeSetLastStatus(int status) __attribute__((weak_import));
extern void shellRuntimeSetLastStatusSticky(int status) __attribute__((weak_import));
#elif defined(__GNUC__)
extern void shellRuntimeSetLastStatus(int status) __attribute__((weak));
extern void shellRuntimeSetLastStatusSticky(int status) __attribute__((weak));
#else
void shellRuntimeSetLastStatus(int status);
void shellRuntimeSetLastStatusSticky(int status);
#endif

// Maximum number of arguments allowed for write/writeln
#define MAX_WRITE_ARGS_VM 32

/* SDL-backed builtins are registered dynamically when available. */
#ifdef SDL
#include "backend_ast/sdl.h"
#define SDL_READKEY_BUFFER_CAPACITY 8
static int gSdlReadKeyBuffer[SDL_READKEY_BUFFER_CAPACITY];
static int gSdlReadKeyBufferStart = 0;
static int gSdlReadKeyBufferCount = 0;

#endif

static const Value* resolveStringPointerBuiltin(const Value* value) {
    const Value* current = value;
    int depth = 0;
    while (current && current->type == TYPE_POINTER &&
           current->base_type_node != STRING_CHAR_PTR_SENTINEL) {
        if (!current->ptr_val) {
            return NULL;
        }
        current = (const Value*)current->ptr_val;
        if (++depth > 16) {
            return NULL;
        }
    }
    return current;
}

static int builtinValueIsStringLike(const Value* value) {
    if (!value) return 0;
    if (value->type == TYPE_STRING) return 1;
    if (value->type == TYPE_POINTER) {
        if (value->base_type_node == STRING_CHAR_PTR_SENTINEL) return 1;
        const Value* resolved = resolveStringPointerBuiltin(value);
        if (!resolved) return 0;
        if (resolved->type == TYPE_STRING) return 1;
        if (resolved->type == TYPE_POINTER && resolved->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return 1;
        }
    }
    return 0;
}

static const char* builtinValueToCString(const Value* value) {
    if (!value) return NULL;
    if (value->type == TYPE_STRING) return value->s_val ? value->s_val : "";
    if (value->type == TYPE_POINTER) {
        if (value->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return (const char*)value->ptr_val;
        }
        const Value* resolved = resolveStringPointerBuiltin(value);
        if (!resolved) return NULL;
        if (resolved->type == TYPE_STRING) {
            return resolved->s_val ? resolved->s_val : "";
        }
        if (resolved->type == TYPE_POINTER && resolved->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return (const char*)resolved->ptr_val;
        }
    }
    return NULL;
}

#ifdef SDL
static bool sdlReadKeyBufferHasData(void) {
    return gSdlReadKeyBufferCount > 0;
}

static int sdlReadKeyBufferPop(void) {
    if (!sdlReadKeyBufferHasData()) {
        return 0;
    }

    int value = gSdlReadKeyBuffer[gSdlReadKeyBufferStart];
    gSdlReadKeyBufferStart = (gSdlReadKeyBufferStart + 1) % SDL_READKEY_BUFFER_CAPACITY;
    gSdlReadKeyBufferCount--;
    return value & 0xFF;
}

static void sdlReadKeyBufferPushBytes(const int* bytes, int length) {
    if (!bytes || length <= 0) {
        return;
    }

    for (int i = 0; i < length; ++i) {
        if (gSdlReadKeyBufferCount >= SDL_READKEY_BUFFER_CAPACITY) {
            break;
        }
        int tail = (gSdlReadKeyBufferStart + gSdlReadKeyBufferCount) % SDL_READKEY_BUFFER_CAPACITY;
        gSdlReadKeyBuffer[tail] = bytes[i] & 0xFF;
        gSdlReadKeyBufferCount++;
    }
}

static int sdlTranslateKeycode(SDL_Keycode code, int* extraBytes, int* extraCount) {
    if (extraBytes) {
        extraBytes[0] = 0;
        extraBytes[1] = 0;
        extraBytes[2] = 0;
        extraBytes[3] = 0;
    }
    if (extraCount) {
        *extraCount = 0;
    }

    switch (code) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            return '\r';
        case SDLK_BACKSPACE:
            return '\b';
        case SDLK_TAB:
            return '\t';
        case SDLK_ESCAPE:
            return 27;
        case SDLK_DELETE:
            return 127;
        case SDLK_LEFT:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'D';
                *extraCount = 2;
            }
            return 27;
        case SDLK_RIGHT:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'C';
                *extraCount = 2;
            }
            return 27;
        case SDLK_UP:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'A';
                *extraCount = 2;
            }
            return 27;
        case SDLK_DOWN:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'B';
                *extraCount = 2;
            }
            return 27;
        case SDLK_HOME:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'H';
                *extraCount = 2;
            }
            return 27;
        case SDLK_END:
            if (extraBytes && extraCount) {
                extraBytes[0] = '[';
                extraBytes[1] = 'F';
                *extraCount = 2;
            }
            return 27;
        case SDLK_KP_0:
            return '0';
        case SDLK_KP_1:
            return '1';
        case SDLK_KP_2:
            return '2';
        case SDLK_KP_3:
            return '3';
        case SDLK_KP_4:
            return '4';
        case SDLK_KP_5:
            return '5';
        case SDLK_KP_6:
            return '6';
        case SDLK_KP_7:
            return '7';
        case SDLK_KP_8:
            return '8';
        case SDLK_KP_9:
            return '9';
        case SDLK_KP_PERIOD:
            return '.';
        case SDLK_KP_DIVIDE:
            return '/';
        case SDLK_KP_MULTIPLY:
            return '*';
        case SDLK_KP_MINUS:
            return '-';
        case SDLK_KP_PLUS:
            return '+';
        case SDLK_KP_EQUALS:
            return '=';
        default:
            break;
    }

    if (code >= 32 && code <= 126) {
        return (int)code;
    }

    if (code >= 0 && code <= 255) {
        return (int)(code & 0xFF);
    }

    return 0;
}

static int sdlFetchReadKeyChar(void) {
    if (!sdlIsGraphicsActive()) {
        return -1;
    }

    if (sdlReadKeyBufferHasData()) {
        return sdlReadKeyBufferPop();
    }

    int extraBytes[4];
    SDL_Keycode keycode;

    for (;;) {
        keycode = sdlWaitNextKeycode();
        if (keycode == SDLK_UNKNOWN) {
            return 0;
        }

        int extraCount = 0;
        int translated = sdlTranslateKeycode(keycode, extraBytes, &extraCount);
        if (extraCount > 0) {
            sdlReadKeyBufferPushBytes(extraBytes, extraCount);
        }

        if (translated != 0) {
            return translated & 0xFF;
        }

        if (sdlReadKeyBufferHasData()) {
            return sdlReadKeyBufferPop();
        }
    }
}
#endif

#ifndef SDL
#if defined(__GNUC__) || defined(__clang__)
#define SDL_UNUSED_FUNC __attribute__((unused))
#else
#define SDL_UNUSED_FUNC
#endif
static Value SDL_UNUSED_FUNC vmBuiltinSDLUnavailable(VM* vm, int arg_count, Value* args) {
    (void)arg_count;
    (void)args;
    const char* name = (vm && vm->current_builtin_name) ? vm->current_builtin_name : "This built-in";
    runtimeError(vm, "Built-in '%s' requires SDL support. Rebuild with -DSDL=ON to enable it.", name);
    if (vm) {
        vm->abort_requested = true;
    }
    return makeNil();
}
#define SDL_HANDLER(fn) vmBuiltinSDLUnavailable
#undef SDL_UNUSED_FUNC
#else
#define SDL_HANDLER(fn) fn
#endif

// Per-thread state to keep core builtins thread-safe
static _Thread_local DIR* dos_dir = NULL; // Used by dosFindfirst/findnext
static _Thread_local unsigned int rand_seed = 1;

// Terminal cursor helper
static int getCursorPosition(int *row, int *col);

// ---- CLike-style conversion helpers (Phase 1) ----
static Value vmBuiltinToInt(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "int(x) expects 1 argument.");
        return makeInt(0);
    }
    Value v = args[0];
    long long i = 0;
    if (isRealType(v.type)) {
        long double d = AS_REAL(v);
        i = (long long)d; // truncate toward zero like C cast
    } else if (IS_INTLIKE(v)) {
        i = AS_INTEGER(v);
    } else if (v.type == TYPE_BOOLEAN) {
        i = v.i_val ? 1 : 0;
    } else if (v.type == TYPE_CHAR) {
        i = v.c_val;
    } else {
        i = 0;
    }
    return makeInt(i);
}

static Value vmBuiltinToDouble(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "double(x) expects 1 argument.");
        return makeReal(0.0);
    }
    Value v = args[0];
    long double d = 0.0L;
    if (isRealType(v.type)) {
        d = AS_REAL(v);
    } else if (IS_INTLIKE(v)) {
        d = (long double)AS_INTEGER(v);
    } else if (v.type == TYPE_BOOLEAN) {
        d = v.i_val ? 1.0L : 0.0L;
    } else if (v.type == TYPE_CHAR) {
        d = (long double)v.c_val;
    } else {
        d = 0.0L;
    }
    return makeReal(d);
}

static Value vmBuiltinToFloat(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "float(x) expects 1 argument.");
        return makeFloat(0.0f);
    }
    Value v = args[0];
    float f = 0.0f;
    if (isRealType(v.type)) {
        f = (float)AS_REAL(v);
    } else if (IS_INTLIKE(v)) {
        f = (float)AS_INTEGER(v);
    } else if (v.type == TYPE_BOOLEAN) {
        f = v.i_val ? 1.0f : 0.0f;
    } else if (v.type == TYPE_CHAR) {
        f = (float)v.c_val;
    } else {
        f = 0.0f;
    }
    return makeFloat(f);
}

static Value vmBuiltinToChar(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "char(x) expects 1 argument.");
        return makeChar('\0');
    }
    Value v = args[0];
    unsigned char c = 0;
    if (isRealType(v.type)) {
        long double d = AS_REAL(v);
        c = (unsigned char)((long long)d); // truncate then narrow
    } else if (IS_INTLIKE(v)) {
        c = (unsigned char)AS_INTEGER(v);
    } else if (v.type == TYPE_BOOLEAN) {
        c = v.i_val ? 1 : 0;
    } else if (v.type == TYPE_CHAR) {
        c = (unsigned char)v.c_val;
    } else {
        c = 0;
    }
    return makeChar((int)c);
}

static Value vmBuiltinToByte(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "byte(x) expects 1 argument.");
        return makeByte(0);
    }
    Value v = args[0];
    unsigned char b = 0;
    if (isRealType(v.type)) {
        long double d = AS_REAL(v);
        b = (unsigned char)((long long)d);
    } else if (IS_INTLIKE(v)) {
        b = (unsigned char)AS_INTEGER(v);
    } else if (v.type == TYPE_BOOLEAN) {
        b = v.i_val ? 1 : 0;
    } else if (v.type == TYPE_CHAR) {
        b = (unsigned char)v.c_val;
    } else {
        b = 0;
    }
    return makeByte(b);
}

static Value vmBuiltinToBool(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "bool(x) expects 1 argument.");
        return makeBoolean(0);
    }
    Value v = args[0];
    int truth = 0;
    if (isRealType(v.type)) {
        truth = (AS_REAL(v) != 0.0L);
    } else if (IS_INTLIKE(v)) {
        truth = (AS_INTEGER(v) != 0);
    } else if (v.type == TYPE_BOOLEAN) {
        truth = v.i_val ? 1 : 0;
    } else if (v.type == TYPE_CHAR) {
        truth = (v.c_val != 0);
    } else {
        truth = 0;
    }
    return makeBoolean(truth);
}

// The new dispatch table for the VM - MUST be defined before the function that uses it
// This list MUST BE SORTED ALPHABETICALLY BY NAME (lowercase).
// SDL/graphics builtins use NULL placeholders; registerGraphicsBuiltins() overrides them
// when SDL support is enabled so legacy builtin IDs remain stable.
static VmBuiltinMapping vmBuiltinDispatchTable[] = {
    {"abs", vmBuiltinAbs},
    {"apiReceive", vmBuiltinApiReceive},
    {"apiSend", vmBuiltinApiSend},
    {"httpsession", vmBuiltinHttpSession},
    {"httpclose", vmBuiltinHttpClose},
    {"httperrorcode", vmBuiltinHttpErrorCode},
    {"httpgetlastheaders", vmBuiltinHttpGetLastHeaders},
    {"httpgetheader", vmBuiltinHttpGetHeader},
    {"httpsetheader", vmBuiltinHttpSetHeader},
    {"httpclearheaders", vmBuiltinHttpClearHeaders},
    {"httpsetoption", vmBuiltinHttpSetOption},
    {"httprequest", vmBuiltinHttpRequest},
    {"httprequesttofile", vmBuiltinHttpRequestToFile},
    {"httprequestasync", vmBuiltinHttpRequestAsync},
    {"httprequestasynctofile", vmBuiltinHttpRequestAsyncToFile},
    {"httpisdone", vmBuiltinHttpIsDone},
    {"httptryawait", vmBuiltinHttpTryAwait},
    {"httpcancel", vmBuiltinHttpCancel},
    {"httpgetasyncprogress", vmBuiltinHttpGetAsyncProgress},
    {"httpgetasynctotal", vmBuiltinHttpGetAsyncTotal},
    {"httpawait", vmBuiltinHttpAwait},
    {"httplasterror", vmBuiltinHttpLastError},
    {"jsonget", vmBuiltinJsonGet},
    {"append", vmBuiltinAppend},
    {"arccos", vmBuiltinArccos},
    {"arcsin", vmBuiltinArcsin},
    {"arctan", vmBuiltinArctan},
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
    {"bool", vmBuiltinToBool},
    {"byte", vmBuiltinToByte},
    {"bytecodeversion", vmBuiltinBytecodeVersion},
    {"ceil", vmBuiltinCeil},
    {"char", vmBuiltinToChar},
    {"chr", vmBuiltinChr},
    {"cleardevice", NULL},
    {"clreol", vmBuiltinClreol},
    {"clrscr", vmBuiltinClrscr},
    {"close", vmBuiltinClose},
    {"closegraph", NULL},
    {"closegraph3d", NULL},
    {"copy", vmBuiltinCopy},
    {"cos", vmBuiltinCos},
    {"cosh", vmBuiltinCosh},
    {"cotan", vmBuiltinCotan},
    {"cursoroff", vmBuiltinCursoroff},
    {"cursoron", vmBuiltinCursoron},
    {"createtargettexture", NULL}, // Moved
    {"createtexture", NULL}, // Moved
    {"dec", vmBuiltinDec},
    {"delay", vmBuiltinDelay},
    {"deline", vmBuiltinDeline},
    {"destroytexture", NULL},
    {"dispose", vmBuiltinDispose},
    {"dnslookup", vmBuiltinDnsLookup},
    {"dosExec", vmBuiltinDosExec},
    {"dosFindfirst", vmBuiltinDosFindfirst},
    {"dosFindnext", vmBuiltinDosFindnext},
    {"dosGetdate", vmBuiltinDosGetdate},
    {"dosGetenv", vmBuiltinDosGetenv},
    {"dosGetfattr", vmBuiltinDosGetfattr},
    {"dosGettime", vmBuiltinDosGettime},
    {"dosMkdir", vmBuiltinDosMkdir},
    {"dosRmdir", vmBuiltinDosRmdir},
    {"double", vmBuiltinToDouble},
    {"drawcircle", NULL}, // Moved
    {"drawline", NULL}, // Moved
    {"drawpolygon", NULL}, // Moved
    {"drawrect", NULL}, // Moved
    {"eof", vmBuiltinEof},
    {"erase", vmBuiltinErase},
    {"exec", vmBuiltinDosExec},
    {"exit", vmBuiltinExit},
    {"exp", vmBuiltinExp},
    {"fillcircle", NULL},
    {"fillrect", NULL},
    {"findfirst", vmBuiltinDosFindfirst},
    {"findnext", vmBuiltinDosFindnext},
    {"float", vmBuiltinToFloat},
    {"floor", vmBuiltinFloor},
    {"formatfloat", vmBuiltinFormatfloat},
    {"freesound", NULL},
    {"getdate", vmBuiltinDosGetdate},
    {"getenv", vmBuiltinGetenv},
    {"getenvint", vmBuiltinGetenvint},
    {"getfattr", vmBuiltinDosGetfattr},
    {"getmaxx", NULL},
    {"getmaxy", NULL},
    {"getmousestate", NULL},
    {"getpixelcolor", NULL}, // Moved
    {"gettextsize", NULL},
    {"getticks", NULL},
    {"glbegin", NULL},
    {"glclear", NULL},
    {"glclearcolor", NULL},
    {"glcleardepth", NULL},
    {"glcolor3f", NULL},
    {"gldepthtest", NULL},
    {"glend", NULL},
    {"glfrustum", NULL},
    {"glloadidentity", NULL},
    {"glmatrixmode", NULL},
    {"glpopmatrix", NULL},
    {"glpushmatrix", NULL},
    {"glrotatef", NULL},
    {"glscalef", NULL},
    {"glperspective", NULL},
    {"glsetswapinterval", NULL},
    {"glswapwindow", NULL},
    {"gltranslatef", NULL},
    {"glvertex3f", NULL},
    {"glviewport", NULL},
    {"gettime", vmBuiltinDosGettime},
    {"graphloop", NULL},
    {"gotoxy", vmBuiltinGotoxy},
    {"halt", vmBuiltinHalt},
    {"hidecursor", vmBuiltinHidecursor},
    {"high", vmBuiltinHigh},
    {"highvideo", vmBuiltinHighvideo},
    {"inc", vmBuiltinInc},
    {"initgraph", NULL},
    {"initgraph3d", NULL},
    {"initsoundsystem", NULL},
    {"inittextsystem", NULL},
    {"insline", vmBuiltinInsline},
    {"int", vmBuiltinToInt},
    {"inttostr", vmBuiltinInttostr},
    {"invertcolors", vmBuiltinInvertcolors},
    {"ioresult", vmBuiltinIoresult},
    {"issoundplaying", NULL}, // Moved
    {"keypressed", vmBuiltinKeypressed},
    {"length", vmBuiltinLength},
    {"ln", vmBuiltinLn},
    {"log10", vmBuiltinLog10},
    {"loadimagetotexture", NULL}, // Moved
    {"loadsound", NULL},
    {"low", vmBuiltinLow},
    {"lowvideo", vmBuiltinLowvideo},
    {"max", vmBuiltinMax},
    {"min", vmBuiltinMin},
    {"mkdir", vmBuiltinDosMkdir},
    {"mstreamcreate", vmBuiltinMstreamcreate},
    {"mstreamfree", vmBuiltinMstreamfree},
    {"mstreamloadfromfile", vmBuiltinMstreamloadfromfile},
    {"mstreamsavetofile", vmBuiltinMstreamsavetofile},
    {"mstreambuffer", vmBuiltinMstreambuffer},
    {"newobj", vmBuiltinNewObj},
    {"new", vmBuiltinNew},
    {"normalcolors", vmBuiltinNormalcolors},
    {"normvideo", vmBuiltinNormvideo},
    {"ord", vmBuiltinOrd},
    {"outtextxy", NULL}, // Moved
    {"paramcount", vmBuiltinParamcount},
    {"paramstr", vmBuiltinParamstr},
    {"playsound", NULL},
    {"stopallsounds", NULL},
    {"pollkey", NULL},
    {"iskeydown", NULL},
    {"popscreen", vmBuiltinPopscreen},
    {"pos", vmBuiltinPos},
    {"power", vmBuiltinPower},
    {"printf", vmBuiltinPrintf},
    {"fopen", vmBuiltinFopen},
    {"fclose", vmBuiltinFclose},
    {"pushscreen", vmBuiltinPushscreen},
    {"putpixel", NULL},
    {"write", vmBuiltinWrite}, // Preserve legacy builtin id for write
    {"fprintf", vmBuiltinFprintf}, // Registered after write to avoid shifting legacy id 176
    {"quitsoundsystem", NULL},
    {"quittextsystem", NULL},
    {"random", vmBuiltinRandom},
    {"randomize", vmBuiltinRandomize},
    {"read", vmBuiltinRead},
    {"readkey", vmBuiltinReadkey},
    {"readln", vmBuiltinReadln},
    {"real", vmBuiltinReal},
    {"realtostr", vmBuiltinRealtostr},
    {"rename", vmBuiltinRename},
    {"rendercopy", NULL}, // Moved
    {"rendercopyex", NULL}, // Moved
    {"rendercopyrect", NULL},
    {"rendertexttotexture", NULL},
    {"reset", vmBuiltinReset},
    {"restorecursor", vmBuiltinRestorecursor},
    {"rewrite", vmBuiltinRewrite},
    {"rmdir", vmBuiltinDosRmdir},
    {"round", vmBuiltinRound},
    {"savecursor", vmBuiltinSavecursor},
    {"screencols", vmBuiltinScreencols},
    {"screenrows", vmBuiltinScreenrows},
    {"setlength", vmBuiltinSetlength},
    {"setalphablend", NULL},
    {"setcolor", NULL}, // Moved
    {"setrendertarget", NULL}, // Moved
    {"setrgbcolor", NULL},
    {"showcursor", vmBuiltinShowcursor},
    {"sin", vmBuiltinSin},
    {"sinh", vmBuiltinSinh},
    {"socketaccept", vmBuiltinSocketAccept},
    {"socketbind", vmBuiltinSocketBind},
    {"socketbindaddr", vmBuiltinSocketBindAddr},
    {"socketclose", vmBuiltinSocketClose},
    {"socketconnect", vmBuiltinSocketConnect},
    {"socketcreate", vmBuiltinSocketCreate},
    {"socketlasterror", vmBuiltinSocketLastError},
    {"socketlisten", vmBuiltinSocketListen},
    {"socketpoll", vmBuiltinSocketPoll},
    {"socketreceive", vmBuiltinSocketReceive},
    {"socketsend", vmBuiltinSocketSend},
    {"socketsetblocking", vmBuiltinSocketSetBlocking},
    {"sqr", vmBuiltinSqr},
    {"sqrt", vmBuiltinSqrt},
    {"str", vmBuiltinStr},
    {"succ", vmBuiltinSucc},
    {"tan", vmBuiltinTan},
    {"tanh", vmBuiltinTanh},
    {"textbackground", vmBuiltinTextbackground},
    {"textbackgrounde", vmBuiltinTextbackgrounde},
    {"textcolor", vmBuiltinTextcolor},
    {"textcolore", vmBuiltinTextcolore},
    {"trunc", vmBuiltinTrunc},
    {"underlinetext", vmBuiltinUnderlinetext},
    {"upcase", vmBuiltinUpcase},
    {"toupper", vmBuiltinUpcase},
    {"updatescreen", NULL},
    {"updatetexture", NULL},
    {"val", vmBuiltinVal},
    {"valreal", vmBuiltinValreal},
    {"vmversion", vmBuiltinVMVersion},
    {"waitkeyevent", NULL}, // Moved
    {"wherex", vmBuiltinWherex},
    {"wherey", vmBuiltinWherey},
    {"window", vmBuiltinWindow},
    {"quitrequested", vmBuiltinQuitrequested},
    {"getscreensize", NULL},
    {"pollkeyany", vmBuiltinPollkeyany},
    {"threadgetresult", vmBuiltinThreadGetResult},
    {"threadgetstatus", vmBuiltinThreadGetStatus},
    {"threadspawnbuiltin", vmBuiltinThreadSpawnBuiltin},
    {"waitforthread", vmBuiltinWaitForThread},
    {"glcullface", NULL}, // Append new builtins above the placeholder to avoid shifting legacy IDs.
    {"to be filled", NULL}
};

static const size_t num_vm_builtins = sizeof(vmBuiltinDispatchTable) / sizeof(vmBuiltinDispatchTable[0]);

static bool threadBuiltinIsAllowlisted(int id);

/* Dynamic registry for user-supplied VM built-ins. */
static VmBuiltinMapping *extra_vm_builtins = NULL;
static size_t num_extra_vm_builtins = 0;
static pthread_mutex_t builtin_registry_mutex;
static pthread_once_t builtin_registry_once = PTHREAD_ONCE_INIT;

typedef struct {
    Symbol symbol;
    size_t id;
} BuiltinRegistryEntry;

static HashTable *builtinRegistryHash = NULL;

static VmBuiltinMapping *builtinMappingFromId(size_t id) {
    if (id < num_vm_builtins) {
        return &vmBuiltinDispatchTable[id];
    }
    size_t extra_index = id - num_vm_builtins;
    if (extra_index < num_extra_vm_builtins) {
        return &extra_vm_builtins[extra_index];
    }
    return NULL;
}

static bool canonicalizeBuiltinName(const char *name, char *out, size_t out_size) {
    if (!name || !out || out_size == 0) {
        return false;
    }

    size_t i = 0;
    for (; i + 1 < out_size && name[i]; ++i) {
        out[i] = (char)tolower((unsigned char)name[i]);
    }
    out[i] = '\0';
    return i > 0;
}

static BuiltinRegistryEntry *lookupBuiltinEntryUnlocked(const char *canonical_name) {
    if (!canonical_name || !builtinRegistryHash) {
        return NULL;
    }
    Symbol *sym = hashTableLookup(builtinRegistryHash, canonical_name);
    if (!sym) {
        return NULL;
    }
    return (BuiltinRegistryEntry *)sym;
}

static void builtinRegistryInsertUnlocked(const char *canonical_name, size_t id) {
    if (!builtinRegistryHash || !canonical_name || !*canonical_name) {
        return;
    }

    BuiltinRegistryEntry *existing = lookupBuiltinEntryUnlocked(canonical_name);
    if (existing) {
        existing->id = id;
        if (existing->symbol.name && strcmp(existing->symbol.name, canonical_name) != 0) {
            char *dup = strdup(canonical_name);
            if (dup) {
                free(existing->symbol.name);
                existing->symbol.name = dup;
            }
        }
        return;
    }

    BuiltinRegistryEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return;
    }
    entry->symbol.name = strdup(canonical_name);
    if (!entry->symbol.name) {
        free(entry);
        return;
    }
    entry->symbol.is_alias = true;
    entry->id = id;
    hashTableInsert(builtinRegistryHash, &entry->symbol);
}

static VmBuiltinMapping *builtinRegistryLookupMappingUnlocked(const char *canonical_name, size_t *out_id) {
    if (out_id) {
        *out_id = SIZE_MAX;
    }
    if (!canonical_name || !*canonical_name) {
        return NULL;
    }

    if (builtinRegistryHash) {
        BuiltinRegistryEntry *entry = lookupBuiltinEntryUnlocked(canonical_name);
        if (!entry) {
            return NULL;
        }
        if (out_id) {
            *out_id = entry->id;
        }
        return builtinMappingFromId(entry->id);
    }

    for (size_t i = 0; i < num_vm_builtins; ++i) {
        if (strcasecmp(canonical_name, vmBuiltinDispatchTable[i].name) == 0) {
            if (out_id) {
                *out_id = i;
            }
            return &vmBuiltinDispatchTable[i];
        }
    }
    for (size_t i = 0; i < num_extra_vm_builtins; ++i) {
        if (strcasecmp(canonical_name, extra_vm_builtins[i].name) == 0) {
            if (out_id) {
                *out_id = num_vm_builtins + i;
            }
            return &extra_vm_builtins[i];
        }
    }
    return NULL;
}

static void initBuiltinRegistryMutex(void) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&builtin_registry_mutex, &attr);
    pthread_mutexattr_destroy(&attr);

    builtinRegistryHash = createHashTable();
    if (!builtinRegistryHash) {
        return;
    }

    for (size_t i = 0; i < num_vm_builtins; ++i) {
        char canonical[MAX_SYMBOL_LENGTH];
        if (canonicalizeBuiltinName(vmBuiltinDispatchTable[i].name, canonical, sizeof(canonical))) {
            builtinRegistryInsertUnlocked(canonical, i);
        }
    }
}

void registerVmBuiltin(const char *name, VmBuiltinFn handler,
        BuiltinRoutineType type, const char *display_name) {
    if (!name || !handler) return;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);

    if (type == BUILTIN_TYPE_FUNCTION || type == BUILTIN_TYPE_PROCEDURE) {
        const char *reg_name = display_name ? display_name : name;
        ASTNodeType declType = (type == BUILTIN_TYPE_FUNCTION)
                                   ? AST_FUNCTION_DECL
                                   : AST_PROCEDURE_DECL;
        registerBuiltinFunction(reg_name, declType, NULL);
    }

    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    size_t existing_id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, &existing_id);
    if (mapping) {
        mapping->handler = handler;
        pthread_mutex_unlock(&builtin_registry_mutex);
        return;
    }

    VmBuiltinMapping *new_table = realloc(extra_vm_builtins,
        sizeof(VmBuiltinMapping) * (num_extra_vm_builtins + 1));
    if (!new_table) {
        pthread_mutex_unlock(&builtin_registry_mutex);
        return;
    }
    extra_vm_builtins = new_table;
    extra_vm_builtins[num_extra_vm_builtins].name = strdup(canonical);
    extra_vm_builtins[num_extra_vm_builtins].handler = handler;
    size_t new_index = num_extra_vm_builtins;
    num_extra_vm_builtins++;
    builtinRegistryInsertUnlocked(canonical, num_vm_builtins + new_index);
    pthread_mutex_unlock(&builtin_registry_mutex);
}

// This function now comes AFTER the table and comparison function it uses.
VmBuiltinFn getVmBuiltinHandler(const char *name) {
    if (!name) return NULL;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return NULL;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    VmBuiltinFn handler = NULL;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, NULL);
    if (mapping) {
        handler = mapping->handler;
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return handler;
}

VmBuiltinFn getVmBuiltinHandlerById(int id) {
    if (id < 0) return NULL;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    VmBuiltinFn handler = NULL;
    if ((size_t)id < num_vm_builtins) {
        handler = vmBuiltinDispatchTable[id].handler;
    } else {
        size_t extra_index = (size_t)id - num_vm_builtins;
        if (extra_index < num_extra_vm_builtins) {
            handler = extra_vm_builtins[extra_index].handler;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return handler;
}

const char* getVmBuiltinNameById(int id) {
    if (id < 0) return NULL;
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    const char* name = NULL;
    if ((size_t)id < num_vm_builtins) {
        name = vmBuiltinDispatchTable[id].name;
    } else {
        size_t extra_index = (size_t)id - num_vm_builtins;
        if (extra_index < num_extra_vm_builtins) {
            name = extra_vm_builtins[extra_index].name;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return name;
}

bool getVmBuiltinMapping(const char *name, VmBuiltinMapping *out_mapping, int *out_id) {
    if (out_id) {
        *out_id = -1;
    }
    if (!name) {
        return false;
    }
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return false;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    size_t id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, &id);
    bool found = mapping != NULL;
    if (found) {
        if (out_mapping) {
            *out_mapping = *mapping;
        }
        if (out_id && id <= (size_t)INT_MAX) {
            *out_id = (int)id;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return found;
}

bool getVmBuiltinMappingCanonical(const char *canonical_name, VmBuiltinMapping *out_mapping, int *out_id) {
    if (out_id) {
        *out_id = -1;
    }
    if (!canonical_name || !*canonical_name) {
        return false;
    }

    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    size_t id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical_name, &id);
    bool found = mapping != NULL;
    if (found) {
        if (out_mapping) {
            *out_mapping = *mapping;
        }
        if (out_id && id <= (size_t)INT_MAX) {
            *out_id = (int)id;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);
    return found;
}

int getVmBuiltinID(const char *name) {
    if (!name) {
        return -1;
    }
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        return -1;
    }

    pthread_mutex_lock(&builtin_registry_mutex);
    size_t id = SIZE_MAX;
    VmBuiltinMapping *mapping = builtinRegistryLookupMappingUnlocked(canonical, &id);
    int result = -1;
    if (mapping && id != SIZE_MAX && id <= (size_t)INT_MAX) {
        if (id < num_vm_builtins) {
            if (mapping->handler) {
                result = (int)id;
            }
        } else if (id - num_vm_builtins < num_extra_vm_builtins) {
            result = (int)id;
        }
    }
    pthread_mutex_unlock(&builtin_registry_mutex);

    return result;
}

Value vmBuiltinSqr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Sqr expects 1 argument.");
        return makeInt(0);
    }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) {
        long long v = AS_INTEGER(arg);
        return makeInt(v * v);
    } else if (isRealType(arg.type)) {
        long double v = AS_REAL(arg);
        return makeReal(v * v);
    }
    runtimeError(vm, "Sqr expects an Integer or Real argument. Got %s.", varTypeToString(arg.type));
    return makeInt(0);
}

Value vmBuiltinChr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "Chr expects 1 integer argument.");
        return makeChar('\0');
    }
    long long code = AS_INTEGER(args[0]);
    if (code < 0 || code > PASCAL_CHAR_MAX) {
        runtimeError(vm, "Chr argument out of range.");
        return makeChar('\0');
    }
    return makeChar((int)code);
}

Value vmBuiltinSucc(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Succ expects 1 argument.");
        return makeVoid();
    }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) {
        return makeInt(AS_INTEGER(arg) + 1);
    }
    switch(arg.type) {
        case TYPE_CHAR:
            if (arg.c_val >= PASCAL_CHAR_MAX) {
                runtimeError(vm, "Succ char overflow.");
                return makeVoid();
            }
            return makeChar(arg.c_val + 1);
        case TYPE_BOOLEAN: {
            long long next_val = arg.i_val + 1;
            int bool_result = next_val > 1 ? 1 : (next_val != 0);
            return makeBoolean(bool_result);
        }
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
            runtimeError(vm, "Succ requires an ordinal type argument. Got %s.",
                         varTypeToString(arg.type));
            return makeVoid();
    }
}

Value vmBuiltinUpcase(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Upcase expects 1 argument, got %d.", arg_count);
        return makeChar('\0');
    }

    Value arg = args[0];
    int c;
    if (arg.type == TYPE_CHAR) {
        c = arg.c_val;
    } else if (IS_INTLIKE(arg)) {
        c = (int)AS_INTEGER(arg);
    } else if (IS_REAL(arg)) {
        /*
         * Some frontends currently promote integer literals or variables to a
         * floating‑point type when used as arguments.  Accept real numbers and
         * coerce them back to an integer so `toupper` behaves correctly even if
         * the value was widened to a real earlier in the pipeline.
         */
        c = (int)AS_REAL(arg);
    } else if (arg.type == TYPE_STRING) {
        const char* s = AS_STRING(arg);
        if (s && s[0] != '\0') {
            c = (unsigned char)s[0];
        } else {
            runtimeError(vm,
                         "Upcase expects a non-empty string or char argument. Got an empty string.");
            return makeChar('\0');
        }
    } else {
        runtimeError(vm,
                     "Upcase expects a char, int, or non-empty string argument. Got %s.",
                     varTypeToString(arg.type));
        return makeChar('\0');
    }
    return makeChar(toupper((unsigned char)c));
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

Value vmBuiltinPrintf(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "printf expects a format string as the first argument.");
        return makeInt(0);
    }
    const char* fmt = AS_STRING(args[0]);
    int arg_index = 1;
    for (size_t i = 0; fmt && fmt[i] != '\0'; i++) {
        char c = fmt[i];
        if (c == '\\' && fmt[i + 1] != '\0') {
            char esc = fmt[++i];
            switch (esc) {
                case 'n': fputc('\n', stdout); break;
                case 'r': fputc('\r', stdout); break;
                case 't': fputc('\t', stdout); break;
                case '\\': fputc('\\', stdout); break;
                case '"': fputc('"', stdout); break;
                default: fputc(esc, stdout); break;
            }
            continue;
        }
        if (c == '%' && fmt[i + 1] != '\0') {
            if (fmt[i + 1] == '%') {
                fputc('%', stdout);
                i++;
                continue;
            }
            size_t j = i + 1;
            char flags[8];
            size_t flag_len = 0;
            const char* flag_chars = "-+ #0'";
            while (fmt[j] && strchr(flag_chars, fmt[j]) != NULL) {
                if (flag_len + 1 < sizeof(flags)) {
                    flags[flag_len++] = fmt[j];
                }
                j++;
            }
            flags[flag_len] = '\0';
            bool width_specified = false;
            int width = 0;
            while (isdigit((unsigned char)fmt[j])) {
                width_specified = true;
                width = width * 10 + (fmt[j] - '0');
                j++;
            }
            int precision = -1;
            if (fmt[j] == '.') {
                j++;
                precision = 0;
                while (isdigit((unsigned char)fmt[j])) {
                    precision = precision * 10 + (fmt[j] - '0');
                    j++;
                }
            }
            // Parse length modifiers and record small-width flags; we normalize by truncating manually
            bool mod_h = false, mod_hh = false;
            char length_mod[3] = {0};
            size_t length_len = 0;
            if (fmt[j] == 'h') {
                mod_h = true;
                length_mod[length_len++] = 'h';
                j++;
                if (fmt[j] == 'h') {
                    mod_hh = true;
                    mod_h = false;
                    length_mod[length_len++] = 'h';
                    j++;
                }
            } else if (fmt[j] == 'l') {
                length_mod[length_len++] = 'l';
                j++;
                if (fmt[j] == 'l') {
                    length_mod[length_len++] = 'l';
                    j++;
                }
            } else {
                const char* length_mods = "Ljzt";
                while (fmt[j] && strchr(length_mods, fmt[j]) != NULL) {
                    if (length_len + 1 < sizeof(length_mod)) {
                        length_mod[length_len++] = fmt[j];
                    }
                    j++;
                }
            }
            length_mod[length_len < sizeof(length_mod) ? length_len : (sizeof(length_mod) - 1)] = '\0';
            char spec = fmt[j];
            if (spec == '\0') {
                runtimeError(vm, "printf: incomplete format specifier.");
                return makeInt(0);
            }
            char fmtbuf[32];
            char buf[256];
            size_t pos = 0;
            fmtbuf[pos++] = '%';
            if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], flags, flag_len);
                pos += flag_len;
            }
            if (width_specified) {
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            if (precision >= 0 && pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = '.';
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            if (length_len > 0 && pos + length_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], length_mod, length_len);
                pos += length_len;
            }
            if (pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = spec;
            }
            fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';
            bool has_wide_char_length = false;
            for (size_t n = 0; n < length_len; ++n) {
                if (length_mod[n] == 'l' || length_mod[n] == 'L') {
                    has_wide_char_length = true;
                    break;
                }
            }
            if (arg_index < arg_count) {
                Value v = args[arg_index++];
                switch (spec) {
                    case 'd':
                    case 'i': {
                        long long iv = asI64(v);
                        if (mod_hh) iv = (signed char)iv; else if (mod_h) iv = (short)iv; // wider mods use default
                        snprintf(buf, sizeof(buf), fmtbuf, iv);
                        fputs(buf, stdout);
                        break; }
                    case 'u':
                    case 'o':
                    case 'x':
                    case 'X': {
                        unsigned long long uv = (unsigned long long)asI64(v);
                        if (mod_hh) uv = (unsigned char)uv; else if (mod_h) uv = (unsigned short)uv;
                        snprintf(buf, sizeof(buf), fmtbuf, uv);
                        fputs(buf, stdout);
                        break; }
                    case 'f':
                    case 'F':
                    case 'e':
                    case 'E':
                    case 'g':
                    case 'G':
                    case 'a':
                    case 'A':
                        snprintf(buf, sizeof(buf), fmtbuf, (double)AS_REAL(v));
                        fputs(buf, stdout);
                        break;
                    case 'c': {
                        char ch = (v.type == TYPE_CHAR) ? v.c_val : (char)asI64(v);
                        char safe_fmt[sizeof(fmtbuf)];
                        const char* format = fmtbuf;
                        if (has_wide_char_length) {
                            strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                            safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                            char* mod_pos = strstr(safe_fmt, length_mod);
                            if (mod_pos) {
                                size_t remove_len = strlen(length_mod);
                                memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                                format = safe_fmt;
                            }
                        }
                        snprintf(buf, sizeof(buf), format, ch);
                        fputs(buf, stdout);
                        break;
                    }
                    case 's': {
                        const char* sv = (v.type == TYPE_STRING && v.s_val) ? v.s_val : "";
                        char safe_fmt[sizeof(fmtbuf)];
                        const char* format = fmtbuf;
                        if (has_wide_char_length) {
                            strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                            safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                            char* mod_pos = strstr(safe_fmt, length_mod);
                            if (mod_pos) {
                                size_t remove_len = strlen(length_mod);
                                memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                                format = safe_fmt;
                            }
                        }
                        snprintf(buf, sizeof(buf), format, sv);
                        fputs(buf, stdout);
                        break;
                    }
                    case 'p':
                        snprintf(buf, sizeof(buf), fmtbuf, (void*)(uintptr_t)asI64(v));
                        fputs(buf, stdout);
                        break;
                    default:
                        printValueToStream(v, stdout);
                        break;
                }
            } else {
                fputc('%', stdout);
                fputc(spec, stdout);
            }
            i = j;
            continue;
        }
        fputc(c, stdout);
    }
    fflush(stdout);
    return makeInt(0);
}

// fprintf(file, fmt, ...)
Value vmBuiltinFprintf(VM* vm, int arg_count, Value* args) {
    if (arg_count < 2) {
        runtimeError(vm, "fprintf expects at least (file, format).");
        return makeInt(0);
    }
    // Determine output FILE*
    FILE* output_stream = NULL;
    const Value* farg = &args[0];
    if (farg->type == TYPE_POINTER && farg->ptr_val) farg = (const Value*)farg->ptr_val;
    if (farg->type == TYPE_FILE && farg->f_val) {
        output_stream = farg->f_val;
    } else {
        runtimeError(vm, "fprintf first argument must be an open file.");
        return makeInt(0);
    }
    if (args[1].type != TYPE_STRING || !args[1].s_val) {
        runtimeError(vm, "fprintf expects a format string as the second argument.");
        return makeInt(0);
    }
    const char* fmt = AS_STRING(args[1]);
    int arg_index = 2;
    for (size_t i = 0; fmt && fmt[i] != '\0'; i++) {
        char c = fmt[i];
        if (c == '\\' && fmt[i + 1] != '\0') {
            char esc = fmt[++i];
            switch (esc) {
                case 'n': fputc('\n', output_stream); break;
                case 'r': fputc('\r', output_stream); break;
                case 't': fputc('\t', output_stream); break;
                case '\\': fputc('\\', output_stream); break;
                case '"': fputc('"', output_stream); break;
                default: fputc(esc, output_stream); break;
            }
            continue;
        }
        if (c == '%' && fmt[i + 1] != '\0') {
            if (fmt[i + 1] == '%') {
                fputc('%', output_stream);
                i++;
                continue;
            }
            size_t j = i + 1;
            char flags[8];
            size_t flag_len = 0;
            const char* flag_chars = "-+ #0'";
            while (fmt[j] && strchr(flag_chars, fmt[j]) != NULL) {
                if (flag_len + 1 < sizeof(flags)) {
                    flags[flag_len++] = fmt[j];
                }
                j++;
            }
            flags[flag_len] = '\0';
            bool width_specified = false;
            int width = 0;
            while (isdigit((unsigned char)fmt[j])) { width_specified = true; width = width * 10 + (fmt[j]-'0'); j++; }
            int precision = -1;
            if (fmt[j] == '.') {
                j++; precision = 0;
                while (isdigit((unsigned char)fmt[j])) { precision = precision * 10 + (fmt[j]-'0'); j++; }
            }
            bool mod_h = false, mod_hh = false;
            char length_mod[3] = {0};
            size_t length_len = 0;
            if (fmt[j] == 'h') {
                mod_h = true;
                length_mod[length_len++] = 'h';
                j++;
                if (fmt[j] == 'h') {
                    mod_hh = true;
                    mod_h = false;
                    length_mod[length_len++] = 'h';
                    j++;
                }
            } else if (fmt[j] == 'l') {
                length_mod[length_len++] = 'l';
                j++;
                if (fmt[j] == 'l') {
                    length_mod[length_len++] = 'l';
                    j++;
                }
            } else {
                const char* length_mods = "Ljzt";
                while (fmt[j] && strchr(length_mods, fmt[j]) != NULL) {
                    if (length_len + 1 < sizeof(length_mod)) {
                        length_mod[length_len++] = fmt[j];
                    }
                    j++;
                }
            }
            length_mod[length_len < sizeof(length_mod) ? length_len : (sizeof(length_mod) - 1)] = '\0';
            char spec = fmt[j];
            if (!spec) { runtimeError(vm, "fprintf: incomplete format specifier."); return makeInt(0); }
            char fmtbuf[32]; char buf[256];
            size_t pos = 0;
            fmtbuf[pos++] = '%';
            if (flag_len > 0 && pos + flag_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], flags, flag_len);
                pos += flag_len;
            }
            if (width_specified) {
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", width);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            if (precision >= 0 && pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = '.';
                int written = snprintf(&fmtbuf[pos], sizeof(fmtbuf) - pos, "%d", precision);
                if (written > 0) {
                    if ((size_t)written >= sizeof(fmtbuf) - pos) {
                        pos = sizeof(fmtbuf) - 1;
                    } else {
                        pos += (size_t)written;
                    }
                }
            }
            if (length_len > 0 && pos + length_len < sizeof(fmtbuf)) {
                memcpy(&fmtbuf[pos], length_mod, length_len);
                pos += length_len;
            }
            if (pos < sizeof(fmtbuf)) {
                fmtbuf[pos++] = spec;
            }
            fmtbuf[pos < sizeof(fmtbuf) ? pos : (sizeof(fmtbuf) - 1)] = '\0';
            bool has_wide_char_length = false;
            for (size_t n = 0; n < length_len; ++n) {
                if (length_mod[n] == 'l' || length_mod[n] == 'L') {
                    has_wide_char_length = true;
                    break;
                }
            }
            if (arg_index < arg_count) {
                Value v = args[arg_index++];
                switch (spec) {
                    case 'd': case 'i': {
                        long long iv = asI64(v);
                        if (mod_hh) iv = (signed char)iv; else if (mod_h) iv = (short)iv;
                        snprintf(buf, sizeof(buf), fmtbuf, iv);
                        fputs(buf, output_stream);
                        break; }
                    case 'u': case 'o': case 'x': case 'X': {
                        unsigned long long uv = (unsigned long long)asI64(v);
                        if (mod_hh) uv = (unsigned char)uv; else if (mod_h) uv = (unsigned short)uv;
                        snprintf(buf, sizeof(buf), fmtbuf, uv);
                        fputs(buf, output_stream);
                        break; }
                    case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
                        snprintf(buf, sizeof(buf), fmtbuf, (double)AS_REAL(v));
                        fputs(buf, output_stream);
                        break; }
                    case 'c': {
                        char ch = (v.type == TYPE_CHAR) ? v.c_val : (char)asI64(v);
                        char safe_fmt[sizeof(fmtbuf)];
                        const char* format = fmtbuf;
                        if (has_wide_char_length) {
                            strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                            safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                            char* mod_pos = strstr(safe_fmt, length_mod);
                            if (mod_pos) {
                                size_t remove_len = strlen(length_mod);
                                memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                                format = safe_fmt;
                            }
                        }
                        snprintf(buf, sizeof(buf), format, ch);
                        fputs(buf, output_stream);
                        break; }
                    case 's': {
                        const char* sv = (v.type == TYPE_STRING && v.s_val) ? v.s_val : "";
                        char safe_fmt[sizeof(fmtbuf)];
                        const char* format = fmtbuf;
                        if (has_wide_char_length) {
                            strncpy(safe_fmt, fmtbuf, sizeof(safe_fmt));
                            safe_fmt[sizeof(safe_fmt) - 1] = '\0';
                            char* mod_pos = strstr(safe_fmt, length_mod);
                            if (mod_pos) {
                                size_t remove_len = strlen(length_mod);
                                memmove(mod_pos, mod_pos + remove_len, strlen(mod_pos + remove_len) + 1);
                                format = safe_fmt;
                            }
                        }
                        snprintf(buf, sizeof(buf), format, sv);
                        fputs(buf, output_stream);
                        break; }
                    case 'p': {
                        snprintf(buf, sizeof(buf), fmtbuf, (void*)(uintptr_t)asI64(v));
                        fputs(buf, output_stream);
                        break; }
                    default:
                        printValueToStream(v, output_stream);
                        break;
                }
            } else {
                fputc('%', output_stream);
                fputc(spec, output_stream);
            }
            i = j;
            continue;
        }
        fputc(c, output_stream);
    }
    fflush(output_stream);
    return makeInt(0);
}

// fopen(path, mode) -> FILE
Value vmBuiltinFopen(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
        runtimeError(vm, "fopen expects (path:string, mode:string).");
        return makeVoid();
    }
    const char* path = AS_STRING(args[0]);
    const char* mode = AS_STRING(args[1]);
    FILE* f = fopen(path, mode);
    if (!f) {
        runtimeError(vm, "fopen failed for '%s'", path);
        return makeVoid();
    }
    Value v = makeVoid();
    v.type = TYPE_FILE;
    v.f_val = f;
    v.filename = strdup(path);
    return v;
}

// fclose(file)
Value vmBuiltinFclose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "fclose expects (file).");
        return makeVoid();
    }
    const Value* farg = &args[0];
    if (farg->type == TYPE_POINTER && farg->ptr_val) farg = (const Value*)farg->ptr_val;
    if (farg->type != TYPE_FILE || !farg->f_val) {
        runtimeError(vm, "fclose requires a valid file argument.");
        return makeVoid();
    }
    fclose(farg->f_val);
    return makeVoid();
}

Value vmBuiltinCopy(VM* vm, int arg_count, Value* args) {
    // Allow the first argument to be a char
    if (arg_count != 3 || (args[0].type != TYPE_STRING && args[0].type != TYPE_CHAR) || !IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2])) {
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

static bool resizeDynamicArrayValue(VM* vm,
                                    Value* array_value,
                                    int dimension_count,
                                    const long long* lengths) {
    if (!array_value || array_value->type != TYPE_ARRAY) {
        runtimeError(vm, "SetLength target is not an array.");
        return false;
    }
    if (dimension_count <= 0) {
        runtimeError(vm, "SetLength requires at least one dimension for arrays.");
        return false;
    }

    if (array_value->dimensions > 0 && array_value->dimensions != dimension_count) {
        runtimeError(vm, "SetLength dimension count (%d) does not match existing array (%d).",
                     dimension_count,
                     array_value->dimensions);
        return false;
    }

    VarType element_type = array_value->element_type;
    AST* element_type_def = array_value->element_type_def;

    int* new_lower = (int*)malloc(sizeof(int) * dimension_count);
    int* new_upper = (int*)malloc(sizeof(int) * dimension_count);
    if (!new_lower || !new_upper) {
        if (new_lower) free(new_lower);
        if (new_upper) free(new_upper);
        runtimeError(vm, "SetLength: memory allocation failed for array bounds.");
        return false;
    }

    size_t new_total = 1;
    bool saw_zero = false;
    for (int i = 0; i < dimension_count; ++i) {
        long long len = lengths[i];
        if (len < 0) {
            runtimeError(vm, "SetLength: array length must be non-negative.");
            free(new_lower);
            free(new_upper);
            return false;
        }
        if (len == 0) {
            new_lower[i] = 0;
            new_upper[i] = -1;
            saw_zero = true;
        } else {
            if (len > INT_MAX) {
                runtimeError(vm, "SetLength: array length exceeds supported range.");
                free(new_lower);
                free(new_upper);
                return false;
            }
            new_lower[i] = 0;
            new_upper[i] = (int)len - 1;
            if (!saw_zero) {
                if (new_total > SIZE_MAX / (size_t)len) {
                    runtimeError(vm, "SetLength: requested array size is too large.");
                    free(new_lower);
                    free(new_upper);
                    return false;
                }
                new_total *= (size_t)len;
            }
        }
    }

    if (saw_zero) {
        new_total = 0;
    }

    size_t old_total = 0;
    if (array_value->array_val && array_value->dimensions > 0 &&
        array_value->lower_bounds && array_value->upper_bounds) {
        old_total = 1;
        for (int i = 0; i < array_value->dimensions; ++i) {
            int span = array_value->upper_bounds[i] - array_value->lower_bounds[i] + 1;
            if (span <= 0) {
                old_total = 0;
                break;
            }
            old_total *= (size_t)span;
        }
    }

    Value* new_elements = NULL;
    int* copy_lower = NULL;
    int* copy_upper = NULL;
    int* copy_indices = NULL;
    if (new_total > 0) {
        new_elements = (Value*)malloc(sizeof(Value) * new_total);
        if (!new_elements) {
            runtimeError(vm, "SetLength: memory allocation failed for array contents.");
            goto setlength_cleanup_failure;
        }

        for (size_t i = 0; i < new_total; ++i) {
            new_elements[i] = makeValueForType(element_type, element_type_def, NULL);
        }

        if (old_total > 0 && array_value->array_val && array_value->lower_bounds &&
            array_value->upper_bounds && array_value->dimensions == dimension_count) {
            copy_lower = (int*)malloc(sizeof(int) * dimension_count);
            copy_upper = (int*)malloc(sizeof(int) * dimension_count);
            if (!copy_lower || !copy_upper) {
                runtimeError(vm, "SetLength: memory allocation failed while preserving array contents.");
                goto setlength_cleanup_failure;
            }

            bool has_overlap = true;
            for (int i = 0; i < dimension_count; ++i) {
                int overlap_low = array_value->lower_bounds[i] > new_lower[i] ?
                                  array_value->lower_bounds[i] : new_lower[i];
                int overlap_high = array_value->upper_bounds[i] < new_upper[i] ?
                                   array_value->upper_bounds[i] : new_upper[i];
                if (overlap_high < overlap_low) {
                    has_overlap = false;
                    break;
                }
                copy_lower[i] = overlap_low;
                copy_upper[i] = overlap_high;
            }

            if (has_overlap) {
                copy_indices = (int*)malloc(sizeof(int) * dimension_count);
                if (!copy_indices) {
                    runtimeError(vm, "SetLength: memory allocation failed while preserving array contents.");
                    goto setlength_cleanup_failure;
                }

                Value old_array_stub = *array_value;
                Value new_array_stub;
                memset(&new_array_stub, 0, sizeof(Value));
                new_array_stub.type = TYPE_ARRAY;
                new_array_stub.dimensions = dimension_count;
                new_array_stub.lower_bounds = new_lower;
                new_array_stub.upper_bounds = new_upper;

                for (int i = 0; i < dimension_count; ++i) {
                    copy_indices[i] = copy_lower[i];
                }

                while (true) {
                    int old_offset = computeFlatOffset(&old_array_stub, copy_indices);
                    int new_offset = computeFlatOffset(&new_array_stub, copy_indices);
                    freeValue(&new_elements[new_offset]);
                    new_elements[new_offset] = makeCopyOfValue(&array_value->array_val[old_offset]);

                    int dim = dimension_count - 1;
                    while (dim >= 0) {
                        if (copy_indices[dim] < copy_upper[dim]) {
                            copy_indices[dim]++;
                            break;
                        }
                        copy_indices[dim] = copy_lower[dim];
                        --dim;
                    }
                    if (dim < 0) {
                        break;
                    }
                }
            }
        }
    }

    if (copy_indices) {
        free(copy_indices);
        copy_indices = NULL;
    }
    if (copy_lower) {
        free(copy_lower);
        copy_lower = NULL;
    }
    if (copy_upper) {
        free(copy_upper);
        copy_upper = NULL;
    }

    if (array_value->array_val) {
        for (size_t i = 0; i < old_total; ++i) {
            freeValue(&array_value->array_val[i]);
        }
        free(array_value->array_val);
    }
    free(array_value->lower_bounds);
    free(array_value->upper_bounds);

    array_value->lower_bounds = new_lower;
    array_value->upper_bounds = new_upper;
    array_value->array_val = new_elements;
    array_value->dimensions = dimension_count;
    array_value->lower_bound = (dimension_count >= 1) ? new_lower[0] : 0;
    array_value->upper_bound = (dimension_count >= 1) ? new_upper[0] : -1;
    array_value->element_type = element_type;
    array_value->element_type_def = element_type_def;

    if (new_total == 0) {
        array_value->array_val = NULL;
    }

    return true;

setlength_cleanup_failure:
    if (copy_indices) {
        free(copy_indices);
    }
    if (copy_lower) {
        free(copy_lower);
    }
    if (copy_upper) {
        free(copy_upper);
    }
    if (new_elements) {
        for (size_t i = 0; i < new_total; ++i) {
            freeValue(&new_elements[i]);
        }
        free(new_elements);
    }
    free(new_lower);
    free(new_upper);
    return false;
}

Value vmBuiltinSetlength(VM* vm, int arg_count, Value* args) {
    if (arg_count < 2 || args[0].type != TYPE_POINTER) {
        runtimeError(vm, "SetLength expects a pointer target followed by length arguments.");
        return makeVoid();
    }

    Value* target = (Value*)args[0].ptr_val;
    if (!target) {
        runtimeError(vm, "SetLength received a nil pointer.");
        return makeVoid();
    }

    if (target->type != TYPE_ARRAY) {
        if (arg_count != 2 || !IS_INTLIKE(args[1])) {
            runtimeError(vm, "SetLength expects (var string, integer).");
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
            memset(new_buf + copy_len, ' ', (size_t)new_len - copy_len);
        }
        new_buf[new_len] = '\0';

        target->s_val = new_buf;
        target->max_length = -1;
        return makeVoid();
    }

    int dimension_count = arg_count - 1;
    long long* lengths = (long long*)malloc(sizeof(long long) * (size_t)dimension_count);
    if (!lengths) {
        runtimeError(vm, "SetLength: memory allocation failed.");
        return makeVoid();
    }

    for (int i = 0; i < dimension_count; ++i) {
        if (!IS_INTLIKE(args[i + 1])) {
            free(lengths);
            runtimeError(vm, "SetLength dimension arguments must be integers.");
            return makeVoid();
        }
        lengths[i] = AS_INTEGER(args[i + 1]);
    }

    if (!resizeDynamicArrayValue(vm, target, dimension_count, lengths)) {
        free(lengths);
        return makeVoid();
    }

    free(lengths);
    return makeVoid();
}

Value vmBuiltinRealtostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !isRealType(args[0].type)) {
        runtimeError(vm, "RealToStr expects 1 real argument.");
        return makeString("");
    }
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%Lf", AS_REAL(args[0]));
    return makeString(buffer);
}

Value vmBuiltinFormatfloat(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2 || !IS_NUMERIC(args[0])) {
        runtimeError(vm, "FormatFloat expects (numeric [, integer precision]).");
        return makeString("");
    }

    long double value = isRealType(args[0].type) ? AS_REAL(args[0]) : (long double)AS_INTEGER(args[0]);

    int precision = PASCAL_DEFAULT_FLOAT_PRECISION;
    if (arg_count == 2) {
        if (!IS_INTLIKE(args[1])) {
            runtimeError(vm, "FormatFloat precision must be an integer.");
            return makeString("");
        }
        long long requested = AS_INTEGER(args[1]);
        if (requested < 0) {
            requested = 0;
        } else if (requested > 18) {
            requested = 18;
        }
        precision = (int)requested;
    }

    char fmt[16];
    snprintf(fmt, sizeof(fmt), "%%.%dLf", precision);

    char buffer[128];
    int written = snprintf(buffer, sizeof(buffer), fmt, value);
    if (written < 0 || written >= (int)sizeof(buffer)) {
        runtimeError(vm, "FormatFloat failed to format value.");
        return makeString("");
    }

    return makeString(buffer);
}

Value vmBuiltinParamcount(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "ParamCount expects 0 arguments.");
        return makeInt64(0);
    }
    /*
     * ParamCount should reflect the number of command line parameters as an
     * integer.  Since the VM now supports multiple integer widths, use the
     * widest standard signed integer to avoid inadvertent promotion to a
     * floating type or other category.
     */
    return makeInt64(gParamCount);
}

Value vmBuiltinParamstr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
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
static int vm_termios_saved = 0;
static pthread_mutex_t vm_term_mutex = PTHREAD_MUTEX_INITIALIZER;

static _Thread_local int vm_raw_mode = 0;
static _Thread_local int vm_alt_screen_depth = 0; // Track nested alternate screen buffers

typedef struct {
    char fg[32];
    char bg[32];
    int valid;
} VmColorState;

#define VM_COLOR_STACK_MAX 16
static _Thread_local VmColorState vm_color_stack[VM_COLOR_STACK_MAX];
static _Thread_local int vm_color_stack_depth = 0;

static void vmEnableRawMode(void); // Forward declaration
static void vmSetupTermHandlers(void);
static void vmRegisterRestoreHandlers(void);
static void vmPushColorState(void);
static void vmPopColorState(void);
static void vmRestoreColorState(void);
static int vmQueryColor(const char *query, char *dest, size_t dest_size);
static void vmAtExitCleanup(void);

static pthread_key_t vm_thread_cleanup_key;
static pthread_once_t vm_thread_cleanup_key_once = PTHREAD_ONCE_INIT;
static pthread_once_t vm_restore_once = PTHREAD_ONCE_INIT;

static void vmThreadCleanup(void *unused) {
    (void)unused;
    vmAtExitCleanup();
}

static void vmCreateThreadKey(void) {
    pthread_key_create(&vm_thread_cleanup_key, vmThreadCleanup);
}

static int vmTcgetattr(int fd, struct termios *term) {
    int res;
    do {
        res = tcgetattr(fd, term);
    } while (res < 0 && errno == EINTR);
    return res;
}

static int vmTcsetattr(int fd, int optional_actions, const struct termios *term) {
    int res;
    do {
        res = tcsetattr(fd, optional_actions, term);
    } while (res < 0 && errno == EINTR);
    return res;
}

static void vmRestoreTerminal(void) {
    pthread_mutex_lock(&vm_term_mutex);
    if (vm_termios_saved && vm_raw_mode) {
        if (vmTcsetattr(STDIN_FILENO, TCSANOW, &vm_orig_termios) == 0) {
            vm_raw_mode = 0;
        }
    }
    pthread_mutex_unlock(&vm_term_mutex);
}

// Query terminal for current color (OSC 10/11) and store result in dest
static int vmQueryColor(const char *query, char *dest, size_t dest_size) {
    struct termios oldt, raw;
    char buf[64];
    size_t i = 0;
    char ch;

    if (!isatty(STDIN_FILENO))
        return -1;

    if (vmTcgetattr(STDIN_FILENO, &oldt) < 0)
        return -1;

    raw = oldt;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 5; // 0.5s timeout

    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &raw) < 0) {
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt);
        return -1;
    }

    if (write(STDOUT_FILENO, query, strlen(query)) == -1) {
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt);
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

    vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt);

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
    if (len > 0) {
        if (write(STDOUT_FILENO, seq, len) != len) {
            perror("vmRestoreColorState: write fg");
        }
    }
    len = snprintf(seq, sizeof(seq), "\x1B]11;%s\x07", cs->bg);
    if (len > 0) {
        if (write(STDOUT_FILENO, seq, len) != len) {
            perror("vmRestoreColorState: write bg");
        }
    }
}

// atexit handler: restore terminal settings and ensure cursor visibility
static void vmAtExitCleanup(void) {
    vmRestoreTerminal();
    if (isatty(STDOUT_FILENO)) {
        const char show_cursor[] = "\x1B[?25h"; // Ensure cursor is visible
        if (write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1) != (ssize_t)(sizeof(show_cursor) - 1)) {
            perror("vmAtExitCleanup: write show_cursor");
        }
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

static void vmRegisterRestoreHandlers(void) {
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
}

static void vmSetupTermHandlers(void) {
    pthread_once(&vm_thread_cleanup_key_once, vmCreateThreadKey);
    pthread_setspecific(vm_thread_cleanup_key, (void *)1);

    pthread_mutex_lock(&vm_term_mutex);
    if (!vm_termios_saved) {
        if (vmTcgetattr(STDIN_FILENO, &vm_orig_termios) == 0) {
            vm_termios_saved = 1;
        }
    }
    pthread_mutex_unlock(&vm_term_mutex);

    pthread_once(&vm_restore_once, vmRegisterRestoreHandlers);
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
    if (write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1) != (ssize_t)(sizeof(show_cursor) - 1)) {
        perror("vmPrepareCanonicalInput: write show_cursor");
    }

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
    pthread_mutex_lock(&vm_term_mutex);
    if (vm_raw_mode) {
        pthread_mutex_unlock(&vm_term_mutex);
        return;
    }

    if (!vm_termios_saved) {
        if (vmTcgetattr(STDIN_FILENO, &vm_orig_termios) != 0) {
            pthread_mutex_unlock(&vm_term_mutex);
            return;
        }
        vm_termios_saved = 1;
    }

    struct termios raw = vm_orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;

    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0) {
        vm_raw_mode = 1;
    }
    pthread_mutex_unlock(&vm_term_mutex);
}

// Restore the terminal to a canonical, line-buffered state suitable for
// Read()/ReadLn().  This undoes any prior ReadKey-induced raw mode,
// discards leftover input, ensures echoing, and makes the cursor visible.
static void vmPrepareCanonicalInput(void) {
    vmRestoreTerminal();
    tcflush(STDIN_FILENO, TCIFLUSH);
    const char show_cursor[] = "\x1B[?25h";
    if (write(STDOUT_FILENO, show_cursor, sizeof(show_cursor) - 1) != (ssize_t)(sizeof(show_cursor) - 1)) {
        perror("vmPrepareCanonicalInput: write show_cursor");
    }
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
    if (vmTcgetattr(STDIN_FILENO, &oldt) < 0) {
        perror("getCursorPosition: tcgetattr failed");
        return -1; // Critical failure
    }

    // --- Prepare and Set Raw Mode ---
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO); // Disable canonical mode and echo
    newt.c_cc[VMIN] = 0;              // Non-blocking read
    newt.c_cc[VTIME] = 2;             // Timeout 0.2 seconds (adjust if needed)

    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &newt) < 0) {
        int setup_errno = errno;
        perror("getCursorPosition: tcsetattr (set raw) failed");
        // Attempt to restore original settings even if setting new ones failed
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Best effort restore
        errno = setup_errno; // Restore errno for accurate reporting
        return -1; // Critical failure
    }

    // --- Write DSR Query ---
    const char *dsr_query = "\x1B[6n"; // ANSI Device Status Report for cursor position
    if (write(STDOUT_FILENO, dsr_query, strlen(dsr_query)) == -1) {
        int write_errno = errno;
        perror("getCursorPosition: write DSR query failed");
        vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt); // Restore terminal settings
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
    if (vmTcsetattr(STDIN_FILENO, TCSANOW, &oldt) < 0) {
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

Value vmBuiltinPollkeyany(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0) {
        runtimeError(vm, "PollKeyAny expects 0 arguments.");
        return makeInt(0);
    }

#ifdef SDL
    SDL_Keycode sdl_code;
    if (sdlPollNextKey(&sdl_code)) {
        return makeInt((int)sdl_code);
    }
#endif

    vmEnableRawMode();
    int bytes_available = 0;
    ioctl(STDIN_FILENO, FIONREAD, &bytes_available);
    if (bytes_available > 0) {
        unsigned char ch_byte = 0;
        if (read(STDIN_FILENO, &ch_byte, 1) == 1) {
            return makeInt((int)ch_byte);
        }
    }

    return makeInt(0);
}

Value vmBuiltinReadkey(VM* vm, int arg_count, Value* args) {
    if (arg_count != 0 && arg_count != 1) {
        runtimeError(vm, "ReadKey expects 0 or 1 argument.");
        return makeChar('\0');
    }

    int c = 0;
#ifdef SDL
    if (sdlIsGraphicsActive()) {
        c = sdlFetchReadKeyChar();
        if (c < 0) {
            c = 0;
        }
    } else
#endif
    {
        vmEnableRawMode();

        unsigned char ch_byte;
        if (read(STDIN_FILENO, &ch_byte, 1) != 1) {
            ch_byte = '\0';
        }
        c = ch_byte;
    }

    if (arg_count == 1) {
        if (args[0].type != TYPE_POINTER || args[0].ptr_val == NULL) {
            runtimeError(vm, "ReadKey argument must be a VAR char.");
        } else {
            Value* dst = (Value*)args[0].ptr_val;
                if (dst->type == TYPE_CHAR) {
                    dst->c_val = c;
                    SET_INT_VALUE(dst, dst->c_val);
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
    return makeBoolean(atomic_load(&break_requested) != 0);
}

Value vmBuiltinGotoxy(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
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
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextColor expects 1 integer argument.");
        return makeVoid();
    }
    long long colorCode = AS_INTEGER(args[0]);
    gCurrentTextColor = (int)(colorCode % 16);
    gCurrentTextBold = (colorCode >= 8 && colorCode <= 15);
    gCurrentColorIsExt = false;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinTextbackground(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextBackground expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)(AS_INTEGER(args[0]) % 8);
    gCurrentBgIsExt = false;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}
Value vmBuiltinTextcolore(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextColorE expects an integer argument.");
        return makeVoid();
    }
    gCurrentTextColor = (int)AS_INTEGER(args[0]);
    gCurrentTextBold = false;
    gCurrentColorIsExt = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinTextbackgrounde(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "TextBackgroundE expects 1 integer argument.");
        return makeVoid();
    }
    gCurrentTextBackground = (int)AS_INTEGER(args[0]);
    gCurrentBgIsExt = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinBoldtext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "BoldText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBold = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinUnderlinetext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "UnderlineText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextUnderline = true;
    markTextAttrDirty();
    return makeVoid();
}

Value vmBuiltinBlinktext(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "BlinkText expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBlink = true;

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinLowvideo(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "LowVideo expects no arguments.");
        return makeVoid();
    }
    gCurrentTextBold = false;
    gCurrentTextColor &= 0x07;

    markTextAttrDirty();

    syncTextAttrSymbol();
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

    markTextAttrDirty();

    syncTextAttrSymbol();
    return makeVoid();
}

Value vmBuiltinClrscr(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "ClrScr expects no arguments.");
        return makeVoid();
    }

    if (isatty(STDOUT_FILENO)) {
        bool color_was_applied = applyCurrentTextAttributes(stdout);
        fputs("\x1B[2J\x1B[H", stdout);
        if (color_was_applied) {
            resetTextAttributes(stdout);
        }
        fprintf(stdout, "\x1B[%d;%dH", gWindowTop, gWindowLeft);
        fflush(stdout);
    }

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

    markTextAttrDirty();

    syncTextAttrSymbol();
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
            if (write(STDOUT_FILENO, enter_alt, sizeof(enter_alt) - 1) != (ssize_t)(sizeof(enter_alt) - 1)) {
                perror("vmBuiltinPushscreen: write enter_alt");
            }
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
                if (write(STDOUT_FILENO, exit_alt, sizeof(exit_alt) - 1) != (ssize_t)(sizeof(exit_alt) - 1)) {
                    perror("vmBuiltinPopscreen: write exit_alt");
                }
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
        !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) ||
        !IS_INTLIKE(args[2]) || !IS_INTLIKE(args[3])) {
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
    long double x = IS_INTLIKE(arg) ? (long double)AS_INTEGER(arg) : AS_REAL(arg);
    if (x < 0) { runtimeError(vm, "sqrt expects a non-negative argument."); return makeReal(0.0); }
    if (arg.type == TYPE_LONG_DOUBLE) {
        return makeLongDouble(sqrtl(x));
    }
    return makeReal(sqrt((double)x));
}

Value vmBuiltinExp(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "exp expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(exp(x));
}

Value vmBuiltinLn(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ln expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    if (x <= 0) { runtimeError(vm, "ln expects a positive argument."); return makeReal(0.0); }
    return makeReal(log(x));
}

Value vmBuiltinCos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cos expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(cos(x));
}

Value vmBuiltinSin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sin expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(sin(x));
}

Value vmBuiltinTan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "tan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(tan(x));
}

Value vmBuiltinArctan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "arctan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(atan(x));
}

Value vmBuiltinArcsin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "arcsin expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(asin(x));
}

Value vmBuiltinArccos(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "arccos expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    return makeReal(acos(x));
}

Value vmBuiltinCotan(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cotan expects 1 argument."); return makeReal(0.0); }
    Value arg = args[0];
    double x = IS_INTLIKE(arg) ? (double)AS_INTEGER(arg) : (double)AS_REAL(arg);
    double t = tan(x);
    return makeReal(1.0 / t);
}

Value vmBuiltinPower(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "power expects 2 arguments.");
        return makeReal(0.0);
    }

    bool base_is_int = IS_INTLIKE(args[0]);
    bool exp_is_int  = IS_INTLIKE(args[1]);

    if (base_is_int && exp_is_int) {
        long long base = AS_INTEGER(args[0]);
        long long exp  = AS_INTEGER(args[1]);
        if (exp >= 0) {
            long long result = 1;
            long long b = base;
            long long e = exp;
            bool overflow = false;
            while (e > 0 && !overflow) {
                if (e & 1) {
                    overflow |= __builtin_mul_overflow(result, b, &result);
                }
                e >>= 1;
                if (e)
                    overflow |= __builtin_mul_overflow(b, b, &b);
            }
            if (!overflow) {
                return makeInt(result);
            }
            // fall through to real computation on overflow
        }
        // negative exponent falls through to real computation
    }

    double base = base_is_int ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    double exponent = exp_is_int ? (double)AS_INTEGER(args[1]) : (double)AS_REAL(args[1]);
    return makeReal(pow(base, exponent));
}

Value vmBuiltinLog10(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "log10 expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(log10(x));
}

Value vmBuiltinSinh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "sinh expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(sinh(x));
}

Value vmBuiltinCosh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "cosh expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(cosh(x));
}

Value vmBuiltinTanh(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "tanh expects 1 argument."); return makeReal(0.0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeReal(tanh(x));
}

Value vmBuiltinMax(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "max expects 2 arguments."); return makeInt(0); }

    bool aInt = IS_INTLIKE(args[0]);
    bool bInt = IS_INTLIKE(args[1]);

    if (aInt && bInt) {
        long long a = AS_INTEGER(args[0]);
        long long b = AS_INTEGER(args[1]);
        return makeInt((a > b) ? a : b);
    } else {
        double a = aInt ? (double)AS_INTEGER(args[0]) : AS_REAL(args[0]);
        double b = bInt ? (double)AS_INTEGER(args[1]) : AS_REAL(args[1]);
        return makeReal((a > b) ? a : b);
    }
}

Value vmBuiltinMin(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "min expects 2 arguments."); return makeInt(0); }

    bool aInt = IS_INTLIKE(args[0]);
    bool bInt = IS_INTLIKE(args[1]);

    if (aInt && bInt) {
        long long a = AS_INTEGER(args[0]);
        long long b = AS_INTEGER(args[1]);
        return makeInt((a < b) ? a : b);
    } else {
        double a = aInt ? (double)AS_INTEGER(args[0]) : AS_REAL(args[0]);
        double b = bInt ? (double)AS_INTEGER(args[1]) : AS_REAL(args[1]);
        return makeReal((a < b) ? a : b);
    }
}

Value vmBuiltinFloor(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "floor expects 1 argument."); return makeInt(0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeInt((long long)floor(x));
}

Value vmBuiltinCeil(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ceil expects 1 argument."); return makeInt(0); }
    double x = IS_INTLIKE(args[0]) ? (double)AS_INTEGER(args[0]) : (double)AS_REAL(args[0]);
    return makeInt((long long)ceil(x));
}

Value vmBuiltinTrunc(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "trunc expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) return makeInt(AS_INTEGER(arg));
    if (isRealType(arg.type)) return makeInt((long long)AS_REAL(arg));
    runtimeError(vm, "trunc expects a numeric argument.");
    return makeInt(0);
}

static inline bool isOrdinalDelta(const Value* v) {
    return isIntlikeType(v->type) || v->type == TYPE_CHAR /* || v->type == TYPE_BOOLEAN */;
}

static inline long long coerceDeltaToI64(const Value* v) {
    switch (v->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            return v->i_val;
        case TYPE_CHAR:
            return v->c_val;
        default:
            return 0;
    }
}

Value vmBuiltinOrd(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "ord expects 1 argument."); return makeInt(0); }
    Value arg = args[0];
    if (arg.type == TYPE_CHAR) return makeInt(arg.c_val);
    if (arg.type == TYPE_BOOLEAN) return makeInt(arg.i_val);
    if (arg.type == TYPE_ENUM) return makeInt(arg.enum_val.ordinal);
    if (IS_INTLIKE(arg)) return makeInt(AS_INTEGER(arg));
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
            SET_INT_VALUE(target, target->i_val + delta);
            break;

        case TYPE_BYTE: {
            long long next = target->i_val + delta;
            if (next < 0 || next > 255) {
                runtimeWarning(vm, "Warning: Range check error incrementing BYTE to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFF);
            break;
        }

        case TYPE_WORD: {
            long long next = target->i_val + delta;
            if (next < 0 || next > 65535) {
                runtimeWarning(vm, "Warning: Range check error incrementing WORD to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFFFF);
            break;
        }

        case TYPE_CHAR: {
            long long next = target->c_val + delta;
            if (next < 0 || next > PASCAL_CHAR_MAX) {
                runtimeWarning(vm, "Warning: Range check error incrementing CHAR to %lld.", next);
            }
            target->c_val = (int)next;
            SET_INT_VALUE(target, target->c_val);
            break;
        }

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
            SET_INT_VALUE(target, target->i_val - delta);
            break;

        case TYPE_BYTE: {
            long long next = target->i_val - delta;
            if (next < 0 || next > 255) {
                runtimeWarning(vm, "Warning: Range check error decrementing BYTE to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFF);
            break;
        }

        case TYPE_WORD: {
            long long next = target->i_val - delta;
            if (next < 0 || next > 65535) {
                runtimeWarning(vm, "Warning: Range check error decrementing WORD to %lld.", next);
            }
            SET_INT_VALUE(target, next & 0xFFFF);
            break;
        }

        case TYPE_CHAR: {
            long long next = target->c_val - delta;
            if (next < 0 || next > PASCAL_CHAR_MAX) {
                runtimeWarning(vm, "Warning: Range check error decrementing CHAR to %lld.", next);
            }
            target->c_val = (int)next;
            SET_INT_VALUE(target, target->c_val);
            break;
        }

        case TYPE_ENUM:
            target->enum_val.ordinal -= (int)delta;
            break;

        default:
            runtimeError(vm, "Cannot Dec a non-ordinal type.");
            break;
    }

    return makeVoid(); // procedure
}

typedef struct {
    bool hasBounds;
    bool hitNilPointer;
    int lower;
    int upper;
} ArrayBoundsResult;

static ArrayBoundsResult resolveFirstDimBounds(Value* arg) {
    ArrayBoundsResult result;
    result.hasBounds = false;
    result.hitNilPointer = false;
    result.lower = 0;
    result.upper = -1;

    Value* current = arg;
    for (int depth = 0; depth < 8 && current; ++depth) {
        if (current->type == TYPE_ARRAY) {
            int lower = 0;
            int upper = -1;
            if (current->dimensions > 0 && current->lower_bounds && current->upper_bounds) {
                lower = current->lower_bounds[0];
                upper = current->upper_bounds[0];
            } else {
                lower = current->lower_bound;
                upper = current->upper_bound;
            }
            result.hasBounds = true;
            result.lower = lower;
            result.upper = upper;
            return result;
        }

        if (current->type != TYPE_POINTER) {
            break;
        }

        if (!current->ptr_val) {
            result.hitNilPointer = true;
            return result;
        }

        Value* next = (Value*)current->ptr_val;
        if (next == current) {
            break;
        }
        current = next;
    }

    return result;
}

Value vmBuiltinLow(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Low() expects a single array or type identifier argument.");
        return makeInt(0);
    }

    Value arg = args[0];
    const char* typeName = NULL;
    AST* typeDef = NULL;
    VarType t = TYPE_UNKNOWN;

    ArrayBoundsResult bounds = resolveFirstDimBounds(&arg);
    if (bounds.hasBounds) {
        return makeInt(bounds.lower);
    }
    if (bounds.hitNilPointer) {
        runtimeError(vm, "Low() cannot dereference a nil array reference.");
        return makeInt(0);
    }

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
        runtimeError(vm, "High() expects a single array or type identifier argument.");
        return makeInt(0);
    }

    Value arg = args[0];
    const char* typeName = NULL;
    AST* typeDef = NULL;
    VarType t = TYPE_UNKNOWN;

    ArrayBoundsResult bounds = resolveFirstDimBounds(&arg);
    if (bounds.hasBounds) {
        return makeInt(bounds.upper);
    }
    if (bounds.hitNilPointer) {
        runtimeError(vm, "High() cannot dereference a nil array reference.");
        return makeInt(0);
    }

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
        case TYPE_CHAR:    return makeChar(PASCAL_CHAR_MAX);
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
    // (debug logging removed)
    // Determine base type. Default to INTEGER if metadata is unavailable.
    VarType baseVarType = baseTypeNode ? TYPE_VOID : TYPE_INT32;
    AST* actualBaseTypeDef = baseTypeNode;

    if (actualBaseTypeDef && actualBaseTypeDef->type == AST_VARIABLE && actualBaseTypeDef->token) {
        const char* typeName = actualBaseTypeDef->token->value;
        if (strcasecmp(typeName, "integer")==0) { baseVarType=TYPE_INTEGER; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "real")==0 || strcasecmp(typeName, "double")==0) { baseVarType=TYPE_DOUBLE; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "float")==0) { baseVarType=TYPE_FLOAT; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "char")==0) { baseVarType=TYPE_CHAR; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "string")==0 || strcasecmp(typeName, "str")==0) { baseVarType=TYPE_STRING; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "boolean")==0 || strcasecmp(typeName, "bool")==0) { baseVarType=TYPE_BOOLEAN; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "byte")==0) { baseVarType=TYPE_BYTE; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "word")==0) { baseVarType=TYPE_WORD; actualBaseTypeDef = NULL; }
        else if (strcasecmp(typeName, "int")==0) { baseVarType=TYPE_INT32; actualBaseTypeDef = NULL; }
        else {
            AST* lookedUpType = lookupType(typeName);
            if (!lookedUpType) { runtimeError(vm, "Cannot resolve base type '%s' in new().", typeName); return makeVoid(); }
            actualBaseTypeDef = lookedUpType;
            baseVarType = actualBaseTypeDef->var_type;
        }
    } else if (actualBaseTypeDef) {
         baseVarType = actualBaseTypeDef->var_type;
    }

    if (baseVarType == TYPE_VOID) {
        // Final fallback: allocate as INTEGER
        baseVarType = TYPE_INT32;
        actualBaseTypeDef = NULL;
    }
    
    Value* allocated_memory = malloc(sizeof(Value));
    if (!allocated_memory) { runtimeError(vm, "Memory allocation failed in new()."); return makeVoid(); }
    
    *(allocated_memory) = makeValueForType(baseVarType, actualBaseTypeDef, NULL);
    // (debug logging removed)

    // Update the pointer variable that was passed by reference
    pointerVarValuePtr->ptr_val = allocated_memory;
    pointerVarValuePtr->type = TYPE_POINTER;

    // Safety: if base type metadata is unknown, treat as integer for subsequent dereferences
    if (!pointerVarValuePtr->base_type_node) {
        Token* baseTok = newToken(TOKEN_IDENTIFIER, "integer", 0, 0);
        AST* baseNode = newASTNode(AST_VARIABLE, baseTok);
        setTypeAST(baseNode, TYPE_INT32);
        freeToken(baseTok);
        pointerVarValuePtr->base_type_node = baseNode;
    }
    // (debug logging removed)

    return makeVoid();
}

// newobj(typeName: string): pointer
Value vmBuiltinNewObj(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING || !args[0].s_val) {
        runtimeError(vm, "newobj expects 1 string type name.");
        return makeNil();
    }
    const char* typeName = args[0].s_val;
    AST* typeDef = lookupType(typeName);
    if (!typeDef) {
        runtimeError(vm, "newobj: unknown type '%s'", typeName ? typeName : "");
        return makeNil();
    }
    VarType vt = typeDef->var_type;
    Value* allocated = (Value*)malloc(sizeof(Value));
    if (!allocated) { runtimeError(vm, "newobj: allocation failed"); return makeNil(); }
    *allocated = makeValueForType(vt, typeDef, NULL);
    Value ret = makeVoid();
    ret.type = TYPE_POINTER;
    ret.ptr_val = allocated;
    ret.base_type_node = typeDef;
    return ret;
}

Value vmBuiltinExit(VM* vm, int arg_count, Value* args) {
    if (arg_count > 1 || (arg_count == 1 && !IS_INTLIKE(args[0]))) {
        runtimeError(vm, "exit expects 0 or 1 integer argument.");
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

Value vmBuiltinAppend(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Append requires 1 argument."); return makeVoid(); }

    if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
        runtimeError(vm, "Append: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)args[0].ptr_val;

    if (fileVarLValue->type != TYPE_FILE) { runtimeError(vm, "Argument to Append must be a file variable."); return makeVoid(); }
    if (fileVarLValue->filename == NULL) { runtimeError(vm, "File variable not assigned a name before Append."); return makeVoid(); }
    if (fileVarLValue->f_val) fclose(fileVarLValue->f_val);

    FILE* f = fopen(fileVarLValue->filename, "a");
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

Value vmBuiltinRename(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) { runtimeError(vm, "Rename requires 2 arguments."); return makeVoid(); }

    if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
        runtimeError(vm, "Rename: First argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)args[0].ptr_val;

    if (fileVarLValue->type != TYPE_FILE) { runtimeError(vm, "First argument to Rename must be a file variable."); return makeVoid(); }
    if (fileVarLValue->filename == NULL) { runtimeError(vm, "File variable not assigned a name before Rename."); return makeVoid(); }
    if (args[1].type != TYPE_STRING) { runtimeError(vm, "Second argument to Rename must be a string."); return makeVoid(); }
    if (fileVarLValue->f_val) { fclose(fileVarLValue->f_val); fileVarLValue->f_val = NULL; }

    int res = rename(fileVarLValue->filename, args[1].s_val);
    if (res != 0) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
        free(fileVarLValue->filename);
        fileVarLValue->filename = args[1].s_val ? strdup(args[1].s_val) : NULL;
        if (args[1].s_val && !fileVarLValue->filename) {
            runtimeError(vm, "Memory allocation failed for filename in Rename.");
        }
    }
    return makeVoid();
}

Value vmBuiltinErase(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Erase requires 1 argument."); return makeVoid(); }

    if (args[0].type != TYPE_POINTER || !args[0].ptr_val) {
        runtimeError(vm, "Erase: Argument must be a VAR file parameter.");
        return makeVoid();
    }
    Value* fileVarLValue = (Value*)args[0].ptr_val;

    if (fileVarLValue->type != TYPE_FILE) { runtimeError(vm, "Argument to Erase must be a file variable."); return makeVoid(); }
    if (fileVarLValue->filename == NULL) { runtimeError(vm, "File variable not assigned a name before Erase."); return makeVoid(); }
    if (fileVarLValue->f_val) { fclose(fileVarLValue->f_val); fileVarLValue->f_val = NULL; }

    int res = remove(fileVarLValue->filename);
    if (res != 0) {
        last_io_error = errno ? errno : 1;
    } else {
        last_io_error = 0;
    }
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
            dst->c_val = ch;
            SET_INT_VALUE(dst, dst->c_val);
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
                SET_INT_VALUE(dst, v);
                break;
            }
            case TYPE_FLOAT: {
                errno = 0;
                float v = strtof(buffer, NULL);
                if (errno == ERANGE) { last_io_error = 1; v = 0.0f; }
                SET_REAL_VALUE(dst, v);
                break;
            }
            case TYPE_REAL: {
                errno = 0;
                double v = strtod(buffer, NULL);
                if (errno == ERANGE) { last_io_error = 1; v = 0.0; }
                SET_REAL_VALUE(dst, v);
                break;
            }
            case TYPE_BOOLEAN: {
                if (strcasecmp(buffer, "true") == 0 || strcmp(buffer, "1") == 0) {
                    SET_INT_VALUE(dst, 1);
                } else if (strcasecmp(buffer, "false") == 0 || strcmp(buffer, "0") == 0) {
                    SET_INT_VALUE(dst, 0);
                } else {
                    SET_INT_VALUE(dst, 0);
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
            case TYPE_INT8:
            case TYPE_INT16:
            case TYPE_INT32: /* TYPE_INTEGER */
            case TYPE_INT64: {
                errno = 0;
                char* endp = NULL;
                long long v = strtoll(p, &endp, 10);
                if (endp == p || errno == ERANGE) { last_io_error = 1; v = 0; }
                SET_INT_VALUE(dst, v);
                p = endp ? endp : p;
                break;
            }
            case TYPE_UINT8:
            case TYPE_BYTE:
            case TYPE_UINT16:
            case TYPE_WORD:
            case TYPE_UINT32:
            case TYPE_UINT64: {
                errno = 0;
                char* endp = NULL;
                unsigned long long v = strtoull(p, &endp, 10);
                if (endp == p || errno == ERANGE) { last_io_error = 1; v = 0; }
                SET_INT_VALUE(dst, v);
                p = endp ? endp : p;
                break;
            }
            case TYPE_FLOAT:
            case TYPE_DOUBLE: /* TYPE_REAL */
            case TYPE_LONG_DOUBLE: {
                errno = 0;
                char* endp = NULL;
                long double v = strtold(p, &endp);
                if (endp == p || errno == ERANGE) { last_io_error = 1; v = 0.0; }
                SET_REAL_VALUE(dst, v);
                p = endp ? endp : p;
                break;
            }
            case TYPE_BOOLEAN: {
                if (strncasecmp(p, "true", 4) == 0) {
                    SET_INT_VALUE(dst, 1);
                    p += 4;
                } else if (strncasecmp(p, "false", 5) == 0) {
                    SET_INT_VALUE(dst, 0);
                    p += 5;
                } else {
                    errno = 0;
                    char* endp = NULL;
                    long long v = strtoll(p, &endp, 10);
                    if (endp == p || errno == ERANGE) { last_io_error = 1; v = 0; }
                    SET_INT_VALUE(dst, v ? 1 : 0);
                    p = endp ? endp : p;
                }
                break;
            }
            case TYPE_CHAR: {
                if (*p) {
                dst->c_val = (unsigned char)*p++;
                SET_INT_VALUE(dst, dst->c_val);
                } else {
                    dst->c_val = '\0';
                    SET_INT_VALUE(dst, 0);
                    last_io_error = 1;
                }
                break;
            }

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

Value vmBuiltinWrite(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1) {
        runtimeError(vm, "Write expects at least a newline flag.");
        return makeVoid();
    }

    bool newline = false;
    bool suppress_spacing = (gSuppressWriteSpacing != 0);
    bool suppress_spacing_flag = false;
    Value flag = args[0];
    if (isRealType(flag.type)) {
        newline = (AS_REAL(flag) != 0.0);
    } else if (IS_INTLIKE(flag)) {
        long long raw = AS_INTEGER(flag);
        newline = (raw & VM_WRITE_FLAG_NEWLINE) != 0;
        suppress_spacing_flag = ((raw & VM_WRITE_FLAG_SUPPRESS_SPACING) != 0);
        suppress_spacing = suppress_spacing || suppress_spacing_flag;
    } else if (flag.type == TYPE_BOOLEAN) {
        newline = flag.i_val != 0;
    } else if (flag.type == TYPE_CHAR) {
        newline = flag.c_val != 0;
    }

    FILE* output_stream = stdout;
    int start_index = 1;
    bool first_arg_is_file_by_value = false;

    if (arg_count > 1) {
        const Value* first = &args[1];
        if (first->type == TYPE_POINTER && first->ptr_val) first = (const Value*)first->ptr_val;
        if (first->type == TYPE_FILE) {
            if (!first->f_val) {
                runtimeError(vm, "File not open for writing.");
                return makeVoid();
            }
            output_stream = first->f_val;
            start_index = 2;
            if (args[1].type == TYPE_FILE) first_arg_is_file_by_value = true;
        }
    }

    int print_arg_count = arg_count - start_index;
    if (print_arg_count > MAX_WRITE_ARGS_VM) {
        runtimeError(vm, "VM Error: Too many arguments for WRITE/WRITELN (max %d).", MAX_WRITE_ARGS_VM);
        return makeVoid();
    }

    bool color_was_applied = false;
    if (output_stream == stdout) {
        color_was_applied = applyCurrentTextAttributes(output_stream);
    }

    Value prev = makeVoid();
    bool has_prev = false;
    for (int i = start_index; i < arg_count; i++) {
        Value val = args[i];
        if (!suppress_spacing && has_prev) {
            bool add_space = true;
            const char *no_space_after = "=,.;:?!-)]}>)\"'";
            if (prev.type == TYPE_STRING && prev.s_val) {
                size_t len = strlen(prev.s_val);
                if (len == 0) {
                    add_space = false;
                } else {
                    char last = prev.s_val[len - 1];
                    if (isspace((unsigned char)last) || strchr(no_space_after, last)) {
                        add_space = false;
                    }
                }
            } else if (prev.type == TYPE_CHAR) {
                char last = (char)prev.c_val;
                if (isspace((unsigned char)last) || strchr(no_space_after, last)) {
                    add_space = false;
                }
            }
            if (val.type == TYPE_STRING && val.s_val) {
                size_t len_cur = strlen(val.s_val);
                if (len_cur > 0) {
                    char first = val.s_val[0];
                    if (isspace((unsigned char)first) || strchr(",.;:)]}!?)", first)) {
                        add_space = false;
                    }
                }
            } else if (val.type == TYPE_CHAR) {
                char first = (char)val.c_val;
                if (isspace((unsigned char)first) || strchr(",.;:)]}!?", first)) {
                    add_space = false;
                }
            }
            if (add_space) {
                fputc(' ', output_stream);
            }
        }
        if (suppress_spacing_flag && val.type == TYPE_BOOLEAN) {
            fputs(val.i_val ? "1" : "0", output_stream);
        } else if (val.type == TYPE_STRING) {
            if (output_stream == stdout) {
                fputs(val.s_val ? val.s_val : "", output_stream);
            } else {
                size_t len = val.s_val ? strlen(val.s_val) : 0;
                fwrite(val.s_val ? val.s_val : "", 1, len, output_stream);
            }
        } else if (val.type == TYPE_CHAR) {
            fputc(val.c_val, output_stream);
        } else {
            printValueToStream(val, output_stream);
        }
        prev = val;
        has_prev = true;
    }

    if (newline) {
        fprintf(output_stream, "\n");
    }

    if (color_was_applied) {
        resetTextAttributes(output_stream);
    }

    fflush(output_stream);
    if (first_arg_is_file_by_value) { args[1].type = TYPE_NIL; args[1].f_val = NULL; }

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
    rand_seed = (unsigned int)time(NULL);
    return makeVoid();
}

Value vmBuiltinRandom(VM* vm, int arg_count, Value* args) {
    if (arg_count == 0) {
        return makeReal((double)rand_r(&rand_seed) / ((double)RAND_MAX + 1.0));
    }
    if (arg_count == 1 && IS_INTLIKE(args[0])) {
        long long n = AS_INTEGER(args[0]);
        if (n <= 0) { runtimeError(vm, "Random argument must be > 0."); return makeInt(0); }
        return makeInt(rand_r(&rand_seed) % n);
    }
    runtimeError(vm, "Random requires 0 arguments, or 1 integer argument.");
    return makeVoid();
}

// --- VM BUILT-IN IMPLEMENTATIONS: DOS/OS ---

Value vmBuiltinDosGetenv(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dosGetenv expects 1 string argument.");
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
        !IS_INTLIKE(args[1])) {
        runtimeError(vm, "getEnvInt expects (string, integer).");
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
    if (dst->type == TYPE_REAL || dst->type == TYPE_FLOAT) {
        double r = strtod(s, &endptr);
        if (errno != 0 || (endptr && *endptr != '\0')) {
            *code = makeInt((int)((endptr ? endptr : s) - s) + 1);
        } else {
            SET_REAL_VALUE(dst, r);
            *code = makeInt(0);
        }
    } else {
        long long n = strtoll(s, &endptr, 10);
        if (errno != 0 || (endptr && *endptr != '\0')) {
            *code = makeInt((int)((endptr ? endptr : s) - s) + 1);
        } else {
            SET_INT_VALUE(dst, n);
            *code = makeInt(0);
        }
    }
    return makeVoid();
}

Value vmBuiltinValreal(VM* vm, int arg_count, Value* args) {
    return vmBuiltinVal(vm, arg_count, args);
}

Value vmBuiltinVMVersion(VM* vm, int arg_count, Value* args) {
    (void)vm; (void)args;
    return arg_count == 0 ? makeInt(pscal_vm_version()) : makeInt(-1);
}

Value vmBuiltinBytecodeVersion(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0 || !vm || !vm->chunk) return makeInt(-1);
    return makeInt(vm->chunk->version);
}

Value vmBuiltinDosExec(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
        runtimeError(vm, "dosExec expects 2 string arguments.");
        return makeInt(-1);
    }
    const char* path = AS_STRING(args[0]);
    const char* cmdline = AS_STRING(args[1]);
    size_t len = strlen(path) + strlen(cmdline) + 2;
    char* cmd = malloc(len);
    if (!cmd) {
        runtimeError(vm, "dosExec memory allocation failed.");
        return makeInt(-1);
    }
    snprintf(cmd, len, "%s %s", path, cmdline);
    int res = system(cmd);
    free(cmd);
    return makeInt(res);
}

Value vmBuiltinDosMkdir(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dosMkdir expects 1 string argument.");
        return makeInt(errno);
    }
    int rc = mkdir(AS_STRING(args[0]), 0777);
    return makeInt(rc == 0 ? 0 : errno);
}

Value vmBuiltinDosRmdir(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dosRmdir expects 1 string argument.");
        return makeInt(errno);
    }
    int rc = rmdir(AS_STRING(args[0]));
    return makeInt(rc == 0 ? 0 : errno);
}

Value vmBuiltinDosFindfirst(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dosFindfirst expects 1 string argument.");
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
        runtimeError(vm, "dosFindnext expects 0 arguments.");
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
        runtimeError(vm, "dosGetfattr expects 1 string argument.");
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
        runtimeError(vm, "dosGetdate expects 4 var arguments.");
        return makeVoid();
    }
    time_t t = time(NULL);
    struct tm tm_info;
#if defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_info, &t);
#else
    if (!localtime_r(&t, &tm_info)) {
        struct tm *tmp = localtime(&t);
        if (tmp) tm_info = *tmp; else memset(&tm_info, 0, sizeof(tm_info));
    }
#endif
    Value* year = (Value*)args[0].ptr_val;
    Value* month = (Value*)args[1].ptr_val;
    Value* day = (Value*)args[2].ptr_val;
    Value* dow = (Value*)args[3].ptr_val;
    if (year) { year->type = TYPE_WORD; SET_INT_VALUE(year, tm_info.tm_year + 1900); }
    if (month) { month->type = TYPE_WORD; SET_INT_VALUE(month, tm_info.tm_mon + 1); }
    if (day) { day->type = TYPE_WORD; SET_INT_VALUE(day, tm_info.tm_mday); }
    if (dow) { dow->type = TYPE_WORD; SET_INT_VALUE(dow, tm_info.tm_wday); }
    return makeVoid();
}

Value vmBuiltinDosGettime(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4) {
        runtimeError(vm, "dosGettime expects 4 var arguments.");
        return makeVoid();
    }
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm_info;
#if defined(_WIN32) && !defined(__CYGWIN__)
    localtime_s(&tm_info, &tv.tv_sec);
#else
    if (!localtime_r(&tv.tv_sec, &tm_info)) {
        struct tm *tmp = localtime(&tv.tv_sec);
        if (tmp) tm_info = *tmp; else memset(&tm_info, 0, sizeof(tm_info));
    }
#endif
    Value* hour = (Value*)args[0].ptr_val;
    Value* min = (Value*)args[1].ptr_val;
    Value* sec = (Value*)args[2].ptr_val;
    Value* sec100 = (Value*)args[3].ptr_val;
    if (hour) { hour->type = TYPE_WORD; SET_INT_VALUE(hour, tm_info.tm_hour); }
    if (min) { min->type = TYPE_WORD; SET_INT_VALUE(min, tm_info.tm_min); }
    if (sec) { sec->type = TYPE_WORD; SET_INT_VALUE(sec, tm_info.tm_sec); }
    if (sec100) { sec100->type = TYPE_WORD; SET_INT_VALUE(sec100, (int)(tv.tv_usec / 10000)); }
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
    MStream *ms = createMStream();
    if (!ms) {
        runtimeError(vm, "Memory allocation error for MStream structure in MStreamCreate.");
        return makeVoid();
    }
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
    size_t read_bytes = fread(buffer, 1, size, f);
    if (read_bytes != size) {
        fprintf(stderr, "MStreamLoadFromFile: short read or read error.\n");
        free(buffer);
        fclose(f);
        return makeBoolean(0);
    }
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
        releaseMStream(ms);
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

Value vmBuiltinMstreamFromString(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "MStreamFromString expects 1 argument (string).");
        return makeMStream(NULL);
    }

    if (!builtinValueIsStringLike(&args[0])) {
        runtimeError(vm, "MStreamFromString requires a string argument.");
        return makeMStream(NULL);
    }

    const char* payload = builtinValueToCString(&args[0]);
    if (!payload) {
        payload = "";
    }

    size_t len = strlen(payload);
    size_t capacity = len + 1;

    MStream* ms = createMStream();
    if (!ms) {
        runtimeError(vm, "MStreamFromString failed to allocate stream.");
        return makeMStream(NULL);
    }

    if (capacity > 0) {
        unsigned char* buffer = (unsigned char*)malloc(capacity);
        if (!buffer) {
            releaseMStream(ms);
            runtimeError(vm, "MStreamFromString failed to allocate buffer.");
            return makeMStream(NULL);
        }
        if (len > 0) {
            memcpy(buffer, payload, len);
        }
        buffer[len] = '\0';
        ms->buffer = buffer;
        ms->capacity = (int)capacity;
        ms->size = (int)len;
    }

    return makeMStream(ms);
}

Value vmBuiltinReal(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Real() expects 1 argument.");
        return makeReal(0.0);
    }
    Value arg = args[0];
    if (IS_INTLIKE(arg)) {
        return makeReal((double)AS_INTEGER(arg));
    }
    if (arg.type == TYPE_CHAR) {
        return makeReal((double)arg.c_val);
    }
    if (isRealType(arg.type)) {
        return makeReal(AS_REAL(arg));
    }
    runtimeError(vm, "Real() argument must be an Integer, Ordinal, or Real type. Got %s.", varTypeToString(arg.type));
    return makeReal(0.0);
}


Value vmBuiltinInttostr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "IntToStr requires 1 argument."); return makeString(""); }
    Value arg = args[0];
    long long value_to_convert = 0;
    if (IS_INTLIKE(arg)) {
        value_to_convert = AS_INTEGER(arg);
    } else if (arg.type == TYPE_CHAR) {
        value_to_convert = (long long)arg.c_val;
    } else {
        runtimeError(vm, "IntToStr requires an integer-compatible argument."); return makeString("");
    }
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%lld", value_to_convert);
    return makeString(buffer);
}

Value vmBuiltinStr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[1].type != TYPE_POINTER) {
        runtimeError(vm, "Str expects (value, var string).");
        return makeVoid();
    }
    Value val = args[0];
    Value* dest = (Value*)args[1].ptr_val;
    if (!dest) {
        runtimeError(vm, "Str received a nil pointer.");
        return makeVoid();
    }

    char* new_buf = NULL;
    if (val.type == TYPE_STRING) {
        const char* src = val.s_val ? val.s_val : "";
        new_buf = strdup(src);
    } else {
        char buffer[64];
        switch (val.type) {
            case TYPE_CHAR:
                snprintf(buffer, sizeof(buffer), "%c", val.c_val);
                break;
            case TYPE_BOOLEAN:
                snprintf(buffer, sizeof(buffer), "%s", val.i_val ? "TRUE" : "FALSE");
                break;
            default:
                if (IS_INTLIKE(val)) {
                    snprintf(buffer, sizeof(buffer), "%lld", AS_INTEGER(val));
                } else if (isRealType(val.type)) {
                    snprintf(buffer, sizeof(buffer), "%Lf", AS_REAL(val));
                } else {
                    runtimeError(vm, "Str expects a numeric, char, or formatted string argument.");
                    return makeVoid();
                }
                break;
        }
        new_buf = strdup(buffer);
    }
    if (!new_buf) {
        runtimeError(vm, "Str: memory allocation failed.");
        return makeVoid();
    }
    freeValue(dest);
    dest->type = TYPE_STRING;
    dest->s_val = new_buf;
    dest->max_length = -1;
    return makeVoid();
}

Value vmBuiltinLength(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "Length expects 1 argument.");
        return makeInt(0);
    }

    Value arg = args[0];

    if (arg.type == TYPE_POINTER) {
        if (!arg.ptr_val) {
            runtimeError(vm, "Length() cannot dereference a nil pointer argument.");
            return makeInt(0);
        }
        Value* pointed = (Value*)arg.ptr_val;
        if (pointed->type == TYPE_STRING) {
            return makeInt(pointed->s_val ? (long long)strlen(pointed->s_val) : 0);
        }
    }

    if (arg.type == TYPE_STRING) {
        return makeInt(arg.s_val ? (long long)strlen(arg.s_val) : 0);
    }

    if (arg.type == TYPE_CHAR) {
        return makeInt(1);
    }

    ArrayBoundsResult bounds = resolveFirstDimBounds(&arg);
    if (bounds.hasBounds) {
        long long len = (long long)bounds.upper - (long long)bounds.lower + 1;
        if (len < 0) {
            len = 0;
        }
        return makeInt(len);
    }
    if (bounds.hitNilPointer) {
        runtimeError(vm, "Length() cannot dereference a nil array reference.");
        return makeInt(0);
    }

    runtimeError(vm, "Length expects a string or array argument.");
    return makeInt(0);
}


Value vmBuiltinAbs(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "abs expects 1 argument."); return makeInt(0); }
    if (IS_INTLIKE(args[0])) return makeInt(llabs(AS_INTEGER(args[0])));
    if (isRealType(args[0].type)) return makeReal(fabsl(AS_REAL(args[0])));
    runtimeError(vm, "abs expects a numeric argument.");
    return makeInt(0);
}

Value vmBuiltinRound(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) { runtimeError(vm, "Round expects 1 argument."); return makeInt(0); }
    if (isRealType(args[0].type)) return makeInt((long long)llround(AS_REAL(args[0])));
    if (IS_INTLIKE(args[0])) return makeInt(AS_INTEGER(args[0]));
    runtimeError(vm, "Round expects a numeric argument.");
    return makeInt(0);
}

Value vmBuiltinHalt(VM* vm, int arg_count, Value* args) {
    long long code = 0;
    if (arg_count == 0) {
        // No exit code supplied, default to 0.
    } else if (arg_count == 1 && IS_INTLIKE(args[0])) {
        code = AS_INTEGER(args[0]);
    } else {
        runtimeError(vm, "Halt expects 0 or 1 integer argument.");
    }
    exit(vmExitWithCleanup((int)code));
    return makeVoid(); // Unreachable
}

Value vmBuiltinDelay(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "Delay requires an integer argument.");
        return makeVoid();
    }
    long long ms = AS_INTEGER(args[0]);
    if (ms > 0) usleep((useconds_t)ms * 1000);
    return makeVoid();
}

static bool parseThreadIdValue(const Value* value, int* outId) {
    if (!value || !outId) {
        return false;
    }
    if (value->type == TYPE_THREAD || IS_INTLIKE(*value)) {
        long long raw = asI64(*value);
        if (raw <= 0 || raw >= VM_MAX_THREADS) {
            return false;
        }
        *outId = (int)raw;
        return true;
    }
    return false;
}

static bool parseBooleanValue(const Value* value, bool* outBool) {
    if (!value || !outBool) {
        return false;
    }
    if (value->type == TYPE_BOOLEAN) {
        *outBool = value->i_val != 0;
        return true;
    }
    if (IS_INTLIKE(*value)) {
        *outBool = asI64(*value) != 0;
        return true;
    }
    return false;
}

Value vmBuiltinWaitForThread(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeInt(-1);
    }
    if (arg_count != 1) {
        runtimeError(vm, "WaitForThread expects exactly 1 argument (thread id).");
        return makeInt(-1);
    }

    Value tidVal = args[0];
    if (!(tidVal.type == TYPE_THREAD || IS_INTLIKE(tidVal))) {
        runtimeError(vm, "WaitForThread argument must be a thread id.");
        return makeInt(-1);
    }

    int id = (int)asI64(tidVal);
    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    bool joined = thread_vm ? vmJoinThreadById(thread_vm, id) : false;
    if (!joined && thread_vm && thread_vm != vm) {
        joined = vmJoinThreadById(vm, id);
        if (joined) {
            thread_vm = vm;
        }
    }
    if (!joined) {
        runtimeError(vm, "WaitForThread received invalid thread id %d.", id);
        return makeInt(-1);
    }

    bool status_flag = true;
    if (vmThreadTakeResult(thread_vm, id, NULL, false, &status_flag, false)) {
        return makeInt(status_flag ? 0 : 1);
    }

    return makeInt(0);
}

Value vmBuiltinThreadSpawnBuiltin(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeInt(-1);
    }
    if (arg_count < 1) {
        runtimeError(vm, "ThreadSpawnBuiltin expects a builtin identifier followed by optional arguments.");
        return makeInt(-1);
    }

    Value target = args[0];
    int builtin_id = -1;
    const char* builtin_name = NULL;
    if (target.type == TYPE_STRING || target.type == TYPE_POINTER) {
        const char* source_name = builtinValueToCString(&target);
        if (!source_name || !*source_name) {
            runtimeError(vm, "ThreadSpawnBuiltin requires a builtin name or id.");
            return makeInt(-1);
        }
        builtin_id = getVmBuiltinID(source_name);
        builtin_name = getVmBuiltinNameById(builtin_id);
    } else if (IS_INTLIKE(target)) {
        builtin_id = (int)asI64(target);
        builtin_name = getVmBuiltinNameById(builtin_id);
    } else {
        runtimeError(vm, "ThreadSpawnBuiltin requires a builtin name (string) or id (integer).");
        return makeInt(-1);
    }

    if (builtin_id < 0 || !builtin_name) {
        runtimeError(vm, "ThreadSpawnBuiltin received an unknown builtin identifier.");
        return makeInt(-1);
    }
    if (!threadBuiltinIsAllowlisted(builtin_id)) {
        runtimeError(vm, "Builtin '%s' is not approved for threaded execution.", builtin_name);
        if (shellRuntimeSetLastStatusSticky) {
            shellRuntimeSetLastStatusSticky(1);
#if defined(FRONTEND_SHELL)
            if (vm) {
                vm->abort_requested = false;
                vm->exit_requested = false;
            }
#endif
        } else if (shellRuntimeSetLastStatus) {
            shellRuntimeSetLastStatus(1);
#if defined(FRONTEND_SHELL)
            if (vm) {
                vm->abort_requested = false;
                vm->exit_requested = false;
            }
#endif
        }
        return makeInt(-1);
    }

    const Value* builtin_args = (arg_count > 1) ? &args[1] : NULL;
    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }
    int thread_id = vmSpawnBuiltinThread(thread_vm, builtin_id, builtin_name, arg_count - 1, builtin_args);
    if (thread_id < 0) {
        runtimeError(vm, "ThreadSpawnBuiltin failed to start builtin '%s'.", builtin_name);
        return makeInt(-1);
    }

    Value thread_value = makeInt(thread_id);
    thread_value.type = TYPE_THREAD;
    return thread_value;
}

Value vmBuiltinThreadGetResult(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeNil();
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "ThreadGetResult expects a thread id and optional consumeStatus flag.");
        return makeNil();
    }

    int thread_id = -1;
    if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "ThreadGetResult argument must be a valid thread id.");
        return makeNil();
    }
    if (thread_id >= VM_MAX_THREADS) {
        runtimeError(vm, "ThreadGetResult received thread id %d out of range.", thread_id);
        return makeNil();
    }

    bool consume_status = false;
    if (arg_count == 2) {
        if (!parseBooleanValue(&args[1], &consume_status)) {
            runtimeError(vm, "ThreadGetResult consume flag must be boolean or integer.");
            return makeNil();
        }
    }

    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    Thread* slot = NULL;
    if (thread_vm && thread_id > 0 && thread_id < VM_MAX_THREADS) {
        slot = &thread_vm->threads[thread_id];
        if (slot->active) {
            runtimeError(vm, "Thread %d is still running; join it before retrieving the result.", thread_id);
            return makeNil();
        }
    }

    bool status = false;
    Value result = makeNil();
    if (thread_vm && vmThreadTakeResult(thread_vm, thread_id, &result, true, &status, consume_status)) {
        return result;
    }

    if (thread_vm && thread_vm != vm) {
        Thread* fallback_slot = NULL;
        if (thread_id > 0 && thread_id < VM_MAX_THREADS) {
            fallback_slot = &vm->threads[thread_id];
            if (fallback_slot->active) {
                runtimeError(vm,
                             "Thread %d is still running; join it before retrieving the result.",
                             thread_id);
                return makeNil();
            }
        }
        if (vmThreadTakeResult(vm, thread_id, &result, true, &status, consume_status)) {
            return result;
        }
    }

    runtimeError(vm, "Thread %d has no stored result.", thread_id);
    return makeNil();
}

Value vmBuiltinThreadGetStatus(VM* vm, int arg_count, Value* args) {
    if (!vm) {
        return makeBoolean(false);
    }
    if (arg_count < 1 || arg_count > 2) {
        runtimeError(vm, "ThreadGetStatus expects a thread id and optional dropResult flag.");
        return makeBoolean(false);
    }

    int thread_id = -1;
    if (!parseThreadIdValue(&args[0], &thread_id)) {
        runtimeError(vm, "ThreadGetStatus argument must be a valid thread id.");
        return makeBoolean(false);
    }
    if (thread_id >= VM_MAX_THREADS) {
        runtimeError(vm, "ThreadGetStatus received thread id %d out of range.", thread_id);
        return makeBoolean(false);
    }

    bool drop_result = false;
    if (arg_count == 2) {
        if (!parseBooleanValue(&args[1], &drop_result)) {
            runtimeError(vm, "ThreadGetStatus drop flag must be boolean or integer.");
            return makeBoolean(false);
        }
    }

    VM* thread_vm = vm;
    if (vm && vm->threadOwner) {
        thread_vm = vm->threadOwner;
    }

    Thread* slot = NULL;
    if (thread_vm && thread_id > 0 && thread_id < VM_MAX_THREADS) {
        slot = &thread_vm->threads[thread_id];
        if (slot->active) {
            runtimeError(vm, "Thread %d is still running; join it before querying status.", thread_id);
            return makeBoolean(false);
        }
    }

    bool status = false;
    Value dropped = makeNil();
    if (thread_vm &&
        vmThreadTakeResult(thread_vm, thread_id, drop_result ? &dropped : NULL, drop_result, &status, true)) {
        if (drop_result) {
            freeValue(&dropped);
        }
        return makeBoolean(status);
    }

    if (thread_vm && thread_vm != vm) {
        Thread* fallback_slot = NULL;
        if (thread_id > 0 && thread_id < VM_MAX_THREADS) {
            fallback_slot = &vm->threads[thread_id];
            if (fallback_slot->active) {
                runtimeError(vm, "Thread %d is still running; join it before querying status.", thread_id);
                if (drop_result) {
                    freeValue(&dropped);
                }
                return makeBoolean(false);
            }
        }
        if (vmThreadTakeResult(vm, thread_id, drop_result ? &dropped : NULL, drop_result, &status, true)) {
            if (drop_result) {
                freeValue(&dropped);
            }
            return makeBoolean(status);
        }
    }

    runtimeError(vm, "Thread %d has no stored status.", thread_id);
    if (drop_result) {
        freeValue(&dropped);
    }
    return makeBoolean(false);
}

int getBuiltinIDForCompiler(const char *name) {
    return getVmBuiltinID(name);
}

typedef struct {
    char *name;
    BuiltinRoutineType type;
} RegisteredBuiltin;

static RegisteredBuiltin* builtin_registry = NULL;
static int builtin_registry_count = 0;
static int builtin_registry_capacity = 0;
static pthread_once_t builtin_registration_once = PTHREAD_ONCE_INIT;

/*
 * Thread-safe builtin allowlist.
 *
 * Only builtins that are re-entrant and do not mutate global VM state may run
 * on worker threads. Audit new candidates carefully (no shared static buffers,
 * no implicit interaction with the interpreter state) before adding them to
 * this table.
 */
static pthread_once_t gThreadBuiltinAllowlistOnce = PTHREAD_ONCE_INIT;
static bool *gThreadBuiltinAllowlist = NULL;
static size_t gThreadBuiltinAllowlistCount = 0;
static const char *const kThreadBuiltinAllowlistNames[] = {
    "delay",
    "httprequest",
    "httprequesttofile",
    "httprequestasync",
    "httprequestasynctofile",
    "httptryawait",
    "httpawait",
    "httpisdone",
    "httpcancel",
    "httpgetasyncprogress",
    "httpgetasynctotal",
    "httpgetlastheaders",
    "httpgetheader",
    "httpclearheaders",
    "httpsetheader",
    "httpsetoption",
    "httperrorcode",
    "httplasterror",
    "apireceive",
    "apisend",
    "dnslookup"
};

static void initThreadBuiltinAllowlist(void) {
    gThreadBuiltinAllowlistCount = num_vm_builtins;
    if (gThreadBuiltinAllowlistCount == 0) {
        return;
    }

    gThreadBuiltinAllowlist = calloc(gThreadBuiltinAllowlistCount, sizeof(bool));
    if (!gThreadBuiltinAllowlist) {
        gThreadBuiltinAllowlistCount = 0;
        return;
    }

    const size_t count = sizeof(kThreadBuiltinAllowlistNames) / sizeof(kThreadBuiltinAllowlistNames[0]);
    for (size_t i = 0; i < count; ++i) {
        int id = getVmBuiltinID(kThreadBuiltinAllowlistNames[i]);
        if (id >= 0 && (size_t)id < gThreadBuiltinAllowlistCount) {
            gThreadBuiltinAllowlist[id] = true;
        }
    }
}

static bool threadBuiltinIsAllowlisted(int id) {
    if (id < 0) {
        return false;
    }

    pthread_once(&gThreadBuiltinAllowlistOnce, initThreadBuiltinAllowlist);
    if (!gThreadBuiltinAllowlist || gThreadBuiltinAllowlistCount == 0) {
        return false;
    }

    if ((size_t)id < gThreadBuiltinAllowlistCount) {
        return gThreadBuiltinAllowlist[id];
    }
    return false;
}

typedef struct {
    Symbol symbol;
    BuiltinRoutineType builtinType;
    int registryIndex;
} BuiltinTypeEntry;

static HashTable *builtinTypeHash = NULL;

static void destroyBuiltinTypeHashUnlocked(void) {
    if (!builtinTypeHash) {
        return;
    }
    freeHashTable(builtinTypeHash);
    builtinTypeHash = NULL;
}

static BuiltinRoutineType builtinRoutineTypeFromDecl(ASTNodeType declType) {
    return (declType == AST_FUNCTION_DECL)
               ? BUILTIN_TYPE_FUNCTION
               : BUILTIN_TYPE_PROCEDURE;
}

static void registerBuiltinFunctionLinear(const char *name, BuiltinRoutineType type) {
    if (!name) {
        return;
    }

    for (int i = 0; i < builtin_registry_count; ++i) {
        if (strcasecmp(name, builtin_registry[i].name) == 0) {
            builtin_registry[i].type = type;
            return;
        }
    }

    if (builtin_registry_count >= builtin_registry_capacity) {
        int new_capacity = builtin_registry_capacity < 64 ? 64 : builtin_registry_capacity * 2;
        RegisteredBuiltin* new_registry = realloc(builtin_registry, sizeof(RegisteredBuiltin) * new_capacity);
        if (!new_registry) {
            return;
        }
        builtin_registry = new_registry;
        builtin_registry_capacity = new_capacity;
    }

    char *dup_name = strdup(name);
    if (!dup_name) {
        return;
    }

    builtin_registry[builtin_registry_count].name = dup_name;
    builtin_registry[builtin_registry_count].type = type;
    builtin_registry_count++;
}

static BuiltinTypeEntry *lookupBuiltinTypeEntryUnlocked(const char *canonical_name) {
    if (!builtinTypeHash || !canonical_name || !*canonical_name) {
        return NULL;
    }

    Symbol *sym = hashTableLookup(builtinTypeHash, canonical_name);
    if (!sym) {
        return NULL;
    }
    return (BuiltinTypeEntry *)sym;
}

static bool ensureBuiltinTypeHashUnlocked(void) {
    if (builtinTypeHash) {
        return true;
    }

    builtinTypeHash = createHashTable();
    if (!builtinTypeHash) {
        return false;
    }

    for (int i = 0; i < builtin_registry_count; ++i) {
        char canonical[MAX_SYMBOL_LENGTH];
        if (!canonicalizeBuiltinName(builtin_registry[i].name, canonical, sizeof(canonical))) {
            continue;
        }

        BuiltinTypeEntry *entry = calloc(1, sizeof(*entry));
        if (!entry) {
            destroyBuiltinTypeHashUnlocked();
            return false;
        }

        entry->symbol.name = strdup(canonical);
        if (!entry->symbol.name) {
            free(entry);
            destroyBuiltinTypeHashUnlocked();
            return false;
        }
        entry->symbol.is_alias = true;
        entry->builtinType = builtin_registry[i].type;
        entry->registryIndex = i;
        hashTableInsert(builtinTypeHash, &entry->symbol);
    }

    return true;
}

static void registerBuiltinFunctionUnlocked(const char *name,
        ASTNodeType declType,
        const char* unit_context_name_param_for_addproc) {
    (void)unit_context_name_param_for_addproc;
    if (!name) {
        return;
    }

    BuiltinRoutineType builtinType = builtinRoutineTypeFromDecl(declType);

    char canonical[MAX_SYMBOL_LENGTH];
    if (!canonicalizeBuiltinName(name, canonical, sizeof(canonical))) {
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    if (!ensureBuiltinTypeHashUnlocked()) {
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    BuiltinTypeEntry *existing = lookupBuiltinTypeEntryUnlocked(canonical);
    if (existing) {
        existing->builtinType = builtinType;
        if (existing->registryIndex >= 0 && existing->registryIndex < builtin_registry_count) {
            builtin_registry[existing->registryIndex].type = builtinType;
        }
        return;
    }

    if (builtin_registry_count >= builtin_registry_capacity) {
        int new_capacity = builtin_registry_capacity < 64 ? 64 : builtin_registry_capacity * 2;
        RegisteredBuiltin* new_registry = realloc(builtin_registry, sizeof(RegisteredBuiltin) * new_capacity);
        if (!new_registry) {
            return;
        }
        builtin_registry = new_registry;
        builtin_registry_capacity = new_capacity;
    }

    BuiltinTypeEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        destroyBuiltinTypeHashUnlocked();
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    entry->symbol.name = strdup(canonical);
    if (!entry->symbol.name) {
        free(entry);
        destroyBuiltinTypeHashUnlocked();
        registerBuiltinFunctionLinear(name, builtinType);
        return;
    }

    char *dup_name = strdup(name);
    if (!dup_name) {
        free(entry->symbol.name);
        free(entry);
        return;
    }

    int index = builtin_registry_count;
    builtin_registry[index].name = dup_name;
    builtin_registry[index].type = builtinType;
    builtin_registry_count++;

    entry->symbol.is_alias = true;
    entry->builtinType = builtinType;
    entry->registryIndex = index;
    hashTableInsert(builtinTypeHash, &entry->symbol);
}

void registerBuiltinFunction(const char *name, ASTNodeType declType, const char* unit_context_name_param_for_addproc) {
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    registerBuiltinFunctionUnlocked(name, declType, unit_context_name_param_for_addproc);
    pthread_mutex_unlock(&builtin_registry_mutex);
}

int isBuiltin(const char *name) {
    if (!name) {
        return 0;
    }

    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);

    int result = 0;
    char canonical[MAX_SYMBOL_LENGTH];
    if (canonicalizeBuiltinName(name, canonical, sizeof(canonical)) && ensureBuiltinTypeHashUnlocked()) {
        result = lookupBuiltinTypeEntryUnlocked(canonical) != NULL;
    }

    if (!result) {
        for (int i = 0; i < builtin_registry_count; ++i) {
            if (strcasecmp(name, builtin_registry[i].name) == 0) {
                result = 1;
                break;
            }
        }
    }

    pthread_mutex_unlock(&builtin_registry_mutex);

    if (!result) {
        result = getVmBuiltinID(name) != -1;
    }

    return result;
}

BuiltinRoutineType getBuiltinType(const char *name) {
    if (!name) {
        return BUILTIN_TYPE_NONE;
    }

    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);

    BuiltinRoutineType result = BUILTIN_TYPE_NONE;
    char canonical[MAX_SYMBOL_LENGTH];
    if (canonicalizeBuiltinName(name, canonical, sizeof(canonical)) && ensureBuiltinTypeHashUnlocked()) {
        BuiltinTypeEntry *entry = lookupBuiltinTypeEntryUnlocked(canonical);
        if (entry) {
            result = entry->builtinType;
        }
    }

    if (result == BUILTIN_TYPE_NONE) {
        for (int i = 0; i < builtin_registry_count; ++i) {
            if (strcasecmp(name, builtin_registry[i].name) == 0) {
                result = builtin_registry[i].type;
                break;
            }
        }
    }

    pthread_mutex_unlock(&builtin_registry_mutex);

    return result;
}

static void populateBuiltinRegistry(void) {
    pthread_once(&builtin_registry_once, initBuiltinRegistryMutex);
    pthread_mutex_lock(&builtin_registry_mutex);
    /*
     * Core numeric conversion helpers.  These mirror the small "C-like"
     * casting helpers exposed by several front ends (Rea, CLike, etc.).
     * Historically each front end registered these manually which made the
     * setup fragile—forgetting to do so caused the compiler to fall back to
     * emitting indirect calls and the VM would later report missing globals
     * such as "float".  Register them here so every front end shares the
     * same canonical metadata and the compiler can always resolve their
     * routine types.
     */
    registerBuiltinFunctionUnlocked("int",    AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("double", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("float",  AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("char",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("bool",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("byte",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("toint",    AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("todouble", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tofloat",  AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tochar",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tobool",   AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("tobyte",   AST_FUNCTION_DECL, NULL);

    /* General built-in functions and procedures */
    // Rea/CLike: object allocation helper
    registerBuiltinFunctionUnlocked("newobj", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Abs", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("apiReceive", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("apiSend", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpSession", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpClose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpSetHeader", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpClearHeaders", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpSetOption", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequest", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequestToFile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequestAsync", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpRequestAsyncToFile", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpIsDone", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpTryAwait", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpCancel", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetAsyncProgress", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetAsyncTotal", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpAwait", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpLastError", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetLastHeaders", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpErrorCode", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HttpGetHeader", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("DnsLookup", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketAccept", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketBind", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketBindAddr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketClose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketConnect", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketCreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketLastError", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketListen", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketPoll", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketReceive", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketSend", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SocketSetBlocking", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Append", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcCos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcSin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ArcTan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Assign", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Beep", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Byte", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Ceil", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Chr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Close", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ClrEol", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Copy", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Cos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Cosh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Cotan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("CursorOff", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("CursorOn", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Dec", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Delay", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("DelLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Dispose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosExec", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosFindfirst", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosFindnext", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGetenv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGetfattr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosMkdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosRmdir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGetdate", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("dosGettime", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("EOF", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("exec", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Exit", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Exp", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("findFirst", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("findNext", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Floor", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getDate", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("getEnv", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getEnvInt", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getFAttr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("getTime", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Halt", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("HideCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("High", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("HighVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Inc", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("InsLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("IntToStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("InvertColors", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("IOResult", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("KeyPressed", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Length", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SetLength", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Ln", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Log10", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Low", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Max", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Min", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("mkDir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamCreate", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamFree", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamFromString", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamLoadFromFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamSaveToFile", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("MStreamBuffer", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("New", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("NormalColors", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Ord", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ParamCount", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ParamStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("PopScreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Pos", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Power", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("PushScreen", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("QuitRequested", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Random", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Randomize", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ReadKey", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Real", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("FormatFloat", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("RealToStr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Rename", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Erase", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Reset", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("RestoreCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Rewrite", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("rmDir", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Round", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("SaveCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ScreenCols", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ScreenRows", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ShowCursor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sinh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sqr", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Sqrt", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Str", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Succ", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Tan", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Tanh", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("GotoXY", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BoldText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIBoldText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BlinkText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIBlinkText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("UnderlineText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIUnderlineText", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("LowVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BILowVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("NormVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BINormVideo", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ClrScr", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIClrScr", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TermBackground", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextBackground", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextBackgroundE", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextColor", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("TextColorE", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Trunc", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("UpCase", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BytecodeVersion", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Val", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ValReal", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("VMVersion", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Window", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Write", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("WhereX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIWhereX", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("WhereY", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("BIWhereY", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("printf", AST_FUNCTION_DECL, NULL); // special-case handled by compiler
    registerBuiltinFunctionUnlocked("CreateThread", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("WaitForThread", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadSpawnBuiltin", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadGetResult", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ThreadGetStatus", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("mutex", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("rcmutex", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("lock", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("unlock", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("destroy", AST_PROCEDURE_DECL, NULL);

    // Additional registrations to ensure CLike builtins are classified correctly
    registerBuiltinFunctionUnlocked("Fopen", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Fclose", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("Fprintf", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("Read", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("ReadLn", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("DeLine", AST_PROCEDURE_DECL, NULL);
    registerBuiltinFunctionUnlocked("JsonGet", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("ToUpper", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunctionUnlocked("toupper", AST_FUNCTION_DECL, NULL);

    /* Allow externally linked modules to add more builtins. */
    registerExtendedBuiltins();
    /* CLike-style cast helper synonyms to avoid keyword collisions */
    registerVmBuiltin("toint",    vmBuiltinToInt,    BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("todouble", vmBuiltinToDouble, BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("tofloat",  vmBuiltinToFloat,  BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("tochar",   vmBuiltinToChar,   BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("tobool",   vmBuiltinToBool,   BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("tobyte",   vmBuiltinToByte,   BUILTIN_TYPE_FUNCTION, NULL);
    registerVmBuiltin("mstreamfromstring", vmBuiltinMstreamFromString, BUILTIN_TYPE_FUNCTION, NULL);
    pthread_mutex_unlock(&builtin_registry_mutex);
}
void registerAllBuiltins(void) {
    pthread_once(&builtin_registration_once, populateBuiltinRegistry);
}
