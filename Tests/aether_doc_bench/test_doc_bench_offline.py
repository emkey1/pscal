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

The later sections drive the whole harness as a subprocess, against
fake_aether.py (a directive-interpreting stand-in for the compiler, so they run
on a machine without one, such as the python-only CI job) or, where a test needs
real Aether semantics, the binary named by $AETHER_BIN (skipped when unset).

Run standalone:  python3 Tests/aether_doc_bench/test_doc_bench_offline.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import contextlib
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_doc_bench as adb  # noqa: E402

BENCH_DIR = REPO_ROOT / "Tests" / "aether_doc_bench"
HARNESS = REPO_ROOT / "tools" / "aether_doc_bench.py"
FAKE_AETHER = BENCH_DIR / "fake_aether.py"
MOCK_MODEL = BENCH_DIR / "mock_model.py"
SMOKE_TASKS = BENCH_DIR / "smoke_tasks.json"


class Skipped(Exception):
    """Raised by a test that cannot run here (e.g. no real aether binary)."""


def skip(reason: str) -> None:
    try:
        import pytest  # noqa: F401
    except ImportError:
        raise Skipped(reason)
    pytest.skip(reason)


def real_aether_bin() -> pathlib.Path:
    """The real compiler for tests that need Aether semantics, else skip."""
    candidates = [os.environ.get("AETHER_BIN") or ""]
    candidates += [str(REPO_ROOT / "build" / "bin" / "aether"), str(REPO_ROOT / "components" / "aether" / "build" / "aether")]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return pathlib.Path(candidate)
    skip("no aether binary: set AETHER_BIN to run this test")
    raise AssertionError("unreachable")


@contextlib.contextmanager
def workdir():
    with tempfile.TemporaryDirectory(prefix="adb-offline-") as name:
        yield pathlib.Path(name)


def write_json(path: pathlib.Path, payload) -> pathlib.Path:
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return path


def mock_destination(dest_id: str = "mock", model_script: pathlib.Path = MOCK_MODEL, **extra) -> dict:
    entry = {
        "id": dest_id,
        "type": "command",
        "command_template": f"{sys.executable} {model_script} {{prompt_file}}",
    }
    entry.update(extra)
    return entry


def run_harness(argv: list[str], env: dict | None = None, timeout: int = 180,
                script: pathlib.Path = HARNESS) -> subprocess.CompletedProcess:
    full_env = dict(os.environ)
    for key in ("MOCK_SEED_LOG", "MOCK_MODEL_FAKE_PROGRAMS", "FAKE_AETHER_LOG", "FAKE_AETHER_VERSION"):
        full_env.pop(key, None)
    full_env.update(env or {})
    return subprocess.run(
        [sys.executable, str(script), *argv],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        timeout=timeout,
        env=full_env,
    )


def fake_run(tmp: pathlib.Path, extra_argv: list[str], *, tasks: pathlib.Path = SMOKE_TASKS,
             destinations: list[dict] | None = None, env: dict | None = None,
             allow_skew: bool = True) -> tuple[subprocess.CompletedProcess, dict | None]:
    """Run the harness on the fake compiler with the mock model; return the
    process and the parsed report (None when no report was written)."""
    config = write_json(tmp / "destinations.json", {"destinations": destinations or [mock_destination()]})
    out = tmp / "report.json"
    argv = [
        "--tasks", str(tasks),
        "--destinations-config", str(config),
        "--aether-bin", str(FAKE_AETHER),
        "--output-json", str(out),
    ]
    if allow_skew:
        argv.append("--allow-skew")
    if "--docs" not in extra_argv and not any(a.startswith("--doc") for a in extra_argv):
        argv += ["--docs", "none"]
    full_env = {"MOCK_MODEL_FAKE_PROGRAMS": "1"}
    full_env.update(env or {})
    proc = run_harness(argv + extra_argv, env=full_env)
    report = json.loads(out.read_text()) if out.exists() else None
    return proc, report


def all_cases(report: dict, key: str = "results"):
    for dest in report["destinations"]:
        for variant in dest["variants"]:
            for case in variant.get(key, []):
                yield dest, variant, case


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
    assert "trailing newlines differ" not in detail


def test_token_mismatch_also_flags_a_trailing_newline_delta():
    # A content fix alone would not have made this exact; say so in the same
    # round rather than waiting for the next one.
    detail = adb.describe_stdout_mismatch("a b\n", "a c\n\n")
    assert "missing: b" in detail
    assert "trailing newlines differ" in detail
    assert "expected 1, observed 2" in detail


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


# --------------------------------------------------------------------------- #
# W1-05: reproducibility -- destinations, seeds, binary snapshot, skew guard
# --------------------------------------------------------------------------- #


