#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clike/parser.h"
#include "clike/codegen.h"
#include "clike/builtins.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "globals.h"

int gParamCount = 0;
char **gParamValues = NULL;

static void initSymbolSystemClike(void) {
    globalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

static const char *CLIKE_USAGE =
    "Usage: clike <options> <source.c> [program_parameters...]\n"
    "   Options:\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n";

int main(int argc, char **argv) {
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    const char *path = NULL;
    int clike_params_start = 0;

    if (argc == 1) {
        fprintf(stderr, "%s\n", CLIKE_USAGE);
        return 1;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s\n", argv[i], CLIKE_USAGE);
            return 1;
        } else {
            path = argv[i];
            clike_params_start = i + 1;
            break;
        }
    }

    if (!path) {
        fprintf(stderr, "Error: No source file specified.\n%s\n", CLIKE_USAGE);
        return 1;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    char *src = (char*)malloc(len + 1); fread(src,1,len,f); src[len]='\0'; fclose(f);

    ParserClike parser; initParserClike(&parser, src);
    ASTNodeClike *prog = parseProgramClike(&parser);

    if (dump_ast_json_flag) {
        fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
        dumpASTClikeJSON(prog, stdout);
        fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
        freeASTClike(prog);
        free(src);
        return 0;
    }

    if (clike_params_start < argc) {
        gParamCount = argc - clike_params_start;
        gParamValues = &argv[clike_params_start];
    }

    initSymbolSystemClike();
    clike_register_builtins();

    BytecodeChunk chunk; clike_compile(prog, &chunk);
    if (dump_bytecode_flag) {
        fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
        disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
        fprintf(stderr, "\n--- Executing Program with VM ---\n");
    }

    VM vm; initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, procedure_table);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    freeASTClike(prog);
    free(src);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (procedure_table) freeHashTable(procedure_table);
    return result == INTERPRET_OK ? 0 : 1;
}

