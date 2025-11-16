#include "smallclu/smallclu.h"

#include "backend_ast/builtin.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
extern void shellRuntimeSetLastStatus(int status) __attribute__((weak_import));
#elif defined(__GNUC__)
extern void shellRuntimeSetLastStatus(int status) __attribute__((weak));
#else
void shellRuntimeSetLastStatus(int status);
#endif

static char *smallcluDuplicateArg(const Value *value) {
    if (!value) {
        return strdup("");
    }
    if (value->type == TYPE_STRING && value->s_val) {
        return strdup(value->s_val);
    }
    if (IS_INTLIKE(*value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)AS_INTEGER(*value));
        return strdup(buf);
    }
    if (isRealType(value->type)) {
        char buf[64];
        long double real = AS_REAL(*value);
        snprintf(buf, sizeof(buf), "%.17Lg", real);
        return strdup(buf);
    }
    return strdup("");
}

static Value smallcluInvokeBuiltin(VM *vm, int arg_count, Value *args, const char *name) {
    const SmallcluApplet *applet = smallcluFindApplet(name);
    if (!applet) {
        if (shellRuntimeSetLastStatus) {
            shellRuntimeSetLastStatus(127);
        }
        return makeVoid();
    }

    int argc = arg_count + 1;
    char **argv = (char **)calloc((size_t)(argc + 1), sizeof(char *));
    if (!argv) {
        if (shellRuntimeSetLastStatus) {
            shellRuntimeSetLastStatus(1);
        }
        return makeVoid();
    }

    bool ok = true;
    argv[0] = strdup(applet->name);
    if (!argv[0]) {
        ok = false;
    }
    for (int i = 0; ok && i < arg_count; ++i) {
        argv[i + 1] = smallcluDuplicateArg(&args[i]);
        if (!argv[i + 1]) {
            ok = false;
        }
    }

    int status = ok ? smallcluDispatchApplet(applet, argc, argv) : 1;
    if (shellRuntimeSetLastStatus) {
        shellRuntimeSetLastStatus(status);
    }

    for (int i = 0; i < argc; ++i) {
        free(argv[i]);
    }
    free(argv);

    (void)vm;
    return makeVoid();
}

#define DEFINE_SMALLCLU_WRAPPER(name_literal, ident)                                        \
    static Value vmBuiltinSmallclu_##ident(VM *vm, int arg_count, Value *args) {            \
        return smallcluInvokeBuiltin(vm, arg_count, args, name_literal);                    \
    }

DEFINE_SMALLCLU_WRAPPER("cat", cat)
DEFINE_SMALLCLU_WRAPPER("clear", clear)
DEFINE_SMALLCLU_WRAPPER("cls", cls)
DEFINE_SMALLCLU_WRAPPER("editor", editor)
DEFINE_SMALLCLU_WRAPPER("date", date)
DEFINE_SMALLCLU_WRAPPER("cal", cal)
DEFINE_SMALLCLU_WRAPPER("head", head)
DEFINE_SMALLCLU_WRAPPER("tail", tail)
DEFINE_SMALLCLU_WRAPPER("touch", touch)
DEFINE_SMALLCLU_WRAPPER("grep", grep)
DEFINE_SMALLCLU_WRAPPER("wc", wc)
DEFINE_SMALLCLU_WRAPPER("du", du)
DEFINE_SMALLCLU_WRAPPER("find", find)
DEFINE_SMALLCLU_WRAPPER("stty", stty)
DEFINE_SMALLCLU_WRAPPER("resize", resize)
DEFINE_SMALLCLU_WRAPPER("sort", sort)
DEFINE_SMALLCLU_WRAPPER("uniq", uniq)
DEFINE_SMALLCLU_WRAPPER("sed", sed)
DEFINE_SMALLCLU_WRAPPER("cut", cut)
DEFINE_SMALLCLU_WRAPPER("tr", tr)
DEFINE_SMALLCLU_WRAPPER("id", id)
#if defined(PSCAL_TARGET_IOS)
DEFINE_SMALLCLU_WRAPPER("mkdir", mkdir)
DEFINE_SMALLCLU_WRAPPER("cp", cp)
DEFINE_SMALLCLU_WRAPPER("mv", mv)
DEFINE_SMALLCLU_WRAPPER("rm", rm)
#endif
#if defined(PSCAL_TARGET_IOS)
DEFINE_SMALLCLU_WRAPPER("elvis", elvis)
#endif
DEFINE_SMALLCLU_WRAPPER("less", less)
DEFINE_SMALLCLU_WRAPPER("ls", ls)
DEFINE_SMALLCLU_WRAPPER("more", more)

