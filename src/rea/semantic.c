#include "rea/semantic.h"
#include "symbol/symbol.h"
#include "Pascal/globals.h"
#include "core/types.h"
#include "core/utils.h"
#include "rea/parser.h"
#include "ast/ast.h"
#include "ast/closure_registry.h"
#include "compiler/compiler.h"
#include "backend_ast/builtin.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#if defined(_WIN32)
#include <direct.h>
#else
#include <unistd.h>
#endif

// Forward declaration from core/utils.c
Token *newToken(TokenType type, const char *value, int line, int column);
// Needed to evaluate class constant expressions at compile time
Value evaluateCompileTimeValue(AST *node);

/* ------------------------------------------------------------------------- */
/*  Internal helpers                                                         */
/* ------------------------------------------------------------------------- */

typedef struct ClassInfo {
    char *name;               /* Class name */
    char *parent_name;        /* Parent class name (if any) */
    struct ClassInfo *parent; /* Resolved parent pointer */
    HashTable *fields;        /* Field symbol table */
    HashTable *methods;       /* Method symbol table */
} ClassInfo;

static HashTable *class_table = NULL;    /* Maps class name -> ClassInfo */
static AST *gProgramRoot = NULL;         /* Needed for declaration lookups */

typedef enum {
    REA_MODULE_EXPORT_CONST,
    REA_MODULE_EXPORT_VAR,
    REA_MODULE_EXPORT_FUNCTION,
    REA_MODULE_EXPORT_PROCEDURE,
    REA_MODULE_EXPORT_TYPE
} ReaModuleExportKind;

typedef struct ReaModuleExport {
    char *name;                /* Unqualified export name */
    ReaModuleExportKind kind;  /* Export category */
    AST *decl;                 /* Pointer to declaration node inside module AST */
} ReaModuleExport;

typedef struct ReaModuleInfo {
    char *path;                /* Source path (as imported) */
    char *directory;           /* Directory containing the source */
    char *name;                /* Declared module name */
    AST *ast;                  /* Parsed AST root (PROGRAM) */
    AST *module_node;          /* Pointer to AST_MODULE node */
    ReaModuleExport *exports;  /* Dynamic array of exports */
    int export_count;
    int export_capacity;
    bool processed;            /* Semantic analysis applied */
    bool in_progress;          /* Guards against cyclic imports */
} ReaModuleInfo;

typedef struct ReaModuleBinding {
    char *alias;               /* Accessible name within current scope */
    ReaModuleInfo *module;     /* Target module */
    bool allow_unqualified_exports;
} ReaModuleBinding;

typedef struct ReaModuleBindingList {
    ReaModuleBinding *items;
    int count;
    int capacity;
} ReaModuleBindingList;

static ReaModuleInfo **gLoadedModules = NULL;
static int gLoadedModuleCount = 0;
static int gLoadedModuleCapacity = 0;

static void freeModuleInfo(ReaModuleInfo *info) {
    if (!info) return;
    free(info->path);
    free(info->directory);
    free(info->name);
    if (info->exports) {
        for (int i = 0; i < info->export_count; i++) {
            free(info->exports[i].name);
        }
        free(info->exports);
    }
    if (info->ast) {
        freeAST(info->ast);
    }
    free(info);
}

static void clearModuleCache(void) {
    if (gLoadedModules) {
        for (int i = 0; i < gLoadedModuleCount; i++) {
            freeModuleInfo(gLoadedModules[i]);
        }
        free(gLoadedModules);
    }
    gLoadedModules = NULL;
    gLoadedModuleCount = 0;
    gLoadedModuleCapacity = 0;
}

static ReaModuleBindingList *gActiveBindings = NULL;
static char **gModuleDirStack = NULL;
static int gModuleDirDepth = 0;
static int gModuleDirCapacity = 0;
static char *reaDupString(const char *s);
static char *duplicateDirName(const char *path);
static char *tryResolveFromRepository(const char *relative, bool *out_exists);
static char *tryResolveRepoLibFromBase(const char *baseDir, const char *relative, bool *out_exists);
static char *joinPaths(const char *base, const char *relative);

static char **gEnvImportPaths = NULL;
static int gEnvImportPathCount = 0;
static int gEnvImportPathCapacity = 0;
static bool gEnvImportPathsLoaded = false;

static void clearEnvImportPaths(void) {
    if (gEnvImportPaths) {
        for (int i = 0; i < gEnvImportPathCount; i++) {
            free(gEnvImportPaths[i]);
        }
        free(gEnvImportPaths);
    }
    gEnvImportPaths = NULL;
    gEnvImportPathCount = 0;
    gEnvImportPathCapacity = 0;
    gEnvImportPathsLoaded = false;
}

#define REA_IMPORT_PATH_ENV "REA_IMPORT_PATH"
#define REA_DEFAULT_IMPORT_DIR "/usr/local/lib/rea"

static char **gGenericTypeNames = NULL;
static int gGenericTypeCount = 0;
static int gGenericTypeCapacity = 0;
static int *gGenericFrameStack = NULL;
static int gGenericFrameDepth = 0;
static int gGenericFrameCapacity = 0;

static bool ensureGenericNameCapacity(int needed) {
    if (gGenericTypeCapacity >= needed) return true;
    int newCap = gGenericTypeCapacity ? gGenericTypeCapacity * 2 : 8;
    while (newCap < needed) newCap *= 2;
    char **resized = (char **)realloc(gGenericTypeNames, (size_t)newCap * sizeof(char *));
    if (!resized) {
        fprintf(stderr, "Memory allocation failure expanding generic type table.\n");
        return false;
    }
    for (int i = gGenericTypeCapacity; i < newCap; i++) {
        resized[i] = NULL;
    }
    gGenericTypeNames = resized;
    gGenericTypeCapacity = newCap;
    return true;
}

static bool ensureGenericFrameCapacity(int needed) {
    if (gGenericFrameCapacity >= needed) return true;
    int newCap = gGenericFrameCapacity ? gGenericFrameCapacity * 2 : 8;
    while (newCap < needed) newCap *= 2;
    int *resized = (int *)realloc(gGenericFrameStack, (size_t)newCap * sizeof(int));
    if (!resized) {
        fprintf(stderr, "Memory allocation failure expanding generic frame stack.\n");
        return false;
    }
    gGenericFrameStack = resized;
    gGenericFrameCapacity = newCap;
    return true;
}

static void pushGenericFrame(void) {
    if (!ensureGenericFrameCapacity(gGenericFrameDepth + 1)) {
        EXIT_FAILURE_HANDLER();
    }
    gGenericFrameStack[gGenericFrameDepth++] = gGenericTypeCount;
}

static void popGenericFrame(void) {
    if (gGenericFrameDepth <= 0) return;
    int start = gGenericFrameStack[--gGenericFrameDepth];
    for (int i = gGenericTypeCount - 1; i >= start; i--) {
        free(gGenericTypeNames[i]);
        gGenericTypeNames[i] = NULL;
    }
    gGenericTypeCount = start;
}

static void addGenericTypeName(const char *name) {
    if (!name) return;
    if (!ensureGenericNameCapacity(gGenericTypeCount + 1)) {
        EXIT_FAILURE_HANDLER();
    }
    gGenericTypeNames[gGenericTypeCount] = strdup(name);
    if (!gGenericTypeNames[gGenericTypeCount]) {
        fprintf(stderr, "Memory allocation failure storing generic type name.\n");
        EXIT_FAILURE_HANDLER();
    }
    gGenericTypeCount++;
}

static bool isGenericTypeName(const char *name) {
    if (!name) return false;
    for (int i = gGenericTypeCount - 1; i >= 0; i--) {
        if (gGenericTypeNames[i] && strcasecmp(gGenericTypeNames[i], name) == 0) {
            return true;
        }
    }
    return false;
}

static void clearGenericTypeState(void) {
    for (int i = 0; i < gGenericTypeCount; i++) {
        free(gGenericTypeNames[i]);
    }
    free(gGenericTypeNames);
    gGenericTypeNames = NULL;
    gGenericTypeCount = 0;
    gGenericTypeCapacity = 0;
    free(gGenericFrameStack);
    gGenericFrameStack = NULL;
    gGenericFrameDepth = 0;
    gGenericFrameCapacity = 0;
}

static void freeDirStack(void) {
    if (!gModuleDirStack) return;
    for (int i = 0; i < gModuleDirDepth; i++) {
        free(gModuleDirStack[i]);
    }
    free(gModuleDirStack);
    gModuleDirStack = NULL;
    gModuleDirDepth = 0;
    gModuleDirCapacity = 0;
}

static bool ensureDirStackCapacity(int needed) {
    if (gModuleDirCapacity >= needed) return true;
    int newCap = gModuleDirCapacity ? gModuleDirCapacity * 2 : 8;
    while (newCap < needed) newCap *= 2;
    char **resized = (char **)realloc(gModuleDirStack, (size_t)newCap * sizeof(char *));
    if (!resized) return false;
    for (int i = gModuleDirCapacity; i < newCap; i++) {
        resized[i] = NULL;
    }
    gModuleDirStack = resized;
    gModuleDirCapacity = newCap;
    return true;
}

static bool pushModuleDir(const char *dir) {
    if (!ensureDirStackCapacity(gModuleDirDepth + 1)) {
        fprintf(stderr, "Memory allocation failure expanding module directory stack.\n");
        return false;
    }
    gModuleDirStack[gModuleDirDepth++] = dir ? reaDupString(dir) : NULL;
    return true;
}

static bool ensureEnvImportPathCapacity(int needed) {
    if (gEnvImportPathCapacity >= needed) {
        return true;
    }
    int newCap = gEnvImportPathCapacity ? gEnvImportPathCapacity * 2 : 4;
    while (newCap < needed) {
        newCap *= 2;
    }
    char **resized = (char **)realloc(gEnvImportPaths, (size_t)newCap * sizeof(char *));
    if (!resized) {
        fprintf(stderr, "Memory allocation failure expanding import search path list.\n");
        return false;
    }
    for (int i = gEnvImportPathCapacity; i < newCap; i++) {
        resized[i] = NULL;
    }
    gEnvImportPaths = resized;
    gEnvImportPathCapacity = newCap;
    return true;
}

static void appendEnvImportPath(const char *path) {
    if (!path || !*path) {
        return;
    }
    if (!ensureEnvImportPathCapacity(gEnvImportPathCount + 1)) {
        EXIT_FAILURE_HANDLER();
    }
    gEnvImportPaths[gEnvImportPathCount] = reaDupString(path);
    gEnvImportPathCount++;
}

static void loadEnvImportPaths(void) {
    if (gEnvImportPathsLoaded) {
        return;
    }
    gEnvImportPathsLoaded = true;

    const char *raw = getenv(REA_IMPORT_PATH_ENV);
    if (!raw || !*raw) {
        return;
    }

    char *cursor = reaDupString(raw);
    if (!cursor) {
        return;
    }

#if defined(_WIN32)
    const char *delims = ";";
#else
    const char *delims = ":;";
#endif

    char *iter = cursor;
    while (*iter) {
        while (*iter == ' ' || *iter == '\t') {
            iter++;
        }
        char *start = iter;
        while (*iter && strchr(delims, *iter) == NULL) {
            iter++;
        }
        char saved = *iter;
        if (*iter) {
            *iter = '\0';
            iter++;
        }

        char *end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        *end = '\0';

        if (*start) {
            appendEnvImportPath(start);
        }

        if (!saved) {
            break;
        }
    }

    free(cursor);
}

static char *tryResolveFromDirectory(const char *dir, const char *relative, bool *out_exists) {
    if (!dir || !*dir || !relative || !*relative) {
        return NULL;
    }
    char *candidate = joinPaths(dir, relative);
    if (!candidate) {
        return NULL;
    }
    FILE *fp = fopen(candidate, "rb");
    if (fp) {
        if (out_exists) {
            *out_exists = true;
        }
        fclose(fp);
        return candidate;
    }
    free(candidate);
    return NULL;
}

static char *tryResolveRepoLibFromBase(const char *baseDir, const char *relative, bool *out_exists) {
    if (!baseDir || !*baseDir || !relative || !*relative) {
        return NULL;
    }

    char *cursor = reaDupString(baseDir);
    if (!cursor) {
        return NULL;
    }

    while (cursor && *cursor) {
        char *libDir = joinPaths(cursor, "lib/rea");
        if (libDir) {
            char *resolved = tryResolveFromDirectory(libDir, relative, out_exists);
            free(libDir);
            if (resolved) {
                free(cursor);
                return resolved;
            }
        }

        char *parent = duplicateDirName(cursor);
        if (!parent) {
            break;
        }
        if (strcmp(parent, cursor) == 0) {
            free(parent);
            break;
        }
        free(cursor);
        cursor = parent;
    }

    free(cursor);
    return NULL;
}

static char *tryResolveFromRepository(const char *relative, bool *out_exists) {
    if (!relative || !*relative) {
        return NULL;
    }

    for (int idx = gModuleDirDepth - 1; idx >= 0; idx--) {
        const char *base = gModuleDirStack ? gModuleDirStack[idx] : NULL;
        char *resolved = tryResolveRepoLibFromBase(base, relative, out_exists);
        if (resolved) {
            return resolved;
        }
    }

#if defined(_WIN32)
    char *cwd = _getcwd(NULL, 0);
#else
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
#if defined(PATH_MAX)
        char buffer[PATH_MAX];
        if (getcwd(buffer, sizeof(buffer))) {
            cwd = reaDupString(buffer);
        }
#endif
    }
#endif
    if (cwd) {
        char *resolved = tryResolveRepoLibFromBase(cwd, relative, out_exists);
        if (resolved) {
            free(cwd);
            return resolved;
        }
        free(cwd);
    }

    char *resolved = tryResolveRepoLibFromBase(".", relative, out_exists);
    if (resolved) {
        return resolved;
    }

    return NULL;
}

static void popModuleDir(void) {
    if (gModuleDirDepth <= 0) return;
    gModuleDirDepth--;
    free(gModuleDirStack[gModuleDirDepth]);
    gModuleDirStack[gModuleDirDepth] = NULL;
}

static char *reaDupString(const char *s) {
    if (!s) return NULL;
    char *copy = strdup(s);
    if (!copy) {
        fprintf(stderr, "Memory allocation failure duplicating string.\n");
        EXIT_FAILURE_HANDLER();
    }
    return copy;
}

static bool pathIsAbsolute(const char *path) {
    if (!path || !*path) return false;
    if (path[0] == '/' || path[0] == '\\') return true;
#if defined(_WIN32)
    if (isalpha((unsigned char)path[0]) && path[1] == ':') return true;
#endif
    return false;
}

static char *joinPaths(const char *base, const char *relative) {
    if (!relative || !*relative) return base ? reaDupString(base) : NULL;
    if (!base || !*base) return reaDupString(relative);
    size_t base_len = strlen(base);
    bool need_sep = base_len > 0 && base[base_len - 1] != '/' && base[base_len - 1] != '\\';
    size_t rel_len = strlen(relative);
    size_t total = base_len + (need_sep ? 1 : 0) + rel_len + 1;
    char *result = (char *)malloc(total);
    if (!result) {
        fprintf(stderr, "Memory allocation failure joining paths.\n");
        return NULL;
    }
    strcpy(result, base);
    if (need_sep) {
        result[base_len] = '/';
        memcpy(result + base_len + 1, relative, rel_len + 1);
    } else {
        memcpy(result + base_len, relative, rel_len + 1);
    }
    return result;
}

static char *duplicateDirName(const char *path) {
    if (!path) return NULL;
    const char *slash = strrchr(path, '/');
#if defined(_WIN32)
    const char *backslash = strrchr(path, '\\');
    if (!slash || (backslash && backslash > slash)) {
        slash = backslash;
    }
#endif
    if (!slash) {
        return reaDupString(".");
    }
    size_t len = (size_t)(slash - path);
    if (len == 0) len = 1; // Preserve root "" -> "/"
    char *dir = (char *)malloc(len + 1);
    if (!dir) {
        fprintf(stderr, "Memory allocation failure duplicating directory name.\n");
        return NULL;
    }
    memcpy(dir, path, len);
    dir[len] = '\0';
    return dir;
}

void reaSemanticSetSourcePath(const char *path) {
    freeDirStack();
    if (!path) {
        pushModuleDir(".");
        return;
    }
    char *dir = duplicateDirName(path);
    if (!dir) {
        pushModuleDir(".");
        return;
    }
    pushModuleDir(dir);
    free(dir);
}

