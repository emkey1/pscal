# Aether for LLMs with Small Contexts

Aether is a compact PSCAL front end. It uses the existing backend, bytecode
compiler, and VM. It is not a separate runtime.

## Highest-Value Rules

1. **FX-001.** Every `print(...)`, `println(...)`, task helper call, and
   `ai_chat(...)` call must be inside `fx { ... }`.
2. **SYN-001.** Aether syntax only: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`. Never `return`, `class`, `for`, `while`, `var`,
   `def`, `func`, `=>`.
3. **BUILT-001.** The helpers named in this document are the complete callable
   surface. If a function is not listed here, it does not exist. Do not invent
   helpers (`substring`, `to_upper`, `parse_int`, ...).
4. **IMP-001.** Never invent `use "..."` imports. Only import verified modules.
5. **MOD-001.** If a module exports `getFortyTwo`, call `getFortyTwo`. Do not
   rename exports to `get_forty_two`, `AetherName`, `APP_NAME`, or other guesses.
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handles: no arithmetic,
   never assign one where the other is expected.
7. **TYPE-001.** If inference is not obviously safe, add the type explicitly.
8. **ANN-001.** `@pre`, `@post`, `@pure`, `@cost` go directly above the
   function, never inside it, never bare (`@pre` with no expression).
9. **MUT-001.** Plain `let` is already mutable. Never generate `let mut`.
10. **ORDER-001.** Define types, helpers, and modules before `main` uses them.
11. **LEN-001.** `toon_len(node)` for TOON arrays; `length(xs)` for dynamic
    arrays.
12. **TUP-001.** Tuples are narrow: `let (a, b) = pair();` on a direct
    top-level helper call only. Never `let value = pair();`. Tuple `@post`
    checks must use positional slots like `result.0`, `result.1`.
13. **OUT-001.** Return raw Aether source only. No Markdown fences.
14. **ROOT-001.** If the JSON starts with `{`, extract the named array with
    `toon_key(root, "...")` before iterating. Only iterate `root` directly
    when the JSON starts with `[`.
15. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. Do not rely on guessed globals or out-of-scope loop locals.
16. **METH-001.** Methods do not capture outer locals. If a method needs `i`,
    `name`, or another caller value, pass it as a parameter.
17. **FIELD-001.** Inside a method, a local may reuse a field name. Bare
    `valid` means the local; `self.valid` means the field.
18. **FIELD-002.** Record and type field names must exist exactly as declared.
    Do not invent fields.
19. **FLOW-001.** Every non-`Void` helper must return a value on every
    reachable top-level path.
20. **FMT-001.** If the prompt specifies exact output, match it exactly:
    spacing, casing, line order, and decimal precision.
21. **NAME-001.** Do not redeclare a local name in the same scope. Pick fresh
    names such as `values`, `count`, `sum`, `maxValue`.
22. **MOD-002.** Canonical import form is `use "module_name";`. After import,
    call exported names directly. Never guess `export { ... }`, `JsonDoc`,
    `json.parseFile(...)`, `Int.MIN`, or `value.toString()`.

Default stance: single-file programs; variadic `println("label = ", v)` over
mixed-type `+`; explicit types for TOON values and non-trivial helper results;
real logic, never hard-coded expected output; preserve provided export names
exactly.

## Core syntax

- Comments: prefer `// line comment`. Block comments are accepted, but models
  should still generate `//`. Text literals are double-quoted; escape `\"`.
- Types: `Int`, `Real`, `Text`, `Bool` (`true`/`false`), `Void`, plus opaque
  `ToonDoc`/`ToonNode`. `println(boolValue)` prints `true` or `false`. No
  conversion helpers exist; use variadic `println`.
- Operators: `+ - * / %`, `== != < <= > >=`, `!`, `&&`, `||`.

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

Every `fn` declares `-> ReturnType`; procedures use `-> Void` and bare `ret;`.
`fn helper(x: Int) { ... }` is invalid.

```aether
const Name: Text = "Aether";
let count: Int = 0;
count = count + 1;
```

Conditionals (parentheses optional; canonical multi-branch is sequential
`if` + `ret`):

```aether
if score >= 90 {
    ret "ready";
}
ret "blocked";
```

Statement-level `else` is supported.

Inline `if ... else ...` expressions are allowed only on the right-hand side
of declarations, assignments, and `ret` — never inside `println(...)` args:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
```

Loops (`break` exits; range is half-open):

```aether
loop index < total { index = index + 1; }
loop i in 0..count { fx { println(i); } }
loop { break; }
```

`continue` is supported.

## Records: `type`

```aether
type Counter {
    value: Int;            // fields end with ';'

    fn bump() -> Int {
        self.value = self.value + 1;   // lowercase self, never Self
        ret self.value;
    }
}
```

- `new Counter()` zeroes fields (Int `0`, Real `0.0`, Bool `false`, Text empty)
- canonical init: `Point { x: 3, y: 4 }`; `Point(x: 3, y: 4)` accepted
- records are pointer-backed: mutations through a callee are visible to the
  caller
- a top-level `fn bump(self: Counter) -> Int` is an extension method; call it
  as `counter.bump()`

## Safe inference

Omit the type only for: literals (`42`, `3.5`, `"text"`, `true`),
`new Type()`, and calls to functions/methods with known declared return types.
Annotate everything else — especially TOON extractions, branchy results, and
arithmetic where operand types aren't visible at a glance.

```aether
loop i in 0..5 {
    let square: Int = i * i;
    fx { println(i, " => ", square); }
}
```

## Effects (FX-001)

```aether
fn main() -> Void {
    fx {
        println("hello");   // good: inside fx
    }
    ret;
}
```

`println("hello");` at function scope is wrong — wrap it in `fx { ... }`.

## Printing and Real formatting

```aether
fx {
    println("Drop ", j, " -> ID: ", tx.id);  // variadic, never '+' guessing
    println(pct:0:2);                        // width:precision => 95.50
}
```

`println(realValue)` defaults to 6 decimals; use `value:0:2` style when exact
output matters. `Int / Int` is integer-style division; force a `Real` operand
for decimals:

```aether
let pct: Real = ok * 100.0 / total;
```

Exact-output discipline:

- print exactly the labels requested, for example `avg0=0`, not `avg0 = 0`
- for percentages and averages that must show decimals, format them explicitly
- do not add extra headings, blank lines, or explanatory text

## Text

```aether
if name == "Aether" { ... }            // canonical; string_eq(a,b) accepted
let nameLen: Int = string_len(name);   // canonical; name.len accepted
```

Treat this as the safe Text surface for generated code. Do not invent richer
helpers.

## Dynamic arrays

```aether
let xs: Int[] = [];
xs = xs + [7];              // append
let n: Int = length(xs);    // len(xs) and xs.len accepted
let first: Int = xs[0];     // indexed read
xs[0] = 9;                  // indexed write
let ys: Int[] = [1, 2, 3];  // multi-element literal
```

Never `toon_len(xs)` on a dynamic array. Indexed reads/writes and
multi-element literals are supported.

Use distinct local names inside one scope. Do not redeclare `xs`, `count`, or
other loop variables later in the same function.

## TOON rules

Helpers (complete surface): `has_toon()`, `toon_parse(text)`,
`toon_parse_file(path)`, `toon_root(doc)`, `toon_close(doc)`,
`toon_key(node, key)`, `toon_at(node, i)`, `toon_len(node)`,
`toon_get_text/int/real/bool(node, key)` plus `_or(node, key, fallback)`
variants, `toon_text/int/real/bool/null_value(node)`, `toon_type(node)`,
`toon_has_key(node, key)`, `toon_has_at(node, i)`,
`toon_is_text/int/real/bool/null/arr/obj(node)`.

Keys are `Text`, indexes `Int`. Always `toon_close(doc)`.

Handle ownership:

- `ToonDoc` owns all `ToonNode` handles derived from it
- `toon_root(...)`, `toon_key(...)`, and `toon_at(...)` create node handles
- `toon_free(node)` releases one node handle early
- `toon_close(doc)` releases the document and any remaining child handles
- for short-lived documents, `toon_close(doc)` at the end is usually enough
- in large loops, prefer `toon_free(...)` for temporary nodes to avoid
  unnecessary handle buildup before document close

Root shape (ROOT-001):

```aether
// JSON starts with '[': root IS the array
loop i in 0..toon_len(root) {
    let row: ToonNode = toon_at(root, i);
}

// JSON starts with '{': extract the named array first
let jobs: ToonNode = toon_key(root, "jobs");
loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
}
```

Never do this:

```aether
let doc: ToonDoc = toon_parse_file("payload.json");
let name: Text = toon_get_text(doc, "name");
```

Always do this:

```aether
let doc: ToonDoc = toon_parse_file("payload.json");
let root: ToonNode = toon_root(doc);
let name: Text = toon_get_text(root, "name");
```

Large-loop pattern:

```aether
let doc: ToonDoc = toon_parse_file(path);
let root: ToonNode = toon_root(doc);
let jobs: ToonNode = toon_key(root, "jobs");

loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
    let name: ToonNode = toon_key(job, "name");

    fx {
        println(toon_text_value(name));
    }

    toon_free(name);
    toon_free(job);
}

