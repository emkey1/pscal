#include "registry.h"
#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    char **keys;
    size_t *values;
    size_t size;
    size_t capacity;
} StringIndex;

typedef struct ExtBuiltinGroup_s {
    char *name; /* NULL indicates the default/ungrouped bucket. */
    char *full_name; /* Cached fully qualified group name for nested groups. */
    char **functions;
    size_t function_count;
    size_t function_capacity;
    StringIndex function_index;
    struct ExtBuiltinGroup_s **children;
    size_t child_count;
    size_t child_capacity;
    StringIndex child_index;
} ExtBuiltinGroup;

typedef struct {
    char *name;
    ExtBuiltinGroup **groups;
    size_t group_count;
    size_t group_capacity;
    StringIndex group_index;
    ExtBuiltinGroup default_group;
    int has_default_group;
} ExtBuiltinCategory;

typedef struct {
    ExtBuiltinCategory **categories;
    size_t count;
    size_t capacity;
    StringIndex index;
} ExtBuiltinRegistry;

static void stringIndexInit(StringIndex *index) {
    if (!index) return;
    index->keys = NULL;
    index->values = NULL;
    index->size = 0;
    index->capacity = 0;
}

static void stringIndexDestroy(StringIndex *index) {
    if (!index) return;
    free(index->keys);
    free(index->values);
    index->keys = NULL;
    index->values = NULL;
    index->size = 0;
    index->capacity = 0;
}

static size_t hashCaseFold(const char *text) {
    size_t hash = 1469598103934665603ULL; /* FNV-1a 64-bit offset basis */
    while (text && *text) {
        unsigned char lower = (unsigned char)tolower((unsigned char)*text);
        hash ^= lower;
        hash *= 1099511628211ULL; /* FNV-1a prime */
        ++text;
    }
    return hash;
}

static int stringIndexResize(StringIndex *index, size_t new_capacity) {
    if (!index) return 0;
    if (new_capacity == 0) {
        stringIndexDestroy(index);
        return 1;
    }

    char **new_keys = calloc(new_capacity, sizeof(char *));
    size_t *new_values = calloc(new_capacity, sizeof(size_t));
    if (!new_keys || !new_values) {
        free(new_keys);
        free(new_values);
        return 0;
    }

    size_t old_capacity = index->capacity;
    char **old_keys = index->keys;
    size_t *old_values = index->values;

    index->keys = new_keys;
    index->values = new_values;
    index->capacity = new_capacity;
    index->size = 0;

    if (old_capacity > 0 && old_keys) {
        size_t mask = new_capacity - 1;
        for (size_t i = 0; i < old_capacity; ++i) {
            if (!old_keys[i]) {
                continue;
            }
            size_t pos = hashCaseFold(old_keys[i]) & mask;
            while (index->keys[pos]) {
                pos = (pos + 1) & mask;
            }
            index->keys[pos] = old_keys[i];
            index->values[pos] = old_values[i];
            index->size++;
        }
    }

    free(old_keys);
    free(old_values);
    return 1;
}

static int stringIndexEnsureCapacity(StringIndex *index) {
    if (!index) return 0;
    size_t threshold = index->capacity ? index->capacity / 2 : 0;
    if (index->capacity != 0 && index->size < threshold) {
        return 1;
    }

    size_t new_capacity = index->capacity ? index->capacity * 2 : 8;
    if (new_capacity < 8) {
        new_capacity = 8;
    }
    while (new_capacity && index->size >= (new_capacity / 2)) {
        new_capacity *= 2;
    }
    return stringIndexResize(index, new_capacity);
}

static size_t stringIndexLookup(const StringIndex *index, const char *key) {
    if (!index || !key || index->capacity == 0) {
        return SIZE_MAX;
    }
    size_t mask = index->capacity - 1;
    size_t pos = hashCaseFold(key) & mask;
    while (index->keys[pos]) {
        if (strcasecmp(index->keys[pos], key) == 0) {
            return index->values[pos];
        }
        pos = (pos + 1) & mask;
    }
    return SIZE_MAX;
}

