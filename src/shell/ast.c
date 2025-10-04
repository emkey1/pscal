#include "shell/ast.h"
#include <stdlib.h>
#include <string.h>

static void shellStringArrayInit(ShellStringArray *array) {
    if (!array) return;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellStringArrayFree(ShellStringArray *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) {
        free(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellStringArrayAppend(ShellStringArray *array, const char *value) {
    if (!array || !value) return;
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        char **new_items = (char **)realloc(array->items, new_capacity * sizeof(char *));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = strdup(value);
}

static void shellCommandSubstitutionArrayInit(ShellCommandSubstitutionArray *array) {
    if (!array) return;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellCommandSubstitutionArrayFree(ShellCommandSubstitutionArray *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) {
        free(array->items[i].command);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellCommandSubstitutionArrayAppend(ShellCommandSubstitutionArray *array,
                                               ShellCommandSubstitutionStyle style,
                                               const char *command,
                                               size_t span_length) {
    if (!array || !command) return;
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellCommandSubstitution *new_items = (ShellCommandSubstitution *)realloc(
            array->items, new_capacity * sizeof(ShellCommandSubstitution));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    ShellCommandSubstitution *entry = &array->items[array->count++];
    entry->style = style;
    entry->span_length = span_length;
    entry->command = strdup(command);
    if (!entry->command) {
        array->count--;
    }
}

static void shellWordArrayInit(ShellWordArray *array) {
    if (!array) return;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellWordArrayAppend(ShellWordArray *array, ShellWord *word) {
    if (!array || !word) return;
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellWord **new_items = (ShellWord **)realloc(array->items, new_capacity * sizeof(ShellWord *));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = word;
}

static void shellWordArrayFree(ShellWordArray *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) {
        shellFreeWord(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellRedirectionArrayInit(ShellRedirectionArray *array) {
    if (!array) return;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellRedirectionArrayAppend(ShellRedirectionArray *array, ShellRedirection *redir) {
    if (!array || !redir) return;
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellRedirection **new_items = (ShellRedirection **)realloc(array->items, new_capacity * sizeof(ShellRedirection *));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = redir;
}

static void shellRedirectionArrayFree(ShellRedirectionArray *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) {
        shellFreeRedirection(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellCommandArrayInit(ShellCommandArray *array) {
    if (!array) return;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellCommandArrayAppend(ShellCommandArray *array, ShellCommand *command) {
    if (!array || !command) return;
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellCommand **new_items = (ShellCommand **)realloc(array->items, new_capacity * sizeof(ShellCommand *));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = command;
}

static void shellCommandArrayFree(ShellCommandArray *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) {
        shellFreeCommand(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellCaseClauseArrayInit(ShellCaseClauseArray *array) {
    if (!array) return;
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

static void shellCaseClauseArrayAppend(ShellCaseClauseArray *array, ShellCaseClause *clause) {
    if (!array || !clause) return;
    if (array->count + 1 > array->capacity) {
        size_t new_capacity = array->capacity ? array->capacity * 2 : 4;
        ShellCaseClause **new_items = (ShellCaseClause **)realloc(array->items, new_capacity * sizeof(ShellCaseClause *));
        if (!new_items) {
            return;
        }
        array->items = new_items;
        array->capacity = new_capacity;
    }
    array->items[array->count++] = clause;
}

static void shellCaseClauseArrayFree(ShellCaseClauseArray *array) {
    if (!array) return;
    for (size_t i = 0; i < array->count; ++i) {
        shellFreeCaseClause(array->items[i]);
    }
    free(array->items);
    array->items = NULL;
    array->count = 0;
    array->capacity = 0;
}

ShellWord *shellCreateWord(const char *text, bool single_quoted, bool double_quoted,
                           bool has_param_expansion, bool has_arith_expansion,
                           int line, int column) {
    ShellWord *word = (ShellWord *)calloc(1, sizeof(ShellWord));
    if (!word) {
        return NULL;
    }
    word->text = text ? strdup(text) : strdup("");
    word->single_quoted = single_quoted;
    word->double_quoted = double_quoted;
    word->has_parameter_expansion = has_param_expansion;
    word->has_arithmetic_expansion = has_arith_expansion;
    word->is_assignment = false;
    word->line = line;
    word->column = column;
    word->has_command_substitution = false;
    shellStringArrayInit(&word->expansions);
    shellCommandSubstitutionArrayInit(&word->command_substitutions);
    return word;
}

void shellWordAddExpansion(ShellWord *word, const char *name) {
    if (!word || !name) return;
    shellStringArrayAppend(&word->expansions, name);
}

void shellWordAddCommandSubstitution(ShellWord *word, ShellCommandSubstitutionStyle style,
                                     const char *command, size_t span_length) {
    if (!word || !command) {
        return;
    }
    shellCommandSubstitutionArrayAppend(&word->command_substitutions, style, command, span_length);
    if (word->command_substitutions.count > 0) {
        word->has_command_substitution = true;
    }
}

void shellFreeWord(ShellWord *word) {
    if (!word) return;
    free(word->text);
    shellStringArrayFree(&word->expansions);
    shellCommandSubstitutionArrayFree(&word->command_substitutions);
    free(word);
}

ShellRedirection *shellCreateRedirection(ShellRedirectionType type, const char *io_number,
                                         ShellWord *target, int line, int column) {
    ShellRedirection *redir = (ShellRedirection *)calloc(1, sizeof(ShellRedirection));
    if (!redir) {
        return NULL;
    }
    redir->type = type;
    redir->io_number = io_number ? strdup(io_number) : NULL;
    redir->target = target;
    redir->here_document = NULL;
    redir->dup_target = NULL;
    redir->line = line;
    redir->column = column;
    return redir;
}

void shellFreeRedirection(ShellRedirection *redir) {
    if (!redir) return;
    free(redir->io_number);
    shellFreeWord(redir->target);
    free(redir->here_document);
    free(redir->dup_target);
    free(redir);
}

void shellRedirectionSetHereDocument(ShellRedirection *redir, const char *payload) {
    if (!redir) {
        return;
    }
    free(redir->here_document);
    redir->here_document = payload ? strdup(payload) : NULL;
}

const char *shellRedirectionGetHereDocument(const ShellRedirection *redir) {
    return redir ? redir->here_document : NULL;
}

void shellRedirectionSetDupTarget(ShellRedirection *redir, const char *target) {
    if (!redir) {
        return;
    }
    free(redir->dup_target);
    redir->dup_target = target ? strdup(target) : NULL;
}

const char *shellRedirectionGetDupTarget(const ShellRedirection *redir) {
    return redir ? redir->dup_target : NULL;
}

ShellWord *shellRedirectionGetWordTarget(const ShellRedirection *redir) {
    return redir ? redir->target : NULL;
}

ShellPipeline *shellCreatePipeline(void) {
    ShellPipeline *pipeline = (ShellPipeline *)calloc(1, sizeof(ShellPipeline));
    if (!pipeline) {
        return NULL;
    }
    pipeline->commands = NULL;
    pipeline->command_count = 0;
    pipeline->negated = false;
    pipeline->has_explicit_negation = false;
    return pipeline;
}

void shellPipelineAddCommand(ShellPipeline *pipeline, ShellCommand *command) {
    if (!pipeline || !command) return;
    size_t new_count = pipeline->command_count + 1;
    ShellCommand **new_items = (ShellCommand **)realloc(pipeline->commands, new_count * sizeof(ShellCommand *));
    if (!new_items) {
        return;
    }
    pipeline->commands = new_items;
    pipeline->commands[pipeline->command_count] = command;
    pipeline->command_count = new_count;
}

void shellFreePipeline(ShellPipeline *pipeline) {
    if (!pipeline) return;
    for (size_t i = 0; i < pipeline->command_count; ++i) {
        shellFreeCommand(pipeline->commands[i]);
    }
    free(pipeline->commands);
    free(pipeline);
}

void shellPipelineSetNegated(ShellPipeline *pipeline, bool negated) {
    if (!pipeline) {
        return;
    }
    pipeline->negated = negated;
    pipeline->has_explicit_negation = negated;
}

bool shellPipelineIsNegated(const ShellPipeline *pipeline) {
    if (!pipeline) {
        return false;
    }
    return pipeline->negated;
}

bool shellPipelineHasExplicitNegation(const ShellPipeline *pipeline) {
    if (!pipeline) {
        return false;
    }
    return pipeline->has_explicit_negation;
}

ShellLogicalList *shellCreateLogicalList(void) {
    ShellLogicalList *list = (ShellLogicalList *)calloc(1, sizeof(ShellLogicalList));
    if (!list) {
        return NULL;
    }
    list->pipelines = NULL;
    list->connectors = NULL;
    list->count = 0;
    return list;
}

void shellLogicalListAdd(ShellLogicalList *list, ShellPipeline *pipeline, ShellLogicalConnector connector) {
    if (!list || !pipeline) return;
    size_t new_count = list->count + 1;
    ShellPipeline **new_pipelines = (ShellPipeline **)realloc(list->pipelines, new_count * sizeof(ShellPipeline *));
    ShellLogicalConnector *new_connectors = (ShellLogicalConnector *)realloc(list->connectors, new_count * sizeof(ShellLogicalConnector));
    if (!new_pipelines || !new_connectors) {
        free(new_pipelines);
        free(new_connectors);
        return;
    }
    list->pipelines = new_pipelines;
    list->connectors = new_connectors;
    list->pipelines[list->count] = pipeline;
    list->connectors[list->count] = connector;
    list->count = new_count;
}

void shellFreeLogicalList(ShellLogicalList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; ++i) {
        shellFreePipeline(list->pipelines[i]);
    }
    free(list->pipelines);
    free(list->connectors);
    free(list);
}

ShellLoop *shellCreateLoop(bool is_until, ShellPipeline *condition, ShellProgram *body) {
    ShellLoop *loop = (ShellLoop *)calloc(1, sizeof(ShellLoop));
    if (!loop) {
        return NULL;
    }
    loop->is_until = is_until;
    loop->condition = condition;
    loop->body = body;
    return loop;
}

void shellFreeLoop(ShellLoop *loop) {
    if (!loop) return;
    shellFreePipeline(loop->condition);
    shellFreeProgram(loop->body);
    free(loop);
}

ShellConditional *shellCreateConditional(ShellPipeline *condition, ShellProgram *then_branch,
                                         ShellProgram *else_branch) {
    ShellConditional *conditional = (ShellConditional *)calloc(1, sizeof(ShellConditional));
    if (!conditional) {
        return NULL;
    }
    conditional->condition = condition;
    conditional->then_branch = then_branch;
    conditional->else_branch = else_branch;
    return conditional;
}

void shellFreeConditional(ShellConditional *conditional) {
    if (!conditional) return;
    shellFreePipeline(conditional->condition);
    shellFreeProgram(conditional->then_branch);
    shellFreeProgram(conditional->else_branch);
    free(conditional);
}

ShellCase *shellCreateCase(ShellWord *subject) {
    ShellCase *case_stmt = (ShellCase *)calloc(1, sizeof(ShellCase));
    if (!case_stmt) {
        return NULL;
    }
    case_stmt->subject = subject;
    shellCaseClauseArrayInit(&case_stmt->clauses);
    return case_stmt;
}

void shellCaseAddClause(ShellCase *case_stmt, ShellCaseClause *clause) {
    if (!case_stmt || !clause) return;
    shellCaseClauseArrayAppend(&case_stmt->clauses, clause);
}

ShellCaseClause *shellCreateCaseClause(int line, int column) {
    ShellCaseClause *clause = (ShellCaseClause *)calloc(1, sizeof(ShellCaseClause));
    if (!clause) {
        return NULL;
    }
    shellWordArrayInit(&clause->patterns);
    clause->body = NULL;
    clause->line = line;
    clause->column = column;
    return clause;
}

void shellCaseClauseAddPattern(ShellCaseClause *clause, ShellWord *pattern) {
    if (!clause || !pattern) return;
    shellWordArrayAppend(&clause->patterns, pattern);
}

void shellCaseClauseSetBody(ShellCaseClause *clause, ShellProgram *body) {
    if (!clause) return;
    clause->body = body;
}

void shellFreeCaseClause(ShellCaseClause *clause) {
    if (!clause) return;
    shellWordArrayFree(&clause->patterns);
    shellFreeProgram(clause->body);
    free(clause);
}

void shellFreeCase(ShellCase *case_stmt) {
    if (!case_stmt) return;
    shellFreeWord(case_stmt->subject);
    shellCaseClauseArrayFree(&case_stmt->clauses);
    free(case_stmt);
}

static ShellCommand *shellCreateCommandInternal(ShellCommandType type) {
    ShellCommand *command = (ShellCommand *)calloc(1, sizeof(ShellCommand));
    if (!command) {
        return NULL;
    }
    command->type = type;
    command->line = 0;
    command->column = 0;
    command->exec.runs_in_background = false;
    command->exec.pipeline_index = -1;
    command->exec.is_pipeline_head = false;
    command->exec.is_pipeline_tail = false;
    command->exec.is_async_parent = false;
    if (type == SHELL_COMMAND_SIMPLE) {
        shellWordArrayInit(&command->data.simple.words);
        shellRedirectionArrayInit(&command->data.simple.redirections);
    } else if (type == SHELL_COMMAND_BRACE_GROUP) {
        command->data.brace_group.body = NULL;
        shellRedirectionArrayInit(&command->data.brace_group.redirections);
    }
    return command;
}

ShellCommand *shellCreateSimpleCommand(void) {
    return shellCreateCommandInternal(SHELL_COMMAND_SIMPLE);
}

ShellCommand *shellCreatePipelineCommand(ShellPipeline *pipeline) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_PIPELINE);
    if (cmd) {
        cmd->data.pipeline = pipeline;
    }
    return cmd;
}

ShellCommand *shellCreateLogicalCommand(ShellLogicalList *logical) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_LOGICAL);
    if (cmd) {
        cmd->data.logical = logical;
    }
    return cmd;
}

ShellCommand *shellCreateSubshellCommand(ShellProgram *body) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_SUBSHELL);
    if (cmd) {
        cmd->data.subshell.body = body;
    }
    return cmd;
}

ShellCommand *shellCreateBraceGroupCommand(ShellProgram *body) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_BRACE_GROUP);
    if (cmd) {
        cmd->data.brace_group.body = body;
    }
    return cmd;
}

ShellCommand *shellCreateLoopCommand(ShellLoop *loop) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_LOOP);
    if (cmd) {
        cmd->data.loop = loop;
    }
    return cmd;
}

ShellCommand *shellCreateConditionalCommand(ShellConditional *conditional) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_CONDITIONAL);
    if (cmd) {
        cmd->data.conditional = conditional;
    }
    return cmd;
}

ShellCommand *shellCreateCaseCommand(ShellCase *case_stmt) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_CASE);
    if (cmd) {
        cmd->data.case_stmt = case_stmt;
    }
    return cmd;
}

