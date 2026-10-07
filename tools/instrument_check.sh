#!/bin/bash
# The instrument check: everything that says whether the benchmark still
# measures what it claims, at zero GPU, against one aether binary.
#
#   tools/instrument_check.sh --aether-bin BIN [--log] [--aether-sha SHA]
#                             [--docs-dir DIR] [--replay GLOB ...]
#   tools/instrument_check.sh --lint-only          # no binary (the CI case)
#
# Steps (each one's result goes into the summary; missing tools are recorded
# as skipped, never as passed):
#   1. oracle   tools/aether_oracle_check.py: references, negatives, sandbox
#   2. replay   tools/replay_bench.py over the tracked results/** (plus any
#               --replay globs, e.g. a local harness_out/**), with waivers
#   3. corpus   tools/recapture_expected.py --check (expected-stdout drift)
#   4. gates    the corpus build gates (W1-12/W1-13), dry run, when the
#               dataset builder offers --check-gates
#   5. contam   tools/check_guide_contamination.py (W1-14), when present
# With --log, one row is appended to Tests/aether_doc_bench/results/
# instrument_log.csv: date, umbrella sha, aether sha, VERSION, binary sha256,
# guide stamps and shas, replay counts, unwaived count, corpus drift, oracle
# failures, skipped steps. Run it for every merged FE item (locally) and every
# components/aether pin bump. Exits 1 on an oracle failure or an unwaived
# replay regression.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

AETHER_BIN=""
LOG=0
LINT_ONLY=0
AETHER_SHA=""
DOCS_DIR=""
REPLAY_GLOBS=()
while [ $# -gt 0 ]; do
    case "$1" in
        --aether-bin) AETHER_BIN=$2; shift 2 ;;
        --log) LOG=1; shift ;;
        --lint-only) LINT_ONLY=1; shift ;;
        --aether-sha) AETHER_SHA=$2; shift 2 ;;
        --docs-dir) DOCS_DIR=$2; shift 2 ;;
        --replay) REPLAY_GLOBS+=("$2"); shift 2 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

if [ $LINT_ONLY -eq 1 ]; then
    python3 tools/aether_oracle_check.py --lint-only || exit 1
    python3 tools/replay_bench.py --check-waivers || exit 1
    exit 0
fi

[ -n "$AETHER_BIN" ] || { echo "usage: $0 --aether-bin BIN [--log] (or --lint-only)" >&2; exit 2; }
case "$AETHER_BIN" in /*) ;; *) AETHER_BIN="$PWD/$AETHER_BIN" ;; esac
[ -x "$AETHER_BIN" ] || { echo "not an executable: $AETHER_BIN" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/instrument-check.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
SKIPPED=()

echo "[1/5] oracle"
python3 tools/aether_oracle_check.py --aether-bin "$AETHER_BIN" --quiet --report-json "$WORK/oracle.json"
ORACLE_RC=$?

echo "[2/5] replay"
python3 tools/replay_bench.py --aether-bin "$AETHER_BIN" --report-json "$WORK/replay.json" \
    "Tests/aether_doc_bench/results/**/*.json" ${REPLAY_GLOBS[@]+"${REPLAY_GLOBS[@]}"}
REPLAY_RC=$?

echo "[3/5] corpus recapture --check"
if [ -f tools/recapture_expected.py ]; then
    python3 tools/recapture_expected.py --check --aether-bin "$AETHER_BIN" --report-json "$WORK/recapture.json" \
        >"$WORK/recapture.log" 2>&1
    echo "      exit $? (drift is reported; see the summary)"
else
    SKIPPED+=(recapture)
fi

echo "[4/5] corpus build gates (dry run)"
if python3 tools/aether_specialization_build_dataset.py --help 2>/dev/null | grep -q -- '--check-gates'; then
    python3 tools/aether_specialization_build_dataset.py --check-gates --aether-bin "$AETHER_BIN" \
        >"$WORK/gates.log" 2>&1
    echo "$?" >"$WORK/gates.rc"
