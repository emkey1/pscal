#!/usr/bin/env python3
"""Offline unit tests for tools/aether_doc_bench.py failure feedback.

No model calls and no compiler: these feed expected/observed stdout pairs into
``describe_stdout_mismatch`` / ``derive_failure_summary`` and assert that the
message handed to the repair round actually names the difference.

The motivating case is FMT-001-class whitespace failure. ``toon_fleet_rollup``
(tasks_frontier.json) produced output that was correct except for one extra
trailing blank line from a stray ``println("")``. ``describe_stdout_mismatch``
used to ``rstrip("\\n")`` both sides before diffing, so the only difference was
erased, the unified diff came back empty, and the repair prompt carried the bare
string ``stdout_mismatch`` plus two blobs that render identically. gpt-5-mini
burned two repair rounds in each of two independent runs without ever seeing
what was wrong. Any whitespace-only mismatch was effectively unrepairable, which
understates every model's score on exact-output tasks.

Run standalone:  python3 Tests/aether_doc_bench/test_doc_bench_offline.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_doc_bench as adb  # noqa: E402


# The real strings from the toon_fleet_rollup failure, hardcoded so the test
# does not depend on a session scratchpad that no longer exists.
TOON_EXPECTED = (
    "a1: cores=8 mem=32 tags=1\n"
    "b2: cores=4 mem=0 tags=0\n"
    "c3: cores=16 mem=128 tags=2\n"
    "totals cores=28 mem=160 tagged=2\n"
)
TOON_OBSERVED = TOON_EXPECTED + "\n"


def _run(returncode: int = 0, stdout: str = "", stderr: str = "") -> dict:
    """A run result shaped like aether_doc_bench.compile_and_run's return."""
    return {
        "command": ["aether", "--no-cache", "prog.aether"],
        "returncode": returncode,
        "stdout": stdout,
        "stderr": stderr,
        "diagnostics": None,
        "elapsed_seconds": 0.0,
        "exact_stdout_match": False,
    }


def test_toon_fleet_rollup_extra_blank_line_is_described():
    detail = adb.describe_stdout_mismatch(TOON_EXPECTED, TOON_OBSERVED)
    assert detail != "stdout_mismatch", "the regression: repair feedback carried no information"
    lowered = detail.lower()
    assert "trailing newline" in lowered
    # The counts have to be there; "they differ somehow" is not actionable.
    assert "1 newline character(s)" in detail
    assert "observed ends with 2" in detail
    # repr-style tails make the difference visible in a prompt.
    assert repr("tagged=2\n") in detail or "tagged=2\\n'" in detail
    assert "delete 1 trailing newline" in detail


def test_missing_final_newline_is_described():
    detail = adb.describe_stdout_mismatch("alpha\nbeta\n", "alpha\nbeta")
    assert detail != "stdout_mismatch"
    assert "trailing newline" in detail.lower()
    assert "emit 1 more trailing newline" in detail


def test_trailing_spaces_on_a_line_are_described():
    detail = adb.describe_stdout_mismatch("alpha\nbeta\n", "alpha  \nbeta\n")
    assert detail != "stdout_mismatch"
    assert "whitespace" in detail.lower()
    assert "line 1" in detail
    assert repr("alpha  \n") in detail


def test_leading_indentation_difference_is_described():
    detail = adb.describe_stdout_mismatch("alpha\n", "  alpha\n")
    assert "whitespace" in detail.lower()
    assert repr("  alpha\n") in detail


def test_crlf_line_endings_are_described():
    detail = adb.describe_stdout_mismatch("alpha\n", "alpha\r\n")
    assert "whitespace" in detail.lower()
    assert "\\r" in detail, "the carriage return must be visible, not silently rendered"


def test_single_line_whitespace_only_is_not_called_a_reordering():
    # The token branch used to answer "same tokens, different order/positions"
    # for these, which is actively misleading.
    detail = adb.describe_stdout_mismatch("total=42\n", "total=42\n\n")
    assert "order/positions" not in detail
    assert "trailing newline" in detail.lower()


def test_real_content_mismatch_still_produces_a_diff():
    detail = adb.describe_stdout_mismatch("alpha\nbeta\n", "alpha\nGAMMA\n")
    assert detail.startswith("stdout_mismatch:\n")
    assert "-beta" in detail
    assert "+GAMMA" in detail


def test_content_mismatch_also_flags_a_trailing_newline_delta():
    detail = adb.describe_stdout_mismatch("alpha\nbeta\n", "alpha\nGAMMA\n\n")
    assert "-beta" in detail
    assert "trailing newlines differ" in detail
    assert "expected 1, observed 2" in detail


def test_single_line_token_reordering_still_reported():
    detail = adb.describe_stdout_mismatch("a,b,c\n", "c,b,a\n")
    assert "same tokens, different order/positions" in detail


def test_single_line_missing_token_still_reported():
    detail = adb.describe_stdout_mismatch("a,b,c\n", "a,b\n")
    assert "missing: c" in detail


def test_derive_failure_summary_passes_whitespace_detail_through():
    # The repair path calls derive_failure_summary, not describe_stdout_mismatch
    # directly; make sure the detail survives the wrapper.
    summary = adb.derive_failure_summary(
        generated_ok=True,
        run=_run(returncode=0, stdout=TOON_OBSERVED),
        expected_stdout=TOON_EXPECTED,
    )
    assert summary != "stdout_mismatch"
    assert "trailing newline" in summary.lower()


def test_derive_failure_summary_unaffected_for_nonzero_exit():
    summary = adb.derive_failure_summary(
        generated_ok=True,
        run=_run(returncode=1, stderr="boom: it broke\nsecond line\n"),
        expected_stdout=TOON_EXPECTED,
    )
    assert summary == "boom: it broke"


def test_repair_prompt_embeds_the_whitespace_detail():
    task = adb.Task(
        task_id="toon_fleet_rollup",
        title="Fleet rollup",
        prompt="Print the rollup.",
        expected_stdout=TOON_EXPECTED,
    )
    summary = adb.derive_failure_summary(
        generated_ok=True,
        run=_run(returncode=0, stdout=TOON_OBSERVED),
        expected_stdout=TOON_EXPECTED,
    )
    prompt = adb.build_repair_prompt(
        doc_name="medium",
        doc_text="<guide>",
        task=task,
        previous_source="println(\"\")",
        attempt_number=1,
        failure_summary=summary,
        observed_stdout=TOON_OBSERVED,
        observed_stderr="",
    )
    assert "TRAILING NEWLINES" in prompt
    assert "delete 1 trailing newline" in prompt


def _main() -> int:
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {name}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"ERROR {name}: {type(exc).__name__}: {exc}")
        else:
            print(f"ok   {name}")
    print("FAILED" if failures else "all tests passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_main())
