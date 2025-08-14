#include "core/cache.h"
#include "core/utils.h" // for Value constructors
#include "symbol/symbol.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <pwd.h>
#include <limits.h>
#include <fcntl.h>

#ifndef PROGRAM_VERSION
#define PROGRAM_VERSION "undefined.version_DEV"
#endif

#define CACHE_DIR ".pscal_cache"
#define CACHE_MAGIC 0x50534243 /* 'PSBC' */
#define CACHE_VERSION 2

static int compare_versions(const char* a, const char* b) {
    size_t len_a = strcspn(a, "_");
    size_t len_b = strcspn(b, "_");
    size_t min = len_a < len_b ? len_a : len_b;
    int cmp = strncmp(a, b, min);
    if (cmp == 0) {
        if (len_a == len_b) return 0;
        return (len_a < len_b) ? -1 : 1;
    }
    return cmp;
}

static unsigned long hash_path(const char* path) {
    uint32_t hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return (unsigned long)hash;
}

static char* ensure_cache_dir(void) {
    const char* home = getenv("HOME");
    if (!home || !*home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir) {
            home = pw->pw_dir;
        } else {
            fprintf(stderr, "Error: Could not determine home directory for cache.\n");
            exit(EXIT_FAILURE);
        }
    }

    size_t dir_len = strlen(home) + 1 + strlen(CACHE_DIR) + 1;
    char* dir = (char*)malloc(dir_len);
    if (!dir) {
        fprintf(stderr, "Error: Out of memory creating cache directory path.\n");
        exit(EXIT_FAILURE);
    }
    snprintf(dir, dir_len, "%s/%s", home, CACHE_DIR);

    struct stat st;
    if (stat(dir, &st) != 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "Error: Could not access cache directory '%s': %s\n", dir, strerror(errno));
            free(dir);
            exit(EXIT_FAILURE);
        }
        if (mkdir(dir, S_IRWXU) != 0) {
            fprintf(stderr, "Error: Could not create cache directory '%s': %s\n", dir, strerror(errno));
            free(dir);
            exit(EXIT_FAILURE);
        }
    } else {
        if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            if (chmod(dir, S_IRWXU) != 0) {
                fprintf(stderr, "Error: Could not set permissions on cache directory '%s': %s\n", dir, strerror(errno));
                free(dir);
                exit(EXIT_FAILURE);
            }
        }
    }
    return dir;
}

static char* build_cache_path(const char* source_path) {
    char* dir = ensure_cache_dir();

    char canonical[PATH_MAX];
    const char* path_for_hash = source_path;
    if (realpath(source_path, canonical)) {
        path_for_hash = canonical;
    }

    unsigned long h = hash_path(path_for_hash);
    size_t path_len = strlen(dir) + 1 + 32;
    char* full = (char*)malloc(path_len);
    if (!full) {
        free(dir);
        fprintf(stderr, "Error: Out of memory constructing cache path.\n");
        exit(EXIT_FAILURE);
    }
    snprintf(full, path_len, "%s/%lu.bc", dir, h);
    free(dir);
    return full;
}

static bool write_value(FILE* f, const Value* v) {
    fwrite(&v->type, sizeof(v->type), 1, f);
    switch (v->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            fwrite(&v->i_val, sizeof(v->i_val), 1, f); break;
        case TYPE_REAL:
            fwrite(&v->r_val, sizeof(v->r_val), 1, f); break;
        case TYPE_CHAR:
            fwrite(&v->c_val, sizeof(v->c_val), 1, f); break;
        case TYPE_STRING: {
            int len = v->s_val ? (int)strlen(v->s_val) : 0;
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
        default:
            return false;
    }
    return true;
}

static bool read_value(FILE* f, Value* out) {
    if (fread(&out->type, sizeof(out->type), 1, f) != 1) return false;
    switch (out->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
            if (fread(&out->i_val, sizeof(out->i_val), 1, f) != 1) return false;
            break;
        case TYPE_REAL:
            if (fread(&out->r_val, sizeof(out->r_val), 1, f) != 1) return false;
            break;
        case TYPE_CHAR:
            if (fread(&out->c_val, sizeof(out->c_val), 1, f) != 1) return false;
            break;
        case TYPE_STRING: {
            int len = 0;
            if (fread(&len, sizeof(len), 1, f) != 1) return false;
            if (len > 0) {
                out->s_val = (char*)malloc(len + 1);
                if (!out->s_val) return false;
                if (fread(out->s_val, 1, len, f) != (size_t)len) return false;
                out->s_val[len] = '\0';
            } else {
                out->s_val = NULL;
            }
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
        default:
            return false;
    }
    return true;
}

/* --- Procedure table serialization helpers --- */

static int count_procedures(HashTable* table) {
    if (!table) return 0;
    int count = 0;
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (s->is_alias) continue;
            count++;
            if (s->type_def && s->type_def->symbol_table) {
                count += count_procedures((HashTable*)s->type_def->symbol_table);
            }
        }
    }
    return count;
}

