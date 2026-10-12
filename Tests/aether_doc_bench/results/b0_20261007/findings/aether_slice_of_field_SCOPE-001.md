# Compiler defect: a range slice of a field fails with a leaked temporary (SCOPE-001)

Found during the B0 boards (2026-10-07..10). Diagnosis only; nothing in the aether repo was changed.

## Symptom

`obj.field[lo..hi]` does not compile:

    t2_field_slice.aether:6: [SCOPE-001] identifier '__aether_slice_0' not in scope.

`__aether_slice_N` is a temporary the compiler itself generates, so the diagnostic names something the
author never wrote and gives no hint at the real cause or a workaround.

## Minimal reproduction (`slice_field_repro/`)

| File | Expression | 2026-09-23-1 | 2026-10-06-1 (B0 binary) | 2026-10-08-1 (L1) |
|---|---|---|---|---|
| t1_local_slice | `s[0..5]`, `s` a local Text | (ok) | prints `hello` | prints `hello` |
| t2_field_slice | `b.t[0..5]`, `t` a Text field | SCOPE-001 | SCOPE-001 | SCOPE-001 |
| t3_field_via_local | `let s = b.t; s[0..5]` | (not run) | prints `hello` | prints `hello` |
| t4_field_array_slice | `b.xs[0..2]`, `xs` an `Int[]` field | SCOPE-001 | SCOPE-001 | SCOPE-001 |
| t6_local_array_slice | `xs[0..2]`, `xs` a local `Int[]` | (not run) | prints `2` | prints `2` |

So: slicing a local works, indexing a field works (`b.xs[1]`), slicing a field does not, for Text and for
arrays, inside methods (`self.expr[a..b]`) and outside (`p.text[a..b]`). It is not a recent regression and
L0/L1 did not fix it.

## Likely cause

`buildArraySlice` in `src/aether/ast_lower.c` picks between "Text slice -> `copy(base, lo, hi - lo)`" and
"array slice -> hoisted `__aether_slice_N` temp plus a copy loop" from `inferLetTypeName(p, base)`. For a
field-access base that inference yields no type at parse time, so a Text field misses the `copy` path, and the
array path's hoisted declaration does not end up in scope of its own use. (Read from the source; not confirmed
with a debugger.)

## Effect on the boards

5 of the first 805 cases hit it (4x `hard2_expr_eval`, 1x `hard2_heap_sort`), from four model/host
combinations: local qwen3.6-35b-a3b, local devstral-small-2, OpenRouter qwen3.6-35b-a3b (2x) and
OpenRouter qwen3.6-27b. In all three programs checked by hand (one per model family and host), replacing only the
slice with its documented equivalent (`copy(...)`, or copying the field to a local first) makes the program
print the task's expected output exactly: the models' programs were correct, and the defect turned a
first-attempt pass into a failure. Four of the five were fixed in a repair round (the model rewrote the
slice); one (devstral, `hard2_heap_sort`) never recovered.

The scores stand as measured (that is what the frozen compiler does), but `hard2_expr_eval` first-attempt
failures with this diagnostic are compiler-caused, not guide- or model-caused. A paired replay on a fixed
compiler should show them as fail->pass.

## Suggested handling

1. Fix the lowering so a field base is typed (or lower the slice after types are known), with these five
   programs as fixtures.
2. Until then, either the guide says to copy a field to a local before slicing, or the diagnostic says so:
   a user-facing error should never name a compiler temporary.
