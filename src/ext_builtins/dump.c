#include "ext_builtins/dump.h"

#include "ext_builtins/registry.h"

void extBuiltinDumpInventory(FILE *out) {
    if (!out) {
        out = stdout;
    }

    size_t category_count = extBuiltinGetCategoryCount();
    for (size_t i = 0; i < category_count; ++i) {
        const char *category = extBuiltinGetCategoryName(i);
        if (!category) {
            continue;
        }
        fprintf(out, "category %s\n", category);
        size_t fn_count = extBuiltinGetFunctionCount(category);
        for (size_t j = 0; j < fn_count; ++j) {
            const char *fn = extBuiltinGetFunctionName(category, j);
            if (!fn) {
                continue;
            }
            fprintf(out, "function %s %s\n", category, fn);
        }
    }
    fflush(out);
}
