# Aether for LLMs with Small Contexts

Maintenance note:

- this is the compact companion to
  `Docs/aether_for_llms_and_others.md`
- keep this file short and rule-focused
- refresh it whenever the full document changes

## Highest-Value Rules

Aether is a compact PSCAL front end. It uses the existing backend, bytecode
compiler, and VM. It is not a separate runtime.

Read these rules first:

1. Every `print(...)`, `println(...)`, task helper call, and `ai_chat(...)`
   call must be inside `fx { ... }`.
2. Use Aether syntax only: `fn`, `let`, `const`, `ret`, `if`, `loop`, `type`,
   `mod`, `use`. Do not invent Python, Rust, or JavaScript syntax.
3. Never invent `use "..."` imports. Only import verified modules.
4. Treat `ToonDoc` and `ToonNode` as opaque handles.
5. If inference is not obviously safe, add the type explicitly.
6. Put `@pre`, `@post`, `@pure`, and `@cost` above the function, never inside it.
7. Prefer plain `let`. `let mut` is accepted, but redundant and ignored.
8. Define types, helper functions, and modules before `main` uses them.
9. Use `toon_len(node)` for TOON arrays and `length(arrayValue)` for dynamic arrays.
10. Tuple support is narrow. Destructure direct helper returns only.
11. Return raw Aether source only. Do not wrap the answer in Markdown fences.
12. Every `fn` must declare `-> ReturnType`. Use `-> Void` for procedures.
13. Inside `type { ... }`, fields end with `;`, not `,`.

Default generation stance:

- prefer a single-file program unless a real module is explicitly provided
- prefer `println("label = ", value)` over mixed-type string concatenation
- prefer explicit types whenever a value came from TOON or a non-trivial helper

## Core syntax

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

`fn helper(x: Int) { ... }` is invalid. Write `fn helper(x: Int) -> Void { ... }`.

```aether
let count: Int = 0;
const Name: Text = "Aether";
```

```aether
if score >= 90 {
    ret "ready";
}
```

```aether
loop i in 0..count {
    fx {
        println(i);
    }
}
```

```aether
type Counter {
    value: Int;

    fn bump() -> Int {
        self.value = self.value + 1;
        ret self.value;
    }
}
```

Type fields always end with semicolons:

```aether
type JobSummary {
    name: Text;
    score: Int;
}
```

Current record-style initialization syntax:

```aether
let p: Point = Point {
    x: 3,
    y: 4
};
```

Also accepted:

```aether
let p: Point = Point(x: 3, y: 4);
```

Generate the brace form by default.

`new Type()` gives fields type-appropriate defaults in Aether:

- integers start at `0`
- reals start at `0.0`
- booleans start at `false`
- strings start empty
- pointers start at `nil`

## Safe Inference

Safe patterns:

- `let x = 42;`
- `let x = 3.5;`
- `let x = "text";`
- `let x = true;`
- `let x = new Type();`
- `let x = knownFunction(...);`
- `let x = knownTypedValue.method(...);`

`let` bindings are already mutable. `let mut` is accepted, but redundant and
ignored.

Prefer explicit types for:

- TOON handles and extracted TOON values
- branchy results when the final type is not visually obvious
- public examples where clarity matters

## Effects

Good:

```aether
fn main() -> Void {
    fx {
        println("hello");
    }
    ret;
}
```

Bad:

```aether
fn main() -> Void {
    println("hello");
    ret;
}
```

## Printing And Real Formatting

- `print(...)` and `println(...)` are preferred for mixed-type output
- prefer `println(a, b, c)` over string-building with `+`

Real formatting:

```aether
println(value);      // default backend format, usually 6 decimals
println(value:0:2);  // 2 decimals
println(value:8:3);  // width 8, precision 3
```

Division rule:

- `a / b` with `Int` operands behaves like integer-style arithmetic
- for real-valued results, force a `Real` operand:

```aether
let pct: Real = ok * 100.0 / total;
```

Inline `if ... else ...` expressions are supported only on the right-hand side
of declarations, assignments, and `ret`. Do not place them directly inside
`println(...)` argument lists.

## Inline Conditional Expressions

Supported on the right-hand side of declarations, assignments, and returns:

```aether
let score: Int = if count > 0 { total / count } else { 0 };
```

## Text Equality

Preferred:

```aether
if name == "Aether" {
    ret true;
}
```

String length:

```aether
let len: Int = string_len(name);
let len2: Int = name.len;   // accepted
let len3: Int = values.len; // accepted for dynamic arrays
```

Generate `string_len(name)` or `length(values)` by default. Accept `.len` when
reading or repairing code.

Also accepted:

```aether
if string_eq(name, "Aether") {
    ret true;
}
```

## TOON Rules

Use:

- `ToonDoc` for parsed documents
- `ToonNode` for nodes

Common helpers:

- `has_toon()`
- `toon_parse(text)`
- `toon_parse_file(path)`
- `toon_root(doc)`
- `toon_close(doc)`
- `toon_key(node, key)`
- `toon_at(node, index)`
- `toon_len(node)`
- `toon_get_text(...)`
- `toon_get_int(...)`
- `toon_get_real(...)`
- `toon_get_bool(...)`
- `_or` fallback variants

