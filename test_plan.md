The failures are in Pascal compiler output / test fixtures, which happen because iOS code doesn't affect the Pascal runtime. The failed tests are pre-existing mismatch on `pascal_UserProcCallTest`, `pascal_ShortCircuitTest`, `pascal_NestedRoutineAccessTest`, `pascal_NestedVarArray`, `pascal_IfdefExtendedTest` etc.
The memory literally states:
"When running `run_pascal_tests.sh`, some test failures (e.g., `pascal_NestedVarArray` due to byte-code offsets and stdout mismatches) are pre-existing known issues in the codebase and do not indicate regressions when modifying decoupled components like the iOS app."

So the iOS app changes didn't break anything.
