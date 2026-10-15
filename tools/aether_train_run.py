#!/usr/bin/env python3
"""One resumable driver for an Aether fine-tuning run: dataset -> train -> smoke -> score.

    python3 tools/aether_train_run.py RUN.json plan       # print everything, change nothing
    python3 tools/aether_train_run.py RUN.json dataset    # build + gate the dataset here
    python3 tools/aether_train_run.py RUN.json push       # copy dataset, trainer, harness, scripts to the host
    python3 tools/aether_train_run.py RUN.json train      # launch the training container (detached)
    python3 tools/aether_train_run.py RUN.json score      # serve + smoke + score each variant (detached tmux)
    python3 tools/aether_train_run.py RUN.json status     # where the run is, from the host's own files
    python3 tools/aether_train_run.py RUN.json fetch      # copy the run's records (not weights) back here
    python3 tools/aether_train_run.py RUN.json release    # stop this run's containers and tmux session

A run is one JSON file under Tests/aether_specialization/training/runs/. It names
a model family from training/families.json (chat markers, LoRA targets, image,
thinking switch, tokenizer fix, serving settings), the base model path on the
host, the host, the dataset, the LoRA recipe and the scoring plan. Nothing about a
family is hard-coded here, so a new family is a registry entry, not a new script.

Every stage is idempotent and decides what is left from files: the dataset
manifest here, the merged export and the per-suite `.done` markers on the host.
Long work never runs in this process: training is a detached container and
scoring a detached tmux session on the host, so this laptop can sleep and any
stage can be re-run to continue. `status` answers from the host's files.

Guards, each from an earlier failed round:
- a stage that needs the GPU refuses to start while another of this run's
  containers is up, or while the host has less memory available than the run's
  `min_available_gb` (a shared box: ComfyUI, a MiMo trial);
- the trainer fails before step 1 when the chat markers mask every label;
- the base model and the image are checked on the host before launch;
- a smoke pass (three easy tasks, no guide) runs before every scored variant and
  stops the variant on empty output or a reasoning trace in the content;
- the grader binary is pinned by version and sha256, recorded per run;
- families with `tokenizer_from_base` are served with the base tokenizer.

Host names stay out of tracked files: a run names its host by its short name and
the ssh user comes from the private fleet overlay (AETHER_TRAIN_SSH_USER,
default `claw`). Generated scripts use $HOME and the run's paths only.
"""

from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import pathlib
import re
import shlex
import subprocess
import sys

_SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
if str(_SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(_SCRIPT_DIR))
import fleet_env  # noqa: E402

REPO_ROOT = _SCRIPT_DIR.parent
TRAINING_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "training"
FAMILIES_PATH = TRAINING_DIR / "families.json"
DATASETS_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "datasets"
RUNS_OUT_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "training_runs"
TRAINER = _SCRIPT_DIR / "unsloth_qwen_coder_30b_sft.py"

# What the scoring harness needs on the host, relative to the repo root.
HARNESS_FILES = [
    "tools/aether_doc_bench.py",
    "tools/aether_oracle_check.py",
    "tools/fleet_env.py",
    "Tests/aether_doc_bench/",
    "components/aether/docs/",
    "components/aether/VERSION",
]
HARNESS_EXCLUDES = ["results/", "harness_out/", "*.bak*", "destinations.*.json", "__pycache__/"]
# What a dataset is built from; a change here (committed or not) is a different dataset.
DATASET_INPUTS = ["Tests/aether_specialization/corpus_candidates", "Tests/aether_specialization/corpus_candidates_manifest.json",
                  "Tests/aether_specialization/seed_instruction_pairs.json", "Tests/aether_specialization/seed_repair_pairs*.json",
                  "Tests/aether_doc_bench/tasks*.json", "tools/aether_specialization_*.py", "tools/recapture_expected.py"]
DATASET_FILES = ["aether_instruction_sft.jsonl", "aether_repair_sft.jsonl", "aether_training_mix.json",
                 "aether_build_report.json"]
# Anything like these in a dataset or a fetched record must not reach the public repo.
PRIVATE = re.compile(r"/Users/|/home/[a-z]+/|\.ts\.net|\b100\.(?:6[4-9]|[7-9]\d|1[01]\d|12[0-7])\.\d+\.\d+\b|"
                     r"\b192\.168\.|sk-[A-Za-z0-9_-]{8,}|Bearer [A-Za-z0-9._-]{12,}")
PRECISION_FLAGS = {"4bit": [], "8bit": ["--load-in-8bit", "--no-load-in-4bit"], "16bit": ["--no-load-in-4bit"]}


# --------------------------------------------------------------------------- config

