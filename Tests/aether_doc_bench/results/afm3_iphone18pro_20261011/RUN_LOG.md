# Running notes: Apple on-device model on an iPhone 18 Pro (fold into README.md, then delete)

- Owner request 2026-10-11: benchmark the A20. Device: iPhone 18 Pro (iPhone19,2), 12 GB, iOS 27.0 (24A8428),
  Developer Mode on, wired to the MacBook Air; not on the tailnet, so the harness runs on the Air and reaches the
  bridge over the CoreDevice USB tunnel (the Air must stay awake and the phone unlocked, in the foreground, on power).
- Bridge: tools/apple_fm_bridge built at umbrella 20426ef7 with Xcode 27.0 (27A266a), Release, development-signed;
  installed and launched with devicectl. /v1/models reports variant "AFM 3 Core" for apple-on-device -- the same
  variant as the M4 iPad Pro run (results/afm3_20261008), not coreAdvanced3.
- At first launch the model was unavailable (modelNotReady, "enable Apple Intelligence in Settings"); waiting for it.
- Board: identical to afm3_20261008/board_card: card-v1 (sha256 d1c5606c..., stamp 2026-10-07-1), frozen binary
  addfeb8d... (2026-10-06-1), suites tasks_v2_pos/hard_v2/hard_nontoon/cs/traps (same manifests, e.g. v2_pos
  6208989e...), 3 repeats, seed 42+r, 2 repairs, max_output 1500, context 4096. Harness 20426ef7 (the iPad run used
  38a77cf9): same prompt templates; the only scoring difference is that a reply cut off at the output cap with no
  program is now no_answer instead of infra.
- Destination (gitignored): Tests/aether_doc_bench/destinations.apple_fm_iphone.local.json. Token read from the app's
  preferences over the cable into a private file outside the repo.
- The model became available only after Siri's language was set to US English and the waitlist joined (UK English
  left it at modelNotReady; iOS 27 Settings shows just "Siri", no Apple Intelligence section, on both devices).
- With that, the phone serves variant "AFM 3 Core Advanced" with an 8,192-token context -- the coreAdvanced3 variant
  and twice the window of the M4 iPad Pro's "AFM 3 Core" (4,096). So this is a new condition, not a hardware twin of
  afm3_20261008: destination apple-ondevice-afm3adv-iphone18pro, prompt_context_limit 8192, max_output_tokens 2500
  (iPad: 4096 / 1500). Everything else as the iPad board.
- Smoke (smoke/afm3adv__smoke.json): 4/4 generated, 0 infra, 0/4 exact; failures are the model's (print instead of
  println, `return`), the same habits the iPad run recorded. Board started 2026-10-11 (screen a20card on the Air,
  caffeinate -is).
- IS "CORE ADVANCED" JUST "CORE" WITH A BIGGER WINDOW? Checked 2026-10-11 on the first 66-67 tasks_v2_pos cases shared
  with the iPad board (identical prompt sha256 and seed in every case): 0 identical first-attempt programs, median
  SequenceMatcher similarity 0.42; first-attempt passes: both 1, iPad only 5, phone only 4. Control: on the phone the
  smoke run and the board run gave byte-identical programs for the same prompt+seed (3/3), so the model is
  deterministic and the difference is not sampling noise. Both devices report the same prompt token counts (same
  tokenizer). Habits differ (programs using println 26 vs 38, print( 37 vs 22, @pure 32 vs 19, const 41 vs 29,
  return 15 vs 17; iPad vs phone). hello_fx: phone gives the same 3-line program for seeds 42 and 43, iPad a longer
  one that varies by seed. Conclusion: different weights (a sampler difference between OS builds 24A437/24A8428
  cannot be fully excluded). The beta acceptance arrived during the run; variant_log.txt shows the variant unchanged.

## 2026-10-11 10:08-10:25Z -- owner: simple items only; v6, then a 3,500- and a 4,000-token guide

Owner, after the early card numbers: "After this run, lets only test on the simple items. If we can't get
those near perfect, it's only marginally interesting how the others do. Please test with the v6 guide and
then draft both a 3500 and 4000 token guide and test each."

- The card board keeps running its five suites (it was "this run"). Everything after it is tasks_v2_pos only
  (35 tasks x 3 repeats, seeds 42+r, 2 repairs, frozen binary), one board at a time on the phone:
  v6 -> g3500 -> g4000 (`guide_boards/run.sh`, `guide_boards/queue.sh`), then the Python baseline
  (`board_python/`, trimmed to tasks_v2_pos; `board_python/queue.sh`).
- `guides/v6_table_plus.md` is the iPad run's v6, byte-identical (sha256 ec3b7923...).
- `guides/g3500.md` (10,449 chars, ~3,460 device tokens, sha256 8814f6d2...) and `guides/g4000.md`
  (12,138 chars, ~4,020 device tokens, sha256 6a96f3f3...) are new drafts, proposals only; nothing in the
  aether repo was edited. Device tokens are estimated as chars / 3.02 (the ratio measured for the card and
  v6 on this tokenizer); the reports carry the measured prompt tokens.
- What the drafts change relative to v6, from the phone's first-attempt failures on the card board (105
  cases: 10 pass, 22 silent-wrong, 73 coded; first errors `return` x22, `import` x9, `print`/`println`
  outside fx x7, a const array, `+=`, `if c then a else b`, `not`, invented `parse_json`/`MemoryStream`):
  1. A complete program and five rules FIRST (v7 on the iPad showed position matters): only declarations at
     top level, `fn main() -> Void ... ret;`, `ret` never `return`, one `println` per output line with the
     spaces in the text, ranges exclude their end, print only what is asked. The card's examples print with
     `print(i, " ")`, and the phone copied that: most of its silent-wrong answers are missing newlines or
     separators, or a top-level `fx` with no `main`.
  2. Table rows for `use "file";` (not `import`), `!x` (not `not x`), `if c { a } else { b }` as a value.
  3. Sections the card does not have and a third of the simple tasks need: contracts (`@pre`/`@post`,
     `result`, `result.0`), `value:0:2` real formatting and Int-to-Real arithmetic, `div`/`mod`, digit
     separators, modules, reading JSON through the TOON calls (file and has-key guard), memory streams,
     extension methods, scanning an array with `length` and an index.
  4. g4000 = g3500 + a loop-with-break program that prints as it goes, a second JSON program (inline text,
     kind probes) and a five-line checklist at the very end (nearest the task).