static int stringIndexInsert(StringIndex *index, const char *key, size_t value) {
    if (!index || !key) {
        return 0;
    }
    if (!stringIndexEnsureCapacity(index)) {
        return 0;
    }
    size_t mask = index->capacity - 1;
    size_t pos = hashCaseFold(key) & mask;
    while (index->keys[pos]) {
        if (strcasecmp(index->keys[pos], key) == 0) {
            index->values[pos] = value;
            return 1;
        }
        pos = (pos + 1) & mask;
    }
    index->keys[pos] = (char *)key;
    index->values[pos] = value;
    index->size++;
    return 1;
}

static int appendGroupPointer(ExtBuiltinGroup ***array, size_t *count,
                              size_t *capacity, ExtBuiltinGroup *value) {
    if (!array || !count || !capacity) {
        return 0;
    }
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        ExtBuiltinGroup **new_array =
            realloc(*array, sizeof(ExtBuiltinGroup *) * new_capacity);
        if (!new_array) {
            return 0;
        }
        *array = new_array;
        *capacity = new_capacity;
    }
    (*array)[*count] = value;
    (*count)++;
    return 1;
}

static int appendStringPointer(char ***array, size_t *count, size_t *capacity,
                               char *value) {
    if (!array || !count || !capacity) {
        return 0;
    }
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        char **new_array = realloc(*array, sizeof(char *) * new_capacity);
        if (!new_array) {
            return 0;
        }
        *array = new_array;
        *capacity = new_capacity;
    }
    (*array)[*count] = value;
    (*count)++;
    return 1;
}

static int appendCategoryPointer(ExtBuiltinCategory ***array, size_t *count,
                                 size_t *capacity, ExtBuiltinCategory *value) {
    if (!array || !count || !capacity) {
        return 0;
    }
    if (*count == *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4 : (*capacity * 2);
        ExtBuiltinCategory **new_array =
            realloc(*array, sizeof(ExtBuiltinCategory *) * new_capacity);
        if (!new_array) {
            return 0;
        }
        *array = new_array;
        *capacity = new_capacity;
    }
    (*array)[*count] = value;
    (*count)++;
    return 1;
}

static void initGroup(ExtBuiltinGroup *group) {
    if (!group) return;
    group->name = NULL;
    group->full_name = NULL;
    group->functions = NULL;
    group->function_count = 0;
    group->function_capacity = 0;
    stringIndexInit(&group->function_index);
    group->children = NULL;
    group->child_count = 0;
    group->child_capacity = 0;
    stringIndexInit(&group->child_index);
}

static void freeGroupRecursive(ExtBuiltinGroup *group, int free_self) {
    if (!group) return;
    for (size_t i = 0; i < group->child_count; ++i) {
        freeGroupRecursive(group->children[i], 1);
    }
    free(group->children);
    group->children = NULL;
    group->child_count = 0;
    group->child_capacity = 0;
    stringIndexDestroy(&group->child_index);

    for (size_t i = 0; i < group->function_count; ++i) {
        free(group->functions[i]);
    }
    free(group->functions);
    group->functions = NULL;
    group->function_count = 0;
    group->function_capacity = 0;
    stringIndexDestroy(&group->function_index);

    free(group->name);
    group->name = NULL;
    free(group->full_name);
    group->full_name = NULL;

    if (free_self) {
        free(group);
    }
}

static void freeCategory(ExtBuiltinCategory *category) {
    if (!category) return;
    for (size_t i = 0; i < category->group_count; ++i) {
        freeGroupRecursive(category->groups[i], 1);
    }
    free(category->groups);
    category->groups = NULL;
    category->group_count = 0;
    category->group_capacity = 0;
    stringIndexDestroy(&category->group_index);

    freeGroupRecursive(&category->default_group, 0);
    category->has_default_group = 0;

    free(category->name);
    category->name = NULL;
    free(category);
}

