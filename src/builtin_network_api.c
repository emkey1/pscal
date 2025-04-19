/* src/builtin_network_api.c */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include "builtin.h"
#include "interpreter.h"
#include "globals.h"
#include "utils.h"

/* Callback for libcurl: writes received data into a MStream */
static size_t writeCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t real_size = size * nmemb;
    MStream *mstream = (MStream *)userp;
    unsigned char *new_buffer = realloc(mstream->buffer, mstream->size + real_size + 1);
    if (!new_buffer) {
        fprintf(stderr, "Memory allocation error in write_callback\n");
        return 0;
    }
    mstream->buffer = new_buffer;
    memcpy(&(mstream->buffer[mstream->size]), contents, real_size);
    mstream->size += real_size;
    mstream->buffer[mstream->size] = '\0';
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

    if (url_val.type != TYPE_STRING) {
        fprintf(stderr, "Runtime error: api_send expects URL as a string.\n");
        EXIT_FAILURE_HANDLER();
    }

    char *request_body = NULL;
    if (body_val.type == TYPE_STRING) {
        request_body = body_val.s_val;
    } else if (body_val.type == TYPE_MEMORYSTREAM) {
        request_body = (char *)body_val.mstream->buffer;
    } else {
        fprintf(stderr, "Runtime error: api_send request body must be a string or memory stream.\n");
        EXIT_FAILURE_HANDLER();
    }

    /* Initialize a memory stream to store the response */
    MStream *response_stream = malloc(sizeof(MStream));
    if (!response_stream) {
        fprintf(stderr, "Memory allocation error for response stream structure.\n");
        EXIT_FAILURE_HANDLER();
    }
    response_stream->buffer = malloc(1); // start with an empty buffer
    if (!response_stream->buffer) {
        fprintf(stderr, "Memory allocation error for response stream buffer.\n");
        EXIT_FAILURE_HANDLER();
    }
    response_stream->size = 0;

    CURL *curl = curl_easy_init();
    if (!curl) {
        fprintf(stderr, "curl initialization failed.\n");
        free(response_stream->buffer);
        free(response_stream);
        EXIT_FAILURE_HANDLER();
    }

    struct curl_slist *headers = NULL;
    /* Set a default Content-Type header; adjust as needed for your API */
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url_val.s_val);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, response_stream);


    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        EXIT_FAILURE_HANDLER();
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);

    /* Return the response as a memory stream */
    return makeMStream(response_stream);
}

/* Built–in function: api_receive(MStream)
   For now, this simply converts the memory stream into a string.
   You could extend it to parse JSON or otherwise process the API response.
*/
Value executeBuiltinAPIReceive(AST *node) {
    Value response = eval(node->children[0]);
    if (response.type != TYPE_MEMORYSTREAM) {
        fprintf(stderr, "Runtime error: api_receive expects a memory stream.\n");
        EXIT_FAILURE_HANDLER();
    }
    return makeString((char *)response.mstream->buffer);
}

