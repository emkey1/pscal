Title: <concise summary>

Summary
- What does this change do and why?

Checklist
- Base branch is `devel` (feature → devel). For releases, use `devel` → `main`.
- CI passes (build, tests, examples).
- Scope is minimal and focused.
- Docs updated if behavior or public APIs changed.

Notes
- PRs targeting `main` from any branch other than `devel` will be rejected by CI.
- Please avoid long‑running feature branches; prefer smaller, incremental PRs.

