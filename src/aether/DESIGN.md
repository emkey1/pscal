# Aether Design

## 1. Overview

Aether is a planned standalone front end for the PSCAL suite.

Like Pascal, Rea, CLike, and the shell front end, Aether will:

- parse its own source language,
- perform its own front-end semantic analysis,
- lower into the shared PSCAL AST and bytecode pipeline,
- execute on the existing PSCAL VM.

Aether does **not** define a separate VM, a separate bytecode engine, or a
separate runtime model. That is a hard architectural rule. If Aether needs new
capabilities, they should be added to the shared compiler/runtime path in a
minimal, clean way so the whole suite benefits and the system remains coherent.

The purpose of Aether is to provide a language that is:

- compact enough for agent generation,
- high-correctness per token,
- explicit enough for automated reasoning,
- readable enough for skilled human review,
- compatible with the strengths of PSCAL's existing compiler and VM.

In short: Aether is meant to be a practical language for human and agent
co-development on top of PSCAL, not a research toy and not a replacement
runtime.

## 2. Problem It Solves

The PSCAL suite already covers several useful source styles:

- Pascal is structured and expressive, but relatively verbose for agent output.
- CLike is familiar and direct, but it encourages syntax that tends to be
  noisier and less intention-revealing for planning-oriented agent workflows.
- Rea explores more modern language features, but it is optimized around its
  own object-oriented direction rather than agent-oriented source economy.

For the intended Aether use case, the missing piece is a language with the
following properties:

- low token count in source,
- high validity rate for generated source,
- regular syntax with few special cases,
- strong correspondence between source constructs and backend behavior,
- explicit expression of side effects and contracts,
- source that a human can still audit quickly without mentally decoding a
  bytecode-like notation.

That combination matters in agentic workflows because generated source is not
just written once. It is repeatedly:

- drafted,
- patched,
- summarized,
- re-executed,
- verified,
- inspected by humans.

If the language is too verbose, agent cost rises.
If it is too cryptic, human review quality drops.
If it is too magical, compiler/runtime reasoning becomes fragile.

Aether is intended to sit in that middle ground.

The practical optimization target is not "shortest possible source." It is:

> high correctness per token across the full draft-debug-review loop.

That means a construct that is slightly longer but much easier to generate,
repair, diagnose, and review is often the better Aether design.

## 3. Design Goals

### 3.1 Primary Goals

1. **Compact source**
   Aether should reduce token count compared with Pascal-like or Python-like
   spellings whenever that reduction does not materially harm readability.
   That includes preferring source-level names like `print(...)` and
   `println(...)` over backend-oriented spellings such as `write` and
   `writeln` when the lowering is direct.

2. **High correctness per token**
   Aether should optimize for valid generation, easy repair, stable diffs, and
   predictable diagnostics rather than for shortest source in isolation.

3. **Human auditability**
   A skilled human should be able to read an Aether file and understand intent
   without first translating it into another language mentally.

4. **Predictable lowering**
   Aether constructs should map cleanly onto the shared PSCAL AST and bytecode
   compiler. The language should avoid depending on broad backend rewrites.

5. **Explicit side-effect boundaries**
   The language should make effectful operations visible in source instead of
   burying them in ordinary expressions.

6. **Standalone front end**
   Aether must eventually own its own lexer, parser, semantic analysis, test
   suite, and front-end state. It may share backend code, but not front-end
   identity.

### 3.2 Secondary Goals

- good defaults for agent-generated modules and scripts,
- easy embedding in the existing build and test infrastructure,
- compatibility with PSCAL's concurrency model,
- future support for signed bytecode artifacts.

### 3.3 Non-Goals

- a new VM,
- a new bytecode format for ordinary execution,
- obfuscation,
- excessive compiler magic,
- maximizing surface-area features in the first release.

## 4. Why Aether Makes Sense for the Intended Use Case

### 4.1 Agent-Oriented Source Needs

Agent-produced code benefits from:

- short, regular declarations,
- low punctuation overhead,
- fewer synonyms for the same operation,
- small deltas between edits,
- visible boundaries around expensive or effectful actions.

For example, compact spellings like:

```aether
const Greeting: Text = "hello";

fn main() -> Void {
    let msg: Text = Greeting;
    ret;
}
```

are cheaper to emit and revise than more verbose alternatives, but still
readable to a human.

### 4.2 Human Review Needs

Humans reviewing agent output care about:

- what data is defined,
- what effects occur,
- where control flow branches,
- what the compiler/runtime will likely do.