static ExtBuiltinGroup *attachGroup(ExtBuiltinGroup ***array, size_t *count,
                                    size_t *capacity, StringIndex *index,
                                    ExtBuiltinGroup *group) {
    if (!array || !count || !capacity || !index || !group || !group->name) {
        return NULL;
    }
    size_t position = *count;
    if (!appendGroupPointer(array, count, capacity, group)) {
        return NULL;
    }
    if (!stringIndexInsert(index, group->name, position)) {
        (*count)--;
        (*array)[*count] = NULL;
        return NULL;
    }
    return group;
}

static ExtBuiltinGroup *ensureGroupComponent(ExtBuiltinGroup ***array,
                                             size_t *count, size_t *capacity,
                                             StringIndex *index,
                                             ExtBuiltinGroup *parent,
                                             const char *component,
                                             size_t component_len) {
    if (!array || !count || !capacity || !index || !component || component_len == 0) {
        return NULL;
    }

    char *component_name = dupComponent(component, component_len);
    if (!component_name) {
        return NULL;
    }

    size_t existing_index = stringIndexLookup(index, component_name);
    if (existing_index != SIZE_MAX && existing_index < *count) {
        ExtBuiltinGroup *existing = (*array)[existing_index];
        free(component_name);
        return existing;
    }

    ExtBuiltinGroup *group = calloc(1, sizeof(*group));
    if (!group) {
        free(component_name);
        return NULL;
    }
    initGroup(group);
    group->name = component_name;
    group->full_name = buildFullName(parent, group->name);
    if (!group->full_name) {
        free(group->name);
        group->name = NULL;
        free(group);
        return NULL;
    }

    if (!attachGroup(array, count, capacity, index, group)) {
        free(group->full_name);
        group->full_name = NULL;
        free(group->name);
        group->name = NULL;
        free(group);
        return NULL;
    }

    return group;
}

/* Global registry of extended builtins.  Access is protected by
 * registry_mutex so callers can safely register/query builtins from
 * multiple threads. */
static ExtBuiltinRegistry registry = {0};
static pthread_mutex_t registry_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Helpers below assume registry_mutex is held. */
static ExtBuiltinCategory *findCategory(const char *name) {
    if (!name) return NULL;
    size_t index = stringIndexLookup(&registry.index, name);
    if (index == SIZE_MAX || index >= registry.count) {
        return NULL;
    }
    return registry.categories[index];
}

static ExtBuiltinCategory *ensureCategory(const char *name) {
    ExtBuiltinCategory *existing = findCategory(name);
    if (existing) {
        return existing;
    }

    ExtBuiltinCategory *category = calloc(1, sizeof(*category));
    if (!category) {
        return NULL;
    }
    stringIndexInit(&category->group_index);
    initGroup(&category->default_group);

    const char *source_name = name ? name : "";
    category->name = strdup(source_name);
    if (!category->name) {
        freeCategory(category);
        return NULL;
    }

    size_t index = registry.count;
    if (!appendCategoryPointer(&registry.categories, &registry.count,
                               &registry.capacity, category)) {
        freeCategory(category);
        return NULL;
    }
    if (!stringIndexInsert(&registry.index, category->name, index)) {
        registry.count--;
        registry.categories[registry.count] = NULL;
        freeCategory(category);
        return NULL;
    }
    return category;
}

static int isDefaultGroup(const char *group) {
    return !group || *group == '\0';
}

