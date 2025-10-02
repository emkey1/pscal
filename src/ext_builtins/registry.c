#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

typedef struct ExtBuiltinGroup_s {
    char *name; /* NULL indicates the default/ungrouped bucket. */
    char *full_name; /* Cached fully qualified group name for nested groups. */
    char **functions;
    size_t function_count;
    struct ExtBuiltinGroup_s *children;
    size_t child_count;
} ExtBuiltinGroup;

typedef struct {
    char *name;
    ExtBuiltinGroup *groups;
    size_t group_count;
    ExtBuiltinGroup default_group;
    int has_default_group;
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
    ExtBuiltinCategory *new_array =
        realloc(categories, sizeof(ExtBuiltinCategory) * (category_count + 1));
    if (!new_array) return NULL;
    categories = new_array;
    cat = &categories[category_count];
    cat->name = NULL;
    cat->groups = NULL;
    cat->group_count = 0;
    cat->default_group.name = NULL;
    cat->default_group.full_name = NULL;
    cat->default_group.functions = NULL;
    cat->default_group.function_count = 0;
    cat->default_group.children = NULL;
    cat->default_group.child_count = 0;
    cat->has_default_group = 0;

    const char *source_name = name ? name : "";
    cat->name = strdup(source_name);
    if (!cat->name) {
        ExtBuiltinCategory *shrunk =
            realloc(categories, sizeof(ExtBuiltinCategory) * category_count);
        if (shrunk || category_count == 0) {
            categories = shrunk;
        }
        return NULL;
    }

    category_count++;
    return cat;
}

static int isDefaultGroup(const char *group) {
    return !group || *group == '\0';
}

static int segmentEqualsLen(const char *component, size_t component_len,
                            const char *name) {
    if (!component || !name) return 0;
    size_t name_len = strlen(name);
    if (name_len != component_len) {
        return 0;
    }
    return strncasecmp(name, component, component_len) == 0;
}

static ExtBuiltinGroup *findChild(ExtBuiltinGroup *groups, size_t count,
                                  const char *component, size_t component_len) {
    if (!groups || !component || component_len == 0) {
        return NULL;
    }
    for (size_t i = 0; i < count; ++i) {
        if (segmentEqualsLen(component, component_len, groups[i].name)) {
            return &groups[i];
        }
    }
    return NULL;
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

static ExtBuiltinGroup *ensureChild(ExtBuiltinGroup **groups, size_t *count,
                                    ExtBuiltinGroup *parent,
                                    const char *component,
                                    size_t component_len) {
    if (!groups || !count || !component || component_len == 0) {
        return NULL;
    }
    ExtBuiltinGroup *existing = findChild(*groups, *count, component, component_len);
    if (existing) {
        return existing;
    }

    ExtBuiltinGroup *new_groups =
        realloc(*groups, sizeof(ExtBuiltinGroup) * (*count + 1));
    if (!new_groups) {
        return NULL;
    }
    *groups = new_groups;
    ExtBuiltinGroup *child = &new_groups[*count];
    child->name = NULL;
    child->full_name = NULL;
    child->functions = NULL;
    child->function_count = 0;
    child->children = NULL;
    child->child_count = 0;

    child->name = dupComponent(component, component_len);
    if (!child->name) {
        ExtBuiltinGroup *shrunk = realloc(*groups, sizeof(ExtBuiltinGroup) * (*count));
        if (shrunk || *count == 0) {
            *groups = shrunk;
        }
        return NULL;
    }
    child->full_name = buildFullName(parent, component);
    if (!child->full_name) {
        free(child->name);
        child->name = NULL;
        ExtBuiltinGroup *shrunk = realloc(*groups, sizeof(ExtBuiltinGroup) * (*count));
        if (shrunk || *count == 0) {
            *groups = shrunk;
        }
        return NULL;
    }

    (*count)++;
    return child;
}

static ExtBuiltinGroup *findGroup(ExtBuiltinCategory *category,
                                  const char *group_name) {
    if (!category || isDefaultGroup(group_name)) {
        return NULL;
    }
    const char *cursor = group_name;
    ExtBuiltinGroup *current = NULL;
    ExtBuiltinGroup *children = category->groups;
    size_t child_count = category->group_count;
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
        current = findChild(children, child_count, start, len);
        if (!current) {
            return NULL;
        }
        children = current->children;
        child_count = current->child_count;
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
    ExtBuiltinGroup **children = &category->groups;
    size_t *child_count = &category->group_count;

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
        ExtBuiltinGroup *next =
            ensureChild(children, child_count, current, start, len);
        if (!next) {
            return NULL;
        }
        current = next;
        children = &current->children;
        child_count = &current->child_count;
    }
    return current;
}

static size_t countGroupsRecursive(const ExtBuiltinGroup *groups,
                                   size_t count) {
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        ++total;
        total += countGroupsRecursive(groups[i].children, groups[i].child_count);
    }
    return total;
}

static const char *getGroupNameAtRecursive(const ExtBuiltinGroup *groups,
                                           size_t count, size_t *index) {
    for (size_t i = 0; i < count; ++i) {
        if (*index == 0) {
            return groups[i].full_name;
        }
        --(*index);
        const char *child =
            getGroupNameAtRecursive(groups[i].children, groups[i].child_count,
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
    for (size_t j = 0; j < group->function_count; ++j) {
        if (strcasecmp(group->functions[j], func) == 0) {
            return 1;
        }
    }
    for (size_t i = 0; i < group->child_count; ++i) {
        if (groupHasFunction(&group->children[i], func)) {
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
    for (size_t i = 0; i < grp->function_count; ++i) {
        if (strcasecmp(grp->functions[i], func) == 0) {
            pthread_mutex_unlock(&registry_mutex);
            return; /* already registered */
        }
    }
    char **new_funcs = realloc(grp->functions,
                               sizeof(char *) * (grp->function_count + 1));
    if (!new_funcs) {
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    grp->functions = new_funcs;
    grp->functions[grp->function_count] = NULL;
    char *copy = strdup(func);
    if (!copy) {
        char **shrunk =
            realloc(grp->functions, sizeof(char *) * grp->function_count);
        if (shrunk || grp->function_count == 0) {
            grp->functions = shrunk;
        }
        pthread_mutex_unlock(&registry_mutex);
        return;
    }
    grp->functions[grp->function_count] = copy;
    grp->function_count++;
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
        if (cat->has_default_group) {
            for (size_t j = 0; j < cat->default_group.function_count; ++j) {
                if (strcasecmp(cat->default_group.functions[j], func) == 0) {
                    present = 1;
                    break;
                }
            }
        }
        for (size_t i = 0; i < cat->group_count && !present; ++i) {
            if (groupHasFunction(&cat->groups[i], func)) {
                present = 1;
            }
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return present;
}
