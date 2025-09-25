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

#define CACHE_ROOT ".pscal"
#define CACHE_DIR "bc_cache"
#define CACHE_MAGIC 0x50534243 /* 'PSBC' */


static unsigned long hashPath(const char* path) {
    char* abs_path = realpath(path, NULL);
    const unsigned char* p = (const unsigned char*)(abs_path ? abs_path : path);
    uint32_t hash = 2166136261u;
    for (; *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    if (abs_path) free(abs_path);
    return (unsigned long)hash;
}

char* buildCachePath(const char* source_path) {
    const char* home = getenv("HOME");
    if (!home) return NULL;
    size_t root_len = strlen(home) + 1 + strlen(CACHE_ROOT) + 1;
    char* root = (char*)malloc(root_len);
    if (!root) return NULL;
    snprintf(root, root_len, "%s/%s", home, CACHE_ROOT);
    if (mkdir(root, 0700) != 0 && errno != EEXIST) {
        free(root);
        return NULL;
    }

    if (chmod(root, 0700) != 0) {
        free(root);
        return NULL;
    }

    size_t dir_len = strlen(root) + 1 + strlen(CACHE_DIR) + 1;
    char* dir = (char*)malloc(dir_len);
    if (!dir) {
        free(root);
        return NULL;
    }
    snprintf(dir, dir_len, "%s/%s", root, CACHE_DIR);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        free(dir);
        free(root);
        return NULL;
    }
    free(root);

    unsigned long h = hashPath(source_path);
    size_t path_len = dir_len + 32;
    char* full = (char*)malloc(path_len);
    if (!full) { free(dir); return NULL; }
    snprintf(full, path_len, "%s/%lu.bc", dir, h);
    free(dir);
    return full;
}

static char* resolveExecutablePath(const char* executable) {
    if (!executable || !*executable) return NULL;

#ifdef _WIN32
    if (strchr(executable, '/') || strchr(executable, '\\')) {
        return realpath(executable, NULL);
    }
#else
    if (strchr(executable, '/')) {
        return realpath(executable, NULL);
    }
#endif

#ifdef _WIN32
    const char path_sep = ';';
    const char dir_sep = '\\';
#else
    const char path_sep = ':';
    const char dir_sep = '/';
#endif

    const char* path_env = getenv("PATH");
    if (!path_env || !*path_env) return NULL;

    size_t exe_len = strlen(executable);
    const char* segment_start = path_env;
    while (segment_start && *segment_start) {
        const char* separator = strchr(segment_start, path_sep);
        size_t segment_len = separator ? (size_t)(separator - segment_start) : strlen(segment_start);
        size_t alloc_len = (segment_len ? segment_len + 1 : 0) + exe_len + 1;
        char* candidate = (char*)malloc(alloc_len);
        if (!candidate) {
            return NULL;
        }
        if (segment_len > 0) {
            memcpy(candidate, segment_start, segment_len);
            candidate[segment_len] = dir_sep;
            memcpy(candidate + segment_len + 1, executable, exe_len + 1);
        } else {
            memcpy(candidate, executable, exe_len + 1);
        }

        char* resolved = realpath(candidate, NULL);
        free(candidate);
        if (resolved) {
            return resolved;
        }

        if (!separator) {
            break;
        }
        segment_start = separator + 1;
    }

    return NULL;
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

typedef struct {
    Symbol* symbol;
    Symbol* parent;
    char* parent_name;
} EnclosingFixup;

static Symbol* findProcedureSymbolDeep(HashTable* table, const char* name) {
    if (!table || !name) return NULL;
    Symbol* sym = hashTableLookup(table, name);
    if (sym) {
        return resolveSymbolAlias(sym);
    }
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* iter = table->buckets[i]; iter; iter = iter->next) {
            if (iter->type_def && iter->type_def->symbol_table) {
                HashTable* nested = (HashTable*)iter->type_def->symbol_table;
                Symbol* found = findProcedureSymbolDeep(nested, name);
                if (found) {
                    return resolveSymbolAlias(found);
                }
            }
        }
    }
    return NULL;
}

static HashTable* findProcedureScope(HashTable* table,
                                     const char* parent_name,
                                     Symbol** out_parent) {
    if (out_parent) {
        *out_parent = NULL;
    }
    if (!table) {
        return NULL;
    }
    if (!parent_name || parent_name[0] == '\0') {
        return table;
    }

    Symbol* parent = findProcedureSymbolDeep(table, parent_name);
    if (!parent || !parent->type_def || !parent->type_def->symbol_table) {
        return NULL;
    }

    if (out_parent) {
        *out_parent = parent;
    }
    return (HashTable*)parent->type_def->symbol_table;
}

