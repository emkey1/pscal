#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

typedef struct {
    char *name;
    char **functions;
    size_t function_count;
} ExtBuiltinCategory;

/* Global registry of extended builtins.  Access is protected by
 * registry_mutex so callers can safely register/query builtins from
 * multiple threads. */
static ExtBuiltinCategory *categories = NULL;
static size_t category_count = 0;
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Helpers below assume registry_mutex is held. */
static ExtBuiltinCategory *findCategory(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < category_count; ++i) {
        if (strcasecmp(categories[i].name, name) == 0) {
            return &categories[i];
        }
    }
    return NULL;
}

static ExtBuiltinCategory *ensureCategory(const char *name) {
    ExtBuiltinCategory *cat = findCategory(name);
    if (cat) return cat;
    ExtBuiltinCategory *new_array = realloc(categories, sizeof(ExtBuiltinCategory) * (category_count + 1));
    if (!new_array) return NULL;
    categories = new_array;
    cat = &categories[category_count];
    cat->name = strdup(name ? name : "");
    cat->functions = NULL;
    cat->function_count = 0;
    category_count++;
    return cat;
}

void extBuiltinRegisterCategory(const char *name) {
    if (!name) return;
    pthread_mutex_lock(&registry_mutex);
    ensureCategory(name);
    pthread_mutex_unlock(&registry_mutex);
}

void extBuiltinRegisterFunction(const char *category, const char *func) {
    if (!category || !func) return;
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = ensureCategory(category);
    if (!cat) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    for (size_t i = 0; i < cat->function_count; ++i) {
        if (strcasecmp(cat->functions[i], func) == 0) {
            pthread_mutex_unlock(&registry_mutex);
            return; /* already registered */
        }
    }
    char **new_funcs = realloc(cat->functions, sizeof(char*) * (cat->function_count + 1));
    if (!new_funcs) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    cat->functions = new_funcs;
    cat->functions[cat->function_count] = strdup(func);
    cat->function_count++;
    pthread_mutex_unlock(&registry_mutex);
}

size_t extBuiltinGetCategoryCount(void) {
    pthread_mutex_lock(&registry_mutex);
    size_t count = category_count;
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetCategoryName(size_t index) {
    pthread_mutex_lock(&registry_mutex);
    const char *name = (index < category_count) ? categories[index].name : NULL;
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

size_t extBuiltinGetFunctionCount(const char *category) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    size_t count = cat ? cat->function_count : 0;
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetFunctionName(const char *category, size_t index) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    const char *name = (!cat || index >= cat->function_count) ? NULL : cat->functions[index];
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

int extBuiltinHasFunction(const char *category, const char *func) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    int present = 0;
    if (cat && func) {
        for (size_t i = 0; i < cat->function_count; ++i) {
            if (strcasecmp(cat->functions[i], func) == 0) {
                present = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return present;
}

