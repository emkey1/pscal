# PSCAL iOS/iPadOS Build Notes

This document will eventually host the authoritative instructions for building
the PSCAL static libraries and SwiftUI runner for iOS/iPadOS.  For now it
collects the decisions and open TODOs as we bring up the new target.

## 1. Goals
1. Cross-compile the PSCAL VM and every language frontend as static libraries.
2. Allow `cmake --preset ios-simulator` / `ios-device` to spit out Xcode
   project files or Ninja builds that Swift can link against.
3. Keep the existing macOS/Linux workflows untouched—CI should simply gain
   a new leg that exercises the iOS presets.
4. Make SDL2 optional but available so SDL demos render on iPad.

## 2. Toolchain Layout (work-in-progress)
```
cmake/toolchains/ios.cmake    # new toolchain file (stub created)
ios/
  README.md                   # project overview + SwiftUI modules
  PscalApp.xcodeproj          # (to be added) SwiftUI host app
```
build/ios-simulator           # CMake preset output directory (Ninja/Ninja Multi-Config)
  libpscal_*.a                # static archives for each frontend + VM

## 3. Dependency Checklist
- [ ] Xcode 15+ with iOS 17 SDK.
- [ ] SDL2 (official iOS framework or source build).
- [ ] Swift package for terminal rendering (candidate libraries TBD).
- [ ] Touch keyboard + hardware keyboard testing matrix.

## 4. Presets
`CMakePresets.json` now includes two opt-in configure presets:

| Preset | Description | Notes |
| --- | --- | --- |
| `ios-simulator` | Builds PSCAL libs for the iOS simulator | Uses `iphonesimulator` SDK, `arm64` arch, enables `SDL=ON`, `PSCAL_BUILD_STATIC_LIBS=ON`. |
| `ios-device` | Builds PSCAL libs for physical devices | Uses `iphoneos` SDK, `arm64` arch, enables `SDL=ON`, `PSCAL_BUILD_STATIC_LIBS=ON`. |

Both reference `cmake/toolchains/ios.cmake`, which sets `CMAKE_SYSTEM_NAME=iOS`
and toggles the SDK/arch based on `PSCALI_IOS_PLATFORM`.
`SDL` is disabled by default until the iOS-compatible SDL frameworks land; flip it
back on once an iOS-safe SDL package is wired in.
The presets only build the static archives—CLI executables, install rules, and ctest
hooks are skipped to avoid linking against macOS-only dependencies.

## 5. Static Libraries
When `PSCAL_BUILD_STATIC_LIBS=ON` (the default for the iOS presets) CMake now
emits embeddable archives alongside the regular executables:

| Target | Archive | Entry Point | Notes |
| --- | --- | --- | --- |
| `pscal_pascal_static` | `libpscal_pascal_static.a` | `int pascal_main(int argc, char *argv[])` | Exposes the Pascal compiler frontend. |
| `pscal_dascal_static` | `libpscal_dascal_static.a` | `int dascal_main(int argc, char *argv[])` | Debug variant of the Pascal frontend; requires `-DBUILD_DASCAL=ON`. |
| `pscal_vm_static` | `libpscal_vm_static.a` | `int pscalvm_main(int argc, char **argv)` | Runs compiled PSCAL bytecode. |
| `pscal_exsh_static` | `libpscal_exsh_static.a` | `int exsh_main(int argc, char **argv)` | Full exsh shell; this is what the SwiftUI host launches. |
| `pscal_clike_static` | `libpscal_clike_static.a` | `int clike_main(int argc, char **argv)` | Tiny C frontend. |
| `pscal_clike_repl_static` | `libpscal_clike_repl_static.a` | `int clike_repl_main(void)` | Interactive C-like REPL harness (no argv needed). |
| `pscal_rea_static` | `libpscal_rea_static.a` | `int rea_main(int argc, char **argv)` | Rea language frontend. |
| `pscal_pscald_static` | `libpscal_pscald_static.a` | `int pscald_main(int argc, char **argv)` | Bytecode disassembler; requires `-DBUILD_PSCALD=ON`. |
| `pscal_json2bc_static` | `libpscal_json2bc_static.a` | `int pscaljson2bc_main(int argc, char **argv)` | AST JSON → bytecode helper. |

Each CLI now exposes a stable `*_main` symbol so the app can invoke it without
pulling in the POSIX `main` entry point. When we build the static archives we
define `PSCAL_NO_CLI_ENTRYPOINTS`, which strips the traditional `main` symbol
and avoids duplicate entry points inside the Swift binary. Swift/ObjC++ shims
should include the relevant header (or forward declare the function) and call it
with the same argv vector the CLI expects.
`pscal_dascal_static` and `pscal_pscald_static` obey the same feature toggles as
their executable counterparts, so add `-DBUILD_DASCAL=ON` / `-DBUILD_PSCALD=ON`
when configuring if you need those archives. The iOS presets now build the
bundled libcurl against the in-tree OpenSSL while enabling Apple SecTrust, so
HTTPS validation uses the platform trust store without shipping a CA bundle.

