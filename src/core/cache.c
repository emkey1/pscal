#include "core/cache.h"
#include "core/utils.h" // for Value constructors
#include "Pascal/globals.h"
#include "core/version.h"
#include "symbol/symbol.h"
#include "Pascal/parser.h"
#include "ast/ast.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>

#define CACHE_DIR ".pscal_cache"
#define CACHE_MAGIC 0x50534243 /* 'PSBC' */


static unsigned long hashPath(const char* path) {
    uint32_t hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return (unsigned long)hash;
}

static char* buildCachePath(const char* source_path) {
    const char* home = getenv("HOME");
    if (!home) return NULL;
    size_t dir_len = strlen(home) + 1 + strlen(CACHE_DIR) + 1;
    char* dir = (char*)malloc(dir_len);
    if (!dir) return NULL;
    snprintf(dir, dir_len, "%s/%s", home, CACHE_DIR);
    mkdir(dir, 0777); // ensure directory exists

    char* abs_path = realpath(source_path, NULL);
    const char* hash_src = abs_path ? abs_path : source_path;
    unsigned long h = hashPath(hash_src);
    if (abs_path) free(abs_path);
    size_t path_len = dir_len + 32;
    char* full = (char*)malloc(path_len);
    if (!full) { free(dir); return NULL; }
    snprintf(full, path_len, "%s/%lu.bc", dir, h);
    free(dir);
    return full;
}

static bool writeSourcePath(FILE* f, const char* source_path) {
    char* abs = realpath(source_path, NULL);
    const char* src = abs ? abs : source_path;
    int len = (int)strlen(src);
    int stored = -len; /* negative signals presence of path */
    if (fwrite(&stored, sizeof(stored), 1, f) != 1) {
        if (abs) free(abs);
        return false;
    }
    bool ok = fwrite(src, 1, len, f) == (size_t)len;
    if (abs) free(abs);
    return ok;
}

static bool verifySourcePath(FILE* f, const char* source_path) {
    long pos = ftell(f);
    int stored = 0;
    if (fread(&stored, sizeof(stored), 1, f) != 1) return false;
    if (stored >= 0) {
        /* old cache without embedded path */
        fseek(f, pos, SEEK_SET);
        return true;
    }
    int len = -stored;
    char* buf = (char*)malloc(len + 1);
    if (!buf) return false;
    if (fread(buf, 1, len, f) != (size_t)len) { free(buf); return false; }
    buf[len] = '\0';
    char* abs = realpath(source_path, NULL);
    const char* src = abs ? abs : source_path;
    bool match = (strcmp(buf, src) == 0);
    if (abs) free(abs);
    free(buf);
    return match;
}

static void skipSourcePath(FILE* f) {
    long pos = ftell(f);
    int stored = 0;
    if (fread(&stored, sizeof(stored), 1, f) != 1) {
        fseek(f, pos, SEEK_SET);
        return;
    }
    if (stored >= 0) {
        fseek(f, pos, SEEK_SET);
        return;
    }
    int len = -stored;
    fseek(f, len, SEEK_CUR);
}

static bool isCacheFresh(const char* cache_path, const char* source_path) {
    struct stat src_stat, cache_stat;
    if (stat(source_path, &src_stat) != 0) return false;
    if (stat(cache_path, &cache_stat) != 0) return false;
#if defined(__APPLE__)
#define PSCAL_STAT_SEC(st)  ((st).st_mtimespec.tv_sec)
#else
#define PSCAL_STAT_SEC(st)  ((st).st_mtim.tv_sec)
#endif
    /*
     * Some filesystems only provide one-second timestamp resolution. To avoid
     * using stale bytecode, require the cache entry to be strictly newer than
     * the source file in whole seconds.
     */
    if (PSCAL_STAT_SEC(cache_stat) <= PSCAL_STAT_SEC(src_stat)) {
        return false;
    }
    return true;
#undef PSCAL_STAT_SEC
}

