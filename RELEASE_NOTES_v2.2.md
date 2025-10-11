# Pscal v2.2

Date: 2025-09-18

## Highlights
- Pascal: Procedure/function pointers with address-of (`@`) and indirect calls.
- Concurrency: `CreateThread(@Proc, arg)` and `WaitForThread(thread)` across front-ends.
- Tools: New standalone bytecode decompiler (`pscald`) and AST JSON → Bytecode compiler (`pscaljson2bc`) with shell completions.
- HTTP: Expanded TLS, pinning, proxies, and file:// URL handling for hermetic tests.
- Build/CI: macOS SDK auto-detect, optional Homebrew curl, clean `-Wall` builds.

## New
- Pascal
  - Address-of (`@`) for routines; first-class procedure/function pointer types.
  - Indirect pointer calls in both expression and statement contexts.
- Rea front end (experimental)
  - New object-oriented language targeting the PSCAL VM with class-based syntax.
  - Classes, fields, methods, constructors, and single inheritance via `extends`.
  - Virtual dispatch for methods; `my`/`myself` for instance access; `super` for parent calls.
  - Familiar control flow and expressions (C-like precedence), plus access to VM built-ins.
  - Docs: `Docs/rea_overview.md`, `Docs/rea_language_reference.md`, `Docs/rea_tutorial.md`.
  - Examples: `Examples/rea/base/hello`, `Examples/rea/base/showcase`, `Examples/rea/base/method_demo`, plus SDL demos under `Examples/rea/sdl`.
- VM/Threads
  - Indirect call opcodes; thread entry supports an initial pointer argument.
- Tools
  - `pscald <bytecode.bc>`: disassemble bytecode equivalent to `--dump-bytecode-only`.
  - `pscaljson2bc`: compile AST JSON to bytecode; supports `--dump-bytecode` and `--dump-bytecode-only`.
  - Shell completions for `pscaljson2bc` (Bash/Zsh), optionally installed via CMake.
- Examples
  - Threads + procedure pointers demo: `Examples/pascal/base/ThreadsProcPtrDemo`.
  - CLike minimal HTTP server and docs: `Examples/clike/base/simple_web_server` + `Docs/simple_web_server.md`.

## Improvements
- Bytecode cache
  - Embeds source path (for provenance); strengthened staleness detection when source and cache mtimes match; binary-newer-than-cache invalidates as expected.
  - Utilities: `tools/dpc`, `tools/find_cache_name` to inspect cache/source mapping.
- HTTP
  - Per-session options (`tls_min/max`, `alpn`, `ciphers`, `pin_sha256`, proxies, DNS resolve overrides`).
  - `file://` handled directly with synthesized headers for hermetic tests.
- CLike
  - `SDL_ENABLED` define under `-DSDL=ON` for guarded code.
  - REPL (`clike-repl`) for interactive experimentation.
- Docs
  - Updated README and guides for procedure/function pointers, thread APIs, AST JSON pipeline, and HTTP security.
- Build
  - macOS: SDK auto-detection; opt-in `-DPSCAL_USE_BREW_CURL=ON` to prefer Homebrew curl.
  - Installer: misc library copy fixed (ensures `${PSCAL_INSTALL_ROOT}/misc` exists).

## Fixed
- Pointer metadata propagation for locals/globals enabling reliable `new(p)` and `p^` dereference behavior.
- Parser support for named parameters in procedure/function pointer types.
- Installer: corrected target directory creation for misc assets.

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
- Headless defaults for SDL; opt-in with `RUN_SDL=1`.
- Network tests gated by `RUN_NET_TESTS=1`.

## Compatibility
- VM bytecode cache version: `PSCAL_VM_VERSION = 5`.
- `CreateThread` remains backward compatible with 1-arg form.

## Known Notes
- Rea remains experimental; some OO tests are skipped in CI via `REA_SKIP_TESTS`.
- SDL tests require real drivers; otherwise skipped under dummy drivers.
