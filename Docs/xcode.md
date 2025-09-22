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
- **pscal-runner** – Helper executable that launches another PSCAL binary

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

## Running Different Executables

The shared **pascal** scheme now builds a lightweight `pscal-runner` helper.
When you press **Run**, the helper invokes whichever PSCAL binary you request
and forwards any launch arguments.

- Use the scheme's *Run > Environment Variables* table to change the
  `PSCAL_RUN_TARGET` value (default `pascal`). Set it to another product name
  such as `pscalvm`, `clike`, or `pscaljson2bc`.
- Alternatively, enable `PSCAL_RUN_EXECUTABLE` and point it at a specific
  relative or absolute path if you need to run a custom build.
- Arguments added under *Run > Arguments Passed On Launch* are passed directly
  to the selected executable so you can test different flag combinations
  without editing source files.

This keeps the build products in sync—every binary listed above is built when
you run the scheme—while making it easy to swap the active executable and its
command-line arguments from inside Xcode.