def _expect_load_failure(payload: dict, needle: str) -> None:
    with workdir() as tmp:
        path = write_json(tmp / "destinations.json", payload)
        try:
            adb.load_destinations(path)
        except SystemExit as exc:
            assert needle in str(exc), f"message {exc} does not name {needle!r}"
        else:
            raise AssertionError(f"load_destinations accepted {payload}")


def test_unknown_destination_key_fails():
    # The cs-aug18 shape: a key nothing reads used to vanish without a word.
    _expect_load_failure({"destinations": [mock_destination(sede=42)]}, "sede")
    _expect_load_failure({"seed": 42, "destinations": [mock_destination()]}, "seed")
    # Annotations and the miner's own keys stay legal.
    with workdir() as tmp:
        path = write_json(tmp / "d.json", {
            "_note": ["fine"],
            "destinations": [mock_destination(_tier="low", guide="small", system="x", seed=7)],
        })
        loaded = adb.load_destinations(path)
        assert loaded[0].seed == 7


def test_destination_seed_rules():
    _expect_load_failure({"destinations": [mock_destination(seed="42")]}, "integer")
    _expect_load_failure(
        {"destinations": [{"id": "r", "type": "openai_responses", "model": "m", "seed": 1}]},
        "cannot be seeded",
    )
    _expect_load_failure(
        {"destinations": [mock_destination(seed=1, extra_body={"seed": 2})]}, "extra_body.seed"
    )


def test_tracked_destination_files_have_no_unknown_keys():
    files = sorted(BENCH_DIR.glob("destinations*.json")) + [BENCH_DIR / "repair_test_destinations.json"]
    assert files
    for path in files:
        adb.validate_destination_config(json.loads(path.read_text()), path)


def test_request_seed_policy():
    local = adb.Destination(destination_id="l", kind="openai_chat_completions", base_url="http://claw1:8900/v1")
    cloud = adb.Destination(destination_id="c", kind="openai_chat_completions", base_url="https://api.example.com/v1")
    responses = adb.Destination(destination_id="r", kind="openai_responses", base_url="https://api.openai.com/v1")
    assert [adb.request_seed(local, r, 42) for r in range(3)] == [42, 43, 44]
    assert adb.request_seed(cloud, 0, 42) is None, "cloud destinations opt in with their own seed"
    cloud.seed = 5
    assert adb.request_seed(cloud, 2, 42) == 7
    assert adb.request_seed(responses, 0, 42) is None
    assert adb.request_seed(local, 1, None) is None


def test_chat_request_carries_seed_and_clamped_budget():
    seen = {}

    def fake_request(url, body, api_key, **kwargs):
        seen["body"] = body
        return {"choices": [{"message": {"content": "x"}, "finish_reason": "stop"}]}

    original = adb.http_json_request
    adb.http_json_request = fake_request
    try:
        dest = adb.Destination(destination_id="l", kind="openai_chat_completions", model="m",
                               base_url="http://h:1/v1", max_output_tokens=24000)
        adb.invoke_openai_chat_completions("p", dest, adb.RequestOptions(seed=43, max_tokens=7000))
    finally:
        adb.http_json_request = original
    assert seen["body"]["seed"] == 43
    assert seen["body"]["max_tokens"] == 7000


def test_mock_model_logs_seeds_42_43_44_across_repeats():
    with workdir() as tmp:
        log = tmp / "seeds.jsonl"
        proc, report = fake_run(
            tmp,
            ["--repeats", "3", "--seed-base", "42", "--task", "hello_fx", "--task", "classify_scores"],
            env={"MOCK_SEED_LOG": str(log)},
        )
        assert proc.returncode == 0, proc.stderr
        seen: dict[str, list[str]] = {}
        for line in log.read_text().splitlines():
            entry = json.loads(line)
            seen.setdefault(entry["task_ids"][0], []).append(entry["seed"])
        assert seen == {"hello_fx": ["42", "43", "44"], "classify_scores": ["42", "43", "44"]}, seen
        assert sorted({(c["repeat_index"], c["seed"]) for _, _, c in all_cases(report)}) == [(0, 42), (1, 43), (2, 44)]
        # A destination's own seed is its base, and --start-repeat re-runs one repeat.
        log.unlink()
        proc, _ = fake_run(
            tmp,
            ["--start-repeat", "2", "--repeats", "1", "--task", "hello_fx"],
            destinations=[mock_destination(seed=100)],
            env={"MOCK_SEED_LOG": str(log)},
        )
        assert proc.returncode == 0, proc.stderr
        assert [json.loads(x)["seed"] for x in log.read_text().splitlines()] == ["102"]


