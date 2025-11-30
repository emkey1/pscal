#include "runtime_clipboard.h"

#include <stdlib.h>
#include <string.h>

/* Platform hooks; may be provided by the host (e.g., iOS/macOS bridge). */
int pscalPlatformClipboardSet(const char *utf8, size_t len) __attribute__((weak));
char *pscalPlatformClipboardGet(size_t *out_len) __attribute__((weak));

static char *clipboard_fallback = NULL;
static size_t clipboard_fallback_len = 0;

/* Default stubs when no platform implementation is linked. */
int pscalPlatformClipboardSet(const char *utf8, size_t len) {
	(void)utf8;
	(void)len;
	return -1;
}

char *pscalPlatformClipboardGet(size_t *out_len) {
	(void)out_len;
	return NULL;
}

int runtimeClipboardSet(const char *utf8, size_t len) {
	if (!utf8)
		return -1;
	if (pscalPlatformClipboardSet) {
		if (pscalPlatformClipboardSet(utf8, len) == 0) {
			return 0;
		}
	}
	free(clipboard_fallback);
	clipboard_fallback = (char *)malloc(len + 1);
	if (!clipboard_fallback)
		return -1;
	memcpy(clipboard_fallback, utf8, len);
	clipboard_fallback[len] = '\0';
	clipboard_fallback_len = len;
	return 0;
}

char *runtimeClipboardGet(size_t *out_len) {
	if (pscalPlatformClipboardGet) {
		char *buf = pscalPlatformClipboardGet(out_len);
		if (buf)
			return buf;
	}
	if (!clipboard_fallback)
		return NULL;
	char *buf = (char *)malloc(clipboard_fallback_len + 1);
	if (!buf)
		return NULL;
	memcpy(buf, clipboard_fallback, clipboard_fallback_len + 1);
	if (out_len)
		*out_len = clipboard_fallback_len;
	return buf;
}
