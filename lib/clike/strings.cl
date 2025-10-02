// String helpers that parallel lib/rea/strings.

int strings_isWhitespace(str ch) {
    if (ch == " " || ch == "\t" || ch == "\n" || ch == "\r") {
        return 1;
    }
    return 0;
}

str strings_resolvePadChar(str padding) {
    if (length(padding) == 0) {
        return " ";
    }
    return copy(padding, 1, 1);
}

int strings_contains(str haystack, str needle) {
    int needleLen = length(needle);
    if (needleLen == 0) {
        return 1;
    }
    int haystackLen = length(haystack);
    if (haystackLen == 0 || needleLen > haystackLen) {
        return 0;
    }
    int i = 1;
    int limit = haystackLen - needleLen + 1;
    while (i <= limit) {
        if (copy(haystack, i, needleLen) == needle) {
            return 1;
        }
        i = i + 1;
    }
    return 0;
}

int strings_startsWith(str value, str prefix) {
    int prefixLen = length(prefix);
    if (prefixLen == 0) {
        return 1;
    }
    if (length(value) < prefixLen) {
        return 0;
    }
    return copy(value, 1, prefixLen) == prefix ? 1 : 0;
}

int strings_endsWith(str value, str suffix) {
    int suffixLen = length(suffix);
    if (suffixLen == 0) {
        return 1;
    }
    int valueLen = length(value);
    if (valueLen < suffixLen) {
        return 0;
    }
    return copy(value, valueLen - suffixLen + 1, suffixLen) == suffix ? 1 : 0;
}

str strings_trimLeft(str value) {
    int len = length(value);
    int i = 1;
    while (i <= len && strings_isWhitespace(copy(value, i, 1))) {
        i = i + 1;
    }
    if (i > len) {
        return "";
    }
    return copy(value, i, len - i + 1);
}

str strings_trimRight(str value) {
    int len = length(value);
    int i = len;
    while (i >= 1 && strings_isWhitespace(copy(value, i, 1))) {
        i = i - 1;
    }
    if (i <= 0) {
        return "";
    }
    return copy(value, 1, i);
}

str strings_trim(str value) {
    return strings_trimRight(strings_trimLeft(value));
}

str strings_padLeft(str value, int width, str padding) {
    int valueLen = length(value);
    if (width <= valueLen) {
        return value;
    }
    int padLen = width - valueLen;
    str padChar = strings_resolvePadChar(padding);
    str result = "";
    int i = 0;
    while (i < padLen) {
        result = result + padChar;
        i = i + 1;
    }
    result = result + value;
    return result;
}

str strings_padRight(str value, int width, str padding) {
    int valueLen = length(value);
    if (width <= valueLen) {
        return value;
    }
    int padLen = width - valueLen;
    str padChar = strings_resolvePadChar(padding);
    str result = value;
    int i = 0;
    while (i < padLen) {
        result = result + padChar;
        i = i + 1;
    }
    return result;
}

str strings_toUpper(str value) {
    int len = length(value);
    if (len == 0) {
        return value;
    }
    str result = "";
    int i = 1;
    while (i <= len) {
        char ch = value[i];
        int code = ord(ch);
        if (code >= ord('a') && code <= ord('z')) {
            ch = tochar(code - 32);
        }
        result = result + ch;
        i = i + 1;
    }
    return result;
}

str strings_toLower(str value) {
    int len = length(value);
    if (len == 0) {
        return value;
    }
    str result = "";
    int i = 1;
    while (i <= len) {
        char ch = value[i];
        int code = ord(ch);
        if (code >= ord('A') && code <= ord('Z')) {
            ch = tochar(code + 32);
        }
        result = result + ch;
        i = i + 1;
    }
    return result;
}

str strings_sortCharacters(str value) {
    int len = length(value);
    if (len <= 1) {
        return value;
    }
    str remaining = value;
    str result = "";
    while (length(remaining) > 0) {
        str minChar = copy(remaining, 1, 1);
        int minIndex = 1;
        int i = 2;
        int remLen = length(remaining);
        while (i <= remLen) {
            str current = copy(remaining, i, 1);
            if (current < minChar) {
                minChar = current;
                minIndex = i;
            }
            i = i + 1;
        }
        result = result + minChar;
        remLen = length(remaining);
        if (minIndex == 1) {
            remaining = copy(remaining, 2, remLen - 1);
        } else if (minIndex == remLen) {
            remaining = copy(remaining, 1, remLen - 1);
        } else {
            str left = copy(remaining, 1, minIndex - 1);
            str right = copy(remaining, minIndex + 1, remLen - minIndex);
            remaining = left + right;
        }
    }
    return result;
}

str strings_repeat(str value, int count) {
    if (count <= 0 || length(value) == 0) {
        return "";
    }
    str result = "";
    int i = 0;
    while (i < count) {
        result = result + value;
        i = i + 1;
    }
    return result;
}
