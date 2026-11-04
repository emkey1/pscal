# PSCAL iOS Host Overview

This directory will contain the SwiftUI runner that embeds the PSCAL toolchain.
The high-level structure we are targeting:

```
ios/
  README.md               # this file
  PscalApp.xcodeproj      # SwiftUI workspace
  Sources/
    App/
      PscalApp.swift      # entry point
      TerminalView.swift  # VT100 renderer + keyboard capture
      SDLContainer.swift  # SDL bridging layer
    Bridge/
      PSCALRuntime.mm     # Objective-C++ glue that calls the VM
      PSCALRuntime.h
```

## Status
- [x] Directory + documentation scaffolding.
- [x] SwiftUI terminal (`PscalApp.swift` + `TerminalView.swift` + VT100 renderer).
- [x] Actual Xcode project (`PscalApp.xcodeproj`).
- [x] Swift/ObjC bridge that invokes `exsh_main` (via `PSCALRuntime.mm` + bridging header).
- [ ] SDL view integration.

As we implement each milestone the checklist above will be expanded and linked
to the relevant commits.

## Building the SwiftUI Host
1. Configure and build the iOS static archives (plus the standalone tool
   runner) via CMake. For example:
   ```sh
   cmake --preset ios-simulator-release
   cmake --build --preset ios-simulator-release --target \
       pscal_core_static \
       pscal_exsh_static \
       pscal_pascal_static \
       pscal_dascal_static \
       pscal_clike_static \
       pscal_rea_static \
       pscal_vm_static \
       pscal_json2bc_static \
       pscal_pscald_static \
       pscal_tool_runner
   ```
   Repeat with `--preset ios-device-release` for real hardware. The legacy
   `ios-simulator` / `ios-device` presets remain available for debug builds.
2. Open `ios/PscalApp.xcodeproj` in Xcode. The target automatically adds
   configuration-matched iOS build directories (debug or release) to the
   library/header search paths so it can find the CMake-produced archives and
   generated headers. Link `libpscal_core_static.a` along with the frontend
   archives (`libpscal_exsh_static.a`, `libpscal_pascal_static.a`, etc.) so the
   shared runtime is available at link time. The `Embed Tool Runner` build phase
   copies `pscal_tool_runner` from the corresponding configuration-matched
   CMake build directory into the app bundle so the shell can launch the
   non-exsh frontends in a separate process.
3. Select the desired destination (e.g., "My Mac (Designed for iPad)" or an
   iPad simulator) and build/run. The app links against `libpscal_exsh_static.a`
   and starts `exsh_main` through the bridge.
4. `PSCALRuntimeConfigureHandlers` wires stdout/stderr into a PTY-backed pump so
   Swift receives live output. `TerminalView` renders it via the VT100 buffer and
   the hidden `TerminalInputBridge` feeds keystrokes straight into the PTY (no
   auxiliary text field).
5. `TerminalBuffer.swift` is a VT100-style screen model (cursor moves, colors,
   SGR attributes, erase commands). `TerminalInputBridge` captures keyboard
   events with an invisible `UITextView` so typing happens directly at the exsh
   prompt, exactly like a hardware terminal.
6. `TerminalInputBridge` automatically reclaims focus when you tap anywhere in
   the terminal view, so typing always happens beside the prompt.

## Built-in Tool Commands

On iOS the shell exposes the PSCAL toolchain as pure builtins, so you can invoke
them without external binaries:

- `ls` – lightweight directory listing that works even without `/bin/ls`.
- `pascal`, `dascal`, `clike`, `rea` – compiler frontends for each language.
- `pscalvm` – run bytecode artifacts (`.pscalbc`) directly.
- `pscaljson2bc` – convert AST JSON into bytecode inside the sandbox.
- `pscald` – bytecode disassembler (available when `BUILD_PSCALD=ON`).

All of these builtins are backed by the static libraries produced by CMake, so
they stay in sync with the main toolchain.

If you rebuild the static libs, simply rerun the CMake commands—Xcode will pick
up the updated `.a` files without additional project changes.

## In-App Command Test
**Run this on a Release simulator build before any TestFlight upload.** CI only
compiles the app and no host suite runs its commands, so code can be badly broken
on iOS with every suite green.

```sh
SDK_NAME=iphonesimulator CONFIGURATION=Release bash ios/Tools/ensure_static_libs.sh
xcodebuild -project ios/PSCAL.xcodeproj -scheme PscalApp -configuration Release \
    -sdk iphonesimulator -destination 'generic/platform=iOS Simulator' \
    -derivedDataPath build/DerivedData-ios-sim CODE_SIGNING_ALLOWED=NO build
ios/Tools/app_smoke_test.py
```

`app_smoke_test.py` runs about 640 commands inside the app and compares their output:
applets, pipelines, redirections, absolute paths, subshells and background groups,
every language in sequence and in pipelines, scripts, Tiny and local `rsync`. It drives no UI. It installs the app fresh, makes
`~/.exshrc` in the app's container run the commands, and reads their result files
from the host. When it finishes it uninstalls the app and shuts down any simulator
it booted.

It takes the newest Release simulator build and a shut-down iPad or iPhone simulator
that does not have the app. Use `--app` and `--device <udid or name>` to choose.

It exits 0 on a clean run. It exits 1 on an unexpected result or a new crash report,
or if the app dies or stops making progress, and prints each of them. It exits 2 if
the run could not be set up. Failures listed under Known Issues in the release notes
are marked as expected in `KNOWN` in the script. Keep that table in step with the
notes of the release you are building.

To look into a failure:

- `--only a,b` runs just those tests (`--list` prints the names).
- `--probe <test or command>` gives each command a launch of its own, so a crash
  points at the command that caused it.
- `--bisect <test>` finds which earlier test makes a test fail.
- `--keep` leaves the app installed and the simulator booted, and `-v` prints the
  known failures too.

## Runtime Output Smoke Check (in-app)
From the terminal tab, run:
```sh
pascal -v
clike -v
rea -v
rea --badflag
```
Expected: each `-v` prints a version line, and the bad flag produces an error
on stderr that still appears in the same terminal.
