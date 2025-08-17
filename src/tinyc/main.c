#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tinyc/parser.h"
#include "tinyc/codegen.h"
#include "tinyc/builtins.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "globals.h"

int gParamCount = 0;
char **gParamValues = NULL;

static void initSymbolSystemTinyC(void) {
    globalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: tinyc <source.c>\n");
        return 1;
    }
    const char *path = argv[1];
    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return 1; }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    char *src = (char*)malloc(len + 1); fread(src,1,len,f); src[len]='\0'; fclose(f);

    ParserTinyC parser; initParserTinyC(&parser, src);
    ASTNodeTinyC *prog = parseProgramTinyC(&parser);

    BytecodeChunk chunk; tinyc_compile(prog, &chunk);

    initSymbolSystemTinyC();
    tinyc_register_builtins();
    VM vm; initVM(&vm);
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, procedure_table);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    freeASTTinyC(prog);
    free(src);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (procedure_table) freeHashTable(procedure_table);
    return result == INTERPRET_OK ? 0 : 1;
}
