/* src/builtin_network_api.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <pthread.h>
#include "backend_ast/builtin.h"
#include "Pascal/globals.h"
#include "core/utils.h"
#include "vm/vm.h"

/* Callback for libcurl: writes received data into a MStream */
static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    MStream *mstream = (MStream *)userp;

    if (real_size == 0) { // If no data in this call, just return.
        return 0; // Return 0 for no data, but not an error. Or should it be real_size? Libcurl expects real_size.
                  // If real_size is 0, it means libcurl isn't passing data. Let's return real_size.
    }
    
    if (mstream->size + real_size + 1 > mstream->capacity) {
        size_t new_capacity = mstream->size + real_size + 1;
        // Simple doubling strategy or specific increase
        if (new_capacity < mstream->capacity * 2 && mstream->capacity > 0) { // check mstream->capacity > 0
             new_capacity = mstream->capacity * 2;
        } else if (new_capacity < 16) { // ensure minimum capacity if doubling isn't enough or capacity was 0
            new_capacity = 16;
        }
        // Ensure new_capacity is at least what's absolutely needed
        if (new_capacity < mstream->size + real_size + 1) {
            new_capacity = mstream->size + real_size + 1;
        }



        unsigned char *new_buffer = realloc(mstream->buffer, new_capacity);
        if (!new_buffer) {
            fprintf(stderr, "Memory allocation error in write_callback (realloc)\n");
            return 0;
        }
        mstream->buffer = new_buffer;
        mstream->capacity = new_capacity;
    }

    memcpy(&(mstream->buffer[mstream->size]), contents, real_size);
    mstream->size += real_size;
    mstream->buffer[mstream->size] = '\0';

    char temp_chunk[65]; // Print up to 64 chars of the current chunk
    size_t print_len_chunk = real_size < 64 ? real_size : 64;
    strncpy(temp_chunk, (char*)contents, print_len_chunk);
    temp_chunk[print_len_chunk] = '\0';

    char temp_buffer[65]; // Print up to 64 chars of the accumulated buffer
    size_t print_len_buffer = mstream->size < 64 ? mstream->size : 64;
    strncpy(temp_buffer, (char*)mstream->buffer, print_len_buffer);
    temp_buffer[print_len_buffer] = '\0';

    return real_size;
}

/* headerAccumCallback declared later after HttpSession typedef */

// -------------------- Simple HTTP Session API (sync) --------------------

typedef struct HttpSession_s {
    CURL* curl;
    struct curl_slist* headers; // accumulated request headers
    long timeout_ms;
    long follow_redirects;
    char* user_agent;
    long last_status;
    int active;
    // TLS/Proxy options
    char* ca_path;        // CURLOPT_CAINFO
    char* client_cert;    // CURLOPT_SSLCERT
    char* client_key;     // CURLOPT_SSLKEY
    char* proxy;          // CURLOPT_PROXY
    long verify_peer;     // CURLOPT_SSL_VERIFYPEER (0/1)
    long verify_host;     // CURLOPT_SSL_VERIFYHOST (0/1 maps to 0 or 2)
    long force_http2;     // 0/1
    // Auth and last-results
    char* basic_auth;     // user:pass for basic auth
    char* last_headers;   // raw response headers from last request
    int   last_error_code; // VM-specific error code (0 on success)
    char* last_error_msg; // last libcurl error message
} HttpSession;

/* Callback for libcurl: accumulates raw response headers into the session */
static size_t headerAccumCallback(char *buffer, size_t size, size_t nitems, void *userdata) {
    size_t real_size = size * nitems;
    HttpSession* s = (HttpSession*)userdata;
    if (!s) return real_size;
    if (real_size == 0) return 0;
    size_t old_len = s->last_headers ? strlen(s->last_headers) : 0;
    char* nb = (char*)realloc(s->last_headers, old_len + real_size + 1);
    if (!nb) return real_size; // drop headers on OOM
    s->last_headers = nb;
    memcpy(s->last_headers + old_len, buffer, real_size);
    s->last_headers[old_len + real_size] = '\0';
    return real_size;
}

#define MAX_HTTP_SESSIONS 32
static HttpSession g_http_sessions[MAX_HTTP_SESSIONS];