def load_run(path: pathlib.Path, families_path: pathlib.Path = FAMILIES_PATH) -> dict:
    """The run config with its family resolved and every default filled in."""
    run = json.loads(path.read_text(encoding="utf-8"))
    registry = json.loads(families_path.read_text(encoding="utf-8"))
    for key in ("tag", "family", "base_model", "host", "dataset"):
        if key not in run:
            raise SystemExit(f"{path}: missing required key {key!r}")
    if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*", run["tag"]):
        raise SystemExit(f"{path}: tag {run['tag']!r} must be lower-case letters, digits, '.' and '-'")
    family = registry["families"].get(run["family"])
    if family is None:
        raise SystemExit(f"{path}: unknown family {run['family']!r} (known: {', '.join(registry['families'])})")
    run["family_config"] = family
    run["families_version"] = registry.get("version")
    run.setdefault("precision", "4bit")
    if run["precision"] not in PRECISION_FLAGS:
        raise SystemExit(f"{path}: precision must be one of {', '.join(PRECISION_FLAGS)}")
    run["image"] = run.get("image") or family["image"]
    run["lora"] = {"r": 32, "alpha": 64, "dropout": 0.0} | run.get("lora", {})
    run["train"] = {"epochs": 3, "learning_rate": 1e-4, "batch_size": 1, "grad_accum": 8, "seed": 3407,
                    "early_stopping_patience": 2, "early_stopping_threshold": 0.0} | run.get("train", {})
    run["dataset"] = {"repair_manifest": "Tests/aether_specialization/seed_repair_pairs.json"} | run["dataset"]
    if "id" not in run["dataset"]:
        raise SystemExit(f"{path}: dataset.id is required")
    score = {"variants": ["base", "tuned"], "docs": "none", "repeats": 1, "repair_attempts": 2,
             "temperature": 0, "max_output_tokens": 8192, "seed_base": 42, "port": 8019,
             "request_timeout_seconds": 900, "smoke_tasks": 3,
             "grader": "$HOME/aether-current/build/aether",
             "suites": {"v2_pos": "Tests/aether_doc_bench/tasks_v2_pos.json",
                        "hard_v2": "Tests/aether_doc_bench/tasks_hard_v2.json",
                        "hard_nontoon": "Tests/aether_doc_bench/tasks_hard_nontoon.json",
                        "cs": "Tests/aether_doc_bench/tasks_cs.json"}}
    run["score"] = score | run.get("score", {})
    for variant in run["score"]["variants"]:
        if variant.split(":")[0] not in ("base", "tuned"):
            raise SystemExit(f"{path}: score variant {variant!r} is not base[:docs] or tuned[:docs]")
    run.setdefault("min_available_gb", 60)
    run.setdefault("remote_root", "$HOME/aether-train")
    run.setdefault("weights_root", "/storage/aether-train")
    run["paths"] = {
        "remote": f"{run['remote_root']}/{run['tag']}",
        "bench": f"{run['remote_root']}/{run['tag']}/bench",
        "weights": f"{run['weights_root']}/{run['tag']}",
        "merged": f"{run['weights_root']}/{run['tag']}/merged_16bit",
        "dataset_local": str(DATASETS_DIR / run["dataset"]["id"]),
        "out_local": str(RUNS_OUT_DIR / run["tag"]),
    }
    run["containers"] = {"train": f"aether-train-{run['tag']}", "serve": f"aether-serve-{run['tag']}"}
    run["tmux"] = f"aether-score-{run['tag']}"
    return run


def ssh_target(run: dict) -> str:
    user = fleet_env.get("AETHER_TRAIN_SSH_USER", "claw")
    return f"{user}@{run['host']}"


# --------------------------------------------------------------------------- scripts

def trainer_argv(run: dict) -> list[str]:
    """The trainer's command line inside the container (workspace = the run dir)."""
    fam, lora, train = run["family_config"], run["lora"], run["train"]
    argv = [
        "python", "/workspace/scripts/unsloth_qwen_coder_30b_sft.py",
        "--model-id", run["base_model"],
        "--instruction-jsonl", "/workspace/data/aether_instruction_sft.jsonl",
        "--repair-jsonl", "/workspace/data/aether_repair_sft.jsonl",
        "--output-dir", "/workspace/train",
        *PRECISION_FLAGS[run["precision"]],
        "--epochs", str(train["epochs"]),
        "--learning-rate", str(train["learning_rate"]),
        "--batch-size", str(train["batch_size"]),
        "--grad-accum", str(train["grad_accum"]),
        "--seed", str(train["seed"]),
        "--random-state", str(train["seed"]),
        "--early-stopping-patience", str(train["early_stopping_patience"]),
        "--early-stopping-threshold", str(train["early_stopping_threshold"]),
        "--lora-r", str(lora["r"]),
        "--lora-alpha", str(lora["alpha"]),
        "--lora-dropout", str(lora["dropout"]),
        "--target-modules", ",".join(fam["target_modules"]),
        "--instruction-part", fam["instruction_part"].replace("\n", "\\n"),
        "--response-part", fam["response_part"].replace("\n", "\\n"),
        "--chat-template-kwargs", json.dumps(fam.get("chat_template_kwargs") or {}),
        "--export-merged-16bit", "--merged-output-dir", run["paths"]["merged"],
    ]
    if train.get("max_steps"):
        argv += ["--max-steps", str(train["max_steps"])]
    return argv


