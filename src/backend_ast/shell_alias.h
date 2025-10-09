#ifndef SHELL_ALIAS_H
#define SHELL_ALIAS_H

#include <stdbool.h>

/* Returns true when an alias with the provided name exists.
 * When true, out_value receives the alias value text (never NULL).
 * The returned pointer is owned by the alias table and must not be freed. */
bool shellAliasLookup(const char *name, const char **out_value);

#endif /* SHELL_ALIAS_H */