static int httpAllocSession(void) {
    for (int i = 0; i < MAX_HTTP_SESSIONS; i++) {
    if (!g_http_sessions[i].active) {
        memset(&g_http_sessions[i], 0, sizeof(HttpSession));
        g_http_sessions[i].curl = curl_easy_init();
        if (!g_http_sessions[i].curl) return -1;
        g_http_sessions[i].timeout_ms = 15000; // default 15s
        g_http_sessions[i].follow_redirects = 1;
        g_http_sessions[i].user_agent = strdup("PscalInterpreter/1.0");
        g_http_sessions[i].last_status = 0;
        g_http_sessions[i].active = 1;
        g_http_sessions[i].verify_peer = 1;
        g_http_sessions[i].verify_host = 1;
        return i;
    }
    }
    return -1;
}

static void httpFreeSession(int id) {
    if (id < 0 || id >= MAX_HTTP_SESSIONS) return;
    HttpSession* s = &g_http_sessions[id];
    if (!s->active) return;
    if (s->headers) { curl_slist_free_all(s->headers); s->headers = NULL; }
    if (s->curl) { curl_easy_cleanup(s->curl); s->curl = NULL; }
    if (s->user_agent) { free(s->user_agent); s->user_agent = NULL; }
    if (s->basic_auth) { free(s->basic_auth); s->basic_auth = NULL; }
    if (s->ca_path) { free(s->ca_path); s->ca_path = NULL; }
    if (s->client_cert) { free(s->client_cert); s->client_cert = NULL; }
    if (s->client_key) { free(s->client_key); s->client_key = NULL; }
    if (s->proxy) { free(s->proxy); s->proxy = NULL; }
    if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
    if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
    s->active = 0;
}

static HttpSession* httpGet(int id) {
    if (id < 0 || id >= MAX_HTTP_SESSIONS) return NULL;
    if (!g_http_sessions[id].active) return NULL;
    return &g_http_sessions[id];
}

// httpSession(): Integer
Value vmBuiltinHttpSession(VM* vm, int arg_count, Value* args) {
    (void)args;
    if (arg_count != 0) {
        runtimeError(vm, "httpSession expects no arguments.");
        return makeInt(-1);
    }
    int id = httpAllocSession();
    if (id < 0) {
        runtimeError(vm, "httpSession: no free session slots or curl init failed.");
        return makeInt(-1);
    }
    return makeInt(id);
}

// httpClose(session): void
Value vmBuiltinHttpClose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpClose expects 1 integer session id.");
        return makeVoid();
    }
    int id = (int)AS_INTEGER(args[0]);
    httpFreeSession(id);
    return makeVoid();
}

// httpSetHeader(session, name, value): void
Value vmBuiltinHttpSetHeader(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING || args[2].type != TYPE_STRING) {
        runtimeError(vm, "httpSetHeader expects (session:int, name:string, value:string).");
        return makeVoid();
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) { runtimeError(vm, "httpSetHeader: invalid session id."); return makeVoid(); }
    size_t line_len = strlen(args[1].s_val) + 2 + strlen(args[2].s_val) + 1;
    char* line = (char*)malloc(line_len);
    if (!line) { runtimeError(vm, "httpSetHeader: malloc failed."); return makeVoid(); }
    snprintf(line, line_len, "%s: %s", args[1].s_val, args[2].s_val);
    s->headers = curl_slist_append(s->headers, line);
    free(line);
    return makeVoid();
}

// httpClearHeaders(session): void
Value vmBuiltinHttpClearHeaders(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpClearHeaders expects 1 integer session id.");
        return makeVoid();
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) { runtimeError(vm, "httpClearHeaders: invalid session id."); return makeVoid(); }
    if (s->headers) { curl_slist_free_all(s->headers); s->headers = NULL; }
    return makeVoid();
}

