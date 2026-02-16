#include "shell/codegen.h"
#include "shell/builtins.h"
#include "shell/word_encoding.h"
#include "shell/function.h"
#include "vm/string_sentinels.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "Pascal/globals.h"
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

static int addBuiltinNameConstant(BytecodeChunk *chunk,
                                  const char *encoded_name,
                                  const char *canonical_hint) {
    if (!chunk) {
        return -1;
    }
    int name_index = addStringConstant(chunk, encoded_name);
    if (name_index < 0) {
        return name_index;
    }

    if (getBuiltinLowercaseIndex(chunk, name_index) >= 0) {
        return name_index;
    }

    const char *lower_source = (canonical_hint && *canonical_hint) ? canonical_hint : encoded_name;
    char lowered[MAX_SYMBOL_LENGTH];
    strncpy(lowered, lower_source ? lower_source : "", sizeof(lowered) - 1);
    lowered[sizeof(lowered) - 1] = '\0';
    toLowerString(lowered);

    Value lower_val = makeString(lowered);
    int lower_index = addConstantToChunk(chunk, &lower_val);
    freeValue(&lower_val);
    setBuiltinLowercaseIndex(chunk, name_index, lower_index);
    return name_index;
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

#define LOOP_COND_KIND_NONE   0
#define LOOP_COND_KIND_TEST   1
#define LOOP_COND_KIND_BRACKET 2
#define LOOP_COND_KIND_COLON  3
#define LOOP_COND_KIND_TRUE   4
#define LOOP_COND_KIND_FALSE  5
#define LOOP_COND_KIND_ARITH  6

typedef struct {
    int kind;
    char **encoded_words;
    size_t word_count;
    char *arith_expression;
} LoopConditionSpec;

static void initLoopConditionSpec(LoopConditionSpec *spec) {
    if (!spec) {
        return;
    }
    spec->kind = LOOP_COND_KIND_NONE;
    spec->encoded_words = NULL;
    spec->word_count = 0;
    spec->arith_expression = NULL;
}

static void freeLoopConditionSpec(LoopConditionSpec *spec) {
    if (!spec) {
        return;
    }
    if (spec->encoded_words) {
        for (size_t i = 0; i < spec->word_count; ++i) {
            free(spec->encoded_words[i]);
        }
        free(spec->encoded_words);
    }
    spec->encoded_words = NULL;
    spec->word_count = 0;
    if (spec->arith_expression) {
        free(spec->arith_expression);
        spec->arith_expression = NULL;
    }
    spec->kind = LOOP_COND_KIND_NONE;
}

static bool wordIsLiteralCommand(const ShellWord *word) {
    if (!word || !word->text) {
        return false;
    }
    if (word->single_quoted || word->double_quoted) {
        return false;
    }
    if (word->has_parameter_expansion || word->has_command_substitution || word->has_arithmetic_expansion) {
        return false;
    }
    return true;
}

static bool gatherLoopConditionSpec(const ShellLoop *loop, LoopConditionSpec *out_spec) {
    if (!loop || !out_spec) {
        return false;
    }
    initLoopConditionSpec(out_spec);
    if (loop->is_for || loop->is_cstyle_for) {
        return false;
    }
    const ShellCommand *cond = loop->condition;
    if (!cond) {
        return false;
    }
    if (cond->type == SHELL_COMMAND_PIPELINE && cond->data.pipeline && cond->data.pipeline->command_count == 1) {
        const ShellPipeline *pipeline = cond->data.pipeline;
        if (pipeline && pipeline->commands && pipeline->commands[0]) {
            cond = pipeline->commands[0];
        }
    }
    if (cond->exec.runs_in_background) {
        return false;
    }
    if (cond->type == SHELL_COMMAND_SIMPLE) {
        const ShellWordArray *words = &cond->data.simple.words;
        if (cond->redirections.count > 0 || words->count == 0) {
            return false;
        }
        const ShellWord *first = words->items[0];
        if (!wordIsLiteralCommand(first)) {
            return false;
        }
        const char *cmd = first->text ? first->text : "";
        int kind = LOOP_COND_KIND_NONE;
        if (strcmp(cmd, "test") == 0) {
            if (words->count > 4) {
                return false;
            }
            kind = LOOP_COND_KIND_TEST;
        } else if (strcmp(cmd, "[") == 0) {
            if (words->count < 2 || words->count > 5) {
                return false;
            }
            const ShellWord *last = words->items[words->count - 1];
            if (!last || !last->text || strcmp(last->text, "]") != 0) {
                return false;
            }
            if (!wordIsLiteralCommand(last)) {
                return false;
            }
            kind = LOOP_COND_KIND_BRACKET;
        } else if (strcmp(cmd, ":") == 0) {
            if (words->count != 1) {
                return false;
            }
            kind = LOOP_COND_KIND_COLON;
        } else if (strcmp(cmd, "true") == 0) {
            if (words->count != 1) {
                return false;
            }
            kind = LOOP_COND_KIND_TRUE;
        } else if (strcmp(cmd, "false") == 0) {
            if (words->count != 1) {
                return false;
            }
            kind = LOOP_COND_KIND_FALSE;
        } else {
            return false;
        }
        if (kind == LOOP_COND_KIND_TEST || kind == LOOP_COND_KIND_BRACKET) {
            for (size_t i = 1; i < words->count; ++i) {
                const ShellWord *word = words->items[i];
                if (!word || !word->text) {
                    continue;
                }
                if (strcmp(word->text, "-a") == 0 || strcmp(word->text, "-o") == 0) {
                    return false;
                }
            }
            char **encoded = (char **)calloc(words->count, sizeof(char *));
            if (!encoded) {
                return false;
            }
            for (size_t i = 0; i < words->count; ++i) {
                encoded[i] = encodeWord(words->items[i]);
                if (!encoded[i]) {
                    for (size_t j = 0; j < i; ++j) {
                        free(encoded[j]);
                    }
                    free(encoded);
                    return false;
                }
            }
            out_spec->encoded_words = encoded;
            out_spec->word_count = words->count;
        }
        out_spec->kind = kind;
        return true;
    } else if (cond->type == SHELL_COMMAND_ARITHMETIC) {
        const char *expr = cond->data.arithmetic.expression ? cond->data.arithmetic.expression : "";
        if (cond->redirections.count > 0) {
            return false;
        }
        out_spec->arith_expression = strdup(expr);
        if (!out_spec->arith_expression) {
            return false;
        }
        out_spec->kind = LOOP_COND_KIND_ARITH;
        return true;
    }
    return false;
}

typedef enum {
    LOOP_BODY_KIND_NONE = 0,
    LOOP_BODY_KIND_COLON,
    LOOP_BODY_KIND_TRUE,
    LOOP_BODY_KIND_FALSE,
    LOOP_BODY_KIND_TEST,
    LOOP_BODY_KIND_BRACKET,
    LOOP_BODY_KIND_ARITH,
    LOOP_BODY_KIND_TEST_ARITH,
    LOOP_BODY_KIND_BRACKET_ARITH
} LoopBodyKind;

typedef struct {
    LoopBodyKind kind;
    char **test_words;
    size_t test_word_count;
    char *arith_expression;
} LoopBodySpec;

static void initLoopBodySpec(LoopBodySpec *spec) {
    if (!spec) {
        return;
    }
    spec->kind = LOOP_BODY_KIND_NONE;
    spec->test_words = NULL;
    spec->test_word_count = 0;
    spec->arith_expression = NULL;
}

static void freeLoopBodySpec(LoopBodySpec *spec) {
    if (!spec) {
        return;
    }
    if (spec->test_words) {
        for (size_t i = 0; i < spec->test_word_count; ++i) {
            free(spec->test_words[i]);
        }
        free(spec->test_words);
        spec->test_words = NULL;
    }
    spec->test_word_count = 0;
    if (spec->arith_expression) {
        free(spec->arith_expression);
        spec->arith_expression = NULL;
    }
    spec->kind = LOOP_BODY_KIND_NONE;
}

typedef enum {
    BODY_CMD_NONE = 0,
    BODY_CMD_COLON,
    BODY_CMD_TRUE,
    BODY_CMD_FALSE,
    BODY_CMD_TEST,
    BODY_CMD_BRACKET,
    BODY_CMD_ARITH,
    BODY_CMD_OTHER
} BodyCommandType;

static const ShellCommand *unwrapPipelineCommand(const ShellCommand *cmd) {
    if (!cmd || cmd->type != SHELL_COMMAND_PIPELINE || !cmd->data.pipeline) {
        return cmd;
    }
    const ShellPipeline *pipeline = cmd->data.pipeline;
    if (!pipeline || pipeline->command_count != 1 || !pipeline->commands || !pipeline->commands[0]) {
        return cmd;
    }
    return pipeline->commands[0];
}

static bool captureArithmeticExpression(const ShellCommand *cmd, LoopBodySpec *spec) {
    if (!cmd || cmd->type != SHELL_COMMAND_ARITHMETIC || !spec) {
        return false;
    }
    const char *expr = cmd->data.arithmetic.expression ? cmd->data.arithmetic.expression : "";
    spec->arith_expression = strdup(expr);
    if (!spec->arith_expression) {
        return false;
    }
    return true;
}

static bool captureTestWords(const ShellCommand *cmd, LoopBodySpec *spec) {
    if (!cmd || cmd->type != SHELL_COMMAND_SIMPLE || !spec) {
        return false;
    }
    const ShellWordArray *words = &cmd->data.simple.words;
    size_t count = words->count;
    if (count == 0) {
        return false;
    }
    char **encoded = (char **)calloc(count, sizeof(char *));
    if (!encoded) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        encoded[i] = encodeWord(words->items[i]);
        if (!encoded[i]) {
            for (size_t j = 0; j < i; ++j) {
                free(encoded[j]);
            }
            free(encoded);
            return false;
        }
    }
    spec->test_words = encoded;
    spec->test_word_count = count;
    return true;
}

