#include "core/preproc.h"
#include "backend_ast/builtin.h"
#include "ext_builtins/registry.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    bool outer_active;
    bool branch_taken;
} IfState;

static bool isDefinedSimple(const char *name, const char **defines, int count) {
    if (!name || !*name || !defines) {
        return false;
    }
    for (int i = 0; i < count; ++i) {
        if (defines[i] && strcmp(defines[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static bool evaluateExtendedCondition(const char *arg, bool *handled) {
    if (handled) {
        *handled = false;
    }
    if (!arg) {
        return false;
    }

    const char *p = arg;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (strncasecmp(p, "extended", 8) != 0 || (p[8] && !isspace((unsigned char)p[8]))) {
        return false;
    }
    p += 8;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (handled) {
        *handled = true;
    }
    if (!*p) {
        return false;
    }

    const char *cat_start = p;
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    size_t cat_len = (size_t)(p - cat_start);
    if (cat_len == 0) {
        return false;
    }
    char *category = (char *)malloc(cat_len + 1);
    if (!category) {
        return false;
    }
    memcpy(category, cat_start, cat_len);
    category[cat_len] = '\0';

    while (*p && isspace((unsigned char)*p)) {
        p++;
    }

    char *function = NULL;
    if (*p && !(p[0] == '/' && (p[1] == '/' || p[1] == '*'))) {
        const char *fn_start = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        size_t fn_len = (size_t)(p - fn_start);
        if (fn_len > 0) {
            function = (char *)malloc(fn_len + 1);
            if (!function) {
                free(category);
                return false;
            }
            memcpy(function, fn_start, fn_len);
            function[fn_len] = '\0';
        }
    }

    registerExtendedBuiltins();

    bool present = extBuiltinHasCategory(category);
    if (present && function) {
        present = extBuiltinHasFunction(category, function);
    }

    free(category);
    if (function) {
        free(function);
    }
    return present;
}

static bool evaluateCondition(const char *arg, const char **defines, int define_count) {
    if (!arg) {
        return false;
    }

    bool handled = false;
    bool extended_result = evaluateExtendedCondition(arg, &handled);
    if (handled) {
        return extended_result;
    }

    const char *p = arg;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        return false;
    }
    const char *name_start = p;
    while (*p && !isspace((unsigned char)*p)) {
        p++;
    }
    size_t name_len = (size_t)(p - name_start);
    if (name_len == 0) {
        return false;
    }
    char *name = (char *)malloc(name_len + 1);
    if (!name) {
        return false;
    }
    memcpy(name, name_start, name_len);
    name[name_len] = '\0';
    bool defined = isDefinedSimple(name, defines, define_count);
    free(name);
    return defined;
}

char *preprocessConditionals(const char *source, const char **defines, int define_count) {
    if (!source) {
        return NULL;
    }
    size_t len = strlen(source);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t out_pos = 0;

    IfState stack[64];
    int sp = 0;
    bool emit = true;

    const char *p = source;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') {
            p++;
        }
        const char *line_end = p;
        bool has_newline = (*p == '\n');

        const char *trim = line_start;
        while (trim < line_end && (*trim == ' ' || *trim == '\t')) {
            trim++;
        }

        if (trim < line_end && *trim == '#') {
            trim++;
            while (trim < line_end && isspace((unsigned char)*trim)) {
                trim++;
            }
            const char *word_start = trim;
            while (trim < line_end && isalpha((unsigned char)*trim)) {
                trim++;
            }
            size_t wlen = (size_t)(trim - word_start);
            char directive[16];
            if (wlen >= sizeof(directive)) {
                wlen = sizeof(directive) - 1;
            }
            memcpy(directive, word_start, wlen);
            directive[wlen] = '\0';

            while (trim < line_end && isspace((unsigned char)*trim)) {
                trim++;
            }
            size_t arg_len = (size_t)(line_end - trim);
            char *arg = NULL;
            if (arg_len > 0) {
                arg = (char *)malloc(arg_len + 1);
                if (arg) {
                    memcpy(arg, trim, arg_len);
                    arg[arg_len] = '\0';
                }
            }
            const char *arg_text = arg ? arg : "";

            if (strcmp(directive, "ifdef") == 0) {
                bool cond = evaluateCondition(arg_text, defines, define_count);
                if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    stack[sp++] = (IfState){emit, cond && emit};
                    emit = cond && emit;
                }
            } else if (strcmp(directive, "ifndef") == 0) {
                bool cond = !evaluateCondition(arg_text, defines, define_count);
                if (sp < (int)(sizeof(stack) / sizeof(stack[0]))) {
                    stack[sp++] = (IfState){emit, cond && emit};
                    emit = cond && emit;
                }
            } else if (strcmp(directive, "elif") == 0 || strcmp(directive, "elseif") == 0) {
                if (sp > 0) {
                    IfState *st = &stack[sp - 1];
                    if (!st->outer_active || st->branch_taken) {
                        emit = false;
                    } else {
                        bool cond = evaluateCondition(arg_text, defines, define_count);
                        emit = st->outer_active && cond;
                        if (emit) {
                            st->branch_taken = true;
                        }
                    }
                }
            } else if (strcmp(directive, "else") == 0) {
                if (sp > 0) {
                    IfState *st = &stack[sp - 1];
                    if (!st->outer_active || st->branch_taken) {
                        emit = false;
                    } else {
                        emit = true;
                        st->branch_taken = true;
                    }
                }
            } else if (strcmp(directive, "endif") == 0) {
                if (sp > 0) {
                    IfState st = stack[--sp];
                    emit = st.outer_active;
                }
            } else if (strcmp(directive, "import") == 0) {
                if (emit) {
                    size_t copy_len = (size_t)(line_end - line_start);
                    memcpy(out + out_pos, line_start, copy_len);
                    out_pos += copy_len;
                }
            }

            if (arg) {
                free(arg);
            }
        } else {
            if (emit) {
                size_t copy_len = (size_t)(line_end - line_start);
                memcpy(out + out_pos, line_start, copy_len);
                out_pos += copy_len;
            }
        }

        if (has_newline) {
            out[out_pos++] = '\n';
            p++;
        }
    }

    out[out_pos] = '\0';
    return out;
}
