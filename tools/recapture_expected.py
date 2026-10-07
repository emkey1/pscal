#!/usr/bin/env python3
"""Detect (and optionally re-capture) expected-stdout drift in the Aether
specialization corpus after compiler behavior changes.

Runs every manifest candidate against the current aether binary and compares
actual stdout to the manifest's stored ``stdout``.

  --check            (default) report drift, exit 1 if any found — the
                     pre-flight mode tools/aether_specialization_prepare_assets.py
                     runs before every dataset build
  --update           print a unified diff of every drifted golden and exit 1
                     without writing anything
  --update --accept  rewrite the drifted ``stdout`` fields in place, with
                     provenance in ``metadata.recaptured`` (aether version + date)

A drifted golden is a compiler behavior change until a person says otherwise:
re-capturing from the current binary would bless a regression as the new
truth, so nothing is written without reading the diff and passing --accept.

Environment-dependent candidates (``metadata.environment_dependent``) are
compared but reported separately and never fail --check on their own.
Candidates whose source no longer matches the manifest sha256 are flagged as
``source_drift`` (the program itself changed; fix that first). Pass
``--accept-source-drift`` with ``--update --accept`` to deliberately
re-baseline those entries too (refreshes sha256 AND stdout — changes corpus
provenance, so only do this when the source edits were intentional).
"""

from __future__ import annotations

import argparse
import datetime
import difflib
import hashlib
import json
import pathlib
import shutil
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_AETHER_BIN = REPO_ROOT / "build" / "bin" / "aether"
DEFAULT_MANIFEST = (
    REPO_ROOT / "Tests" / "aether_specialization" / "corpus_candidates_manifest.json"
)
DEFAULT_FIXTURES_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "fixtures"
# Three corpus programs take 24-32 s on a loaded rig; 20 s made them read as
# run failures.
RUN_TIMEOUT_SECONDS = 60


def aether_version(aether_bin: pathlib.Path) -> str:
    try:
        proc = subprocess.run(
            [str(aether_bin), "--version"], text=True, capture_output=True, timeout=10
        )
        return (proc.stdout or proc.stderr).strip().splitlines()[0]
    except Exception:
        return "unknown"


