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
- SDL-dependent sources are present but guarded by `#ifdef SDL`; if you need the
  SDL runtime in Xcode, add the SDL frameworks via the target build settings and
  define the `SDL` macro.
