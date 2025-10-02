// HTTP helpers matching lib/rea/http.

import "filesystem.cl";

void http_storeStatus(int* outStatus, int status) {
    if (outStatus != NULL) {
        *outStatus = status;
    }
}

void http_storeError(int* outError, int value) {
    if (outError != NULL) {
        *outError = value;
    }
}

int http_requestWithBody(
    str method,
    str url,
    int hasBody,
    str body,
    str contentType,
    str accept,
    str* outResponse,
    int* outStatus
) {
    if (outResponse == NULL) {
        http_storeStatus(outStatus, -1);
        return 0;
    }

    mstream out;
    out = mstreamcreate();
    str response = "";
    int session = httpsession();
    if (session < 0) {
        http_storeStatus(outStatus, -1);
        mstreamfree(&out);
        *outResponse = "";
        return 0;
    }

    if (length(contentType) > 0) {
        httpsetheader(session, "Content-Type", contentType);
    }
    if (length(accept) > 0) {
        httpsetheader(session, "Accept", accept);
    }

    int status;
    if (hasBody) {
        status = httprequest(session, method, url, body, out);
    } else {
        status = httprequest(session, method, url, NULL, out);
    }
    http_storeStatus(outStatus, status);

    if (status >= 0) {
        response = mstreambuffer(out);
    } else {
        response = "";
    }

    httpclose(session);
    mstreamfree(&out);
    *outResponse = response;
    if (status >= 0) {
        return 1;
    }
    return 0;
}

int http_get(str url, str* outResponse, int* outStatus) {
    return http_requestWithBody("GET", url, 0, "", "", "", outResponse, outStatus);
}

int http_getJson(str url, str* outResponse, int* outStatus) {
    return http_requestWithBody("GET", url, 0, "", "", "application/json", outResponse, outStatus);
}

int http_post(str url, str body, str contentType, str* outResponse, int* outStatus) {
    return http_requestWithBody("POST", url, 1, body, contentType, "", outResponse, outStatus);
}

int http_postJson(str url, str body, str* outResponse, int* outStatus) {
    return http_requestWithBody("POST", url, 1, body, "application/json", "application/json", outResponse, outStatus);
}

int http_put(str url, str body, str contentType, str* outResponse, int* outStatus) {
    return http_requestWithBody("PUT", url, 1, body, contentType, "", outResponse, outStatus);
}

int http_downloadToFile(str url, str path, int* outStatus, int* outError) {
    mstream out;
    out = mstreamcreate();
    int session = httpsession();
    if (session < 0) {
        http_storeStatus(outStatus, -1);
        mstreamfree(&out);
        http_storeError(outError, -1);
        return 0;
    }

    int status = httprequest(session, "GET", url, NULL, out);
    http_storeStatus(outStatus, status);
    if (status < 0) {
        httpclose(session);
        mstreamfree(&out);
        http_storeError(outError, -1);
        return 0;
    }

    str body = mstreambuffer(out);
    httpclose(session);
    mstreamfree(&out);

    text f;
    int writeErr = 0;
    int ok = filesystem_writeAllText(path, body, &writeErr);
    http_storeError(outError, writeErr);
    return ok;
}