static BodyCommandType classifyBodyCommand(const ShellCommand *cmd) {
    if (!cmd) {
        return BODY_CMD_NONE;
    }
    if (cmd->exec.runs_in_background || cmd->exec.is_async_parent) {
        return BODY_CMD_OTHER;
    }
    switch (cmd->type) {
        case SHELL_COMMAND_SIMPLE: {
            const ShellWordArray *words = &cmd->data.simple.words;
            const ShellRedirectionArray *redirs = &cmd->redirections;
            if (redirs->count > 0) {
                return BODY_CMD_OTHER;
            }
            if (words->count == 0) {
                return BODY_CMD_NONE;
            }
            const ShellWord *first = words->items[0];
            if (!wordIsLiteralCommand(first) || !first->text) {
                return BODY_CMD_OTHER;
            }
            const char *text = first->text;
            if (strcmp(text, ":") == 0) {
                return BODY_CMD_COLON;
            }
            if (strcmp(text, "true") == 0) {
                return BODY_CMD_TRUE;
            }
            if (strcmp(text, "false") == 0) {
                return BODY_CMD_FALSE;
            }
            if (strcmp(text, "test") == 0) {
                return BODY_CMD_TEST;
            }
            if (strcmp(text, "[") == 0) {
                if (words->count < 2) {
                    return BODY_CMD_OTHER;
                }
                const ShellWord *last = words->items[words->count - 1];
                if (!last || !last->text || strcmp(last->text, "]") != 0 || !wordIsLiteralCommand(last)) {
                    return BODY_CMD_OTHER;
                }
                return BODY_CMD_BRACKET;
            }
            if (strcmp(text, "[[") == 0) {
                if (words->count < 2) {
                    return BODY_CMD_OTHER;
                }
                const ShellWord *last = words->items[words->count - 1];
                if (!last || !last->text || strcmp(last->text, "]]") != 0 || !wordIsLiteralCommand(last)) {
                    return BODY_CMD_OTHER;
                }
                return BODY_CMD_BRACKET;
            }
            return BODY_CMD_OTHER;
        }
        case SHELL_COMMAND_ARITHMETIC: {
            const ShellRedirectionArray *redirs = &cmd->redirections;
            if (redirs->count > 0) {
                return BODY_CMD_OTHER;
            }
            return BODY_CMD_ARITH;
        }
        default:
            return BODY_CMD_OTHER;
    }
}

