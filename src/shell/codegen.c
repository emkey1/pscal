#include "shell/codegen.h"
#include "shell/builtins.h"
#include "shell/word_encoding.h"
#include "shell/function.h"
#include "core/utils.h"
#include "vm/vm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void encodeHexDigits(size_t value, size_t width, char *out) {
    static const char digits[] = "0123456789ABCDEF";
    if (!out) {
        return;
    }
    for (size_t i = 0; i < width; ++i) {
        size_t shift = (width - 1 - i) * 4;
        size_t nibble;
        if (shift >= sizeof(size_t) * 8) {
            nibble = 0;
        } else {
            nibble = (value >> shift) & 0xF;
        }
        out[i] = digits[nibble & 0xF];
    }
}

static size_t buildCommandSubstitutionMetadata(const ShellWord *word, char **out_meta) {
    if (out_meta) {
        *out_meta = NULL;
    }
    if (!word) {
        return 0;
    }
    size_t count = word->command_substitutions.count;
    size_t meta_len = 4; // always store count
    for (size_t i = 0; i < count; ++i) {
        const ShellCommandSubstitution *sub = &word->command_substitutions.items[i];
        size_t cmd_len = sub->command ? strlen(sub->command) : 0;
        meta_len += 1 + 6 + 6 + cmd_len;
    }
    if (meta_len == 0) {
        return 0;
    }
    char *meta = (char *)malloc(meta_len);
    if (!meta) {
        return 0;
    }
    size_t offset = 0;
    encodeHexDigits(count, 4, meta + offset);
    offset += 4;
    for (size_t i = 0; i < count; ++i) {
        const ShellCommandSubstitution *sub = &word->command_substitutions.items[i];
        meta[offset++] = (sub->style == SHELL_COMMAND_SUBSTITUTION_BACKTICK) ? 'B' : 'D';
        encodeHexDigits(sub->span_length, 6, meta + offset);
        offset += 6;
        size_t cmd_len = sub->command ? strlen(sub->command) : 0;
        encodeHexDigits(cmd_len, 6, meta + offset);
        offset += 6;
        if (cmd_len > 0 && sub->command) {
            memcpy(meta + offset, sub->command, cmd_len);
        }
        offset += cmd_len;
    }
    if (out_meta) {
        *out_meta = meta;
    } else {
        free(meta);
    }
    return meta_len;
}

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

static char *encodeWord(const ShellWord *word) {
    if (!word) {
        return strdup("");
    }
    uint8_t flags = 0;
    if (word->single_quoted) {
        flags |= SHELL_WORD_FLAG_SINGLE_QUOTED;
    }
    if (word->double_quoted) {
        flags |= SHELL_WORD_FLAG_DOUBLE_QUOTED;
    }
    if (word->has_parameter_expansion) {
        flags |= SHELL_WORD_FLAG_HAS_PARAM;
    }
    if (word->has_command_substitution || word->command_substitutions.count > 0) {
        flags |= SHELL_WORD_FLAG_HAS_COMMAND;
    }
    if (word->has_arithmetic_expansion) {
        flags |= SHELL_WORD_FLAG_HAS_ARITHMETIC;
    }
    if (word->is_assignment) {
        flags |= SHELL_WORD_FLAG_ASSIGNMENT;
    }
    const char *text = word->text ? word->text : "";
    size_t len = strlen(text);
    char *meta = NULL;
    size_t meta_len = buildCommandSubstitutionMetadata(word, &meta);
    char len_hex[6];
    encodeHexDigits(meta_len, 6, len_hex);
    size_t total_len = 2 + 6 + meta_len + len + 1;
    char *encoded = (char *)malloc(total_len);
    if (!encoded) {
        free(meta);
        return NULL;
    }
    encoded[0] = SHELL_WORD_ENCODE_PREFIX;
    encoded[1] = (char)(flags + 1);
    memcpy(encoded + 2, len_hex, 6);
    if (meta_len > 0 && meta) {
        memcpy(encoded + 8, meta, meta_len);
    }
    memcpy(encoded + 8 + meta_len, text, len + 1);
    free(meta);
    return encoded;
}

