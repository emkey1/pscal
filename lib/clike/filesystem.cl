// Filesystem helpers modeled after lib/rea/filesystem.

void filesystem_resetError(int* outError, int value) {
    if (outError != NULL) {
        *outError = value;
    }
}

int filesystem_readAllText(str path, str* outContents, int* outError) {
    if (outContents == NULL) {
        filesystem_resetError(outError, -1);
        return 0;
    }

    str contents = "";
    text f;
    int firstLine = 1;

    assign(f, path);
    reset(f);
    int err = ioresult();
    if (err != 0) {
        filesystem_resetError(outError, err);
        *outContents = "";
        return 0;
    }

    while (!eof(f)) {
        str line;
        readln(f, line);
        if (firstLine) {
            contents = line;
            firstLine = 0;
        } else {
            contents = contents + "\n" + line;
        }
    }

    close(f);
    err = ioresult();
    if (err != 0) {
        filesystem_resetError(outError, err);
        *outContents = contents;
        return 0;
    }

    filesystem_resetError(outError, 0);
    *outContents = contents;
    return 1;
}

int filesystem_writeAllText(str path, str contents, int* outError) {
    text f;

    assign(f, path);
    rewrite(f);
    int err = ioresult();
    if (err != 0) {
        filesystem_resetError(outError, err);
        return 0;
    }

    write(f, contents);
    err = ioresult();
    if (err != 0) {
        filesystem_resetError(outError, err);
        close(f);
        ioresult();
        return 0;
    }

    close(f);
    err = ioresult();
    if (err != 0) {
        filesystem_resetError(outError, err);
        return 0;
    }

    filesystem_resetError(outError, 0);
    return 1;
}

str filesystem_joinPath(str left, str right) {
    if (length(left) == 0) {
        return right;
    }
    if (length(right) == 0) {
        return left;
    }

    int leftEndsSlash = 0;
    str lastChar = copy(left, length(left), 1);
    if (lastChar == "/" || lastChar == "\\") {
        leftEndsSlash = 1;
    }

    int rightStartsSlash = 0;
    str firstChar = copy(right, 1, 1);
    if (firstChar == "/" || firstChar == "\\") {
        rightStartsSlash = 1;
    }

    if (leftEndsSlash && rightStartsSlash) {
        int rightLen = length(right);
        if (rightLen <= 1) {
            return left;
        }
        return left + copy(right, 2, rightLen - 1);
    }

    if (!leftEndsSlash && !rightStartsSlash) {
        return left + "/" + right;
    }

    return left + right;
}

str filesystem_expandUser(str path) {
    if (length(path) == 0) {
        return path;
    }
    if (path[1] != '~') {
        return path;
    }

    str home = getenv("HOME");
    if (length(home) == 0) {
        home = getenv("USERPROFILE");
    }
    if (length(home) == 0) {
        return path;
    }

    int pathLen = length(path);
    if (pathLen == 1) {
        return home;
    }

    str next = copy(path, 2, 1);
    if (next != "/" && next != "\\") {
        return path;
    }

    int homeEndsSlash = 0;
    str homeLast = copy(home, length(home), 1);
    if (homeLast == "/" || homeLast == "\\") {
        homeEndsSlash = 1;
    }

    int remainderLen = pathLen - 1;
    str remainder = copy(path, 2, remainderLen);

    if (homeEndsSlash) {
        if (remainderLen == 0) {
            return home;
        }
        if (remainder[1] == '/' || remainder[1] == '\\') {
            if (remainderLen <= 1) {
                return home;
            }
            return home + copy(remainder, 2, remainderLen - 1);
        }
        return home + remainder;
    }

    if (remainderLen == 0) {
        return home;
    }

    if (remainder[1] == '/' || remainder[1] == '\\') {
        if (remainderLen <= 1) {
            return home;
        }
        return home + copy(remainder, 2, remainderLen - 1);
    }

    return home + "/" + remainder;
}

int filesystem_readFirstLine(str path, str* outLine, int* outError) {
    if (outLine == NULL) {
        filesystem_resetError(outError, -1);
        return 0;
    }

    str contents;
    int readOk = filesystem_readAllText(path, &contents, outError);
    if (!readOk) {
        *outLine = "";
        return 0;
    }

    int len = length(contents);
    int i = 1;
    while (i <= len) {
        if (contents[i] == '\n' || contents[i] == '\r') {
            if (i <= 1) {
                *outLine = "";
                return 1;
            }
            *outLine = copy(contents, 1, i - 1);
            return 1;
        }
        i = i + 1;
    }

    *outLine = contents;
    filesystem_resetError(outError, 0);
    return 1;
}
