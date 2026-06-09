#include "aether/translate.h"

#include <stdlib.h>

static int *g_aether_rewrite_line_map = NULL;
static int g_aether_rewrite_line_count = 0;
static int g_aether_rewrite_line_cap = 0;

void aetherClearRewriteLineMap(void) {
    free(g_aether_rewrite_line_map);
    g_aether_rewrite_line_map = NULL;
    g_aether_rewrite_line_count = 0;
    g_aether_rewrite_line_cap = 0;
}

static int ensureRewriteLineCapacity(int needed) {
    int newCap;
    int *resized;

    if (g_aether_rewrite_line_cap >= needed) {
        return 1;
    }
    newCap = g_aether_rewrite_line_cap ? g_aether_rewrite_line_cap * 2 : 128;
    while (newCap < needed) {
        newCap *= 2;
    }
    resized = (int *)realloc(g_aether_rewrite_line_map, (size_t)newCap * sizeof(int));
    if (!resized) {
        return 0;
    }
    for (int i = g_aether_rewrite_line_cap; i < newCap; i++) {
        resized[i] = 0;
    }
    g_aether_rewrite_line_map = resized;
    g_aether_rewrite_line_cap = newCap;
    return 1;
}

int aetherNoteRewriteLineMapping(int rewrittenLine, int sourceLine) {
    if (rewrittenLine <= 0) {
        return 1;
    }
    if (!ensureRewriteLineCapacity(rewrittenLine + 1)) {
        return 0;
    }
    if (rewrittenLine > g_aether_rewrite_line_count) {
        g_aether_rewrite_line_count = rewrittenLine;
    }
    if (g_aether_rewrite_line_map[rewrittenLine] == 0) {
        g_aether_rewrite_line_map[rewrittenLine] = sourceLine > 0 ? sourceLine : 1;
    }
    return 1;
}

int aetherMapRewrittenLineToSource(int rewrittenLine) {
    if (rewrittenLine <= 0) {
        return rewrittenLine;
    }
    if (rewrittenLine <= g_aether_rewrite_line_count &&
        g_aether_rewrite_line_map &&
        g_aether_rewrite_line_map[rewrittenLine] > 0) {
        return g_aether_rewrite_line_map[rewrittenLine];
    }
    return rewrittenLine;
}

int aetherHasRewriteLineMap(void) {
    return g_aether_rewrite_line_count > 0;
}
