/* Auto-generated include: word expansion and substitution logic. Included from shell.c. */
#if defined(PSCAL_TARGET_IOS)
#undef stdin
#undef stdout
#undef stderr
#define stdin shellVprocStdin()
#define stdout shellVprocStdout()
#define stderr shellVprocStderr()
#endif

static char *shellExpandWord(const char *text,
                             uint8_t flags,
                             const char *meta,
                             size_t meta_len,
                             bool **out_quoted_map,
                             size_t *out_quoted_len,
                             bool *out_array_zero);

static char *shellNormalizeDollarCommandInline(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len && command[i + 1] == '\n') {
            i++;
            continue;
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static char *shellNormalizeBacktickCommandInline(const char *command, size_t len) {
    if (!command) {
        return NULL;
    }
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; ++i) {
        char c = command[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            out[j++] = '\'';
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            out[j++] = '"';
            continue;
        }
        if (c == '\\' && i + 1 < len) {
            char next = command[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
            if (next == '\\' || next == '`' || next == '$') {
                out[j++] = next;
                i++;
                continue;
            }
        }
        out[j++] = c;
    }
    out[j] = '\0';
    char *shrunk = (char *)realloc(out, j + 1);
    return shrunk ? shrunk : out;
}

static bool shellParseInlineDollarCommand(const char *text,
                                         size_t start,
                                         size_t text_len,
                                         size_t *out_span,
                                         char **out_command) {
    if (!text || start + 1 >= text_len || text[start] != '$' || text[start + 1] != '(') {
        return false;
    }
    size_t i = start + 2;
    int depth = 1;
    bool in_single = false;
    bool in_double = false;
    while (i < text_len) {
        char c = text[i];
        if (c == '\\' && i + 1 < text_len) {
            if (text[i + 1] == '\n') {
                i += 2;
                continue;
            }
            if (!in_single) {
                i += 2;
                continue;
            }
        }
        if (!in_double && c == '\'') {
            in_single = !in_single;
            i++;
            continue;
        }
        if (!in_single && c == '"') {
            in_double = !in_double;
            i++;
            continue;
        }
        if (in_single || in_double) {
            i++;
            continue;
        }
        if (c == '(') {
            depth++;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
        }
        i++;
    }
    if (depth != 0 || i >= text_len || text[i] != ')') {
        return false;
    }
    size_t span = (i + 1) - start;
    if (out_span) {
        *out_span = span;
    }
    if (out_command) {
        size_t inner_start = start + 2;
        size_t inner_len = i - inner_start;
        *out_command = shellNormalizeDollarCommandInline(text + inner_start, inner_len);
        if (!*out_command) {
            return false;
        }
    }
    return true;
}

static bool shellParseInlineBacktickCommand(const char *text,
                                            size_t start,
                                            size_t text_len,
                                            size_t *out_span,
                                            char **out_command) {
    if (!text || start >= text_len || text[start] != '`') {
        return false;
    }
    size_t i = start + 1;
    while (i < text_len) {
        char c = text[i];
        if (c == '`') {
            break;
        }
        if (c == '\\' && i + 1 < text_len) {
            i += 2;
            continue;
        }
        i++;
    }
    if (i >= text_len || text[i] != '`') {
        return false;
    }
    size_t span = (i + 1) - start;
    if (out_span) {
        *out_span = span;
    }
    if (out_command) {
        size_t inner_len = (i - start) - 1;
        *out_command = shellNormalizeBacktickCommandInline(text + start + 1, inner_len);
        if (!*out_command) {
            return false;
        }
    }
    return true;
}

static char *shellExpandHereDocument(const char *body, bool quoted) {
    if (quoted) {
        return body ? strdup(body) : strdup("");
    }
    return shellExpandWord(body, SHELL_WORD_FLAG_HAS_ARITHMETIC, NULL, 0, NULL, NULL, NULL);
}

static char *shellExpandParameter(const char *input,
                                  size_t *out_consumed,
                                  bool *out_is_array_expansion,
                                  size_t *out_array_count) {
    if (out_is_array_expansion) {
        *out_is_array_expansion = false;
    }
    if (out_array_count) {
        *out_array_count = 0;
    }
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || !*input) {
        return NULL;
    }
    if (*input == '{') {
        const char *closing = strchr(input + 1, '}');
        if (!closing) {
            return NULL;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(closing - input + 1);
        }
        const char *inner = input + 1;
        size_t inner_len = (size_t)(closing - inner);
        if (inner_len == 0) {
            return strdup("");
        }
        if (*inner == '#') {
            const char *name_start = inner + 1;
            if (name_start >= closing) {
                return NULL;
            }
            const char *cursor = name_start;
            while (cursor < closing &&
                   (isalnum((unsigned char)*cursor) || *cursor == '_')) {
                cursor++;
            }
            if (cursor == name_start) {
                return NULL;
            }
            size_t name_len = (size_t)(cursor - name_start);
            if (cursor == closing) {
                char *value = shellLookupParameterValue(name_start, name_len);
                if (!value) {
                    return NULL;
                }
                size_t val_len = strlen(value);
                free(value);
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%zu", val_len);
                return strdup(buffer);
            }
            if (*cursor != '[') {
                return NULL;
            }
            const char *subscript_start = cursor + 1;
            const char *subscript_end =
                memchr(subscript_start, ']', (size_t)(closing - subscript_start));
            if (!subscript_end || subscript_end > closing) {
                return NULL;
            }
            size_t subscript_len = (size_t)(subscript_end - subscript_start);
            const char *after_bracket = subscript_end + 1;
            while (after_bracket < closing &&
                   isspace((unsigned char)*after_bracket)) {
                after_bracket++;
            }
            if (after_bracket != closing) {
                return NULL;
            }
            while (subscript_len > 0 &&
                   isspace((unsigned char)subscript_start[0])) {
                subscript_start++;
                subscript_len--;
            }
            while (subscript_len > 0 &&
                   isspace((unsigned char)subscript_start[subscript_len - 1])) {
                subscript_len--;
            }
            if (subscript_len == 1 &&
                (subscript_start[0] == '@' || subscript_start[0] == '*')) {
                const ShellArrayVariable *array_var =
                    shellArrayRegistryLookup(name_start, name_len);
                size_t count = 0;
                if (array_var) {
                    count = array_var->count;
                } else {
                    char *raw = shellLookupRawEnvironmentValue(name_start, name_len);
                    if (!raw) {
                        return NULL;
                    }
                    char **items = NULL;
                    if (!shellParseArrayValues(raw, &items, &count)) {
                        free(raw);
                        return NULL;
                    }
                    free(raw);
                    shellFreeArrayValues(items, count);
                }
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%zu", count);
                return strdup(buffer);
            }
            char *element = shellExpandArraySubscriptValue(
                name_start, name_len, subscript_start, subscript_len, NULL, NULL);
            if (!element) {
                return NULL;
            }
            size_t elem_len = strlen(element);
            free(element);
            char buffer[32];
            snprintf(buffer, sizeof(buffer), "%zu", elem_len);
            return strdup(buffer);
        }

        if (*inner == '!') {
            const char *name_start = inner + 1;
            if (name_start >= closing) {
                return NULL;
            }
            const char *cursor = name_start;
            while (cursor < closing && (isalnum((unsigned char)*cursor) || *cursor == '_')) {
                cursor++;
            }
            if (cursor == name_start) {
                return NULL;
            }
            size_t name_len = (size_t)(cursor - name_start);
            if (cursor == closing || *cursor != '[') {
                return NULL;
            }
            const char *subscript_start = cursor + 1;
            const char *subscript_end = memchr(subscript_start, ']', (size_t)(closing - subscript_start));
            if (!subscript_end || subscript_end > closing) {
                return NULL;
            }
            size_t subscript_len = (size_t)(subscript_end - subscript_start);
            const char *after_bracket = subscript_end + 1;
            while (after_bracket < closing && isspace((unsigned char)*after_bracket)) {
                after_bracket++;
            }
            if (after_bracket != closing) {
                return NULL;
            }
            if (!(subscript_len == 1 && (subscript_start[0] == '@' || subscript_start[0] == '*'))) {
                return NULL;
            }
            const ShellArrayVariable *array_var = shellArrayRegistryLookup(name_start, name_len);
            char **items = NULL;
            char **keys = NULL;
            size_t count = 0;
            ShellArrayKind kind = SHELL_ARRAY_KIND_INDEXED;
            bool using_registry = false;
            if (array_var) {
                items = array_var->values;
                keys = array_var->keys;
                count = array_var->count;
                kind = array_var->kind;
                using_registry = true;
            } else {
                char *raw = shellLookupRawEnvironmentValue(name_start, name_len);
                if (!raw) {
                    return NULL;
                }
                if (!shellParseArrayLiteral(raw, &items, &keys, &count, &kind)) {
                    free(raw);
                    return NULL;
                }
                free(raw);
            }
            char *joined = NULL;
            if (kind == SHELL_ARRAY_KIND_ASSOCIATIVE) {
                if (subscript_start[0] == '@') {
                    joined = shellJoinArrayValuesWithSeparator(keys, count, SHELL_ARRAY_ELEMENT_SEP);
                } else {
                    joined = shellJoinArrayValues(keys, count);
                }
            } else {
                char separator = (subscript_start[0] == '@') ? SHELL_ARRAY_ELEMENT_SEP : ' ';
                joined = shellJoinNumericIndices(count, separator);
            }
            if (!using_registry) {
                shellFreeArrayValues(items, count);
                if (keys) {
                    shellFreeArrayValues(keys, count);
                }
            }
            if (!joined) {
                joined = strdup("");
            }
            return joined;
        }

        const char *inner_end = inner + inner_len;
        const char *default_pos = NULL;
        bool default_requires_value = false;
        size_t bracket_depth = 0;
        for (const char *scan = inner; scan < inner_end; ++scan) {
            char ch = *scan;
            if (ch == '[') {
                bracket_depth++;
                continue;
            }
            if (ch == ']' && bracket_depth > 0) {
                bracket_depth--;
                continue;
            }
            if (bracket_depth > 0) {
                continue;
            }
            if (ch == ':' && (scan + 1) < inner_end && scan[1] == '-') {
                default_pos = scan;
                default_requires_value = true;
                break;
            }
            if (ch == '-' && (scan == inner || scan[-1] != ':')) {
                default_pos = scan;
                default_requires_value = false;
                break;
            }
        }
        if (default_pos) {
            size_t name_len = (size_t)(default_pos - inner);
            if (name_len == 0) {
                return NULL;
            }
            bool simple_name = false;
            if (name_len == 1) {
                unsigned char first = (unsigned char)inner[0];
                if (isalnum(first) || first == '_' || first == '*' || first == '@' ||
                    first == '#' || first == '?' || first == '$') {
                    simple_name = true;
                }
            } else {
                simple_name = true;
                for (size_t i = 0; i < name_len; ++i) {
                    unsigned char ch = (unsigned char)inner[i];
                    if (!isalnum(ch) && ch != '_') {
                        simple_name = false;
                        break;
                    }
                }
            }
            if (simple_name) {
                bool is_set = false;
                char *value = shellLookupParameterValueInternal(inner, name_len, &is_set);
                if (!value) {
                    return NULL;
                }
                bool use_default = !is_set || (default_requires_value && value[0] == '\0');
                if (!use_default) {
                    return value;
                }
                free(value);
                const char *default_start = default_pos + (default_requires_value ? 2 : 1);
                size_t default_len = (size_t)(inner_end - default_start);
                char *raw_default = (char *)malloc(default_len + 1);
                if (!raw_default) {
                    return NULL;
                }
                if (default_len > 0) {
                    memcpy(raw_default, default_start, default_len);
                }
                raw_default[default_len] = '\0';
                char *expanded_default = shellExpandWord(raw_default, 0, NULL, 0, NULL, NULL, NULL);
                free(raw_default);
                if (!expanded_default) {
                    return NULL;
                }
                return expanded_default;
            }
        }

        const char *colon = memchr(inner, ':', inner_len);
        if (colon && colon > inner) {
            const char *offset_start = colon + 1;
            if (offset_start >= inner_end ||
                !isdigit((unsigned char)*offset_start)) {
                return NULL;
            }
            size_t offset_value = 0;
            size_t offset_digits = 0;
            const char *cursor = offset_start;
            while (cursor < inner_end && isdigit((unsigned char)*cursor)) {
                if (offset_value > (SIZE_MAX - 9) / 10) {
                    offset_value = SIZE_MAX;
                } else {
                    offset_value = offset_value * 10 + (size_t)(*cursor - '0');
                }
                cursor++;
                offset_digits++;
            }
            if (offset_digits == 0) {
                return NULL;
            }
            const char *after_offset = cursor;
            bool have_length = false;
            size_t length_value = 0;
            if (after_offset < inner_end) {
                if (*after_offset != ':') {
                    return NULL;
                }
                const char *length_start = after_offset + 1;
                if (length_start >= inner_end ||
                    !isdigit((unsigned char)*length_start)) {
                    return NULL;
                }
                const char *length_cursor = length_start;
                size_t length_digits = 0;
                while (length_cursor < inner_end &&
                       isdigit((unsigned char)*length_cursor)) {
                    if (length_value > (SIZE_MAX - 9) / 10) {
                        length_value = SIZE_MAX;
                    } else {
                        length_value =
                            length_value * 10 + (size_t)(*length_cursor - '0');
                    }
                    length_cursor++;
                    length_digits++;
                }
                if (length_digits == 0 || length_cursor != inner_end) {
                    return NULL;
                }
                have_length = true;
            }
            size_t name_len = (size_t)(colon - inner);
            char *value = shellLookupParameterValue(inner, name_len);
            if (!value) {
                return NULL;
            }
            size_t value_len = strlen(value);
            size_t start_index = offset_value;
            if (start_index > value_len) {
                start_index = value_len;
            }
            size_t available = value_len - start_index;
            size_t copy_len = available;
            if (have_length && length_value < copy_len) {
                copy_len = length_value;
            }
            char *result = (char *)malloc(copy_len + 1);
            if (!result) {
                free(value);
                return NULL;
            }
            if (copy_len > 0) {
                memcpy(result, value + start_index, copy_len);
            }
            result[copy_len] = '\0';
            free(value);
            return result;
        }

        const char *cursor = inner;
        while (cursor < closing &&
               (isalnum((unsigned char)*cursor) || *cursor == '_')) {
            cursor++;
        }
        if (cursor == inner) {
            return NULL;
        }
        size_t name_len = (size_t)(cursor - inner);
        if (cursor < closing && *cursor == '[') {
            const char *subscript_start = cursor + 1;
            const char *subscript_end = memchr(subscript_start, ']', (size_t)(closing - subscript_start));
            if (!subscript_end || subscript_end > closing) {
                return NULL;
            }
            size_t subscript_len = (size_t)(subscript_end - subscript_start);
            const char *after_bracket = subscript_end + 1;
            while (after_bracket < closing && isspace((unsigned char)*after_bracket)) {
                after_bracket++;
            }
            if (after_bracket != closing) {
                return NULL;
            }
            size_t array_count = 0;
            bool array_expansion = false;
            char *expanded = shellExpandArraySubscriptValue(inner,
                                                            name_len,
                                                            subscript_start,
                                                            subscript_len,
                                                            &array_count,
                                                            &array_expansion);
            if (expanded && array_expansion) {
                if (out_is_array_expansion) {
                    *out_is_array_expansion = true;
                }
                if (out_array_count) {
                    *out_array_count = array_count;
                }
            }
            return expanded;
        }
        if (cursor < closing && (*cursor == '%' || *cursor == '#')) {
            bool remove_suffix = (*cursor == '%');
            bool longest = false;
            char op = *cursor;
            cursor++;
            if (cursor < closing && *cursor == op) {
                longest = true;
                cursor++;
            }
            const char *pattern_start = cursor;
            size_t pattern_len = (size_t)(closing - pattern_start);
            char *expanded_pattern = shellExpandPatternText(pattern_start, pattern_len);
            if (!expanded_pattern) {
                return NULL;
            }
            char *value = shellLookupParameterValue(inner, name_len);
            if (!value) {
                free(expanded_pattern);
                return NULL;
            }
            char *result = remove_suffix ? shellRemovePatternSuffix(value, expanded_pattern, longest)
                                         : shellRemovePatternPrefix(value, expanded_pattern, longest);
            free(value);
            free(expanded_pattern);
            return result;
        }
        if (cursor != closing) {
            return NULL;
        }
        return shellLookupParameterValue(inner, name_len);
    }

    if (*input == '$') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", (int)getpid());
        return strdup(buffer);
    }

    if (*input == '?') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gShellRuntime.last_status);
        return strdup(buffer);
    }

    if (*input == '!') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        pid_t pid = gShellRuntime.last_bg_pid;
        if (pid <= 0) {
            return strdup("");
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", (int)pid);
        return strdup(buffer);
    }

    if (*input == '#') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", gParamCount);
        return strdup(buffer);
    }

    if (*input == '*' || *input == '@') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        size_t array_count = 0;
        char *joined = shellJoinPositionalParameters(*input == '@', &array_count);
        if (joined && *input == '@') {
            if (out_is_array_expansion) {
                *out_is_array_expansion = true;
            }
            if (out_array_count) {
                *out_array_count = array_count;
            }
        }
        return joined;
    }

    if (*input == '0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        if (gShellArg0) {
            return strdup(gShellArg0);
        }
        return strdup("exsh");
    }

    if (isdigit((unsigned char)*input)) {
        const char *cursor = input;
        while (isdigit((unsigned char)*cursor)) {
            cursor++;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellLookupParameterValue(input, (size_t)(cursor - input));
    }

    if (isalpha((unsigned char)*input) || *input == '_') {
        const char *cursor = input + 1;
        while (isalnum((unsigned char)*cursor) || *cursor == '_') {
            cursor++;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellLookupParameterValue(input, (size_t)(cursor - input));
    }

    return NULL;
}

static void shellMarkArithmeticError(void) {
    shellUpdateStatus(1);
    gShellArithmeticErrorPending = true;
}

typedef struct {
    const char *input;
    size_t length;
    size_t pos;
} ShellArithmeticParser;

static void shellArithmeticSkipWhitespace(ShellArithmeticParser *parser) {
    if (!parser) {
        return;
    }
    while (parser->pos < parser->length &&
           isspace((unsigned char)parser->input[parser->pos])) {
        parser->pos++;
    }
}

static bool shellArithmeticParseValueString(const char *text, long long *out_value) {
    if (!out_value) {
        return false;
    }
    if (!text || *text == '\0') {
        *out_value = 0;
        return true;
    }
    errno = 0;
    char *endptr = NULL;
    long long value = strtoll(text, &endptr, 0);
    if (errno != 0) {
        return false;
    }
    if (endptr && *endptr != '\0') {
        while (endptr && *endptr && isspace((unsigned char)*endptr)) {
            endptr++;
        }
        if (endptr && *endptr != '\0') {
            return false;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticParseExpression(ShellArithmeticParser *parser, long long *out_value);

static bool shellArithmeticParsePrimary(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return false;
    }
    char c = parser->input[parser->pos];
    if (c == '(') {
        parser->pos++;
        if (!shellArithmeticParseExpression(parser, out_value)) {
            return false;
        }
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length || parser->input[parser->pos] != ')') {
            return false;
        }
        parser->pos++;
        return true;
    }
    if (c == '$') {
        parser->pos++;
        size_t consumed = 0;
        char *value = shellExpandParameter(parser->input + parser->pos,
                                           &consumed,
                                           NULL,
                                           NULL);
        if (!value) {
            return false;
        }
        parser->pos += consumed;
        long long parsed = 0;
        bool ok = shellArithmeticParseValueString(value, &parsed);
        free(value);
        if (!ok) {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = parser->pos;
        parser->pos++;
        while (parser->pos < parser->length) {
            char ch = parser->input[parser->pos];
            if (!isalnum((unsigned char)ch) && ch != '_') {
                break;
            }
            parser->pos++;
        }
        size_t len = parser->pos - start;
        char *value = shellLookupParameterValue(parser->input + start, len);
        if (!value) {
            return false;
        }
        long long parsed = 0;
        bool ok = shellArithmeticParseValueString(value, &parsed);
        free(value);
        if (!ok) {
            return false;
        }
        *out_value = parsed;
        return true;
    }
    if (isdigit((unsigned char)c)) {
        const char *start_ptr = parser->input + parser->pos;
        errno = 0;
        char *endptr = NULL;
        long long value = strtoll(start_ptr, &endptr, 0);
        if (errno != 0 || endptr == start_ptr) {
            return false;
        }
        size_t consumed = (size_t)(endptr - start_ptr);
        parser->pos += consumed;
        if (parser->pos < parser->length) {
            char next = parser->input[parser->pos];
            if (isalnum((unsigned char)next) || next == '_') {
                return false;
            }
        }
        *out_value = value;
        return true;
    }
    return false;
}

static bool shellArithmeticParseUnary(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    shellArithmeticSkipWhitespace(parser);
    if (parser->pos >= parser->length) {
        return false;
    }
    char c = parser->input[parser->pos];
    if (c == '+') {
        parser->pos++;
        return shellArithmeticParseUnary(parser, out_value);
    }
    if (c == '-') {
        parser->pos++;
        long long value = 0;
        if (!shellArithmeticParseUnary(parser, &value)) {
            return false;
        }
        *out_value = -value;
        return true;
    }
    return shellArithmeticParsePrimary(parser, out_value);
}

static bool shellArithmeticParseTerm(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    long long value = 0;
    if (!shellArithmeticParseUnary(parser, &value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length) {
            break;
        }
        char op = parser->input[parser->pos];
        if (op != '*' && op != '/' && op != '%') {
            break;
        }
        parser->pos++;
        long long rhs = 0;
        if (!shellArithmeticParseUnary(parser, &rhs)) {
            return false;
        }
        if (op == '*') {
            value *= rhs;
        } else if (op == '/') {
            if (rhs == 0) {
                return false;
            }
            value /= rhs;
        } else {
            if (rhs == 0) {
                return false;
            }
            value %= rhs;
        }
    }
    *out_value = value;
    return true;
}

static bool shellArithmeticParseExpression(ShellArithmeticParser *parser, long long *out_value) {
    if (!parser || !out_value) {
        return false;
    }
    long long value = 0;
    if (!shellArithmeticParseTerm(parser, &value)) {
        return false;
    }
    while (true) {
        shellArithmeticSkipWhitespace(parser);
        if (parser->pos >= parser->length) {
            break;
        }
        char op = parser->input[parser->pos];
        if (op != '+' && op != '-') {
            break;
        }
        parser->pos++;
        long long rhs = 0;
        if (!shellArithmeticParseTerm(parser, &rhs)) {
            return false;
        }
        if (op == '+') {
            value += rhs;
        } else {
            value -= rhs;
        }
    }
    *out_value = value;
    return true;
}

static char *shellLetFindAssignment(char *expr) {
    if (!expr) {
        return NULL;
    }

    bool in_single = false;
    bool in_double = false;
    for (char *p = expr; *p; ++p) {
        if (*p == '\\' && p[1]) {
            ++p;
            continue;
        }
        if (!in_double && *p == 0x27 /* '\'' */) {
            in_single = !in_single;
            continue;
        }
        if (!in_single && *p == '"') {
            in_double = !in_double;
            continue;
        }
        if (in_single || in_double) {
            continue;
        }
        if (*p != '=') {
            continue;
        }

        char prev = (p > expr) ? p[-1] : '\0';
        char next = p[1];
        if (prev == '=' || prev == '!' || prev == '<' || prev == '>') {
            continue;
        }
        if (next == '=') {
            continue;
        }
        return p;
    }
    return NULL;
}

static char *shellEvaluateArithmetic(const char *expr, bool *out_error) {
    if (out_error) {
        *out_error = false;
    }
    if (!expr) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    ShellArithmeticParser parser;
    parser.input = expr;
    parser.length = strlen(expr);
    parser.pos = 0;
    long long value = 0;
    if (!shellArithmeticParseExpression(&parser, &value)) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    shellArithmeticSkipWhitespace(&parser);
    if (parser.pos < parser.length) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char buffer[64];
    int written = snprintf(buffer, sizeof(buffer), "%lld", value);
    if (written < 0) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    char *result = strdup(buffer);
    if (!result) {
        if (out_error) {
            *out_error = true;
        }
        return NULL;
    }
    return result;
}

static bool shellLetEvaluateExpression(VM *vm, const char *text, long long *out_value) {
    if (!text) {
        runtimeError(vm, "let: expected expression");
        shellMarkArithmeticError();
        return false;
    }

    char *copy = strdup(text);
    if (!copy) {
        runtimeError(vm, "let: out of memory");
        shellMarkArithmeticError();
        return false;
    }

    char *start = copy;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)end[-1])) {
        end--;
    }
    *end = '\0';

    if (*start == '\0') {
        runtimeError(vm, "let: expected expression");
        shellMarkArithmeticError();
        free(copy);
        return false;
    }

    char *eq = shellLetFindAssignment(start);
    if (!eq) {
        bool eval_error = false;
        char *result = shellEvaluateArithmetic(start, &eval_error);
        if (!result || eval_error) {
            runtimeError(vm, "let: arithmetic syntax error: \"%s\"", text);
            shellMarkArithmeticError();
            free(result);
            free(copy);
            return false;
        }
        long long value = 0;
        bool ok = shellArithmeticParseValueString(result, &value);
        free(result);
        if (!ok) {
            runtimeError(vm, "let: arithmetic syntax error: \"%s\"", text);
            shellMarkArithmeticError();
            free(copy);
            return false;
        }
        if (out_value) {
            *out_value = value;
        }
        free(copy);
        return true;
    }
    char *rhs = eq + 1;
    while (*rhs && isspace((unsigned char)*rhs)) {
        rhs++;
    }
    if (*rhs == '\0') {
        runtimeError(vm, "let: arithmetic syntax error: \"%s\"", text);
        shellMarkArithmeticError();
        free(copy);
        return false;
    }

    bool eval_error = false;
    char *rhs_result = shellEvaluateArithmetic(rhs, &eval_error);
    if (!rhs_result || eval_error) {
        runtimeError(vm, "let: arithmetic syntax error: \"%s\"", text);
        shellMarkArithmeticError();
        free(rhs_result);
        free(copy);
        return false;
    }
    long long rhs_value = 0;
    bool rhs_ok = shellArithmeticParseValueString(rhs_result, &rhs_value);
    free(rhs_result);
    if (!rhs_ok) {
        runtimeError(vm, "let: arithmetic syntax error: \"%s\"", text);
        shellMarkArithmeticError();
        free(copy);
        return false;
    }

    char *lhs_end = eq;
    while (lhs_end > start && isspace((unsigned char)lhs_end[-1])) {
        lhs_end--;
    }

    char op = '\0';
    if (lhs_end > start) {
        char candidate = lhs_end[-1];
        if (candidate == '+' || candidate == '-' || candidate == '*' || candidate == '/' || candidate == '%') {
            op = candidate;
            lhs_end--;
            while (lhs_end > start && isspace((unsigned char)lhs_end[-1])) {
                lhs_end--;
            }
        }
    }

    lhs_end[0] = '\0';
    char *name = start;
    while (*name && isspace((unsigned char)*name)) {
        name++;
    }
    if (*name == '\0') {
        runtimeError(vm, "let: expected identifier");
        shellMarkArithmeticError();
        free(copy);
        return false;
    }
    if (!shellIsValidEnvName(name)) {
        runtimeError(vm, "let: %s: invalid identifier", name);
        shellMarkArithmeticError();
        free(copy);
        return false;
    }

    long long new_value = rhs_value;
    if (op != '\0') {
        char *current_text = shellLookupParameterValue(name, strlen(name));
        long long current_value = 0;
        bool current_ok = shellArithmeticParseValueString(current_text, &current_value);
        free(current_text);
        if (!current_ok) {
            runtimeError(vm, "let: %s: invalid numeric value", name);
            shellMarkArithmeticError();
            free(copy);
            return false;
        }
        switch (op) {
            case '+':
                new_value = current_value + rhs_value;
                break;
            case '-':
                new_value = current_value - rhs_value;
                break;
            case '*':
                new_value = current_value * rhs_value;
                break;
            case '/':
                if (rhs_value == 0) {
                    runtimeError(vm, "let: division by zero");
                    shellMarkArithmeticError();
                    free(copy);
                    return false;
                }
                new_value = current_value / rhs_value;
                break;
            case '%':
                if (rhs_value == 0) {
                    runtimeError(vm, "let: division by zero");
                    shellMarkArithmeticError();
                    free(copy);
                    return false;
                }
                new_value = current_value % rhs_value;
                break;
            default:
                runtimeError(vm, "let: unsupported assignment");
                shellMarkArithmeticError();
                free(copy);
                return false;
        }
    }

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%lld", new_value);
    if (!shellSetTrackedVariable(name, buffer, false)) {
        runtimeError(vm, "let: failed to assign %s", name);
        shellMarkArithmeticError();
        free(copy);
        return false;
    }

    if (out_value) {
        *out_value = new_value;
    }
    free(copy);
    return true;
}

