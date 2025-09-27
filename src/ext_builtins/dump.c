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

        if (extBuiltinHasGroup(category, NULL)) {
            const char *default_group = "default";
            fprintf(out, "group %s %s\n", category, default_group);
            size_t fn_count = extBuiltinGetFunctionCount(category, NULL);
            for (size_t j = 0; j < fn_count; ++j) {
                const char *fn = extBuiltinGetFunctionName(category, NULL, j);
                if (!fn) {
                    continue;
                }
                fprintf(out, "function %s %s %s\n", category, default_group, fn);
            }
        }

        size_t group_count = extBuiltinGetGroupCount(category);
        for (size_t g = 0; g < group_count; ++g) {
            const char *group = extBuiltinGetGroupName(category, g);
            if (!group) {
                continue;
            }
            fprintf(out, "group %s %s\n", category, group);
            size_t fn_count = extBuiltinGetFunctionCount(category, group);
            for (size_t j = 0; j < fn_count; ++j) {
                const char *fn =
                    extBuiltinGetFunctionName(category, group, j);
                if (!fn) {
                    continue;
                }
                fprintf(out, "function %s %s %s\n", category, group, fn);
            }
        }
    }
    fflush(out);
}
