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
import math
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



# --------------------------------------------------------------------------- #
# W1-06: a run timeout is a measured failure that keeps its source
# --------------------------------------------------------------------------- #

# A command "model" driven by a JSON plan in $SCRIPTED_MODEL_PLAN:
#   {"initial": SOURCE, "repair": SOURCE, "fail_on": "initial"|"repair"|null,
#    "prompt_log": FILE}
# It answers SOURCE for the matching round (repair rounds are recognised by the
# repair prompt's "Repair attempt number:" line), exits 1 for a fail_on round,
# and appends every prompt it is sent to prompt_log as one JSON line.
SCRIPTED_MODEL = r'''
import json, os, pathlib, sys
plan = json.loads(os.environ["SCRIPTED_MODEL_PLAN"])
prompt = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
kind = "repair" if "Repair attempt number:" in prompt else "initial"
if plan.get("prompt_log"):
    with open(plan["prompt_log"], "a", encoding="utf-8") as handle:
        handle.write(json.dumps({"kind": kind, "prompt": prompt,
                                 "max_tokens": os.environ.get("AETHER_BENCH_MAX_TOKENS", ""),
                                 "seed": os.environ.get("AETHER_BENCH_SEED", "")}) + "\n")
if plan.get("fail_on") == kind:
    sys.stderr.write("upstream exploded\n")
    raise SystemExit(1)
if plan.get("fail_first_n"):
    counter = pathlib.Path(plan["counter"])
    seen = int(counter.read_text()) if counter.exists() else 0
    counter.write_text(str(seen + 1))
    if seen < plan["fail_first_n"]:
        sys.stderr.write("HTTP API error 429: rate limited\n")
        raise SystemExit(1)
sys.stdout.write(plan.get(kind) or plan.get("initial") or "")
'''


def scripted_run(tmp: pathlib.Path, plan: dict, extra_argv: list[str], tasks: list[dict],
                 env: dict | None = None) -> tuple[subprocess.CompletedProcess, dict | None]:
    script = tmp / "scripted_model.py"
    script.write_text(SCRIPTED_MODEL, encoding="utf-8")
    manifest = write_json(tmp / "tasks.json", {"version": "test-1", "tasks": tasks})
    full_env = {"SCRIPTED_MODEL_PLAN": json.dumps(plan)}
    full_env.update(env or {})
    return fake_run(tmp, extra_argv, tasks=manifest,
                    destinations=[mock_destination("scripted", model_script=script)], env=full_env)


def simple_task(task_id: str = "t1", expected: str = "ok\n", **extra) -> dict:
    task = {"id": task_id, "title": task_id, "prompt": f"Print {expected!r}.", "expected_stdout": expected}
    task.update(extra)
    return task


def test_strip_run_dirs_from_paths():
    samples = {
        "/var/folders/0s/n0_9/T/aether-doc-bench-ab12_x/t1.aether:3: [FX-001] boom":
            "t1.aether:3: [FX-001] boom",
        "/private/var/folders/0s/T/aether-doc-bench-zz9/t1.aether:3: x": "t1.aether:3: x",
        "error in /tmp/python-doc-bench-q1w2/t1.py line 2": "error in t1.py line 2",
        "no paths here": "no paths here",
    }
    for raw, want in samples.items():
        assert adb.strip_run_dirs(raw) == want, (raw, adb.strip_run_dirs(raw))
    fp = adb.derive_failure_fingerprint({
        "generated_ok": True,
        "run": {"returncode": 1, "stderr": "/tmp/aether-doc-bench-k3/t1.aether:1: boom\n", "diagnostics": None},
    })
    assert fp == "run_error:boom", fp


def test_run_captured_times_out_without_raising_and_caps_output():
    with workdir() as tmp:
        looping = adb.run_captured([sys.executable, "-c", "import time\nprint('partial', flush=True)\nwhile True: time.sleep(0.05)"],
                                   cwd=tmp, timeout=1)
        assert looping["returncode"] == 124 and looping["timed_out"] is True
        assert looping["stdout"] == "partial\n"
        flood = adb.run_captured([sys.executable, "-c", "import sys\nsys.stdout.write('x' * 50000)"],
                                 cwd=tmp, timeout=20, cap_bytes=1000)
        assert flood["returncode"] == 0 and len(flood["stdout"]) == 1000 and flood["stdout_truncated"] is True
        echoed = adb.run_captured([sys.executable, "-c", "import sys; print(sys.stdin.read().count('\\n'))"],
                                  cwd=tmp, timeout=20)
        assert echoed["stdout"] == "0\n", "a program with no task stdin reads EOF, not the harness's terminal"


def test_python_lane_timeout_is_rc_124():
    task = adb.Task(task_id="loop", title="loop", prompt="", expected_stdout="", timeout_seconds=1)
    run = adb.run_python_task(task, "while True:\n    pass\n")
    assert run["returncode"] == 124 and run["timed_out"] is True
    assert adb.derive_failure_fingerprint({"generated_ok": True, "run": run}, "loop") == "timeout:loop"


def _sandbox_task(task_id: str, expected: str = "") -> "adb.Task":
    return adb.Task(task_id=task_id, title=task_id, prompt="", expected_stdout=expected, timeout_seconds=20,
                    files={"data.txt": "42\n"})


def test_python_baseline_runs_sandboxed():
    if sys.platform != "darwin":
        raise Skipped("the baseline sandbox is sandbox-exec, macOS only")
    os.environ.pop("AETHER_BENCH_UNSANDBOXED_BASELINE", None)
    ok = adb.run_python_task(_sandbox_task("ok", "42 hi\n"),
                             "import math\nopen('note.txt', 'w').write('hi')\n"
                             "print(open('data.txt').read().strip(), open('note.txt').read())\n")
    assert ok["returncode"] == 0 and ok["exact_stdout_match"], ok
    blocked = {
        "network": "import socket\nsocket.create_connection(('1.1.1.1', 80), timeout=3)\nprint('ESCAPED')\n",
        "subprocess": "import subprocess\nsubprocess.run(['/bin/echo', 'x'])\nprint('ESCAPED')\n",
        "os.system": "import os\nrc = os.system('/bin/echo x')\nprint('ESCAPED' if rc == 0 else 'blocked')\n",
        "home read": "import os\nprint(os.listdir(os.path.expanduser('~'))[:1], 'ESCAPED')\n",
        "home write": "import os\nopen(os.path.expanduser('~/.aether_sandbox_probe'), 'w').write('x')\nprint('ESCAPED')\n",
        "tmp read": "import os\nprint(os.listdir('/private/tmp')[:1], 'ESCAPED')\n",
    }
    for name, code in blocked.items():
        run = adb.run_python_task(_sandbox_task(name.replace(' ', '_')), code)
        assert "ESCAPED" not in run["stdout"], (name, run)
        assert not run.get("infra_failed"), (name, run)
    assert not os.path.exists(os.path.expanduser("~/.aether_sandbox_probe"))
    assert adb.baseline_sandbox_preflight() == []
    info = adb.baseline_sandbox_info()
    assert info["mechanism"] == "sandbox-exec" and len(info["profile_sha256"]) == 64


def test_python_baseline_fails_closed_without_a_sandbox():
    real_which = adb.shutil.which
    adb.shutil.which = lambda name, *a, **k: None if name == "sandbox-exec" else real_which(name, *a, **k)
    try:
        os.environ.pop("AETHER_BENCH_UNSANDBOXED_BASELINE", None)
        run = adb.run_python_task(_sandbox_task("nosandbox", "x\n"), "print('x')\n")
        assert run.get("infra_failed") and run["returncode"] == -1 and "sandbox" in run["stderr"], run
        assert run["stdout"] == "", run  # never ran
        assert adb.baseline_sandbox_preflight(), "pre-flight must report the missing sandbox"
    finally:
        adb.shutil.which = real_which


def test_baseline_board_runs_the_sandbox_preflight_and_refuses_without_it():
    if sys.platform != "darwin":
        raise Skipped("the baseline sandbox is sandbox-exec, macOS only")
    with workdir() as tmp:
        proc, _ = fake_run(tmp, ["--python-baseline", "--skip-aether", "--task", "hello_fx", "--preflight-only"])
        assert proc.returncode == 0, proc.stderr
        assert json.loads(proc.stdout)["baseline_sandbox_preflight"] == "ok", proc.stdout
        # No sandbox-exec on PATH: the board must refuse before any model call.
        bindir = tmp / "bin"
        bindir.mkdir()
        (bindir / "python3").symlink_to(os.path.realpath(sys.executable))
        proc, report = fake_run(tmp, ["--python-baseline", "--skip-aether", "--task", "hello_fx"],
                                env={"PATH": str(bindir)})
        assert proc.returncode != 0 and "baseline sandbox pre-flight failed" in proc.stderr, proc.stderr
        assert report is None or not any(True for _ in all_cases(report, "python_baseline_results"))


def test_python_prompt_fingerprint_is_unchanged():
    # The sandbox changes how a baseline program runs, never what the model is asked.
    assert adb.prompt_template_fingerprint()["sha256"].startswith("4941abd6"), adb.prompt_template_fingerprint()


