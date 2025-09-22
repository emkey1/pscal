#include <ctype.h>
#include <errno.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct ArgList {
    char **items;
    size_t count;
    size_t capacity;
} ArgList;

static void freeArgList(ArgList *list) {
    if (!list) {
        return;
    }
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int ensureBufferCapacity(char **buffer, size_t *capacity, size_t needed) {
    if (needed <= *capacity) {
        return 0;
    }
    size_t newCapacity = *capacity ? *capacity : 16;
    while (newCapacity < needed) {
        newCapacity *= 2;
    }
    char *resized = realloc(*buffer, newCapacity);
    if (!resized) {
        return -1;
    }
    *buffer = resized;
    *capacity = newCapacity;
    return 0;
}

static int appendArgOwned(ArgList *list, char *value) {
    if (!list || !value) {
        return -1;
    }
    if (list->count == list->capacity) {
        size_t newCapacity = list->capacity ? list->capacity * 2 : 4;
        char **resized = realloc(list->items, newCapacity * sizeof(char *));
        if (!resized) {
            return -1;
        }
        list->items = resized;
        list->capacity = newCapacity;
    }
    list->items[list->count++] = value;
    return 0;
}

static int parseArgumentString(const char *input, ArgList *list) {
    if (!input || !list) {
        return 0;
    }

    const char *cursor = input;
    while (*cursor) {
        while (*cursor && isspace((unsigned char)*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        char *buffer = NULL;
        size_t capacity = 0;
        size_t length = 0;
        bool inSingleQuote = false;
        bool inDoubleQuote = false;
        bool escapeNext = false;

        while (*cursor) {
            char ch = *cursor;
            if (escapeNext) {
                if (ensureBufferCapacity(&buffer, &capacity, length + 1) != 0) {
                    free(buffer);
                    return -1;
                }
                buffer[length++] = ch;
                escapeNext = false;
                ++cursor;
                continue;
            }

            if (ch == '\\') {
                escapeNext = true;
                ++cursor;
                continue;
            }

            if (ch == '\'' && !inDoubleQuote) {
                inSingleQuote = !inSingleQuote;
                ++cursor;
                continue;
            }

            if (ch == '"' && !inSingleQuote) {
                inDoubleQuote = !inDoubleQuote;
                ++cursor;
                continue;
            }

            if (!inSingleQuote && !inDoubleQuote && isspace((unsigned char)ch)) {
                break;
            }

            if (ensureBufferCapacity(&buffer, &capacity, length + 1) != 0) {
                free(buffer);
                return -1;
            }
            buffer[length++] = ch;
            ++cursor;
        }

        if (escapeNext || inSingleQuote || inDoubleQuote) {
            fprintf(stderr, "[pscal-runner] unmatched quote or escape sequence in arguments: %s\n", input);
            free(buffer);
            return -1;
        }

        if (ensureBufferCapacity(&buffer, &capacity, length + 1) != 0) {
            free(buffer);
            return -1;
        }
        buffer[length] = '\0';

        if (appendArgOwned(list, buffer) != 0) {
            free(buffer);
            return -1;
        }

        while (*cursor && isspace((unsigned char)*cursor)) {
            ++cursor;
        }
    }

    return 0;
}

static char *trimWhitespace(char *text) {
    if (!text) {
        return NULL;
    }
    char *start = text;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    char *end = start + strlen(start);
    while (end > start && isspace((unsigned char)*(end - 1))) {
        --end;
    }
    *end = '\0';
    return start;
}

static char *duplicateTrimmed(const char *text) {
    if (!text) {
        return NULL;
    }
    char *copy = strdup(text);
    if (!copy) {
        return NULL;
    }
    char *trimmed = trimWhitespace(copy);
    char *result = strdup(trimmed);
    free(copy);
    return result;
}

static char *resolvePath(const char *baseDir, const char *path) {
    if (!path) {
        return NULL;
    }
    if (path[0] == '\0') {
        return strdup("");
    }
    if (path[0] == '/') {
        return strdup(path);
    }

    char *resolved = NULL;
    if (baseDir && baseDir[0] != '\0') {
        size_t combinedLength = strlen(baseDir) + 1 + strlen(path) + 1;
        char *combined = malloc(combinedLength);
        if (!combined) {
            return NULL;
        }
        snprintf(combined, combinedLength, "%s/%s", baseDir, path);
        resolved = realpath(combined, NULL);
        if (!resolved) {
            resolved = combined;
        } else {
            free(combined);
        }
    } else {
        resolved = realpath(path, NULL);
        if (!resolved) {
            resolved = strdup(path);
        }
    }
    return resolved;
}

static int loadConfig(const char *path, const char *baseDir, ArgList *args, char **binaryName, char **workingDir) {
    FILE *file = fopen(path, "r");
    if (!file) {
        fprintf(stderr, "[pscal-runner] warning: unable to open configuration file '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[4096];
    unsigned int lineNumber = 0;
    int status = 0;

    while (fgets(line, sizeof(line), file)) {
        ++lineNumber;
        char *trimmedLine = trimWhitespace(line);
        if (*trimmedLine == '\0' || *trimmedLine == '#') {
            continue;
        }

        char *equals = strchr(trimmedLine, '=');
        if (!equals) {
            fprintf(stderr, "[pscal-runner] ignoring malformed line %u in %s\n", lineNumber, path);
            continue;
        }

        *equals = '\0';
        char *key = trimWhitespace(trimmedLine);
        char *value = trimWhitespace(equals + 1);

        if (strcmp(key, "binary") == 0) {
            if (*value == '\0') {
                fprintf(stderr, "[pscal-runner] ignoring empty binary entry on line %u in %s\n", lineNumber, path);
                continue;
            }
            char *copy = strdup(value);
            if (!copy) {
                status = -1;
                break;
            }
            free(*binaryName);
            *binaryName = copy;
        } else if (strcmp(key, "args") == 0) {
            if (*value == '\0') {
                continue;
            }
            if (parseArgumentString(value, args) != 0) {
                fprintf(stderr, "[pscal-runner] invalid arguments on line %u in %s\n", lineNumber, path);
                status = -1;
                break;
            }
        } else if (strcmp(key, "working_dir") == 0) {
            free(*workingDir);
            *workingDir = resolvePath(baseDir, value);
            if (!*workingDir) {
                fprintf(stderr, "[pscal-runner] failed to resolve working directory on line %u in %s\n", lineNumber, path);
                status = -1;
                break;
            }
        } else {
            fprintf(stderr, "[pscal-runner] ignoring unknown key '%s' on line %u in %s\n", key, lineNumber, path);
        }
    }

    fclose(file);
    return status;
}

static char *getExecutableDirectory(void) {
    char stackPath[PATH_MAX];
    uint32_t stackSize = (uint32_t)sizeof(stackPath);
    char *resolvedPath = NULL;

    int result = _NSGetExecutablePath(stackPath, &stackSize);
    if (result == -1) {
        char *dynamicPath = malloc(stackSize);
        if (!dynamicPath) {
            return NULL;
        }
        if (_NSGetExecutablePath(dynamicPath, &stackSize) != 0) {
            free(dynamicPath);
            return NULL;
        }
        resolvedPath = realpath(dynamicPath, NULL);
        free(dynamicPath);
    } else {
        resolvedPath = realpath(stackPath, NULL);
    }

    if (!resolvedPath) {
        return NULL;
    }

    char *dirCopy = strdup(resolvedPath);
    free(resolvedPath);
    if (!dirCopy) {
        return NULL;
    }

    char *dirName = dirname(dirCopy);
    char *resultPath = dirName ? strdup(dirName) : NULL;
    free(dirCopy);
    return resultPath;
}

static void writeQuoted(FILE *stream, const char *text) {
    bool needsQuotes = false;
    for (const char *cursor = text; cursor && *cursor; ++cursor) {
        unsigned char ch = (unsigned char)*cursor;
        if (isspace(ch) || ch == '"' || ch == '\\') {
            needsQuotes = true;
            break;
        }
    }

    if (!needsQuotes) {
        fprintf(stream, " %s", text);
        return;
    }

    fputc(' ', stream);
    fputc('"', stream);
    for (const char *cursor = text; cursor && *cursor; ++cursor) {
        char ch = *cursor;
        if (ch == '"' || ch == '\\') {
            fputc('\\', stream);
        }
        fputc(ch, stream);
    }
    fputc('"', stream);
}

static void printLaunchSummary(const char *path, const ArgList *args) {
    fprintf(stderr, "[pscal-runner] Launching %s", path);
    if (args) {
        for (size_t i = 0; i < args->count; ++i) {
            writeQuoted(stderr, args->items[i]);
        }
    }
    fputc('\n', stderr);
}

int main(void) {
    ArgList arguments = {0};
    char *binaryName = strdup("pascal");
    char *workingDirectory = NULL;
    char *configDir = NULL;

    if (!binaryName) {
        fprintf(stderr, "[pscal-runner] out of memory\n");
        return EXIT_FAILURE;
    }

    const char *configPath = getenv("PSCAL_RUN_CONFIG");
    if (configPath && *configPath) {
        char *configCopy = strdup(configPath);
        if (!configCopy) {
            freeArgList(&arguments);
            free(binaryName);
            fprintf(stderr, "[pscal-runner] out of memory\n");
            return EXIT_FAILURE;
        }
        char *dirName = dirname(configCopy);
        if (dirName) {
            configDir = strdup(dirName);
        }
        free(configCopy);

        if (!configDir) {
            fprintf(stderr, "[pscal-runner] unable to determine configuration directory\n");
            freeArgList(&arguments);
            free(binaryName);
            return EXIT_FAILURE;
        }

        if (access(configPath, R_OK) == 0) {
            if (loadConfig(configPath, configDir, &arguments, &binaryName, &workingDirectory) != 0) {
                freeArgList(&arguments);
                free(binaryName);
                free(workingDirectory);
                free(configDir);
                return EXIT_FAILURE;
            }
        } else {
            fprintf(stderr, "[pscal-runner] warning: cannot read configuration file '%s': %s\n", configPath, strerror(errno));
        }
    }

    const char *envBinary = getenv("PSCAL_RUN_BINARY");
    if (envBinary && *envBinary) {
        char *overrideBinary = duplicateTrimmed(envBinary);
        if (!overrideBinary) {
            fprintf(stderr, "[pscal-runner] out of memory\n");
            freeArgList(&arguments);
            free(binaryName);
            free(workingDirectory);
            free(configDir);
            return EXIT_FAILURE;
        }
        if (*overrideBinary) {
            free(binaryName);
            binaryName = overrideBinary;
        } else {
            free(overrideBinary);
        }
    }

    const char *envArgs = getenv("PSCAL_RUN_ARGUMENTS");
    if (envArgs && *envArgs) {
        ArgList overrideArgs = {0};
        if (parseArgumentString(envArgs, &overrideArgs) != 0) {
            fprintf(stderr, "[pscal-runner] failed to parse PSCAL_RUN_ARGUMENTS\n");
            freeArgList(&overrideArgs);
            freeArgList(&arguments);
            free(binaryName);
            free(workingDirectory);
            free(configDir);
            return EXIT_FAILURE;
        }
        freeArgList(&arguments);
        arguments = overrideArgs;
    }

    const char *envWorkingDir = getenv("PSCAL_RUN_WORKING_DIRECTORY");
    if (envWorkingDir && *envWorkingDir) {
        char *resolved = resolvePath(configDir, envWorkingDir);
        if (!resolved) {
            fprintf(stderr, "[pscal-runner] failed to resolve PSCAL_RUN_WORKING_DIRECTORY\n");
            freeArgList(&arguments);
            free(binaryName);
            free(workingDirectory);
            free(configDir);
            return EXIT_FAILURE;
        }
        free(workingDirectory);
        workingDirectory = resolved;
    }

    char *runnerDir = getExecutableDirectory();
    if (!runnerDir) {
        fprintf(stderr, "[pscal-runner] unable to locate build directory\n");
        freeArgList(&arguments);
        free(binaryName);
        free(workingDirectory);
        free(configDir);
        return EXIT_FAILURE;
    }

    if (!binaryName || *binaryName == '\0') {
        fprintf(stderr, "[pscal-runner] no binary specified\n");
        freeArgList(&arguments);
        free(binaryName);
        free(workingDirectory);
        free(configDir);
        free(runnerDir);
        return EXIT_FAILURE;
    }

    char targetPath[PATH_MAX];
    if (snprintf(targetPath, sizeof(targetPath), "%s/%s", runnerDir, binaryName) >= (int)sizeof(targetPath)) {
        fprintf(stderr, "[pscal-runner] executable path is too long\n");
        freeArgList(&arguments);
        free(binaryName);
        free(workingDirectory);
        free(configDir);
        free(runnerDir);
        return EXIT_FAILURE;
    }

    if (access(targetPath, X_OK) != 0) {
        fprintf(stderr, "[pscal-runner] executable '%s' is not available in %s\n", binaryName, runnerDir);
        freeArgList(&arguments);
        free(binaryName);
        free(workingDirectory);
        free(configDir);
        free(runnerDir);
        return EXIT_FAILURE;
    }

    if (workingDirectory && chdir(workingDirectory) != 0) {
        fprintf(stderr, "[pscal-runner] unable to change directory to '%s': %s\n", workingDirectory, strerror(errno));
        freeArgList(&arguments);
        free(binaryName);
        free(workingDirectory);
        free(configDir);
        free(runnerDir);
        return EXIT_FAILURE;
    }

    size_t argc = arguments.count + 2;
    char **childArgv = calloc(argc, sizeof(char *));
    if (!childArgv) {
        fprintf(stderr, "[pscal-runner] out of memory\n");
        freeArgList(&arguments);
        free(binaryName);
        free(workingDirectory);
        free(configDir);
        free(runnerDir);
        return EXIT_FAILURE;
    }

    childArgv[0] = binaryName;
    for (size_t i = 0; i < arguments.count; ++i) {
        childArgv[i + 1] = arguments.items[i];
    }
    childArgv[arguments.count + 1] = NULL;

    printLaunchSummary(targetPath, &arguments);

    execv(targetPath, childArgv);

    fprintf(stderr, "[pscal-runner] failed to launch '%s': %s\n", targetPath, strerror(errno));

    free(childArgv);
    freeArgList(&arguments);
    free(binaryName);
    free(workingDirectory);
    free(configDir);
    free(runnerDir);
    return EXIT_FAILURE;
}
