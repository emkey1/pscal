#include "shell/builtins.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/register.h"
#include "common/builtin_shared.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if defined(PSCAL_TARGET_IOS) || (defined(TARGET_OS_MACCATALYST) && TARGET_OS_MACCATALYST)
#define PSCAL_MOBILE_PLATFORM 1
#endif

#if defined(PSCAL_MOBILE_PLATFORM)
#include "smallclue/smallclue.h"
#endif
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    const char *name;
    const char *canonical;
    int id;
} ShellBuiltinEntry;

static const ShellBuiltinEntry kShellBuiltins[] = {
    {"cd", "cd", 1},
    {"pwd", "pwd", 2},
    {"echo", "echo", 3},
    {"exit", "exit", 4},
    {"exec", "exec", 31},
    {"true", "true", 5},
    {"false", "false", 6},
    {"set", "set", 7},
    {"unset", "unset", 8},
    {"export", "export", 9},
    {"read", "read", 10},
    {"test", "test", 11},
    {"[", "test", 11},
    {"[[", "__shell_double_bracket", 1013},
    {"shift", "shift", 12},
    {"alias", "alias", 13},
    {"unalias", "unalias", 38},
    {"caller", "caller", 52},
    {"history", "history", 14},
    {"setenv", "setenv", 15},
    {"unsetenv", "unsetenv", 16},
    {"declare", "declare", 32},
    {"typeset", "declare", 32},
    {"readonly", "readonly", 40},
    {"command", "command", 41},
    {"enable", "enable", 53},
    {"printf", "printf", 46},
    {"getopts", "getopts", 48},
    {"mapfile", "mapfile", 49},
    {"readarray", "mapfile", 49},
    {"cat", "cat", -1},
    {"clear", "clear", -1},
    {"cls", "clear", -1},
    {"jobs", "jobs", 17},
    {"fg", "fg", 18},
    {"bg", "bg", 19},
    {"wait", "wait", 20},
    {"WaitForThread", "waitforthread", 1056},
#ifdef PSCAL_MOBILE_PLATFORM
    {"cal", "cal", -1},
    {"chmod", "chmod", -1},
    {"clike", "clike", -1},
    {"cp", "cp", -1},
    {"curl", "curl", -1},
    {"cut", "cut", -1},
    {"date", "date", -1},
    {"du", "du", -1},
    {"env", "env", -1},
    {"version", "version", -1},
    {"vproc-test", "vproc-test", -1},
    {"nextvi", "nextvi", -1},
    {"vi", "nextvi", -1},
    {"pwd", "pwd", -1},
    {"basename", "basename", -1},
    {"dirname", "dirname", -1},
    {"df", "df", -1},
    {"sleep", "sleep", -1},
    {"tee", "tee", -1},
    {"xargs", "xargs", -1},
    {"yes", "yes", -1},
    {"no", "no", -1},
    {"traceroute", "traceroute", -1},
    {"ps", "lps", 1057},
    {"ps", "lps", 1057},
    {"lps", "lps", 1057},
    {"ps-threads", "ps-threads", 55},
    {"kill", "kill", -1},
    {"file", "file", -1},
    {"find", "find", -1},
    {"grep", "grep", -1},
    {"gwin", "gwin", -1},
    {"head", "head", -1},
    {"id", "id", -1},
    {"ipaddr", "ipaddr", -1},
    {"host", "host", -1},
    {"ls", "ls", -1},
    {"md", "md", -1},
    {"ln", "ln", -1},
    {"mkdir", "mkdir", -1},
    {"nslookup", "nslookup", -1},
    {"rmdir", "rmdir", -1},
    {"mv", "mv", -1},
    {"pbcopy", "pbcopy", -1},
    {"pbpaste", "pbpaste", -1},
    {"pascal", "pascal", -1},
    {"pscaljson2bc", "pscaljson2bc", -1},
#ifdef BUILD_PSCALD
    {"pscald", "pscald", -1},
#endif
#ifdef BUILD_DASCAL
    {"dascal", "dascal", -1},
#endif
    {"pscalvm", "pscalvm", -1},
    {"rea", "rea", -1},
    {"exsh", "exsh", -1},
    {"sh", "exsh", -1},
    {"resize", "resize", -1},
    {"rm", "rm", -1},
    {"ping", "ping", -1},
    {"scp", "scp", -1},
    {"sftp", "sftp", -1},
    {"script", "script", -1},
    {"sed", "sed", -1},
    {"sort", "sort", -1},
    {"stty", "stty", -1},
    {"tset", "tset", -1},
    {"tty", "tty", -1},
    {"tail", "tail", -1},
    {"telnet", "telnet", -1},
    {"touch", "touch", -1},
    {"tr", "tr", -1},
    {"uptime", "uptime", -1},
    {"uname", "uname", -1},
    {"watch", "watch", -1},
    {"top", "top", -1},
#ifdef SMALLCLUE_WITH_EXSH
    {"sh", "sh", -1},
#endif
    {"ssh", "ssh", -1},
    {"ssh-keygen", "ssh-keygen", -1},
    {"uniq", "uniq", -1},
    {"wc", "wc", -1},
    {"wget", "wget", -1},
    {"addt", "addt", -1},
    {"addtab", "addt", -1},
    {"smallclue-help", "smallclue-help", -1},
    {"dmesg", "dmesg", -1},
    {"licenses", "licenses", -1},
#endif
#if defined(PSCAL_TAB_TITLE_SUPPORT)
    {"tabname", "tabname", -1},
    {"tname", "tabname", -1},
#endif
    {"ThreadSpawnBuiltin", "threadspawnbuiltin", -1},
    {"ThreadGetResult", "threadgetresult", -1},
    {"ThreadGetStatus", "threadgetstatus", -1},
    {"builtin", "builtin", 21},
    {"source", "source", 21},
    {".", "source", 21},
    {"trap", "trap", 22},
    {"local", "local", 23},
    {"break", "break", 24},
    {"continue", "continue", 25},
    {":", ":", 26},
    {"eval", "eval", 27},
    {"return", "return", 28},
    {"finger", "finger", 29},
    {"help", "help", 30},
    {"stdioinfo", "stdioinfo", -1},
    {"bind", "bind", 33},
    {"shopt", "shopt", 34},
    {"type", "type", 42},
    {"which", "which", 54},
    {"dirs", "dirs", 35},
    {"pushd", "pushd", 36},
    {"popd", "popd", 37},
    {"let", "let", 39},
    {"umask", "umask", 43},
    {"times", "times", 47},
    {"logout", "logout", 44},
    {"disown", "disown", 45},
    {"kill", "kill", 51},
    {"hash", "hash", 50},
    {"enable", "enable", 53},
    {"__shell_exec", "__shell_exec", 1001},
    {"__shell_pipeline", "__shell_pipeline", 1002},
    {"__shell_arithmetic", "__shell_arithmetic", 1016},
    {"__shell_and", "__shell_and", 1003},
    {"__shell_or", "__shell_or", 1004},
    {"__shell_subshell", "__shell_subshell", 1005},
    {"__shell_loop", "__shell_loop", 1006},
    {"__shell_if", "__shell_if", 1007},
    {"__shell_case", "__shell_case", 1008},
    {"__shell_case_clause", "__shell_case_clause", 1009},
    {"__shell_case_end", "__shell_case_end", 1010},
    {"__shell_define_function", "__shell_define_function", 1011},
    {"__shell_loop_end", "__shell_loop_end", 1012},
    {"__shell_double_bracket", "__shell_double_bracket", 1013},
    {"__shell_enter_condition", "__shell_enter_condition", 1014},
    {"__shell_leave_condition", "__shell_leave_condition", 1015},
    {"__shell_leave_condition_preserve", "__shell_leave_condition_preserve", 1017}
};