static bool populateSingleBodySpec(BodyCommandType type, const ShellCommand *cmd, LoopBodySpec *spec) {
    switch (type) {
        case BODY_CMD_COLON:
            spec->kind = LOOP_BODY_KIND_COLON;
            return true;
        case BODY_CMD_TRUE:
            spec->kind = LOOP_BODY_KIND_TRUE;
            return true;
        case BODY_CMD_FALSE:
            spec->kind = LOOP_BODY_KIND_FALSE;
            return true;
        case BODY_CMD_TEST:
            if (!captureTestWords(cmd, spec)) {
                return false;
            }
            spec->kind = LOOP_BODY_KIND_TEST;
            return true;
        case BODY_CMD_BRACKET:
            if (!captureTestWords(cmd, spec)) {
                return false;
            }
            spec->kind = LOOP_BODY_KIND_BRACKET;
            return true;
        case BODY_CMD_ARITH:
            if (!captureArithmeticExpression(cmd, spec)) {
                return false;
            }
            spec->kind = LOOP_BODY_KIND_ARITH;
            return true;
        case BODY_CMD_NONE:
            spec->kind = LOOP_BODY_KIND_COLON;
            return true;
        default:
            return false;
    }
}

static bool gatherLoopBodySpec(const ShellLoop *loop, LoopBodySpec *out_spec) {
    if (!loop || !out_spec) {
        return false;
    }
    initLoopBodySpec(out_spec);

    const ShellProgram *body = loop->body;
    if (!body) {
        out_spec->kind = LOOP_BODY_KIND_COLON;
        return true;
    }

    typedef struct {
        BodyCommandType type;
        const ShellCommand *cmd;
    } BodyComponent;

    BodyComponent components[3];
    size_t component_count = 0;

    for (size_t i = 0; i < body->commands.count; ++i) {
        const ShellCommand *candidate = body->commands.items[i];
        if (!candidate) {
            continue;
        }
        if (candidate->exec.runs_in_background || candidate->exec.is_async_parent) {
            return false;
        }
        const ShellCommand *unwrapped = unwrapPipelineCommand(candidate);
        BodyCommandType type = classifyBodyCommand(unwrapped);
        if (type == BODY_CMD_NONE) {
            continue;
        }
        if (type == BODY_CMD_OTHER) {
            return false;
        }
        components[component_count].type = type;
        components[component_count].cmd = unwrapped;
        component_count++;
        if (component_count > 2) {
            return false;
        }
    }

    if (component_count == 0) {
        out_spec->kind = LOOP_BODY_KIND_COLON;
        return true;
    }

    if (component_count == 1) {
        if (!populateSingleBodySpec(components[0].type, components[0].cmd, out_spec)) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
            return false;
        }
        return true;
    }

    BodyCommandType firstType = components[0].type;
    BodyCommandType secondType = components[1].type;

    if (firstType == BODY_CMD_COLON || firstType == BODY_CMD_TRUE) {
        bool ok = populateSingleBodySpec(secondType, components[1].cmd, out_spec);
        if (!ok) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
        }
        return ok;
    }

    if (firstType == BODY_CMD_ARITH && (secondType == BODY_CMD_COLON || secondType == BODY_CMD_TRUE)) {
        bool ok = populateSingleBodySpec(firstType, components[0].cmd, out_spec);
        if (!ok) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
        }
        return ok;
    }

    if ((firstType == BODY_CMD_TEST || firstType == BODY_CMD_BRACKET) && secondType == BODY_CMD_ARITH) {
        if (!captureTestWords(components[0].cmd, out_spec)) {
            return false;
        }
        if (!captureArithmeticExpression(components[1].cmd, out_spec)) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
            return false;
        }
        out_spec->kind = (firstType == BODY_CMD_TEST) ? LOOP_BODY_KIND_TEST_ARITH : LOOP_BODY_KIND_BRACKET_ARITH;
        return true;
    }

    if (firstType == BODY_CMD_ARITH && (secondType == BODY_CMD_COLON || secondType == BODY_CMD_TRUE)) {
        bool ok = populateSingleBodySpec(firstType, components[0].cmd, out_spec);
        if (!ok) {
            freeLoopBodySpec(out_spec);
            initLoopBodySpec(out_spec);
        }
        return ok;
    }

    if ((firstType == BODY_CMD_COLON || firstType == BODY_CMD_TRUE) && (secondType == BODY_CMD_COLON || secondType == BODY_CMD_TRUE)) {
        out_spec->kind = LOOP_BODY_KIND_COLON;
        return true;
    }

    freeLoopBodySpec(out_spec);
    initLoopBodySpec(out_spec);
    return false;
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
    int name_index = addBuiltinNameConstant(chunk, canonical, canonical);
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
        const ShellRedirectionArray *redirs = &command->redirections;
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

