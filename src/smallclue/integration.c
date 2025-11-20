#include "smallclue/smallclue.h"

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

static char *smallclueDuplicateArg(const Value *value) {
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

static Value smallclueInvokeBuiltin(VM *vm, int arg_count, Value *args, const char *name) {
    const SmallclueApplet *applet = smallclueFindApplet(name);
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
        argv[i + 1] = smallclueDuplicateArg(&args[i]);
        if (!argv[i + 1]) {
            ok = false;
        }
    }

    int status = ok ? smallclueDispatchApplet(applet, argc, argv) : 1;
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

#define DEFINE_SMALLCLUE_WRAPPER(name_literal, ident)                                        \
    static Value vmBuiltinSmallclue_##ident(VM *vm, int arg_count, Value *args) {            \
        return smallclueInvokeBuiltin(vm, arg_count, args, name_literal);                    \
    }

DEFINE_SMALLCLUE_WRAPPER("cat", cat)
DEFINE_SMALLCLUE_WRAPPER("clear", clear)
DEFINE_SMALLCLUE_WRAPPER("cls", cls)
DEFINE_SMALLCLUE_WRAPPER("date", date)
DEFINE_SMALLCLUE_WRAPPER("cal", cal)
DEFINE_SMALLCLUE_WRAPPER("head", head)
DEFINE_SMALLCLUE_WRAPPER("tail", tail)
DEFINE_SMALLCLUE_WRAPPER("touch", touch)
DEFINE_SMALLCLUE_WRAPPER("grep", grep)
DEFINE_SMALLCLUE_WRAPPER("wc", wc)
DEFINE_SMALLCLUE_WRAPPER("du", du)
DEFINE_SMALLCLUE_WRAPPER("find", find)
DEFINE_SMALLCLUE_WRAPPER("stty", stty)
DEFINE_SMALLCLUE_WRAPPER("resize", resize)
DEFINE_SMALLCLUE_WRAPPER("sort", sort)
DEFINE_SMALLCLUE_WRAPPER("uniq", uniq)
DEFINE_SMALLCLUE_WRAPPER("sed", sed)
DEFINE_SMALLCLUE_WRAPPER("cut", cut)
DEFINE_SMALLCLUE_WRAPPER("curl", curl)
DEFINE_SMALLCLUE_WRAPPER("tr", tr)
DEFINE_SMALLCLUE_WRAPPER("id", id)
#if SMALLCLUE_HAS_IFADDRS
DEFINE_SMALLCLUE_WRAPPER("ipaddr", ipaddr)
#endif
DEFINE_SMALLCLUE_WRAPPER("df", df)
DEFINE_SMALLCLUE_WRAPPER("pwd", pwd)
DEFINE_SMALLCLUE_WRAPPER("chmod", chmod)
DEFINE_SMALLCLUE_WRAPPER("true", truecmd)
DEFINE_SMALLCLUE_WRAPPER("false", falsecmd)
DEFINE_SMALLCLUE_WRAPPER("sleep", sleepcmd)
DEFINE_SMALLCLUE_WRAPPER("basename", basename)
DEFINE_SMALLCLUE_WRAPPER("dirname", dirname)
DEFINE_SMALLCLUE_WRAPPER("tee", tee)
DEFINE_SMALLCLUE_WRAPPER("test", testcmd)
DEFINE_SMALLCLUE_WRAPPER("[", bracket)
DEFINE_SMALLCLUE_WRAPPER("xargs", xargs)
DEFINE_SMALLCLUE_WRAPPER("ps", ps)
DEFINE_SMALLCLUE_WRAPPER("kill", kill)
DEFINE_SMALLCLUE_WRAPPER("file", file)
DEFINE_SMALLCLUE_WRAPPER("scp", scp)
DEFINE_SMALLCLUE_WRAPPER("sftp", sftp)
DEFINE_SMALLCLUE_WRAPPER("ssh", ssh)
DEFINE_SMALLCLUE_WRAPPER("ssh-keygen", sshkeygen)
#if defined(PSCAL_TARGET_IOS)
DEFINE_SMALLCLUE_WRAPPER("mkdir", mkdir)
DEFINE_SMALLCLUE_WRAPPER("cp", cp)
DEFINE_SMALLCLUE_WRAPPER("mv", mv)
DEFINE_SMALLCLUE_WRAPPER("rm", rm)
DEFINE_SMALLCLUE_WRAPPER("rmdir", rmdir)
DEFINE_SMALLCLUE_WRAPPER("ln", ln)
DEFINE_SMALLCLUE_WRAPPER("ping", ping)
DEFINE_SMALLCLUE_WRAPPER("env", env)
#endif
#if defined(PSCAL_TARGET_IOS)
DEFINE_SMALLCLUE_WRAPPER("elvis", elvis)
DEFINE_SMALLCLUE_WRAPPER("vi", vi)
#endif
DEFINE_SMALLCLUE_WRAPPER("less", less)
DEFINE_SMALLCLUE_WRAPPER("ls", ls)
DEFINE_SMALLCLUE_WRAPPER("md", md)
DEFINE_SMALLCLUE_WRAPPER("wget", wget)
DEFINE_SMALLCLUE_WRAPPER("more", more)

