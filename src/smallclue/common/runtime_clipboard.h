#ifndef SMALLCLUE_RUNTIME_CLIPBOARD_H
#define SMALLCLUE_RUNTIME_CLIPBOARD_H

#include <stddef.h>

/* Simple in-process clipboard used by pbcopy/pbpaste. */
int runtimeClipboardSet(const char *data, size_t len);
char *runtimeClipboardGet(size_t *len_out);

#endif /* SMALLCLUE_RUNTIME_CLIPBOARD_H */
