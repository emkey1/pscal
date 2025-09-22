int main() {
    int doc;
    int root;
    int nameHandle;
    int versionHandle;
    int enabledHandle;
    int valuesHandle;
    int piHandle;
    int nestedHandle;
    int presentHandle;
    int maybeHandle;
    int valueHandle;
    int arrLen;
    int sum;
    int i;
    double piValue;

    doc = YyjsonReadFile("Tests/data/yyjson_sample.json");
    if (doc < 0) {
        printf("file_parse_failed\n");
        return 1;
    }

    root = YyjsonGetRoot(doc);
    printf("root_type=%s\n", YyjsonGetType(root));

    nameHandle = YyjsonGetKey(root, "name");
    printf("name=%s\n", YyjsonGetString(nameHandle));

    versionHandle = YyjsonGetKey(root, "version");
    printf("version=%d\n", (int)YyjsonGetInt(versionHandle));

    enabledHandle = YyjsonGetKey(root, "enabled");
    printf("enabled=%d\n", YyjsonGetBool(enabledHandle));

    valuesHandle = YyjsonGetKey(root, "values");
    arrLen = YyjsonGetLength(valuesHandle);
    printf("values_len=%d\n", arrLen);
    sum = 0;
    for (i = 0; i < arrLen; i = i + 1) {
        valueHandle = YyjsonGetIndex(valuesHandle, i);
        if (valueHandle >= 0) {
            sum = sum + (int)YyjsonGetInt(valueHandle);
            YyjsonFreeValue(valueHandle);
        }
    }
    printf("values_sum=%d\n", sum);

    piHandle = YyjsonGetKey(root, "pi");
    piValue = YyjsonGetNumber(piHandle);
    printf("pi=%.5f\n", piValue);

    nestedHandle = YyjsonGetKey(root, "nested");
    presentHandle = YyjsonGetKey(nestedHandle, "present");
    printf("nested_present=%s\n", YyjsonGetString(presentHandle));

    maybeHandle = YyjsonGetKey(root, "maybe");
    printf("maybe_is_null=%d\n", YyjsonIsNull(maybeHandle));

    printf("file_name=%s\n", YyjsonGetString(nameHandle));

    YyjsonFreeValue(maybeHandle);
    YyjsonFreeValue(presentHandle);
    YyjsonFreeValue(nestedHandle);
    YyjsonFreeValue(piHandle);
    YyjsonFreeValue(valuesHandle);
    YyjsonFreeValue(enabledHandle);
    YyjsonFreeValue(versionHandle);
    YyjsonFreeValue(nameHandle);
    YyjsonFreeValue(root);
    YyjsonDocFree(doc);

    return 0;
}