Aether should therefore prefer syntax that is compact but unsurprising. It
should not collapse into symbolic shorthand that only helps token counts while
harming review clarity.

### 4.3 PSCAL-Specific Fit

PSCAL already provides:

- a shared AST and bytecode compiler,
- a stable VM,
- concurrency support,
- builtins and extended builtins,
- caching and bytecode serialization,
- multiple front ends with existing integration patterns.

Aether can therefore focus on the source-language problem rather than trying to
replace the rest of the system.

That is why Aether makes sense here: the suite already has a good backend.
What is needed is a front end optimized for agentic authoring and human
inspection.

## 5. Core Language Philosophy

The Aether surface language should follow these rules:

1. Use short keywords where they provide clear value.
2. Keep syntax regular and composable.
3. Favor one obvious spelling for core constructs.
4. Make effectful intent visible.
5. Optimize for the whole authoring loop, not just initial source size.
6. Avoid backend inventions unless they clearly improve source economy or
   correctness.

This means Aether may intentionally choose:

- `fn` instead of `function`,
- `ret` instead of `return`,
- `fx` instead of `effect`,
- typed `let` declarations with `name: Type`,

while still preserving readable block structure and recognizable control flow.

## 6. Architectural Model

### 6.1 Final Architecture

At maturity, Aether should look like this:

```text
Aether source
  -> Aether lexer
  -> Aether parser
  -> Aether semantic analysis
  -> shared PSCAL AST
  -> shared bytecode compiler
  -> shared PSCAL bytecode
  -> shared PSCAL VM
```

### 6.2 Current Bootstrap

The current bootstrap uses:

```text
Aether source
  -> Aether source rewrite layer
  -> Rea parser/semantic path
  -> shared PSCAL AST
  -> shared bytecode compiler
  -> shared PSCAL VM
```

This is intentionally transitional. It exists to:

- establish the `aether` binary and build target,
- provide an executable iteration loop,
- validate syntax choices quickly,
- avoid premature backend churn.

The rewrite layer is not the final design.

### 6.3 Shared Backend Rule

Aether-specific behavior should stay in the Aether front end unless there is a
clear backend reason to centralize it.

Backend changes are allowed only when they are:

- small,
- clean,
- broadly justifiable,
- useful for Aether without compromising other front ends.

### 6.4 Shared Semantic Validation

Aether should eventually have two semantic layers:

1. **Aether-specific frontend semantics**

   - binding and scope,
   - Aether typing rules,
   - effect checking,
   - contract checking,
   - Aether-specific diagnostics.

2. **Shared PSCAL AST validation**

   - backend type invariants,
   - bytecode-safety assumptions,
   - VM compatibility,
   - optimizer assumptions,
   - shared structural checks.

This keeps Aether source semantics owned by the front end while preventing
backend invariants from drifting across languages.

## 7. Syntax Direction

### 7.1 Token Economy

Source token economy is a primary goal, but not the only goal.

Aether should optimize for:

- shorter keywords,
- reduced boilerplate,
- fewer redundant markers,
- compact literal forms,
- compact declaration forms.

It should not optimize by making source inscrutable.

The real cost model includes:

- initial generation,
- compile failures,
- repair prompts,
- stable diffs,
- diagnostic clarity,
- human review time.

That is why Aether should optimize for correctness per token, not for source
shortness alone.

### 7.2 Declarations

Preferred direction:

```aether
const Greeting: Text = "hello";
let count: Int = 0;
fn main() -> Void { ... }
```

This gives:

- compact keyword set,
- left-to-right readability,
- explicit type attachment,
- direct lowering to existing declaration machinery.

### 7.3 Control Flow

Preferred compact control flow:

```aether
if ready {
    ret;
} else {
    ret;
}
```

The language should prefer concise and familiar block forms over more verbose
English-like spellings such as `when ... then`.

### 7.4 Effects

Effects should be visible in source:

```aether
fx {
    writeln(msg);
}
```

The compact spelling matters because effect boundaries may appear frequently in
agent-generated code. `fx` is short, distinct, and still understandable.

### 7.5 Contracts

Contracts should be represented as lightweight annotations:

```aether
@pre n >= 0
@post result >= 1
@pure
fn fact(n: Int) -> Int { ... }
```

The exact lowering may evolve, but the surface form should stay compact and
regular.

Contracts must not be decorative. If a contract form exists, the compiler or
runtime should be able to do something meaningful with it.

## 8. Aether Core

Before Aether grows into a larger language, it should define a small, coherent
core.

The first meaningful Aether Core should include:

- modules,
- imports,
- constants,
- typed `let` bindings,
- functions and procedures,
- `if`,
- `while`,
- simple `for`,
- compact record/struct-like types,
- explicit public signatures,
- `fx` regions,
- contracts lowered to ordinary checks,
- deterministic formatting and diagnostics.

The first meaningful Aether Core should explicitly defer:

- classes and inheritance,
- operator overloading,
- reflection,
- macros,
- metaprogramming,
- advanced generics,
- implicit conversions,
- rich async sugar,
- user-defined syntax.

## 9. Semantic Model

### 9.1 Variables and Constants

Aether distinguishes:

- mutable local/global bindings via `let`,
- immutable bindings via `const`.

These should lower directly onto shared declaration and constant-handling
machinery.

### 9.2 Functions

Functions should:

- declare argument names and types explicitly,
- declare return types explicitly,
- support annotations,
- lower cleanly to the shared function/procedure model.

### 9.3 Effects

The semantic purpose of `fx` is to mark code that may perform I/O or other
observable side effects.

Initial implementation direction:

- `fx` is a front-end semantic fence.
- Calls to designated effectful routines outside `fx` should eventually be
  rejected by semantic analysis.
- `fx` should not require a separate VM frame model or separate runtime.

This yields the user-facing clarity we want without fabricating a new execution
subsystem.

The effect story should become a real semantic model, not just a style rule.
The initial categories do not need to be elaborate, but they should be
concrete. A practical early set is:

- `pure`
- `mut`
- `io`
- `fail`
- `spawn`
- `nondet`

Not all of these need first-class syntax immediately, but the semantic
direction should be explicit so effect propagation and restrictions are
mechanical rather than decorative.

### 9.4 Parallelism

`par` is a desirable Aether feature because PSCAL already has concurrency
support. However, it should be introduced carefully.

Initial semantic constraints should likely include:

- only allow direct call forms at first,
- restrict mutable outer captures,
- make data-sharing rules explicit,
- lower to existing thread primitives.

### 9.5 Contracts

Contracts should initially lower into ordinary guard code rather than requiring
deep VM semantics.

Example direction:

- `@pre` becomes a check at function entry,
- `@post` becomes a check before returning,
- `@pure` informs analysis and optimization,
- `@cost` is frontend-validated immediately, even before it grows runtime
  meaning: it must attach to the next function and use a positive integer
  budget with an optional compact unit such as `ms`, `ops`, or `steps`.

This keeps the shared compiler concise while still enabling future expansion.

These should be treated as executable or analyzable language constructs, not as
free-form prose. Aether should avoid contract forms that merely look formal but
do not affect checking, diagnostics, or tooling.

That applies to attachment and syntax as well as runtime meaning. An annotation
that does not bind cleanly to the next function, or that has malformed syntax,
should fail in the frontend rather than silently degrade into commentary.

## 10. Data Model

### 10.1 Primitive Types

The initial high-level type set should stay small:

- `Int`
- `Real`
- `Text`
- `Bool`
- `Void`

These should map onto the suite's existing underlying value model rather than
introducing a distinct Aether runtime type universe.

### 10.2 Structured Data

Aether should support user-defined types, but v1 should stay disciplined.

Practical direction:

- named `type` declarations,
- compact record-like structures,
- later expansion as needed.

In the bootstrap path, a compact `type Name { ... }` surface can lower onto the
shared object/class machinery as long as the lowering is direct and readable.
That gives Aether a concise data-shape construct without creating a separate
runtime object model. A compact construction form that still lowers to
ordinary object creation and field assignment is also a good fit for this
phase, for example `let point: Point = Point { x: 1, y: 2 };`.

For method bodies, Aether should prefer `self` as the source-level receiver
spelling even if the shared backend internally still uses `myself`.

### 10.3 TOON

TOON is attractive because compact data literals matter for agent-generated
source. But it should be implemented carefully.

Important rule:

- TOON must be parsed structurally.
- It should not be treated as a single naive lexer token.

That means nested data should be understood as syntax, not just captured as a
blob.

The initial implementation may lower TOON into existing PSCAL-compatible data
construction patterns or library helpers before deciding whether additional
backend support is justified.

In the current bootstrap, Aether should prefer embedding canonical TOON text
using the upstream TOON syntax itself, rather than inventing a fake JSON-like
placeholder notation. A small frontend wrapper that lowers embedded TOON blocks
to strings or helper calls is acceptable in v0; a separate runtime type is not.