static char *encodeHexString(const char *input) {
    if (!input) {
        return strdup("");
    }
    size_t len = strlen(input);
    char *encoded = (char *)malloc(len * 2 + 1);
    if (!encoded) {
        return NULL;
    }
    static const char hex_digits[] = "0123456789ABCDEF";
    for (size_t i = 0; i < len; ++i) {
        unsigned char byte = (unsigned char)input[i];
        encoded[2 * i] = hex_digits[(byte >> 4) & 0xF];
        encoded[2 * i + 1] = hex_digits[byte & 0xF];
    }
    encoded[len * 2] = '\0';
    return encoded;
}

static void emitPushWord(BytecodeChunk *chunk, const ShellWord *word, int line) {
    char *encoded = encodeWord(word);
    if (!encoded) {
        emitPushString(chunk, "", line);
        return;
    }
    emitPushString(chunk, encoded, line);
    free(encoded);
}

static void emitPushInt(BytecodeChunk *chunk, int value, int line) {
    Value constant = makeInt(value);
    int index = addConstantToChunk(chunk, &constant);
    freeValue(&constant);
    emitConstantOperand(chunk, index, line);
}

static void emitCallHost(BytecodeChunk *chunk, HostFunctionID id, int line) {
    writeBytecodeChunk(chunk, CALL_HOST, line);
    writeBytecodeChunk(chunk, (uint8_t)id, line);
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
        case SHELL_REDIRECT_HERE_STRING: return "<<<";
        case SHELL_REDIRECT_DUP_INPUT: return "<&";
        case SHELL_REDIRECT_DUP_OUTPUT: return ">&";
        case SHELL_REDIRECT_CLOBBER: return ">|";
    }
    return "";
}

static char *buildPipelineMetadata(const ShellPipeline *pipeline) {
    size_t stage_count = pipeline ? pipeline->command_count : 0;
    bool negated = pipeline ? shellPipelineHasExplicitNegation(pipeline) : false;
    size_t merge_len = stage_count;

    char *merge = (char *)malloc(merge_len + 1);
    if (!merge) {
        return NULL;
    }
    for (size_t i = 0; i < merge_len; ++i) {
        merge[i] = shellPipelineGetMergeStderr(pipeline, i) ? '1' : '0';
    }
    merge[merge_len] = '\0';

    size_t meta_len = (size_t)snprintf(NULL, 0, "stages=%zu;negated=%d;merge=%s",
                                      stage_count,
                                      negated ? 1 : 0,
                                      merge);
    char *meta = (char *)malloc(meta_len + 1);
    if (!meta) {
        free(merge);
        return NULL;
    }
    snprintf(meta, meta_len + 1, "stages=%zu;negated=%d;merge=%s",
             stage_count,
             negated ? 1 : 0,
             merge);
    free(merge);
    return meta;
}

