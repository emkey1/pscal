#include "backend_ast/builtin.h"
#include "shell/builtins.h"
#include "core/utils.h"
#include "vm/vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void shellUpdateStatus(int status);

typedef struct {
    const char *name;
    const char *summary;
    const char *usage;
    const char *detail;
    const char *const *aliases;
    size_t alias_count;
} ShellHelpTopic;

static const char *const kShellHelpSourceAliases[] = {"."};

static const ShellHelpTopic kShellHelpTopics[] = {
    {
        "alias",
        "Define or display shell aliases.",
        "alias [name=value ...]",
        "Without arguments prints the stored alias definitions as alias name='value'. Each NAME=VALUE argument updates or creates an alias.",
        NULL,
        0
    },
    {
        "unalias",
        "Remove shell aliases.",
        "unalias [-a] [name ...]",
        "Deletes the aliases identified by NAME. With -a all aliases are removed. Providing NAME alongside -a results in an error.",
        NULL,
        0
    },
    {
        "bind",
        "Configure readline behaviour.",
        "bind [-p] [spec ...]",
        "Accepts readline \"set\" directives and remembers their most recent values. The -p flag prints the stored settings in \"set name value\" form. Other invocations are currently accepted as no-ops.",
        NULL,
        0
    },
    {
        "bg",
        "Resume a stopped job in the background.",
        "bg [job]",
        "Targets the most recently launched job when no job is supplied. Job specifiers may be numeric indexes or begin with '%'.",
        NULL,
        0
    },
    {
        "break",
        "Exit from the innermost loop(s).",
        "break [n]",
        "Accepts an optional positive integer count; the default of 1 exits only the innermost active loop.",
        NULL,
        0
    },
    {
        "builtin",
        "Invoke a PSCAL VM builtin directly.",
        "builtin name [args ...]",
        "Arguments are forwarded to the named VM builtin. Prefix an argument with int:, float:/double:/real:, bool:/boolean:, str:/string:/raw:, or nil: to coerce the value; other arguments are passed as strings. When the VM builtin returns a non-void value it is printed to stdout on success.",
        NULL,
        0
    },
    {
        "command",
        "Query command resolution metadata.",
        "command [-a] [-p] [-v|-V] [name ...]",
        "With -v prints the first match for each NAME, favouring aliases, functions, builtins, and executable paths. -V prints verbose descriptions. The -a flag lists every match and -p searches using the default PATH. Execution without -v or -V is not currently supported.",
        NULL,
        0
    },
    {
        "cd",
        "Change the current working directory.",
        "cd [dir]",
        "With no arguments cd switches to $HOME. Successful runs update the PWD environment variable.",
        NULL,
        0
    },
    {
        "dirs",
        "Display the directory stack.",
        "dirs",
        "Prints the current directory stack with the most recent entry first. Options such as -c are not yet supported.",
        NULL,
        0
    },
    {
        "pushd",
        "Push a directory onto the stack and change to it.",
        "pushd [dir]",
        "With DIR changes to the target directory and pushes the previous working directory onto the stack. Without arguments swaps the top two entries.",
        NULL,
        0
    },
    {
        "popd",
        "Pop the directory stack.",
        "popd",
        "Removes the top stack entry and switches to the new top directory. Fails when the stack contains only a single entry.",
        NULL,
        0
    },
    {
        "printf",
        "Format and print data to standard output.",
        "printf format [arguments]",
        "Follows the POSIX printf builtin. Supports most common printf(3) conversion specifiers and stores output into a variable when invoked with -v name.",
        NULL,
        0
    },
    {
        "read",
        "Read a line from standard input.",
        "read [-r] [-a array] [-p prompt] [variable ...]",
        "Reads a line from stdin splitting fields using $IFS. With no variables assigns to REPLY. -a stores fields into an array and -r disables backslash escaping.",
        NULL,
        0
    },
    {
        "return",
        "Return from the current function or sourced file.",
        "return [n]",
        "Sets the shell status to N (default 0) and unwinds the current function or sourced script.",
        NULL,
        0
    },
    {
        "set",
        "Display or alter shell variables.",
        "set [name=value ...]",
        "With NAME=VALUE pairs updates shell variables. Without arguments prints the environment sorted in lexical order.",
        NULL,
        0
    },
    {
        "setenv",
        "Set an environment variable.",
        "setenv name value",
        "Updates or adds NAME with VALUE in the environment. The variable is tracked by the shell so array assignments remain consistent.",
        NULL,
        0
    },
    {
        "shift",
        "Rotate positional parameters.",
        "shift [n]",
        "Discards N (default 1) positional parameters from the left shifting the remainder forward.",
        NULL,
        0
    },
    {
        "source",
        "Execute a script in the current shell context.",
        "source file [args ...]",
        "Loads FILE, running it within the current shell process. Additional arguments populate positional parameters for the duration of the call.",
        kShellHelpSourceAliases,
        sizeof(kShellHelpSourceAliases) / sizeof(kShellHelpSourceAliases[0])
    },
    {
        "type",
        "Describe how a command name resolves.",
        "type name [name ...]",
        "Reports whether NAME refers to an alias, function, builtin, or executable. Accepts multiple names.",
        NULL,
        0
    },
    {
        "umask",
        "Display or set the file creation mask.",
        "umask [-S] [mode]",
        "Without MODE prints the current mask. With MODE updates it. -S prints a symbolic representation.",
        NULL,
        0
    }
};