static void freeBindingList(ReaModuleBindingList *list) {
    if (!list) return;
    for (int i = 0; i < list->count; i++) {
        free(list->items[i].alias);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool ensureBindingCapacity(ReaModuleBindingList *list, int needed) {
    if (!list) return false;
    if (list->capacity >= needed) return true;
    int newCap = list->capacity ? list->capacity * 2 : 4;
    while (newCap < needed) newCap *= 2;
    ReaModuleBinding *resized = (ReaModuleBinding *)realloc(list->items, (size_t)newCap * sizeof(ReaModuleBinding));
    if (!resized) {
        fprintf(stderr, "Memory allocation failure expanding module binding list.\n");
        return false;
    }
    list->items = resized;
    list->capacity = newCap;
    return true;
}

static ReaModuleBinding *findBindingInList(const ReaModuleBindingList *list, const char *alias) {
    if (!list || !alias) return NULL;
    for (int i = 0; i < list->count; i++) {
        if (strcasecmp(list->items[i].alias, alias) == 0) {
            return &list->items[i];
        }
    }
    return NULL;
}

static bool addBinding(ReaModuleBindingList *list, const char *alias, ReaModuleInfo *module, int line, bool allow_unqualified_exports) {
    if (!list || !alias || !module) return false;
    ReaModuleBinding *existing = findBindingInList(list, alias);
    if (existing) {
        if (existing->module != module) {
            fprintf(stderr, "L%d: duplicate module alias '%s'.\n", line, alias);
            pascal_semantic_error_count++;
            return false;
        }
        if (allow_unqualified_exports) {
            existing->allow_unqualified_exports = true;
        }
        return true; // duplicate binding to same module; ignore
    }
    if (!ensureBindingCapacity(list, list->count + 1)) {
        EXIT_FAILURE_HANDLER();
    }
    list->items[list->count].alias = reaDupString(alias);
    list->items[list->count].module = module;
    list->items[list->count].allow_unqualified_exports = allow_unqualified_exports;
    list->count++;
    return true;
}

static ReaModuleBinding *findActiveBinding(const char *name) {
    if (!gActiveBindings) return NULL;
    return findBindingInList(gActiveBindings, name);
}

static char *tryResolveRelativePath(const char *relative, bool *out_exists) {
    if (!relative || !*relative) return NULL;

    for (int idx = gModuleDirDepth - 1; idx >= 0; idx--) {
        const char *base = gModuleDirStack ? gModuleDirStack[idx] : NULL;
        if (!base) continue;
        char *candidate = joinPaths(base, relative);
        if (!candidate) continue;
        FILE *fp = fopen(candidate, "rb");
        if (fp) {
            if (out_exists) *out_exists = true;
            fclose(fp);
            return candidate;
        }
        free(candidate);
    }

    char *candidate = reaDupString(relative);
    if (!candidate) return NULL;
    FILE *fp = fopen(candidate, "rb");
    if (fp) {
        if (out_exists) *out_exists = true;
        fclose(fp);
        return candidate;
    }
    free(candidate);

    loadEnvImportPaths();
    for (int i = 0; i < gEnvImportPathCount; i++) {
        char *resolved = tryResolveFromDirectory(gEnvImportPaths[i], relative, out_exists);
        if (resolved) {
            return resolved;
        }
    }

    char *repoResolved = tryResolveFromRepository(relative, out_exists);
    if (repoResolved) {
        return repoResolved;
    }

    char *defaultResolved = tryResolveFromDirectory(REA_DEFAULT_IMPORT_DIR, relative, out_exists);
    if (defaultResolved) {
        return defaultResolved;
    }

    return NULL;
}

static char *resolveAlternateSupportPath(const char *path, bool *out_exists) {
    if (!path) return NULL;
    const char *supportMarker = strstr(path, "__support");
    if (!supportMarker) return NULL;
    size_t prefixLen = (size_t)(supportMarker - path);
    if (prefixLen == 0) return NULL;

    const char *suffix = supportMarker;
    char *prefix = (char *)malloc(prefixLen + 1);
    if (!prefix) {
        return NULL;
    }
    memcpy(prefix, path, prefixLen);
    prefix[prefixLen] = '\0';

    while (true) {
        char *underscore = strrchr(prefix, '_');
        if (!underscore) break;
        *underscore = '\0';
        if (*prefix == '\0') break;
        size_t newLen = strlen(prefix) + strlen(suffix) + 1;
        char *candidateRelative = (char *)malloc(newLen);
        if (!candidateRelative) {
            free(prefix);
            return NULL;
        }
        strcpy(candidateRelative, prefix);
        strcat(candidateRelative, suffix);
        char *resolved = tryResolveRelativePath(candidateRelative, out_exists);
        free(candidateRelative);
        if (resolved) {
            free(prefix);
            return resolved;
        }
    }

    free(prefix);
    return NULL;
}

static char *resolveModulePath(const char *path, bool *out_exists) {
    if (out_exists) *out_exists = false;
    if (!path || !*path) return NULL;

    if (pathIsAbsolute(path)) {
        FILE *fp = fopen(path, "rb");
        if (fp) {
            if (out_exists) *out_exists = true;
            fclose(fp);
        }
        return reaDupString(path);
    }

    char *resolved = tryResolveRelativePath(path, out_exists);
    if (resolved) {
        return resolved;
    }

    resolved = resolveAlternateSupportPath(path, out_exists);
    if (resolved) {
        return resolved;
    }

    return reaDupString(path);
}

static ReaModuleInfo *findModuleByPath(const char *path) {
    if (!path) return NULL;
    for (int i = 0; i < gLoadedModuleCount; i++) {
        ReaModuleInfo *info = gLoadedModules[i];
        if (!info || !info->path) continue;
        if (strcasecmp(info->path, path) == 0) {
            return info;
        }
    }
    return NULL;
}

static ReaModuleInfo *appendModuleInfo(void) {
    if (gLoadedModuleCount >= gLoadedModuleCapacity) {
        int newCap = gLoadedModuleCapacity ? gLoadedModuleCapacity * 2 : 4;
        ReaModuleInfo **resized = (ReaModuleInfo **)realloc(gLoadedModules, (size_t)newCap * sizeof(ReaModuleInfo *));
        if (!resized) {
            fprintf(stderr, "Memory allocation failure expanding module registry.\n");
            EXIT_FAILURE_HANDLER();
        }
        for (int i = gLoadedModuleCapacity; i < newCap; i++) {
            resized[i] = NULL;
        }
        gLoadedModules = resized;
        gLoadedModuleCapacity = newCap;
    }
    ReaModuleInfo *info = (ReaModuleInfo *)calloc(1, sizeof(ReaModuleInfo));
    if (!info) {
        fprintf(stderr, "Memory allocation failure creating module record.\n");
        EXIT_FAILURE_HANDLER();
    }
    gLoadedModules[gLoadedModuleCount++] = info;
    return info;
}

static AST *getDeclsCompound(AST *node) {
    if (!node) return NULL;
    AST *block = NULL;
    if (node->type == AST_PROGRAM || node->type == AST_MODULE) {
        block = node->right;
    } else if (node->type == AST_BLOCK) {
        block = node;
    }
    if (!block || block->child_count <= 0 || !block->children) {
        return NULL;
    }
    AST *decls = block->children[0];
    if (decls && decls->type == AST_COMPOUND) {
        return decls;
    }
    return NULL;
}

static AST *findModuleNode(AST *root) {
    if (!root) return NULL;
    if (root->type == AST_MODULE) {
        return root;
    }
    AST *decls = getDeclsCompound(root);
    if (!decls) return NULL;
    for (int i = 0; i < decls->child_count; i++) {
        AST *child = decls->children[i];
        if (child && child->type == AST_MODULE) {
            return child;
        }
    }
    return NULL;
}

static char *readFileContents(const char *path, size_t *outLen) {
    if (outLen) *outLen = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        fprintf(stderr, "Error: unable to open module '%s'.\n", path);
        pascal_semantic_error_count++;
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        fprintf(stderr, "Error: unable to seek module '%s'.\n", path);
        pascal_semantic_error_count++;
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0) {
        fclose(fp);
        fprintf(stderr, "Error: unable to determine size of module '%s'.\n", path);
        pascal_semantic_error_count++;
        return NULL;
    }
    rewind(fp);
    char *buffer = (char*)malloc((size_t)len + 1);
    if (!buffer) {
        fclose(fp);
        fprintf(stderr, "Memory allocation failure loading module '%s'.\n", path);
        EXIT_FAILURE_HANDLER();
    }
    size_t readCount = fread(buffer, 1, (size_t)len, fp);
    fclose(fp);
    if (readCount != (size_t)len) {
        free(buffer);
        fprintf(stderr, "Error: unable to read module '%s'.\n", path);
        pascal_semantic_error_count++;
        return NULL;
    }
    buffer[len] = '\0';
    if (outLen) *outLen = (size_t)len;
    return buffer;
}

static bool ensureModuleExportCapacity(ReaModuleInfo *module, int needed) {
    if (!module) return false;
    if (module->export_capacity >= needed) return true;
    int newCap = module->export_capacity ? module->export_capacity * 2 : 4;
    while (newCap < needed) newCap *= 2;
    ReaModuleExport *resized = (ReaModuleExport *)realloc(module->exports, (size_t)newCap * sizeof(ReaModuleExport));
    if (!resized) {
        fprintf(stderr, "Memory allocation failure expanding export table for module '%s'.\n", module->name ? module->name : "(unknown)");
        EXIT_FAILURE_HANDLER();
    }
    for (int i = module->export_capacity; i < newCap; i++) {
        memset(&resized[i], 0, sizeof(ReaModuleExport));
    }
    module->exports = resized;
    module->export_capacity = newCap;
    return true;
}

static char *makeQualifiedName(const char *moduleName, const char *member) {
    if (!moduleName || !member) return NULL;
    size_t len = strlen(moduleName) + 1 + strlen(member) + 1;
    char *buf = (char*)malloc(len);
    if (!buf) {
        fprintf(stderr, "Memory allocation failure composing qualified name.\n");
        EXIT_FAILURE_HANDLER();
    }
    snprintf(buf, len, "%s.%s", moduleName, member);
    return buf;
}

static void addModuleExport(ReaModuleInfo *module, const char *name, ReaModuleExportKind kind, AST *decl) {
    if (!module || !name) return;
    if (!ensureModuleExportCapacity(module, module->export_count + 1)) {
        EXIT_FAILURE_HANDLER();
    }
    ReaModuleExport *exp = &module->exports[module->export_count++];
    exp->name = reaDupString(name);
    exp->kind = kind;
    exp->decl = decl;
}

static void collectModuleExports(ReaModuleInfo *module) {
    if (!module || !module->module_node) return;
    AST *decls = getDeclsCompound(module->module_node);
    if (!decls) return;
    for (int i = 0; i < decls->child_count; i++) {
        AST *decl = decls->children[i];
        if (!decl || !decl->is_exported) continue;
        switch (decl->type) {
            case AST_CONST_DECL:
                if (decl->token && decl->token->value) {
                    addModuleExport(module, decl->token->value, REA_MODULE_EXPORT_CONST, decl);
                }
                break;
            case AST_VAR_DECL:
                for (int j = 0; j < decl->child_count; j++) {
                    AST *varNode = decl->children[j];
                    if (!varNode || !varNode->token || !varNode->token->value) continue;
                    addModuleExport(module, varNode->token->value, REA_MODULE_EXPORT_VAR, decl);
                }
                break;
            case AST_FUNCTION_DECL:
                if (decl->token && decl->token->value) {
                    addModuleExport(module, decl->token->value, REA_MODULE_EXPORT_FUNCTION, decl);
                }
                break;
            case AST_PROCEDURE_DECL:
                if (decl->token && decl->token->value) {
                    addModuleExport(module, decl->token->value, REA_MODULE_EXPORT_PROCEDURE, decl);
                }
                break;
            case AST_TYPE_DECL:
                if (decl->token && decl->token->value) {
                    addModuleExport(module, decl->token->value, REA_MODULE_EXPORT_TYPE, decl);
                }
                break;
            default:
                break;
        }
    }
}

static int countFunctionParams(AST *decl) {
    if (!decl) return 0;
    int total = 0;
    for (int i = 0; i < decl->child_count; i++) {
        AST *paramGroup = decl->children[i];
        if (!paramGroup || paramGroup->type != AST_VAR_DECL) continue;
        int groupCount = paramGroup->child_count > 0 ? paramGroup->child_count : 1;
        total += groupCount;
    }
    if (total > 255) {
        total = 255;
    }
    if (total < 0) {
        total = 0;
    }
    return total;
}

static Symbol *ensureModuleProcedureSymbol(ReaModuleInfo *module, AST *decl) {
    if (!module || !module->name || !decl || !decl->token || !decl->token->value) {
        return NULL;
    }

    char *qualified = makeQualifiedName(module->name, decl->token->value);
    if (!qualified) {
        return NULL;
    }

    char lowerName[MAX_SYMBOL_LENGTH];
    strncpy(lowerName, qualified, sizeof(lowerName) - 1);
    lowerName[sizeof(lowerName) - 1] = '\0';
    toLowerString(lowerName);

    Symbol *sym = lookupProcedure(lowerName);
    if (!sym) {
        sym = (Symbol*)calloc(1, sizeof(Symbol));
        if (!sym) {
            fprintf(stderr, "Memory allocation failure registering module procedure '%s'.\n", qualified);
            EXIT_FAILURE_HANDLER();
        }
        sym->name = strdup(lowerName);
        if (!sym->name) {
            fprintf(stderr, "Memory allocation failure duplicating procedure name '%s'.\n", lowerName);
            free(sym);
            EXIT_FAILURE_HANDLER();
        }
        sym->is_alias = false;
        sym->is_const = false;
        sym->is_local_var = false;
        sym->is_inline = false;
        sym->next = NULL;
        sym->real_symbol = NULL;
        sym->enclosing = NULL;
        sym->value = NULL;
        sym->type_def = NULL;
        sym->is_defined = false;
        sym->bytecode_address = 0;
        sym->arity = 0;
        sym->locals_count = 0;
        sym->upvalue_count = 0;
        if (procedure_table) {
            hashTableInsert(procedure_table, sym);
        }
    }

    if (sym) {
        if (sym->type_def) {
            freeAST(sym->type_def);
        }
        sym->type_def = copyAST(decl);
        sym->type = decl ? decl->var_type : TYPE_UNKNOWN;
        sym->is_defined = false;
        sym->arity = (uint8_t)countFunctionParams(decl);
    }

    free(qualified);
    return sym;
}

static void registerModuleInternalProcedures(ReaModuleInfo *module) {
    if (!module || !module->module_node) return;
    AST *decls = getDeclsCompound(module->module_node);
    if (!decls) return;
    for (int i = 0; i < decls->child_count; i++) {
        AST *decl = decls->children[i];
        if (!decl) continue;
        if (decl->type == AST_FUNCTION_DECL || decl->type == AST_PROCEDURE_DECL) {
            ensureModuleProcedureSymbol(module, decl);
        }
    }
}

static ReaModuleExport *findModuleExport(const ReaModuleInfo *module, const char *name) {
    if (!module || !name) return NULL;
    for (int i = 0; i < module->export_count; i++) {
        ReaModuleExport *exp = &module->exports[i];
        if (exp->name && strcasecmp(exp->name, name) == 0) {
            return exp;
        }
    }
    return NULL;
}

static AST *findGlobalFunctionDecl(const char *name) {
    if (!gProgramRoot || !name) return NULL;
    AST *decls = getDeclsCompound(gProgramRoot);
    if (!decls) return NULL;
    for (int i = 0; i < decls->child_count; i++) {
        AST *child = decls->children[i];
        if (!child) continue;
        if ((child->type == AST_FUNCTION_DECL || child->type == AST_PROCEDURE_DECL) &&
            child->token && child->token->value && strcasecmp(child->token->value, name) == 0) {
            return child;
        }
    }
    return NULL;
}

static void collectImportBindings(AST *decls, ReaModuleBindingList *bindings);
static ReaModuleInfo *loadModuleRecursive(const char *path);
static void registerModuleExports(ReaModuleInfo *module);
static void analyzeProgramWithBindings(AST *root, ReaModuleBindingList *bindings);

static void collectImportBindings(AST *decls, ReaModuleBindingList *bindings) {
    if (!decls || !bindings) return;
    for (int i = 0; i < decls->child_count; i++) {
        AST *child = decls->children[i];
        if (!child || child->type != AST_USES_CLAUSE) continue;
        bool sawExplicitImports = false;
        for (int j = 0; j < child->child_count; j++) {
            AST *importNode = child->children[j];
            if (!importNode || importNode->type != AST_IMPORT || !importNode->token || !importNode->token->value) continue;
            sawExplicitImports = true;
            const char *path = importNode->token->value;
            ReaModuleInfo *mod = loadModuleRecursive(path);
            if (!mod) continue;
            const char *alias = NULL;
            if (importNode->left && importNode->left->token && importNode->left->token->value) {
                alias = importNode->left->token->value;
            }
            int line = importNode->token ? importNode->token->line : 0;
            if (alias && *alias) {
                addBinding(bindings, alias, mod, line, false);
                if (mod->name) {
                    addBinding(bindings, mod->name, mod, line, false);
                }
            } else if (mod->name) {
                addBinding(bindings, mod->name, mod, line, true);
            }
        }
        if (!sawExplicitImports && child->unit_list) {
            for (int j = 0; j < listSize(child->unit_list); j++) {
                const char *path = (const char *)listGet(child->unit_list, j);
                if (!path || !*path) continue;
                ReaModuleInfo *mod = loadModuleRecursive(path);
                if (!mod) continue;
                if (mod->name) {
                    addBinding(bindings, mod->name, mod, 0, true);
                }
            }
        }
    }
}

static ReaModuleInfo *loadModuleRecursive(const char *path) {
    if (!path) return NULL;

    bool pathExists = false;
    char *resolved = resolveModulePath(path, &pathExists);
    if (!resolved) {
        fprintf(stderr, "Error: unable to resolve module path '%s'.\n", path);
        pascal_semantic_error_count++;
        return NULL;
    }

    ReaModuleInfo *existing = findModuleByPath(resolved);
    if (existing) {
        free(resolved);
        if (existing->in_progress) {
            fprintf(stderr, "Cyclic module dependency detected involving '%s'.\n", existing->path ? existing->path : path);
            pascal_semantic_error_count++;
        }
        return existing;
    }

    size_t sourceLen = 0;
    char *source = readFileContents(resolved, &sourceLen);
    if (!source) {
        fprintf(stderr, "Error: unable to open module '%s'.\n", resolved);
        pascal_semantic_error_count++;
        free(resolved);
        return NULL;
    }

    AST *ast = parseRea(source);
    if (!ast) {
        free(source);
        free(resolved);
        return NULL;
    }

    if (!verifyASTLinks(ast, NULL)) {
        fprintf(stderr, "AST verification failed while parsing module '%s'.\n", resolved);
        pascal_semantic_error_count++;
        freeAST(ast);
        free(source);
        free(resolved);
        return NULL;
    }

    annotateTypes(ast, NULL, ast);

    AST *moduleNode = findModuleNode(ast);
    if (!moduleNode || !moduleNode->token || !moduleNode->token->value) {
        fprintf(stderr, "Module file '%s' does not contain a module declaration.\n", resolved);
        pascal_semantic_error_count++;
        freeAST(ast);
        free(source);
        free(resolved);
        return NULL;
    }

    ReaModuleInfo *info = appendModuleInfo();
    info->path = resolved;
    info->directory = duplicateDirName(resolved);
    info->name = reaDupString(moduleNode->token->value);
    info->ast = ast;
    info->module_node = moduleNode;
    info->export_count = 0;
    info->processed = false;
    info->in_progress = true;

    AST *decls = getDeclsCompound(moduleNode);
    AST *programDecls = getDeclsCompound(ast);
    ReaModuleBindingList moduleBindings = {0};

    bool pushed_dir = false;
    if (info->directory && pushModuleDir(info->directory)) {
        pushed_dir = true;
    }

    collectImportBindings(programDecls, &moduleBindings);
    collectImportBindings(decls, &moduleBindings);

    collectModuleExports(info);

    registerModuleInternalProcedures(info);

    analyzeProgramWithBindings(ast, &moduleBindings);
    if (pushed_dir) {
        popModuleDir();
    }

    info->processed = true;
    info->in_progress = false;

    registerModuleExports(info);

    freeBindingList(&moduleBindings);
    free(source);
    return info;
}

static void registerModuleExports(ReaModuleInfo *module) {
    if (!module || !module->name) return;
    for (int i = 0; i < module->export_count; i++) {
        ReaModuleExport *exp = &module->exports[i];
        if (!exp->name) continue;
        switch (exp->kind) {
            case REA_MODULE_EXPORT_FUNCTION:
            case REA_MODULE_EXPORT_PROCEDURE: {
                ensureModuleProcedureSymbol(module, exp->decl);
                break;
            }
            case REA_MODULE_EXPORT_CONST: {
                char *qualified = makeQualifiedName(module->name, exp->name);
                if (!qualified) break;
                AST *decl = exp->decl;
                if (decl && decl->left) {
                    Value v = evaluateCompileTimeValue(decl->left);
                    if (v.type != TYPE_VOID && v.type != TYPE_UNKNOWN) {
                        insertConstGlobalSymbol(qualified, v);
                        addCompilerConstant(qualified, &v, decl->token ? decl->token->line : 0);
                    }
                    freeValue(&v);
                }
                free(qualified);
                break;
            }
            case REA_MODULE_EXPORT_VAR: {
                char *qualified = makeQualifiedName(module->name, exp->name);
                if (!qualified) break;
                AST *decl = exp->decl;
                if (decl) {
                    AST *typeNode = decl->right;
                    VarType vt = decl->var_type;
                    if (vt == TYPE_UNKNOWN && typeNode) {
                        vt = typeNode->var_type;
                    }
                    insertGlobalSymbol(qualified, vt, typeNode);
                }
                free(qualified);
                break;
            }
            case REA_MODULE_EXPORT_TYPE:
            default:
                break;
        }
    }
}

static ReaModuleInfo *moduleFromExpression(AST *expr) {
    if (!expr) return NULL;
    if (expr->type == AST_VARIABLE && expr->token && expr->token->value) {
        ReaModuleBinding *binding = findActiveBinding(expr->token->value);
        if (binding) return binding->module;
    }
    return NULL;
}

static void convertFieldAccessToVariable(AST *node, const char *qualifiedName, VarType type, AST *typeDef) {
    if (!node || !qualifiedName) return;
    int line = node->token ? node->token->line : 0;
    if (node->left) {
        freeAST(node->left);
        node->left = NULL;
    }
    if (node->right) {
        freeAST(node->right);
        node->right = NULL;
    }
    if (node->extra) {
        freeAST(node->extra);
        node->extra = NULL;
    }
    if (node->token) {
        freeToken(node->token);
        node->token = NULL;
    }
    node->type = AST_VARIABLE;
    node->token = newToken(TOKEN_IDENTIFIER, qualifiedName, line, 0);
    node->var_type = type;
    if (node->type_def) {
        freeAST(node->type_def);
        node->type_def = NULL;
    }
    node->type_def = typeDef ? copyAST(typeDef) : NULL;
    node->child_count = 0;
    if (node->children) {
        free(node->children);
        node->children = NULL;
        node->child_capacity = 0;
    }
}

static bool handleModuleFieldAccess(AST *node) {
    if (!node || node->type != AST_FIELD_ACCESS) return false;
    ReaModuleInfo *module = moduleFromExpression(node->left);
    if (!module) return false;
    const char *member = (node->right && node->right->token) ? node->right->token->value : NULL;
    if (!member) return false;
    ReaModuleExport *exp = findModuleExport(module, member);
    if (!exp) {
        fprintf(stderr, "L%d: '%s' is not exported from module '%s'.\n",
                node->token ? node->token->line : (node->right && node->right->token ? node->right->token->line : 0),
                member,
                module->name ? module->name : "(unknown)");
        pascal_semantic_error_count++;
        return true;
    }

    if (exp->kind == REA_MODULE_EXPORT_CONST || exp->kind == REA_MODULE_EXPORT_VAR) {
        char *qualified = makeQualifiedName(module->name, exp->name);
        VarType vt = (exp->kind == REA_MODULE_EXPORT_CONST && exp->decl) ? exp->decl->var_type : TYPE_UNKNOWN;
        AST *typeNode = NULL;
        if (exp->kind == REA_MODULE_EXPORT_VAR && exp->decl) {
            typeNode = exp->decl->right;
            if (typeNode && vt == TYPE_UNKNOWN) vt = typeNode->var_type;
        }
        convertFieldAccessToVariable(node, qualified, vt, typeNode);
        free(qualified);
    } else {
        fprintf(stderr, "L%d: member '%s' is not a value exported from module '%s'.\n",
                node->token ? node->token->line : 0,
                member,
                module->name ? module->name : "(unknown)");
        pascal_semantic_error_count++;
    }
    return true;
}

static void adjustCallChildrenForModule(AST *call) {
    if (!call || call->child_count <= 0 || !call->children) return;
    // Shift arguments left to remove the module expression stored as first child.
    freeAST(call->children[0]);
    for (int i = 1; i < call->child_count; i++) {
        call->children[i - 1] = call->children[i];
    }
    call->child_count--;
    if (call->child_count >= 0) {
        call->children[call->child_count] = NULL;
    }
}

static bool handleModuleCall(AST *node) {
    if (!node || node->type != AST_PROCEDURE_CALL) return false;
    ReaModuleInfo *module = moduleFromExpression(node->left);
    if (!module) return false;
    const char *member = node->token ? node->token->value : NULL;
    if (!member && node->right && node->right->token) {
        member = node->right->token->value;
    }
    if (!member) return false;
    ReaModuleExport *exp = findModuleExport(module, member);
    if (!exp || (exp->kind != REA_MODULE_EXPORT_FUNCTION && exp->kind != REA_MODULE_EXPORT_PROCEDURE)) {
        fprintf(stderr, "L%d: '%s' is not exported from module '%s'.\n",
                node->token ? node->token->line : 0,
                member,
                module->name ? module->name : "(unknown)");
        pascal_semantic_error_count++;
        return true;
    }

    char *qualified = makeQualifiedName(module->name, exp->name);
    adjustCallChildrenForModule(node);
    if (node->left) {
        node->left = NULL;
    }
    int line = node->token ? node->token->line : 0;
    if (node->token) {
        freeToken(node->token);
    }
    node->token = newToken(TOKEN_IDENTIFIER, qualified, line, 0);
    node->var_type = (exp->kind == REA_MODULE_EXPORT_FUNCTION && exp->decl) ? exp->decl->var_type : TYPE_VOID;
    if (node->type_def) {
        freeAST(node->type_def);
        node->type_def = NULL;
    }
    if (exp->kind == REA_MODULE_EXPORT_FUNCTION && exp->decl) {
        node->type_def = exp->decl->right ? copyAST(exp->decl->right) : NULL;
    }
    node->i_val = 0;
    free(qualified);
    return true;
}

static int countAccessibleExports(const char *name, ReaModuleBindingList *bindings, ReaModuleInfo **firstModule, ReaModuleExport **firstExport) {
    if (firstModule) *firstModule = NULL;
    if (firstExport) *firstExport = NULL;
    if (!bindings || !name) return 0;
    int count = 0;
    ReaModuleInfo **seen = NULL;
    int seenCount = 0;
    for (int i = 0; i < bindings->count; i++) {
        if (!bindings->items[i].allow_unqualified_exports) {
            continue;
        }
        ReaModuleInfo *module = bindings->items[i].module;
        if (!module) continue;
        bool alreadySeen = false;
        for (int s = 0; s < seenCount; s++) {
            if (seen[s] == module) {
                alreadySeen = true;
                break;
            }
        }
        if (alreadySeen) continue;
        ReaModuleExport *exp = findModuleExport(module, name);
        if (exp) {
            if (count == 0) {
                if (firstModule) *firstModule = module;
                if (firstExport) *firstExport = exp;
            }
            count++;
        }
        seen = (ReaModuleInfo **)realloc(seen, (size_t)(seenCount + 1) * sizeof(ReaModuleInfo *));
        if (!seen) {
            fprintf(stderr, "Memory allocation failure tracking module export lookups.\n");
            EXIT_FAILURE_HANDLER();
        }
        seen[seenCount++] = module;
    }
    free(seen);
    return count;
}

static int gMatchTempCounter = 0;

static AST *cloneTypeForVar(VarType type, AST *typeDef, int line) {
    if (typeDef) {
        return copyAST(typeDef);
    }
    const char *name = NULL;
    switch (type) {
        case TYPE_INT64: name = "int"; break;
        case TYPE_INT32: name = "int32"; break;
        case TYPE_INT16: name = "int16"; break;
        case TYPE_INT8:  name = "int8"; break;
        case TYPE_UINT64: name = "uint64"; break;
        case TYPE_UINT32: name = "uint32"; break;
        case TYPE_UINT16: name = "uint16"; break;
        case TYPE_UINT8:  name = "uint8"; break;
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
            name = "float";
            break;
        case TYPE_FLOAT: name = "float32"; break;
        case TYPE_BOOLEAN: name = "bool"; break;
        case TYPE_STRING: name = "str"; break;
        case TYPE_CHAR:   name = "char"; break;
        case TYPE_BYTE:   name = "byte"; break;
        default:
            break;
    }
    if (name) {
        Token *tok = newToken(TOKEN_IDENTIFIER, name, line, 0);
        AST *typeNode = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(typeNode, type);
        return typeNode;
    }
    if (type == TYPE_POINTER) {
        AST *ptrNode = newASTNode(AST_POINTER_TYPE, NULL);
        setTypeAST(ptrNode, TYPE_POINTER);
        Token *tok = newToken(TOKEN_IDENTIFIER, "byte", line, 0);
        AST *base = newASTNode(AST_TYPE_IDENTIFIER, tok);
        setTypeAST(base, TYPE_BYTE);
        setRight(ptrNode, base);
        return ptrNode;
    }
    return NULL;
}

static AST *createBooleanLiteral(bool value, int line) {
    Token *tok = newToken(value ? TOKEN_TRUE : TOKEN_FALSE, value ? "true" : "false", line, 0);
    AST *node = newASTNode(AST_BOOLEAN, tok);
    node->i_val = value ? 1 : 0;
    setTypeAST(node, TYPE_BOOLEAN);
    return node;
}

static AST *createNumberLiteral(long long value, VarType type, int line) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", value);
    Token *tok = newToken(TOKEN_INTEGER_CONST, buf, line, 0);
    AST *node = newASTNode(AST_NUMBER, tok);
    node->i_val = (int)value;
    setTypeAST(node, type);
    return node;
}