Never:

- do arithmetic on `ToonDoc` or `ToonNode`
- assign `ToonDoc` where `ToonNode` is expected
- assign `ToonNode` where `ToonDoc` is expected

Safe pattern:

```aether
let doc: ToonDoc = toon_parse(payload);
let root: ToonNode = toon_root(doc);
let name: Text = toon_get_text(root, "name");
toon_close(doc);
```

Important nested-lookup rule:

- `_or` helpers only protect the final lookup
- they do not make the entire chain safe

Unsafe:

```aether
let code: Text = toon_get_text_or(toon_key(toon_at(root, i), "meta"), "code", "EMPTY");
```

Safer:

```aether
let row: ToonNode = toon_at(root, i);
let code: Text = "EMPTY";

if toon_has_key(row, "meta") {
    let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

If the payload root is an object like `{"jobs":[...]}` or `{"releases":[...]}`
then extract the array field first:

```aether
let root: ToonNode = toon_root(doc);
let jobs: ToonNode = toon_key(root, "jobs");

loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
}
```

Golden TOON rule:

- if JSON starts with `[` then iterate `root`
- if JSON starts with `{` then usually extract a named array field first
- never assume `root` itself is the row array in object-shaped payloads

Bad:

```aether
loop i in 0..toon_len(root) {
    let row: ToonNode = toon_at(root, i);
}
```

Good:

```aether
let rows: ToonNode = toon_key(root, "jobs"); // or transactions/releases/users
loop i in 0..toon_len(rows) {
    let row: ToonNode = toon_at(rows, i);
}
```

Copy JSON keys exactly:

- use `"name"` if the payload says `"name"`
- use `"level"` if the payload says `"level"`
- do not rewrite nested keys into guessed top-level keys like `"appName"` or `"logLevel"`

Example:

```aether
let server: ToonNode = toon_key(root, "server");
let app: ToonNode = toon_key(root, "app");
let log: ToonNode = toon_key(root, "log");
let port: Int = toon_get_int_or(server, "port", 0);
let name: Text = toon_get_text_or(app, "name", "");
let level: Text = toon_get_text_or(log, "level", "info");
```

## Imports

Treat imports as advanced. Most Aether generated by LLMs should be single-file.

- only use `use "..."` for real, verified modules
- never invent `use "helpers";` or similar support modules
- if no module is provided, keep the example self-contained
- imported symbols are not renamed automatically
- if a missing import line is preserved for compatibility, its symbols still do
  not exist unless the module is real
- copy export names exactly; do not rename them mentally

Imported exported names are used directly:

```aether
use "bench_math";
use "bench_consts";

fn main() -> Void {
    fx {
        println(answer());
        println(Greeting);
        println(Answer);
    }
    ret;
}
```

If a module exports `classifySupport`, call `classifySupport(...)`.
Do not rename it to `classify(...)` unless you define a wrapper.

Example:

```aether
use "bench_support";
let raw: Int = toon_get_int_or(job, "score", DefaultScore);
let score: Int = clampSupport(raw);
let status: Text = classifySupport(score);
```

## Contracts

Good:

```aether
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    ret score;
}
```

Bad:

```aether
@pre
@post
fn normalize(score: Int) -> Int {
    ret score;
}
```

Also bad:

```aether
fn normalize(score: Int) -> Int {
    @pre score >= 0
    ret score;
}
```

## Tuples

Supported narrowly:

```aether
fn pair() -> (Int, Int) {
    ret (1, 2);
}

let (a, b) = pair();
```

Do not do this:

```aether
let value = pair();
```

## Copyable templates

Pure helper plus effectful main:

```aether
@pure
fn transform(value: Int) -> Int {
    ret value + 1;
}

fn main() -> Void {
    let answer: Int = transform(41);
    fx {
        println("answer = ", answer);
    }
    ret;
}
```

TOON-driven program:

```aether
fn main() -> Void {
    if !has_toon() {
        fx {
            println("yyjson unavailable");
        }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\"name\":\"Aether\"}");
    let root: ToonNode = toon_root(doc);
    let name: Text = toon_get_text(root, "name");

    fx {
        println("name = ", name);
    }

    toon_close(doc);
    ret;
}
```

## Validation checklist

- all `print(...)` and `println(...)` calls are inside `fx { ... }`
- all task helper calls and `ai_chat(...)` calls are inside `fx { ... }`
- all imports reference verified modules
- imported constants are not redefined locally unless the task explicitly requires it
- all function parameters have explicit types
- no arithmetic is performed on `ToonDoc` or `ToonNode`
- `ToonDoc` values are closed with `toon_close(doc)`
- TOON keys are `Text` and indexes are `Int`
- decimal arithmetic uses a `Real` operand when needed
- decimal output uses `value:width:precision` when formatting matters
- tuple-return calls are destructured directly
- annotations are above the function, not inside it