// ----- AST serialization helpers -----
static bool writeToken(FILE* f, const Token* tok) {
    int has = (tok != NULL);
    fwrite(&has, sizeof(has), 1, f);
    if (!has) return true;
    fwrite(&tok->type, sizeof(tok->type), 1, f);
    int len = tok && tok->value ? (int)tok->length : 0;
    fwrite(&len, sizeof(len), 1, f);
    if (len > 0) fwrite(tok->value, 1, len, f);
    return true;
}

static Token* readToken(FILE* f) {
    int has = 0;
    if (fread(&has, sizeof(has), 1, f) != 1) return NULL;
    if (!has) return NULL;
    TokenType type;
    if (fread(&type, sizeof(type), 1, f) != 1) return NULL;
    int len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return NULL;
    char* buf = (char*)malloc(len + 1);
    if (!buf) return NULL;
    if (len > 0 && fread(buf, 1, len, f) != (size_t)len) { free(buf); return NULL; }
    buf[len] = '\0';
    Token* tok = (Token*)malloc(sizeof(Token));
    if (!tok) { free(buf); return NULL; }
    tok->type = type;
    tok->value = buf;
    tok->length = (size_t)len;
    tok->line = 0;
    tok->column = 0;
    return tok;
}

static bool writeAst(FILE* f, const AST* node) {
    int has = (node != NULL);
    fwrite(&has, sizeof(has), 1, f);
    if (!has) return true;
    fwrite(&node->type, sizeof(node->type), 1, f);
    fwrite(&node->var_type, sizeof(node->var_type), 1, f);
    writeToken(f, node->token);
    fwrite(&node->i_val, sizeof(node->i_val), 1, f);
    writeAst(f, node->left);
    writeAst(f, node->right);
    writeAst(f, node->extra);
    fwrite(&node->child_count, sizeof(node->child_count), 1, f);
    for (int i = 0; i < node->child_count; i++) {
        writeAst(f, node->children[i]);
    }
    return true;
}

static AST* readAst(FILE* f) {
    int has = 0;
    if (fread(&has, sizeof(has), 1, f) != 1) return NULL;
    if (!has) return NULL;
    ASTNodeType t;
    VarType vt;
    if (fread(&t, sizeof(t), 1, f) != 1) return NULL;
    if (fread(&vt, sizeof(vt), 1, f) != 1) return NULL;
    Token* tok = readToken(f);
    int i_val = 0;
    if (fread(&i_val, sizeof(i_val), 1, f) != 1) { if (tok) freeToken(tok); return NULL; }
    AST* node = newASTNode(t, tok);
    if (node) {
        setTypeAST(node, vt);
        node->i_val = i_val;
        node->left = readAst(f); if (node->left) node->left->parent = node;
        node->right = readAst(f); if (node->right) node->right->parent = node;
        node->extra = readAst(f); if (node->extra) node->extra->parent = node;
        int child_count = 0;
        if (fread(&child_count, sizeof(child_count), 1, f) != 1) { freeAST(node); return NULL; }
        if (child_count > 0) {
            node->children = (AST**)malloc(sizeof(AST*) * child_count);
            if (!node->children) { freeAST(node); return NULL; }
            node->child_count = child_count;
            node->child_capacity = child_count;
            for (int i = 0; i < child_count; i++) {
                node->children[i] = readAst(f);
                if (node->children[i]) node->children[i]->parent = node;
            }
        }
    }
    return node;
}