static char *buildRedirectionMetadata(const ShellRedirection *redir) {
    if (!redir) {
        return NULL;
    }
    const char *fd_text = (redir->io_number && *redir->io_number) ? redir->io_number : "";
    const char *type_name = redirTypeName(redir->type);

    char *encoded_word = NULL;
    {
        ShellWord *target = shellRedirectionGetWordTarget(redir);
        if (target) {
            char *encoded = encodeWord(target);
            if (encoded) {
                encoded_word = encodeHexString(encoded);
            }
            free(encoded);
        }
    }
    if (!encoded_word) {
        encoded_word = strdup("");
        if (!encoded_word) {
            return NULL;
        }
    }

    const char *dup_target = shellRedirectionGetDupTarget(redir);
    char *dup_hex = encodeHexString(dup_target ? dup_target : "");
    if (!dup_hex) {
        free(encoded_word);
        return NULL;
    }

    const char *here_body = shellRedirectionGetHereDocument(redir);
    bool here_quoted = shellRedirectionHereDocumentIsQuoted(redir);
    char *here_hex = encodeHexString(here_body ? here_body : "");
    if (!here_hex) {
        free(encoded_word);
        free(dup_hex);
        return NULL;
    }

    const char *here_string_literal = shellRedirectionGetHereStringLiteral(redir);
    char *here_string_hex = encodeHexString(here_string_literal ? here_string_literal : "");
    if (!here_string_hex) {
        free(encoded_word);
        free(dup_hex);
        free(here_hex);
        return NULL;
    }

    size_t meta_len = (size_t)snprintf(NULL, 0,
                                       "redir:fd=%s;type=%s;word=%s;dup=%s;here=%s;hereq=%d;hstr=%s",
                                       fd_text,
                                       type_name ? type_name : "",
                                       encoded_word,
                                       dup_hex,
                                       here_hex,
                                       here_quoted ? 1 : 0,
                                       here_string_hex);
    char *meta = (char *)malloc(meta_len + 1);
    if (!meta) {
        free(encoded_word);
        free(dup_hex);
        free(here_hex);
        free(here_string_hex);
        return NULL;
    }
    snprintf(meta, meta_len + 1,
             "redir:fd=%s;type=%s;word=%s;dup=%s;here=%s;hereq=%d;hstr=%s",
             fd_text,
             type_name ? type_name : "",
             encoded_word,
             dup_hex,
             here_hex,
             here_quoted ? 1 : 0,
             here_string_hex);

    free(encoded_word);
    free(dup_hex);
    free(here_hex);
    free(here_string_hex);
    return meta;
}

static void compileCommand(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background);
static void compileProgram(BytecodeChunk *chunk, const ShellProgram *program);
static void compilePipeline(BytecodeChunk *chunk, const ShellPipeline *pipeline, bool runs_in_background);
static void compileCase(BytecodeChunk *chunk, const ShellCase *case_stmt, int line);
static void compileFunction(BytecodeChunk *chunk, const ShellFunction *function, int line);
static void compileArithmetic(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background);

static void compileSimple(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    int line = command ? command->line : 0;
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=%d;pipe=%d;head=%d;tail=%d;line=%d;col=%d",
             (command && command->exec.runs_in_background) || runs_in_background ? 1 : 0,
             command ? command->exec.pipeline_index : -1,
             command && command->exec.is_pipeline_head ? 1 : 0,
             command && command->exec.is_pipeline_tail ? 1 : 0,
             command ? command->line : 0,
             command ? command->column : 0);
    emitPushString(chunk, meta, line);

    size_t arg_count = 1; // metadata entry
    if (command) {
        const ShellWordArray *words = &command->data.simple.words;
        for (size_t i = 0; i < words->count; ++i) {
            const ShellWord *word = words->items[i];
            emitPushWord(chunk, word, line);
            arg_count++;
        }
        const ShellRedirectionArray *redirs = &command->data.simple.redirections;
        for (size_t i = 0; i < redirs->count; ++i) {
            const ShellRedirection *redir = redirs->items[i];
            char *serialized = buildRedirectionMetadata(redir);
            if (!serialized) {
                emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
            } else {
                emitPushString(chunk, serialized, line);
                free(serialized);
            }
            arg_count++;
        }
    }

    if (arg_count > 255) {
        fprintf(stderr, "shell codegen warning: argument vector truncated (%zu).\n", arg_count);
        arg_count = 255;
    }
    emitBuiltinProc(chunk, "__shell_exec", (uint8_t)arg_count, line);
}