- Every code block in both drafts runs on the frozen binary and prints exactly the text block that follows it
  (`guides/verify_guides.py`; 9 and 11 programs). `tools/audit_guides.py --strict` (aether 6659679) finds no
  benchmark identifier, module, file or export name in either.
- CAVEAT for the README: the drafts were written after reading this suite's task list and the phone's
  failures on it, so tasks_v2_pos is a development set for them, not a held-out one. The examples use
  different programs, names and data than the tasks, but the feature coverage was chosen to match. A gain
  here needs confirming on tasks the drafts were not written against before it is quoted as a guide effect.

## 2026-10-11 10:24Z -- phone unplugged mid-board; one bridge-aware queue from here

At about 10:24Z the phone left USB (devicectl: "available (paired)", bridge hostname no longer resolves). The
card board was in tasks_hard_v2: 25 cases measured, 17 infrastructure failures (no headline for that suite
until they are re-run); the card driver stopped as designed. The first v6 attempt failed its model check and
wrote a report with no cases (kept in `guide_boards/rejected/`, not a result).

Per the owner's "simple items only", the card board's remaining suites (hard_nontoon, cs, traps) are NOT run
on the phone; its simple-suite result (105 cases) is complete. `phone_queue.sh` (log `phone_queue.log`)
replaces the earlier queues: v6 -> g3500 -> g4000 -> Python baseline, all on tasks_v2_pos, then a re-run of
the 17 unmeasured hard_v2 card cases so that suite is whole. Before each step, and after any step that ends
early, it waits until the bridge answers; a zero-case report is set aside and the step starts again, a partial
one is continued with --resume.

## 2026-10-11 10:39Z -- bridge reached over the wireless network from here on

After the owner replugged the phone, devicectl reported it connected over the local network, and the
CoreDevice tunnel hostname resolved for one request and then stopped again. The owner pointed at the phone's
wireless hostname, which answers in ~0.14 s. From v6 onward the destination's base_url and the queue's
reachability check use that host (kept in the untracked destinations file and in
`~/b0-tools/iphone26/bridge_host`; the previous destinations file is backed up beside it). Same bridge app,
same model, same request shape; only the transport differs from the card board (USB tunnel).

## 2026-10-11 10:47-11:37Z -- Homebrew replaced Python under the running v6 board

At 10:47Z a Homebrew upgrade on this Mac removed python@3.14 3.14.7 (the interpreter the v6 harness was
running on), installed 3.14.8, and repointed `python3` at a new 3.15.0. The harness parent kept running from
memory, but every model request runs in a freshly spawned worker, and each worker died at import
(`ModuleNotFoundError: No module named 'pkgutil'`); the parent waited out the 600 s request limit each time.
42 cases were measured before the upgrade; 4 more were recorded as infrastructure failures (not scored) over
the next 50 minutes. Noticed at 11:36Z when the owner asked for a status; I had no watcher on per-case pace.