ShellFunction *shellCreateFunction(const char *name, const char *parameter_metadata,
                                   ShellProgram *body) {
    ShellFunction *function = (ShellFunction *)calloc(1, sizeof(ShellFunction));
    if (!function) {
        return NULL;
    }
    function->name = name ? strdup(name) : NULL;
    function->parameter_metadata = parameter_metadata && *parameter_metadata
                                        ? strdup(parameter_metadata)
                                        : NULL;
    function->body = body;
    return function;
}

ShellCommand *shellCreateFunctionCommand(ShellFunction *function) {
    ShellCommand *cmd = shellCreateCommandInternal(SHELL_COMMAND_FUNCTION);
    if (cmd) {
        cmd->data.function = function;
    }
    return cmd;
}

void shellFreeFunction(ShellFunction *function) {
    if (!function) {
        return;
    }
    free(function->name);
    free(function->parameter_metadata);
    shellFreeProgram(function->body);
    free(function);
}

void shellCommandAddWord(ShellCommand *command, ShellWord *word) {
    if (!command || command->type != SHELL_COMMAND_SIMPLE) return;
    shellWordArrayAppend(&command->data.simple.words, word);
}

static ShellRedirectionArray *shellCommandResolveRedirections(ShellCommand *command) {
    if (!command) {
        return NULL;
    }
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            return &command->data.simple.redirections;
        case SHELL_COMMAND_BRACE_GROUP:
            return &command->data.brace_group.redirections;
        default:
            return NULL;
    }
}