static void write_procedure(FILE* f, const Symbol* s) {
    int len = s->name ? (int)strlen(s->name) : 0;
    fwrite(&len, sizeof(len), 1, f);
    if (len > 0) fwrite(s->name, 1, len, f);
    uint8_t flag = s->is_defined ? 1 : 0;
    fwrite(&flag, sizeof(flag), 1, f);
    fwrite(&s->bytecode_address, sizeof(s->bytecode_address), 1, f);
    fwrite(&s->arity, sizeof(s->arity), 1, f);
    fwrite(&s->locals_count, sizeof(s->locals_count), 1, f);
    fwrite(&s->upvalue_count, sizeof(s->upvalue_count), 1, f);
    for (int i = 0; i < s->upvalue_count; ++i) {
        fwrite(&s->upvalues[i].index, sizeof(uint8_t), 1, f);
        uint8_t is_local = s->upvalues[i].isLocal ? 1 : 0;
        uint8_t is_ref = s->upvalues[i].is_ref ? 1 : 0;
        fwrite(&is_local, sizeof(uint8_t), 1, f);
        fwrite(&is_ref, sizeof(uint8_t), 1, f);
    }
}

static void write_procedure_table(FILE* f, HashTable* table) {
    if (!table) return;
    for (int i = 0; i < HASHTABLE_SIZE; ++i) {
        for (Symbol* s = table->buckets[i]; s; s = s->next) {
            if (s->is_alias) continue;
            write_procedure(f, s);
            if (s->type_def && s->type_def->symbol_table) {
                write_procedure_table(f, (HashTable*)s->type_def->symbol_table);
            }
        }
    }
}

static bool read_procedure(FILE* f, HashTable* table) {
    int len = 0;
    if (fread(&len, sizeof(len), 1, f) != 1) return false;
    char* name = (len > 0) ? (char*)malloc(len + 1) : NULL;
    if (len > 0 && (!name || fread(name, 1, len, f) != (size_t)len)) {
        if (name) free(name);
        return false;
    }
    if (name) name[len] = '\0';
    uint8_t flag = 0;
    if (fread(&flag, sizeof(flag), 1, f) != 1) { if (name) free(name); return false; }
    int address = 0;
    uint8_t arity = 0, locals = 0, upcount = 0;
    if (fread(&address, sizeof(address), 1, f) != 1 ||
        fread(&arity, sizeof(arity), 1, f) != 1 ||
        fread(&locals, sizeof(locals), 1, f) != 1 ||
        fread(&upcount, sizeof(upcount), 1, f) != 1) {
        if (name) free(name);
        return false;
    }
    Symbol* sym = (Symbol*)calloc(1, sizeof(Symbol));
    if (!sym) { if (name) free(name); return false; }
    sym->name = name;
    sym->is_defined = flag ? true : false;
    sym->bytecode_address = address;
    sym->arity = arity;
    sym->locals_count = locals;
    sym->upvalue_count = upcount;
    for (int i = 0; i < upcount; ++i) {
        if (fread(&sym->upvalues[i].index, sizeof(uint8_t), 1, f) != 1) { free(sym->name); free(sym); return false; }
        uint8_t is_local = 0, is_ref = 0;
        if (fread(&is_local, sizeof(uint8_t), 1, f) != 1) { free(sym->name); free(sym); return false; }
        if (fread(&is_ref, sizeof(uint8_t), 1, f) != 1) { free(sym->name); free(sym); return false; }
        sym->upvalues[i].isLocal = is_local != 0;
        sym->upvalues[i].is_ref = is_ref != 0;
    }
    hashTableInsert(table, sym);
    return true;
}

