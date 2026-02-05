/* src/builtin_network_api.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <curl/curl.h>
#include <pthread.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#endif
#include "backend_ast/builtin.h"
#include "Pascal/globals.h"
#include "core/utils.h"
#include "vm/vm.h"
#include "vm/string_sentinels.h"
#include "common/pscal_hosts.h"

#ifdef _WIN32
static void ensure_winsock(void) {
    static int initialized = 0;
    if (!initialized) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2,2), &wsa);
        initialized = 1;
    }
}
#endif

static int g_socket_last_error = 0;
static char g_socket_last_error_msg[256];

typedef struct SocketInfo_s {
    int fd;
    int family;
    int socktype;
    struct SocketInfo_s* next;
} SocketInfo;

static SocketInfo* g_socket_info_list = NULL;

static int stringsEqualIgnoreCase(const char* a, const char* b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (tolower(ca) != tolower(cb)) {
            return 0;
        }
    }
    return *a == '\0' && *b == '\0';
}

static int isLocalhostName(const char* host) {
    if (!host) {
        return 0;
    }
    if (stringsEqualIgnoreCase(host, "localhost")) {
        return 1;
    }
    if (stringsEqualIgnoreCase(host, "localhost.")) {
        return 1;
    }
    return 0;
}

static Value makeLocalhostFallbackResult(void) {
    g_socket_last_error = 0;
    g_socket_last_error_msg[0] = '\0';
    return makeString("127.0.0.1");
}

static const Value* resolveStringPointer(const Value* value) {
    const Value* current = value;
    int depth = 0;
    while (current && current->type == TYPE_POINTER &&
           current->base_type_node != STRING_CHAR_PTR_SENTINEL) {
        if (!current->ptr_val) {
            return NULL;
        }
        current = (const Value*)current->ptr_val;
        if (++depth > 16) {
            return NULL;
        }
    }
    return current;
}

static int valueIsStringLike(const Value* value) {
    if (!value) return 0;
    if (value->type == TYPE_STRING) return 1;
    if (value->type == TYPE_POINTER) {
        if (value->base_type_node == STRING_CHAR_PTR_SENTINEL) return 1;
        const Value* resolved = resolveStringPointer(value);
        if (!resolved) return 0;
        if (resolved->type == TYPE_STRING) return 1;
        if (resolved->type == TYPE_POINTER && resolved->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return 1;
        }
    }
    return 0;
}

static const char* valueToCStringLike(const Value* value) {
    if (!value) return NULL;
    if (value->type == TYPE_STRING) return value->s_val ? value->s_val : "";
    if (value->type == TYPE_POINTER) {
        if (value->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return (const char*)value->ptr_val;
        }
        const Value* resolved = resolveStringPointer(value);
        if (!resolved) return NULL;
        if (resolved->type == TYPE_STRING) {
            return resolved->s_val ? resolved->s_val : "";
        }
        if (resolved->type == TYPE_POINTER && resolved->base_type_node == STRING_CHAR_PTR_SENTINEL) {
            return (const char*)resolved->ptr_val;
        }
    }
    return NULL;
}

static int valueIsNullCharPointer(const Value* value) {
    return value && value->type == TYPE_POINTER &&
           value->base_type_node == STRING_CHAR_PTR_SENTINEL && value->ptr_val == NULL;
}

static void registerSocketInfo(int fd, int family, int socktype) {
    SocketInfo* info = (SocketInfo*)malloc(sizeof(SocketInfo));
    if (!info) {
        // Allocation failure should not abort socket creation; fall back to
        // treating the socket as IPv4-only in lookups.
        return;
    }
    info->fd = fd;
    info->family = family;
    info->socktype = socktype;
    info->next = g_socket_info_list;
    g_socket_info_list = info;
}

static void unregisterSocketInfo(int fd) {
    SocketInfo* prev = NULL;
    SocketInfo* cur = g_socket_info_list;
    while (cur) {
        if (cur->fd == fd) {
            if (prev) {
                prev->next = cur->next;
            } else {
                g_socket_info_list = cur->next;
            }
            free(cur);
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

static int lookupSocketInfo(int fd, int* family_out, int* socktype_out) {
    for (SocketInfo* cur = g_socket_info_list; cur; cur = cur->next) {
        if (cur->fd == fd) {
            if (family_out) *family_out = cur->family;
            if (socktype_out) *socktype_out = cur->socktype;
            return 0;
        }
    }
    return -1;
}

#ifdef AF_INET6
static void mapIpv4ToIpv6(const struct sockaddr_in* in4, struct sockaddr_in6* out6) {
    memset(out6, 0, sizeof(*out6));
    out6->sin6_family = AF_INET6;
    out6->sin6_port = in4->sin_port;
    unsigned char* addr_bytes = (unsigned char*)&out6->sin6_addr;
    addr_bytes[10] = 0xff;
    addr_bytes[11] = 0xff;
    memcpy(addr_bytes + 12, &in4->sin_addr, sizeof(in4->sin_addr));
}
#endif

static int mapSocketError(int err) {
#ifdef _WIN32
    switch(err) {
        case WSAETIMEDOUT: return 3;
        case WSAECONNREFUSED:
        case WSAENETUNREACH:
        case WSAEHOSTUNREACH: return 6;
        case WSAHOST_NOT_FOUND:
        case WSANO_DATA: return 5;
        default: return 1;
    }
#else
    switch(err) {
        case ETIMEDOUT: return 3;
        case ECONNREFUSED:
        case ENETUNREACH:
        case EHOSTUNREACH: return 6;
        default: return 1;
    }
#endif
}

static void setSocketError(int err) {
    g_socket_last_error = mapSocketError(err);
#ifdef _WIN32
    snprintf(g_socket_last_error_msg, sizeof(g_socket_last_error_msg), "err %d", err);
#else
    snprintf(g_socket_last_error_msg, sizeof(g_socket_last_error_msg), "%s", strerror(err));
#endif
}

static bool socketConsumeInterrupt(VM *vm) {
    if (!pscalRuntimeConsumeSigint()) {
        return false;
    }
    if (vm) {
        vm->abort_requested = true;
        vm->exit_requested = true;
    }
#ifdef _WIN32
    setSocketError(WSAEINTR);
#else
    setSocketError(EINTR);
#endif
    return true;
}

#ifndef _WIN32
static int socketWaitReadable(VM *vm, int fd) {
    for (;;) {
        if (socketConsumeInterrupt(vm)) {
            return -1;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int res = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (res > 0) {
            return 0;
        }
        if (res == 0) {
            continue;
        }
        if (errno == EINTR) {
            continue;
        }
        setSocketError(errno);
        return -1;
    }
}
#endif

static void setSocketAddrInfoError(int err) {
#ifdef _WIN32
    switch (err) {
#ifdef EAI_AGAIN
        case EAI_AGAIN:
#endif
#ifdef WSATRY_AGAIN
        case WSATRY_AGAIN:
#endif
            g_socket_last_error = 3; // temporary failure
            break;
#ifdef EAI_NONAME
        case EAI_NONAME:
#endif
#ifdef EAI_NODATA
        case EAI_NODATA:
#endif
        case WSAHOST_NOT_FOUND:
        case WSANO_DATA:
            g_socket_last_error = 5; // host not found
            break;
        default:
            g_socket_last_error = 1;
            break;
    }
    const char* msg = gai_strerrorA(err);
    if (!msg) msg = "name resolution failure";
    snprintf(g_socket_last_error_msg, sizeof(g_socket_last_error_msg), "%s", msg);
#else
    switch (err) {
#ifdef EAI_AGAIN
        case EAI_AGAIN:
            g_socket_last_error = 3; // try again later
            break;
#endif
#ifdef EAI_NONAME
        case EAI_NONAME:
#endif
#ifdef EAI_NODATA
        case EAI_NODATA:
#endif
            g_socket_last_error = 5; // host not found
            break;
        default:
            g_socket_last_error = 1;
            break;
    }
    const char* msg = gai_strerror(err);
    if (!msg) msg = "name resolution failure";
    snprintf(g_socket_last_error_msg, sizeof(g_socket_last_error_msg), "%s", msg);
#endif
}

static void sleep_ms(long ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    if (ms <= 0) {
        usleep(0);
    } else {
        unsigned long long usec_total = (unsigned long long)ms * 1000ULL;
        useconds_t max_delay = (useconds_t)~(useconds_t)0;
        if (usec_total > (unsigned long long)max_delay) {
            usec_total = (unsigned long long)max_delay;
        }
        usleep((useconds_t)usec_total);
    }
#endif
}

typedef struct DataUrlPayload_s {
    unsigned char* data;
    size_t length;
    char* content_type;
} DataUrlPayload;

static void dataUrlPayloadFree(DataUrlPayload* payload) {
    if (!payload) return;
    if (payload->data) {
        free(payload->data);
        payload->data = NULL;
    }
    if (payload->content_type) {
        free(payload->content_type);
        payload->content_type = NULL;
    }
    payload->length = 0;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    c = (char)tolower((unsigned char)c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static int decodePercentEncoded(const char* input, size_t len, unsigned char** out, size_t* out_len) {
    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    size_t capacity = len + 1;
    if (capacity == 0) capacity = 1;
    unsigned char* buffer = (unsigned char*)malloc(capacity);
    if (!buffer) return -1;
    size_t pos = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '%') {
            if (i + 2 >= len) {
                free(buffer);
                return -2;
            }
            int hi = hexValue(input[i + 1]);
            int lo = hexValue(input[i + 2]);
            if (hi < 0 || lo < 0) {
                free(buffer);
                return -2;
            }
            buffer[pos++] = (unsigned char)((hi << 4) | lo);
            i += 2;
        } else {
            buffer[pos++] = c;
        }
    }
    buffer[pos] = '\0';
    *out = buffer;
    *out_len = pos;
    return 0;
}

static int base64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static int base64DecodeBuffer(const char* input, size_t len, unsigned char** out, size_t* out_len) {
    if (!out || !out_len) return -1;
    *out = NULL;
    *out_len = 0;
    char* clean = (char*)malloc(len + 1);
    if (!clean) return -1;
    size_t clean_len = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)input[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') continue;
        clean[clean_len++] = (char)c;
    }
    clean[clean_len] = '\0';
    if (clean_len == 0) {
        unsigned char* buffer = (unsigned char*)malloc(1);
        if (!buffer) {
            free(clean);
            return -1;
        }
        buffer[0] = '\0';
        *out = buffer;
        *out_len = 0;
        free(clean);
        return 0;
    }
    if (clean_len % 4 != 0) {
        free(clean);
        return -2;
    }
    size_t out_cap = ((clean_len + 3) / 4) * 3 + 1;
    unsigned char* buffer = (unsigned char*)malloc(out_cap);
    if (!buffer) {
        free(clean);
        return -1;
    }
    size_t pos = 0;
    for (size_t i = 0; i < clean_len; i += 4) {
        char c0 = clean[i];
        char c1 = clean[i + 1];
        char c2 = clean[i + 2];
        char c3 = clean[i + 3];
        int v0 = base64Value(c0);
        int v1 = base64Value(c1);
        if (v0 < 0 || v1 < 0) {
            free(buffer);
            free(clean);
            return -3;
        }
        int v2 = (c2 == '=') ? -2 : base64Value(c2);
        int v3 = (c3 == '=') ? -2 : base64Value(c3);
        if ((v2 < 0 && v2 != -2) || (v3 < 0 && v3 != -2)) {
            free(buffer);
            free(clean);
            return -3;
        }
        buffer[pos++] = (unsigned char)((v0 << 2) | (v1 >> 4));
        if (v2 == -2) {
            if (v3 != -2 || i + 4 != clean_len) {
                free(buffer);
                free(clean);
                return -3;
            }
            break;
        }
        buffer[pos++] = (unsigned char)(((v1 & 0xF) << 4) | (v2 >> 2));
        if (v3 == -2) {
            if (i + 4 != clean_len) {
                free(buffer);
                free(clean);
                return -3;
            }
            break;
        }
        buffer[pos++] = (unsigned char)(((v2 & 0x3) << 6) | v3);
    }
    buffer[pos] = '\0';
    *out = buffer;
    *out_len = pos;
    free(clean);
    return 0;
}

static int parseDataUrl(const char* url, DataUrlPayload* payload, char** err_out) {
    if (!payload) return -1;
    if (err_out) *err_out = NULL;
    payload->data = NULL;
    payload->length = 0;
    payload->content_type = NULL;
    if (!url) {
        if (err_out) *err_out = strdup("invalid data URL");
        return -1;
    }
    if (strncasecmp(url, "data:", 5) != 0) {
        if (err_out) *err_out = strdup("invalid data URL");
        return -1;
    }
    const char* comma = strchr(url + 5, ',');
    if (!comma) {
        if (err_out) *err_out = strdup("invalid data URL (missing comma)");
        return -1;
    }
    size_t meta_len = (size_t)(comma - (url + 5));
    size_t data_len = strlen(comma + 1);
    char* metadata = NULL;
    if (meta_len > 0) {
        metadata = (char*)malloc(meta_len + 1);
        if (!metadata) {
            if (err_out) *err_out = strdup("out of memory");
            return -1;
        }
        memcpy(metadata, url + 5, meta_len);
        metadata[meta_len] = '\0';
    }

    int base64_flag = 0;
    char* content_type_buf = NULL;
    int mediatype_set = 0;
    if (metadata) {
        char* cursor = metadata;
        while (1) {
            char* next = strchr(cursor, ';');
            if (next) *next = '\0';
            if (*cursor) {
                if (strcasecmp(cursor, "base64") == 0) {
                    base64_flag = 1;
                } else if (!mediatype_set && strchr(cursor, '/')) {
                    size_t token_len = strlen(cursor);
                    content_type_buf = (char*)malloc(token_len + 1);
                    if (!content_type_buf) {
                        if (err_out) *err_out = strdup("out of memory");
                        free(metadata);
                        return -1;
                    }
                    memcpy(content_type_buf, cursor, token_len + 1);
                    mediatype_set = 1;
                } else {
                    if (!mediatype_set) {
                        const char* def_media = "text/plain";
                        size_t def_len = strlen(def_media);
                        content_type_buf = (char*)malloc(def_len + 1);
                        if (!content_type_buf) {
                            if (err_out) *err_out = strdup("out of memory");
                            free(metadata);
                            return -1;
                        }
                        memcpy(content_type_buf, def_media, def_len + 1);
                        mediatype_set = 1;
                    }
                    size_t old_len = strlen(content_type_buf);
                    size_t token_len = strlen(cursor);
                    char* newbuf = (char*)realloc(content_type_buf, old_len + 1 + token_len + 1);
                    if (!newbuf) {
                        if (err_out) *err_out = strdup("out of memory");
                        free(metadata);
                        free(content_type_buf);
                        return -1;
                    }
                    newbuf[old_len] = ';';
                    memcpy(newbuf + old_len + 1, cursor, token_len + 1);
                    content_type_buf = newbuf;
                }
            }
            if (!next) break;
            cursor = next + 1;
        }
    }
    if (!mediatype_set) {
        const char* def_ct = "text/plain;charset=US-ASCII";
        content_type_buf = strdup(def_ct);
        if (!content_type_buf) {
            if (err_out) *err_out = strdup("out of memory");
            if (metadata) free(metadata);
            return -1;
        }
        mediatype_set = 1;
    }

    unsigned char* percent_buf = NULL;
    size_t percent_len = 0;
    unsigned char* final_buf = NULL;
    size_t final_len = 0;
    int rc;
    const char* data_part = comma + 1;

    if (base64_flag) {
        rc = decodePercentEncoded(data_part, data_len, &percent_buf, &percent_len);
        if (rc == -1) {
            if (err_out) *err_out = strdup("out of memory");
            goto fail;
        } else if (rc != 0) {
            if (err_out) *err_out = strdup("invalid percent-encoding in data URL");
            goto fail;
        }
        rc = base64DecodeBuffer((const char*)percent_buf, percent_len, &final_buf, &final_len);
        if (rc == -1) {
            if (err_out) *err_out = strdup("out of memory");
            goto fail;
        } else if (rc != 0) {
            if (err_out) *err_out = strdup("invalid base64 content in data URL");
            goto fail;
        }
    } else {
        rc = decodePercentEncoded(data_part, data_len, &final_buf, &final_len);
        if (rc == -1) {
            if (err_out) *err_out = strdup("out of memory");
            goto fail;
        } else if (rc != 0) {
            if (err_out) *err_out = strdup("invalid percent-encoding in data URL");
            goto fail;
        }
    }

    if (percent_buf) {
        free(percent_buf);
        percent_buf = NULL;
    }

    payload->data = final_buf;
    payload->length = final_len;
    payload->content_type = content_type_buf;
    if (metadata) free(metadata);
    return 0;

fail:
    if (metadata) free(metadata);
    if (content_type_buf) free(content_type_buf);
    if (percent_buf) free(percent_buf);
    if (final_buf) free(final_buf);
    if (err_out && !*err_out) {
        *err_out = strdup("invalid data URL");
    }
    return -1;
}

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



        if (new_capacity > (size_t)INT_MAX) {
            fprintf(stderr, "Memory allocation error in write_callback: capacity overflow\n");
            return 0;
        }

        unsigned char *new_buffer = realloc(mstream->buffer, new_capacity);
        if (!new_buffer) {
            fprintf(stderr, "Memory allocation error in write_callback (realloc)\n");
            return 0;
        }
        mstream->buffer = new_buffer;
        mstream->capacity = (int)new_capacity;
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

/* Forward declare async job for header callback */
typedef struct HttpAsyncJob_s HttpAsyncJob;