static const ShellRedirectionArray *shellCommandResolveRedirectionsConst(const ShellCommand *command) {
    if (!command) {
        return NULL;
    }
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            return &command->data.simple.redirections;
        case SHELL_COMMAND_BRACE_GROUP:
            return &command->data.brace_group.redirections;
        default:
            return NULL;
    }
}

void shellCommandAddRedirection(ShellCommand *command, ShellRedirection *redir) {
    if (!command || !redir) {
        return;
    }
    ShellRedirectionArray *array = shellCommandResolveRedirections(command);
    if (!array) {
        shellFreeRedirection(redir);
        return;
    }
    shellRedirectionArrayAppend(array, redir);
}

ShellRedirectionArray *shellCommandGetMutableRedirections(ShellCommand *command) {
    return shellCommandResolveRedirections(command);
}

const ShellRedirectionArray *shellCommandGetRedirections(const ShellCommand *command) {
    return shellCommandResolveRedirectionsConst(command);
}

void shellFreeCommand(ShellCommand *command) {
    if (!command) return;
    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            shellWordArrayFree(&command->data.simple.words);
            shellRedirectionArrayFree(&command->data.simple.redirections);
            break;
        case SHELL_COMMAND_PIPELINE:
            shellFreePipeline(command->data.pipeline);
            break;
        case SHELL_COMMAND_LOGICAL:
            shellFreeLogicalList(command->data.logical);
            break;
        case SHELL_COMMAND_SUBSHELL:
            shellFreeProgram(command->data.subshell.body);
            break;
        case SHELL_COMMAND_BRACE_GROUP:
            shellFreeProgram(command->data.brace_group.body);
            shellRedirectionArrayFree(&command->data.brace_group.redirections);
            break;
        case SHELL_COMMAND_LOOP:
            shellFreeLoop(command->data.loop);
            break;
        case SHELL_COMMAND_CONDITIONAL:
            shellFreeConditional(command->data.conditional);
            break;
        case SHELL_COMMAND_CASE:
            shellFreeCase(command->data.case_stmt);
            break;
        case SHELL_COMMAND_FUNCTION:
            shellFreeFunction(command->data.function);
            break;
    }
    free(command);
}

