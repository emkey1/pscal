#include "core/cache.h"
#include "core/utils.h" // for Value constructors
#include "Pascal/globals.h"
#include "core/version.h"
#include "symbol/symbol.h"
#include "Pascal/parser.h"
#include "shell/function.h"
#include "vm/string_sentinels.h"
#include "ast/ast.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <ctype.h>
#include <limits.h>

#define CACHE_ROOT ".pscal"
#define CACHE_DIR "bc_cache"
#define CACHE_MAGIC 0x50534232 /* 'PSB2' */

static uint32_t g_astCacheVersion = 0;

#define FNV1A64_OFFSET 1469598103934665603ULL
#define FNV1A64_PRIME 1099511628211ULL


typedef struct {
    char* path;
    time_t mtime;
} CacheCandidate;

typedef struct {
    const BytecodeChunk** items;
    size_t count;
    size_t capacity;
} ChunkHashContext;

static void fnv1aUpdate(uint64_t* hash, const void* data, size_t len) {
    if (!hash || !data) return;
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < len; ++i) {
        *hash ^= bytes[i];
        *hash *= FNV1A64_PRIME;
    }
}

static void fnv1aUpdateUInt32(uint64_t* hash, uint32_t value) {
    fnv1aUpdate(hash, &value, sizeof(value));
}

static void fnv1aUpdateInt(uint64_t* hash, int value) {
    fnv1aUpdate(hash, &value, sizeof(value));
}

static void fnv1aUpdateUInt64(uint64_t* hash, uint64_t value) {
    fnv1aUpdate(hash, &value, sizeof(value));
}

static bool hasPathPrefix(const char* path, const char* prefix) {
    if (!path || !prefix) {
        return false;
    }
    size_t prefix_len = strlen(prefix);
    while (prefix_len > 0) {
#ifndef _WIN32
        if (prefix[prefix_len - 1] != '/') {
            break;
        }
#else
        if (prefix[prefix_len - 1] != '/' && prefix[prefix_len - 1] != '\\') {
            break;
        }
#endif
        prefix_len--;
    }
    if (prefix_len == 0) {
        return false;
    }
    if (strncmp(path, prefix, prefix_len) != 0) {
        return false;
    }
#ifndef _WIN32
    char next = path[prefix_len];
    return next == '/' || next == '\0';
#else
    char next = path[prefix_len];
    return next == '/' || next == '\\' || next == '\0';
#endif
}

static bool isLikelyTemporaryPath(const char* path) {
    if (!path || !path[0]) {
        return false;
    }

    const char* candidates[8];
    size_t candidate_count = 0;

    const char* env_tmp = getenv("TMPDIR");
    if (env_tmp && env_tmp[0]) {
        candidates[candidate_count++] = env_tmp;
    }
    env_tmp = getenv("TEMP");
    if (env_tmp && env_tmp[0]) {
        candidates[candidate_count++] = env_tmp;
    }
    env_tmp = getenv("TMP");
    if (env_tmp && env_tmp[0]) {
        bool duplicate = false;
        for (size_t i = 0; i < candidate_count; ++i) {
            if (strcmp(candidates[i], env_tmp) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            candidates[candidate_count++] = env_tmp;
        }
    }

#ifndef _WIN32
    const char* defaults[] = { "/tmp", "/var/tmp", "/private/tmp", "/private/var/tmp", "/dev/shm" };
#else
    const char* defaults[] = { "C\\Windows\\Temp", "C:/Windows/Temp", "C\\Temp", "C:/Temp" };
#endif
    for (size_t i = 0; i < sizeof(defaults) / sizeof(defaults[0]); ++i) {
        bool duplicate = false;
        for (size_t j = 0; j < candidate_count; ++j) {
            if (strcmp(candidates[j], defaults[i]) == 0) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            candidates[candidate_count++] = defaults[i];
        }
    }

    for (size_t i = 0; i < candidate_count; ++i) {
        if (hasPathPrefix(path, candidates[i])) {
            return true;
        }
    }
    return false;
}

static uint64_t computePathHash(const char* source_path, const char* sanitized_base) {
    uint64_t hash = FNV1A64_OFFSET;
    const char* path_for_hash = NULL;

    if (source_path && source_path[0]) {
#ifndef _WIN32
        char* resolved = realpath(source_path, NULL);
        if (resolved) {
            path_for_hash = resolved;
        } else {
            path_for_hash = source_path;
        }
#else
        char resolved[_MAX_PATH];
        if (_fullpath(resolved, source_path, _MAX_PATH)) {
            path_for_hash = resolved;
        } else {
            path_for_hash = source_path;
        }
#endif
    }

    bool treat_as_temp = isLikelyTemporaryPath(path_for_hash);
    if (!treat_as_temp && path_for_hash && path_for_hash[0]) {
        fnv1aUpdate(&hash, path_for_hash, strlen(path_for_hash));
    } else if (sanitized_base && sanitized_base[0]) {
        fnv1aUpdate(&hash, sanitized_base, strlen(sanitized_base));
    } else if (path_for_hash && path_for_hash[0]) {
        fnv1aUpdate(&hash, path_for_hash, strlen(path_for_hash));
    } else {
        fnv1aUpdate(&hash, "<none>", 6);
    }

#ifndef _WIN32
    if (path_for_hash && path_for_hash != source_path) {
        free((void*)path_for_hash);
    }
#endif

    return hash;
}

static const char* basenameForPath(const char* path) {
    if (!path) return "";
    const char* last_slash = strrchr(path, '/');
#ifdef _WIN32
    const char* last_backslash = strrchr(path, '\\');
    if (!last_slash || (last_backslash && last_backslash > last_slash)) {
        last_slash = last_backslash;
    }
#endif
    return last_slash ? last_slash + 1 : path;
}

static char* sanitizeFileComponent(const char* name) {
    if (!name) return strdup("");
    size_t len = strlen(name);
    char* sanitized = (char*)malloc(len + 1);
    if (!sanitized) return NULL;
    for (size_t i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (isalnum(ch) || ch == '.' || ch == '-' || ch == '_') {
            sanitized[i] = (char)ch;
        } else {
            sanitized[i] = '_';
        }
    }
    sanitized[len] = '\0';
    return sanitized;
}

static bool computeSourceHash(const char* source_path, bool require_file, uint64_t* out_hash) {
    if (!out_hash) return false;
    if (!source_path || source_path[0] == '\0') {
        if (require_file) return false;
        uint64_t fallback = FNV1A64_OFFSET;
        fnv1aUpdate(&fallback, "<none>", 6);
        *out_hash = fallback;
        return true;
    }
    FILE* f = fopen(source_path, "rb");
    if (!f) {
        if (!require_file) {
            uint64_t fallback = FNV1A64_OFFSET;
            fnv1aUpdate(&fallback, source_path, strlen(source_path));
            *out_hash = fallback;
            return true;
        }
        return false;
    }
    uint64_t hash = FNV1A64_OFFSET;
    unsigned char buffer[4096];
    size_t read_bytes = 0;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), f)) > 0) {
        fnv1aUpdate(&hash, buffer, read_bytes);
    }
    if (ferror(f)) {
        fclose(f);
        return false;
    }
    fclose(f);
    *out_hash = hash;
    return true;
}

