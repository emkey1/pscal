# Aether card

*Card version: 2026-10-07-1*

Aether is a small statically typed language. Emit raw Aether source only.

## Program shape

- A program is top-level `const`, `type` and `fn` declarations, in any order;
  it runs `fn main() -> Void`.
- Every parameter is typed and every `fn` declares its return type:
  `fn add(a: Int, b: Int) -> Int`. Return with `ret value;`, or `ret;` in a
  `-> Void` function.
- **Effects.** `print`, `println` and every host call (random numbers, clock,
  files) must sit inside `fx { ... }`, which may hold `let`, `if` and `loop`.
  Plain computation needs no `fx`.

## Values

- Types: `Int`, `Real`, `Text`, `Bool`, `Void`, arrays `T[]` (nested `T[][]`),
  and your own `type`s.
- `let x: Int = 0;` declares a mutable local; reassign with `x = x + 1;`.
  Declare a name before using it, once per scope. `const` declares a constant.
- The type may be omitted for a literal or call initializer (`let n = 5;`).
  Always annotate array literals: `let xs: Int[] = [1, 2];`.

## Control flow

```aether
const LIMIT: Int = 3;

fn main() -> Void {
    let xs: Int[] = [4, 9, 2];
    let k: Int = LIMIT;
    fx {
        loop i in 0..3 { print(i, " "); }           // range, end excluded
        loop i in 10..0 step -3 { print(i, " "); }  // stepped range
        loop x in xs { print(x, " "); }             // each element
        loop ch in "ab" { print(ch, " "); }         // each character
        loop k > 0 { k = k - 1; }                   // while k > 0
        loop { break; }                             // until break
        if k > 0 {
            println("more");
        } else if k == 0 {
            println("done");
        }
    }
    ret;
}
```

```text
0 1 2 10 7 4 1 4 9 2 a b done
```

A range or foreach loop declares its own variable. `continue` skips to the
next iteration.

## Functions, tuples, `@pure`

```aether
@pure
fn minMax(xs: Int[]) -> (Int, Int) {
    let lo: Int = xs[0];
    let hi: Int = xs[0];
    loop x in xs {
        lo = min(lo, x);
        hi = max(hi, x);
    }
    ret (lo, hi);
}

fn main() -> Void {
    let (lo, hi) = minMax([4, 9, 2]);
    let t = minMax([7, 5]);
    fx { println(lo, " ", hi, " ", t.0); }
    ret;
}
```

```text
2 9 5
```

Destructure a direct call, or bind it and read `t.0`; never `f(x).0`. A
`@pure` function may not use `fx` and calls only pure builtins and other
`@pure` functions. Functions are not values (no lambdas), and there is no
`map`, `filter`, `sort`, `join` or `replace`: write a `loop`.

## Records and methods

```aether
type Counter {
    label: Text = "count";
    value: Int = 0;

    fn add(by: Int) -> Void {
        self.value = self.value + by;
        ret;
    }
}

fn main() -> Void {
    let c: Counter = new Counter { label: "hits" };
    let d: Counter = new Counter();
    c.add(2);
    c.add(3);
    fx { println(c.label, "=", c.value, " ", d.label, "=", d.value); }
    ret;
}
```

```text
hits=5 count=0
```

Give fields literal defaults (`0`, `""`, `[]`). `new T()` keeps them;
`new T { field: value }` overrides some. Methods live inside the `type`, use
`self.field`, and never declare a `self` parameter.

## Arrays

```aether
fn main() -> Void {
    let xs: Int[] = [];
    loop i in 0..5 {
        xs = xs + [i * i];
    }
    xs[0] = 7;
    let mid: Int[] = xs[1..3];
    let grid: Int[][] = [[1, 2], [3, 4]];
    fx { println(length(xs), " ", xs[4], " ", mid[0], " ", grid[1][0]); }
    ret;
}
```

```text
5 16 1 3
```

Append with `xs = xs + [v];`. Indexes are 0-based; `a..b` is half-open in
slices and loops. `println` does not print a whole array; loop over it.

## Text

```aether
fn main() -> Void {
    let s: Text = "hello world";
    let words: Text[] = split(s, " ");
    let n: Int = parse_int("41") + 1;
    fx {
        println(s[0], " ", s[0..5], " ", length(s), " ", s == "hello world");
        println(copy(s, 6, 5), " ", pos("o", s), " ", pos("z", s));
        println(words[1], trim("  !  "), " n=" + int_to_text(n));
    }
    ret;
}
```

```text
h hello 11 true
world 4 -1
world! n=42
```

`s[i]` is a one-character `Text`; `copy(s, start, count)` is a substring;
`pos(needle, s)` is the first index or `-1`. `+` joins `Text`; convert with
`int_to_text(n)` or `formatfloat(r, digits)`. `parse_int` gives `0` for
non-numeric text.

## Printing

- `println(a, b)` prints its arguments with **no separator**, then a newline;
  `print` adds no newline.
- A `Real` prints with 6 decimals: `println(2.5)` prints `2.500000`;
  `formatfloat(2.5, 1)` is `"2.5"`.
- Print exactly the requested output, nothing more.

## Diagnostics

Most errors carry a code in brackets.

| Code | Cause | Fix |
|---|---|---|
| FX-001 | effectful call outside `fx` | wrap it in `fx { ... }` |
| SYN-001 | `return`, `class`, `var`, an untyped parameter, no `-> T` | `ret`, `type`, `let`, `name: Type`, `-> Void` |
| SCOPE-001 | unknown name or invented builtin | declare it first, or write it as a loop |
| TYPE-001 | type cannot be inferred | annotate: `let xs: Int[] = [];` |
| TYPE-002 | unknown type such as `Integer` | a type listed under Values |
| BUILT-002 | wrong argument count | `formatfloat(r, 2)`, `copy(s, start, count)` |
| NAME-001 | local declared twice | assign without `let`, or rename |
| TUP-001 | `f().0`, or destructuring a non-call | bind first: `let t = f();` |
| ARR-002 | a `T[]` indexed twice | declare it `T[][]` |
| ARR-003 | index out of range at run time | check the bound; append before reading |
| ANN-001 | `@pure` function using `fx` or impure calls | move the effect out, or mark the callee `@pure` |
