#include "clike/preproc.h"
#include "clike/errors.h"
#include "core/preproc.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CLIKE_MAX_INCLUDE_DEPTH 32

static char *duplicate_range(const char *start, size_t len) {
    if (!start || len == 0) {
        char *empty = (char *)malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) return NULL;
    memcpy(copy, start, len);
    copy[len] = '\0';
    return copy;
}

static char *directory_from_path(const char *path) {
    if (!path) return NULL;
    const char *last_slash = strrchr(path, '/');
#ifdef _WIN32
    const char *last_backslash = strrchr(path, '\\');
    if (!last_slash || (last_backslash && last_backslash > last_slash)) {
        last_slash = last_backslash;
    }
#endif
    if (!last_slash) return NULL;
    size_t len = (size_t)(last_slash - path);
    char *dir = (char *)malloc(len + 1);
    if (!dir) return NULL;
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

static char *join_path(const char *dir, const char *rel) {
    if (!rel) return NULL;
    if (!dir || !*dir) return duplicate_range(rel, strlen(rel));
    size_t dir_len = strlen(dir);
    size_t rel_len = strlen(rel);
    size_t need_sep = (dir[dir_len - 1] == '/'
#ifdef _WIN32
                       || dir[dir_len - 1] == '\\'
#endif
                       ) ? 0 : 1;
    size_t total = dir_len + need_sep + rel_len + 1;
    char *out = (char *)malloc(total);
    if (!out) return NULL;
    memcpy(out, dir, dir_len);
    if (need_sep) out[dir_len++] = '/';
    memcpy(out + dir_len, rel, rel_len);
    out[dir_len + rel_len] = '\0';
    return out;
}

static char *read_file(const char *path) {
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

static void append_text(char **buffer, size_t *length, size_t *capacity,
                        const char *text, size_t text_len) {
    if (!text || text_len == 0) return;
    if (!*buffer) {
        *capacity = text_len + 1;
        *buffer = (char *)malloc(*capacity);
        if (!*buffer) return;
        (*buffer)[0] = '\0';
        *length = 0;
    }
    if (*length + text_len + 1 > *capacity) {
        size_t new_cap = *capacity ? *capacity : 16;
        while (*length + text_len + 1 > new_cap) {
            new_cap *= 2;
        }
        char *resized = (char *)realloc(*buffer, new_cap);
        if (!resized) return;
        *buffer = resized;
        *capacity = new_cap;
    }
    memcpy(*buffer + *length, text, text_len);
    *length += text_len;
    (*buffer)[*length] = '\0';
}

static char *load_include_source(const char *base_dir,
                                 const char *include_name,
                                 char **resolved_path_out) {
    if (!include_name) return NULL;
    char *candidate = join_path(base_dir, include_name);
    char *content = NULL;
    if (candidate) {
        content = read_file(candidate);
        if (content) {
            if (resolved_path_out) {
                *resolved_path_out = candidate;
            } else {
                free(candidate);
            }
            return content;
        }
        free(candidate);
    }
    char *direct = duplicate_range(include_name, strlen(include_name));
    if (!direct) return NULL;
    content = read_file(direct);
    if (content) {
        if (resolved_path_out) {
            *resolved_path_out = direct;
        } else {
            free(direct);
        }
        return content;
    }
    free(direct);
    return NULL;
}

static char *expand_includes_recursive(const char *source_path,
                                       const char *source,
                                       int depth) {
    if (!source) return NULL;
    if (depth > CLIKE_MAX_INCLUDE_DEPTH) {
        fprintf(stderr, "Include depth exceeded while processing '%s'\n",
                source_path ? source_path : "<input>");
        clike_error_count++;
        return duplicate_range(source, strlen(source));
    }

    char *base_dir = directory_from_path(source_path);
    char *result = NULL;
    size_t length = 0;
    size_t capacity = 0;

    const char *p = source;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        const char *line_end = p;
        int has_newline = 0;
        if (*p == '\n') {
            has_newline = 1;
            p++;
        }

        const char *trim = line_start;
        while (trim < line_end && (*trim == ' ' || *trim == '\t')) trim++;
        int handled_include = 0;
        if (trim < line_end && strncmp(trim, "#include", 8) == 0) {
            const char *cursor = trim + 8;
            while (cursor < line_end && isspace((unsigned char)*cursor)) cursor++;
            if (cursor < line_end && *cursor == '"') {
                cursor++;
                const char *path_start = cursor;
                while (cursor < line_end && *cursor != '"') cursor++;
                if (cursor < line_end && *cursor == '"') {
                    size_t path_len = (size_t)(cursor - path_start);
                    char *include_name = duplicate_range(path_start, path_len);
                    char *resolved_path = NULL;
                    char *included_source = load_include_source(base_dir, include_name, &resolved_path);
                    if (included_source) {
                        char *expanded = expand_includes_recursive(resolved_path,
                                                                  included_source,
                                                                  depth + 1);
                        if (expanded) {
                            append_text(&result, &length, &capacity, expanded, strlen(expanded));
                            free(expanded);
                        }
                        free(included_source);
                        free(resolved_path);
                    } else {
                        fprintf(stderr, "Could not open include '%s'\n",
                                include_name ? include_name : "");
                        clike_error_count++;
                        append_text(&result, &length, &capacity,
                                    line_start, (size_t)(line_end - line_start));
                    }
                    free(include_name);
                    if (has_newline) {
                        append_text(&result, &length, &capacity, "\n", 1);
                    }
                    handled_include = 1;
                }
            }
        }

        if (!handled_include) {
            append_text(&result, &length, &capacity,
                        line_start, (size_t)(line_end - line_start));
            if (has_newline) {
                append_text(&result, &length, &capacity, "\n", 1);
            }
        }
    }

    free(base_dir);
    if (!result) {
        return duplicate_range(source, strlen(source));
    }
    return result;
}

char* clikePreprocess(const char *source_path,
                      const char *source,
                      const char **defines,
                      int define_count) {
    if (!source) return NULL;
    char *expanded = expand_includes_recursive(source_path, source, 0);
    const char *input = expanded ? expanded : source;
    char *processed = preprocessConditionals(input, defines, define_count);
    if (expanded) free(expanded);
    return processed;
}