static void compileSubshell(BytecodeChunk *chunk, const ShellCommand *command) {
    if (!command) {
        return;
    }
    const ShellProgram *body = command->data.subshell.body;
    int line = command->line;
    int index = command->exec.pipeline_index;
    char meta[128];
    snprintf(meta, sizeof(meta), "mode=enter;subshell=%d", index);
    emitPushString(chunk, meta, line);
    size_t arg_count = 1;
    /* Serialize any attached redirections for the enter phase. */
    const ShellRedirectionArray *enter_redirs = shellCommandGetRedirections(command);
    if (enter_redirs && enter_redirs->count > 0) {
        for (size_t i = 0; i < enter_redirs->count && arg_count < 255; ++i) {
            const ShellRedirection *redir = enter_redirs->items[i];
            char *serialized = buildRedirectionMetadata(redir);
            if (serialized) {
                emitPushString(chunk, serialized, line);
                free(serialized);
            } else {
                emitPushString(chunk, "redir:fd=;type=;word=;dup=;here=", line);
            }
            arg_count++;
        }
    }
    emitBuiltinProc(chunk, "__shell_subshell", (uint8_t)arg_count, line);
    compileProgram(chunk, body);
    emitPushString(chunk, "mode=leave", line);
    emitBuiltinProc(chunk, "__shell_subshell", 1, line);
}