#undef DEFINE_SMALLCLU_WRAPPER

static pthread_once_t g_smallclu_builtin_once = PTHREAD_ONCE_INIT;

static void smallcluRegisterBuiltinsOnce(void) {
    registerVmBuiltin("cat", vmBuiltinSmallclu_cat, BUILTIN_TYPE_PROCEDURE, "cat");
    registerVmBuiltin("ls", vmBuiltinSmallclu_ls, BUILTIN_TYPE_PROCEDURE, "ls");
    registerVmBuiltin("less", vmBuiltinSmallclu_less, BUILTIN_TYPE_PROCEDURE, "less");
    registerVmBuiltin("more", vmBuiltinSmallclu_more, BUILTIN_TYPE_PROCEDURE, "more");
    registerVmBuiltin("clear", vmBuiltinSmallclu_clear, BUILTIN_TYPE_PROCEDURE, "clear");
    registerVmBuiltin("cls", vmBuiltinSmallclu_cls, BUILTIN_TYPE_PROCEDURE, "cls");
    registerVmBuiltin("editor", vmBuiltinSmallclu_editor, BUILTIN_TYPE_PROCEDURE, "editor");
    registerVmBuiltin("date", vmBuiltinSmallclu_date, BUILTIN_TYPE_PROCEDURE, "date");
    registerVmBuiltin("cal", vmBuiltinSmallclu_cal, BUILTIN_TYPE_PROCEDURE, "cal");
    registerVmBuiltin("head", vmBuiltinSmallclu_head, BUILTIN_TYPE_PROCEDURE, "head");
    registerVmBuiltin("tail", vmBuiltinSmallclu_tail, BUILTIN_TYPE_PROCEDURE, "tail");
    registerVmBuiltin("touch", vmBuiltinSmallclu_touch, BUILTIN_TYPE_PROCEDURE, "touch");
    registerVmBuiltin("grep", vmBuiltinSmallclu_grep, BUILTIN_TYPE_PROCEDURE, "grep");
    registerVmBuiltin("wc", vmBuiltinSmallclu_wc, BUILTIN_TYPE_PROCEDURE, "wc");
    registerVmBuiltin("du", vmBuiltinSmallclu_du, BUILTIN_TYPE_PROCEDURE, "du");
    registerVmBuiltin("find", vmBuiltinSmallclu_find, BUILTIN_TYPE_PROCEDURE, "find");
    registerVmBuiltin("stty", vmBuiltinSmallclu_stty, BUILTIN_TYPE_PROCEDURE, "stty");
    registerVmBuiltin("resize", vmBuiltinSmallclu_resize, BUILTIN_TYPE_PROCEDURE, "resize");
    registerVmBuiltin("sort", vmBuiltinSmallclu_sort, BUILTIN_TYPE_PROCEDURE, "sort");
    registerVmBuiltin("uniq", vmBuiltinSmallclu_uniq, BUILTIN_TYPE_PROCEDURE, "uniq");
    registerVmBuiltin("sed", vmBuiltinSmallclu_sed, BUILTIN_TYPE_PROCEDURE, "sed");
    registerVmBuiltin("cut", vmBuiltinSmallclu_cut, BUILTIN_TYPE_PROCEDURE, "cut");
    registerVmBuiltin("tr", vmBuiltinSmallclu_tr, BUILTIN_TYPE_PROCEDURE, "tr");
    registerVmBuiltin("id", vmBuiltinSmallclu_id, BUILTIN_TYPE_PROCEDURE, "id");
#if defined(PSCAL_TARGET_IOS)
    registerVmBuiltin("mkdir", vmBuiltinSmallclu_mkdir, BUILTIN_TYPE_PROCEDURE, "mkdir");
    registerVmBuiltin("cp", vmBuiltinSmallclu_cp, BUILTIN_TYPE_PROCEDURE, "cp");
    registerVmBuiltin("mv", vmBuiltinSmallclu_mv, BUILTIN_TYPE_PROCEDURE, "mv");
    registerVmBuiltin("rm", vmBuiltinSmallclu_rm, BUILTIN_TYPE_PROCEDURE, "rm");
    registerVmBuiltin("elvis", vmBuiltinSmallclu_elvis, BUILTIN_TYPE_PROCEDURE, "elvis");
#endif
}

void smallcluRegisterBuiltins(void) {
    pthread_once(&g_smallclu_builtin_once, smallcluRegisterBuiltinsOnce);
}
