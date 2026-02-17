# PSCAL iPadOS / iOS Roadmap

This document outlines the work required to ship a PSCAL suite on iPadOS/iOS.
The goal is to deliver an app that boots into the `exsh` shell front end,
executes PSCAL bytecode produced by any supported language frontend (Pascal,
Rea, C-like, shell), and optionally exercises SDL-based demos.  The roadmap
is organised so the existing macOS/Linux build remains the source of truth.

## 1. Product Goals
1. Provide an iPad-optimised terminal experience that feels like `exsh` on macOS.
2. Support running programs compiled on-device (Pascal/CLike/Rea/exsh) and
   loading PSCAL bytecode artifacts dropped into the app’s Documents folder.
3. Offer SDL2 rendering/audio so SDL demos work when the device is in
   landscape with keyboard attached.
4. Maintain one set of sources/build scripts; iOS builds should simply be an
   additional CMake preset / Xcode workspace.

## 2. Architecture Overview
- **Runner App**: native SwiftUI wrapper hosting a `MetalView` for SDL +
  a `TerminalView` (either a custom renderer or leveraging libvt100).
- **PSCAL Core**: same VM + language frontends compiled as static libraries
  (`libpscalvm`, `libpascal`, `libclike`, etc.).
- **exsh UX**: the app launches straight into exsh. Touch/keyboard input
  feeds exsh’s stdin; stdout/stderr render in the terminal view. App shell
  exposes a command palette for uploading files, toggling SDL view, etc.
- **File Access**: use the app’s Documents directory plus Files integration
  (UIDocumentPicker) to import/export scripts and caches.

## 3. Build System Tasks
1. **CMake presets**: add `ios-simulator` and `ios-device` presets that
   cross-compile the VM/core libraries as static archives. Use the same
   C++17/clang flags as macOS with minor adjustments.
2. **Xcode project**: create `ios/PscalApp.xcodeproj` that links the PSCAL
   static libs, wraps them in a Swift/Objective-C bridge, and embeds SDL2.
3. **Continuous Integration**: add GitHub Actions job targeting `macos-13`
   with `xcodebuild -scheme PscalApp -sdk iphonesimulator` to ensure iOS
   builds never regress, while retaining the existing macOS/Linux workflows.

## 4. SDL2 Integration
1. Adopt the official SDL2 iOS framework (or build from source) and expose it
   via CMake so both macOS and iOS builds use the same abstraction layer.
2. Implement an SDL→SwiftUI bridge: keep a hidden SDL window whose render
   target is a `MetalLayer` or `UIView`. When PSCAL code calls `InitGraph`,
   switch the UI to the SDL view; when it closes, return to the terminal.
3. SDL input: forward touch and hardware keyboard events to SDL so existing
   demos receive mouse/keyboard without modification.

## 5. Terminal / exsh UX
1. Create a SwiftUI `TerminalView` backed by a small VT100 parser (libtsm,
   EmulationKit, or a lightweight in-tree implementation).
2. Provide gesture affordances:
   - Two-finger swipe = scrollback.
   - Tap+hold = selection + copy.
   - On-screen keyboard toggle and hardware keyboard shortcuts (⌘+K to clear,
     ⌘+R to run saved script).
3. Persist shell history and Files-based shortcuts inside the app’s container.

## 6. Language Frontends & Tooling
1. Ship Pascal, CLike, Rea, shell and the VM as static libs. The iPad app
   will bundle CLI wrappers so `pascal hello.pas` works the same as macOS.
2. Add a small “project” browser showing `.pas`, `.rea`, `.cl` etc. stored
   in Documents. Commands typed in exsh can reference these paths directly.
3. Bytecode caching: reuse `.pscal/bc_cache` but limit maximum size and expose
   a toggle in Settings to purge caches if storage is tight.

## 7. Development Timeline (high level)
| Phase | Milestone | Duration |
| --- | --- | --- |
| 1 | CMake + static libs building for iOS (device & simulator) | 2 weeks |
| 2 | SwiftUI shell prototype (TerminalView, exsh REPL) | 3 weeks |
| 3 | SDL bridge + view switching | 3 weeks |
| 4 | File integration, Files picker, caching, settings | 2 weeks |
| 5 | QA/pass: run regression suites on-device, tune input | 2 weeks |
| 6 | App Store prep, TestFlight, docs | 1 week |

## 8. Risks & Mitigations
| Risk | Mitigation |
| --- | --- |
| SDL on iOS requires backgroundable audio/video contexts | Use SDL’s official iOS template and keep the app in foreground while SDL view is active. |
| exsh needs POSIX-like APIs (fork, pipes) not on iOS | Keep exsh single-process in iOS build; emulate pipes via Grand Central Dispatch queues. |
| File access restrictions | Encourage Files integration and rely on UIDocumentPicker + app sandbox. |
| Performance / memory constraints on older devices | Enforce `-Os` builds, set bytecode cache limits, profile with Instruments. |

## 9. Deliverables
1. `ios/PscalApp.xcodeproj`, SwiftUI app scaffolding, README.
2. Updated CMake presets & documentation (`Docs/ios_build.md`).
3. Automated test plan (unit tests for Swift bridging, smoke test running
   sample Pascal/CLike/Rea programs + SDL demo).
4. App Store-ready marketing copy + onboarding instructions.

Once these pieces land, PSCAL will have a fully supported iPadOS shell/SDL
experience without destabilising the macOS/Linux toolchain.

---

## 10. Current Status (2024-XX-XX)
| Area | Progress |
| --- | --- |
| Build Docs | `Docs/ios_build.md` created + updated with presets/toolchain status. |
| Toolchain | `cmake/toolchains/ios.cmake` now sets `CMAKE_SYSTEM_NAME=iOS` and handles simulator/device via `PSCALI_IOS_PLATFORM`. |
| CMake Presets | `CMakePresets.json` adds opt-in `ios-simulator` / `ios-device` configurations. |
| Entry Points | Frontends now expose `exsh_main`, `pascal_main`, `clike_main`, `rea_main`, `pscalvm_main`, and `pscaljson2bc_main`. `PSCAL_NO_CLI_ENTRYPOINTS` strips duplicate `main` symbols when building libraries. |
| Static Libs | `PSCAL_BUILD_STATIC_LIBS` builds `libpscal_{pascal,dascal,vm,exsh,clike,clike_repl,rea,json2bc,pscald}_static.a` (dascal/pscald rows obey the usual `BUILD_*` toggles). The iOS presets now default to `(SDL=ON, PSCAL_BUILD_STATIC_LIBS=ON, PSCAL_USE_BREW_CURL=ON)` and skip CLI executables/tests so cross-compiles focus on embeddable archives with graphics/audio support available. |
| App Layout | `ios/README.md` + SwiftUI stubs (`PscalApp.swift`, `TerminalView.swift`) live under `ios/Sources`. `ios/PscalApp.xcodeproj` links `libpscal_exsh_static.a` through the `PSCALRuntime` bridge so SwiftUI can spawn the VM. |
| Terminal UX | VT100 renderer + hidden input bridge bring inline editing, and iOS-only builtins (`ls`, `pascal`, `clike`, `rea`, `pscalvm`, `pscaljson2bc`, `pscald`) replace missing CLI binaries. |

Upcoming work:
1. Flesh out the Swift/ObjC++ bridge (stdin/stdout plumbing, lifecycle hooks) now that the Xcode project links the static libs.
2. Build a real terminal renderer + input pipeline so SwiftUI can display exsh output instead of placeholder text.
3. Start integrating SDL (Metal-backed view switching) once the terminal UI is functional.
