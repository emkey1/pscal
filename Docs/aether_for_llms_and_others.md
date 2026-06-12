# Aether for Humans and LLMs

Maintenance note:

- full reference / source-of-truth version; keep it human-usable and precise
  enough to extract into small-context LLM prompts
- when this file changes, refresh `Docs/aether_for_llms_with_small_contexts.md`
  in the same commit; **LLM-critical** sections almost always require that update
- resolve every `[VERIFY: ...]` marker against the compiler, then delete the
  marker; never ship a VERIFY marker, since this document is the only way most
  LLMs learn Aether

Aether is a compact front end for the PSCAL suite. It targets the existing
shared PSCAL backend, bytecode compiler, and VM. It is not a separate runtime.

If you only read one part of this document, read **Highest-Value Rules** and
**Never Generate These**.

## Rule labels

- **Hard rule**: violating this usually produces invalid Aether or wrong behavior.
- **Canonical**: preferred style for new Aether, especially LLM-generated Aether.
- **Accepted**: may compile, but generate only when preserving existing code.
- **Avoid**: likely wrong, fragile, or misleading even if it sometimes works.
- **LLM-critical**: high-value rule mirrored in the small-context guide.

## Highest-Value Rules

**LLM-critical.** These determine whether generated Aether works on the first try.

1. **FX-001.** Every `print(...)`, `println(...)`, task helper call, and AI
   helper call must be inside `fx { ... }`.
2. **SYN-001.** Use Aether keywords: `fn`, `let`, `const`, `ret`, `if`, `loop`,
   `type`, `mod`, `use`. Do not import syntax from Python, Rust, JavaScript,
   Go, or Pascal.
3. **TYPE-001.** Prefer explicit types when inference is not obviously safe.
4. **MUT-001.** `let` bindings are already mutable. Generate plain `let`;
   never `let mut` in new code.
5. **ANN-001.** Put `@pure`, `@pre`, `@post`, `@cost` directly above the
   function they decorate, never inside the body.
6. **TOON-001.** `ToonDoc` and `ToonNode` are opaque handle types, not
   integers or records. Never mix them or do arithmetic on them.
7. **IMP-001.** Use verified modules only. Never invent imports such as
   `use "helpers";`.
8. **ORDER-001.** Define types and helper functions before `main` uses them.
9. **LEN-001.** `toon_len(node)` for TOON arrays; `length(arrayValue)` for
   dynamic arrays.
10. **TUP-001.** Tuples are narrow: destructure direct top-level helper
    returns (`let (a, b) = pair();`) only. Assume nothing else.
11. **OUT-001.** When asked to generate code, return raw Aether source only.
    No Markdown fences.
12. **BUILT-001.** The helpers and builtins listed in this document are the
    complete callable surface. If a function is not listed here, it does not
    exist. Do not invent helpers (`substring`, `to_upper`, `parse_int`, ...).
13. **ROOT-001.** `toon_root(doc)` returns the top-level value. If the JSON is
    object-shaped, extract the named array with `toon_key(...)` before
    iterating; never iterate an object root as if it were the array.
14. **SCOPE-001.** A name must be declared before use and still be in scope at
    the use site. Do not rely on guessed globals, expired loop locals, or
    helper-local names from another function.

Fast failure checks: output outside `fx` is wrong; a guessed import is wrong;
a helper not listed in this document is wrong; arithmetic on `ToonDoc` /
`ToonNode` is wrong; `return` is wrong; contracts inside a body are wrong;
when unsure about a type, add it explicitly.

## Never Generate These

**LLM-critical.** Common LLM failure modes. Never generate:

- `return`; use `ret` (SYN-001)
- `class`; use `type` (SYN-001)
- `for`, `while`, `var`, `func`, `def`, `=>`, Python-style colons (SYN-001)
- `print` / `println` / task helpers / `ai_chat` outside `fx { ... }` (FX-001)
- invented imports such as `use "helpers";` (IMP-001)
- invented helper functions not listed in this document (BUILT-001)
- names that were never declared, or locals reused outside their scope
  (SCOPE-001)
- arithmetic on `ToonDoc` or `ToonNode`, or assigning one where the other is
  expected (TOON-001)
