#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char *name;
    char **functions;
    size_t function_count;
} ExtBuiltinCategory;

static ExtBuiltinCategory *categories = NULL;
static size_t category_count = 0;

static ExtBuiltinCategory *find_category(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < category_count; ++i) {
        if (strcasecmp(categories[i].name, name) == 0) {
            return &categories[i];
        }
    }
    return NULL;
}

static ExtBuiltinCategory *ensure_category(const char *name) {
    ExtBuiltinCategory *cat = find_category(name);
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
    ensure_category(name);
}

void extBuiltinRegisterFunction(const char *category, const char *func) {
    if (!category || !func) return;
    ExtBuiltinCategory *cat = ensure_category(category);
    if (!cat) return;
    for (size_t i = 0; i < cat->function_count; ++i) {
        if (strcasecmp(cat->functions[i], func) == 0) {
            return; /* already registered */
        }
    }
    char **new_funcs = realloc(cat->functions, sizeof(char*) * (cat->function_count + 1));
    if (!new_funcs) return;
    cat->functions = new_funcs;
    cat->functions[cat->function_count] = strdup(func);
    cat->function_count++;
}

size_t extBuiltinGetCategoryCount(void) {
    return category_count;
}

const char *extBuiltinGetCategoryName(size_t index) {
    if (index >= category_count) return NULL;
    return categories[index].name;
}

size_t extBuiltinGetFunctionCount(const char *category) {
    ExtBuiltinCategory *cat = find_category(category);
    return cat ? cat->function_count : 0;
}

const char *extBuiltinGetFunctionName(const char *category, size_t index) {
    ExtBuiltinCategory *cat = find_category(category);
    if (!cat || index >= cat->function_count) return NULL;
    return cat->functions[index];
}

int extBuiltinHasFunction(const char *category, const char *func) {
    ExtBuiltinCategory *cat = find_category(category);
    if (!cat || !func) return 0;
    for (size_t i = 0; i < cat->function_count; ++i) {
        if (strcasecmp(cat->functions[i], func) == 0) {
            return 1;
        }
    }
    return 0;
}

