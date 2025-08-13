#include "core/cache.h"
#include "core/utils.h" // for Value constructors
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
#define CACHE_VERSION 1

static unsigned long hash_path(const char* path) {
    uint32_t hash = 2166136261u;
    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        hash ^= *p;
        hash *= 16777619u;
    }
    return (unsigned long)hash;
}

static char* build_cache_path(const char* source_path) {
    const char* home = getenv("HOME");
    if (!home) return NULL;
    size_t dir_len = strlen(home) + 1 + strlen(CACHE_DIR) + 1;
    char* dir = (char*)malloc(dir_len);
    if (!dir) return NULL;
    snprintf(dir, dir_len, "%s/%s", home, CACHE_DIR);
    mkdir(dir, 0777); // ensure directory exists

    unsigned long h = hash_path(source_path);
    size_t path_len = dir_len + 32;
    char* full = (char*)malloc(path_len);
    if (!full) { free(dir); return NULL; }
    snprintf(full, path_len, "%s/%lu.bc", dir, h);
    free(dir);
    return full;
}

static bool is_cache_fresh(const char* cache_path, const char* source_path) {
    struct stat src_stat, cache_stat;
    if (stat(source_path, &src_stat) != 0) return false;
    if (stat(cache_path, &cache_stat) != 0) return false;
    return difftime(cache_stat.st_mtime, src_stat.st_mtime) >= 0;
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

bool loadBytecodeFromCache(const char* source_path, BytecodeChunk* chunk) {
    char* cache_path = build_cache_path(source_path);
    if (!cache_path) return false;
    bool ok = false;
    if (is_cache_fresh(cache_path, source_path)) {
        FILE* f = fopen(cache_path, "rb");
        if (f) {
            uint32_t magic = 0, ver = 0;
            if (fread(&magic, sizeof(magic), 1, f) == 1 &&
                fread(&ver, sizeof(ver), 1, f) == 1 &&
                magic == CACHE_MAGIC && ver == CACHE_VERSION) {
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
                        }
                    }
                }
            }
            fclose(f);
        }
    }
    free(cache_path);
    if (!ok) {
        free(chunk->code); chunk->code = NULL; chunk->lines = NULL; chunk->constants = NULL;
        chunk->count = chunk->capacity = 0; chunk->constants_count = chunk->constants_capacity = 0;
    }
    return ok;
}

void saveBytecodeToCache(const char* source_path, const BytecodeChunk* chunk) {
    char* cache_path = build_cache_path(source_path);
    if (!cache_path) return;
    FILE* f = fopen(cache_path, "wb");
    if (!f) { free(cache_path); return; }
    uint32_t magic = CACHE_MAGIC, ver = CACHE_VERSION;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&chunk->count, sizeof(chunk->count), 1, f);
    fwrite(&chunk->constants_count, sizeof(chunk->constants_count), 1, f);
    fwrite(chunk->code, 1, chunk->count, f);
    fwrite(chunk->lines, sizeof(int), chunk->count, f);
    for (int i = 0; i < chunk->constants_count; ++i) {
        if (!write_value(f, &chunk->constants[i])) { break; }
    }
    fclose(f);
    free(cache_path);
}
