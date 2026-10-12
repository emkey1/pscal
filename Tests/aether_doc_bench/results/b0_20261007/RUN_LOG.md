# Running notes for the b0_20261007 README (fold in, then delete)

- 2026-10-07 12:50Z: local lane started on the MacBook Air. It went into clamshell sleep on battery at 13:14Z;
  each maintenance wake turned an in-flight request into an infra failure. Aborted; logs in `aborted_air/`.
- 17:02Z: lane moved to m4mini (same binary sha256, umbrella 8c9d7ba3 clone, Python 3.13). The `m4t`
  hosts entry on m4mini points at a dead link-local address, so the destinations use T'Ra's tailnet IP.
- 17:18Z (18:18 BST in the m5 log): m5 LM Studio hit a Metal out-of-memory error (co-tenant ornith-1.5-35b x2 +
  qwen3.5-9b + qwen3.6-35b loaded); the array_minmax request at 17:21Z never passed prompt processing and hung.
  18:47Z: m4mini kernel panic in IOGPUGroupMemory (a `node` process), which killed T'Ra and the lane. Partial unit (17 cases) kept in `aborted_m4mini/`; the unit re-ran from case 1 at 19:13Z.
- 19:59Z: harness switched between units from umbrella 8c9d7ba3 (harness sha b1bd95ae...) to 9d685f30
  (--resume, T'Ra model release). Prompt-template sha256 unchanged (4941abd6...), no request or scoring change.
  Unit B0/mid-qwen36-35b-a3b__tasks_v2_pos__r0 and all B0-full units carry the old harness sha; every later
  unit carries the new one. The driver now waits for T'Ra before each unit (T'Ra and the driver share m4mini).
- B0-full ran from the Air (cloud only), umbrella 8c9d7ba3, before the switch.
- Not run: deepseek-coder-6.7b and qwen3-8b-nothink (not served anywhere); B0-small is qwen35-9b only, at its
  native 262K window (the 16K window is not emulated). ornith-1.0-9b replaced by prism-coder-7b in the G panel
  (LM Studio override loads it at 11,612 tokens). devstral: record which variant (Q8_0 GGUF vs MLX 4-bit) loaded.
- T'Ra m5_remote allow-list extended for the panel (backup config.yaml.bak-aether-b0-panel-20261007T110737Z).
- First unit miss: two_type_methods, uncoded "Function 'p.shiftx' expects 2 arguments, got 1" (uncoded diagnostic).
- ~20:4xZ: second harness switch, 9d685f30 -> 38a77cf9 (W1-11 references, W1-20 histogram, W1-22 traps,
  W1-23 scale), at the boundary after B0/mid-qwen36-35b-a3b__tasks_hard_v2__r0. Prompt fingerprint still
  4941abd6...; classification change (rc 0 on a task expecting non-zero = silent_wrong) touches no B0/P0 task (all
  expect rc 0). 38a77cf9 adds reference_solution to 9 v2_pos and 19 cs tasks, which changes those manifests'
  sha256 though no model-visible field; B0/B0-small/P0 therefore read tasks_frozen/ (byte-identical to 8c9d7ba3,
  sha256 5c9c6d73/fa092145/388d2a8d/c38ff47a, the values the first units recorded).
