#pragma once

#include <stddef.h>

#include "ios/vproc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t snapshot_index;
    int depth;
} VProcTreeRow;

/* Build a deterministic parent/child traversal order for the supplied snapshot
 * array. The output contains snapshot indices plus depth (root=0).
 *
 * Traversal order:
 *  - grouped by session id (sid ascending)
 *  - within a session, roots sorted by pid ascending
 *  - children sorted by pid ascending
 */
size_t vprocBuildTreeRows(const VProcSnapshot *snapshots,
                          size_t snapshot_count,
                          VProcTreeRow *out,
                          size_t out_capacity);

#ifdef __cplusplus
}
#endif