// httpSetOption(session, key, value): void (value can be int or string)
Value vmBuiltinHttpSetOption(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING) {
        runtimeError(vm, "httpSetOption expects (session:int, key:string, value:int|string).");
        return makeVoid();
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) { runtimeError(vm, "httpSetOption: invalid session id."); return makeVoid(); }
    const char* key = args[1].s_val;
    if (strcasecmp(key, "timeout_ms") == 0 && IS_INTLIKE(args[2])) {
        s->timeout_ms = (long)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "follow_redirects") == 0 && IS_INTLIKE(args[2])) {
        s->follow_redirects = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "user_agent") == 0 && args[2].type == TYPE_STRING) {
        if (s->user_agent) free(s->user_agent);
        s->user_agent = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "ca_path") == 0 && args[2].type == TYPE_STRING) {
        if (s->ca_path) free(s->ca_path);
        s->ca_path = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "client_cert") == 0 && args[2].type == TYPE_STRING) {
        if (s->client_cert) free(s->client_cert);
        s->client_cert = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "client_key") == 0 && args[2].type == TYPE_STRING) {
        if (s->client_key) free(s->client_key);
        s->client_key = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "proxy") == 0 && args[2].type == TYPE_STRING) {
        if (s->proxy) free(s->proxy);
        s->proxy = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "verify_peer") == 0 && IS_INTLIKE(args[2])) {
        s->verify_peer = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "verify_host") == 0 && IS_INTLIKE(args[2])) {
        s->verify_host = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "http2") == 0 && IS_INTLIKE(args[2])) {
        s->force_http2 = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "basic_auth") == 0 && args[2].type == TYPE_STRING) {
        if (s->basic_auth) free(s->basic_auth);
        s->basic_auth = strdup(args[2].s_val ? args[2].s_val : "");
    } else {
        runtimeError(vm, "httpSetOption: unsupported option or value type for '%s'.", key);
    }
    return makeVoid();
}