static bool writeValue(FILE* f, const Value* v) {
    fwrite(&v->type, sizeof(v->type), 1, f);
    switch (v->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            fwrite(&v->i_val, sizeof(v->i_val), 1, f); break;
        case TYPE_FLOAT:
            fwrite(&v->real.f32_val, sizeof(v->real.f32_val), 1, f); break;
        case TYPE_REAL:
            fwrite(&v->real.d_val, sizeof(v->real.d_val), 1, f); break;
        case TYPE_LONG_DOUBLE:
            fwrite(&v->real.r_val, sizeof(v->real.r_val), 1, f); break;
        case TYPE_CHAR:
            fwrite(&v->c_val, sizeof(v->c_val), 1, f); break;
        case TYPE_STRING: {
            int len = v->s_val ? (int)strlen(v->s_val) : -1;
            fwrite(&len, sizeof(len), 1, f);
            if (len > 0) fwrite(v->s_val, 1, len, f);
            break;
        }
        case TYPE_NIL:
            break;
        case TYPE_ENUM: {
            int len = v->enum_val.enum_name ? (int)strlen(v->enum_val.enum_name) : 0;
            fwrite(&len, sizeof(len), 1, f);
            if (len > 0) fwrite(v->enum_val.enum_name, 1, len, f);
            fwrite(&v->enum_val.ordinal, sizeof(v->enum_val.ordinal), 1, f);
            break;
        }
        case TYPE_SET: {
            int sz = v->set_val.set_size;
            fwrite(&sz, sizeof(sz), 1, f);
            if (sz > 0 && v->set_val.set_values) {
                fwrite(v->set_val.set_values, sizeof(long long), sz, f);
            }
            break;
        }
        case TYPE_ARRAY: {
            int dims = v->dimensions;
            fwrite(&dims, sizeof(dims), 1, f);
            fwrite(&v->element_type, sizeof(v->element_type), 1, f);
            for (int i = 0; i < dims; i++) {
                int lb = v->lower_bounds ? v->lower_bounds[i] : 0;
                int ub = v->upper_bounds ? v->upper_bounds[i] : -1;
                fwrite(&lb, sizeof(lb), 1, f);
                fwrite(&ub, sizeof(ub), 1, f);
            }
            int total = 1;
            if (!v->lower_bounds || !v->upper_bounds) {
                total = 0;
            } else {
                for (int i = 0; i < dims; i++) {
                    int span = (v->upper_bounds[i] - v->lower_bounds[i] + 1);
                    if (span <= 0) { total = 0; break; }
                    total *= span;
                }
            }
            for (int i = 0; i < total; i++) {
                if (!writeValue(f, &v->array_val[i])) return false;
            }
            break;
        }
        default:
            return false;
    }
    return true;
}