- annotations inside a function body (ANN-001)
- a tuple-return call bound to one variable: `let value = pair();` (TUP-001)
- mixed-type output that guesses `+` will stringify numbers
- `write(...)` / `writeln(...)` spellings in Aether source
  [VERIFY: print/println lower to write/writeln; confirm the Pascal spellings
  are rejected (keep this bullet) or accepted (move to Accepted forms)]
- Markdown fences around the program

## Canonical vs accepted forms

Generate the canonical form unless preserving existing code.

| Topic | Canonical | Accepted | Avoid |
|---|---|---|---|
| Mutable binding | `let x: Int = 0;` | `let mut x: Int = 0;` | treating `let` as immutable |
| Return | `ret value;` (`ret;` for Void) | — | `return value;` |
| Output | variadic `println(a, b)` in `fx` | text-only `+` concatenation | `Text + Int` guessing |
| Text equality | `a == b` | `string_eq(a, b)` | inventing `.equals(...)` |
| Text length | `string_len(t)` | `t.len` | inventing `strlen(...)` |
| Dynamic array length | `length(xs)` | `len(xs)`, `xs.len` | `toon_len(xs)` |
| TOON array length | `toon_len(node)` | — | `length(node)` |
| Method call | `counter.bump()` | `bump(counter)` if receiver obvious | class syntax |
| Record init | `Point { x: 3, y: 4 }` | `Point(x: 3, y: 4)` | — |
| TOON nested lookup | bind and validate intermediates | nested calls when shape guaranteed | `_or` on missing intermediate |
| Imports | verified `use "module";` | self-contained code | guessed modules |

## What Aether is for

Small-to-medium automation programs that parse structured payloads, extract
typed fields, classify or transform data, and print or store results inside
`fx` — with visible effect boundaries and lightweight contracts, lowering onto
the shared PSCAL toolchain. It is not a separate runtime, not a dynamic
scripting language, and not a place to import imagined libraries.

## Smallest useful program

```aether
fn main() -> Void {
    fx {
        println("Hello from Aether");
    }
    ret;
}
```

## Safe generation algorithm for LLMs

**LLM-critical.** Generate in this order:

1. Decide whether the program needs TOON.
2. Define `type` blocks before functions that instantiate them.
3. Define helper functions before callers.
4. Give every function parameter an explicit type.
5. Prefer explicit `let name: Type = ...` unless the initializer is trivial.
6. Put all output, task helpers, and AI calls inside `fx`.
7. Use `ret`, never `return`.
8. Close every parsed `ToonDoc` with `toon_close(doc)`.
9. Run the Validation Checklist before final output.

## Lexical basics

- **Comments**: `// line comment`. This is the only comment form.
  [VERIFY: confirm `//` is the sole form and block comments `/* */` are
  rejected; if block comments exist, document them in one line]
- **Text literals**: double-quoted; escape embedded quotes as `\"`.
  [VERIFY: enumerate the full escape set, e.g. `\n`, `\t`, `\\`; LLMs will
  otherwise guess C escapes]
- **Statements** end with `;`. Blocks use `{ }`.

## Primitive types

| Type | Literals | Notes |
|---|---|---|
| `Int` | `42`, `-1` | integer arithmetic |
| `Real` | `3.5` | use a `Real` operand to force real division |
| `Text` | `"hi"` | string type |
| `Bool` | `true`, `false` | `println(flag)` prints [VERIFY: `true`/`false` or Pascal-style `TRUE`/`FALSE`? state the exact output] |
| `Void` | — | return type for procedures |
| `ToonDoc`, `ToonNode` | — | opaque handles (TOON-001) |

There are no conversion helpers between `Int`, `Real`, and `Text` other than
those listed in this document; use variadic `println(...)` for mixed output.
[VERIFY: if conversion builtins exist (e.g. Int→Text), list them here and in
the builtin tables instead of this sentence]

## Operators

| Category | Operators |
|---|---|
| Arithmetic | `+ - * /` on `Int`/`Real`; `Int / Int` is integer-style division |
| Modulo | [VERIFY: `%` or a `mod` keyword? give one example] |
| Comparison | `== != < <= > >=` |
| Logical | `!` for negation; [VERIFY: `&&` / `||`? `and` / `or`? Pick one, show one compound-condition example, and add the rejected spellings to Never Generate] |
| Text | `+` concatenation (text-compatible operands only); `==` equality |
| Array | `xs + [v]` append; [VERIFY: does `xs + ys` concatenate two arrays?] |