static void compileArithmetic(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    int line = command ? command->line : 0;
    char meta[128];
    snprintf(meta, sizeof(meta), "bg=%d;pipe=%d;head=%d;tail=%d;line=%d;col=%d",
             (command && command->exec.runs_in_background) || runs_in_background ? 1 : 0,
             command ? command->exec.pipeline_index : -1,
             command && command->exec.is_pipeline_head ? 1 : 0,
             command && command->exec.is_pipeline_tail ? 1 : 0,
             command ? command->line : 0,
             command ? command->column : 0);
    emitPushString(chunk, meta, line);

    size_t arg_count = 1;
    const char *expr = (command && command->data.arithmetic.expression)
                           ? command->data.arithmetic.expression
                           : "";
    emitPushString(chunk, expr, line);
    arg_count++;

    const ShellRedirectionArray *redirs = command ? shellCommandGetRedirections(command) : NULL;
    size_t redir_count = redirs ? redirs->count : 0;
    for (size_t i = 0; i < redir_count; ++i) {
        const ShellRedirection *redir = redirs->items[i];
        char *serialized = buildRedirectionMetadata(redir);
        if (!serialized) {
            emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
        } else {
            emitPushString(chunk, serialized, line);
            free(serialized);
        }
        arg_count++;
    }

    if (arg_count > 255) {
        fprintf(stderr, "shell codegen warning: arithmetic command args truncated (%zu).\n", arg_count);
        arg_count = 255;
    }
    emitBuiltinProc(chunk, "__shell_arithmetic", (uint8_t)arg_count, line);
}

static void compilePipeline(BytecodeChunk *chunk, const ShellPipeline *pipeline, bool runs_in_background) {
    if (!pipeline) {
        return;
    }
    char *meta = buildPipelineMetadata(pipeline);
    int line = (pipeline->command_count > 0 && pipeline->commands[0]) ? pipeline->commands[0]->line : 0;
    if (!meta) {
        emitPushString(chunk, "stages=0;negated=0;merge=", line);
    } else {
        emitPushString(chunk, meta, line);
        free(meta);
    }
    emitBuiltinProc(chunk, "__shell_pipeline", 1, line);
    for (size_t i = 0; i < pipeline->command_count; ++i) {
        bool stage_background = runs_in_background && (i + 1 == pipeline->command_count);
        ShellCommand *stage_command = pipeline->commands[i];
        if (stage_command) {
            shellCommandPropagatePipelineMetadata(stage_command,
                                                  (int)i,
                                                  i == 0,
                                                  (i + 1) == pipeline->command_count);
        }
        compileCommand(chunk, pipeline->commands[i], stage_background);
    }
}

