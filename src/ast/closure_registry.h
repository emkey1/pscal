#ifndef CLOSURE_REGISTRY_H
#define CLOSURE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct AST;

typedef struct {
    uint8_t slot_index;
    bool is_by_ref;
} ClosureCaptureDescriptor;

typedef struct {
    struct AST **functions;
    ClosureCaptureDescriptor **descriptors;
    size_t *descriptor_counts;
    bool *captures_outer_scope;
    bool *escapes;
    size_t count;
    size_t capacity;
} ClosureCaptureRegistry;

void closureRegistryInit(ClosureCaptureRegistry *registry);
void closureRegistryReset(ClosureCaptureRegistry *registry);
void closureRegistryDestroy(ClosureCaptureRegistry *registry);
bool closureRegistryRecord(ClosureCaptureRegistry *registry,
                           struct AST *func,
                           bool captures_outer_scope,
                           const ClosureCaptureDescriptor *descriptors,
                           size_t descriptor_count,
                           bool escapes);
bool closureRegistryCaptures(const ClosureCaptureRegistry *registry, const struct AST *func);
bool closureRegistryEscapes(const ClosureCaptureRegistry *registry, const struct AST *func);
const ClosureCaptureDescriptor *closureRegistryGetDescriptors(const ClosureCaptureRegistry *registry,
                                                              const struct AST *func,
                                                              size_t *out_count);

#endif /* CLOSURE_REGISTRY_H */
