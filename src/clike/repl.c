#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "clike/parser.h"
#include "clike/codegen.h"
#include "clike/builtins.h"
#include "clike/semantics.h"
#include "clike/opt.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "globals.h"

int gParamCount = 0;
char **gParamValues = NULL;
int clike_error_count = 0;
int clike_warning_count = 0;

static void initSymbolSystemClike(void) {
    globalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

int main(void) {
    char line[1024];
    while (1) {
        printf("clike> ");
        if (!fgets(line, sizeof(line), stdin)) break;
        if (strncmp(line, ":quit", 5) == 0) break;

        const char *prefix = "int main() {\n";
        const char *suffix = "\n}\n";
        size_t len = strlen(prefix) + strlen(line) + strlen(suffix) + 1;
        char *src = (char*)malloc(len);
        snprintf(src, len, "%s%s%s", prefix, line, suffix);

        ParserClike parser; initParserClike(&parser, src);
        ASTNodeClike *prog = parseProgramClike(&parser);
        initSymbolSystemClike();
        clike_register_builtins();
        analyzeSemanticsClike(prog);
        prog = optimizeClikeAST(prog);
        if (clike_error_count == 0) {
            BytecodeChunk chunk; clike_compile(prog, &chunk);
            VM vm; initVM(&vm);
            interpretBytecode(&vm, &chunk, globalSymbols, procedure_table);
            freeVM(&vm);
            freeBytecodeChunk(&chunk);
        }
        freeASTClike(prog);
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        clike_error_count = 0;
        clike_warning_count = 0;
    }
    return 0;
}