/* Helper to tee response to FILE and MStream */
typedef struct DualSink_s {
    FILE* f;
    MStream* ms;
} DualSink;

static size_t dualWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    DualSink* d = (DualSink*)userp;
    if (!d) return 0;
    if (d->f) {
        size_t n = fwrite(contents, 1, real_size, d->f);
        if (n != real_size) return 0;
    }
    if (d->ms) {
        size_t n2 = writeCallback(contents, size, nmemb, (void*)d->ms);
        if (n2 != real_size) return 0;
    }
    return real_size;
}

/* Header accumulator for async jobs declared after HttpAsyncJob definition below */

/* Callback: writes received data directly to a FILE* */
static size_t fileWriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    FILE* f = (FILE*)userp;
    if (!f) return 0;
    size_t n = fwrite(contents, 1, real_size, f);
    return n;
}

/* headerAccumCallback declared later after HttpSession typedef */

// -------------------- Simple HTTP Session API (sync) --------------------

typedef struct HttpSession_s {
    CURL* curl;
    struct curl_slist* headers; // accumulated request headers
    struct curl_slist* resolve; // host:port:address entries
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
    char* proxy_userpwd;  // CURLOPT_PROXYUSERPWD
    long proxy_type;      // CURLOPT_PROXYTYPE
    long verify_peer;     // CURLOPT_SSL_VERIFYPEER (0/1)
    long verify_host;     // CURLOPT_SSL_VERIFYHOST (0/1 maps to 0 or 2)
    long force_http2;     // 0/1
    long alpn;            // 0/1, CURLOPT_SSL_ENABLE_ALPN
    long tls_min;         // 10/11/12/13 -> TLS versions
    long tls_max;         // 10/11/12/13 -> TLS max
    char* ciphers;        // CURLOPT_SSL_CIPHER_LIST
    char* pinned_pubkey;  // CURLOPT_PINNEDPUBLICKEY
    char* out_file;       // optional sink for HttpRequest
    char* accept_encoding; // CURLOPT_ACCEPT_ENCODING
    long  accept_encoding_disabled; // explicitly cleared by user
    char* cookie_file;    // CURLOPT_COOKIEFILE
    char* cookie_jar;     // CURLOPT_COOKIEJAR
    long max_retries;     // number of retries
    long retry_delay_ms;  // initial backoff delay
    curl_off_t max_recv_speed; // rate limiting
    curl_off_t max_send_speed;
    char* upload_file;    // path for streaming upload
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
        g_http_sessions[i].max_retries = 0;
        g_http_sessions[i].retry_delay_ms = 0;
        g_http_sessions[i].max_recv_speed = 0;
        g_http_sessions[i].max_send_speed = 0;
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
    if (s->out_file) { free(s->out_file); s->out_file = NULL; }
    if (s->accept_encoding) { free(s->accept_encoding); s->accept_encoding = NULL; }
    s->accept_encoding_disabled = 0;
    if (s->cookie_file) { free(s->cookie_file); s->cookie_file = NULL; }
    if (s->cookie_jar) { free(s->cookie_jar); s->cookie_jar = NULL; }
    if (s->basic_auth) { free(s->basic_auth); s->basic_auth = NULL; }
    if (s->ca_path) { free(s->ca_path); s->ca_path = NULL; }
    if (s->client_cert) { free(s->client_cert); s->client_cert = NULL; }
    if (s->client_key) { free(s->client_key); s->client_key = NULL; }
    if (s->proxy) { free(s->proxy); s->proxy = NULL; }
    if (s->proxy_userpwd) { free(s->proxy_userpwd); s->proxy_userpwd = NULL; }
    if (s->ciphers) { free(s->ciphers); s->ciphers = NULL; }
    if (s->pinned_pubkey) { free(s->pinned_pubkey); s->pinned_pubkey = NULL; }
    if (s->upload_file) { free(s->upload_file); s->upload_file = NULL; }
    if (s->resolve) { curl_slist_free_all(s->resolve); s->resolve = NULL; }
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
    } else if (strcasecmp(key, "proxy_userpwd") == 0 && args[2].type == TYPE_STRING) {
        if (s->proxy_userpwd) free(s->proxy_userpwd);
        s->proxy_userpwd = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "proxy_type") == 0 && args[2].type == TYPE_STRING) {
        const char* v = args[2].s_val ? args[2].s_val : "";
        if (strcasecmp(v, "http") == 0) s->proxy_type = (long)CURLPROXY_HTTP;
#ifdef CURLPROXY_HTTPS
        else if (strcasecmp(v, "https") == 0) s->proxy_type = (long)CURLPROXY_HTTPS;
#endif
        else if (strcasecmp(v, "socks5") == 0) s->proxy_type = (long)CURLPROXY_SOCKS5;
        else if (strcasecmp(v, "socks4") == 0) s->proxy_type = (long)CURLPROXY_SOCKS4;
    } else if (strcasecmp(key, "tls_min") == 0 && IS_INTLIKE(args[2])) {
        s->tls_min = (long)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "tls_max") == 0 && IS_INTLIKE(args[2])) {
        s->tls_max = (long)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "ciphers") == 0 && args[2].type == TYPE_STRING) {
        if (s->ciphers) free(s->ciphers);
        s->ciphers = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "alpn") == 0 && IS_INTLIKE(args[2])) {
        s->alpn = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "pin_sha256") == 0 && args[2].type == TYPE_STRING) {
        if (s->pinned_pubkey) free(s->pinned_pubkey);
        s->pinned_pubkey = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "resolve_add") == 0 && args[2].type == TYPE_STRING) {
        s->resolve = curl_slist_append(s->resolve, args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "resolve_clear") == 0) {
        if (s->resolve) { curl_slist_free_all(s->resolve); s->resolve = NULL; }
    } else if (strcasecmp(key, "verify_peer") == 0 && IS_INTLIKE(args[2])) {
        s->verify_peer = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "verify_host") == 0 && IS_INTLIKE(args[2])) {
        s->verify_host = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "http2") == 0 && IS_INTLIKE(args[2])) {
        s->force_http2 = (long)AS_INTEGER(args[2]) ? 1L : 0L;
    } else if (strcasecmp(key, "basic_auth") == 0 && args[2].type == TYPE_STRING) {
        if (s->basic_auth) free(s->basic_auth);
        s->basic_auth = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "out_file") == 0 && args[2].type == TYPE_STRING) {
        if (s->out_file) free(s->out_file);
        s->out_file = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "accept_encoding") == 0) {
        /* Always clear the curl handle so callers can disable compression
           even after a request has set the default empty string. */
        curl_easy_setopt(s->curl, CURLOPT_ACCEPT_ENCODING, NULL);
        if (s->accept_encoding) {
            free(s->accept_encoding);
            s->accept_encoding = NULL;
        }
        if (args[2].type == TYPE_STRING) {
            s->accept_encoding = strdup(args[2].s_val ? args[2].s_val : "");
            s->accept_encoding_disabled = 0;
        } else if (IS_INTLIKE(args[2])) {
            s->accept_encoding_disabled = 1;
        } else {
            runtimeError(vm, "httpSetOption: accept_encoding expects string or int.");
        }
    } else if (strcasecmp(key, "cookie_file") == 0 && args[2].type == TYPE_STRING) {
        if (s->cookie_file) free(s->cookie_file);
        s->cookie_file = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "cookie_jar") == 0 && args[2].type == TYPE_STRING) {
        if (s->cookie_jar) free(s->cookie_jar);
        s->cookie_jar = strdup(args[2].s_val ? args[2].s_val : "");
    } else if (strcasecmp(key, "retry_max") == 0 && IS_INTLIKE(args[2])) {
        s->max_retries = (long)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "retry_delay_ms") == 0 && IS_INTLIKE(args[2])) {
        s->retry_delay_ms = (long)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "max_recv_speed") == 0 && IS_INTLIKE(args[2])) {
        s->max_recv_speed = (curl_off_t)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "max_send_speed") == 0 && IS_INTLIKE(args[2])) {
        s->max_send_speed = (curl_off_t)AS_INTEGER(args[2]);
    } else if (strcasecmp(key, "upload_file") == 0 && args[2].type == TYPE_STRING) {
        if (s->upload_file) free(s->upload_file);
        s->upload_file = strdup(args[2].s_val ? args[2].s_val : "");
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
        unsigned char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            if (args[4].mstream->capacity < args[4].mstream->size + (int)n + 1) {
                int newcap = args[4].mstream->size + (int)n + 1;
                unsigned char* newbuf = (unsigned char*)realloc(args[4].mstream->buffer, (size_t)newcap);
                if (!newbuf) { fclose(f); runtimeError(vm, "httpRequest: out-of-memory reading file '%s'", path); return makeInt(-1); }
                args[4].mstream->buffer = newbuf;
                args[4].mstream->capacity = newcap;
            }
            memcpy(args[4].mstream->buffer + args[4].mstream->size, buf, n);
            args[4].mstream->size += (int)n;
        }
        fclose(f);
        size_t nread = (size_t)args[4].mstream->size;
        while (nread > 0 && (args[4].mstream->buffer[nread-1] == '\n' || args[4].mstream->buffer[nread-1] == '\r')) nread--;
        args[4].mstream->size = (int)nread;
        if (args[4].mstream->capacity < (int)nread + 1) {
            unsigned char* newbuf = (unsigned char*)realloc(args[4].mstream->buffer, nread + 1);
            if (!newbuf) { runtimeError(vm, "httpRequest: out-of-memory reading file '%s'", path); return makeInt(-1); }
            args[4].mstream->buffer = newbuf;
            args[4].mstream->capacity = (int)nread + 1;
        }
        args[4].mstream->buffer[nread] = '\0';
        // If an out_file is configured, mirror content to that file
        if (s->out_file && s->out_file[0] && args[4].mstream && args[4].mstream->buffer) {
            FILE* of = fopen(s->out_file, "wb");
            if (of) {
                fwrite(args[4].mstream->buffer, 1, (size_t)args[4].mstream->size, of);
                fclose(of);
            } else {
                if (s->last_error_msg) free(s->last_error_msg);
                s->last_error_msg = strdup("cannot open out_file");
                s->last_error_code = 2;
            }
        }

        // Synthesize a minimal header block for testability
        const char* content_type = "application/octet-stream";
        size_t path_len = strlen(path);
        if (path_len >= 4) {
            const char* ext = path + path_len - 4;
            if (strcasecmp(ext, ".txt") == 0) content_type = "text/plain";
            else if (strcasecmp(ext, ".htm") == 0 || strcasecmp(ext, ".html") == 0) content_type = "text/html";
            else if (strcasecmp(ext, ".json") == 0) content_type = "application/json";
        }
        char hdr[256];
        int hdrlen = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\nContent-Length: %d\r\nContent-Type: %s\r\n\r\n",
                              (int)nread, content_type);
        if (hdrlen > 0) {
            s->last_headers = (char*)malloc((size_t)hdrlen + 1);
            if (s->last_headers) {
                memcpy(s->last_headers, hdr, (size_t)hdrlen + 1);
            }
        }
        // Mimic successful HTTP fetch
        s->last_status = 200;
        return makeInt(200);
    }
    else if (url && strncasecmp(url, "data:", 5) == 0) {
        if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
        if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
        s->last_error_code = 0;

        DataUrlPayload payload = {0};
        char* err_msg = NULL;
        if (parseDataUrl(url, &payload, &err_msg) != 0) {
            s->last_error_code = 2;
            if (s->last_error_msg) free(s->last_error_msg);
            s->last_error_msg = err_msg ? err_msg : strdup("invalid data URL");
            if (!s->last_error_msg) s->last_error_msg = strdup("invalid data URL");
            runtimeError(vm, "httpRequest: %s", s->last_error_msg ? s->last_error_msg : "invalid data URL");
            dataUrlPayloadFree(&payload);
            return makeInt(-1);
        }

        size_t required = payload.length + 1;
        if (required == 0) required = 1;
        if (args[4].mstream->capacity < (int)required || !args[4].mstream->buffer) {
            unsigned char* newbuf = (unsigned char*)realloc(args[4].mstream->buffer, required);
            if (!newbuf) {
                dataUrlPayloadFree(&payload);
                if (err_msg) free(err_msg);
                s->last_error_code = 2;
                s->last_error_msg = strdup("out of memory");
                runtimeError(vm, "httpRequest: out of memory handling data URL");
                return makeInt(-1);
            }
            args[4].mstream->buffer = newbuf;
            args[4].mstream->capacity = (int)required;
        }
        if (!args[4].mstream->buffer) {
            dataUrlPayloadFree(&payload);
            if (err_msg) free(err_msg);
            s->last_error_code = 2;
            s->last_error_msg = strdup("out of memory");
            runtimeError(vm, "httpRequest: out of memory handling data URL");
            return makeInt(-1);
        }

        if (payload.length > 0) {
            memcpy(args[4].mstream->buffer, payload.data, payload.length);
        }
        args[4].mstream->size = (int)payload.length;
        args[4].mstream->buffer[payload.length] = '\0';

        if (s->out_file && s->out_file[0]) {
            FILE* of = fopen(s->out_file, "wb");
            if (of) {
                if (payload.length > 0) fwrite(payload.data, 1, payload.length, of);
                fclose(of);
            } else {
                if (s->last_error_msg) free(s->last_error_msg);
                s->last_error_msg = strdup("cannot open out_file");
                s->last_error_code = 2;
            }
        }

        const char* content_type = payload.content_type ? payload.content_type : "text/plain;charset=US-ASCII";
        int hdrlen = snprintf(NULL, 0,
                              "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                              payload.length, content_type);
        if (hdrlen >= 0) {
            s->last_headers = (char*)malloc((size_t)hdrlen + 1);
            if (s->last_headers) {
                snprintf(s->last_headers, (size_t)hdrlen + 1,
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                         payload.length, content_type);
            }
        }

        s->last_status = 200;
        dataUrlPayloadFree(&payload);
        if (err_msg) free(err_msg);
        return makeInt(200);
    }

    // Prepare CURL easy handle
    curl_easy_reset(s->curl);
    // Reset last headers and errors
    if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
    if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
    s->last_error_code = 0;
    curl_easy_setopt(s->curl, CURLOPT_URL, url);
    // Choose sink: memory stream only or file+stream
    FILE* tmp_out_file = NULL; // closed before return
    DualSink dual = (DualSink){0};
    if (s->out_file && s->out_file[0]) {
        tmp_out_file = fopen(s->out_file, "wb");
        if (!tmp_out_file) {
            s->last_error_code = 2;
            if (s->last_error_msg) free(s->last_error_msg);
            s->last_error_msg = strdup("cannot open out_file");
            runtimeError(vm, "httpRequest: cannot open out_file '%s'", s->out_file);
            return makeInt(-1);
        }
        dual.f = tmp_out_file;
        dual.ms = args[4].mstream;
        curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, dualWriteCallback);
        curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, &dual);
    } else {
        curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, args[4].mstream);
    }
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT_MS, s->timeout_ms);
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, s->follow_redirects);
    // Capture headers
    curl_easy_setopt(s->curl, CURLOPT_HEADERFUNCTION, headerAccumCallback);
    curl_easy_setopt(s->curl, CURLOPT_HEADERDATA, s);
    if (s->user_agent) curl_easy_setopt(s->curl, CURLOPT_USERAGENT, s->user_agent);
    if (s->headers) curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, s->headers);
    if (s->resolve) curl_easy_setopt(s->curl, CURLOPT_RESOLVE, s->resolve);
    if (!s->accept_encoding_disabled) {
        curl_easy_setopt(s->curl, CURLOPT_ACCEPT_ENCODING,
                         s->accept_encoding ? s->accept_encoding : "");
    }
    if (s->cookie_file) curl_easy_setopt(s->curl, CURLOPT_COOKIEFILE, s->cookie_file);
    if (s->cookie_jar) curl_easy_setopt(s->curl, CURLOPT_COOKIEJAR, s->cookie_jar);
    if (s->max_recv_speed > 0) curl_easy_setopt(s->curl, CURLOPT_MAX_RECV_SPEED_LARGE, s->max_recv_speed);
    if (s->max_send_speed > 0) curl_easy_setopt(s->curl, CURLOPT_MAX_SEND_SPEED_LARGE, s->max_send_speed);
    if (s->basic_auth && s->basic_auth[0]) {
        curl_easy_setopt(s->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(s->curl, CURLOPT_USERPWD, s->basic_auth);
    }
    FILE* upload_fp = NULL;
    if (s->upload_file && s->upload_file[0]) {
        upload_fp = fopen(s->upload_file, "rb");
        if (!upload_fp) {
            s->last_error_code = 2;
            if (s->last_error_msg) free(s->last_error_msg);
            s->last_error_msg = strdup("cannot open upload file");
            if (tmp_out_file) fclose(tmp_out_file);
            runtimeError(vm, "httpRequest: cannot open upload_file '%s'", s->upload_file);
            return makeInt(-1);
        }
        curl_easy_setopt(s->curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(s->curl, CURLOPT_READDATA, upload_fp);
        fseeko(upload_fp, 0, SEEK_END);
        curl_off_t up_size = ftello(upload_fp);
        fseeko(upload_fp, 0, SEEK_SET);
        curl_easy_setopt(s->curl, CURLOPT_INFILESIZE_LARGE, up_size);
        if (strcasecmp(method, "POST") == 0) {
            curl_easy_setopt(s->curl, CURLOPT_POST, 1L);
        } else if (strcasecmp(method, "PUT") == 0) {
            curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "PUT");
        } else {
            curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, method);
        }
    } else {
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
        if (s->proxy_userpwd && s->proxy_userpwd[0]) curl_easy_setopt(s->curl, CURLOPT_PROXYUSERPWD, s->proxy_userpwd);
        if (s->proxy_type) curl_easy_setopt(s->curl, CURLOPT_PROXYTYPE, s->proxy_type);
    }