static AST *createVarRef(const char *name, VarType type, AST *typeDef, int line) {
    Token *tok = newToken(TOKEN_IDENTIFIER, name, line, 0);
    AST *var = newASTNode(AST_VARIABLE, tok);
    setTypeAST(var, type);
    if (typeDef) {
        var->type_def = copyAST(typeDef);
    }
    return var;
}

static AST *createAssignment(AST *lhs, AST *rhs, int line) {
    Token *tok = newToken(TOKEN_ASSIGN, ":=", line, 0);
    AST *assign = newASTNode(AST_ASSIGN, tok);
    setLeft(assign, lhs);
    setRight(assign, rhs);
    setTypeAST(assign, lhs ? lhs->var_type : TYPE_VOID);
    return assign;
}

static void appendStatementsFromBlock(AST *target, AST *block) {
    if (!target || !block) return;
    if (block->type == AST_COMPOUND) {
        for (int i = 0; i < block->child_count; i++) {
            AST *child = block->children[i];
            if (!child) continue;
            block->children[i] = NULL;
            addChild(target, child);
        }
        block->child_count = 0;
        freeAST(block);
    } else {
        addChild(target, block);
    }
}

static AST *desugarMatchNode(AST *match);
static AST *desugarTryNode(AST *node, VarType currentFunctionType);
static AST *desugarThrowNode(AST *node, VarType currentFunctionType);
static AST *desugarNode(AST *node, VarType currentFunctionType);
static void ensureExceptionGlobals(AST *root);

