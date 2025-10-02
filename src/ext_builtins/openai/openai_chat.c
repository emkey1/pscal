#include "backend_ast/builtin.h"
#include "core/utils.h"
#include <curl/curl.h>
#include <ctype.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *data;
    size_t length;
} OpenAIBuffer;

static size_t openaiWriteCallback(void *contents, size_t size, size_t nmemb,
                                  void *userp) {
    size_t total = size * nmemb;
    OpenAIBuffer *buffer = (OpenAIBuffer *)userp;
    if (total == 0) {
        return 0;
    }
    char *new_data = realloc(buffer->data, buffer->length + total + 1);
    if (!new_data) {
        return 0;
    }
    buffer->data = new_data;
    memcpy(buffer->data + buffer->length, contents, total);
    buffer->length += total;
    buffer->data[buffer->length] = '\0';
    return total;
}

static char *openaiJsonEscape(const char *input) {
    if (!input) {
        return strdup("");
    }
    size_t len = strlen(input);
    /* Worst case every character expands to "\u00XX" (6 chars). */
    size_t max_len = len * 6 + 1;
    char *out = malloc(max_len);
    if (!out) {
        return NULL;
    }
    size_t pos = 0;
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)input[i];
        switch (c) {
            case '\"':
                memcpy(out + pos, "\\\"", 2);
                pos += 2;
                break;
            case '\\':
                memcpy(out + pos, "\\\\", 2);
                pos += 2;
                break;
            case '\b':
                memcpy(out + pos, "\\b", 2);
                pos += 2;
                break;
            case '\f':
                memcpy(out + pos, "\\f", 2);
                pos += 2;
                break;
            case '\n':
                memcpy(out + pos, "\\n", 2);
                pos += 2;
                break;
            case '\r':
                memcpy(out + pos, "\\r", 2);
                pos += 2;
                break;
            case '\t':
                memcpy(out + pos, "\\t", 2);
                pos += 2;
                break;
            default:
                if (c < 0x20) {
                    snprintf(out + pos, 7, "\\u%04X", (unsigned int)c);
                    pos += 6;
                } else {
                    out[pos++] = (char)c;
                }
                break;
        }
    }
    out[pos] = '\0';
    return out;
}

static int openaiExtractOptionsSlice(const char *options,
                                     const char **out_start,
                                     size_t *out_len) {
    if (!options) {
        return 0;
    }
    const char *start = options;
    const char *end = options + strlen(options);
    while (start < end && isspace((unsigned char)*start)) {
        start++;
    }
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    if (start >= end) {
        return 0;
    }
    if (*start == '{' && end - start >= 2 && *(end - 1) == '}') {
        start++;
        end--;
        while (start < end && isspace((unsigned char)*start)) {
            start++;
        }
        while (end > start && isspace((unsigned char)*(end - 1))) {
            end--;
        }
    }
    if (start >= end) {
        return 0;
    }
    *out_start = start;
    *out_len = (size_t)(end - start);
    return 1;
}

static char *openaiBuildRequestBody(const char *model,
                                    const char *messages_json,
                                    const char *options_json) {
    char *escaped_model = openaiJsonEscape(model);
    if (!escaped_model) {
        return NULL;
    }
    const char *options_start = NULL;
    size_t options_len = 0;
    if (options_json && openaiExtractOptionsSlice(options_json, &options_start,
                                                  &options_len)) {
        /* options_len already computed */
    } else {
        options_start = NULL;
        options_len = 0;
    }

    size_t messages_len = messages_json ? strlen(messages_json) : 0;
    size_t prefix_len = strlen("{\"model\":\"") + strlen(escaped_model) +
                        strlen("\",\"messages\":");
    size_t messages_section_len =
        (messages_json && messages_len > 0) ? messages_len : 2; /* [] */
    size_t options_section_len =
        (options_len > 0 && options_start) ? 1 + options_len : 0; /* ,options */
    size_t total_len = prefix_len + messages_section_len + options_section_len +
                       1 /* } */ + 1; /* \0 */
    char *body = malloc(total_len);
    if (!body) {
        free(escaped_model);
        return NULL;
    }
    size_t pos = 0;
    pos += snprintf(body + pos, total_len - pos, "{\"model\":\"%s\",\"messages\":",
                    escaped_model);
    if (messages_json && messages_len > 0) {
        memcpy(body + pos, messages_json, messages_len);
        pos += messages_len;
    } else {
        body[pos++] = '[';
        body[pos++] = ']';
    }
    if (options_len > 0 && options_start) {
        body[pos++] = ',';
        memcpy(body + pos, options_start, options_len);
        pos += options_len;
    }
    body[pos++] = '}';
    body[pos] = '\0';
    free(escaped_model);
    return body;
}