static char *shellExpandWord(const char *text,
                             uint8_t flags,
                             const char *meta,
                             size_t meta_len,
                             bool **out_quoted_map,
                             size_t *out_quoted_len,
                             bool *out_array_zero) {
    if (out_quoted_map) {
        *out_quoted_map = NULL;
    }
    if (out_quoted_len) {
        *out_quoted_len = 0;
    }
    if (out_array_zero) {
        *out_array_zero = false;
    }
    if (!text) {
        return strdup("");
    }

    bool track_quotes = out_quoted_map && out_quoted_len;
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    size_t quoted_cap = 0;

    ShellMetaSubstitution *subs = NULL;
    size_t sub_count = 0;
    if (!shellParseCommandMetadata(meta, meta_len, &subs, &sub_count)) {
        subs = NULL;
        sub_count = 0;
    }

    size_t text_len = strlen(text);
    size_t length = 0;
    size_t capacity = text_len + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        shellFreeMetaSubstitutions(subs, sub_count);
        return NULL;
    }
    buffer[0] = '\0';

    bool base_single = (flags & SHELL_WORD_FLAG_SINGLE_QUOTED) != 0;
    bool base_double = (flags & SHELL_WORD_FLAG_DOUBLE_QUOTED) != 0;
    bool in_single_segment = false;
    bool in_double_segment = false;
    bool saw_single_marker = false;
    bool saw_double_marker = false;
    bool has_arithmetic = (flags & SHELL_WORD_FLAG_HAS_ARITHMETIC) != 0;
    size_t sub_index = 0;

    bool saw_empty_array_expansion = false;

    for (size_t i = 0; i < text_len;) {
        char c = text[i];
        if (c == SHELL_QUOTE_MARK_SINGLE) {
            saw_single_marker = true;
            in_single_segment = !in_single_segment;
            i++;
            continue;
        }
        if (c == SHELL_QUOTE_MARK_DOUBLE) {
            saw_double_marker = true;
            in_double_segment = !in_double_segment;
            i++;
            continue;
        }

        bool effective_single = in_single_segment || (!saw_single_marker && base_single);
        bool effective_double = in_double_segment || (!saw_double_marker && base_double);
        bool quoted_flag = effective_single || effective_double;

        if (effective_single) {
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap, true, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }

        bool handled = false;
        if (sub_index < sub_count) {
            ShellMetaSubstitution *sub = &subs[sub_index];
            size_t span = sub->span_length;
            if (sub->style == SHELL_META_SUBSTITUTION_DOLLAR && c == '$' && i + 1 < text_len && text[i + 1] == '(') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        size_t out_len = strlen(output);
                        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                          quoted_flag, out_len)) {
                            free(output);
                            goto expand_error;
                        }
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            } else if (sub->style == SHELL_META_SUBSTITUTION_BACKTICK && c == '`') {
                if (span > 0 && i + span <= text_len) {
                    char *output = shellRunCommandSubstitution(sub->command);
                    if (output) {
                        size_t out_len = strlen(output);
                        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                          quoted_flag, out_len)) {
                            free(output);
                            goto expand_error;
                        }
                        shellBufferAppendString(&buffer, &length, &capacity, output);
                        free(output);
                    }
                    i += span;
                    sub_index++;
                    handled = true;
                } else {
                    sub_index++;
                }
            }
        }
        if (handled) {
            continue;
        }

        if (sub_count == 0 && c == '$' && i + 1 < text_len && text[i + 1] == '(' &&
            !(i + 2 < text_len && text[i + 2] == '(')) {
            size_t span = 0;
            char *command = NULL;
            if (shellParseInlineDollarCommand(text, i, text_len, &span, &command)) {
                char *output = shellRunCommandSubstitution(command);
                free(command);
                if (output) {
                    size_t out_len = strlen(output);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(output);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, output);
                    free(output);
                }
                i += span;
                continue;
            }
        }
        if (sub_count == 0 && c == '`') {
            size_t span = 0;
            char *command = NULL;
            if (shellParseInlineBacktickCommand(text, i, text_len, &span, &command)) {
                char *output = shellRunCommandSubstitution(command);
                free(command);
                if (output) {
                    size_t out_len = strlen(output);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(output);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, output);
                    free(output);
                }
                i += span;
                continue;
            }
        }

        if (c == '$' && has_arithmetic && i + 2 < text_len && text[i + 1] == '(' && text[i + 2] == '(') {
            size_t expr_start = i + 3;
            size_t j = expr_start;
            int depth = 1;
            while (j < text_len) {
                char inner = text[j];
                if (inner == '(') {
                    depth++;
                } else if (inner == ')') {
                    depth--;
                    if (depth == 0) {
                        break;
                    }
                }
                j++;
            }
            size_t span = 0;
            if (depth == 0 && j + 1 < text_len && text[j + 1] == ')') {
                span = (j + 2) - i;
                size_t expr_len = j - expr_start;
                char *expr = (char *)malloc(expr_len + 1);
                if (!expr) {
                    shellMarkArithmeticError();
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, span)) {
                        goto expand_error;
                    }
                    shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                    i += span;
                    continue;
                }
                if (expr_len > 0) {
                    memcpy(expr, text + expr_start, expr_len);
                }
                expr[expr_len] = '\0';
                bool eval_error = false;
                char *result = shellEvaluateArithmetic(expr, &eval_error);
                free(expr);
                if (!eval_error && result) {
                    size_t out_len = strlen(result);
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, out_len)) {
                        free(result);
                        goto expand_error;
                    }
                    shellBufferAppendString(&buffer, &length, &capacity, result);
                    free(result);
                } else {
                    shellMarkArithmeticError();
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, span)) {
                        goto expand_error;
                    }
                    shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                }
                i += span;
                continue;
            } else {
                span = text_len - i;
                shellMarkArithmeticError();
                if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                  quoted_flag, span)) {
                    goto expand_error;
                }
                shellBufferAppendSlice(&buffer, &length, &capacity, text + i, span);
                i = text_len;
                continue;
            }
        }

        bool treat_as_double = effective_double;
        if (c == '\\') {
            if (i + 1 < text_len) {
                char next = text[i + 1];
                if (!treat_as_double || next == '$' || next == '"' || next == '\\' || next == '`' || next == '\n') {
                    if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                      quoted_flag, 1)) {
                        goto expand_error;
                    }
                    shellBufferAppendChar(&buffer, &length, &capacity, next);
                    i += 2;
                    continue;
                }
            }
            if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                              quoted_flag, 1)) {
                goto expand_error;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }

        if (c == '$') {
            size_t consumed = 0;
            bool array_like = false;
            size_t array_count = 0;
            char *expanded = shellExpandParameter(text + i + 1,
                                                 &consumed,
                                                 &array_like,
                                                 &array_count);
            if (expanded) {
                size_t out_len = strlen(expanded);
                if (array_like && array_count == 0) {
                    saw_empty_array_expansion = true;
                }
                if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                                  quoted_flag, out_len)) {
                    free(expanded);
                    goto expand_error;
                }
                shellBufferAppendString(&buffer, &length, &capacity, expanded);
                free(expanded);
                i += consumed + 1;
                continue;
            }
        }

        if (!shellQuotedMapAppendRepeated(track_quotes, &quoted_map, &quoted_len, &quoted_cap,
                                          quoted_flag, 1)) {
            goto expand_error;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    shellFreeMetaSubstitutions(subs, sub_count);
    if (track_quotes) {
        *out_quoted_map = quoted_map;
        *out_quoted_len = quoted_len;
    } else {
        free(quoted_map);
    }
    if (out_array_zero && saw_empty_array_expansion && length == 0) {
        *out_array_zero = true;
    }
    return buffer;

expand_error:
    shellFreeMetaSubstitutions(subs, sub_count);
    free(buffer);
    free(quoted_map);
    return NULL;
}