static AST *desugarMatchNode(AST *match) {
    if (!match) return NULL;
    AST *expr = match->left;
    match->left = NULL;
    int line = expr && expr->token ? expr->token->line : 0;

    char valueName[64];
    char handledName[64];
    snprintf(valueName, sizeof(valueName), "__rea_match_val_%d", gMatchTempCounter);
    snprintf(handledName, sizeof(handledName), "__rea_match_handled_%d", gMatchTempCounter);
    gMatchTempCounter++;

    VarType valueType = expr ? expr->var_type : TYPE_INT64;
    AST *valueTypeNode = cloneTypeForVar(valueType, expr ? expr->type_def : NULL, line);
    if (!valueTypeNode) {
        valueTypeNode = cloneTypeForVar(TYPE_INT64, NULL, line);
        valueType = TYPE_INT64;
    }

    Token *valueTok = newToken(TOKEN_IDENTIFIER, valueName, line, 0);
    AST *valueVar = newASTNode(AST_VARIABLE, valueTok);
    setTypeAST(valueVar, valueTypeNode ? valueTypeNode->var_type : valueType);
    if (valueTypeNode) {
        valueVar->type_def = copyAST(valueTypeNode);
    }
    AST *valueDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(valueDecl, valueVar);
    setRight(valueDecl, valueTypeNode);
    setTypeAST(valueDecl, valueVar->var_type);
    setLeft(valueDecl, expr);

    Token *handledTok = newToken(TOKEN_IDENTIFIER, handledName, line, 0);
    AST *handledVar = newASTNode(AST_VARIABLE, handledTok);
    setTypeAST(handledVar, TYPE_BOOLEAN);
    AST *handledTypeNode = cloneTypeForVar(TYPE_BOOLEAN, NULL, line);
    AST *handledDecl = newASTNode(AST_VAR_DECL, NULL);
    addChild(handledDecl, handledVar);
    setRight(handledDecl, handledTypeNode);
    setTypeAST(handledDecl, TYPE_BOOLEAN);
    setLeft(handledDecl, createBooleanLiteral(false, line));

    AST *result = newASTNode(AST_COMPOUND, NULL);
    addChild(result, valueDecl);
    addChild(result, handledDecl);

    for (int i = 0; i < match->child_count; i++) {
        AST *branch = match->children[i];
        if (!branch) continue;
        AST *pattern = branch->left;
        AST *guard = branch->extra;
        AST *body = branch->right;
        branch->left = NULL;
        branch->extra = NULL;
        branch->right = NULL;

        int branchLine = pattern && pattern->token ? pattern->token->line : line;
        AST *patternBlock = newASTNode(AST_COMPOUND, NULL);

        bool bindsName = pattern && pattern->type == AST_PATTERN_BINDING;
        if (bindsName) {
            Token *bindingTok = pattern->token;
            pattern->token = NULL;
            AST *bindingVar = newASTNode(AST_VARIABLE, bindingTok);
            setTypeAST(bindingVar, valueVar->var_type);
            AST *bindingTypeNode = cloneTypeForVar(valueVar->var_type, valueVar->type_def, bindingTok ? bindingTok->line : branchLine);
            AST *bindingDecl = newASTNode(AST_VAR_DECL, NULL);
            addChild(bindingDecl, bindingVar);
            setRight(bindingDecl, bindingTypeNode);
            setTypeAST(bindingDecl, valueVar->var_type);
            AST *matchRef = createVarRef(valueName, valueVar->var_type, valueVar->type_def, bindingTok ? bindingTok->line : branchLine);
            setLeft(bindingDecl, matchRef);
            addChild(patternBlock, bindingDecl);
            freeAST(pattern);
            pattern = NULL;
        }

        AST *condition = NULL;
        if (bindsName) {
            condition = createBooleanLiteral(true, branchLine);
        } else if (pattern) {
            int condLine = pattern->token ? pattern->token->line : branchLine;
            Token *eqTok = newToken(TOKEN_EQUAL, "=", condLine, 0);
            AST *matchRef = createVarRef(valueName, valueVar->var_type, valueVar->type_def, condLine);
            AST *cond = newASTNode(AST_BINARY_OP, eqTok);
            setLeft(cond, matchRef);
            setRight(cond, pattern);
            setTypeAST(cond, TYPE_BOOLEAN);
            condition = cond;
        } else {
            condition = createBooleanLiteral(true, branchLine);
        }

        AST *handledAssign = createAssignment(createVarRef(handledName, TYPE_BOOLEAN, NULL, branchLine),
                                              createBooleanLiteral(true, branchLine),
                                              branchLine);

        if (guard) {
            AST *guardBlock = newASTNode(AST_COMPOUND, NULL);
            addChild(guardBlock, handledAssign);
            appendStatementsFromBlock(guardBlock, body);
            AST *guardIf = newASTNode(AST_IF, NULL);
            setLeft(guardIf, guard);
            setRight(guardIf, guardBlock);
            addChild(patternBlock, guardIf);
        } else {
            addChild(patternBlock, handledAssign);
            appendStatementsFromBlock(patternBlock, body);
        }

        Token *notTok = newToken(TOKEN_NOT, "not", branchLine, 0);
        AST *notHandled = newASTNode(AST_UNARY_OP, notTok);
        setLeft(notHandled, createVarRef(handledName, TYPE_BOOLEAN, NULL, branchLine));
        setTypeAST(notHandled, TYPE_BOOLEAN);

        AST *patternIf = newASTNode(AST_IF, NULL);
        setLeft(patternIf, condition);
        setRight(patternIf, patternBlock);

        AST *outerBlock = newASTNode(AST_COMPOUND, NULL);
        addChild(outerBlock, patternIf);

        AST *outerIf = newASTNode(AST_IF, NULL);
        setLeft(outerIf, notHandled);
        setRight(outerIf, outerBlock);
        addChild(result, outerIf);

        freeAST(branch);
    }

    AST *defaultBlock = match->extra;
    match->extra = NULL;
    if (defaultBlock) {
        Token *notTok = newToken(TOKEN_NOT, "not", line, 0);
        AST *notHandled = newASTNode(AST_UNARY_OP, notTok);
        setLeft(notHandled, createVarRef(handledName, TYPE_BOOLEAN, NULL, line));
        setTypeAST(notHandled, TYPE_BOOLEAN);
        AST *defaultBody = newASTNode(AST_COMPOUND, NULL);
        appendStatementsFromBlock(defaultBody, defaultBlock);
        AST *defaultIf = newASTNode(AST_IF, NULL);
        setLeft(defaultIf, notHandled);
        setRight(defaultIf, defaultBody);
        addChild(result, defaultIf);
    }

    if (match->children) {
        free(match->children);
    }
    match->child_count = 0;
    match->child_capacity = 0;
    free(match);
    return result;
}

static AST *desugarTryNode(AST *node, VarType currentFunctionType) {
    if (!node) return NULL;
    AST *tryBlock = node->left;
    AST *catchNode = node->right;
    node->left = NULL;
    node->right = NULL;

    AST *result = newASTNode(AST_COMPOUND, NULL);

    AST *pendingReset = createAssignment(createVarRef("__rea_exc_pending", TYPE_BOOLEAN, NULL, 0),
                                         createBooleanLiteral(false, 0),
                                         0);
    addChild(result, pendingReset);

    if (tryBlock) {
        appendStatementsFromBlock(result, tryBlock);
    }

    AST *catchDecl = NULL;
    AST *catchBody = NULL;
    if (catchNode) {
        catchDecl = catchNode->left;
        catchBody = catchNode->right;
        catchNode->left = NULL;
        catchNode->right = NULL;
        freeAST(catchNode);
    }

    AST *ifBody = newASTNode(AST_COMPOUND, NULL);
    if (catchDecl) {
        AST *valueRef = createVarRef("__rea_exc_value", TYPE_INT64, NULL, 0);
        setLeft(catchDecl, valueRef);
        addChild(ifBody, catchDecl);
    }
    AST *clearPending = createAssignment(createVarRef("__rea_exc_pending", TYPE_BOOLEAN, NULL, 0),
                                         createBooleanLiteral(false, 0),
                                         0);
    addChild(ifBody, clearPending);
    appendStatementsFromBlock(ifBody, catchBody);

    AST *condition = createVarRef("__rea_exc_pending", TYPE_BOOLEAN, NULL, 0);
    AST *catchIf = newASTNode(AST_IF, NULL);
    setLeft(catchIf, condition);
    setRight(catchIf, ifBody);
    addChild(result, catchIf);

    free(node);
    return result;
}

static AST *desugarThrowNode(AST *node, VarType currentFunctionType) {
    if (!node) return NULL;
    int line = node->i_val;
    AST *expr = node->left;
    node->left = NULL;

    AST *result = newASTNode(AST_COMPOUND, NULL);

    AST *setPending = createAssignment(createVarRef("__rea_exc_pending", TYPE_BOOLEAN, NULL, line),
                                       createBooleanLiteral(true, line),
                                       line);
    addChild(result, setPending);

    AST *valueExpr = expr ? expr : createNumberLiteral(0, TYPE_INT64, line);
    AST *setValue = createAssignment(createVarRef("__rea_exc_value", TYPE_INT64, NULL, line),
                                     valueExpr,
                                     line);
    addChild(result, setValue);

    Token *retTok = newToken(TOKEN_RETURN, "return", line, 0);
    AST *ret = newASTNode(AST_RETURN, retTok);
    AST *retValue = NULL;
    switch (currentFunctionType) {
        case TYPE_BOOLEAN:
            retValue = createBooleanLiteral(false, line);
            break;
        case TYPE_POINTER: {
            Token *nilTok = newToken(TOKEN_NIL, "nil", line, 0);
            AST *nilNode = newASTNode(AST_NIL, nilTok);
            setTypeAST(nilNode, TYPE_POINTER);
            retValue = nilNode;
            break;
        }
        case TYPE_INT32:
        case TYPE_INT16:
        case TYPE_INT8:
        case TYPE_UINT64:
        case TYPE_UINT32:
        case TYPE_UINT16:
        case TYPE_UINT8:
        case TYPE_DOUBLE:
        case TYPE_LONG_DOUBLE:
        case TYPE_FLOAT:
        case TYPE_INT64:
            retValue = createNumberLiteral(0, currentFunctionType, line);
            break;
        default:
            retValue = NULL;
            break;
    }
    setLeft(ret, retValue);
    setTypeAST(ret, currentFunctionType);
    addChild(result, ret);

    free(node);
    return result;
}

static AST *desugarNode(AST *node, VarType currentFunctionType) {
    if (!node) return NULL;

    if (node->type == AST_FUNCTION_DECL) {
        VarType retType = node->var_type;
        if (node->left) {
            AST *newLeft = desugarNode(node->left, currentFunctionType);
            if (newLeft != node->left) setLeft(node, newLeft);
        }
        if (node->right) {
            AST *newRight = desugarNode(node->right, currentFunctionType);
            if (newRight != node->right) setRight(node, newRight);
        }
        if (node->extra) {
            AST *newBody = desugarNode(node->extra, retType);
            if (newBody != node->extra) setExtra(node, newBody);
        }
        for (int i = 0; i < node->child_count; i++) {
            if (!node->children || !node->children[i]) continue;
            AST *child = node->children[i];
            AST *newChild = desugarNode(child, currentFunctionType);
            if (newChild != child) {
                node->children[i] = newChild;
                if (newChild) newChild->parent = node;
            }
        }
        return node;
    }
    if (node->type == AST_PROCEDURE_DECL) {
        if (node->left) {
            AST *newLeft = desugarNode(node->left, currentFunctionType);
            if (newLeft != node->left) setLeft(node, newLeft);
        }
        if (node->extra) {
            AST *newExtra = desugarNode(node->extra, TYPE_VOID);
            if (newExtra != node->extra) setExtra(node, newExtra);
        }
        if (node->right) {
            AST *newRight = desugarNode(node->right, TYPE_VOID);
            if (newRight != node->right) setRight(node, newRight);
        }
        for (int i = 0; i < node->child_count; i++) {
            if (!node->children || !node->children[i]) continue;
            AST *child = node->children[i];
            AST *newChild = desugarNode(child, currentFunctionType);
            if (newChild != child) {
                node->children[i] = newChild;
                if (newChild) newChild->parent = node;
            }
        }
        return node;
    }

    if (node->left) {
        AST *newLeft = desugarNode(node->left, currentFunctionType);
        if (newLeft != node->left) setLeft(node, newLeft);
    }
    if (node->right) {
        AST *newRight = desugarNode(node->right, currentFunctionType);
        if (newRight != node->right) setRight(node, newRight);
    }
    if (node->extra) {
        AST *newExtra = desugarNode(node->extra, currentFunctionType);
        if (newExtra != node->extra) setExtra(node, newExtra);
    }
    for (int i = 0; i < node->child_count; i++) {
        if (!node->children || !node->children[i]) continue;
        AST *child = node->children[i];
        AST *newChild = desugarNode(child, currentFunctionType);
        if (newChild != child) {
            node->children[i] = newChild;
            if (newChild) newChild->parent = node;
        }
    }

    if (node->type == AST_MATCH) {
        return desugarMatchNode(node);
    }
    if (node->type == AST_TRY) {
        return desugarTryNode(node, currentFunctionType);
    }
    if (node->type == AST_THROW) {
        return desugarThrowNode(node, currentFunctionType);
    }
    return node;
}

static int declarationLine(AST *decl);

static bool astContainsExceptions(AST *node) {
    if (!node) return false;
    if (node->type == AST_TRY || node->type == AST_THROW) {
        return true;
    }
    if (astContainsExceptions(node->left)) return true;
    if (astContainsExceptions(node->right)) return true;
    if (astContainsExceptions(node->extra)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (astContainsExceptions(node->children[i])) return true;
    }
    return false;
}

static AST *findEnclosingCompound(AST *node);

static AST *findVarDeclAnywhere(AST *node, const char *ident, int referenceLine) {
    if (!node || !ident) return NULL;
    if (node->type == AST_VAR_DECL) {
        for (int j = 0; j < node->child_count; j++) {
            AST *nameNode = node->children[j];
            if (!nameNode) continue;
            if (nameNode->type == AST_VARIABLE && nameNode->token &&
                strcasecmp(nameNode->token->value, ident) == 0) {
                int declLine = declarationLine(node);
                if (referenceLine <= 0 || declLine <= 0 || declLine <= referenceLine) {
                    return node;
                }
            } else if (nameNode->type == AST_ASSIGN && nameNode->left &&
                       nameNode->left->type == AST_VARIABLE &&
                       nameNode->left->token &&
                       strcasecmp(nameNode->left->token->value, ident) == 0) {
                int declLine = declarationLine(node);
                if (referenceLine <= 0 || declLine <= 0 || declLine <= referenceLine) {
                    return node;
                }
            }
        }
    } else if (node->type == AST_CONST_DECL && node->token &&
               strcasecmp(node->token->value, ident) == 0) {
        int declLine = declarationLine(node);
        if (referenceLine <= 0 || declLine <= 0 || declLine <= referenceLine) {
            return node;
        }
    }
    AST *res = findVarDeclAnywhere(node->left, ident, referenceLine);
    if (res) return res;
    res = findVarDeclAnywhere(node->right, ident, referenceLine);
    if (res) return res;
    res = findVarDeclAnywhere(node->extra, ident, referenceLine);
    if (res) return res;
    for (int i = 0; i < node->child_count; i++) {
        res = findVarDeclAnywhere(node->children[i], ident, referenceLine);
        if (res) return res;
    }
    return NULL;
}

static bool isDeclarationCompound(AST *node) {
    if (!node || node->type != AST_COMPOUND || node->i_val != 1) return false;
    bool hasChild = false;
    for (int i = 0; i < node->child_count; i++) {
        AST *child = node->children[i];
        if (!child) continue;
        hasChild = true;
        if (child->type == AST_VAR_DECL || child->type == AST_CONST_DECL) {
            continue;
        }
        if (child->type == AST_COMPOUND && isDeclarationCompound(child)) {
            continue;
        }
        return false;
    }
    return hasChild;
}

static void flattenDeclarationCompounds(AST *node) {
    if (!node) return;
    flattenDeclarationCompounds(node->left);
    flattenDeclarationCompounds(node->right);
    flattenDeclarationCompounds(node->extra);
    for (int i = 0; i < node->child_count; i++) {
        flattenDeclarationCompounds(node->children[i]);
    }

    if (!node->children || node->child_count <= 0) {
        return;
    }

    bool hasFlattenable = false;
    for (int i = 0; i < node->child_count; i++) {
        AST *child = node->children[i];
        if (child && child->type == AST_COMPOUND && isDeclarationCompound(child)) {
            hasFlattenable = true;
            break;
        }
    }
    if (!hasFlattenable) {
        return;
    }

    int newCount = 0;
    for (int i = 0; i < node->child_count; i++) {
        AST *child = node->children[i];
        if (!child) continue;
        if (child->type == AST_COMPOUND && isDeclarationCompound(child)) {
            for (int j = 0; j < child->child_count; j++) {
                if (child->children[j]) {
                    newCount++;
                }
            }
        } else {
            newCount++;
        }
    }

    AST **flattened = (AST **)malloc((size_t)newCount * sizeof(AST *));
    if (!flattened) {
        fprintf(stderr, "Memory allocation failure while flattening declaration groups.\n");
        EXIT_FAILURE_HANDLER();
    }

    int outIndex = 0;
    for (int i = 0; i < node->child_count; i++) {
        AST *child = node->children[i];
        if (!child) continue;
        if (child->type == AST_COMPOUND && isDeclarationCompound(child)) {
            for (int j = 0; j < child->child_count; j++) {
                AST *grand = child->children[j];
                if (!grand) continue;
                child->children[j] = NULL;
                grand->parent = node;
                flattened[outIndex++] = grand;
            }
            free(child->children);
            child->children = NULL;
            child->child_count = 0;
            child->child_capacity = 0;
            child->left = child->right = child->extra = NULL;
            child->parent = NULL;
            freeAST(child);
        } else {
            flattened[outIndex++] = child;
        }
    }

    free(node->children);
    node->children = flattened;
    node->child_count = outIndex;
    node->child_capacity = outIndex;
}

static AST *findDeclInCompound(AST *node, const char *ident, int referenceLine) {
    if (!node || !ident) return NULL;
    if (node->type == AST_VAR_DECL) {
        for (int childIdx = 0; childIdx < node->child_count; childIdx++) {
            AST *varNode = node->children[childIdx];
            if (!varNode || !varNode->token || !varNode->token->value) continue;
            if (strcasecmp(varNode->token->value, ident) == 0) {
                int declLine = declarationLine(node);
                if (referenceLine <= 0 || declLine <= 0 || declLine <= referenceLine) {
                    return node;
                }
            }
        }
        return NULL;
    }
    if (node->type == AST_CONST_DECL) {
        if (node->token && node->token->value &&
            strcasecmp(node->token->value, ident) == 0) {
            int declLine = declarationLine(node);
            if (referenceLine <= 0 || declLine <= 0 || declLine <= referenceLine) {
                return node;
            }
        }
        return NULL;
    }
    if (node->type == AST_COMPOUND && isDeclarationCompound(node)) {
        for (int i = 0; i < node->child_count; i++) {
            AST *child = node->children[i];
            AST *found = findDeclInCompound(child, ident, referenceLine);
            if (found) return found;
        }
    }
    return NULL;
}

static void ensureExceptionGlobals(AST *root) {
    if (!root || !astContainsExceptions(root)) {
        return;
    }
    AST *decls = getDeclsCompound(root);
    if (!decls) return;

    bool hasPending = false;
    bool hasValue = false;
    for (int i = 0; i < decls->child_count; i++) {
        AST *child = decls->children[i];
        if (!child || child->type != AST_VAR_DECL) continue;
        for (int j = 0; j < child->child_count; j++) {
            AST *varNode = child->children[j];
            if (!varNode || !varNode->token || !varNode->token->value) continue;
            if (strcasecmp(varNode->token->value, "__rea_exc_pending") == 0) {
                hasPending = true;
            } else if (strcasecmp(varNode->token->value, "__rea_exc_value") == 0) {
                hasValue = true;
            }
        }
    }

    if (!hasPending) {
        AST *pendingDecl = newASTNode(AST_VAR_DECL, NULL);
        Token *pendingTok = newToken(TOKEN_IDENTIFIER, "__rea_exc_pending", 0, 0);
        AST *pendingVar = newASTNode(AST_VARIABLE, pendingTok);
        setTypeAST(pendingVar, TYPE_BOOLEAN);
        AST *pendingType = cloneTypeForVar(TYPE_BOOLEAN, NULL, 0);
        addChild(pendingDecl, pendingVar);
        setRight(pendingDecl, pendingType);
        setTypeAST(pendingDecl, TYPE_BOOLEAN);
        setLeft(pendingDecl, createBooleanLiteral(false, 0));
        addChild(decls, pendingDecl);
    }

    if (!hasValue) {
        AST *valueDecl = newASTNode(AST_VAR_DECL, NULL);
        Token *valueTok = newToken(TOKEN_IDENTIFIER, "__rea_exc_value", 0, 0);
        AST *valueVar = newASTNode(AST_VARIABLE, valueTok);
        setTypeAST(valueVar, TYPE_INT64);
        AST *valueType = cloneTypeForVar(TYPE_INT64, NULL, 0);
        addChild(valueDecl, valueVar);
        setRight(valueDecl, valueType);
        setTypeAST(valueDecl, TYPE_INT64);
        setLeft(valueDecl, createNumberLiteral(0, TYPE_INT64, 0));
        addChild(decls, valueDecl);
    }
}

