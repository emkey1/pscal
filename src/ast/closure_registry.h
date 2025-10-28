#ifndef CLOSURE_REGISTRY_H
#define CLOSURE_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>

struct AST;

typedef struct {
    struct AST **functions;
    bool *captures_outer_scope;
    size_t count;
    size_t capacity;
} ClosureCaptureRegistry;

void closureRegistryInit(ClosureCaptureRegistry *registry);
void closureRegistryReset(ClosureCaptureRegistry *registry);
void closureRegistryDestroy(ClosureCaptureRegistry *registry);
bool closureRegistryRecord(ClosureCaptureRegistry *registry, struct AST *func, bool captures_outer_scope);
bool closureRegistryCaptures(const ClosureCaptureRegistry *registry, const struct AST *func);

#endif /* CLOSURE_REGISTRY_H */