static void shellFreeRedirections(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->redir_count; ++i) {
        free(cmd->redirs[i].path);
        free(cmd->redirs[i].here_doc);
        free(cmd->redirs[i].here_string_literal);
        free(cmd->redirs[i].here_string);
    }
    free(cmd->redirs);
    cmd->redirs = NULL;
    cmd->redir_count = 0;
}

static void shellFreeCommand(ShellCommand *cmd) {
    if (!cmd) {
        return;
    }
    for (size_t i = 0; i < cmd->assignment_count; ++i) {
        free(cmd->assignments[i].text);
    }
    free(cmd->assignments);
    cmd->assignments = NULL;
    cmd->assignment_count = 0;
    for (size_t i = 0; i < cmd->argc; ++i) {
        free(cmd->argv[i]);
    }
    free(cmd->argv);
    cmd->argv = NULL;
    cmd->argc = 0;
    shellFreeRedirections(cmd);
}

static bool shellParseBool(const char *value, bool *out_flag) {
    if (!value || !out_flag) {
        return false;
    }
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "true") == 0 || strcasecmp(value, "yes") == 0) {
        *out_flag = true;
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "false") == 0 || strcasecmp(value, "no") == 0) {
        *out_flag = false;
        return true;
    }
    return false;
}

static bool shellStringEqualsIgnoreCase(const char *lhs, const char *rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    return strcasecmp(lhs, rhs) == 0;
}

