#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdint.h>
#include "clike/parser.h"
#include "clike/codegen.h"
#include "clike/builtins.h"
#include "clike/semantics.h"
#include "clike/errors.h"
#include "clike/opt.h"
#include "clike/preproc.h"
#include "vm/vm.h"
#include "core/cache.h"
#include "core/utils.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "backend_ast/builtin.h"

int gParamCount = 0;
char **gParamValues = NULL;

int clike_error_count = 0;
int clike_warning_count = 0;

static void initSymbolSystemClike(void) {
    globalSymbols = createHashTable();
    constGlobalSymbols = createHashTable();
    procedure_table = createHashTable();
    current_procedure_table = procedure_table;
}

static const char *CLIKE_USAGE =
    "Usage: clike <options> <source.cl> [program_parameters...]\n"
    "   Options:\n"
    "     --dump-ast-json             Dump AST to JSON and exit.\n"
    "     --dump-bytecode             Dump compiled bytecode before execution.\n"
"     --dump-bytecode-only       Dump compiled bytecode and exit (no execution).\n";

static unsigned long hashPathLocal(const char* path) {
    uint32_t hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return (unsigned long)hash;
}

static char* buildCachePathLocal(const char* source_path) {
    const char* home = getenv("HOME");
    if (!home) return NULL;
    size_t dir_len = strlen(home) + 1 + strlen(".pscal_cache") + 1;
    char* dir = (char*)malloc(dir_len);
    if (!dir) return NULL;
    snprintf(dir, dir_len, "%s/%s", home, ".pscal_cache");
    mkdir(dir, 0777);
    unsigned long h = hashPathLocal(source_path);
    size_t path_len = dir_len + 32;
    char* full = (char*)malloc(path_len);
    if (!full) { free(dir); return NULL; }
    snprintf(full, path_len, "%s/%lu.bc", dir, h);
    free(dir);
    return full;
}

static char* resolveImportPath(const char* orig_path) {
    FILE *f = fopen(orig_path, "rb");
    if (f) { fclose(f); return strdup(orig_path); }
    const char *lib_dir = getenv("CLIKE_LIB_DIR");
    if (lib_dir && *lib_dir) {
        size_t len = strlen(lib_dir) + 1 + strlen(orig_path) + 1;
        char *path = (char*)malloc(len);
        if (path) {
            snprintf(path, len, "%s/%s", lib_dir, orig_path);
            f = fopen(path, "rb");
            if (f) { fclose(f); return path; }
            free(path);
        }
    }
    const char *default_dir = "/usr/local/pscal/clike/lib";
    size_t len = strlen(default_dir) + 1 + strlen(orig_path) + 1;
    char *path = (char*)malloc(len);
    if (!path) return NULL;
    snprintf(path, len, "%s/%s", default_dir, orig_path);
    f = fopen(path, "rb");
    if (f) { fclose(f); return path; }
    free(path);
    return NULL;
}

