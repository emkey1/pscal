#!/usr/bin/env python3
"""Offline tests for the should-fail generator in tools/aether_wild_eval.py.

`--mode negatives` breaks each generated reference (fx block removed, an effect
inside @pure, `return` for `ret`, a builtin with the wrong argument count, a
misspelled type, a contract that cannot hold) and keeps only the programs that
fail with exactly the one code the break is meant to cause.

The first tests need no compiler: they check that every template yields at
least two breaks, that --seed fixes the output, that each break really changes
the program, and that the keep/drop filter is strict. The last one needs real
Aether semantics and runs only with $AETHER_BIN set: it generates the manifest
and puts it through tools/aether_oracle_check.py, the way the board checks its
hand-written negatives.

Run standalone:  python3 Tests/aether_doc_bench/test_wild_eval_negatives.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_wild_eval as w  # noqa: E402

WILD_EVAL = REPO_ROOT / "tools" / "aether_wild_eval.py"
ORACLE_CHECK = REPO_ROOT / "tools" / "aether_oracle_check.py"


class Skipped(Exception):
    """Raised by a test that cannot run here (no real aether binary)."""


def skip(reason: str) -> None:
    try:
        import pytest  # noqa: F401
    except ImportError:
        raise Skipped(reason)
    pytest.skip(reason)


def test_every_template_gets_at_least_two_breaks():
    for seed in range(12):
        tasks = w.generate(len(w.TEMPLATES) * 2, seed)
        per: dict[str, set[str]] = {}
        for item in w.break_candidates(tasks, seed):
            per.setdefault(item["source_task"], set()).add(item["break"])
        for t in tasks:
            assert len(per.get(t["id"], ())) >= 2, (seed, t["id"], per.get(t["id"]))


def test_same_seed_same_candidates_and_other_seed_differs():
    a = w.break_candidates(w.generate(20, 7), 7)
    b = w.break_candidates(w.generate(20, 7), 7)
    c = w.break_candidates(w.generate(20, 8), 8)
    assert json.dumps(a) == json.dumps(b)
    assert json.dumps(a) != json.dumps(c)


def test_each_break_changes_the_reference_and_is_well_formed():
    tasks = {t["id"]: t for t in w.generate(20, 3)}
    codes = {name: code for name, code, _ in w.BREAKS}
    ids = set()
    for item in w.break_candidates(list(tasks.values()), 3):
        assert item["program"] != tasks[item["source_task"]]["reference"], item["id"]
        assert item["should_fail"] is True and item["expected_error_code"] == codes[item["break"]]
        assert item["id"] not in ids
        ids.add(item["id"])


def test_breaks_do_what_they_say():
    max_ref = w.t_max(w.random.Random(1))["reference"]
    rng = w.random.Random(0)
    assert "fx {" not in w.b_no_fx(max_ref, rng)[0]
    assert "fx { println(a); }" in w.b_effect_in_pure(max_ref, rng)[0]
    assert "return" in w.b_return_for_ret(max_ref, rng)[0]
    prog = w.b_failing_contract(max_ref, rng)[0]
    assert prog.count("@pure") == 1 and ("@pre(a < a)\n@pure" in prog or "@post(result < result)\n@pure" in prog)
    clamp_ref = w.t_clamp(w.random.Random(1))["reference"]
    assert w.b_misspelled_type(clamp_ref, rng) is None  # only `-> Void`, which reports FLOW-002
    arity = w.b_builtin_arity(clamp_ref, rng)[0]
    assert arity.count(",") in (1, 3), arity


def test_filter_keeps_only_the_exact_code():
    assert w.exact_failure(1, "x.aether:3: [FX-001] Aether effect error", "FX-001")
    assert not w.exact_failure(0, "[FX-001]", "FX-001")                    # did not fail
    assert not w.exact_failure(1, "[FX-001] ... [ANN-001] ...", "FX-001")  # a second error
    assert not w.exact_failure(1, "abs expects 1 argument.", "BUILT-002")  # uncoded
    assert w.exact_failure(1, "[ANN-001] a\n[ANN-001] b", "ANN-001")       # repeats are one code


def test_generate_negatives_drops_what_the_runner_rejects():
    tasks = w.generate(5, 7)

    def fake_run(src: str):
        if "return" in src:
            return 1, "prog.aether:4: [SYN-001] 'return' is not Aether syntax."
        if "Itn" in src or "Integr" in src or "Imt" in src:
            return 1, "[TYPE-002] unknown type\n[FLOW-002] extra"  # two codes: dropped
        return 0, ""

    kept, dropped = w.generate_negatives(tasks, 7, run=fake_run)
    assert kept and all(k["break"] == "return_for_ret" for k in kept)
    assert len(kept) + len(dropped) == len(w.break_candidates(tasks, 7))
    assert all("observed_codes" in d for d in dropped)


def test_real_compiler_negatives_pass_the_oracle_check():
    aether = os.environ.get("AETHER_BIN")
    if not aether or not pathlib.Path(aether).exists():
        skip("AETHER_BIN not set: needs a real aether binary")
    with tempfile.TemporaryDirectory(prefix="wild-neg-") as td:
        out = pathlib.Path(td) / "neg.json"
        env = {**os.environ, "AETHER_BIN": aether}
        gen = subprocess.run([sys.executable, str(WILD_EVAL), "--mode", "negatives", "--n", "10",
                              "--seed", "11", "--dump", str(out)], capture_output=True, text=True, env=env)
        assert gen.returncode == 0, gen.stdout + gen.stderr
        manifest = json.loads(out.read_text())
        per: dict[str, int] = {}
        for item in manifest["tasks"]:
            per[item["source_task"]] = per.get(item["source_task"], 0) + 1
        assert len(per) == 10 and min(per.values()) >= 2, per
        check = subprocess.run([sys.executable, str(ORACLE_CHECK), "--tasks", str(out), "--aether-bin", aether,
                                "--quiet"], capture_output=True, text=True)
        assert check.returncode == 0, check.stdout[-2000:] + check.stderr[-2000:]
        assert f"negatives {len(manifest['tasks'])}/{len(manifest['tasks'])}" in check.stdout


if __name__ == "__main__":
    tests = [v for k, v in sorted(globals().items())
             if k.startswith("test_") and callable(v)]
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