static void countProceduresRecursive(HashTable* table, int* count) {
    if (!table || !count) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* sym = table->buckets[i]; sym; sym = sym->next) {
            if (!sym || sym->is_alias) continue;
            (*count)++;
            if (sym->type_def && sym->type_def->symbol_table) {
                HashTable* nested = (HashTable*)sym->type_def->symbol_table;
                countProceduresRecursive(nested, count);
            }
        }
    }
}

static bool writeProcedureEntriesRecursive(FILE* f, HashTable* table) {
    if (!table) return true;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        for (Symbol* sym = table->buckets[i]; sym; sym = sym->next) {
            if (!sym || sym->is_alias) continue;
            int name_len = (int)strlen(sym->name);
            if (fwrite(&name_len, sizeof(name_len), 1, f) != 1) return false;
            if (fwrite(sym->name, 1, name_len, f) != (size_t)name_len) return false;
            if (fwrite(&sym->bytecode_address, sizeof(sym->bytecode_address), 1, f) != 1) return false;
            uint16_t locals = sym->locals_count;
            if (fwrite(&locals, sizeof(locals), 1, f) != 1) return false;
            if (fwrite(&sym->upvalue_count, sizeof(sym->upvalue_count), 1, f) != 1) return false;
            if (fwrite(&sym->type, sizeof(sym->type), 1, f) != 1) return false;
            if (fwrite(&sym->arity, sizeof(sym->arity), 1, f) != 1) return false;
            Symbol* enclosing = resolveSymbolAlias(sym->enclosing);
            uint8_t has_enclosing = (enclosing && enclosing->name) ? 1 : 0;
            if (fwrite(&has_enclosing, sizeof(has_enclosing), 1, f) != 1) return false;
            if (has_enclosing) {
                int parent_len = (int)strlen(enclosing->name);
                if (fwrite(&parent_len, sizeof(parent_len), 1, f) != 1) return false;
                if (fwrite(enclosing->name, 1, parent_len, f) != (size_t)parent_len) return false;
            }
            for (int uv = 0; uv < sym->upvalue_count; uv++) {
                uint8_t idx = sym->upvalues[uv].index;
                uint8_t isLocal = sym->upvalues[uv].isLocal ? 1 : 0;
                uint8_t isRef = sym->upvalues[uv].is_ref ? 1 : 0;
                if (fwrite(&idx, sizeof(idx), 1, f) != 1) return false;
                if (fwrite(&isLocal, sizeof(isLocal), 1, f) != 1) return false;
                if (fwrite(&isRef, sizeof(isRef), 1, f) != 1) return false;
            }
            if (sym->type_def && sym->type_def->symbol_table) {
                HashTable* nested = (HashTable*)sym->type_def->symbol_table;
                if (!writeProcedureEntriesRecursive(f, nested)) return false;
            }
        }
    }
    return true;
}

static bool appendEnclosingFixup(EnclosingFixup** fixups,
                                 int* count,
                                 int* capacity,
                                 Symbol* sym,
                                 char* parent_name,
                                 Symbol* parent) {
    if (!fixups || !count || !capacity || !sym) return false;
    if (*count == *capacity) {
        int new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        EnclosingFixup* new_arr = (EnclosingFixup*)realloc(*fixups, (size_t)new_cap * sizeof(EnclosingFixup));
        if (!new_arr) {
            return false;
        }
        *fixups = new_arr;
        *capacity = new_cap;
    }
    (*fixups)[*count].symbol = sym;
    (*fixups)[*count].parent = parent;
    (*fixups)[*count].parent_name = parent_name;
    (*count)++;
    return true;
}

