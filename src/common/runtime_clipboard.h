#pragma once

#include <stddef.h>

/* Sets the clipboard contents to the provided UTF-8 text.
 * Returns 0 on success, non-zero on failure.
 */
int runtimeClipboardSet(const char *utf8, size_t len);

/* Retrieves UTF-8 clipboard contents.
 * On success, returns a heap-allocated buffer (null-terminated) and writes length to out_len.
 * Caller must free the returned buffer. Returns NULL on failure.
 */
char *runtimeClipboardGet(size_t *out_len);

/* Convenience wrapper that fetches the clipboard as a null-terminated string.
 * Caller must free the returned buffer. Returns NULL on failure.
 */
static inline char *runtimeClipboardGetString(void) {
	return runtimeClipboardGet(NULL);
}