static bool shellTryParseIntegerLiteral(const char *text, long long *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    long long value = strtoll(text, &end, 0);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static bool shellLooksLikeFloatLiteral(const char *text) {
    if (!text) {
        return false;
    }
    for (const char *cursor = text; *cursor; ++cursor) {
        if (*cursor == '.' || *cursor == 'e' || *cursor == 'E') {
            return true;
        }
    }
    if (shellStringEqualsIgnoreCase(text, "inf") ||
        shellStringEqualsIgnoreCase(text, "+inf") ||
        shellStringEqualsIgnoreCase(text, "-inf") ||
        shellStringEqualsIgnoreCase(text, "infinity") ||
        shellStringEqualsIgnoreCase(text, "+infinity") ||
        shellStringEqualsIgnoreCase(text, "-infinity") ||
        shellStringEqualsIgnoreCase(text, "nan") ||
        shellStringEqualsIgnoreCase(text, "+nan") ||
        shellStringEqualsIgnoreCase(text, "-nan")) {
        return true;
    }
    return false;
}

static bool shellTryParseFloatLiteral(const char *text, double *out_value) {
    if (!text || !*text) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    double value = strtod(text, &end);
    if (errno != 0 || !end || *end != '\0') {
        return false;
    }
    if (out_value) {
        *out_value = value;
    }
    return true;
}

static Value shellConvertBuiltinArgument(const char *text) {
    if (!text) {
        return makeString("");
    }

    enum {
        SHELL_ARG_MODE_AUTO,
        SHELL_ARG_MODE_STRING,
        SHELL_ARG_MODE_BOOL,
        SHELL_ARG_MODE_INT,
        SHELL_ARG_MODE_FLOAT,
        SHELL_ARG_MODE_NIL
    } mode = SHELL_ARG_MODE_AUTO;

    const char *payload = text;

    if (strncasecmp(payload, "str:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "string:", 7) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 7;
    } else if (strncasecmp(payload, "raw:", 4) == 0) {
        mode = SHELL_ARG_MODE_STRING;
        payload += 4;
    } else if (strncasecmp(payload, "bool:", 5) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 5;
    } else if (strncasecmp(payload, "boolean:", 8) == 0) {
        mode = SHELL_ARG_MODE_BOOL;
        payload += 8;
    } else if (strncasecmp(payload, "int:", 4) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 4;
    } else if (strncasecmp(payload, "integer:", 8) == 0) {
        mode = SHELL_ARG_MODE_INT;
        payload += 8;
    } else if (strncasecmp(payload, "float:", 6) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 6;
    } else if (strncasecmp(payload, "double:", 7) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 7;
    } else if (strncasecmp(payload, "real:", 5) == 0) {
        mode = SHELL_ARG_MODE_FLOAT;
        payload += 5;
    } else if (strncasecmp(payload, "nil:", 4) == 0) {
        mode = SHELL_ARG_MODE_NIL;
        payload += 4;
    }

    if (mode == SHELL_ARG_MODE_STRING) {
        return makeString(payload ? payload : "");
    }

    if (mode == SHELL_ARG_MODE_NIL) {
        return makeNil();
    }

    if (mode == SHELL_ARG_MODE_BOOL || mode == SHELL_ARG_MODE_AUTO) {
        bool flag = false;
        if (shellParseBool(payload, &flag)) {
            return makeBoolean(flag ? 1 : 0);
        }
        if (mode == SHELL_ARG_MODE_BOOL) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_INT || mode == SHELL_ARG_MODE_AUTO) {
        long long int_value = 0;
        if (shellTryParseIntegerLiteral(payload, &int_value)) {
            return makeInt(int_value);
        }
        if (mode == SHELL_ARG_MODE_INT) {
            return makeString(payload ? payload : "");
        }
    }

    if (mode == SHELL_ARG_MODE_FLOAT || mode == SHELL_ARG_MODE_AUTO) {
        if (mode == SHELL_ARG_MODE_FLOAT || shellLooksLikeFloatLiteral(payload)) {
            double dbl_value = 0.0;
            if (shellTryParseFloatLiteral(payload, &dbl_value)) {
                return makeDouble(dbl_value);
            }
            if (mode == SHELL_ARG_MODE_FLOAT) {
                return makeString(payload ? payload : "");
            }
        }
    }

    if (mode == SHELL_ARG_MODE_AUTO && payload && *payload) {
        if (shellStringEqualsIgnoreCase(payload, "nil") ||
            shellStringEqualsIgnoreCase(payload, "null")) {
            return makeNil();
        }
    }

    return makeString(payload ? payload : "");
}

static void shellUpdateStatus(int status) {
    if (gShellStatusSticky && status == 0) {
        gShellStatusSticky = false;
        return;
    }
    shellRuntimeSetLastStatus(status);
}

/*
 * POSIX specifies that foreground commands should see the shell's inherited
 * signal dispositions, except that asynchronous lists without job control must
 * inherit SIG_IGN for SIGINT and SIGQUIT, and that traps fire only after the
 * foreground job or wait completes.  We record pending signals in an
 * async-signal-safe manner and reconcile them once we're back in the main
 * interpreter loop so the runtime can unwind cleanly before honouring traps.
 */
static void shellHandlePendingSignal(int signo) {
    if (signo <= 0 || signo >= NSIG) {
        return;
    }

    ShellTrapAction trap_action = shellRuntimeGetSignalTrapAction(signo);
    if (trap_action == SHELL_TRAP_ACTION_IGNORE) {
        return;
    }

    bool command_trap = (trap_action == SHELL_TRAP_ACTION_COMMAND);

    if (!command_trap) {
        shellUpdateStatus(128 + signo);
    }

    if (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) {
        bool stop_request = (signo == SIGTSTP) && gShellRuntime.job_control_enabled;
        if (gShellCurrentVm && !stop_request) {
            gShellCurrentVm->exit_requested = true;
            gShellCurrentVm->current_builtin_name = "signal";
        }

        if (!gShellRuntime.job_control_enabled && !stop_request) {
            shellRuntimeRequestExit();
        }

        if (gShellLoopStackSize > 0 && !stop_request) {
            gShellRuntime.break_requested = true;
            gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
            shellLoopRequestBreakLevels((int)gShellLoopStackSize);
        }

        bool propagate_default =
            (gShellExitOnSignalFlag && (signo == SIGINT || signo == SIGQUIT || signo == SIGTSTP) &&
             !gShellRuntime.trap_enabled && !command_trap);
#if defined(PSCAL_TARGET_IOS)
        if (signo == SIGTSTP) {
            /* Never let SIGTSTP stop the host app; handle it logically instead. */
            propagate_default = false;
        }
#endif

        if (propagate_default) {
            struct sigaction action;
            memset(&action, 0, sizeof(action));
            sigemptyset(&action.sa_mask);
            action.sa_handler = SIG_DFL;
            sigaction(signo, &action, NULL);
            raise(signo);
            if (signo == SIGTSTP) {
                memset(&action, 0, sizeof(action));
                sigemptyset(&action.sa_mask);
                action.sa_handler = shellSignalHandler;
                action.sa_flags |= SA_RESTART;
                sigaction(signo, &action, NULL);
            }
        }

        if (command_trap) {
            shellRuntimeRunSignalTrap(signo);
            bool exit_now = gShellExitRequested ||
                             (gShellCurrentVm && gShellCurrentVm->exit_requested);
            if (exit_now && gShellLoopStackSize > 0) {
                gShellRuntime.break_requested = true;
                gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
                shellLoopRequestBreakLevels((int)gShellLoopStackSize);
            }
        }
        return;
    }

    if (command_trap) {
        shellRuntimeRunSignalTrap(signo);
        bool exit_now = gShellExitRequested ||
                         (gShellCurrentVm && gShellCurrentVm->exit_requested);
        if (exit_now && gShellLoopStackSize > 0) {
            gShellRuntime.break_requested = true;
            gShellRuntime.break_requested_levels = (int)gShellLoopStackSize;
            shellLoopRequestBreakLevels((int)gShellLoopStackSize);
        }
    }
}

void shellRuntimeProcessPendingSignals(void) {
    for (int signo = 1; signo < NSIG; ++signo) {
        if (!gShellPendingSignals[signo]) {
            continue;
        }
        gShellPendingSignals[signo] = 0;
        shellHandlePendingSignal(signo);
    }
}

void shellRuntimeSetExitOnSignal(bool enabled) {
    gShellExitOnSignalFlag = enabled ? 1 : 0;
}

bool shellRuntimeExitOnSignal(void) {
    return gShellExitOnSignalFlag != 0;
}

void shellRuntimeInitSignals(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_handler = shellSignalHandler;
    action.sa_flags |= SA_RESTART;

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGQUIT, &action, NULL);
    sigaction(SIGTSTP, &action, NULL);
}

static bool shellHistoryEnsureCapacity(size_t needed) {
    if (gShellHistory.capacity >= needed) {
        return true;
    }
    size_t new_capacity = gShellHistory.capacity ? gShellHistory.capacity * 2 : 16;
    if (new_capacity < needed) {
        new_capacity = needed;
    }
    if (new_capacity > SIZE_MAX / sizeof(char *)) {
        return false;
    }
    char **entries = realloc(gShellHistory.entries, new_capacity * sizeof(char *));
    if (!entries) {
        return false;
    }
    gShellHistory.entries = entries;
    gShellHistory.capacity = new_capacity;
    return true;
}

void shellRuntimeRecordHistory(const char *line) {
    if (!line) {
        return;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        --len;
    }
    if (len == 0) {
        return;
    }
    bool has_content = false;
    for (size_t i = 0; i < len; ++i) {
        if (line[i] != ' ' && line[i] != '\t') {
            has_content = true;
            break;
        }
    }
    if (!has_content) {
        return;
    }
    if (!shellHistoryEnsureCapacity(gShellHistory.count + 1)) {
        return;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    gShellHistory.entries[gShellHistory.count++] = copy;
}

size_t shellRuntimeHistoryCount(void) {
    return gShellHistory.count;
}

bool shellRuntimeHistoryGetEntry(size_t reverse_index, char **out_line) {
    if (!out_line) {
        return false;
    }
    *out_line = NULL;
    if (reverse_index >= gShellHistory.count) {
        return false;
    }
    const char *entry = gShellHistory.entries[gShellHistory.count - reverse_index - 1];
    if (!entry) {
        return false;
    }
    char *copy = strdup(entry);
    if (!copy) {
        return false;
    }
    *out_line = copy;
    return true;
}

void shellRuntimeSetArg0(const char *name) {
    char *copy = NULL;
    if (name && *name) {
        copy = strdup(name);
        if (!copy) {
            return;
        }
    }
    free(gShellArg0);
    gShellArg0 = copy;
}

const char *shellRuntimeGetArg0(void) {
    return gShellArg0;
}

int shellRuntimeCurrentCommandLine(void) {
    return gShellCurrentCommandLine;
}

int shellRuntimeCurrentCommandColumn(void) {
    return gShellCurrentCommandColumn;
}

void shellRuntimeInitJobControl(void) {
    shellEnsureJobControl();
}

void shellRuntimeRestoreForeground(void) {
    shellEnsureJobControl();
    shellJobControlRestoreForeground();
}

typedef enum {
    SHELL_HISTORY_EXPAND_OK,
    SHELL_HISTORY_EXPAND_NOT_FOUND,
    SHELL_HISTORY_EXPAND_INVALID
} ShellHistoryExpandResult;

static bool shellIsHistoryTerminator(char c) {
    switch (c) {
        case '\0':
        case ' ':
        case '\t':
        case '\n':
        case '\r':
        case ';':
        case '&':
        case '|':
        case '(':
        case ')':
        case '<':
        case '>':
            return true;
        default:
            return false;
    }
}

static ShellHistoryExpandResult shellExpandHistoryDesignatorAt(const char *input,
                                                               size_t *out_consumed,
                                                               char **out_line) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_consumed) {
        *out_consumed = 0;
    }
    if (!input || input[0] != '!') {
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *cursor = input + 1;
    if (*cursor == '\0') {
        if (out_consumed) {
            *out_consumed = 1;
        }
        return SHELL_HISTORY_EXPAND_INVALID;
    }

    const char *designator_start = NULL;
    size_t designator_len = 0;
    const char *entry = NULL;

    if (*cursor == '$' || *cursor == '*' || *cursor == '^') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
        entry = shellHistoryEntryByIndex(-1);
        if (!entry) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_NOT_FOUND;
        }
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    long numeric_index = 0;
    bool has_index = false;
    const char *search_token_start = NULL;
    size_t search_token_len = 0;
    bool search_substring = false;
    bool search_regex = false;

    if (*cursor == '!') {
        numeric_index = -1;
        has_index = true;
        cursor++;
    } else if (*cursor == '-') {
        char *endptr = NULL;
        long value = strtol(cursor + 1, &endptr, 10);
        if (endptr == cursor + 1) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor + 1 - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = -value;
        cursor = endptr;
        has_index = true;
    } else if (isdigit((unsigned char)*cursor)) {
        char *endptr = NULL;
        long value = strtol(cursor, &endptr, 10);
        if (endptr == cursor) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        numeric_index = value;
        cursor = endptr;
        has_index = true;
    } else if (*cursor == '?') {
        cursor++;
        const char *start = cursor;
        const char *closing = strchr(cursor, '?');
        if (!closing) {
            if (out_consumed) {
                *out_consumed = strlen(input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(closing - start);
        if (search_token_len >= 2 && start[0] == '/' && start[search_token_len - 1] == '/') {
            search_regex = true;
            search_token_start = start + 1;
            search_token_len -= 2;
            if (search_token_len == 0) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        }
        cursor = closing + 1;
        search_substring = true;
    } else {
        const char *start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor) &&
               *cursor != ':' && *cursor != '$' && *cursor != '^' && *cursor != '*') {
            cursor++;
        }
        if (cursor == start) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
        search_token_start = start;
        search_token_len = (size_t)(cursor - start);
    }

    if (*cursor == '$' || *cursor == '^' || *cursor == '*') {
        designator_start = cursor;
        designator_len = 1;
        cursor++;
    } else if (*cursor == ':') {
        cursor++;
        designator_start = cursor;
        while (*cursor && !shellIsHistoryTerminator(*cursor)) {
            cursor++;
        }
        designator_len = (size_t)(cursor - designator_start);
        if (designator_len == 0) {
            if (out_consumed) {
                *out_consumed = (size_t)(cursor - input);
            }
            return SHELL_HISTORY_EXPAND_INVALID;
        }
    }

    if (has_index) {
        entry = shellHistoryEntryByIndex(numeric_index);
    } else if (search_substring) {
        if (search_regex) {
            bool invalid = false;
            entry = shellHistoryFindByRegex(search_token_start, search_token_len, &invalid);
            if (!entry && invalid) {
                if (out_consumed) {
                    *out_consumed = (size_t)(cursor - input);
                }
                return SHELL_HISTORY_EXPAND_INVALID;
            }
        } else {
            entry = shellHistoryFindBySubstring(search_token_start, search_token_len);
        }
    } else {
        entry = shellHistoryFindByPrefix(search_token_start, search_token_len);
    }

    if (!entry) {
        if (out_consumed) {
            *out_consumed = (size_t)(cursor - input);
        }
        return SHELL_HISTORY_EXPAND_NOT_FOUND;
    }

    if (out_consumed) {
        *out_consumed = (size_t)(cursor - input);
    }

    if (designator_start && designator_len > 0) {
        return shellApplyHistoryDesignator(entry, designator_start, designator_len, out_line)
                   ? SHELL_HISTORY_EXPAND_OK
                   : SHELL_HISTORY_EXPAND_INVALID;
    }

    *out_line = strdup(entry);
    if (!*out_line) {
        return SHELL_HISTORY_EXPAND_INVALID;
    }
    return SHELL_HISTORY_EXPAND_OK;
}

bool shellRuntimeExpandHistoryReference(const char *input,
                                        char **out_line,
                                        bool *out_did_expand,
                                        char **out_error_token) {
    if (out_line) {
        *out_line = NULL;
    }
    if (out_did_expand) {
        *out_did_expand = false;
    }
    if (out_error_token) {
        *out_error_token = NULL;
    }
    if (!input || !out_line) {
        return false;
    }

    size_t capacity = strlen(input) + 1;
    if (capacity < 32) {
        capacity = 32;
    }
    char *buffer = (char *)malloc(capacity);
    if (!buffer) {
        return false;
    }
    buffer[0] = '\0';
    size_t length = 0;

    bool in_single = false;
    bool in_double = false;

    for (size_t i = 0; input[i];) {
        char c = input[i];
        if (c == '\\' && !in_single) {
            if (input[i + 1] == '!') {
                shellBufferAppendChar(&buffer, &length, &capacity, '!');
                i += 2;
                continue;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '\'') {
            if (!in_double) {
                in_single = !in_single;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '"') {
            if (!in_single) {
                in_double = !in_double;
            }
            shellBufferAppendChar(&buffer, &length, &capacity, c);
            i++;
            continue;
        }
        if (c == '!' && !in_single) {
            size_t consumed = 0;
            char *replacement = NULL;
            ShellHistoryExpandResult result =
                shellExpandHistoryDesignatorAt(input + i, &consumed, &replacement);
            if (result != SHELL_HISTORY_EXPAND_OK) {
                if (out_error_token) {
                    size_t error_len = consumed > 0 ? consumed : 1;
                    char *token = strndup(input + i, error_len);
                    *out_error_token = token;
                }
                free(replacement);
                free(buffer);
                return false;
            }
            if (replacement) {
                shellBufferAppendString(&buffer, &length, &capacity, replacement);
                free(replacement);
            }
            if (out_did_expand) {
                *out_did_expand = true;
            }
            i += consumed;
            continue;
        }
        shellBufferAppendChar(&buffer, &length, &capacity, c);
        i++;
    }

    *out_line = buffer;
    return true;
}

#if defined(PSCAL_TARGET_IOS)
static bool shellDispatchIOSFrontendBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || !cmd->argv || !cmd->argv[0]) {
        return false;
    }
    const char *name = cmd->argv[0];
    VmBuiltinFn handler = NULL;
    if (strcasecmp(name, "clike") == 0) {
        handler = vmBuiltinShellClike;
    } else if (strcasecmp(name, "pascal") == 0) {
        handler = vmBuiltinShellPascal;
    } else if (strcasecmp(name, "rea") == 0) {
        handler = vmBuiltinShellRea;
    } else if (strcasecmp(name, "pscalvm") == 0) {
        handler = vmBuiltinShellPscalVm;
    } else if (strcasecmp(name, "pscaljson2bc") == 0) {
        handler = vmBuiltinShellPscalJson2bc;
    } else if (strcasecmp(name, "exsh") == 0 || strcasecmp(name, "sh") == 0) {
        handler = vmBuiltinShellExshTool;
    }
    if (!handler) {
        return false;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    Value *args = NULL;
    if (arg_count > 0) {
        args = calloc((size_t)arg_count, sizeof(Value));
        if (!args) {
            shellUpdateStatus(1);
            return true;
        }
        for (int i = 0; i < arg_count; ++i) {
            args[i] = makeString(cmd->argv[i + 1]);
        }
    }
    const char *previous_name = shellRuntimeCurrentCommandName();
    shellRuntimeSetCurrentCommandName(name);
    bool previous_background = gShellCurrentBuiltinBackground;
    gShellCurrentBuiltinBackground = cmd->background;
    handler(vm, arg_count, args);
    gShellCurrentBuiltinBackground = previous_background;
    shellRuntimeSetCurrentCommandName(previous_name);
    if (args) {
        for (int i = 0; i < arg_count; ++i) {
            freeValue(&args[i]);
        }
        free(args);
    }
    return true;
}
#endif

static bool shellIsRuntimeBuiltin(const char *name) {
    if (!name || !*name) {
        return false;
    }
    if (shellRuntimeBuiltinDisabled(name)) {
        return false;
    }
#if defined(PSCAL_TARGET_IOS)
    /* Frontends are statically linked on iOS; treat them as builtins even if
     * their compiler ids were not resolved. */
    if (strcasecmp(name, "clike") == 0 ||
        strcasecmp(name, "pascal") == 0 ||
        strcasecmp(name, "rea") == 0 ||
        strcasecmp(name, "pscalvm") == 0 ||
        strcasecmp(name, "pscaljson2bc") == 0 ||
        strcasecmp(name, "exsh") == 0 || strcasecmp(name, "sh") == 0) {
        return true;
    }
#endif
    return shellIsBuiltinName(name);
}

static bool shellInvokeFunction(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0) {
        return false;
    }
    const char *name = cmd->argv[0];
    ShellFunctionEntry *entry = shellFindFunctionEntry(name);
    if (!entry || !entry->compiled) {
        return false;
    }
    char **saved_params = gParamValues;
    int saved_count = gParamCount;
    bool saved_owned = gShellPositionalOwned;
    char **function_params = NULL;
    int function_count = (cmd->argc > 1) ? (int)cmd->argc - 1 : 0;
    if (function_count > 0) {
        function_params = (char **)calloc((size_t)function_count, sizeof(char *));
        if (!function_params) {
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        bool ok = true;
        for (int i = 0; i < function_count; ++i) {
            const char *arg = cmd->argv[i + 1] ? cmd->argv[i + 1] : "";
            function_params[i] = strdup(arg);
            if (!function_params[i]) {
                ok = false;
                break;
            }
        }
        if (!ok) {
            for (int i = 0; i < function_count; ++i) {
                free(function_params[i]);
            }
            free(function_params);
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            return true;
        }
        gParamValues = function_params;
        gParamCount = function_count;
        gShellPositionalOwned = true;
    } else {
        gParamValues = NULL;
        gParamCount = 0;
        gShellPositionalOwned = false;
    }
    bool scope_pushed = shellLocalScopePush();
    if (!scope_pushed) {
        runtimeError(vm, "%s: out of memory", name);
        shellUpdateStatus(1);
        if (gShellPositionalOwned) {
            shellFreeOwnedPositionalParameters();
        } else {
            gParamValues = NULL;
            gParamCount = 0;
        }
        gParamValues = saved_params;
        gParamCount = saved_count;
        gShellPositionalOwned = saved_owned;
        return true;
    }
    static VM function_vm;
    static bool function_vm_initialized = false;
    static bool function_vm_in_use = false;
    VM *invoke_vm = NULL;
    VM *nested_vm = NULL;
    bool using_cached_vm = false;
    bool nested_vm_initialized = false;
    bool frame_pushed = false;
    InterpretResult result = INTERPRET_OK;
    if (!function_vm_in_use) {
        if (!function_vm_initialized) {
            initVM(&function_vm);
            function_vm_initialized = true;
        } else {
            vmResetExecutionState(&function_vm);
        }
        function_vm_in_use = true;
        using_cached_vm = true;
        invoke_vm = &function_vm;
    } else {
        nested_vm = (VM *)malloc(sizeof(VM));
        if (!nested_vm) {
            runtimeError(vm, "%s: out of memory", name);
            shellUpdateStatus(1);
            goto invoke_cleanup;
        }
        initVM(nested_vm);
        invoke_vm = nested_vm;
        nested_vm_initialized = true;
    }

    if (!invoke_vm) {
        goto invoke_cleanup;
    }
    invoke_vm->frontendContext = vm ? vm->frontendContext : NULL;

    if (!shellRuntimePushCallFrame(name, cmd->line, cmd->column)) {
        runtimeError(vm, "%s: out of memory", name);
        shellUpdateStatus(1);
        goto invoke_cleanup;
    }
    frame_pushed = true;

    result = interpretBytecode(invoke_vm, &entry->compiled->chunk,
                               globalSymbols, constGlobalSymbols, procedure_table, 0);
    if (result != INTERPRET_OK) {
        shellUpdateStatus(1);
    } else {
        shellUpdateStatus(shellRuntimeLastStatus());
    }

invoke_cleanup:
    if (using_cached_vm) {
        vmResetExecutionState(&function_vm);
        function_vm_in_use = false;
    } else if (nested_vm) {
        if (nested_vm_initialized) {
            freeVM(nested_vm);
        }
        free(nested_vm);
    }
    if (frame_pushed) {
        shellRuntimePopCallFrame();
    }
    if (scope_pushed) {
        shellLocalScopePop();
    }
    if (gShellPositionalOwned) {
        shellFreeOwnedPositionalParameters();
    } else {
        gParamValues = NULL;
        gParamCount = 0;
    }
    gParamValues = saved_params;
    gParamCount = saved_count;
    gShellPositionalOwned = saved_owned;
    return true;
}

#if defined(PSCAL_TARGET_IOS)
extern int PSCALRuntimeSetTabTitle(const char *title);

static const char *shellSshTargetFromCommand(const ShellCommand *cmd) {
    if (!cmd || cmd->argc <= 1) {
        return NULL;
    }
    bool skip_next = false;
    for (size_t i = 1; i < cmd->argc; ++i) {
        const char *arg = cmd->argv[i];
        if (!arg) {
            continue;
        }
        if (skip_next) {
            skip_next = false;
            continue;
        }
        if (strcmp(arg, "-l") == 0 || strcmp(arg, "-p") == 0 ||
            strcmp(arg, "-i") == 0 || strcmp(arg, "-F") == 0 ||
            strcmp(arg, "-o") == 0 || strcmp(arg, "-b") == 0) {
            skip_next = true;
            continue;
        }
        if (arg[0] == '-') {
            continue;
        }
        return arg;
    }
    return NULL;
}
#endif

static bool shellInvokeBuiltin(VM *vm, ShellCommand *cmd) {
    if (!cmd || cmd->argc == 0 || !cmd->argv[0] || *cmd->argv[0] == '\0') {
        return false;
    }
    const char *name = cmd->argv[0];
    if (!cmd->ignore_functions && shellInvokeFunction(vm, cmd)) {
        return true;
    }
    const char *canonical = shellBuiltinCanonicalName(name);
    const char *lookup = canonical ? canonical : name;
    if (shellRuntimeBuiltinDisabled(lookup)) {
        return false;
    }
    bool is_runtime_builtin = shellIsRuntimeBuiltin(lookup);
    if (!is_runtime_builtin) {
        return false;
    }
    VmBuiltinFn handler = getVmBuiltinHandler(lookup);
    if (!handler && canonical && canonical != name) {
        handler = getVmBuiltinHandler(name);
    }
#if defined(PSCAL_TARGET_IOS)
    if (!handler && canonical) {
        if (strcasecmp(canonical, "clike") == 0) {
            handler = vmBuiltinShellClike;
        } else if (strcasecmp(canonical, "pascal") == 0) {
            handler = vmBuiltinShellPascal;
        } else if (strcasecmp(canonical, "rea") == 0) {
            handler = vmBuiltinShellRea;
        } else if (strcasecmp(canonical, "pscalvm") == 0) {
            handler = vmBuiltinShellPscalVm;
        } else if (strcasecmp(canonical, "pscaljson2bc") == 0) {
            handler = vmBuiltinShellPscalJson2bc;
        } else if (strcasecmp(canonical, "exsh") == 0 || strcasecmp(canonical, "sh") == 0) {
            handler = vmBuiltinShellExshTool;
        }
    }
#endif
    if (!handler && is_runtime_builtin) {
        /* Fallback to canonical lookup via builtin table if registry failed. */
        const char *fallback = shellBuiltinCanonicalName(name);
        if (fallback && fallback != canonical) {
            handler = getVmBuiltinHandler(fallback);
            canonical = fallback;
            lookup = canonical ? canonical : name;
        }
    }
#if defined(PSCAL_TARGET_IOS)
    if (canonical && strcasecmp(canonical, "ssh") == 0) {
        const char *target = shellSshTargetFromCommand(cmd);
        if (target && *target) {
            char label[64];
            snprintf(label, sizeof(label), "SSH %s", target);
            (void)PSCALRuntimeSetTabTitle(label);
        }
    }
#endif
#if defined(PSCAL_TARGET_IOS)
    if (!handler && is_runtime_builtin && shellDispatchIOSFrontendBuiltin(vm, cmd)) {
        return true;
    }
#endif
    if (!handler) {
        if (!is_runtime_builtin) {
            return false;
        }
        fprintf(stderr, "[builtin-missing] name=%s canonical=%s\n",
                name,
                canonical ? canonical : "<null>");
        const char *label = canonical ? canonical : name;
        if (vm) {
            runtimeError(vm, "shell builtin '%s': not available", label ? label : "<builtin>");
        } else {
            fprintf(stderr, "exsh: shell builtin '%s' is not available\n", label ? label : "<builtin>");
        }
        shellUpdateStatus(127);
        return true;
    }
    int arg_count = (cmd->argc > 0) ? (int)cmd->argc - 1 : 0;
    Value *args = NULL;
    int previous_line = gShellCurrentCommandLine;
    int previous_column = gShellCurrentCommandColumn;
    shellRuntimeSetCurrentCommandLocation(cmd->line, cmd->column);
    ShellExecRedirBackup *redir_backups = NULL;
    size_t redir_backup_count = 0;
    shellFlushStandardStreams();
    if (!shellApplyExecRedirections(vm, cmd, &redir_backups, &redir_backup_count)) {
        if (args) {
            for (int i = 0; i < arg_count; ++i) {
                freeValue(&args[i]);
            }
            free(args);
        }
        shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
        return true;
    }

    const char *previous_name = shellRuntimeCurrentCommandName();
    shellRuntimeSetCurrentCommandName(name);
    bool previous_background = gShellCurrentBuiltinBackground;
    gShellCurrentBuiltinBackground = cmd->background;
    bool profile_enabled = vmOpcodeProfileIsEnabled();
    const char *profile_name = canonical ? canonical : name;
    if (profile_enabled) {
        vmProfileShellBuiltin(profile_name);
    }

    bool handled = false;
    if (canonical && strcmp(canonical, "test") == 0) {
        const char **expr_args = (const char **)(cmd->argv + 1);
        int expr_argc = (int)cmd->argc - 1;
        bool is_bracket = (name && strcmp(name, "[") == 0);
        bool fast_result = false;
        if (shellBuiltinTestFastEval(expr_args, expr_argc, is_bracket, &fast_result)) {
            shellUpdateStatus(fast_result ? 0 : 1);
            handled = true;
        }
    }
    if (!handled && canonical && strcmp(canonical, ":") == 0) {
        shellUpdateStatus(0);
        handled = true;
    }
    if (!handled && canonical && strcmp(canonical, "true") == 0) {
        shellUpdateStatus(0);
        handled = true;
    }
    if (!handled && canonical && strcmp(canonical, "false") == 0) {
        shellUpdateStatus(1);
        handled = true;
    }
    if (!handled && canonical && strcmp(canonical, "__shell_arithmetic") == 0) {
        const char *expression = (cmd->argc > 1 && cmd->argv[1]) ? cmd->argv[1] : NULL;
        int status = 0;
        if (expression && shellBuiltinArithmeticFastEval(expression, &status)) {
            shellUpdateStatus(status);
            handled = true;
        }
    }

    if (!handled) {
        if (arg_count > 0) {
            args = calloc((size_t)arg_count, sizeof(Value));
            if (!args) {
                runtimeError(vm, "shell builtin '%s': out of memory", name);
                shellUpdateStatus(1);
                gShellCurrentBuiltinBackground = previous_background;
                shellRuntimeSetCurrentCommandName(previous_name);
                shellFlushStandardStreams();
                shellRestoreExecRedirections(redir_backups, redir_backup_count);
                shellFreeExecRedirBackups(redir_backups, redir_backup_count);
                shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
                return true;
            }
            for (int i = 0; i < arg_count; ++i) {
                args[i] = makeString(cmd->argv[i + 1]);
            }
        }
        handler(vm, arg_count, args);
    }
    gShellCurrentBuiltinBackground = previous_background;
    shellRuntimeSetCurrentCommandName(previous_name);
    shellFlushStandardStreams();
    shellRestoreExecRedirections(redir_backups, redir_backup_count);
    shellFreeExecRedirBackups(redir_backups, redir_backup_count);
    if (args) {
        for (int i = 0; i < arg_count; ++i) {
            freeValue(&args[i]);
        }
        free(args);
    }
    shellRuntimeSetCurrentCommandLocation(previous_line, previous_column);
    return true;
}

static int shellStatusFromWait(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    if (WIFSTOPPED(status)) {
        return 128 + WSTOPSIG(status);
    }
    return status;
}

static void shellFreeJob(ShellJob *job) {
    if (!job) {
        return;
    }
    if (job->pids && job->pid_count > 0) {
        for (size_t i = 0; i < job->pid_count; ++i) {
            if (job->pids[i] > 0) {
                vprocSetJobId(job->pids[i], 0);
                vprocSetCommandLabel(job->pids[i], NULL);
            }
        }
    }
    if (job->pgid > 0) {
        vprocSetJobId(job->pgid, 0);
        vprocSetCommandLabel(job->pgid, NULL);
    }
    free(job->command);
    job->command = NULL;
    free(job->pids);
    job->pids = NULL;
    job->pid_count = 0;
    job->pgid = -1;
    job->running = false;
    job->stopped = false;
    job->disowned = false;
    job->last_status = 0;
}

static void shellRemoveJobAt(size_t index) {
    if (index >= gShellJobCount) {
        return;
    }
    ShellJob *job = &gShellJobs[index];
    ShellRuntimeState *ctx = shellGetContext();
    if (ctx) {
        ctx->last_removed_job_id = job->id;
        ctx->last_removed_job_status = job->last_status;
        ctx->last_removed_job_valid = true;
    }
    shellFreeJob(job);
    if (index + 1 < gShellJobCount) {
        memmove(&gShellJobs[index],
                &gShellJobs[index + 1],
                (gShellJobCount - index - 1) * sizeof(ShellJob));
    }
    gShellJobCount--;
    if (gShellJobCount == 0 && gShellJobs) {
        free(gShellJobs);
        gShellJobs = NULL;
    }
}

static ShellJob *shellFindJobByPid(pid_t pid);

static size_t shellAllocateJobId(void) {
    size_t candidate = 1;
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = vprocGetShellSelfPid();
    int current_sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : 0;
    size_t snap_cap = vprocSnapshot(NULL, 0);
    if (snap_cap > 0) {
        VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
        if (snaps) {
            size_t snap_count = vprocSnapshot(snaps, snap_cap);
            bool used = true;
            while (used) {
                used = false;
                for (size_t i = 0; i < snap_count; ++i) {
                    const VProcSnapshot *s = &snaps[i];
                    if (!s || s->pid <= 0 || s->job_id <= 0) {
                        continue;
                    }
                    if (current_sid > 0 && s->sid > 0 && s->sid != current_sid) {
                        continue;
                    }
                    if ((size_t)s->job_id == candidate) {
                        used = true;
                        candidate++;
                        break;
                    }
                }
            }
            free(snaps);
            gShellRuntimeDefault.job_next_id = candidate + 1;
            if (SC()) {
                SC()->job_next_id = gShellRuntimeDefault.job_next_id;
            }
            return candidate;
        }
    }
#endif
    bool used = true;
    while (used) {
        used = false;
        for (size_t i = 0; i < gShellJobCount; ++i) {
            if (gShellJobs[i].id == candidate) {
                used = true;
                candidate++;
                break;
            }
        }
    }
    gShellRuntimeDefault.job_next_id = candidate + 1;
    if (SC()) {
        SC()->job_next_id = gShellRuntimeDefault.job_next_id;
    }
    return candidate;
}

static ShellJob *shellRegisterJob(pid_t pgid, const pid_t *pids, size_t pid_count, const ShellCommand *cmd) {
    if (pgid <= 0 || !pids || pid_count == 0) {
        return NULL;
    }
    pid_t *pid_copy = malloc(sizeof(pid_t) * pid_count);
    if (!pid_copy) {
        return NULL;
    }
    memcpy(pid_copy, pids, sizeof(pid_t) * pid_count);

    char label_buf[128] = {0};
    char *summary = NULL;
    if (cmd && cmd->argc > 0 && cmd->argv) {
        size_t len = 0;
        for (size_t i = 0; i < cmd->argc; ++i) {
            len += strlen(cmd->argv[i]) + 1;
        }
        summary = malloc(len + 1);
        if (summary) {
            summary[0] = '\0';
            for (size_t i = 0; i < cmd->argc; ++i) {
                strcat(summary, cmd->argv[i]);
                if (i + 1 < cmd->argc) {
                    strcat(summary, " ");
                }
            }
        }
    }

    if (!summary) {
        /* Try to reuse any stored label from the vproc table. */
        if (!vprocGetCommandLabel(pgid, label_buf, sizeof(label_buf)) && pid_count > 0) {
            (void)vprocGetCommandLabel(pids[0], label_buf, sizeof(label_buf));
        }
        if (label_buf[0]) {
            summary = strdup(label_buf);
        } else {
            summary = strdup("unknown");
        }
    }
    if (summary && label_buf[0] == '\0') {
        strncpy(label_buf, summary, sizeof(label_buf) - 1);
        label_buf[sizeof(label_buf) - 1] = '\0';
    }

    ShellJob *new_jobs = realloc(gShellJobs, sizeof(ShellJob) * (gShellJobCount + 1));
    if (!new_jobs) {
        free(summary);
        free(pid_copy);
        return NULL;
    }

    gShellJobs = new_jobs;
    ShellJob *job = &gShellJobs[gShellJobCount++];
    /* Preserve any existing vproc job id; otherwise allocate a new one and stamp it. */
    int job_id = vprocGetJobId(pgid);
    if (job_id <= 0 && pid_count > 0) {
        job_id = vprocGetJobId(pids[0]);
    }
    if (job_id > 0) {
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *existing = &gShellJobs[i];
            if (existing->id != (size_t)job_id) {
                continue;
            }
            bool same_job = (existing->pgid == pgid);
            if (!same_job && existing->pids && existing->pid_count > 0) {
                for (size_t j = 0; j < existing->pid_count; ++j) {
                    if (existing->pids[j] <= 0) {
                        continue;
                    }
                    for (size_t k = 0; k < pid_count; ++k) {
                        if (existing->pids[j] == pids[k]) {
                            same_job = true;
                            break;
                        }
                    }
                    if (same_job) {
                        break;
                    }
                }
            }
            if (!same_job) {
                job_id = 0;
            }
            break;
        }
    }
    if (job_id <= 0) {
        job_id = (int)shellAllocateJobId();
        vprocSetJobId(pgid, job_id);
        for (size_t i = 0; i < pid_count; ++i) {
            vprocSetJobId(pids[i], job_id);
        }
    } else {
        size_t next_id = (size_t)job_id + 1;
        if (gShellRuntimeDefault.job_next_id < next_id) {
            gShellRuntimeDefault.job_next_id = next_id;
        }
        if (SC() && SC()->job_next_id < next_id) {
            SC()->job_next_id = next_id;
        }
    }
    job->id = (size_t)job_id;
    job->pgid = pgid;
    job->pids = pid_copy;
    job->pid_count = pid_count;
    for (size_t i = 0; i < pid_count; ++i) {
        vprocSetCommandLabel(pids[i], summary);
        if (pgid > 0) {
            vprocSetPgid(pids[i], (int)pgid);
            int shell_pid = vprocGetShellSelfPid();
            if (shell_pid > 0 && pids[i] != shell_pid) {
                int sid = vprocGetSid((int)pids[i]);
                /* Never rewrite the session leader's parent (kernel/sid). */
                if (sid > 0 && sid == (int)pids[i]) {
                    continue;
                }
                vprocSetParent(pids[i], shell_pid);
            }
        }
    }
    vprocSetCommandLabel(pgid, summary);
    job->running = true;
    job->stopped = false;
    job->disowned = false;
    job->last_status = 0;
    job->command = summary;
    return job;
}

static int shellCollectJobs(void) {
#if defined(PSCAL_TARGET_IOS)
    int shell_pid = 0;
    int current_sid = 0;
    /* Mirror synthetic vproc tasks into the job table if they were launched
     * without registering a ShellJob.
     *
     * Important: do NOT treat every synthetic vproc task as a "job". Jobs are
     * shell-managed background pipelines. We only mirror entries that already
     * carry a non-zero vproc job id, otherwise core session entities (kernel,
     * shell, pumps, etc.) pollute the job table and break job-spec semantics. */
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    shell_pid = vprocGetShellSelfPid();
    current_sid = (shell_pid > 0) ? vprocGetSid(shell_pid) : 0;
    size_t snap_cap = vprocSnapshot(NULL, 0);
    if (snap_cap > 0) {
            VProcSnapshot *snaps = (VProcSnapshot *)calloc(snap_cap, sizeof(VProcSnapshot));
            if (snaps) {
                size_t snap_count = vprocSnapshot(snaps, snap_cap);
                if (dbg) {
                    fprintf(stderr, "[jobs-ios] snapshot count=%zu\n", snap_count);
                }
    for (size_t i = 0; i < snap_count; ++i) {
        const VProcSnapshot *s = &snaps[i];
        if (!s || s->pid <= 0 || s->exited) {
            continue;
        }
        /* When multiple shells/windows are active in the same process, vproc
         * snapshots can include tasks from other sessions. Only mirror tasks
         * that belong to the current shell session. */
        if (current_sid > 0 && s->sid > 0 && s->sid != current_sid) {
            continue;
        }
        bool has_jobid = (s->job_id > 0);
        if (!has_jobid) {
            continue;
        }
        /* Ignore snapshots that have no command label; these are typically
         * helper threads (output pumps, etc.) that should not appear as
         * jobs. */
        if (s->command[0] == '\0') {
            continue;
        }
        /* Skip snapshots whose label is the shell (self) to avoid mirroring the
         * foreground shell as a background job after reaping. */
        if (strcmp(s->command, "shell") == 0) {
            continue;
        }
        if (shell_pid > 0 && s->pid == shell_pid) {
            continue;
        }
        if (s->pid == vprocGetShellSelfPid() ||
            pthread_equal(s->tid, pthread_self())) {
            continue;
        }
                    if (dbg) {
                        fprintf(stderr, "[jobs-ios] snapshot pid=%d tid=%p exited=%d stopped=%d status=%d label='%s'\n",
                                (int)s->pid,
                                (void *)s->tid,
                                (int)s->exited,
                                (int)s->stopped,
                                s->status,
                                s->command);
                    }
                    if (shellFindJobByPid((pid_t)s->pid)) {
                        continue;
                    }
                    pid_t pid_copy[1] = { (pid_t)s->pid };
                    ShellJob *job = shellRegisterJob((pid_t)s->pid, pid_copy, 1, NULL);
                    if (job) {
                        job->running = !s->exited && !s->stopped;
                        job->stopped = s->stopped;
                        job->last_status = s->status;
                        if (!job->command && s->command[0]) {
                            job->command = strdup(s->command);
                        }
                        /* If the snapshot carries a job id, keep it stable so %n works. */
                        if (s->job_id > 0) {
                            job->id = (size_t)s->job_id;
                            vprocSetJobId(s->pid, s->job_id);
                        }
                    }
                }
                free(snaps);
            }
        }
#endif
    int reaped = 0;
    for (size_t i = 0; i < gShellJobCount;) {
        ShellJob *job = &gShellJobs[i];
        bool job_active = false;

#if defined(PSCAL_TARGET_IOS)
        if (current_sid > 0 && job && job->pgid > 0) {
            int job_sid = vprocGetSid((int)job->pgid);
            if (job_sid > 0 && job_sid != current_sid) {
                shellRemoveJobAt(i);
                reaped++;
                continue;
            }
        }
        int stored_id = vprocGetJobId(job->pgid);
        if (stored_id <= 0 && job->pid_count > 0) {
            stored_id = vprocGetJobId(job->pids[0]);
        }
        if (stored_id > 0) {
            job->id = (size_t)stored_id;
        }
#endif

        if (!job->pids || job->pid_count == 0) {
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        for (size_t j = 0; j < job->pid_count; ++j) {
            pid_t pid = job->pids[j];
            if (pid <= 0) {
                continue;
            }
            int status = 0;
            pid_t res = vprocWaitPidShim(pid, &status, WNOHANG | WUNTRACED | WCONTINUED);
            if (res == 0) {
                job_active = true;
                continue;
            }
            if (res < 0) {
                if (errno == EINTR) {
                    job_active = true;
                    continue;
                }
                if (errno == ECHILD) {
                    /* Synthetic tasks may not be parented to the shell; treat as still active. */
                    job_active = true;
                    continue;
                }
                job->pids[j] = -1;
                continue;
            }
            if (WIFSTOPPED(status)) {
                job->stopped = true;
                job->running = false;
                job_active = true;
            } else if (WIFCONTINUED(status)) {
                job->stopped = false;
                job->running = true;
                job_active = true;
            } else {
                job->last_status = shellStatusFromWait(status);
                job->pids[j] = -1;
            }
        }

        if (!job->stopped) {
            for (size_t j = 0; j < job->pid_count; ++j) {
                if (job->pids[j] > 0) {
                    job_active = true;
                    job->running = true;
                    break;
                }
            }
        }

        bool all_done = true;
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] > 0) {
                all_done = false;
                break;
            }
        }

        if (all_done) {
            int status = job->last_status;
            bool disowned = job->disowned;
            if (!disowned) {
                shellUpdateStatus(status);
            }
            shellRemoveJobAt(i);
            reaped++;
            continue;
        }

        if (!job_active && !job->stopped) {
            job->running = true;
        }

        ++i;
    }
    if (gShellJobCount == 0) {
        shellResetJobIds();
    }