#ifdef CURLOPT_HTTP_VERSION
    if (s->force_http2) {
#ifdef CURL_HTTP_VERSION_2TLS
        curl_easy_setopt(s->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#elif defined(CURL_HTTP_VERSION_2_0)
        curl_easy_setopt(s->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#endif
    // Extra TLS knobs
#ifdef CURLOPT_SSL_ENABLE_ALPN
    curl_easy_setopt(s->curl, CURLOPT_SSL_ENABLE_ALPN, s->alpn);
#endif
    if (s->tls_min) {
        long v = 0;
        switch (s->tls_min) {
            case 10: v = CURL_SSLVERSION_TLSv1; break;
#ifdef CURL_SSLVERSION_TLSv1_1
            case 11: v = CURL_SSLVERSION_TLSv1_1; break;
#endif
#ifdef CURL_SSLVERSION_TLSv1_2
            case 12: v = CURL_SSLVERSION_TLSv1_2; break;
#endif
#ifdef CURL_SSLVERSION_TLSv1_3
            case 13: v = CURL_SSLVERSION_TLSv1_3; break;
#endif
        }
        if (v) curl_easy_setopt(s->curl, CURLOPT_SSLVERSION, v);
    }
#ifdef CURL_SSLVERSION_MAX_DEFAULT
    if (s->tls_max) {
        long vmax = 0;
#ifdef CURL_SSLVERSION_MAX_TLSv1_0
        if (s->tls_max == 10) vmax = CURL_SSLVERSION_MAX_TLSv1_0;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_1
        if (s->tls_max == 11) vmax = CURL_SSLVERSION_MAX_TLSv1_1;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_2
        if (s->tls_max == 12) vmax = CURL_SSLVERSION_MAX_TLSv1_2;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_3
        if (s->tls_max == 13) vmax = CURL_SSLVERSION_MAX_TLSv1_3;
#endif
        if (vmax) curl_easy_setopt(s->curl, CURLOPT_SSLVERSION, vmax);
    }
#endif
    if (s->ciphers && s->ciphers[0]) curl_easy_setopt(s->curl, CURLOPT_SSL_CIPHER_LIST, s->ciphers);
#ifdef CURLOPT_PINNEDPUBLICKEY
    if (s->pinned_pubkey && s->pinned_pubkey[0]) curl_easy_setopt(s->curl, CURLOPT_PINNEDPUBLICKEY, s->pinned_pubkey);
#endif
    }
#endif

    // Clear out buffer of output mstream before writing
    args[4].mstream->size = 0;
    if (args[4].mstream->buffer && args[4].mstream->capacity > 0) {
        args[4].mstream->buffer[0] = '\0';
    }

    long http_code = 0;
    CURLcode res = CURLE_OK;
    long delay = s->retry_delay_ms;
    int attempt = 0;
    while (1) {
        res = curl_easy_perform(s->curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code < 500) break;
        }
        if (attempt >= s->max_retries) break;
        attempt++;
        if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
        if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
        args[4].mstream->size = 0;
        if (args[4].mstream->buffer && args[4].mstream->capacity > 0) args[4].mstream->buffer[0] = '\0';
        if (tmp_out_file) {
            fclose(tmp_out_file);
            tmp_out_file = fopen(s->out_file, "wb");
            if (!tmp_out_file) {
                s->last_error_code = 2;
                if (s->last_error_msg) free(s->last_error_msg);
                s->last_error_msg = strdup("cannot open out_file");
                break;
            }
            dual.f = tmp_out_file;
        }
        if (upload_fp) fseeko(upload_fp, 0, SEEK_SET);
        if (delay > 0) { sleep_ms(delay); delay *= 2; }
    }

    if (upload_fp) fclose(upload_fp);

    if (res == CURLE_OK && http_code < 500) {
        s->last_status = http_code;
        if (tmp_out_file) fclose(tmp_out_file);
        return makeInt((int)http_code);
    }

    if (res != CURLE_OK) {
        // Map curl error to a small VM error space
        // 1: generic, 2: I/O, 3: timeout, 4: ssl, 5: resolve, 6: connect
        int code = 1;
        switch (res) {
            case CURLE_OPERATION_TIMEDOUT: code = 3; break;
            case CURLE_SSL_CONNECT_ERROR:
            case CURLE_PEER_FAILED_VERIFICATION:
#if defined(CURLE_SSL_CACERT) && (CURLE_SSL_CACERT != CURLE_PEER_FAILED_VERIFICATION)
            case CURLE_SSL_CACERT:
#endif
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
        s->last_status = -code;
        if (tmp_out_file) fclose(tmp_out_file);
        return makeInt(-code);
    }

    s->last_status = http_code;
    s->last_error_code = 1;
    if (s->last_error_msg) free(s->last_error_msg);
    char errbuf[64];
    snprintf(errbuf, sizeof(errbuf), "HTTP status %ld", http_code);
    s->last_error_msg = strdup(errbuf);
    if (tmp_out_file) fclose(tmp_out_file);
    return makeInt((int)http_code);
}

// httpRequestToFile(session, method, url, bodyStrOrMStreamOrNil, outFilename): Integer (status)
Value vmBuiltinHttpRequestToFile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING || args[2].type != TYPE_STRING) {
        runtimeError(vm, "httpRequestToFile expects (session:int, method:string, url:string, body:string|mstream|nil, out:string).");
        return makeInt(-1);
    }
    HttpSession* s = httpGet((int)AS_INTEGER(args[0]));
    if (!s) { runtimeError(vm, "httpRequestToFile: invalid session id."); return makeInt(-1); }

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
        runtimeError(vm, "httpRequestToFile: body must be string, mstream or nil.");
        return makeInt(-1);
    }
    if (args[4].type != TYPE_STRING || !args[4].s_val) {
        runtimeError(vm, "httpRequestToFile: out must be a filename string.");
        return makeInt(-1);
    }
    const char* out_path = args[4].s_val;

    // Reset last headers and errors
    if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
    if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
    s->last_error_code = 0;

    // Handle file:// specially
    if (url && strncasecmp(url, "file://", 7) == 0) {
        const char* path = url + 7;
        FILE* in = fopen(path, "rb");
        if (!in) {
            s->last_error_code = 2;
            s->last_error_msg = strdup("cannot open local file");
            runtimeError(vm, "httpRequestToFile: cannot open local file '%s'", path);
            return makeInt(-1);
        }
        FILE* out = fopen(out_path, "wb");
        if (!out) {
            fclose(in);
            s->last_error_code = 2;
            s->last_error_msg = strdup("cannot open out file");
            runtimeError(vm, "httpRequestToFile: cannot open out file '%s'", out_path);
            return makeInt(-1);
        }
        char buf[8192]; size_t total = 0; size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            fwrite(buf, 1, n, out); total += n;
        }
        fclose(in); fclose(out);
        // Synthesize headers
        const char* content_type = "application/octet-stream";
        size_t path_len = strlen(path);
        if (path_len >= 4) {
            const char* ext = path + path_len - 4;
            if (strcasecmp(ext, ".txt") == 0) content_type = "text/plain";
            else if (strcasecmp(ext, ".htm") == 0 || strcasecmp(ext, ".html") == 0) content_type = "text/html";
            else if (strcasecmp(ext, ".json") == 0) content_type = "application/json";
        }
        char hdr[256];
        int hdrlen = snprintf(hdr, sizeof(hdr),
                              "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                              total, content_type);
        if (hdrlen > 0) {
            s->last_headers = (char*)malloc((size_t)hdrlen + 1);
            if (s->last_headers) memcpy(s->last_headers, hdr, (size_t)hdrlen + 1);
        }
        s->last_status = 200;
        return makeInt(200);
    } else if (url && strncasecmp(url, "data:", 5) == 0) {
        DataUrlPayload payload = {0};
        char* err_msg = NULL;
        if (parseDataUrl(url, &payload, &err_msg) != 0) {
            s->last_error_code = 2;
            if (s->last_error_msg) free(s->last_error_msg);
            s->last_error_msg = err_msg ? err_msg : strdup("invalid data URL");
            if (!s->last_error_msg) s->last_error_msg = strdup("invalid data URL");
            runtimeError(vm, "httpRequestToFile: %s", s->last_error_msg ? s->last_error_msg : "invalid data URL");
            dataUrlPayloadFree(&payload);
            return makeInt(-1);
        }

        FILE* out = fopen(out_path, "wb");
        if (!out) {
            s->last_error_code = 2;
            if (s->last_error_msg) free(s->last_error_msg);
            s->last_error_msg = strdup("cannot open out file");
            runtimeError(vm, "httpRequestToFile: cannot open out file '%s'", out_path);
            dataUrlPayloadFree(&payload);
            if (err_msg) free(err_msg);
            return makeInt(-1);
        }
        if (payload.length > 0) fwrite(payload.data, 1, payload.length, out);
        fclose(out);

        const char* content_type = payload.content_type ? payload.content_type : "text/plain;charset=US-ASCII";
        int hdrlen = snprintf(NULL, 0,
                              "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                              payload.length, content_type);
        if (hdrlen >= 0) {
            s->last_headers = (char*)malloc((size_t)hdrlen + 1);
            if (s->last_headers) {
                snprintf(s->last_headers, (size_t)hdrlen + 1,
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                         payload.length, content_type);
            }
        }
        s->last_status = 200;
        dataUrlPayloadFree(&payload);
        if (err_msg) free(err_msg);
        return makeInt(200);
    }

    // Network path
    FILE* out = fopen(out_path, "wb");
    if (!out) {
        s->last_error_code = 2;
        s->last_error_msg = strdup("cannot open out file");
        runtimeError(vm, "httpRequestToFile: cannot open out file '%s'", out_path);
        return makeInt(-1);
    }
    curl_easy_reset(s->curl);
    curl_easy_setopt(s->curl, CURLOPT_URL, url);
    curl_easy_setopt(s->curl, CURLOPT_WRITEFUNCTION, fileWriteCallback);
    curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(s->curl, CURLOPT_TIMEOUT_MS, s->timeout_ms);
    curl_easy_setopt(s->curl, CURLOPT_FOLLOWLOCATION, s->follow_redirects);
    curl_easy_setopt(s->curl, CURLOPT_HEADERFUNCTION, headerAccumCallback);
    curl_easy_setopt(s->curl, CURLOPT_HEADERDATA, s);
    if (s->user_agent) curl_easy_setopt(s->curl, CURLOPT_USERAGENT, s->user_agent);
    if (s->headers) curl_easy_setopt(s->curl, CURLOPT_HTTPHEADER, s->headers);
    if (s->resolve) curl_easy_setopt(s->curl, CURLOPT_RESOLVE, s->resolve);
    if (!s->accept_encoding_disabled) {
        curl_easy_setopt(s->curl, CURLOPT_ACCEPT_ENCODING,
                         s->accept_encoding ? s->accept_encoding : "");
    }
    if (s->cookie_file) curl_easy_setopt(s->curl, CURLOPT_COOKIEFILE, s->cookie_file);
    if (s->cookie_jar) curl_easy_setopt(s->curl, CURLOPT_COOKIEJAR, s->cookie_jar);
    if (s->max_recv_speed > 0) curl_easy_setopt(s->curl, CURLOPT_MAX_RECV_SPEED_LARGE, s->max_recv_speed);
    if (s->max_send_speed > 0) curl_easy_setopt(s->curl, CURLOPT_MAX_SEND_SPEED_LARGE, s->max_send_speed);
    if (s->basic_auth && s->basic_auth[0]) {
        curl_easy_setopt(s->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(s->curl, CURLOPT_USERPWD, s->basic_auth);
    }
    FILE* upload_fp = NULL;
    if (s->upload_file && s->upload_file[0]) {
        upload_fp = fopen(s->upload_file, "rb");
        if (!upload_fp) {
            s->last_error_code = 2;
            s->last_error_msg = strdup("cannot open upload file");
            fclose(out);
            runtimeError(vm, "httpRequestToFile: cannot open upload_file '%s'", s->upload_file);
            return makeInt(-1);
        }
        curl_easy_setopt(s->curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(s->curl, CURLOPT_READDATA, upload_fp);
        fseeko(upload_fp, 0, SEEK_END); curl_off_t up_size = ftello(upload_fp); fseeko(upload_fp, 0, SEEK_SET);
        curl_easy_setopt(s->curl, CURLOPT_INFILESIZE_LARGE, up_size);
        if (strcasecmp(method, "POST") == 0) {
            curl_easy_setopt(s->curl, CURLOPT_POST, 1L);
        } else if (strcasecmp(method, "PUT") == 0) {
            curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, "PUT");
        } else {
            curl_easy_setopt(s->curl, CURLOPT_CUSTOMREQUEST, method);
        }
    } else {
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
    }
    // TLS/Proxy options
    if (s->ca_path && s->ca_path[0]) curl_easy_setopt(s->curl, CURLOPT_CAINFO, s->ca_path);
    if (s->client_cert && s->client_cert[0]) curl_easy_setopt(s->curl, CURLOPT_SSLCERT, s->client_cert);
    if (s->client_key && s->client_key[0]) curl_easy_setopt(s->curl, CURLOPT_SSLKEY, s->client_key);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYPEER, s->verify_peer);
    curl_easy_setopt(s->curl, CURLOPT_SSL_VERIFYHOST, s->verify_host ? 2L : 0L);
    if (s->proxy && s->proxy[0]) {
        curl_easy_setopt(s->curl, CURLOPT_PROXY, s->proxy);
        if (s->proxy_userpwd && s->proxy_userpwd[0]) curl_easy_setopt(s->curl, CURLOPT_PROXYUSERPWD, s->proxy_userpwd);
        if (s->proxy_type) curl_easy_setopt(s->curl, CURLOPT_PROXYTYPE, s->proxy_type);
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
    // Extra TLS knobs
#ifdef CURLOPT_SSL_ENABLE_ALPN
    curl_easy_setopt(s->curl, CURLOPT_SSL_ENABLE_ALPN, s->alpn);
#endif
    if (s->tls_min) {
        long v = 0;
        switch (s->tls_min) {
            case 10: v = CURL_SSLVERSION_TLSv1; break;
#ifdef CURL_SSLVERSION_TLSv1_1
            case 11: v = CURL_SSLVERSION_TLSv1_1; break;
#endif
#ifdef CURL_SSLVERSION_TLSv1_2
            case 12: v = CURL_SSLVERSION_TLSv1_2; break;
#endif
#ifdef CURL_SSLVERSION_TLSv1_3
            case 13: v = CURL_SSLVERSION_TLSv1_3; break;
#endif
        }
        if (v) curl_easy_setopt(s->curl, CURLOPT_SSLVERSION, v);
    }
#ifdef CURL_SSLVERSION_MAX_DEFAULT
    if (s->tls_max) {
        long vmax = 0;
#ifdef CURL_SSLVERSION_MAX_TLSv1_0
        if (s->tls_max == 10) vmax = CURL_SSLVERSION_MAX_TLSv1_0;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_1
        if (s->tls_max == 11) vmax = CURL_SSLVERSION_MAX_TLSv1_1;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_2
        if (s->tls_max == 12) vmax = CURL_SSLVERSION_MAX_TLSv1_2;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_3
        if (s->tls_max == 13) vmax = CURL_SSLVERSION_MAX_TLSv1_3;
#endif
        if (vmax) curl_easy_setopt(s->curl, CURLOPT_SSLVERSION, vmax);
    }
#endif
    if (s->ciphers && s->ciphers[0]) curl_easy_setopt(s->curl, CURLOPT_SSL_CIPHER_LIST, s->ciphers);
#ifdef CURLOPT_PINNEDPUBLICKEY
    if (s->pinned_pubkey && s->pinned_pubkey[0]) curl_easy_setopt(s->curl, CURLOPT_PINNEDPUBLICKEY, s->pinned_pubkey);
#endif
    long http_code = 0;
    CURLcode res = CURLE_OK;
    long delay = s->retry_delay_ms;
    int attempt = 0;
    while (1) {
        res = curl_easy_perform(s->curl);
        if (res == CURLE_OK) {
            curl_easy_getinfo(s->curl, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code < 500) break;
        }
        if (attempt >= s->max_retries) break;
        attempt++;
        if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
        if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
        if (upload_fp) fseeko(upload_fp, 0, SEEK_SET);
        fclose(out);
        out = fopen(out_path, "wb");
        if (!out) {
            s->last_error_code = 2;
            if (s->last_error_msg) free(s->last_error_msg);
            s->last_error_msg = strdup("cannot open out file");
            if (upload_fp) fclose(upload_fp);
            runtimeError(vm, "httpRequestToFile: cannot open out file '%s'", out_path);
            return makeInt(-1);
        }
        curl_easy_setopt(s->curl, CURLOPT_WRITEDATA, out);
        if (delay > 0) { sleep_ms(delay); delay *= 2; }
    }
    if (upload_fp) fclose(upload_fp);
    if (res == CURLE_OK && http_code < 500) {
        s->last_status = http_code;
        if (out) fclose(out);
        return makeInt((int)http_code);
    }
    if (res != CURLE_OK) {
        int code = 1;
        switch (res) {
            case CURLE_OPERATION_TIMEDOUT: code = 3; break;
            case CURLE_SSL_CONNECT_ERROR:
            case CURLE_PEER_FAILED_VERIFICATION:
#if defined(CURLE_SSL_CACERT) && (CURLE_SSL_CACERT != CURLE_PEER_FAILED_VERIFICATION)
            case CURLE_SSL_CACERT:
#endif
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
        s->last_status = -code;
        if (out) fclose(out);
        return makeInt(-code);
    }
    s->last_status = http_code;
    s->last_error_code = 1;
    if (s->last_error_msg) free(s->last_error_msg);
    char errbuf[64];
    snprintf(errbuf, sizeof(errbuf), "HTTP status %ld", http_code);
    s->last_error_msg = strdup(errbuf);
    if (out) fclose(out);
    return makeInt((int)http_code);
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
    size_t request_body_length = 0;
    if (args[1].type == TYPE_STRING) {
        request_body_content = args[1].s_val ? args[1].s_val : "";
        request_body_length = strlen(request_body_content);
    } else if (args[1].type == TYPE_MEMORYSTREAM && args[1].mstream != NULL) {
        request_body_content = (const char*)args[1].mstream->buffer;
        request_body_length = args[1].mstream->size;
    } else {
        runtimeError(vm, "apiSend: Request body must be a string or memory stream.");
        return makeVoid();
    }

    // Initialize response stream
    MStream *response_stream = createMStream();
    if (!response_stream) {
        runtimeError(vm, "apiSend: Memory allocation error for response stream structure.");
        return makeVoid();
    }
    response_stream->buffer = malloc(16); // Initial small buffer
    if (!response_stream->buffer) {
        releaseMStream(response_stream);
        runtimeError(vm, "apiSend: Memory allocation error for response stream buffer.");
        return makeVoid();
    }
    response_stream->buffer[0] = '\0';
    response_stream->size = 0;
    response_stream->capacity = 16;

    CURL *curl = curl_easy_init();
    if (!curl) {
        runtimeError(vm, "apiSend: curl initialization failed.");
        releaseMStream(response_stream);
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
    if (request_body_content && request_body_length > 0) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body_content);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)request_body_length);
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
        releaseMStream(response_stream);
        return makeVoid();
    }
    if (http_code >= 400) {
        runtimeError(vm, "apiSend: HTTP request failed with code %ld. Response (partial):\n%s", http_code, response_stream->buffer ? (char*)response_stream->buffer : "(empty)");
        releaseMStream(response_stream);
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

// -------------------- Socket API --------------------

Value vmBuiltinSocketLastError(VM* vm, int arg_count, Value* args) {
    (void)args; (void)arg_count; (void)vm;
    return makeInt(g_socket_last_error);
}

Value vmBuiltinSocketCreate(VM* vm, int arg_count, Value* args) {
    if (arg_count < 1 || arg_count > 2 || !IS_INTLIKE(args[0]) ||
        (arg_count == 2 && !IS_INTLIKE(args[1]))) {
        runtimeError(vm, "socketCreate expects (type[, family]).");
        return makeInt(-1);
    }
    int type = (int)AS_INTEGER(args[0]);
    int family = AF_INET;
    if (arg_count == 2) {
        int fam_arg = (int)AS_INTEGER(args[1]);
        if (fam_arg == AF_INET || fam_arg == 0 || fam_arg == 4) {
            family = AF_INET;
        }
#ifdef AF_INET6
        else if (fam_arg == AF_INET6 || fam_arg == 6) {
            family = AF_INET6;
        }
#endif
        else {
            runtimeError(vm, "socketCreate family must be 4 (IPv4) or 6 (IPv6).");
            return makeInt(-1);
        }
    }
#ifdef _WIN32
    ensure_winsock();
#endif
    int socktype = (type == 1) ? SOCK_DGRAM : SOCK_STREAM;
    int proto = (type == 1) ? IPPROTO_UDP : IPPROTO_TCP;
    int s = (int)socket(family, socktype, proto);
    if (s < 0) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeInt(-1);
    }
#ifdef AF_INET6
    if (family == AF_INET6) {
#ifdef IPV6_V6ONLY
#ifdef _WIN32
        DWORD off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&off, sizeof(off));
#else
        int off = 0;
        setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
#endif
#endif
    }