Fix: killed the harness, named the interpreter in the phone scripts (`PYTHON`, default
/opt/homebrew/bin/python3.14, now 3.14.8) and restarted the queue; --resume kept the 42 measured cases and
re-ran the 4 failed ones. The card board (ended 10:24Z) ran wholly on 3.14.7; v6 is split 3.14.7 / 3.14.8;
later boards are 3.14.8. The interpreter does not touch prompts or scoring.

## 2026-10-11 13:00Z -- v6, g3500, g4000 on the simple suite (105 cases each); a third draft, g3700

| guide | measured prompt tokens (median) | first attempt | after repair | silent-wrong | coded | uncoded |
|---|---|---|---|---|---|---|
| card | 2,017 | 10 | 11 | 22 | 73 | 0 |
| v6 | 2,301 | 4 | 7 | 23 | 75 | 3 |
| g3500 | 3,451 | 28 | 31 | 19 | 50 | 8 |
| g4000 | 3,948 | 19 | 26 | 36 | 45 | 5 |

(`guide_boards/comparison.md` has the per-task grid and first errors.) g3500 vs g4000 case by case: 17 pass
only under g3500, 8 only under g4000, 11 under both (two-sided sign test p = 0.11): not a reliable difference,
and the three repeats of a task are nearly the same program at T 0.2, so the effective sample is closer to 35
tasks than 105 cases. What the longer draft changed is the failure mix: fewer compile errors (58 -> 50), far
more programs that run and print the wrong text (19 -> 36). 30 of those 36 are near misses in the shape of
the output: several requested lines printed on one line with spaces (the guides' own examples print several
values per `println`), a missing space, an inclusive range written `1..n`.

g3700 (`guides/g3700.md`, 11,341 chars, ~3,755 device tokens, sha256 cbd00fc0...) = g3500 with: the examples
rewritten to print one value per line; "values asked for on separate lines get one println each"; `ret` "leaves
the function at once; it never prints and never collects values" (the model writes `ret i * i;` inside a loop
as if it emitted a value); "an array is a let inside a function, never a const" (9 first errors under g4000);
a one-line inline `toon_parse` form; and g4000's closing checklist. All 9 programs verified on the frozen
binary; strict audit clean. Queued after the Python baseline (`phone_queue2.sh`). Same development-set caveat.

## 2026-10-11 13:09Z -- Python baseline on the simple suite (`board_python/`)

Same 35 task prompts, no guide, the model asked for Python; 105 cases, 0 infrastructure failures, 8.5 minutes.
First attempt 41 (39%, CI 30-49%), after repair 47, silent-wrong 25, errors 39.

11 of the 35 tasks cannot be done in Python as posed (three import an Aether module file, five use the TOON
calls and expect `true` not `True`, one the memory-stream builtins, two ask for `@pre`/`@post`). Split:

| | 24 language-neutral tasks (72 cases) FA / FX | 11 Aether-specific tasks (33 cases) FA / FX |
|---|---|---|
| Python, no guide | 38 / 44 | 3 / 3 |
| Aether, card | 10 / 11 | 0 / 0 |
| Aether, v6 | 4 / 7 | 0 / 0 |
| Aether, g3500 | 16 / 19 | 12 / 12 |
| Aether, g4000 | 13 / 18 | 6 / 8 |

So on tasks both languages can express, this model writes a correct Python program first time about half the
time (53%) and a correct Aether program 22% of the time with the best guide so far. The Python figure is the
practical ceiling for "near perfect" on this model: several Python misses are the same output-shape slips
(`True` for `true`, values on one line) that sink its Aether answers.

## 2026-10-11 13:53Z -- g3700 result; phone queue complete

g3700 (105 cases, median prompt 3,713 tokens): first attempt 21 (20%, CI 13-29%), after repair 33,
silent-wrong 30, coded 48, uncoded 6. Against g3500 case by case: 14 pass only under g3500, 7 only under
g3700, 14 under both (sign test p = 0.19). Neutral tasks 12/72, Aether-specific 9/33.

The three long drafts sit together (first attempt 28 / 19 / 21, after repair 31 / 26 / 33) and well above the
card and v6 (10 / 4). Between drafts, whole tasks flip (tuple_order_post PPP eee eee; array_minmax eee eee PPP;
contract_normalize ueu wew PPP; recursion_factorial PPP www eee): 15 of the 35 tasks pass first time under at
least one of the three drafts, but no single draft holds all 15, and 20 tasks never pass first time under any. That is this model's sensitivity to
wording, not a guide effect I can steer with one more edit; ranking the three drafts would need more seeds or
tasks. The one-value-per-line rewrite did not remove the output-shape misses (silent-wrong 30).

Card board, tasks_hard_v2: the 17 unmeasured cases were re-run at 13:09-13:18Z; the suite is whole: 42 cases,
0 first attempt, 0 after repair. `phone_queue.log` ends QUEUE2 COMPLETE; nothing is running on the phone.