// httpRequest(session, method, url, bodyStrOrMStreamOrNil, outMStream): Integer (status)
Value vmBuiltinHttpRequest(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING || args[2].type != TYPE_STRING) {
        runtimeError(vm, "httpRequest expects (session:int, method:string, url:string, body:string|mstream|nil, out:mstream).");
        return makeInt(-1);
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) { runtimeError(vm, "httpRequest: invalid session id."); return makeInt(-1); }

    const char* method = args[1].s_val ? args[1].s_val : "GET";
    const char* url = args[2].s_val;
    const char* body_ptr = NULL;
    size_t body_len = 0;
    if (args[3].type == TYPE_STRING && args[3].s_val) {
        body_ptr = args[3].s_val; body_len = strlen(args[3].s_val);
    } else if (args[3].type == TYPE_MEMORYSTREAM && args[3].mstream) {
        body_ptr = (const char*)args[3].mstream->buffer; body_len = (size_t)args[3].mstream->size;
    } else if (args[3].type == TYPE_NIL) {
        // ok, no body
    } else {
        runtimeError(vm, "httpRequest: body must be string, mstream or nil.");
        return makeInt(-1);
    }
    if (args[4].type != TYPE_MEMORYSTREAM || !args[4].mstream) {
        runtimeError(vm, "httpRequest: out must be a valid mstream.");
        return makeInt(-1);
    }

    // Special-case local file URLs to avoid relying on libcurl's file:// support
    if (url && strncasecmp(url, "file://", 7) == 0) {
        // Clear last headers and errors
        if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
        if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
        s->last_error_code = 0;
        const char* path = url + 7; // e.g. file:///Users/... -> "/Users/..."
        // Clear output mstream before writing
        args[4].mstream->size = 0;
        if (args[4].mstream->buffer && args[4].mstream->capacity > 0) {
            args[4].mstream->buffer[0] = '\0';
        }

        FILE* f = fopen(path, "rb");
        if (!f) {
            if (s->last_error_msg) { free(s->last_error_msg); }
            s->last_error_msg = strdup("cannot open local file");
            s->last_error_code = 2; // VMERR_IO
            runtimeError(vm, "httpRequest: cannot open local file '%s'", path);
            return makeInt(-1);
        }
        fseek(f, 0, SEEK_END);
        long fsize = ftell(f);
        if (fsize < 0) { fsize = 0; }
        rewind(f);

        // Ensure capacity
        if (args[4].mstream->capacity < fsize + 1) {
            unsigned char* newbuf = (unsigned char*)realloc(args[4].mstream->buffer, (size_t)fsize + 1);
            if (!newbuf) {
                fclose(f);
                runtimeError(vm, "httpRequest: out-of-memory reading file '%s'", path);
                return makeInt(-1);
            }
            args[4].mstream->buffer = newbuf;
            args[4].mstream->capacity = (int)fsize + 1;
        }

        size_t nread = fread(args[4].mstream->buffer, 1, (size_t)fsize, f);
        fclose(f);
        args[4].mstream->size = (int)nread;
        if (args[4].mstream->buffer) {
            args[4].mstream->buffer[nread] = '\0';
        }
        // Mimic successful HTTP fetch
        s->last_status = 200;
        return makeInt(200);
    }

    // Prepare CURL easy handle
    curl_easy_reset(s->curl);
    // Reset last headers and errors
    if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
    if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
    s->last_error_code = 0;
    curl_easy_setopt(s->curl, CURLOPT_URL, url);
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, args[4].mstream);
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT_MS, s->timeout_ms);
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, s->follow_redirects);
    // Capture headers
    curl_easy_setopt(s->curl, CURLOPT_HEADERFUNCTION, headerAccumCallback);
    curl_easy_setopt(s->curl, CURLOPT_HEADERDATA, s);
    if (s->user_agent) curl_easy_setopt(s->curl, CURLOPT_USERAGENT, s->user_agent);
    if (s->headers) curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, s->headers);
    if (s->basic_auth && s->basic_auth[0]) {
        curl_easy_setopt(s->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(s->curl, CURLOPT_USERPWD, s->basic_auth);
    }

    // Method + body
    if (strcasecmp(method, "GET") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_HTTPGET, 1L);
    } else if (strcasecmp(method, "POST") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_POST, 1L);
        if (body_ptr && body_len > 0) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, body_ptr);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcasecmp(method, "PUT") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (body_ptr && body_len > 0) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, body_ptr);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    } else if (strcasecmp(method, "DELETE") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, method);
        if (body_ptr && body_len > 0) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, body_ptr);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
        }
    }

    // TLS/Proxy options
    if (s->ca_path && s->ca_path[0]) {
        curl_easy_setopt(s->curl, CURLOPT_CAINFO, s->ca_path);
    }
    if (s->client_cert && s->client_cert[0]) {
        curl_easy_setopt(s->curl, CURLOPT_SSLCERT, s->client_cert);
    }
    if (s->client_key && s->client_key[0]) {
        curl_easy_setopt(s->curl, CURLOPT_SSLKEY, s->client_key);
    }
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYPEER, s->verify_peer);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYHOST, s->verify_host ? 2L : 0L);
    if (s->proxy && s->proxy[0]) {
        curl_easy_setopt(s->curl, CURLOPT_PROXY, s->proxy);
    }
#ifdef CURLOPT_HTTP_VERSION
    if (s->force_http2) {
#ifdef CURL_HTTP_VERSION_2TLS
        curl_easy_setopt(s->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#elif defined(CURL_HTTP_VERSION_2_0)
        curl_easy_setopt(s->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#endif
    }
#endif

    // Clear out buffer of output mstream before writing
    args[4].mstream->size = 0;
    if (args[4].mstream->buffer && args[4].mstream->capacity > 0) {
        args[4].mstream->buffer[0] = '\0';
    }

    CURLcode res = curl_easy_perform(s->curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &http_code);
        s->last_status = http_code;
        return makeInt((int)http_code);
    } else {
        // Map curl error to a small VM error space
        // 1: generic, 2: I/O, 3: timeout, 4: ssl, 5: resolve, 6: connect
        int code = 1;
        switch (res) {
            case CURLE_OPERATION_TIMEDOUT: code = 3; break;
            case CURLE_SSL_CONNECT_ERROR:
            case CURLE_PEER_FAILED_VERIFICATION:
            case CURLE_SSL_CACERT:
#ifdef CURLE_SSL_CACERT_BADFILE
            case CURLE_SSL_CACERT_BADFILE:
#endif
            case CURLE_USE_SSL_FAILED: code = 4; break;
            case CURLE_COULDNT_RESOLVE_HOST:
            case CURLE_COULDNT_RESOLVE_PROXY: code = 5; break;
            case CURLE_COULDNT_CONNECT: code = 6; break;
            case CURLE_READ_ERROR:
            case CURLE_WRITE_ERROR:
            case CURLE_FILE_COULDNT_READ_FILE: code = 2; break;
            default: code = 1; break;
        }
        s->last_error_code = code;
        if (s->last_error_msg) free(s->last_error_msg);
        s->last_error_msg = strdup(curl_easy_strerror(res));
        runtimeError(vm, "httpRequest: curl failed: %s", curl_easy_strerror(res));
        return makeInt(-1);
    }
}

