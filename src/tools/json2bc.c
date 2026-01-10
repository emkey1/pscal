// pscaljson2bc: read AST JSON from stdin or a file and compile to bytecode.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include "tools/ast_json_loader.h"
#include "compiler/compiler.h"
#include "compiler/bytecode.h"
#include "backend_ast/builtin.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "core/cache.h"
#include "core/utils.h"
#include "common/frontend_kind.h"

static const char* USAGE =
    "Usage: pscaljson2bc [--dump-bytecode | --dump-bytecode-only] [-o <out.bc>] [<ast.json>]\n"
    "  If no input file is provided or '-' is used, reads from stdin.\n"
    "  -h, --help                 Show this help and exit.\n";

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

static void predeclare_procedures(AST* node) {
    if (!node) return;

    // Create procedure/function symbols in the global procedure_table so that
    // calls can be resolved and implementations compiled. Store a DEEP COPY of
    // the declaration AST in sym->type_def as expected by freeProcedureTable().
    if ((node->type == AST_PROCEDURE_DECL || node->type == AST_FUNCTION_DECL) &&
        node->token && node->token->value) {
        Symbol* sym = (Symbol*)malloc(sizeof(Symbol));
        if (sym) {
            memset(sym, 0, sizeof(Symbol));
            sym->name = strdup(node->token->value);
            if (!sym->name) { free(sym); sym = NULL; }
        }
        if (sym) {
            // Normalize to lowercase for lookups
            for (char* p = sym->name; *p; ++p) *p = (char)tolower((unsigned char)*p);
            sym->type = (node->type == AST_FUNCTION_DECL) ? node->var_type : TYPE_VOID;

            // Deep copy of the declaration; compiler and cleanup expect ownership
            sym->type_def = copyAST(node);
            if (!sym->type_def) {
                free(sym->name);
                free(sym);
            } else {
                // Basic metadata; arity from parameter groups if present
                sym->arity = sym->type_def->child_count;
                sym->is_inline = sym->type_def->is_inline;
                sym->bytecode_address = -1;
                sym->locals_count = 0;
                sym->value = NULL;       // routines have no value payload
                sym->is_alias = false;
                sym->is_const = false;
                sym->is_local_var = false;
                sym->slot_index = -1;
                sym->is_defined = true;  // definition present in this translation unit

                // Ensure types are annotated on the copied decl for downstream use
                annotateTypes(sym->type_def, NULL, sym->type_def);

                hashTableInsert(procedure_table, sym);
            }
        }
    }

    if (node->left) predeclare_procedures(node->left);
    if (node->right) predeclare_procedures(node->right);
    if (node->extra) predeclare_procedures(node->extra);
    for (int i = 0; i < node->child_count; i++) {
        predeclare_procedures(node->children[i]);
    }
}

int pscaljson2bc_main(int argc, char** argv) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
#define JSON2BC_RETURN(value)           \
    do {                                \
        int __json2bc_rc = (value);     \
        frontendPopKind(previousKind);  \
        return __json2bc_rc;            \
    } while (0)
    int dump_bc = 0, dump_only = 0;
    const char* in_path = NULL;
    const char* out_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("%s", USAGE);
            JSON2BC_RETURN(EXIT_SUCCESS);
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) { dump_bc = 1; }
        else if (strcmp(argv[i], "--dump-bytecode-only") == 0) { dump_bc = 1; dump_only = 1; }
        else if ((strcmp(argv[i], "-o") == 0 || strcmp(argv[i], "--output") == 0) && i+1 < argc) { out_path = argv[++i]; }
        else if (argv[i][0] == '-') { fprintf(stderr, "%s", USAGE); JSON2BC_RETURN(EXIT_FAILURE); }
        else { in_path = argv[i]; }
    }

    FILE* in = stdin;
    if (in_path && strcmp(in_path, "-") != 0) {
        in = fopen(in_path, "rb");
        if (!in) { perror("open input"); JSON2BC_RETURN(EXIT_FAILURE); }
    }
    char* json = slurp(in);
    if (in != stdin) fclose(in);
    if (!json) { fprintf(stderr, "Failed to read input.\n"); JSON2BC_RETURN(EXIT_FAILURE); }

    AST* root = loadASTFromJSON(json);
    free(json);
    if (!root) { fprintf(stderr, "Failed to parse AST JSON.\n");
        // Best-effort: avoid leaving a stale or partial output file on failure.
        if (out_path && strcmp(out_path, "-") != 0) { unlink(out_path); }
        JSON2BC_RETURN(EXIT_FAILURE); }

    initSymbolSystemMinimal();
    registerAllBuiltins();

    // Frontends that dump AST JSON (like clike) typically represent function bodies
    // as a single COMPOUND block without a separate declarations section. Enable
    // dynamic-locals so the compiler discovers local variables declared inside
    // the body and assigns them slots before use.
    compilerEnableDynamicLocals(1);

    predeclare_procedures(root);

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
        if (out_path && strcmp(out_path, "-") != 0) { unlink(out_path); }
        JSON2BC_RETURN(EXIT_FAILURE);
    }

    if (dump_bc) {
        const char* disasm_name = in_path ? bytecodeDisplayNameForPath(in_path) : "<stdin>";
        disassembleBytecodeChunk(&chunk, disasm_name, procedure_table);
        if (dump_only) {
            freeBytecodeChunk(&chunk);
            freeAST(root);
            freeProcedureTable();
            freeTypeTableASTNodes();
            freeTypeTable();
            if (globalSymbols) freeHashTable(globalSymbols);
            if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
            JSON2BC_RETURN(EXIT_SUCCESS);
        }
    }

    // Write bytecode to output, preserving metadata so the VM can load it.
    if (out_path && strcmp(out_path, "-") != 0) {
        if (!saveBytecodeToFile(out_path, in_path ? in_path : "<stdin>", &chunk)) {
            fprintf(stderr, "Failed to write bytecode to %s\n", out_path);
            freeBytecodeChunk(&chunk);
            freeAST(root);
            freeProcedureTable();
            freeTypeTableASTNodes();
            freeTypeTable();
            if (globalSymbols) freeHashTable(globalSymbols);
            if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
            JSON2BC_RETURN(EXIT_FAILURE);
        }
    } else {
        FILE* out = stdout;
        if (chunk.count > 0 && chunk.code) {
            size_t written = fwrite(chunk.code, 1, (size_t)chunk.count, out);
            if (written != (size_t)chunk.count) {
                fprintf(stderr, "Short write: wrote %zu of %d bytes.\n", written, chunk.count);
            }
        }
    }

    freeBytecodeChunk(&chunk);
    freeAST(root);
    freeProcedureTable();
    freeTypeTableASTNodes();
    freeTypeTable();
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    JSON2BC_RETURN(EXIT_SUCCESS);
}
#undef JSON2BC_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char** argv) {
    return pscaljson2bc_main(argc, argv);
}
#endif
