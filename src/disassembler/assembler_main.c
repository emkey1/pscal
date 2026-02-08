/*
 * MIT License
 *
 * Copyright (c) 2024 PSCAL contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Note: PSCAL versions prior to 2.22 were released under the Unlicense.
 */

#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/frontend_kind.h"

static const char *PSCALASM_USAGE =
    "Usage: pscalasm <disassembly.txt|-> <output.pbc>\n"
    "       pscald --asm <input.pbc> 2> dump.txt\n"
    "       pscalasm dump.txt rebuilt.pbc\n";

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

static int append_byte(uint8_t **buffer, size_t *count, size_t *capacity, uint8_t value) {
    if (*count >= *capacity) {
        size_t new_capacity = (*capacity == 0) ? 4096u : (*capacity * 2u);
        uint8_t *new_buffer = (uint8_t *)realloc(*buffer, new_capacity);
        if (!new_buffer) {
            return 0;
        }
        *buffer = new_buffer;
        *capacity = new_capacity;
    }
    (*buffer)[(*count)++] = value;
    return 1;
}

static int parse_pscalasm_block(FILE *in, uint8_t **out_bytes, size_t *out_len) {
    char line[8192];
    int in_block = 0;
    int in_hex = 0;
    int found_block = 0;
    long long expected_bytes = -1;

    uint8_t *bytes = NULL;
    size_t count = 0;
    size_t capacity = 0;

    while (fgets(line, sizeof(line), in) != NULL) {
        if (!in_block) {
            if (strstr(line, "== PSCALASM BEGIN v1 ==") != NULL) {
                in_block = 1;
                found_block = 1;
            }
            continue;
        }

        if (strstr(line, "== PSCALASM END ==") != NULL) {
            break;
        }

        if (strncmp(line, "bytes:", 6) == 0) {
            const char *p = line + 6;
            while (*p != '\0' && isspace((unsigned char)*p)) {
                ++p;
            }
            if (*p != '\0') {
                char *end = NULL;
                errno = 0;
                long long parsed = strtoll(p, &end, 10);
                if (errno == 0 && end != p && parsed >= 0) {
                    expected_bytes = parsed;
                }
            }
            continue;
        }

        if (strncmp(line, "hex:", 4) == 0) {
            in_hex = 1;
            continue;
        }

        if (!in_hex) {
            continue;
        }

        for (const char *p = line; *p != '\0'; ) {
            while (*p != '\0' && !isxdigit((unsigned char)*p)) {
                ++p;
            }
            if (*p == '\0') {
                break;
            }

            int hi = hex_nibble(*p);
            if (hi < 0) {
                ++p;
                continue;
            }
            ++p;

            while (*p != '\0' && !isxdigit((unsigned char)*p)) {
                ++p;
            }
            if (*p == '\0') {
                break;
            }

            int lo = hex_nibble(*p);
            if (lo < 0) {
                ++p;
                continue;
            }
            ++p;

            if (!append_byte(&bytes, &count, &capacity, (uint8_t)((hi << 4) | lo))) {
                free(bytes);
                return 0;
            }
        }
    }

    if (!found_block) {
        fprintf(stderr, "pscalasm: no PSCALASM block found (run `pscald --asm ...`).\n");
        free(bytes);
        return 0;
    }

    if (expected_bytes >= 0 && (size_t)expected_bytes != count) {
        fprintf(stderr,
                "pscalasm: byte count mismatch (header=%lld parsed=%zu).\n",
                expected_bytes,
                count);
        free(bytes);
        return 0;
    }

    *out_bytes = bytes;
    *out_len = count;
    return 1;
}

static int write_output_file(const char *path, const uint8_t *bytes, size_t len) {
    FILE *out = fopen(path, "wb");
    if (!out) {
        fprintf(stderr, "pscalasm: cannot open output '%s': %s\n", path, strerror(errno));
        return 0;
    }

    if (len > 0) {
        size_t written = fwrite(bytes, 1, len, out);
        if (written != len) {
            fprintf(stderr, "pscalasm: short write to '%s'.\n", path);
            fclose(out);
            return 0;
        }
    }

    if (fclose(out) != 0) {
        fprintf(stderr, "pscalasm: failed to close '%s': %s\n", path, strerror(errno));
        return 0;
    }

    return 1;
}

int pscalasm_main(int argc, char **argv) {
    FrontendKind previousKind = frontendPushKind(FRONTEND_KIND_PASCAL);
#define PSCALASM_RETURN(value)         \
    do {                                \
        int __pscalasm_rc = (value);    \
        frontendPopKind(previousKind);  \
        return __pscalasm_rc;           \
    } while (0)

    if (argc == 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("%s", PSCALASM_USAGE);
        PSCALASM_RETURN(EXIT_SUCCESS);
    }

    if (argc != 3) {
        fprintf(stderr, "%s", PSCALASM_USAGE);
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    const char *input_path = argv[1];
    const char *output_path = argv[2];

    FILE *in = NULL;
    if (strcmp(input_path, "-") == 0) {
        in = stdin;
    } else {
        in = fopen(input_path, "rb");
        if (!in) {
            fprintf(stderr, "pscalasm: cannot open input '%s': %s\n", input_path, strerror(errno));
            PSCALASM_RETURN(EXIT_FAILURE);
        }
    }

    uint8_t *bytes = NULL;
    size_t len = 0;
    int ok = parse_pscalasm_block(in, &bytes, &len);

    if (in != stdin) {
        fclose(in);
    }

    if (!ok) {
        free(bytes);
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    ok = write_output_file(output_path, bytes, len);
    free(bytes);
    if (!ok) {
        PSCALASM_RETURN(EXIT_FAILURE);
    }

    PSCALASM_RETURN(EXIT_SUCCESS);
}
#undef PSCALASM_RETURN

#ifndef PSCAL_NO_CLI_ENTRYPOINTS
int main(int argc, char **argv) {
    return pscalasm_main(argc, argv);
}
#endif