// -------------------- Existing simple helpers --------------------
/* Built–in function: apiSend(URL, requestBody)
   - URL is a string.
   - requestBody can be a string or a memory stream.
   Returns: a memory stream containing the API response.
*/
Value vmBuiltinApiSend(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2) {
        runtimeError(vm, "apiSend expects 2 arguments (URL: String, RequestBody: String/MStream).");
        return makeVoid(); // Return void on error
    }

    // Arg 0: URL (String)
    if (args[0].type != TYPE_STRING || args[0].s_val == NULL) {
        runtimeError(vm, "apiSend: URL argument must be a non-null string.");
        return makeVoid();
    }
    const char* url = args[0].s_val;

    // Arg 1: RequestBody (String or MStream)
    const char* request_body_content = NULL;
    if (args[1].type == TYPE_STRING) {
        request_body_content = args[1].s_val ? args[1].s_val : "";
    } else if (args[1].type == TYPE_MEMORYSTREAM && args[1].mstream != NULL) {
        request_body_content = (char*)args[1].mstream->buffer;
    } else {
        runtimeError(vm, "apiSend: Request body must be a string or memory stream.");
        return makeVoid();
    }

    // Initialize response stream
    MStream *response_stream = malloc(sizeof(MStream));
    if (!response_stream) {
        runtimeError(vm, "apiSend: Memory allocation error for response stream structure.");
        return makeVoid();
    }
    response_stream->buffer = malloc(16); // Initial small buffer
    if (!response_stream->buffer) {
        free(response_stream);
        runtimeError(vm, "apiSend: Memory allocation error for response stream buffer.");
        return makeVoid();
    }
    response_stream->buffer[0] = '\0';
    response_stream->size = 0;
    response_stream->capacity = 16;

    CURL *curl = curl_easy_init();
    if (!curl) {
        runtimeError(vm, "apiSend: curl initialization failed.");
        free(response_stream->buffer);
        free(response_stream);
        return makeVoid();
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_stream);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "PscalInterpreter/1.0"); // Custom User-Agent

    // Check if request_body_content is provided; if so, assume POST.
    // If it's an empty string or NULL, it defaults to GET.
    if (request_body_content && strlen(request_body_content) > 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body_content);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(request_body_content));
    }

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        runtimeError(vm, "apiSend: curl_easy_perform() failed: %s", curl_easy_strerror(res));
        // Free response_stream here as it's an error condition
        if (response_stream->buffer) free(response_stream->buffer);
        free(response_stream);
        return makeVoid();
    }
    if (http_code >= 400) {
        runtimeError(vm, "apiSend: HTTP request failed with code %ld. Response (partial):\n%s", http_code, response_stream->buffer ? (char*)response_stream->buffer : "(empty)");
        if (response_stream->buffer) free(response_stream->buffer);
        free(response_stream);
        return makeVoid();
    }

    // Return the response as a memory stream
    return makeMStream(response_stream);
}

Value vmBuiltinApiReceive(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1) {
        runtimeError(vm, "apiReceive expects 1 argument (MStream).");
        return makeString(""); // Return empty string on error
    }

    // Arg 0: MStream
    if (args[0].type != TYPE_MEMORYSTREAM || args[0].mstream == NULL) {
        runtimeError(vm, "apiReceive: Argument must be a valid MStream.");
        return makeString("");
    }
    MStream* mstream = args[0].mstream;

    // Ensure buffer is not NULL before creating string, even if size is 0
    const char* buffer_content = mstream->buffer ? (char*)mstream->buffer : "";
    Value result_string = makeString(buffer_content);

    // No need to free `args[0]` as it's an R-value representing the MStream object,
    // and the MStream object's memory is managed by MStreamFree.

    return result_string;
}

