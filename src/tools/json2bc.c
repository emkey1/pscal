// pscaljson2bc: read AST JSON from stdin or a file and compile to bytecode.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tools/ast_json_loader.h"
#include "compiler/compiler.h"
#include "compiler/bytecode.h"
#include "backend_ast/builtin.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "core/utils.h"

int gParamCount = 0;
char **gParamValues = NULL;

static const char* USAGE =
    "Usage: pscaljson2bc [--dump-bytecode | --dump-bytecode-only] [-o <out.bc>] [<ast.json>]\n"
    "  If no input file is provided or '-' is used, reads from stdin.\n";

static char* slurp(FILE* f) {
    if (!f) return NULL;
    size_t cap = 4096, len = 0;
    char* buf = (char*)malloc(cap);
    if (!buf) return NULL;
    while (!feof(f)) {
        if (len + 4096 > cap) { cap *= 2; buf = (char*)realloc(buf, cap); if (!buf) return NULL; }
        size_t n = fread(buf + len, 1, 4096, f);
        len += n;
    }
    if (len + 1 > cap) { cap += 1; buf = (char*)realloc(buf, cap); if (!buf) return NULL; }
    buf[len] = '\0';
    return buf;
}

static void initSymbolSystemMinimal(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

int main(int argc, char** argv) {
    int dump_bc = 0, dump_only = 0;
    const char* in_path = NULL;
    const char* out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-bytecode") == 0) { dump_bc = 1; }
        else if (strcmp(argv[i], "--dump-bytecode-only") == 0) { dump_bc = 1; dump_only = 1; }
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i+1 < argc) { out_path = argv[++i]; }
        else if (argv[i][0] == '-') { fprintf(stderr, "%s", USAGE); return EXIT_FAILURE; }
        else { in_path = argv[i]; }
    }

    FILE* in = stdin;
    if (in_path && strcmp(in_path, "-") != 0) {
        in = fopen(in_path, "rb");
        if (!in) { perror("open input"); return EXIT_FAILURE; }
    }
    char* json = slurp(in);
    if (in != stdin) fclose(in);
    if (!json) { fprintf(stderr, "Failed to read input.\n"); return EXIT_FAILURE; }

    AST* root = loadASTFromJSON(json);
    free(json);
    if (!root) { fprintf(stderr, "Failed to parse AST JSON.\n"); return EXIT_FAILURE; }

    initSymbolSystemMinimal();
    registerAllBuiltins();

    BytecodeChunk chunk; initBytecodeChunk(&chunk);
    bool ok = compileASTToBytecode(root, &chunk);
    if (ok) finalizeBytecode(&chunk);
    if (!ok) {
        freeBytecodeChunk(&chunk);
        freeAST(root);
        freeProcedureTable();
        freeTypeTableASTNodes();
        freeTypeTable();
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        return EXIT_FAILURE;
    }

    if (dump_bc) {
        disassembleBytecodeChunk(&chunk, in_path ? in_path : "<stdin>", procedure_table);
        if (dump_only) {
            freeBytecodeChunk(&chunk);
            freeAST(root);
            freeProcedureTable();
            freeTypeTableASTNodes();
            freeTypeTable();
            if (globalSymbols) freeHashTable(globalSymbols);
            if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
            return EXIT_SUCCESS;
        }
    }

    // Write raw bytecode to output
    FILE* out = stdout;
    if (out_path && strcmp(out_path, "-") != 0) {
        out = fopen(out_path, "wb");
        if (!out) { perror("open output"); freeBytecodeChunk(&chunk); freeAST(root); return EXIT_FAILURE; }
    }
    if (chunk.count > 0 && chunk.code) {
        size_t written = fwrite(chunk.code, 1, (size_t)chunk.count, out);
        if (written != (size_t)chunk.count) {
            fprintf(stderr, "Short write: wrote %zu of %d bytes.\n", written, chunk.count);
        }
    }
    if (out != stdout) fclose(out);

    freeBytecodeChunk(&chunk);
    freeAST(root);
    freeProcedureTable();
    freeTypeTableASTNodes();
    freeTypeTable();
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    return EXIT_SUCCESS;
}
