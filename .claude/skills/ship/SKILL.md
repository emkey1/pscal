---
name: ship
description: Commit, push, bump gitlinks, and deploy verified PSCAL/Aether work across the multi-repo layout and the claw fleet. Use after a verified fix, or when the user says "commit and push", "ship it", or asks whether everything is committed/deployed.
---

# /ship — the PSCAL multi-repo ship flow

Execute the full chain. Skip steps whose inputs are unchanged, but always
*check* each step rather than assuming.

## 0. Inventory

- In a fresh Claude worktree (`/Users/mke/git/pscal/.claude/worktrees/<name>`)
  every submodule is uninitialized (`git submodule status` shows a leading
  `-`). Init the ones you need, borrowing objects from the main clone:
  `git submodule update --init --reference /Users/mke/git/pscal/<path> -- <path>`.
- `git status` in PBuild AND in each `components/<name>` with changes
  (`git submodule foreach --quiet 'git status --porcelain | head -1 && echo $name' 2>/dev/null` or check each).
- Identify which components changed: pscal-core, rea, aether, clike, pascal, exsh.
- If an Aether language-affecting change shipped without a VERSION bump, bump
  it now via `components/aether/tools/bump_version.py` + CHANGELOG entry.

## 1. Component commits

For each dirty component: commit with a descriptive message, push to origin
(they are all HTTPS remotes under github.com/emkey1).

## 2. Aether external/ pins

If pscal-core or rea changed, the standalone aether build breaks silently
(PBuild compiles `components/<name>/` directly and never reads aether's
`external/`, so it stays green). Update the vendored pins:

```
cd components/aether
git -C external/pscal-core fetch origin && git -C external/pscal-core checkout <new-sha>
git -C external/rea fetch origin && git -C external/rea checkout <new-sha>
git add external/ && git commit -m "chore: bump external pins" && git push
```

If `external/` isn't checked out (e.g. a fresh worktree), set the pins without
it: `git -C components/aether update-index --cacheinfo 160000,<sha>,external/<name>`,
then commit and push aether.

## 3. PBuild gitlink bump

From the PBuild checkout you're working in (the clone at
`/Users/mke/git/pscal`, or a Claude worktree under its `.claude/worktrees/`):

```
git add components/<changed...>
git commit -m "chore: bump <name> gitlink (<one-line reason>)"
git push origin HEAD:main
```

Branch is **main**. A worktree is on a `claude/...` branch; `HEAD:main` pushes
it straight to main (fast-forward only; stop and report if it isn't one).

## 4. Deploy verification

Only aether, rea and pscal-core feed the claw binary. A pascal/clike/exsh-only
bump needs no deploy; skip this step and say so in the report.

The post-commit hook (`tools/git-hooks/post-commit`) runs
`tools/deploy_aether_to_claws.sh` in the background when a PBuild commit
touches `components/{aether,rea,pscal-core}` (builds the gitlink SHA on
claw1/claw2/claw3, best-effort; log in `/tmp/aether_autodeploy.log`). It only
fires if `tools/install_aether_autodeploy.sh` installed it, so check first.
Worktrees share the main clone's hooks:

```
ls -l "$(git rev-parse --git-common-dir)/hooks/post-commit"
```

If the hook is missing (it was on 2026-09-22) or the commit skipped hooks, run
`bash tools/deploy_aether_to_claws.sh` by hand from the checkout you committed
in. It reads `components/aether/VERSION`, so that submodule must be initialized,
and it can outlast a foreground Bash call, so run it in the background.
Confirm the final `=== done: N/3 hosts current` line.
Spot-check: `ssh claw@claw1.tailfe3968.ts.net '~/aether-current/build/aether --version'`
and compare to `components/aether/VERSION`.

## 5. Report

One short summary: what was committed where (SHAs), gitlinks bumped, deploy
verified or not (and why). If anything was intentionally left uncommitted,
say so explicitly.
