#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/env_snapshot.h"

static void clearTestVars(void) {
    unsetenv("PSCAL_ENV_SNAPSHOT_AA");
    unsetenv("PSCAL_ENV_SNAPSHOT_BB");
    unsetenv("PSCAL_ENV_SNAPSHOT_CC");
    unsetenv("PSCAL_ENV_SNAPSHOT_DD");
}

static void assertVarEquals(const char *name, const char *expected) {
    const char *value = getenv(name);
    assert(value != NULL);
    assert(strcmp(value, expected) == 0);
}

static void testRestoreRemovesAddedVars(void) {
    PscalEnvSnapshot snapshot = {0};

    clearTestVars();
    assert(setenv("PSCAL_ENV_SNAPSHOT_AA", "1", 1) == 0);
    assert(setenv("PSCAL_ENV_SNAPSHOT_BB", "1", 1) == 0);
    assert(pscalEnvSnapshotTake(&snapshot));

    assert(setenv("PSCAL_ENV_SNAPSHOT_CC", "1", 1) == 0);
    assert(setenv("PSCAL_ENV_SNAPSHOT_DD", "1", 1) == 0);
    assert(pscalEnvSnapshotRestore(&snapshot));

    assertVarEquals("PSCAL_ENV_SNAPSHOT_AA", "1");
    assertVarEquals("PSCAL_ENV_SNAPSHOT_BB", "1");
    assert(getenv("PSCAL_ENV_SNAPSHOT_CC") == NULL);
    assert(getenv("PSCAL_ENV_SNAPSHOT_DD") == NULL);
    clearTestVars();
}

static void testRestoreOverwrittenValues(void) {
    PscalEnvSnapshot snapshot = {0};

    clearTestVars();
    assert(setenv("PSCAL_ENV_SNAPSHOT_AA", "before", 1) == 0);
    assert(pscalEnvSnapshotTake(&snapshot));

    assert(setenv("PSCAL_ENV_SNAPSHOT_AA", "after", 1) == 0);
    assert(pscalEnvSnapshotRestore(&snapshot));

    assertVarEquals("PSCAL_ENV_SNAPSHOT_AA", "before");
    clearTestVars();
}

int main(void) {
    testRestoreRemovesAddedVars();
    testRestoreOverwrittenValues();
    puts("env snapshot restore tests passed");
    return 0;
}
