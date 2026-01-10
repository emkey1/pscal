#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Returns true when PATH_TRUNCATE is set to a non-empty absolute prefix.
 */
bool pathTruncateEnabled(void);

/**
 * Removes the PATH_TRUNCATE prefix from `absolute_path` and writes the user-facing
 * representation into `out`. When the prefix does not match or PATH_TRUNCATE is
 * disabled the original string is copied verbatim.
 */
bool pathTruncateStrip(const char *absolute_path, char *out, size_t out_size);

/**
 * Expands user supplied paths into the filesystem path by re-applying the
 * PATH_TRUNCATE prefix when necessary. Relative paths and absolute paths that
 * already contain the full prefix are returned unchanged.
 */
bool pathTruncateExpand(const char *input_path, char *out, size_t out_size);

/**
 * Applies the PATH_TRUNCATE environment variable using the provided prefix.
 * Passing NULL clears PATH_TRUNCATE entirely. Internal caches are flushed so
 * subsequent path lookups observe the new value immediately.
 */
void pathTruncateApplyEnvironment(const char *prefix);

/* Ensures the emulated /dev directory under the truncated prefix exists and
 * populates symlinks for /dev/null and /dev/zero that point to the real
 * system devices. */
void pathTruncateProvisionDev(const char *prefix);

/* Ensures a minimal /proc tree under the truncated prefix and generates
 * a lightweight cpuinfo file for compatibility. */
void pathTruncateProvisionProc(const char *prefix);

#ifdef __cplusplus
}
#endif