#endif
    registerSocketInfo(s, family, socktype);
    g_socket_last_error = 0;
    return makeInt(s);
}

Value vmBuiltinSocketClose(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "socketClose expects 1 integer argument.");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
#ifdef _WIN32
    int r = closesocket(s);
    if (r != 0) setSocketError(WSAGetLastError());
#else
    int r = close(s);
    if (r != 0) setSocketError(errno);
#endif
    if (r != 0) return makeInt(-1);
    unregisterSocketInfo(s);
    g_socket_last_error = 0;
    return makeInt(0);
}

Value vmBuiltinSocketConnect(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || !valueIsStringLike(&args[1]) || !IS_INTLIKE(args[2])) {
        runtimeError(vm, "socketConnect expects (socket, host, port).");
        return makeInt(-1);
    }
    if (valueIsNullCharPointer(&args[1])) {
        runtimeError(vm, "socketConnect host pointer is NULL.");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    const char* host = valueToCStringLike(&args[1]);
    if (!host) host = "";
    int port = (int)AS_INTEGER(args[2]);
    char portstr[16]; snprintf(portstr, sizeof(portstr), "%d", port);
    int family = AF_INET;
    int socktype = SOCK_STREAM;
    lookupSocketInfo(s, &family, &socktype);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    if (family == AF_INET) {
        hints.ai_family = AF_INET;
    }
#ifdef AF_INET6
    else if (family == AF_INET6) {
#ifdef AI_V4MAPPED
        hints.ai_family = AF_INET6;
        hints.ai_flags |= AI_V4MAPPED;
#ifdef AI_ALL
        hints.ai_flags |= AI_ALL;
#endif
#else
        hints.ai_family = AF_UNSPEC;
#endif
    }
#endif
    hints.ai_socktype = socktype;
#ifdef AI_ADDRCONFIG
    if (hints.ai_family == AF_UNSPEC) {
        hints.ai_flags |= AI_ADDRCONFIG;
    }
#endif
    int gai_err = pscalHostsGetAddrInfo(host, portstr, &hints, &res);
    if (gai_err != 0) {
        if (res) pscalHostsFreeAddrInfo(res);
        setSocketAddrInfoError(gai_err);
        return makeInt(-1);
    }
    if (!res) {
#ifdef EAI_FAIL
        setSocketAddrInfoError(EAI_FAIL);
#else
        setSocketAddrInfoError(-1);
#endif
        return makeInt(-1);
    }

    int connected = 0;
    int attempted = 0;
    int last_err = 0;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        if (!rp->ai_addr) continue;
        if (family == AF_INET) {
            if (rp->ai_family != AF_INET) continue;
            attempted = 1;
#ifdef _WIN32
            if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
#else
            if (connect(s, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
#endif
                connected = 1;
                break;
            }
        }
#ifdef AF_INET6
        else if (family == AF_INET6) {
            if (rp->ai_family == AF_INET6) {
                attempted = 1;
#ifdef _WIN32
                if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
#else
                if (connect(s, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
#endif
                    connected = 1;
                    break;
                }
            } else if (rp->ai_family == AF_INET) {
                struct sockaddr_in6 mapped;
                mapIpv4ToIpv6((struct sockaddr_in*)rp->ai_addr, &mapped);
                attempted = 1;
#ifdef _WIN32
                if (connect(s, (struct sockaddr*)&mapped, sizeof(mapped)) == 0) {
#else
                if (connect(s, (struct sockaddr*)&mapped, (socklen_t)sizeof(mapped)) == 0) {
#endif
                    connected = 1;
                    break;
                }
            } else {
                continue;
            }
        }
#endif
        else {
            attempted = 1;
#ifdef _WIN32
            if (connect(s, rp->ai_addr, (int)rp->ai_addrlen) == 0) {
#else
            if (connect(s, rp->ai_addr, (socklen_t)rp->ai_addrlen) == 0) {
#endif
                connected = 1;
                break;
            }
        }
#ifdef _WIN32
        last_err = WSAGetLastError();
#else
        last_err = errno;
#endif
    }
    pscalHostsFreeAddrInfo(res);
    if (!connected) {
        if (!attempted) {
#if defined(EAI_NONAME)
            setSocketAddrInfoError(EAI_NONAME);
#elif defined(EAI_NODATA)
            setSocketAddrInfoError(EAI_NODATA);
#elif defined(_WIN32)
            setSocketAddrInfoError(WSAHOST_NOT_FOUND);
#else
            setSocketError(errno != 0 ? errno : ENOENT);
#endif
        } else {
            if (last_err == 0) {
#ifdef _WIN32
                last_err = WSAECONNREFUSED;
#else
                last_err = ECONNREFUSED;
#endif
            }
            setSocketError(last_err);
        }
        return makeInt(-1);
    }
    g_socket_last_error = 0;
    return makeInt(0);
}

Value vmBuiltinSocketBind(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "socketBind expects (socket,int port).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    int port = (int)AS_INTEGER(args[1]);
    int family = AF_INET;
    lookupSocketInfo(s, &family, NULL);
    int r = -1;
    int optval = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));
#ifdef AF_INET6
    if (family == AF_INET6) {
        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((unsigned short)port);
        addr6.sin6_addr = in6addr_any;
        r = bind(s, (struct sockaddr*)&addr6, sizeof(addr6));
    } else
#endif
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons((unsigned short)port);
        r = bind(s, (struct sockaddr*)&addr, sizeof(addr));
    }
    if (r != 0) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeInt(-1);
    }
    g_socket_last_error = 0;
    return makeInt(0);
}

