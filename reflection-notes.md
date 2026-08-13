# Reflection notes — PBuild session-transcript mining, 2026-07-02

Source: all 46 transcripts in `~/.claude/projects/-Users-mke-PBuild/` plus 2
worktree-variant transcripts (Jun 13 – Jul 2, 2026), scanned by four extraction
subagents (user-typed messages, corrections, tool-error counts, Bash
command-frequency stats), then clustered here. Session ids are transcript
basenames (first 8 chars).

Ranked most-leverage-first. Each cluster: evidence, recurrence, and a verdict:
**skill**, **automation/fix**, **CLAUDE.md fix**, or **nothing**.

The single framing fact: **PBuild has no CLAUDE.md at all.** Every environment
fact (fleet topology, build commands, deploy flow, standing policies) lives
only in auto-memory, whose recall is probabilistic. Roughly half the clusters
below are downstream of that one gap.

---

## 1. Create a CLAUDE.md — the environment preamble is re-typed constantly and memory recall keeps missing

**Verdict: CLAUDE.md fix (an hour, one-time). Highest pain-to-cost ratio in the corpus.**

The user re-supplies the same environment block over and over, and polices
memory recall when it fails:

- Full env preamble (umbrella path, build dir, submodule layout, `--no-cache`,
  `cmake --build ... --target rea`, suite baselines) re-typed nearly verbatim in
  0bf280a7, da7f850a, dc605836 — three sessions within ~24h of each other.
- Host identity re-taught repeatedly: 78ab3d17 "Lets start using the tailscale
  DNS for nodes. Please save the following..."; 3b2c0831 "Dude, you need to keep
  better track of this stuff. We've used that label MANY times.
  macbook-pro-2.tailfe3968.ts.net is the DNS name."; "No, this is not mint...
  This laptop is macbook-pro-2"; "Err, m4t is actually a 64GB m4 Mac Mini."
