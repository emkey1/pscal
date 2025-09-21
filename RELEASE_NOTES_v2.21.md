# Pscal v2.21

Date: 2025-09-21

## Highlights
- Bytecode & VM: Dedicated `TO_BOOL`, direct field/array load instructions, and explicit builtin/user procedure call opcodes make truthiness and dispatch consistent across front ends.
- Language front ends: Pascal, CLike, and Rea gain XOR expressions, Rea adds a `nil` literal, and Pascal introduces an `override builtin` directive for intentional overrides.
- Tooling & Tests: All compilers understand `--no-cache`, regression scripts run cache-free, and a new Pascal PerformanceBenchmark program exercises the VM.
- Reliability: Deferred global initializer handling keeps constructors, vtables, and mutex registries ordered correctly even under nested `new` calls and spawned threads.

## New
- Bytecode & VM
  - Added `TO_BOOL` to normalize truthiness without extra `NOT` sequences.
  - Introduced `LOAD_FIELD_VALUE`, `LOAD_FIELD_VALUE16`, `LOAD_ELEMENT_VALUE`, and `LOAD_ELEMENT_VALUE_CONST` so the VM can fetch record and array values directly.
  - Added `CALL_BUILTIN_PROC` and `CALL_USER_PROC`, storing builtin IDs alongside names and allowing user procedures (including nested Pascal routines and constructors) to be invoked by name.
- Front-end languages
  - Pascal and CLike now parse and fold `xor` expressions, emitting the VM's native XOR opcode; Rea lexes the keyword as well.
  - Rea recognizes a `nil` literal and uses `TO_BOOL` for short-circuiting so boolean expressions match Pascal/CLike semantics.
  - Pascal's lexer accepts ASCII identifiers on all libc implementations and comment directives such as `// override builtin fibonacci` to silence intentional builtin replacements.
- CLI & Docs
  - Pascal, CLike, and Rea CLIs gained `--no-cache` to force recompilation, and documentation for standalone front ends now covers the new builtin procedure call flow.
  - Added `Examples/Pascal/PerformanceBenchmark`, a comprehensive workload that overrides builtin math routines, along with updated language reference notes.

## Improvements
- Compiler & VM
  - Deferred global variable initializers until after vtables are emitted, queuing definitions and replaying them once constructor metadata is in place.
  - Constant index array reads emit `LOAD_ELEMENT_VALUE_CONST`, `GET_ELEMENT_ADDRESS_CONST`, and other targeted bytecode, while `INC_LOCAL`/`DEC_LOCAL` update real and enum locals safely.
  - Builtin registry ordering stays stable whether SDL is enabled or not, and console attribute tracking leaves terminals in their default color scheme.
- Tooling & Tests
  - Regression scripts run all front ends with `--no-cache`, normalize stderr without `memmem`, respect case-sensitive paths on macOS, and disassemble `Examples/rea/hangman5` to ensure vtables precede global constructors.
- Build & Packaging
  - The build probes for AddressSanitizer support before enabling it for `dascal`, falling back gracefully when the toolchain lacks `-fsanitize=address`.

## Fixed
- Ensured Pascal global initializers, zero-argument constructors, and nested `CALL_USER_PROC` invocations run in source order with correct vtable wiring.
- Pascal cached-bytecode detection no longer depends on `memmem`, and musl-based builds tokenize identifiers correctly.
- Rea keeps method metadata in sync when recompiling, allocates call-frame slots reliably, and the hangman example verifies that `WordRepository`'s vtable is emitted before its global constructor executes.
- Mutex registries are shared between threads spawned from dynamic locals so mutex IDs remain valid across VM instances.

## Build & Install
- Configure and build:
  - `cmake -S . -B build [-DSDL=ON] [-DRELEASE_BUILD=ON]`
  - `cmake --build build -j`
- Install (CMake):
  - `cmake --install build` (installs `pascal`, `pscalvm`, `pscald` if enabled, `pscaljson2bc`, and optional completions)
- Environment:
  - `PASCAL_LIB_DIR`, `CLIKE_LIB_DIR`, `RUN_SDL`, `SDL_VIDEODRIVER`, `SDL_AUDIODRIVER`, `RUN_NET_TESTS`.

## Testing
- Run all suites:
  - `Tests/run_all_tests` or `ctest --test-dir build -j`
- Headless defaults for SDL remain off unless `RUN_SDL=1`.
- Networked examples require `RUN_NET_TESTS=1`.

## Compatibility
- VM bytecode cache version: `PSCAL_VM_VERSION = 7`. Bytecode generated prior to v2.21 will be recompiled automatically because of new opcodes and cache metadata.
- `CALL_BUILTIN_PROC`/`CALL_USER_PROC` and direct load instructions must be understood by consumer tools; update custom front ends accordingly.

## Known Notes
- Rea remains experimental; selected OO tests stay skipped via `REA_SKIP_TESTS`.
- SDL graphics tests still need real video/audio drivers; otherwise they run under dummy backends when unset.