def run_candidate(
    aether_bin: pathlib.Path, source_path: pathlib.Path, fixtures_dir: pathlib.Path
) -> tuple[int | None, str, str]:
    """Run one candidate in a fresh temp cwd seeded with the shared fixtures."""
    with tempfile.TemporaryDirectory(prefix="aether-recapture-") as tmp_name:
        tmp_dir = pathlib.Path(tmp_name)
        if fixtures_dir.is_dir():
            for fixture in fixtures_dir.iterdir():
                if fixture.is_file():
                    shutil.copy2(fixture, tmp_dir / fixture.name)
        try:
            proc = subprocess.run(
                [str(aether_bin), "--no-cache", str(source_path)],
                cwd=tmp_dir,
                text=True,
                capture_output=True,
                timeout=RUN_TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired:
            return None, "", f"timeout after {RUN_TIMEOUT_SECONDS} s"
        return proc.returncode, proc.stdout, proc.stderr


def golden_diff(repo_path: str, expected: str | None, actual: str) -> str:
    return "".join(
        difflib.unified_diff(
            (expected or "").splitlines(keepends=True),
            actual.splitlines(keepends=True),
            fromfile=f"{repo_path} (manifest)",
            tofile=f"{repo_path} (current binary)",
        )
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--fixtures-dir", type=pathlib.Path, default=DEFAULT_FIXTURES_DIR)
    parser.add_argument("--update", action="store_true",
                        help="show the diff of every drifted golden; writes only with --accept")
    parser.add_argument("--accept", action="store_true",
                        help="with --update: write the re-captured goldens after you have read the diff")
    parser.add_argument("--check", action="store_true", help="report only (default)")
    parser.add_argument(
        "--accept-source-drift",
        action="store_true",
        help="with --update --accept: re-baseline sha256+stdout for source-drift entries",
    )
    parser.add_argument("--only", metavar="SUBSTR", help="limit to repo_paths containing SUBSTR")
    parser.add_argument("--report-json", type=pathlib.Path, help="write full report here")
    args = parser.parse_args()

    if args.accept and not args.update:
        parser.error("--accept only makes sense with --update")
    if args.accept_source_drift and not args.update:
        parser.error("--accept-source-drift only makes sense with --update")
    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    items = manifest.get("items", [])
    version = aether_version(args.aether_bin)
    today = datetime.date.today().isoformat()

    ok, drifted, env_dep_drifted, source_drift, run_failed, missing = [], [], [], [], [], []
    # (item, new fields) applied only on --update --accept
    pending: list[tuple[dict, dict]] = []

    for item in items:
        repo_path = item.get("repo_path", "")
        if args.only and args.only not in repo_path:
            continue
        source_path = REPO_ROOT / repo_path
        if not source_path.is_file():
            missing.append(repo_path)
            continue

        source = source_path.read_text(encoding="utf-8")
        sha = hashlib.sha256(source.encode("utf-8")).hexdigest()
        sha_mismatch = bool(item.get("sha256")) and sha != item["sha256"]
        if sha_mismatch and not (args.update and args.accept_source_drift):
            source_drift.append(repo_path)
            continue

        returncode, stdout, stderr = run_candidate(
            args.aether_bin, source_path, args.fixtures_dir
        )
        if returncode != 0:
            run_failed.append(
                {"repo_path": repo_path, "returncode": returncode,
                 "stderr": stderr.strip()[-400:]}
            )
            continue

        expected = item.get("stdout")
        if sha_mismatch:
            # --update --accept-source-drift: deliberate re-baseline
            drifted.append({"repo_path": repo_path, "expected": expected, "actual": stdout})
            pending.append((item, {
                "sha256": sha,
                "stdout": stdout,
                "recaptured": {"aether_version": version, "date": today, "source_rebaselined": True},
            }))
            continue

        if expected is None or stdout == expected:
            ok.append(repo_path)
            continue

        record = {"repo_path": repo_path, "expected": expected, "actual": stdout}
        if item.get("metadata", {}).get("environment_dependent"):
            # Only the path: this output is the running host's cwd, HOME,
            # clock or heap addresses, and the report may be shared.
            env_dep_drifted.append({"repo_path": repo_path})
        else:
            drifted.append(record)
            pending.append((item, {
                "stdout": stdout,
                "recaptured": {"aether_version": version, "date": today},
            }))

    write = bool(args.update and args.accept and pending)
    if write:
        for item, fields in pending:
            recaptured = fields.pop("recaptured")
            item.update(fields)
            item.setdefault("metadata", {})["recaptured"] = recaptured
        args.manifest.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    report = {
        "aether_version": version,
        "checked": len(ok) + len(drifted) + len(env_dep_drifted),
        "ok": len(ok),
        "drifted": drifted,
        "environment_dependent_drifted": env_dep_drifted,
        "source_drift": source_drift,
        "run_failed": run_failed,
        "missing": missing,
        "updated": write,
    }
    if args.report_json:
        args.report_json.write_text(json.dumps(report, indent=2), encoding="utf-8")

    print(f"aether: {version}")
    print(
        f"ok={len(ok)} drifted={len(drifted)} env-dep-drifted={len(env_dep_drifted)} "
        f"source-drift={len(source_drift)} run-failed={len(run_failed)} missing={len(missing)}"
    )
    for rec in drifted:
        if rec["expected"] == rec["actual"]:
            print(f"  REBASELINED {rec['repo_path']} (stdout unchanged, sha refreshed)")
            continue
        print(f"  DRIFT {rec['repo_path']}")
        if args.update:
            print(golden_diff(rec["repo_path"], rec["expected"], rec["actual"]), end="")
        else:
            print(f"    expected: {rec['expected']!r}")
            print(f"    actual:   {rec['actual']!r}")
    for rec in run_failed:
        print(f"  FAIL  {rec['repo_path']} rc={rec['returncode']} {rec['stderr'][:120]!r}")
    for path in source_drift:
        print(f"  SRC-DRIFT {path} (sha256 mismatch — program changed, not output)")
    if write:
        print(f"re-captured {len(pending)} entries into {args.manifest}")
    elif args.update and pending:
        print(
            f"not written: {len(pending)} golden(s) would change. Read the diff above; "
            "if the new output is correct, re-run with --update --accept."
        )
        return 1

    if drifted or run_failed or source_drift or missing:
        return 0 if write and not (run_failed or source_drift or missing) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