ShellProgram *shellCreateProgram(void) {
    ShellProgram *program = (ShellProgram *)calloc(1, sizeof(ShellProgram));
    if (!program) {
        return NULL;
    }
    shellCommandArrayInit(&program->commands);
    return program;
}

void shellProgramAddCommand(ShellProgram *program, ShellCommand *command) {
    if (!program || !command) return;
    shellCommandArrayAppend(&program->commands, command);
}

void shellFreeProgram(ShellProgram *program) {
    if (!program) return;
    shellCommandArrayFree(&program->commands);
    free(program);
}

static void shellPrintIndent(FILE *out, int indent) {
    for (int i = 0; i < indent; ++i) {
        fputc(' ', out);
    }
}

static void shellDumpWordJson(FILE *out, const ShellWord *word, int indent) {
    shellPrintIndent(out, indent);
    fprintf(out, "{\n");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"text\": \"%s\",\n", word && word->text ? word->text : "");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"singleQuoted\": %s,\n", word && word->single_quoted ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"doubleQuoted\": %s,\n", word && word->double_quoted ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"hasParameterExpansion\": %s,\n",
            word && word->has_parameter_expansion ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"hasCommandSubstitution\": %s,\n",
            word && word->has_command_substitution ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"isAssignment\": %s",
            word && word->is_assignment ? "true" : "false");

    bool printed_section = false;
    if (word && word->expansions.count > 0) {
        fprintf(out, ",\n");
        shellPrintIndent(out, indent + 2);
        fprintf(out, "\"expansions\": [");
        for (size_t i = 0; i < word->expansions.count; ++i) {
            fprintf(out, "\"%s\"%s", word->expansions.items[i], (i + 1 < word->expansions.count) ? ", " : "");
        }
        fprintf(out, "]");
        printed_section = true;
    }
    if (word && word->command_substitutions.count > 0) {
        fprintf(out, printed_section ? ",\n" : ",\n");
        shellPrintIndent(out, indent + 2);
        fprintf(out, "\"commandSubstitutions\": [");
        for (size_t i = 0; i < word->command_substitutions.count; ++i) {
            const ShellCommandSubstitution *sub = &word->command_substitutions.items[i];
            const char *style = (sub->style == SHELL_COMMAND_SUBSTITUTION_BACKTICK) ? "backtick" : "dollar";
            fprintf(out, "{\"style\": \"%s\", \"span\": %zu, \"command\": \"%s\"}%s",
                    style,
                    sub->span_length,
                    sub->command ? sub->command : "",
                    (i + 1 < word->command_substitutions.count) ? ", " : "");
        }
        fprintf(out, "]");
        printed_section = true;
    }
    fprintf(out, "\n");
    shellPrintIndent(out, indent);
    fprintf(out, "}");
}