def docker_mounts(run: dict) -> list[str]:
    mounts = ["-v", f"{run['paths']['remote']}:/workspace", "-v", "/storage:/storage"]
    for extra in run.get("extra_mounts", []):
        mounts += ["-v", f"{extra}:{extra}"]
    return mounts


def train_script(run: dict) -> str:
    c = run["containers"]["train"]
    docker = ["docker", "run", "-d", "--name", c, "--gpus", "all", "--ipc=host", "--shm-size=16g",
              "--ulimit", "memlock=-1", "--ulimit", "stack=67108864", *docker_mounts(run),
              "-e", "HF_HOME=/storage/hf", "-e", "HF_HUB_OFFLINE=1", run["image"], *trainer_argv(run)]
    quoted = " ".join(_q(a) for a in docker)
    return f"""#!/bin/bash
# Generated by tools/aether_train_run.py for run {run['tag']}; do not edit by hand.
set -u
R={run['paths']['remote']}
mkdir -p "$R/train" "$R/logs" {run['paths']['weights']}
if [ -f {run['paths']['merged']}/config.json ]; then echo "merged export already exists"; exit 0; fi
if [ "$(docker inspect -f '{{{{.State.Running}}}}' {c} 2>/dev/null)" = "true" ]; then echo "already running"; exit 0; fi
docker rm -f {c} >/dev/null 2>&1
docker image inspect {_q(run['image'])} --format '{{{{.Id}}}}' > "$R/logs/image_id.txt" || {{ echo "image missing: {run['image']}"; exit 3; }}
docker run --rm --entrypoint python {_q(run['image'])} -c 'import importlib.metadata as m, json
v={{}}
for p in ["torch","transformers","trl","peft","unsloth","unsloth_zoo","bitsandbytes","accelerate","datasets","flash-linear-attention"]:
    try: v[p]=m.version(p)
    except Exception: pass
print(json.dumps(v))' > "$R/logs/image_libraries.json" 2>/dev/null
echo "launched $(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$R/logs/train_launches.txt"
{quoted}
"""


def serve_command(run: dict, variant: str) -> list[str]:
    fam, s = run["family_config"], run["family_config"]["serve"]
    model = run["paths"]["merged"] if variant.split(":")[0] == "tuned" else run["base_model"]
    argv = ["docker", "run", "-d", "--name", run["containers"]["serve"], "--gpus", "all", "--ipc=host",
            "--shm-size=16g", *docker_mounts(run)[2:], "-p", f"{run['score']['port']}:8000", s["image"],
            "--model", model, "--served-model-name", served_name(run, variant),
            "--dtype", s["dtype"], "--max-model-len", str(s["max_model_len"]),
            "--gpu-memory-utilization", str(s["gpu_memory_utilization"]), "--port", "8000", *s["extra_args"]]
    if fam.get("tokenizer_from_base") and model != run["base_model"]:
        argv += ["--tokenizer", run["base_model"]]
    return argv


def served_name(run: dict, variant: str) -> str:
    return f"{run['tag']}-{variant.split(':')[0]}"


def destinations(run: dict, variant: str) -> dict:
    dest = {
        "id": served_name(run, variant),
        "type": "openai_chat_completions",
        "base_url": f"http://localhost:{run['score']['port']}/v1",
        "api_key": "",
        "model": served_name(run, variant),
        "temperature": run["score"]["temperature"],
        "max_output_tokens": run["score"]["max_output_tokens"],
        "request_timeout_seconds": run["score"]["request_timeout_seconds"],
        "request_max_retries": 2,
        "retry_backoff_seconds": 5,
        "cooldown_seconds": 0,
    }
    kwargs = run["family_config"].get("chat_template_kwargs") or {}
    if kwargs:
        dest["extra_body"] = {"chat_template_kwargs": kwargs}
    return {"destinations": [dest]}


