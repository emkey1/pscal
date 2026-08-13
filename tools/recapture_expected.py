#!/usr/bin/env python3
"""Detect (and optionally re-capture) expected-stdout drift in the Aether
specialization corpus after compiler behavior changes.

Runs every manifest candidate against the current aether binary and compares
actual stdout to the manifest's stored ``stdout``.

  --check   (default) report drift, exit 1 if any found — CI/pre-flight mode
  --update  rewrite drifted ``stdout`` fields in place, with provenance in
            ``metadata.recaptured`` (aether version + date)

Environment-dependent candidates (``metadata.environment_dependent``) are
compared but reported separately and never fail --check on their own.
Candidates whose source no longer matches the manifest sha256 are flagged as
``source_drift`` (the program itself changed; fix that first). Pass
``--accept-source-drift`` with ``--update`` to deliberately re-baseline those
entries too (refreshes sha256 AND stdout — changes corpus provenance, so only
do this when the source edits were intentional).
"""

from __future__ import annotations

import argparse
import datetime
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
RUN_TIMEOUT_SECONDS = 20


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
            return None, "", "timeout"
        return proc.returncode, proc.stdout, proc.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--aether-bin", type=pathlib.Path, default=DEFAULT_AETHER_BIN)
    parser.add_argument("--manifest", type=pathlib.Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--fixtures-dir", type=pathlib.Path, default=DEFAULT_FIXTURES_DIR)
    parser.add_argument("--update", action="store_true", help="re-capture drifted stdout")
    parser.add_argument("--check", action="store_true", help="report only (default)")
    parser.add_argument(
        "--accept-source-drift",
        action="store_true",
        help="with --update: re-baseline sha256+stdout for source-drift entries",
    )
    parser.add_argument("--only", metavar="SUBSTR", help="limit to repo_paths containing SUBSTR")
    parser.add_argument("--report-json", type=pathlib.Path, help="write full report here")
    args = parser.parse_args()

    if not args.aether_bin.exists():
        raise SystemExit(f"missing aether binary: {args.aether_bin}")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    items = manifest.get("items", [])
    version = aether_version(args.aether_bin)

    ok, drifted, env_dep_drifted, source_drift, run_failed, missing = [], [], [], [], [], []

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
            item["sha256"] = sha
            item["stdout"] = stdout
            item.setdefault("metadata", {})["recaptured"] = {
                "aether_version": version,
                "date": datetime.date.today().isoformat(),
                "source_rebaselined": True,
            }
            drifted.append(
                {"repo_path": repo_path, "expected": expected, "actual": stdout}
            )
            continue

        if expected is None or stdout == expected:
            ok.append(repo_path)
            continue

        record = {"repo_path": repo_path, "expected": expected, "actual": stdout}
        if item.get("metadata", {}).get("environment_dependent"):
            env_dep_drifted.append(record)
        else:
            drifted.append(record)
            if args.update:
                item["stdout"] = stdout
                item.setdefault("metadata", {})["recaptured"] = {
                    "aether_version": version,
                    "date": datetime.date.today().isoformat(),
                }

    if args.update and drifted:
        args.manifest.write_text(
            json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
        )

    report = {
        "aether_version": version,
        "checked": len(ok) + len(drifted) + len(env_dep_drifted),
        "ok": len(ok),
        "drifted": drifted,
        "environment_dependent_drifted": env_dep_drifted,
        "source_drift": source_drift,
        "run_failed": run_failed,
        "missing": missing,
        "updated": bool(args.update and drifted),
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
        print(f"    expected: {rec['expected']!r}")
        print(f"    actual:   {rec['actual']!r}")
    for rec in run_failed:
        print(f"  FAIL  {rec['repo_path']} rc={rec['returncode']} {rec['stderr'][:120]!r}")
    for path in source_drift:
        print(f"  SRC-DRIFT {path} (sha256 mismatch — program changed, not output)")
    if args.update and drifted:
        print(f"re-captured {len(drifted)} entries into {args.manifest}")

    if drifted or run_failed or source_drift or missing:
        return 0 if args.update and not (run_failed or source_drift or missing) else 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