static bool readValue(FILE* f, Value* out) {
    if (fread(&out->type, sizeof(out->type), 1, f) != 1) return false;
    switch (out->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            if (fread(&out->i_val, sizeof(out->i_val), 1, f) != 1) return false;
            break;
        case TYPE_FLOAT: {
            float tmp;
            if (fread(&tmp, sizeof(tmp), 1, f) != 1) return false;
            SET_REAL_VALUE(out, tmp);
            break; }
        case TYPE_REAL: {
            double tmp;
            if (fread(&tmp, sizeof(tmp), 1, f) != 1) return false;
            SET_REAL_VALUE(out, tmp);
            break; }
        case TYPE_LONG_DOUBLE: {
            long double tmp;
            if (fread(&tmp, sizeof(tmp), 1, f) != 1) return false;
            SET_REAL_VALUE(out, tmp);
            break; }
        case TYPE_CHAR:
            if (fread(&out->c_val, sizeof(out->c_val), 1, f) != 1) return false;
            break;
        case TYPE_STRING: {
            int len = 0;
            if (fread(&len, sizeof(len), 1, f) != 1) return false;
            if (len >= 0) {
                out->s_val = (char*)malloc(len + 1);
                if (!out->s_val) return false;
                if (len > 0 && fread(out->s_val, 1, len, f) != (size_t)len) return false;
                out->s_val[len] = '\0';
            } else {
                out->s_val = NULL;
            }
            out->max_length = -1;
            break;
        }
        case TYPE_NIL:
            break;
        case TYPE_ENUM: {
            int len = 0;
            if (fread(&len, sizeof(len), 1, f) != 1) return false;
            if (len > 0) {
                out->enum_val.enum_name = (char*)malloc(len + 1);
                if (!out->enum_val.enum_name) return false;
                if (fread(out->enum_val.enum_name, 1, len, f) != (size_t)len) return false;
                out->enum_val.enum_name[len] = '\0';
            } else {
                out->enum_val.enum_name = NULL;
            }
            if (fread(&out->enum_val.ordinal, sizeof(out->enum_val.ordinal), 1, f) != 1) return false;
            break;
        }
        case TYPE_SET: {
            int sz = 0;
            if (fread(&sz, sizeof(sz), 1, f) != 1) return false;
            out->set_val.set_size = sz;
            if (sz > 0) {
                out->set_val.set_values = (long long*)malloc(sizeof(long long) * sz);
                if (!out->set_val.set_values) return false;
                if (fread(out->set_val.set_values, sizeof(long long), sz, f) != (size_t)sz) return false;
            } else {
                out->set_val.set_values = NULL;
            }
            break;
        }
        case TYPE_ARRAY: {
            int dims = 0;
            if (fread(&dims, sizeof(dims), 1, f) != 1) return false;
            out->dimensions = dims;
            if (fread(&out->element_type, sizeof(out->element_type), 1, f) != 1) return false;
            if (dims > 0) {
                out->lower_bounds = (int*)malloc(sizeof(int) * dims);
                out->upper_bounds = (int*)malloc(sizeof(int) * dims);
                if (!out->lower_bounds || !out->upper_bounds) return false;
                for (int i = 0; i < dims; i++) {
                    if (fread(&out->lower_bounds[i], sizeof(int), 1, f) != 1) return false;
                    if (fread(&out->upper_bounds[i], sizeof(int), 1, f) != 1) return false;
                }
                out->lower_bound = out->lower_bounds[0];
                out->upper_bound = out->upper_bounds[0];
            } else {
                out->lower_bounds = out->upper_bounds = NULL;
                out->lower_bound = out->upper_bound = 0;
            }
            int total = 1;
            for (int i = 0; i < dims; i++) {
                int span = (out->upper_bounds[i] - out->lower_bounds[i] + 1);
                if (span <= 0) { total = 0; break; }
                total *= span;
            }
            out->array_val = NULL;
            if (total > 0) {
                out->array_val = (Value*)calloc((size_t)total, sizeof(Value));
                if (!out->array_val) return false;
                for (int i = 0; i < total; i++) {
                    if (!readValue(f, &out->array_val[i])) return false;
                }
            }
            break;
        }
        default:
            return false;
    }
    return true;
}