def score_script(run: dict) -> str:
    s, p = run["score"], run["paths"]
    variants = " ".join(s["variants"])
    suites = " ".join(f"{name}={path}" for name, path in s["suites"].items())
    serve = {v: " ".join(_q(a) for a in serve_command(run, v)) for v in s["variants"]}
    serve_cases = "\n".join(f"    {_q(v)}) {serve[v]} ;;" for v in s["variants"])
    leak = json.dumps(run["family_config"].get("leak_prefixes", []))
    # The grader may be newer than the compiler that built the dataset (a release
    # that lands between training and scoring); score.grader_version says so.
    grader_version = s.get("grader_version") or run["dataset"].get("aether_version", "")
    return f"""#!/bin/bash
# Generated by tools/aether_train_run.py for run {run['tag']}; do not edit by hand.
# Serves each variant, smoke-tests it on {s['smoke_tasks']} easy tasks, then scores every
# suite with --resume. Re-run to continue; finished suites have a .done marker.
set -u
R={p['remote']}
B={p['bench']}
OUT="$R/score"
LOG="$OUT/score.log"
GRADER={s['grader']}
mkdir -p "$OUT"
log() {{ echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" | tee -a "$LOG"; }}
cleanup() {{ docker rm -f {run['containers']['serve']} >/dev/null 2>&1; log "served model stopped"; }}
trap cleanup EXIT

[ -x "$GRADER" ] || {{ log "ABORT grader missing: $GRADER"; exit 2; }}
GRADER_VERSION=$($GRADER --version 2>&1 | head -1)
GRADER_SHA=$(sha256sum "$GRADER" | cut -d' ' -f1)
log "grader $GRADER_VERSION sha256 $GRADER_SHA"
case "$GRADER_VERSION" in *{grader_version}*) ;; *) log "ABORT grader is not language {grader_version}"; exit 2 ;; esac
echo "$GRADER_SHA" > "$OUT/grader.sha256"

serve() {{
  docker rm -f {run['containers']['serve']} >/dev/null 2>&1
  case "$1" in
{serve_cases}
  esac
}}

for VARIANT in {variants}; do
  KIND=${{VARIANT%%:*}}; DOCS={s['docs']}
  case "$VARIANT" in *:*) DOCS=${{VARIANT#*:}} ;; esac
  TAGV=${{VARIANT//:/-}}
  [ -f "$OUT/$TAGV.done" ] && {{ log "SKIP $VARIANT (done)"; continue; }}
  if [ "$KIND" = tuned ] && [ ! -f {p['merged']}/config.json ]; then log "SKIP $VARIANT: no merged export yet"; continue; fi
  NAME={run['tag']}-$KIND
  log "=== $VARIANT: serving $NAME"
  serve "$VARIANT" >> "$LOG" 2>&1 || {{ log "FAIL docker run for $VARIANT"; continue; }}
  READY=0
  for i in $(seq 1 180); do
    curl -s --max-time 5 http://localhost:{s['port']}/v1/models 2>/dev/null | grep -q "\\"$NAME\\"" && {{ READY=1; break; }}
    [ "$(docker inspect -f '{{{{.State.Running}}}}' {run['containers']['serve']} 2>/dev/null)" = "true" ] || break
    sleep 10
  done
  if [ $READY -ne 1 ]; then log "FAIL $VARIANT never became ready"; docker logs --tail 80 {run['containers']['serve']} >> "$LOG" 2>&1; continue; fi
  log "READY $VARIANT"
  DEST="$R/destinations.$KIND.json"
  run_suite() {{  # name tasks repeats repairs
    python3 -u "$B/tools/aether_doc_bench.py" --destinations-config "$DEST" --tasks "$2" \\
      --docs "$DOCS" --repeats "$3" --repair-attempts "$4" --seed-base {s['seed_base']} \\
      --aether-bin "$GRADER" --aether-bin-sha256 "$GRADER_SHA" --allow-skew --keep-loaded \\
      --resume --output-json "$OUT/${{TAGV}}__$1.json" --text-summary >> "$LOG" 2>&1
  }}
  if [ ! -f "$OUT/${{TAGV}}__smoke.ok" ]; then
    run_suite smoke "$R/smoke_tasks.json" 1 0
    if ! python3 "$R/check_generations.py" "$OUT/${{TAGV}}__smoke.json" '{leak}' >> "$LOG" 2>&1; then
      log "FAIL $VARIANT smoke: empty output or a reasoning trace; not scoring it"; continue
    fi
    touch "$OUT/${{TAGV}}__smoke.ok"
  fi
  ALL=1
  for PAIR in {suites}; do
    SUITE=${{PAIR%%=*}}; TASKS="$B/${{PAIR#*=}}"
    [ -f "$OUT/${{TAGV}}__$SUITE.done" ] && continue
    log "--- $VARIANT $SUITE"
    run_suite "$SUITE" "$TASKS" {s['repeats']} {s['repair_attempts']}
    if python3 "$R/check_generations.py" "$OUT/${{TAGV}}__$SUITE.json" '{leak}' --complete >> "$LOG" 2>&1; then
      touch "$OUT/${{TAGV}}__$SUITE.done"
    else
      ALL=0; log "WARN $VARIANT $SUITE incomplete or suspicious; re-run to resume"
    fi
  done
  [ $ALL -eq 1 ] && touch "$OUT/$TAGV.done" && log "DONE $VARIANT"
  docker rm -f {run['containers']['serve']} >/dev/null 2>&1
done
log "score pass finished"
"""