#if defined(PSCAL_TARGET_IOS)
    if (dbg) {
        fprintf(stderr, "[jobs-ios] table after collect count=%zu\n", gShellJobCount);
        for (size_t i = 0; i < gShellJobCount; ++i) {
            ShellJob *job = &gShellJobs[i];
            fprintf(stderr, "[jobs-ios]   job id=%zu pgid=%d running=%d stopped=%d pid_count=%zu command='%s'\n",
                    job->id,
                    (int)job->pgid,
                    (int)job->running,
                    (int)job->stopped,
                    job->pid_count,
                    job->command ? job->command : "");
            for (size_t j = 0; j < job->pid_count; ++j) {
                fprintf(stderr, "[jobs-ios]     pid[%zu]=%d\n", j, (int)job->pids[j]);
            }
        }
    }
#endif
    return reaped;
}

static size_t shellJobVisibleCount(void) {
    size_t count = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->disowned) {
            ++count;
        }
    }
    return count;
}

static bool shellJobFindVisibleIndex(size_t visible_number, size_t *out_index) {
    if (!out_index || visible_number == 0) {
        return false;
    }
    bool dbg = getenv("PSCALI_KILL_DEBUG") != NULL;
    size_t ordinal = 0;
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (job->disowned) {
            continue;
        }
        ++ordinal;
        if (dbg) {
            fprintf(stderr, "[jobs-ios] vis=%zu ordinal=%zu id=%zu pgid=%d running=%d stopped=%d\n",
                    visible_number, ordinal, job->id, (int)job->pgid,
                    (int)job->running, (int)job->stopped);
        }
        if (job->id == visible_number || ordinal == visible_number) {
            *out_index = i;
            return true;
        }
    }
    return false;
}

