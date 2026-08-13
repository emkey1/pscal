# Repo-extraction dry run

This directory validates that the shared core (`pscal_core_static`) can be built
as a standalone, front-end-free library, and that a front end can build against
it using only core's public headers. It is the proof-of-concept for splitting
PSCAL into separate repositories (`pscal-core` + per-frontend repos + umbrella).

`pscal-core/CMakeLists.txt` builds, out of the existing monorepo sources:
1. `pscal_core_static` — the minimal core (SDL/curl/sqlite OFF, no Apple vendored
   deps): ~63 source files, include roots `src/` + `third_party/yyjson` + `lib` +
   `third-party/nextvi` + the generated `pscal_paths.h`, linking only `m` and
   `Threads`. No frontend directory is on the include path.
2. `clike` — built against `pscal_core_static`, simulating the future `clike`
   repo consuming `pscal-core`.

## Result (validated)

```
cmake -S extract-dryrun/pscal-core -B /tmp/pscal-core-standalone
cmake --build /tmp/pscal-core-standalone -j8
/tmp/pscal-core-standalone/clike --no-cache prog.cl   # runs correctly
```

Both targets build; the standalone CLike runs. This confirms the core is genuinely
self-contained and the repo boundary is clean.

## Notes for the real extraction

- `pscal_paths.h` is reused here from the monorepo's `build/generated/`. The real
  `pscal-core` repo reproduces the `configure_file` step (monorepo CMakeLists:191).
- `PSCAL_NO_CLI_ENTRYPOINTS` is core-private (suppresses `main()` in lib TUs); the
  frontend executables must NOT define it.
- nextvi's `vi.c` defines `main()`, renamed via `-Dmain=nextvi_main_entry` so it
  does not collide with a frontend's `main()`.
- The full (non-minimal) core adds SDL/curl/sqlite-gated sources and, on Apple/iOS,
  the smallclue/OpenSSH/openrsync vendored trees.