static ClosureCaptureRegistry gClosureRegistry;
static bool gClosureRegistryInitialized = false;

static void ensureClosureRegistry(void) {
    if (!gClosureRegistryInitialized) {
        closureRegistryInit(&gClosureRegistry);
        gClosureRegistryInitialized = true;
    }
}

static void resetClosureRegistry(void) {
    ensureClosureRegistry();
    closureRegistryReset(&gClosureRegistry);
}

static void destroyClosureRegistry(void) {
    if (!gClosureRegistryInitialized) {
        return;
    }
    closureRegistryDestroy(&gClosureRegistry);
    gClosureRegistryInitialized = false;
}

static void recordClosureCapture(AST *func, bool captures) {
    if (!func) {
        return;
    }
    ensureClosureRegistry();
    closureRegistryRecord(&gClosureRegistry, func, captures, NULL, 0, false);
}

static bool closureCapturesOuterScope(AST *func) {
    if (!gClosureRegistryInitialized || !func) {
        return false;
    }
    return closureRegistryCaptures(&gClosureRegistry, func);
}

static AST *findEnclosingFunction(AST *node) {
    if (!node) return NULL;
    AST *curr = node->parent;
    while (curr) {
        if (curr->type == AST_FUNCTION_DECL || curr->type == AST_PROCEDURE_DECL) {
            return curr;
        }
        curr = curr->parent;
    }
    return NULL;
}

static AST *getFunctionBody(AST *func) {
    if (!func) return NULL;
    if (func->type == AST_FUNCTION_DECL) {
        return func->extra;
    }
    if (func->type == AST_PROCEDURE_DECL) {
        return func->right;
    }
    return NULL;
}

static AST *findEnclosingCompound(AST *node) {
    while (node && node->type != AST_COMPOUND) {
        node = node->parent;
    }
    return node;
}

static int declarationLine(AST *decl) {
    if (!decl) return 0;
    if (decl->token) {
        return decl->token->line;
    }
    if (decl->child_count > 0 && decl->children) {
        for (int i = 0; i < decl->child_count; i++) {
            AST *child = decl->children[i];
            if (!child) continue;
            if (child->token) return child->token->line;
            if (child->left && child->left->token) return child->left->token->line;
            if (child->right && child->right->token) return child->right->token->line;
        }
    }
    if (decl->left && decl->left->token) return decl->left->token->line;
    if (decl->right && decl->right->token) return decl->right->token->line;
    return 0;
}

static bool functionCapturesOuterVisitor(AST *node, AST *func) {
    if (!node || !func) return false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        return false; /* Nested function handled separately */
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        const char *name = node->token->value;
        if (strcasecmp(name, "myself") != 0 && strcasecmp(name, "my") != 0) {
            AST *decl = findStaticDeclarationInAST(name, node, gProgramRoot);
            if (decl && (decl->type == AST_VAR_DECL || decl->type == AST_CONST_DECL)) {
                AST *owner = findEnclosingFunction(decl);
                if (owner && owner != func) {
                    return true;
                }
            }
        }
    }

    if (node->left && functionCapturesOuterVisitor(node->left, func)) return true;
    if (node->right && functionCapturesOuterVisitor(node->right, func)) return true;
    if (node->extra && functionCapturesOuterVisitor(node->extra, func)) return true;
    for (int i = 0; i < node->child_count; i++) {
        if (functionCapturesOuterVisitor(node->children[i], func)) return true;
    }
    return false;
}

static bool functionCapturesOuter(AST *func) {
    AST *body = getFunctionBody(func);
    if (!body) return false;
    return functionCapturesOuterVisitor(body, func);
}

static void analyzeClosureCaptures(AST *node) {
    if (!node) return;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        bool captures = functionCapturesOuter(node);
        recordClosureCapture(node, captures);
    }
    if (node->left) analyzeClosureCaptures(node->left);
    if (node->right) analyzeClosureCaptures(node->right);
    if (node->extra) analyzeClosureCaptures(node->extra);
    for (int i = 0; i < node->child_count; i++) {
        analyzeClosureCaptures(node->children[i]);
    }
}

static AST *findFunctionInSubtree(AST *node, const char *name) {
    if (!node || !name) return NULL;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) &&
        node->token && node->token->value &&
        strcasecmp(node->token->value, name) == 0) {
        return node;
    }
    AST *found = findFunctionInSubtree(node->left, name);
    if (found) return found;
    found = findFunctionInSubtree(node->right, name);
    if (found) return found;
    found = findFunctionInSubtree(node->extra, name);
    if (found) return found;
    for (int i = 0; i < node->child_count; i++) {
        found = findFunctionInSubtree(node->children[i], name);
        if (found) return found;
    }
    return NULL;
}

static char *lowerDup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *r = (char *)malloc(len + 1);
    if (!r) return NULL;
    for (size_t i = 0; i < len; i++) r[i] = (char)tolower((unsigned char)s[i]);
    r[len] = '\0';
    return r;
}

static void lowerCopy(const char *s, char *buf) {
    size_t i = 0;
    if (!s) { buf[0] = '\0'; return; }
    for (; s[i] && i < MAX_SYMBOL_LENGTH - 1; i++) {
        buf[i] = (char)tolower((unsigned char)s[i]);
    }
    buf[i] = '\0';
}

static HashTable *ensureProcedureTable(void) {
    if (!procedure_table) {
        procedure_table = createHashTable();
        current_procedure_table = procedure_table;
    } else if (!current_procedure_table) {
        current_procedure_table = procedure_table;
    }
    return procedure_table;
}

static void ensureReaSymbolTables(void) {
    if (!globalSymbols) {
        globalSymbols = createHashTable();
    }
    if (!constGlobalSymbols) {
        constGlobalSymbols = createHashTable();
    }
    ensureProcedureTable();
}

static HashTable *ensureClassMethods(ClassInfo *ci) {
    if (!ci) return NULL;
    if (!ci->methods) {
        ci->methods = createHashTable();
    }
    return ci->methods;
}

static ClassInfo *lookupClass(const char *name) {
    if (!class_table || !name) return NULL;
    char lower[MAX_SYMBOL_LENGTH];
    lowerCopy(name, lower);
    Symbol *sym = hashTableLookup(class_table, lower);
    if (!sym || !sym->value) return NULL;
    ClassInfo *ci = (ClassInfo *)sym->value->ptr_val;
    if (ci) {
        ensureClassMethods(ci);
    }
    return ci;
}

static AST *getFunctionParam(AST *func, int index) {
    if (!func) return NULL;
    int running = 0;
    for (int i = 0; i < func->child_count; i++) {
        AST *param = func->children[i];
        if (!param || param->type != AST_VAR_DECL) continue;
        int span = param->child_count > 0 ? param->child_count : 1;
        if (index < running + span) {
            return param;
        }
        running += span;
    }
    return NULL;
}

static bool paramIsImplicitSelf(AST *param) {
    if (!param || param->child_count <= 0) return false;
    AST *nameNode = param->children[0];
    if (!nameNode || !nameNode->token || !nameNode->token->value) return false;
    const char *name = nameNode->token->value;
    return (strcasecmp(name, "myself") == 0 || strcasecmp(name, "my") == 0);
}

static void insertClassInfo(ClassInfo *ci) {
    if (!class_table) class_table = createHashTable();
    if (!ci || !class_table || !ci->name) return;
    Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
    Value *v = (Value *)calloc(1, sizeof(Value));
    if (!sym || !v) { free(sym); free(v); return; }
    sym->name = lowerDup(ci->name);
    v->ptr_val = (Value *)ci;  /* store as pointer */
    sym->value = v;
    hashTableInsert(class_table, sym);
}

static void freeSymbolTable(HashTable *table, bool freeTypeDefs) {
    if (!table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = table->buckets[i];
        while (s) {
            Symbol *next = s->next;
            if (s->name) free(s->name);
            if (freeTypeDefs && s->type_def) freeAST(s->type_def);
            if (s->value) free(s->value);
            free(s);
            s = next;
        }
    }
    free(table);
}

static void freeClassTable(void) {
    if (!class_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            Symbol *next = s->next;
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci) {
                if (ci->fields) {
                    /* Field type_defs reference the original AST; do not free them here */
                    freeSymbolTable(ci->fields, false);
                }
                if (ci->methods) freeSymbolTable(ci->methods, false);
                free(ci->parent_name);
                free(ci->name);
                free(ci);
            }
            if (s->value) free(s->value);
            if (s->name) free(s->name);
            free(s);
            s = next;
        }
    }
    free(class_table);
    class_table = NULL;
}

/* ------------------------------------------------------------------------- */
/*  Class and method collection                                             */
/* ------------------------------------------------------------------------- */

static void collectClasses(AST *node) {
    if (!node) return;
    if (node->type == AST_TYPE_DECL && node->left && node->left->type == AST_RECORD_TYPE && node->token && node->token->value) {
        ClassInfo *ci = (ClassInfo *)calloc(1, sizeof(ClassInfo));
        if (!ci) return;
        ci->name = strdup(node->token->value);
        if (node->left->extra && node->left->extra->token && node->left->extra->token->value) {
            ci->parent_name = strdup(node->left->extra->token->value);
        }
        ci->fields = createHashTable();
        ci->methods = createHashTable();
        /* Gather fields and constants */
        for (int i = 0; i < node->left->child_count; i++) {
            AST *field = node->left->children[i];
            if (!field) continue;
            const char *fname = NULL;
            AST *ftype = NULL;
            if (field->type == AST_VAR_DECL) {
                AST *var = field->child_count > 0 ? field->children[0] : NULL;
                if (!var || !var->token || !var->token->value) continue;
                fname = var->token->value;
                ftype = field->right;
            } else if (field->type == AST_CONST_DECL) {
                if (!field->token || !field->token->value) continue;
                fname = field->token->value;
                /* For const declarations, ftype may appear on the right or, if omitted,
                   the value's node carries the type information. */
                ftype = field->right ? field->right : (field->left ? field->left : NULL);
            } else {
                continue;
            }
            char *lname = lowerDup(fname);
            if (!lname) continue;
            if (hashTableLookup(ci->fields, lname)) {
                fprintf(stderr, "Duplicate field '%s' in class '%s'\n", fname, ci->name);
                pascal_semantic_error_count++;
                free(lname);
                continue;
            }
            Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
            if (!sym) { free(lname); continue; }
            sym->name = lname;
            /*
             * Store a reference to the original AST node rather than a deep copy.
             * copyAST would duplicate pointers to tokens, and freeing the copied
             * tree would free those tokens while the original AST still needs
             * them, leading to double-free or use-after-free errors.
             */
            sym->type_def = ftype;
            if (field->type == AST_CONST_DECL) {
                sym->is_const = true;
                if (field->left) {
                    Value v = evaluateCompileTimeValue(field->left);
                    if (v.type != TYPE_VOID && v.type != TYPE_UNKNOWN) {
                        Value *vp = (Value *)calloc(1, sizeof(Value));
                        if (vp) {
                            *vp = v; /* shallow copy; safe for numeric types */
                            sym->value = vp;
                            sym->type = v.type;
                        }
                    }
                }
            }
            hashTableInsert(ci->fields, sym);
        }
        insertClassInfo(ci);
    }
    if (node->left) collectClasses(node->left);
    if (node->right) collectClasses(node->right);
    if (node->extra) collectClasses(node->extra);
    for (int i = 0; i < node->child_count; i++) {
        collectClasses(node->children[i]);
    }
}

static void ensureConstructorAliasForClass(const char *cls, Symbol *target) {
    if (!cls || !target || !procedure_table) return;

    char classLower[MAX_SYMBOL_LENGTH];
    lowerCopy(cls, classLower);

    Symbol *existing = hashTableLookup(procedure_table, classLower);
    if (existing) {
        if (existing->is_alias && existing->real_symbol == target) {
            return;
        }
        if (existing->type_def && existing->type_def != target->type_def) {
            freeAST(existing->type_def);
        }
        existing->is_alias = true;
        existing->real_symbol = target;
        existing->type = target->type;
        existing->type_def = target->type_def ? copyAST(target->type_def) : NULL;
        return;
    }

    Symbol *alias = (Symbol *)calloc(1, sizeof(Symbol));
    if (!alias) return;
    alias->name = strdup(classLower);
    if (!alias->name) {
        free(alias);
        return;
    }
    alias->is_alias = true;
    alias->real_symbol = target;
    alias->type = target->type;
    alias->type_def = target->type_def ? copyAST(target->type_def) : NULL;
    hashTableInsert(procedure_table, alias);
}

static void ensureSelfParam(AST *node, const char *cls) {
    if (!node || !cls) return;
    bool hasSelf = false;
    if (node->child_count > 0) {
        AST *param = node->children[0];
        if (param && param->type == AST_VAR_DECL) {
            AST *ptype = param->right;
            while (ptype && (ptype->type == AST_POINTER_TYPE || ptype->type == AST_ARRAY_TYPE)) {
                ptype = ptype->right;
            }
            if (ptype && ptype->type == AST_TYPE_REFERENCE && ptype->token && ptype->token->value &&
                strcasecmp(ptype->token->value, cls) == 0) {
                hasSelf = true;
            }
        }
    }
    if (hasSelf) return;

    Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", node->token ? node->token->line : 0, 0);
    Token *clsTok = newToken(TOKEN_IDENTIFIER, cls, node->token ? node->token->line : 0, 0);
    AST *typeRef = newASTNode(AST_TYPE_REFERENCE, clsTok);
    setTypeAST(typeRef, TYPE_RECORD);
    AST *ptrType = newASTNode(AST_POINTER_TYPE, NULL);
    setRight(ptrType, typeRef);
    setTypeAST(ptrType, TYPE_POINTER);
    AST *varDecl = newASTNode(AST_VAR_DECL, selfTok);
    setRight(varDecl, ptrType);
    setTypeAST(varDecl, TYPE_POINTER);

    addChild(node, NULL);
    for (int i = node->child_count - 1; i > 0; i--) {
        node->children[i] = node->children[i - 1];
        if (node->children[i]) node->children[i]->parent = node;
    }
    node->children[0] = varDecl;
    varDecl->parent = node;
}