bool loadBytecodeFromCache(const char* source_path, BytecodeChunk* chunk, HashTable* procedure_table) {
    char* cache_path = build_cache_path(source_path);
    if (!cache_path) return false;

    struct stat src_stat, cache_stat;
    if (stat(source_path, &src_stat) != 0) {
        fprintf(stderr, "Error: Could not stat source file '%s': %s\n", source_path, strerror(errno));
        free(cache_path);
        exit(EXIT_FAILURE);
    }
    if (stat(cache_path, &cache_stat) != 0) {
        if (errno != ENOENT) {
            fprintf(stderr, "Error: Could not access cache file '%s': %s\n", cache_path, strerror(errno));
            free(cache_path);
            exit(EXIT_FAILURE);
        }
        free(cache_path);
        return false;
    }
    if (difftime(cache_stat.st_mtime, src_stat.st_mtime) < 0) {
        free(cache_path);
        return false;
    }

    FILE* f = fopen(cache_path, "rb");
    if (!f) {
        fprintf(stderr, "Error: Could not open cache file '%s': %s\n", cache_path, strerror(errno));
        free(cache_path);
        exit(EXIT_FAILURE);
    }

    bool ok = false;
    uint32_t magic = 0, ver = 0;
    if (fread(&magic, sizeof(magic), 1, f) == 1 &&
        fread(&ver, sizeof(ver), 1, f) == 1 &&
        magic == CACHE_MAGIC && ver == CACHE_VERSION) {
        int ver_len = 0;
        if (fread(&ver_len, sizeof(ver_len), 1, f) == 1 && ver_len > 0) {
            char* cached_ver = (char*)malloc(ver_len + 1);
            if (cached_ver && fread(cached_ver, 1, ver_len, f) == (size_t)ver_len) {
                cached_ver[ver_len] = '\0';
                int cmp = compare_versions(PROGRAM_VERSION, cached_ver);
                free(cached_ver);
                if (cmp > 0) {
                    fclose(f);
                    unlink(cache_path);
                    free(cache_path);
                    return false;
                }
                int count = 0, const_count = 0;
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
                            for (int i = 0; i < const_count; ++i) {
                                if (!read_value(f, &chunk->constants[i])) { ok = false; break; }
                            }
                            if (ok) {
                                int proc_count = 0;
                                if (fread(&proc_count, sizeof(proc_count), 1, f) == 1) {
                                    for (int i = 0; i < proc_count; ++i) {
                                        if (!read_procedure(f, procedure_table)) { ok = false; break; }
                                    }
                                } else {
                                    ok = false;
                                }
                            }
                        }
                    }
                }
            } else {
                if (cached_ver) free(cached_ver);
                fclose(f);
                unlink(cache_path);
                free(cache_path);
                return false;
            }
        } else {
            fclose(f);
            unlink(cache_path);
            free(cache_path);
            return false;
        }
    }
    fclose(f);
    free(cache_path);
    if (!ok) {
        free(chunk->code); chunk->code = NULL; chunk->lines = NULL; chunk->constants = NULL;
        chunk->count = chunk->capacity = 0; chunk->constants_count = chunk->constants_capacity = 0;
    }
    return ok;
}

void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk, HashTable* procedure_table) {
    char* cache_path = build_cache_path(source_path);
    if (!cache_path) {
        fprintf(stderr, "Error: Could not determine cache path for '%s'.\n", source_path);
        exit(EXIT_FAILURE);
    }
    int fd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (fd < 0) {
        fprintf(stderr, "Error: Could not write cache file '%s': %s\n", cache_path, strerror(errno));
        free(cache_path);
        exit(EXIT_FAILURE);
    }
    FILE* f = fdopen(fd, "wb");
    if (!f) {
        fprintf(stderr, "Error: Could not open cache file '%s': %s\n", cache_path, strerror(errno));
        close(fd);
        free(cache_path);
        exit(EXIT_FAILURE);
    }
    uint32_t magic = CACHE_MAGIC, ver = CACHE_VERSION;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    int ver_len = (int)strlen(PROGRAM_VERSION);
    fwrite(&ver_len, sizeof(ver_len), 1, f);
    fwrite(PROGRAM_VERSION, 1, ver_len, f);
    fwrite(&chunk->count, sizeof(chunk->count), 1, f);
    fwrite(&chunk->constants_count, sizeof(chunk->constants_count), 1, f);
    fwrite(chunk->code, 1, chunk->count, f);
    fwrite(chunk->lines, sizeof(int), chunk->count, f);
    for (int i = 0; i < chunk->constants_count; ++i) {
        if (!write_value(f, &chunk->constants[i])) { break; }
    }
    int proc_count = count_procedures(procedure_table);
    fwrite(&proc_count, sizeof(proc_count), 1, f);
    write_procedure_table(f, procedure_table);
    if (fclose(f) != 0) {
        fprintf(stderr, "Error: Failed to close cache file '%s': %s\n", cache_path, strerror(errno));
        free(cache_path);
        exit(EXIT_FAILURE);
    }
    free(cache_path);
}
