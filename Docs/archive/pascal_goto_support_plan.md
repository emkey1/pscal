# Pascal `goto` Support Plan

## Task List

- [x] **Extend lexical and AST support**
  - [x] Add `goto` and `label` tokens to the Pascal token enumeration and keyword lookup.
  - [x] Introduce AST node variants for labels and goto statements, plus helpers for creation and printing.
  - [x] Thread the new node types through any existing AST visitation/serialization utilities.

- [x] **Update the Pascal parser**
  - [x] Parse `label` declaration blocks at the routine level and attach the labels to the current compilation scope.
  - [x] Parse statement labels (`<number>:`) and `goto <number>` statements, emitting the new AST nodes.
  - [x] Ensure parser error recovery handles malformed label blocks and goto statements gracefully.

- [x] **Track label metadata during compilation**
  - [x] Store per-routine label tables in the appropriate compiler state structure, including definition sites.
  - [x] Validate duplicate label declarations and undefined goto targets with clear diagnostics.
  - [x] Decide and document whether cross-procedure gotos are disallowed (they should be) and enforce that rule.

- [x] **Emit bytecode for labels and gotos**
  - [x] When compiling a label, record the current bytecode offset so forward references can be patched.
  - [x] Emit unconditional jump instructions for goto statements and patch them once the target offset is known.
  - [x] Cover nested blocks and ensure clean state resets when exiting routines.

- [x] **Add regression coverage and documentation**
  - [x] Promote the `BlackJack` example (which already contains gotos) into an automated test that now passes.
  - [x] Add new scope-verification cases for valid and invalid label/goto usage.
  - [x] Update the TODO list and Pascal front-end documentation to reflect that goto is supported.