static bool writeChunkCore(FILE* f, const BytecodeChunk* chunk);
static bool readChunkCore(FILE* f,
                          BytecodeChunk* chunk,
                          uint32_t version,
                          uint64_t source_hash,
                          uint64_t expected_combined_hash,
                          bool verify_combined);
static bool writePointerValue(FILE* f, const Value* v);
static bool readPointerValue(FILE* f, Value* out);
static bool readValue(FILE* f, Value* out);
static void countProceduresRecursive(HashTable* table, int* count);
static bool writeProcedureEntriesRecursive(FILE* f, HashTable* table);
static bool loadProceduresFromStream(FILE* f, int proc_count, uint32_t chunk_version);
static void hashValue(uint64_t* hash, const Value* v, ChunkHashContext* ctx);
static uint64_t computeChunkHashInternal(const BytecodeChunk* chunk, ChunkHashContext* ctx);
static bool chunkHashContextContains(const ChunkHashContext* ctx, const BytecodeChunk* chunk);
static bool chunkHashContextPush(ChunkHashContext* ctx, const BytecodeChunk* chunk);
static void chunkHashContextPop(ChunkHashContext* ctx);

static uint64_t computeChunkHash(const BytecodeChunk* chunk) {
    ChunkHashContext ctx = {0};
    uint64_t hash = computeChunkHashInternal(chunk, &ctx);
    free(ctx.items);
    return hash;
}

static bool chunkHashContextContains(const ChunkHashContext* ctx, const BytecodeChunk* chunk) {
    if (!ctx || !chunk || ctx->count == 0) {
        return false;
    }
    for (size_t i = 0; i < ctx->count; ++i) {
        if (ctx->items[i] == chunk) {
            return true;
        }
    }
    return false;
}

static bool chunkHashContextPush(ChunkHashContext* ctx, const BytecodeChunk* chunk) {
    if (!ctx || !chunk) {
        return true;
    }
    if (chunkHashContextContains(ctx, chunk)) {
        return true;
    }
    if (ctx->count == ctx->capacity) {
        size_t new_capacity = ctx->capacity ? ctx->capacity * 2 : 8;
        const BytecodeChunk** new_items = (const BytecodeChunk**)realloc(ctx->items, new_capacity * sizeof(const BytecodeChunk*));
        if (!new_items) {
            return false;
        }
        ctx->items = new_items;
        ctx->capacity = new_capacity;
    }
    ctx->items[ctx->count++] = chunk;
    return true;
}

static void chunkHashContextPop(ChunkHashContext* ctx) {
    if (!ctx || ctx->count == 0) {
        return;
    }
    ctx->count--;
}

static uint64_t computeChunkHashInternal(const BytecodeChunk* chunk, ChunkHashContext* ctx) {
    uint64_t hash = FNV1A64_OFFSET;
    if (!chunk) {
        return hash;
    }

    if (chunkHashContextContains(ctx, chunk)) {
        fnv1aUpdateUInt64(&hash, (uint64_t)(uintptr_t)chunk);
        return hash;
    }

    bool pushed = chunkHashContextPush(ctx, chunk);
    if (!pushed) {
        fnv1aUpdateUInt64(&hash, (uint64_t)(uintptr_t)chunk);
        return hash;
    }

    fnv1aUpdateUInt32(&hash, chunk->version);
    fnv1aUpdateInt(&hash, chunk->count);
    if (chunk->code && chunk->count > 0) {
        fnv1aUpdate(&hash, chunk->code, (size_t)chunk->count);
    }
    fnv1aUpdateInt(&hash, chunk->constants_count);
    if (chunk->lines && chunk->count > 0) {
        fnv1aUpdate(&hash, chunk->lines, sizeof(int) * (size_t)chunk->count);
    }
    if (chunk->constants && chunk->constants_count > 0) {
        for (int i = 0; i < chunk->constants_count; ++i) {
            hashValue(&hash, &chunk->constants[i], ctx);
        }
    }

    if (ctx) {
        chunkHashContextPop(ctx);
    }
    return hash;
}

static uint64_t computeCombinedHash(uint64_t source_hash, const BytecodeChunk* chunk) {
    uint64_t combined = FNV1A64_OFFSET;
    fnv1aUpdateUInt64(&combined, source_hash);
    uint64_t chunk_hash = computeChunkHash(chunk);
    fnv1aUpdateUInt64(&combined, chunk_hash);
    return combined;
}

static CacheCandidate* gatherCacheCandidates(const char* dir, const char* prefix, size_t* out_count) {
    if (out_count) *out_count = 0;
    if (!dir || !prefix) return NULL;
    DIR* cache_dir = opendir(dir);
    if (!cache_dir) return NULL;

    CacheCandidate* candidates = NULL;
    size_t candidate_count = 0;
    size_t candidate_capacity = 0;
    struct dirent* entry;
    size_t prefix_length = strlen(prefix);
    while ((entry = readdir(cache_dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, prefix, prefix_length) != 0) continue;
        size_t name_len = strlen(entry->d_name);
        if (name_len < 3 || strcmp(entry->d_name + name_len - 3, ".bc") != 0) continue;

        size_t path_len = strlen(dir) + 1 + name_len + 1;
        char* full_path = (char*)malloc(path_len);
        if (!full_path) continue;
        snprintf(full_path, path_len, "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            free(full_path);
            continue;
        }

        if (candidate_count == candidate_capacity) {
            size_t new_cap = candidate_capacity ? candidate_capacity * 2 : 4;
            CacheCandidate* new_arr = (CacheCandidate*)realloc(candidates, new_cap * sizeof(CacheCandidate));
            if (!new_arr) {
                free(full_path);
                continue;
            }
            candidates = new_arr;
            candidate_capacity = new_cap;
        }
        candidates[candidate_count].path = full_path;
        candidates[candidate_count].mtime = st.st_mtime;
        candidate_count++;
    }
    closedir(cache_dir);

    if (candidate_count > 1) {
        for (size_t i = 0; i < candidate_count - 1; ++i) {
            for (size_t j = i + 1; j < candidate_count; ++j) {
                if (candidates[j].mtime > candidates[i].mtime) {
                    CacheCandidate tmp = candidates[i];
                    candidates[i] = candidates[j];
                    candidates[j] = tmp;
                }
            }
        }
    }

    if (out_count) *out_count = candidate_count;
    return candidates;
}

static void freeCandidates(CacheCandidate* candidates, size_t count) {
    if (!candidates) return;
    for (size_t i = 0; i < count; ++i) {
        free(candidates[i].path);
    }
    free(candidates);
}

