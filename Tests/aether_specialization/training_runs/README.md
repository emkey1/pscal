# Aether training-run recipes

Durable, committed records of every Aether specialization fine-tune: the EXACT
reproducible launch command, image tag, base model, data dir, every hyperparameter,
and the export path. One Markdown file per run (`<run-name>.md`).

## Why this exists

Training containers get pruned. When the `q36-sdpa` (q36-aether) container was pruned,
its exact launch command was lost — only the trainer-written `run_metadata.json`
survived, which omits the image tag and the fragile Blackwell runtime flags. That must
not happen again.

## Standing practice — EVERY training launch MUST

1. **Write the exact command to a committed recipe file here** (`training_runs/<run-name>.md`)
   — at or immediately after launch — and commit + push it (umbrella repo, branch
   `AetherLang`). Capture it from `docker inspect <container>` (Image, Args, Mounts,
   HostConfig ulimits/ipc/shm, Env) so it is the *actual* command, not an approximation.
2. **Write the exact command to persistent on-rig storage too**, alongside the output
   dir, so it survives container pruning independent of this repo:
   `/storage/<run-name>/LAUNCH_COMMAND.md`. The trainer also writes
   `runs/sft-<run-name>/run_metadata.json` (hyperparameters) — good, but it does NOT
   record the image tag or docker/runtime flags, so the `LAUNCH_COMMAND.md` is required
   in addition.

A run is not "launched" until both copies exist.

## The established rig + flow

- **Rig:** claw2 GB10, workspace `~/training/aether-qwen-coder-30b-unsloth`.
- **Wrapper:** `train_any.sh <tag> <model> <r> <alpha> <data> <ep>` (defaults
  `r=32 alpha=64 data=data_v78 ep=3`; image default `aether-unsloth-qwen3-coder-30b:q3`).
  Override LoRA r/alpha explicitly when the recipe differs from the defaults.
- **Non-Qwen models** (e.g. Mistral) REQUIRE chat-turn markers via env:
  `INSTR_PART="[INST]" RESP_PART="[/INST]" ./train_any.sh …`. Forgetting them masks all
  labels → `Removed all N samples → num_samples=0` crash at ~4min. Verify the launch via
  `docker logs`: the `Filter … N/N` line must keep all samples (not "Removed all").
- **Corpus sync:** rsync a pre-built `Tests/aether_specialization/out_*/` dir's
  `aether_*` files into a new `data_<name>/` on claw2. Use an ABSOLUTE remote path
  (`rsync host:relpath` mangles the target).
- **Detach:** `docker run -d` (what `train_any.sh` uses) is daemon-managed and survives
  SSH drops / session death — no tmux needed.

## Runs documented here

| File | Run | Model | Status |
|---|---|---|---|
| `mistral24b-cs-aug2-builtins.md` | `mistral24b-cs-aug2-builtins` | Mistral-Small-24B | launched 2026-06-28 (exact command captured) |
| `q36-aether-RECONSTRUCTED.md` | `q36-sdpa` / q36-aether | Qwen3.6-35B-A3B hybrid MoE | DEFERRED; PARTIAL recipe reconstructed from surviving metadata (exact command lost to pruning) |
| `cs-aug18-precision-grid.md` | 6 tags, see file | Qwen3-8B + Qwen3.5-9B, x{4bit,8bit,16bit} | launched 2026-07-19 (queue script, exact command captured); live status in `cs-aug18-precision-grid-status.md` |
