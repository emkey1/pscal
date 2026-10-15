# Fine-tuning runs: registry, run configs, driver

One driver, `tools/aether_train_run.py`, takes a run from dataset to scores. It
replaces the rig-only scripts in `../claw2_snapshot_20261015/` (kept as a record)
and implements recommendations 3, 4 and 7 of the training-framework assessment.

| file | what |
|---|---|
| `families.json` | per model family: chat markers for response masking, LoRA targets, training image, the chat-template switch (thinking off), whether to serve with the base tokenizer, vLLM settings, reasoning-leak prefixes. Each entry cites the run that established it. |
| `runs/<tag>.json` | one run: family, base model path on the host, host short name, precision, LoRA recipe, dataset id and compiler version, scoring plan |
| `../datasets/<id>/` | the dataset a run trains on, with `DATASET_MANIFEST.json` (file sha256s, umbrella commit, compiler version and sha256, record counts) |
| `../training_runs/<tag>/` | generated scripts and the records fetched back from the host (no weights) |

## Running one

```
R=Tests/aether_specialization/training/runs/qwen38-27b-l1-pilot.json
python3 tools/aether_train_run.py $R plan       # everything it would do; changes nothing
python3 tools/aether_train_run.py $R dataset    # build + gate here, against the pinned compiler
python3 tools/aether_train_run.py $R push       # dataset, trainer, harness subset, scripts -> host; checks model/image
python3 tools/aether_train_run.py $R train      # detached training container on the host
python3 tools/aether_train_run.py $R status     # from the host's own files, any time
python3 tools/aether_train_run.py $R score      # detached tmux: serve, smoke, score base and tuned
python3 tools/aether_train_run.py $R fetch      # records back here (pack reports before committing)
python3 tools/aether_train_run.py $R release    # stop this run's serving container and tmux
```

Every stage can be re-run: training resumes from its last intact checkpoint,
scoring resumes each suite with the harness's `--resume`, and a finished stage
is skipped. Nothing long runs on the machine that launched it.

The ssh user comes from the private fleet overlay (`AETHER_TRAIN_SSH_USER`,
default `claw`); runs name hosts by their short names only.

## Guards

- **One GPU job per host, and only with room.** `train` and `score` refuse while
  another `aether-train-*` / `aether-serve-*` container is up or the host has less
  than `min_available_gb` available. The Sparks are shared (ComfyUI, MiMo
  serving); freeing one is the owner's call, never the driver's.
- **Markers.** The trainer stops before step 1 if the family's markers leave more
  than a tenth of the rows with no trainable token (a wrong pair masks
  everything; it happened twice).
- **Template.** Training text is rendered with the family's
  `chat_template_kwargs`, the same ones the served requests send. Qwen3.8's
  template adds a "Reasoning effort is set to xhigh" system turn otherwise.
- **Export smoke.** Each scored variant first runs three easy tasks with no guide;
  empty output or a reasoning trace stops that variant before any suite runs.
- **Grader pin.** The scoring script checks the grader's language version
  against the dataset's and passes its sha256 to the harness.
- **Records.** `trainer` writes seed, data-order seed, early-stopping settings,
  dataset sha256s, library versions and the first rendered training text to
  `train/run_metadata.json`; `train.sh` records the image id and its libraries.

## Scoring plan

The default plan scores the untrained base and the fine-tune, both with no
guide, on the four B0 suites (tasks_v2_pos, tasks_hard_v2, tasks_hard_nontoon,
tasks_cs: 73 tasks), T=0, seed 42, two repair rounds, generous output budget.
Add `"base:medium"` to `score.variants` to also measure the base with the medium
guide (the bar a no-guide fine-tune has to reach). Repeats default to 1; a
hyperparameter conclusion needs 3 seeds (assessment item 11).

## Adding a family

Probe the template first (`../claw2_snapshot_20261015/rig_dir/tmpl_probe.py`, or
render one conversation with and without the thinking switch), confirm the
markers appear verbatim in the rendered text, then add the entry with its
evidence. Image choice: a family whose `model_type` the image's transformers
does not know cannot train there.