// socketBindAddr(socket, host:string, port:int)
Value vmBuiltinSocketBindAddr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || !valueIsStringLike(&args[1]) || !IS_INTLIKE(args[2])) {
        runtimeError(vm, "socketBindAddr expects (socket:int, host:string, port:int).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    const char* host = valueToCStringLike(&args[1]);
    if (!host) host = "127.0.0.1";
    int port = (int)AS_INTEGER(args[2]);
    int family = AF_INET;
    lookupSocketInfo(s, &family, NULL);
    int r = -1;
    int optval = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char*)&optval, sizeof(optval));
#ifdef AF_INET6
    if (family == AF_INET6) {
        struct sockaddr_in6 addr6;
        memset(&addr6, 0, sizeof(addr6));
        addr6.sin6_family = AF_INET6;
        addr6.sin6_port = htons((unsigned short)port);
        if (!host || !*host) {
            addr6.sin6_addr = in6addr_any;
        } else if (inet_pton(AF_INET6, host, &addr6.sin6_addr) != 1) {
            struct in_addr addr4;
            if (inet_pton(AF_INET, host, &addr4) == 1) {
                struct sockaddr_in tmp4;
                memset(&tmp4, 0, sizeof(tmp4));
                tmp4.sin_family = AF_INET;
                tmp4.sin_port = htons((unsigned short)port);
                tmp4.sin_addr = addr4;
                mapIpv4ToIpv6(&tmp4, &addr6);
            } else {
#ifdef _WIN32
                setSocketError(WSAEINVAL);
#else
                setSocketError(EINVAL);
#endif
                return makeInt(-1);
            }
        }
        r = bind(s, (struct sockaddr*)&addr6, sizeof(addr6));
    } else
#endif
    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons((unsigned short)port);
        int p = inet_pton(AF_INET, host, &addr.sin_addr);
        if (p != 1) {
#ifdef _WIN32
            setSocketError(WSAEINVAL);
#else
            setSocketError(EINVAL);
#endif
            return makeInt(-1);
        }
        r = bind(s, (struct sockaddr*)&addr, sizeof(addr));
    }
    if (r != 0) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeInt(-1);
    }
    g_socket_last_error = 0;
    return makeInt(0);
}

Value vmBuiltinSocketListen(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "socketListen expects (socket,int backlog).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    int backlog = (int)AS_INTEGER(args[1]);
    int r = listen(s, backlog);
    if (r != 0) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeInt(-1);
    }
    g_socket_last_error = 0;
    return makeInt(0);
}

Value vmBuiltinSocketAccept(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "socketAccept expects (socket).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    int parent_family = AF_INET;
    int parent_type = SOCK_STREAM;
    lookupSocketInfo(s, &parent_family, &parent_type);
    int r = -1;
    for (;;) {
#ifndef _WIN32
        if (socketWaitReadable(vm, s) != 0) {
            return makeInt(-1);
        }
#else
        if (socketConsumeInterrupt(vm)) {
            return makeInt(-1);
        }
#endif
        r = (int)accept(s, NULL, NULL);
        if (r >= 0) {
            break;
        }
#ifdef _WIN32
        int err = WSAGetLastError();
        if (err == WSAEINTR || err == WSAEWOULDBLOCK) {
            continue;
        }
        setSocketError(err);
#else
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
            continue;
        }
        setSocketError(errno);
#endif
        return makeInt(-1);
    }
    registerSocketInfo(r, parent_family, parent_type);
    g_socket_last_error = 0;
    return makeInt(r);
}

Value vmBuiltinSocketPeerAddr(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "socketPeerAddr expects (socket).");
        return makeNil();
    }
    int s = (int)AS_INTEGER(args[0]);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    int rc = getpeername(s, (struct sockaddr *)&addr, &len);
    if (rc != 0) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeNil();
    }
    char host[INET6_ADDRSTRLEN] = {0};
    void *src = NULL;
    if (addr.ss_family == AF_INET) {
        src = &((struct sockaddr_in *)&addr)->sin_addr;
    } else if (addr.ss_family == AF_INET6) {
        src = &((struct sockaddr_in6 *)&addr)->sin6_addr;
    }
    if (!src || !inet_ntop(addr.ss_family, src, host, sizeof(host))) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeNil();
    }
    g_socket_last_error = 0;
    return makeString(host);
}

Value vmBuiltinSocketSend(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "socketSend expects (socket, data).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    const char* data = NULL;
    size_t len = 0;
    if (valueIsStringLike(&args[1])) {
        if (valueIsNullCharPointer(&args[1])) {
            runtimeError(vm, "socketSend data pointer is NULL.");
            return makeInt(-1);
        }
        data = valueToCStringLike(&args[1]);
        len = data ? strlen(data) : 0;
    } else if (args[1].type == TYPE_MEMORYSTREAM && args[1].mstream) {
        data = (const char*)args[1].mstream->buffer;
        len = args[1].mstream->size;
    } else {
        runtimeError(vm, "socketSend data must be string or mstream.");
        return makeInt(-1);
    }
    int sent = (int)send(s, data, len, 0);
    if (sent < 0) {
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e != WSAEWOULDBLOCK) setSocketError(e); else g_socket_last_error = 0;
#else
        if (errno != EWOULDBLOCK && errno != EAGAIN) setSocketError(errno); else g_socket_last_error = 0;
#endif
        return makeInt(-1);
    }
    g_socket_last_error = 0;
    return makeInt(sent);
}

Value vmBuiltinSocketReceive(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1])) {
        runtimeError(vm, "socketReceive expects (socket, maxlen).");
        return makeMStream(NULL);
    }
    int s = (int)AS_INTEGER(args[0]);
    int maxlen = (int)AS_INTEGER(args[1]);
    if (maxlen <= 0) maxlen = 4096;
    MStream* ms = createMStream();
    if (!ms) return makeMStream(NULL);
    ms->buffer = malloc(maxlen+1);
    if (!ms->buffer) {
        releaseMStream(ms);
        return makeMStream(NULL);
    }
    int n = -1;
    for (;;) {
#ifndef _WIN32
        if (socketWaitReadable(vm, s) != 0) {
            releaseMStream(ms);
            return makeMStream(NULL);
        }
#else
        if (socketConsumeInterrupt(vm)) {
            releaseMStream(ms);
            return makeMStream(NULL);
        }
#endif
        n = (int)recv(s, ms->buffer, maxlen, 0);
        if (n >= 0) {
            break;
        }
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAEINTR || e == WSAEWOULDBLOCK) {
            continue;
        }
        setSocketError(e);
#else
        if (errno == EINTR || errno == EWOULDBLOCK || errno == EAGAIN) {
            continue;
        }
        setSocketError(errno);
#endif
        releaseMStream(ms);
        return makeMStream(NULL);
    }
    ms->size = n;
    ms->buffer[n] = '\0';
    ms->capacity = maxlen+1;
    g_socket_last_error = 0;
    return makeMStream(ms);
}

Value vmBuiltinSocketSetBlocking(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_BOOLEAN) {
        runtimeError(vm, "socketSetBlocking expects (socket, boolean).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    int blocking = args[1].i_val != 0;
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    int r = ioctlsocket(s, FIONBIO, &mode);
    if (r != 0) setSocketError(WSAGetLastError());
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) { setSocketError(errno); return makeInt(-1); }
    if (blocking) flags &= ~O_NONBLOCK; else flags |= O_NONBLOCK;
    int r = fcntl(s, F_SETFL, flags);
    if (r != 0) setSocketError(errno);
#endif
    if (r != 0) return makeInt(-1);
    g_socket_last_error = 0;
    return makeInt(0);
}

