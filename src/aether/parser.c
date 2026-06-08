#include "aether/parser.h"

#include <stdlib.h>

#include "aether/semantic.h"
#include "aether/translate.h"
#include "rea/parser.h"

static char *g_aether_last_source = NULL;

static void aetherRememberSource(const char *source) {
    size_t len;
    char *copy;

    free(g_aether_last_source);
    g_aether_last_source = NULL;
    if (!source) {
        return;
    }
    len = strlen(source);
    copy = (char *)malloc(len + 1);
    if (!copy) {
        return;
    }
    memcpy(copy, source, len + 1);
    g_aether_last_source = copy;
}

AST *parseAether(const char *source) {
    char *rewritten;
    AST *ast;
    const char *sourcePath;

    if (!source) {
        return NULL;
    }

    aetherRememberSource(source);
    sourcePath = aetherSemanticGetSourcePath();
    rewritten = aetherRewriteSource(source, sourcePath);
    if (!rewritten) {
        return NULL;
    }

    ast = parseRea(rewritten);
    free(rewritten);
    return ast;
}

void aetherSetStrictMode(int enable) {
    reaSetStrictMode(enable);
}

const char *aetherGetLastSource(void) {
    return g_aether_last_source;
}

void aetherClearLastSource(void) {
    free(g_aether_last_source);
    g_aether_last_source = NULL;
}
