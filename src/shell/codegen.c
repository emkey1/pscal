#include "shell/codegen.h"
#include "shell/builtins.h"
#include "core/utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int addStringConstant(BytecodeChunk *chunk, const char *str) {
    Value val = makeString(str ? str : "");
    int index = addConstantToChunk(chunk, &val);
    freeValue(&val);
    return index;
}

static void emitConstantOperand(BytecodeChunk *chunk, int constant_index, int line) {
    if (constant_index < 0) {
        fprintf(stderr, "shell codegen error: negative constant index.\n");
        return;
    }
    if (constant_index <= 0xFF) {
        writeBytecodeChunk(chunk, CONSTANT, line);
        writeBytecodeChunk(chunk, (uint8_t)constant_index, line);
        return;
    }
    if (constant_index <= 0xFFFF) {
        writeBytecodeChunk(chunk, CONSTANT16, line);
        emitShort(chunk, (uint16_t)constant_index, line);
        return;
    }
    fprintf(stderr, "shell codegen error: constant table overflow (%d).\n", constant_index);
}

static void emitPushString(BytecodeChunk *chunk, const char *value, int line) {
    int idx = addStringConstant(chunk, value);
    emitConstantOperand(chunk, idx, line);
}

static void emitBuiltinProc(BytecodeChunk *chunk, const char *name, uint8_t arg_count, int line) {
    const char *canonical = shellBuiltinCanonicalName(name);
    int name_index = addStringConstant(chunk, canonical);
    int builtin_id = shellGetBuiltinId(name);
    if (builtin_id < 0) {
        fprintf(stderr, "shell codegen warning: unknown builtin '%s'\n", name);
        writeBytecodeChunk(chunk, CALL_BUILTIN, line);
        emitShort(chunk, (uint16_t)name_index, line);
        writeBytecodeChunk(chunk, arg_count, line);
        return;
    }
    writeBytecodeChunk(chunk, CALL_BUILTIN_PROC, line);
    emitShort(chunk, (uint16_t)builtin_id, line);
    emitShort(chunk, (uint16_t)name_index, line);
    writeBytecodeChunk(chunk, arg_count, line);
}

static const char *redirTypeName(ShellRedirectionType type) {
    switch (type) {
        case SHELL_REDIRECT_INPUT: return "<";
        case SHELL_REDIRECT_OUTPUT: return ">";
        case SHELL_REDIRECT_APPEND: return ">>";
        case SHELL_REDIRECT_HEREDOC: return "<<";
        case SHELL_REDIRECT_DUP_INPUT: return "<&";
        case SHELL_REDIRECT_DUP_OUTPUT: return ">&";
        case SHELL_REDIRECT_CLOBBER: return ">|";
    }
    return "";
}

static void compileCommand(BytecodeChunk *chunk, const ShellCommand *command);
static void compileProgram(BytecodeChunk *chunk, const ShellProgram *program);
static void compilePipeline(BytecodeChunk *chunk, const ShellPipeline *pipeline);

static void compileSimple(BytecodeChunk *chunk, const ShellCommand *command) {
    int line = command ? command->line : 0;
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=%d;pipe=%d;head=%d;tail=%d",
             command && command->exec.runs_in_background ? 1 : 0,
             command ? command->exec.pipeline_index : -1,
             command && command->exec.is_pipeline_head ? 1 : 0,
             command && command->exec.is_pipeline_tail ? 1 : 0);
    emitPushString(chunk, meta, line);

    size_t arg_count = 1; // metadata entry
    if (command) {
        const ShellWordArray *words = &command->data.simple.words;
        for (size_t i = 0; i < words->count; ++i) {
            const ShellWord *word = words->items[i];
            emitPushString(chunk, word && word->text ? word->text : "", line);
            arg_count++;
        }
        const ShellRedirectionArray *redirs = &command->data.simple.redirections;
        for (size_t i = 0; i < redirs->count; ++i) {
            const ShellRedirection *redir = redirs->items[i];
            char buffer[256];
            snprintf(buffer, sizeof(buffer), "redir:%s:%s:%s",
                     redir && redir->io_number ? redir->io_number : "",
                     redirTypeName(redir ? redir->type : SHELL_REDIRECT_OUTPUT),
                     (redir && redir->target && redir->target->text) ? redir->target->text : "");
            emitPushString(chunk, buffer, line);
            arg_count++;
        }
    }

    if (arg_count > 255) {
        fprintf(stderr, "shell codegen warning: argument vector truncated (%zu).\n", arg_count);
        arg_count = 255;
    }
    emitBuiltinProc(chunk, "__shell_exec", (uint8_t)arg_count, line);
}