static bool shellFindJobIndexByPid(pid_t pid, size_t *out_index) {
    if (!out_index || pid <= 0) {
        return false;
    }
    for (size_t i = 0; i < gShellJobCount; ++i) {
        ShellJob *job = &gShellJobs[i];
        if (!job->pids || job->pid_count == 0) {
            continue;
        }
        for (size_t j = 0; j < job->pid_count; ++j) {
            if (job->pids[j] == pid) {
                *out_index = i;
                return true;
            }
        }
    }
    return false;
}

static bool shellParseJobSpecifier(VM *vm, const char *name, Value spec, size_t *out_index) {
    if (spec.type == TYPE_STRING && spec.s_val) {
        const char *text = spec.s_val;
        if (text[0] == '%') {
            text++;
        }
        if (*text == '\0') {
            runtimeError(vm, "%s: invalid job spec", name);
            return false;
        }
        char *end = NULL;
        long index = strtol(text, &end, 10);
        if (*end != '\0' || index <= 0) {
            runtimeError(vm, "%s: invalid job '%s'", name, spec.s_val);
            return false;
        }
        size_t pid_index = 0;
        if (shellFindJobIndexByPid((pid_t)index, &pid_index)) {
            *out_index = pid_index;
            return true;
        }
        if (!shellJobFindVisibleIndex((size_t)index, out_index)) {
            runtimeError(vm, "%s: invalid job '%s'", name, spec.s_val);
            return false;
        }
        return true;
    }

    if (IS_INTLIKE(spec)) {
        long index = (long)AS_INTEGER(spec);
        if (index <= 0) {
            runtimeError(vm, "%s: invalid job index", name);
            return false;
        }
        size_t pid_index = 0;
        if (shellFindJobIndexByPid((pid_t)index, &pid_index)) {
            *out_index = pid_index;
            return true;
        }
        if (shellJobFindVisibleIndex((size_t)index, out_index)) {
            return true;
        }
        runtimeError(vm, "%s: invalid job index", name);
        return false;
    }

    runtimeError(vm, "%s: job spec must be a string or integer", name);
    return false;
}

