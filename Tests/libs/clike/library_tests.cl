import "crt.cl";
import "strings.cl";
import "filesystem.cl";
import "http.cl";
import "json.cl";
import "datetime.cl";
import "math_utils.cl";

int executedTests = 0;
int failedTests = 0;
int skippedTests = 0;
str DOUBLE_QUOTE = tochar(34);

str quote(str value) {
    return DOUBLE_QUOTE + value + DOUBLE_QUOTE;
}

void markPass(str name) {
    executedTests = executedTests + 1;
    printf("PASS %s\n", name);
}

void markFail(str name, str detail) {
    executedTests = executedTests + 1;
    failedTests = failedTests + 1;
    printf("FAIL %s: %s\n", name, detail);
}

void markSkip(str name, str reason) {
    skippedTests = skippedTests + 1;
    printf("SKIP %s: %s\n", name, reason);
}

void assertTrue(str name, int condition, str detail) {
    if (condition != 0) {
        markPass(name);
    } else {
        markFail(name, detail);
    }
}

void assertFalse(str name, int condition, str detail) {
    if (condition == 0) {
        markPass(name);
    } else {
        markFail(name, detail);
    }
}

void assertEqualInt(str name, int expected, int actual) {
    if (expected == actual) {
        markPass(name);
    } else {
        markFail(name, "expected " + inttostr(expected) + " but got " + inttostr(actual));
    }
}

void assertEqualStr(str name, str expected, str actual) {
    if (expected == actual) {
        markPass(name);
    } else {
        markFail(name, "expected " + quote(expected) + " but got " + quote(actual));
    }
}

void testCRT() {
    printf("\n-- CRT --\n");
    assertEqualInt("CRT.BLACK", 0, CRT_BLACK());
    assertEqualInt("CRT.LIGHT_RED", 12, CRT_LIGHT_RED());
    markSkip("CRT.TextAttr", "mutable TextAttr not available in module-safe runtime");
}

void testMathUtils() {
    printf("\n-- math_utils --\n");
    assertEqualInt("math_utils.square", 16, square(4));
    assertEqualInt("math_utils.cube", 27, cube(3));
}

void testStrings() {
    printf("\n-- strings --\n");
    assertEqualInt("strings.contains match", 1, strings_contains("hello world", "world"));
    assertEqualInt("strings.contains miss", 0, strings_contains("hello", "xyz"));
    assertEqualInt("strings.startsWith", 1, strings_startsWith("pscal", "ps"));
    assertEqualInt("strings.endsWith", 1, strings_endsWith("pscal", "al"));
    assertEqualStr("strings.trim", "trimmed", strings_trim("  trimmed  "));
    assertEqualStr("strings.padLeft", "***pad", strings_padLeft("pad", 6, "*"));
    assertEqualStr("strings.padRight", "pad---", strings_padRight("pad", 6, "-"));
    assertEqualStr("strings.toUpper", "PSCAL", strings_toUpper("pscal"));
    assertEqualStr("strings.toLower", "pascal", strings_toLower("PASCAL"));
    assertEqualStr("strings.sortCharacters", "aelp", strings_sortCharacters("peal"));
    assertEqualStr("strings.repeat", "haha", strings_repeat("ha", 2));
}

void testFilesystem(str tmpDir) {
    if (length(tmpDir) == 0) {
        markSkip("filesystem suite", "CLIKE_TEST_TMPDIR not set");
        return;
    }

    printf("\n-- filesystem --\n");
    str path = filesystem_joinPath(tmpDir, "clike_library_test.txt");
    int writeErr = 0;
    int writeOk = filesystem_writeAllText(path, "line1\nline2", &writeErr);
    assertEqualInt("filesystem.writeAllText", 1, writeOk);
    assertEqualInt("filesystem.writeAllText error", 0, writeErr);

    str contents;
    int readErr = 0;
    int readOk = filesystem_readAllText(path, &contents, &readErr);
    assertEqualInt("filesystem.readAllText success", 1, readOk);
    assertEqualInt("filesystem.readAllText error", 0, readErr);
    assertEqualStr("filesystem.readAllText", "line1\nline2", contents);

    str first;
    int firstErr = 0;
    int firstOk = filesystem_readFirstLine(path, &first, &firstErr);
    assertEqualInt("filesystem.readFirstLine success", 1, firstOk);
    assertEqualInt("filesystem.readFirstLine error", 0, firstErr);
    assertEqualStr("filesystem.readFirstLine", "line1", first);

    str home = getenv("HOME");
    if (length(home) > 0) {
        str expanded = filesystem_expandUser("~/documents");
        str expected = filesystem_joinPath(home, "documents");
        assertEqualStr("filesystem.expandUser", expected, expanded);
    } else {
        markSkip("filesystem.expandUser", "HOME not set");
    }
}