static void compilePipeline(BytecodeChunk *chunk, const ShellPipeline *pipeline) {
    if (!pipeline) {
        return;
    }
    char meta[128];
    snprintf(meta, sizeof(meta), "stages=%zu;negated=%d",
             pipeline->command_count,
             pipeline->negated ? 1 : 0);
    emitPushString(chunk, meta,
                   pipeline->command_count > 0 && pipeline->commands[0] ? pipeline->commands[0]->line : 0);
    emitBuiltinProc(chunk, "__shell_pipeline", 1,
                    pipeline->command_count > 0 && pipeline->commands[0] ? pipeline->commands[0]->line : 0);
    for (size_t i = 0; i < pipeline->command_count; ++i) {
        compileCommand(chunk, pipeline->commands[i]);
    }
}

static void compileLogical(BytecodeChunk *chunk, const ShellLogicalList *logical, int line) {
    if (!logical) {
        return;
    }
    for (size_t i = 0; i < logical->count; ++i) {
        compilePipeline(chunk, logical->pipelines[i]);
        if (i + 1 < logical->count) {
            ShellLogicalConnector connector = logical->connectors[i + 1];
            const char *name = connector == SHELL_LOGICAL_AND ? "__shell_and" : "__shell_or";
            char meta[32];
            snprintf(meta, sizeof(meta), "connector=%s", connector == SHELL_LOGICAL_AND ? "&&" : "||");
            emitPushString(chunk, meta, line);
            emitBuiltinProc(chunk, name, 1, line);
        }
    }
}

static void compileSubshell(BytecodeChunk *chunk, const ShellProgram *body, int line, int index) {
    char meta[64];
    snprintf(meta, sizeof(meta), "subshell=%d", index);
    emitPushString(chunk, meta, line);
    emitBuiltinProc(chunk, "__shell_subshell", 1, line);
    compileProgram(chunk, body);
}

static void compileLoop(BytecodeChunk *chunk, const ShellLoop *loop, int line) {
    if (!loop) {
        return;
    }
    char meta[64];
    snprintf(meta, sizeof(meta), "until=%d", loop->is_until ? 1 : 0);
    emitPushString(chunk, meta, line);
    emitBuiltinProc(chunk, "__shell_loop", 1, line);
    compilePipeline(chunk, loop->condition);
    compileProgram(chunk, loop->body);
}

static void compileConditional(BytecodeChunk *chunk, const ShellConditional *conditional, int line) {
    if (!conditional) {
        return;
    }
    emitPushString(chunk, "branch=if", line);
    emitBuiltinProc(chunk, "__shell_if", 1, line);
    compilePipeline(chunk, conditional->condition);
    compileProgram(chunk, conditional->then_branch);
    compileProgram(chunk, conditional->else_branch);
}

static void compileCommand(BytecodeChunk *chunk, const ShellCommand *command) {
    if (!command) {
        return;
    }
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            compileSimple(chunk, command);
            break;
        case SHELL_COMMAND_PIPELINE:
            compilePipeline(chunk, command->data.pipeline);
            break;
        case SHELL_COMMAND_LOGICAL:
            compileLogical(chunk, command->data.logical, command->line);
            break;
        case SHELL_COMMAND_SUBSHELL:
            compileSubshell(chunk, command->data.subshell.body, command->line, command->exec.pipeline_index);
            break;
        case SHELL_COMMAND_LOOP:
            compileLoop(chunk, command->data.loop, command->line);
            break;
        case SHELL_COMMAND_CONDITIONAL:
            compileConditional(chunk, command->data.conditional, command->line);
            break;
    }
}

static void compileProgram(BytecodeChunk *chunk, const ShellProgram *program) {
    if (!program) {
        return;
    }
    for (size_t i = 0; i < program->commands.count; ++i) {
        compileCommand(chunk, program->commands.items[i]);
    }
}

void shellCompile(const ShellProgram *program, BytecodeChunk *chunk) {
    if (!chunk) {
        return;
    }
    initBytecodeChunk(chunk);
    compileProgram(chunk, program);
    writeBytecodeChunk(chunk, RETURN, 0);
}
