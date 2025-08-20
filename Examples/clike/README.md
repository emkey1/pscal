# Clike language examples

This folder contains sample programs for the Pscal clike front ends.

Tiny language (`.tiny`) programs can be compiled to bytecode with:

```
tools/clike Examples/Clike/<program>.tiny /tmp/<program>.pbc
```

C-like examples (`.c`) can be executed with the native clike compiler built in
`build/bin`:

```
build/bin/clike Examples/Clike/<program>.c
```

## Programs

- `countdown.tiny` – read an integer and count down to 1.
- `max.tiny` – read two integers and print the larger value.
- `sum.tiny` – compute the sum of 1..n.
- `sdl_multibouncingballs.c` – SDL multi bouncing balls demo ported from Pascal.
- `hangman5.c` – text-based hangman game ported from Pascal.
