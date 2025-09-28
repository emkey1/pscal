// Filesystem helpers modeled after lib/rea/filesystem.

int FS_LastReadOk = 0;
int FS_LastReadError = 0;
int FS_LastWriteError = 0;

void filesystem_markReadFailure(int code) {
    FS_LastReadOk = 0;
    FS_LastReadError = code;
}

void filesystem_markReadSuccess() {
    FS_LastReadOk = 1;
    FS_LastReadError = 0;
}

int filesystem_lastReadSucceeded() {
    return FS_LastReadOk;
}

int filesystem_lastReadErrorCode() {
    return FS_LastReadError;
}

int filesystem_lastWriteErrorCode() {
    return FS_LastWriteError;
}

str filesystem_readAllText(str path) {
    str contents = "";
    text f;
    int firstLine = 1;

    assign(f, path);
    reset(f);
    int err = ioresult();
    if (err != 0) {
        filesystem_markReadFailure(err);
        return "";
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
        filesystem_markReadFailure(err);
        return contents;
    }

    filesystem_markReadSuccess();
    return contents;
}

int filesystem_writeAllText(str path, str contents) {
    text f;

    assign(f, path);
    rewrite(f);
    int err = ioresult();
    if (err != 0) {
        FS_LastWriteError = err;
        return 0;
    }

    write(f, contents);
    err = ioresult();
    if (err != 0) {
        FS_LastWriteError = err;
        close(f);
        ioresult();
        return 0;
    }

    close(f);
    err = ioresult();
    if (err != 0) {
        FS_LastWriteError = err;
        return 0;
    }

    FS_LastWriteError = 0;
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

str filesystem_readFirstLine(str path) {
    str contents = filesystem_readAllText(path);
    if (!filesystem_lastReadSucceeded()) {
        return "";
    }
    int len = length(contents);
    int i = 1;
    while (i <= len) {
        if (contents[i] == '\n' || contents[i] == '\r') {
            if (i <= 1) {
                return "";
            }
            return copy(contents, 1, i - 1);
        }
        i = i + 1;
    }
    return contents;
}