Value vmBuiltinSocketPoll(VM* vm, int arg_count, Value* args) {
    if (arg_count != 3 || !IS_INTLIKE(args[0]) || !IS_INTLIKE(args[1]) || !IS_INTLIKE(args[2])) {
        runtimeError(vm, "socketPoll expects (socket, timeout_ms, flags).");
        return makeInt(-1);
    }
    int s = (int)AS_INTEGER(args[0]);
    int timeout = (int)AS_INTEGER(args[1]);
    int flags = (int)AS_INTEGER(args[2]);
    fd_set rfds, wfds;
    FD_ZERO(&rfds); FD_ZERO(&wfds);
    if (flags & 1) FD_SET(s, &rfds);
    if (flags & 2) FD_SET(s, &wfds);
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;
    int r = select(s+1, &rfds, &wfds, NULL, &tv);
    if (r < 0) {
#ifdef _WIN32
        setSocketError(WSAGetLastError());
#else
        setSocketError(errno);
#endif
        return makeInt(-1);
    }
    if (r == 0) return makeInt(0);
    int out = 0;
    if (FD_ISSET(s, &rfds)) out |= 1;
    if (FD_ISSET(s, &wfds)) out |= 2;
    g_socket_last_error = 0;
    return makeInt(out);
}

static void markDnsLookupFailure(VM* vm) {
    if (!vm) {
        return;
    }
    if (vm->owningThread) {
        vm->abort_requested = true;
    }
}

Value vmBuiltinDnsLookup(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || args[0].type != TYPE_STRING) {
        runtimeError(vm, "dnsLookup expects (hostname).");
        return makeString("");
    }
    const char* host = args[0].s_val;
#ifdef _WIN32
    ensure_winsock();
#endif
    if (isLocalhostName(host)) {
        return makeLocalhostFallbackResult();
    }
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;

    const int max_attempts = 3;
    int attempt = 0;
    int e = 0;
    do {
        if (res) {
            pscalHostsFreeAddrInfo(res);
            res = NULL;
        }
        e = pscalHostsGetAddrInfo(host, NULL, &hints, &res);
        if (e == 0) {
            break;
        }

        if (isLocalhostName(host)) {
            if (res) pscalHostsFreeAddrInfo(res);
            return makeLocalhostFallbackResult();
        }

        bool transient = false;
#ifdef EAI_AGAIN
        if (e == EAI_AGAIN) {
            transient = true;
        }
#endif
#ifdef EAI_FAIL
        if (e == EAI_FAIL) {
            transient = true;
        }
#endif
#ifdef EAI_SYSTEM
        if (e == EAI_SYSTEM && (errno == EINTR || errno == EAGAIN)) {
            transient = true;
        }
#endif
        if (!transient || attempt + 1 >= max_attempts) {
            break;
        }
        attempt++;
        sleep_ms(25 * attempt);
    } while (attempt < max_attempts);

    if (e != 0) {
        if (res) pscalHostsFreeAddrInfo(res);
        setSocketAddrInfoError(e);
        markDnsLookupFailure(vm);
        return makeString("");
    }
    if (!res) {
        if (isLocalhostName(host)) {
            return makeLocalhostFallbackResult();
        }
#ifdef EAI_FAIL
        setSocketAddrInfoError(EAI_FAIL);
#else
        setSocketAddrInfoError(-1);
#endif
        markDnsLookupFailure(vm);
        return makeString("");
    }
    struct addrinfo* first_v4 = NULL;
    struct addrinfo* first_v6 = NULL;
    for (struct addrinfo* rp = res; rp; rp = rp->ai_next) {
        if (!rp->ai_addr) continue;
        if (!first_v4 && rp->ai_family == AF_INET) first_v4 = rp;
#ifdef AF_INET6
        if (!first_v6 && rp->ai_family == AF_INET6) first_v6 = rp;
#endif
    }
    char buf[INET6_ADDRSTRLEN];
    const char* ip = NULL;
    if (first_v4) {
        struct sockaddr_in* addr4 = (struct sockaddr_in*)first_v4->ai_addr;
        ip = inet_ntop(AF_INET, &addr4->sin_addr, buf, sizeof(buf));
    }
#ifdef AF_INET6
    else if (first_v6) {
        struct sockaddr_in6* addr6 = (struct sockaddr_in6*)first_v6->ai_addr;
        ip = inet_ntop(AF_INET6, &addr6->sin6_addr, buf, sizeof(buf));
    }
#endif
    pscalHostsFreeAddrInfo(res);
    if (!ip) {
        if (isLocalhostName(host)) {
            return makeLocalhostFallbackResult();
        }
#ifdef EAI_NONAME
        setSocketAddrInfoError(EAI_NONAME);
#elif defined(_WIN32)
        setSocketError(WSAHOST_NOT_FOUND);
#else
        setSocketError(errno != 0 ? errno : ENOENT);
#endif
        markDnsLookupFailure(vm);
        return makeString("");
    }
    g_socket_last_error = 0;
    return makeString(buf);
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
    // Mirror of session options at submission time
    char* out_file;
    char* user_agent;
    char* basic_auth;
    char* accept_encoding;
    long  accept_encoding_disabled;
    char* cookie_file;
    char* cookie_jar;
    long max_retries;
    long retry_delay_ms;
    curl_off_t max_recv_speed;
    curl_off_t max_send_speed;
    char* upload_file;
    char* ca_path; char* client_cert; char* client_key; char* proxy;
    char* proxy_userpwd; long proxy_type;
    long alpn; long tls_min; long tls_max; char* ciphers; char* pinned_pubkey;
    long timeout_ms; long follow_redirects; long verify_peer; long verify_host; long force_http2;
    struct curl_slist* headers_slist; // copied list
    struct curl_slist* resolve_slist; // copied list
    // Per-job accumulators
    char* last_headers;
    int   last_error_code;
    char* last_error_msg;
    // Cancellation + progress
    volatile int cancel_requested;
    long long dl_now;
    long long dl_total;
    int done;
} HttpAsyncJob;

#define MAX_HTTP_ASYNC 32
static HttpAsyncJob g_http_async[MAX_HTTP_ASYNC];
static pthread_mutex_t g_http_async_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Now that HttpAsyncJob is defined, provide helpers that reference it */
static size_t headerAccumJob(char *buffer, size_t size, size_t nitems, void *userdata) {
    HttpAsyncJob* j = (HttpAsyncJob*)userdata;
    size_t real_size = size * nitems;
    size_t old_len = j->last_headers ? strlen(j->last_headers) : 0;
    char* nb = (char*)realloc(j->last_headers, old_len + real_size + 1);
    if (!nb) return real_size; // drop on OOM
    j->last_headers = nb;
    memcpy(j->last_headers + old_len, buffer, real_size);
    j->last_headers[old_len + real_size] = '\0';
    return real_size;
}

#if LIBCURL_VERSION_NUM >= 0x072000
static int xferInfoCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    (void)ultotal; (void)ulnow;
    HttpAsyncJob* j = (HttpAsyncJob*)clientp;
    if (!j) return 0;
    j->dl_total = (long long)dltotal;
    j->dl_now = (long long)dlnow;
    if (j->cancel_requested) return 1; // abort transfer
    return 0;
}
#else
static int progressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    (void)ultotal; (void)ulnow;
    HttpAsyncJob* j = (HttpAsyncJob*)clientp;
    if (!j) return 0;
    j->dl_total = (long long)dltotal;
    j->dl_now = (long long)dlnow;
    if (j->cancel_requested) return 1; // abort transfer
    return 0;
}
#endif

static int httpAllocAsync(void) {
    pthread_mutex_lock(&g_http_async_mutex);
    for (int i = 0; i < MAX_HTTP_ASYNC; i++) {
        if (!g_http_async[i].active) {
            memset(&g_http_async[i], 0, sizeof(HttpAsyncJob));
            g_http_async[i].active = 1;
            /* Keep mutex locked for caller to initialize job safely */
            return i;
        }
    }
    pthread_mutex_unlock(&g_http_async_mutex);
    return -1;
}