static const ShellHelpTopic *shellHelpFindTopic(const char *name) {
    if (!name) {
        return NULL;
    }
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    const char *canonical = shellBuiltinCanonicalName(name);
    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        if (strcasecmp(topic->name, name) == 0) {
            return topic;
        }
        if (canonical && strcasecmp(topic->name, canonical) == 0) {
            return topic;
        }
        for (size_t j = 0; j < topic->alias_count; ++j) {
            if (strcasecmp(topic->aliases[j], name) == 0) {
                return topic;
            }
        }
    }
    return NULL;
}

void shellHelpPrintOverview(void) {
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    size_t width = 0;
    char display[128];

    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        const char *name = topic->name;
        if (topic->alias_count > 0 && topic->aliases) {
            snprintf(display, sizeof(display), "%s (%s)", name, topic->aliases[0]);
            name = display;
        }
        size_t len = strlen(name);
        if (len > width) {
            width = len;
        }
    }

    printf("exsh builtins. Type 'help <function>' for detailed usage.\n\n");
    printf("%-*s  %s\n", (int)width, "Builtin", "Summary");
    printf("%-*s  %s\n", (int)width, "------", "-------");

    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        const char *name = topic->name;
        if (topic->alias_count > 0 && topic->aliases) {
            snprintf(display, sizeof(display), "%s (%s)", name, topic->aliases[0]);
            name = display;
        }
        printf("%-*s  %s\n", (int)width, name, topic->summary);
    }
}

static void shellHelpPrintCatalog(void) {
    size_t topic_count = sizeof(kShellHelpTopics) / sizeof(kShellHelpTopics[0]);
    for (size_t i = 0; i < topic_count; ++i) {
        const ShellHelpTopic *topic = &kShellHelpTopics[i];
        printf("%-12s %s\n", topic->name, topic->summary);
    }
}

static void shellHelpPrintTopic(const ShellHelpTopic *topic) {
    if (!topic) {
        return;
    }
    printf("%s - %s\n", topic->name, topic->summary);
    if (topic->alias_count > 0 && topic->aliases) {
        printf("Aliases: ");
        for (size_t i = 0; i < topic->alias_count; ++i) {
            printf("%s%s", topic->aliases[i], (i + 1 < topic->alias_count) ? " " : "\n");
        }
    }
    if (topic->usage && *topic->usage) {
        printf("Usage: %s\n", topic->usage);
    }
    if (topic->detail && *topic->detail) {
        printf("\n%s\n", topic->detail);
    }
}

Value vmBuiltinShellHelp(VM *vm, int arg_count, Value *args) {
    if (arg_count == 0) {
        shellHelpPrintOverview();
        shellUpdateStatus(0);
        return makeVoid();
    }

    if (arg_count > 1) {
        runtimeError(vm, "help: expected at most one builtin name");
        shellUpdateStatus(1);
        return makeVoid();
    }

    if (args[0].type != TYPE_STRING || !args[0].s_val || args[0].s_val[0] == '\0') {
        runtimeError(vm, "help: expected builtin name as string");
        shellUpdateStatus(1);
        return makeVoid();
    }

    const char *requested = args[0].s_val;
    if (strcmp(requested, "-l") == 0) {
        shellHelpPrintCatalog();
        shellUpdateStatus(0);
        return makeVoid();
    }

    const ShellHelpTopic *topic = shellHelpFindTopic(requested);
    if (!topic) {
        const char *canonical = shellBuiltinCanonicalName(requested);
        topic = shellHelpFindTopic(canonical);
    }

    if (!topic) {
        runtimeError(vm, "help: unknown builtin '%s'", requested);
        shellUpdateStatus(1);
        return makeVoid();
    }

    shellHelpPrintTopic(topic);
    shellUpdateStatus(0);
    return makeVoid();
}