static char *shellLowercase(const char *name) {
    if (!name) {
        return NULL;
    }
    size_t len = strlen(name);
    char *lower = (char *)malloc(len + 1);
    if (!lower) {
        return NULL;
    }
    for (size_t i = 0; i < len; ++i) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    lower[len] = '\0';
    return lower;
}

void shellRegisterBuiltins(HashTable *table) {
    sharedRegisterExtendedBuiltins();
    if (!table) {
        return;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        const ShellBuiltinEntry *entry = &kShellBuiltins[i];
        Symbol *symbol = (Symbol *)calloc(1, sizeof(Symbol));
        if (!symbol) {
            continue;
        }
        char *lower = shellLowercase(entry->canonical);
        symbol->name = lower ? lower : strdup(entry->canonical);
        symbol->type = TYPE_VOID;
        symbol->is_alias = false;
        symbol->is_const = true;
        symbol->is_defined = true;
        int builtin_id = getBuiltinIDForCompiler(entry->canonical);
        if (builtin_id < 0) {
            builtin_id = entry->id;
        }
        symbol->bytecode_address = builtin_id;
        symbol->value = NULL;
        symbol->type_def = NULL;
        hashTableInsert(table, symbol);
    }
}

int shellGetBuiltinId(const char *name) {
    sharedRegisterExtendedBuiltins();
    if (!name) {
        return -1;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        if (strcasecmp(kShellBuiltins[i].name, name) == 0 ||
            strcasecmp(kShellBuiltins[i].canonical, name) == 0) {
            int builtin_id = getBuiltinIDForCompiler(kShellBuiltins[i].canonical);
            if (builtin_id >= 0) {
                return builtin_id;
            }
            return kShellBuiltins[i].id;
        }
    }
    return -1;
}