Unary minus on numeric literals and expressions is supported.
[VERIFY: confirm]

## Functions

```text
fn name(arg: Type, ...) -> ReturnType { ... }
```

Hard rules:

- every function declares an explicit return type; procedures use `-> Void`
- every parameter has an explicit type
- use `ret value;`, or bare `ret;` in `Void` functions; never `return`
- `ret` is legal inside an `fx` block [VERIFY: confirm; if not, state that
  `ret` must appear outside `fx` and adjust templates]

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

### Narrow tuple returns (TUP-001)

```aether
fn pair() -> (Int, Int) {
    ret (1, 2);
}

fn main() -> Void {
    let (a, b) = pair();
    fx { println(a, " ", b); }
    ret;
}
```

Limits: top-level helper functions only; destructuring must be a direct call;
no binding to a single name; no tuple-return methods; no `@post` on
tuple-return functions. [VERIFY: arity — pairs only, or `(a, b, c)` and mixed
element types too? State the exact limit.]

## Variables and constants

```aether
const Limit: Int = 42;
let count: Int = 0;
let label: Text = "Aether";
```

`let` bindings are mutable (MUT-001). `let mut` is accepted, redundant, and
ignored; never generate it.

### Inference policy (TYPE-001)

Omit the type only for these initializers:

- literals: `42`, `3.5`, `"text"`, `true`
- `new Type()`
- calls to functions with known declared return types, including methods on
  known typed bindings and the TOON/string helpers in this document

Everything else gets an explicit type — especially TOON extractions with
non-trivial shape, branchy results, and arithmetic such as `base + 1` where
the operand types are not visible at a glance. When in doubt, annotate.

## Records: `type`

```aether
type JobSummary {
    name: Text;
    score: Int;

    fn isReady() -> Bool {
        ret self.score >= 90;
    }
}
```

- use `type`, never `class`; fields are `name: Type;` (semicolons)
- inside methods use lowercase `self`, never `Self`
- top-level helpers whose first parameter is `self: Type` are extension-style
  methods; call them with method syntax (`counter.bump()`)

Record values are pointer-backed: passing a record to a function or method
and mutating its fields is visible to the caller.
[VERIFY: confirm reference semantics; if records are ever copied by value,
state exactly when]

### Construction

```aether
let counter = new Counter();        // zeroed defaults
let point: Point = Point { x: 3, y: 4 };  // canonical field init
```

`new Type()` defaults: integers `0`, reals `0.0`, booleans `false`, text empty,
pointers `nil`, arrays empty-initialized. `Type(x: 3, y: 4)` is accepted but
non-canonical. Generated code should not otherwise use pointers or `nil`;
they are backend details. [VERIFY: if user-facing pointer syntax exists and is
wanted, document it; otherwise keep this fence]

## Conditionals

Parentheses are optional. The canonical multi-branch pattern is sequential
`if` + `ret`:

```aether
if score >= 90 {
    ret "ready";
}
if score >= 70 {
    ret "review";
}
ret "blocked";
```

[VERIFY: statement-level `else` / `else if`. LLMs will emit `} else {`
instinctively. Either (a) document it as supported with one example here, or
(b) add `else` blocks to Never Generate with sequential `if` + `ret` as the
repair. Do not leave this unstated.]

Inline conditional expressions are allowed on the right-hand side of
declarations, assignments, and returns — but never directly inside
`println(...)` argument lists; bind first:

```aether
let label: Text = if ready { "ready" } else { "blocked" };
fx { println(label); }
```

## Loops

```aether
loop index < total {        // condition loop
    index = index + 1;
}

loop i in 0..count {        // half-open range; i is Int
    fx { println(i); }
}

loop {                      // infinite
    break;
}
```

`break` exits the nearest loop. [VERIFY: is `continue` supported? One line
either way; if unsupported, add it to Never Generate.]

## Effects: `fx` (FX-001)

**LLM-critical.** All visible effects go inside `fx { ... }`: `print`,
`println`, task helpers, `ai_chat`, and other effectful builtins. Compute pure
values outside `fx`, then perform effects inside it.

```aether
fn report(msg: Text) -> Void {
    fx {
        println("report: ", msg);
    }
    ret;
}
```

Wrong: `println("hi");` at function scope. Right: wrap it in `fx { ... }`.

## Printing and formatting

