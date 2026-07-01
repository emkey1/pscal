#!/usr/bin/env python3
"""Offline unit tests for tools/aether_idea_miner.py failure analysis.

No model calls and no compiler: these feed synthetic ``run`` dicts (the shape
``aether_doc_bench.compile_and_run`` returns) straight into ``analyze_failure``
/ ``finding_key`` and assert the resulting fingerprint.

The motivating case is the STDOUT-reported runtime failure that used to be
misclassified as "silent": a legitimately-violated ``@post`` contract prints
``Aether @post failed in <fn>`` to STDOUT with return code 1 and *empty* stderr
and no diagnostics (verified against the real ``build-sdl/aether``). The miner
previously saw "no diagnostic + empty stderr + rc!=0" and fingerprinted it as
``silent:returncode=1``, polluting the silent-failure finding category.

Run standalone:  python3 Tests/aether_doc_bench/test_miner_offline.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import pathlib
import sys

# The miner lives in tools/ and imports aether_doc_bench from there.
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_idea_miner as m  # noqa: E402


def _run(returncode: int = 0, stdout: str = "", stderr: str = "", diagnostics=None) -> dict:
    """A run result shaped like aether_doc_bench.compile_and_run's return."""
    return {
        "command": ["aether", "--no-cache", "prog.aether"],
        "returncode": returncode,
        "stdout": stdout,
        "stderr": stderr,
        "diagnostics": diagnostics,
        "elapsed_seconds": 0.0,
        "exact_stdout_match": returncode == 0 and stdout == "",
    }


def test_post_violation_on_stdout_is_runtime_not_silent():
    # Verified real behavior: a violated @post prints to STDOUT, exits 1, with
    # empty stderr and no diagnostics.
    run = _run(returncode=1, stdout="Aether @post failed in inc\n", stderr="")
    f = m.analyze_failure("<source>", run)
    assert f is not None
    assert f["silent"] is False, "stdout-reported failure must not be 'silent'"
    assert f["message"] == "Aether @post failed in inc"
    assert f["stderr_head"] == "Aether @post failed in inc"
    assert f["phase"] == "runtime"
    key = m.finding_key(f)
    assert key.startswith("runtime:"), f"expected a runtime fingerprint, got {key!r}"


def test_all_known_stdout_error_prefixes_reclassified():
    for line in (
        "Aether @pre failed in g",
        "Runtime Error: index out of range",
        "Compiler error: internal panic",
    ):
        run = _run(returncode=1, stdout=line + "\n", stderr="")
        f = m.analyze_failure("", run)
        assert f["silent"] is False, f"{line!r} was misclassified as silent"
        assert f["message"] == line
        assert not m.finding_key(f).startswith("silent:"), line


def test_error_line_after_normal_output_is_found():
    # A program that prints, then trips its contract: the marker is not on the
    # first line, but scanning all stdout lines still finds it.
    run = _run(returncode=1, stdout="partial result\nAether @post failed in f\n", stderr="")
    f = m.analyze_failure("", run)
    assert f["silent"] is False
    assert f["message"] == "Aether @post failed in f"
    assert m.finding_key(f).startswith("runtime:")


def test_genuinely_silent_failure_still_silent():
    # rc != 0, nothing on either stream, no diagnostics -> genuinely silent.
    run = _run(returncode=1, stdout="", stderr="")
    f = m.analyze_failure("", run)
    assert f["silent"] is True
    assert m.finding_key(f) == "silent:returncode=1"


def test_unrelated_stdout_output_with_crash_stays_silent():
    # Normal output then a bare nonzero exit (no known error marker) is still
    # silent -- we only reclassify on the known runtime-error prefixes.
    run = _run(returncode=1, stdout="42\nhello\n", stderr="")
    f = m.analyze_failure("", run)
    assert f["silent"] is True
    assert m.finding_key(f).startswith("silent:")


def test_stderr_path_unchanged_and_stdout_ignored_when_diagnostic_present():
    # A real coded diagnostic still classifies by code; the stdout error line is
    # ignored because we only consult stdout when there is nothing else.
    run = _run(
        returncode=1,
        stdout="Aether @post failed in noise\n",
        stderr="",
        diagnostics=[{
            "code": "SCOPE-001",
            "severity": "error",
            "phase": "semantic",
            "message": "identifier 'foo' not in scope",
            "line": 1,
        }],
    )
    f = m.analyze_failure("let x = foo\n", run)
    assert f["code"] == "SCOPE-001"
    assert f["silent"] is False
    assert f["is_missing_identifier"] is True
    assert m.finding_key(f) == "missing:foo"


def test_plain_stderr_failure_unchanged():
    run = _run(returncode=1, stdout="", stderr="boom: something failed\n")
    f = m.analyze_failure("", run)
    assert f["silent"] is False
    assert f["stderr_head"] == "boom: something failed"
    assert m.finding_key(f) == "stderr:boom: something failed"


def test_clean_run_returns_none():
    assert m.analyze_failure("", _run(returncode=0, stdout="ok\n")) is None


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
    failed = 0
    for t in tests:
        try:
            t()
            print(f"ok    - {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  - {t.__name__}: {e}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"ERROR - {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    raise SystemExit(1 if failed else 0)
