---
name: ship
description: Commit, push, bump gitlinks, and deploy verified PSCAL/Aether work across the multi-repo layout and the claw fleet. Use after a verified fix, or when the user says "commit and push", "ship it", or asks whether everything is committed/deployed.
---

# /ship — the PSCAL multi-repo ship flow

Execute the full chain. Skip steps whose inputs are unchanged, but always
*check* each step rather than assuming.

## 0. Inventory

- `git status` in PBuild AND in each `components/<name>` with changes
  (`git submodule foreach --quiet 'git status --porcelain | head -1 && echo $name' 2>/dev/null` or check each).
- Identify which components changed: pscal-core, rea, aether, clike, pascal, exsh.
- If an Aether language-affecting change shipped without a VERSION bump, bump
  it now via `components/aether/tools/bump_version.py` + CHANGELOG entry.

## 1. Component commits

For each dirty component: commit with a descriptive message, push to origin
(they are all SSH remotes under github.com/emkey1).

## 2. Aether external/ pins

If pscal-core or rea changed, the standalone aether build breaks silently
(PBuild's FetchContent override masks it). Update the vendored pins:

```
cd components/aether
git -C external/pscal-core fetch origin && git -C external/pscal-core checkout <new-sha>
git -C external/rea fetch origin && git -C external/rea checkout <new-sha>
git add external/ && git commit -m "chore: bump external pins" && git push
```

## 3. PBuild gitlink bump

```
cd /Users/mke/PBuild
git add components/<changed...>
git commit -m "chore: bump <name> gitlink (<one-line reason>)"
git push origin AetherLang
```

Branch is **AetherLang**. If main should track (default for language work):
`git push origin AetherLang:main` (fast-forward only; stop and report if it
isn't one).

## 4. Deploy verification

The post-commit hook runs `tools/deploy_aether_to_claws.sh` (builds the pushed
SHA on claw1/claw2/claw3, best-effort). Confirm it fired; if the commit was
made in an environment where hooks were skipped, run the script manually.
Spot-check: `ssh claw@claw1.tailfe3968.ts.net '~/aether-current/build/bin/aether --version'`
and compare to `components/aether/VERSION`.

## 5. Report

One short summary: what was committed where (SHAs), gitlinks bumped, deploy
verified or not (and why). If anything was intentionally left uncommitted,
say so explicitly.
