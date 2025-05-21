/* src/builtin_network_api.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "backend_ast/builtin.h"
#include "backend_ast/interpreter.h"
#include "globals.h"
#include "core/utils.h"

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

/* Built–in function: api_send(URL, requestBody)
   - URL is a string.
   - requestBody can be a string or a memory stream.
   Returns: a memory stream containing the API response.
*/
Value executeBuiltinAPISend(AST *node) {
    if (node->child_count != 2) {
        fprintf(stderr, "Runtime error: api_send expects 2 arguments: URL and request body.\n");
        EXIT_FAILURE_HANDLER();
    }
    Value url_val = eval(node->children[0]);
    Value body_val = eval(node->children[1]);

    if (url_val.type != TYPE_STRING || url_val.s_val == NULL) { // Added NULL check for s_val
        fprintf(stderr, "Runtime error: api_send expects URL as a non-null string.\n");
        freeValue(&url_val); // Free evaluated values
        freeValue(&body_val);
        EXIT_FAILURE_HANDLER();
    }

    // request_body is not used for GET, but we evaluate it to free it properly
    // char *request_body_content = NULL;
    // if (body_val.type == TYPE_STRING) {
    //     request_body_content = body_val.s_val;
    // } else if (body_val.type == TYPE_MEMORYSTREAM && body_val.mstream != NULL) {
    //     request_body_content = (char *)body_val.mstream->buffer;
    // } else if (body_val.type != TYPE_STRING && body_val.type != TYPE_MEMORYSTREAM) {
    //     fprintf(stderr, "Runtime error: api_send request body must be a string or memory stream.\n");
    //     freeValue(&url_val);
    //     freeValue(&body_val);
    //     EXIT_FAILURE_HANDLER();
    // }
    // For this specific GET request, the body_val is an empty string '', so no complex logic needed.

    /* Initialize a memory stream to store the response */
    MStream *response_stream = malloc(sizeof(MStream));
    if (!response_stream) {
        fprintf(stderr, "Memory allocation error for response stream structure.\n");
        freeValue(&url_val);
        freeValue(&body_val);
        EXIT_FAILURE_HANDLER();
    }
    response_stream->buffer = malloc(16); // start with an empty buffer, null-terminated
    if (!response_stream->buffer) {
        fprintf(stderr, "Memory allocation error for response stream buffer.\n");
        free(response_stream);
        freeValue(&url_val);
        freeValue(&body_val);
        EXIT_FAILURE_HANDLER();
    }
    response_stream->buffer[0] = '\0'; // Ensure it's an empty string initially
    response_stream->size = 0;
    response_stream->capacity = 16; // Initial capacity

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl initialization failed.\n");
        free(response_stream->buffer);
        free(response_stream);
        freeValue(&url_val);
        freeValue(&body_val);
        EXIT_FAILURE_HANDLER();
    }
    

    // struct curl_slist *headers = NULL; // Headers usually not needed for this simple GET

    curl_easy_setopt(curl, CURLOPT_URL, url_val.s_val);
    // By default, if CURLOPT_POSTFIELDS is not set, libcurl performs a GET request.
    // So, we REMOVE the CURLOPT_POST and CURLOPT_POSTFIELDS options.
    // curl_easy_setopt(curl, CURLOPT_POST, 1L); // REMOVE THIS
    // curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body_content); // REMOVE THIS

    // if (headers) { // Only if headers were added
    //    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_stream);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // Fail on HTTP errors >= 400
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L); // 15 seconds timeout
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // Follow HTTP redirects
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36"); // Common User-Agent
    // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // For debugging only


    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "Runtime error: curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        // Clean up before exiting
        curl_easy_cleanup(curl);
        // curl_slist_free_all(headers); // If headers were used
        if (response_stream->buffer) free(response_stream->buffer);
        free(response_stream);
        freeValue(&url_val);
        freeValue(&body_val);
        // Return an empty MStream or signal error in a way Pscal can check?
        // For now, EXIT_FAILURE_HANDLER is consistent with other errors.
        // If you want to return an error MStream, make one with empty/error state.
        EXIT_FAILURE_HANDLER();
    }
    
    // Check HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
        fprintf(stderr, "Runtime error: HTTP request failed with code %ld. Response:\n%s\n", http_code, response_stream->buffer ? (char*)response_stream->buffer : "(empty)");
        curl_easy_cleanup(curl);
        if (response_stream->buffer) free(response_stream->buffer);
        free(response_stream);
        freeValue(&url_val);
        freeValue(&body_val);
        EXIT_FAILURE_HANDLER();
    }


    curl_easy_cleanup(curl);
    // curl_slist_free_all(headers); // If headers were used

    // Free the evaluated URL and body values (body_val might be an empty string or MStream)
    freeValue(&url_val);
    freeValue(&body_val);
    if (response_stream && response_stream->buffer) {
        char temp_debug_buffer[65];
        size_t print_len = response_stream->size < 64 ? response_stream->size : 64;
        strncpy(temp_debug_buffer, (char*)response_stream->buffer, print_len);
        temp_debug_buffer[print_len] = '\0';
    }

    /* Return the response as a memory stream */
    return makeMStream(response_stream);
}

/* Built–in function: api_receive(MStream)
   For now, this simply converts the memory stream into a string.
*/
Value executeBuiltinAPIReceive(AST *node) {
    if (node->child_count != 1) { // Ensure exactly one argument
        fprintf(stderr, "Runtime error: api_receive expects 1 argument (a memory stream).\n");
        EXIT_FAILURE_HANDLER();
    }
    Value response_mstream_val = eval(node->children[0]);
    if (response_mstream_val.type == TYPE_MEMORYSTREAM && response_mstream_val.mstream != NULL) {
        MStream* eval_ms = response_mstream_val.mstream; // Get the MStream*
        if (eval_ms->buffer != NULL) { // Check if the buffer pointer member is not NULL
            char temp_debug_buffer[65];
            size_t print_len = eval_ms->size < 64 ? eval_ms->size : 64;
            strncpy(temp_debug_buffer, (char*)eval_ms->buffer, print_len);
            temp_debug_buffer[print_len] = '\0';
        }
     }
     fflush(stderr);
    
    // Ensure buffer is not NULL before creating string, even if size is 0
    const char* buffer_content = response_mstream_val.mstream->buffer ? (char *)response_mstream_val.mstream->buffer : "";
    Value result_string = makeString(buffer_content);

    // The MStream Value itself (response_mstream_val) doesn't own the mstream struct's memory
    // if it was just a copy of the pointer. The Pscal script is responsible for freeing the MStream
    // using MStreamFree(ms) eventually. So, we don't freeValue(&response_mstream_val) here as that
    // would free the mstream and its buffer, which is incorrect if api_receive is just a conversion.
    // However, if eval created a copy of an MStream Value that included a deep copy of the MStream struct
    // and its buffer (which makeCopyOfValue for MStream doesn't currently do, it's a shallow pointer copy),
    // then freeing it would be appropriate.
    // Given the current makeCopyOfValue and typical Value handling, we shouldn't free it here.

    return result_string;
}

