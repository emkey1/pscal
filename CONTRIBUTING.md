# Contributing

Thanks for your interest in contributing to Pscal! This document explains the basic branch flow and how to submit changes.

## Branch Strategy

- `devel` is the integration branch.
  - Open pull requests from a feature branch into `devel`.
  - CI runs the full test suites and example compilation on `devel`.
- `main` holds releases/tags only.
  - When preparing a release, merge or fast-forward from `devel` to `main` (or tag off `devel` as appropriate) and create a release tag.
- Do not merge `main` → `devel`.
  - Accidental PRs in that direction are blocked by CI and should be closed.

A simple flow:

1. Create a topic branch from `devel`, e.g. `feature/xyz`.
2. Commit your changes and ensure tests/examples pass locally.
3. Open a PR: base = `devel`, head = `feature/xyz`.
4. After review and green CI, merge into `devel`.
5. Releases are cut from `devel` (to `main`) when ready.

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

