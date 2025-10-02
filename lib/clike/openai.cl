// Helpers for interacting with the OpenAI extended builtin.

str openai_hexDigits() {
    return "0123456789ABCDEF";
}

int openai_hexDigitValue(str ch) {
    if (length(ch) != 1) {
        return -1;
    }
    char c = ch[1];
    if (c >= '0' && c <= '9') {
        return ord(c) - ord('0');
    }
    if (c >= 'A' && c <= 'F') {
        return ord(c) - ord('A') + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return ord(c) - ord('a') + 10;
    }
    return -1;
}

int openai_parseHex(str hex) {
    if (length(hex) != 4) {
        return -1;
    }
    int value = 0;
    int i = 1;
    while (i <= 4) {
        str digitStr = copy(hex, i, 1);
        int digit = openai_hexDigitValue(digitStr);
        if (digit < 0) {
            return -1;
        }
        value = (value << 4) + digit;
        i = i + 1;
    }
    return value;
}

str openai_hexByte(int value) {
    str digits = openai_hexDigits();
    int high = ((value >> 4) & 15) + 1;
    int low = (value & 15) + 1;
    str result = "";
    result = result + copy(digits, high, 1);
    result = result + copy(digits, low, 1);
    return result;
}

str openai_jsonEscape(str text) {
    str result = "";
    int len = length(text);
    int i = 1;
    while (i <= len) {
        str ch = copy(text, i, 1);
        int code = ord(ch[1]);
        if (ch == "\"") {
            result = result + "\\\"";
        } else if (ch == "\\") {
            result = result + "\\\\";
        } else if (code == 8) {
            result = result + "\\b";
        } else if (code == 9) {
            result = result + "\\t";
        } else if (code == 10) {
            result = result + "\\n";
        } else if (code == 12) {
            result = result + "\\f";
        } else if (code == 13) {
            result = result + "\\r";
        } else if (code < 32) {
            result = result + "\\u00" + openai_hexByte(code);
        } else {
            result = result + ch;
        }
        i = i + 1;
    }
    return result;
}

int openai_isWhitespace(str ch) {
    if (ch == " " || ch == "\t" || ch == "\n" || ch == "\r") {
        return 1;
    }
    return 0;
}

int openai_skipWhitespace(str text, int index) {
    int len = length(text);
    while (index <= len) {
        str ch = copy(text, index, 1);
        if (!openai_isWhitespace(ch)) {
            break;
        }
        index = index + 1;
    }
    return index;
}

int openai_findSubstring(str haystack, str needle, int startIndex) {
    int hayLen = length(haystack);
    int needleLen = length(needle);
    if (needleLen == 0) {
        return startIndex;
    }
    int i = startIndex;
    int limit = hayLen - needleLen + 1;
    while (i <= limit) {
        if (copy(haystack, i, needleLen) == needle) {
            return i;
        }
        i = i + 1;
    }
    return 0;
}

str openai_parseJsonString(str text, int startIndex, int* outIndex, int* ok) {
    int len = length(text);
    int index = startIndex;
    str result = "";
    int success = 0;
    while (index <= len) {
        str ch = copy(text, index, 1);
        if (ch == "\"") {
            index = index + 1;
            success = 1;
            break;
        } else if (ch == "\\") {
            index = index + 1;
            if (index > len) {
                break;
            }
            str esc = copy(text, index, 1);
            if (esc == "\"" || esc == "\\" || esc == "/") {
                result = result + esc;
            } else if (esc == "b") {
                result = result + tochar(8);
            } else if (esc == "f") {
                result = result + tochar(12);
            } else if (esc == "n") {
                result = result + "\n";
            } else if (esc == "r") {
                result = result + "\r";
            } else if (esc == "t") {
                result = result + "\t";
            } else if (esc == "u") {
                if (index + 4 > len) {
                    break;
                }
                str hex = copy(text, index + 1, 4);
                int code = openai_parseHex(hex);
                if (code < 0) {
                    break;
                }
                result = result + tochar(code & 255);
                index = index + 4;
            } else {
                result = result + esc;
            }
        } else {
            result = result + ch;
        }
        index = index + 1;
    }
    if (outIndex != NULL) {
        *outIndex = index;
    }
    if (ok != NULL) {
        *ok = success;
    }
    return result;
}

str openai_extractFirstContent(str response) {
    int len = length(response);
    int searchStart = 1;
    while (searchStart <= len) {
        int idx = openai_findSubstring(response, "\"content\"", searchStart);
        if (idx <= 0) {
            break;
        }
        int cursor = idx + 9;
        cursor = openai_skipWhitespace(response, cursor);
        if (cursor <= len && copy(response, cursor, 1) == ":") {
            cursor = cursor + 1;
            cursor = openai_skipWhitespace(response, cursor);
            if (cursor <= len && copy(response, cursor, 1) == "\"") {
                cursor = cursor + 1;
                int nextIndex = cursor;
                int ok = 0;
                str value = openai_parseJsonString(response, cursor, &nextIndex, &ok);
                if (ok) {
                    return value;
                }
            }
        }
        searchStart = idx + 8;
    }
    return response;
}

str openai_composeMessages(str systemPrompt, str userPrompt) {
    str result = "[";
    if (length(systemPrompt) > 0) {
        result = result + "{\"role\":\"system\",\"content\":\"" +
            openai_jsonEscape(systemPrompt) + "\"}";
        if (length(userPrompt) > 0) {
            result = result + ",";
        }
    }
    result = result + "{\"role\":\"user\",\"content\":\"" +
        openai_jsonEscape(userPrompt) + "\"}]";
    return result;
}

str openai_chatRaw(str model, str messagesJson) {
    return OpenAIChatCompletions(model, messagesJson);
}

str openai_chatCustom(str model, str messagesJson, str optionsJson,
                      str apiKey, str baseUrl) {
    return OpenAIChatCompletions(model, messagesJson, optionsJson, apiKey, baseUrl);
}

str openai_chatWithOptions(str model, str systemPrompt, str userPrompt,
                           str optionsJson, str apiKey, str baseUrl) {
    str messages = openai_composeMessages(systemPrompt, userPrompt);
    str response = OpenAIChatCompletions(model, messages, optionsJson, apiKey, baseUrl);
    str content = openai_extractFirstContent(response);
    return content;
}

str openai_chatWithSystem(str model, str systemPrompt, str userPrompt) {
    return openai_chatWithOptions(model, systemPrompt, userPrompt, "", "", "");
}

str openai_chat(str model, str userPrompt) {
    return openai_chatWithSystem(model, "", userPrompt);
}