def test_report_carries_binary_and_guide_sha256s():
    with workdir() as tmp:
        guide = tmp / "medium.md"
        guide.write_text("# Guide\n\n*Guide version: 2026-09-05-1*\n\nbody\n", encoding="utf-8")
        proc, report = fake_run(tmp, ["--doc", f"medium={guide}"])
        assert proc.returncode == 0, proc.stderr
        fake_sha = hashlib.sha256(FAKE_AETHER.read_bytes()).hexdigest()
        assert report["binary_sha256"] == fake_sha
        assert report["toolchain"]["binary_sha256_at_end"] == fake_sha
        guide_sha = hashlib.sha256(guide.read_bytes()).hexdigest()
        assert report["guides"]["medium"]["sha256"] == guide_sha
        assert report["guides"]["medium"]["version"] == "2026-09-05-1"
        variant = report["destinations"][0]["variants"][0]
        assert variant["doc_name"] == "medium" and variant["doc_sha256"] == guide_sha
        assert report["tasks_sha256"] == hashlib.sha256(SMOKE_TASKS.read_bytes()).hexdigest()
        assert report["destinations_sha256"]
        assert report["harness"]["prompt_template"]["sha256"]
        assert report["harness"]["files"]["tools/aether_doc_bench.py"]
        assert report["provenance"]["umbrella_head"]
        assert "components_aether_submodule" in report["provenance"]
        assert report["skew_guard"]["allowed_by_flag"] is True and report["skew_guard"]["reasons"]
        shas = {a["run"]["binary_sha256"] for _, _, c in all_cases(report) for a in c["attempts"]}
        assert shas == {fake_sha}, shas
        assert all(c["run"]["exact_stdout_match"] for _, _, c in all_cases(report))


def test_dirty_binary_aborts():
    with workdir() as tmp:
        proc, report = fake_run(tmp, [], env={"FAKE_AETHER_VERSION": "2026-10-06-1+abc1234-dirty"},
                                allow_skew=False)
        assert proc.returncode != 0
        assert "dirty" in proc.stderr and "skew guard" in proc.stderr, proc.stderr
        assert report is None, "a refused run must not write a report"
        # --allow-skew lets it through and records why.
        proc, report = fake_run(tmp, [], env={"FAKE_AETHER_VERSION": "2026-10-06-1+abc1234-dirty"})
        assert proc.returncode == 0, proc.stderr
        assert any("dirty" in r for r in report["skew_guard"]["reasons"])


def test_standalone_binary_needs_allow_skew_and_its_sha():
    env = {"FAKE_AETHER_VERSION": "2026-10-06-1 (latest tag: untagged)"}
    fake_sha = hashlib.sha256(FAKE_AETHER.read_bytes()).hexdigest()
    with workdir() as tmp:
        proc, _ = fake_run(tmp, [], env=env, allow_skew=False)
        assert proc.returncode != 0 and "standalone" in proc.stderr, proc.stderr
        proc, _ = fake_run(tmp, [], env=env)
        assert proc.returncode != 0 and "--aether-bin-sha256" in proc.stderr, proc.stderr
        proc, _ = fake_run(tmp, ["--aether-bin-sha256", "0" * 64], env=env)
        assert proc.returncode != 0 and "does not match" in proc.stderr, proc.stderr
        proc, report = fake_run(tmp, ["--aether-bin-sha256", fake_sha], env=env)
        assert proc.returncode == 0, proc.stderr
        assert report["toolchain"]["standalone_build"] is True
        assert report["skew_guard"]["expected_binary_sha256"] == fake_sha


