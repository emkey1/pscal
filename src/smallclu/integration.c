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
#if defined(PSCAL_TARGET_IOS)
    registerVmBuiltin("elvis", vmBuiltinSmallclu_elvis, BUILTIN_TYPE_PROCEDURE, "elvis");
#endif
}

void smallcluRegisterBuiltins(void) {
    pthread_once(&g_smallclu_builtin_once, smallcluRegisterBuiltinsOnce);
}
