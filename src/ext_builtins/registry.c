#include "registry.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <pthread.h>

typedef struct {
    char *name; /* NULL indicates the default/ungrouped bucket. */
    char **functions;
    size_t function_count;
} ExtBuiltinGroup;

typedef struct {
    char *name;
    ExtBuiltinGroup *groups;
    size_t group_count;
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

static int groupNameEquals(const ExtBuiltinGroup *group, const char *name) {
    if (!group) return 0;
    if (isDefaultGroup(name)) {
        return group->name == NULL;
    }
    if (!group->name) {
        return 0;
    }
    return strcasecmp(group->name, name) == 0;
}

static ExtBuiltinGroup *findGroup(ExtBuiltinCategory *category,
                                  const char *group_name) {
    if (!category) return NULL;
    for (size_t i = 0; i < category->group_count; ++i) {
        if (groupNameEquals(&category->groups[i], group_name)) {
            return &category->groups[i];
        }
    }
    return NULL;
}

static ExtBuiltinGroup *ensureGroup(ExtBuiltinCategory *category,
                                    const char *group_name) {
    if (!category) return NULL;
    ExtBuiltinGroup *group = findGroup(category, group_name);
    if (group) return group;

    ExtBuiltinGroup *new_groups =
        realloc(category->groups,
                sizeof(ExtBuiltinGroup) * (category->group_count + 1));
    if (!new_groups) {
        return NULL;
    }
    category->groups = new_groups;
    group = &category->groups[category->group_count];
    group->name = NULL;
    group->functions = NULL;
    group->function_count = 0;
    if (!isDefaultGroup(group_name)) {
        group->name = strdup(group_name);
        if (!group->name) {
            ExtBuiltinGroup *shrunk =
                realloc(category->groups,
                        sizeof(ExtBuiltinGroup) * category->group_count);
            if (shrunk || category->group_count == 0) {
                category->groups = shrunk;
            }
            return NULL;
        }
    }
    category->group_count++;
    return group;
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
    size_t count = 0;
    if (cat) {
        for (size_t i = 0; i < cat->group_count; ++i) {
            if (cat->groups[i].name) {
                ++count;
            }
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetGroupName(const char *category, size_t index) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    const char *name = NULL;
    if (cat) {
        size_t seen = 0;
        for (size_t i = 0; i < cat->group_count; ++i) {
            if (!cat->groups[i].name) {
                continue;
            }
            if (seen == index) {
                name = cat->groups[i].name;
                break;
            }
            ++seen;
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

int extBuiltinHasGroup(const char *category, const char *group) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    int present = 0;
    if (cat) {
        ExtBuiltinGroup *grp = findGroup(cat, group);
        present = grp != NULL;
    }
    pthread_mutex_unlock(&registry_mutex);
    return present;
}

size_t extBuiltinGetFunctionCount(const char *category, const char *group) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    ExtBuiltinGroup *grp = findGroup(cat, group);
    size_t count = grp ? grp->function_count : 0;
    pthread_mutex_unlock(&registry_mutex);
    return count;
}

const char *extBuiltinGetFunctionName(const char *category, const char *group,
                                      size_t index) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    ExtBuiltinGroup *grp = findGroup(cat, group);
    const char *name =
        (!grp || index >= grp->function_count) ? NULL : grp->functions[index];
    pthread_mutex_unlock(&registry_mutex);
    return name;
}

int extBuiltinHasFunction(const char *category, const char *func) {
    pthread_mutex_lock(&registry_mutex);
    ExtBuiltinCategory *cat = findCategory(category);
    int present = 0;
    if (cat && func) {
        for (size_t i = 0; i < cat->group_count && !present; ++i) {
            ExtBuiltinGroup *grp = &cat->groups[i];
            for (size_t j = 0; j < grp->function_count; ++j) {
                if (strcasecmp(grp->functions[j], func) == 0) {
                    present = 1;
                    break;
                }
            }
        }
    }
    pthread_mutex_unlock(&registry_mutex);
    return present;
}