void testHttp(str baseUrl, str tmpDir) {
    if (length(baseUrl) == 0) {
        markSkip("http suite", "CLIKE_TEST_HTTP_BASE_URL not set");
        return;
    }

    printf("\n-- http --\n");
    str getBody;
    int getStatus = 0;
    int getOk = http_get(baseUrl + "/text", &getBody, &getStatus);
    assertEqualInt("http.get request", 1, getOk);
    assertEqualInt("http.get status", 200, getStatus);
    assertEqualStr("http.get", "hello world", getBody);

    str jsonBody;
    int jsonStatus = 0;
    int jsonOk = http_getJson(baseUrl + "/json", &jsonBody, &jsonStatus);
    assertEqualInt("http.getJson request", 1, jsonOk);
    assertEqualInt("http.getJson status", 200, jsonStatus);
    assertTrue("http.getJson accept header", strings_contains(jsonBody, "application/json"), "expected Accept header to be echoed");

    str postPayload = "{" + quote("hello") + ": " + quote("world") + "}";
    str postSummary;
    int postStatus = 0;
    int postOk = http_postJson(baseUrl + "/post-json", postPayload, &postSummary, &postStatus);
    assertEqualInt("http.postJson request", 1, postOk);
    assertEqualInt("http.postJson status", 200, postStatus);
    assertTrue("http.postJson response", strings_contains(postSummary, "method: POST"), "unexpected POST summary");

    str putSummary;
    int putStatus = 0;
    int putOk = http_put(baseUrl + "/text", "payload", "text/plain", &putSummary, &putStatus);
    assertEqualInt("http.put request", 1, putOk);
    assertEqualInt("http.put status", 200, putStatus);
    assertTrue("http.put response", strings_contains(putSummary, "method: PUT"), "unexpected PUT summary");

    str notFound;
    int notFoundStatus = 0;
    int notFoundOk = http_get(baseUrl + "/status/404", &notFound, &notFoundStatus);
    assertEqualInt("http.404 request", 1, notFoundOk);
    assertEqualInt("http.404 status", 404, notFoundStatus);
    assertEqualStr("http.404 body", "not found", notFound);

    if (length(tmpDir) > 0) {
        str downloadPath = filesystem_joinPath(tmpDir, "download.txt");
        int downloadStatus = 0;
        int downloadErrCode = 0;
        int ok = http_downloadToFile(baseUrl + "/download", downloadPath, &downloadStatus, &downloadErrCode);
        assertEqualInt("http.downloadToFile", 1, ok);
        assertEqualInt("http.downloadToFile status", 200, downloadStatus);
        assertEqualInt("http.downloadToFile error", 0, downloadErrCode);
        str downloaded;
        int downloadErr = 0;
        int downloadOk = filesystem_readAllText(downloadPath, &downloaded, &downloadErr);
        assertEqualInt("http.download read success", 1, downloadOk);
        assertEqualInt("http.download read error", 0, downloadErr);
        assertEqualStr("http.download contents", "download body", downloaded);
    } else {
        markSkip("http.downloadToFile", "CLIKE_TEST_TMPDIR not set");
    }
}

