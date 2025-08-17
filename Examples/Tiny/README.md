# Tiny language examples

These programs demonstrate the tiny language that `tools/tinyc.py` compiles to
Pscal bytecode. Each source file can be compiled with:

```
python tools/tinyc.py Examples/Tiny/<program>.tiny /tmp/<program>.pbc
```

Run the resulting bytecode with `pscalvm` from a built tree.

## Programs

- `countdown.tiny` – read an integer and count down to 1.
- `max.tiny` – read two integers and print the larger value.
- `sum.tiny` – compute the sum of 1..n.