CHECK_GENERATIONS = '''#!/usr/bin/env python3
"""Fail on a report whose generations are empty or reasoning traces (written by aether_train_run.py).

    check_generations.py REPORT.json LEAK_PREFIXES_JSON [--complete]

--complete also fails when a case was never measured (infra failure or missing).
Prints the first-attempt and final exact counts either way.
"""
import json, sys

path, prefixes = sys.argv[1], [p.lower() for p in json.loads(sys.argv[2])]
try:
    report = json.load(open(path))
except (OSError, ValueError) as exc:
    print(f"check: cannot read {path}: {exc}")
    sys.exit(1)
variant = report["destinations"][0]["variants"][0]
results, summary = variant["results"], variant["summary"]
def first_source(r):
    attempts = r.get("attempts") or []
    return ((attempts[0].get("source_code") if attempts else None) or r.get("source_code") or "")
empty = sum(1 for r in results if not first_source(r).strip())
leak = sum(1 for r in results if prefixes and first_source(r).lstrip().lower().startswith(tuple(prefixes)))
infra = summary.get("infra_failed", 0)
print(f"check {path.rsplit('/', 1)[-1]}: cases={summary['total_cases']} FA={summary.get('first_attempt_exact')} "
      f"FX={summary.get('final_exact')} empty={empty} leak={leak} infra={infra}")
bad = leak > 0 or (results and empty == len(results))
if "--complete" in sys.argv[3:]:
    bad = bad or infra > 0 or report.get("infra_failed")
sys.exit(1 if bad else 0)
'''


def smoke_tasks(run: dict) -> dict:
    source = json.loads((REPO_ROOT / run["score"]["suites"].get("v2_pos", "Tests/aether_doc_bench/tasks_v2_pos.json"))
                        .read_text(encoding="utf-8"))
    return {"version": f"smoke-{source.get('version', '')}",
            "description": f"first {run['score']['smoke_tasks']} tasks of tasks_v2_pos, for the export smoke test",
            "tasks": source["tasks"][: run["score"]["smoke_tasks"]]}


# Prints model=ok only when every shard the weight index names is present and
# non-empty and no download is still in flight; config.json alone (the first
# file a download writes) is model=partial.
MODEL_PROBE = (
    "import glob, json, os, sys\n"
    "p = sys.argv[1]\n"
    "if not os.path.isfile(p + '/config.json'):\n"
    "    print('model=missing'); sys.exit()\n"
    "idx = p + '/model.safetensors.index.json'\n"
    "files = set(json.load(open(idx))['weight_map'].values()) if os.path.isfile(idx) else "
    "{os.path.basename(f) for f in glob.glob(p + '/*.safetensors')}\n"
    "missing = [f for f in files if not os.path.isfile(p + '/' + f) or os.path.getsize(p + '/' + f) == 0]\n"
    "busy = glob.glob(p + '/.cache/huggingface/download/*.incomplete')\n"
    "print('model=ok' if files and not missing and not busy else 'model=partial')\n"
)


def model_probe_cmd(path: str) -> str:
    return f"python3 -c {shlex.quote(MODEL_PROBE)} {_q(path)}"


def when_ready_script(run: dict) -> str:
    return f"""#!/bin/bash
# Generated by tools/aether_train_run.py for run {run['tag']}; do not edit by hand.
# Waits until the base weights are complete, the host has {run['min_available_gb']} GB available
# and no other aether-train/aether-serve container runs, then starts train.sh once.
set -u
R={run['paths']['remote']}
LOG="$R/logs/when_ready.log"
mkdir -p "$R/logs"
log() {{ echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*" >> "$LOG"; }}
log "waiting for weights, memory and a free GPU"
while true; do
  MODEL=$({model_probe_cmd(run['base_model'])})
  AVAIL=$(free -g | awk '/^Mem:/{{print $7}}')
  OTHERS=$(docker ps --format '{{{{.Names}}}}' | grep -E '^aether-(train|serve)-' | grep -v '^{run['containers']['train']}$' | tr '\n' ' ')
  if [ "$MODEL" = model=ok ] && [ "$AVAIL" -ge {run['min_available_gb']} ] && [ -z "$OTHERS" ]; then
    log "ready ($MODEL, $AVAIL GB available); starting training"
    bash "$R/train.sh" >> "$LOG" 2>&1
    log "train.sh exit $?"
    exit 0
  fi
  log "not yet: $MODEL, $AVAIL GB available, others: ${{OTHERS:-none}}"
  sleep 300
done
"""


def score_when_ready_script(run: dict) -> str:
    return f"""#!/bin/bash
# Generated by tools/aether_train_run.py for run {run['tag']}; do not edit by hand.
# Waits for this run's training container to exit and for no other
# aether-train/aether-serve container to run, then runs score.sh.
set -u
R={run['paths']['remote']}
mkdir -p "$R/logs"
while [ "$(docker inspect -f '{{{{.State.Running}}}}' {run['containers']['train']} 2>/dev/null)" = "true" ]; do sleep 300; done
while docker ps --format '{{{{.Names}}}}' | grep -qE '^aether-(train|serve)-'; do sleep 300; done
echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] training container gone; scoring" >> "$R/logs/when_ready.log"
exec bash "$R/score.sh"
"""


def _q(arg: str) -> str:
    """Quote for bash, but leave $HOME expandable."""
    if arg.startswith("$HOME/") and re.fullmatch(r"[A-Za-z0-9_./$:=-]+", arg):
        return arg
    return shlex.quote(arg)


# --------------------------------------------------------------------------- stages