- Deploy topology re-explained in 2383fc7d ("The claws are also supposed to
  pull the latest changes"), 78ab3d17, a8b9dbc3.
- Same-day work lost: 78ab3d17 "Dude, check your freaking memory. We did a
  training and a test suite earlier today." → "And in the future, please keep
  better track."
- The user actively curates memory by hand to compensate: 16a6488e "Please save
  that instead of an IP or the label m4t"; db9b8391 "Please fully document the
  care and feeding of this capability and make it available in your memory."

CLAUDE.md loads unconditionally; memory recall does not. **Proposal:** a
PBuild/CLAUDE.md with: (a) fleet table (claw1/claw2/claw3, m2t/m4t/m5t =
macbook-pro-2, tailscale FQDNs, RAM, roles); (b) repo layout + the multi-repo
commit→gitlink-bump→push→deploy flow; (c) canonical build/test commands and
suite baselines; (d) the standing policies that keep slipping (see #2, #4, #5);
(e) pointers to the memory files for deep detail. This is the same move as the
iSH exercise's #1 + #6, and it retires the largest [re-explain] class.

## 2. "Please commit and push" (+ deploy to claws) — the most repeated message in the corpus

**Verdict: CLAUDE.md policy (tiny) + small automation (a `/ship` skill or script wrapping the existing pieces).**

Some variant of "commit and push" closes ~30 of 46 sessions, often multiple
times per session, and the user has already stated the rule explicitly:

- 78ab3d17: "you should always commit, push and pull/rebuild once you have
  verified a fix." (stated as a standing rule; still not happening proactively)
- 431f8459: "Please commit and push" ×4 in one session; 0d795c7d: 13× git
  commit / 12× git push driven turn-by-turn; 7b57085e "Thank you. commited and
  pushed?"; 7850d641 "Push please." / "Has the doc edit happened yet BTW?"
- The deploy tail keeps getting dropped: ae5e4f5b/8259730e autodeploy escalation
  ("Actually, I need it fixed, and it should auto deploy to all three of the
  claws"), a8b9dbc3 stale `aether -v` after a fresh pull (main-branch gitlink
  lag), 8e8cd35d "We should then set things up so this never ever ever happens
  again, followed by rescoring all the recent benchmark runs."

The machinery exists (tools/deploy_aether_to_claws.sh, bump flow in memory).
**Proposal:** CLAUDE.md: *"After a verified fix: commit the component, bump the
gitlink(s), commit+push PBuild, confirm autodeploy (or run
deploy_aether_to_claws.sh), without being asked. Active branch is AetherLang;
keep origin/main's aether gitlink in sync (`git push origin AetherLang:main`)."*
Optionally wrap the whole chain in one `/ship` skill so it is a single step
instead of a remembered sequence.

## 3. Long-run survivability + status-poll babysitting — the biggest wall-clock sink

**Verdict: CLAUDE.md rules (from existing ⭐ memories) + adopt notification/loop machinery. No new build needed; enforcement is the gap.**

The marathon sessions (8e8cd35d 60MB, 78ab3d17 65MB, 3b2c0831, ae5e4f5b) are
dominated by fragile long jobs and manual heartbeats:

- 8e8cd35d: "Its bullshit that the waiter is dying... That is double bad, no
  good."; "Ugh. It's preferable to run stuff remote, and monitor from here fyi"
  (local jobs died when the laptop closed); the durable-cron self-check message
  repeats ~18×.
- 78ab3d17: "I can't begin to tell you how tired I am of things getting killed
  or dying without completing."; "Can you put it in your high priority memory to
  always verify new scripts? The failure rate was brutal overnight."
- Status polling as a conversational mode: "Continue from where you left off"
  (~25× across sessions), "Status please?"/"ping"/"Progress?" (~30×), "Good
  morning. Did Phase 2 complete correctly?", "Laptop was asleep for a bit."
- ae5e4f5b: 2-minute Bash timeouts and exit-143 kills on long LLM calls, the
  exact anti-pattern the user's own long-runs memory forbids.

The rules already exist as memories (long-runs-detach-resumable,
verify-new-scripts, triage-bogus-results) but keep being relearned mid-session.
**Proposal:** promote to a CLAUDE.md "Long runs" block: tmux+caffeinate or
remote+detached only, resumable per-unit drivers, smoke-test one item and
verify the real artifact before any overnight run, and send a PushNotification
on completion instead of waiting to be pinged. Recurrence is extreme; cost is
copying four memories into the always-loaded file.

## 4. Resource budgets and bogus-result triage still slip despite ⭐ memories

**Verdict: CLAUDE.md fix (5 minutes). Same promote-from-memory move as #3.**

The two most emotionally charged corrections in the corpus, both already
memorized, both still recurring at the time:

- 3b2c0831: "General comment, please stop fucking short changing models that
  are tiny compared to available ram on context and token budgets. I've asked
  this before, but it still keeps happening."
- 78ab3d17: "Err, where did that 8K context cap come from?"; "32k is useless.
  128k would be OK."; 8e8cd35d: "There is no reason to be stingy on the token
  returned budget."
- Bogus zeros accepted without triage cost days: 78ab3d17 "there is no way a
  model that capable goes 0 for 30"; "0/8 does not make sense"; "We've been
  essentially stuck for a couple of days now." → the triage-immediately rule.
- Idle hardware: 3b2c0831 "why isn't claw1 in use?", "node 2 seems mostly
  idle?" — the user pushes large-and-parallel, defaults keep coming back
  small-and-serial.

**Proposal:** three CLAUDE.md lines: big context/output budgets always (128GB
hosts, never a serving tool's default); after every eval, sanity-check the
score and triage bogus results immediately (gen_ok → returncodes → stderr →
sample gens); prefer parallel across the fleet, via the T'Ra queue.

## 5. Spawned-chip prompt hygiene — one stale template caused the same correction 5+ times

**Verdict: CLAUDE.md rule for chip authoring (tiny). The specific instance is mooted; the class is not.**

Chips written before the Jun-27 AST-parser cutover kept describing Aether as "a
line-based text rewriter in translate.c", and the user had to interrupt five
sessions with the same correction: b88688c3 ("Do not update translate.c"),
03cf5b9b, ccfb8cd3 ("the default with Aether is no the AST generator"),
1a8d5d1d, 3412831f ("Just wanted to be sure that it is understood that the
Aether translation code is deprecated"). Related chip friction: fb821b4c
duplicate/stale auto-suggestions ("Another automated suggestion. Legit?" — "I
think this has already been done?"), ae5e4f5b lost chips after an app restart
("Many of those were already completed I believe. Please check the
repo/recent commits.").

**Proposal:** CLAUDE.md rule: *"Chip/spawned-task prompts must not embed
architecture snapshots; point at living docs (parser_roadmap.md, CLAUDE.md)
and state 'verify current architecture before editing'. Before acting on a
chip, check recent commits for whether it is already done."*

## 6. The compiler-bug work-order format — codify what already works

**Verdict: skill (`/bug-drill`), medium cost, solid recurrence (~10 sessions), lower urgency once #1 lands.**

The user hand-writes near-identical, fully-specified work orders for
Rea/Aether/Pascal bugs: exact repro, rebuild command, suite baselines, fenced
no-touch zones ("Do not touch the emitArrayLiteralRuntime ... path",
da7f850a), verification steps. Sessions with the format have near-zero
corrections (0bf280a7, da7f850a, dc605836, d98c7fb1, 2383fc7d, 220c2fe4,
b63c9d57, c881520d). The cost is that each prompt re-states the whole
environment (which #1 removes) and the closing flow (which #2 removes). A
`/bug-drill <repro>` skill that loads the env, runs the repro against
build/bin, fixes on the AST path, reruns the affected suites against their
baselines, and executes the #2 ship flow would make the drill a one-liner.
Build after #1/#2; much of its value is subsumed by them.

## 7. Benchmark/eval ops playbook — recurring failure fingerprints deserve one place

**Verdict: skill or checked-in runbook (`docs/bench_runbook.md` + optional `/bench-triage`), medium cost, high recurrence.**

Every big eval session re-hits the same class of silent failures, each of which
was once diagnosed at length: stop-token-in-reasoning empty content, context
overflow, dead/stale endpoint, unloaded model, direct-hit GPU contention
(the qwen3.6 "WTF would the model be forced onto CPU?" mystery in ae5e4f5b,
interrupted twice), scheduler etiquette taught over three messages ("Err, can
you not hold the m5t queue constantly?" → "please don't submit more than one
per end point"), expiring GLM proxy JWT, per-host LM Studio tokens hand-fed in
chat (78ab3d17 ×2). Sessions: 8e8cd35d, 78ab3d17, 3b2c0831, ae5e4f5b, ab32b92a.
Most fingerprints are already individual memories; the leverage is one runbook
the bench harness work always loads: pre-flight (T'Ra targets, token paths,
context/output budgets per #4), run detached (#3), post-flight bogus-score
triage tree. Worth building; this is where the multi-day losses happened.

## 8. Permission-classifier stalls and prompt friction — run the allowlist skill

**Verdict: fix (the built-in `fewer-permission-prompts` skill does exactly this). Tiny.**

- ab32b92a: 4× "claude-opus-4-8 is temporarily unavailable, so auto mode cannot
  determine the safety of Bash" mid-pipeline; 78ab3d17: 5× the same stall.
- The hot commands are utterly repetitive and safe: `cd /Users/mke/PBuild`
  (2334×), `ssh -o` to the claws (2148× in one batch alone), `cmake --build`,
  `./build/bin/{aether,rea} --no-cache`, suite scripts, `python3 -c` probes.

The current settings.local.json allowlist is a grab-bag of one-off literals.
One pass of `fewer-permission-prompts` removes the model-classifier dependency
from exactly the commands that run hundreds of times per session.

## 9. Fixture/golden-output drift — recurring, wants a re-capture tool

**Verdict: automation (small script/CI check), moderate recurrence.**

Compiler behavior changes silently invalidate stored expectations, and each
discovery is manual: f0a883d0 (manifest expected "Result: 15" vs actual
"Result:15" after the writeln fix; plus capability-dependent expecteds
"whether the aether build links yyjson/AI"), 0d795c7d (rea disasm fixtures
captured against old builtin id 177), the standing Pascal stale-disasm
baseline (pscal-test-baseline memory), and f0a883d0's headline: a previous
automated re-capture task "missed" files and the user had to re-specify it.
**Proposal:** one `tools/recapture_expected.py --check|--update` that re-runs
corpus/suite fixtures against the current binary, diffs, and either reports
drift (CI/pre-commit mode) or re-captures with provenance. Kills a quiet class
of "corpus says the compiler is wrong" noise that feeds training data.

## 10. Fresh-host bootstrap and submodule recovery — script the known path

**Verdict: automation (small `tools/bootstrap_host.sh`), moderate recurrence, cheap.**

Three sessions in one week walked the same serial whack-a-mole on new/slow
boxes: 5b0ccef6 (ipp-m4: "not our ref" submodule wedge → missing openssl →
curl → -lm → -lz → tiny.pbc install break, one failure per turn), e2a11793
(build box: wedged recursive clone, missing OpenSSL, sudo setup), 7baaf69e
(clone issues again). The recovery steps are already a memory
(pscal-submodule-recovery); the dep list is known. A bootstrap script (clone
`--recurse-submodules --no-recommend-shallow`, verify submodule health, apt
dep install, cmake configure with all errors surfaced) turns six round-trips
into one. The tiny.pbc install break specifically cost two sessions before
being fixed ("braking 'make install' for ages now").

## 11. Worktree merge-back targets the wrong branch

**Verdict: CLAUDE.md line (tiny). Folded into #1's branch section.**

Both worktree sessions ended in manual merge confusion, one landing on `main`
when work lives on `AetherLang`: 639a4a77 "Please save me from my stupidity
and merge your changes with branch AetherLang"; 19c7592a "Please commit and
merge back to the main branch" (ambiguous). Combined with the
aether-main-branch-lag incident (a8b9dbc3), one CLAUDE.md line fixes it:
*"Active development branch is AetherLang. Worktree merges land there; sync
main via `git push origin AetherLang:main`."*

## 12. Credential handling — policy exists, add the pointer table

**Verdict: CLAUDE.md pointer (tiny). The behavioral rule is already a ⭐ memory.**

Tokens are hand-fed in chat repeatedly (LM Studio per-host tokens ×3 hosts,
HF token "Don't forget we have a hugging face token" ×3 verbatim in 78ab3d17,
z.ai key location, GLM proxy JWT refresh, claw3 sudo password), and the
corruption hot-button: ab32b92a "if I had a dollar for every time I've heard
something similar to '/storage/hf/token on claw2 is corrupted...', I'd have a
lot of dollars." **Proposal:** a CLAUDE.md "Credentials" table of *locations
only* (never values): openclaw.json for GLM/LM Studio, ~/zap on claw1,
/storage/hf/token (read-only, never write), so no session ever asks or
guesses. The never-write rule rides along from memory into CLAUDE.md.

## 13. Multi-model relay / external-review shuttling — already self-solved

**Verdict: nothing.**

The user used to hand-paste external LLM reviews and analyses as specs
(6442059f claude.ai analysis, 5b0ccef6 CRT/hterm diagnoses, 7850d641 Kimi/Mimo
reviews ×4). The idea-miner harness (ae5e4f5b, a8b9dbc3) automated the
generative side. The remaining paste-a-spec flow works fine as-is.

## 14. Mechanical tool friction — diffuse, mostly harness-level

**Verdict: nothing to build; two lines in CLAUDE.md if desired.**

- Write-before-Read tool violations: 9× in ab32b92a, 6× in 3b2c0831, 10× in
  78ab3d17 — mostly memory-file writes.
- zsh-isms: `no matches found` globs (7baaf69e ×9, ae5e4f5b), `(eval):1: == not
  found` (ab32b92a); relative `cd` after cwd reset (0d795c7d ×2).
- macOS case-insensitive FS vs git (`tools/` vs `Tools/`) bit twice (77042910,
  78ab3d17) — fixed, but the pattern "Mac-authored state silently diverging on
  the Linux claws" is the axis to watch.
- Claude Code app friction reported upstream-worthy: stale flashing-dot session
  indicators (8259730e), lost chips on restart (ae5e4f5b), "safe to relaunch
  during background work?" (ab32b92a).

---

## Summary table

| # | Cluster | Recurrence | Verdict | Cost |
|---|---------|-----------|---------|------|
| 1 | No CLAUDE.md; env preamble re-typed, memory recall slips | ~15+ sessions | write CLAUDE.md | small |
| 2 | Commit/push/deploy ritual + claw skew | ~30 sessions | CLAUDE.md policy + `/ship` | small |
| 3 | Long-run fragility + status babysitting | 5 marathon sessions, days lost | CLAUDE.md (promote ⭐ memories) | tiny |
| 4 | Budget stinginess + bogus-score triage | repeated hot-button corrections | CLAUDE.md (promote ⭐ memories) | tiny |
| 5 | Stale chip-prompt boilerplate | 5+ identical corrections | CLAUDE.md chip rule | tiny |
| 6 | Compiler-bug work-order format | ~10 sessions | skill `/bug-drill` | medium |
| 7 | Bench/eval failure fingerprints | 5 big sessions, multi-day losses | runbook + optional skill | medium |
| 8 | Permission-classifier stalls | 9+ stalls, 2000+ hot commands | run fewer-permission-prompts | tiny |
| 9 | Golden-output/fixture drift | 4+ incidents | re-capture script | small |
| 10 | Fresh-host bootstrap whack-a-mole | 3 sessions in a week | bootstrap script | small |
| 11 | Worktree merges to wrong branch | 2 sessions + 1 lag incident | CLAUDE.md line | tiny |
| 12 | Credentials fed in chat / corrupted files | recurring hot button | CLAUDE.md pointer table | tiny |
| 13 | External-review shuttling | self-solved (idea-miner) | nothing | — |
| 14 | Mechanical tool friction | diffuse | nothing / notes | — |

If you only do one thing: **#1**, and while writing that file fold in #2, #3,
#4, #5, #11, #12 (they are each a few lines of the same document). That single
edit, under an hour, addresses six of the top clusters. Then **#8** (one skill
invocation) and **#7** (the runbook) attack where the actual multi-day losses
happened.