`print` / `println` are the Aether output builtins. For mixed types, use
variadic arguments — never guess that `+` stringifies:

```aether
fx {
    println("Drop ", j, " -> ID: ", tx.id, " | Amt: ", tx.amount);
    println("pi ~= ", 3.14159:0:2);   // width:precision => 3.14
}
```

`println(realValue)` uses the backend default (6 decimals). For stable decimal
output use `value:width:precision`; width `0` means "precision only".

| Use case | Syntax | Output |
|---|---|---|
| percentage | `value:0:2` | `95.50` |
| right-aligned | `value:10:2` | `     95.50` |
| default | `value` | `3.141593` |

Real division: `Int / Int` is integer-style; introduce a `Real` operand to
force a real result:

```aether
let successRate: Real = successful * 100.0 / total;
```

## TOON handle ownership

`ToonDoc` owns all `ToonNode` handles derived from it.

- `toon_parse(...)` and `toon_parse_file(...)` return a `ToonDoc`
- `toon_root(...)`, `toon_key(...)`, and `toon_at(...)` return `ToonNode`
  handles associated with that document
- `toon_free(node)` releases one node handle early
- `toon_close(doc)` releases the document and any remaining child handles
  derived from it

Practical rule:

- if a document is short-lived, `toon_close(doc)` at the end is usually
  sufficient
- if a loop creates many temporary node handles, prefer `toon_free(...)` for
  those transient nodes to reduce temporary handle-table growth before close

Example:

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

## Purity and contracts (ANN-001)

Annotations attach to the next function and sit directly above it — never
inside the body, never bare (`@pre` with no expression).

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    ret "blocked";
}

@pre score >= 0
@post result <= 100
fn clamp(score: Int) -> Int {
    if score > 100 {
        ret 100;
    }
    ret score;
}
```

- `@pure` functions reject effectful builtins and calls into known non-pure
  Aether functions
- `@pre` / `@post` take expressions; `@post` may reference `result`
- [VERIFY: may multiple `@pre` lines stack on one function? one line either way]
- `@cost <n><unit>` validates a budget annotation; units: `ns us ms s op ops
  step steps`

## Strings

`Text` helpers — this is the complete string surface (BUILT-001):

```aether
let nameLen: Int = string_len(name);   // canonical; name.len accepted
if status == "ready" { ... }           // canonical; string_eq(a,b) accepted
let s: Text = "ab" + "cd";             // concatenation of Text operands
```

[VERIFY: if substring/index/case/split helpers exist, list them here; if not,
keep the "complete surface" claim — it is the strongest anti-hallucination
rule available]

## Dynamic arrays

```aether
let xs: Int[] = [];
xs = xs + [7];          // append pattern
let n: Int = length(xs);
```

- `Type[]` declares; `[]` is the empty literal
- [VERIFY: element access — `xs[0]` read and `xs[0] = v;` write? Show one
  read and one write example; LLMs cannot do useful array work without this]
- [VERIFY: multi-element literals `[1, 2, 3]`? arrays of Text / records?]
- `length(xs)` canonical; `len(xs)` and `xs.len` accepted
- never `toon_len(xs)` on a dynamic array (LEN-001)

## Structured data: TOON

**LLM-critical.** TOON helpers ride on yyjson; JSON-compatible payloads are
the safe path. `ToonDoc` = parsed document, `ToonNode` = node within it; both
opaque (TOON-001). Keys are `Text`, indexes `Int`. Always `toon_close(doc)`.

### Helper surface (complete — BUILT-001)

| Need | Helper |
|---|---|
| availability | `has_toon()` |
| parse text / file | `toon_parse(text)`, `toon_parse_file(path)` |
| root / close | `toon_root(doc)`, `toon_close(doc)` |
| object field / array element | `toon_key(node, key)`, `toon_at(node, i)` |
| TOON array length | `toon_len(node)` |
| typed field get | `toon_get_text/int/real/bool(node, key)` |
| typed field get w/ fallback | `toon_get_text_or/int_or/real_or/bool_or(node, key, fb)` |
| node value | `toon_text/int/real/bool/null_value(node)` |
| kind / membership | `toon_type(node)`, `toon_has_key(node, key)`, `toon_has_at(node, i)` |
| shape checks | `toon_is_text/int/real/bool/null/arr/obj(node)` |

[VERIFY: enumerate the strings `toon_type(node)` returns, e.g. "obj", "arr"]

### Root shape rule (ROOT-001)

Decide the top-level shape first:

```aether
// JSON starts with '[': root IS the array
let root: ToonNode = toon_root(doc);
loop i in 0..toon_len(root) {
    let user: ToonNode = toon_at(root, i);
}