// -------------------- HTTP Async --------------------

typedef struct HttpAsyncJob_s {
    int active;
    pthread_t th;
    int session;
    char* method;
    char* url;
    char* body;
    size_t body_len;
    MStream* result; // allocated by job thread
    long status;
    char* error;
    int done;
} HttpAsyncJob;

#define MAX_HTTP_ASYNC 32
static HttpAsyncJob g_http_async[MAX_HTTP_ASYNC];

static int httpAllocAsync(void) {
    for (int i = 0; i < MAX_HTTP_ASYNC; i++) {
        if (!g_http_async[i].active) {
            memset(&g_http_async[i], 0, sizeof(HttpAsyncJob));
            g_http_async[i].active = 1;
            return i;
        }
    }
    return -1;
}

static void* httpAsyncThread(void* arg) {
    int id = (int)(intptr_t)arg;
    HttpAsyncJob* job = &g_http_async[id];
    HttpSession* s = httpGet(job->session);
    if (!s || !s->curl) {
        job->status = -1;
        job->error = strdup("invalid session");
        job->done = 1;
        return NULL;
    }
    // Set up output mstream
    job->result = (MStream*)malloc(sizeof(MStream));
    if (!job->result) {
        job->status = -1;
        job->error = strdup("malloc failed");
        job->done = 1;
        return NULL;
    }
    job->result->buffer = (unsigned char*)malloc(16);
    if (!job->result->buffer) {
        free(job->result); job->result = NULL;
        job->status = -1; job->error = strdup("malloc failed"); job->done = 1; return NULL;
    }
    job->result->buffer[0] = '\0'; job->result->size = 0; job->result->capacity = 16;

    // Configure CURL
    curl_easy_reset(s->curl);
    curl_easy_setopt(s->curl, CURLOPT_URL, job->url);
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, job->result);
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT_MS, s->timeout_ms);
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, s->follow_redirects);
    if (s->user_agent) curl_easy_setopt(s->curl, CURLOPT_USERAGENT, s->user_agent);
    if (s->headers) curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, s->headers);
    if (strcasecmp(job->method, "GET") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_HTTPGET, 1L);
    } else if (strcasecmp(job->method, "POST") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_POST, 1L);
        if (job->body && job->body_len > 0) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, job->body);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)job->body_len);
        }
    } else if (strcasecmp(job->method, "PUT") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "PUT");
        if (job->body && job->body_len > 0) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, job->body);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)job->body_len);
        }
    } else if (strcasecmp(job->method, "DELETE") == 0) {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    } else {
        curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, job->method);
        if (job->body && job->body_len > 0) {
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDS, job->body);
            curl_easy_setopt(s->curl, CURLOPT_POSTFIELDSIZE, (long)job->body_len);
        }
    }
    // TLS/proxy same as sync
    if (s->ca_path && s->ca_path[0]) curl_easy_setopt(s->curl, CURLOPT_CAINFO, s->ca_path);
    if (s->client_cert && s->client_cert[0]) curl_easy_setopt(s->curl, CURLOPT_SSLCERT, s->client_cert);
    if (s->client_key && s->client_key[0]) curl_easy_setopt(s->curl, CURLOPT_SSLKEY, s->client_key);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYPEER, s->verify_peer);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYHOST, s->verify_host ? 2L : 0L);
    if (s->proxy && s->proxy[0]) curl_easy_setopt(s->curl, CURLOPT_PROXY, s->proxy);
#ifdef CURLOPT_HTTP_VERSION
    if (s->force_http2) {
#ifdef CURL_HTTP_VERSION_2TLS
        curl_easy_setopt(s->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#elif defined(CURL_HTTP_VERSION_2_0)
        curl_easy_setopt(s->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#endif
    }
#endif

    CURLcode res = curl_easy_perform(s->curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &http_code);
        job->status = http_code;
    } else {
        job->status = -1;
        job->error = strdup(curl_easy_strerror(res));
    }
    job->done = 1;
    return NULL;
}

