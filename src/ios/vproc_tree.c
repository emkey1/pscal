#include "ios/vproc_tree.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    int pid;
    int parent_pid;
    int sid;
    size_t snap_index;
} VProcTreeNode;

typedef struct {
    int key_pid;
    size_t node_index;
} VProcPidIndex;

static int cmpPidIndex(const void *a, const void *b) {
    const VProcPidIndex *pa = (const VProcPidIndex *)a;
    const VProcPidIndex *pb = (const VProcPidIndex *)b;
    if (pa->key_pid < pb->key_pid) return -1;
    if (pa->key_pid > pb->key_pid) return 1;
    return 0;
}

static int cmpIntAsc(const void *a, const void *b) {
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    if (ia < ib) return -1;
    if (ia > ib) return 1;
    return 0;
}

static const VProcTreeNode *gVprocTreeSortNodes = NULL;

static int cmpNodeIndexByPid(const void *a, const void *b) {
    const VProcTreeNode *nodes = gVprocTreeSortNodes;
    size_t ia = *(const size_t *)a;
    size_t ib = *(const size_t *)b;
    int pa = nodes[ia].pid;
    int pb = nodes[ib].pid;
    if (pa < pb) return -1;
    if (pa > pb) return 1;
    return 0;
}

static size_t findNodeByPid(const VProcPidIndex *index,
                            size_t index_count,
                            int pid,
                            bool *found) {
    if (found) *found = false;
    if (!index || index_count == 0 || pid <= 0) {
        return 0;
    }
    VProcPidIndex key = {.key_pid = pid, .node_index = 0};
    const VProcPidIndex *res = (const VProcPidIndex *)bsearch(&key, index, index_count,
                                                              sizeof(VProcPidIndex), cmpPidIndex);
    if (!res) {
        return 0;
    }
    if (found) *found = true;
    return res->node_index;
}

static void treeVisit(size_t node_index,
                      int depth,
                      const VProcTreeNode *nodes,
                      const size_t *children,
                      const size_t *child_offsets,
                      const size_t *child_counts,
                      bool *visited,
                      VProcTreeRow *out,
                      size_t out_capacity,
                      size_t *out_count) {
    if (!nodes || !visited || !out_count) {
        return;
    }
    if (visited[node_index]) {
        return;
    }
    visited[node_index] = true;

    if (out && *out_count < out_capacity) {
        out[*out_count].snapshot_index = nodes[node_index].snap_index;
        out[*out_count].depth = depth;
    }
    (*out_count)++;

    size_t begin = child_offsets[node_index];
    size_t count = child_counts[node_index];
    for (size_t i = 0; i < count; ++i) {
        size_t child_node = children[begin + i];
        treeVisit(child_node, depth + 1, nodes, children, child_offsets, child_counts,
                  visited, out, out_capacity, out_count);
    }
}

