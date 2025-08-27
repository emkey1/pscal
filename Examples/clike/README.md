# CLike language examples

This folder contains sample programs for the Pscal CLike front end.

Clike files can be run by running the 'clike' binary, which will compile them 
byte code and execute the byte code in the PSCAL VM.

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
- `sdl_multibouncingballs.cl` – SDL multi bouncing balls demo ported from Pascal.
- `sdl_mandelbrot_row` – SDL Mandelbrot renderer using the MandelbrotRow builtin; left click to zoom in, right click to zoom out.
- `show_pid` – Uses an extended builtin function to show the process ID
- `sort_string` – Shows how to copy and sort a string via a `str*` parameter
- `vm_version_demo` – Prints VM and bytecode versions and exits on mismatch

The clike front end resolves imports by first checking the directory in the
`CLIKE_LIB_DIR` environment variable and falling back to
`/usr/local/pscal/clike/lib`.