static void* httpAsyncThread(void* arg) {
    int id = (int)(intptr_t)arg;
    HttpAsyncJob* job = &g_http_async[id];
    HttpSession* s = httpGet(job->session);
    if (!s) {
        job->status = -1;
        job->error = strdup("invalid session");
        job->done = 1;
        return NULL;
    }
    // Set up output mstream
    job->result = createMStream();
    if (!job->result) {
        job->status = -1;
        job->error = strdup("malloc failed");
        job->done = 1;
        return NULL;
    }
    job->result->buffer = (unsigned char*)malloc(16);
    if (!job->result->buffer) {
        releaseMStream(job->result);
        job->result = NULL;
        job->status = -1; job->error = strdup("malloc failed"); job->done = 1; return NULL;
    }
    job->result->buffer[0] = '\0'; job->result->size = 0; job->result->capacity = 16;
    // file:// fast-path
    if (job->url && strncasecmp(job->url, "file://", 7) == 0) {
        const char* path = job->url + 7;
        FILE* in = fopen(path, "rb");
        if (!in) {
            job->status = -1;
            job->error = strdup("cannot open local file");
            job->done = 1;
            return NULL;
        }
        size_t total = 0; unsigned char buf[8192]; size_t n;
        while (1) {
            if (job->cancel_requested) {
                fclose(in);
                if (job->out_file && job->out_file[0]) remove(job->out_file);
                job->status = -1;
                if (job->last_error_msg) { free(job->last_error_msg); job->last_error_msg = NULL; }
                job->last_error_msg = strdup("canceled");
                job->done = 1;
                return NULL;
            }
            n = fread(buf, 1, sizeof(buf), in);
            if (n == 0) break;
            writeCallback(buf, 1, n, job->result);
            total += n;
            job->dl_now = (long long)total;
            if (job->cancel_requested) {
                fclose(in);
                if (job->out_file && job->out_file[0]) remove(job->out_file);
                job->status = -1;
                if (job->last_error_msg) { free(job->last_error_msg); job->last_error_msg = NULL; }
                job->last_error_msg = strdup("canceled");
                job->done = 1;
                return NULL;
            }
            if (job->max_recv_speed > 0) {
                unsigned long long delay_ms = 0;
                unsigned long long speed = (unsigned long long)job->max_recv_speed;
                delay_ms = ((unsigned long long)n * 1000ULL) / speed;
                if (delay_ms == 0) delay_ms = 1; // yield so cancel/polling can progress
                while (delay_ms > 0) {
                    unsigned long long slice = delay_ms;
                    if (slice > 50ULL) slice = 50ULL;
                    sleep_ms((long)slice);
                    delay_ms -= slice;
                    if (job->cancel_requested) {
                        fclose(in);
                        if (job->out_file && job->out_file[0]) remove(job->out_file);
                        job->status = -1;
                        if (job->last_error_msg) { free(job->last_error_msg); job->last_error_msg = NULL; }
                        job->last_error_msg = strdup("canceled");
                        job->done = 1;
                        return NULL;
                    }
                }
            }
        }
        fclose(in);
        if (job->cancel_requested) {
            if (job->out_file && job->out_file[0]) remove(job->out_file);
            job->status = -1;
            if (job->last_error_msg) { free(job->last_error_msg); job->last_error_msg = NULL; }
            job->last_error_msg = strdup("canceled");
            job->done = 1;
            return NULL;
        }
        if (job->out_file && job->out_file[0] && job->result && job->result->buffer) {
            FILE* of = fopen(job->out_file, "wb");
            if (of) { fwrite(job->result->buffer, 1, (size_t)job->result->size, of); fclose(of); }
        }
        // headers
        const char* content_type = "application/octet-stream";
        size_t path_len = strlen(path);
        if (path_len >= 4) {
            const char* ext = path + path_len - 4;
            if (strcasecmp(ext, ".txt") == 0) content_type = "text/plain";
            else if (strcasecmp(ext, ".htm") == 0 || strcasecmp(ext, ".html") == 0) content_type = "text/html";
            else if (strcasecmp(ext, ".json") == 0) content_type = "application/json";
        }
        char hdr[256];
        int hdrlen = snprintf(hdr, sizeof(hdr), "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n", total, content_type);
        if (hdrlen > 0) { job->last_headers = strndup(hdr, (size_t)hdrlen); }
        job->status = 200;
        job->done = 1;
        return NULL;
    } else if (job->url && strncasecmp(job->url, "data:", 5) == 0) {
        DataUrlPayload payload = {0};
        char* err_msg = NULL;
        if (parseDataUrl(job->url, &payload, &err_msg) != 0) {
            const char* msg = err_msg ? err_msg : "invalid data URL";
            job->status = -1;
            job->last_error_code = 2;
            job->error = strdup(msg);
            job->last_error_msg = strdup(msg);
            if (err_msg) free(err_msg);
            dataUrlPayloadFree(&payload);
            job->done = 1;
            return NULL;
        }

        size_t required = payload.length + 1;
        if (required == 0) required = 1;
        if (job->result->capacity < (int)required || !job->result->buffer) {
            unsigned char* newbuf = (unsigned char*)realloc(job->result->buffer, required);
            if (!newbuf) {
                const char* msg = "out of memory";
                job->status = -1;
                job->last_error_code = 2;
                job->error = strdup(msg);
                job->last_error_msg = strdup(msg);
                dataUrlPayloadFree(&payload);
                if (err_msg) free(err_msg);
                job->done = 1;
                return NULL;
            }
            job->result->buffer = newbuf;
            job->result->capacity = (int)required;
        }
        if (payload.length > 0) {
            memcpy(job->result->buffer, payload.data, payload.length);
        }
        job->result->size = (int)payload.length;
        job->result->buffer[payload.length] = '\0';

        if (job->out_file && job->out_file[0]) {
            FILE* of = fopen(job->out_file, "wb");
            if (of) {
                if (payload.length > 0) fwrite(payload.data, 1, payload.length, of);
                fclose(of);
            }
        }

        const char* content_type = payload.content_type ? payload.content_type : "text/plain;charset=US-ASCII";
        int hdrlen = snprintf(NULL, 0,
                              "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                              payload.length, content_type);
        if (hdrlen >= 0) {
            job->last_headers = (char*)malloc((size_t)hdrlen + 1);
            if (job->last_headers) {
                snprintf(job->last_headers, (size_t)hdrlen + 1,
                         "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nContent-Type: %s\r\n\r\n",
                         payload.length, content_type);
            }
        }
        job->status = 200;
        job->last_error_code = 0;
        job->dl_now = (long long)payload.length;
        job->dl_total = (long long)payload.length;
        dataUrlPayloadFree(&payload);
        if (err_msg) free(err_msg);
        job->done = 1;
        return NULL;
    }

    // Configure CURL with per-job easy handle
    CURL* eh = curl_easy_init();
    if (!eh) { job->status = -1; job->error = strdup("curl init failed"); job->done = 1; return NULL; }
    curl_easy_setopt(eh, CURLOPT_URL, job->url);
    // choose sink
    FILE* tmp_file = NULL;
    DualSink dual = (DualSink){0};
    if (job->out_file && job->out_file[0]) {
        tmp_file = fopen(job->out_file, "wb");
        if (tmp_file) { dual.f = tmp_file; }
        dual.ms = job->result;
        curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, dualWriteCallback);
        curl_easy_setopt(eh, CURLOPT_WRITEDATA, &dual);
    } else {
        curl_easy_setopt(eh, CURLOPT_WRITEFUNCTION, writeCallback);
        curl_easy_setopt(eh, CURLOPT_WRITEDATA, job->result);
    }
    curl_easy_setopt(eh, CURLOPT_TIMEOUT_MS, job->timeout_ms);
    curl_easy_setopt(eh, CURLOPT_FOLLOWLOCATION, job->follow_redirects);
    curl_easy_setopt(eh, CURLOPT_HEADERFUNCTION, headerAccumJob);
    curl_easy_setopt(eh, CURLOPT_HEADERDATA, job);
    if (job->user_agent && job->user_agent[0]) curl_easy_setopt(eh, CURLOPT_USERAGENT, job->user_agent);
    if (job->headers_slist) curl_easy_setopt(eh, CURLOPT_HTTPHEADER, job->headers_slist);
    if (job->resolve_slist) curl_easy_setopt(eh, CURLOPT_RESOLVE, job->resolve_slist);
    if (!job->accept_encoding_disabled) {
        curl_easy_setopt(eh, CURLOPT_ACCEPT_ENCODING,
                         job->accept_encoding ? job->accept_encoding : "");
    }
    if (job->cookie_file) curl_easy_setopt(eh, CURLOPT_COOKIEFILE, job->cookie_file);
    if (job->cookie_jar) curl_easy_setopt(eh, CURLOPT_COOKIEJAR, job->cookie_jar);
    if (job->max_recv_speed > 0) curl_easy_setopt(eh, CURLOPT_MAX_RECV_SPEED_LARGE, job->max_recv_speed);
    if (job->max_send_speed > 0) curl_easy_setopt(eh, CURLOPT_MAX_SEND_SPEED_LARGE, job->max_send_speed);
    if (job->basic_auth && job->basic_auth[0]) {
        curl_easy_setopt(eh, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(eh, CURLOPT_USERPWD, job->basic_auth);
    }
    if (job->proxy && job->proxy[0]) {
        curl_easy_setopt(eh, CURLOPT_PROXY, job->proxy);
        if (job->proxy_userpwd && job->proxy_userpwd[0]) curl_easy_setopt(eh, CURLOPT_PROXYUSERPWD, job->proxy_userpwd);
        if (job->proxy_type) curl_easy_setopt(eh, CURLOPT_PROXYTYPE, job->proxy_type);
    }
    // Progress + cancel
#if LIBCURL_VERSION_NUM >= 0x072000
    curl_easy_setopt(eh, CURLOPT_XFERINFOFUNCTION, xferInfoCallback);
    curl_easy_setopt(eh, CURLOPT_XFERINFODATA, job);
    curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 0L);
#else
    curl_easy_setopt(eh, CURLOPT_PROGRESSFUNCTION, progressCallback);
    curl_easy_setopt(eh, CURLOPT_PROGRESSDATA, job);
    curl_easy_setopt(eh, CURLOPT_NOPROGRESS, 0L);
#endif
    FILE* upload_fp = NULL;
    if (job->upload_file && job->upload_file[0]) {
        upload_fp = fopen(job->upload_file, "rb");
        if (!upload_fp) {
            job->status = -1; job->error = strdup("cannot open upload file");
            if (tmp_file) fclose(tmp_file);
            if (eh) curl_easy_cleanup(eh);
            job->done = 1; return NULL;
        }
        curl_easy_setopt(eh, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(eh, CURLOPT_READDATA, upload_fp);
        fseeko(upload_fp, 0, SEEK_END); curl_off_t up_size = ftello(upload_fp); fseeko(upload_fp, 0, SEEK_SET);
        curl_easy_setopt(eh, CURLOPT_INFILESIZE_LARGE, up_size);
        if (strcasecmp(job->method, "POST") == 0) {
            curl_easy_setopt(eh, CURLOPT_POST, 1L);
        } else if (strcasecmp(job->method, "PUT") == 0) {
            curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "PUT");
        } else {
            curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, job->method);
        }
    } else {
        if (strcasecmp(job->method, "GET") == 0) {
            curl_easy_setopt(eh, CURLOPT_HTTPGET, 1L);
        } else if (strcasecmp(job->method, "POST") == 0) {
            curl_easy_setopt(eh, CURLOPT_POST, 1L);
            if (job->body && job->body_len > 0) {
                curl_easy_setopt(eh, CURLOPT_POSTFIELDS, job->body);
                curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE, (long)job->body_len);
            }
        } else if (strcasecmp(job->method, "PUT") == 0) {
            curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "PUT");
            if (job->body && job->body_len > 0) {
                curl_easy_setopt(eh, CURLOPT_POSTFIELDS, job->body);
                curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE, (long)job->body_len);
            }
        } else if (strcasecmp(job->method, "DELETE") == 0) {
            curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, "DELETE");
        } else {
            curl_easy_setopt(eh, CURLOPT_CUSTOMREQUEST, job->method);
            if (job->body && job->body_len > 0) {
                curl_easy_setopt(eh, CURLOPT_POSTFIELDS, job->body);
                curl_easy_setopt(eh, CURLOPT_POSTFIELDSIZE, (long)job->body_len);
            }
        }
    }
    if (job->ca_path && job->ca_path[0]) curl_easy_setopt(eh, CURLOPT_CAINFO, job->ca_path);
    if (job->client_cert && job->client_cert[0]) curl_easy_setopt(eh, CURLOPT_SSLCERT, job->client_cert);
    if (job->client_key && job->client_key[0]) curl_easy_setopt(eh, CURLOPT_SSLKEY, job->client_key);
    curl_easy_setopt(eh, CURLOPT_SSL_VERIFYPEER, job->verify_peer);
    curl_easy_setopt(eh, CURLOPT_SSL_VERIFYHOST, job->verify_host ? 2L : 0L);
    if (job->proxy && job->proxy[0]) curl_easy_setopt(eh, CURLOPT_PROXY, job->proxy);
#ifdef CURLOPT_HTTP_VERSION
    if (job->force_http2) {
#ifdef CURL_HTTP_VERSION_2TLS
        curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
#elif defined(CURL_HTTP_VERSION_2_0)
        curl_easy_setopt(eh, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_0);
#endif
    }
#endif
    // Extra TLS knobs
#ifdef CURLOPT_SSL_ENABLE_ALPN
    curl_easy_setopt(eh, CURLOPT_SSL_ENABLE_ALPN, job->alpn);
#endif
    if (job->tls_min) {
        long v = 0;
        switch (job->tls_min) {
            case 10: v = CURL_SSLVERSION_TLSv1; break;
#ifdef CURL_SSLVERSION_TLSv1_1
            case 11: v = CURL_SSLVERSION_TLSv1_1; break;
#endif
#ifdef CURL_SSLVERSION_TLSv1_2
            case 12: v = CURL_SSLVERSION_TLSv1_2; break;
#endif
#ifdef CURL_SSLVERSION_TLSv1_3
            case 13: v = CURL_SSLVERSION_TLSv1_3; break;
#endif
        }
        if (v) curl_easy_setopt(eh, CURLOPT_SSLVERSION, v);
    }
#ifdef CURL_SSLVERSION_MAX_DEFAULT
    if (job->tls_max) {
        long vmax = 0;
#ifdef CURL_SSLVERSION_MAX_TLSv1_0
        if (job->tls_max == 10) vmax = CURL_SSLVERSION_MAX_TLSv1_0;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_1
        if (job->tls_max == 11) vmax = CURL_SSLVERSION_MAX_TLSv1_1;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_2
        if (job->tls_max == 12) vmax = CURL_SSLVERSION_MAX_TLSv1_2;
#endif
#ifdef CURL_SSLVERSION_MAX_TLSv1_3
        if (job->tls_max == 13) vmax = CURL_SSLVERSION_MAX_TLSv1_3;
#endif
        if (vmax) curl_easy_setopt(eh, CURLOPT_SSLVERSION, vmax);
    }
#endif
    if (job->ciphers && job->ciphers[0]) curl_easy_setopt(eh, CURLOPT_SSL_CIPHER_LIST, job->ciphers);
#ifdef CURLOPT_PINNEDPUBLICKEY
    if (job->pinned_pubkey && job->pinned_pubkey[0]) curl_easy_setopt(eh, CURLOPT_PINNEDPUBLICKEY, job->pinned_pubkey);
#endif

    long http_code = 0;
    CURLcode res = CURLE_OK;
    long delay = job->retry_delay_ms;
    int attempt = 0;
    while (1) {
        res = curl_easy_perform(eh);
        if (res == CURLE_OK) {
            curl_easy_getinfo(eh, CURLINFO_RESPONSE_CODE, &http_code);
            if (http_code < 500) break;
        }
        if (attempt >= job->max_retries) break;
        attempt++;
        if (job->last_headers) { free(job->last_headers); job->last_headers = NULL; }
        if (job->last_error_msg) { free(job->last_error_msg); job->last_error_msg = NULL; }
        if (job->result) { job->result->size = 0; if (job->result->buffer) job->result->buffer[0] = '\0'; }
        if (tmp_file) {
            fclose(tmp_file);
            tmp_file = fopen(job->out_file, "wb");
            if (!tmp_file) {
                job->status = -1;
                job->last_error_code = 2;
                if (job->last_error_msg) free(job->last_error_msg);
                job->last_error_msg = strdup("cannot open out_file");
                if (job->error) free(job->error);
                job->error = strdup("cannot open out_file");
                res = CURLE_WRITE_ERROR;
                break;
            }
            dual.f = tmp_file;
        }
        if (upload_fp) fseeko(upload_fp, 0, SEEK_SET);
        if (delay > 0) { sleep_ms(delay); delay *= 2; }
    }
    if (upload_fp) fclose(upload_fp);
    if (res == CURLE_OK && http_code < 500) {
        job->status = http_code;
    } else {
        job->status = -1;
        if (res != CURLE_OK) {
            if (!job->last_error_msg) job->last_error_msg = strdup(curl_easy_strerror(res));
            int code = 1;
            switch (res) {
                case CURLE_OPERATION_TIMEDOUT: code = 3; break;
                case CURLE_SSL_CONNECT_ERROR:
                case CURLE_PEER_FAILED_VERIFICATION:
#if defined(CURLE_SSL_CACERT) && (CURLE_SSL_CACERT != CURLE_PEER_FAILED_VERIFICATION)
                case CURLE_SSL_CACERT:
#endif
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
            if (!job->last_error_code) job->last_error_code = code;
            if (!job->error && job->last_error_msg) job->error = strdup(job->last_error_msg);
        } else {
            if (!job->last_error_code) job->last_error_code = 1;
            if (!job->last_error_msg) job->last_error_msg = strdup("HTTP error");
            if (!job->error) job->error = strdup("HTTP error");
        }
    }
    if (tmp_file) fclose(tmp_file);
    if (eh) curl_easy_cleanup(eh);
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
    HttpAsyncJob* job = &g_http_async[id]; /* g_http_async_mutex is locked */
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
    // Snapshot session options for thread safety
    HttpSession* s = httpGet(job->session);
    if (s) {
        job->timeout_ms = s->timeout_ms; job->follow_redirects = s->follow_redirects;
        job->verify_peer = s->verify_peer; job->verify_host = s->verify_host; job->force_http2 = s->force_http2;
        job->user_agent = s->user_agent ? strdup(s->user_agent) : NULL;
        job->basic_auth = s->basic_auth ? strdup(s->basic_auth) : NULL;
        job->accept_encoding = s->accept_encoding ? strdup(s->accept_encoding) : NULL;
        job->accept_encoding_disabled = s->accept_encoding_disabled;
        job->cookie_file = s->cookie_file ? strdup(s->cookie_file) : NULL;
        job->cookie_jar = s->cookie_jar ? strdup(s->cookie_jar) : NULL;
        job->max_retries = s->max_retries;
        job->retry_delay_ms = s->retry_delay_ms;
        job->max_recv_speed = s->max_recv_speed;
        job->max_send_speed = s->max_send_speed;
        job->upload_file = s->upload_file ? strdup(s->upload_file) : NULL;
        job->ca_path = s->ca_path ? strdup(s->ca_path) : NULL;
        job->client_cert = s->client_cert ? strdup(s->client_cert) : NULL;
        job->client_key = s->client_key ? strdup(s->client_key) : NULL;
        job->proxy = s->proxy ? strdup(s->proxy) : NULL;
        job->proxy_userpwd = s->proxy_userpwd ? strdup(s->proxy_userpwd) : NULL;
        job->proxy_type = s->proxy_type;
        job->alpn = s->alpn; job->tls_min = s->tls_min; job->tls_max = s->tls_max;
        job->ciphers = s->ciphers ? strdup(s->ciphers) : NULL;
        job->pinned_pubkey = s->pinned_pubkey ? strdup(s->pinned_pubkey) : NULL;
        job->out_file = s->out_file ? strdup(s->out_file) : NULL;
        // Duplicate headers slist
        struct curl_slist* p = s->headers;
        struct curl_slist* tail = NULL;
        while (p) {
            struct curl_slist* node = curl_slist_append(NULL, p->data);
            if (node) {
                if (!job->headers_slist) { job->headers_slist = node; tail = node; }
                else { tail->next = node; tail = node; }
            }
            p = p->next;
        }
        // Duplicate resolve slist
        p = s->resolve; tail = NULL;
        while (p) {
            struct curl_slist* node = curl_slist_append(NULL, p->data);
            if (node) { if (!job->resolve_slist) { job->resolve_slist = node; tail = node; } else { tail->next = node; tail = node; } }
            p = p->next;
        }
    }
    if (pthread_create(&job->th, NULL, httpAsyncThread, (void*)(intptr_t)id) != 0) {
        if (job->method) free(job->method);
        if (job->url) free(job->url);
        if (job->body) free(job->body);
        if (job->user_agent) free(job->user_agent);
        if (job->basic_auth) free(job->basic_auth);
        if (job->accept_encoding) free(job->accept_encoding);
        if (job->cookie_file) free(job->cookie_file);
        if (job->cookie_jar) free(job->cookie_jar);
        if (job->upload_file) free(job->upload_file);
        if (job->ca_path) free(job->ca_path);
        if (job->client_cert) free(job->client_cert);
        if (job->client_key) free(job->client_key);
        if (job->proxy) free(job->proxy);
        if (job->proxy_userpwd) free(job->proxy_userpwd);
        if (job->ciphers) free(job->ciphers);
        if (job->pinned_pubkey) free(job->pinned_pubkey);
        if (job->out_file) free(job->out_file);
        if (job->headers_slist) curl_slist_free_all(job->headers_slist);
        if (job->resolve_slist) curl_slist_free_all(job->resolve_slist);
        job->active = 0;
        pthread_mutex_unlock(&g_http_async_mutex);
        runtimeError(vm, "httpRequestAsync: pthread_create failed.");
        return makeInt(-1);
    }
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(id);
}