#undef DEFINE_SMALLCLUE_WRAPPER

static pthread_once_t g_smallclue_builtin_once = PTHREAD_ONCE_INIT;

static void smallclueRegisterBuiltinsOnce(void) {
    registerVmBuiltin("cat", vmBuiltinSmallclue_cat, BUILTIN_TYPE_PROCEDURE, "cat");
    registerVmBuiltin("ls", vmBuiltinSmallclue_ls, BUILTIN_TYPE_PROCEDURE, "ls");
    registerVmBuiltin("md", vmBuiltinSmallclue_md, BUILTIN_TYPE_PROCEDURE, "md");
    registerVmBuiltin("less", vmBuiltinSmallclue_less, BUILTIN_TYPE_PROCEDURE, "less");
    registerVmBuiltin("more", vmBuiltinSmallclue_more, BUILTIN_TYPE_PROCEDURE, "more");
    registerVmBuiltin("clear", vmBuiltinSmallclue_clear, BUILTIN_TYPE_PROCEDURE, "clear");
    registerVmBuiltin("cls", vmBuiltinSmallclue_cls, BUILTIN_TYPE_PROCEDURE, "cls");
    registerVmBuiltin("date", vmBuiltinSmallclue_date, BUILTIN_TYPE_PROCEDURE, "date");
    registerVmBuiltin("cal", vmBuiltinSmallclue_cal, BUILTIN_TYPE_PROCEDURE, "cal");
    registerVmBuiltin("head", vmBuiltinSmallclue_head, BUILTIN_TYPE_PROCEDURE, "head");
    registerVmBuiltin("tail", vmBuiltinSmallclue_tail, BUILTIN_TYPE_PROCEDURE, "tail");
    registerVmBuiltin("touch", vmBuiltinSmallclue_touch, BUILTIN_TYPE_PROCEDURE, "touch");
    registerVmBuiltin("grep", vmBuiltinSmallclue_grep, BUILTIN_TYPE_PROCEDURE, "grep");
    registerVmBuiltin("wc", vmBuiltinSmallclue_wc, BUILTIN_TYPE_PROCEDURE, "wc");
    registerVmBuiltin("du", vmBuiltinSmallclue_du, BUILTIN_TYPE_PROCEDURE, "du");
    registerVmBuiltin("find", vmBuiltinSmallclue_find, BUILTIN_TYPE_PROCEDURE, "find");
    registerVmBuiltin("stty", vmBuiltinSmallclue_stty, BUILTIN_TYPE_PROCEDURE, "stty");
    registerVmBuiltin("resize", vmBuiltinSmallclue_resize, BUILTIN_TYPE_PROCEDURE, "resize");
    registerVmBuiltin("sort", vmBuiltinSmallclue_sort, BUILTIN_TYPE_PROCEDURE, "sort");
    registerVmBuiltin("uniq", vmBuiltinSmallclue_uniq, BUILTIN_TYPE_PROCEDURE, "uniq");
    registerVmBuiltin("sed", vmBuiltinSmallclue_sed, BUILTIN_TYPE_PROCEDURE, "sed");
    registerVmBuiltin("cut", vmBuiltinSmallclue_cut, BUILTIN_TYPE_PROCEDURE, "cut");
    registerVmBuiltin("curl", vmBuiltinSmallclue_curl, BUILTIN_TYPE_PROCEDURE, "curl");
    registerVmBuiltin("tr", vmBuiltinSmallclue_tr, BUILTIN_TYPE_PROCEDURE, "tr");
    registerVmBuiltin("id", vmBuiltinSmallclue_id, BUILTIN_TYPE_PROCEDURE, "id");
#if SMALLCLUE_HAS_IFADDRS
    registerVmBuiltin("ipaddr", vmBuiltinSmallclue_ipaddr, BUILTIN_TYPE_PROCEDURE, "ipaddr");
#endif
    registerVmBuiltin("df", vmBuiltinSmallclue_df, BUILTIN_TYPE_PROCEDURE, "df");
    registerVmBuiltin("file", vmBuiltinSmallclue_file, BUILTIN_TYPE_PROCEDURE, "file");
    registerVmBuiltin("pwd", vmBuiltinSmallclue_pwd, BUILTIN_TYPE_PROCEDURE, "pwd");
    registerVmBuiltin("chmod", vmBuiltinSmallclue_chmod, BUILTIN_TYPE_PROCEDURE, "chmod");
    registerVmBuiltin("true", vmBuiltinSmallclue_truecmd, BUILTIN_TYPE_PROCEDURE, "true");
    registerVmBuiltin("false", vmBuiltinSmallclue_falsecmd, BUILTIN_TYPE_PROCEDURE, "false");
    registerVmBuiltin("sleep", vmBuiltinSmallclue_sleepcmd, BUILTIN_TYPE_PROCEDURE, "sleep");
    registerVmBuiltin("basename", vmBuiltinSmallclue_basename, BUILTIN_TYPE_PROCEDURE, "basename");
    registerVmBuiltin("dirname", vmBuiltinSmallclue_dirname, BUILTIN_TYPE_PROCEDURE, "dirname");
    registerVmBuiltin("tee", vmBuiltinSmallclue_tee, BUILTIN_TYPE_PROCEDURE, "tee");
    registerVmBuiltin("test", vmBuiltinSmallclue_testcmd, BUILTIN_TYPE_PROCEDURE, "test");
    registerVmBuiltin("[", vmBuiltinSmallclue_bracket, BUILTIN_TYPE_PROCEDURE, "[");
    registerVmBuiltin("xargs", vmBuiltinSmallclue_xargs, BUILTIN_TYPE_PROCEDURE, "xargs");
    registerVmBuiltin("ps", vmBuiltinSmallclue_ps, BUILTIN_TYPE_PROCEDURE, "ps");
    registerVmBuiltin("kill", vmBuiltinSmallclue_kill, BUILTIN_TYPE_PROCEDURE, "kill");
    registerVmBuiltin("scp", vmBuiltinSmallclue_scp, BUILTIN_TYPE_PROCEDURE, "scp");
    registerVmBuiltin("sftp", vmBuiltinSmallclue_sftp, BUILTIN_TYPE_PROCEDURE, "sftp");
    registerVmBuiltin("ssh", vmBuiltinSmallclue_ssh, BUILTIN_TYPE_PROCEDURE, "ssh");
    registerVmBuiltin("ssh-keygen", vmBuiltinSmallclue_sshkeygen, BUILTIN_TYPE_PROCEDURE, "ssh-keygen");
#if defined(PSCAL_TARGET_IOS)
    registerVmBuiltin("mkdir", vmBuiltinSmallclue_mkdir, BUILTIN_TYPE_PROCEDURE, "mkdir");
    registerVmBuiltin("cp", vmBuiltinSmallclue_cp, BUILTIN_TYPE_PROCEDURE, "cp");
    registerVmBuiltin("mv", vmBuiltinSmallclue_mv, BUILTIN_TYPE_PROCEDURE, "mv");
    registerVmBuiltin("rm", vmBuiltinSmallclue_rm, BUILTIN_TYPE_PROCEDURE, "rm");
    registerVmBuiltin("rmdir", vmBuiltinSmallclue_rmdir, BUILTIN_TYPE_PROCEDURE, "rmdir");
    registerVmBuiltin("ln", vmBuiltinSmallclue_ln, BUILTIN_TYPE_PROCEDURE, "ln");
    registerVmBuiltin("ping", vmBuiltinSmallclue_ping, BUILTIN_TYPE_PROCEDURE, "ping");
    registerVmBuiltin("env", vmBuiltinSmallclue_env, BUILTIN_TYPE_PROCEDURE, "env");
    registerVmBuiltin("elvis", vmBuiltinSmallclue_elvis, BUILTIN_TYPE_PROCEDURE, "elvis");
    registerVmBuiltin("vi", vmBuiltinSmallclue_vi, BUILTIN_TYPE_PROCEDURE, "vi");
#endif
    registerVmBuiltin("wget", vmBuiltinSmallclue_wget, BUILTIN_TYPE_PROCEDURE, "wget");
}

void smallclueRegisterBuiltins(void) {
    pthread_once(&g_smallclue_builtin_once, smallclueRegisterBuiltinsOnce);
}