As a practical bridge, compact Aether helper spellings may lower directly onto
the existing shared JSON helper library rather than introducing Aether-only
data APIs. That keeps TOON usable in source while preserving the shared
compiler and VM boundary. In v0, that bridge should be described honestly as a
JSON-compatible literal path, not as full semantic support for all TOON
documents. A small step beyond direct inline literals is still reasonable:
simple local `Text`/`TOON` bindings can be folded back into literal parse
calls when the frontend can prove their source text.

Handle-oriented access helpers that map one-to-one onto existing yyjson
builtins are a good fit for the bootstrap because they preserve predictable
lowering and avoid introducing Aether-only runtime machinery.

Slightly higher-level keyed scalar helpers are also a good fit when the
lowering stays just as direct, for example `toon_get_text(root, "name")`
becoming a yyjson key lookup followed by a string extraction.

The same applies to capability probes: a compact source-level helper like
`has_toon()` is preferable to surfacing backend-specific builtin names and
extension identifiers directly in ordinary Aether code.

Opaque source-level handle types such as `ToonDoc` and `ToonNode` are also a
good fit even before Aether owns richer native runtime types, as long as the
bootstrap lowering to the shared backend remains explicit and predictable.

Even in the bootstrap phase, those opaque types should carry at least some
front-end semantic weight. Obvious arithmetic misuse should be rejected rather
than silently treated like ordinary integers in user-facing Aether source.
Cross-assignment between `ToonDoc` and `ToonNode` should be rejected as well,
even if both still lower to the same shared backend representation.
The same goes for obvious helper-call mismatches like `toon_close(root)` or
`toon_text_value(doc)` when the source-level handle kinds are known.
The same frontend semantic layer should also enforce typed scalar extraction:
`toon_get_text(...)` / `toon_text_value(...)` should flow into `Text`,
`toon_get_int(...)` / `toon_int_value(...)` into `Int`,
`toon_get_real(...)` / `toon_real_value(...)` into `Real`, and
`toon_get_bool(...)` / `toon_bool_value(...)` into `Bool`. Structural helpers
like `toon_len(...)` and `toon_null_value(...)` should participate in that same
typed scalar model as `Int` and `Bool` respectively.
Once those bindings exist, direct scalar-to-scalar assignment should preserve
the same source-level type as well, so mismatches like assigning `Text` into a
`Bool` binding fail in Aether before lowering.
The same applies to helper arguments when they come from typed bindings:
`toon_key(...)` and `toon_get_*` should require `Text` keys, while
`toon_at(...)` should require an `Int` index.
Likewise, `toon_parse(...)` should require a `Text` or `TOON` payload binding
when its source argument is a named variable rather than a literal.
`toon_parse_file(...)` should similarly require a `Text` path binding for named
arguments at the Aether source level.
On top of extraction, Aether should expose compact inspection helpers like
`toon_type(...)` and `toon_is_*` so agents can branch on node kind without
falling back to backend-specific yyjson names.
For the same reason it should expose compact presence checks such as
`toon_has_key(...)` and `toon_has_at(...)`, so common “is this here?” queries
do not require raw handle-probing patterns in user code.
Likewise, defaulted lookup helpers such as `toon_get_text_or(...)` and
`toon_get_int_or(...)` are a good fit for agent-oriented code, because they
encode a very common data-extraction pattern directly in the source language
without forcing explicit presence checks and branching at each call site.

## 11. Why Standalone Front-End Status Matters

If Aether remained permanently as "a rewrite layer in front of Rea", that would
cause long-term problems:

- semantics would be constrained by another front end's grammar,
- diagnostics would reflect the wrong source model,
- feature evolution would become awkward,
- testing would not cleanly isolate Aether behavior,
- design decisions would be distorted by REA compatibility.

That is why standalone front-end status is not cosmetic. It is required for:

- correctness,
- maintainability,
- language identity,
- clean diagnostics,
- future feature growth.

The backend may be shared. The front end should not be conceptually borrowed.

## 12. Bytecode and VM Relationship

### 11.1 Shared Bytecode Compiler

The bytecode compiler should remain the same shared compiler used by the rest
of the suite.

Aether may motivate:

- small lowering helpers,
- metadata handling,
- minor compiler cleanups,
- narrow new opcodes only if clearly justified,

but it should not fork the compiler.

### 11.2 Shared VM

The PSCAL VM remains the execution engine.

This is important because it preserves:

- operational consistency,
- compatibility with existing infrastructure,
- common debugging paths,
- common serialization and caching behavior,
- common testing and deployment behavior.

### 11.3 Extension Discipline

If Aether needs backend support, it should prefer:

1. front-end lowering first,
2. shared compiler helper second,
3. minimal VM change last.

This prevents Aether from becoming backend-driven instead of source-driven.

