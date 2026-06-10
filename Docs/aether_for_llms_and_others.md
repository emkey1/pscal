# Aether for Humans and LLMs

Maintenance note:

- this is the full reference version
- keep it human-usable
- when this file changes, also refresh
  `Docs/aether_for_llms_with_small_contexts.md` in the same commit

This is the practical guide to reading and writing Aether code as it exists
today in this repository.

If you are a human, the goal is to make Aether easy to scan and easy to use.
If you are an LLM, the goal is to help you generate valid Aether on the first
try instead of guessing at unsupported language features.

Aether is a compact front end for the PSCAL suite. It targets the existing
shared PSCAL backend, bytecode compiler, and VM. It is not a separate runtime.

## Short version

Write Aether like this:

- use `fn`, `let`, `const`, `ret`, `if`, `loop`, `type`, `mod`, `use`
- put side effects inside `fx { ... }`
- use `@pure` for pure helpers
- use `@pre` and `@post` for runtime-checked contracts
- prefer explicit types when inference is not obviously safe
- prefer plain `let`; `let mut` is accepted but redundant and ignored
- define types and helper functions before `main` uses them
- use `length(arrayValue)` for dynamic array length, not `len(...)`
- use `ToonDoc` and `ToonNode` for structured TOON/JSON-style data
- use the examples under `Examples/aether/` as the ground truth for style

Golden rule:

- every `print(...)`, `println(...)`, task helper call, and AI helper call
  must be inside `fx { ... }`
- this is a compile-time rule, not just a style preference

Do not write Aether like this:

- do not invent new keywords
- do not invent module imports
- do not assume Python syntax, JavaScript syntax, or Rust syntax will work
- do not call effectful builtins outside `fx`
- do not treat `ToonDoc` and `ToonNode` as plain integers
- do not assume general tuple support exists beyond direct tuple-return helpers
- do not rely on broad magical inference; Aether inference is intentionally
  narrow

## What Aether is good for

Aether is aimed at compact, structured, agent-friendly programs.

The best fit today is:

- small to medium automation programs
- programs that consume structured data
- programs that need visible effect boundaries
- programs that benefit from lightweight contracts
- programs where humans and agents both need to understand the source quickly

Typical Aether jobs look like this:

- parse a payload from a string or file
- extract typed fields
- classify or transform the data
- print or store a result inside `fx`
- use a few compact helper types and functions

Nested TOON helper expressions are supported. For example:

```aether
let name: Text = toon_get_text(toon_at(jobs, i), "name");
```

That said, intermediate `ToonNode` bindings are still often easier for humans
and LLMs to read and debug:

```aether
let job: ToonNode = toon_at(jobs, i);
let name: Text = toon_get_text(job, "name");
```

Narrow tuple support now exists, but it is intentionally small:

- tuple returns are for top-level helper functions only
- destructuring must be a direct call, for example `let (a, b) = pair();`
- binding a tuple-return call to one name is not supported
- tuple-return methods are not supported
- `@post` on tuple-return functions is not supported yet
- this tuple surface is intentionally narrow and should not be expanded without
  a strong justification that preserves Aether's compactness and clarity

## The mental model

Think of Aether as:

- a compact source language
- with explicit effect boundaries
- with lightweight contracts
- with a TOON/JSON-friendly data surface
- lowering onto the existing PSCAL toolchain

Do not think of it as:

- a separate VM
- a general-purpose dynamic scripting language
- a language where inference always figures everything out

## The smallest useful program

```aether
fn main() -> Void {
    fx {
        println("Hello from Aether");
    }
    ret;
}
```

That already shows three important rules:

- `main` is just a normal function
- output belongs in `fx { ... }`
- Aether uses `ret`, not `return`

## Core syntax

### Functions

```aether
fn add(a: Int, b: Int) -> Int {
    ret a + b;
}
```

Function form:

```text
fn name(arg: Type, ...) -> ReturnType { ... }
```

### Variables and constants

Explicit types:

```aether
const Limit: Int = 42;
let count: Int = 0;
let label: Text = "Aether";
```

Bindings declared with `let` are already mutable. `let mut` is accepted for
compatibility with LLM output, but it is redundant and ignored. Prefer plain
`let`.

Good:

```aether
let ready: Int = 0;
ready = ready + 1;
```