void testJson(str jsonPath) {
    str hasYyjson = getenv("CLIKE_TEST_HAS_YYJSON");
    if (length(hasYyjson) > 0 && hasYyjson != "1") {
        markSkip("json suite", "yyjson extended built-ins disabled");
        return;
    }
    if (!json_isAvailable()) {
        markSkip("json suite", "yyjson built-ins unavailable");
        return;
    }

    printf("\n-- json --\n");
    str inlineJson =
        "{" +
        quote("name") + ":" + quote("pscal") + "," +
        quote("version") + ":3," +
        quote("enabled") + ":true," +
        quote("values") + ":[1,2]," +
        quote("missing") + ":null}";
    int doc = json_parse(inlineJson);
    assertTrue("json.parse", doc >= 0, "expected parse to succeed");
    int root = json_root(doc);
    assertTrue("json.root", root >= 0, "expected root handle");
    assertEqualStr("json.getString", "pscal", json_getString(root, "name", "fallback"));
    assertEqualInt("json.getInt", 3, json_getInt(root, "version", -1));
    assertEqualInt("json.getBool", 1, json_getBool(root, "enabled", 0));
    assertEqualInt("json.isNull", 1, json_isNull(root, "missing"));
    json_closeDocument(doc);

    if (length(jsonPath) == 0) {
        markSkip("json.parseFile", "CLIKE_TEST_JSON_PATH not set");
        return;
    }

    int fileDoc = json_parseFile(jsonPath);
    assertTrue("json.parseFile", fileDoc >= 0, "expected file parse to succeed");
    int fileRoot = json_root(fileDoc);
    assertTrue("json.fileRoot", fileRoot >= 0, "expected file root");
    assertEqualStr("json.file getString", "pscal", json_getString(fileRoot, "name", "fallback"));
    assertEqualInt("json.file getInt", 2, json_getInt(fileRoot, "version", -1));
    assertEqualInt("json.file getBool", 1, json_getBool(fileRoot, "enabled", 0));
    assertEqualInt("json.file getInt fallback", 42, json_getInt(fileRoot, "missingNumber", 42));
    json_closeDocument(fileDoc);
}

void testDatetime() {
    printf("\n-- datetime --\n");
    assertEqualStr("datetime.formatUnix", "1970-01-01 00:00:00", datetime_formatUnix(0, 0));
    assertEqualStr("datetime.iso8601", "1970-01-01T00:00:00+00:00", datetime_iso8601(0, 0));
    assertEqualStr("datetime.offset positive", "+01:30", datetime_formatUtcOffset(5400));
    assertEqualStr("datetime.offset negative", "-01:00", datetime_formatUtcOffset(-3600));
    assertEqualInt("datetime.startOfDay", 0, datetime_startOfDay(12345, 0));
    assertEqualInt("datetime.endOfDay", 86399, datetime_endOfDay(0, 0));
    assertEqualInt("datetime.addDays", 172800, datetime_addDays(86400, 1));
    assertEqualInt("datetime.addHours", 7200, datetime_addHours(0, 2));
    assertEqualInt("datetime.addMinutes", 180, datetime_addMinutes(0, 3));
    assertEqualInt("datetime.daysBetween", 2, datetime_daysBetween(0, 172800));
    assertEqualStr("datetime.describeDifference", "1h 1m 1s", datetime_describeDifference(0, 3661));
}

void printSummary() {
    printf("\nExecuted: %d\n", executedTests);
    printf("Failed: %d\n", failedTests);
    printf("Skipped: %d\n", skippedTests);
}

int main() {
    printf("CLike Library Test Suite\n");
    testCRT();
    testMathUtils();
    testStrings();

    str tmpDir = getenv("CLIKE_TEST_TMPDIR");
    testFilesystem(tmpDir);

    str baseUrl = getenv("CLIKE_TEST_HTTP_BASE_URL");
    testHttp(baseUrl, tmpDir);

    str jsonPath = getenv("CLIKE_TEST_JSON_PATH");
    testJson(jsonPath);

    testDatetime();
    printSummary();
    if (failedTests > 0) {
        return 1;
    }
    return 0;
}
