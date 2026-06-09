# Aether for Humans and LLMs

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
- use `ToonDoc` and `ToonNode` for structured TOON/JSON-style data
- use the examples under `Examples/aether/` as the ground truth for style

Do not write Aether like this:

- do not invent new keywords
- do not assume Python syntax, JavaScript syntax, or Rust syntax will work
- do not call effectful builtins outside `fx`
- do not treat `ToonDoc` and `ToonNode` as plain integers
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
- known TOON helper return values

If inference is not obviously safe, add the type explicitly.

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
- `@pre` and `@post` must contain expressions
- `@post` may refer to `result`

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

## Modules

Use quoted `use` imports and compact `mod`/`export` syntax.

Imported module:

```aether
mod Helpers {
    export const DefaultScore = 72;

    @pure
    export fn classify(score: Int) -> Text {
        if score >= 90 {
            ret "ready";
        }
        ret "review";
    }
}
```

Consumer:

```aether
use "helpers";

fn main() -> Void {
    let score = DefaultScore;
    fx {
        println(classify(score));
    }
    ret;
}
```

See:

- `Examples/aether/base/module_demo`
- `Examples/aether/base/module_consts_demo`
- `Examples/aether/showcase/agent_report`

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

### Object creation

Plain construction:

```aether
let counter = new Counter();
```

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
- use `Text` keys and `Int` indexes
- close parsed documents with `toon_close(doc)`
- prefer explicit types around TOON code when the shape is not obvious

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
use "agent_support";

@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    ret "review";
}

fn main() -> Void {
    let score = DefaultScore;
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

This is a shortened version of the multi-file showcase:

```aether
use "agent_support";

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

- imports
- contracts
- type blocks
- `self`
- inference
- TOON traversal
- compact helper use

For the complete version, see:

- `Examples/aether/showcase/agent_report`
- `Examples/aether/showcase/agent_support`

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
10. When in doubt, copy the shape from `Examples/aether/base` or the showcase.

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

### Mistake: using the wrong receiver spelling

Bad:

```aether
Self.value = 1;
```

Good:

```aether
self.value = 1;
```

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