int main(int argc, char **argv) {
    // Keep terminal untouched for clike: no raw mode or color push
    int dump_ast_json_flag = 0;
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    const char *path = NULL;
    int clike_params_start = 0;

    if (argc == 1) {
        fprintf(stderr, "%s\n", CLIKE_USAGE);
        return EXIT_FAILURE;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-ast-json") == 0) {
            dump_ast_json_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (strcmp(argv[i], "--dump-bytecode-only") == 0) {
            dump_bytecode_flag = 1;
            dump_bytecode_only_flag = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n%s\n", argv[i], CLIKE_USAGE);
            return EXIT_FAILURE;
        } else {
            path = argv[i];
            clike_params_start = i + 1;
            break;
        }
    }

    if (!path) {
        fprintf(stderr, "Error: No source file specified.\n%s\n", CLIKE_USAGE);
        return EXIT_FAILURE;
    }

    FILE *f = fopen(path, "rb");
    if (!f) { perror("open"); return EXIT_FAILURE; }
    fseek(f, 0, SEEK_END); long len = ftell(f); rewind(f);
    char *src = (char*)malloc(len + 1);
    if (!src) { fclose(f); return EXIT_FAILURE; }
    size_t bytes_read = fread(src,1,len,f);
    if (bytes_read != (size_t)len) {
        fprintf(stderr, "Error reading source file '%s'\n", path);
        free(src);
        fclose(f);
        return EXIT_FAILURE;
    }
    src[len]='\0'; fclose(f);

    const char *defines[1];
    int define_count = 0;
#ifdef SDL
    defines[define_count++] = "SDL_ENABLED";
#endif
    char *pre_src = clikePreprocess(src, defines, define_count);

    ParserClike parser; initParserClike(&parser, pre_src ? pre_src : src);
    ASTNodeClike *prog = parseProgramClike(&parser);
    freeParserClike(&parser);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after parsing.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        return vmExitWithCleanup(EXIT_FAILURE);
    }

    if (dump_ast_json_flag) {
        fprintf(stderr, "--- Dumping AST to JSON (stdout) ---\n");
        dumpASTClikeJSON(prog, stdout);
        fprintf(stderr, "\n--- AST JSON Dump Complete (stderr print)---\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        return EXIT_SUCCESS;
    }

    if (clike_params_start < argc) {
        gParamCount = argc - clike_params_start;
        gParamValues = &argv[clike_params_start];
    }

    initSymbolSystemClike();
    clikeRegisterBuiltins();
    analyzeSemanticsClike(prog);

    if (!verifyASTClikeLinks(prog, NULL)) {
        fprintf(stderr, "AST verification failed after semantic analysis.\n");
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_FAILURE;
    }

    if (clike_warning_count > 0) {
        fprintf(stderr, "Compilation finished with %d warning(s).\n", clike_warning_count);
    }
    if (clike_error_count > 0) {
        fprintf(stderr, "Compilation halted with %d error(s).\n", clike_error_count);
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return clike_error_count > 255 ? 255 : clike_error_count;
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
        return EXIT_FAILURE;
    }

    char **dep_paths = NULL;
    if (clike_import_count > 0) {
        dep_paths = (char**)malloc(sizeof(char*) * clike_import_count);
        if (dep_paths) {
            for (int i = 0; i < clike_import_count; ++i) {
                dep_paths[i] = resolveImportPath(clike_imports[i]);
                if (!dep_paths[i]) dep_paths[i] = strdup(clike_imports[i]);
            }
        }
    }
    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    bool used_cache = loadBytecodeFromCache(path, &chunk);
    if (used_cache) {
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st) ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st) ((st).st_mtim.tv_sec)
#endif
        char* cache_path = buildCachePathLocal(path);
        struct stat cache_stat;
        if (!cache_path || stat(cache_path, &cache_stat) != 0) {
            if (cache_path) free(cache_path);
            freeBytecodeChunk(&chunk);
            initBytecodeChunk(&chunk);
            used_cache = false;
        } else {
            for (int i = 0; i < clike_import_count && used_cache; ++i) {
                struct stat dep_stat;
                if (stat(clike_imports[i], &dep_stat) != 0 ||
                    PSCAL_STAT_SEC(cache_stat) <= PSCAL_STAT_SEC(dep_stat)) {
                    freeBytecodeChunk(&chunk);
                    initBytecodeChunk(&chunk);
                    used_cache = false;
                    break;
                }
            }
            free(cache_path);
        }
#undef PSCAL_STAT_SEC
    }
    if (!used_cache) {
        clikeCompile(prog, &chunk);
        saveBytecodeToCache(path, &chunk);
        fprintf(stderr, "Compilation successful. Byte code size: %d bytes, Constants: %d\n",
                chunk.count, chunk.constants_count);
        if (dump_bytecode_flag) {
            fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
            disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
            if (!dump_bytecode_only_flag) {
                fprintf(stderr, "\n--- executing Program with VM ---\n");
            }
        }
    } else {
        fprintf(stderr, "Loaded cached byte code. Byte code size: %d bytes, Constants: %d\n",
                chunk.count, chunk.constants_count);
        if (dump_bytecode_flag) {
            disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
            if (!dump_bytecode_only_flag) {
                fprintf(stderr, "\n--- executing Program with VM (cached) ---\n");
            }
        }
    }

    if (dump_bytecode_only_flag) {
        // Cleanup and exit without executing
        freeBytecodeChunk(&chunk);
        freeASTClike(prog);
        clikeFreeStructs();
        free(src);
        if (pre_src) free(pre_src);
        if (globalSymbols) freeHashTable(globalSymbols);
        if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
        if (procedure_table) freeHashTable(procedure_table);
        return EXIT_SUCCESS;
    }

    VM vm; initVM(&vm);
    // Inline trace toggle via comment: /* trace on */ or // trace on
    if ((pre_src && strstr(pre_src, "trace on")) || (src && strstr(src, "trace on"))) {
        vm.trace_head_instructions = 16;
    }
    InterpretResult result = interpretBytecode(&vm, &chunk, globalSymbols, constGlobalSymbols, procedure_table, 0);
    freeVM(&vm);
    freeBytecodeChunk(&chunk);
    freeASTClike(prog);
    clikeFreeStructs();
    free(src);
    if (pre_src) free(pre_src);
    if (globalSymbols) freeHashTable(globalSymbols);
    if (constGlobalSymbols) freeHashTable(constGlobalSymbols);
    if (procedure_table) freeHashTable(procedure_table);
    return result == INTERPRET_OK ? EXIT_SUCCESS : EXIT_FAILURE;
}
