#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/frontend_kind.h"

extern int pascal_main(int argc, char **argv);
extern int clike_main(int argc, char **argv);
extern int rea_main(int argc, char **argv);
extern int pscalvm_main(int argc, char **argv);
extern int pscaljson2bc_main(int argc, char **argv);
#ifdef PSCAL_TARGET_IOS
int pscal_openssh_ssh_main(int argc, char **argv);
int pscal_openssh_scp_main(int argc, char **argv);
int pscal_openssh_sftp_main(int argc, char **argv);
#endif
#ifdef BUILD_DASCAL
extern int dascal_main(int argc, char **argv);
#endif
#ifdef BUILD_PSCALD
extern int pscald_main(int argc, char **argv);
extern int pscalasm_main(int argc, char **argv);
#endif

typedef int (*ToolEntryFn)(int argc, char **argv);

typedef struct {
    const char *name;
    ToolEntryFn entry;
    FrontendKind kind;
} ToolDescriptor;

static const ToolDescriptor kToolDescriptors[] = {
    {"pascal", pascal_main, FRONTEND_KIND_PASCAL},
    {"clike", clike_main, FRONTEND_KIND_CLIKE},
    {"rea", rea_main, FRONTEND_KIND_REA},
    {"pscalvm", pscalvm_main, FRONTEND_KIND_PASCAL},
    {"pscaljson2bc", pscaljson2bc_main, FRONTEND_KIND_PASCAL},
#ifdef BUILD_DASCAL
    {"dascal", dascal_main, FRONTEND_KIND_PASCAL},
#endif
#ifdef BUILD_PSCALD
    {"pscald", pscald_main, FRONTEND_KIND_PASCAL},
    {"pscalasm", pscalasm_main, FRONTEND_KIND_PASCAL},
#endif
#ifdef PSCAL_TARGET_IOS
    {"ssh", pscal_openssh_ssh_main, FRONTEND_KIND_PASCAL},
    {"scp", pscal_openssh_scp_main, FRONTEND_KIND_PASCAL},
    {"sftp", pscal_openssh_sftp_main, FRONTEND_KIND_PASCAL},
#endif
};

static void printUsage(const char *program) {
    fprintf(stderr, "Usage: %s <tool> [args...]\n", program ? program : "pscal_tool_runner");
    fprintf(stderr, "Available tools:\n");
    for (size_t i = 0; i < sizeof(kToolDescriptors) / sizeof(kToolDescriptors[0]); ++i) {
        fprintf(stderr, "  - %s\n", kToolDescriptors[i].name);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *tool_name = argv[1];
    const ToolDescriptor *descriptor = NULL;
    for (size_t i = 0; i < sizeof(kToolDescriptors) / sizeof(kToolDescriptors[0]); ++i) {
        if (strcmp(kToolDescriptors[i].name, tool_name) == 0) {
            descriptor = &kToolDescriptors[i];
            break;
        }
    }

    if (!descriptor) {
        fprintf(stderr, "pscal_tool_runner: unknown tool '%s'\n", tool_name);
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }

    int child_argc = argc - 1;
    char **child_argv = argv + 1;
    return descriptor->entry(child_argc, child_argv);
}
