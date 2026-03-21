#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char **entries;
    size_t count;
} PscalEnvSnapshot;

bool pscalEnvSnapshotTake(PscalEnvSnapshot *snapshot);
bool pscalEnvSnapshotRestore(PscalEnvSnapshot *snapshot);
void pscalEnvSnapshotClear(PscalEnvSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
