#include "shell/semantics.h"
#include "shell/builtins.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char *shellDuplicateName(const char *name) {
    if (!name) {
        return NULL;
    }
    size_t len = strlen(name);
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, name, len + 1);
    return copy;
}

static void shellDefineVariable(ShellSemanticContext *ctx, const char *name) {
    if (!ctx || !ctx->variable_table || !name || !*name) {
        return;
    }
    if (hashTableLookup(ctx->variable_table, name)) {
        return;
    }
    Symbol *symbol = (Symbol *)calloc(1, sizeof(Symbol));
    if (!symbol) {
        return;
    }
    symbol->name = shellDuplicateName(name);
    symbol->type = TYPE_STRING;
    symbol->is_const = false;
    symbol->is_alias = false;
    symbol->is_defined = true;
    hashTableInsert(ctx->variable_table, symbol);
}

static bool shellVariableDefined(ShellSemanticContext *ctx, const char *name) {
    if (!ctx || !ctx->variable_table || !name) {
        return false;
    }
    return hashTableLookup(ctx->variable_table, name) != NULL;
}

static bool shellIsSpecialParameterName(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (strcmp(name, "?") == 0 || strcmp(name, "#") == 0 ||
        strcmp(name, "*") == 0 || strcmp(name, "@") == 0 ||
        strcmp(name, "0") == 0) {
        return true;
    }
    for (const char *p = name; *p; ++p) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    return true;
}

static void shellAnnotatePipeline(ShellPipeline *pipeline) {
    if (!pipeline) {
        return;
    }
    for (size_t i = 0; i < pipeline->command_count; ++i) {
        ShellCommand *cmd = pipeline->commands[i];
        if (!cmd) {
            continue;
        }
        cmd->exec.pipeline_index = (int)i;
        cmd->exec.is_pipeline_head = (i == 0);
        cmd->exec.is_pipeline_tail = (i + 1 == pipeline->command_count);
    }
}

static void shellAnalyzeProgramInternal(ShellSemanticContext *ctx, ShellProgram *program);
static void shellAnalyzeCommand(ShellSemanticContext *ctx, ShellCommand *command);

void shellInitSemanticContext(ShellSemanticContext *ctx) {
    if (!ctx) {
        return;
    }
    ctx->builtin_table = createHashTable();
    ctx->variable_table = createHashTable();
    ctx->error_count = 0;
    ctx->warning_count = 0;
    shellRegisterBuiltins(ctx->builtin_table);
}

void shellFreeSemanticContext(ShellSemanticContext *ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->builtin_table) {
        freeHashTable(ctx->builtin_table);
        ctx->builtin_table = NULL;
    }
    if (ctx->variable_table) {
        freeHashTable(ctx->variable_table);
        ctx->variable_table = NULL;
    }
}

static void shellReportUndefinedBuiltin(ShellSemanticContext *ctx, const ShellWord *word) {
    if (!ctx || !word || !word->text) {
        return;
    }
    ctx->warning_count++;
    fprintf(stderr, "shell semantic warning (%d:%d): unknown command '%s'\n",
            word->line, word->column, word->text);
}

static bool shellCommandExistsOnPath(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (strchr(name, '/')) {
        return access(name, X_OK) == 0;
    }

    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) {
        path_env = "/bin:/usr/bin";
    }

    size_t name_len = strlen(name);
    const char *cursor = path_env;
    while (*cursor) {
        const char *sep = strchr(cursor, ':');
        size_t dir_len = sep ? (size_t)(sep - cursor) : strlen(cursor);
        size_t prefix_len = dir_len;
        bool use_current_dir = (dir_len == 0);
        if (use_current_dir) {
            prefix_len = 1; // "" -> "."
        }

        size_t total_len = prefix_len + 1 + name_len + 1;
        char *candidate = (char *)malloc(total_len);
        if (!candidate) {
            if (!sep) {
                break;
            }
            cursor = sep + 1;
            continue;
        }

        size_t pos = 0;
        if (use_current_dir) {
            candidate[pos++] = '.';
        } else if (dir_len > 0) {
            memcpy(candidate + pos, cursor, dir_len);
            pos += dir_len;
        }
        if (pos == 0 || candidate[pos - 1] != '/') {
            candidate[pos++] = '/';
        }
        memcpy(candidate + pos, name, name_len);
        pos += name_len;
        candidate[pos] = '\0';

        bool found = access(candidate, X_OK) == 0;
        free(candidate);
        if (found) {
            return true;
        }

        if (!sep) {
            break;
        }
        cursor = sep + 1;
    }

    return false;
}

