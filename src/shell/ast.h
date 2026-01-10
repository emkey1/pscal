#ifndef SHELL_AST_H
#define SHELL_AST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ShellCommand;
struct ShellProgram;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} ShellStringArray;

typedef enum {
    SHELL_COMMAND_SUBSTITUTION_DOLLAR,
    SHELL_COMMAND_SUBSTITUTION_BACKTICK
} ShellCommandSubstitutionStyle;

typedef struct {
    ShellCommandSubstitutionStyle style;
    char *command;
    size_t span_length;
} ShellCommandSubstitution;

typedef struct ShellCommandSubstitutionArray {
    ShellCommandSubstitution *items;
    size_t count;
    size_t capacity;
} ShellCommandSubstitutionArray;

typedef struct {
    char *text;
    bool single_quoted;
    bool double_quoted;
    bool has_parameter_expansion;
    bool has_command_substitution;
    bool has_arithmetic_expansion;
    bool is_assignment;
    ShellStringArray expansions;
    ShellCommandSubstitutionArray command_substitutions;
    int line;
    int column;
} ShellWord;

typedef enum {
    SHELL_REDIRECT_INPUT,
    SHELL_REDIRECT_OUTPUT,
    SHELL_REDIRECT_APPEND,
    SHELL_REDIRECT_HEREDOC,
    SHELL_REDIRECT_HERE_STRING,
    SHELL_REDIRECT_DUP_INPUT,
    SHELL_REDIRECT_DUP_OUTPUT,
    SHELL_REDIRECT_CLOBBER
} ShellRedirectionType;

typedef struct {
    ShellRedirectionType type;
    char *io_number;
    ShellWord *target;
    char *here_document;
    char *here_string_literal;
    char *dup_target;
    bool here_document_quoted;
    int line;
    int column;
} ShellRedirection;

typedef struct {
    ShellRedirection **items;
    size_t count;
    size_t capacity;
} ShellRedirectionArray;

typedef struct {
    ShellWord **items;
    size_t count;
    size_t capacity;
} ShellWordArray;

typedef struct {
    struct ShellCommand **items;
    size_t count;
    size_t capacity;
} ShellCommandArray;

typedef struct {
    struct ShellCommand **commands;
    size_t command_count;
    bool negated;
    bool has_explicit_negation;
    bool *merge_stderr;
} ShellPipeline;

typedef enum {
    SHELL_LOGICAL_AND,
    SHELL_LOGICAL_OR
} ShellLogicalConnector;

typedef struct {
    ShellPipeline **pipelines;
    ShellLogicalConnector *connectors;
    size_t count;
} ShellLogicalList;

typedef struct ShellLoop {
    bool is_until;
    bool is_for;
    bool is_cstyle_for;
    ShellWord *for_variable;
    ShellWordArray for_values;
    struct ShellCommand *condition;
    struct ShellProgram *body;
    ShellRedirectionArray redirections;
    char *cstyle_init;
    char *cstyle_condition;
    char *cstyle_update;
} ShellLoop;

typedef struct ShellConditional {
    struct ShellCommand *condition;
    struct ShellProgram *then_branch;
    struct ShellProgram *else_branch;
} ShellConditional;

typedef struct ShellCaseClause {
    ShellWordArray patterns;
    struct ShellProgram *body;
    int line;
    int column;
} ShellCaseClause;

typedef struct {
    ShellCaseClause **items;
    size_t count;
    size_t capacity;
} ShellCaseClauseArray;

typedef struct ShellCase {
    ShellWord *subject;
    ShellCaseClauseArray clauses;
} ShellCase;

typedef struct ShellBraceGroup {
    struct ShellProgram *body;
    ShellRedirectionArray redirections;
} ShellBraceGroup;

typedef struct ShellFunction {
    char *name;
    char *parameter_metadata;
    struct ShellProgram *body;
} ShellFunction;

typedef enum {
    SHELL_COMMAND_SIMPLE,
    SHELL_COMMAND_ARITHMETIC,
    SHELL_COMMAND_PIPELINE,
    SHELL_COMMAND_LOGICAL,
    SHELL_COMMAND_SUBSHELL,
    SHELL_COMMAND_BRACE_GROUP,
    SHELL_COMMAND_LOOP,
    SHELL_COMMAND_CONDITIONAL,
    SHELL_COMMAND_CASE,
    SHELL_COMMAND_FUNCTION
} ShellCommandType;

typedef struct {
    bool runs_in_background;
    int pipeline_index;
    bool is_pipeline_head;
    bool is_pipeline_tail;
    bool is_async_parent;
} ShellExecutionMetadata;

typedef struct ShellCommand {
    ShellCommandType type;
    ShellExecutionMetadata exec;
    int line;
    int column;
    ShellRedirectionArray redirections;
    union {
        struct {
            ShellWordArray words;
        } simple;
        struct {
            char *expression;
        } arithmetic;
        ShellPipeline *pipeline;
        ShellLogicalList *logical;
        struct {
            struct ShellProgram *body;
        } subshell;
        ShellBraceGroup brace_group;
        ShellLoop *loop;
        ShellConditional *conditional;
        ShellCase *case_stmt;
        ShellFunction *function;
    } data;
} ShellCommand;

