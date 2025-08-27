#ifndef EXT_BUILTIN_REGISTRY_H
#define EXT_BUILTIN_REGISTRY_H

#include <stddef.h>

/* Register a category of extended built-in functions. */
void extBuiltinRegisterCategory(const char *name);

/* Register a function within a category. */
void extBuiltinRegisterFunction(const char *category, const char *func);

/* Query the number of registered categories. */
size_t extBuiltinGetCategoryCount(void);

/* Retrieve the name of the category at the given index (0-based).
 * Returns NULL if index is out of range. */
const char *extBuiltinGetCategoryName(size_t index);

/* Query the number of functions registered for a given category.
 * Returns 0 if the category is unknown. */
size_t extBuiltinGetFunctionCount(const char *category);

/* Retrieve the name of the function at the given index within a category.
 * Returns NULL if the category or index is invalid. */
const char *extBuiltinGetFunctionName(const char *category, size_t index);

/* Check if a function exists within a category. */
int extBuiltinHasFunction(const char *category, const char *func);

#endif /* EXT_BUILTIN_REGISTRY_H */