static void resetChunk(BytecodeChunk* chunk, int read_consts) {
    if (!chunk) return;
    if (chunk->constants) {
        for (int i = 0; i < read_consts; ++i) {
            freeValue(&chunk->constants[i]);
        }
    }
    free(chunk->code);
    free(chunk->lines);
    free(chunk->constants);
    free(chunk->builtin_lowercase_indices);
    initBytecodeChunk(chunk);
}

static void sanitizeCompilerId(const char* compiler_id, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) {
        return;
    }
    size_t out_idx = 0;
    if (compiler_id && compiler_id[0]) {
        for (const char* p = compiler_id; *p && out_idx + 1 < buffer_size; ++p) {
            unsigned char ch = (unsigned char)(*p);
            if ((ch >= 'A' && ch <= 'Z')) {
                buffer[out_idx++] = (char)(ch - 'A' + 'a');
            } else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
                buffer[out_idx++] = (char)ch;
            } else if (*p == '-' || *p == '_') {
                buffer[out_idx++] = '-';
            }
        }
    }
    if (out_idx == 0) {
        strncpy(buffer, "pscal", buffer_size - 1);
        buffer[buffer_size - 1] = '\0';
        out_idx = strlen(buffer);
    }
    if (out_idx >= buffer_size) {
        out_idx = buffer_size - 1;
    }
    buffer[out_idx] = '\0';
}

static char* ensureCacheDirectory(const char* compiler_id, char* out_safe_id, size_t safe_id_size) {
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

    if (out_safe_id && safe_id_size > 0) {
        sanitizeCompilerId(compiler_id, out_safe_id, safe_id_size);
    }

    return dir;
}

char* buildCachePath(const char* source_path, const char* compiler_id) {
    char safe_id[32];
    char* dir = ensureCacheDirectory(compiler_id, safe_id, sizeof(safe_id));
    if (!dir) return NULL;

    const char* base_name = basenameForPath(source_path);
    char* sanitized_base = sanitizeFileComponent(base_name);
    if (!sanitized_base) {
        free(dir);
        return NULL;
    }

    uint64_t source_hash = 0;
    if (!computeSourceHash(source_path, true, &source_hash)) {
        free(dir);
        free(sanitized_base);
        return NULL;
    }

    char source_hex[17];
    snprintf(source_hex, sizeof(source_hex), "%016llx", (unsigned long long)source_hash);

    uint64_t path_hash = computePathHash(source_path, sanitized_base);
    char path_hex[17];
    snprintf(path_hex, sizeof(path_hex), "%016llx", (unsigned long long)path_hash);

    size_t prefix_len = strlen(safe_id) + strlen(sanitized_base) + strlen(path_hex) + strlen(source_hex) + 5;
    char* prefix = (char*)malloc(prefix_len);
    if (!prefix) {
        free(dir);
        free(sanitized_base);
        return NULL;
    }
    snprintf(prefix, prefix_len, "%s-%s-%s-%s-", safe_id, sanitized_base, path_hex, source_hex);

    size_t candidate_count = 0;
    CacheCandidate* candidates = gatherCacheCandidates(dir, prefix, &candidate_count);

    char* result = NULL;
    if (candidate_count > 0 && candidates) {
        result = strdup(candidates[0].path);
    }

    freeCandidates(candidates, candidate_count);
    free(prefix);
    free(sanitized_base);
    free(dir);
    return result;
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

static bool verifySourcePath(FILE* f, const char* source_path, bool strict) {
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
    if (!match && strict) {
        free(buf);
        return false;
    }
    free(buf);
    return true;
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
    tok->is_char_code = false;
    return tok;
}

static bool writeAst(FILE* f, const AST* node) {
    int has = (node != NULL);
    fwrite(&has, sizeof(has), 1, f);
    if (!has) return true;
    fwrite(&node->type, sizeof(node->type), 1, f);
    fwrite(&node->var_type, sizeof(node->var_type), 1, f);
    if (g_astCacheVersion >= 9) {
        uint8_t flags = 0;
        if (node->by_ref) flags |= 0x01;
        if (node->is_inline) flags |= 0x02;
        if (node->is_virtual) flags |= 0x04;
        if (node->is_global_scope) flags |= 0x08;
        fwrite(&flags, sizeof(flags), 1, f);
    }
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
    uint8_t flags = 0;
    if (g_astCacheVersion >= 9) {
        if (fread(&flags, sizeof(flags), 1, f) != 1) return NULL;
    }
    Token* tok = readToken(f);
    int i_val = 0;
    if (fread(&i_val, sizeof(i_val), 1, f) != 1) { if (tok) freeToken(tok); return NULL; }
    AST* node = newASTNode(t, tok);
    if (node) {
        setTypeAST(node, vt);
        node->i_val = i_val;
        if (g_astCacheVersion >= 9) {
            node->by_ref = (flags & 0x01) != 0;
            node->is_inline = (flags & 0x02) != 0;
            node->is_virtual = (flags & 0x04) != 0;
            node->is_global_scope = (flags & 0x08) != 0;
        }
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
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT64:
            fwrite(&v->i_val, sizeof(v->i_val), 1, f); break;
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64:
            fwrite(&v->u_val, sizeof(v->u_val), 1, f); break;
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
            if (total > 0 && arrayUsesPackedBytes(v)) {
                if (!v->array_raw) return false;
                for (int i = 0; i < total; i++) {
                    Value temp = makeByte(v->array_raw[i]);
                    if (!writeValue(f, &temp)) return false;
                }
            } else {
                for (int i = 0; i < total; i++) {
                    if (!writeValue(f, &v->array_val[i])) return false;
                }
            }
            break;
        }
        case TYPE_POINTER:
            return writePointerValue(f, v);
        default:
            return false;
    }
    return true;
}