Accepted but redundant:

```aether
let mut ready: Int = 0;
```

Obvious inference:

```aether
const Name = "Aether";
let count = 42;
let enabled = true;
let ratio = 3.5;
```

Inference also works in some common Aether-specific cases:

- function calls with known return signatures
- imported constants with obvious exported types
- `new Type()`
- method calls on known typed bindings
- `string_len(textValue)`
- known TOON helper return values
- simple numeric expressions over known `Int` / `Real` values, such as
  `count + 1` or `base_amount * multiplier`
- simple method / function results with non-ambiguous declared return types,
  including pointer-backed record values returned from helpers

If inference is not obviously safe, add the type explicitly.

Preferred declaration order:

- define helper functions before their callers
- define `type` blocks before you instantiate them or call their methods
- define module exports before writing the code that imports and uses them

For LLMs, these are the safe inference patterns to rely on:

- `let x = 42;`
- `let x = 3.5;`
- `let x = "text";`
- `let x = true;`
- `let x = new Type();`
- `let x = knownFunction(...);`
- `let x = knownTypedValue.method(...);`

Always add an explicit type for:

- TOON handles and extracted TOON values when the shape is not trivial
- branchy results when the final type is not obvious at a glance
- public examples where clarity matters more than shaving a few tokens

### Conditionals

```aether
if score >= 90 {
    ret "ready";
}
if score >= 70 {
    ret "review";
}
ret "blocked";
```

Parentheses are not required.

A compact inline conditional expression is also supported on the right-hand
side of declarations, assignments, and returns:

```aether
let score: Int = if count > 0 { total / count } else { 0 };
```

For text equality, prefer plain `==`, but `string_eq(a, b)` is also accepted
as a compact alias and lowers to the same comparison.

### Loops

Condition loop:

```aether
loop index < total {
    index = index + 1;
}
```

Half-open range loop:

```aether
loop i in 0..count {
    fx {
        println(i);
    }
}
```

Infinite loop:

```aether
loop {
    break;
}
```

### Return

Use `ret`:

```aether
ret value;
```

For `Void` functions, this is also fine:

```aether
ret;
```

Tuple-return helpers are also supported in a narrow form:

```aether
fn pair() -> (Int, Int) {
    ret (1, 2);
}

fn main() -> Void {
    let (a, b) = pair();
    fx {
        println(a);
        println(b);
    }
    ret;
}
```

Use that exact pattern. Do not do this:

```aether
let value = pair();
```

## Effects: `fx`

`fx { ... }` marks effectful work.

Use it for:

- printing
- selected runtime/task helpers
- AI helper calls
- other known effectful builtins

Example:

```aether
fn report(msg: Text) -> Void {
    fx {
        println("report: ", msg);
    }
    ret;
}
```

This is important because Aether checks some effectful calls before the shared
backend sees them.

A good default rule for LLMs:

- if the code prints, launches tasks, or does other visible runtime work,
  put that part inside `fx`

Runtime and compile-time failures should report original Aether source lines,
not lowered Rea lines. When debugging, trust the reported Aether line first.

If you invoke the compiler with `--diagnostics-json` or `--diagnostics-toon`,
runtime failures are also emitted in structured form with the original Aether
file and line when available.

Frontend failures now follow the same pattern for important Aether-owned cases
too. In particular, unresolved `use "..."` imports should report as Aether
import errors with the original source file and line.

In ordinary CLI mode, runtime failures should also include a plain-text
`file:line:` prefix before the message when the source label is available.

## Printing and numeric formatting

`print(...)` and `println(...)` are Aether spellings for the shared
`write(...)` and `writeln(...)` builtins.

For mixed-type output, prefer variadic `print(...)` / `println(...)` arguments
instead of building one large string with `+`.

Use this pattern:

```aether
fx {
    println("score = ", 42);
    println("pi ~= ", 3.14:0:2);
}
```

### Real formatting in `println()`

Plain `println(realValue)` currently uses the backend default real formatting,
which is 6 digits after the decimal point.

When you need stable decimal output, use the Pascal-style formatting form:

```aether
println(realValue:width:precision);
```

Where:

- `width` is the minimum field width
- `precision` is the number of digits after the decimal point
- use width `0` when you only care about decimal precision