Example simulator build:

```sh
$ cmake --preset ios-simulator
$ cmake --build --preset ios-simulator --target pscal_exsh_static
# archives land under build/ios-simulator/libpscal_exsh_static.a
```

## 6. SwiftUI Host Project
`ios/PscalApp.xcodeproj` is a minimal SwiftUI runner that links the
`libpscal_exsh_static.a` archive. The project expects you to build the static
libs ahead of time:

```sh
cmake --preset ios-simulator
cmake --build --preset ios-simulator --target \
    pscal_core_static \
    pscal_exsh_static \
    pscal_pascal_static \
    pscal_dascal_static \
    pscal_clike_static \
    pscal_rea_static \
    pscal_vm_static \
    pscal_json2bc_static \
    pscal_pscald_static
```

Repeat those build steps with the `ios-device` preset when you need a hardware
binary. These archives provide every frontend entry point that the iOS app links
against, enabling the new `pascal`, `dascal`, `clike`, `rea`, `pscalvm`,
`pscaljson2bc`, and `pscald` shell builtins. `libpscal_core_static.a` contains
the shared runtime/compiler code and must always be linked alongside the
frontend-specific archives in Xcode.

The Xcode target points its header/library search paths at
`../build/ios-{simulator,device}` so it can pick up both the archives and
generated headers. `PSCALRuntime.mm` bridges `exsh_main` into Swift via the new
bridging header (`PscalApp-Bridging-Header.h`), so Swift can simply call
`PscalRuntimeBootstrap.shared.start()`. `TerminalBuffer` implements a VT100
parser (cursor moves, SGR colors, erase commands) so SwiftUI renders a faithful
terminal surface, and the hidden `TerminalInputBridge` captures keyboard events
so commands are typed directly at the prompt—no visible auxiliary text field is
required.

## 7. SwiftUI Stubs
`ios/Sources/App/PscalApp.swift` and `TerminalView.swift` provide a minimal
entry point so we can link the PSCAL libs once the Xcode project exists.
`TerminalView` now streams live exsh output via the VT100 renderer and captures
keyboard input inline using the invisible `TerminalInputBridge`.

## 8. Next Steps
1. Wire the toolchain/presets into CI to ensure `cmake --preset ios-simulator`
   stays healthy.
2. Add an actual Xcode project / Swift package manifest that imports the PSCAL
   static libs.
3. Begin the Objective-C++ bridge that exposes `exsh_main` to Swift.
4. Investigate iOS-safe shims for POSIX calls (pipes, fork, etc.).

This file will grow as each of the steps above lands; contributions welcome.

## 9. Runtime Assets & Examples
- The entire `Examples/` tree from the repository is referenced directly in the
  Xcode project (folder reference). Xcode bundles it into the application so the
  simulator/device binary always ships with the stock demos.
- On launch the Swift side runs `RuntimeAssetInstaller`, which copies the
  bundled `Examples` directory into the sandbox at `~/Documents/Examples`,
  writes a `.examples.version` marker based on `CFBundleVersion`, and skips the
  copy on subsequent runs unless the bundle version changes.
- The installer also sets the process working directory to `~/Documents` so
  running `ls` immediately shows `Examples`, and exports `PSCAL_EXAMPLES_ROOT`
  (plus `PSCALI_WORKSPACE_ROOT`) so native code can locate the staged tree.
  This mirrors the macOS/Linux developer experience—`cd Examples/pascal/...`
  works the same way inside the iOS shell without requiring manual uploads.
- Editor can render inline or in a floating editor window. Use Terminal Settings →
  “editor/vi” to choose your preferred mode. The `PSCALI_EDITOR_WINDOW_MODE`
  environment variable (`window`/`inline`) still seeds the default on first run.
- In addition to `Examples`, the iOS bundle now ships with the same runtime
  payload that normally installs to `/usr/local/pscal` (e.g. `lib/`, `fonts/`,
  `etc/`, docs, and the test fixtures). `RuntimeAssetInstaller` copies those
  directories into `~/Documents/pscal_runtime`, recreates the compatibility
  symlinks (`pascal/lib`, `misc`, etc.), and points `PSCAL_INSTALL_ROOT`,
  `PASCAL_LIB_DIR`, `CLIKE_LIB_DIR`, and the new `PSCALI_INSTALL_ROOT`
  environment variable at that staged tree so the native compilers can find
  their resources without touching the read-only system paths.

## 10. Runtime Debug Toggles
- `PSCALI_PTY_OUTPUT_LOG=1` writes raw PTY output to `~/Documents/var/log/pscal_runtime.log` (disabled by default to avoid throttling terminal output).