def _git_repo_with_docs(root: pathlib.Path) -> str:
    (root / "docs").mkdir(parents=True)
    (root / "docs" / "aether_for_llms_medium_contexts.md").write_text("*Guide version: 2026-09-05-1*\n")
    env = dict(os.environ, GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    for argv in (["init", "-q"], ["add", "."], ["commit", "-q", "-m", "docs"]):
        subprocess.run(["git", "-C", str(root), *argv], check=True, capture_output=True, env=env)
    return subprocess.run(["git", "-C", str(root), "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()


def test_binary_commit_must_match_the_aether_root_head():
    with workdir() as tmp:
        head = _git_repo_with_docs(tmp / "aether")
        root_args = ["--aether-root", str(tmp / "aether"), "--docs", "medium"]
        proc, report = fake_run(tmp, root_args, env={"FAKE_AETHER_VERSION": f"2026-10-06-1+{head[:7]}"},
                                allow_skew=False)
        assert proc.returncode == 0, proc.stderr
        assert report["skew_guard"]["ok"] is True
        assert report["provenance"]["aether_root_checkout"]["head"] == head
        assert report["destinations"][0]["variants"][0]["doc_version"] == "2026-09-05-1"
        proc, _ = fake_run(tmp, root_args, env={"FAKE_AETHER_VERSION": "2026-10-06-1+0000000"}, allow_skew=False)
        assert proc.returncode != 0 and "+0000000" in proc.stderr, proc.stderr


def test_variants_interleave_per_task_with_rotation():
    with workdir() as tmp:
        a, b = tmp / "a.md", tmp / "b.md"
        a.write_text("guide A\n")
        b.write_text("guide B\n")
        proc, report = fake_run(tmp, ["--doc", f"ga={a}", "--doc", f"gb={b}"])
        assert proc.returncode == 0, proc.stderr
        order = sorted(
            (c["case_sequence"], c["task_id"], v["doc_name"]) for _, v, c in all_cases(report)
        )
        task_ids = [t["id"] for t in json.loads(SMOKE_TASKS.read_text())["tasks"]]
        expected = []
        for index, task_id in enumerate(task_ids):
            pair = ["ga", "gb"] if index % 2 == 0 else ["gb", "ga"]
            expected += [(task_id, name) for name in pair]
        assert [(t, d) for _, t, d in order] == expected, order


def test_aether_args_reach_every_call_and_the_snapshot_is_used():
    with workdir() as tmp:
        log = tmp / "calls.jsonl"
        proc, report = fake_run(tmp, ["--aether-arg=--strict", "--task", "hello_fx"],
                                env={"FAKE_AETHER_LOG": str(log)})
        assert proc.returncode == 0, proc.stderr
        calls = [json.loads(x)["argv"] for x in log.read_text().splitlines()]
        runs = [c for c in calls if "--version" not in c]
        assert runs, calls
        for argv in calls:
            assert pathlib.Path(argv[0]).resolve() != FAKE_AETHER.resolve(), "ran the original, not the snapshot"
            assert "aether-bench-run-" in argv[0]
        for argv in runs:
            assert argv.index("--strict") < len(argv) - 1, argv
            assert argv[-1].endswith(".aether")
        assert report["toolchain"]["aether_args"] == ["--strict"]
        case = next(c for _, _, c in all_cases(report))
        assert "--strict" in case["run"]["command"]


def test_doc_override_wrapper_maps_to_doc_flag():
    with workdir() as tmp:
        guide = tmp / "g.md"
        guide.write_text("*Guide version: 2026-01-01-1*\n")
        config = write_json(tmp / "d.json", {"destinations": [mock_destination()]})
        proc = run_harness(
            ["medium", str(guide), "--tasks", str(SMOKE_TASKS), "--destinations-config", str(config),
             "--aether-bin", str(FAKE_AETHER), "--allow-skew", "--preflight-only"],
            script=REPO_ROOT / "tools" / "run_aether_doc_bench_with_doc.py",
        )
        assert proc.returncode == 0, proc.stderr
        summary = json.loads(proc.stdout)
        assert summary["docs"] == {"medium": hashlib.sha256(guide.read_bytes()).hexdigest()}


def test_smoke_run_on_the_real_binary_has_one_binary_sha256():
    binary = real_aether_bin()
    with workdir() as tmp:
        config = write_json(tmp / "d.json", {"destinations": [mock_destination()]})
        out = tmp / "r.json"
        proc = run_harness([
            "--tasks", str(SMOKE_TASKS), "--destinations-config", str(config), "--docs", "none",
            "--aether-bin", str(binary), "--allow-skew",
            "--aether-bin-sha256", hashlib.sha256(binary.read_bytes()).hexdigest(),
            "--output-json", str(out),
        ])
        assert proc.returncode == 0, proc.stderr
        report = json.loads(out.read_text())
        shas = {a["run"]["binary_sha256"] for _, _, c in all_cases(report) for a in c["attempts"]}
        assert shas == {report["binary_sha256"]}, shas
        assert sum(c["run"]["exact_stdout_match"] for _, _, c in all_cases(report)) == 4



def _main() -> int:
    failures = 0
    skipped = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_") or not callable(fn):
            continue
        try:
            fn()
        except Skipped as exc:
            skipped += 1
            print(f"skip {name}: {exc}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL {name}: {exc}")
        except Exception as exc:  # noqa: BLE001
            failures += 1
            print(f"ERROR {name}: {type(exc).__name__}: {exc}")
        else:
            print(f"ok   {name}")
    note = f" ({skipped} skipped)" if skipped else ""
    print(("FAILED" if failures else "all tests passed") + note)
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(_main())