Example:

```aether
fn main() -> Void {
    let pi: Real = 3.14159265;
    fx {
        println(pi);      // Default format: 3.141593
        println(pi:0:2);  // Width 0, 2 decimals: 3.14
        println(pi:8:3);  // Width 8, 3 decimals:    3.142
    }
    ret;
}
```

Common cases:

| Use case | Syntax | Output |
|---|---|---|
| Simple percentage | `value:0:2` | `95.50` |
| Right-aligned amount | `value:10:2` | `     95.50` |
| Wider scientific-style display | `value:12:4` | `      3.1416` |
| Default backend format | `value` | `3.141593` |

For percentages, force real arithmetic before formatting:

```aether
let successRate: Real = successful * 100.0 / total;
fx {
    println("success = ", successRate:0:2, "%");
}
```

Division rule:

- `a / b` with `Int` operands follows integer-style arithmetic
- to force a real-valued result, introduce a `Real` operand, for example
  `successful * 100.0 / total`

Do not assume `Text + Int` or `Text + Real` is a universally safe pattern.
Some concatenation cases may appear to work in narrow contexts, but for humans
and especially for LLMs, the reliable rule is:

- if values are mixed types, prefer `println(a, b, c)` or `print(a, b, c)`
- use `+` for text-building only when the operands are already clearly
  text-compatible or explicitly converted

Bad:

```aether
println("Drop " + j + " -> ID: " + tx.id + " | Amt: " + tx.amount);
```

Good:

```aether
println("Drop ", j, " -> ID: ", tx.id, " | Amt: ", tx.amount);
```

If you want a rule that works well for LLM-generated code, use this one:

- for visible output, default to comma-separated `println(...)` arguments
- do not guess that `+` will stringify numbers for you
- for controlled decimal output, use `value:width:precision`

## Purity and contracts

### `@pure`

Use `@pure` on helpers that should stay effect-free.

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    ret "blocked";
}
```

Current Aether rules reject direct effectful builtins inside pure functions and
also reject direct calls into known non-pure Aether functions.

## Strings

Use `Text` for string values.

The compact length helper is:

```aether
let len = string_len(name);
```

`string_len(...)` lowers to the shared backend string-length builtin and
returns `Int`, so the inferred form above is valid.

### `@pre` and `@post`

Use contracts when you want runtime-checked function assumptions.

```aether
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    if score > 100 {
        ret 100;
    }
    ret score;
}
```

Rules:

- annotations attach to the next function
- do not place `@pre` or `@post` inside the function body
- `@pre` and `@post` must contain expressions
- `@post` may refer to `result`

Correct:

```aether
@pre score >= 0
@post result >= 0
fn normalize(score: Int) -> Int {
    ret score;
}
```

Also correct:

```aether
@pre score >= 0
@post result <= 100
fn clamp(score: Int) -> Int {
    if score > 100 {
        ret 100;
    }
    ret score;
}
```

Wrong:

```aether
fn normalize(score: Int) -> Int {
    @pre score >= 0
    @post result >= 0
    ret score;
}
```

Also wrong:

```aether
@pre
@post
fn normalize(score: Int) -> Int {
    ret score;
}
```

### `@cost`

Use `@cost` when you want a validated budget annotation.

```aether
@cost 5ms
fn fastPath() -> Int {
    ret 42;
}
```

Supported units include:

- `ns`
- `us`
- `ms`
- `s`
- `op` / `ops`
- `step` / `steps`

## Modules (advanced)

Use quoted `use` imports and compact `mod`/`export` syntax.

Important import rule:

- only write `use "..."` when the module is explicitly provided in the prompt,
  shown in the repository, or otherwise verified to exist
- if no real module is available, keep the program self-contained instead of
  inventing a helper module
- generic names like `helpers` are especially risky unless the file is known to
  exist

For LLMs, this is a hard rule:

- never invent a `use` target from pattern matching alone
- if you cannot verify the module, do not import it
- imported symbol names must match the exported names exactly
- `use "module_name";` does not rename exported symbols for you

When writing standalone snippets for docs, tests, chats, or LLM-generated
examples:

- prefer no `use` imports unless the module is part of the supplied context
- inline small constants and helpers instead of assuming support files exist
- if a `use` target does not exist, the compiler should fail with an Aether
  import error on the original source line

## Types and methods

Aether supports compact `type` blocks.

```aether
type Counter {
    value: Int;

    fn bump() -> Int {
        self.value = self.value + 1;
        ret self.value;
    }
}
```

Important details:

- use `type`, not `class`
- fields use `name: Type;`
- inside methods, use `self`
- `self` lowers onto the shared backend receiver model
- top-level helpers that start with `self: Type` are treated as extension-style
  methods and should be called with method syntax

Example:

```aether
fn bump(self: Counter) -> Int {
    self.value = self.value + 1;
    ret self.value;
}