static bool writeChunkCore(FILE* f, const BytecodeChunk* chunk) {
    if (!f || !chunk) {
        return false;
    }

    uint32_t prev_version = g_astCacheVersion;
    g_astCacheVersion = chunk->version;
    bool success = false;

    if (fwrite(&chunk->count, sizeof(chunk->count), 1, f) != 1) goto cleanup;
    if (fwrite(&chunk->constants_count, sizeof(chunk->constants_count), 1, f) != 1) goto cleanup;

    if (chunk->count > 0) {
        if (!chunk->code || !chunk->lines) goto cleanup;
        if (fwrite(chunk->code, 1, (size_t)chunk->count, f) != (size_t)chunk->count) goto cleanup;
        if (fwrite(chunk->lines, sizeof(int), (size_t)chunk->count, f) != (size_t)chunk->count) goto cleanup;
    }

    for (int i = 0; i < chunk->constants_count; ++i) {
        if (!writeValue(f, &chunk->constants[i])) {
            goto cleanup;
        }
    }

    int builtin_map_count = 0;
    if (chunk->builtin_lowercase_indices) {
        for (int i = 0; i < chunk->constants_count; ++i) {
            int lower_idx = chunk->builtin_lowercase_indices[i];
            if (lower_idx >= 0 && lower_idx < chunk->constants_count) {
                builtin_map_count++;
            }
        }
    }
    if (fwrite(&builtin_map_count, sizeof(builtin_map_count), 1, f) != 1) goto cleanup;
    if (chunk->builtin_lowercase_indices) {
        for (int i = 0; i < chunk->constants_count; ++i) {
            int lower_idx = chunk->builtin_lowercase_indices[i];
            if (lower_idx >= 0 && lower_idx < chunk->constants_count) {
                if (fwrite(&i, sizeof(i), 1, f) != 1) goto cleanup;
                if (fwrite(&lower_idx, sizeof(lower_idx), 1, f) != 1) goto cleanup;
            }
        }
    }

    int proc_count = 0;
    countProceduresRecursive(procedure_table, &proc_count);
    int stored_proc_count = -(proc_count + 1);
    if (fwrite(&stored_proc_count, sizeof(stored_proc_count), 1, f) != 1) goto cleanup;
    if (!writeProcedureEntriesRecursive(f, procedure_table)) {
        goto cleanup;
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
    if (fwrite(&const_sym_count, sizeof(const_sym_count), 1, f) != 1) goto cleanup;
    if (globalSymbols) {
        for (int i = 0; i < HASHTABLE_SIZE; i++) {
            for (Symbol* sym = globalSymbols->buckets[i]; sym; sym = sym->next) {
                if (sym->is_alias || !sym->is_const) continue;
                int name_len = (int)strlen(sym->name);
                if (fwrite(&name_len, sizeof(name_len), 1, f) != 1) goto cleanup;
                if (fwrite(sym->name, 1, (size_t)name_len, f) != (size_t)name_len) goto cleanup;
                if (fwrite(&sym->type, sizeof(sym->type), 1, f) != 1) goto cleanup;
                if (sym->value) {
                    if (!writeValue(f, sym->value)) goto cleanup;
                } else {
                    Value tmp = makeVoid();
                    if (!writeValue(f, &tmp)) goto cleanup;
                }
            }
        }
    }

    int type_count = 0;
    for (TypeEntry* entry = type_table; entry; entry = entry->next) type_count++;
    if (fwrite(&type_count, sizeof(type_count), 1, f) != 1) goto cleanup;
    for (TypeEntry* entry = type_table; entry; entry = entry->next) {
        int name_len = (int)strlen(entry->name);
        if (fwrite(&name_len, sizeof(name_len), 1, f) != 1) goto cleanup;
        if (fwrite(entry->name, 1, (size_t)name_len, f) != (size_t)name_len) goto cleanup;
        writeAst(f, entry->typeAST);
    }

    success = true;

cleanup:
    g_astCacheVersion = prev_version;
    return success;
}

static bool readChunkCore(FILE* f,
                          BytecodeChunk* chunk,
                          uint32_t version,
                          uint64_t source_hash,
                          uint64_t expected_combined_hash,
                          bool verify_combined) {
    if (!f || !chunk) {
        return false;
    }

    int count = 0;
    int const_count = 0;
    if (fread(&count, sizeof(count), 1, f) != 1 ||
        fread(&const_count, sizeof(const_count), 1, f) != 1) {
        return false;
    }

    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->constants = NULL;
    chunk->builtin_lowercase_indices = NULL;

    uint32_t prev_version = g_astCacheVersion;
    g_astCacheVersion = version;
#define RETURN_FALSE do { g_astCacheVersion = prev_version; return false; } while (0)

    if (count > 0) {
        chunk->code = (uint8_t*)malloc((size_t)count);
        chunk->lines = (int*)malloc(sizeof(int) * (size_t)count);
        if (!chunk->code || !chunk->lines) {
            free(chunk->code);
            free(chunk->lines);
            RETURN_FALSE;
        }
    }

    if (const_count > 0) {
        chunk->constants = (Value*)calloc((size_t)const_count, sizeof(Value));
        if (!chunk->constants) {
            free(chunk->code);
            free(chunk->lines);
            chunk->code = NULL;
            chunk->lines = NULL;
            RETURN_FALSE;
        }
        chunk->global_symbol_cache = (Symbol**)calloc((size_t)const_count, sizeof(Symbol*));
        if (!chunk->global_symbol_cache) {
            free(chunk->code);
            free(chunk->lines);
            free(chunk->constants);
            chunk->code = NULL;
            chunk->lines = NULL;
            chunk->constants = NULL;
            RETURN_FALSE;
        }
    }

    chunk->count = count;
    chunk->capacity = count;
    chunk->constants_count = const_count;
    chunk->constants_capacity = const_count;

    if (count > 0) {
        if (fread(chunk->code, 1, (size_t)count, f) != (size_t)count ||
            fread(chunk->lines, sizeof(int), (size_t)count, f) != (size_t)count) {
            resetChunk(chunk, 0);
            RETURN_FALSE;
        }
    }

    int read_consts = 0;
    for (; read_consts < const_count; ++read_consts) {
        if (!readValue(f, &chunk->constants[read_consts])) {
            resetChunk(chunk, read_consts);
            RETURN_FALSE;
        }
    }

    if (const_count > 0) {
        chunk->builtin_lowercase_indices = (int*)malloc(sizeof(int) * (size_t)const_count);
        if (!chunk->builtin_lowercase_indices) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        for (int i = 0; i < const_count; ++i) {
            chunk->builtin_lowercase_indices[i] = -1;
        }
    }

    if (version >= 8) {
        int builtin_map_count = 0;
        if (fread(&builtin_map_count, sizeof(builtin_map_count), 1, f) != 1) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        for (int i = 0; i < builtin_map_count; ++i) {
            int original_idx = 0;
            int lower_idx = 0;
            if (fread(&original_idx, sizeof(original_idx), 1, f) != 1 ||
                fread(&lower_idx, sizeof(lower_idx), 1, f) != 1) {
                resetChunk(chunk, const_count);
                RETURN_FALSE;
            }
            if (chunk->builtin_lowercase_indices &&
                original_idx >= 0 && original_idx < chunk->constants_count) {
                chunk->builtin_lowercase_indices[original_idx] = lower_idx;
            }
        }
    } else {
        int dummy = 0;
        if (fread(&dummy, sizeof(dummy), 1, f) != 1) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
    }

    if (verify_combined) {
        uint64_t computed = computeCombinedHash(source_hash, chunk);
        if (computed != expected_combined_hash) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
    }

    int stored_proc_count = 0;
    if (fread(&stored_proc_count, sizeof(stored_proc_count), 1, f) != 1) {
        resetChunk(chunk, const_count);
        RETURN_FALSE;
    }
    if (stored_proc_count < 0) {
        int proc_count = -stored_proc_count - 1;
        if (!loadProceduresFromStream(f, proc_count, version)) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
    } else {
        resetChunk(chunk, const_count);
        RETURN_FALSE;
    }

    int const_sym_count = 0;
    if (fread(&const_sym_count, sizeof(const_sym_count), 1, f) != 1) {
        resetChunk(chunk, const_count);
        RETURN_FALSE;
    }
    for (int i = 0; i < const_sym_count; ++i) {
        int name_len = 0;
        if (fread(&name_len, sizeof(name_len), 1, f) != 1) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        char* name = (char*)malloc((size_t)name_len + 1);
        if (!name) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        if (fread(name, 1, (size_t)name_len, f) != (size_t)name_len) {
            free(name);
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        name[name_len] = '\0';

        VarType type;
        if (fread(&type, sizeof(type), 1, f) != 1) {
            free(name);
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }

        Value val = {0};
        if (!readValue(f, &val)) {
            free(name);
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }

        insertGlobalSymbol(name, type, NULL);
        Symbol* sym = lookupGlobalSymbol(name);
        if (sym && sym->value) {
            freeValue(sym->value);
            *(sym->value) = val;
            sym->is_const = true;
            insertConstGlobalSymbol(name, *(sym->value));
        } else {
            freeValue(&val);
        }
        free(name);
    }

    int type_count = 0;
    if (fread(&type_count, sizeof(type_count), 1, f) != 1) {
        resetChunk(chunk, const_count);
        RETURN_FALSE;
    }
    for (int i = 0; i < type_count; ++i) {
        int name_len = 0;
        if (fread(&name_len, sizeof(name_len), 1, f) != 1) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        char* name = (char*)malloc((size_t)name_len + 1);
        if (!name) {
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        if (fread(name, 1, (size_t)name_len, f) != (size_t)name_len) {
            free(name);
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        name[name_len] = '\0';
        AST* type_ast = readAst(f);
        if (!type_ast) {
            free(name);
            resetChunk(chunk, const_count);
            RETURN_FALSE;
        }
        insertType(name, type_ast);
        freeAST(type_ast);
        free(name);
    }

    g_astCacheVersion = prev_version;

#undef RETURN_FALSE

    return true;
}

static bool writePointerValue(FILE* f, const Value* v) {
    if (!f || !v) {
        return false;
    }

    uint8_t kind = 0;
    if (!v->ptr_val) {
        return fwrite(&kind, sizeof(kind), 1, f) == 1;
    }

    if (v->base_type_node == STRING_CHAR_PTR_SENTINEL ||
        v->base_type_node == SERIALIZED_CHAR_PTR_SENTINEL) {
        const char *text = (const char *)v->ptr_val;
        if (!text) {
            return false;
        }
        int len = (int)strlen(text);
        kind = 2;
        if (fwrite(&kind, sizeof(kind), 1, f) != 1) {
            return false;
        }
        if (fwrite(&len, sizeof(len), 1, f) != 1) {
            return false;
        }
        if (len > 0 && fwrite(text, 1, (size_t)len, f) != (size_t)len) {
            return false;
        }
        return true;
    }

    if (v->base_type_node == SHELL_FUNCTION_PTR_SENTINEL) {
        ShellCompiledFunction* compiled = (ShellCompiledFunction*)v->ptr_val;
        if (!compiled || compiled->magic != SHELL_COMPILED_FUNCTION_MAGIC) {
            return false;
        }

        kind = 1;
        if (fwrite(&kind, sizeof(kind), 1, f) != 1) {
            return false;
        }
        if (fwrite(&compiled->chunk.version, sizeof(compiled->chunk.version), 1, f) != 1) {
            return false;
        }
        return writeChunkCore(f, &compiled->chunk);
    }

    kind = 3;
    if (fwrite(&kind, sizeof(kind), 1, f) != 1) {
        return false;
    }
    uint64_t raw_addr = (uint64_t)(uintptr_t)v->ptr_val;
    if (fwrite(&raw_addr, sizeof(raw_addr), 1, f) != 1) {
        return false;
    }
    return true;
}

static bool readPointerValue(FILE* f, Value* out) {
    if (!f || !out) {
        return false;
    }

    uint8_t kind = 0;
    if (fread(&kind, sizeof(kind), 1, f) != 1) {
        return false;
    }
    if (kind == 0) {
        out->ptr_val = NULL;
        out->base_type_node = NULL;
        return true;
    }
    if (kind == 2) {
        int len = 0;
        if (fread(&len, sizeof(len), 1, f) != 1 || len < 0) {
            return false;
        }
        char *text = (char *)malloc((size_t)len + 1u);
        if (!text) {
            return false;
        }
        if (len > 0 && fread(text, 1, (size_t)len, f) != (size_t)len) {
            free(text);
            return false;
        }
        text[len] = '\0';
        out->ptr_val = (Value*)text;
        out->base_type_node = SERIALIZED_CHAR_PTR_SENTINEL;
        out->element_type = TYPE_UNKNOWN;
        return true;
    }
    if (kind == 3) {
        uint64_t raw_addr = 0;
        if (fread(&raw_addr, sizeof(raw_addr), 1, f) != 1) {
            return false;
        }
        out->ptr_val = (Value*)(uintptr_t)raw_addr;
        out->base_type_node = OPAQUE_POINTER_SENTINEL;
        out->element_type = TYPE_UNKNOWN;
        return true;
    }
    if (kind != 1) {
        return false;
    }

    ShellCompiledFunction* compiled = (ShellCompiledFunction*)calloc(1, sizeof(ShellCompiledFunction));
    if (!compiled) {
        return false;
    }
    compiled->magic = SHELL_COMPILED_FUNCTION_MAGIC;
    initBytecodeChunk(&compiled->chunk);

    uint32_t version = 0;
    if (fread(&version, sizeof(version), 1, f) != 1) {
        free(compiled);
        return false;
    }
    compiled->chunk.version = version;

    if (!readChunkCore(f, &compiled->chunk, version, 0, 0, false)) {
        freeBytecodeChunk(&compiled->chunk);
        free(compiled);
        return false;
    }

    out->ptr_val = (Value*)compiled;
    out->base_type_node = SHELL_FUNCTION_PTR_SENTINEL;
    out->element_type = TYPE_UNKNOWN;
    return true;
}

static void hashValue(uint64_t* hash, const Value* v, ChunkHashContext* ctx) {
    if (!hash) return;
    if (!v) {
        int marker = -1;
        fnv1aUpdateInt(hash, marker);
        return;
    }
    fnv1aUpdateInt(hash, v->type);
    switch (v->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_INT8:
        case TYPE_UINT8:
        case TYPE_INT16:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_INT64:
        case TYPE_UINT64:
            fnv1aUpdate(&(*hash), &v->i_val, sizeof(v->i_val));
            break;
        case TYPE_FLOAT:
            fnv1aUpdate(hash, &v->real.f32_val, sizeof(v->real.f32_val));
            break;
        case TYPE_REAL:
            fnv1aUpdate(hash, &v->real.d_val, sizeof(v->real.d_val));
            break;
        case TYPE_LONG_DOUBLE:
            fnv1aUpdate(hash, &v->real.r_val, sizeof(v->real.r_val));
            break;
        case TYPE_CHAR:
            fnv1aUpdate(hash, &v->c_val, sizeof(v->c_val));
            break;
        case TYPE_STRING: {
            int len = v->s_val ? (int)strlen(v->s_val) : -1;
            fnv1aUpdateInt(hash, len);
            if (len > 0) {
                fnv1aUpdate(hash, v->s_val, (size_t)len);
            }
            break;
        }
        case TYPE_ENUM: {
            int len = v->enum_val.enum_name ? (int)strlen(v->enum_val.enum_name) : 0;
            fnv1aUpdateInt(hash, len);
            if (len > 0) {
                fnv1aUpdate(hash, v->enum_val.enum_name, (size_t)len);
            }
            fnv1aUpdateInt(hash, v->enum_val.ordinal);
            break;
        }
        case TYPE_SET: {
            int sz = v->set_val.set_size;
            fnv1aUpdateInt(hash, sz);
            if (sz > 0 && v->set_val.set_values) {
                fnv1aUpdate(hash, v->set_val.set_values, sizeof(long long) * (size_t)sz);
            }
            break;
        }
        case TYPE_ARRAY: {
            int dims = v->dimensions;
            fnv1aUpdateInt(hash, dims);
            fnv1aUpdateInt(hash, v->element_type);
            for (int i = 0; i < dims; ++i) {
                int lb = v->lower_bounds ? v->lower_bounds[i] : 0;
                int ub = v->upper_bounds ? v->upper_bounds[i] : -1;
                fnv1aUpdateInt(hash, lb);
                fnv1aUpdateInt(hash, ub);
            }
            int total = 0;
            if (dims > 0 && v->lower_bounds && v->upper_bounds) {
                total = 1;
                for (int i = 0; i < dims; ++i) {
                    int span = v->upper_bounds[i] - v->lower_bounds[i] + 1;
                    if (span <= 0) { total = 0; break; }
                    total *= span;
                }
            }
            if (total > 0 && v->array_val) {
                for (int i = 0; i < total; ++i) {
                    hashValue(hash, &v->array_val[i], ctx);
                }
            } else if (total > 0 && arrayUsesPackedBytes(v) && v->array_raw) {
                fnv1aUpdate(hash, v->array_raw, (size_t)total);
            }
            break;
        }
        case TYPE_POINTER: {
            if (!v->ptr_val) {
                fnv1aUpdateUInt64(hash, 0);
                break;
            }
            if (v->base_type_node == STRING_CHAR_PTR_SENTINEL ||
                v->base_type_node == SERIALIZED_CHAR_PTR_SENTINEL) {
                const char *text = (const char *)v->ptr_val;
                int len = text ? (int)strlen(text) : 0;
                fnv1aUpdateInt(hash, len);
                if (len > 0 && text) {
                    fnv1aUpdate(hash, text, (size_t)len);
                }
                break;
            }
            if (v->base_type_node == SHELL_FUNCTION_PTR_SENTINEL) {
                ShellCompiledFunction* compiled = (ShellCompiledFunction*)v->ptr_val;
                if (!compiled || compiled->magic != SHELL_COMPILED_FUNCTION_MAGIC) {
                    fnv1aUpdateUInt64(hash, (uint64_t)(uintptr_t)v->ptr_val);
                    break;
                }
                fnv1aUpdateUInt32(hash, compiled->chunk.version);
                uint64_t nested = computeChunkHashInternal(&compiled->chunk, ctx);
                fnv1aUpdateUInt64(hash, nested);
                break;
            }
            fnv1aUpdateUInt64(hash, (uint64_t)(uintptr_t)v->ptr_val);
            break;
        }
        case TYPE_FILE:
        case TYPE_MEMORYSTREAM:
        case TYPE_THREAD:
            fnv1aUpdateUInt64(hash, (uint64_t)(uintptr_t)v->ptr_val);
            break;
        case TYPE_NIL:
        case TYPE_VOID:
        case TYPE_UNKNOWN:
        case TYPE_RECORD:
            break;
        default:
            fnv1aUpdate(&(*hash), &v->i_val, sizeof(v->i_val));
            break;
    }
}

static bool readValue(FILE* f, Value* out) {
    if (!out) return false;
    memset(out, 0, sizeof(Value));
    if (fread(&out->type, sizeof(out->type), 1, f) != 1) return false;
    switch (out->type) {
        case TYPE_INTEGER:
        case TYPE_WORD:
        case TYPE_BYTE:
        case TYPE_BOOLEAN:
        case TYPE_INT8:
        case TYPE_INT16:
        case TYPE_INT64:
            if (fread(&out->i_val, sizeof(out->i_val), 1, f) != 1) return false;
            out->u_val = (unsigned long long)out->i_val;
            break;
        case TYPE_UINT8:
        case TYPE_UINT16:
        case TYPE_UINT32:
        case TYPE_UINT64: {
            unsigned long long tmp = 0;
            if (fread(&tmp, sizeof(tmp), 1, f) != 1) return false;
            out->u_val = tmp;
            out->i_val = (long long)tmp;
            break;
        }
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
            SET_INT_VALUE(out, out->c_val);
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
            out->array_is_packed = isPackedByteElementType(out->element_type);
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
            out->array_raw = NULL;
            if (total > 0) {
                if (out->array_is_packed) {
                    out->array_raw = (uint8_t*)calloc((size_t)total, sizeof(uint8_t));
                    if (!out->array_raw) return false;
                    for (int i = 0; i < total; i++) {
                        Value temp;
                        if (!readValue(f, &temp)) return false;
                        out->array_raw[i] = (unsigned char)AS_INTEGER(temp);
                        freeValue(&temp);
                    }
                } else {
                    out->array_val = (Value*)calloc((size_t)total, sizeof(Value));
                    if (!out->array_val) return false;
                    for (int i = 0; i < total; i++) {
                        if (!readValue(f, &out->array_val[i])) return false;
                    }
                }
            }
            break;
        }
        case TYPE_POINTER:
            return readPointerValue(f, out);
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
    if (!parent) {
        return NULL;
    }

    if (!parent->type_def) {
        parent->type_def = newASTNode(AST_PROCEDURE_DECL, NULL);
        if (!parent->type_def) {
            return NULL;
        }
    }
    if (!parent->type_def->symbol_table) {
        HashTable* nested = createHashTable();
        if (!nested) {
            return NULL;
        }
        parent->type_def->symbol_table = (Symbol*)nested;
    }
    if (!parent->type_def->symbol_table) {
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

static void ensureProcedureAlias(HashTable* table, const char* alias_name, Symbol* target) {
    if (!table || !alias_name || !*alias_name || !target) {
        return;
    }

    Symbol* existing = hashTableLookup(table, alias_name);
    if (existing) {
        if (existing == target) {
            return;
        }
        if (existing->is_alias) {
            existing->real_symbol = target;
            existing->type = target->type;
        }
        return;
    }

    Symbol* alias = (Symbol*)calloc(1, sizeof(Symbol));
    if (!alias) {
        return;
    }

    alias->name = strdup(alias_name);
    if (!alias->name) {
        free(alias);
        return;
    }

    alias->is_alias = true;
    alias->real_symbol = target;
    alias->type = target->type;
    alias->type_def = NULL;
    alias->value = NULL;

    hashTableInsert(table, alias);
}

static void restoreConstructorAliases(HashTable* table) {
    if (!table) {
        return;
    }

    for (int bucket = 0; bucket < HASHTABLE_SIZE; ++bucket) {
        for (Symbol* sym = table->buckets[bucket]; sym; sym = sym->next) {
            const char* last_dot = NULL;
            const char* method_name = NULL;
            size_t prefix_len = 0;
            char class_name_buf[MAX_SYMBOL_LENGTH];
            const char* simple_start = NULL;
            const char* simple_name = NULL;

            if (!sym || !sym->name || sym->is_alias) {
                continue;
            }

            last_dot = strrchr(sym->name, '.');
            if (!last_dot || last_dot == sym->name) {
                continue;
            }

            method_name = last_dot + 1;
            if (!method_name || *method_name == '\0') {
                continue;
            }

            prefix_len = (size_t)(last_dot - sym->name);
            if (prefix_len == 0 || prefix_len >= MAX_SYMBOL_LENGTH) {
                continue;
            }

            memcpy(class_name_buf, sym->name, prefix_len);
            class_name_buf[prefix_len] = '\0';

            simple_start = strrchr(class_name_buf, '.');
            simple_name = simple_start ? simple_start + 1 : class_name_buf;
            if (!simple_name || *simple_name == '\0') {
                continue;
            }

            if (strcmp(method_name, simple_name) != 0) {
                continue;
            }

            ensureProcedureAlias(table, simple_name, sym);
            if (strcmp(class_name_buf, simple_name) != 0) {
                ensureProcedureAlias(table, class_name_buf, sym);
            }
        }
    }
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
                           const char* compiler_id,
                           const char* frontend_path,
                           const char** dependencies,
                           int dep_count,
                           BytecodeChunk* chunk) {
    if (!chunk || chunk->count > 0) {
        return false;
    }

    char safe_id[32];
    char* dir = ensureCacheDirectory(compiler_id, safe_id, sizeof(safe_id));
    if (!dir) return false;

    char* sanitized_base = sanitizeFileComponent(basenameForPath(source_path));
    if (!sanitized_base) {
        free(dir);
        return false;
    }

    uint64_t source_hash = 0;
    if (!computeSourceHash(source_path, true, &source_hash)) {
        free(dir);
        free(sanitized_base);
        return false;
    }

    char source_hex[17];
    snprintf(source_hex, sizeof(source_hex), "%016llx", (unsigned long long)source_hash);
    uint64_t path_hash = computePathHash(source_path, sanitized_base);
    char path_hex[17];
    snprintf(path_hex, sizeof(path_hex), "%016llx", (unsigned long long)path_hash);

    size_t prefix_len = strlen(safe_id) + strlen(sanitized_base) + strlen(path_hex) + strlen(source_hex) + 5;
    char* prefix = (char*)malloc(prefix_len);
    if (!prefix) {
        free(dir);
        free(sanitized_base);
        return false;
    }
    snprintf(prefix, prefix_len, "%s-%s-%s-%s-", safe_id, sanitized_base, path_hex, source_hex);

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
    /* If we cannot resolve the frontend path, avoid using potentially stale
     * cached bytecode that was produced by a different binary (common when
     * in-process tool runners set argv[0] to just the tool name). */
    if (frontend_path && frontend_path[0] && !frontend_for_cache) {
        free(prefix);
        free(sanitized_base);
        free(dir);
        return false;
    }

    size_t candidate_count = 0;
    CacheCandidate* candidates = gatherCacheCandidates(dir, prefix, &candidate_count);

    bool ok = false;
    bool abort_all = false;
    const char* strict_env = getenv("PSCAL_STRICT_VM");
    bool strict = strict_env && strict_env[0] != '\0';
    uint32_t vm_ver = pscal_vm_version();

    for (size_t idx = 0; idx < candidate_count && !ok; ++idx) {
        const char* cache_path = candidates[idx].path;

        /*
         * Rely on the source hash encoded in the cache filename/header to
         * validate the script contents. Tools like shellbench rewrite
         * temporary scripts for each run, so their file modification times are
         * always newer than any cached bytecode. Ignoring the source mtime
         * allows those callers to benefit from caching while still invalidating
         * entries when the frontend binary changes.
         */
        if (frontend_for_cache && !isCacheFresh(cache_path, frontend_for_cache)) {
            unlink(cache_path);
            continue;
        }

        bool deps_ok = true;
        for (int dep_idx = 0; dependencies && dep_idx < dep_count; ++dep_idx) {
            if (!isCacheFresh(cache_path, dependencies[dep_idx])) {
                deps_ok = false;
                break;
            }
        }
        if (!deps_ok) {
            unlink(cache_path);
            continue;
        }

        FILE* f = fopen(cache_path, "rb");
        if (!f) {
            continue;
        }

        uint32_t magic = 0, ver = 0;
        uint64_t stored_source_hash = 0, stored_combined_hash = 0;
        if (fread(&magic, sizeof(magic), 1, f) != 1 ||
            fread(&ver, sizeof(ver), 1, f) != 1 ||
            fread(&stored_source_hash, sizeof(stored_source_hash), 1, f) != 1 ||
            fread(&stored_combined_hash, sizeof(stored_combined_hash), 1, f) != 1 ||
            magic != CACHE_MAGIC) {
            fclose(f);
            unlink(cache_path);
            continue;
        }

        if (ver > vm_ver) {
            if (strict) {
                fprintf(stderr,
                        "Cached bytecode requires VM version %u but current VM version is %u\n",
                        ver, vm_ver);
                fclose(f);
                abort_all = true;
                break;
            } else {
                fprintf(stderr,
                        "Warning: cached bytecode targets VM version %u but running version is %u\n",
                        ver, vm_ver);
            }
        }

        if (stored_source_hash != source_hash) {
            fclose(f);
            unlink(cache_path);
            continue;
        }

        chunk->version = ver;
        if (!verifySourcePath(f, source_path, strict)) {
            fclose(f);
            unlink(cache_path);
            continue;
        }

        if (!readChunkCore(f, chunk, ver, source_hash, stored_combined_hash, true)) {
            fclose(f);
            unlink(cache_path);
            continue;
        }

        fclose(f);

        restoreConstructorAliases(procedure_table);

        ok = true;
    }

    if (resolved_frontend) free(resolved_frontend);
    freeCandidates(candidates, candidate_count);
    free(prefix);
    free(sanitized_base);
    free(dir);

    if (!ok || abort_all) {
        resetChunk(chunk, chunk->constants_count);
    }
    if (abort_all) {
        return false;
    }
    return ok;
}

bool loadBytecodeFromFile(const char* file_path, BytecodeChunk* chunk) {
    bool ok = false;
    int const_count = 0;
    int read_consts = 0;
    uint32_t prev_ast_version = g_astCacheVersion;

    FILE* f = fopen(file_path, "rb");
    if (f) {
        uint32_t magic = 0, ver = 0;
        uint64_t source_hash = 0, combined_hash = 0;
        if (fread(&magic, sizeof(magic), 1, f) == 1 &&
            fread(&ver, sizeof(ver), 1, f) == 1 &&
            fread(&source_hash, sizeof(source_hash), 1, f) == 1 &&
            fread(&combined_hash, sizeof(combined_hash), 1, f) == 1 &&
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
                    g_astCacheVersion = prev_ast_version;
                    return false;
                } else {
                    fprintf(stderr,
                            "Warning: bytecode targets VM version %u but running version is %u\\n",
                            ver, vm_ver);
                }
            }
            chunk->version = ver;
            g_astCacheVersion = ver;
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
                        if (ok && chunk->constants_capacity > 0) {
                            chunk->builtin_lowercase_indices = (int*)malloc(sizeof(int) * chunk->constants_capacity);
                            if (!chunk->builtin_lowercase_indices) {
                                ok = false;
                            } else {
                                for (int i = 0; i < chunk->constants_capacity; ++i) {
                                    chunk->builtin_lowercase_indices[i] = -1;
                                }
                            }
                        }
                        if (ok && ver >= 8) {
                            int builtin_map_count = 0;
                            if (fread(&builtin_map_count, sizeof(builtin_map_count), 1, f) != 1) {
                                ok = false;
                            } else {
                                for (int i = 0; i < builtin_map_count; ++i) {
                                    int original_idx = 0;
                                    int lower_idx = 0;
                                    if (fread(&original_idx, sizeof(original_idx), 1, f) != 1 ||
                                        fread(&lower_idx, sizeof(lower_idx), 1, f) != 1) {
                                        ok = false;
                                        break;
                                    }
                                    if (!chunk->builtin_lowercase_indices) {
                                        continue;
                                    }
                                    if (original_idx >= 0 && original_idx < chunk->constants_count) {
                                        chunk->builtin_lowercase_indices[original_idx] = lower_idx;
                                    }
                                }
                            }
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

    g_astCacheVersion = prev_ast_version;

    if (ok) {
        restoreConstructorAliases(procedure_table);
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

static bool serializeBytecodeChunk(FILE* f,
                                   const char* source_path,
                                   const BytecodeChunk* chunk,
                                   uint64_t source_hash,
                                   uint64_t combined_hash) {
    if (!f) return false;
    uint32_t magic = CACHE_MAGIC, ver = chunk->version;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&source_hash, sizeof(source_hash), 1, f);
    fwrite(&combined_hash, sizeof(combined_hash), 1, f);
    if (!writeSourcePath(f, source_path)) {
        return false;
    }
    return writeChunkCore(f, chunk);
}

void saveBytecodeToCache(const char* source_path, const char* compiler_id, const BytecodeChunk* chunk) {
    if (!chunk) return;

    uint64_t source_hash = 0, combined_hash = 0;
    if (!computeSourceHash(source_path, true, &source_hash)) {
        return;
    }
    combined_hash = computeCombinedHash(source_hash, chunk);

    char safe_id[32];
    char* dir = ensureCacheDirectory(compiler_id, safe_id, sizeof(safe_id));
    if (!dir) return;

    char* sanitized_base = sanitizeFileComponent(basenameForPath(source_path));
    if (!sanitized_base) {
        free(dir);
        return;
    }

    char source_hex[17];
    char combined_hex[17];
    snprintf(source_hex, sizeof(source_hex), "%016llx", (unsigned long long)source_hash);
    snprintf(combined_hex, sizeof(combined_hex), "%016llx", (unsigned long long)combined_hash);

    uint64_t path_hash = computePathHash(source_path, sanitized_base);
    char path_hex[17];
    snprintf(path_hex, sizeof(path_hex), "%016llx", (unsigned long long)path_hash);

    size_t prefix_len = strlen(safe_id) + strlen(sanitized_base) + strlen(path_hex) + strlen(source_hex) + 5;
    char* prefix = (char*)malloc(prefix_len);
    if (!prefix) {
        free(sanitized_base);
        free(dir);
        return;
    }
    snprintf(prefix, prefix_len, "%s-%s-%s-%s-", safe_id, sanitized_base, path_hex, source_hex);

    size_t path_len = strlen(dir) + strlen(prefix) + strlen(combined_hex) + 5;
    char* cache_path = (char*)malloc(path_len);
    if (!cache_path) {
        free(prefix);
        free(sanitized_base);
        free(dir);
        return;
    }
    snprintf(cache_path, path_len, "%s/%s%s.bc", dir, prefix, combined_hex);

    size_t candidate_count = 0;
    CacheCandidate* candidates = gatherCacheCandidates(dir, prefix, &candidate_count);
    if (candidates) {
        for (size_t i = 0; i < candidate_count; ++i) {
            if (strcmp(candidates[i].path, cache_path) != 0) {
                unlink(candidates[i].path);
            }
        }
    }
    freeCandidates(candidates, candidate_count);

    FILE* f = fopen(cache_path, "wb");
    if (!f) {
        free(cache_path);
        free(prefix);
        free(sanitized_base);
        free(dir);
        return;
    }
    if (!serializeBytecodeChunk(f, source_path, chunk, source_hash, combined_hash)) {
        fclose(f);
        unlink(cache_path);
        free(cache_path);
        free(prefix);
        free(sanitized_base);
        free(dir);
        return;
    }
    fclose(f);
    free(cache_path);
    free(prefix);
    free(sanitized_base);
    free(dir);
}

bool saveBytecodeToFile(const char* file_path, const char* source_path, const BytecodeChunk* chunk) {
    if (!chunk) return false;
    uint64_t source_hash = 0, combined_hash = 0;
    computeSourceHash(source_path, false, &source_hash);
    combined_hash = computeCombinedHash(source_hash, chunk);

    FILE* f = fopen(file_path, "wb");
    if (!f) return false;
    bool ok = serializeBytecodeChunk(f, source_path, chunk, source_hash, combined_hash);
    fclose(f);
    return ok;
}