// JSON starts with '{': extract the named array, then iterate
let root: ToonNode = toon_root(doc);
let jobs: ToonNode = toon_key(root, "jobs");
loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
}
```

`toon_at(root, 0)` is the first *element*, never "the array". If the prompt
names a collection (`jobs`, `users`, `releases`) and the payload is
object-shaped, bind that named array first.

### Key fidelity (KEY-001)

Copy JSON keys exactly; never normalize (`"name"` under `"app"` is
`toon_get_text_or(app, "name", "")`, not `toon_get_text_or(root, "appName",
"")`). One-character keys like `"v"` are valid.

### Nested lookup safety (NEST-001)

`_or` helpers protect only the final keyed lookup on a valid object node —
not the path to it. Guard intermediates:

```aether
let code: Text = "EMPTY";
if toon_has_key(row, "meta") {
    let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

Avoid `toon_get_text_or(toon_key(row, "meta"), "code", "EMPTY")` unless
`"meta"` is guaranteed present. Nested calls are fine when the shape is known,
but intermediate bindings are easier to read and debug.

### Worked example

```aether
fn main() -> Void {
    if !has_toon() {
        fx { println("yyjson unavailable"); }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\"name\":\"Aether\",\"enabled\":true,\"count\":2}");
    let root: ToonNode = toon_root(doc);

    let name: Text = toon_get_text(root, "name");
    let enabled: Bool = toon_get_bool(root, "enabled");
    let count: Int = toon_get_int_or(root, "count", 0);

    fx {
        println("name = ", name);
        println("enabled = ", enabled);
        println("count = ", count);
    }

    toon_close(doc);
    ret;
}
```

## Tasks and AI helpers

Compact aliases over shared runtime helpers; all are effectful — call inside
`fx` (FX-001). Surface: `task_spawn`, `task_queue`, `task_wait`,
`task_lookup`, `task_status`, `task_result`, `task_stats`, `task_stats_json`,
`sleep`, `ai_chat`. Probes: `has_ai()`, `has_builtin(category, function)`.

Use `sleep(ms)` for a blocking millisecond pause. It is the compact Aether
spelling for the shared PSCAL `delay(ms)` builtin and is effectful, so it must
stay inside `fx`. `task_wait(handle)` waits for a task handle; it is not a
timer and should not be used like `task_wait(100)`.

Structured discovery also exists for sophisticated agents:
- `builtins_json()` returns a compact JSON list of Aether-visible builtin names
- `builtins_json(true)` returns richer JSON metadata, including aliases and
  usage hints when available
- `builtin_info(name)` returns JSON metadata for one builtin name

Use discovery when the prompt depends on optional runtime capabilities. Do not
replace the documented core subset with ad hoc discovery in ordinary examples.

[VERIFY: give one-line signatures (args, return types) for `ai_chat` and the
task helpers an LLM should generate, plus the valid `has_builtin` categories —
or state explicitly that LLMs should not generate these calls unless the
prompt supplies signatures. Listing names without signatures invites guessed
arguments.]

## Modules (IMP-001, MOD-001)

Hard rules:

- write `use "..."` only when the module is provided in the prompt, present in
  the repository, or otherwise verified; otherwise stay self-contained
- a missing `use` target may be silently ignored in compatibility mode, but
  its names still do not exist
- **MOD-001**: imported names must match exported names exactly; `use` does
  not rename. If a module exports `classifySupport`, call
  `classifySupport(...)` — never a guessed local like `classify(...)` unless
  you define the wrapper yourself
- file naming: `mod PascalCaseName` lives in a snake_case file consumed as
  `use "pascal_case_name";` (e.g. `mod ModuleConsts` → `use "module_consts";`)
  [VERIFY: confirm this mapping is exactly mod-name → snake_case file name,
  and state where the compiler searches for module files]

```aether
mod BenchSupport {
    export const DefaultScore: Int = 50;
    export fn clampSupport(score: Int) -> Int { ... }
}
```

```aether
use "bench_support";

fn summarize(job: ToonNode) -> Int {
    let raw: Int = toon_get_int_or(job, "score", DefaultScore);
    ret clampSupport(raw);
}
```

[VERIFY: can `type` blocks be exported, or only `const` and `fn`?]

Real module examples: `Examples/aether/base/module_demo`,
`Examples/aether/base/module_consts_demo`.

## Diagnostics

Failures report original Aether source lines (`file:line:` prefix in CLI
mode), not lowered Rea lines; trust the Aether line first.
`--diagnostics-json` / `--diagnostics-toon` emit structured failures.
`--verbose-compat` (alias `--verbose-errors`) surfaces normally-quiet
compatibility shims such as ignored missing imports and redundant `let mut`.

## Copyable templates

### Pure helper + effectful main

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

### TOON array classification

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    if score >= 70 {
        ret "review";
    }
    ret "blocked";
}