static void shellDumpRedirectionJson(FILE *out, const ShellRedirection *redir, int indent) {
    shellPrintIndent(out, indent);
    fprintf(out, "{\n");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"type\": %d,\n", redir ? (int)redir->type : 0);
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"ioNumber\": \"%s\",\n", redir && redir->io_number ? redir->io_number : "");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"target\": ");
    if (redir && redir->target) {
        shellDumpWordJson(out, redir->target, indent + 2);
        fprintf(out, "\n");
    } else {
        fprintf(out, "null\n");
    }
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"hereDocument\": %s,\n",
            (redir && redir->here_document) ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"hereDocumentPayload\": \"%s\",\n",
            (redir && redir->here_document) ? redir->here_document : "");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"dupTarget\": \"%s\"\n", (redir && redir->dup_target) ? redir->dup_target : "");
    shellPrintIndent(out, indent);
    fprintf(out, "}");
}

static void shellDumpCommandJson(FILE *out, const ShellCommand *command, int indent);

static void shellDumpPipelineJson(FILE *out, const ShellPipeline *pipeline, int indent) {
    shellPrintIndent(out, indent);
    fprintf(out, "{\n");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"negated\": %s,\n", pipeline && pipeline->negated ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"explicitNegation\": %s,\n",
            pipeline && pipeline->has_explicit_negation ? "true" : "false");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"commands\": [\n");
    if (pipeline) {
        for (size_t i = 0; i < pipeline->command_count; ++i) {
            shellDumpCommandJson(out, pipeline->commands[i], indent + 4);
            if (i + 1 < pipeline->command_count) {
                fprintf(out, ",\n");
            } else {
                fprintf(out, "\n");
            }
        }
    }
    shellPrintIndent(out, indent + 2);
    fprintf(out, "]\n");
    shellPrintIndent(out, indent);
    fprintf(out, "}");
}

