#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "clike/parser.h"
#include "clike/codegen.h"
#include "clike/builtins.h"
#include "clike/semantics.h"
#include "clike/opt.h"
#include "clike/preproc.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"
#include "common/frontend_kind.h"
int clike_error_count = 0;
int clike_warning_count = 0;

static void initSymbolSystemClike(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

int clike_repl_main(void) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_CLIKE);
#define CLIKE_REPL_RETURN(value)        \
    do {                                \
        int __clike_rc = (value);       \
        frontendPopKind(previousKind);  \
        return __clike_rc;              \
    } while (0)
    // Do not change terminal state for clike REPL; rely on normal TTY buffering

    struct termios raw_termios;
    tcgetattr(STDIN_FILENO, &raw_termios);
    struct termios canon_termios = raw_termios;
    canon_termios.c_lflag |= (ICANON | ECHO);
    canon_termios.c_cc[VMIN] = 1;
    canon_termios.c_cc[VTIME] = 0;

    char line[1024];
    while (1) {
        tcsetattr(STDIN_FILENO, TCSANOW, &canon_termios);
        printf("clike> ");
        fflush(stdout);
        if (!fgets(line, sizeof(line), stdin)) break;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_termios);
        if (strncmp(line, ":quit", 5) == 0) break;

        const char *prefix = "int main() {\n";
        const char *suffix = "\nreturn 0;\n}\n";
        size_t len = strlen(prefix) + strlen(line) + strlen(suffix) + 1;
        char *src = (char*)malloc(len);
        snprintf(src, len, "%s%s%s", prefix, line, suffix);

        const char *defines[1];
        int define_count = 0;
#ifdef SDL
        defines[define_count++] = "SDL_ENABLED";
#endif
        char *pre_src = clikePreprocess(src, NULL, defines, define_count);

        ParserClike parser; initParserClike(&parser, pre_src ? pre_src : src);
        ASTNodeClike *prog = parseProgramClike(&parser);
        freeParserClike(&parser);

        /*
         * If the user entered a simple expression (rather than a function
         * call or statement), automatically wrap it in a printf so that the
         * REPL echoes the result. This mimics the behaviour documented in the
         * tutorial where entering `2 + 2;` prints `4`.
         */
        if (prog && prog->child_count == 1) {
            ASTNodeClike *fn = prog->children[0];
            if (fn->type == TCAST_FUN_DECL && fn->right && fn->right->child_count >= 1) {
                ASTNodeClike *stmt = fn->right->children[0];
                /* The final statement is the implicit 'return 0;' appended above. */
                ASTNodeClike *last = fn->right->children[fn->right->child_count - 1];
                if (last->type == TCAST_RETURN && stmt->type == TCAST_EXPR_STMT && stmt->left && stmt->left->type != TCAST_CALL) {
                    ASTNodeClike *expr = stmt->left;

                    ClikeToken printfTok = {0};
                    printfTok.type = CLIKE_TOKEN_IDENTIFIER;
                    printfTok.lexeme = "printf";
                    printfTok.length = 6;
                    printfTok.line = expr->token.line;
                    printfTok.column = expr->token.column;

                    ClikeToken fmtTok = {0};
                    fmtTok.type = CLIKE_TOKEN_STRING;
                    fmtTok.lexeme = "%lld\n";
                    fmtTok.length = 5;
                    fmtTok.line = expr->token.line;
                    fmtTok.column = expr->token.column;

                    ASTNodeClike *call = newASTNodeClike(TCAST_CALL, printfTok);
                    ASTNodeClike *fmtNode = newASTNodeClike(TCAST_STRING, fmtTok);
                    addChildClike(call, fmtNode);
                    addChildClike(call, expr);
                    setLeftClike(stmt, call);
                }
            }
        }

        if (!verifyASTClikeLinks(prog, NULL)) {
            fprintf(stderr, "AST verification failed after parsing.\n");
            freeASTClike(prog);
            clikeFreeStructs();
            free(src);
            for (int i = 0; i < clike_import_count; ++i) free(clike_imports[i]);
            free(clike_imports); clike_imports = NULL; clike_import_count = 0;
            CLIKE_REPL_RETURN(EXIT_FAILURE);
        }
        initSymbolSystemClike();
        clikeRegisterBuiltins();
        analyzeSemanticsClike(prog, NULL);

        if (!verifyASTClikeLinks(prog, NULL)) {
            fprintf(stderr, "AST verification failed after semantic analysis.\n");
            freeASTClike(prog);
            clikeFreeStructs();
            free(src);
            if (globalSymbols) freeHashTable(globalSymbols);
            if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
            if (procedure_table) freeHashTable(procedure_table);
            for (int i = 0; i < clike_import_count; ++i) free(clike_imports[i]);
            free(clike_imports); clike_imports = NULL; clike_import_count = 0;
            CLIKE_REPL_RETURN(EXIT_FAILURE);
        }
        prog = optimizeClikeAST(prog);

        if (!verifyASTClikeLinks(prog, NULL)) {
            fprintf(stderr, "AST verification failed after optimization.\n");
            freeASTClike(prog);
            clikeFreeStructs();
            free(src);
            if (globalSymbols) freeHashTable(globalSymbols);
            if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
            if (procedure_table) freeHashTable(procedure_table);
            for (int i = 0; i < clike_import_count; ++i) free(clike_imports[i]);
            free(clike_imports); clike_imports = NULL; clike_import_count = 0;
            CLIKE_REPL_RETURN(EXIT_FAILURE);
        }
        if (clike_error_count == 0) {
            BytecodeChunk chunk; clikeCompile(prog, &chunk);
            VM vm; initVM(&vm);
            interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
            freeVM(&vm);
            freeBytecodeChunk(&chunk);
        }
        freeASTClike(prog);
        clikeFreeStructs();
        if (pre_src) free(pre_src);
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        for (int i = 0; i < clike_import_count; ++i) free(clike_imports[i]);
        free(clike_imports); clike_imports = NULL; clike_import_count = 0;
        clike_error_count = 0;
        clike_warning_count = 0;
    }
    CLIKE_REPL_RETURN(EXIT_SUCCESS);
}
#undef CLIKE_REPL_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(void) {
    return clike_repl_main();
}
#endif
