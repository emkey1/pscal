#include "clike/preproc.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

static bool isDefined(const char *name, const char **defines, int count) {
    for (int i = 0; i < count; ++i) {
        if (strcmp(defines[i], name) == 0) return true;
    }
    return false;
}

char* clike_preprocess(const char *source, const char **defines, int count) {
    if (!source) return NULL;
    size_t len = strlen(source);
    char *out = (char*)malloc(len + 1);
    if (!out) return NULL;
    size_t out_pos = 0;

    typedef struct { bool outer_active; bool branch_taken; } IfState;
    IfState stack[64];
    int sp = 0;
    bool emit = true;

    const char *p = source;
    while (*p) {
        const char *line_start = p;
        while (*p && *p != '\n') p++;
        const char *line_end = p;
        bool has_newline = (*p == '\n');

        const char *trim = line_start;
        while (trim < line_end && (*trim == ' ' || *trim == '\t')) trim++;

        if (trim < line_end && *trim == '#') {
            trim++;
            while (trim < line_end && isspace((unsigned char)*trim)) trim++;
            const char *word_start = trim;
            while (trim < line_end && isalpha((unsigned char)*trim)) trim++;
            size_t wlen = trim - word_start;
            char directive[16];
            if (wlen > 15) wlen = 15;
            memcpy(directive, word_start, wlen);
            directive[wlen] = '\0';

            while (trim < line_end && isspace((unsigned char)*trim)) trim++;
            const char *arg_start = trim;
            while (trim < line_end && !isspace((unsigned char)*trim)) trim++;
            size_t arg_len = trim - arg_start;
            char arg[64];
            if (arg_len > 63) arg_len = 63;
            memcpy(arg, arg_start, arg_len);
            arg[arg_len] = '\0';

            if (strcmp(directive, "ifdef") == 0) {
                bool cond = isDefined(arg, defines, count);
                stack[sp++] = (IfState){emit, cond && emit};
                emit = cond && emit;
            } else if (strcmp(directive, "ifndef") == 0) {
                bool cond = !isDefined(arg, defines, count);
                stack[sp++] = (IfState){emit, cond && emit};
                emit = cond && emit;
            } else if (strcmp(directive, "elif") == 0 || strcmp(directive, "elseif") == 0) {
                if (sp > 0) {
                    IfState *st = &stack[sp-1];
                    if (!st->outer_active || st->branch_taken) {
                        emit = false;
                    } else {
                        bool cond = isDefined(arg, defines, count);
                        emit = st->outer_active && cond;
                        if (emit) st->branch_taken = true;
                    }
                }
            } else if (strcmp(directive, "else") == 0) {
                if (sp > 0) {
                    IfState *st = &stack[sp-1];
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
            }
            // no output for directive lines
        } else {
            if (emit) {
                memcpy(out + out_pos, line_start, line_end - line_start);
                out_pos += line_end - line_start;
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