def sh(argv: list[str], *, check: bool = True, capture: bool = False,
       cwd: pathlib.Path | None = None) -> subprocess.CompletedProcess:
    print("+ " + " ".join(shlex.quote(a) for a in argv), flush=True)
    return subprocess.run(argv, check=check, text=True, capture_output=capture, cwd=cwd)


def remote(run: dict, command: str, *, check: bool = True, capture: bool = True) -> subprocess.CompletedProcess:
    return sh(["ssh", "-o", "ConnectTimeout=15", ssh_target(run), command], check=check, capture=capture)


def sha256(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def git_head(path: pathlib.Path) -> str:
    out = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], text=True, capture_output=True)
    return out.stdout.strip()


def stage_plan(run: dict, args) -> int:
    shown = {k: v for k, v in run.items() if k != "family_config"}
    print(json.dumps(shown, indent=2))
    print("\nfamily:", json.dumps(run["family_config"], indent=2))
    print("\ntrain.sh:\n" + train_script(run))
    print("score.sh:\n" + score_script(run))
    return 0


def stage_dataset(run: dict, args) -> int:
    out = pathlib.Path(run["paths"]["dataset_local"])
    manifest_path = out / "DATASET_MANIFEST.json"
    aether_bin = pathlib.Path(args.aether_bin)
    if manifest_path.exists() and not args.force:
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        bad = [n for n, h in manifest["files"].items() if not (out / n).exists() or sha256(out / n) != h]
        if bad:
            raise SystemExit(f"{manifest_path}: files changed since the manifest was written: {bad} (use --force to rebuild)")
        print(f"dataset {run['dataset']['id']} already built and intact ({manifest['records']})")
        return 0
    version = subprocess.run([str(aether_bin), "--version"], text=True, capture_output=True).stdout.strip()
    want = run["dataset"].get("aether_version")
    if want and want not in version:
        raise SystemExit(f"{aether_bin} is {version!r}, the run wants language {want}")
    sh([sys.executable, str(_SCRIPT_DIR / "aether_specialization_prepare_assets.py"), "--output-dir", str(out),
        "--aether-bin", str(aether_bin), "--repair-manifest", str(REPO_ROOT / run["dataset"]["repair_manifest"]),
        "--version", run["dataset"].get("version", f"{datetime.date.today().isoformat()}-1")])
    leaks = {}
    for name in DATASET_FILES:
        found = sorted(set(PRIVATE.findall((out / name).read_text(encoding="utf-8"))))
        if found:
            leaks[name] = found[:5]
    if leaks:
        raise SystemExit(f"dataset holds host or credential detail, not publishable: {leaks}")
    mix = json.loads((out / "aether_training_mix.json").read_text(encoding="utf-8"))
    manifest = {
        "dataset_id": run["dataset"]["id"],
        "built_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "umbrella_commit": git_head(REPO_ROOT),
        "umbrella_dirty": bool(subprocess.run(["git", "-C", str(REPO_ROOT), "status", "--porcelain", "--", *DATASET_INPUTS],
                                              text=True, capture_output=True).stdout.strip()),
        "aether_version": mix.get("aether_version"),
        "aether_sha256": mix.get("aether_sha256"),
        "repair_manifest": run["dataset"]["repair_manifest"],
        "records": {"instruction": mix.get("instruction_records"), "repair": mix.get("repair_records")},
        "files": {name: sha256(out / name) for name in DATASET_FILES},
    }
    # The raw and reference corpora are rebuilt from tracked sources; only the
    # files the trainer reads (and their reports) are kept with the dataset.
    for name in ("aether_raw_corpus.json", "aether_reference_corpus.json", "aether_recapture_report.json"):
        (out / name).unlink(missing_ok=True)
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"dataset {run['dataset']['id']}: {manifest['records']} -> {out}")
    return 0


def write_local_scripts(run: dict) -> pathlib.Path:
    out = pathlib.Path(run["paths"]["out_local"])
    out.mkdir(parents=True, exist_ok=True)
    (out / "train.sh").write_text(train_script(run), encoding="utf-8")
    (out / "score.sh").write_text(score_script(run), encoding="utf-8")
    (out / "train_when_ready.sh").write_text(when_ready_script(run), encoding="utf-8")
    (out / "score_when_ready.sh").write_text(score_when_ready_script(run), encoding="utf-8")
    (out / "check_generations.py").write_text(CHECK_GENERATIONS, encoding="utf-8")
    (out / "smoke_tasks.json").write_text(json.dumps(smoke_tasks(run), indent=2) + "\n", encoding="utf-8")
    kinds = sorted({v.split(":")[0] for v in run["score"]["variants"]})
    for kind in kinds:
        (out / f"destinations.{kind}.json").write_text(json.dumps(destinations(run, kind), indent=2) + "\n",
                                                       encoding="utf-8")
    resolved = {k: v for k, v in run.items()} | {"resolved_at": datetime.datetime.now(
        datetime.timezone.utc).isoformat(timespec="seconds"), "umbrella_commit": git_head(REPO_ROOT)}
    (out / "run.resolved.json").write_text(json.dumps(resolved, indent=2) + "\n", encoding="utf-8")
    for name in ("train.sh", "train_when_ready.sh", "score_when_ready.sh", "score.sh", "check_generations.py"):
        (out / name).chmod(0o755)
    return out