static void shellDumpLogicalListJson(FILE *out, const ShellLogicalList *list, int indent) {
    shellPrintIndent(out, indent);
    fprintf(out, "{\n");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"pipelines\": [\n");
    if (list) {
        for (size_t i = 0; i < list->count; ++i) {
            shellDumpPipelineJson(out, list->pipelines[i], indent + 4);
            if (i + 1 < list->count) {
                fprintf(out, ",\n");
                shellPrintIndent(out, indent + 4);
                fprintf(out, "/* connector: %s */\n", list->connectors[i] == SHELL_LOGICAL_AND ? "&&" : "||");
            } else {
                fprintf(out, "\n");
            }
        }
    }
    shellPrintIndent(out, indent + 2);
    fprintf(out, "]\n");
    shellPrintIndent(out, indent);
    fprintf(out, "}");
}

static void shellDumpProgramJson(FILE *out, const ShellProgram *program, int indent) {
    shellPrintIndent(out, indent);
    fprintf(out, "{\n");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"commands\": [\n");
    if (program) {
        for (size_t i = 0; i < program->commands.count; ++i) {
            shellDumpCommandJson(out, program->commands.items[i], indent + 4);
            if (i + 1 < program->commands.count) {
                fprintf(out, ",\n");
            } else {
                fprintf(out, "\n");
            }
        }
    }
    shellPrintIndent(out, indent + 2);
    fprintf(out, "]\n");
    shellPrintIndent(out, indent);
    fprintf(out, "}");
}

