# CLAUDE.md — PBuild (PSCAL umbrella)

Standing environment facts and policies for every session. Coding practices
and test-harness details live in [AGENTS.md](AGENTS.md); deep operational lore
lives in the auto-memory files (this file wins if they disagree).

## Repo layout and branches

- This is the thin umbrella/integrator repo. The real code lives in git
  submodules under `components/`: `pscal-core`, `rea`, `aether`, `clike`,
  `pascal`, `exsh` (all public under github.com/emkey1). Dependency chain:
  aether → rea → pscal-core.
- PBuild builds the components from `components/` siblings via
  `FETCHCONTENT_SOURCE_DIR` overrides. **Edit `components/<name>/`, never
  `build/_deps/` (stale, uncompiled copies).**
- aether additionally vendors rea+pscal-core as submodules under
  `components/aether/external/`. When pscal-core or rea change, those pins go
  stale and break the *standalone* aether build while PBuild stays green.
  Bump them as part of shipping (see Ship flow).
- **Active development branch is `main`.** Commit and push there directly.
  (`AetherLang` was the active branch early on, with `main` synced from it by
  fast-forward; that's no longer the case — as of 2026-07-03 `main` is
  current and `AetherLang` should be treated like any other branch, not a
  required landing spot.)

## Build and test

- Configure/build: `cmake --build build --target <aether|rea|pascal|clike|exsh>`
  (build dir already configured). Binaries land in `build/bin/`.
- Run frontends with `--no-cache` when testing compiler changes:
  `./build/bin/aether --no-cache prog.ae`.
- Full sweep: `python3 Tests/run_all_suites.py` (core, library, scope). Consult
  the per-suite baselines before declaring regressions (memory:
  pscal-test-baseline has the itemized known failures).
- Aether language version: `components/aether/VERSION` (YYYY-MM-DD-N). Bump
  only on language-affecting changes via `components/aether/tools/bump_version.py`
  + CHANGELOG.
- Aether parse errors come from the AST parser (`ast_parser.c`), the default
  frontend since 2026-06-27. Grep the error string there first.

## Fleet (always tailscale FQDNs, never raw IPs — they rotate)

| Host | FQDN | Hardware | Role |
|------|------|----------|------|
| claw1 | claw1.tailfe3968.ts.net | GB10 Spark, 128GB | rich Ollama bench host (:11434), /storage NFS server (7.3T), ds4 |
| claw2 | claw2.tailfe3968.ts.net | GB10 Spark, 128GB | training rig; docker Ollama on **:11435** (snap owns :11434) |
| claw3 | claw3.tailfe3968.ts.net | GB10 Spark, 128GB | image/video gen (ComfyUI :8188); keep clean |
| m4t | m4mini.tailfe3968.ts.net | M4 mini, 64GB | T'Ra scheduler :8793, YouTube transcription :8792 |
| m2t | (LM Studio :1215) | M2 Mac, 32GB | small LM Studio node |
| m5t / this laptop | macbook-pro-2.tailfe3968.ts.net | M4 MBP, 128GB | primary workstation, LM Studio :1215 |
| pscal build box | 169.254.143.229 port 1022 | Devuan 6, very slow | link-local; run long ops detached |

- ssh user on the claws is `claw`, passwordless + passwordless sudo.
- **All shared-GPU LLM jobs go through the T'Ra scheduler at
  http://100.121.116.25:8793 (m4t)** — never hit Ollama/LM Studio directly for
  batch work (direct hits caused the qwen3.6 contention mystery). Workflow:
  `GET /api/targets` (never guess names) → `/jobs/explain` → `POST /jobs` →
  poll `/jobs/{id}?wait=`. Max one job per endpoint; different endpoints in
  parallel is encouraged.

## Ship flow — do this after every verified fix, without being asked

1. Commit in the component repo(s) and push.
2. Bump dependent gitlinks: if pscal-core or rea changed, update
   `components/aether/external/` pins (`git -C components/aether update-index
   --cacheinfo 160000,<sha>,external/<name>` or checkout+add) and push aether.