static void compileLogical(BytecodeChunk *chunk, const ShellLogicalList *logical, int line) {
    if (!logical || logical->count == 0) {
        return;
    }

    size_t pipeline_count = logical->count;
    size_t connector_count = (pipeline_count > 0) ? (pipeline_count - 1) : 0;

    bool guard_condition = connector_count > 0;
    if (guard_condition) {
        emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
    }

    if (connector_count == 0) {
        compilePipeline(chunk, logical->pipelines[0], false);
        if (guard_condition) {
            emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
        }
        return;
    }

    int *patch_sites = (int *)malloc(connector_count * sizeof(int));
    if (!patch_sites) {
        for (size_t i = 0; i < pipeline_count; ++i) {
            compilePipeline(chunk, logical->pipelines[i], false);
            if (i + 1 < pipeline_count) {
                ShellLogicalConnector connector = logical->connectors[i + 1];
                const char *name = connector == SHELL_LOGICAL_AND ? "__shell_and" : "__shell_or";
                char meta[32];
                snprintf(meta, sizeof(meta), "connector=%s", connector == SHELL_LOGICAL_AND ? "&&" : "||");
                emitPushString(chunk, meta, line);
                emitBuiltinProc(chunk, name, 1, line);
            }
        }
        if (guard_condition) {
            emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
        }
        return;
    }

    size_t patch_count = 0;
    for (size_t i = 0; i < pipeline_count - 1; ++i) {
        compilePipeline(chunk, logical->pipelines[i], false);
        emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, line);
        emitPushInt(chunk, 0, line);
        writeBytecodeChunk(chunk, EQUAL, line);
        if (logical->connectors[i + 1] == SHELL_LOGICAL_OR) {
            writeBytecodeChunk(chunk, NOT, line);
        }
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        patch_sites[patch_count++] = chunk->count;
        emitShort(chunk, 0xFFFF, line);
    }

    compilePipeline(chunk, logical->pipelines[pipeline_count - 1], false);

    int leave_label = chunk->count;
    if (guard_condition) {
        emitBuiltinProc(chunk, "__shell_leave_condition_preserve", 0, line);
    }

    int end_label = chunk->count;
    for (size_t i = 0; i < patch_count; ++i) {
        int index = patch_sites[i];
        uint16_t target = guard_condition ? leave_label : end_label;
        uint16_t offset = (uint16_t)(target - (index + 2));
        patchShort(chunk, index, offset);
    }

    free(patch_sites);
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

    bool isFor = loop->is_for;
    bool isCStyle = loop->is_cstyle_for;
    const ShellRedirectionArray *redirs = loop ? &loop->redirections : NULL;
    size_t redir_count = redirs ? redirs->count : 0;

    char meta[128];
    if (isFor) {
        snprintf(meta, sizeof(meta), "mode=for;redirs=%zu", redir_count);
    } else if (isCStyle) {
        snprintf(meta, sizeof(meta), "mode=cfor;redirs=%zu", redir_count);
    } else {
        snprintf(meta, sizeof(meta), "mode=%s;redirs=%zu", loop->is_until ? "until" : "while", redir_count);
    }
    emitPushString(chunk, meta, line);

    uint8_t arg_count = 1;
    if (isFor) {
        if (loop->for_variable) {
            emitPushWord(chunk, loop->for_variable, line);
            arg_count++;
        } else {
            emitPushString(chunk, "", line);
            arg_count++;
        }
        size_t list_count = loop->for_values.count;
        if (list_count + 2 > 255) {
            fprintf(stderr, "shell codegen warning: for loop list truncated (%zu).\n", list_count);
            list_count = 253;
        }
        for (size_t i = 0; i < list_count; ++i) {
            emitPushWord(chunk, loop->for_values.items[i], line);
            if (arg_count < 255) {
                arg_count++;
            }
        }
    } else if (isCStyle) {
        const char *init = loop->cstyle_init ? loop->cstyle_init : "";
        const char *cond = loop->cstyle_condition ? loop->cstyle_condition : "";
        const char *update = loop->cstyle_update ? loop->cstyle_update : "";
        emitPushString(chunk, init, line);
        if (arg_count < 255) arg_count++;
        emitPushString(chunk, cond, line);
        if (arg_count < 255) arg_count++;
        emitPushString(chunk, update, line);
        if (arg_count < 255) arg_count++;
    }

    if (redirs && redir_count > 0) {
        size_t emit_count = redir_count;
        if ((size_t)arg_count + emit_count > 255) {
            if (arg_count < 255) {
                emit_count = 255 - arg_count;
            } else {
                emit_count = 0;
            }
            fprintf(stderr, "shell codegen warning: loop redirections truncated (%zu).\n", redir_count);
        }
        for (size_t i = 0; i < emit_count; ++i) {
            const ShellRedirection *redir = redirs->items[i];
            char *serialized = buildRedirectionMetadata(redir);
            if (!serialized) {
                emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
            } else {
                emitPushString(chunk, serialized, line);
                free(serialized);
            }
            if (arg_count < 255) {
                arg_count++;
            }
        }
    }

    emitBuiltinProc(chunk, "__shell_loop", arg_count, line);

    int conditionStart = chunk->count;
    int exitJump = -1;

    bool usesLoopReady = isFor || isCStyle;
    if (usesLoopReady) {
        emitCallHost(chunk, HOST_FN_SHELL_LOOP_IS_READY, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        exitJump = chunk->count;
        emitShort(chunk, 0xFFFF, line);
    } else {
        emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
        compileCommand(chunk, loop->condition, false);
        emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, line);
        emitPushInt(chunk, 0, line);
        writeBytecodeChunk(chunk, EQUAL, line);
        if (loop->is_until) {
            writeBytecodeChunk(chunk, NOT, line);
        }
        emitBuiltinProc(chunk, "__shell_leave_condition", 0, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        exitJump = chunk->count;
        emitShort(chunk, 0xFFFF, line);
    }

    compileProgram(chunk, loop->body);

    emitCallHost(chunk, HOST_FN_SHELL_LOOP_ADVANCE, line);
    writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
    int exitJump2 = chunk->count;
    emitShort(chunk, 0xFFFF, line);

    writeBytecodeChunk(chunk, JUMP, line);
    int loopJump = chunk->count;
    emitShort(chunk, 0xFFFF, line);

    int exitLabel = chunk->count;
    emitBuiltinProc(chunk, "__shell_loop_end", 0, line);

    uint16_t loopOffset = (uint16_t)(conditionStart - (loopJump + 2));
    patchShort(chunk, loopJump, loopOffset);

    if (exitJump >= 0) {
        uint16_t exitOffset = (uint16_t)(exitLabel - (exitJump + 2));
        patchShort(chunk, exitJump, exitOffset);
    }

    uint16_t exitOffset2 = (uint16_t)(exitLabel - (exitJump2 + 2));
    patchShort(chunk, exitJump2, exitOffset2);
}