static void shellDumpCommandJson(FILE *out, const ShellCommand *command, int indent) {
    shellPrintIndent(out, indent);
    fprintf(out, "{\n");
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"type\": %d,\n", command ? (int)command->type : -1);
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"exec\": { \"background\": %s, \"pipelineIndex\": %d },\n",
            command && command->exec.runs_in_background ? "true" : "false",
            command ? command->exec.pipeline_index : -1);
    shellPrintIndent(out, indent + 2);
    fprintf(out, "\"payload\": ");
    if (!command) {
        fprintf(out, "null\n");
        shellPrintIndent(out, indent);
        fprintf(out, "}");
        return;
    }

    switch (command->type) {
        case SHELL_COMMAND_SIMPLE:
            fprintf(out, "{\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"words\": [\n");
            for (size_t i = 0; i < command->data.simple.words.count; ++i) {
                shellDumpWordJson(out, command->data.simple.words.items[i], indent + 6);
                if (i + 1 < command->data.simple.words.count) {
                    fprintf(out, ",\n");
                } else {
                    fprintf(out, "\n");
                }
            }
            shellPrintIndent(out, indent + 4);
            fprintf(out, "]\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, ",\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"redirections\": [\n");
            for (size_t i = 0; i < command->data.simple.redirections.count; ++i) {
                shellDumpRedirectionJson(out, command->data.simple.redirections.items[i], indent + 6);
                if (i + 1 < command->data.simple.redirections.count) {
                    fprintf(out, ",\n");
                } else {
                    fprintf(out, "\n");
                }
            }
            shellPrintIndent(out, indent + 4);
            fprintf(out, "]\n");
            shellPrintIndent(out, indent + 2);
            fprintf(out, "}\n");
            break;
        case SHELL_COMMAND_PIPELINE:
            shellDumpPipelineJson(out, command->data.pipeline, indent + 2);
            fprintf(out, "\n");
            break;
        case SHELL_COMMAND_LOGICAL:
            shellDumpLogicalListJson(out, command->data.logical, indent + 2);
            fprintf(out, "\n");
            break;
        case SHELL_COMMAND_SUBSHELL:
            shellDumpProgramJson(out, command->data.subshell.body, indent + 2);
            fprintf(out, "\n");
            break;
        case SHELL_COMMAND_BRACE_GROUP:
            fprintf(out, "{\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"body\": ");
            shellDumpProgramJson(out, command->data.brace_group.body, indent + 4);
            fprintf(out, ",\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"redirections\": [\n");
            const ShellRedirectionArray *brace_redirs =
                shellCommandGetRedirections(command);
            size_t brace_count = brace_redirs ? brace_redirs->count : 0;
            for (size_t i = 0; i < brace_count; ++i) {
                shellDumpRedirectionJson(out, brace_redirs->items[i], indent + 6);
                if (i + 1 < brace_count) {
                    fprintf(out, ",\n");
                } else {
                    fprintf(out, "\n");
                }
            }
            shellPrintIndent(out, indent + 4);
            fprintf(out, "]\n");
            shellPrintIndent(out, indent + 2);
            fprintf(out, "}\n");
            break;
        case SHELL_COMMAND_LOOP:
            fprintf(out, "{\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"isUntil\": %s,\n", command->data.loop && command->data.loop->is_until ? "true" : "false");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"condition\": ");
            shellDumpPipelineJson(out, command->data.loop ? command->data.loop->condition : NULL, indent + 4);
            fprintf(out, ",\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"body\": ");
            shellDumpProgramJson(out, command->data.loop ? command->data.loop->body : NULL, indent + 4);
            fprintf(out, "\n");
            shellPrintIndent(out, indent + 2);
            fprintf(out, "}\n");
            break;
        case SHELL_COMMAND_CONDITIONAL:
            fprintf(out, "{\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"condition\": ");
            shellDumpPipelineJson(out, command->data.conditional ? command->data.conditional->condition : NULL, indent + 4);
            fprintf(out, ",\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"then\": ");
            shellDumpProgramJson(out, command->data.conditional ? command->data.conditional->then_branch : NULL, indent + 4);
            fprintf(out, ",\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"else\": ");
            shellDumpProgramJson(out, command->data.conditional ? command->data.conditional->else_branch : NULL, indent + 4);
            fprintf(out, "\n");
            shellPrintIndent(out, indent + 2);
            fprintf(out, "}\n");
            break;
        case SHELL_COMMAND_CASE:
            fprintf(out, "{\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"subject\": ");
            shellDumpWordJson(out, command->data.case_stmt ? command->data.case_stmt->subject : NULL, indent + 4);
            fprintf(out, ",\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"clauses\": [\n");
            if (command->data.case_stmt) {
                for (size_t i = 0; i < command->data.case_stmt->clauses.count; ++i) {
                    ShellCaseClause *clause = command->data.case_stmt->clauses.items[i];
                    shellPrintIndent(out, indent + 6);
                    fprintf(out, "{\n");
                    shellPrintIndent(out, indent + 8);
                    fprintf(out, "\"patterns\": [\n");
                    if (clause) {
                        for (size_t j = 0; j < clause->patterns.count; ++j) {
                            shellDumpWordJson(out, clause->patterns.items[j], indent + 10);
                            if (j + 1 < clause->patterns.count) {
                                fprintf(out, ",\n");
                            } else {
                                fprintf(out, "\n");
                            }
                        }
                    }
                    shellPrintIndent(out, indent + 8);
                    fprintf(out, "],\n");
                    shellPrintIndent(out, indent + 8);
                    fprintf(out, "\"body\": ");
                    shellDumpProgramJson(out, clause ? clause->body : NULL, indent + 8);
                    fprintf(out, "\n");
                    shellPrintIndent(out, indent + 6);
                    fprintf(out, "}");
                    if (i + 1 < command->data.case_stmt->clauses.count) {
                        fprintf(out, ",\n");
                    } else {
                        fprintf(out, "\n");
                    }
                }
            }
            shellPrintIndent(out, indent + 4);
            fprintf(out, "]\n");
            shellPrintIndent(out, indent + 2);
            fprintf(out, "}\n");
            break;
        case SHELL_COMMAND_FUNCTION:
            fprintf(out, "{\n");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"name\": \"%s\",\n",
                    command->data.function && command->data.function->name ? command->data.function->name : "");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"parameters\": \"%s\",\n",
                    command->data.function && command->data.function->parameter_metadata
                        ? command->data.function->parameter_metadata
                        : "");
            shellPrintIndent(out, indent + 4);
            fprintf(out, "\"body\": ");
            shellDumpProgramJson(out, command->data.function ? command->data.function->body : NULL, indent + 4);
            fprintf(out, "\n");
            shellPrintIndent(out, indent + 2);
            fprintf(out, "}\n");
            break;
    }
    shellPrintIndent(out, indent);
    fprintf(out, "}");
}

void shellDumpAstJson(FILE *out, const ShellProgram *program) {
    if (!out) {
        return;
    }
    shellDumpProgramJson(out, program, 0);
    fprintf(out, "\n");
}
