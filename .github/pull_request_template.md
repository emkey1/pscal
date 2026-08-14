Title: <concise summary>

Summary
- What does this change do and why?

Checklist
- Base branch is `main`. Branch from `main`, PR back into `main`.
- CI passes (build, tests, examples, iOS build).
- Scope is minimal and focused.
- Docs updated if behavior or public APIs changed.

Notes
- Component code lives in the `components/` submodules, not in this repo. If your
  change touches a frontend or the VM, open it against the component repo
  (`pscal-core`, `rea`, `aether`, `clike`, `pascal`, `exsh`) and bump the gitlink here.
- Please avoid long-running feature branches; prefer smaller, incremental PRs.