3. Bump the `components/` gitlinks in PBuild, commit, push `main`.
4. Autodeploy (post-commit hook → `tools/deploy_aether_to_claws.sh`) builds the
   pushed SHA on all three claws. Verify it ran; confirm with
   `ssh claw@clawN '~/aether-current/build/bin/aether --version'` when in doubt.
5. If the language changed, also confirm the VERSION bump (step 0, really).

Never leave verified work uncommitted at the end of a task. The `/ship` skill
wraps steps 1–4.

## Long runs (anything over ~10 minutes)

- Run remote-and-detached (tmux on the claws) or local under
  `tmux + caffeinate`. Never the harness background Bash for hours-long jobs
  (they die on session interrupt/compaction) and never a foreground Bash call
  that can hit the 2-minute timeout.
- Drivers must be resumable: per-unit output + skip-finished check, `set -u`
  not `-e`, so any death resumes with no lost work.
- **Smoke-test every new script on one item and inspect the real output
  artifact before trusting it for a long/overnight run.** "Process alive" and
  completion echoes prove nothing. Watch macOS↔Linux mismatches (`timeout`
  absent on mac, `sed -i ''` vs `sed -i`).
- On completion, notify (PushNotification) rather than waiting to be polled.
  Status questions should be answerable from the run's own log/progress files.

## Budgets and result triage

- **Never use a serving tool's default context or a stingy output-token cap.**
  These are 128GB hosts: set context large (20k–32k minimum, 128k where it
  fits) and output budgets generous — reasoning models return EMPTY content
  when truncated. Ollama `/v1` silently ignores `options.num_ctx`; use
  `OLLAMA_CONTEXT_LENGTH` or a `PARAMETER num_ctx` model variant. Ollama
  keep-alive default (5 min) is too short; use 1h.
- **Sanity-check every eval score the moment it lands.** A bogus/surprising
  result (0/30 from a capable model) means immediate triage — gen_ok →
  returncodes → stderr histogram → sample generations — then fix and rerun.
  Never flag-and-move-on.
- Prefer parallel advancement across the fleet (via T'Ra) over serial
  measurement; idle claws are wasted money.

## Credentials — locations only, and a hard rule

**NEVER write, append, or heredoc into a token/credential file. Read-only.**
(This is how tokens get corrupted and how leaks happen.)

| What | Where |
|------|-------|
| GLM/z.ai proxy JWT (expires; refresh on 401) | `~/.openclaw-autoclaw/openclaw.json` → models.providers.zai X-Authorization |
| LM Studio API keys (per host) | `~/.openclaw-autoclaw/openclaw.json` (localhost:1215 provider apiKey); note m5t token contains a `:` |
| z.ai API key | `claw@claw1:~/zap` |
| Hugging Face token | `/storage/hf/token` on the claws (currently read-only scope) |
| Gemini/Google token | openclaw.json |

Never do unauthenticated HF downloads; we have a token — use it.

## Chips / spawned tasks

- When authoring a chip prompt, don't embed architecture snapshots (they go
  stale — the "translate.c rewriter" boilerplate cost five corrected sessions).
  Point at living docs (`components/aether/docs/parser_roadmap.md`, this file)
  and say "verify current architecture before editing".
- When picking up a chip, first check recent commits — it may already be done.

## Environment quirks

- macOS filesystem is case-insensitive; the claws are Linux. Never create
  paths differing only by case (the `Tools/` vs `tools/` split); `git add`
  can silently no-op across case variants.
- zsh: quote or escape `=`/globs in ad-hoc commands (`echo ===` fails with
  "== not found"; unmatched globs error with "no matches found").
- Use absolute paths in Bash tool calls; cwd does not persist reliably.
- Heredocs over ssh are fragile: write scripts locally, `scp`, run detached
  via `setsid`/tmux.
- Read a file before Writing over it (harness requirement); memory files
  included.