static void compileLoop(BytecodeChunk *chunk, const ShellCommand *command, bool runs_in_background) {
    (void)runs_in_background;
    if (!command) {
        return;
    }
    const ShellLoop *loop = command->data.loop;
    if (!loop) {
        return;
    }

    int line = command->line;
    int pipeline_index = command->exec.pipeline_index;
    int pipeline_head = command->exec.is_pipeline_head ? 1 : 0;
    int pipeline_tail = command->exec.is_pipeline_tail ? 1 : 0;

    bool isFor = loop->is_for;
    bool isCStyle = loop->is_cstyle_for;
    const ShellRedirectionArray *redirs = loop ? &loop->redirections : NULL;
    size_t redir_count = redirs ? redirs->count : 0;

    LoopConditionSpec cond_spec;
    initLoopConditionSpec(&cond_spec);
    bool cond_fast = false;
    size_t cond_payload_count = 0;
    if (!isFor && !isCStyle && gatherLoopConditionSpec(loop, &cond_spec)) {
        if (cond_spec.kind != LOOP_COND_KIND_NONE) {
            cond_fast = true;
            if (cond_spec.kind == LOOP_COND_KIND_ARITH) {
                cond_payload_count = 1;
            } else if (cond_spec.kind == LOOP_COND_KIND_TEST || cond_spec.kind == LOOP_COND_KIND_BRACKET) {
                cond_payload_count = cond_spec.word_count;
            } else {
                cond_payload_count = 0;
            }
        }
    }

    LoopBodySpec body_spec;
    initLoopBodySpec(&body_spec);
    bool body_fast = false;
    size_t body_payload_count = 0;
    size_t body_expression_count = 0;
    if (!isFor && !isCStyle && gatherLoopBodySpec(loop, &body_spec)) {
        if (body_spec.kind != LOOP_BODY_KIND_NONE) {
            body_fast = true;
            switch (body_spec.kind) {
                case LOOP_BODY_KIND_ARITH:
                    body_expression_count = 1;
                    break;
                case LOOP_BODY_KIND_TEST:
                case LOOP_BODY_KIND_BRACKET:
                    body_payload_count = body_spec.test_word_count;
                    break;
                case LOOP_BODY_KIND_TEST_ARITH:
                case LOOP_BODY_KIND_BRACKET_ARITH:
                    body_payload_count = body_spec.test_word_count;
                    body_expression_count = 1;
                    break;
                default:
                    break;
            }
        }
    }

    if (!isFor && !isCStyle) {
        size_t projected = 1 + cond_payload_count + body_payload_count + body_expression_count + redir_count;
        if (projected > 255) {
            if (body_fast && (body_payload_count + body_expression_count) > 0) {
                body_fast = false;
                body_payload_count = 0;
                body_expression_count = 0;
            }
            projected = 1 + cond_payload_count + body_payload_count + body_expression_count + redir_count;
            if (projected > 255 && cond_fast && cond_payload_count > 0) {
                cond_fast = false;
                cond_payload_count = 0;
                freeLoopConditionSpec(&cond_spec);
                initLoopConditionSpec(&cond_spec);
            }
        }
    }

    int cond_kind_meta = cond_fast ? cond_spec.kind : LOOP_COND_KIND_NONE;
    size_t cond_words_meta = cond_fast ? cond_payload_count : 0;
    int body_kind_meta = body_fast ? body_spec.kind : LOOP_BODY_KIND_NONE;
    size_t body_words_meta = body_fast ? body_payload_count : 0;

    if (!body_fast) {
        body_payload_count = 0;
        body_expression_count = 0;
    }

    char meta[192];
    if (isFor) {
        snprintf(meta,
                 sizeof(meta),
                 "mode=for;redirs=%zu;condkind=0;condwords=0;bodykind=0;bodywords=0;pipe=%d;head=%d;tail=%d",
                 redir_count,
                 pipeline_index,
                 pipeline_head,
                 pipeline_tail);
    } else if (isCStyle) {
        snprintf(meta,
                 sizeof(meta),
                 "mode=cfor;redirs=%zu;condkind=0;condwords=0;bodykind=0;bodywords=0;pipe=%d;head=%d;tail=%d",
                 redir_count,
                 pipeline_index,
                 pipeline_head,
                 pipeline_tail);
    } else {
        snprintf(meta,
                 sizeof(meta),
                 "mode=%s;redirs=%zu;condkind=%d;condwords=%zu;bodykind=%d;bodywords=%zu;pipe=%d;head=%d;tail=%d",
                 loop->is_until ? "until" : "while",
                 redir_count,
                 cond_kind_meta,
                 cond_words_meta,
                 body_kind_meta,
                 body_words_meta,
                 pipeline_index,
                 pipeline_head,
                 pipeline_tail);
    }
    emitPushString(chunk, meta, line);

    uint8_t arg_count = 1;
    if (cond_fast) {
        if (cond_spec.kind == LOOP_COND_KIND_ARITH) {
            const char *expr = cond_spec.arith_expression ? cond_spec.arith_expression : "";
            emitPushString(chunk, expr, line);
            if (arg_count < 255) {
                arg_count++;
            } else {
                cond_fast = false;
            }
        } else if (cond_spec.kind == LOOP_COND_KIND_TEST || cond_spec.kind == LOOP_COND_KIND_BRACKET) {
            for (size_t i = 0; i < cond_spec.word_count; ++i) {
                if (arg_count >= 255) {
                    cond_fast = false;
                    break;
                }
                const char *encoded = cond_spec.encoded_words[i] ? cond_spec.encoded_words[i] : "";
                emitPushString(chunk, encoded, line);
                arg_count++;
            }
            if (!cond_fast) {
                freeLoopConditionSpec(&cond_spec);
                initLoopConditionSpec(&cond_spec);
                cond_payload_count = 0;
                cond_kind_meta = LOOP_COND_KIND_NONE;
            }
        }
    }
    if (body_fast) {
        if (body_words_meta > 0) {
            for (size_t i = 0; i < body_spec.test_word_count; ++i) {
                if (arg_count >= 255) {
                    body_fast = false;
                    break;
                }
                const char *encoded = body_spec.test_words[i] ? body_spec.test_words[i] : "";
                emitPushString(chunk, encoded, line);
                arg_count++;
            }
        }
        if (body_fast && body_expression_count > 0) {
            const char *expr = body_spec.arith_expression ? body_spec.arith_expression : "";
            emitPushString(chunk, expr, line);
            if (arg_count < 255) {
                arg_count++;
            } else {
                body_fast = false;
            }
        }
        if (!body_fast) {
            body_kind_meta = LOOP_BODY_KIND_NONE;
            body_words_meta = 0;
            body_payload_count = 0;
            body_expression_count = 0;
        }
    }
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
    bool fusedLoop = false;

    bool usesLoopReady = isFor || isCStyle;
    if (usesLoopReady) {
        emitCallHost(chunk, HOST_FN_SHELL_LOOP_IS_READY, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        exitJump = chunk->count;
        emitShort(chunk, 0xFFFF, line);
    } else if (cond_fast && body_fast && cond_kind_meta != LOOP_COND_KIND_NONE) {
        fusedLoop = true;
        emitCallHost(chunk, HOST_FN_SHELL_LOOP_CHECK_BODY, line);
        writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
        exitJump = chunk->count;
        emitShort(chunk, 0xFFFF, line);
    } else {
        if (cond_fast && cond_kind_meta != LOOP_COND_KIND_NONE) {
            emitCallHost(chunk, HOST_FN_SHELL_LOOP_CHECK_CONDITION, line);
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
    }

    int exitJump2 = -1;
    if (!fusedLoop) {
        if (body_fast) {
            emitCallHost(chunk, HOST_FN_SHELL_LOOP_EXEC_BODY, line);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            exitJump2 = chunk->count;
            emitShort(chunk, 0xFFFF, line);
        } else {
            compileProgram(chunk, loop->body);
            emitCallHost(chunk, HOST_FN_SHELL_LOOP_ADVANCE, line);
            writeBytecodeChunk(chunk, JUMP_IF_FALSE, line);
            exitJump2 = chunk->count;
            emitShort(chunk, 0xFFFF, line);
        }
    }

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

    if (exitJump2 >= 0) {
        uint16_t exitOffset2 = (uint16_t)(exitLabel - (exitJump2 + 2));
        patchShort(chunk, exitJump2, exitOffset2);
    }

    freeLoopConditionSpec(&cond_spec);
    freeLoopBodySpec(&body_spec);
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
    compiled->magic = SHELL_COMPILED_FUNCTION_MAGIC;
    shellCompile(function->body, &compiled->chunk);
    Value ptr = makePointer(compiled, SHELL_FUNCTION_PTR_SENTINEL);
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
            compileSubshell(chunk, command);
            break;
        case SHELL_COMMAND_BRACE_GROUP:
            compileProgram(chunk, command->data.brace_group.body);
            break;
        case SHELL_COMMAND_LOOP:
            compileLoop(chunk, command, runs_in_background);
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