else
    echo "      skipped: the dataset builder has no --check-gates yet (W1-12/W1-13)"
    SKIPPED+=(build_gates)
fi

echo "[5/5] guide contamination"
if [ -f tools/check_guide_contamination.py ]; then
    python3 tools/check_guide_contamination.py --aether-bin "$AETHER_BIN" >"$WORK/contam.log" 2>&1
    echo "$?" >"$WORK/contam.rc"
else
    echo "      skipped: tools/check_guide_contamination.py does not exist yet (W1-14)"
    SKIPPED+=(contamination)
fi

python3 - "$WORK" "$LOG" "$AETHER_SHA" "$DOCS_DIR" "${SKIPPED[*]+${SKIPPED[*]}}" <<'PYEOF'
import csv, datetime, json, pathlib, sys
sys.path.insert(0, "tools")
import aether_doc_bench as adb

work, log, aether_sha, docs_dir, skipped = pathlib.Path(sys.argv[1]), sys.argv[2] == "1", sys.argv[3], sys.argv[4], sys.argv[5]
def load(name):
    path = work / name
    return json.loads(path.read_text()) if path.exists() else None
oracle, replay, recapture = load("oracle.json"), load("replay.json"), load("recapture.json")
tool = (oracle or {}).get("toolchain") or (replay or {}).get("replay_binary") or {}
first = ((replay or {}).get("tallies") or {}).get("first") or {}
drift = None
if recapture:
    drift = recapture.get("drift_count", recapture.get("drifted"))
    if isinstance(drift, list):
        drift = len(drift)
docs = pathlib.Path(docs_dir) if docs_dir else adb.DEFAULT_AETHER_ROOT / "docs"
guides = []
for name, filename in adb.DOC_FILENAMES.items():
    path = docs / filename
    if path.is_file():
        record = adb.guide_record(name, path, path.read_text(encoding="utf-8"))
        guides.append(f"{name}={record['version']}:{record['sha256'][:12]}")
def rc(name):
    path = work / name
    return path.read_text().strip() if path.exists() else ""
row = {
    "date": datetime.date.today().isoformat(),
    "pscal_sha": (adb._git(adb.REPO_ROOT, "rev-parse", "--short=12", "HEAD") or "")
                 + ("-dirty" if adb._git(adb.REPO_ROOT, "status", "--porcelain", "--untracked-files=no") else ""),
    "aether_sha": aether_sha or tool.get("binary_commit") or "standalone",
    "version": tool.get("language_version", ""),
    "binary_sha256": tool.get("binary_sha256", ""),
    "guides": ";".join(guides) or "unavailable",
    "replay_first_attempts": first.get("attempts", ""),
    "replay_first_pass": f"{first.get('old_pass', '')}->{first.get('new_pass', '')}" if first else "",
    "replay_fail_to_pass": first.get("fail_to_pass", ""),
    "replay_pass_to_fail": first.get("pass_to_fail", ""),
    "replay_waived": len((replay or {}).get("waived") or []),
    "replay_unwaived": len((replay or {}).get("unwaived") or []) if replay else "",
    "corpus_drift": "" if drift is None else drift,
    "oracle_failures": (oracle or {}).get("failures", ""),
    "oracle_references": f"{(oracle or {}).get('references_passed', '')}/{(oracle or {}).get('references_total', '')}",
    "gates_rc": rc("gates.rc"),
    "contamination_rc": rc("contam.rc"),
    "skipped": skipped,
}
print("\nsummary:")
for key, value in row.items():
    print(f"  {key:22} {value}")
if log:
    path = adb.REPO_ROOT / "Tests" / "aether_doc_bench" / "results" / "instrument_log.csv"
    new = not path.exists()
    with path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(row))
        if new:
            writer.writeheader()
        writer.writerow(row)
    print(f"appended a row to {adb.display_path(path)}")
PYEOF

if [ $ORACLE_RC -ne 0 ] || [ $REPLAY_RC -ne 0 ]; then
    echo "instrument check FAILED (oracle rc=$ORACLE_RC, replay rc=$REPLAY_RC)"
    exit 1
fi
echo "instrument check passed"
