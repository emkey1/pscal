#ifndef EXT_BUILTIN_REGISTRY_H
#define EXT_BUILTIN_REGISTRY_H

#include <stddef.h>

/* Register a category of extended built-in functions. */
void extBuiltinRegisterCategory(const char *name);

/* Register a named group within a category.  Groups may be specified using
 * a '/'-delimited path (e.g., "user/profiles/pat").  Intermediate groups are
 * created on demand. */
void extBuiltinRegisterGroup(const char *category, const char *group);

/* Register a function within a group. */
void extBuiltinRegisterFunction(const char *category, const char *group,
                                const char *func);

/* Query the number of registered categories. */
size_t extBuiltinGetCategoryCount(void);

/* Retrieve the name of the category at the given index (0-based).
 * Returns NULL if index is out of range. */
const char *extBuiltinGetCategoryName(size_t index);

/* Check if a category exists. */
int extBuiltinHasCategory(const char *category);

/* Query the number of groups registered for a given category.
 * Returns 0 if the category is unknown. */
size_t extBuiltinGetGroupCount(const char *category);

/* Retrieve the name of the group at the given index within a category.
 * Returns NULL if the category or index is invalid. */
const char *extBuiltinGetGroupName(const char *category, size_t index);

/* Check if a group exists within a category. */
int extBuiltinHasGroup(const char *category, const char *group);

/* Query the number of functions registered for a given group within a
 * category. Returns 0 if the category or group is unknown. */
size_t extBuiltinGetFunctionCount(const char *category, const char *group);

/* Retrieve the name of the function at the given index within a group.
 * Returns NULL if the category, group, or index is invalid. */
const char *extBuiltinGetFunctionName(const char *category, const char *group,
                                      size_t index);

/* Check if a function exists within a category. */
int extBuiltinHasFunction(const char *category, const char *func);

#endif /* EXT_BUILTIN_REGISTRY_H */
