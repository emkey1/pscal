# Pscal v2.1-beta

Date: 2025-09-02

This release delivers substantial improvements to the CLike front-end, adds compile-only (disassembly) modes to both front-ends, expands regression test coverage, and strengthens CI.

## Highlights
- New flags for compile-only workflows:
  - `--dump-bytecode`: compile and disassemble, then execute.
  - `--dump-bytecode-only`: compile and disassemble, then exit (no execution).
- CLike semantics:
  - Short-circuit logical `&&` and `||`.
  - Shift operators `<<` and `>>` with standard precedence and left-associativity.
  - Improved `~` behavior on integers (bitwise-like via `(-x - 1)`); non-integers fall back to logical NOT.
- Examples: all compile (dump-only mode) under headless SDL defaults.
- CI: builds, runs tests, and compiles all Examples in dump-only mode.

## Getting Started
```sh
cmake -S . -B build [-DSDL=ON]
cmake --build build
# Pascal
build/bin/pascal --dump-bytecode-only Examples/Pascal/hello
# CLike
build/bin/clike --dump-bytecode-only Examples/clike/hello
```

## SDL
- When built with `-DSDL=ON`, the CLike preprocessor defines `SDL_ENABLED` so you can guard code:

```c
#ifdef SDL_ENABLED
  // SDL graphics/audio
#endif
```

- Headless defaults: test scripts set `SDL_VIDEODRIVER=dummy` and `SDL_AUDIODRIVER=dummy` (skip SDL tests).
- To run SDL content, set `RUN_SDL=1` and a real video driver.

## Environment Variables
- `CLIKE_LIB_DIR`: search directory for CLike imports.
- `PASCAL_LIB_DIR`: root library directory for Pascal units (.pl).
- `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`, `RUN_SDL`: control SDL behavior for tests/examples.

## Compatibility
- VM bytecode cache version: `PSCAL_VM_VERSION = 5`.

## Changes Since v2.0
- Front-ends: new compile-only flags in Pascal and CLike.
- CLike: parser/codegen upgraded for shifts and short-circuit; improved `~` for integers.
- Tests: added CLike and Pascal cases; VM-oriented checks.
- Examples: fixed `chudnovsky_native` constant literal typing.
- Docs: updated README and tutorial for options, semantics, SDL, and env vars.
- CI: compiles all Examples in dump-only mode, in addition to test suites.

## Known Notes
- `~` uses an arithmetic expansion for integer types; there is no dedicated VM bitwise-not opcode.
- Shifts are supported for integer-like types; shifting real values is not meaningful and should be avoided.

Enjoy!

