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

typedef enum ConfigLoadResult {
    CONFIG_LOAD_SUCCESS = 0,
    CONFIG_LOAD_NOT_FOUND = 1,
    CONFIG_LOAD_ERROR = -1
} ConfigLoadResult;

static const char *getProjectDirectoryEnv(void) {
    static const char *const candidates[] = {
        "PROJECT_DIR",
        "SRCROOT",
        "SOURCE_ROOT",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        const char *value = getenv(candidates[i]);
        if (value && *value) {
            return value;
        }
    }
    return NULL;
}

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

static char *expandEnvironmentMacros(const char *input) {
    if (!input) {
        return NULL;
    }

    size_t capacity = strlen(input) + 1;
    char *output = malloc(capacity);
    if (!output) {
        return NULL;
    }

    size_t outLength = 0;
    for (size_t i = 0; input[i] != '\0';) {
        if (input[i] == '$' && input[i + 1] == '(') {
            size_t nameStart = i + 2;
            size_t nameEnd = nameStart;
            while (input[nameEnd] && input[nameEnd] != ')') {
                ++nameEnd;
            }
            if (input[nameEnd] == ')' && nameEnd > nameStart) {
                size_t nameLen = nameEnd - nameStart;
                char *name = malloc(nameLen + 1);
                if (!name) {
                    free(output);
                    return NULL;
                }
                memcpy(name, input + nameStart, nameLen);
                name[nameLen] = '\0';
                const char *value = getenv(name);
                free(name);
                if (value && *value) {
                    size_t valueLen = strlen(value);
                    if (ensureBufferCapacity(&output, &capacity, outLength + valueLen + 1) != 0) {
                        free(output);
                        return NULL;
                    }
                    memcpy(output + outLength, value, valueLen);
                    outLength += valueLen;
                    i = nameEnd + 1;
                    continue;
                }
            }
        }

        if (ensureBufferCapacity(&output, &capacity, outLength + 2) != 0) {
            free(output);
            return NULL;
        }
        output[outLength++] = input[i++];
    }

    output[outLength] = '\0';
    return output;
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

static char *joinPath(const char *base, const char *component) {
    if (!base || !component) {
        return NULL;
    }

    size_t baseLength = strlen(base);
    bool needsSlash = baseLength > 0 && base[baseLength - 1] != '/';
    size_t totalLength = baseLength + (needsSlash ? 1 : 0) + strlen(component) + 1;

    char *combined = malloc(totalLength);
    if (!combined) {
        errno = ENOMEM;
        return NULL;
    }

    if (needsSlash) {
        snprintf(combined, totalLength, "%s/%s", base, component);
    } else {
        snprintf(combined, totalLength, "%s%s", base, component);
    }

    return combined;
}

static char *duplicateParentDirectory(const char *path) {
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    char *pathCopy = strdup(path);
    if (!pathCopy) {
        errno = ENOMEM;
        return NULL;
    }

    char *parent = dirname(pathCopy);
    if (!parent) {
        free(pathCopy);
        errno = EINVAL;
        return NULL;
    }

    char *result = strdup(parent);
    free(pathCopy);

    if (!result) {
        errno = ENOMEM;
        return NULL;
    }

    return result;
}

static char *getDefaultConfigPath(void) {
    char *sourcePath = realpath(__FILE__, NULL);
    if (!sourcePath) {
        sourcePath = strdup(__FILE__);
        if (!sourcePath) {
            errno = ENOMEM;
            return NULL;
        }
    }

    char *supportDir = duplicateParentDirectory(sourcePath);
    free(sourcePath);
    if (!supportDir) {
        return NULL;
    }

    char *projectDir = duplicateParentDirectory(supportDir);
    free(supportDir);
    if (!projectDir) {
        return NULL;
    }

    char *configPath = joinPath(projectDir, "RunConfiguration.cfg");
    free(projectDir);

    if (!configPath) {
        errno = ENOMEM;
        return NULL;
    }

    return configPath;
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

static ConfigLoadResult loadConfigFromPath(const char *path, bool warnOnMissing, ArgList *args, char **binaryName, char **workingDir, char **configDir) {
    if (!path || !*path) {
        return CONFIG_LOAD_NOT_FOUND;
    }

    if (access(path, R_OK) != 0) {
        if (warnOnMissing) {
            int savedErrno = errno;
            fprintf(stderr, "[pscal-runner] warning: cannot read configuration file '%s': %s\n", path, strerror(savedErrno));
        }
        return CONFIG_LOAD_NOT_FOUND;
    }

    char *pathCopy = strdup(path);
    if (!pathCopy) {
        fprintf(stderr, "[pscal-runner] out of memory\n");
        return CONFIG_LOAD_ERROR;
    }

    char *dirName = dirname(pathCopy);
    char *dirCopy = dirName ? strdup(dirName) : NULL;
    if (dirName && !dirCopy) {
        fprintf(stderr, "[pscal-runner] out of memory\n");
        free(pathCopy);
        return CONFIG_LOAD_ERROR;
    }

    if (loadConfig(path, dirCopy, args, binaryName, workingDir) != 0) {
        free(dirCopy);
        free(pathCopy);
        return CONFIG_LOAD_ERROR;
    }

    free(pathCopy);
    free(*configDir);
    *configDir = dirCopy;

    return CONFIG_LOAD_SUCCESS;
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

    bool configLoaded = false;
    const char *configEnv = getenv("PSCAL_RUN_CONFIG");
    char *expandedConfigPath = NULL;
    if (configEnv && *configEnv) {
        expandedConfigPath = expandEnvironmentMacros(configEnv);
        if (!expandedConfigPath) {
            fprintf(stderr, "[pscal-runner] out of memory\n");
            freeArgList(&arguments);
            free(binaryName);
            return EXIT_FAILURE;
        }
        char *envConfigDir = duplicateParentDirectory(expandedConfigPath);
        if (!envConfigDir) {
            fprintf(stderr, "[pscal-runner] unable to determine configuration directory\n");
            freeArgList(&arguments);
            free(binaryName);
            free(expandedConfigPath);
            return EXIT_FAILURE;
        }
        free(configDir);
        configDir = envConfigDir;

        ConfigLoadResult result = loadConfigFromPath(expandedConfigPath, true, &arguments, &binaryName, &workingDirectory, &configDir);
        free(expandedConfigPath);
        expandedConfigPath = NULL;
        if (result == CONFIG_LOAD_ERROR) {
            freeArgList(&arguments);
            free(binaryName);
            free(workingDirectory);
            free(configDir);
            return EXIT_FAILURE;
        }
        if (result == CONFIG_LOAD_SUCCESS) {
            configLoaded = true;
        }
    }

    free(expandedConfigPath);

    if (!configLoaded) {
        const char *projectDir = getProjectDirectoryEnv();
        if (projectDir && *projectDir) {
            char *projectConfigPath = joinPath(projectDir, "RunConfiguration.cfg");
            if (!projectConfigPath) {
                fprintf(stderr, "[pscal-runner] out of memory\n");
                freeArgList(&arguments);
                free(binaryName);
                free(workingDirectory);
                free(configDir);
                return EXIT_FAILURE;
            }
            ConfigLoadResult result = loadConfigFromPath(projectConfigPath, false, &arguments, &binaryName, &workingDirectory, &configDir);
            free(projectConfigPath);
            if (result == CONFIG_LOAD_ERROR) {
                freeArgList(&arguments);
                free(binaryName);
                free(workingDirectory);
                free(configDir);
                return EXIT_FAILURE;
            }
            if (result == CONFIG_LOAD_SUCCESS) {
                configLoaded = true;
            }
        }
    }

    if (!configLoaded) {
        errno = 0;
        char *defaultConfigPath = getDefaultConfigPath();
        if (!defaultConfigPath) {
            if (errno == ENOMEM) {
                fprintf(stderr, "[pscal-runner] out of memory\n");
                freeArgList(&arguments);
                free(binaryName);
                free(workingDirectory);
                free(configDir);
                return EXIT_FAILURE;
            }
        } else {
            ConfigLoadResult result = loadConfigFromPath(defaultConfigPath, false, &arguments, &binaryName, &workingDirectory, &configDir);
            free(defaultConfigPath);
            if (result == CONFIG_LOAD_ERROR) {
                freeArgList(&arguments);
                free(binaryName);
                free(workingDirectory);
                free(configDir);
                return EXIT_FAILURE;
            }
            if (result == CONFIG_LOAD_SUCCESS) {
                configLoaded = true;
            }
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
        char *expandedWorkingDir = expandEnvironmentMacros(envWorkingDir);
        if (!expandedWorkingDir) {
            fprintf(stderr, "[pscal-runner] out of memory\n");
            freeArgList(&arguments);
            free(binaryName);
            free(workingDirectory);
            free(configDir);
            return EXIT_FAILURE;
        }
        char *resolved = resolvePath(configDir, expandedWorkingDir);
        free(expandedWorkingDir);
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