def test_infinite_loop_is_a_measured_timeout_with_a_repair_round():
    import importlib.util

    with workdir() as tmp:
        log = tmp / "prompts.jsonl"
        plan = {"initial": "//! print partial\n//! loop\n", "repair": "//! loop\n", "prompt_log": str(log)}
        proc, report = scripted_run(tmp, plan, ["--repair-attempts", "1"],
                                    [simple_task("hangs", timeout_seconds=2)])
        assert proc.returncode == 0, proc.stderr
        case = next(c for _, _, c in all_cases(report))
        first = case["attempts"][0]
        assert first["run"]["returncode"] == 124 and first["run"]["timed_out"] is True
        assert first["run"]["stdout"] == "partial\n", "partial stdout must survive the kill"
        assert first["source_code"].startswith("//! print partial"), "the source must be kept"
        assert case["generated_ok"] is True
        assert case["attempt_count"] == 2, "a repair round is issued after a timeout"
        assert case["failure_fingerprint"] == "timeout:hangs", case["failure_fingerprint"]
        prompts = [json.loads(x) for x in log.read_text().splitlines()]
        assert [p["kind"] for p in prompts] == ["initial", "repair"]
        assert "your program exceeded 2 s" in prompts[1]["prompt"]
        assert "partial" in prompts[1]["prompt"]
        # rerun_nogen_cases.py re-rolls provider events only; a timeout is a
        # measurement and must be left alone.
        spec = importlib.util.spec_from_file_location("rerun_nogen_cases", BENCH_DIR / "rerun_nogen_cases.py")
        rerun = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(rerun)
        board = tmp / "board"
        board.mkdir()
        (board / "scripted_simple.json").write_text(json.dumps(report))
        assert rerun.find_nogen(board) == []
        assert rerun.find_nogen(board, include_truncated=True) == []