fn main() -> Void {
    let counter = new Counter();
    let next: Int = counter.bump();
    ret;
}
```

Do not call extension-style helpers like plain free functions:

```aether
counter.bump();     // good
bump(counter);      // also accepted when the receiver type is obvious
```

If Aether can prove the first argument is the receiver, it rewrites the free
call to method form. When in doubt, method form is still the clearest source
style for both humans and LLMs.

### Object creation

Plain construction:

```aether
let counter = new Counter();
```

For Aether code, `new Type()` applies type-appropriate default field
initialization automatically:

- integer-like fields start at `0`
- real fields start at `0.0`
- booleans start at `false`
- strings/text start empty
- pointers start as `nil`
- arrays use their normal array-field initialization path

So this is valid and predictable:

```aether
let counter = new Counter();
let next: Int = counter.bump();
```

You can still assign fields explicitly when that makes the example clearer.

Compact field initialization:

```aether
let point: Point = Point {
    x: 20,
    y: 22
};
```

Method-call inference now works in common cases:

```aether
let ready = summary.isReady();
```

### Dynamic arrays

Use `Type[]` for dynamic arrays:

```aether
let xs: Int[] = [];
xs = xs + [7];
xs = xs + [9];

fx {
    println(length(xs));
}
```

Important details:

- `length(xs)` is the compact array-length helper
- `len(xs)` is accepted as a compact alias for `length(xs)`
- `xs = xs + [value]` is the supported compact append pattern
- `let xs: Int[] = [];` is supported

## Structured data: TOON

The easiest way to think about TOON in Aether today is:

- Aether uses TOON-shaped helpers for structured data
- the current implementation is built on the shared yyjson path
- JSON-compatible payloads are the safest bet

### Handle types

Use:

- `ToonDoc` for parsed documents
- `ToonNode` for nodes inside the document

Example:

```aether
let doc: ToonDoc = toon_parse("{\"name\":\"Aether\",\"count\":2}");
let root: ToonNode = toon_root(doc);
let count: Int = toon_get_int(root, "count");
toon_close(doc);
```

### Common helpers

Parsing:

- `has_toon()`
- `toon_parse(text)`
- `toon_parse_file(path)`
- `toon_root(doc)`
- `toon_close(doc)`

Navigation:

- `toon_key(node, key)`
- `toon_at(node, index)`
- `toon_len(node)`

Typed extraction:

- `toon_get_text(node, key)`
- `toon_get_int(node, key)`
- `toon_get_real(node, key)`
- `toon_get_bool(node, key)`
- `toon_text_value(node)`
- `toon_int_value(node)`
- `toon_real_value(node)`
- `toon_bool_value(node)`
- `toon_null_value(node)`

Fallback extraction:

- `toon_get_text_or(node, key, fallback)`
- `toon_get_int_or(node, key, fallback)`
- `toon_get_real_or(node, key, fallback)`
- `toon_get_bool_or(node, key, fallback)`

Important fallback rule:

- `_or` helpers only protect the final keyed lookup on a valid object node
- they do not make an entire nested traversal path safe
- if an intermediate object might be missing, guard that intermediate step first

Shape and type checks:

- `toon_type(node)`
- `toon_is_text(node)`
- `toon_is_int(node)`
- `toon_is_real(node)`
- `toon_is_bool(node)`
- `toon_is_null(node)`
- `toon_is_arr(node)`
- `toon_is_obj(node)`
- `toon_has_key(node, key)`
- `toon_has_at(node, index)`

### TOON example

```aether
fn main() -> Void {
    if !has_toon() {
        fx {
            println("yyjson unavailable");
        }
        ret;
    }

    let doc = toon_parse("{\"name\":\"Aether\",\"enabled\":true,\"count\":2}");
    let root = toon_root(doc);

    let name = toon_get_text(root, "name");
    let enabled = toon_get_bool(root, "enabled");
    let count = toon_get_int_or(root, "count", 0);

    fx {
        println("name = ", name);
        println("enabled = ", enabled);
        println("count = ", count);
    }

    toon_close(doc);
    ret;
}
```

### TOON rules for LLMs

Follow these rules exactly:

- treat `ToonDoc` and `ToonNode` as opaque handle types
- do not do arithmetic on them
- do not assign a `ToonDoc` where a `ToonNode` is expected
- do not assign a `ToonNode` where a `ToonDoc` is expected
- do not treat them as plain scalar values or pattern-match them as if they
  were ordinary records
- use `Text` keys and `Int` indexes
- close parsed documents with `toon_close(doc)`
- prefer explicit types around TOON code when the shape is not obvious

Common TOON iteration pattern:

```aether
let doc: ToonDoc = toon_parse(users_json);
let root: ToonNode = toon_root(doc);

