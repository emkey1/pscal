# CLike language examples

This folder contains sample programs for the Pscal CLike front end.

Clike files can be run by running the 'clike' binary, which will compile them 
bytecode and execute the bytecode in the PSCAL VM.

C-like examples (`.cl`) can be executed with the native clike compiler built in
`build/bin`:

```
build/bin/clike Examples/Clike/<program>
```

## Programs
- `factorial_native` – Calculates factorials using a native function
- `factorial_ext` – Calculates factorials using an extended builtin function
- `fibonacci_native` – Calculates fibonacci numbers using a native function
- `fibonacci_ext` – Calculates fibonacci numbers using an extended builtin function
- `chudnovsky_native` – Approximates π using a native Chudnovsky implementation
- `chudnovsky_ext` – Approximates π using an extended builtin implementation

- `hangman5` – text-based hangman game ported from Pascal.
- `hello` – Hello World!
- `module_demo` – demonstrates importing `math_utils.cl` from the clike
   library search path.
- `sdl_multibouncingballs` – SDL multi bouncing balls demo ported from Pascal.
- `sdl_mandelbrot_interactive` – SDL Mandelbrot renderer using the MandelbrotRow builtin; left click to zoom in, right click to zoom out.
- `sdl_smoke` – Rotating 3D cube demo that exercises the OpenGL helpers (requires building with SDL support).
- `sdl_getmousestate` – SDL demo printing mouse coordinates and button states.
- `show_pid` – Uses an extended builtin function to show the process ID
- `sqlite_yyjson_demo` – Uses extended builtins for SQLite and Yyjson to load
   JSON data into a database after checking availability with `#ifdef`
- `sqlite_http_import_demo` – Downloads a CSV dataset over HTTP, imports it
   into SQLite, and runs parameterised summary queries
- `sort_string` – Shows how to copy and sort a string via a `str*` parameter
- `vm_version_demo` – Prints VM and bytecode versions and exits on mismatch

The clike front end resolves imports by first checking the directory in the
`CLIKE_LIB_DIR` environment variable and falling back to
`${PSCAL_INSTALL_ROOT}/clike/lib` (which defaults to
`${CMAKE_INSTALL_PREFIX}/pscal/clike/lib` unless overridden at configure
time).