def test_real_aether_infinite_loop_times_out():
    binary = real_aether_bin()
    task = adb.Task(task_id="spin", title="spin", prompt="", expected_stdout="", timeout_seconds=2)
    source = "fn main() -> Void {\n    let i: Int = 0;\n    loop i >= 0 {\n        i = i + 1;\n    }\n    ret;\n}\n"
    run = adb.compile_and_run(task, source, adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net,proc"))
    assert run["returncode"] == 124 and run["timed_out"] is True, run
    assert "diagnostics_timed_out" not in run, "the diagnostics rerun is skipped after a timeout"


def test_compile_error_stderr_and_fingerprint_carry_no_temp_path():
    with workdir() as tmp:
        plan = {"initial": "not a directive program\n"}
        proc, report = scripted_run(tmp, plan, [], [simple_task("broken")])
        assert proc.returncode == 0, proc.stderr
        case = next(c for _, _, c in all_cases(report))
        assert case["run"]["stderr"].startswith("broken.aether:1: [SYN-001]"), case["run"]["stderr"]
        assert "-doc-bench-" not in json.dumps(case)


def test_provider_error_in_a_repair_round_keeps_the_first_attempt():
    with workdir() as tmp:
        plan = {"initial": "//! print wrong\n", "fail_on": "repair"}
        proc, report = scripted_run(tmp, plan, ["--repair-attempts", "2"], [simple_task("t1")])
        assert proc.returncode in (0, 3), proc.stderr
        case = next(c for _, _, c in all_cases(report))
        assert len(case["attempts"]) == 2, case["attempts"]
        assert case["attempts"][0]["source_code"] == "//! print wrong"
        assert case["attempts"][0]["run"]["stdout"] == "wrong\n"
        assert case["attempts"][1]["generated_ok"] is False
        assert "command provider failed" in case["attempts"][1]["generation_error"]



# --------------------------------------------------------------------------- #
# W1-07: repair prompt -- bounded full source, a window round the cited line
# --------------------------------------------------------------------------- #


def _long_program(lines: int = 400, width: int = 30) -> str:
    return "\n".join(f"    let v{i:04d}: Int = {i};".ljust(width) for i in range(1, lines + 1)) + "\n"


def test_long_source_window_contains_the_cited_line():
    source = _long_program()
    assert len(source) > 10000
    run = {"returncode": 1, "stdout": "", "stderr": "", "diagnostics": [
        {"severity": "error", "code": "TYPE-001", "line": 300, "message": "bad"}]}
    feedback = adb.build_repair_feedback(source, run, source_limit=8000, feedback_limit=1200)
    shown = feedback["previous_source"]
    assert "let v0300: Int = 300;" in shown
    assert "let v0280: Int = 280;" in shown and "let v0320: Int = 320;" in shown
    assert "let v0001: Int = 1;" in shown and "let v0400: Int = 400;" in shown
    assert "lines omitted" in shown
    assert len(shown) <= 8000
    assert feedback["meta"]["source_truncated"] is True and feedback["meta"]["cited_line"] == 300
    # The repair prompt carries it, and the cited line is found from stderr too.
    stderr_run = {"returncode": 1, "stdout": "", "stderr": "t.aether:300: [TYPE-001] bad\n", "diagnostics": None}
    assert adb.cited_source_line(stderr_run) == 300
    small = adb.build_repair_feedback("fn main() -> Void { ret; }\n", run)
    assert small["meta"]["source_truncated"] is False


def test_warning_flood_collapses_to_one_line():
    flood = "t.aether:1: warning: [NARROW-001] narrowing\n" * 1024 + "t.aether:9: [SCOPE-001] identifier 'x' not in scope.\n"
    feedback = adb.build_repair_feedback("src", {"returncode": 1, "stdout": "", "stderr": flood, "diagnostics": None})
    stderr = feedback["observed_stderr"]
    assert "[repeated 1024 times]" in stderr
    assert stderr.count("NARROW-001") == 1
    assert "SCOPE-001" in stderr, "the coded error must survive the cap"
    assert feedback["meta"]["stderr_collapsed"] is True


def test_repair_attempt_records_truncation_end_to_end():
    with workdir() as tmp:
        log = tmp / "prompts.jsonl"
        long_source = "".join(f"// filler line {i}\n" for i in range(1, 500))  # >8K chars, no directives
        plan = {"initial": long_source, "repair": "//! print ok\n", "prompt_log": str(log)}
        proc, report = scripted_run(tmp, plan, ["--repair-attempts", "1"], [simple_task("big")])
        assert proc.returncode == 0, proc.stderr
        case = next(c for _, _, c in all_cases(report))
        repair = case["attempts"][1]
        assert repair["source_truncated"] is True and repair["source_cap"] == 8000
        assert "stderr_collapsed" in repair
        prompt = json.loads(log.read_text().splitlines()[1])["prompt"]
        assert "lines omitted" in prompt
        assert case["run"]["exact_stdout_match"] is True


# --------------------------------------------------------------------------- #
# W1-08: the context guard counts output and measured tokens
# --------------------------------------------------------------------------- #

# Synthetic guides sized like the real tiers in measured tokens (~29K full,
# ~15.9K medium, ~11.9K small; chars/3.4 when tiktoken is not installed).
GUIDE_CHARS = {"full": 29000 * 34 // 10, "medium": 15913 * 34 // 10, "small": 11898 * 34 // 10}


def _synthetic_guides(tmp: pathlib.Path) -> dict:
    variants = {}
    for name, chars in GUIDE_CHARS.items():
        path = tmp / f"{name}.md"
        path.write_text(f"*Guide version: 2026-09-05-1*\n" + ("word " * (chars // 5)), encoding="utf-8")
        variants[name] = path
    variants["none"] = None
    return variants


def test_context_guard_clamps_a_32k_request_and_records_it():
    with workdir() as tmp:
        guides = _synthetic_guides(tmp)
        log = tmp / "prompts.jsonl"
        plan = {"initial": "//! print ok\n", "prompt_log": str(log)}
        script = tmp / "scripted_model.py"
        script.write_text(SCRIPTED_MODEL, encoding="utf-8")
        manifest = write_json(tmp / "tasks.json", {"version": "t", "tasks": [simple_task("t1")]})
        dest = mock_destination("q32k", model_script=script, prompt_context_limit=32768, max_output_tokens=24000)
        proc, report = fake_run(tmp, ["--doc", f"medium={guides['medium']}"], tasks=manifest,
                                destinations=[dest], env={"SCRIPTED_MODEL_PLAN": json.dumps(plan)})
        assert proc.returncode == 0, proc.stderr
        case = next(c for _, _, c in all_cases(report))
        fit = case["attempts"][0]["context_fit"]
        assert fit["context_limit"] == 32768 and fit["context_source"] == "config"
        assert fit["clamped"] is True
        assert fit["max_tokens_sent"] == 32768 - fit["prompt_tokens_measured"] - 512
        assert 14000 < fit["max_tokens_sent"] < 18000, fit
        assert case["attempts"][0]["request"]["max_tokens"] == fit["max_tokens_sent"]
        sent = json.loads(log.read_text().splitlines()[0])["max_tokens"]
        assert sent == str(fit["max_tokens_sent"]), "the clamp must reach the request"
        assert report["destinations"][0]["variants"][0]["doc_tokens_o200k"] is None or \
            report["destinations"][0]["variants"][0]["doc_tokens_o200k"] > 10000


def test_context_overflow_is_not_sent_blind():
    with workdir() as tmp:
        guides = _synthetic_guides(tmp)
        log = tmp / "prompts.jsonl"
        plan = {"initial": "//! print ok\n", "prompt_log": str(log)}
        script = tmp / "scripted_model.py"
        script.write_text(SCRIPTED_MODEL, encoding="utf-8")
        manifest = write_json(tmp / "tasks.json", {"version": "t", "tasks": [simple_task("t1")]})
        dest = mock_destination("q8k", model_script=script, prompt_context_limit=8192, max_output_tokens=4000)
        proc, report = fake_run(tmp, ["--doc", f"small={guides['small']}"], tasks=manifest,
                                destinations=[dest], env={"SCRIPTED_MODEL_PLAN": json.dumps(plan)})
        case = next(c for _, _, c in all_cases(report))
        assert case["attempts"][0]["not_sent"] == "context_overflow"
        assert "context_overflow" in case["attempts"][0]["generation_error"]
        assert not log.exists(), "the model must not be called"


def test_token_counting_and_context_detection_paths():
    calls = []

    def fake_post(url, body, api_key, **kwargs):
        calls.append(url)
        return {"tokens": list(range(37))}

    def fake_get(url, api_key):
        if url.endswith("/v1/models"):
            return {"data": [{"id": "m", "max_model_len": 32768}]}
        raise RuntimeError("404")

    saved = (adb.http_json_request, adb.http_json_get)
    adb.http_json_request, adb.http_json_get = fake_post, fake_get
    try:
        local = adb.Destination(destination_id="vllm-x", kind="openai_chat_completions", model="m",
                                base_url="http://claw9:8000/v1")
        assert adb.count_prompt_tokens("hello world", local) == (37, "provider_tokenize:llama_cpp")
        assert calls == ["http://claw9:8000/tokenize"]
        assert adb.resolve_context_limit(local) == (32768, "models_api")
        cloud = adb.Destination(destination_id="cloud-x", kind="openai_chat_completions", model="m",
                                base_url="https://api.example.com/v1")
        count, method = adb.count_prompt_tokens("x" * 340, cloud)
        assert method in ("tiktoken_o200k", "chars_div_3.4") and calls == ["http://claw9:8000/tokenize"]
        if method == "chars_div_3.4":
            assert count == 100
        assert adb.resolve_context_limit(cloud) == (None, None)
        assert adb.context_limit_required(local) and not adb.context_limit_required(cloud)
        assert adb.context_limit_required(adb.Destination(destination_id="t", kind="tra_queue"))
        assert not adb.context_limit_required(adb.Destination(destination_id="c", kind="command"))
    finally:
        adb.http_json_request, adb.http_json_get = saved


def test_unknown_context_on_a_self_hosted_destination_is_refused():
    def fail_get(url, api_key):
        raise RuntimeError("unreachable")

    with workdir() as tmp:
        config = write_json(tmp / "d.json", {"destinations": [
            {"id": "lane", "type": "openai_chat_completions", "model": "m", "base_url": "http://127.0.0.1:9/v1"}]})
        # In-process so no socket is opened: the detector is stubbed to fail.
        saved = adb.http_json_get
        adb.http_json_get = fail_get
        try:
            try:
                adb.main(["--tasks", str(SMOKE_TASKS), "--destinations-config", str(config), "--docs", "none",
                          "--aether-bin", str(FAKE_AETHER), "--allow-skew", "--preflight-only"])
            except SystemExit as exc:
                assert "prompt_context_limit" in str(exc), exc
            else:
                raise AssertionError("an unknown self-hosted context must be refused")
        finally:
            adb.http_json_get = saved


def test_prompt_token_gap_is_recorded():
    attempt = {"usage": {"prompt_tokens": 1000}, "context_fit": {"prompt_tokens_measured": 1100,
                                                                 "prompt_tokens_method": "chars_div_3.4"}}
    adb.record_prompt_token_gap(attempt, adb.Destination(destination_id="gap", kind="command"))
    assert attempt["prompt_token_gap"] == 0.1


def test_miner_routes_by_measured_fit():
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    import aether_idea_miner as miner

    with workdir() as tmp:
        guides = _synthetic_guides(tmp)

        def prompt_for(name, text):
            return miner.build_generation_prompt(guide_name=name, guide_text=text, n_programs=5, avoid_intents=[])

        def pick(context, output):
            dest = adb.Destination(destination_id=f"m{context}", kind="command",
                                   prompt_context_limit=context, max_output_tokens=output)
            return miner.choose_guide_name(dest, "auto", None, guides, prompt_for_guide=prompt_for)

        assert pick(32768, 8000) == "medium"
        assert pick(8192, 2000) is None, "small (~11.9K) cannot fit 8K; never route it there"
        assert pick(131072, 8000) == "full"
        assert pick(24576, 8000) == "small"


# --------------------------------------------------------------------------- #
# W1-09: FA/FX, thesis failure classes, CIs, infra separation
# --------------------------------------------------------------------------- #


def _case(task_id, first_run, final_run=None, repeat=0, **first_extra):
    first = {"generated_ok": True, "source_code": "src", "run": first_run, **first_extra}
    attempts = [first] + ([{"generated_ok": True, "source_code": "src2", "run": final_run}] if final_run else [])
    final = attempts[-1]
    return {"task_id": task_id, "repeat_index": repeat, "attempts": attempts,
            "generated_ok": final["generated_ok"], "run": final["run"],
            "resolved_after_repair": bool(final_run and final_run["exact_stdout_match"])}


def _r(rc=0, exact=False, stdout="", stderr="", **extra):
    return {"returncode": rc, "exact_stdout_match": exact, "stdout": stdout, "stderr": stderr, **extra}


def test_summary_classifies_first_attempt_failures():
    results = [
        _case("a", _r(exact=True)),                                       # pass
        _case("b", _r(rc=0, stdout="wrong\n")),                           # silent_wrong
        _case("c", _r(rc=124, timed_out=True), _r(exact=True)),           # crash_hang, fixed
        _case("d", _r(rc=139)),                                           # crash_hang (signal)
        _case("e", _r(rc=1, stderr="t.aether:3: [FX-001] needs fx\n"), _r(exact=True)),  # coded
        _case("f", _r(rc=1, stderr="Aether @post failed in g\n")),       # uncoded
        _case("g", _r(rc=1, stderr="", diagnostics=[{"code": "SCOPE-001"}])),  # coded via diagnostics
        _case("h", _r(rc=3, stdout="x\n", expected_returncode=3)),        # silent_wrong at expected rc
    ]
    summary = adb.summarize(results)
    assert summary["first_attempt_classes"] == {
        "pass": 1, "silent_wrong": 2, "crash_hang": 2, "no_answer": 0, "uncoded_error": 1, "coded_error": 2,
        "infra_failed": 0, "not_sent": 0,
    }, summary["first_attempt_classes"]
    assert summary["first_attempt_exact"] == 1 and summary["final_exact"] == 3
    lo, hi = summary["fa_ci95"]
    assert 0 < lo < 0.125 < hi < 0.6, summary["fa_ci95"]
    assert summary["headline_ok"] is True


def test_summary_majority_flaky_and_wilson():
    results = [_case("t", _r(exact=True), repeat=0), _case("t", _r(rc=0, stdout="no"), repeat=1),
               _case("t", _r(exact=True), repeat=2), _case("u", _r(exact=True), repeat=0)]
    summary = adb.summarize(results)
    assert summary["task_majority_fa"] == 2 and summary["tasks"] == 2
    assert summary["flaky_fa"] == ["t"]
    assert summary["per_task"]["t"] == {"repeats": 3, "fa_passes": 2, "fx_passes": 2}
    assert summary["per_task_first_attempt_classes"]["t"] == {"pass": 2, "silent_wrong": 1}
    assert adb.wilson_interval(15, 15) == [0.7961, 1.0]
    assert adb.wilson_interval(0, 0) is None


def test_infra_failures_stay_in_the_denominator_and_block_the_headline():
    infra = {"task_id": "z", "repeat_index": 0, "generated_ok": False, "infra_failed": True,
             "infra_kind": "rate_limited", "generation_error": "HTTP API error 429: slow down",
             "attempts": [{"generated_ok": False, "infra_failed": True, "infra_kind": "rate_limited",
                           "run": _r(rc=-1)}], "run": _r(rc=-1)}
    legacy = {"task_id": "y", "generated_ok": False, "generation_error": "HTTP API error 402: insufficient_quota",
              "attempts": [], "run": _r(rc=-1)}
    summary = adb.summarize([_case("a", _r(exact=True)), infra, legacy])
    assert summary["total_cases"] == 3, "never dropped from the denominator"
    assert summary["infra_failed"] == 2 and summary["headline_ok"] is False
    assert summary["first_attempt_classes"]["infra_failed"] == 2
    assert "HEADLINE WITHHELD" in adb.headline_line(summary)
    for message, kind in (("HTTP API error 429: x", "rate_limited"), ("insufficient_quota", "quota"),
                          ("RESOURCE_EXHAUSTED", "quota"), ("HTTP API error 503: x", "http_5xx"),
                          ("provider request exceeded 900 seconds", "provider_timeout"),
                          ("HTTP API request failed: [Errno 61] Connection refused", "transport")):
        assert adb.classify_infra_failure(message) == kind, message
    assert adb.classify_infra_failure("t.aether:3: [FX-001] boom") is None


def test_infra_failure_exits_3_and_rerun_nogen_cases_measures_it():
    with workdir() as tmp:
        plan = {"initial": "//! print ok\n", "fail_first_n": 1, "counter": str(tmp / "count")}
        proc, report = scripted_run(tmp, plan, [], [simple_task("t1"), simple_task("t2")])
        assert proc.returncode == 3, (proc.returncode, proc.stderr)
        assert "no headline" in proc.stderr
        variant = report["destinations"][0]["variants"][0]
        assert variant["summary"]["infra_failed"] == 1 and variant["summary"]["total_cases"] == 2
        bad = [c for c in variant["results"] if c.get("infra_failed")]
        assert bad and bad[0]["infra_kind"] == "rate_limited"
        rerun = run_harness(["--report", str(tmp / "report.json"), "--apply"],
                            env={"MOCK_MODEL_FAKE_PROGRAMS": "1",
                                 "SCRIPTED_MODEL_PLAN": json.dumps(plan)},
                            script=BENCH_DIR / "rerun_nogen_cases.py")
        assert rerun.returncode == 0, rerun.stdout + rerun.stderr
        patched = json.loads((tmp / "report.json").read_text())
        variant = patched["destinations"][0]["variants"][0]
        assert variant["summary"]["infra_failed"] == 0 and variant["summary"]["headline_ok"] is True
        assert variant["summary"]["final_exact"] == 2
        fixed = [c for c in variant["results"] if c.get("rerun_of_infra_failure")]
        assert len(fixed) == 1 and fixed[0]["task_id"] == bad[0]["task_id"]


def test_finish_reason_is_stored_on_each_attempt():
    def fake_request(url, body, api_key, **kwargs):
        return {"choices": [{"message": {"content": "x"}, "finish_reason": "length"}]}

    original = adb.http_json_request
    adb.http_json_request = fake_request
    try:
        dest = adb.Destination(destination_id="l", kind="openai_chat_completions", model="m",
                               base_url="https://h/v1")
        out = adb.invoke_openai_chat_completions("p", dest)
    finally:
        adb.http_json_request = original
    assert out["finish_reason"] == "length"


def test_resummarize_ds4_board_reproduces_15_of_15_with_a_ci():
    path = BENCH_DIR / "results" / "local_tiers_20260811" / "high-ds4__tasks_frontier.json"
    proc = run_harness(["--resummarize", str(path)])
    assert proc.returncode == 0, proc.stderr
    assert "FA 15/15" in proc.stdout and "FX 15/15" in proc.stdout and "[79.6-100.0%]" in proc.stdout, proc.stdout


def test_paired_bootstrap_non_inferiority():
    import importlib.util

    spec = importlib.util.spec_from_file_location("sfm", BENCH_DIR / "summarize_full_vs_medium.py")
    sfm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(sfm)

    def report(full_pass, medium_pass):
        def variant(name, passes):
            return {"doc_name": name, "results": [
                {"task_id": f"t{i}", "run": {"exact_stdout_match": ok},
                 "attempts": [{"run": {"exact_stdout_match": ok}}]} for i, ok in enumerate(passes)]}
        return {"destinations": [{"variants": [variant("full", full_pass), variant("medium", medium_pass)]}]}

    equal = {("m", "simple"): report([True] * 30, [True] * 30)}
    mean, lo, hi = sfm.bootstrap_ci(sfm.paired_differences(equal, "fa"), 500, 1)
    assert mean == 0 and lo == 0 and hi == 0
    worse = {("m", "simple"): report([True] * 30, [True] * 20 + [False] * 10)}
    mean, lo, hi = sfm.bootstrap_ci(sfm.paired_differences(worse, "fx"), 500, 1)
    assert round(mean, 1) == -33.3 and hi < -3, (mean, lo, hi)


# --------------------------------------------------------------------------- #
# W1-21: expected exit code, stdin, hidden expected stdout
# --------------------------------------------------------------------------- #


def test_hidden_prompt_has_no_expected_block_but_repairs_do():
    with workdir() as tmp:
        manifest = write_json(tmp / "traps.json", {"version": "t", "hide_expected_stdout": True, "tasks": [
            simple_task("trap", expected="SECRET-OUTPUT\n", prompt="Print the secret word, then a newline."),
            simple_task("shown", expected="VISIBLE\n", hide_expected_stdout=False)]})
        tasks = {t.task_id: t for t in adb.load_tasks(manifest)}
    trap, shown = tasks["trap"], tasks["shown"]
    assert trap.hide_expected_stdout and not shown.hide_expected_stdout
    prompt = adb.build_prompt("medium", "guide", trap)
    assert "Expected stdout" not in prompt and "SECRET-OUTPUT" not in prompt
    assert "the output the task specifies" in prompt
    assert prompt.rstrip().endswith("\n        Aether source:") or prompt.rstrip().endswith("\nAether source:")
    assert "        Aether source:" not in prompt.replace("\n        Aether source:", "")
    assert "VISIBLE" in adb.build_prompt("medium", "guide", shown)
    repair = adb.build_repair_prompt(doc_name="medium", doc_text="g", task=trap, previous_source="s",
                                     attempt_number=1, failure_summary="f", observed_stdout="o", observed_stderr="")
    assert "Expected stdout:" in repair and "SECRET-OUTPUT" in repair, "repairs show it (D37a option b)"
    batch = adb.build_batch_prompt("medium", "guide", [trap, shown])
    assert "SECRET-OUTPUT" not in batch and "VISIBLE" in batch
    for builder in (adb.build_python_prompt, adb.build_rust_prompt):
        assert "SECRET-OUTPUT" not in builder(trap)


# The trap and scale suites were built on the W1-21 fields (hidden expected,
# exit status, stdin); every suite before them keeps its prompts byte for byte.
NEW_SCHEMA_SUITES = {"tasks_traps.json", "tasks_scale.json"}


def test_existing_suites_keep_their_prompts():
    for path in sorted(BENCH_DIR.glob("*tasks*.json")):
        if path.name in NEW_SCHEMA_SUITES:
            continue
        for task in adb.load_tasks(path):
            assert not task.hide_expected_stdout and task.expected_returncode == 0 and task.stdin is None, task.task_id
            assert task.expected_stdout in adb.build_prompt("medium", "g", task)


def test_expected_returncode_task_passes_with_exit_3():
    with workdir() as tmp:
        task = simple_task("halts", expected="bye\n", expected_returncode=3)
        plan = {"initial": "//! print bye\n//! exit 3\n"}
        proc, report = scripted_run(tmp, plan, [], [task])
        assert proc.returncode == 0, proc.stderr
        case = next(c for _, _, c in all_cases(report))
        assert case["run"]["returncode"] == 3 and case["run"]["exact_stdout_match"] is True
        assert report["destinations"][0]["variants"][0]["summary"]["fa_rate"] == 1.0
        status = adb.Task(task_id="halts", title="t", prompt="p", expected_stdout="bye\n", expected_returncode=3)
        assert "Expected exit status:" in adb.build_prompt("none", "", status)
        summary = adb.derive_failure_summary(True, {"returncode": 0, "stdout": "bye\n", "expected_returncode": 3},
                                             expected_stdout="bye\n")
        assert summary.startswith("exit_status_mismatch: the task requires exit status 3")


def test_real_aether_halt_3_passes_an_rc_3_task():
    binary = real_aether_bin()
    task = adb.Task(task_id="halts", title="t", prompt="", expected_stdout="bye\n", expected_returncode=3)
    source = 'fn main() -> Void {\n    fx {\n        println("bye");\n        halt(3);\n    }\n}\n'
    # halt is a proc-class effect: under the bench's default --deny net,proc it
    # is refused, so an exit-status task has to run with proc allowed.
    denied = adb.compile_and_run(task, source, adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net,proc"))
    assert denied["returncode"] != 3 and "denied" in denied["stderr"], denied
    run = adb.compile_and_run(task, source, adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net"))
    assert run["returncode"] == 3 and run["exact_stdout_match"] is True, run


def test_stdin_task_reads_three_lines():
    with workdir() as tmp:
        task = simple_task("filter", expected="lines=3\n", stdin="one\ntwo\nthree\n")
        proc, report = scripted_run(tmp, {"initial": "//! count-stdin\n"}, [], [task])
        assert proc.returncode == 0, proc.stderr
        assert next(c for _, _, c in all_cases(report))["run"]["exact_stdout_match"] is True
    from_file = adb.Task(task_id="f", title="f", prompt="", expected_stdout="three|two|one\n",
                         files={"in.txt": "one\ntwo\nthree\n"}, stdin={"file": "in.txt"})
    py = adb.run_python_task(from_file, "import sys\nprint('|'.join(reversed(sys.stdin.read().split())))\n")
    assert py["exact_stdout_match"] is True, py


def test_real_aether_reads_three_stdin_lines():
    binary = real_aether_bin()
    task = adb.Task(task_id="rd", title="t", prompt="", expected_stdout="three|two|one\n", stdin="one\ntwo\nthree\n")
    source = ('fn main() -> Void {\n    let a: Text = "";\n    let b: Text = "";\n    let c: Text = "";\n'
              '    fx {\n        readln(a);\n        readln(b);\n        readln(c);\n'
              '        println(c, "|", b, "|", a);\n    }\n}\n')
    run = adb.compile_and_run(task, source, adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net,proc"))
    assert run["returncode"] == 0 and "three|two|one" in run["stdout"], run
    # Not asserted exact: today's readln writes ESC[?25h to a piped stdout
    # (B-backend-coupling-4), which is exactly what a stdin trap must see.


# --------------------------------------------------------------------------- #
# W6-03: severity-aware failure summary and fingerprint
# --------------------------------------------------------------------------- #

SCOPE_ERROR = {"severity": "error", "phase": "semantic", "kind": "scope", "code": "SCOPE-001",
               "file": "w.aether", "line": 6, "message": "identifier 'printn' not in scope."}
# --diagnostics-json for a PREC-001 warning ahead of the error, as today's rea
# emits it (captured from the 2026-10-06-1 binary) ...
WARNING_FIRST_TODAY = [
    {"severity": "error", "phase": "compile", "kind": "generic", "code": None, "file": "w.aether", "line": 4,
     "message": "warning: [PREC-001] Aether precedence warning: '&' binds looser than '==', so this parses as "
                "`a & (b == c)` and produces an Int, not a Bool."},
    SCOPE_ERROR,
]
# ... and as it will once rea parses "warning: [CODE]" (W6-15).
WARNING_FIRST_W615 = [
    {"severity": "warning", "phase": "compile", "kind": "precedence", "code": "PREC-001", "file": "w.aether",
     "line": 4, "message": "Aether precedence warning: '&' binds looser than '=='."},
    SCOPE_ERROR,
]


def test_warning_first_diagnostics_summarise_the_error():
    for fixture in (WARNING_FIRST_TODAY, WARNING_FIRST_W615):
        run = {"returncode": 1, "stdout": "", "stderr": "", "diagnostics": fixture}
        assert adb.derive_failure_summary(True, run) == "SCOPE-001: identifier 'printn' not in scope."
        assert adb.derive_failure_fingerprint({"generated_ok": True, "run": run}) == "run_error_code:SCOPE-001"
        assert adb.cited_source_line(run) == 6
    # A warnings-only list still falls back to the first record.
    only = {"returncode": 1, "stdout": "", "stderr": "", "diagnostics": WARNING_FIRST_W615[:1]}
    assert adb.derive_failure_summary(True, only).startswith("PREC-001")


def test_miner_primary_diagnostic_skips_warnings():
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    import aether_idea_miner as miner

    for fixture in (WARNING_FIRST_TODAY, WARNING_FIRST_W615):
        f = miner.analyze_failure("1\n2\n3\n4\n5\nprintn(c);\n", {"returncode": 1, "stdout": "", "stderr": "",
                                                                  "diagnostics": fixture})
        assert f["code"] == "SCOPE-001" and miner.finding_key(f) == "missing:printn", f


def test_real_aether_warning_before_error():
    binary = real_aether_bin()
    task = adb.Task(task_id="w", title="w", prompt="", expected_stdout="")
    source = ("fn main() -> Void {\n    let a: Int = 6;\n    let b: Int = 3;\n    let c: Int = a & b == 2;\n"
              "    fx {\n        printn(c);\n    }\n}\n")
    run = adb.compile_and_run(task, source, adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net,proc"))
    assert run["diagnostics"] and "PREC-001" in json.dumps(run["diagnostics"][0]), run["diagnostics"]
    assert adb.derive_failure_summary(True, run) == "SCOPE-001: identifier 'printn' not in scope."
    assert adb.derive_failure_fingerprint({"generated_ok": True, "run": run}) == "run_error_code:SCOPE-001"


# --------------------------------------------------------------------------- #
# W1-10: the oracle lap
# --------------------------------------------------------------------------- #


def test_sliding_window_expected_matches_a_python_reference():
    task = next(t for t in json.loads((BENCH_DIR / "tasks_frontier_algo.json").read_text())["tasks"]
                if t["id"] == "algo_sliding_window_max")
    xs, k = [1, 3, -1, -3, 5, 3, 6, 7], 3
    maxes = [max(xs[i:i + k]) for i in range(len(xs) - k + 1)]
    python_expected = "maxes:" + "".join(f" {m}" for m in maxes) + f"\nwindows={len(maxes)}\n"
    assert python_expected == "maxes: 3 3 5 5 6 7\nwindows=6\n"
    assert task["expected_stdout"] == python_expected
    assert "loop t in 0..pending" in task["reference_solution"], "the reference must hoist the bound"
    assert json.loads((BENCH_DIR / "tasks_frontier_algo.json").read_text())["version"] != "2026-08-10-1"


def test_list_tasks_works_on_all_14_manifests():
    manifests = sorted(BENCH_DIR.glob("*tasks*.json"))
    assert len(manifests) == 14, [m.name for m in manifests]
    for manifest in manifests:
        proc = run_harness(["--tasks", str(manifest), "--list-tasks"])
        assert proc.returncode == 0 and proc.stdout.strip(), (manifest.name, proc.stderr)
    assert adb.DEFAULT_TASKS.name == "tasks_v2_pos.json"
    ids = [t.task_id for t in adb.load_tasks(BENCH_DIR / "tasks_v2.json")]
    assert "effect_boundary_reject" not in ids, "should_fail entries are not tasks"
    assert "module_toon_report" not in [t.task_id for t in adb.load_tasks(BENCH_DIR / "tasks.json")], "D37c"


def test_oracle_lint_only_needs_no_binary():
    proc = run_harness(["--lint-only"], script=REPO_ROOT / "tools" / "aether_oracle_check.py")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "14 manifests, 0 problem(s)" in proc.stdout


def test_oracle_check_passes_on_the_real_binary_quickly():
    binary = real_aether_bin()
    with workdir() as tmp:
        out = tmp / "oracle.json"
        proc = run_harness(["--aether-bin", str(binary), "--quiet", "--report-json", str(out)],
                           script=REPO_ROOT / "tools" / "aether_oracle_check.py")
        assert proc.returncode == 0, proc.stdout + proc.stderr
        report = json.loads(out.read_text())
        assert report["references_passed"] == report["references_total"] == 192
        assert report["python_references_passed"] == report["python_references_total"] == PY_CHECKED
        assert report["board_agreement"]["agreeing"] == report["board_agreement"]["tasks"] == 114
        assert sum(n["ok"] for n in report["negatives"]) == len(report["negatives"]) == 4
        assert report["sandbox_probe"]["ok"] is True
        # The target is under 10 s on an idle Release build. A loaded machine or an
        # -O0 build runs 2-3x slower, so only a runaway fails the test.
        if report["elapsed_seconds"] >= 10:
            print(f"note: oracle check took {report['elapsed_seconds']:.1f} s (target < 10 s)")
        assert report["elapsed_seconds"] < 60, report["elapsed_seconds"]


def test_harness_preflight_aborts_on_a_broken_reference():
    with workdir() as tmp:
        good = simple_task("good", reference_solution="//! print ok\n")
        broken = simple_task("broken", reference_solution="//! print WRONG\n")
        plan = {"initial": "//! print ok\n"}
        proc, report = scripted_run(tmp, plan, [], [good, broken])
        assert proc.returncode != 0 and "oracle pre-flight failed" in proc.stderr, proc.stderr
        assert "broken" in proc.stderr and report is None
        proc, report = scripted_run(tmp, plan, ["--allow-broken-oracle"], [good, broken])
        assert proc.returncode == 0, proc.stderr
        assert report["oracle"]["broken"] == ["broken"] and report["oracle"]["sandbox_probe"]["ok"] is True
        oracle = {c["task_id"]: c["oracle_ok"] for _, _, c in all_cases(report)}
        assert oracle == {"good": True, "broken": False}


# --------------------------------------------------------------------------- #
# W1-11: an independent oracle for every board task
# --------------------------------------------------------------------------- #

# Python references run once per manifest that carries the task id: the 114
# board tasks, the traps, and the ids tasks.json / tasks_v2*.json / tasks_hard /
# smoke share (identical tasks) -- the default lap skips the scale suite.
PY_CHECKED = 210


def test_every_board_task_has_both_references():
    import aether_oracle_check as oracle_check

    count = 0
    for name in oracle_check.BOARD_MANIFESTS:
        for task in adb.load_tasks(BENCH_DIR / name):
            count += 1
            assert task.reference_solution, (name, task.task_id)
            assert (BENCH_DIR / "py_refs" / f"{task.task_id}.py").is_file(), (name, task.task_id)
    assert count == 114, count
    assert sum(1 for t in adb.load_tasks(BENCH_DIR / "tasks_cs.json") if t.reference_solution) == 19


def test_board_python_references_agree_without_a_compiler():
    import aether_oracle_check as oracle_check

    bad = {}
    for name in oracle_check.BOARD_MANIFESTS:
        for task_id, result in oracle_check.check_python_references(adb.load_tasks(BENCH_DIR / name)).items():
            if not result["ok"]:
                bad[f"{name}:{task_id}"] = result["detail"]
    assert not bad, bad
    ambiguities = json.loads((BENCH_DIR / "py_refs" / "ambiguities.json").read_text())["tasks"]
    assert {a["id"] for a in ambiguities if a["disagreed"]} == {"eligibility_bool_logic", "spec_tokenizer_positions"}


# --------------------------------------------------------------------------- #
# W1-22: the silent-wrong trap suite
# --------------------------------------------------------------------------- #

TRAPS = BENCH_DIR / "tasks_traps.json"


def test_trap_suite_shape():
    raw = json.loads(TRAPS.read_text())
    items = raw["tasks"]
    assert raw["hide_expected_stdout"] is True and raw["version"]
    assert 20 <= len(items) <= 25, len(items)
    for item in items:
        assert item["id"].startswith("trap_"), item["id"]
        trap = item["trap"]
        assert trap["natural_program"] and trap["class"] and trap["fix"], item["id"]
        assert (BENCH_DIR / "py_refs" / f"{item['id']}.py").is_file(), item["id"]
        assert item["reference_solution"], item["id"]
        assert "Expected stdout" not in adb.build_prompt("none", "", adb.load_tasks(TRAPS)[0])
    # At least 70% of the traps are silent rc-0 shapes on the frozen B0/B1 binary.
    frozen = [item["trap"]["observed"]["2026-10-06-1"] for item in items]
    assert sum(c == "silent_wrong" for c in frozen) >= 0.7 * len(items), frozen
    assert all(c != "pass" for c in frozen), frozen
    # Every task is hidden on the first attempt.
    assert all(t.hide_expected_stdout for t in adb.load_tasks(TRAPS))


def test_trap_python_references_match_without_a_compiler():
    import aether_oracle_check as oracle_check

    results = oracle_check.check_python_references(adb.load_tasks(TRAPS))
    assert len(results) == 24 and all(r["ok"] for r in results.values()), \
        {k: v["detail"] for k, v in results.items() if not v["ok"]}


def test_sandbox_allow_narrows_only_its_own_task():
    args = adb.argparse.Namespace(sandbox_deny="net,proc", aether_args=[])
    plain = adb.Task(task_id="a", title="a", prompt="", expected_stdout="")
    exits = adb.Task(task_id="b", title="b", prompt="", expected_stdout="", expected_returncode=2,
                     sandbox_allow=("proc",))
    assert adb.aether_flags(args, plain) == ["--deny", "net,proc"]
    assert adb.aether_flags(args, exits) == ["--deny", "net"]
    assert adb.aether_flags(args) == ["--deny", "net,proc"]
    task = next(t for t in adb.load_tasks(TRAPS) if t.task_id == "trap_exit_status")
    assert task.sandbox_allow == ("proc",) and task.expected_returncode == 2
    import aether_oracle_check as oracle_check

    with workdir() as tmp:
        bad = write_json(tmp / "tasks_bad.json", {"version": "t", "tasks": [simple_task("x", sandbox_allow=["net"])]})
        assert any("sandbox_allow" in p for p in oracle_check.lint_manifest(bad))


def test_rc0_on_a_failure_status_task_is_silent_wrong():
    # trap_exit_status on 2026-10-06-1: exit(2) returned from the helper, the
    # program carried on and exited 0 -- a success status, wrong result.
    attempt = {"generated_ok": True, "source_code": "x",
               "run": {"returncode": 0, "expected_returncode": 2, "exact_stdout_match": False, "stderr": ""}}
    assert adb.classify_attempt(attempt) == "silent_wrong"
    attempt["run"]["returncode"] = 1
    assert adb.classify_attempt(attempt) == "uncoded_error"


def test_real_aether_trap_programs_match_their_recorded_classes():
    binary = real_aether_bin()
    version, _ = adb.capture_aether_version(binary)
    items = json.loads(TRAPS.read_text())["tasks"]
    if not any(version in (item["trap"].get("observed") or {}) for item in items):
        skip(f"no trap classes recorded for aether {version}")
    import aether_oracle_check as oracle_check

    args = adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net,proc", aether_args=[])
    results = oracle_check.check_traps(TRAPS, args, version)
    drift = {r["id"]: (r["recorded"], r["class"]) for r in results if r["recorded"] != r["class"]}
    assert not drift, drift


# --------------------------------------------------------------------------- #
# W1-23: the performance/scale tier
# --------------------------------------------------------------------------- #

SCALE = BENCH_DIR / "tasks_scale.json"


def test_scale_suite_shape_and_timeouts():
    raw = json.loads(SCALE.read_text())
    items = raw["tasks"]
    assert len(items) == 8 and raw["version"]
    for item in items:
        assert item["id"].startswith("scale_") and item["reference_solution"], item["id"]
        assert (BENCH_DIR / "py_refs" / f"{item['id']}.py").is_file(), item["id"]
        scale = item["scale"]
        assert scale["natural_program"] and scale["stresses"], item["id"]
        ref = max(scale["timing"]["reference_seconds"].values())
        assert item["timeout_seconds"] == max(20, math.ceil(10 * ref)), (item["id"], ref)
    # The instrument has to see the eager-copy / quadratic paths today.
    frozen = [i["scale"]["observed"].get("2026-10-06-1", {}).get("returncode") for i in items]
    assert frozen.count(124) >= 3, frozen
    sizes = {name: len(text) for t in adb.load_tasks(SCALE) for name, text in (t.files or {}).items()}
    assert sizes["orders.json"] > 4_500_000 and sizes["corpus.txt"] >= 100_000, sizes


def test_generated_files_are_deterministic_and_sha_checked():
    sys.path.insert(0, str(BENCH_DIR))
    import scale_inputs

    spec = {"generator": "word_stream", "seed": 3, "words": 50, "keys": 20}
    assert scale_inputs.generate(spec) == scale_inputs.word_stream(3, 50, 20)
    digest = scale_inputs.sha256_text(scale_inputs.generate(spec))
    with workdir() as tmp:
        good = simple_task("g", expected="x\n", generated_files={"w.txt": {**spec, "sha256": digest}})
        bad = simple_task("b", expected="x\n", generated_files={"w.txt": {**spec, "sha256": "0" * 64}})
        tasks = adb.load_tasks(write_json(tmp / "tasks_ok.json", {"version": "t", "tasks": [good]}))
        assert tasks[0].files["w.txt"] == scale_inputs.generate(spec)
        try:
            adb.load_tasks(write_json(tmp / "tasks_bad.json", {"version": "t", "tasks": [bad]}))
        except SystemExit as exc:
            assert "sha256" in str(exc)
        else:
            raise AssertionError("a generated file with the wrong sha256 must not load")
        import aether_oracle_check as oracle_check

        unpinned = simple_task("u", expected="x\n", generated_files={"w.txt": spec})
        problems = oracle_check.lint_manifest(write_json(tmp / "tasks_u.json", {"version": "t", "tasks": [unpinned]}))
        assert any("sha256" in p for p in problems), problems


def test_scale_python_references_match():
    import aether_oracle_check as oracle_check

    results = oracle_check.check_python_references(adb.load_tasks(SCALE))
    assert len(results) == 8 and all(r["ok"] for r in results.values()), \
        {k: v["detail"] for k, v in results.items() if not v["ok"]}


def test_oracle_default_lap_skips_the_slow_scale_references():
    import aether_oracle_check as oracle_check

    assert "tasks_scale.json" in oracle_check.SLOW_MANIFESTS
    assert SCALE in oracle_check.default_manifests()


def test_replay_task_keeps_sandbox_allow_and_generated_files():
    sys.path.insert(0, str(REPO_ROOT / "tools"))
    import replay_bench

    raw = next(t for t in json.loads(TRAPS.read_text())["tasks"] if t["id"] == "trap_exit_status")
    assert replay_bench.to_task(raw).sandbox_allow == ("proc",)
    raw = next(t for t in json.loads(SCALE.read_text())["tasks"] if t["id"] == "scale_text_scan")
    assert len(replay_bench.to_task(raw).files["corpus.txt"]) >= 100_000
    assert "sandbox_allow" in replay_bench.GRADING_FIELDS and "generated_files" in replay_bench.GRADING_FIELDS


# --------------------------------------------------------------------------- #
# W1-20: first-attempt failure histogram
# --------------------------------------------------------------------------- #

HISTOGRAM = REPO_ROOT / "tools" / "aether_failure_histogram.py"


def _hist_case(task_id: str, rc: int, stdout: str, stderr: str = "", diagnostics=None, timed_out=False,
               exact=False, expected_rc=0, source="fn main() -> Void { ret; }\n") -> dict:
    run = {"returncode": rc, "stdout": stdout, "stderr": stderr, "diagnostics": diagnostics,
           "exact_stdout_match": exact, "timed_out": timed_out, "expected_returncode": expected_rc}
    first = {"generated_ok": True, "source_code": source, "run": run}
    return {"task_id": task_id, "repeat_index": 0, "generated_ok": True, "source_code": source,
            "run": run, "attempts": [first]}


def test_failure_histogram_buckets_first_attempts():
    with workdir() as tmp:
        tasks = write_json(tmp / "tasks_h.json", {"version": "h-1", "tasks": [
            simple_task("num", expected="avg = 3\n"), simple_task("miss", expected="a\nb\nc\n"),
            simple_task("code", expected="x\n"), simple_task("unc", expected="x\n"),
            simple_task("slow", expected="x\n"), simple_task("ok", expected="x\n"),
            simple_task("status", expected="bye\n", expected_returncode=2)]})
        scope = [{"severity": "warning", "code": "PREC-001", "message": "warning: precedence"},
                 {"severity": "error", "code": "SCOPE-001", "message": "identifier 'sum' not in scope", "line": 1}]
        cases = [
            _hist_case("num", 0, "avg = 3.500000\n"), _hist_case("miss", 0, "a\nc\n"),
            _hist_case("code", 1, "", "/tmp/aether-doc-bench-x/code.aether:1: [SCOPE-001] boom", scope),
            _hist_case("unc", 1, "", "/tmp/aether-doc-bench-y/unc.aether:7: Runtime Error: index 12 out of range"),
            _hist_case("slow", 124, "", timed_out=True), _hist_case("ok", 0, "x\n", exact=True),
            _hist_case("status", 0, "bye\n", expected_rc=2),
        ]
        report = {"tasks_file": str(tasks), "aether_version": "2026-10-07-1",
                  "guides": {"medium": {"version": "2026-09-05-1"}},
                  "destinations": [{"destination_id": "d1", "variants": [
                      {"doc_name": "medium", "results": cases}, {"doc_name": "none", "results": cases[:3]}]}]}
        write_json(tmp / "report.json", report)
        out_md, out_json = tmp / "h.md", tmp / "h.json"
        proc = run_harness([str(tmp), "--out-md", str(out_md), "--out-json", str(out_json)], script=HISTOGRAM)
        assert proc.returncode == 0, proc.stderr
        result = json.loads(out_json.read_text())
        medium = next(g for g in result["groups"] if g["variant"] == "medium")
        assert (medium["guide_stamp"], medium["aether_version"], medium["attempts"]) == ("2026-09-05-1", "2026-10-07-1", 7)
        buckets = {(b["class"], b["bucket"]): b["tasks"] for b in medium["buckets"]}
        assert buckets[("silent_wrong", "stdout:numeric_format")] == ["num"], buckets
        assert buckets[("silent_wrong", "stdout:missing_line")] == ["miss"]
        assert buckets[("silent_wrong", "stdout:exit_status")] == ["status"]
        assert buckets[("coded_error", "SCOPE-001")] == ["code"], "the error, not the warning before it"
        assert buckets[("uncoded_error", "uncoded: <path>:N: Runtime Error: index N out of range")] == ["unc"], buckets
        assert buckets[("crash_hang", "timeout")] == ["slow"]
        text = out_md.read_text()
        assert "## medium @ 2026-09-05-1 on aether 2026-10-07-1" in text and "/tmp/" not in text
        proc = run_harness([str(tmp / "report.json"), "--doc", "none", "--by-construct", "--out-json", str(out_json)],
                           script=HISTOGRAM)
        assert proc.returncode == 0, proc.stderr
        none = json.loads(out_json.read_text())["groups"]
        assert [g["variant"] for g in none] == ["none"]
        tags = {b["bucket"] for b in none[0]["buckets"]}
        assert "missing: sum" in tags, tags


def test_no_doc_names_the_missing_triage_tool():
    for path in [REPO_ROOT / "Tests" / "aether_specialization" / "README_corpus_structure.md",
                 BENCH_DIR / "README.md"]:
        assert "none_fail_detail" not in path.read_text(), path


# --------------------------------------------------------------------------- #
# W1-16: paired replay with declared-break waivers
# --------------------------------------------------------------------------- #

REPLAY = REPO_ROOT / "tools" / "replay_bench.py"


def _replay_fixture(tmp: pathlib.Path) -> pathlib.Path:
    manifest = write_json(tmp / "tasks.json", {"version": "r-1", "tasks": [simple_task("t1", expected="hello\n")]})
    source = "//! print hello\n"
    report = {
        "tasks_file": str(manifest), "tasks_version": "r-1", "aether_version": "2026-01-01-1+abc1234",
        "destinations": [{"destination_id": "m", "variants": [{"doc_name": "medium", "results": [{
            "task_id": "t1", "repeat_index": 0, "generated_ok": True,
            "attempts": [{"generated_ok": True, "source_code": source,
                          "run": {"returncode": 0, "stdout": "hello\n", "stderr": "", "exact_stdout_match": True}}],
        }]}]}],
    }
    (tmp / "board").mkdir()
    return write_json(tmp / "board" / "r.json", report)


def test_replay_gate_fails_on_a_broken_arm_and_honours_waivers():
    import hashlib as _h

    with workdir() as tmp:
        report = _replay_fixture(tmp)
        out = tmp / "replay.json"
        base = ["--aether-bin", str(FAKE_AETHER), str(report), "--report-json", str(out)]
        same = run_harness(base + ["--waivers", str(tmp / "none.json")], script=REPLAY)
        assert same.returncode == 0, same.stdout + same.stderr
        assert json.loads(out.read_text())["tallies"]["first"]["new_pass"] == 1
        broken = run_harness(base + ["--aether-arg=--fake-flip", "--waivers", str(tmp / "none.json")], script=REPLAY)
        assert broken.returncode == 1 and "UNWAIVED pass->fail" in broken.stdout, broken.stdout
        sha = _h.sha256(b"//! print hello\n").hexdigest()
        for version, expect in (("2026-10-06-1", 0), ("2025-12-31-1", 1), ("2026-12-01-1", 1)):
            write_json(tmp / "w.json", {"schema": 1, "waivers": {version: {
                "reason": "declared", "entries": [{"task_id": "t1", "source_sha256": sha}]}}})
            proc = run_harness(base + ["--aether-arg=--fake-flip", "--waivers", str(tmp / "w.json")], script=REPLAY)
            assert proc.returncode == expect, (version, proc.stdout)


def test_tracked_waiver_file_is_valid():
    proc = run_harness(["--check-waivers"], script=REPLAY)
    assert proc.returncode == 0, proc.stdout
    data = json.loads((BENCH_DIR / "replay_waivers.json").read_text())
    assert set(data["waivers"]) == {"2026-07-26-1", "2026-08-09-1"}


def test_instrument_check_lint_only_runs_without_a_binary():
    proc = subprocess.run(["bash", str(REPO_ROOT / "tools" / "instrument_check.sh"), "--lint-only"],
                          cwd=str(REPO_ROOT), capture_output=True, text=True, timeout=120)
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_tracked_replay_reproduces_242_to_245_with_every_regression_waived():
    binary = real_aether_bin()
    with workdir() as tmp:
        out = tmp / "replay.json"
        proc = run_harness(["--aether-bin", str(binary), "--report-json", str(out)], script=REPLAY, timeout=600)
        assert proc.returncode == 0, proc.stdout + proc.stderr
        result = json.loads(out.read_text())
        first = result["tallies"]["first"]
        assert (first["attempts"], first["old_pass"], first["new_pass"]) == (274, 242, 245), first
        assert (first["fail_to_pass"], first["pass_to_fail"]) == (11, 8), first
        assert result["unwaived"] == []
        assert sum(1 for r in result["waived"] if r["kind"] == "first" and r["transition"] == "pass->fail") == 8


def _seed_log_calls(log: pathlib.Path) -> list[tuple[str, str]]:
    return [(json.loads(x)["task_ids"][0], json.loads(x)["seed"]) for x in log.read_text().splitlines()]


def test_resume_runs_only_missing_and_infra_failed_cases():
    with workdir() as tmp:
        log = tmp / "seeds.jsonl"
        argv = ["--repeats", "2", "--seed-base", "42", "--task", "hello_fx", "--task", "classify_scores"]
        proc, report = fake_run(tmp, argv, env={"MOCK_SEED_LOG": str(log)})
        assert proc.returncode == 0, proc.stderr
        assert len(list(all_cases(report))) == 4
        # A crash after three cases: the checkpoint lost the fourth, and one of the
        # three it kept measured nothing.
        results = report["destinations"][0]["variants"][0]["results"]
        results.sort(key=lambda c: c["case_sequence"])
        lost = results.pop()
        infra = results[0]
        infra["infra_failed"] = True
        (tmp / "report.json").write_text(json.dumps(report), encoding="utf-8")
        log.unlink()
        proc, resumed = fake_run(tmp, argv + ["--resume"], env={"MOCK_SEED_LOG": str(log)})
        assert proc.returncode == 0, proc.stderr
        assert "[resume]" in proc.stderr and "keeping 2 finished case(s)" in proc.stderr, proc.stderr
        rerun = sorted(_seed_log_calls(log))
        want = sorted((c["task_id"], str(c["seed"])) for c in (lost, infra))
        assert rerun == want, (rerun, want)
        cases = list(all_cases(resumed))
        assert sorted((c["task_id"], c["repeat_index"]) for _, _, c in cases) == sorted(
            (t, r) for t in ("hello_fx", "classify_scores") for r in (0, 1))
        assert not any(c.get("infra_failed") for _, _, c in cases)
        sequences = [c["case_sequence"] for _, _, c in cases]
        assert len(set(sequences)) == len(sequences), sequences
        assert resumed["created_at_unix"] == report["created_at_unix"]
        entry = resumed["resumed"][-1]
        assert entry["kept_cases"] == 2 and entry["dropped_infra_cases"] == 1, entry
        # Resuming a finished report runs nothing.
        log.unlink()
        proc, again = fake_run(tmp, argv + ["--resume"], env={"MOCK_SEED_LOG": str(log)})
        assert proc.returncode == 0, proc.stderr
        assert not log.exists() or not log.read_text().strip()
        assert len(list(all_cases(again))) == 4 and len(again["resumed"]) == 2


def test_resume_refuses_a_report_from_another_configuration():
    with workdir() as tmp:
        argv = ["--seed-base", "42", "--task", "hello_fx"]
        proc, _ = fake_run(tmp, argv)
        assert proc.returncode == 0, proc.stderr
        proc, _ = fake_run(tmp, ["--seed-base", "7", "--task", "hello_fx", "--resume"])
        assert proc.returncode != 0
        assert "was not made by this configuration" in proc.stderr and "run_config.seed_base" in proc.stderr, proc.stderr
        proc, _ = fake_run(tmp, argv + ["--resume"], destinations=[mock_destination(model="other-model")])
        assert proc.returncode != 0 and "destination mock" in proc.stderr, proc.stderr


def test_resume_without_a_report_starts_fresh_and_needs_output_json():
    with workdir() as tmp:
        proc, report = fake_run(tmp, ["--task", "hello_fx", "--resume"])
        assert proc.returncode == 0, proc.stderr
        assert "starting a fresh run" in proc.stderr
        assert len(list(all_cases(report))) == 1 and "resumed" not in report
        proc = run_harness(["--task", "hello_fx", "--resume", "--allow-skew", "--docs", "none",
                            "--aether-bin", str(FAKE_AETHER)])
        assert proc.returncode != 0 and "--resume needs --output-json" in proc.stderr, proc.stderr


def test_release_posts_the_model_to_the_queue_and_skips_other_kinds():
    import http.server
    import threading

    seen: list[dict] = []

    class Queue(http.server.BaseHTTPRequestHandler):
        def do_POST(self):  # noqa: N802
            body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
            seen.append({"path": self.path, "body": body})
            reply = json.dumps({"model": body["model"], "targets": [
                {"target": "m5_remote", "model": body["model"],
                 "released": [{"model": body["model"], "instance_id": "i1"}]}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)

        def log_message(self, *args):
            pass

    server = http.server.HTTPServer(("127.0.0.1", 0), Queue)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        dest = adb.Destination(destination_id="q", kind="tra_queue", model="qwen/qwen3.6-35b-a3b",
                               base_url=f"http://127.0.0.1:{server.server_port}",
                               preferred_targets=["m5_remote"])
        reply = adb.release_destination_model(dest)
        assert seen == [{"path": "/api/llm/release", "body": {
            "model": "qwen/qwen3.6-35b-a3b", "submitter": "aether_doc_bench", "targets": ["m5_remote"]}}], seen
        assert reply["targets"][0]["released"][0]["instance_id"] == "i1"
    finally:
        server.shutdown()
    # An unreachable queue is reported, never raised: the run's results stand.
    dead = adb.Destination(destination_id="q", kind="tra_queue", model="m", base_url="http://127.0.0.1:9")
    assert "error" in adb.release_destination_model(dead)
    assert "skipped" in adb.release_destination_model(adb.Destination(destination_id="c", kind="command"))


def test_each_destination_records_its_release_unless_kept_loaded():
    with workdir() as tmp:
        proc, report = fake_run(tmp, ["--task", "hello_fx"])
        assert proc.returncode == 0, proc.stderr
        assert "skipped" in report["destinations"][0]["model_release"]
        proc, report = fake_run(tmp, ["--task", "hello_fx", "--keep-loaded"])
        assert proc.returncode == 0, proc.stderr
        assert "model_release" not in report["destinations"][0]


def _no_answer_attempt(**extra):
    attempt = {"generated_ok": False, "source_code": "", "finish_reason": "length",
               "run": {"returncode": -1, "stdout": "", "stderr": "", "exact_stdout_match": False}}
    attempt.update(extra)
    return attempt


def test_cap_hit_without_a_program_is_no_answer_not_infra():
    # An older record tagged the cap hit as infra (empty_output); it is still a verdict.
    attempt = _no_answer_attempt(infra_failed=True, infra_kind="empty_output")
    assert adb.classify_attempt(attempt) == "no_answer"
    case = {"task_id": "t", "generated_ok": False, "infra_failed": True, "attempts": [attempt],
            "run": attempt["run"]}
    assert not adb.case_is_infra_failed(case)
    summary = adb.summarize([case])
    assert summary["first_attempt_classes"]["no_answer"] == 1, summary["first_attempt_classes"]
    assert summary["first_attempt_classes"]["infra_failed"] == 0
    assert summary["headline_ok"] and summary["first_attempt_exact"] == 0
    # An empty reply that did NOT hit the cap is still infra.
    plain = _no_answer_attempt(finish_reason="stop", infra_failed=True, infra_kind="empty_output")
    assert adb.classify_attempt(plain) == "infra_failed"


def test_no_answer_error_makes_a_no_answer_attempt_with_repair_feedback():
    exc = adb.NoAnswerError("no answer: still generating", "deadline", {"run_seconds": 8800.0})
    attempt = adb.failed_generation_attempt("initial", "aether", exc, None)
    assert attempt["no_answer"] == "deadline" and not attempt.get("infra_failed"), attempt
    assert attempt["no_answer_detail"]["run_seconds"] == 8800.0
    assert adb.classify_attempt(attempt) == "no_answer"
    summary = adb.derive_failure_summary(False, attempt["run"], generation_error=attempt["generation_error"])
    assert summary.startswith("no_answer:") and "[" not in summary, summary


def test_no_answer_first_attempt_goes_to_repair():
    task = adb.Task(task_id="t", title="t", prompt="p", expected_stdout="ok\n")
    args = adb.argparse.Namespace(repair_attempts=2, repair_feedback_limit=1200, repair_source_limit=8000)
    calls, summaries = [], []
    passing = {"generated_ok": True, "source_code": "print ok", "finish_reason": "stop",
               "run": {"returncode": 0, "stdout": "ok\n", "stderr": "", "exact_stdout_match": True}}

    def fake_evaluate(prompt, prompt_kind, destination, task, args, runner="aether", options=None):
        calls.append(prompt_kind)
        if len(calls) == 1:
            raise adb.NoAnswerError("no answer: cap", "length")
        return dict(passing)

    def builder(**kwargs):
        summaries.append(kwargs["failure_summary"])
        return "repair prompt"

    saved = adb.evaluate_attempt
    adb.evaluate_attempt = fake_evaluate
    try:
        case = adb.execute_case(initial_prompt="p", destination=None, task=task, args=args,
                                runner="aether", repair_prompt_builder=builder)
    finally:
        adb.evaluate_attempt = saved
    assert calls == ["initial", "repair"], calls
    assert case["attempts"][0]["no_answer"] == "length"
    assert case["resolved_after_repair"] and not case["infra_failed"], case
    assert summaries and summaries[0].startswith("no_answer:"), summaries


def test_deadline_verdict_needs_a_running_job_for_most_of_the_deadline():
    from datetime import datetime, timedelta, timezone
    now = datetime.now(timezone.utc)
    replies = {}
    saved = adb.http_json_get
    adb.http_json_get = lambda url, key: replies["status"]
    try:
        replies["status"] = {"status": "running", "started_at": (now - timedelta(seconds=9000)).isoformat()}
        verdict = adb._deadline_verdict(("http://q", "job1"), 8900)
        assert verdict and verdict["job_id"] == "job1" and verdict["run_seconds"] >= 8999, verdict
        replies["status"] = {"status": "pending", "started_at": None}        # queued: infra
        assert adb._deadline_verdict(("http://q", "job1"), 8900) is None
        replies["status"] = {"status": "running", "started_at": (now - timedelta(seconds=600)).isoformat()}
        assert adb._deadline_verdict(("http://q", "job1"), 8900) is None     # started late: infra
        assert adb._deadline_verdict(None, 8900) is None                     # no job reported
    finally:
        adb.http_json_get = saved


def test_rerun_nogen_never_reruns_no_answer():
    import importlib.util
    spec = importlib.util.spec_from_file_location("rerun_nogen_cases_t", BENCH_DIR / "rerun_nogen_cases.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    no_answer = _no_answer_attempt(no_answer="deadline")
    assert not mod.needs_rerun({"generated_ok": False, "attempts": [no_answer], "run": no_answer["run"]})
    infra = _no_answer_attempt(finish_reason=None, infra_failed=True, infra_kind="provider_timeout")
    assert mod.needs_rerun({"generated_ok": False, "attempts": [infra], "run": infra["run"]})


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
