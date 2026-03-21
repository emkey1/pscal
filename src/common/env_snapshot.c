#include "common/env_snapshot.h"

#include <stdlib.h>
#include <string.h>

extern char **environ;

static void pscalEnvSnapshotFreeNames(char **names, size_t count) {
    if (!names) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        free(names[i]);
    }
    free(names);
}

static bool pscalEnvSnapshotCollectNames(char ***out_names, size_t *out_count) {
    if (!out_names || !out_count) {
        return false;
    }
    *out_names = NULL;
    *out_count = 0;

    size_t count = 0;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        const char *entry = *cursor;
        const char *eq = entry ? strchr(entry, '=') : NULL;
        if (!eq || entry == eq) {
            continue;
        }
        count++;
    }

    if (count == 0) {
        return true;
    }

    char **names = (char **)calloc(count, sizeof(char *));
    if (!names) {
        return false;
    }

    size_t written = 0;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        const char *entry = *cursor;
        const char *eq = entry ? strchr(entry, '=') : NULL;
        if (!eq || entry == eq) {
            continue;
        }
        size_t name_len = (size_t)(eq - entry);
        char *name = (char *)malloc(name_len + 1);
        if (!name) {
            pscalEnvSnapshotFreeNames(names, written);
            return false;
        }
        memcpy(name, entry, name_len);
        name[name_len] = '\0';
        names[written++] = name;
    }

    *out_names = names;
    *out_count = written;
    return true;
}

void pscalEnvSnapshotClear(PscalEnvSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    if (snapshot->entries) {
        for (size_t i = 0; i < snapshot->count; ++i) {
            free(snapshot->entries[i]);
        }
        free(snapshot->entries);
    }
    snapshot->entries = NULL;
    snapshot->count = 0;
}

bool pscalEnvSnapshotTake(PscalEnvSnapshot *snapshot) {
    if (!snapshot) {
        return false;
    }

    pscalEnvSnapshotClear(snapshot);

    size_t count = 0;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        count++;
    }

    char **copy = (char **)calloc(count + 1, sizeof(char *));
    if (!copy) {
        return false;
    }

    size_t written = 0;
    for (char **cursor = environ; cursor && *cursor; ++cursor) {
        copy[written] = strdup(*cursor);
        if (!copy[written]) {
            snapshot->entries = copy;
            snapshot->count = written;
            pscalEnvSnapshotClear(snapshot);
            return false;
        }
        written++;
    }

    copy[written] = NULL;
    snapshot->entries = copy;
    snapshot->count = written;
    return true;
}

bool pscalEnvSnapshotRestore(PscalEnvSnapshot *snapshot) {
    if (!snapshot || !snapshot->entries) {
        return true;
    }

    bool ok = true;
    char **names = NULL;
    size_t name_count = 0;
    if (!pscalEnvSnapshotCollectNames(&names, &name_count)) {
        return false;
    }

    for (size_t i = 0; i < name_count; ++i) {
        if (unsetenv(names[i]) != 0) {
            ok = false;
        }
    }
    pscalEnvSnapshotFreeNames(names, name_count);

    for (size_t i = 0; i < snapshot->count; ++i) {
        const char *entry = snapshot->entries[i];
        const char *eq = entry ? strchr(entry, '=') : NULL;
        if (!eq || entry == eq) {
            ok = false;
            continue;
        }
        size_t name_len = (size_t)(eq - entry);
        char *name = (char *)malloc(name_len + 1);
        if (!name) {
            ok = false;
            continue;
        }
        memcpy(name, entry, name_len);
        name[name_len] = '\0';
        if (setenv(name, eq + 1, 1) != 0) {
            ok = false;
        }
        free(name);
    }

    pscalEnvSnapshotClear(snapshot);
    return ok;
}
