#ifndef SHELL_BUFFER_H
#define SHELL_BUFFER_H

#include <stdbool.h>
#include <stddef.h>

bool shellBufferEnsure(char **buffer, size_t *length, size_t *capacity, size_t extra);
void shellBufferAppendChar(char **buffer, size_t *length, size_t *capacity, char c);
void shellBufferAppendString(char **buffer, size_t *length, size_t *capacity, const char *str);
void shellBufferAppendSlice(char **buffer,
                            size_t *length,
                            size_t *capacity,
                            const char *data,
                            size_t slice_len);

#endif /* SHELL_BUFFER_H */
