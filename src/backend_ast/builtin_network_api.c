/* src/builtin_network_api.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "backend_ast/builtin.h"
#include "globals.h"
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