typedef struct ShellProgram {
    ShellCommandArray commands;
} ShellProgram;

ShellWord *shellCreateWord(const char *text, bool single_quoted, bool double_quoted,
                           bool has_param_expansion, bool has_arith_expansion,
                           int line, int column);
void shellWordAddExpansion(ShellWord *word, const char *name);
void shellWordAddCommandSubstitution(ShellWord *word, ShellCommandSubstitutionStyle style,
                                     const char *command, size_t span_length);
void shellFreeWord(ShellWord *word);

ShellRedirection *shellCreateRedirection(ShellRedirectionType type, const char *io_number,
                                         ShellWord *target, int line, int column);
void shellFreeRedirection(ShellRedirection *redir);
void shellRedirectionSetHereDocument(ShellRedirection *redir, const char *payload, bool quoted);
const char *shellRedirectionGetHereDocument(const ShellRedirection *redir);
bool shellRedirectionHereDocumentIsQuoted(const ShellRedirection *redir);
void shellRedirectionSetHereStringLiteral(ShellRedirection *redir, const char *literal);
const char *shellRedirectionGetHereStringLiteral(const ShellRedirection *redir);
void shellRedirectionSetDupTarget(ShellRedirection *redir, const char *target);
const char *shellRedirectionGetDupTarget(const ShellRedirection *redir);
ShellWord *shellRedirectionGetWordTarget(const ShellRedirection *redir);

ShellPipeline *shellCreatePipeline(void);
void shellPipelineAddCommand(ShellPipeline *pipeline, ShellCommand *command);
void shellFreePipeline(ShellPipeline *pipeline);
void shellPipelineSetNegated(ShellPipeline *pipeline, bool negated);
bool shellPipelineIsNegated(const ShellPipeline *pipeline);
bool shellPipelineHasExplicitNegation(const ShellPipeline *pipeline);
void shellPipelineSetMergeStderr(ShellPipeline *pipeline, size_t index, bool merge);
bool shellPipelineGetMergeStderr(const ShellPipeline *pipeline, size_t index);

ShellLogicalList *shellCreateLogicalList(void);
void shellLogicalListAdd(ShellLogicalList *list, ShellPipeline *pipeline, ShellLogicalConnector connector);
void shellFreeLogicalList(ShellLogicalList *list);

ShellLoop *shellCreateLoop(bool is_until, struct ShellCommand *condition, ShellProgram *body);
ShellLoop *shellCreateForLoop(ShellWord *variable, ShellWordArray *values, ShellProgram *body);
ShellLoop *shellCreateCStyleForLoop(const char *initializer, const char *condition, const char *update,
                                    ShellProgram *body);
void shellFreeLoop(ShellLoop *loop);

ShellConditional *shellCreateConditional(struct ShellCommand *condition, ShellProgram *then_branch,
                                         ShellProgram *else_branch);
void shellFreeConditional(ShellConditional *conditional);

ShellCase *shellCreateCase(ShellWord *subject);
void shellCaseAddClause(ShellCase *case_stmt, ShellCaseClause *clause);
ShellCaseClause *shellCreateCaseClause(int line, int column);
void shellCaseClauseAddPattern(ShellCaseClause *clause, ShellWord *pattern);
void shellCaseClauseSetBody(ShellCaseClause *clause, struct ShellProgram *body);
void shellFreeCaseClause(ShellCaseClause *clause);
void shellFreeCase(ShellCase *case_stmt);

ShellCommand *shellCreateSimpleCommand(void);
ShellCommand *shellCreateArithmeticCommand(char *expression);
ShellCommand *shellCreatePipelineCommand(ShellPipeline *pipeline);
ShellCommand *shellCreateLogicalCommand(ShellLogicalList *logical);
ShellCommand *shellCreateSubshellCommand(ShellProgram *body);
ShellCommand *shellCreateBraceGroupCommand(ShellProgram *body);
ShellCommand *shellCreateLoopCommand(ShellLoop *loop);
ShellCommand *shellCreateConditionalCommand(ShellConditional *conditional);
ShellCommand *shellCreateCaseCommand(ShellCase *case_stmt);
ShellCommand *shellCreateFunctionCommand(ShellFunction *function);
ShellFunction *shellCreateFunction(const char *name, const char *parameter_metadata,
                                   struct ShellProgram *body);
void shellFreeFunction(ShellFunction *function);
void shellCommandAddWord(ShellCommand *command, ShellWord *word);
void shellCommandAddRedirection(ShellCommand *command, ShellRedirection *redir);
ShellRedirectionArray *shellCommandGetMutableRedirections(ShellCommand *command);
const ShellRedirectionArray *shellCommandGetRedirections(const ShellCommand *command);
void shellFreeCommand(ShellCommand *command);

ShellProgram *shellCreateProgram(void);
void shellProgramAddCommand(ShellProgram *program, ShellCommand *command);
void shellFreeProgram(ShellProgram *program);
void shellCommandPropagatePipelineMetadata(ShellCommand *command,
                                           int pipeline_index,
                                           bool is_pipeline_head,
                                           bool is_pipeline_tail);

void shellDumpAstJson(FILE *out, const ShellProgram *program);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_AST_H */