bool loadBytecodeFromCache(const char* source_path,
                           const char** dependencies,
                           int dep_count,
                           BytecodeChunk* chunk) {
    char* cache_path = buildCachePath(source_path);
    if (!cache_path) return false;
    if (chunk && chunk->count > 0) {
        free(cache_path);
        return false;
    }
    bool ok = false;
    int const_count = 0;
    int read_consts = 0;

    if (!isCacheFresh(cache_path, source_path)) {
        free(cache_path);
        return false;
    }
    for (int i = 0; dependencies && i < dep_count; ++i) {
        if (!isCacheFresh(cache_path, dependencies[i])) {
            free(cache_path);
            return false;
        }
    }

    FILE* f = fopen(cache_path, "rb");
    if (f) {
            uint32_t magic = 0, ver = 0;
            if (fread(&magic, sizeof(magic), 1, f) == 1 &&
                fread(&ver, sizeof(ver), 1, f) == 1 &&
                magic == CACHE_MAGIC) {
                const char* strict_env = getenv("PSCAL_STRICT_VM");
                bool strict = strict_env && strict_env[0] != '\0';
                uint32_t vm_ver = pscal_vm_version();
                if (ver > vm_ver) {
                    if (strict) {
                        fprintf(stderr,
                                "Cached bytecode requires VM version %u but current VM version is %u\\n",
                                ver, vm_ver);
                        fclose(f);
                        free(cache_path);
                        return false;
                    } else {
                        fprintf(stderr,
                                "Warning: cached bytecode targets VM version %u but running version is %u\\n",
                                ver, vm_ver);
                    }
                }
                chunk->version = ver;
                if (!verifySourcePath(f, source_path)) {
                    fclose(f);
                    free(cache_path);
                    return false;
                }
                int count = 0;
                if (fread(&count, sizeof(count), 1, f) == 1 &&
                    fread(&const_count, sizeof(const_count), 1, f) == 1) {
                    chunk->code = (uint8_t*)malloc(count);
                    chunk->lines = (int*)malloc(sizeof(int) * count);
                    chunk->constants = (Value*)calloc(const_count, sizeof(Value));
                    if (chunk->code && chunk->lines && chunk->constants) {
                        chunk->count = count; chunk->capacity = count;
                        chunk->constants_count = const_count; chunk->constants_capacity = const_count;
                        if (fread(chunk->code, 1, count, f) == (size_t)count &&
                            fread(chunk->lines, sizeof(int), count, f) == (size_t)count) {
                            ok = true;
                            for (read_consts = 0; read_consts < const_count; ++read_consts) {
                                if (!readValue(f, &chunk->constants[read_consts])) { ok = false; break; }
                            }
                            if (ok) {
                                int proc_count = 0;
                                if (fread(&proc_count, sizeof(proc_count), 1, f) == 1) {
                                    for (int i = 0; i < proc_count; ++i) {
                                        int name_len = 0;
                                        if (fread(&name_len, sizeof(name_len), 1, f) != 1) { ok = false; break; }
                                        char* name = (char*)malloc(name_len + 1);
                                        if (!name) { ok = false; break; }
                                        if (fread(name, 1, name_len, f) != (size_t)name_len) { free(name); ok = false; break; }
                                        name[name_len] = '\0';
                                        int addr = 0; uint8_t locals = 0, upvals = 0; VarType type;
                                        if (fread(&addr, sizeof(addr), 1, f) != 1 ||
                                            fread(&locals, sizeof(locals), 1, f) != 1 ||
                                            fread(&upvals, sizeof(upvals), 1, f) != 1 ||
                                            fread(&type, sizeof(type), 1, f) != 1) {
                                            free(name); ok = false; break; }
                                        // Standalone VM runs start without a populated procedure table.
                                        // Recreate entries from the bytecode when they're absent.
                                        Symbol* sym = lookupProcedure(name);
                                        if (sym) {
                                            sym->bytecode_address = addr;
                                            sym->locals_count = locals;
                                            sym->upvalue_count = upvals;
                                            sym->type = type;
                                            sym->is_defined = true;
                                        } else {
                                            sym = (Symbol*)calloc(1, sizeof(Symbol));
                                            if (!sym) {
                                                free(name);
                                                ok = false;
                                                break;
                                            }
                                            sym->name = strdup(name);
                                            if (!sym->name) {
                                                free(sym);
                                                free(name);
                                                ok = false;
                                                break;
                                            }
                                            toLowerString(sym->name);
                                            sym->bytecode_address = addr;
                                            sym->locals_count = locals;
                                            sym->upvalue_count = upvals;
                                            sym->type = type;
                                            sym->is_defined = true;
                                            sym->is_alias = false;
                                            sym->is_local_var = false;
                                            sym->is_const = false;
                                            sym->real_symbol = NULL;
                                            hashTableInsert(procedure_table, sym);
                                        }
                                        free(name);
                                    }

                                    if (ok) {
                                        int const_sym_count = 0;
                                        if (fread(&const_sym_count, sizeof(const_sym_count), 1, f) == 1) {
                                            for (int i = 0; i < const_sym_count; ++i) {
                                                int name_len = 0;
                                                if (fread(&name_len, sizeof(name_len), 1, f) != 1) { ok = false; break; }
                                                char* name = (char*)malloc(name_len + 1);
                                                if (!name) { ok = false; break; }
                                                if (fread(name, 1, name_len, f) != (size_t)name_len) {
                                                    free(name); ok = false; break; }
                                                name[name_len] = '\0';
                                                VarType type;
                                                if (fread(&type, sizeof(type), 1, f) != 1) { free(name); ok = false; break; }

                                                Value val = {0};

                                                if (!readValue(f, &val)) { free(name); ok = false; break; }
                                                insertGlobalSymbol(name, type, NULL);
                                                Symbol* sym = lookupGlobalSymbol(name);
                                                if (sym && sym->value) {
                                                    freeValue(sym->value);
                                                    *(sym->value) = val;
                                                    sym->is_const = true;
                                                } else {
                                                    freeValue(&val);
                                                }
                                                free(name);
                                            }
                                        } else {
                                            ok = false;
                                        }
                                    }
                                } else {
                                    ok = false;
                                }
                            }
                        }
                    }
                }
            }
            fclose(f);
        }
    free(cache_path);
    if (!ok) {
        for (int i = 0; i < read_consts; ++i) {
            freeValue(&chunk->constants[i]);
        }
        free(chunk->code);
        free(chunk->lines);
        free(chunk->constants);
        initBytecodeChunk(chunk);
    }
    return ok;
}