// HttpRequestAsync(session, method, url, bodyStrOrMStreamOrNil): Integer (async id)
Value vmBuiltinHttpRequestAsync(VM* vm, int arg_count, Value* args) {
    if (arg_count != 4 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING || args[2].type != TYPE_STRING) {
        runtimeError(vm, "httpRequestAsync expects (session:int, method:string, url:string, body:string|mstream|nil).");
        return makeInt(-1);
    }
    int id = httpAllocAsync();
    if (id < 0) { runtimeError(vm, "httpRequestAsync: no free slots."); return makeInt(-1); }
    HttpAsyncJob* job = &g_http_async[id];
    job->session = (int)AS_INTEGER(args[0]);
    job->method = strdup(args[1].s_val ? args[1].s_val : "GET");
    job->url = strdup(args[2].s_val ? args[2].s_val : "");
    if (args[3].type == TYPE_STRING && args[3].s_val) {
        job->body_len = strlen(args[3].s_val);
        job->body = (char*)malloc(job->body_len + 1);
        if (job->body) { memcpy(job->body, args[3].s_val, job->body_len + 1); }
    } else if (args[3].type == TYPE_MEMORYSTREAM && args[3].mstream) {
        job->body_len = (size_t)args[3].mstream->size;
        job->body = (char*)malloc(job->body_len + 1);
        if (job->body && args[3].mstream->buffer) {
            memcpy(job->body, args[3].mstream->buffer, job->body_len);
            job->body[job->body_len] = '\0';
        }
    }
    if (pthread_create(&job->th, NULL, httpAsyncThread, (void*)(intptr_t)id) != 0) {
        if (job->method) free(job->method);
        if (job->url) free(job->url);
        if (job->body) free(job->body);
        job->active = 0;
        runtimeError(vm, "httpRequestAsync: pthread_create failed.");
        return makeInt(-1);
    }
    return makeInt(id);
}

// HttpAwait(asyncId, out:mstream): Integer (status)
Value vmBuiltinHttpAwait(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_MEMORYSTREAM || !args[1].mstream) {
        runtimeError(vm, "httpAwait expects (id:int, out:mstream).");
        return makeInt(-1);
    }
    int id = (int)AS_INTEGER(args[0]);
    if (id < 0 || id >= MAX_HTTP_ASYNC) { runtimeError(vm, "httpAwait: invalid id."); return makeInt(-1); }
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { runtimeError(vm, "httpAwait: job not active."); return makeInt(-1); }
    pthread_join(job->th, NULL);
    int status = (int)job->status;
    // Copy result into provided mstream
    if (job->result && job->result->buffer) {
        MStream* out = args[1].mstream;
        // ensure capacity
        if ((size_t)out->capacity < job->result->size + 1) {
            unsigned char* nb = realloc(out->buffer, job->result->size + 1);
            if (nb) { out->buffer = nb; out->capacity = (int)(job->result->size + 1); }
        }
        memcpy(out->buffer, job->result->buffer, job->result->size);
        out->size = (int)job->result->size;
        out->buffer[out->size] = '\0';
    }
    // Free job resources
    if (job->result) { if (job->result->buffer) free(job->result->buffer); free(job->result); }
    if (job->method) free(job->method);
    if (job->url) free(job->url);
    if (job->body) free(job->body);
    if (job->error) free(job->error);
    memset(job, 0, sizeof(HttpAsyncJob));
    return makeInt(status);
}

// HttpLastError(session): String
Value vmBuiltinHttpLastError(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpLastError expects 1 integer session id.");
        return makeString("");
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) return makeString("invalid session");
    if (s->last_error_msg) return makeString(s->last_error_msg);
    return makeString("");
}

// HttpGetLastHeaders(session): String
Value vmBuiltinHttpGetLastHeaders(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpGetLastHeaders expects 1 integer session id.");
        return makeString("");
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) return makeString("invalid session");
    if (s->last_headers) return makeString(s->last_headers);
    return makeString("");
}

