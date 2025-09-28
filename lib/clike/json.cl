// JSON helpers wrapping yyjson built-ins similar to lib/rea/json.

int json_isAvailable() {
    return hasextbuiltin("yyjson", "YyjsonRead") ? 1 : 0;
}

int json_parse(str source) {
    if (!json_isAvailable()) {
        return -1;
    }
    return YyjsonRead(source);
}

int json_parseFile(str path) {
    if (!json_isAvailable()) {
        return -1;
    }
    return YyjsonReadFile(path);
}

void json_closeDocument(int doc) {
    if (doc >= 0) {
        YyjsonDocFree(doc);
    }
}

int json_root(int doc) {
    if (doc < 0) {
        return -1;
    }
    return YyjsonGetRoot(doc);
}

void json_free(int valueHandle) {
    if (valueHandle >= 0) {
        YyjsonFreeValue(valueHandle);
    }
}

int json_hasKey(int objectHandle, str key) {
    if (objectHandle < 0) {
        return 0;
    }
    int handle = YyjsonGetKey(objectHandle, key);
    if (handle >= 0) {
        YyjsonFreeValue(handle);
        return 1;
    }
    return 0;
}

str json_getString(int objectHandle, str key, str fallback) {
    if (objectHandle < 0) {
        return fallback;
    }
    int handle = YyjsonGetKey(objectHandle, key);
    if (handle < 0) {
        return fallback;
    }
    str value = YyjsonGetString(handle);
    YyjsonFreeValue(handle);
    return value;
}

int json_getInt(int objectHandle, str key, int fallback) {
    if (objectHandle < 0) {
        return fallback;
    }
    int handle = YyjsonGetKey(objectHandle, key);
    if (handle < 0) {
        return fallback;
    }
    int result = (int)YyjsonGetInt(handle);
    YyjsonFreeValue(handle);
    return result;
}

float json_getNumber(int objectHandle, str key, float fallback) {
    if (objectHandle < 0) {
        return fallback;
    }
    int handle = YyjsonGetKey(objectHandle, key);
    if (handle < 0) {
        return fallback;
    }
    float result = (float)YyjsonGetNumber(handle);
    YyjsonFreeValue(handle);
    return result;
}

int json_getBool(int objectHandle, str key, int fallback) {
    if (objectHandle < 0) {
        return fallback;
    }
    int handle = YyjsonGetKey(objectHandle, key);
    if (handle < 0) {
        return fallback;
    }
    int value = YyjsonGetBool(handle);
    YyjsonFreeValue(handle);
    return value != 0 ? 1 : 0;
}

int json_isNull(int objectHandle, str key) {
    if (objectHandle < 0) {
        return 1;
    }
    int handle = YyjsonGetKey(objectHandle, key);
    if (handle < 0) {
        return 1;
    }
    int result = YyjsonIsNull(handle);
    YyjsonFreeValue(handle);
    return result != 0 ? 1 : 0;
}

int json_arrayLength(int arrayHandle) {
    if (arrayHandle < 0) {
        return 0;
    }
    return YyjsonGetLength(arrayHandle);
}

int json_arrayIndex(int arrayHandle, int index) {
    if (arrayHandle < 0) {
        return -1;
    }
    if (index < 0) {
        return -1;
    }
    return YyjsonGetIndex(arrayHandle, index);
}

str json_getStringAt(int arrayHandle, int index, str fallback) {
    int handle = json_arrayIndex(arrayHandle, index);
    if (handle < 0) {
        return fallback;
    }
    str value = YyjsonGetString(handle);
    YyjsonFreeValue(handle);
    return value;
}

int json_getIntAt(int arrayHandle, int index, int fallback) {
    int handle = json_arrayIndex(arrayHandle, index);
    if (handle < 0) {
        return fallback;
    }
    int result = (int)YyjsonGetInt(handle);
    YyjsonFreeValue(handle);
    return result;
}

float json_getNumberAt(int arrayHandle, int index, float fallback) {
    int handle = json_arrayIndex(arrayHandle, index);
    if (handle < 0) {
        return fallback;
    }
    float result = (float)YyjsonGetNumber(handle);
    YyjsonFreeValue(handle);
    return result;
}

int json_getBoolAt(int arrayHandle, int index, int fallback) {
    int handle = json_arrayIndex(arrayHandle, index);
    if (handle < 0) {
        return fallback;
    }
    int value = YyjsonGetBool(handle);
    YyjsonFreeValue(handle);
    return value != 0 ? 1 : 0;
}