- Board order from then on: B0 -> B1 -> B0-small -> P0 -> S0 (G1 waits for B0 and B1, plan D3; B1's card arm is
  P0's traps part). B1 = medium+card in one report per unit (variants interleaved per task), tasks_traps
  2026-10-07-1, first-attempt expected stdout hidden. S0 = medium, mid-qwen36-35b-a3b, tasks_scale, 3 repeats,
  timeouts re-timed on m4mini (scale_retime_m4mini.log; commit the bumped tasks_scale.json with the results).
- Trap oracle on the frozen binary (Air, 38a77cf9 export): 24/24 traps fire (23 silent_wrong), references 24/24,
  python 24/24. m4mini re-run in B1/oracle_traps_m4mini.log.
- After B0: run tools/aether_failure_histogram.py results/b0_20261007/ --out-md .../failure_histogram.md.
- Hand-off 2 done 20:37Z. Unit B0/mid-qwen36-35b-a3b__tasks_hard_v2__r0 (FA 13/14, FX 14/14) ran on the 9d685f30
  harness; units from hard_nontoon r0 on run on 38a77cf9. m4mini trap oracle 24/24. Scale retime on m4mini:
  scale_int_loop 167->64 s, scale_json_rollup 627->263 s, version 2026-10-07-2; the clone is dirty with that
  manifest from then on (umbrella_dirty true in later units) until it is committed with the results.
- Likely compiler defect seen in B0 (qwen36-35b-a3b, hard_nontoon r0, hard2_expr_eval first attempt):
  "[SCOPE-001] identifier '__aether_slice_0' not in scope" -- a compiler-generated slice temporary leaking into a
  user-facing diagnostic. Not a model error; raise as a bug against 2026-10-06-1 (check whether L0 fixed it).
- devstral variant actually served (lms ps, 2026-10-07 22:2xZ): mistralai/devstral-small-2-2512 Q8_0 GGUF, context
  393216 (LM Studio's /api/v0 listing says "4bit mlx" for the key; lms ps is the loaded instance). Pin the Q8_0
  file hashes. After the 9d685f30 model release, m5 held only the panel model + the embedding model.
- devstral r0 triage (no artifact: full programs, coded errors, 0 infra): 10 first attempts fail FX-001
  "call to 'toon_parse_file' requires an fx block" (7 hard_v2 TOON tasks, 3 v2_pos), mostly fixed on repair.
  The __aether_slice_0 SCOPE-001 leak recurs (hard2_heap_sort), so two models hit it.
- 2026-10-08 04:55Z (owner request): T'Ra m5_remote max_concurrent_requests 4 -> 1 for the boards (backup
  config.yaml.bak-aether-b0-m5cap-20261008T045531Z). Units before this ran with m5 shared up to 4-wide.
- qwen36-27b hard_v2 r0 had 3 empty_output infra cases (04:38Z), while the Ornith-1.5 co-tenant was loaded again
  beside it on the 4-wide m5; the seeded re-run produced full programs, so transient serving, not an output cap.
- Panel extended (owner request): mid-bonsai2-27b-ternary = Ternary-Bonsai-2-27B-PQ2_0.gguf on the M2 mini (32 GB),
  prism llama.cpp fork b10743 llama-server, -c 262144 -np 1 -ctk/-ctv q8_0 --jinja, via T'Ra m2_remote. T'Ra's
  resource_scheduler_policy.json had no m2 node, so m2_remote was never schedulable (insufficient_node_pool); added
  m2 (24 GB budget, 4 GB reserve; backup ...bak-aether-m2node-20261008T045751Z). Runs as a second lane
  (LANE_TAG=local_m2, status_local_m2.log): B0, B1, P0 only (21 units), in parallel with the m5 lane.
- Every qwen36-27b empty_output infra case lines up with an LM Studio Metal out-of-memory crash on m5
  (kIOGPUCommandBufferCallbackErrorOutOfMemory; 27b crashes at 02:49, 03:10, 05:11, 05:42, 06:32Z on 10-08; 14 OOMs
  that day, Ornith crashed too). Cause: an Ornith-1.5 workload hits m5's LM Studio directly (406 requests on 10-08),
  outside T'Ra, so the cap of 1 cannot serialise it. The re-runs are therefore re-runs of crashes, not of
  output-cap truncations, and introduce no selection bias. Checked: the long (~20 min) 27b jobs all returned code.
- 2026-10-08 08:26Z (owner request): m5 parked in every pool of the LLM proxy (m4mini :8788, ai-routes.json; it had
  been small p0, bulk p0, large p2, deepseek-v4 p2 -- the source of the direct Ornith traffic), backup
  ai-routes.json.bak-park-m5-20261008T082617Z; Ornith 1.5 released from m5 via T'Ra /api/llm/release. claw3 takes
  the small slack. From then on m5 held only the panel model + the embedding model; 0 Ornith requests in the
  following minutes. Units before ~08:26Z on 10-08 ran with the co-tenant (crashes re-run, see above).
- m2 deliberately left in the proxy pools (owner, 2026-10-08): proxy overflow may share m2's single llama-server slot
  with the Bonsai lane. Expect slower Bonsai units, not different results; revisit if it becomes a problem.
- OUTPUT-CAP CHANGE (owner decision, applied 2026-10-09 06:01Z by the owner's run of b0_set_output_cap_96k.sh):
  max_output_tokens 24000 -> 96000 for all 6 local destinations; T'Ra queue_server.py patched to skip its silent 4x
  length retry for submitter aether_doc_bench and to return finish_reason (backup
  queue_server.py.bak-bench-nolenretry-20261008T141823Z; live since the 2026-10-08 16:55Z T'Ra restart).
  Lanes were paused 2026-10-08 14:18Z -> 2026-10-09 06:03Z for this. qwen35-9b v2_pos r0 and hard_v2 r0 (partial),
  which ran under the hidden 24K+96K-retry rule, are kept in B0/superseded_cap24k/ and re-run at 96K. Bonsai
  hard_nontoon r0's rle_roundtrip (the one m2 runaway) is re-run at 96K via rerun_nogen_cases. All other finished
  units never reached 24K tokens (no retry logged for them), so a 96K cap would not have changed them; they stand.
  First case after restart: max_tokens_sent 96000, finish_reason "stop". Harness sha in later units: bf07d70d...
- PROMPT CACHING on m2 (owner request, 2026-10-09 07:44Z): Bonsai's llama-server re-read the whole ~16K-token prompt
  on every request (~3 min at 80-110 tok/s) although the guide prefix was shared (LCP similarity 0.95+): the model is
  hybrid (recurrent), so the cache is reusable only from a saved context checkpoint, and the default
  --checkpoint-min-step 8192 left none near the guide's end. ~/prism-llama/run-server.sh on m2 now passes
  --checkpoint-min-step 1024 (backup run-server.sh.bak-ctxcp-20261009T074342Z; launchd com.mke.prism-llama-server
  restarted). Verified: a different task's prompt then read 600 tokens in 9.5 s. Serving-only change (prompt and
  sampling unchanged); Bonsai units from tasks_cs r0 on ran with it. m5's LM Studio (MLX) already reused the guide
  prefix (0% -> 94% prompt progress within a second), so the m5 lane needed no change.
- B0-small extended (owner request, 2026-10-09): Bonsai 2 27B ternary (7.2 GB) joins the 16K cohort beside
  qwen35-9b ("technically it is" a small model). 12 units (4 suites x 3 repeats, small guide 2026-09-05-1) queued
  as a follow-up m2 pass (screen b0m2small) that starts when the first m2 pass ends; it skips finished units.
  Like qwen35-9b it runs at its native 262K window, not an emulated 16K one.
- m5 OUTAGE 2026-10-09 07:42-13:58Z: T'Ra could not reach mbp5 (LAN "No route to host", Tailscale timeouts) although
  mbp5 stayed awake on AC (no sleep or reboot in pmset/last). Leading suspect: ProtonVPN, which runs as a full tunnel
  on mbp5 (default route via utun13); unconfirmed (no ProtonVPN log found). Jobs waited in T'Ra and resumed; the
  harness re-submitted after its 8900 s deadline. ~6 h lost, no data lost.
- 4-WIDE m5 (owner request, 2026-10-09 15:27Z): T'Ra m5_remote max_concurrent_requests 1 -> 4 (backup
  config.yaml.bak-aether-b0-m5cap4-20261009T152550Z; T'Ra is back in the gui/501 launchd domain). The driver runs up
  to MAXPAR=4 consecutive same-model units in parallel (one model loaded, several requests in flight); B1/B0-small/
  P0/S0 are now ordered model-major so a model's repeats batch together. From qwen35-9b r0 on, m5 units ran with up
  to 4 concurrent requests on one model (MLX batching: seeded samples need not be bit-identical to serial runs).
  The m2 lane stays serial (llama-server -np 1).
- BP (Python baseline) approved by the owner 2026-10-09: same models solve the 73 B0 tasks in Python (no guide,
  --docs none --skip-aether --python-baseline, frozen manifests, 3 repeats, seed 42+r, 2 repairs, 96K cap), as a
  reference for the Aether gap. Gated in run_b0_lane.sh (BP=1) until the harness runs model-written Python sandboxed
  (requested from the harness-lane session; today run_python_task executes it unsandboxed on m4mini). JavaScript
  baseline deferred (needs a node runner + references).
- THIRD harness switch, 2026-10-09 ~16:00Z: ~/b0-pscal fast-forwarded 38a77cf9 -> 994eea07 (sandboxed Python/Rust
  baseline runners; also L1 gitlink + Apple FM bridge, which the boards do not use). Prompt fingerprint 4941abd6
  unchanged; no task manifest, scale generator, oracle or fleet_env change; the only non-runner harness hunks mark a
  failed sandbox as infra. Units started after the switch carry the 994eea07 harness sha. Sandbox on m4mini:
  sandbox-exec, profile_sha256 e08bab1f..., baseline_sandbox_preflight() -> [] when called directly (it is not yet
  called by the harness itself; reported to the harness lane).
- 2026-10-09 ~16:45Z: ~/b0-pscal fast-forwarded 994eea07 -> 478f0bd6 (the baseline sandbox pre-flight is now
  actually called; --preflight-only on m4mini: "baseline_sandbox_preflight": "ok"). BP queued as follow-up passes
  (screens b0m5next: BP=1 MAXPAR=4 after the m5 driver ends; b0m2next: B0-small + BP for Bonsai after the m2 pass).
- ROLLING SLOTS (owner request, 2026-10-09 16:35Z): the driver now starts the next same-model unit as soon as any of
  the MAXPAR slots frees (drains before a model switch), instead of waiting for a whole batch; it adopts units whose
  harness is already running (across a driver hand-off) rather than starting them twice. B0 is now model-major
  (model, repeat, suite); the m5 lane runs with PANEL order qwen35-9b, prism-coder-7b, qwen36-35b-a3b, devstral,
  qwen36-27b, so the in-flight qwen35-9b r0 units were adopted and 9b r1/r2 roll in behind them, and the complete
  r0 board lands after prism r0. Hand-off: old main loop (pid 24788) frozen, its 4 unit subshells finished their units
  themselves; a cleanup screen retires it. Lesson: `screen -X quit` did NOT kill a session's waiting processes on
  m4mini; stale follow-up waiters had to be killed by pid (they would have started duplicate drivers).
- When committing these results: test_tracked_replay_reproduces_242_to_245_with_every_regression_waived globs every
  report under results/, so the B0 reports change its counts (the 4 B0-full reports alone make 274 -> 372 attempts).
  Update its expectation (or re-baseline instrument_check) in the same commit.
- NO_ANSWER CLASS (owner decision 2026-10-09; umbrella 20426ef7, deployed to m4mini ~21:40Z): a reply with no program
  because the model was still generating when its 96K cap (finish_reason length) or the request deadline ran out
  (T'Ra job running >= 90% of the deadline) is now a model failure, first-attempt class no_answer, repaired like any
  failure, never re-run as infra. Queued/never-started timeouts stay infra. Motivation: qwen35-9b on the hard suites
  (hard_nontoon r0: 2 cases of 2.5 h each with no answer). Units started before 20426ef7 scored such cases as infra and
  re-ran them; their re-runs from then on use the new rule. Prompt fingerprint unchanged.
- VERSION-MIX INCIDENT 2026-10-09 ~21:25-?Z (my error): fast-forwarding m4mini to 20426ef7 while harnesses ran made
  pre-deploy harness parents (old code in memory) spawn request workers with the new code; the new worker's job-id
  report was taken as the result, so every later request of those units failed at once as infra ("generation:
  ('http://...', '<job id>')") and left an orphaned T'Ra job. Affected: Bonsai cs r1 (9 cases, re-run by
  rerun_nogen on new code) and the 4 pre-deploy m5 units (qwen35-9b cs/hard_nontoon/hard_v2 r0, v2_pos r1). No score
  is wrong (all infra, all re-run). Orphans cancelled by ~/b0-tools/cancel_orphans.sh on m4mini (7 cancelled
  queued, 1 already running could not be). Lesson recorded in memory (harness-deploy-mid-run).
- COMPILER DEFECT CONFIRMED 2026-10-10: `obj.field[lo..hi]` fails with "[SCOPE-001] identifier '__aether_slice_0' not
  in scope" on 2026-09-23-1, 2026-10-06-1 and L1 2026-10-08-1 (built ~/aether-510b048). Write-up and minimal repro in
  findings/aether_slice_of_field_SCOPE-001.md. 5 of the first 805 cases; the 3 programs checked by hand are correct
  once the slice is worked around. Scores stand; flag hard2_expr_eval first-attempt failures with this diagnostic.
- M5 QUEUE DEATH SPIRAL 2026-10-09 ~23:00Z -> 2026-10-11 08:35Z (lane paused then): qwen35-9b on the hard suites
  generates until T'Ra kills the job at max_runtime (~2.5 h). With 4 units in flight, each new request waited ~2 h
  in T'Ra's queue; the harness deadline (8900 s) counts from SUBMIT, so it expired with the job queued or barely
  started -> infra provider_timeout (the no_answer rule needs >= 90% of the deadline RUNNING), and the harness does
  not cancel the T'Ra job on give-up, so every timed-out request left a job that later ran 2.5 h for nobody. Nothing
  completed for ~33 h. Stopped by freezing driver 61819 and the 4 unit shells, killing the re-runs (checkpoints kept:
  B0/low-qwen35-9b__*__r{0,1}.json.tmp) and cancelling 5 queued orphans; 4 running orphans die at T'Ra's limit.
  Harness defects to fix before any further T'Ra board: (1) start the request deadline when the job STARTS (separate
  queue-wait limit); (2) cancel the T'Ra job when the parent gives up. m5 lane left paused pending the owner's call.
- SCOPE CUT (owner decisions 2026-10-11 ~08:45Z): future boards run on OpenRouter (twin in results/b0_openrouter_20261010).
  m5 lane: only the two models OpenRouter lacks continue -- devstral-small-2 (r1, r2) and prism-coder-7b (r0-2) on B0,
  then B1, P0 and BP for both (~/b0-tools/start_m5_local_only.sh, screen b0m5s; starts when the orphaned 9b jobs clear).
  Dropped locally: qwen35-9b beyond its v2_pos r0 (partial checkpoints kept as B0/low-qwen35-9b__*.json.tmp), repeats
  1-2 of qwen36-35b-a3b and qwen36-27b, B0-small, S0. m2 lane: local Bonsai finishes B1 and stops
  (~/b0-tools/stop_m2_after_b1.sh); its P0, B0-small and BP are not run. The follow-up passes (b0m5next, b0m2next)
  were removed. After the m5 lane: restore m5 in the proxy pools (ai-routes.json.bak-park-m5-20261008T082617Z) and
  decide whether T'Ra's m5 cap stays at 4.
- Hosted vs local, B0 first attempt of 73: qwen36-27b 70/72/72 vs local r0 70; qwen36-35b-a3b 68/72/69 vs local r0 68;
  Bonsai hosted (Darkbloom int4, 32K cap) 57/59 vs local ternary PQ2_0 67/67/67 (FX 68/66 vs 73/73/72).

## 2026-10-11 10:05-10:18Z -- m5 local-only lane: model-load storm, seven units rejected, driver now warms a model first

The local-only lane (devstral-small-2, prism-coder-7b; B0 B1 P0 BP; MAXPAR=4) started at 10:05Z when the last
orphaned qwen35-9b jobs left m5. Its four first units each ran the harness's model check at once, so four
load requests for devstral reached the server together. The server refused them ("insufficient system
resources ... requires approximately 62.40 GB", then "Model is unloaded" for requests already streaming); the
scheduler returned those requests as done with empty content. Seven units ended with "REJECT zero variants"
(preflight skipped the destination; nothing was scored, no checkpoint existed, nothing was lost). An eighth
unit (cs r2) was killed when I replaced the driver; it had no checkpoint either.

Fix (driver sha256 4686027c...): the first unit on a destination runs alone until its checkpoint exists
(written once the harness's model check has passed), then the remaining slots open. The starter
(`start_m5_local_only_2.sh` on the bench host) runs the lane twice, so the second pass re-runs any unit the
first rejected. At 10:17:58Z devstral answered and four units were running. The scheduler reporting an
unloaded-mid-stream request as a successful empty reply is a third T'Ra-path weakness to fix before the next
T'Ra board (with: deadline counts queue time; no cancel on give-up).

## 2026-10-11 ~10:50Z -- owner: one complete pass is enough for local Bonsai on m2

B1 repeat 0 on m2 took 21.6 h for its 48 cases (24 traps x medium and card), plus re-runs of two cases that
ran past the 8,900 s request limit; repeats 1 and 2 would have taken about two more days. Owner: "one complete
run through is sounding good for Bonsai on m2. Lets plan on that." The stop watcher now ends the lane as soon
as B1 repeat 0 has its report (`stop_m2_after_b1_r0.sh` on the bench host); a repeat-1 checkpoint started in
the gap is moved to aborted_m4mini/. Local Bonsai therefore has B0 x 3 repeats and B1 x 1 repeat; its B1 has
no repeat spread and must be labelled n=1 in the summary.

## 2026-10-11 11:11-12:46Z -- prism-coder-7b at four requests in flight: window overflow, every unit set aside

devstral-small-2 finished B0 r1 and r2 at four in flight without trouble (v2_pos r2 31/35, hard_v2 3/14 and
3/14, cs 11/19 and 11/19 first attempt -- in line with r0; one unit, v2_pos r1, still owed after its load-storm
rejection). The lane then moved to prism-coder-7b with four slots. That model's window is 32,768 tokens and the
server shares it between the requests in flight; a medium-guide prompt is ~13-14K tokens, so with more than
two requests active the server answered "Context size has been exceeded" (644 times in 95 minutes, against
192 completed replies). The scheduler passed each of those back as a successful empty reply; the harness
recorded them as infrastructure failures ("empty_output"), re-ran them three rounds, and parked or rejected the
units. One prism unit did reach [done] (hard_nontoon r0, 0/5) but under the same conditions.

Decision: none of the prism output from this window is usable, including the unit that "completed". All of it
is in aborted_m4mini/prism_par4_context_exceeded_20261011/ and nothing from it goes into the summary. Restart
(`start_m5_local_only_3.sh`): prism with ONE request in flight, then devstral with four, then one more pass of
each for rejected units. Lesson for the README's "before the next board": a parallel-slot setting must be
checked against prompt size x slots for every model, not just against the host's request cap; and the
scheduler must return a provider 500 as a failure, not as an empty success.

## 2026-10-11 16:39Z -- local Bonsai B1 r0 closed by hand; m2 released

Asked for the third time whether m2 was free, I found the re-run tool on a second internal round of the same two
cases. Cause, from the case records: in each case an attempt ran past the 8,900 s limit and was scored
no_answer (model failure, as decided); the harness then sent the repair prompt, but the timed-out request was
still generating on m2's single slot (no cancel on give-up), so the repair request sat in the queue behind it
and used up its own deadline there (deadline counts queue time) -> "provider request exceeded 8900 seconds",
an infrastructure failure. Every re-run reproduces that; it could not converge.

Closed as follows. All 48 first attempts are measured: medium 14/24 first attempt (trap_shift_wide = no_answer),
card 11/24 (trap_life_continue = wrong output). After repair: medium 21/24, card 18/24, each a LOWER BOUND by
at most one case, because the last repair attempt of trap_shift_wide (medium) and trap_life_continue (card)
was never measured. The report is B1/infra_pending/mid-bonsai2-27b-ternary__tasks_traps__r0.json (copy of the
checkpoint, which also stays in place); the summary must carry it with that flag and n=1 repeat. Driver, stop
watcher and re-run processes killed; DONE_local_m2 written. The orphaned request (82 min in) could not be
cancelled through the scheduler, so I restarted m2's model server (launchd agent) to abort it: m2 is idle with
no queued or running jobs, Bonsai still loaded.

## 2026-10-11 16:45Z -- prism-coder-7b scores triaged (single slot): low, and it is the model

At one request in flight prism-coder-7b finishes every request normally (finish "stop", no empty replies, no
context errors), and scores B0 first attempt 2/35 and 3/35 (simple), 0/14 and 0/14 (hard), 0/5, 1/19 and 1/19
(cs). Triage: generated 35/35; 33 errors, 30 of them SYN-001 on tokens like an em dash, ':' or a backtick. The
raw replies are mostly not programs: one line of meta-commentary ("hand off to local Aether compiler", "32K
context limit -- write a complete program directly."), or a numbered plan ending "paste the raw Aether source
code here" with no code; when it does write a program it is often correct. One message is sent, no system
prompt is added by the server, and I found no preset for the model. So this is how this fine-tune answers a
~15K-token guide prompt, not a serving fault; the numbers stand as measured. Worth one line in the README: it
is an agent-tuned 7B that narrates the task instead of doing it.

## 2026-10-11 17:41Z -- m5 back in the proxy's small pool (owner)

Owner: prism-coder is small and slow, m5 has RAM and compute to spare, so m5 goes back into the small pool
while the lane finishes. Restored the m5 upstream in the small routes of `local`, `default`, `local-native` and
in `default-fast` (backup `ai-routes.json.bak-unpark-m5-small-20261011T174147Z`); it stays parked in large,
bulk and deepseek-v4. From here the remaining m5 units (prism B0 r2, B1, P0, BP; devstral v2_pos r1, B1, P0,
BP) share the host with pool traffic: timings are not comparable with earlier units, scores should be
unaffected (a different model has its own context window). To watch: the scheduler unloads idle models when
it switches the benchmark model, and pool requests reach the server directly, not through the scheduler.

## 2026-10-11 18:48-19:50Z -- devstral served from a different weight file; Q8_0 set frozen, 4-bit set split off

After the evening re-plan the lane ran devstral again (18:48Z). Checking the first model switch for the pool
interaction, `lms ps` on m5 showed devstral at 14.12 GB: the MLX 4-bit build (safetensors, quantization 4bit).
The B0 units of 2026-10-07 (panel_weights.json) and of this morning (lms ps at 10:17Z: 25.93 GB = the Q8_0
GGUF plus its vision adapter) ran on the Q8_0 GGUF. The model name has two variants on that host and the
server's "selected variant" is now the 4-bit one; a plain load by name (what the scheduler sends) gets it. Why
the selection differs from this morning is not established (suspects: the failed 62 GB Q8_0 loads during the
load storm, or the extra models now resident since m5 rejoined the pool). The `lms` CLI cannot name a variant,
and the scheduler's allow-list has only the plain name, so I could not pin Q8_0 without editing and restarting
the scheduler -- left for the owner.

Consequences:
- Q8_0 devstral = B0 only: r0 (4 units), r1 (hard_v2, hard_nontoon, cs), r2 (all 4). 11 of 12 units; v2_pos r1
  is missing and nothing else will be added to it tonight. No B1/P0/BP exists for Q8_0 devstral.
- Every devstral unit from 18:48Z on is 4-bit and was moved out of the board: B0 v2_pos r1 (27/35 first
  attempt, against 31/35 on Q8_0), B1 r0-r2 (medium 9, 10, 12 of 24; card 3, 4, 2 of 24), P0 v2_pos r0
  (17/35). Copies are in aborted_m4mini/devstral_mlx4bit_20261011/.
- So that the night is not wasted and devstral has ONE self-consistent set, those units seed a separate
  results directory, results/b0_devstral_mlx4bit_20261011/ (same frozen tasks, binary, guides, seeds), and the
  lane completes B0, B1, P0 and BP x 3 repeats there on the 4-bit file (`m5_night.sh` on the bench host, after
  prism-coder's one-repeat boards). `~/b0-tools/devstral_variant_guard.sh` on this Mac checks every 3 minutes
  that the loaded devstral is the MLX 4-bit file and freezes that lane if it is not (log beside it); any unit
  overlapping a mismatch is to be discarded.
- Before the next board: pin the weight FILE, not the model name, for every local destination, and have the
  lane record the loaded file's identity per unit. panel_weights.json recorded what was on disk and loaded at
  the time; nothing re-checked it per unit.

## 2026-10-12 04:40Z -- prism-coder-7b closed out; devstral 4-bit set started

prism-coder-7b, one request in flight, one repeat (owner's one-pass rule): B1 medium 1/24 first attempt (3
after repair), card 0/24 (0); P0 card 1/35 (3) on the simple suite and 0/19 (0) on cs. The traps unit took 4.6 h.
Its Python baseline (BP) was cut by me at 04:40Z: 7 of 35 cases in 2h50m (about 24 minutes a case), none
correct, 2 ran to the length limit -- it would have held m5 for about 20 more hours. The partial checkpoint is
in aborted_m4mini/prism_bp_cut_20261012/; prism has NO Python baseline in the summary. prism-coder's B0 is 3
repeats (2, 3, 5 of 35 simple; 0/14 hard; 0/5; 1/19 cs).
The devstral MLX 4-bit set (results/b0_devstral_mlx4bit_20261011/) started at 04:40Z, four in flight.

## 2026-10-12 06:32Z -- devstral MLX 4-bit set complete; m5 released

results/b0_devstral_mlx4bit_20261011/: B0 12 units, B1 3, P0 6, BP 12 (the Python baseline ran all four B0
suites), 0 rejected, 0 parked. The variant guard logged the MLX 4-bit file (safetensors, 14,120,998,772 bytes)
at every check from 04:40Z to 06:29Z. First attempt, repeats 0/1/2:
  B0 medium  simple 28/27/28 of 35, hard_v2 5/4/5 of 14, hard_nontoon 1/1/0 of 5, cs 13/12/14 of 19
  B1 traps   medium 9/10/12 of 24, card 3/4/2 of 24
  P0 card    simple 17/20/19 of 35, cs 9/7/7 of 19
  BP python  simple 22/22/22 of 35, hard_v2 1/1/1 of 14, hard_nontoon 2/2/1 of 5, cs 18/18/19 of 19
Against the Q8_0 B0 (simple 31/-/31, hard_v2 3/3/3, hard_nontoon 0/1/0, cs 12/11/11) the 4-bit build is
level overall (repeat 0: 47 vs 46 of 73). m5 has no benchmark work queued; devstral unloaded by hand.
