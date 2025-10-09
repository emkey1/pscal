#ifndef SHELL_RUNNER_H
#define SHELL_RUNNER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ShellRunOptions {
    int dump_ast_json;
    int dump_bytecode;
    int dump_bytecode_only;
    int no_cache;
    int vm_trace_head;
    bool quiet;
    bool verbose_errors;
    bool exit_on_signal;
    bool suppress_warnings;
    const char *frontend_path;
} ShellRunOptions;

char *shellLoadFile(const char *path);
int shellRunSource(const char *source,
                   const char *path,
                   const ShellRunOptions *options,
                   bool *out_exit_requested);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_RUNNER_H */
