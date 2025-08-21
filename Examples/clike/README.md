# Clike language examples

This folder contains sample programs for the Pscal clike front ends.

Tiny language (`.tiny`) programs can be compiled to bytecode with:

```
tools/clike Examples/Clike/<program>.tiny /tmp/<program>.pbc
```

C-like examples (`.cl`) can be executed with the native clike compiler built in
`build/bin`:

```
build/bin/clike Examples/Clike/<program>.cl
```

## Programs

- `countdown.tiny` – read an integer and count down to 1.
- `max.tiny` – read two integers and print the larger value.
- `sum.tiny` – compute the sum of 1..n.
- `sdl_multibouncingballs.cl` – SDL multi bouncing balls demo ported from Pascal.
- `hangman5.cl` – text-based hangman game ported from Pascal.
- `module_demo.cl` – demonstrates importing `math_utils.cl` from the clike
  library search path.

The clike front end resolves imports by first checking the directory in the
`CLIKE_LIB_DIR` environment variable and falling back to
`/usr/local/pscal/clike/lib`.