fn main() -> Void {
    if !has_toon() {
        fx { println("yyjson unavailable"); }
        ret;
    }

    let doc: ToonDoc = toon_parse("[{\"name\":\"A\",\"score\":95},{\"name\":\"B\",\"score\":72}]");
    let root: ToonNode = toon_root(doc);

    loop i in 0..toon_len(root) {
        let row: ToonNode = toon_at(root, i);
        let name: Text = toon_get_text(row, "name");
        let score: Int = toon_get_int_or(row, "score", 0);
        let status: Text = classify(score);

        fx {
            println(name, ": ", status);
        }
    }

    toon_close(doc);
    ret;
}
```

### Compact object with method

```aether
type Counter {
    value: Int;

    fn bump() -> Int {
        self.value = self.value + 1;
        ret self.value;
    }
}

fn main() -> Void {
    let counter: Counter = new Counter();
    counter.value = 41;
    let answer: Int = counter.bump();
    fx {
        println(answer);
    }
    ret;
}
```

Larger examples: `Examples/aether/showcase/agent_report`,
`Examples/aether/showcase/release_board`.

## Repair rules

Map compiler complaints to fixes. [VERIFY: where possible, replace "error
mentions X" with the literal diagnostic string the frontend emits — exact
strings make these repairs mechanical for small-context LLMs.]

- mentions `println` / `print` / task helpers / `ai_chat` → wrap in `fx` (FX-001)
- mentions `return` → replace with `ret` (SYN-001)
- mentions `class` → replace with `type` and Aether field syntax (SYN-001)
- mentions unknown import → remove `use` unless verified (IMP-001)
- mentions unknown function → it does not exist; inline the logic (BUILT-001)
- mentions `not in scope` → declare the name earlier, pass it as a parameter,
  or rename the local you actually meant (SCOPE-001)
- mentions `ToonDoc` / `ToonNode` → check handle types; do not mix (TOON-001)
- mentions a tuple → destructure a direct top-level call only (TUP-001)
- mentions annotation placement → move above the function (ANN-001)
- integer result where decimals expected → introduce a `Real` operand (`100.0`)
- unstable decimal output → use `value:0:precision`
- wrong receiver spelling → `self`, never `Self`
- iterating an object root → extract the array with `toon_key` first (ROOT-001)
- fallback didn't save a nested lookup → guard the intermediate node (NEST-001)

## Validation Checklist

Before submitting Aether code, verify:

- all output, task helper, and `ai_chat` calls are inside `fx { ... }` (FX-001)
- every called helper appears in this document (BUILT-001)
- all `use "..."` imports reference real, verified modules; imported names are
  used exactly as exported (IMP-001, MOD-001)
- all function parameters have explicit types; uncertain types are annotated
  (TYPE-001)
- `ret` not `return`; `type` not `class` (SYN-001)
- no `let mut` in new code (MUT-001)
- no arithmetic on or cross-assignment of `ToonDoc` / `ToonNode`; every parsed
  doc is closed with `toon_close(doc)` (TOON-001)
- object-shaped payloads: named array extracted before iteration (ROOT-001);
  JSON keys copied exactly (KEY-001); intermediate nodes guarded before `_or`
  lookups (NEST-001)
- `toon_len` for TOON arrays, `length` for dynamic arrays (LEN-001)
- real arithmetic has a `Real` operand where decimals matter; stable decimal
  output uses `value:width:precision`
- tuple returns destructured directly (TUP-001)
- annotations sit above their functions (ANN-001)
- canonical forms used unless preserving accepted-but-non-canonical source
