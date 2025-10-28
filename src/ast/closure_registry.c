#include "ast/closure_registry.h"

#include <stdlib.h>
#include <string.h>

void closureRegistryInit(ClosureCaptureRegistry *registry) {
    if (!registry) {
        return;
    }
    registry->functions = NULL;
    registry->captures_outer_scope = NULL;
    registry->count = 0;
    registry->capacity = 0;
}

void closureRegistryReset(ClosureCaptureRegistry *registry) {
    if (!registry) {
        return;
    }
    registry->count = 0;
}

void closureRegistryDestroy(ClosureCaptureRegistry *registry) {
    if (!registry) {
        return;
    }
    free(registry->functions);
    free(registry->captures_outer_scope);
    registry->functions = NULL;
    registry->captures_outer_scope = NULL;
    registry->count = 0;
    registry->capacity = 0;
}

static bool ensureClosureRegistryCapacity(ClosureCaptureRegistry *registry, size_t needed) {
    if (!registry) {
        return false;
    }
    if (registry->capacity >= needed) {
        return true;
    }

    size_t new_capacity = registry->capacity ? registry->capacity * 2 : 16;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    struct AST **new_functions = (struct AST **)malloc(new_capacity * sizeof(struct AST *));
    bool *new_captures = (bool *)malloc(new_capacity * sizeof(bool));
    if (!new_functions || !new_captures) {
        free(new_functions);
        free(new_captures);
        return false;
    }

    if (registry->count > 0) {
        memcpy(new_functions, registry->functions, registry->count * sizeof(struct AST *));
        memcpy(new_captures, registry->captures_outer_scope, registry->count * sizeof(bool));
    }

    free(registry->functions);
    free(registry->captures_outer_scope);
    registry->functions = new_functions;
    registry->captures_outer_scope = new_captures;
    registry->capacity = new_capacity;
    return true;
}

bool closureRegistryRecord(ClosureCaptureRegistry *registry, struct AST *func, bool captures_outer_scope) {
    if (!registry || !func) {
        return false;
    }

    for (size_t i = 0; i < registry->count; i++) {
        if (registry->functions[i] == func) {
            if (captures_outer_scope) {
                registry->captures_outer_scope[i] = true;
            }
            return true;
        }
    }

    if (!ensureClosureRegistryCapacity(registry, registry->count + 1)) {
        return false;
    }

    registry->functions[registry->count] = func;
    registry->captures_outer_scope[registry->count] = captures_outer_scope;
    registry->count++;
    return true;
}

bool closureRegistryCaptures(const ClosureCaptureRegistry *registry, const struct AST *func) {
    if (!registry || !func) {
        return false;
    }
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->functions[i] == func) {
            return registry->captures_outer_scope[i];
        }
    }
    return false;
}
