---
name: bug-drill
description: Structured PSCAL/Rea/Aether compiler-bug workflow - reproduce, fix on the current (AST) path, run affected suites against baselines, then ship. Use when given a compiler bug report, a failing repro program, a diagnostic regression, or a chip about a frontend/VM defect.
---

# /bug-drill — compiler-bug work order

The user's proven format, automated. Input: a repro (inline program, file, or
chip description). CLAUDE.md carries the environment; do not re-derive it.

## 1. Reproduce first

- Write the repro to the scratchpad, run it against the CURRENT binary:
  `./build/bin/<frontend> --no-cache <file>` (frontends: aether, rea, pascal,
  clike, exsh; binaries in `build/bin/`).
- Capture exact stdout/stderr/exit code. If it does NOT reproduce, check
  recent commits — chips are often already fixed — and report instead of
  "fixing".
- Useful probes: `--dump-ast-json`, `--dump-bytecode`.

## 2. Locate on the live path

- Aether: the AST parser (`ast_parser.c` in components/aether) is the default
  frontend; grep the diagnostic string there. The translate.c rewriter is
  gone/legacy — never fix there.
- Rea/Pascal/CLike: shared engine lives in components/pscal-core and
  components/rea. Respect the dependency chain aether → rea → pscal-core; a
  pscal-core fix may need coordinated bumps (the /ship skill handles pins).
- Honor any no-touch fences in the work order verbatim.

## 3. Fix minimally

- Edit in `components/<name>/`, never `build/_deps/`.
- VM builtin registry IDs are append-only. NEVER renumber or insert mid-table.

## 4. Verify against baselines

- Re-run the repro (expect fixed) and a couple of adjacent shapes (the
  "fixing two-liner broke one-liner" regression class).
- Rebuild: `cmake --build build --target <frontend>`.
- Run the affected frontend suite(s); compare against the known baselines
  (memory: pscal-test-baseline lists expected failures). New failures = not
  done. `python3 Tests/run_all_suites.py` for wide changes.
- If compiler output-shape changed, check corpus/fixture drift:
  `python3 tools/recapture_expected.py --check` (see its --help).

## 5. Ship

Invoke the `/ship` flow (component commit → pins → gitlink bump → push →
claw deploy verify). If the Aether language surface changed, bump VERSION +
CHANGELOG and update the guide docs (aether_for_llms_and_others.md and the
concise variant) in the same pass.

## 6. Report

Root cause in one or two sentences, the fix location (file:line), suite
results vs baseline, and what shipped where.
