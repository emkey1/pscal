# Agent Notes

## Repository Context
- Runtime and front ends live under `src/`; unit tests under `Tests/`; executables build to `build/bin/` via CMake.
- Pascal/REA compiler logic is concentrated in `src/compiler/compiler.c`; many globals track compile state, so save/restore helpers are common.
- When `git add` fails with permission errors or `.git/index.lock`, rerun with elevation; the workspace sometimes lives on file systems that gate writes.

## Build & Test
- Configure with `cmake -S . -B build [-DSDL=ON] [-DBUILD_DASCAL=ON]`, then `cmake --build build`.
- Use `Tests/run_all_tests`; set `RUN_SDL=1` or `RUN_NET_TESTS=1` as needed. Prefer targeted scripts in `Tests/` for quicker iteration.
- Library conformance lives under `Tests/libs/`. Each front end has its own harness; the Rea suite (`Tests/libs/rea/run_tests.py`) spins up an HTTP helper, inspects optional extended built-ins (yyjson, etc.), and exports `REA_TEST_EXT_BUILTINS`/`REA_TEST_HAS_YYJSON` so JSON checks are skipped when support is absent.
- Scope validation suites live in `Tests/scope_verify/` (`clike`, `pascal`, `rea`). The harnesses are manifest-driven and support `--only`, `--list`, `--seed`, and `--cmd` overrides. Rea's runner (`Tests/scope_verify/rea/rea_scope_test_harness.py`) materialises fixtures via `--update` and records reports under `Tests/scope_verify/rea/out/`.
- Test manifests (e.g., `Tests/scope_verify/rea/tests/manifest.json`) are generated through the corresponding `build_manifest.py`; regenerate them when adding or editing cases to keep fixtures in sync.
- For exsh tests that don't include PSCAL extenstions, the output should always be identical to what the bash shell would produce
- For exsh shellbench (https://github.com/shellspec/shellbench) is used as both benchmarking and functionality verification
- Appropriate tests should always be run.  It is NEVER appropriate not to test.  At the very least you should verify that modified code compiles

## Coding Practices
- C11 style: 4-space indent, braces on same line, camelCase for functions, uppercase snake for macros.
- Many compiler helpers rely on implicit global state; when adding new stacks (like the vtable tracker), ensure push/pop happen on every exit path.
- Keep allocations paired with frees; leak auditors assume matcher functions like `freeVTableClassList` exist.
- When adding VM builtins, append them to the end of the dispatch tables/registration arrays so legacy builtin IDs remain stable.
  Never insert new entries mid-table—shifting established builtin IDs breaks existing bytecode, so this **must never happen**.

## Workflow Tips
- Always read existing globals before introducing new ones—regressions often come from missing reset logic during module/unit compiles.
- Tests are slow; when editing the compiler, run at least the relevant front-end suite prior to broader sweeps.
- Document any env vars you touch (`SDL_VIDEODRIVER`, `RUN_SDL`, etc.) in commits/PRs; future agents look here for quick bootstrapping.
- Binaries tend to have both a --dump-ast-json and --dump-bytecode options which can provide substantial clues as to what is going on in the AST building and compilation portions of the code when debugging.
- Commit early and often—small, frequent commits keep iPadOS and desktop work in sync and avoid multi-thousand-file diffs that are hard to review or push.
