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

## A complete program to copy

```aether
type Item {
    name: Text = "";
    qty: Int = 0;
}

@pure
fn total(items: Item[]) -> Int {
    let sum: Int = 0;
    loop it in items {
        sum = sum + it.qty;
    }
    ret sum;
}

fn main() -> Void {
    let items: Item[] = [];
    items = items + [new Item { name: "pen", qty: 3 }];
    items = items + [new Item { name: "ink", qty: 4 }];
    let avg: Real = total(items) / 2;
    fx {
        loop i in 0..length(items) {
            if items[i].qty > 3 {
                println(i, ": ", items[i].name, " big");
            } else {
                println(i, ": ", items[i].name, " small");
            }
        }
        println("total ", total(items), " avg ", formatfloat(avg, 2));
    }
    ret;
}
```

```text
0: pen small
1: ink big
total 7 avg 3.50
```

- Every `fn` types its parameters and declares `-> T`; `main` is `fn main() -> Void` and ends with `ret;`.
- `print`/`println` sit inside `fx { ... }`; they print their arguments with no separator.
- `a..b` excludes `b`. Indexes start at 0. `s[i]` is a one-character `Text`.
- Text: `+` joins, `int_to_text(n)`, `formatfloat(r, digits)`, `parse_int(t)`, `split(s, " ")`, `trim(s)`, `copy(s, start, count)`, `pos(needle, s)`.
- A `Real` prints with 6 decimals; use `formatfloat(r, 2)` for 2.
- There is no `map`, `filter`, `sort` or `join`: write a `loop`.

## Reading a JSON file

Input files are read with the TOON builtins, inside `fx`. Check `has_toon()`
first, open the file, take its root, then walk keys and indexes:

```aether
fn main() -> Void {
    fx {
        if !has_toon() {
            println("yyjson unavailable");
        } else {
            let doc: ToonDoc = toon_parse_file("items.json");
            let root: ToonNode = toon_root(doc);
            let items: ToonNode = toon_key(root, "items");
            loop i in 0..toon_len(items) {
                let it: ToonNode = toon_at(items, i);
                println(toon_get_text(it, "name"), " ", toon_get_int(it, "qty"));
            }
            toon_close(doc);
        }
    }
    ret;
}
```

Getters: `toon_get_text`, `toon_get_int`, `toon_get_real`, `toon_get_bool`
(node, key), and `toon_get_int_or(node, key, fallback)` and friends when a key
may be missing. Never paste the file's data into the program.

## Errors

A message with `[SYN-001]` names the syntax to change; `[SCOPE-001]` an unknown
name (declare it, or write the loop yourself); `[FX-001]` a print outside `fx`.