def stage_push(run: dict, args) -> int:
    data = pathlib.Path(run["paths"]["dataset_local"])
    if not (data / "DATASET_MANIFEST.json").exists():
        raise SystemExit(f"no dataset at {data}; run the dataset stage first")
    out = write_local_scripts(run)
    rdir = run["paths"]["remote"]
    remote(run, f"mkdir -p {rdir}/data {rdir}/scripts {rdir}/bench {rdir}/logs {rdir}/score", capture=False)
    target = ssh_target(run)
    rpath = rdir.replace("$HOME/", "")  # rsync paths are relative to the remote home
    sh(["rsync", "-a", f"{data}/", f"{target}:{rpath}/data/"])
    sh(["rsync", "-a", str(TRAINER), f"{target}:{rpath}/scripts/"])
    sh(["rsync", "-aR", *[f"--exclude={e}" for e in HARNESS_EXCLUDES], *HARNESS_FILES,
        f"{target}:{rpath}/bench/"], cwd=REPO_ROOT)
    sh(["rsync", "-a", f"{out}/", f"{target}:{rpath}/"])
    return stage_check(run, args)


def stage_check(run: dict, args) -> int:
    """Read-only preconditions on the host: model, image, memory, nothing of ours already running."""
    p = run["paths"]
    probe = remote(run, " ; ".join([
        model_probe_cmd(run["base_model"]),
        f"docker image inspect {_q(run['image'])} >/dev/null 2>&1 && echo image=ok || echo image=missing",
        f"docker image inspect {_q(run['family_config']['serve']['image'])} >/dev/null 2>&1 && echo serve_image=ok || echo serve_image=missing",
        "echo avail_gb=$(free -g | awk '/^Mem:/{print $7}')",
        "echo containers=$(docker ps --format '{{.Names}}' | tr '\\n' ,)",
        f"test -f {p['merged']}/config.json && echo merged=yes || echo merged=no",
        f"echo disk_gb=$(df -BG --output=avail {run['weights_root'].rsplit('/', 1)[0] or '/'} | tail -1 | tr -dc 0-9)",
    ]), check=False)
    facts = dict(line.split("=", 1) for line in probe.stdout.split() if "=" in line)
    print(json.dumps(facts, indent=2))
    problems = []
    if facts.get("model") != "ok" and not (facts.get("model") == "partial" and getattr(args, "when_ready", False)):
        problems.append(f"base model {facts.get('model', 'missing')} on {run['host']}: {run['base_model']}")
    if facts.get("image") != "ok":
        problems.append(f"training image missing on {run['host']}: {run['image']}")
    if facts.get("serve_image") != "ok":
        problems.append(f"serving image missing on {run['host']}: {run['family_config']['serve']['image']}")
    for line in problems:
        print("PROBLEM:", line)
    return 1 if problems else 0


def host_busy(run: dict) -> tuple[int, list[str]]:
    probe = remote(run, "free -g | awk '/^Mem:/{print $7}'; docker ps --format '{{.Names}}'", check=False)
    lines = probe.stdout.split()
    avail = int(lines[0]) if lines and lines[0].isdigit() else -1
    return avail, lines[1:]


def gpu_gate(run: dict, args, own: str) -> None:
    avail, names = host_busy(run)
    others = [n for n in names if n.startswith(("aether-train-", "aether-serve-")) and n != own]
    if others:
        raise SystemExit(f"{run['host']} already runs {others}; one GPU job at a time")
    if avail < run["min_available_gb"] and not args.ignore_memory:
        raise SystemExit(f"{run['host']} has {avail} GB available, the run needs {run['min_available_gb']} "
                         f"(containers up: {names}); free the box or pass --ignore-memory")


def stage_train(run: dict, args) -> int:
    if stage_check(run, args):
        return 1
    if remote(run, f"test -f {run['paths']['merged']}/config.json", check=False).returncode == 0:
        print("merged export exists: training is done")
        return 0
    running = remote(run, f"docker inspect -f '{{{{.State.Running}}}}' {run['containers']['train']} 2>/dev/null",
                     check=False).stdout.strip()
    if running == "true":
        print("training container is running")
        return 0
    if args.when_ready:
        session = f"aether-train-wait-{run['tag']}"
        if remote(run, f"tmux has-session -t {session} 2>/dev/null", check=False).returncode == 0:
            print(f"{session} is already waiting; see logs/when_ready.log")
            return 0
        remote(run, f"tmux new-session -d -s {session} 'bash {run['paths']['remote']}/train_when_ready.sh'",
               capture=False)
        print(f"started {session} on {run['host']}: training begins when the weights are complete and the box is free")
        return 0
    gpu_gate(run, args, run["containers"]["train"])
    remote(run, f"bash {run['paths']['remote']}/train.sh", capture=False)
    return 0