size_t vprocBuildTreeRows(const VProcSnapshot *snapshots,
                          size_t snapshot_count,
                          VProcTreeRow *out,
                          size_t out_capacity) {
    if (!snapshots || snapshot_count == 0) {
        return 0;
    }

    VProcTreeNode *nodes = (VProcTreeNode *)calloc(snapshot_count, sizeof(VProcTreeNode));
    VProcPidIndex *pid_index = (VProcPidIndex *)calloc(snapshot_count, sizeof(VProcPidIndex));
    int *sids = (int *)calloc(snapshot_count, sizeof(int));
    bool *visited = (bool *)calloc(snapshot_count, sizeof(bool));
    if (!nodes || !pid_index || !sids || !visited) {
        free(nodes);
        free(pid_index);
        free(sids);
        free(visited);
        return 0;
    }

    size_t node_count = 0;
    for (size_t i = 0; i < snapshot_count; ++i) {
        const VProcSnapshot *snap = &snapshots[i];
        if (!snap || snap->pid <= 0) {
            continue;
        }
        nodes[node_count].pid = snap->pid;
        nodes[node_count].parent_pid = snap->parent_pid;
        nodes[node_count].sid = snap->sid;
        nodes[node_count].snap_index = i;

        pid_index[node_count].key_pid = snap->pid;
        pid_index[node_count].node_index = node_count;

        sids[node_count] = snap->sid;
        node_count++;
    }

    if (node_count == 0) {
        free(nodes);
        free(pid_index);
        free(sids);
        free(visited);
        return 0;
    }

    qsort(pid_index, node_count, sizeof(VProcPidIndex), cmpPidIndex);
    qsort(sids, node_count, sizeof(int), cmpIntAsc);

    /* Deduplicate session ids. */
    size_t sid_count = 0;
    for (size_t i = 0; i < node_count; ++i) {
        if (i == 0 || sids[i] != sids[i - 1]) {
            sids[sid_count++] = sids[i];
        }
    }

    /* Build parent pointers and child counts. */
    size_t *parent_node = (size_t *)calloc(node_count, sizeof(size_t));
    bool *has_parent = (bool *)calloc(node_count, sizeof(bool));
    size_t *child_counts = (size_t *)calloc(node_count, sizeof(size_t));
    if (!parent_node || !has_parent || !child_counts) {
        free(nodes);
        free(pid_index);
        free(sids);
        free(visited);
        free(parent_node);
        free(has_parent);
        free(child_counts);
        return 0;
    }

    for (size_t i = 0; i < node_count; ++i) {
        bool ok = false;
        size_t pidx = findNodeByPid(pid_index, node_count, nodes[i].parent_pid, &ok);
        if (ok && pidx < node_count &&
            pidx != i) {
            has_parent[i] = true;
            parent_node[i] = pidx;
            child_counts[pidx]++;
        }
    }

    /* Compute child offsets and allocate child list. */
    size_t *child_offsets = (size_t *)calloc(node_count, sizeof(size_t));
    if (!child_offsets) {
        free(nodes);
        free(pid_index);
        free(sids);
        free(visited);
        free(parent_node);
        free(has_parent);
        free(child_counts);
        return 0;
    }

    size_t total_children = 0;
    for (size_t i = 0; i < node_count; ++i) {
        child_offsets[i] = total_children;
        total_children += child_counts[i];
    }

    size_t *children = (size_t *)calloc(total_children ? total_children : 1, sizeof(size_t));
    size_t *child_fill = (size_t *)calloc(node_count, sizeof(size_t));
    if (!children || !child_fill) {
        free(nodes);
        free(pid_index);
        free(sids);
        free(visited);
        free(parent_node);
        free(has_parent);
        free(child_counts);
        free(child_offsets);
        free(children);
        free(child_fill);
        return 0;
    }

    for (size_t i = 0; i < node_count; ++i) {
        if (!has_parent[i]) {
            continue;
        }
        size_t p = parent_node[i];
        size_t pos = child_offsets[p] + child_fill[p]++;
        children[pos] = i;
    }

    /* Sort each parent's children list by pid for stable output. */
    for (size_t i = 0; i < node_count; ++i) {
        size_t begin = child_offsets[i];
        size_t count = child_counts[i];
        if (count <= 1) {
            continue;
        }
        gVprocTreeSortNodes = nodes;
        qsort(children + begin, count, sizeof(size_t), cmpNodeIndexByPid);
    }

    /* Walk sessions in sid order; within each session, visit roots in pid order. */
    size_t out_count = 0;
    for (size_t s = 0; s < sid_count; ++s) {
        int sid = sids[s];
        size_t roots_cap = 0;
        for (size_t i = 0; i < node_count; ++i) {
            if (nodes[i].sid != sid) continue;
            if (!has_parent[i]) roots_cap++;
        }
        if (roots_cap == 0) {
            continue;
        }
        size_t *roots = (size_t *)calloc(roots_cap, sizeof(size_t));
        if (!roots) {
            continue;
        }
        size_t roots_count = 0;
        for (size_t i = 0; i < node_count; ++i) {
            if (nodes[i].sid != sid) continue;
            if (!has_parent[i]) roots[roots_count++] = i;
        }
        gVprocTreeSortNodes = nodes;
        qsort(roots, roots_count, sizeof(size_t), cmpNodeIndexByPid);
        for (size_t r = 0; r < roots_count; ++r) {
            treeVisit(roots[r], 0, nodes, children, child_offsets, child_counts,
                      visited, out, out_capacity, &out_count);
        }
        free(roots);
    }

    gVprocTreeSortNodes = NULL;
    free(nodes);
    free(pid_index);
    free(sids);
    free(visited);
    free(parent_node);
    free(has_parent);
    free(child_counts);
    free(child_offsets);
    free(children);
    free(child_fill);

    /* If the caller's output buffer was too small, return the required count. */
    return out_count;
}
