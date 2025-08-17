# Clike language examples

These programs demonstrate the clike language that `tools/clike` compiles to
Pscal bytecode. Each source file can be compiled with:

```
python tools/clike Examples/Clike/<program>.tiny /tmp/<program>.pbc
```

Run the resulting bytecode with `pscalvm` from a built tree.

## Programs

- `countdown.tiny` – read an integer and count down to 1.
- `max.tiny` – read two integers and print the larger value.
- `sum.tiny` – compute the sum of 1..n.

