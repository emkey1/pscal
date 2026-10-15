# Training and evaluation drivers copied from the training rig (snapshot 2026-10-15)

Every recent fine-tuning round (cs-aug3 through cs-aug20, the precision grids, the
early-stopping test) was driven by scripts that lived only on the training rig, not
in this repository. This directory is a read-only snapshot of them, taken
2026-10-15, so that those rounds can be reproduced and the scripts reviewed. It is a
record, not the maintained pipeline. Its replacement, built from what is here,
is `tools/aether_train_run.py` with the family registry and run configs in
`../training/` (training-framework assessment, recommendation 3).

| directory | what | source on the rig |
|---|---|---|
| `rig_dir/` | the working directory's drivers: `train_any.sh`, `serve_any.sh`, `run_eval_v2.sh` / `run_eval_v2_repair.sh` / `run_eval_any.sh`, the `retrain_queue_*.sh` round queues, the Mistral and Qwen3-8B sweeps, trace generation, template and masking probes, the image's Dockerfile | the training directory (`aether-qwen-coder-30b-unsloth`) |
| `rig_scripts/` | the rig's copy of the trainer (`unsloth_qwen_coder_30b_sft.py`; differs from `tools/` by 4 lines) and the 8-bit export smoke tests | its `scripts/` |
| `home/` | home-directory drivers: the trained/guided sweeps, the cs-aug4 and cs-aug20 eval queues, the qwen3.6 base chain and serving helpers, the bf16 retrain queue, `sweep_leaderboard.py`, `probe_markers.py`, `render_template.py` | the home directory and the shared storage |
| `ministral14b/` | an earlier, separate LoRA project (configs, scripts, sources) | its own directory |

Edits made in copying, so that the files can sit in a public repository: the rig
user's home directory is written `$HOME`, tailnet host names are reduced to their
short names, and the scheduler's address is read from `TRA_QUEUE_URL` (set in the
private fleet overlay). Nothing else was changed; backup files and caches were
left out. No credentials were found in the files (the `api_key` values in them are
placeholders such as `"x"`).

The built datasets of those rounds are in `../out_cs_aug18/`, `../out_cs_aug19/` and
`../out_cs_aug20/`, beside the older `out_cs_aug*` sets, with one change: three
environment-dependent records are removed because their expected output contains
the build machine's environment (`PATH` with home-directory and `~/.claude` paths,
user and host details). The byte-exact originals are in the private results
archive. `DATASETS_REDACTED.json` lists, per file, the original and public sha256
and the records dropped; `aether_raw_corpus.json` was re-serialised after the
drop, and `aether_training_mix.json` still states the original record counts
(cs-aug20: 726 instruction records; the public file has 723).
