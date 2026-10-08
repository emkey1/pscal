Aether is a small statically typed language. Emit raw Aether source only.

## Not Python, JavaScript or Rust

Aether looks familiar but differs. Always write the left column:

| Write | Never write |
|---|---|
| `ret x;` and `ret;` | `return` |
| `fn add(a: Int, b: Int) -> Int { ... }` | `def`, `function`, `fn add(a, b)` |
| `loop i in 0..n { ... }`, `loop x in xs { ... }`, `loop k > 0 { ... }` | `for`, `while` |
| `x = x + 1;` | `x += 1`, `x++` |
| `Int`, `Real`, `Text`, `Bool`, `T[]` | `int`, `float`, `Float`, `String`, `str`, `List` |
| `let xs: Int[] = [];` then `xs = xs + [v];` | `xs.push(v)`, `xs.append(v)` |
| `length(xs)`, `length(s)` | `len(xs)`, `xs.length`, `xs.size()` |
| `const N: Int = 3;` (a single value) | a `const` holding an object, map or file data |
| `fx { println("n = ", n); }` | printing outside `fx`, `"{}"` or `f"..."` |

## Example

```aether
fn square(x: Int) -> Int {
    ret x * x;
}

fn main() -> Void {
    let xs: Int[] = [];
    loop i in 0..4 {
        xs = xs + [square(i)];
    }
    fx {
        loop x in xs { print(x, " "); }
        println("");
        println("count ", length(xs));
    }
    ret;
}
```

```text
0 1 4 9 
count 4
```

Print exactly the requested output. Real numbers: `formatfloat(r, 2)`.
