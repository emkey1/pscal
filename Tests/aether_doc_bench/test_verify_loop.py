#!/usr/bin/env python3
"""Offline tests for tools/aether_verify_loop.py (Aether as verifier).

The model is a script here: each test hands verify_task a list of replies, so
the loop, the repair prompt, the self-grade and the own-contract check are
exercised without a network. Runs go through a fake runner keyed on what the
program contains; the last test uses a real binary and needs $AETHER_BIN
(skipped when unset).

Run standalone:  python3 Tests/aether_doc_bench/test_verify_loop.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import json
import os
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_verify_loop as v  # noqa: E402
import aether_wild_eval as w  # noqa: E402


class Skipped(Exception):
    """Raised by a test that cannot run here (no real aether binary)."""


def skip(reason: str) -> None:
    try:
        import pytest  # noqa: F401
    except ImportError:
        raise Skipped(reason)
    pytest.skip(reason)


def _task(template: str = "abs_val") -> dict:
    return next(t for t in v.generate(len(v.TEMPLATES), 1) if t["template"] == template)


def _script(*replies):
    """A model that gives these replies in order, and records what it was asked."""
    asked = []

    def ask(messages):
        asked.append(messages)
        return replies[len(asked) - 1]
    return ask, asked


def _runner(task):
    """A fake compiler for abs_val: a function that never tests `x < 0` breaks a property."""
    def run(src):
        if "x < 0" not in src.split("fn main(")[0]:
            return 1, "", f"[CON-001] Aether @post failed in {task['function']}\n"
        return 0, task["expected_stdout"], ""
    return run


def test_same_seed_same_tasks_and_every_template_is_used():
    assert json.dumps(v.generate(14, 4)) == json.dumps(v.generate(14, 4))
    assert {t["template"] for t in v.generate(len(v.TEMPLATES), 1)} == {f.__name__[2:] for f in v.TEMPLATES}
    for t in v.generate(len(v.TEMPLATES), 2):
        assert t["posts"] and len(t["calls"]) >= 5 and len(t["broken"]) >= 2


def test_own_contracts_are_the_pre_and_post_lines_above_the_function():
    code = "@pre(x > -1000)\n@post(result >= 0)\n@pure\nfn abs_val(x: Int) -> Int {\n    ret x;\n}\n"
    assert v.own_contracts(code, "abs_val") == ["@pre(x > -1000)", "@post(result >= 0)"]
    assert v.own_contracts("fn abs_val(x: Int) -> Int {\n    ret x;\n}\n", "abs_val") == []
    assert v.own_contracts("fn other(x: Int) -> Int {\n    ret x;\n}\n", "abs_val") == []


def test_a_rejected_candidate_is_repaired_from_the_execution_verdict():
    t = _task()
    broken = "@post(result >= 0)\nfn abs_val(x: Int) -> Int {\n    ret x;\n}\n"
    fixed = "@post(result >= 0)\n" + t["reference"]
    ask, asked = _script(broken, fixed, "YES")
    r = v.verify_task(t, ask, rounds=3, run=_runner(t))
    assert (r["first"], r["final"], r["rounds"]) == ("contract_violation", "pass", 2)
    repair = asked[1][-1]["content"]
    assert "rejected (contract_violation)" in repair and "CON-001" in repair
    assert r["self_grade"] == "yes" and r["self_grade_agrees"] is False     # it said YES; the run said no


def test_the_loop_stops_at_the_round_limit():
    t = _task()
    broken = "fn abs_val(x: Int) -> Int {\n    ret x;\n}\n"
    ask, asked = _script(broken, broken, "NO")
    r = v.verify_task(t, ask, rounds=2, run=_runner(t))
    assert (r["final"], r["rounds"], len(asked)) == ("contract_violation", 2, 3)
    assert r["self_grade_agrees"] is True and r["own_contracts"] == {"contracts": 0}


def test_summary_counts_false_accepts_and_contract_strength():
    rows = [{"first": "pass", "final": "pass", "rounds": 1, "self_grade": "yes", "self_grade_agrees": True,
             "own_contracts": {"contracts": 2, "accepts_reference": True, "caught": 2, "broken": 2}},
            {"first": "silent_wrong", "final": "pass", "rounds": 2, "self_grade": "yes", "self_grade_agrees": False,
             "own_contracts": {"contracts": 1, "accepts_reference": True, "caught": 0, "broken": 3}},
            {"first": "coded_error", "final": "coded_error", "rounds": 3, "own_contracts": {"contracts": 0}}]
    s = v.summarize(rows)
    assert (s["accepted_first_try"], s["accepted_after_loop"], s["model_calls"]) == (1, 2, 8)
    assert s["self_grade"] == {"answered": 2, "agrees_with_execution": 1, "false_accepts": 1, "false_rejects": 0}
    assert s["own_contracts"] == {"written": 2, "accept_reference": 2, "broken_caught": 2, "broken_total": 5}
    assert len(v.report_lines(s)) == 4


def test_the_api_key_comes_from_the_named_variable_only(tmp_path=None):
    import tempfile
    d = pathlib.Path(tmp_path or tempfile.mkdtemp())
    env = d / "x.env"
    env.write_text("OTHER=1\nFAKE_ROUTER_KEY='not-a-real-key'\n")
    os.environ.pop("FAKE_ROUTER_KEY", None)
    assert v._api_key("", str(env)) == ""
    assert v._api_key("FAKE_ROUTER_KEY", str(env)) == "not-a-real-key"


def test_real_compiler_validate_and_strength():
    aether = os.environ.get("AETHER_BIN")
    if not aether or not pathlib.Path(aether).exists():
        skip("AETHER_BIN not set: needs a real aether binary")
    w.AETHER_BIN = os.path.abspath(aether)
    assert v.validate(v.generate(len(v.TEMPLATES), 5)) == 0
    t = _task("isqrt")
    weak = v.contract_strength(t, ["@post(result >= 0)"])
    assert weak["accepts_reference"] and weak["caught"] < weak["broken"]    # half passes result >= 0
    wrong = v.contract_strength(t, ["@post(result * result == n)"])
    assert not wrong["accepts_reference"]                                    # too strong: not every n is square


if __name__ == "__main__":
    tests = [fn for k, fn in sorted(globals().items())
             if k.startswith("test_") and callable(fn)]
    failed = skipped = 0
    for t in tests:
        try:
            t()
            print(f"ok    - {t.__name__}")
        except Skipped as e:
            skipped += 1
            print(f"skip  - {t.__name__}: {e}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL  - {t.__name__}: {e}")
        except Exception as e:  # noqa: BLE001
            failed += 1
            print(f"ERROR - {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{len(tests) - failed - skipped}/{len(tests)} passed, {skipped} skipped")
    raise SystemExit(1 if failed else 0)