static char *buildFullName(const ExtBuiltinGroup *parent,
                           const char *component) {
    size_t component_len = strlen(component);
    size_t prefix_len = (parent && parent->full_name) ? strlen(parent->full_name) : 0;
    size_t total = component_len + (prefix_len ? prefix_len + 1 : 0);
    char *full = malloc(total + 1);
    if (!full) {
        return NULL;
    }
    if (prefix_len) {
        memcpy(full, parent->full_name, prefix_len);
        full[prefix_len] = '/';
        memcpy(full + prefix_len + 1, component, component_len);
        full[total] = '\0';
    } else {
        memcpy(full, component, component_len);
        full[component_len] = '\0';
    }
    return full;
}

static char *dupComponent(const char *component, size_t component_len) {
    char *copy = malloc(component_len + 1);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, component, component_len);
    copy[component_len] = '\0';
    return copy;
}

static ExtBuiltinGroup *findGroup(ExtBuiltinCategory *category,
                                  const char *group_name) {
    if (!category || isDefaultGroup(group_name)) {
        return NULL;
    }

    const char *cursor = group_name;
    ExtBuiltinGroup *current = NULL;
    while (*cursor) {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }

        const char *start = cursor;
        while (*cursor && *cursor != '/') {
            ++cursor;
        }
        size_t len = (size_t)(cursor - start);
        char *component = dupComponent(start, len);
        if (!component) {
            return NULL;
        }

        if (!current) {
            size_t index = stringIndexLookup(&category->group_index, component);
            free(component);
            if (index == SIZE_MAX || index >= category->group_count) {
                return NULL;
            }
            current = category->groups[index];
        } else {
            size_t index = stringIndexLookup(&current->child_index, component);
            free(component);
            if (index == SIZE_MAX || index >= current->child_count) {
                return NULL;
            }
            current = current->children[index];
        }
    }
    return current;
}

static ExtBuiltinGroup *ensureGroup(ExtBuiltinCategory *category,
                                    const char *group_name) {
    if (!category) return NULL;
    if (isDefaultGroup(group_name)) {
        category->has_default_group = 1;
        return &category->default_group;
    }

    const char *cursor = group_name;
    ExtBuiltinGroup *current = NULL;

    while (*cursor) {
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        const char *start = cursor;
        while (*cursor && *cursor != '/') {
            ++cursor;
        }
        size_t len = (size_t)(cursor - start);

        ExtBuiltinGroup ***array = NULL;
        size_t *count = NULL;
        size_t *capacity = NULL;
        StringIndex *index = NULL;
        if (!current) {
            array = &category->groups;
            count = &category->group_count;
            capacity = &category->group_capacity;
            index = &category->group_index;
        } else {
            array = &current->children;
            count = &current->child_count;
            capacity = &current->child_capacity;
            index = &current->child_index;
        }

        ExtBuiltinGroup *next = ensureGroupComponent(array, count, capacity, index,
                                                     current, start, len);
        if (!next) {
            return NULL;
        }
        current = next;
    }
    return current;
}

static size_t countGroupsRecursive(ExtBuiltinGroup *const *groups,
                                   size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (!groups[i]) continue;
        ++total;
        total += countGroupsRecursive(groups[i]->children, groups[i]->child_count);
    }
    return total;
}

static const char *getGroupNameAtRecursive(ExtBuiltinGroup *const *groups,
                                           size_t count, size_t *index) {
    for (size_t i = 0; i < count; ++i) {
        if (!groups[i]) continue;
        if (*index == 0) {
            return groups[i]->full_name;
        }
        --(*index);
        const char *child =
            getGroupNameAtRecursive(groups[i]->children, groups[i]->child_count,
                                     index);
        if (child) {
            return child;
        }
    }
    return NULL;
}

static int groupHasFunction(const ExtBuiltinGroup *group, const char *func) {
    if (!group || !func) {
        return 0;
    }
    if (stringIndexLookup(&group->function_index, func) != SIZE_MAX) {
        return 1;
    }
    for (size_t i = 0; i < group->child_count; ++i) {
        if (groupHasFunction(group->children[i], func)) {
            return 1;
        }
    }
    return 0;
}

