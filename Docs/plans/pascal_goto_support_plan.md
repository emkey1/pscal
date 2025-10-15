# Pascal `goto` Support Plan

## Task List

- [ ] **Extend lexical and AST support**
  - [ ] Add `goto` and `label` tokens to the Pascal token enumeration and keyword lookup.
  - [ ] Introduce AST node variants for labels and goto statements, plus helpers for creation and printing.
  - [ ] Thread the new node types through any existing AST visitation/serialization utilities.

- [ ] **Update the Pascal parser**
  - [ ] Parse `label` declaration blocks at the routine level and attach the labels to the current compilation scope.
  - [ ] Parse statement labels (`<number>:`) and `goto <number>` statements, emitting the new AST nodes.
  - [ ] Ensure parser error recovery handles malformed label blocks and goto statements gracefully.

- [ ] **Track label metadata during compilation**
  - [ ] Store per-routine label tables in the appropriate compiler state structure, including definition sites.
  - [ ] Validate duplicate label declarations and undefined goto targets with clear diagnostics.
  - [ ] Decide and document whether cross-procedure gotos are disallowed (they should be) and enforce that rule.

- [ ] **Emit bytecode for labels and gotos**
  - [ ] When compiling a label, record the current bytecode offset so forward references can be patched.
  - [ ] Emit unconditional jump instructions for goto statements and patch them once the target offset is known.
  - [ ] Cover nested blocks and ensure clean state resets when exiting routines.

- [ ] **Add regression coverage and documentation**
  - [ ] Promote the `BlackJack` example (which already contains gotos) into an automated test that now passes.
  - [ ] Add new scope-verification cases for valid and invalid label/goto usage.
  - [ ] Update the TODO list and Pascal front-end documentation to reflect that goto is supported.