static bool shellResolveJobIndex(VM *vm, const char *name, int arg_count, Value *args, size_t *out_index) {
    size_t visible_count = shellJobVisibleCount();
#if defined(PSCAL_TARGET_IOS)
    if (visible_count == 0) {
        /* If jobs were not registered, try to mirror from vproc snapshots. */
        shellCollectJobs();
        visible_count = shellJobVisibleCount();
    }
#endif
    if (arg_count == 0) {
        if (visible_count == 0) {
            runtimeError(vm, "%s: no current job", name);
            return false;
        }
        for (size_t i = gShellJobCount; i > 0; --i) {
            ShellJob *job = &gShellJobs[i - 1];
            if (!job->disowned) {
                *out_index = i - 1;
                return true;
            }
        }
        runtimeError(vm, "%s: no current job", name);
        return false;
    }
    if (arg_count > 1) {
        runtimeError(vm, "%s: too many arguments", name);
        return false;
    }

    if (shellParseJobSpecifier(vm, name, args[0], out_index)) {
        return true;
    }
#if defined(PSCAL_TARGET_IOS)
    /* One more attempt after refreshing vproc snapshot. */
    shellCollectJobs();
    return shellParseJobSpecifier(vm, name, args[0], out_index);
#else
    return false;
#endif
}

static void shellParseMetadata(const char *meta, ShellCommand *cmd) {
    if (!meta || !cmd) {
        return;
    }
    char *copy = strdup(meta);
    if (!copy) {
        return;
    }
    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        if (eq) {
            *eq = '\0';
            const char *key = cursor;
            const char *value = eq + 1;
            if (strcmp(key, "bg") == 0) {
                shellParseBool(value, &cmd->background);
            } else if (strcmp(key, "pipe") == 0) {
                cmd->pipeline_index = atoi(value);
            } else if (strcmp(key, "head") == 0) {
                shellParseBool(value, &cmd->is_pipeline_head);
            } else if (strcmp(key, "tail") == 0) {
                shellParseBool(value, &cmd->is_pipeline_tail);
            } else if (strcmp(key, "line") == 0) {
                cmd->line = atoi(value);
            } else if (strcmp(key, "col") == 0) {
                cmd->column = atoi(value);
            }
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }
    free(copy);
}

