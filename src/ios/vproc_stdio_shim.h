#ifndef PSCAL_VPROC_STDIO_SHIM_H
#define PSCAL_VPROC_STDIO_SHIM_H

#if defined(PSCAL_TARGET_IOS) && !defined(VPROC_SHIM_DISABLED)
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static inline int pscalStdioIsStdStream(FILE *stream) {
    return stream == stdout || stream == stderr;
}

static inline int pscalStdioFdForStream(FILE *stream) {
    return (stream == stderr) ? STDERR_FILENO : STDOUT_FILENO;
}

static inline int pscalStdioWriteAll(int fd, const void *buf, size_t count) {
    const unsigned char *bytes = (const unsigned char *)buf;
    size_t offset = 0;
    while (offset < count) {
        ssize_t written = vprocWriteShim(fd, bytes + offset, count - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (written == 0) {
            errno = EIO;
            return -1;
        }
        offset += (size_t)written;
    }
    return 0;
}

static inline int pscalLibcVfprintf(FILE *stream, const char *fmt, va_list ap) {
    return vfprintf(stream, fmt, ap);
}

static inline size_t pscalLibcFwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

static inline int pscalLibcFputs(const char *s, FILE *stream) {
    return fputs(s, stream);
}

static inline int pscalLibcFputc(int c, FILE *stream) {
    return fputc(c, stream);
}

static inline int pscalVfprintf(FILE *stream, const char *fmt, va_list ap) {
    if (!pscalStdioIsStdStream(stream)) {
        return pscalLibcVfprintf(stream, fmt, ap);
    }

    char stack_buf[1024];
    va_list copy;
    va_copy(copy, ap);
    int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, copy);
    va_end(copy);

    if (needed < 0) {
        return needed;
    }

    if ((size_t)needed < sizeof(stack_buf)) {
        if (pscalStdioWriteAll(pscalStdioFdForStream(stream), stack_buf, (size_t)needed) < 0) {
            return -1;
        }
        return needed;
    }

    size_t len = (size_t)needed;
    char *heap_buf = (char *)malloc(len + 1);
    if (!heap_buf) {
        errno = ENOMEM;
        return -1;
    }

    va_list copy2;
    va_copy(copy2, ap);
    vsnprintf(heap_buf, len + 1, fmt, copy2);
    va_end(copy2);

    int result = -1;
    if (pscalStdioWriteAll(pscalStdioFdForStream(stream), heap_buf, len) == 0) {
        result = (int)len;
    }
    free(heap_buf);
    return result;
}

static inline int pscalFprintf(FILE *stream, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = pscalVfprintf(stream, fmt, ap);
    va_end(ap);
    return res;
}

static inline int pscalVprintf(const char *fmt, va_list ap) {
    return pscalVfprintf(stdout, fmt, ap);
}

static inline int pscalPrintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int res = pscalVfprintf(stdout, fmt, ap);
    va_end(ap);
    return res;
}

static inline int pscalFputs(const char *s, FILE *stream) {
    if (!pscalStdioIsStdStream(stream)) {
        return pscalLibcFputs(s, stream);
    }
    size_t len = strlen(s);
    if (pscalStdioWriteAll(pscalStdioFdForStream(stream), s, len) < 0) {
        return EOF;
    }
    return 0;
}

static inline int pscalPuts(const char *s) {
    if (pscalFputs(s, stdout) == EOF) {
        return EOF;
    }
    if (pscalStdioWriteAll(STDOUT_FILENO, "\n", 1) < 0) {
        return EOF;
    }
    return 0;
}

static inline int pscalFputc(int c, FILE *stream) {
    if (!pscalStdioIsStdStream(stream)) {
        return pscalLibcFputc(c, stream);
    }
    unsigned char ch = (unsigned char)c;
    if (pscalStdioWriteAll(pscalStdioFdForStream(stream), &ch, 1) < 0) {
        return EOF;
    }
    return (int)ch;
}

static inline int pscalPutchar(int c) {
    return pscalFputc(c, stdout);
}

static inline size_t pscalFwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!pscalStdioIsStdStream(stream)) {
        return pscalLibcFwrite(ptr, size, nmemb, stream);
    }
    if (size == 0 || nmemb == 0) {
        return 0;
    }
    if (nmemb > (SIZE_MAX / size)) {
        errno = EOVERFLOW;
        return 0;
    }
    size_t total = size * nmemb;
    size_t written = 0;
    const unsigned char *bytes = (const unsigned char *)ptr;
    int fd = pscalStdioFdForStream(stream);

    while (written < total) {
        ssize_t chunk = vprocWriteShim(fd, bytes + written, total - written);
        if (chunk < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (chunk == 0) {
            errno = EIO;
            break;
        }
        written += (size_t)chunk;
    }
    return written / size;
}

static inline void pscalPerror(const char *label) {
    int err = errno;
    if (!label || !label[0]) {
        pscalFprintf(stderr, "%s\n", strerror(err));
        return;
    }
    pscalFprintf(stderr, "%s: %s\n", label, strerror(err));
}

#undef vfprintf
#undef fprintf
#undef vprintf
#undef printf
#undef fputs
#undef puts
#undef fputc
#undef putchar
#undef fwrite
#undef perror

#define vfprintf pscalVfprintf
#define fprintf pscalFprintf
#define vprintf pscalVprintf
#define printf pscalPrintf
#define fputs pscalFputs
#define puts pscalPuts
#define fputc pscalFputc
#define putchar pscalPutchar
#define fwrite pscalFwrite
#define perror(label) pscalPerror((label))

#endif /* PSCAL_TARGET_IOS && !VPROC_SHIM_DISABLED */

#endif /* PSCAL_VPROC_STDIO_SHIM_H */