// HttpErrorCode(session): Integer (0=none; 1=generic; 2=io; 3=timeout; 4=ssl; 5=resolve; 6=connect)
Value vmBuiltinHttpErrorCode(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpErrorCode expects 1 integer session id.");
        return makeInt(-1);
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) return makeInt(-1);
    return makeInt(s->last_error_code);
}

// HttpGetHeader(session, name): String (value from last response headers; empty if not found)
Value vmBuiltinHttpGetHeader(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING) {
        runtimeError(vm, "httpGetHeader expects (session:int, name:string).");
        return makeString("");
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s || !s->last_headers) return makeString("");
    const char* name = args[1].s_val ? args[1].s_val : "";
    size_t name_len = strlen(name);
    if (name_len == 0) return makeString("");

    // Identify last header block (after final blank line)
    const char* all = s->last_headers;
    const char* block = all;
    const char* p = all;
    while (1) {
        const char* crlfcrlf = strstr(p, "\r\n\r\n");
        const char* lflf = strstr(p, "\n\n");
        const char* sep = NULL;
        if (crlfcrlf && lflf) sep = (crlfcrlf < lflf) ? crlfcrlf : lflf;
        else if (crlfcrlf) sep = crlfcrlf;
        else if (lflf) sep = lflf;
        else break;
        block = sep + ((sep == crlfcrlf) ? 4 : 2);
        p = block;
    }

    // Scan lines in the last block for header name
    const char* line = block;
    while (line && *line) {
        const char* eol = strchr(line, '\n');
        size_t linelen = eol ? (size_t)(eol - line) : strlen(line);
        // Trim trailing CR
        while (linelen > 0 && (line[linelen - 1] == '\r' || line[linelen - 1] == '\n')) linelen--;
        // Find colon
        const char* colon = memchr(line, ':', linelen);
        if (colon) {
            // header name range: line..colon
            size_t hlen = (size_t)(colon - line);
            // trim trailing spaces from name
            while (hlen > 0 && (line[hlen - 1] == ' ' || line[hlen - 1] == '\t')) hlen--;
            // compare case-insensitive
            if (hlen == name_len && strncasecmp(line, name, name_len) == 0) {
                // value begins after colon and spaces
                const char* valstart = colon + 1;
                // skip spaces/tabs
                while ((*valstart == ' ' || *valstart == '\t') && (valstart < line + linelen)) valstart++;
                size_t vlen = (size_t)((line + linelen) - valstart);
                // trim trailing spaces
                while (vlen > 0 && (valstart[vlen - 1] == ' ' || valstart[vlen - 1] == '\t')) vlen--;
                char* out = (char*)malloc(vlen + 1);
                if (!out) return makeString("");
                memcpy(out, valstart, vlen); out[vlen] = '\0';
                Value sv = makeString(out);
                free(out);
                return sv;
            }
        }
        if (!eol) break;
        line = eol + 1;
    }
    return makeString("");
}

// -------------------- Minimal JSON helper --------------------
// JsonGet(jsonStr, key) -> returns value as string for flat JSON (string/number/bool)
Value vmBuiltinJsonGet(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || args[0].type != TYPE_STRING || args[1].type != TYPE_STRING) {
        runtimeError(vm, "JsonGet expects (json:string, key:string).");
        return makeString("");
    }
    const char* json = args[0].s_val ? args[0].s_val : "";
    const char* key = args[1].s_val ? args[1].s_val : "";
    char pat[256]; snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(json, pat);
    if (!p) return makeString("");
    p = strchr(p + strlen(pat), ':');
    if (!p) return makeString("");
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        const char* q = strchr(p, '"');
        if (!q) return makeString("");
        size_t len = (size_t)(q - p);
        char* out = (char*)malloc(len + 1);
        if (!out) return makeString("");
        memcpy(out, p, len); out[len] = '\0';
        Value s = makeString(out); free(out); return s;
    }
    // number/bool until delimiter
    const char* q = p;
    while (*q && *q != ',' && *q != '}' && *q != ' ' && *q != '\t' && *q != '\n' && *q != '\r') q++;
    size_t len = (size_t)(q - p);
    char* out = (char*)malloc(len + 1);
    if (!out) return makeString("");
    memcpy(out, p, len); out[len] = '\0';
    Value s = makeString(out); free(out); return s;
}