// HttpRequestAsyncToFile(session, method, url, bodyStrOrMStreamOrNil, outPath): Integer (async id)
Value vmBuiltinHttpRequestAsyncToFile(VM* vm, int arg_count, Value* args) {
    if (arg_count != 5 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_STRING || args[2].type != TYPE_STRING) {
        runtimeError(vm, "httpRequestAsyncToFile expects (session:int, method:string, url:string, body:string|mstream|nil, out:string).");
        return makeInt(-1);
    }
    int id = httpAllocAsync();
    if (id < 0) { runtimeError(vm, "httpRequestAsyncToFile: no free slots."); return makeInt(-1); }
    HttpAsyncJob* job = &g_http_async[id]; /* g_http_async_mutex is locked */
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
    if (args[4].type != TYPE_STRING || !args[4].s_val) {
        runtimeError(vm, "httpRequestAsyncToFile: out must be a filename string.");
        // free partial
        if (job->method) free(job->method);
        if (job->url) free(job->url);
        if (job->body) free(job->body);
        job->active = 0;
        pthread_mutex_unlock(&g_http_async_mutex);
        return makeInt(-1);
    }
    job->out_file = strdup(args[4].s_val);
    // Snapshot session options for thread safety
    HttpSession* s = httpGet(job->session);
    if (s) {
        job->timeout_ms = s->timeout_ms; job->follow_redirects = s->follow_redirects;
        job->verify_peer = s->verify_peer; job->verify_host = s->verify_host; job->force_http2 = s->force_http2;
        job->user_agent = s->user_agent ? strdup(s->user_agent) : NULL;
        job->basic_auth = s->basic_auth ? strdup(s->basic_auth) : NULL;
        job->accept_encoding = s->accept_encoding ? strdup(s->accept_encoding) : NULL;
        job->accept_encoding_disabled = s->accept_encoding_disabled;
        job->cookie_file = s->cookie_file ? strdup(s->cookie_file) : NULL;
        job->cookie_jar = s->cookie_jar ? strdup(s->cookie_jar) : NULL;
        job->max_retries = s->max_retries;
        job->retry_delay_ms = s->retry_delay_ms;
        job->max_recv_speed = s->max_recv_speed;
        job->max_send_speed = s->max_send_speed;
        job->upload_file = s->upload_file ? strdup(s->upload_file) : NULL;
        job->ca_path = s->ca_path ? strdup(s->ca_path) : NULL;
        job->client_cert = s->client_cert ? strdup(s->client_cert) : NULL;
        job->client_key = s->client_key ? strdup(s->client_key) : NULL;
        job->proxy = s->proxy ? strdup(s->proxy) : NULL;
        // Duplicate headers slist
        struct curl_slist* p = s->headers; struct curl_slist* tail = NULL;
        while (p) {
            struct curl_slist* node = curl_slist_append(NULL, p->data);
            if (node) { if (!job->headers_slist) { job->headers_slist = node; tail = node; } else { tail->next = node; tail = node; } }
            p = p->next;
        }
    }
    if (pthread_create(&job->th, NULL, httpAsyncThread, (void*)(intptr_t)id) != 0) {
        if (job->method) free(job->method);
        if (job->url) free(job->url);
        if (job->body) free(job->body);
        if (job->user_agent) free(job->user_agent);
        if (job->basic_auth) free(job->basic_auth);
        if (job->accept_encoding) free(job->accept_encoding);
        if (job->cookie_file) free(job->cookie_file);
        if (job->cookie_jar) free(job->cookie_jar);
        if (job->upload_file) free(job->upload_file);
        if (job->ca_path) free(job->ca_path);
        if (job->client_cert) free(job->client_cert);
        if (job->client_key) free(job->client_key);
        if (job->proxy) free(job->proxy);
        if (job->out_file) free(job->out_file);
        if (job->headers_slist) curl_slist_free_all(job->headers_slist);
        job->active = 0;
        pthread_mutex_unlock(&g_http_async_mutex);
        runtimeError(vm, "httpRequestAsyncToFile: pthread_create failed.");
        return makeInt(-1);
    }
    pthread_mutex_unlock(&g_http_async_mutex);
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
    pthread_mutex_lock(&g_http_async_mutex);
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { pthread_mutex_unlock(&g_http_async_mutex); runtimeError(vm, "httpAwait: job not active."); return makeInt(-1); }
    pthread_mutex_unlock(&g_http_async_mutex);
    pthread_join(job->th, NULL);
    pthread_mutex_lock(&g_http_async_mutex);
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
    // Update session last_* fields from job
    HttpSession* s = httpGet(job->session);
    if (s) {
        s->last_status = job->status;
        if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
        if (job->last_headers) s->last_headers = strdup(job->last_headers);
        s->last_error_code = job->last_error_code;
        if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
        if (job->last_error_msg) s->last_error_msg = strdup(job->last_error_msg);
    }
    // Free job resources
    if (job->result) { if (job->result->buffer) free(job->result->buffer); free(job->result); }
    if (job->method) free(job->method);
    if (job->url) free(job->url);
    if (job->body) free(job->body);
    if (job->error) free(job->error);
    if (job->user_agent) free(job->user_agent);
    if (job->basic_auth) free(job->basic_auth);
    if (job->accept_encoding) free(job->accept_encoding);
    if (job->cookie_file) free(job->cookie_file);
    if (job->cookie_jar) free(job->cookie_jar);
    if (job->upload_file) free(job->upload_file);
    if (job->ca_path) free(job->ca_path);
    if (job->client_cert) free(job->client_cert);
    if (job->client_key) free(job->client_key);
    if (job->proxy) free(job->proxy);
    if (job->proxy_userpwd) free(job->proxy_userpwd);
    if (job->ciphers) free(job->ciphers);
    if (job->pinned_pubkey) free(job->pinned_pubkey);
    if (job->out_file) free(job->out_file);
    if (job->headers_slist) curl_slist_free_all(job->headers_slist);
    if (job->resolve_slist) curl_slist_free_all(job->resolve_slist);
    if (job->last_headers) free(job->last_headers);
    if (job->last_error_msg) free(job->last_error_msg);
    memset(job, 0, sizeof(HttpAsyncJob));
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(status);
}

// HttpTryAwait(asyncId, out:mstream): Integer (-2: pending; otherwise status)
Value vmBuiltinHttpTryAwait(VM* vm, int arg_count, Value* args) {
    if (arg_count != 2 || !IS_INTLIKE(args[0]) || args[1].type != TYPE_MEMORYSTREAM || !args[1].mstream) {
        runtimeError(vm, "httpTryAwait expects (id:int, out:mstream).");
        return makeInt(-1);
    }
    int id = (int)AS_INTEGER(args[0]);
    if (id < 0 || id >= MAX_HTTP_ASYNC) { runtimeError(vm, "httpTryAwait: invalid id."); return makeInt(-1); }
    pthread_mutex_lock(&g_http_async_mutex);
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { pthread_mutex_unlock(&g_http_async_mutex); runtimeError(vm, "httpTryAwait: job not active."); return makeInt(-1); }
    if (!job->done) { pthread_mutex_unlock(&g_http_async_mutex); return makeInt(-2); }
    pthread_mutex_unlock(&g_http_async_mutex);
    // Same as await: join and harvest
    pthread_join(job->th, NULL);
    pthread_mutex_lock(&g_http_async_mutex);
    int status = (int)job->status;
    if (job->result && job->result->buffer) {
        MStream* out = args[1].mstream;
        if ((size_t)out->capacity < job->result->size + 1) {
            unsigned char* nb = realloc(out->buffer, job->result->size + 1);
            if (nb) { out->buffer = nb; out->capacity = (int)(job->result->size + 1); }
        }
        memcpy(out->buffer, job->result->buffer, job->result->size);
        out->size = (int)job->result->size;
        out->buffer[out->size] = '\0';
    }
    HttpSession* s = httpGet(job->session);
    if (s) {
        s->last_status = job->status;
        if (s->last_headers) { free(s->last_headers); s->last_headers = NULL; }
        if (job->last_headers) s->last_headers = strdup(job->last_headers);
        s->last_error_code = job->last_error_code;
        if (s->last_error_msg) { free(s->last_error_msg); s->last_error_msg = NULL; }
        if (job->last_error_msg) s->last_error_msg = strdup(job->last_error_msg);
    }
    if (job->result) { if (job->result->buffer) free(job->result->buffer); free(job->result); }
    if (job->method) free(job->method);
    if (job->url) free(job->url);
    if (job->body) free(job->body);
    if (job->error) free(job->error);
    if (job->user_agent) free(job->user_agent);
    if (job->basic_auth) free(job->basic_auth);
    if (job->accept_encoding) free(job->accept_encoding);
    if (job->cookie_file) free(job->cookie_file);
    if (job->cookie_jar) free(job->cookie_jar);
    if (job->upload_file) free(job->upload_file);
    if (job->ca_path) free(job->ca_path);
    if (job->client_cert) free(job->client_cert);
    if (job->client_key) free(job->client_key);
    if (job->proxy) free(job->proxy);
    if (job->out_file) free(job->out_file);
    if (job->headers_slist) curl_slist_free_all(job->headers_slist);
    if (job->last_headers) free(job->last_headers);
    if (job->last_error_msg) free(job->last_error_msg);
    memset(job, 0, sizeof(HttpAsyncJob));
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(status);
}

// HttpIsDone(asyncId): Integer (0/1)
Value vmBuiltinHttpIsDone(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpIsDone expects (id:int).");
        return makeInt(0);
    }
    int id = (int)AS_INTEGER(args[0]);
    if (id < 0 || id >= MAX_HTTP_ASYNC) return makeInt(0);
    pthread_mutex_lock(&g_http_async_mutex);
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { pthread_mutex_unlock(&g_http_async_mutex); return makeInt(0); }
    int done = job->done ? 1 : 0;
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(done);
}

// HttpCancel(asyncId): Integer (1 on success, 0 otherwise)
Value vmBuiltinHttpCancel(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpCancel expects (id:int).");
        return makeInt(0);
    }
    int id = (int)AS_INTEGER(args[0]);
    if (id < 0 || id >= MAX_HTTP_ASYNC) return makeInt(0);
    pthread_mutex_lock(&g_http_async_mutex);
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { pthread_mutex_unlock(&g_http_async_mutex); return makeInt(0); }
    job->cancel_requested = 1;
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(1);
}

// httpGetAsyncProgress(asyncId): Integer (bytes downloaded so far)
Value vmBuiltinHttpGetAsyncProgress(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpGetAsyncProgress expects (id:int).");
        return makeInt(0);
    }
    int id = (int)AS_INTEGER(args[0]);
    if (id < 0 || id >= MAX_HTTP_ASYNC) return makeInt(0);
    pthread_mutex_lock(&g_http_async_mutex);
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { pthread_mutex_unlock(&g_http_async_mutex); return makeInt(0); }
    int progress = (int)job->dl_now;
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(progress);
}

// httpGetAsyncTotal(asyncId): Integer (total bytes expected; 0 if unknown)
Value vmBuiltinHttpGetAsyncTotal(VM* vm, int arg_count, Value* args) {
    if (arg_count != 1 || !IS_INTLIKE(args[0])) {
        runtimeError(vm, "httpGetAsyncTotal expects (id:int).");
        return makeInt(0);
    }
    int id = (int)AS_INTEGER(args[0]);
    if (id < 0 || id >= MAX_HTTP_ASYNC) return makeInt(0);
    pthread_mutex_lock(&g_http_async_mutex);
    HttpAsyncJob* job = &g_http_async[id];
    if (!job->active) { pthread_mutex_unlock(&g_http_async_mutex); return makeInt(0); }
    int total = (int)job->dl_total;
    pthread_mutex_unlock(&g_http_async_mutex);
    return makeInt(total);
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

    // Identify start of the last header block (data before the final blank line)
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
        block = p;
        p = sep + ((sep == crlfcrlf) ? 4 : 2);
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