const char *shellBuiltinCanonicalName(const char *name) {
    if (!name) {
        return "";
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        if (strcasecmp(kShellBuiltins[i].name, name) == 0 ||
            strcasecmp(kShellBuiltins[i].canonical, name) == 0) {
            return kShellBuiltins[i].canonical;
        }
    }
    return name;
}

bool shellIsBuiltinName(const char *name) {
    return shellGetBuiltinId(name) >= 0;
}

void shellVisitBuiltins(ShellBuiltinVisitor visitor, void *context) {
    sharedRegisterExtendedBuiltins();
    if (!visitor) {
        return;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    for (size_t i = 0; i < builtin_count; ++i) {
        const ShellBuiltinEntry *entry = &kShellBuiltins[i];
        visitor(entry->name, entry->canonical, entry->id, context);
    }
#if defined(PSCAL_MOBILE_PLATFORM)
    size_t applet_count = 0;
    const SmallclueApplet *applets = smallclueGetApplets(&applet_count);
    if (applets && applet_count > 0) {
        for (size_t i = 0; i < applet_count; ++i) {
            const char *name = applets[i].name;
            if (!name || !*name) {
                continue;
            }
            bool already_listed = false;
            for (size_t j = 0; j < builtin_count; ++j) {
                const ShellBuiltinEntry *entry = &kShellBuiltins[j];
                if (entry->canonical && strcasecmp(entry->canonical, name) == 0) {
                    already_listed = true;
                    break;
                }
                if (entry->name && strcasecmp(entry->name, name) == 0) {
                    already_listed = true;
                    break;
                }
            }
            if (already_listed) {
                continue;
            }
            visitor(name, name, -1, context);
        }
    }
#endif
}

void shellDumpBuiltins(FILE *out) {
    if (!out) {
        out = stdout;
    }
    size_t builtin_count = sizeof(kShellBuiltins) / sizeof(kShellBuiltins[0]);
    fprintf(out, "Shell builtins (%zu):\n", builtin_count);
    for (size_t i = 0; i < builtin_count; ++i) {
        fprintf(out, "  %s\n", kShellBuiltins[i].name);
    }
}
#include "shell/builtins.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/register.h"