bool loadBytecodeFromFile(const char* file_path, BytecodeChunk* chunk) {
    bool ok = false;
    int const_count = 0;
    int read_consts = 0;

    FILE* f = fopen(file_path, "rb");
    if (f) {
        uint32_t magic = 0, ver = 0;
        if (fread(&magic, sizeof(magic), 1, f) == 1 &&
            fread(&ver, sizeof(ver), 1, f) == 1 &&
            magic == CACHE_MAGIC) {
            const char* strict_env = getenv("PSCAL_STRICT_VM");
            bool strict = strict_env && strict_env[0] != '\0';
            uint32_t vm_ver = pscal_vm_version();
            if (ver > vm_ver) {
                if (strict) {
                    fprintf(stderr,
                            "Bytecode requires VM version %u but this VM only supports version %u\\n",
                            ver, vm_ver);
                    fclose(f);
                    return false;
                } else {
                    fprintf(stderr,
                            "Warning: bytecode targets VM version %u but running version is %u\\n",
                            ver, vm_ver);
                }
            }
            chunk->version = ver;
            skipSourcePath(f);
            int count = 0;
            if (fread(&count, sizeof(count), 1, f) == 1 &&
                fread(&const_count, sizeof(const_count), 1, f) == 1) {
                chunk->code = (uint8_t*)malloc(count);
                chunk->lines = (int*)malloc(sizeof(int) * count);
                chunk->constants = (Value*)calloc(const_count, sizeof(Value));
                if (chunk->code && chunk->lines && chunk->constants) {
                    chunk->count = count;
                    chunk->capacity = count;
                    chunk->constants_count = const_count;
                    chunk->constants_capacity = const_count;
                    if (fread(chunk->code, 1, count, f) == (size_t)count &&
                        fread(chunk->lines, sizeof(int), count, f) == (size_t)count) {
                        ok = true;
                        for (read_consts = 0; read_consts < const_count; ++read_consts) {
                            if (!readValue(f, &chunk->constants[read_consts])) { ok = false; break; }
                        }
                        if (ok) {
                            int proc_count = 0;
                            if (fread(&proc_count, sizeof(proc_count), 1, f) == 1) {
                                for (int i = 0; i < proc_count; ++i) {
                                    int name_len = 0;
                                    if (fread(&name_len, sizeof(name_len), 1, f) != 1) { ok = false; break; }
                                    char* name = (char*)malloc(name_len + 1);
                                    if (!name) { ok = false; break; }
                                    if (fread(name, 1, name_len, f) != (size_t)name_len) { free(name); ok = false; break; }
                                    name[name_len] = '\0';
                                    int addr = 0; uint8_t locals = 0, upvals = 0; VarType type;
                                    if (fread(&addr, sizeof(addr), 1, f) != 1 ||
                                        fread(&locals, sizeof(locals), 1, f) != 1 ||
                                        fread(&upvals, sizeof(upvals), 1, f) != 1 ||
                                        fread(&type, sizeof(type), 1, f) != 1) {
                                        free(name); ok = false; break; }
                                    // When loading raw bytecode the procedure table may be empty,
                                    // so recreate any missing entries from the serialized metadata.
                                    Symbol* sym = lookupProcedure(name);
                                    if (sym) {
                                        sym->bytecode_address = addr;
                                        sym->locals_count = locals;
                                        sym->upvalue_count = upvals;
                                        sym->type = type;
                                        sym->is_defined = true;
                                    } else {
                                        sym = (Symbol*)malloc(sizeof(Symbol));
                                        if (!sym) {
                                            free(name);
                                            ok = false;
                                            break;
                                        }
                                        memset(sym, 0, sizeof(Symbol));
                                        sym->name = strdup(name);
                                        if (!sym->name) {
                                            free(sym);
                                            free(name);
                                            ok = false;
                                            break;
                                        }
                                        toLowerString(sym->name);
                                        sym->bytecode_address = addr;
                                        sym->locals_count = locals;
                                        sym->upvalue_count = upvals;
                                        sym->type = type;
                                        sym->is_defined = true;
                                        sym->is_alias = false;
                                        sym->is_local_var = false;
                                        sym->is_const = false;
                                        sym->real_symbol = NULL;
                                        hashTableInsert(procedure_table, sym);
                                    }
                                    free(name);
                                }

                                if (ok) {
                                    int const_sym_count = 0;
                                    if (fread(&const_sym_count, sizeof(const_sym_count), 1, f) == 1) {
                                        for (int i = 0; i < const_sym_count; ++i) {
                                            int name_len = 0;
                                            if (fread(&name_len, sizeof(name_len), 1, f) != 1) { ok = false; break; }
                                            char* name = (char*)malloc(name_len + 1);
                                            if (!name) { ok = false; break; }
                                            if (fread(name, 1, name_len, f) != (size_t)name_len) {
                                                free(name); ok = false; break; }
                                            name[name_len] = '\0';
                                            VarType type;
                                            if (fread(&type, sizeof(type), 1, f) != 1) { free(name); ok = false; break; }

                                            Value val = {0};
                                            if (!readValue(f, &val)) { free(name); ok = false; break; }
                                            insertGlobalSymbol(name, type, NULL);
                                            Symbol* sym = lookupGlobalSymbol(name);
                                            if (sym && sym->value) {
                                                freeValue(sym->value);
                                                *(sym->value) = val;
                                                sym->is_const = true;
                                            } else {
                                                freeValue(&val);
                                            }
                                            free(name);
                                        }
                                    } else {
                                        ok = false;
                                    }
                                }
                                if (ok) {
                                    int type_count = 0;
                                    if (fread(&type_count, sizeof(type_count), 1, f) == 1) {
                                        for (int i = 0; i < type_count; ++i) {
                                            int name_len = 0;
                                            if (fread(&name_len, sizeof(name_len), 1, f) != 1) { ok = false; break; }
                                            char* name = (char*)malloc(name_len + 1);
                                            if (!name) { ok = false; break; }
                                            if (fread(name, 1, name_len, f) != (size_t)name_len) { free(name); ok = false; break; }
                                            name[name_len] = '\0';
                                            AST* type_ast = readAst(f);
                                            if (!type_ast) { free(name); ok = false; break; }
                                            insertType(name, type_ast);
                                            freeAST(type_ast);
                                            free(name);
                                        }
                                    } else {
                                        ok = false;
                                    }
                                }
                            } else {
                                ok = false;
                            }
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    if (!ok) {
        for (int i = 0; i < read_consts; ++i) {
            freeValue(&chunk->constants[i]);
        }
        free(chunk->code);
        free(chunk->lines);
        free(chunk->constants);
        initBytecodeChunk(chunk);
    }
    return ok;
}

void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk) {
    char* cache_path = buildCachePath(source_path);
    if (!cache_path) return;
    FILE* f = fopen(cache_path, "wb");
    if (!f) { free(cache_path); return; }
    uint32_t magic = CACHE_MAGIC, ver = chunk->version;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    if (!writeSourcePath(f, source_path)) {
        fclose(f);
        free(cache_path);
        return;
    }
    fwrite(&chunk->count, sizeof(chunk->count), 1, f);
    fwrite(&chunk->constants_count, sizeof(chunk->constants_count), 1, f);
    fwrite(chunk->code, 1, chunk->count, f);
    fwrite(chunk->lines, sizeof(int), chunk->count, f);
    for (int i = 0; i < chunk->constants_count; ++i) {
        if (!writeValue(f, &chunk->constants[i])) { break; }
    }
    int proc_count = 0;
    if (procedure_table) {
        for (int i = 0; i < HASHTABLE_SIZE; i++) {
            for (Symbol* sym = procedure_table->buckets[i]; sym; sym = sym->next) {
                proc_count++;
            }
        }
    }
    fwrite(&proc_count, sizeof(proc_count), 1, f);
    if (procedure_table) {
        for (int i = 0; i < HASHTABLE_SIZE; i++) {
            for (Symbol* sym = procedure_table->buckets[i]; sym; sym = sym->next) {
                int name_len = (int)strlen(sym->name);
                fwrite(&name_len, sizeof(name_len), 1, f);
                fwrite(sym->name, 1, name_len, f);
                fwrite(&sym->bytecode_address, sizeof(sym->bytecode_address), 1, f);
                fwrite(&sym->locals_count, sizeof(sym->locals_count), 1, f);
                fwrite(&sym->upvalue_count, sizeof(sym->upvalue_count), 1, f);
                fwrite(&sym->type, sizeof(sym->type), 1, f);
            }
        }
    }

    int const_sym_count = 0;
    if (globalSymbols) {
        for (int i = 0; i < HASHTABLE_SIZE; i++) {
            for (Symbol* sym = globalSymbols->buckets[i]; sym; sym = sym->next) {
                if (sym->is_alias || !sym->is_const) continue;
                const_sym_count++;
            }
        }
    }
    fwrite(&const_sym_count, sizeof(const_sym_count), 1, f);
    if (globalSymbols) {
        for (int i = 0; i < HASHTABLE_SIZE; i++) {
            for (Symbol* sym = globalSymbols->buckets[i]; sym; sym = sym->next) {
                if (sym->is_alias || !sym->is_const) continue;
                int name_len = (int)strlen(sym->name);
                fwrite(&name_len, sizeof(name_len), 1, f);
                fwrite(sym->name, 1, name_len, f);
                fwrite(&sym->type, sizeof(sym->type), 1, f);
                if (sym->value) {
                    writeValue(f, sym->value);
                } else {
                    Value tmp = makeVoid();
                    writeValue(f, &tmp);
                }
            }
        }
    }

    int type_count = 0;
    for (TypeEntry* entry = type_table; entry; entry = entry->next) type_count++;
    fwrite(&type_count, sizeof(type_count), 1, f);
    for (TypeEntry* entry = type_table; entry; entry = entry->next) {
        int name_len = (int)strlen(entry->name);
        fwrite(&name_len, sizeof(name_len), 1, f);
        fwrite(entry->name, 1, name_len, f);
        writeAst(f, entry->typeAST);
    }
    fclose(f);
    free(cache_path);
}