## 13. Tooling Direction

The expected tooling story includes:

- `aether` compiler/runner binary,
- AST dump support,
- bytecode dump support,
- reuse of existing build/test conventions,
- eventual standard library support under `lib/aether/`,
- future editor support once syntax is stable.

Aether should feel like a first-class PSCAL front end operationally, not like a
special-case sidecar.

## 14. Security and Trust Roadmap

### 13.1 Signed Bytecode

Aether's future roadmap should include **optional cryptographic signing of
compiled bytecode artifacts**.

The purpose is:

- integrity,
- authenticity,
- verification that the bytecode artifact has not been altered.

The purpose is **not**:

- secrecy,
- obfuscation,
- making disassembly impossible.

This is a good fit for PSCAL because bytecode is already compact and portable.
Signing can therefore protect trust without inflating source or requiring a new
execution model.

### 13.2 Design Constraints for Signing

The signed-bytecode feature should:

- build on the existing bytecode serialization path,
- preserve compatibility with unsigned artifacts,
- avoid changing normal execution semantics,
- support explicit verification modes,
- remain optional.

Suggested future model:

- canonical bytecode payload,
- cryptographic digest,
- signature block,
- signer/key identifier,
- verification before execution or import when configured.

### 13.3 Relationship to Current Hashing

The suite already contains hashing for cache integrity and invalidation.
That is useful but not sufficient for trust.

The signed-bytecode roadmap should therefore distinguish:

- cache hashing for freshness/integrity bookkeeping,
- cryptographic signatures for artifact trust.

## 15. Implementation Strategy

### 14.1 Phase 0: Bootstrap

Goals:

- create the `aether` target,
- integrate with build/install/test flow,
- validate a minimal compact syntax subset,
- avoid backend churn.

This phase is already underway.

### 14.2 Phase 1: Minimal Native Language Slice

Target features:

- `let`
- `const`
- `fn`
- `ret`
- `mod`
- `use`
- annotation syntax
- `fx`

At first, some of this may still be implemented through front-end rewriting,
but the goal is to establish stable semantics and tests.

### 14.3 Phase 2: Native Lexer and Parser

Replace rewrite dependence with:

- dedicated token set,
- dedicated Aether parser,
- direct AST construction for Aether forms,
- Aether-specific diagnostics.

This is the point where Aether becomes truly standalone at the front-end level.

### 14.4 Phase 3: Semantic Enforcement

Add:

- effect-boundary validation,
- contract lowering and checks,
- tighter symbol/type rules,
- early `par` restrictions.

### 14.5 Phase 4: Data and Library Expansion

Add:

- TOON parsing and lowering,
- Aether stdlib support,
- richer structured-data handling,
- more examples and library tests.

### 14.6 Phase 5: Trust and Distribution

Add:

- optional signed bytecode,
- artifact verification policies,
- integration with cache/load paths,
- test coverage for tamper detection.

## 16. Testing Strategy

Aether should follow the same operational rigor as the rest of the suite.

Testing should include:

- smoke tests for compile/no-run and AST dump,
- parser tests for compact spellings,
- semantic tests for effect and contract rules,
- bytecode dump/regression tests,
- standard library tests,
- future verification tests for signed artifacts.

The language should not rely on informal examples alone.

## 17. Tradeoffs

### 16.1 Compactness vs Readability

Aether intentionally favors compactness more than Pascal does.
That creates risk if taken too far.

The language should therefore compress:

- boilerplate,
- keyword length,
- repeated ceremony,

but not compress:

- control-flow shape,
- type attachment,
- effect visibility,
- reviewability.

### 16.2 Front-End Speed vs Backend Purity

Using a rewrite/bootstrap path speeds early iteration, but should not become a
permanent crutch.

The correct long-term tradeoff is:

- fast bootstrap first,
- native front-end ownership second.

### 16.3 Feature Richness vs Stability

Aether should not attempt to launch with every possible agent-friendly feature.

It is better to ship:

- a small coherent language,
- clean lowering,
- good diagnostics,
- strong tests,

than a broad but unstable design.

## 18. Summary

Aether is intended to be a compact, explicit, human-auditable language for
agentic programming on top of PSCAL.

Its key properties are:

- standalone front end,
- shared backend and VM,
- compact syntax chosen with token economy in mind,
- explicit effect and contract direction,
- disciplined backend changes,
- future optional signed-bytecode support for trust.

The central idea is simple:

Aether should make it cheaper for agents to produce correct source while making
it easier for humans to review that source, all without fragmenting the PSCAL
execution model.