static void shellAnalyzeSimpleCommand(ShellSemanticContext *ctx, ShellCommand *command) {
    if (!ctx || !command) {
        return;
    }
    ShellWordArray *words = &command->data.simple.words;
    if (words->count == 0) {
        return;
    }
    ShellWord *first = words->items[0];
    if (first && first->text && !shellIsBuiltinName(first->text)) {
        if (!hashTableLookup(ctx->builtin_table, first->text)) {
            Symbol *sym = lookupGlobalSymbol(first->text);
            if (!sym && constGlobalSymbols) {
                sym = hashTableLookup(constGlobalSymbols, first->text);
            }
            if (!sym && procedure_table) {
                sym = hashTableLookup(procedure_table, first->text);
            }
            if (!sym && !shellCommandExistsOnPath(first->text)) {
                shellReportUndefinedBuiltin(ctx, first);
            }
        }
    }

    for (size_t i = 0; i < words->count; ++i) {
        ShellWord *word = words->items[i];
        if (!word || !word->text) {
            continue;
        }
        if (!word->single_quoted && !word->double_quoted) {
            const char *eq = strchr(word->text, '=');
            if (eq && eq != word->text) {
                size_t len = (size_t)(eq - word->text);
                char *name = (char *)malloc(len + 1);
                if (name) {
                    memcpy(name, word->text, len);
                    name[len] = '\0';
                    shellDefineVariable(ctx, name);
                    free(name);
                }
            }
        }
        for (size_t j = 0; j < word->expansions.count; ++j) {
            const char *name = word->expansions.items[j];
            if (name) {
                if (shellVariableDefined(ctx, name) || shellIsSpecialParameterName(name)) {
                    continue;
                }
                // Environment variables and dynamically provided shell parameters
                // are resolved at runtime, so we do not treat them as errors here.
            }
        }
    }
}

static void shellAnalyzePipeline(ShellSemanticContext *ctx, ShellPipeline *pipeline) {
    if (!pipeline) {
        return;
    }
    shellAnnotatePipeline(pipeline);
    for (size_t i = 0; i < pipeline->command_count; ++i) {
        shellAnalyzeCommand(ctx, pipeline->commands[i]);
    }
}

static void shellAnalyzeLogical(ShellSemanticContext *ctx, ShellLogicalList *logical) {
    if (!logical) {
        return;
    }
    for (size_t i = 0; i < logical->count; ++i) {
        shellAnalyzePipeline(ctx, logical->pipelines[i]);
    }
}

static void shellAnalyzeConditional(ShellSemanticContext *ctx, ShellConditional *conditional) {
    if (!conditional) {
        return;
    }
    shellAnalyzePipeline(ctx, conditional->condition);
    shellAnalyzeProgramInternal(ctx, conditional->then_branch);
    shellAnalyzeProgramInternal(ctx, conditional->else_branch);
}

static void shellAnalyzeLoop(ShellSemanticContext *ctx, ShellLoop *loop) {
    if (!loop) {
        return;
    }
    shellAnalyzePipeline(ctx, loop->condition);
    shellAnalyzeProgramInternal(ctx, loop->body);
}

static void shellAnalyzeCommand(ShellSemanticContext *ctx, ShellCommand *command) {
    if (!ctx || !command) {
        return;
    }
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            shellAnalyzeSimpleCommand(ctx, command);
            break;
        case SHELL_COMMAND_PIPELINE:
            shellAnalyzePipeline(ctx, command->data.pipeline);
            break;
        case SHELL_COMMAND_LOGICAL:
            shellAnalyzeLogical(ctx, command->data.logical);
            break;
        case SHELL_COMMAND_SUBSHELL:
            shellAnalyzeProgramInternal(ctx, command->data.subshell.body);
            break;
        case SHELL_COMMAND_LOOP:
            shellAnalyzeLoop(ctx, command->data.loop);
            break;
        case SHELL_COMMAND_CONDITIONAL:
            shellAnalyzeConditional(ctx, command->data.conditional);
            break;
    }
}

static void shellAnalyzeProgramInternal(ShellSemanticContext *ctx, ShellProgram *program) {
    if (!program) {
        return;
    }
    for (size_t i = 0; i < program->commands.count; ++i) {
        shellAnalyzeCommand(ctx, program->commands.items[i]);
    }
}

ShellSemanticResult shellAnalyzeProgram(ShellSemanticContext *ctx, ShellProgram *program) {
    ShellSemanticResult result = {0, 0};
    if (!ctx || !program) {
        return result;
    }
    shellAnalyzeProgramInternal(ctx, program);
    result.error_count = ctx->error_count;
    result.warning_count = ctx->warning_count;
    return result;
}
