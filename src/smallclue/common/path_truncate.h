#ifndef SMALLCLUE_PATH_TRUNCATE_H
#define SMALLCLUE_PATH_TRUNCATE_H

#include <stdbool.h>
#include <stddef.h>

/* Platform shim: optionally expand a virtualized path into a real path. */
bool pathTruncateExpand(const char *path, char *buffer, size_t buflen);

#endif /* SMALLCLUE_PATH_TRUNCATE_H */
