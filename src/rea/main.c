/*
 * MIT License
 *
 * Copyright (c) 2024 PSCAL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Note: PSCAL versions prior to 2.22 were released under the Unlicense.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <unistd.h>
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
#include "rea/semantic.h"
#include "Pascal/lexer.h"
#include "Pascal/parser.h"
#include "ext_builtins/dump.h"

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
    "     --dump-bytecode-only   Dump compiled bytecode and exit (no execution).\n"
    "     --dump-ext-builtins    List extended builtin inventory and exit.\n"
    "     --no-cache             Compile fresh (ignore cached bytecode).\n"
    "     --strict               Enable strict parser checks for top-level structure.\n"
    "     --vm-trace-head=N      Trace first N instructions in the VM (also enabled by '{trace on}' in source).\n";

static bool isUnitListFresh(List* unit_list, time_t cache_mtime) {
    if (!unit_list) return true;
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st)  ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st)  ((st).st_mtim.tv_sec)
#endif
    for (int i = 0; i < listSize(unit_list); i++) {
        char *used_unit_name = (char*)listGet(unit_list, i);
        if (!used_unit_name) continue;

        char lower_used_unit_name[MAX_SYMBOL_LENGTH];
        strncpy(lower_used_unit_name, used_unit_name, MAX_SYMBOL_LENGTH - 1);
        lower_used_unit_name[MAX_SYMBOL_LENGTH - 1] = '\0';
        for (int k = 0; lower_used_unit_name[k]; k++) {
            lower_used_unit_name[k] = tolower((unsigned char)lower_used_unit_name[k]);
        }

        char *unit_file_path = findUnitFile(lower_used_unit_name);
        if (!unit_file_path) continue;

        struct stat unit_stat;
        if (stat(unit_file_path, &unit_stat) != 0) {
            free(unit_file_path);
            return false;
        }
        if (cache_mtime <= PSCAL_STAT_SEC(unit_stat)) {
            free(unit_file_path);
            return false;
        }
        free(unit_file_path);
    }
#undef PSCAL_STAT_SEC
    return true;
}

static bool importsAreFresh(AST* node, time_t cache_mtime) {
    if (!node) return true;
    if (node->type == AST_USES_CLAUSE && node->unit_list) {
        if (!isUnitListFresh(node->unit_list, cache_mtime)) return false;
    }
    if (node->left && !importsAreFresh(node->left, cache_mtime)) return false;
    if (node->right && !importsAreFresh(node->right, cache_mtime)) return false;
    if (node->extra && !importsAreFresh(node->extra, cache_mtime)) return false;
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i] && !importsAreFresh(node->children[i], cache_mtime)) return false;
    }
    return true;
}

static void processUnitList(List* unit_list, BytecodeChunk* chunk) {
    if (!unit_list) return;
    for (int i = 0; i < listSize(unit_list); i++) {
        char *used_unit_name_str_from_list = (char*)listGet(unit_list, i);
        if (!used_unit_name_str_from_list) continue;

        char lower_used_unit_name[MAX_SYMBOL_LENGTH];
        strncpy(lower_used_unit_name, used_unit_name_str_from_list, MAX_SYMBOL_LENGTH - 1);
        lower_used_unit_name[MAX_SYMBOL_LENGTH - 1] = '\0';
        for (int k = 0; lower_used_unit_name[k]; k++) {
            lower_used_unit_name[k] = tolower((unsigned char)lower_used_unit_name[k]);
        }

        char *unit_file_path = findUnitFile(lower_used_unit_name);
        if (!unit_file_path) {
            if (!isUnitDocumented(lower_used_unit_name)) {
                fprintf(stderr, "Warning: unit '%s' not found. Skipping.\n", used_unit_name_str_from_list);
            }
            continue; // skip missing unit regardless
        }

        // Read unit file
        char* unit_source_buffer = NULL;
        FILE *unit_file = fopen(unit_file_path, "r");
        if (unit_file) {
            fseek(unit_file, 0, SEEK_END);
            long fsize = ftell(unit_file);
            rewind(unit_file);
            unit_source_buffer = malloc(fsize + 1);
            if (!unit_source_buffer) { fclose(unit_file); free(unit_file_path); EXIT_FAILURE_HANDLER(); }
            size_t bytes_read = fread(unit_source_buffer, 1, fsize, unit_file);
            if (bytes_read != (size_t)fsize) {
                fprintf(stderr, "Error reading unit file '%s'.\n", unit_file_path);
                free(unit_source_buffer);
                fclose(unit_file);
                free(unit_file_path);
                EXIT_FAILURE_HANDLER();
            }
            unit_source_buffer[fsize] = '\0';
            fclose(unit_file);
        } else {
            fprintf(stderr, "Error opening unit file '%s'.\n", unit_file_path);
            free(unit_file_path);
            EXIT_FAILURE_HANDLER();
        }
        free(unit_file_path);

        // Parse the unit using Pascal's unit parser
        Lexer nested_lexer;
        initLexer(&nested_lexer, unit_source_buffer);
        Parser nested_parser_instance;
        nested_parser_instance.lexer = &nested_lexer;
        nested_parser_instance.current_token = getNextToken(&nested_lexer);
        nested_parser_instance.current_unit_name_context = lower_used_unit_name;
        nested_parser_instance.dependency_paths = NULL;

        AST *parsed_unit_ast = unitParser(&nested_parser_instance, 1, lower_used_unit_name, chunk);

        if (nested_parser_instance.current_token) freeToken(nested_parser_instance.current_token);
        if (unit_source_buffer) free(unit_source_buffer);

        if (parsed_unit_ast) {
            // Annotate and compile implementation to assign addresses
            annotateTypes(parsed_unit_ast, NULL, parsed_unit_ast);
            compileUnitImplementation(parsed_unit_ast, chunk);
            // Link globals and alias unqualified names
            linkUnit(parsed_unit_ast, 1);
            freeAST(parsed_unit_ast);
        }
    }
}

static void walkUsesClauses(AST* node, BytecodeChunk* chunk) {
    if (!node) return;
    if (node->type == AST_USES_CLAUSE && node->unit_list) {
        processUnitList(node->unit_list, chunk);
    }
    if (node->left) walkUsesClauses(node->left, chunk);
    if (node->right) walkUsesClauses(node->right, chunk);
    if (node->extra) walkUsesClauses(node->extra, chunk);
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) walkUsesClauses(node->children[i], chunk);
    }
}

static void collectUnitListPaths(List* unit_list, List* out) {
    if (!unit_list || !out) return;
    for (int i = 0; i < listSize(unit_list); i++) {
        char *used_unit_name = (char*)listGet(unit_list, i);
        if (!used_unit_name) continue;

        char lower[MAX_SYMBOL_LENGTH];
        strncpy(lower, used_unit_name, MAX_SYMBOL_LENGTH - 1);
        lower[MAX_SYMBOL_LENGTH - 1] = '\0';
        for (int k = 0; lower[k]; k++) lower[k] = tolower((unsigned char)lower[k]);

        char *unit_file_path = findUnitFile(lower);
        if (unit_file_path) {
            listAppend(out, unit_file_path);
            free(unit_file_path);
        }
    }
}

static void collectUsesClauses(AST* node, List* out) {
    if (!node) return;
    if (node->type == AST_USES_CLAUSE && node->unit_list) {
        collectUnitListPaths(node->unit_list, out);
    }
    if (node->left) collectUsesClauses(node->left, out);
    if (node->right) collectUsesClauses(node->right, out);
    if (node->extra) collectUsesClauses(node->extra, out);
    for (int i = 0; i < node->child_count; i++) {
        if (node->children[i]) collectUsesClauses(node->children[i], out);
    }
}

int main(int argc, char **argv) {
    vmInitTerminalState();

    int dump_ast_json = 0;
    int dump_bytecode_flag = 0;
    int dump_bytecode_only_flag = 0;
    int dump_ext_builtins = 0;
    int vm_trace_head = 0;
    int no_cache = 0;
    int strict_mode = 0;
    int argi = 1;
    while (argc > argi && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--dump-ast-json") == 0) {
            dump_ast_json = 1;
        } else if (strcmp(argv[argi], "--dump-bytecode") == 0) {
            dump_bytecode_flag = 1;
        } else if (strcmp(argv[argi], "--dump-bytecode-only") == 0) {
            dump_bytecode_flag = 1;
            dump_bytecode_only_flag = 1;
        } else if (strcmp(argv[argi], "--dump-ext-builtins") == 0) {
            dump_ext_builtins = 1;
        } else if (strcmp(argv[argi], "--no-cache") == 0) {
            no_cache = 1;
        } else if (strcmp(argv[argi], "--strict") == 0) {
            strict_mode = 1;
        } else if (strncmp(argv[argi], "--vm-trace-head=", 16) == 0) {
            vm_trace_head = atoi(argv[argi] + 16);
        } else {
            fprintf(stderr, "Unknown option: %s\n%s", argv[argi], REA_USAGE);
            return vmExitWithCleanup(EXIT_FAILURE);
        }
        argi++;
    }

    if (dump_ext_builtins) {
        registerExtendedBuiltins();
        extBuiltinDumpInventory(stdout);
        return vmExitWithCleanup(EXIT_SUCCESS);
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

    // Note: Bootstrap of entrypoint is disabled; rely on source top-level or
    // future bytecode-level CALL injection.

    initSymbolSystem();
    registerAllBuiltins();
#ifdef SDL
    registerSdlGlBuiltins();
#endif
    /* C-like style cast helpers */
    registerBuiltinFunction("int", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("double", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("float", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("char", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("bool", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("byte", AST_FUNCTION_DECL, NULL);
    /* synonyms to avoid keyword collisions */
    registerBuiltinFunction("toint", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("todouble", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tofloat", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tochar", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tobool", AST_FUNCTION_DECL, NULL);
    registerBuiltinFunction("tobyte", AST_FUNCTION_DECL, NULL);

    if (strict_mode) reaSetStrictMode(1);
    AST *program = parseRea(src);
    if (!program) {
        free(src);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    reaPerformSemanticAnalysis(program);
    if (pascal_semantic_error_count > 0 && !dump_ast_json) {
        freeAST(program);
        free(src);
        return vmExitWithCleanup(EXIT_FAILURE);
    }
    if (dump_ast_json) {
        annotateTypes(program, NULL, program);
        dumpASTJSON(program, stdout);
        freeAST(program);
        free(src);
        return vmExitWithCleanup(EXIT_SUCCESS);
    }

    List *dep_files = createList();
    collectUsesClauses(program, dep_files);
    int dep_count = listSize(dep_files);
    const char **dep_array = NULL;
    if (dep_count > 0) {
        dep_array = malloc(sizeof(char*) * dep_count);
        for (int i = 0; i < dep_count; i++) dep_array[i] = listGet(dep_files, i);
    }

    BytecodeChunk chunk;
    initBytecodeChunk(&chunk);
    bool used_cache = 0;
    if (!no_cache) used_cache = loadBytecodeFromCache(path, argv[0], dep_array, dep_count, &chunk);
    if (dep_array) free(dep_array);
    if (used_cache) {
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st) ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st) ((st).st_mtim.tv_sec)
#endif
        char* cache_path = buildCachePath(path);
        struct stat cache_stat;
        if (!cache_path || stat(cache_path, &cache_stat) != 0 ||
            !importsAreFresh(program, PSCAL_STAT_SEC(cache_stat))) {
            if (cache_path) free(cache_path);
            freeBytecodeChunk(&chunk);
            initBytecodeChunk(&chunk);
            used_cache = false;
        } else {
            free(cache_path);
        }
#undef PSCAL_STAT_SEC
    }

    InterpretResult result = INTERPRET_COMPILE_ERROR;
    bool compilation_ok = true;
    if (!used_cache) {
        // Handle #import directives by loading and linking Pascal units before compiling the main program
        walkUsesClauses(program, &chunk);

        // Annotate types for the entire program prior to compilation so that
        // qualified method calls can be resolved to their class-mangled routines.
        annotateTypes(program, NULL, program);
        compilerEnableDynamicLocals(1);
        compilation_ok = compileASTToBytecode(program, &chunk);
        compilerEnableDynamicLocals(0);
        if (compilation_ok) {
            finalizeBytecode(&chunk);
            saveBytecodeToCache(path, &chunk);
            fprintf(stderr, "Compilation successful. Byte code size: %d bytes, Constants: %d\n",
                    chunk.count, chunk.constants_count);
            if (dump_bytecode_flag) {
                fprintf(stderr, "--- Compiling Main Program AST to Bytecode ---\n");
                disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
                if (dump_bytecode_only_flag) {
                    _exit(EXIT_SUCCESS);
                } else {
                    fprintf(stderr, "\n--- executing Program with VM ---\n");
                }
            }
        } else {
            fprintf(stderr, "Compilation failed with errors.\n");
        }
    } else {
        fprintf(stderr, "Loaded cached byte code. Byte code size: %d bytes, Constants: %d\n",
                chunk.count, chunk.constants_count);
        if (dump_bytecode_flag) {
            disassembleBytecodeChunk(&chunk, path ? path : "CompiledChunk", procedure_table);
            if (dump_bytecode_only_flag) {
                _exit(EXIT_SUCCESS);
            } else {
                fprintf(stderr, "\n--- executing Program with VM (cached) ---\n");
            }
        }
    }

    if (compilation_ok) {
        if (argc > argi) {
            gParamCount = argc - argi;
            gParamValues = &argv[argi];
        }

        if (dump_bytecode_only_flag) {
            result = INTERPRET_OK;
        } else {
            VM vm;
            initVM(&vm);
            if (vm_trace_head > 0) vm.trace_head_instructions = vm_trace_head;
            // Inline trace toggle via comment directives: trace on/off inside source
            if (!vm_trace_head && src && strstr(src, "trace on")) vm.trace_head_instructions = 16;
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
