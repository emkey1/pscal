# Xcode Project

The repository ships with a hand-maintained Xcode project that mirrors the
standard CMake build.  It lives at [`xcode/Pscal.xcodeproj`](../xcode/Pscal.xcodeproj)
and can be regenerated at any time by running the helper script:

```
python tools/generate_xcodeproj.py
```

Running the script overwrites `project.pbxproj`, the workspace metadata, and the
shared `pascal` scheme.  The script intentionally produces deterministic object
identifiers so that version control diffs stay readable.

## Targets

The following command-line targets are available inside Xcode:

- **pascal** – Pascal front-end with extended builtins (default scheme)
- **pscalvm** – Standalone bytecode VM launcher
- **dascal** – Debug build of the Pascal front-end
- **pscald** – Standalone bytecode disassembler
- **clike** / **clike-repl** – Tiny C front-end and REPL
- **rea** – REA front-end
- **pscaljson2bc** – JSON-to-bytecode conversion utility

Every target inherits the same header search paths and links against the system
`curl`, `sqlite3`, `m`, and `pthread` libraries.  Extended builtins are enabled
by default (`ENABLE_EXT_BUILTIN_*` macros) to keep feature parity with the
CMake build.

## Workflow Tips

- Open `Pscal.xcodeproj` and pick the shared **pascal** scheme to build or run
  the main compiler.
- When new `.c` files are added, rerun `python tools/generate_xcodeproj.py`
  so that the project picks them up.
- SDL-dependent sources are present but guarded by `#ifdef SDL`; see the next
  section for enabling the SDL runtime inside Xcode.

## Enabling SDL Support in Xcode

The generated project keeps SDL optional so that builds succeed on machines
that do not have the frameworks installed.  To toggle SDL on for any of the
schemes:

1. Select the **Project** navigator, click the `Pscal` project, then pick the
   **Build Settings** tab for the desired target (for example **pascal**).
2. Under **Preprocessor Macros** add `SDL=1` to both the Debug and Release
   configurations.  Setting it at the project level will cascade to every
   target.
3. Open the **Build Phases** tab, expand **Link Binary With Libraries**, and add
   the SDL frameworks you have installed (for example `SDL3.framework`,
   `SDL3_ttf.framework`, and `SDL3_mixer.framework`, or the SDL2 equivalents)—
   or point the project at the corresponding Homebrew `.dylib`s if you prefer.
4. The generated project now seeds common SDL include locations—Homebrew's
   `/opt/homebrew/include`, `/usr/local/include`, and the standard
   `SDL3.framework`/`SDL2.framework` headers—so most macOS setups work without
   extra tweaks. If your installation lives somewhere else, add its parent folder to
   **Header Search Paths**.
5. Clean and rebuild; SDL-only examples such as
   `Examples/pascal/sdl/MultiBouncingBalls` will now launch from within Xcode.

To disable SDL again, remove the frameworks and the `SDL` macro entry—no other
changes are required.
