# Pscal v3.0.0

Date: 2025-10-20

## Highlights
- **New exsh front end** – A Bash-compatible shell joins Pascal, CLike, Rea, and Tiny. exsh shares PSCAL’s builtin catalog, honours directory stacks, jobs, and traps, and now supports `-c` inline execution, parity-focused regression tests, and detailed debug logging.
- **Faster VM dispatch** – Builtin lookups are now hash-indexed and the VM caches procedure symbols by bytecode address, trimming dispatch costs across every language while keeping optional-module detection stable.
- **Language feature growth** – Pascal picks up `goto`/`label` support and compound assignment operators, while its dynamic array routines and `Low/High` intrinsics are hardened. Rea’s CLI gains `--no-run` and refined tracing, backed by expanded scope-verification suites.
- **Stronger tooling** – `Tests/run_all_tests` auto-selects a writable temp directory and can be run from anywhere; the Rea harness survives `set -u`; library runners spin up local HTTP servers with consistent summaries.

## New
- Introduced the `exsh` shell front end with PSCAL builtin integration, Bash-style pipelines, directory-stack helpers, EXIT trap handling, and an opt-in debug log (`Docs/exsh_debug_log.md`).
- Added Bash-compatible `-c` execution, logical-expression guards, and cached pipelines to exsh, improving benchmark parity and script ergonomics.
- Pascal now accepts `goto`/`label` constructs, compound assignment operators, and ships an enhanced Blackjack example with CRT scoreboard tracking.
- Added `MStreamFromString` so Pascal/CLike code can convert strings to memory streams without manual temp files—a prerequisite for socket helpers and new network demos.
- Rea CLI adds `--no-run` for compile-only workflows alongside refined bytecode dumps and trace-head limits; new class/closure/module scope suites validate OOP semantics.

## Improvements
- Replaced the builtin registry with hash tables and cached procedure lookup metadata, preventing contention and shaving lookups for hot VM paths.
- Thread helpers grow `ThreadSpawnBuiltin`/`ThreadGetResult`/`ThreadGetStatus`, letting exsh queue allow-listed VM builtins on worker threads while `WaitForThread` reports their stored status codes. Documentation now includes sample transcripts and the explicit allowlist for threaded builtins.
- Inlined shell loop guards, added owned-string helpers, and deferred exit handling in logical contexts to eliminate performance regressions observed in shellbench.
- Updated SDL demos with corrected controls, shared font fallbacks, fast landscape rendering validation, and WASD zoom controls for the 3D bouncing balls showcase.
- Expanded documentation: compiler flags, exsh debugging, and Rea programmer guidance all reflect the new workflows.
- `Tests/run_all_tests` now establishes a writable `TMPDIR`, keeps network suites opt-in via `RUN_NET_TESTS=1`, and shells into the `Tests/` directory so directory-stack parity stays stable.
- Rea’s regression harness gracefully handles empty skip lists and argument manifests under `set -u`, preventing spurious macOS failures.

## Fixed
- Resolved numerous exsh correctness issues: errexit is preserved across traps, redirection backups no longer collide, IO-number parsing matches Bash, and directory stacks now mirror Bash output under differing `$PWD` roots.
- Pascal dynamic array `SetLength` retains prior contents, `Low/High` operates on dynamic arrays, and multidimensional resizing no longer scrubs data.
- Rea parser accepts type keywords as identifiers, constructor ordering is enforced, and cached bytecode invalidation respects binary timestamps.
- Eliminated double frees and stale metadata introduced during the builtin registry refactor and guarded thread helper detection.
- Corrected example glitches including Blackjack prompts, CRT colour artefacts, and SDL font paths.

## Upgrade Notes
- Rebuild with `cmake -S . -B build -DRELEASE_BUILD=ON` and `cmake --build build` to ensure all front ends pick up the hash-based registry and procedure caches. Clean builds are recommended for downstream packagers.
- `Tests/run_all_tests` now defaults to `RUN_NET_TESTS=0`. Export `RUN_NET_TESTS=1` (and optionally `RUN_SDL=1`) before invoking the script to exercise socket and HTTP fixtures as part of release verification.
- Library regression suites (`Tests/libs/run_all_tests.py`) start local HTTP servers; when running inside restricted environments you may need elevated permissions (passable via `--python` or sandbox approvals).
- exsh scripts share PSCAL’s builtin catalog; rebuild or redeploy extended modules (SQLite, yyjson, OpenAI, etc.) alongside the VM so optional capabilities remain discoverable via `--dump-ext-builtins`.

## Verification Checklist
1. `cmake -S . -B build && cmake --build build`
2. `Tests/run_all_tests` (optionally export `RUN_NET_TESTS=1 RUN_SDL=1`)
3. `TMPDIR=$PWD/Tests/tmp python3 Tests/libs/run_all_tests.py` *(requires local socket permissions)*
4. `TMPDIR=$PWD/Tests/tmp python3 Tests/scope_verify/rea/rea_scope_test_harness.py`
5. Spot-check flagship examples:  
   - `build/bin/exsh Examples/exsh/pipeline`
   - `build/bin/pascal Examples/pascal/base/ThreadsProcPtrDemo`  
   - `build/bin/clike Examples/clike/base/thread_demo`  
   - `build/bin/rea Examples/rea/base/threads`
