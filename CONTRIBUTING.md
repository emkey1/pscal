# Contributing

Thanks for your interest in contributing to Pscal! This document explains the branch flow, PR policy, and how to submit changes.

## Branch Strategy and PR Policy

- Default branch: `main`. All work branches from `main` and merges back into it.
  - CI runs the build, the ctest suites, example compilation, and an iOS build
    on every pull request.
- Releases are cut as tags on `main` (see `RELEASE_NOTES_*.md`).

A simple flow:

1) Create a topic branch from `main`, e.g. `feature/xyz`.
2) Commit your changes and ensure tests/examples pass locally.
3) Open a PR: base = `main`, head = `feature/xyz`.
4) After review and green CI, merge into `main`.

## Where the code lives

This repository is a thin umbrella. The languages and runtime live in the
`components/` submodules — `pscal-core`, `rea`, `aether`, `clike`, `pascal`,
`exsh` — each its own repository. A change to a frontend or the VM belongs in
the component repo; the umbrella then records the new gitlink. Building here
compiles the submodule working trees directly, so you can edit
`components/<name>/` and rebuild without any extra step.

## Building

```sh
cmake -S . -B build [-DSDL=ON]
cmake --build build
```

- SDL: When built with `-DSDL=ON`, GUI/audio routines are available.
- Headless defaults in CI use dummy SDL drivers; SDL examples/tests are skipped unless explicitly enabled.

## Tests

Run the regression suites after building:

```sh
Tests/run_clike_tests.sh
Tests/run_pascal_tests.sh
```

In CI, examples are also compiled in dump-only mode using:

- CLike: `--dump-bytecode-only`
- Pascal: `--dump-bytecode-only`

## Environment Variables

- `CLIKE_LIB_DIR`: search directory for CLike `import` modules.
- `PASCAL_LIB_DIR`: root directory for Pascal units (`.pl`).
- `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`: set `dummy` in headless runs.
- `RUN_SDL=1`: opt-in to run SDL tests/examples.

## Code Style

- Keep changes minimal and focused.
- Prefer adding targeted tests where appropriate.
- Avoid introducing new warnings; releases aim for a clean (warning-free) build.

Thank you for contributing!