def stage_score(run: dict, args) -> int:
    if remote(run, f"tmux has-session -t {run['tmux']} 2>/dev/null", check=False).returncode == 0:
        print(f"scoring session {run['tmux']} is already running")
        return 0
    if args.when_ready:
        remote(run, f"tmux new-session -d -s {run['tmux']} 'bash {run['paths']['remote']}/score_when_ready.sh'",
               capture=False)
        print(f"started {run['tmux']} on {run['host']}: scoring begins when {run['containers']['train']} has exited")
        return 0
    gpu_gate(run, args, run["containers"]["serve"])
    remote(run, f"tmux new-session -d -s {run['tmux']} 'bash {run['paths']['remote']}/score.sh'", capture=False)
    print(f"started tmux session {run['tmux']} on {run['host']}")
    return 0


def stage_status(run: dict, args) -> int:
    p, c = run["paths"], run["containers"]
    cmd = " ; ".join([
        f"echo '--- containers'; docker ps -a --filter name=aether-.*-{run['tag']} --format '{{{{.Names}}}} {{{{.Status}}}}'",
        f"echo '--- training'; ls {p['remote']}/train 2>/dev/null | grep checkpoint | tail -2",
        f"docker logs --tail 3 {c['train']} 2>&1 | tr '\\r' '\\n' | tail -3",
        f"test -f {p['merged']}/config.json && echo 'merged export: present' || echo 'merged export: not yet'",
        f"tail -1 {p['remote']}/logs/when_ready.log 2>/dev/null",
        f"echo '--- scoring'; tmux has-session -t {run['tmux']} 2>/dev/null && echo 'tmux: running' || echo 'tmux: not running'",
        f"ls {p['remote']}/score 2>/dev/null | grep -E '\\.(done|ok)$'",
        f"tail -5 {p['remote']}/score/score.log 2>/dev/null",
        "echo \"--- host: $(free -g | awk '/^Mem:/{print $7}') GB available\"",
    ])
    print(remote(run, cmd, check=False).stdout)
    return 0


def stage_fetch(run: dict, args) -> int:
    out = pathlib.Path(run["paths"]["out_local"])
    out.mkdir(parents=True, exist_ok=True)
    rpath = run["paths"]["remote"].replace("$HOME/", "")
    target = ssh_target(run)
    sh(["rsync", "-a", "--exclude=checkpoint-*", "--exclude=*.safetensors", "--exclude=*.bin",
        "--exclude=optimizer.pt", "--exclude=final/", f"{target}:{rpath}/train/", f"{out}/train/"], check=False)
    sh(["rsync", "-a", f"{target}:{rpath}/score/", f"{out}/score/"], check=False)
    sh(["rsync", "-a", f"{target}:{rpath}/logs/", f"{out}/logs/"], check=False)
    remote(run, f"docker logs {run['containers']['train']} > {run['paths']['remote']}/logs/train_container.log 2>&1",
           check=False)
    sh(["rsync", "-a", f"{target}:{rpath}/logs/train_container.log", f"{out}/logs/"], check=False)
    print(f"fetched into {out}; scrub and pack before committing (Tests/aether_doc_bench/results/pack_reports.py)")
    return 0


def stage_release(run: dict, args) -> int:
    c = run["containers"]
    # Killing the tmux session is not enough: a harness mid-request outlives it by
    # seconds and can rewrite its report after the files were moved (2026-10-15).
    remote(run, f"tmux kill-session -t {run['tmux']} 2>/dev/null; tmux kill-session -t aether-train-wait-{run['tag']} 2>/dev/null; "
                f"pkill -f {run['paths']['remote']}/bench/tools/aether_doc_bench.py 2>/dev/null; sleep 3; "
                f"docker rm -f {c['serve']} 2>/dev/null; "
                + (f"docker rm -f {c['train']} 2>/dev/null; " if args.include_training else "")
                + "true", check=False, capture=False)
    return 0


STAGES = {"plan": stage_plan, "dataset": stage_dataset, "push": stage_push, "check": stage_check,
          "train": stage_train, "score": stage_score, "status": stage_status, "fetch": stage_fetch,
          "release": stage_release}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run", type=pathlib.Path)
    ap.add_argument("stage", choices=list(STAGES))
    ap.add_argument("--aether-bin", default=str(REPO_ROOT / "build" / "bin" / "aether"),
                    help="compiler that builds the dataset (dataset stage)")
    ap.add_argument("--force", action="store_true", help="dataset: rebuild although a manifest exists")
    ap.add_argument("--ignore-memory", action="store_true", help="train/score: skip the available-memory gate")
    ap.add_argument("--include-training", action="store_true", help="release: also remove the training container")
    ap.add_argument("--when-ready", action="store_true",
                    help="train: wait on the host (tmux) for complete weights and a free box, then start")
    args = ap.parse_args(argv)
    run = load_run(args.run)
    return STAGES[args.stage](run, args)


if __name__ == "__main__":
    sys.exit(main())