static char *openaiBuildUrl(const char *base_url) {
    const char *path = "/chat/completions";
    if (!base_url || !*base_url) {
        base_url = "https://api.openai.com/v1";
    }
    size_t base_len = strlen(base_url);
    bool has_trailing_slash = base_len > 0 && base_url[base_len - 1] == '/';
    const char *path_tail = has_trailing_slash ? path + 1 : path;
    size_t total = base_len + strlen(path_tail) + 1;
    char *url = malloc(total);
    if (!url) {
        return NULL;
    }
    snprintf(url, total, "%s%s", base_url, path_tail);
    return url;
}

static Value vmBuiltinOpenAIChatCompletions(struct VM_s *vm, int arg_count,
                                            Value *args) {
    if (arg_count < 2 || arg_count > 5) {
        runtimeError(vm,
                     "OpenAIChatCompletions expects between 2 and 5 arguments.");
        return makeString("");
    }
    for (int i = 0; i < arg_count; ++i) {
        if (!IS_STRING(args[i])) {
            runtimeError(vm, "OpenAIChatCompletions expects string arguments.");
            return makeString("");
        }
    }
    const char *model = AS_STRING(args[0]);
    const char *messages_json = AS_STRING(args[1]);
    const char *options_json = (arg_count >= 3) ? AS_STRING(args[2]) : NULL;
    const char *api_key_arg = (arg_count >= 4) ? AS_STRING(args[3]) : NULL;
    const char *base_url_arg = (arg_count >= 5) ? AS_STRING(args[4]) : NULL;

    const char *api_key = NULL;
    if (api_key_arg && *api_key_arg) {
        api_key = api_key_arg;
    } else {
        api_key = getenv("OPENAI_API_KEY");
    }
    if (!api_key || !*api_key) {
        runtimeError(vm, "OpenAIChatCompletions requires an API key via argument"
                         " or OPENAI_API_KEY.");
        return makeString("");
    }

    char *body = openaiBuildRequestBody(model, messages_json, options_json);
    if (!body) {
        runtimeError(vm, "OpenAIChatCompletions failed to allocate request body.");
        return makeString("");
    }

    char *url = openaiBuildUrl(base_url_arg);
    if (!url) {
        free(body);
        runtimeError(vm, "OpenAIChatCompletions failed to build request URL.");
        return makeString("");
    }

    size_t auth_len = strlen("Authorization: Bearer ") + strlen(api_key) + 1;
    char *auth_header = malloc(auth_len);
    if (!auth_header) {
        free(body);
        free(url);
        runtimeError(vm, "OpenAIChatCompletions failed to allocate headers.");
        return makeString("");
    }
    snprintf(auth_header, auth_len, "Authorization: Bearer %s", api_key);

    CURL *curl = curl_easy_init();
    if (!curl) {
        free(body);
        free(url);
        free(auth_header);
        runtimeError(vm, "OpenAIChatCompletions could not initialise curl.");
        return makeString("");
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, auth_header);

    OpenAIBuffer buffer = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    long body_len = (long)strlen(body);
    if (body_len >= 0) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, body_len);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, openaiWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "PscalOpenAI/1.0");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        runtimeError(vm, "OpenAIChatCompletions request failed: %s",
                     curl_easy_strerror(res));
        if (buffer.data) {
            free(buffer.data);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        free(body);
        free(url);
        free(auth_header);
        return makeString("");
    }

    Value result = makeString(buffer.data ? buffer.data : "");

    if (buffer.data) {
        free(buffer.data);
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);
    free(url);
    free(auth_header);
    return result;
}

void registerOpenAIChatCompletionsBuiltin(void) {
    registerVmBuiltin("openaichatcompletions", vmBuiltinOpenAIChatCompletions,
                      BUILTIN_TYPE_FUNCTION, "OpenAIChatCompletions");
}