static void collectMethods(AST *node) {
    if (!node) return;
    HashTable *procTable = ensureProcedureTable();
    (void)procTable;
    if ((node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) && node->token && node->token->value) {
        const char *fullname = node->token->value;
        const char *us = strchr(fullname, '.');
        if (us) {
            size_t cls_len = (size_t)(us - fullname);
            char *cls = (char *)malloc(cls_len + 1);
            if (cls) {
                memcpy(cls, fullname, cls_len);
                cls[cls_len] = '\0';
                const char *mname = us + 1;
                ClassInfo *ci = lookupClass(cls);
                if (!ci) {
                    fprintf(stderr, "Method '%s' defined for unknown class '%s'\n", mname, cls);
                    pascal_semantic_error_count++;
                } else {
                    HashTable *methods = ensureClassMethods(ci);
                    if (!methods) {
                        free(cls);
                        goto recurse;
                    }
                    ensureSelfParam(node, cls);
                    char *lname = lowerDup(mname);
                    if (!lname) {
                        free(cls);
                        goto recurse; /* continue traversal */
                    }
                    if (hashTableLookup(methods, lname)) {
                        fprintf(stderr, "Duplicate method '%s' in class '%s'\n", mname, cls);
                        pascal_semantic_error_count++;
                        free(lname);
                    } else {
                        Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
                        Value *v = (Value *)calloc(1, sizeof(Value));
                        if (sym && v) {
                            sym->name = lname;
                            v->ptr_val = (Value *)node;
                            sym->value = v;
                            sym->type_def = node; /* reference for signature */
                            hashTableInsert(methods, sym);
                            char lowerName[MAX_SYMBOL_LENGTH];
                            lowerCopy(fullname, lowerName);
                            Symbol *existing = lookupProcedure(lowerName);
                            if (!existing) {
                                Symbol *ps = (Symbol *)calloc(1, sizeof(Symbol));
                                if (ps) {
                                    ps->name = strdup(lowerName);
                                    ps->type_def = copyAST(node);
                                    hashTableInsert(procedure_table, ps);
                                    existing = ps;
                                }
                            } else {
                                if (existing->value && existing->type_def && existing->type_def != node) {
                                    freeAST(existing->type_def);
                                }
                                existing->type_def = copyAST(node);
                            }
                            if (existing) {
                                Value *pv = existing->value;
                                if (!pv) {
                                    pv = (Value *)calloc(1, sizeof(Value));
                                    existing->value = pv;
                                }
                                if (pv) {
                                    pv->type = TYPE_POINTER;
                                    pv->ptr_val = (Value *)node;
                                }
                                existing->real_symbol = sym;
                                if (mname && cls && strcasecmp(mname, cls) == 0) {
                                    ensureConstructorAliasForClass(cls, existing);
                                }
                            }
                        } else {
                            free(sym); free(v); free(lname);
                        }
                    }
                }
                free(cls);
            }
        } else if (node->parent && node->parent->type == AST_COMPOUND) {
            /* Handle un-mangled methods; examine first parameter for class type */
            AST *param = (node->child_count > 0) ? node->children[0] : NULL;
            if (!param) {
                /* Some parsers emit an explicit 'myself' VAR_DECL as a sibling
                 * preceding the procedure declaration instead of a child.  If we
                 * detect that pattern, adopt the VAR_DECL as the method's first
                 * parameter so method collection and later argument checks work
                 * as expected.
                 */
                AST *parent = node->parent;
                for (int i = 0; i < parent->child_count; i++) {
                    if (parent->children[i] != node) continue;
                    for (int j = i - 1; j >= 0; j--) {
                        AST *prev = parent->children[j];
                        if (!prev || prev->type != AST_VAR_DECL) continue;
                        AST *decl_var = (prev->child_count > 0) ? prev->children[0] : NULL;
                        const char *decl_name = (decl_var && decl_var->token) ? decl_var->token->value : NULL;
                        if (!decl_name || strcasecmp(decl_name, "myself") != 0) continue;

                        addChild(node, prev);
                        for (int k = j; k < parent->child_count - 1; k++) {
                            parent->children[k] = parent->children[k + 1];
                        }
                        parent->child_count--;
                        param = node->children[0];
                        break;
                    }
                    break;
                }
            }
            if (param && param->type == AST_VAR_DECL) {
                AST *ptype = param->right;
                while (ptype && (ptype->type == AST_POINTER_TYPE || ptype->type == AST_ARRAY_TYPE)) {
                    ptype = ptype->right;
                }
                if (ptype && ptype->type == AST_TYPE_REFERENCE && ptype->token && ptype->token->value) {
                    const char *cls = ptype->token->value;
                    ClassInfo *ci = lookupClass(cls);
                    if (ci) {
                        HashTable *methods = ensureClassMethods(ci);
                        if (!methods) {
                            goto recurse;
                        }
                        size_t ln = strlen(cls) + 1 + strlen(fullname) + 1;
                        char *mangled = (char *)malloc(ln);
                        if (mangled) {
                            snprintf(mangled, ln, "%s.%s", cls, fullname);
                            free(node->token->value);
                            node->token->value = mangled;
                            node->token->length = strlen(mangled);
                            fullname = node->token->value;
                        }
                        ensureSelfParam(node, cls);
                        // Assign method index for implicitly declared methods
                        int method_index = 0;
                        for (int mb = 0; mb < HASHTABLE_SIZE; mb++) {
                            for (Symbol *ms = methods->buckets[mb]; ms; ms = ms->next) {
                                method_index++;
                            }
                        }
                        node->is_virtual = true;
                        node->i_val = method_index;
                        char *lname = lowerDup(fullname + strlen(cls) + 1);
                        if (lname) {
                            if (hashTableLookup(methods, lname)) {
                                fprintf(stderr, "Duplicate method '%s' in class '%s'\n", fullname + strlen(cls) + 1, cls);
                                pascal_semantic_error_count++;
                                free(lname);
                            } else {
                                Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
                                Value *v = (Value *)calloc(1, sizeof(Value));
                                if (sym && v) {
                                    sym->name = lname;
                                    v->ptr_val = (Value *)node;
                                    sym->value = v;
                                    sym->type_def = node;
                                    hashTableInsert(methods, sym);
                                    char lowerName[MAX_SYMBOL_LENGTH];
                                    lowerCopy(fullname, lowerName);
                                    Symbol *existing = lookupProcedure(lowerName);
                                    if (!existing) {
                                        Symbol *ps = (Symbol *)calloc(1, sizeof(Symbol));
                                        if (ps) {
                                            ps->name = strdup(lowerName);
                                            ps->type_def = node;
                                            hashTableInsert(procedure_table, ps);
                                            existing = ps;
                                        }
                                    } else {
                                        existing->type_def = node;
                                    }
                                    // Ensure bare method name aliases to the mangled symbol
                                    if (existing) {
                                        Symbol *alias = lookupProcedure(lname);
                                        if (alias) {
                                            alias->is_alias = true;
                                            alias->real_symbol = existing;
                                        }
                                    }
                                } else {
                                    free(sym); free(v); free(lname);
                                }
                            }
                        }
                    }
                }
            }
        } else if (node->parent && node->parent->type == AST_RECORD_TYPE) {
            AST *typeDecl = node->parent;
            while (typeDecl && typeDecl->type != AST_TYPE_DECL) typeDecl = typeDecl->parent;
            const char *cls = (typeDecl && typeDecl->token) ? typeDecl->token->value : NULL;
            if (cls) {
                ClassInfo *ci = lookupClass(cls);
                if (ci) {
                    HashTable *methods = ensureClassMethods(ci);
                    if (!methods) {
                        goto recurse;
                    }
                    size_t ln = strlen(cls) + 1 + strlen(fullname) + 1;
                    char *mangled = (char *)malloc(ln);
                    if (mangled) {
                        snprintf(mangled, ln, "%s.%s", cls, fullname);
                        free(node->token->value);
                        node->token->value = mangled;
                        node->token->length = strlen(mangled);
                        fullname = node->token->value;
                    }
                    ensureSelfParam(node, cls);
                    char *lname = lowerDup(fullname + strlen(cls) + 1);
                    if (lname) {
                        if (hashTableLookup(methods, lname)) {
                            fprintf(stderr, "Duplicate method '%s' in class '%s'\n", fullname + strlen(cls) + 1, cls);
                            pascal_semantic_error_count++;
                            free(lname);
                        } else {
                            Symbol *sym = (Symbol *)calloc(1, sizeof(Symbol));
                            Value *v = (Value *)calloc(1, sizeof(Value));
                            if (sym && v) {
                                sym->name = lname;
                                v->ptr_val = (Value *)node;
                                sym->value = v;
                                sym->type_def = node;
                                hashTableInsert(methods, sym);
                                char lowerName[MAX_SYMBOL_LENGTH];
                                lowerCopy(fullname, lowerName);
                                Symbol *procSym = lookupProcedure(lowerName);
                                if (!procSym) {
                                    Symbol *ps = (Symbol *)calloc(1, sizeof(Symbol));
                                    if (ps) {
                                        ps->name = strdup(lowerName);
                                        ps->type_def = node;
                                        hashTableInsert(procedure_table, ps);
                                        procSym = ps;
                                    }
                                }
                                if (procSym && cls && fullname) {
                                    const char *methodName = fullname + strlen(cls) + 1;
                                    if (methodName && strcasecmp(methodName, cls) == 0) {
                                        ensureConstructorAliasForClass(cls, procSym);
                                    }
                                }
                            } else {
                                free(sym);
                                free(v);
                                free(lname);
                            }
                        }
                    }
                }
            }
        }
    }
recurse:
    if (node->left) collectMethods(node->left);
    if (node->right) collectMethods(node->right);
    if (node->extra) collectMethods(node->extra);
    for (int i = 0; i < node->child_count; i++) collectMethods(node->children[i]);
}