static bool loadProceduresFromStream(FILE* f, int proc_count, uint32_t chunk_version) {
    EnclosingFixup* fixups = NULL;
    int fixup_count = 0;
    int fixup_capacity = 0;
    bool ok = true;

    for (int i = 0; ok && i < proc_count; ++i) {
        int name_len = 0;
        if (fread(&name_len, sizeof(name_len), 1, f) != 1 || name_len < 0) {
            ok = false;
            break;
        }
        char* name = (char*)malloc((size_t)name_len + 1);
        if (!name) {
            ok = false;
            break;
        }
        if (name_len > 0 && fread(name, 1, (size_t)name_len, f) != (size_t)name_len) {
            free(name);
            ok = false;
            break;
        }
        name[name_len] = '\0';

        int addr = 0;
        uint16_t locals = 0;
        uint8_t upvals = 0;
        VarType type;
        if (fread(&addr, sizeof(addr), 1, f) != 1) {
            free(name);
            ok = false;
            break;
        }

        if (chunk_version >= 7) {
            if (fread(&locals, sizeof(locals), 1, f) != 1) {
                free(name);
                ok = false;
                break;
            }
        } else {
            uint8_t locals8 = 0;
            if (fread(&locals8, sizeof(locals8), 1, f) != 1) {
                free(name);
                ok = false;
                break;
            }
            locals = locals8;
        }

        if (fread(&upvals, sizeof(upvals), 1, f) != 1 ||
            fread(&type, sizeof(type), 1, f) != 1) {
            free(name);
            ok = false;
            break;
        }

        uint8_t arity = 0;
        if (fread(&arity, sizeof(arity), 1, f) != 1) {
            free(name);
            ok = false;
            break;
        }

        uint8_t has_enclosing = 0;
        if (fread(&has_enclosing, sizeof(has_enclosing), 1, f) != 1) {
            free(name);
            ok = false;
            break;
        }

        char* enclosing_name = NULL;
        if (has_enclosing) {
            int parent_len = 0;
            if (fread(&parent_len, sizeof(parent_len), 1, f) != 1 || parent_len < 0) {
                free(name);
                ok = false;
                break;
            }
            enclosing_name = (char*)malloc((size_t)parent_len + 1);
            if (!enclosing_name) {
                free(name);
                ok = false;
                break;
            }
            if (parent_len > 0 && fread(enclosing_name, 1, (size_t)parent_len, f) != (size_t)parent_len) {
                free(name);
                free(enclosing_name);
                ok = false;
                break;
            }
            enclosing_name[parent_len] = '\0';
        }

        Symbol* parent_sym = NULL;
        HashTable* scope_table = procedure_table;
        if (has_enclosing) {
            scope_table = findProcedureScope(procedure_table, enclosing_name, &parent_sym);
            if (!scope_table || !parent_sym) {
                free(name);
                if (enclosing_name) free(enclosing_name);
                ok = false;
                break;
            }
        }

        Symbol* sym = NULL;
        if (scope_table) {
            sym = hashTableLookup(scope_table, name);
            if (sym) {
                sym = resolveSymbolAlias(sym);
            }
        }
        if (!sym) {
            sym = (Symbol*)calloc(1, sizeof(Symbol));
            if (!sym) {
                free(name);
                if (enclosing_name) free(enclosing_name);
                ok = false;
                break;
            }
            sym->name = strdup(name);
            if (!sym->name) {
                free(sym);
                free(name);
                if (enclosing_name) free(enclosing_name);
                ok = false;
                break;
            }
            toLowerString(sym->name);
            sym->value = NULL;
            sym->is_alias = false;
            sym->is_local_var = false;
            sym->is_const = false;
            sym->is_inline = false;
            sym->type_def = NULL;
            sym->next = NULL;
            sym->real_symbol = NULL;
            sym->enclosing = NULL;
            hashTableInsert(scope_table ? scope_table : procedure_table, sym);
        }

        sym->bytecode_address = addr;
        sym->locals_count = locals;
        sym->upvalue_count = upvals;
        sym->type = type;
        sym->arity = arity;
        sym->is_defined = true;
        sym->enclosing = NULL;

        bool upvalues_ok = true;
        for (int uv = 0; uv < upvals; uv++) {
            uint8_t idx = 0, isLocal = 0, isRef = 0;
            if (fread(&idx, sizeof(idx), 1, f) != 1 ||
                fread(&isLocal, sizeof(isLocal), 1, f) != 1 ||
                fread(&isRef, sizeof(isRef), 1, f) != 1) {
                upvalues_ok = false;
                break;
            }
            sym->upvalues[uv].index = idx;
            sym->upvalues[uv].isLocal = (bool)isLocal;
            sym->upvalues[uv].is_ref = (bool)isRef;
        }

        if (!upvalues_ok) {
            if (enclosing_name) free(enclosing_name);
            free(name);
            ok = false;
            break;
        }

        if (has_enclosing) {
            if (!appendEnclosingFixup(&fixups, &fixup_count, &fixup_capacity, sym, enclosing_name, parent_sym)) {
                free(enclosing_name);
                free(name);
                ok = false;
                break;
            }
        } else if (enclosing_name) {
            free(enclosing_name);
        }

        free(name);
    }

    if (ok) {
        for (int i = 0; i < fixup_count; ++i) {
            Symbol* parent = fixups[i].parent;
            if (!parent && fixups[i].parent_name) {
                parent = findProcedureSymbolDeep(procedure_table, fixups[i].parent_name);
                if (parent && parent->is_alias && parent->real_symbol) {
                    parent = parent->real_symbol;
                }
            }
            fixups[i].symbol->enclosing = parent;
            free(fixups[i].parent_name);
        }
    } else {
        for (int i = 0; i < fixup_count; ++i) {
            free(fixups[i].parent_name);
        }
    }

    free(fixups);
    return ok;
}

