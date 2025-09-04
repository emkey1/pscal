#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "ast/ast.h"
#include "compiler/bytecode.h"
#include "compiler/compiler.h"
#include "backend_ast/builtin.h"
#include "rea/parser.h"
#include "rea/ast.h"

int gParamCount = 0;
char **gParamValues = NULL;

static void initSymbolSystem(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

static const char *REA_USAGE =
    "Usage: rea <options> <source.rea> [program_parameters...]\n"
    "   Options:\n"
    "     --dump-ast-json        Dump AST to JSON and exit.\n"
    "     --dump-bytecode        Dump compiled bytecode before execution.\n"
    "     --dump-bytecode-only   Dump compiled bytecode and exit (no execution).\n";

int main(int argc, char **argv) {
    vmInitTerminalState();

    int dump_ast_json = 0;
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    int argi = 1;
    while (argc > argi && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--dump-ast-json") == 0) {
            dump_ast_json = 1;
        } else if (strcmp(argv[argi], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (strcmp(argv[argi], "--dump-bytecode-only") == 0) {
            dump_bytecode_flag = 1;
            dump_bytecode_only_flag = 1;
        } else {
            fprintf(stderr, "Unknown option: %s\n%s", argv[argi], REA_USAGE);
            return vmExitWithCleanup(EXIT_FAILURE);
        }
        argi++;
    }

    if (argc <= argi) {
        fprintf(stderr, "%s", REA_USAGE);
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    const char *path = argv[argi++];
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror("open");
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    char *src = (char *)malloc(len + 1);
    if (!src) {
        fclose(f);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    size_t bytes_read = fread(src, 1, len, f);
    fclose(f);
    if (bytes_read != (size_t)len) {
        free(src);
        fprintf(stderr, "Error reading source file '%s'\n", path);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    src[len] = '\0';

    ReaAST *ast = parseRea(src);
    if (dump_ast_json) {
        reaDumpASTJSON(ast, stdout);
        reaFreeAST(ast);
        free(src);
        return vmExitWithCleanup(EXIT_SUCCESS);
    }
    if (ast) {
        reaFreeAST(ast);
    }

    initSymbolSystem();
    registerAllBuiltins();

    AST *program = newASTNode(AST_PROGRAM, NULL);
    AST *block = newASTNode(AST_BLOCK, NULL);
    setRight(program, block);

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    bool compilation_ok = compileASTToBytecode(program, &chunk);

    InterpretResult result = INTERPRET_COMPILE_ERROR;
    if (compilation_ok) {
        finalizeBytecode(&chunk);

        if (argc > argi) {
            gParamCount = argc - argi;
            gParamValues = &argv[argi];
        }

        if (dump_bytecode_flag) {
            fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
            disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
            if (!dump_bytecode_only_flag) {
                fprintf(stderr, "\n--- executing Program with VM ---\n");
            }
        }

        if (dump_bytecode_only_flag) {
            result = INTERPRET_OK;
        } else {
            VM vm;
            initVM(&vm);
            result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
            freeVM(&vm);
        }
    }

    freeBytecodeChunk(&chunk);
    freeAST(program);
    freeProcedureTable();
    freeTypeTableASTNodes();
    freeTypeTable();

    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);

    free(src);
    return vmExitWithCleanup(result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE);
}