static void compileConditional(BytecodeChunk *chunk, const ShellConditional *conditional, int line) {
    if (!conditional) {
        return;
    }
    emitPushString(chunk, "branch=if", line);
    emitBuiltinProc(chunk, "__shell_if", 1, line);
    emitBuiltinProc(chunk, "__shell_enter_condition", 0, line);
    compileCommand(chunk, conditional->condition, false);
    emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, line);
    emitPushInt(chunk, 0, line);
    writeBytecodeChunk(chunk, EQUAL, line);
    emitBuiltinProc(chunk, "__shell_leave_condition", 0, line);
    writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
    int elseJump = chunk->count;
    emitShort(chunk, 0xFFFF, line);
    compileProgram(chunk, conditional->then_branch);
    bool hasElse = conditional->else_branch != NULL;
    if (hasElse) {
        writeBytecodeChunk(chunk, JUMP, line);
        int endJump = chunk->count;
        emitShort(chunk, 0xFFFF, line);
        uint16_t elseOffset = (uint16_t)(chunk->count - (elseJump + 2));
        patchShort(chunk, elseJump, elseOffset);
        compileProgram(chunk, conditional->else_branch);
        uint16_t endOffset = (uint16_t)(chunk->count - (endJump + 2));
        patchShort(chunk, endJump, endOffset);
    } else {
        uint16_t elseOffset = (uint16_t)(chunk->count - (elseJump + 2));
        patchShort(chunk, elseJump, elseOffset);
    }
}

static void compileCase(BytecodeChunk *chunk, const ShellCase *case_stmt, int line) {
    if (!case_stmt) {
        return;
    }
    char meta[64];
    snprintf(meta, sizeof(meta), "clauses=%zu", case_stmt->clauses.count);
    emitPushString(chunk, meta, line);
    emitPushWord(chunk, case_stmt->subject, line);
    emitBuiltinProc(chunk, "__shell_case", 2, line);

    size_t clause_count = case_stmt->clauses.count;
    int *end_jumps = NULL;
    size_t end_jump_count = 0;
    if (clause_count > 0) {
        end_jumps = (int *)calloc(clause_count, sizeof(int));
        if (!end_jumps) {
            fprintf(stderr, "shell codegen warning: unable to allocate case jump table.\n");
        }
    }

    for (size_t i = 0; i < clause_count; ++i) {
        ShellCaseClause *clause = case_stmt->clauses.items[i];
        size_t pattern_count = clause ? clause->patterns.count : 0;
        char clause_meta[64];
        snprintf(clause_meta, sizeof(clause_meta), "index=%zu;patterns=%zu", i, pattern_count);
        int clause_line = clause ? clause->line : line;
        emitPushString(chunk, clause_meta, clause_line);
        size_t max_patterns = pattern_count;
        if (max_patterns > 254) {
            fprintf(stderr, "shell codegen warning: case clause %zu patterns truncated.\n", i);
            max_patterns = 254;
        }
        for (size_t j = 0; j < max_patterns; ++j) {
            const ShellWord *pattern = clause->patterns.items[j];
            emitPushWord(chunk, pattern, clause_line);
        }
        size_t arg_count = max_patterns + 1;
        emitBuiltinProc(chunk, "__shell_case_clause", (uint8_t)arg_count, clause_line);

        emitCallHost(chunk, HOST_FN_SHELL_LAST_STATUS, clause_line);
        emitPushInt(chunk, 0, clause_line);
        writeBytecodeChunk(chunk, EQUAL, clause_line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, clause_line);
        int skip_body_jump = chunk->count;
        emitShort(chunk, 0xFFFF, clause_line);

        compileProgram(chunk, clause ? clause->body : NULL);

        writeBytecodeChunk(chunk, JUMP, clause_line);
        int end_jump_pos = chunk->count;
        emitShort(chunk, 0xFFFF, clause_line);
        if (end_jumps && end_jump_count < clause_count) {
            end_jumps[end_jump_count++] = end_jump_pos;
        }
        uint16_t skip_offset = (uint16_t)(chunk->count - (skip_body_jump + 2));
        patchShort(chunk, skip_body_jump, skip_offset);
    }

    if (end_jumps) {
        for (size_t i = 0; i < end_jump_count; ++i) {
            int pos = end_jumps[i];
            if (pos >= 0) {
                uint16_t offset = (uint16_t)(chunk->count - (pos + 2));
                patchShort(chunk, pos, offset);
            }
        }
        free(end_jumps);
    }

    emitBuiltinProc(chunk, "__shell_case_end", 0, line);
}

