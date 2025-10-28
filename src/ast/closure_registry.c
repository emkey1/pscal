#include "ast/closure_registry.h"

#include <stdlib.h>
#include <string.h>

void closureRegistryInit(ClosureCaptureRegistry *registry) {
    if (!registry) {
        return;
    }
    registry->functions = NULL;
    registry->descriptors = NULL;
    registry->descriptor_counts = NULL;
    registry->captures_outer_scope = NULL;
    registry->escapes = NULL;
    registry->count = 0;
    registry->capacity = 0;
}

void closureRegistryReset(ClosureCaptureRegistry *registry) {
    if (!registry) {
        return;
    }
    for (size_t i = 0; i < registry->count; i++) {
        free(registry->descriptors[i]);
        registry->descriptors[i] = NULL;
        registry->descriptor_counts[i] = 0;
        registry->captures_outer_scope[i] = false;
        registry->escapes[i] = false;
        registry->functions[i] = NULL;
    }
    registry->count = 0;
}

void closureRegistryDestroy(ClosureCaptureRegistry *registry) {
    if (!registry) {
        return;
    }
    closureRegistryReset(registry);
    free(registry->functions);
    free(registry->descriptors);
    free(registry->descriptor_counts);
    free(registry->captures_outer_scope);
    free(registry->escapes);
    registry->functions = NULL;
    registry->descriptors = NULL;
    registry->descriptor_counts = NULL;
    registry->captures_outer_scope = NULL;
    registry->escapes = NULL;
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
    ClosureCaptureDescriptor **new_descriptors = (ClosureCaptureDescriptor **)malloc(new_capacity * sizeof(ClosureCaptureDescriptor *));
    size_t *new_counts = (size_t *)malloc(new_capacity * sizeof(size_t));
    bool *new_captures = (bool *)malloc(new_capacity * sizeof(bool));
    bool *new_escapes = (bool *)malloc(new_capacity * sizeof(bool));
    if (!new_functions || !new_descriptors || !new_counts || !new_captures || !new_escapes) {
        free(new_functions);
        free(new_descriptors);
        free(new_counts);
        free(new_captures);
        free(new_escapes);
        return false;
    }

    if (registry->count > 0) {
        memcpy(new_functions, registry->functions, registry->count * sizeof(struct AST *));
        memcpy(new_descriptors, registry->descriptors, registry->count * sizeof(ClosureCaptureDescriptor *));
        memcpy(new_counts, registry->descriptor_counts, registry->count * sizeof(size_t));
        memcpy(new_captures, registry->captures_outer_scope, registry->count * sizeof(bool));
        memcpy(new_escapes, registry->escapes, registry->count * sizeof(bool));
    } else {
        memset(new_functions, 0, new_capacity * sizeof(struct AST *));
        memset(new_descriptors, 0, new_capacity * sizeof(ClosureCaptureDescriptor *));
        memset(new_counts, 0, new_capacity * sizeof(size_t));
        memset(new_captures, 0, new_capacity * sizeof(bool));
        memset(new_escapes, 0, new_capacity * sizeof(bool));
    }

    free(registry->functions);
    free(registry->descriptors);
    free(registry->descriptor_counts);
    free(registry->captures_outer_scope);
    free(registry->escapes);
    registry->functions = new_functions;
    registry->descriptors = new_descriptors;
    registry->descriptor_counts = new_counts;
    registry->captures_outer_scope = new_captures;
    registry->escapes = new_escapes;
    registry->capacity = new_capacity;
    return true;
}

static bool storeDescriptors(ClosureCaptureRegistry *registry,
                             size_t index,
                             const ClosureCaptureDescriptor *descriptors,
                             size_t descriptor_count) {
    if (!registry || index >= registry->capacity) {
        return false;
    }

    if (descriptor_count == 0) {
        if (registry->descriptors[index]) {
            free(registry->descriptors[index]);
            registry->descriptors[index] = NULL;
        }
        registry->descriptor_counts[index] = 0;
        return true;
    }

    if (!descriptors) {
        return true;
    }

    ClosureCaptureDescriptor *buffer = registry->descriptors[index];
    if (!buffer) {
        buffer = (ClosureCaptureDescriptor *)malloc(descriptor_count * sizeof(ClosureCaptureDescriptor));
    } else if (registry->descriptor_counts[index] < descriptor_count) {
        buffer = (ClosureCaptureDescriptor *)realloc(buffer, descriptor_count * sizeof(ClosureCaptureDescriptor));
    }

    if (!buffer) {
        registry->descriptor_counts[index] = 0;
        registry->descriptors[index] = NULL;
        return false;
    }

    memcpy(buffer, descriptors, descriptor_count * sizeof(ClosureCaptureDescriptor));
    registry->descriptors[index] = buffer;
    registry->descriptor_counts[index] = descriptor_count;
    return true;
}

bool closureRegistryRecord(ClosureCaptureRegistry *registry,
                           struct AST *func,
                           bool captures_outer_scope,
                           const ClosureCaptureDescriptor *descriptors,
                           size_t descriptor_count,
                           bool escapes) {
    if (!registry || !func) {
        return false;
    }

    for (size_t i = 0; i < registry->count; i++) {
        if (registry->functions[i] == func) {
            if (captures_outer_scope) {
                registry->captures_outer_scope[i] = true;
            }
            if (descriptors || descriptor_count == 0) {
                if (!storeDescriptors(registry, i, descriptors, descriptor_count)) {
                    return false;
                }
            }
            if (escapes) {
                registry->escapes[i] = true;
            }
            return true;
        }
    }

    if (!ensureClosureRegistryCapacity(registry, registry->count + 1)) {
        return false;
    }

    size_t index = registry->count;
    registry->functions[index] = func;
    registry->descriptors[index] = NULL;
    registry->descriptor_counts[index] = 0;
    registry->captures_outer_scope[index] = captures_outer_scope;
    registry->escapes[index] = escapes;
    registry->count++;

    if (descriptors || descriptor_count == 0) {
        if (!storeDescriptors(registry, index, descriptors, descriptor_count)) {
            return false;
        }
    }
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

bool closureRegistryEscapes(const ClosureCaptureRegistry *registry, const struct AST *func) {
    if (!registry || !func) {
        return false;
    }
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->functions[i] == func) {
            return registry->escapes[i];
        }
    }
    return false;
}

const ClosureCaptureDescriptor *closureRegistryGetDescriptors(const ClosureCaptureRegistry *registry,
                                                              const struct AST *func,
                                                              size_t *out_count) {
    if (!registry || !func) {
        if (out_count) {
            *out_count = 0;
        }
        return NULL;
    }
    for (size_t i = 0; i < registry->count; i++) {
        if (registry->functions[i] == func) {
            if (out_count) {
                *out_count = registry->descriptor_counts[i];
            }
            return registry->descriptors[i];
        }
    }
    if (out_count) {
        *out_count = 0;
    }
    return NULL;
}