static void linkParents(void) {
    if (!class_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci && ci->parent_name && !ci->parent) {
                ci->parent = lookupClass(ci->parent_name);
                if (!ci->parent) {
                    fprintf(stderr, "Unknown parent class '%s' for class '%s'\n", ci->parent_name, ci->name);
                    pascal_semantic_error_count++;
                }
            }
            s = s->next;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Override checking                                                        */
/* ------------------------------------------------------------------------- */

static bool paramTypeEquals(AST *a, AST *b) {
    if (!a || !b) return a == b;
    if (a->var_type != b->var_type) return false;
    AST *at = a->right;  /* type node */
    AST *bt = b->right;
    if (at && bt && at->token && bt->token) {
        if (strcasecmp(at->token->value, bt->token->value) != 0) return false;
    }
    return true;
}

static bool signaturesMatch(AST *a, AST *b) {
    if (!a || !b) return a == b;
    if (a->var_type != b->var_type) return false;
    if (a->child_count != b->child_count) return false;
    for (int i = 0; i < a->child_count; i++) {
        if (!paramTypeEquals(a->children[i], b->children[i])) return false;
    }
    return true;
}

static void checkOverrides(void) {
    if (!class_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci && ci->parent) {
                HashTable *methods = ensureClassMethods(ci);
                if (!methods) {
                    s = s->next;
                    continue;
                }
                for (int j = 0; j < HASHTABLE_SIZE; j++) {
                    Symbol *m = methods->buckets[j];
                    while (m) {
                        ClassInfo *p = ci->parent;
                        Symbol *pm = NULL;
                        while (p && !pm) {
                            HashTable *parentMethods = ensureClassMethods(p);
                            pm = parentMethods ? hashTableLookup(parentMethods, m->name) : NULL;
                            p = p->parent;
                        }
                        if (pm) {
                            AST *childDecl = (AST *)m->type_def;
                            AST *parentDecl = (AST *)pm->type_def;
                            if (!signaturesMatch(childDecl, parentDecl)) {
                                fprintf(stderr, "Method '%s' in class '%s' does not properly override parent method\n", m->name, ci->name);
                                pascal_semantic_error_count++;
                            }
                        }
                        m = m->next;
                    }
                }
            }
            s = s->next;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Inherited method alias insertion                                        */
/* ------------------------------------------------------------------------- */

static void addInheritedMethodAliases(void) {
    if (!class_table || !procedure_table) return;

    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *s = class_table->buckets[i];
        while (s) {
            ClassInfo *ci = s->value ? (ClassInfo *)s->value->ptr_val : NULL;
            if (ci && ci->parent) {
                HashTable *childMethods = ensureClassMethods(ci);
                if (!childMethods) {
                    s = s->next;
                    continue;
                }
                ClassInfo *p = ci->parent;
                while (p) {
                    for (int j = 0; j < HASHTABLE_SIZE; j++) {
                        Symbol *m = p->methods ? p->methods->buckets[j] : NULL;
                        while (m) {
                            /* Skip if subclass defines/overrides this method */
                            if (!hashTableLookup(childMethods, m->name)) {
                                char classLower[MAX_SYMBOL_LENGTH];
                                lowerCopy(ci->name, classLower);
                                char aliasName[MAX_SYMBOL_LENGTH * 2];
                                snprintf(aliasName, sizeof(aliasName), "%s.%s", classLower, m->name);
                                if (!hashTableLookup(procedure_table, aliasName)) {
                                    /* Find parent's fully qualified symbol */
                                    char parentLower[MAX_SYMBOL_LENGTH];
                                    lowerCopy(p->name, parentLower);
                                    char targetName[MAX_SYMBOL_LENGTH * 2];
                                    snprintf(targetName, sizeof(targetName), "%s.%s", parentLower, m->name);
                                    Symbol *target = hashTableLookup(procedure_table, targetName);
                                    target = resolveSymbolAlias(target);
                                    if (target) {
                                        Symbol *alias = (Symbol *)calloc(1, sizeof(Symbol));
                                        if (alias) {
                                            alias->name = strdup(aliasName);
                                            alias->is_alias = true;
                                            alias->real_symbol = target;
                                            alias->type = target->type;
                                            alias->type_def = target->type_def ? copyAST(target->type_def) : NULL;
                                            if (alias->type_def && alias->type_def->token) {
                                                size_t ln = strlen(ci->name) + 1 + strlen(m->name) + 1;
                                                char *full = (char *)malloc(ln);
                                                if (full) {
                                                    snprintf(full, ln, "%s.%s", ci->name, m->name);
                                                    free(alias->type_def->token->value);
                                                    alias->type_def->token->value = full;
                                                    alias->type_def->token->length = (int)strlen(full);
                                                }
                                            }
                                            hashTableInsert(procedure_table, alias);
                                        }
                                    }
                                }
                            }
                            m = m->next;
                        }
                    }
                    p = p->parent;
                }
            }
            s = s->next;
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Field/method usage checks                                                */
/* ------------------------------------------------------------------------- */

static Symbol *lookupField(ClassInfo *ci, const char *name) {
    if (!ci || !name) return NULL;
    char lower[MAX_SYMBOL_LENGTH];
    lowerCopy(name, lower);
    ClassInfo *curr = ci;
    while (curr) {
        Symbol *s = hashTableLookup(curr->fields, lower);
        if (s) return s;
        curr = curr->parent;
    }
    return NULL;
}

static Symbol *lookupConstMember(ClassInfo *ci, const char *name) {
    Symbol *sym = lookupField(ci, name);
    if (sym && sym->is_const) {
        return sym;
    }
    return NULL;
}

static Symbol *lookupMethod(ClassInfo *ci, const char *name) {
    if (!ci || !name) return NULL;
    char lower[MAX_SYMBOL_LENGTH];
    lowerCopy(name, lower);
    ClassInfo *curr = ci;
    while (curr) {
        HashTable *methods = ensureClassMethods(curr);
        Symbol *s = methods ? hashTableLookup(methods, lower) : NULL;
        if (s) return s;
        curr = curr->parent;
    }
    return NULL;
}

static void refreshProcedureMethodCopies(void) {
    if (!procedure_table) return;
    for (int i = 0; i < HASHTABLE_SIZE; i++) {
        Symbol *sym = procedure_table->buckets[i];
        while (sym) {
            AST *source = NULL;
            if (sym->value && sym->value->ptr_val) {
                source = (AST*)sym->value->ptr_val;
            } else if (sym->real_symbol && sym->real_symbol->value &&
                       sym->real_symbol->value->ptr_val) {
                source = (AST*)sym->real_symbol->value->ptr_val;
            }
            if (source) {
                AST *updated = copyAST(source);
                if (updated) {
                    if (sym->value && sym->type_def && sym->type_def != source) {
                        freeAST(sym->type_def);
                    }
                    sym->type_def = updated;
                }
            }
            sym = sym->next;
        }
    }
}

static const char *resolveExprClass(AST *expr, ClassInfo *currentClass) {
    if (!expr) return NULL;
    switch (expr->type) {
    case AST_VARIABLE: {
        if (!expr->token || !expr->token->value) return NULL;
        /*
         * The current object reference can appear as the implicit
         * parameter "myself".  If semantic lookup fails, fall back to
         * the class currently being validated so that expressions like
         * `my.field` or `my.method()` resolve correctly.
         */
        if (currentClass && expr->token && expr->token->value &&
            (strcasecmp(expr->token->value, "myself") == 0 ||
             strcasecmp(expr->token->value, "my") == 0)) {
            return currentClass->name;
        }
        AST *decl = findStaticDeclarationInAST(expr->token->value, expr, gProgramRoot);
        if (!decl && currentClass) {
            Symbol *fs = lookupField(currentClass, expr->token->value);
            if (fs && fs->type_def) decl = fs->type_def;
        }
        if (decl && decl->right) {
            AST *type = decl->right;
            /* Drill through array and pointer wrappers */
            while (type && (type->type == AST_ARRAY_TYPE || type->type == AST_POINTER_TYPE)) {
                type = type->right;
            }
            if (type && type->type == AST_TYPE_REFERENCE && type->token) {
                return type->token->value;
            }
            if (type && type->token) {
                return type->token->value;
            }
        }
        return NULL;
    }
    case AST_ARRAY_ACCESS:
        return resolveExprClass(expr->left, currentClass);
    case AST_FIELD_ACCESS: {
        const char *base = resolveExprClass(expr->left, currentClass);
        if (!base) return NULL;
        ClassInfo *ci = lookupClass(base);
        if (!ci) return NULL;
        const char *fname = expr->right && expr->right->token ? expr->right->token->value : NULL;
        Symbol *fs = lookupField(ci, fname);
        if (!fs) {
            fprintf(stderr, "Unknown field '%s' on class '%s'\n", fname ? fname : "(null)", base);
            pascal_semantic_error_count++;
            return NULL;
        }
        if (fs->type_def) {
            AST *type = fs->type_def;
            while (type && (type->type == AST_ARRAY_TYPE || type->type == AST_POINTER_TYPE)) {
                type = type->right;
            }
            if (type && type->token) {
                return type->token->value;
            }
        }
        return NULL;
    }
    case AST_NEW:
        return expr->token ? expr->token->value : NULL;
    default:
        return NULL;
    }
}

static void validateNodeInternal(AST *node, ClassInfo *currentClass) {
    if (!node) return;

    ClassInfo *clsContext = currentClass;
    bool pushedGenericFrame = false;
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        AST *generics = node->left;
        if (generics && generics->type == AST_COMPOUND) {
            pushGenericFrame();
            pushedGenericFrame = true;
            for (int i = 0; i < generics->child_count; i++) {
                AST *param = generics->children[i];
                if (param && param->token && param->token->value) {
                    addGenericTypeName(param->token->value);
                }
            }
        }
    } else if (node->type == AST_TYPE_DECL) {
        AST *generics = node->extra;
        if (generics && generics->type == AST_COMPOUND) {
            pushGenericFrame();
            pushedGenericFrame = true;
            for (int i = 0; i < generics->child_count; i++) {
                AST *param = generics->children[i];
                if (param && param->token && param->token->value) {
                    addGenericTypeName(param->token->value);
                }
            }
        }
    }
    if (node->type == AST_FUNCTION_DECL || node->type == AST_PROCEDURE_DECL) {
        const char *fullname = node->token ? node->token->value : NULL;
        const char *us = fullname ? strchr(fullname, '.') : NULL;
        if (us) {
            size_t len = (size_t)(us - fullname);
            char buf[MAX_SYMBOL_LENGTH];
            if (len >= sizeof(buf)) len = sizeof(buf) - 1;
            memcpy(buf, fullname, len);
            buf[len] = '\0';
            clsContext = lookupClass(buf);
        } else {
            clsContext = NULL;
        }
    }

    if (node->type == AST_ASSIGN) {
        AST *lhs = node->left;
        if (lhs && lhs->type == AST_VARIABLE && lhs->token && lhs->token->value) {
            const char *name = lhs->token->value;
            AST *decl = findStaticDeclarationInAST(name, lhs, gProgramRoot);
            if (decl && decl->type == AST_CONST_DECL) {
                fprintf(stderr, "L%d: cannot assign to constant '%s'.\n",
                        lhs->token->line, name);
                pascal_semantic_error_count++;
            }
        }
    }

    if (node->type == AST_VARIABLE && node->token && node->token->value) {
        if (node->parent && node->parent->type == AST_VAR_DECL) {
            AST *declParent = node->parent;
            AST *recordScope = declParent->parent;
            while (recordScope && recordScope->type == AST_COMPOUND) {
                recordScope = recordScope->parent;
            }
            if (recordScope && recordScope->type == AST_RECORD_TYPE) {
                if (declParent->right) {
                    node->type_def = declParent->right;
                    node->var_type = declParent->right->var_type;
                } else {
                    node->var_type = declParent->var_type;
                }
                if (pushedGenericFrame) popGenericFrame();
                return;
            }
        }
        if (node->parent && node->parent->type == AST_FIELD_ACCESS) {
            AST *fieldAccess = node->parent;
            const char *clsName = resolveExprClass(fieldAccess->left, clsContext);
            if (clsName) {
                ClassInfo *ci = lookupClass(clsName);
                if (ci) {
                    Symbol *fs = lookupField(ci, node->token->value);
                    if (fs && fs->type_def) {
                        node->type_def = copyAST(fs->type_def);
                        node->var_type = fs->type_def->var_type;
                    } else if (fs) {
                        node->var_type = fs->type;
                        node->type_def = fs->type_def ? copyAST(fs->type_def) : NULL;
                    }
                    if (pushedGenericFrame) popGenericFrame();
                    return;
                }
            }
        }
        /* Preserve explicit syntax while still annotating variables with their
         * declared types so later analyses (e.g. array element access) can
         * determine the base type.
         */
        const char *ident = node->token->value;
        AST *decl = findStaticDeclarationInAST(ident, node, gProgramRoot);
        if (!decl && node->parent) {
            decl = findStaticDeclarationInAST(ident, node->parent, gProgramRoot);
        }
        if (!decl && node->parent && node->parent->parent) {
            decl = findStaticDeclarationInAST(ident, node->parent->parent, gProgramRoot);
        }
        if (!decl) {
            if (clsContext) {
                Symbol *constSym = lookupConstMember(clsContext, ident);
                if (constSym) {
                    if (constSym->value) {
                        node->var_type = constSym->value->type;
                    } else if (constSym->type != TYPE_UNKNOWN) {
                        node->var_type = constSym->type;
                    }
                    if (constSym->type_def) {
                        node->type_def = copyAST(constSym->type_def);
                    }
                    if (pushedGenericFrame) popGenericFrame();
                    return;
                }
            }
            AST *ancestor = node->parent;
            while (!decl && ancestor) {
                decl = findStaticDeclarationInAST(ident, ancestor, gProgramRoot);
                ancestor = ancestor->parent;
            }
        }
        if (!decl) {
            int referenceLine = node->token ? node->token->line : 0;
            AST *cursor = node->parent;
            while (!decl && cursor) {
                AST *container = cursor->parent;
                if (container && container->type == AST_COMPOUND && container->children) {
                    for (int idx = 0; idx < container->child_count; idx++) {
                        AST *child = container->children[idx];
                        if (child == cursor) {
                            for (int k = idx - 1; k >= 0 && !decl; k--) {
                                AST *sibling = container->children[k];
                                if (!sibling) continue;
                                AST *found = findDeclInCompound(sibling, ident, referenceLine);
                                if (found) {
                                    decl = found;
                                }
                                if (decl) break;
                            }
                            break;
                        }
                    }
                }
                cursor = cursor->parent;
            }
            if (!decl) {
                decl = findVarDeclAnywhere(gProgramRoot, ident, referenceLine);
                if (decl) {
                    int declLine = declarationLine(decl);
                    if (declLine > 0 && referenceLine > 0 && declLine != referenceLine) {
                        decl = NULL;
                    }
                }
            }
        }
        if (!decl) {
            AST *scope = node->parent;
            while (scope && scope->type != AST_FUNCTION_DECL && scope->type != AST_PROCEDURE_DECL) {
                scope = scope->parent;
            }
            if (scope) {
                AST *body = (scope->type == AST_FUNCTION_DECL) ? scope->extra : scope->right;
                decl = findFunctionInSubtree(body, ident);
                if (decl && (decl->type == AST_FUNCTION_DECL || decl->type == AST_PROCEDURE_DECL) && node->token) {
                    int decl_line = declarationLine(decl);
                    if (decl_line > 0 && decl_line > node->token->line) {
                        decl = NULL;
                    }
                }
            }
        }
        if (!decl) {
            ReaModuleBinding *binding = findActiveBinding(ident);
            if (binding) {
                if (pushedGenericFrame) popGenericFrame();
                return;
            }
            char lowered[MAX_SYMBOL_LENGTH * 2];
            snprintf(lowered, sizeof(lowered), "%s", ident);
            toLowerString(lowered);
            Symbol *proc_sym = lookupProcedure(lowered);
            if (proc_sym && proc_sym->is_defined) {
                if (pushedGenericFrame) popGenericFrame();
                return;
            }
            char lowerIdent[MAX_SYMBOL_LENGTH * 2];
            snprintf(lowerIdent, sizeof(lowerIdent), "%s", ident);
            toLowerString(lowerIdent);
            Symbol *global = lookupGlobalSymbol(lowerIdent);
            if (global) {
                node->var_type = global->type;
                if (pushedGenericFrame) popGenericFrame();
                return;
            }
        }

        if (decl && decl->right) {
            node->type_def = decl->right;
            node->var_type = decl->right->var_type;
        } else {
            if (isGenericTypeName(ident)) {
                node->var_type = TYPE_UNKNOWN;
                if (pushedGenericFrame) popGenericFrame();
                return;
            }
            if (strcasecmp(ident, "myself") != 0 && strcasecmp(ident, "my") != 0) {
                const char *dot = strchr(ident, '.');
                if (dot) {
                    size_t prefix_len = (size_t)(dot - ident);
                    if (prefix_len > 0 && prefix_len < MAX_SYMBOL_LENGTH) {
                        char prefix[MAX_SYMBOL_LENGTH];
                        memcpy(prefix, ident, prefix_len);
                        prefix[prefix_len] = '\0';
                        if (findActiveBinding(prefix)) {
                            if (pushedGenericFrame) popGenericFrame();
                            return;
                        }
                    }
                }
                ReaModuleInfo *firstModule = NULL;
                ReaModuleExport *firstExport = NULL;
                int matches = countAccessibleExports(ident, gActiveBindings, &firstModule, &firstExport);
                if (matches == 1 && firstModule && firstExport) {
                    if (firstExport->kind == REA_MODULE_EXPORT_CONST || firstExport->kind == REA_MODULE_EXPORT_VAR) {
                        char *qualified = makeQualifiedName(firstModule->name, firstExport->name);
                        if (node->token && node->token->value) {
                            free(node->token->value);
                            node->token->value = qualified;
                            node->token->length = strlen(qualified);
                        } else {
                            free(qualified);
                        }

                        if (node->type_def) {
                            freeAST(node->type_def);
                            node->type_def = NULL;
                        }
                        AST *typeNode = firstExport->decl ? firstExport->decl->right : NULL;
                        if (firstExport->kind == REA_MODULE_EXPORT_CONST) {
                            node->var_type = firstExport->decl ? firstExport->decl->var_type : TYPE_UNKNOWN;
                        } else {
                            node->var_type = firstExport->decl ? firstExport->decl->var_type : TYPE_UNKNOWN;
                            if (node->var_type == TYPE_UNKNOWN && typeNode) {
                                node->var_type = typeNode->var_type;
                            }
                        }
                        if (typeNode) {
                            node->type_def = copyAST(typeNode);
                        }
                        if (pushedGenericFrame) popGenericFrame();
                        return;
                    }
                    fprintf(stderr, "L%d: identifier '%s' is not a value export.\n",
                            node->token->line, ident);
                } else if (matches > 1) {
                    fprintf(stderr, "L%d: ambiguous reference to '%s'.\n",
                            node->token->line, ident);
                } else {
                    fprintf(stderr, "L%d: identifier '%s' not in scope.\n",
                            node->token->line, ident);
                }
                pascal_semantic_error_count++;
            }
        }

        if (decl && (decl->type == AST_VAR_DECL || decl->type == AST_CONST_DECL)) {
            AST *decl_func = findEnclosingFunction(decl);
            AST *use_func = findEnclosingFunction(node);
            if (decl_func == use_func) {
                AST *decl_scope = findEnclosingCompound(decl);
                AST *use_scope = findEnclosingCompound(node);
                if (decl_scope && use_scope && decl_scope == use_scope) {
                    int decl_line = declarationLine(decl);
                    if (decl_line > 0 && decl_line > node->token->line) {
                        Symbol *global = lookupGlobalSymbol(ident);
                        if (global) {
                            node->var_type = global->type;
                            node->type_def = global->type_def;
                            decl = NULL;
                        } else {
                            ReaModuleInfo *firstModule = NULL;
                            ReaModuleExport *firstExport = NULL;
                            int matches = countAccessibleExports(ident, gActiveBindings, &firstModule, &firstExport);
                            if (matches == 1 && firstModule && firstExport &&
                                (firstExport->kind == REA_MODULE_EXPORT_CONST || firstExport->kind == REA_MODULE_EXPORT_VAR)) {
                                char *qualified = makeQualifiedName(firstModule->name, firstExport->name);
                                if (node->token && node->token->value) {
                                    free(node->token->value);
                                    node->token->value = qualified;
                                    node->token->length = strlen(qualified);
                                } else {
                                    free(qualified);
                                }
                                if (node->type_def) {
                                    freeAST(node->type_def);
                                    node->type_def = NULL;
                                }
                                AST *typeNode = firstExport->decl ? firstExport->decl->right : NULL;
                                node->var_type = firstExport->decl ? firstExport->decl->var_type : TYPE_UNKNOWN;
                                if (node->var_type == TYPE_UNKNOWN && typeNode) {
                                    node->var_type = typeNode->var_type;
                                }
                                if (typeNode) {
                                    node->type_def = copyAST(typeNode);
                                }
                                decl = NULL;
                            } else if (matches > 1) {
                                fprintf(stderr, "L%d: ambiguous reference to '%s'.\n",
                                        node->token->line, ident);
                            } else {
                                fprintf(stderr, "L%d: identifier '%s' not in scope.\n",
                                        node->token->line, ident);
                            }
                            pascal_semantic_error_count++;
                        }
                    }
                }
            }
        }

        if (decl && (decl->type == AST_FUNCTION_DECL || decl->type == AST_PROCEDURE_DECL)) {
            AST *enclosing = findEnclosingFunction(decl);
            if (enclosing && closureCapturesOuterScope(decl)) {
                bool partOfCall = false;
                AST *parent = node->parent;
                if (parent && parent->type == AST_PROCEDURE_CALL && parent->token && node->token &&
                    parent->token->value && node->token->value &&
                    strcasecmp(parent->token->value, node->token->value) == 0) {
                    partOfCall = true;
                }
                if (!partOfCall) {
                    fprintf(stderr,
                            "L%d: closure captures a local value that would escape its lifetime.\n",
                            node->token->line);
                    pascal_semantic_error_count++;
                }
            }
        }
    }

    if (node->type == AST_FIELD_ACCESS) {
        if (handleModuleFieldAccess(node)) {
            if (node->type != AST_FIELD_ACCESS) {
                validateNodeInternal(node, clsContext);
            }
            if (pushedGenericFrame) popGenericFrame();
            return;
        }
        const char *cls = resolveExprClass(node->left, clsContext);
        if (cls) {
            ClassInfo *ci = lookupClass(cls);
            const char *fname = node->right && node->right->token ? node->right->token->value : NULL;
            if (ci) {
                Symbol *fs = lookupField(ci, fname);
                if (!fs) {
                    fprintf(stderr, "Unknown field '%s' on class '%s'\n", fname ? fname : "(null)", cls);
                    pascal_semantic_error_count++;
                } else if (fs->is_const && fs->value) {
                    /* Replace field access with constant literal */
                    Value *v = fs->value;
                    char buf[64];
                    Token *tok = NULL;
                    ASTNodeType newType = AST_NUMBER;
                    if (v->type == TYPE_DOUBLE || v->type == TYPE_REAL ||
                        v->type == TYPE_LONG_DOUBLE || v->type == TYPE_FLOAT) {
                        snprintf(buf, sizeof(buf), "%Lf", v->real.r_val);
                        tok = newToken(TOKEN_REAL_CONST, strdup(buf), 0, 0);
                        newType = AST_NUMBER;
                    } else if (v->type == TYPE_BOOLEAN) {
                        const char *lex = v->i_val ? "true" : "false";
                        tok = newToken(v->i_val ? TOKEN_TRUE : TOKEN_FALSE,
                                       strdup(lex), 0, 0);
                        newType = AST_BOOLEAN;
                    } else if (v->type == TYPE_STRING) {
                        tok = newToken(TOKEN_STRING_CONST,
                                       v->s_val ? strdup(v->s_val) : strdup(""),
                                       0, 0);
                        newType = AST_STRING;
                    } else if (v->type == TYPE_CHAR) {
                        char chbuf[2] = {(char)v->c_val, '\0'};
                        tok = newToken(TOKEN_STRING_CONST, strdup(chbuf), 0, 0);
                        newType = AST_STRING;
                    } else if (v->type == TYPE_ENUM && v->enum_val.enum_name) {
                        tok = newToken(TOKEN_IDENTIFIER,
                                       strdup(v->enum_val.enum_name), 0, 0);
                        newType = AST_ENUM_VALUE;
                    } else if (isIntlikeType(v->type)) {
                        snprintf(buf, sizeof(buf), "%lld", (long long)v->i_val);
                        tok = newToken(TOKEN_INTEGER_CONST, strdup(buf), 0, 0);
                        newType = AST_NUMBER;
                    }
                    if (tok) {
                        if (node->left) freeAST(node->left);
                        if (node->right) freeAST(node->right);
                        node->left = node->right = node->extra = NULL;
                        node->child_count = 0;
                        node->children = NULL;
                        node->token = tok;
                        node->type = newType;
                        setTypeAST(node, v->type);
                        switch (v->type) {
                            case TYPE_STRING:
                                node->i_val = v->s_val ? (int)strlen(v->s_val) : 0;
                                break;
                            case TYPE_CHAR:
                                node->i_val = 1;
                                break;
                            case TYPE_BOOLEAN:
                                node->i_val = v->i_val ? 1 : 0;
                                break;
                            case TYPE_ENUM:
                                node->i_val = v->enum_val.ordinal;
                                break;
                            default:
                                break;
                        }
                    }
                } else if (fs->type_def) {
                    node->var_type = fs->type_def->var_type;
                    node->type_def = copyAST(fs->type_def);
                }
            }
        }
    } else if (node->type == AST_PROCEDURE_CALL) {
        if (handleModuleCall(node)) {
            if (moduleFromExpression(node->left)) {
                if (pushedGenericFrame) popGenericFrame();
                return;
            }
        }
        bool qualifiedModuleCallResolved = false;
        if (!node->left && node->token && node->token->value) {
            const char *dot = strchr(node->token->value, '.');
            if (dot) {
                size_t prefixLen = (size_t)(dot - node->token->value);
                if (prefixLen > 0 && prefixLen < MAX_SYMBOL_LENGTH) {
                    char prefix[MAX_SYMBOL_LENGTH];
                    memcpy(prefix, node->token->value, prefixLen);
                    prefix[prefixLen] = '\0';
                    const char *member = dot + 1;
                    ReaModuleBinding *binding = findActiveBinding(prefix);
                    if (binding && binding->module) {
                        ReaModuleExport *exp = findModuleExport(binding->module, member);
                        if (!exp) {
                            fprintf(stderr, "L%d: '%s' is not exported from module '%s'.\n",
                                    node->token->line, member,
                                    binding->module->name ? binding->module->name : "(unknown)");
                            pascal_semantic_error_count++;
                        } else if (exp->kind == REA_MODULE_EXPORT_FUNCTION || exp->kind == REA_MODULE_EXPORT_PROCEDURE) {
                            char *qualified = makeQualifiedName(binding->module->name, exp->name);
                            free(node->token->value);
                            node->token->value = qualified;
                            node->token->length = strlen(qualified);
                            if (node->type_def) {
                                freeAST(node->type_def);
                                node->type_def = NULL;
                            }
                            if (exp->kind == REA_MODULE_EXPORT_FUNCTION && exp->decl) {
                                node->var_type = exp->decl->var_type;
                                node->type_def = exp->decl->right ? copyAST(exp->decl->right) : NULL;
                            } else {
                                node->var_type = TYPE_VOID;
                            }
                            qualifiedModuleCallResolved = true;
                        } else {
                            fprintf(stderr, "L%d: '%s' is not callable.\n",
                                    node->token->line, node->token->value);
                            pascal_semantic_error_count++;
                        }
                    }
                }
            }
        }
        AST *callDecl = NULL;
        if (node->token && node->token->value) {
            callDecl = findStaticDeclarationInAST(node->token->value, node, gProgramRoot);
            if (!callDecl) {
                callDecl = findGlobalFunctionDecl(node->token->value);
            }
            if (!callDecl) {
                AST *scope = node->parent;
                while (scope && scope->type != AST_FUNCTION_DECL && scope->type != AST_PROCEDURE_DECL) {
                    scope = scope->parent;
                }
                if (scope) {
                    AST *body = (scope->type == AST_FUNCTION_DECL) ? scope->extra : scope->right;
                    callDecl = findFunctionInSubtree(body, node->token->value);
                }
            }
            if (callDecl && (callDecl->type == AST_FUNCTION_DECL || callDecl->type == AST_PROCEDURE_DECL)) {
                AST *decl_scope = findEnclosingCompound(callDecl);
                AST *use_scope = findEnclosingCompound(node);
                if (decl_scope && use_scope && decl_scope == use_scope) {
                    int decl_line = declarationLine(callDecl);
                    if (decl_line > 0 && decl_line > node->token->line) {
                        Symbol *global = lookupGlobalSymbol(node->token->value);
                        if (!global) {
                            fprintf(stderr, "L%d: identifier '%s' not in scope.\n",
                                    node->token->line, node->token->value);
                            pascal_semantic_error_count++;
                        }
                    }
                }
            }
            AST *enclosing = findEnclosingFunction(node);
            if (enclosing) {
                AST *body = getFunctionBody(enclosing);
                AST *nested = findFunctionInSubtree(body, node->token->value);
                if (nested && nested != enclosing) {
                    AST *decl_scope = findEnclosingCompound(nested);
                    AST *use_scope = findEnclosingCompound(node);
                    if (decl_scope && use_scope && decl_scope == use_scope) {
                        int decl_line = declarationLine(nested);
                        if (decl_line > 0 && decl_line > node->token->line) {
                            fprintf(stderr, "L%d: identifier '%s' not in scope.\n",
                                    node->token->line, node->token->value);
                            pascal_semantic_error_count++;
                        }
                    }
                }
            }
        }
        if (!node->left && node->token && node->token->value && node->i_val == 0) {
            const char *us = strchr(node->token->value, '_');
            if (us) {
                size_t cls_len = (size_t)(us - node->token->value);
                char cls[MAX_SYMBOL_LENGTH];
                if (cls_len < sizeof(cls)) {
                    memcpy(cls, node->token->value, cls_len);
                    cls[cls_len] = '\0';
                    if (lookupClass(cls)) {
                        fprintf(stderr,
                                "Legacy method call '%s' is no longer supported; use instance.%s() instead\n",
                                node->token->value, us + 1);
                        pascal_semantic_error_count++;
                    }
                }
            }
        }
        if (!callDecl && !node->left && node->token && node->token->value &&
            !qualifiedModuleCallResolved && node->i_val != 1) {
            char lowered[MAX_SYMBOL_LENGTH * 2];
            snprintf(lowered, sizeof(lowered), "%s", node->token->value);
            toLowerString(lowered);
            Symbol *proc_sym = lookupProcedure(lowered);
            ReaModuleInfo *firstModule = NULL;
            ReaModuleExport *firstExport = NULL;
            int matches = countAccessibleExports(node->token->value, gActiveBindings, &firstModule, &firstExport);
            if (matches > 1) {
                fprintf(stderr, "L%d: ambiguous reference to '%s'.\n",
                        node->token->line, node->token->value);
                pascal_semantic_error_count++;
            } else if (matches == 1 && firstModule && firstExport) {
                if (firstExport->kind == REA_MODULE_EXPORT_FUNCTION ||
                    firstExport->kind == REA_MODULE_EXPORT_PROCEDURE) {
                    char *qualified = makeQualifiedName(firstModule->name, firstExport->name);
                    free(node->token->value);
                    node->token->value = qualified;
                    node->token->length = strlen(qualified);
                    if (node->type_def) {
                        freeAST(node->type_def);
                        node->type_def = NULL;
                    }
                    if (firstExport->kind == REA_MODULE_EXPORT_FUNCTION && firstExport->decl) {
                        node->var_type = firstExport->decl->var_type;
                        node->type_def = firstExport->decl->right ? copyAST(firstExport->decl->right) : NULL;
                    } else {
                        node->var_type = TYPE_VOID;
                    }
                } else {
                    fprintf(stderr, "L%d: '%s' is not callable.\n",
                            node->token->line, node->token->value);
                    pascal_semantic_error_count++;
                }
            } else if (!isBuiltin(node->token->value) && proc_sym && proc_sym->is_defined) {
                fprintf(stderr, "L%d: identifier '%s' not in scope.\n",
                        node->token->line, node->token->value);
                pascal_semantic_error_count++;
            } else if (!isBuiltin(node->token->value) && (!proc_sym || !proc_sym->is_defined)) {
                fprintf(stderr, "L%d: identifier '%s' not in scope.\n",
                        node->token->line, node->token->value);
                pascal_semantic_error_count++;
            }
        }
        if (node->i_val == 1) {
            /* super constructor/method call already has implicit 'myself' */
            if (node->token && node->token->value && !strchr(node->token->value, '.')) {
                const char *pname = node->token->value;
                size_t ln = strlen(pname) + 1 + strlen(pname) + 1;
                char *m = (char*)malloc(ln);
                if (m) {
                    snprintf(m, ln, "%s.%s", pname, pname);
                    free(node->token->value);
                    node->token->value = m;
                    node->token->length = strlen(m);
                }
            }
        } else if (node->left) {
            const char *cls = resolveExprClass(node->left, clsContext);
            const char *name = node->token ? node->token->value : NULL;
            if (cls && name) {
                const char *method = name;
                const char *us = strchr(name, '.');
                bool already = false;
                if (us && strncasecmp(name, cls, (size_t)(us - name)) == 0) {
                    method = us + 1;
                    already = true;
                }
                ClassInfo *ci = lookupClass(cls);
                if (ci) {
                    Symbol *ms = lookupMethod(ci, method);
                    if (!already && (ms || cls)) {
                        size_t ln = strlen(cls) + 1 + strlen(name) + 1;
                        char *m = (char*)malloc(ln);
                        if (m) {
                            snprintf(m, ln, "%s.%s", cls, name);
                            free(node->token->value);
                            node->token->value = m;
                            node->token->length = strlen(m);
                        }
                    }
                }
            }
            if (node->child_count > 0 && node->children[0] &&
                node->children[0]->type == AST_VARIABLE &&
                node->children[0]->token && node->children[0]->token->value &&
                (strcasecmp(node->children[0]->token->value, "myself") == 0 ||
                 strcasecmp(node->children[0]->token->value, "my") == 0)) {
                /*
                 * The parser places the receiver both as `left` and as the first
                 * child argument.  Using the same node in both locations leads to
                 * double-free errors when the AST is destroyed.  Instead of
                 * discarding the child (which would make the argument list empty),
                 * keep a separate copy so the call still receives the receiver as
                 * its first argument.
                 */
                AST *recv_copy = copyAST(node->left);
                if (recv_copy) {
                    node->children[0] = recv_copy;
                    recv_copy->parent = node;
                }
            }
        } else if (currentClass && node->token) {
            Symbol *sym = lookupMethod(currentClass, node->token->value);
            if (sym && sym->type_def && sym->type_def->token && sym->type_def->token->value) {
                const char *fullname = sym->type_def->token->value;
                size_t ln = strlen(fullname) + 1;
                char *m = (char*)malloc(ln);
                if (m) {
                    memcpy(m, fullname, ln);
                    free(node->token->value);
                    node->token->value = m;
                    node->token->length = ln - 1;
                }

                bool firstIsMyself = false;
                if (node->child_count > 0 && node->children[0] &&
                    node->children[0]->type == AST_VARIABLE &&
                    node->children[0]->token && node->children[0]->token->value &&
                    (strcasecmp(node->children[0]->token->value, "myself") == 0 ||
                     strcasecmp(node->children[0]->token->value, "my") == 0)) {
                    firstIsMyself = true;
                }

                if (!firstIsMyself && node->i_val == 0) {
                    Token *selfTok = newToken(TOKEN_IDENTIFIER, "myself", node->token ? node->token->line : 0, 0);
                    AST *selfVar = newASTNode(AST_VARIABLE, selfTok);
                    selfVar->var_type = TYPE_POINTER;
                    addChild(node, NULL);
                    for (int i = node->child_count - 1; i > 0; i--) {
                        node->children[i] = node->children[i - 1];
                        if (node->children[i]) node->children[i]->parent = node;
                    }
                    node->children[0] = selfVar;
                    selfVar->parent = node;
                    setLeft(node, selfVar);
                } else if (firstIsMyself && node->i_val == 0) {
                    setLeft(node, node->children[0]);
                }
            }
        }

        if (node->token && node->token->value) {
            char lower[MAX_SYMBOL_LENGTH];
            strncpy(lower, node->token->value, sizeof(lower) - 1);
            lower[sizeof(lower) - 1] = '\0';
            for (int i = 0; lower[i]; i++) {
                lower[i] = (char)tolower((unsigned char)lower[i]);
            }

            Symbol *procSym = lookupProcedure(lower);
            if (procSym && procSym->is_alias && procSym->real_symbol) {
                procSym = procSym->real_symbol;
            }

            if (procSym && procSym->type_def) {
                AST *decl = procSym->type_def;
                int totalParams = countFunctionParams(decl);
                if (totalParams > 0) {
                    int implicitCount = 0;
                    AST *firstParam = getFunctionParam(decl, 0);
                    if (paramIsImplicitSelf(firstParam)) {
                        implicitCount = 1;
                    }

                    if (implicitCount == 0) {
                        int explicitParams = totalParams;
                        int providedArgs = node->child_count;

                        int optionalCount = 0;
                        for (int idx = totalParams - 1; idx >= implicitCount; idx--) {
                            AST *paramDecl = getFunctionParam(decl, idx);
                            if (paramDecl && paramDecl->left) {
                                optionalCount++;
                            } else {
                                break;
                            }
                        }

                        int requiredArgs = explicitParams - optionalCount;
                        if (providedArgs < requiredArgs) {
                            fprintf(stderr, "L%d: Not enough arguments for '%s'.\n",
                                    node->token->line, node->token->value);
                            pascal_semantic_error_count++;
                        } else if (providedArgs > explicitParams) {
                            fprintf(stderr, "L%d: Too many arguments for '%s'.\n",
                                    node->token->line, node->token->value);
                            pascal_semantic_error_count++;
                        } else if (providedArgs < explicitParams) {
                            for (int idx = providedArgs; idx < explicitParams; idx++) {
                                AST *paramDecl = getFunctionParam(decl, idx);
                                if (!paramDecl || !paramDecl->left) break;
                                AST *defaultExpr = copyAST(paramDecl->left);
                                if (!defaultExpr) continue;
                                addChild(node, defaultExpr);
                            }
                        }
                    }
                }
            }
        }
    } else if (node->type == AST_ARRAY_ACCESS) {
        if (node->left) validateNodeInternal(node->left, clsContext);
        for (int i = 0; i < node->child_count; i++) {
            if (node->children[i]) validateNodeInternal(node->children[i], clsContext);
        }
        if (node->right) validateNodeInternal(node->right, clsContext);
        if (node->extra) validateNodeInternal(node->extra, clsContext);

        AST *baseType = node->left ? node->left->type_def : NULL;
        for (int i = 0; i < node->child_count && baseType; i++) {
            if (baseType->type == AST_ARRAY_TYPE) {
                baseType = baseType->right;
            } else {
                baseType = NULL;
            }
        }
        if (!baseType) {
            const char *cls = resolveExprClass(node->left, clsContext);
            if (cls) {
                Token *tok = newToken(TOKEN_IDENTIFIER, cls,
                                       node->token ? node->token->line : 0, 0);
                AST *typeRef = newASTNode(AST_TYPE_REFERENCE, tok);
                setTypeAST(typeRef, TYPE_RECORD);
                AST *ptrType = newASTNode(AST_POINTER_TYPE, NULL);
                setRight(ptrType, typeRef);
                setTypeAST(ptrType, TYPE_POINTER);
                node->type_def = ptrType;
            }
            setTypeAST(node, TYPE_POINTER);
            return;
        }
        AST *elemType = copyAST(baseType);
        setTypeAST(node, baseType->var_type);
        if (node->var_type == TYPE_RECORD || node->var_type == TYPE_VOID ||
            node->var_type == TYPE_UNKNOWN || baseType->type == AST_TYPE_REFERENCE ||
            baseType->type == AST_RECORD_TYPE) {
            AST *ptrType = newASTNode(AST_POINTER_TYPE, NULL);
            setRight(ptrType, elemType);
            setTypeAST(ptrType, TYPE_POINTER);
            node->type_def = ptrType;
            setTypeAST(node, TYPE_POINTER);
        } else {
            node->type_def = elemType;
        }
        return;
    }

    ClassInfo *recurseContext = clsContext;
    if (node->type == AST_TYPE_DECL && node->left && node->left->type == AST_RECORD_TYPE &&
        node->token && node->token->value) {
        ClassInfo *declClass = lookupClass(node->token->value);
        if (declClass) {
            recurseContext = declClass;
        }
    }

    if (node->left) validateNodeInternal(node->left, recurseContext);
    if (node->right) validateNodeInternal(node->right, recurseContext);
    if (node->extra) validateNodeInternal(node->extra, recurseContext);
    for (int i = 0; i < node->child_count; i++) validateNodeInternal(node->children[i], recurseContext);

    if (pushedGenericFrame) popGenericFrame();
}

/* ------------------------------------------------------------------------- */
/*  Public entry                                                             */
/* ------------------------------------------------------------------------- */

static void analyzeProgramWithBindings(AST *root, ReaModuleBindingList *bindings) {
    if (!root) return;
    ReaModuleBindingList *previous = gActiveBindings;
    gActiveBindings = bindings;
    gProgramRoot = root;
    resetClosureRegistry();
    collectClasses(root);
    collectMethods(root);
    linkParents();
    checkOverrides();
    addInheritedMethodAliases();
    analyzeClosureCaptures(root);
    validateNodeInternal(root, NULL);
    destroyClosureRegistry();
    refreshProcedureMethodCopies();
    freeClassTable();
    gActiveBindings = previous;
}

void reaPerformSemanticAnalysis(AST *root) {
    if (!root) return;
    ensureReaSymbolTables();
    ensureExceptionGlobals(root);
    AST *rewrittenRoot = desugarNode(root, TYPE_VOID);
    if (rewrittenRoot) {
        root = rewrittenRoot;
    }
    flattenDeclarationCompounds(root);
    ReaModuleBindingList mainBindings = {0};
    AST *decls = getDeclsCompound(root);
    AST *stmts = NULL;
    if (decls && decls->parent && decls->parent->child_count > 1) {
        stmts = decls->parent->children[1];
    } else if (root && root->type == AST_PROGRAM && root->right && root->right->child_count > 1) {
        stmts = root->right->children[1];
    }
    if (stmts) {
        // No additional processing needed for statements when collecting module bindings.
    }
    collectImportBindings(decls, &mainBindings);
    collectImportBindings(stmts, &mainBindings);
    if (decls) {
        for (int i = 0; i < decls->child_count; i++) {
            AST *child = decls->children[i];
            (void)child;
        }
    }
    analyzeProgramWithBindings(root, &mainBindings);
    freeBindingList(&mainBindings);
    clearGenericTypeState();
    freeDirStack();
}

int reaGetLoadedModuleCount(void) {
    return gLoadedModuleCount;
}

AST *reaGetModuleAST(int index) {
    if (index < 0 || index >= gLoadedModuleCount) return NULL;
    ReaModuleInfo *info = gLoadedModules[index];
    return info ? info->ast : NULL;
}

const char *reaGetModulePath(int index) {
    if (index < 0 || index >= gLoadedModuleCount) return NULL;
    ReaModuleInfo *info = gLoadedModules[index];
    return info ? info->path : NULL;
}

const char *reaGetModuleName(int index) {
    if (index < 0 || index >= gLoadedModuleCount) return NULL;
    ReaModuleInfo *info = gLoadedModules[index];
    return info ? info->name : NULL;
}

char *reaResolveImportPath(const char *path) {
    bool exists = false;
    char *resolved = resolveModulePath(path, &exists);
    if (!resolved) {
        return NULL;
    }
    if (!exists) {
        free(resolved);
        return NULL;
    }
    return resolved;
}

void reaSemanticResetState(void) {
    clearModuleCache();
    clearEnvImportPaths();
    clearGenericTypeState();
    freeDirStack();
    freeClassTable();
    gActiveBindings = NULL;
    gProgramRoot = NULL;
}