void extBuiltinRegisterCategory(const char *name) {
    if (!name) return;
    pthread_mutex_lock(&registry_mutex);
    ensureCategory(name);
    pthread_mutex_unlock(&registry_mutex);
}

void extBuiltinRegisterGroup(const char *category, const char *group) {
    if (!category) return;
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = ensureCategory(category);
    if (cat) {
        ensureGroup(cat, group);
    }
    pthread_mutex_unlock(&registry_mutex);
}

void extBuiltinRegisterFunction(const char *category, const char *group,
                                const char *func) {
    if (!category || !func) return;
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = ensureCategory(category);
    if (!cat) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    ExtBuiltinGroup *grp = ensureGroup(cat, group);
    if (!grp) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    if (stringIndexLookup(&grp->function_index, func) != SIZE_MAX) {
        pthread_mutex_unlock(&registry_mutex);
        return; /* already registered */
    }

    char *copy = strdup(func);
    if (!copy) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }

    if (!appendStringPointer(&grp->functions, &grp->function_count,
                             &grp->function_capacity, copy)) {
        free(copy);
        pthread_mutex_unlock(&registry_mutex);
        return;
    }

    size_t position = grp->function_count - 1;
    if (!stringIndexInsert(&grp->function_index, copy, position)) {
        grp->function_count--;
        grp->functions[grp->function_count] = NULL;
        free(copy);
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    pthread_mutex_unlock(&registry_mutex);
}

size_t extBuiltinGetCategoryCount(void) {
    pthread_mutex_lock(&registry_mutex);
    size_t count = registry.count;
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetCategoryName(size_t index) {
    pthread_mutex_lock(&registry_mutex);
    const char *name =
        (index < registry.count && registry.categories[index])
            ? registry.categories[index]->name
            : NULL;
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

int extBuiltinHasCategory(const char *category) {
    pthread_mutex_lock(&registry_mutex);
    int present = findCategory(category) != NULL;
    pthread_mutex_unlock(&registry_mutex);
    return present;
}

size_t extBuiltinGetGroupCount(const char *category) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    size_t count = cat ? countGroupsRecursive(cat->groups, cat->group_count) : 0;
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetGroupName(const char *category, size_t index) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    const char *name = NULL;
    if (cat) {
        size_t idx = index;
        name = getGroupNameAtRecursive(cat->groups, cat->group_count, &idx);
    }
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

int extBuiltinHasGroup(const char *category, const char *group) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    int present = 0;
    if (cat) {
        if (isDefaultGroup(group)) {
            present = cat->has_default_group;
        } else {
            ExtBuiltinGroup *grp = findGroup(cat, group);
            present = grp != NULL;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return present;
}

size_t extBuiltinGetFunctionCount(const char *category, const char *group) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    size_t count = 0;
    if (cat) {
        if (isDefaultGroup(group)) {
            count = cat->has_default_group ? cat->default_group.function_count : 0;
        } else {
            ExtBuiltinGroup *grp = findGroup(cat, group);
            count = grp ? grp->function_count : 0;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetFunctionName(const char *category, const char *group,
                                      size_t index) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    const char *name = NULL;
    if (cat) {
        if (isDefaultGroup(group)) {
            if (cat->has_default_group && index < cat->default_group.function_count) {
                name = cat->default_group.functions[index];
            }
        } else {
            ExtBuiltinGroup *grp = findGroup(cat, group);
            if (grp && index < grp->function_count) {
                name = grp->functions[index];
            }
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

int extBuiltinHasFunction(const char *category, const char *func) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    int present = 0;
    if (cat && func) {
        if (cat->has_default_group &&
            stringIndexLookup(&cat->default_group.function_index, func) !=
                SIZE_MAX) {
            present = 1;
        }
        for (size_t i = 0; i < cat->group_count && !present; ++i) {
            if (groupHasFunction(cat->groups[i], func)) {
                present = 1;
            }
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return present;
}