static bool shellAddArg(ShellCommand *cmd, const char *arg, bool *saw_command_word) {
    if (!cmd || !arg) {
        return false;
    }
    const char *text = NULL;
    const char *meta = NULL;
    size_t meta_len = 0;
    uint8_t flags = 0;
    if (!shellDecodeWordSpec(arg, &text, &flags, &meta, &meta_len)) {
        return false;
    }
    bool *quoted_map = NULL;
    size_t quoted_len = 0;
    bool zero_array = false;
    char *expanded = shellExpandWord(text,
                                     flags,
                                     meta,
                                     meta_len,
                                     &quoted_map,
                                     &quoted_len,
                                     &zero_array);
    if (!expanded) {
        return false;
    }
    bool literal_mode = cmd->disable_field_splitting;
    if (saw_command_word && !*saw_command_word) {
        if ((flags & SHELL_WORD_FLAG_ASSIGNMENT) && shellLooksLikeAssignment(expanded)) {
            bool is_array_literal = shellAssignmentIsArrayLiteral(text, flags);
            if (!shellCommandAppendAssignmentOwned(cmd, expanded, is_array_literal)) {
                free(quoted_map);
                return false;
            }
            free(quoted_map);
            return true;
        }
    } else if (!literal_mode && (flags & SHELL_WORD_FLAG_ASSIGNMENT) && shellLooksLikeAssignment(expanded)) {
        if (!shellCommandAppendArgOwned(cmd, expanded)) {
            free(quoted_map);
            return false;
        }
        if (saw_command_word) {
            *saw_command_word = true;
        }
        free(quoted_map);
        return true;
    }
    if (!literal_mode && cmd->argc == 0 && strcmp(expanded, "[[") == 0) {
        literal_mode = true;
        cmd->disable_field_splitting = true;
    }
    if (literal_mode) {
        if (!shellCommandAppendArgOwned(cmd, expanded)) {
            free(quoted_map);
            return false;
        }
        if (saw_command_word) {
            *saw_command_word = true;
        }
        free(quoted_map);
        return true;
    }
    char **fields = NULL;
    size_t field_count = 0;
    if (!shellSplitExpandedWord(expanded,
                                flags,
                                quoted_map,
                                quoted_len,
                                zero_array,
                                &fields,
                                &field_count)) {
        free(expanded);
        free(quoted_map);
        return false;
    }
    free(expanded);
    free(quoted_map);
    if (field_count == 0) {
        free(fields);
        return true;
    }

    for (size_t i = 0; i < field_count; ++i) {
        char *field = fields[i];
        if (!field) {
            continue;
        }
        bool appended = false;
        if (shellWordShouldGlob(flags, field)) {
            glob_t glob_result;
            const char *glob_pattern = field;
#if defined(PSCAL_TARGET_IOS)
            char expanded_pattern[PATH_MAX];
            bool glob_used_virtual = false;
            if (field[0] == '/' &&
                pathTruncateEnabled() &&
                pathTruncateExpand(field, expanded_pattern, sizeof(expanded_pattern))) {
                glob_pattern = expanded_pattern;
                glob_used_virtual = true;
            }
#endif
            int glob_status = glob(glob_pattern, 0, NULL, &glob_result);
            if (glob_status == 0) {
#if defined(PSCAL_TARGET_IOS)
                if (glob_used_virtual) {
                    for (size_t g = 0; g < glob_result.gl_pathc; ++g) {
                        char *match = glob_result.gl_pathv[g];
                        if (!match) {
                            continue;
                        }
                        char stripped[PATH_MAX];
                        if (pathTruncateStrip(match, stripped, sizeof(stripped))) {
                            char *copy = strdup(stripped);
                            if (copy) {
                                free(glob_result.gl_pathv[g]);
                                glob_result.gl_pathv[g] = copy;
                            }
                        }
                    }
                }
#endif
                size_t original_argc = cmd->argc;
                bool ok = true;
                for (size_t g = 0; g < glob_result.gl_pathc; ++g) {
                    char *dup = strdup(glob_result.gl_pathv[g]);
                    if (!dup) {
                        ok = false;
                        break;
                    }
                    if (!shellCommandAppendArgOwned(cmd, dup)) {
                        ok = false;
                        break;
                    }
                }
                if (!ok) {
                    while (cmd->argc > original_argc) {
                        free(cmd->argv[cmd->argc - 1]);
                        cmd->argc--;
                    }
                    if (cmd->argv) {
                        cmd->argv[cmd->argc] = NULL;
                    }
                    globfree(&glob_result);
                    for (size_t j = i; j < field_count; ++j) {
                        if (fields[j]) {
                            free(fields[j]);
                        }
                    }
                    free(fields);
                    free(field);
                    return false;
                }
                globfree(&glob_result);
                appended = true;
            } else if (glob_status != GLOB_NOMATCH) {
                fprintf(stderr, "exsh: glob failed for '%s'\n", field);
            }
        }
        if (!appended) {
            if (!shellCommandAppendArgOwned(cmd, field)) {
                for (size_t j = i + 1; j < field_count; ++j) {
                    if (fields[j]) {
                        free(fields[j]);
                    }
                }
                free(fields);
                return false;
            }
            appended = true;
        } else {
            free(field);
        }
        fields[i] = NULL;
    }
    free(fields);
    if (saw_command_word) {
        *saw_command_word = true;
    }
    return true;
}

static int decodeHexDigit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    return -1;
}

static char *decodeHexString(const char *hex, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    if (!hex || *hex == '\0') {
        if (out_len) {
            *out_len = 0;
        }
        return strdup("");
    }
    size_t len = strlen(hex);
    if ((len & 1) != 0) {
        return NULL;
    }
    size_t decoded_len = len / 2;
    char *decoded = (char *)malloc(decoded_len + 1);
    if (!decoded) {
        return NULL;
    }
    for (size_t i = 0; i < decoded_len; ++i) {
        int high = decodeHexDigit(hex[i * 2]);
        int low = decodeHexDigit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) {
            free(decoded);
            return NULL;
        }
        decoded[i] = (char)((high << 4) | low);
    }
    decoded[decoded_len] = '\0';
    if (out_len) {
        *out_len = decoded_len;
    }
    return decoded;
}

static bool shellAddRedirection(ShellCommand *cmd, const char *spec) {
    if (!cmd || !spec) {
        return false;
    }
    if (strncmp(spec, "redir:", 6) != 0) {
        return false;
    }
    const char *payload = spec + 6;
    char *copy = strdup(payload);
    if (!copy) {
        return false;
    }

    const char *fd_text = "";
    const char *type_text = "";
    const char *word_hex = "";
    const char *here_hex = "";
    const char *here_string_hex = "";
    bool here_quoted = false;

    char *cursor = copy;
    while (cursor && *cursor) {
        char *next = strchr(cursor, ';');
        if (next) {
            *next = '\0';
        }
        char *eq = strchr(cursor, '=');
        const char *key = cursor;
        const char *value = "";
        if (eq) {
            *eq = '\0';
            value = eq + 1;
        }
        if (strcmp(key, "fd") == 0) {
            fd_text = value;
        } else if (strcmp(key, "type") == 0) {
            type_text = value;
        } else if (strcmp(key, "word") == 0) {
            word_hex = value;
        } else if (strcmp(key, "here") == 0) {
            here_hex = value;
        } else if (strcmp(key, "hereq") == 0) {
            shellParseBool(value, &here_quoted);
        } else if (strcmp(key, "hstr") == 0) {
            here_string_hex = value;
        }
        if (!next) {
            break;
        }
        cursor = next + 1;
    }

    if (!type_text || *type_text == '\0') {
        free(copy);
        return false;
    }

    int fd = -1;
    if (fd_text && *fd_text) {
        fd = atoi(fd_text);
    } else if (strcmp(type_text, "<") == 0 || strcmp(type_text, "<<") == 0 || strcmp(type_text, "<<<") == 0 || strcmp(type_text, "<&") == 0 || strcmp(type_text, "<>") == 0) {
        fd = STDIN_FILENO;
    } else {
        fd = STDOUT_FILENO;
    }

    ShellRedirection redir;
    memset(&redir, 0, sizeof(redir));
    redir.fd = fd;

    char *word_encoded = decodeHexString(word_hex, NULL);
    const char *target_spec = (word_encoded && *word_encoded) ? word_encoded : "";
    const char *target_text = NULL;
    const char *target_meta = NULL;
    size_t target_meta_len = 0;
    uint8_t target_flags = 0;
    if (target_spec && *target_spec) {
        shellDecodeWordSpec(target_spec, &target_text, &target_flags, &target_meta, &target_meta_len);
    }

    char *expanded_target = NULL;
    if (strcmp(type_text, "<<") != 0) {
        if (!target_spec || *target_spec == '\0') {
            free(word_encoded);
            free(copy);
            return false;
        }
        expanded_target = shellExpandWord(target_text,
                                          target_flags,
                                          target_meta,
                                          target_meta_len,
                                          NULL,
                                          NULL,
                                          NULL);
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
    }

    if (strcmp(type_text, "<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDONLY;
        redir.mode = 0;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_APPEND;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "&>") == 0 || strcmp(type_text, "&>>") == 0) {
        /* Redirect both stdout and stderr. */
        bool append = (strcmp(type_text, "&>>") == 0);
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;

        /* Prepare a dup redirection for stderr to stdout. */
        ShellRedirection dup_redir;
        memset(&dup_redir, 0, sizeof(dup_redir));
        dup_redir.fd = STDERR_FILENO;
        dup_redir.kind = SHELL_RUNTIME_REDIR_DUP;
        dup_redir.dup_target_fd = STDOUT_FILENO;

        ShellRedirection *new_redirs = realloc(cmd->redirs,
                                               sizeof(ShellRedirection) * (cmd->redir_count + 2));
        if (!new_redirs) {
            free(redir.path);
            free(expanded_target);
            free(word_encoded);
            free(copy);
            return false;
        }
        cmd->redirs = new_redirs;
        cmd->redirs[cmd->redir_count++] = redir;
        cmd->redirs[cmd->redir_count++] = dup_redir;
        free(word_encoded);
        free(copy);
        return true;
    } else if (strcmp(type_text, "<>") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_RDWR | O_CREAT;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, ">|") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_OPEN;
        redir.flags = O_WRONLY | O_CREAT | O_TRUNC;
        redir.mode = 0666;
        redir.path = expanded_target;
        expanded_target = NULL;
    } else if (strcmp(type_text, "<&") == 0 || strcmp(type_text, ">&") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_DUP;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        if (strcmp(expanded_target, "-") == 0) {
            redir.close_target = true;
        } else {
            char *endptr = NULL;
            errno = 0;
            long value = strtol(expanded_target, &endptr, 10);
            if (errno != 0 || !endptr || *endptr != '\0') {
                free(expanded_target);
                free(copy);
                return false;
            }
            redir.dup_target_fd = (int)value;
        }
        free(expanded_target);
        expanded_target = NULL;
    } else if (strcmp(type_text, "<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HEREDOC;
        size_t body_len = 0;
        char *decoded = decodeHexString(here_hex ? here_hex : "", &body_len);
        if (!decoded) {
            free(word_encoded);
            free(copy);
            return false;
        }
        char *expanded = shellExpandHereDocument(decoded, here_quoted);
        if (!expanded) {
            expanded = decoded;
        } else {
            free(decoded);
        }
        redir.here_doc = expanded;
        redir.here_doc_length = expanded ? strlen(expanded) : 0;
        redir.here_doc_quoted = here_quoted;
    } else if (strcmp(type_text, "<<<") == 0) {
        redir.kind = SHELL_RUNTIME_REDIR_HERE_STRING;
        if (!expanded_target) {
            free(word_encoded);
            free(copy);
            return false;
        }
        char *literal = decodeHexString(here_string_hex ? here_string_hex : "", NULL);
        if (!literal) {
            free(expanded_target);
            free(word_encoded);
            free(copy);
            return false;
        }
        redir.here_string_literal = literal;
        redir.here_string = expanded_target;
        redir.here_string_length = expanded_target ? strlen(expanded_target) : 0;
        expanded_target = NULL;
    } else {
        free(expanded_target);
        free(word_encoded);
        free(copy);
        return false;
    }

    ShellRedirection *new_redirs = realloc(cmd->redirs, sizeof(ShellRedirection) * (cmd->redir_count + 1));
    if (!new_redirs) {
        free(expanded_target);
        free(redir.path);
        free(redir.here_doc);
        free(redir.here_string_literal);
        free(redir.here_string);
        free(word_encoded);
        free(copy);
        return false;
    }
    cmd->redirs = new_redirs;
    cmd->redirs[cmd->redir_count++] = redir;
    free(word_encoded);
    free(copy);
    return true;
}
