# Agent Notes

## Repository Context
- Runtime and front ends live under `src/`; unit tests under `Tests/`; executables build to `build/bin/` via CMake.
- Pascal/REA compiler logic is concentrated in `src/compiler/compiler.c`; many globals track compile state, so save/restore helpers are common.
- Beware Dropbox sync conflicts: `git add` may require escalated permissions; `.git/index.lock` errors usually just mean rerun with elevation.

## Build & Test
- Configure with `cmake -S . -B build [-DSDL=ON] [-DBUILD_DASCAL=ON]`, then `cmake --build build`.
- Use `Tests/run_all_tests`; set `RUN_SDL=1` or `RUN_NET_TESTS=1` as needed. Prefer targeted scripts in `Tests/` for quicker iteration.

## Coding Practices
- C11 style: 4-space indent, braces on same line, camelCase for functions, uppercase snake for macros.
- Many compiler helpers rely on implicit global state; when adding new stacks (like the vtable tracker), ensure push/pop happen on every exit path.
- Keep allocations paired with frees; leak auditors assume matcher functions like `freeVTableClassList` exist.

## Workflow Tips
- Always read existing globals before introducing new ones—regressions often come from missing reset logic during module/unit compiles.
- Tests are slow; when editing the compiler, run at least the relevant front-end suite prior to broader sweeps.
- Document any env vars you touch (`SDL_VIDEODRIVER`, `RUN_SDL`, etc.) in commits/PRs; future agents look here for quick bootstrapping.

