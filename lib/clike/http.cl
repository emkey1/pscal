// HTTP helpers matching lib/rea/http.

int HTTP_LastStatus = 0;

void http_resetStatus(int status) {
    HTTP_LastStatus = status;
}

str http_requestWithBody(str method, str url, int hasBody, str body, str contentType, str accept) {
    mstream out;
    out = mstreamcreate();
    str response = "";
    int session = httpsession();
    if (session < 0) {
        http_resetStatus(-1);
        mstreamfree(&out);
        return response;
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
    http_resetStatus(status);

    if (status >= 0) {
        response = mstreambuffer(out);
    }

    httpclose(session);
    mstreamfree(&out);
    return response;
}

int http_lastResponseStatus() {
    return HTTP_LastStatus;
}

str http_get(str url) {
    return http_requestWithBody("GET", url, 0, "", "", "");
}

str http_getJson(str url) {
    return http_requestWithBody("GET", url, 0, "", "", "application/json");
}

str http_post(str url, str body, str contentType) {
    return http_requestWithBody("POST", url, 1, body, contentType, "");
}

str http_postJson(str url, str body) {
    return http_requestWithBody("POST", url, 1, body, "application/json", "application/json");
}

str http_put(str url, str body, str contentType) {
    return http_requestWithBody("PUT", url, 1, body, contentType, "");
}

int http_wasSuccessful() {
    if (HTTP_LastStatus >= 200 && HTTP_LastStatus < 300) {
        return 1;
    }
    return 0;
}

int http_downloadToFile(str url, str path) {
    mstream out;
    out = mstreamcreate();
    int session = httpsession();
    if (session < 0) {
        http_resetStatus(-1);
        mstreamfree(&out);
        return 0;
    }

    int status = httprequest(session, "GET", url, NULL, out);
    http_resetStatus(status);
    if (status < 0) {
        httpclose(session);
        mstreamfree(&out);
        return 0;
    }

    str body = mstreambuffer(out);
    httpclose(session);
    mstreamfree(&out);

    text f;
    assign(f, path);
    rewrite(f);
    int err = ioresult();
    if (err != 0) {
        return 0;
    }
    write(f, body);
    err = ioresult();
    if (err != 0) {
        close(f);
        ioresult();
        return 0;
    }
    close(f);
    err = ioresult();
    if (err != 0) {
        return 0;
    }
    return 1;
}