static void compileFunction(BytecodeChunk *chunk, const ShellFunction *function, int line) {
    if (!function) {
        return;
    }
    ShellCompiledFunction *compiled = (ShellCompiledFunction *)calloc(1, sizeof(ShellCompiledFunction));
    if (!compiled) {
        fprintf(stderr, "shell codegen warning: unable to allocate function chunk for '%s'.\n",
                function->name ? function->name : "<anonymous>");
        return;
    }
    shellCompile(function->body, &compiled->chunk);
    Value ptr = makePointer(compiled, NULL);
    int ptr_index = addConstantToChunk(chunk, &ptr);
    emitPushString(chunk, function->name ? function->name : "", line);
    emitPushString(chunk, function->parameter_metadata ? function->parameter_metadata : "", line);
    emitConstantOperand(chunk, ptr_index, line);
    emitBuiltinProc(chunk, "__shell_define_function", 3, line);
}

static void compileCommand(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    if (!command) {
        return;
    }
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            compileSimple(chunk, command, runs_in_background);
            break;
        case SHELL_COMMAND_ARITHMETIC:
            compileArithmetic(chunk, command, runs_in_background);
            break;
        case SHELL_COMMAND_PIPELINE:
            compilePipeline(chunk, command->data.pipeline, command->exec.runs_in_background || runs_in_background);
            break;
        case SHELL_COMMAND_LOGICAL:
            compileLogical(chunk, command->data.logical, command->line);
            break;
        case SHELL_COMMAND_SUBSHELL:
            compileSubshell(chunk, command->data.subshell.body, command->line, command->exec.pipeline_index);
            break;
        case SHELL_COMMAND_BRACE_GROUP:
            compileProgram(chunk, command->data.brace_group.body);
            break;
        case SHELL_COMMAND_LOOP:
            compileLoop(chunk, command->data.loop, command->line);
            break;
        case SHELL_COMMAND_CONDITIONAL:
            compileConditional(chunk, command->data.conditional, command->line);
            break;
        case SHELL_COMMAND_CASE:
            compileCase(chunk, command->data.case_stmt, command->line);
            break;
        case SHELL_COMMAND_FUNCTION:
            compileFunction(chunk, command->data.function, command->line);
            break;
    }
}

static void compileProgram(BytecodeChunk *chunk, const ShellProgram *program) {
    if (!program) {
        return;
    }
    for (size_t i = 0; i < program->commands.count; ++i) {
        compileCommand(chunk, program->commands.items[i], false);
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