bool loadBytecodeFromCache(const char* source_path,
                           const char* frontend_path,
                           const char** dependencies,
                           int dep_count,
                           BytecodeChunk* chunk) {
    char* cache_path = buildCachePath(source_path);
    if (!cache_path) return false;
    if (chunk && chunk->count > 0) {
        free(cache_path);
        return false;
    }
    char* resolved_frontend = NULL;
    const char* frontend_for_cache = NULL;
    if (frontend_path && frontend_path[0]) {
        struct stat frontend_stat;
        if (stat(frontend_path, &frontend_stat) == 0) {
            frontend_for_cache = frontend_path;
        } else {
            resolved_frontend = resolveExecutablePath(frontend_path);
            if (resolved_frontend) {
                frontend_for_cache = resolved_frontend;
            }
        }
    }
#define CLEANUP_AND_RETURN_FALSE() \
    do { \
        if (resolved_frontend) { free(resolved_frontend); resolved_frontend = NULL; } \
        free(cache_path); \
        return false; \
    } while (0)
    bool ok = false;
    int const_count = 0;
    int read_consts = 0;

    if (!isCacheFresh(cache_path, source_path) ||
        (frontend_for_cache && !isCacheFresh(cache_path, frontend_for_cache))) {
        unlink(cache_path);
        CLEANUP_AND_RETURN_FALSE();
    }
    for (int i = 0; dependencies && i < dep_count; ++i) {
        if (!isCacheFresh(cache_path, dependencies[i])) {
            unlink(cache_path);
            CLEANUP_AND_RETURN_FALSE();
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
                        CLEANUP_AND_RETURN_FALSE();
                    } else {
                        fprintf(stderr,
                                "Warning: cached bytecode targets VM version %u but running version is %u\\n",
                                ver, vm_ver);
                    }
                }
                chunk->version = ver;
                if (!verifySourcePath(f, source_path)) {
                    fclose(f);
                    CLEANUP_AND_RETURN_FALSE();
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
                                int stored_proc_count = 0;
                                if (fread(&stored_proc_count, sizeof(stored_proc_count), 1, f) == 1) {
                                    if (stored_proc_count < 0) {
                                        int proc_count = -stored_proc_count - 1;
                                        if (!loadProceduresFromStream(f, proc_count, ver)) {
                                            ok = false;
                                        }
                                    } else {
                                        ok = false;
                                        unlink(cache_path);
                                    }
                                } else {
                                    ok = false;
                                }
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
                        }
                    }
                }
            }
            fclose(f);
        }
    if (resolved_frontend) free(resolved_frontend);
    free(cache_path);
#undef CLEANUP_AND_RETURN_FALSE
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
                            int stored_proc_count = 0;
                            if (fread(&stored_proc_count, sizeof(stored_proc_count), 1, f) == 1) {
                                if (stored_proc_count < 0) {
                                    int proc_count = -stored_proc_count - 1;
                                    if (!loadProceduresFromStream(f, proc_count, ver)) {
                                        ok = false;
                                    }
                                } else {
                                    ok = false;
                                }
                            } else {
                                ok = false;
                            }
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

static bool serializeBytecodeChunk(FILE* f, const char* source_path, const BytecodeChunk* chunk) {
    if (!f) return false;
    uint32_t magic = CACHE_MAGIC, ver = chunk->version;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    if (!writeSourcePath(f, source_path)) {
        return false;
    }
    fwrite(&chunk->count, sizeof(chunk->count), 1, f);
    fwrite(&chunk->constants_count, sizeof(chunk->constants_count), 1, f);
    fwrite(chunk->code, 1, chunk->count, f);
    fwrite(chunk->lines, sizeof(int), chunk->count, f);
    for (int i = 0; i < chunk->constants_count; ++i) {
        if (!writeValue(f, &chunk->constants[i])) { return false; }
    }
    int proc_count = 0;
    countProceduresRecursive(procedure_table, &proc_count);
    int stored_proc_count = -(proc_count + 1);
    fwrite(&stored_proc_count, sizeof(stored_proc_count), 1, f);
    if (!writeProcedureEntriesRecursive(f, procedure_table)) {
        return false;
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
    return true;
}

void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk) {
    char* cache_path = buildCachePath(source_path);
    if (!cache_path) return;
    FILE* f = fopen(cache_path, "wb");
    if (!f) { free(cache_path); return; }
    serializeBytecodeChunk(f, source_path, chunk);
    fclose(f);
    free(cache_path);
}

bool saveBytecodeToFile(const char* file_path, const char* source_path, const BytecodeChunk* chunk) {
    FILE* f = fopen(file_path, "wb");
    if (!f) return false;
    bool ok = serializeBytecodeChunk(f, source_path, chunk);
    fclose(f);
    return ok;
}