toon_free(jobs);
toon_close(doc);
```

Key fidelity (KEY-001): copy JSON keys exactly. Never flatten nested objects
into guessed keys (`"appName"`, `"logLevel"`) or dotted keys (`"server.port"`):

```aether
let server: ToonNode = toon_key(root, "server");
let port: Int = toon_get_int_or(server, "port", 0);
```

Nested lookups (NEST-001): `_or` protects only the final lookup, not the path.
Guard intermediates:

```aether
let code: Text = "EMPTY";
if toon_has_key(row, "meta") {
let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

Never: `toon_get_text_or(toon_key(toon_at(root, i), "meta"), "code", "EMPTY");`

Never generate foreign JSON/object APIs such as `JsonDoc`, `JsonNode`,
`json.parseFile(...)`, `root.get(...)`, `Int.MIN`, or `value.toString()`.

## Tasks and AI

`sleep(ms: Int) -> Void`, `task_spawn(target: Text, name: Text, arg) -> Int`,
`task_queue(target: Text, name: Text, arg) -> Int`,
`task_wait(handle: Int) -> Int`, `task_lookup(name: Text) -> Int`,
`task_status(handle: Int) -> Int`, `task_result(handle: Int) -> Int`,
`task_stats() -> Array`, `task_stats_json() -> Text`,
`ai_chat(model: Text, messages: Text, system: Text = "", apiKey: Text = "", endpoint: Text = "") -> Text`,
probes `has_ai() -> Bool`, `has_builtin(category: Text, function: Text) -> Bool`.
All are effectful and must stay inside `fx`. `sleep(ms)` is a blocking
millisecond pause. `task_wait` waits on a task handle, not a duration.

Discovery exists:
- `builtins_json()` -> JSON list of available Aether-visible builtins
- `builtins_json(true)` -> richer JSON metadata
- `builtin_info(name)` -> JSON metadata for one builtin

## Imports (IMP-001, MOD-001)

Imports are advanced; most generated Aether should be single-file.

- only `use "..."` for real, verified modules; a missing import's symbols do
  not exist even if the line is silently ignored
- **MOD-001**: imported names match exported names exactly — `use` does not
  rename. Module exports `classifySupport` → call `classifySupport(...)`,
  never a guessed `classify(...)`. Module exports `Answer` → use `Answer`,
  not `APP_NAME`, `AetherName`, or another invented spelling.
- if a task provides exports named `clampSupport`, `classifySupport`, or
  `PassMark`, call those exact names
- file naming: `mod ModuleConsts` → `use "module_consts";`
- when combining purity with module export, write `@pure` above `export fn`
- for generated code, assume modules export `const` and `fn`; do not generate
  exported `type` blocks

```aether
use "bench_support";
let score: Int = clampSupport(raw);
let status: Text = classifySupport(score);
```

Minimal import recipe:

```aether
use "bench_math";

fn main() -> Void {
    let value: Int = answer();
    fx {
        println(value);
    }
    ret;
}
```

## Contracts (ANN-001)

```aether
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    ret score;
}
```

Bad: bare `@pre` / `@post` with no expression; annotations inside the body.
`@post` may reference `result`. `@cost 5ms` units: `ns us ms s op ops step
steps`.

## Copyable templates

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

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("yyjson unavailable"); }
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

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("yyjson unavailable"); }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\"rows\":[{\"meta\":{\"code\":\"A1\"}},{\"meta\":{}},{\"broken\":true}]}");
    let root: ToonNode = toon_root(doc);
    let rows: ToonNode = toon_key(root, "rows");
    let missing: Int = 0;

    loop i in 0..toon_len(rows) {
        let row: ToonNode = toon_at(rows, i);
        let code: Text = "EMPTY";
        if toon_has_key(row, "meta") {
            let meta: ToonNode = toon_key(row, "meta");
            code = toon_get_text_or(meta, "code", "EMPTY");
        }
        if code == "EMPTY" {
            missing = missing + 1;
        }
        fx {
            println("row ", i, " = ", code);
        }
    }

    fx {
        println("missing = ", missing);
    }

    toon_close(doc);
    ret;
}
```

Large-report recipe:

- parse file
- `let root: ToonNode = toon_root(doc);`
- `let items: ToonNode = toon_key(root, "...");`
- one pure normalize helper
- one pure classify helper
- one mutable totals type
- one loop that extracts, classifies, updates totals, prints
- one final totals block
- `toon_close(doc);`

## Repair rules

- error mentions `println`/`print`/task/`ai_chat` → wrap in `fx` (FX-001)
- mentions `return` → `ret`; mentions `class` → `type` (SYN-001)
- mentions unknown function → it does not exist; inline the logic (BUILT-001)
- mentions unknown import → remove `use` (IMP-001)
- mentions `not in scope` → declare the name earlier or pass it explicitly
  (SCOPE-001)
- mentions `Unknown field` → use the exact declared field name, or add the
  field to the type if the prompt truly requires it (FIELD-002)
- mentions fallthrough/no return value → add an explicit final `ret ...`
  on every reachable top-level path (FLOW-001)
- mentions `ToonDoc` where a node is expected → add `let root: ToonNode = toon_root(doc);`
- mentions wrong output or mismatch → remove extra headings/labels and match
  punctuation, spacing, and decimal precision exactly
- mentions `ToonDoc`/`ToonNode` → check handle types (TOON-001)
- mentions a tuple → destructure a direct call only (TUP-001)
- mentions annotation placement → move above the function (ANN-001)
- integer result where decimals expected → add a `Real` operand (`100.0`)
- iterating an object root → `toon_key` the array first (ROOT-001)

## Validation checklist

- all output, task, and `ai_chat` calls inside `fx { ... }` (FX-001)
- every called helper appears in this document (BUILT-001)
- imports verified; exported names used exactly (IMP-001, MOD-001)
- all parameters typed; uncertain types annotated (TYPE-001)
- `ret` not `return`; `type` not `class`; no `let mut` (SYN-001, MUT-001)
- no arithmetic on / cross-assignment of TOON handles; docs closed (TOON-001)
- object roots: named array extracted (ROOT-001); keys copied exactly
  (KEY-001); intermediates guarded before `_or` (NEST-001)
- `toon_len` vs `length` used correctly (LEN-001)
- `Real` operand where decimals matter; `value:width:precision` for stable output
- tuples destructured directly (TUP-001); annotations above functions (ANN-001)