fx {
    println("--- Users ---");
}

loop i in 0..toon_len(root) {
    let user: ToonNode = toon_at(root, i);
    let name: Text = toon_get_text(user, "name");

    fx {
        println("user ", i, ": ", name);
    }
}

toon_close(doc);
```

Important:

- `toon_root(doc)` returns the top-level parsed value
- if the input JSON starts with `[` and is therefore a top-level array,
  `toon_root(doc)` is the array to iterate
- if the input JSON starts with `{` and is therefore a top-level object,
  `toon_root(doc)` is the object to query by key
- if the parsed JSON text is an array, `toon_root(doc)` is already the array
- do not write `let user_array = toon_at(root, 0)` unless you specifically want the first element
- `toon_at(root, 0)` means "give me the first element stored inside `root`"
- it does not mean "treat `root` as an array variable"
- one-character JSON keys such as `"v"` are valid TOON keys in Aether;
  `toon_has_key(node, "v")` and `toon_get_real_or(node, "v", 0.0)` should work
- `println(...)` still requires `fx`, even for banner lines or one-off status output
- compute pure values outside `fx` when practical, then print inside `fx`

Two very common top-level shapes:

```aether
/* top-level array */
let doc: ToonDoc = toon_parse("[{\"name\":\"Alice\"},{\"name\":\"Bob\"}]");
let root: ToonNode = toon_root(doc);

loop i in 0..toon_len(root) {
    let user: ToonNode = toon_at(root, i);
}
```

When the parsed JSON root is an object containing an array field, first extract
that array node, then iterate it:

```aether
let root: ToonNode = toon_root(doc);
let jobs: ToonNode = toon_key(root, "jobs");

loop i in 0..toon_len(jobs) {
    let job: ToonNode = toon_at(jobs, i);
}
```

```aether
/* top-level object */
let doc: ToonDoc = toon_parse("{\"users\":[{\"name\":\"Alice\"},{\"name\":\"Bob\"}]}");
let root: ToonNode = toon_root(doc);
let users: ToonNode = toon_key(root, "users");

loop i in 0..toon_len(users) {
    let user: ToonNode = toon_at(users, i);
}
```

Safe nested lookup pattern:

```aether
let row: ToonNode = toon_at(root, i);
let code: Text = "EMPTY";

if toon_has_key(row, "meta") {
    let meta: ToonNode = toon_key(row, "meta");
    code = toon_get_text_or(meta, "code", "EMPTY");
}
```

Do not assume this is safe:

```aether
let code: Text = toon_get_text_or(toon_key(toon_at(root, i), "meta"), "code", "EMPTY");
```

Reason:

- the fallback only applies to the final `"code"` lookup
- it does not recover from a missing or invalid intermediate `"meta"` node

General nested-lookup rule:

- nested TOON calls are allowed
- but `_or` helpers only protect the final lookup
- if an intermediate node may be missing or may not have the right shape,
  bind and validate that step first

## Tasks and AI helpers

Aether has compact aliases over shared runtime/task helpers.

Task-oriented names include:

- `task_spawn(...)`
- `task_queue(...)`
- `task_wait(...)`
- `task_lookup(...)`
- `task_status(...)`
- `task_result(...)`
- `task_stats(...)`
- `task_stats_json(...)`

Capability probes:

- `has_ai()`
- `has_builtin(category, function)`

AI helper:

- `ai_chat(...)`

Treat task launches and AI calls as effectful operations. Put them in `fx`.

## A good Aether style

### Good pattern

```aether
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    ret "review";
}

fn main() -> Void {
    let score = 72;
    let status = classify(score);

    fx {
        println("status = ", status);
    }
    ret;
}
```

Why this is good:

- short, regular declarations
- pure logic separated from output
- no hidden effects
- easy for a human or an LLM to patch

### Less good pattern

```aether
fn main() -> Void {
    println("hi");
    ret;
}
```

Why this is bad:

- `println` should be inside `fx`

## A bigger example

This is a shortened single-file example:

```aether
const DefaultScore: Int = 50;

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

@pure
fn priority(score: Int) -> Int {
    if score >= 90 {
        ret 3;
    }
    if score >= 70 {
        ret 2;
    }
    ret 1;
}

type JobSummary {
    name: Text;
    score: Int;
    status: Text;
    priority: Int;

    fn isReady() -> Bool {
        ret self.priority == 3;
    }
}

@pre score >= 0
@post result >= 0
fn normalizeScore(score: Int) -> Int {
    if score > 100 {
        ret 100;
    }
    ret score;
}

fn makeSummary(job: ToonNode) -> JobSummary {
    let name = toon_get_text(job, "name");
    let rawScore = toon_get_int_or(job, "score", DefaultScore);
    let score = normalizeScore(rawScore);
    let summary = new JobSummary();
    summary.name = name;
    summary.score = score;
    summary.status = classify(score);
    summary.priority = priority(score);
    ret summary;
}
```

This example combines:

- contracts
- type blocks
- `self`
- inference
- TOON traversal
- compact helper use

For larger complete examples, see:

- `Examples/aether/showcase/agent_report`
- `Examples/aether/showcase/release_board`

## Copyable templates

### Template: pure helper + effectful main

```aether
@pure
fn transform(value: Int) -> Int {
    ret value + 1;
}

fn main() -> Void {
    let answer = transform(41);
    fx {
        println("answer = ", answer);
    }
    ret;
}
```

### Template: file-driven structured data program

```aether
fn main() -> Void {
    if !has_toon() {
        fx {
            println("yyjson unavailable");
        }
        ret;
    }

    let path: Text = "payload.json";
    let doc = toon_parse_file(path);
    let root = toon_root(doc);
    let name = toon_get_text(root, "name");

    fx {
        println("name = ", name);
    }

    toon_close(doc);
    ret;
}
```

### Template: compact object with method

```aether
type Counter {
    value: Int;

    fn bump() -> Int {
        self.value = self.value + 1;
        ret self.value;
    }
}

fn main() -> Void {
    let counter = new Counter();
    counter.value = 41;
    let answer = counter.bump();
    fx {
        println(answer);
    }
    ret;
}
```

## Appendix: multi-file Aether

Most Aether generated by LLMs should be single-file. Only reach for `use`
when the prompt explicitly provides one or more real module files.

Provided module:

```aether
mod ModuleConsts {
    export const Greeting: Text = "Aether";
    export const Answer: Int = 42;
}
```

Consumer:

```aether
use "module_consts";

fn main() -> Void {
    let greeting = Greeting;
    let answer = Answer;
    fx {
        println(greeting);
        println(answer);
    }
    ret;
}
```

Real module examples:

- `Examples/aether/base/module_demo`
- `Examples/aether/base/module_consts_demo`

## Rules for LLMs

If you are generating Aether, follow these rules in order:

1. Start from an existing example whenever possible.
2. Prefer explicit types unless the initializer is obviously safe to infer.
3. Put all printing and visible runtime work inside `fx`.
4. Mark pure helpers with `@pure`.
5. Use `@pre` and `@post` only directly above the function they decorate.
6. Use `ToonDoc` and `ToonNode` for parsed structured data.
7. Close TOON documents after use.
8. Prefer small helper functions over one giant `main`.
9. Use `type` plus methods for simple stateful objects.
10. Never write `use "..."` unless that module is provided or verified.
11. Generic imports like `use "helpers";` are invalid unless the actual module exists.
12. Treat `ToonDoc` and `ToonNode` as opaque handles, not numbers.
13. If decimal arithmetic matters, force a `Real` into the expression, for
    example `100.0`.
14. If decimal output matters, use `value:width:precision`.
15. When in doubt, copy the shape from `Examples/aether/base` or the showcase.

## Common mistakes

### Mistake: forgetting `fx`

Bad:

```aether
fn main() -> Void {
    println("hello");
    ret;
}
```

Good:

```aether
fn main() -> Void {
    fx {
        println("hello");
    }
    ret;
}
```

### Mistake: assuming broad inference

Bad:

```aether
let answer = base + 1;
```

Safer:

```aether
let answer: Int = base + 1;
```

### Mistake: confusing TOON handle types

Bad:

```aether
let root: ToonDoc = toon_root(doc);
```

Good:

```aether
let root: ToonNode = toon_root(doc);
```

### Mistake: confusing a TOON array with its first element

Bad:

```aether
let root: ToonNode = toon_root(doc);
let user_array: ToonNode = toon_at(root, 0);

loop i in 0..toon_len(user_array) {
    let user: ToonNode = toon_at(user_array, i);
}
```

Good:

```aether
let root: ToonNode = toon_root(doc);

loop i in 0..toon_len(root) {
    let user: ToonNode = toon_at(root, i);
}
```

If `root` came from parsing a JSON array, `root` is the collection to iterate.

This is the key distinction:

- `root` is the array
- `toon_at(root, 0)` is the first element inside that array
- if the first element is an object, calling `toon_len(toon_at(root, 0))`
  does not mean "array length"; it asks for the size/shape of that first
  element instead

### Mistake: treating an object root like the array itself

Bad:

```aether
let root: ToonNode = toon_root(doc);

loop i in 0..toon_len(root) {
    let release: ToonNode = toon_at(root, i);
}
```

Good:

```aether
let root: ToonNode = toon_root(doc);
let releases: ToonNode = toon_key(root, "releases");

loop i in 0..toon_len(releases) {
    let release: ToonNode = toon_at(releases, i);
}
```

If the payload looks like `{"jobs":[...]}` or `{"releases":[...]}`, the root is
an object. Extract the array field with `toon_key(...)` before iterating.

### Mistake: using the wrong receiver spelling

Bad:

```aether
Self.value = 1;
```

Good:

```aether
self.value = 1;
```

### Mistake: printing outside `fx`

Bad:

```aether
println("--- Processing User List ---");
```

Good:

```aether
fx {
    println("--- Processing User List ---");
}
```

## Validation checklist

Before submitting Aether code, verify:

- all `print(...)` and `println(...)` calls are inside `fx { ... }`
- all task helper calls and `ai_chat(...)` calls are inside `fx { ... }`
- all `use "..."` imports reference real, verified modules
- all function parameters have explicit types
- no arithmetic is performed on `ToonDoc` or `ToonNode`
- `ToonDoc` values are closed with `toon_close(doc)`
- TOON keys are `Text` and TOON indexes are `Int`
- decimal arithmetic uses a `Real` operand when needed, for example `100.0`
- decimal output uses `value:width:precision` when stable formatting matters
- tuple-return calls are destructured directly, not bound to a single name
- `@pre`, `@post`, `@pure`, and `@cost` are above the function, not inside it

## Where to look next

Practical examples:

- `Examples/aether/base/README.md`
- `Examples/aether/showcase/README.md`

Implementation notes:

- `src/aether/README.md`
- `src/aether/DESIGN.md`

Best example files to copy from:

- `Examples/aether/base/hello`
- `Examples/aether/base/contracts`
- `Examples/aether/base/contract_layouts`
- `Examples/aether/base/inferred_decls`
- `Examples/aether/base/function_inference`
- `Examples/aether/base/object_inference`
- `Examples/aether/base/self_mutation`
- `Examples/aether/base/toon_access`
- `Examples/aether/base/toon_defaults`
- `Examples/aether/showcase/agent_report`

## Bottom line

If you want valid Aether today, keep the code:

- compact
- explicit about effects
- modest about inference
- typed around TOON handles and extracted values
- close to the existing examples

That is the fastest path for both humans and LLMs to produce working Aether
code.
