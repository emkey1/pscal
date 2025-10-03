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

typedef struct {
    char *text;
    bool single_quoted;
    bool double_quoted;
    bool has_parameter_expansion;
    ShellStringArray expansions;
    int line;
    int column;
} ShellWord;

typedef enum {
    SHELL_REDIRECT_INPUT,
    SHELL_REDIRECT_OUTPUT,
    SHELL_REDIRECT_APPEND,
    SHELL_REDIRECT_HEREDOC,
    SHELL_REDIRECT_DUP_INPUT,
    SHELL_REDIRECT_DUP_OUTPUT,
    SHELL_REDIRECT_CLOBBER
} ShellRedirectionType;

typedef struct {
    ShellRedirectionType type;
    char *io_number;
    ShellWord *target;
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
    ShellPipeline *condition;
    struct ShellProgram *body;
} ShellLoop;

typedef struct ShellConditional {
    ShellPipeline *condition;
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

typedef enum {
    SHELL_COMMAND_SIMPLE,
    SHELL_COMMAND_PIPELINE,
    SHELL_COMMAND_LOGICAL,
    SHELL_COMMAND_SUBSHELL,
    SHELL_COMMAND_LOOP,
    SHELL_COMMAND_CONDITIONAL,
    SHELL_COMMAND_CASE
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
    union {
        struct {
            ShellWordArray words;
            ShellRedirectionArray redirections;
        } simple;
        ShellPipeline *pipeline;
        ShellLogicalList *logical;
        struct {
            struct ShellProgram *body;
        } subshell;
        ShellLoop *loop;
        ShellConditional *conditional;
        ShellCase *case_stmt;
    } data;
} ShellCommand;

typedef struct ShellProgram {
    ShellCommandArray commands;
} ShellProgram;

ShellWord *shellCreateWord(const char *text, bool single_quoted, bool double_quoted,
                           bool has_param_expansion, int line, int column);
void shellWordAddExpansion(ShellWord *word, const char *name);
void shellFreeWord(ShellWord *word);

ShellRedirection *shellCreateRedirection(ShellRedirectionType type, const char *io_number,
                                         ShellWord *target, int line, int column);
void shellFreeRedirection(ShellRedirection *redir);

ShellPipeline *shellCreatePipeline(void);
void shellPipelineAddCommand(ShellPipeline *pipeline, ShellCommand *command);
void shellFreePipeline(ShellPipeline *pipeline);

ShellLogicalList *shellCreateLogicalList(void);
void shellLogicalListAdd(ShellLogicalList *list, ShellPipeline *pipeline, ShellLogicalConnector connector);
void shellFreeLogicalList(ShellLogicalList *list);

ShellLoop *shellCreateLoop(bool is_until, ShellPipeline *condition, ShellProgram *body);
void shellFreeLoop(ShellLoop *loop);

ShellConditional *shellCreateConditional(ShellPipeline *condition, ShellProgram *then_branch,
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
ShellCommand *shellCreatePipelineCommand(ShellPipeline *pipeline);
ShellCommand *shellCreateLogicalCommand(ShellLogicalList *logical);
ShellCommand *shellCreateSubshellCommand(ShellProgram *body);
ShellCommand *shellCreateLoopCommand(ShellLoop *loop);
ShellCommand *shellCreateConditionalCommand(ShellConditional *conditional);
ShellCommand *shellCreateCaseCommand(ShellCase *case_stmt);
void shellCommandAddWord(ShellCommand *command, ShellWord *word);
void shellCommandAddRedirection(ShellCommand *command, ShellRedirection *redir);
void shellFreeCommand(ShellCommand *command);

ShellProgram *shellCreateProgram(void);
void shellProgramAddCommand(ShellProgram *program, ShellCommand *command);
void shellFreeProgram(ShellProgram *program);

void shellDumpAstJson(FILE *out, const ShellProgram *program);

#ifdef __cplusplus
}
#endif

#endif /* SHELL_AST_H */
