#include "clike/preproc.h"
#include "clike/errors.h"
#include "core/preproc.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
} StringBuilder;

static int equalsKeyword(const char *text, const char *kw, size_t len) {
    for (size_t i = 0; i < len; i++) {
        unsigned char a = (unsigned char)text[i];
        unsigned char b = (unsigned char)kw[i];
        if (tolower(a) != tolower(b)) return 0;
    }
    return kw[len] == '\0';
}

static void sbEnsureCapacity(StringBuilder *sb, size_t extra) {
    if (!sb->data) {
        sb->capacity = extra + 1;
        sb->data = (char *)malloc(sb->capacity);
        sb->length = 0;
        return;
    }
    if (sb->length + extra + 1 <= sb->capacity) return;
    size_t new_cap = sb->capacity ? sb->capacity : 16;
    while (new_cap < sb->length + extra + 1) new_cap *= 2;
    char *resized = (char *)realloc(sb->data, new_cap);
    if (!resized) return;
    sb->data = resized;
    sb->capacity = new_cap;
}

static void sbAppend(StringBuilder *sb, const char *text, size_t len) {
    if (!text || len == 0) return;
    sbEnsureCapacity(sb, len);
    if (!sb->data) return;
    memcpy(sb->data + sb->length, text, len);
    sb->length += len;
}

static char* joinPath(const char *dir, const char *file) {
    if (!file) return NULL;
    if (!dir || !*dir) return strdup(file);
    size_t dir_len = strlen(dir);
    size_t file_len = strlen(file);
    int needs_sep = !(dir_len > 0 && (dir[dir_len - 1] == '/' || dir[dir_len - 1] == '\\'));
    size_t total = dir_len + (needs_sep ? 1 : 0) + file_len;
    char *out = (char *)malloc(total + 1);
    if (!out) return NULL;
    memcpy(out, dir, dir_len);
    size_t pos = dir_len;
    if (needs_sep) out[pos++] = '/';
    memcpy(out + pos, file, file_len);
    out[total] = '\0';
    return out;
}

static char* duplicateDirname(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
#ifdef _WIN32
    const char *backslash = strrchr(path, '\\');
    if (backslash && (!slash || backslash > slash)) slash = backslash;
#endif
    if (!slash) return strdup(".");
    size_t len = (size_t)(slash - path);
    if (len == 0) return strdup("/");
    char *dir = (char *)malloc(len + 1);
    if (!dir) return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char* loadFileText(const char *path) {
    if (!path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long len = ftell(f);
    if (len < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t read = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (read != (size_t)len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

static char* expandIncludesInternal(const char *source, const char *current_dir, int depth) {
    if (!source) return NULL;
    if (depth > 32) {
        fprintf(stderr, "Include error: maximum include depth exceeded\n");
        clike_error_count++;
        return NULL;
    }
    StringBuilder sb = {0};
    const char *p = source;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        const char *line_end = p;
        const char *trim = line_start;
        while (trim < line_end && (*trim == ' ' || *trim == '\t')) trim++;
        bool handled = false;
        if (trim < line_end && *trim == '#') {
            const char *kw = trim + 1;
            while (kw < line_end && isspace((unsigned char)*kw)) kw++;
            const char *kw_end = kw;
            while (kw_end < line_end && isalpha((unsigned char)*kw_end)) kw_end++;
            size_t kw_len = (size_t)(kw_end - kw);
            if (kw_len == 7 && equalsKeyword(kw, "include", 7)) {
                const char *path_start = kw_end;
                while (path_start < line_end && isspace((unsigned char)*path_start)) path_start++;
                if (path_start < line_end && (*path_start == '"' || *path_start == '<')) {
                    char end_delim = (*path_start == '"') ? '"' : '>';
                    path_start++;
                    const char *path_end = path_start;
                    while (path_end < line_end && *path_end != end_delim) path_end++;
                    if (path_end < line_end) {
                        size_t inc_len = (size_t)(path_end - path_start);
                        char *inc = (char *)malloc(inc_len + 1);
                        if (inc) {
                            memcpy(inc, path_start, inc_len);
                            inc[inc_len] = '\0';
                            int is_system = (end_delim == '>');
                            if (is_system) {
                                handled = true;
                            } else {
                                char *full = NULL;
                                if (inc[0] == '/' || inc[0] == '\\') {
                                    full = strdup(inc);
                                } else {
                                    full = joinPath(current_dir ? current_dir : ".", inc);
                                }
                                if (!full) {
                                    fprintf(stderr, "Include error: could not resolve path '%s'\n", inc);
                                    clike_error_count++;
                                } else {
                                    char *included = loadFileText(full);
                                    if (!included) {
                                        fprintf(stderr, "Include error: could not open '%s'\n", full);
                                        clike_error_count++;
                                    } else {
                                        char *inc_dir = duplicateDirname(full);
                                        char *expanded = expandIncludesInternal(included, inc_dir ? inc_dir : current_dir, depth + 1);
                                        if (expanded) {
                                            sbAppend(&sb, expanded, strlen(expanded));
                                            free(expanded);
                                        }
                                        free(inc_dir);
                                        free(included);
                                    }
                                    free(full);
                                }
                            }
                            free(inc);
                        }
                        handled = true;
                    }
                }
            }
        }
        if (!handled) {
            sbAppend(&sb, line_start, (size_t)(line_end - line_start));
        }
        if (*line_end == '\n') {
            sbAppend(&sb, "\n", 1);
            p = line_end + 1;
        } else {
            p = line_end;
        }
    }
    sbEnsureCapacity(&sb, 1);
    if (sb.data) sb.data[sb.length] = '\0';
    return sb.data ? sb.data : strdup("");
}

char* clikePreprocess(const char *source, const char *source_path, const char **defines, int define_count) {
    if (!source) return NULL;
    char *base_dir = duplicateDirname(source_path);
    char *expanded = expandIncludesInternal(source, base_dir, 0);
    free(base_dir);
    const char *input = expanded ? expanded : source;
    char *result = preprocessConditionals(input, defines, define_count);
    if (expanded) free(expanded);
    return result;
}
