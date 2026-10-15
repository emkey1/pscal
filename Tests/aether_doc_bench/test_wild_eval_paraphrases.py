#!/usr/bin/env python3
"""Offline tests for --paraphrases in tools/aether_wild_eval.py.

Each wild-eval task can be asked in N wordings of the same request; score mode
then reports how often the pass/fail verdict changes between wordings. No
model and no compiler here: query_model and run_aether are replaced, so the
tests pin the contract -- N=1 is the original prompt and output, every wording
states the values and format the one reference answer needs, and the report
counts calls and consistent tasks correctly.

Run standalone:  python3 Tests/aether_doc_bench/test_wild_eval_paraphrases.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import contextlib
import io
import json
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_wild_eval as w  # noqa: E402


def run_main(argv: list[str]) -> tuple[int, str]:
    out = io.StringIO()
    old = sys.argv
    sys.argv = ["aether_wild_eval.py", *argv]
    code = 0
    try:
        with contextlib.redirect_stdout(out):
            w.main()
    except SystemExit as exc:
        code = int(exc.code or 0)
    finally:
        sys.argv = old
    return code, out.getvalue()


def test_one_wording_is_the_original_task():
    a, b = w.generate(15, 7), w.generate(15, 7, paraphrases=1)
    assert json.dumps(a) == json.dumps(b)
    assert all("paraphrases" not in t and "facts" not in t for t in a)


def test_more_wordings_keep_the_same_tasks():
    one = w.generate(15, 9)
    many = w.generate(15, 9, paraphrases=w.MAX_PARAPHRASES)
    for a, b in zip(one, many):
        assert b["paraphrases"][0] == a["prompt"] == b["prompt"]
        assert {k: v for k, v in b.items() if k not in ("paraphrases", "facts")} == a
        assert len(set(b["paraphrases"])) == w.MAX_PARAPHRASES


def test_every_wording_states_every_fact():
    for seed in range(30):
        for t in w.generate(10, seed, paraphrases=w.MAX_PARAPHRASES):
            for wording in t["paraphrases"]:
                assert w.missing_facts(wording, t["facts"]) == [], (seed, t["id"], wording)


def test_missing_facts_matches_whole_numbers():
    assert w.missing_facts("values 152, 7", ["52"]) == ["52"]
    assert w.missing_facts("values -52, 7", ["52"]) == ["52"]
    assert w.missing_facts("values 52, 7", ["52", "7"]) == []
    assert w.missing_facts("print it", ["sum = <value>"]) == ["sum = <value>"]


def test_score_reports_calls_and_consistency():
    tasks = w.generate(5, 7, paraphrases=3)
    expected = {t["prompt"]: t["expected_stdout"] for t in tasks}
    # Pass on the original wording only for the first two templates (mean,
    # sum); pass on every wording for the rest.
    stable = {t["id"] for t in tasks[2:]}
    by_prompt = {}
    for t in tasks:
        for i, p in enumerate(t["paraphrases"]):
            by_prompt[p] = t["expected_stdout"] if (i == 0 or t["id"] in stable) else "wrong\n"
    calls = []
    orig_q, orig_r = w.query_model, w.run_aether
    w.query_model = lambda prompt, endpoint, model: calls.append(prompt) or by_prompt[prompt]
    w.run_aether = lambda code, with_stderr=False: (0, code + "\n")  # sanitize strips the newline
    try:
        code, out = run_main(["--mode", "score", "--model", "m", "--n", "5", "--seed", "7", "--paraphrases", "3"])
    finally:
        w.query_model, w.run_aether = orig_q, orig_r
    assert code == 0 and len(calls) == 15 and set(expected) <= set(calls)
    assert "wild score: 11/15" in out
    assert "model calls: 15 (5 tasks x 3 wordings) to m" in out
    assert "consistency: 3/5 tasks kept one verdict for every wording" in out
    assert "mean_000         P..  passes 1/3, verdict changes 1" in out
    assert "  mean           0/1 consistent" in out and "  max            1/1 consistent" in out


def test_score_with_one_wording_prints_no_consistency_block():
    tasks = w.generate(3, 7)
    orig_q, orig_r = w.query_model, w.run_aether
    w.query_model = lambda prompt, endpoint, model: next(t["expected_stdout"] for t in tasks if t["prompt"] == prompt)
    w.run_aether = lambda code, with_stderr=False: (0, code + "\n")  # sanitize strips the newline
    try:
        code, out = run_main(["--mode", "score", "--model", "m", "--n", "3", "--seed", "7"])
    finally:
        w.query_model, w.run_aether = orig_q, orig_r
    assert "wild score: 3/3 (100%)" in out and "consistency" not in out and "model calls" not in out
    assert "[PASS] mean_000         rc=0" in out


def test_paraphrases_out_of_range_is_refused():
    try:
        run_main(["--paraphrases", str(w.MAX_PARAPHRASES + 1)])
    except SystemExit:
        pass
    code, _ = run_main(["--paraphrases", "0"])
    assert code == 2


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
