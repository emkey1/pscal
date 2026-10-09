#!/usr/bin/env python3
"""Offline tests for tools/aether_wild_eval.py.

No model calls. The generator and mutator tests need nothing; the tests that
run programs use the real compiler named by $AETHER_BIN (skipped when unset,
as in the python-only CI job; tools/instrument_check.sh runs them with a build).

Run standalone:  python3 Tests/aether_doc_bench/test_wild_eval_offline.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import json
import os
import pathlib
import sys
import tempfile

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_wild_eval as we  # noqa: E402


class Skipped(Exception):
    """Raised by a test that cannot run here (e.g. no real aether binary)."""


def skip(reason: str) -> None:
    try:
        import pytest  # noqa: F401
    except ImportError:
        raise Skipped(reason)
    pytest.skip(reason)


def real_aether_bin() -> pathlib.Path:
    candidates = [os.environ.get("AETHER_BIN") or ""]
    candidates += [str(REPO_ROOT / "build" / "bin" / "aether"), str(REPO_ROOT / "components" / "aether" / "build" / "aether")]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate) and os.access(candidate, os.X_OK):
            return pathlib.Path(candidate)
    skip("no aether binary: set AETHER_BIN to run this test")
    raise AssertionError("unreachable")


# --------------------------------------------------------------------------- #
# J2: should-fail mutants
# --------------------------------------------------------------------------- #

def test_every_template_yields_at_least_two_mutants():
    tasks = we.generate(len(we.TEMPLATES), seed=7)
    by_template: dict[str, set[str]] = {}
    for item in we.negatives(tasks):
        by_template.setdefault(item["template"], set()).add(item["mechanisms"][-1])
        assert item["should_fail"] and item["program"] and item["expected_error_code"], item["id"]
    assert set(by_template) == {t["template"] for t in tasks}, by_template
    assert all(len(rules) >= 2 for rules in by_template.values()), by_template
    # Each rule appears somewhere.
    assert set().union(*by_template.values()) == {rule for rule, _, _ in we.MUTATORS}


def test_mutants_are_deterministic_and_change_the_reference():
    first = we.negatives(we.generate(20, seed=11))
    again = we.negatives(we.generate(20, seed=11))
    assert first == again
    assert len({item["id"] for item in first}) == len(first)
    refs = {t["id"]: t["reference"] for t in we.generate(20, seed=11)}
    assert all(item["program"] != refs[item["id"].split("__")[0]] for item in first)


def test_mutators_break_one_rule_each():
    ref = ("@pure\nfn mx(a: Int, b: Int) -> Int {\n    if a > b { ret a; }\n    ret b;\n}\n"
           "fn main() -> Void {\n    let m: Int = mx(3, 4);\n    fx { println(\"max = \", m); }\n    ret;\n}\n")
    assert "    println(\"max = \", m);\n" in we.m_println_outside_fx(ref)
    assert "{\n    fx { println(\"in pure\"); }\n    if a > b" in we.m_fx_in_pure(ref)
    assert we.m_return_for_ret(ref).endswith("    return;\n}\n")
    assert we.m_return_for_ret(ref).count("ret b;") == 1
    assert we.m_clamp_two_args(ref) is None
    assert we.m_clamp_two_args("println(clamp(5, 1, 9));") == "println(clamp(5, 9));"
    assert "let m: Strng = " in we.m_misspelled_type(ref)
    assert we.m_failing_contract(ref).startswith("@pre a > 1000\n@pure\nfn mx(")
    assert we.m_failing_contract("fn main() -> Void {\n    ret;\n}\n") is None


def test_check_negative_keeps_only_exactly_the_expected_code():
    item = {"program": "x", "expected_error_code": "FX-001"}

    def runner(rc, out="", err=""):
        return lambda program, flags: (rc, out, err)

    assert we.check_negative(item, runner(1, err="p.aether:2: [FX-001] needs fx\n"))[0]
    assert not we.check_negative(item, runner(0, err="[FX-001]"))[0], "exit 0 is not a rejection"
    assert not we.check_negative(item, runner(1, err="[ANN-001] x\n[FX-001] y\n"))[0], "a second code"
    assert not we.check_negative(item, runner(1, err="Aether @pre failed in f\n"))[0], "uncoded"
    # A code repeated on two lines is still exactly one code.
    assert we.check_negative(item, runner(1, err="[FX-001] a\n[FX-001] b\n"))[0]
    # The --deny proc refusal after a CON-001 carries no code of its own.
    con = {"program": "x", "expected_error_code": "CON-001"}
    deny = ("[CON-001] Aether @pre failed in mx\np.aether:3: VM Error: builtin 'halt' denied by "
            "--deny/PSCAL_VM_DENY policy (effect mask 0x4 intersects denied 0x6).\n")
    assert we.check_negative(con, runner(1, err=deny))[0]


def test_real_aether_keeps_every_mutant_and_the_oracle_check_agrees():
    binary = real_aether_bin()
    import aether_oracle_check as oracle_check

    we.AETHER_BIN = str(binary)
    items = we.negatives(we.generate(10, seed=7))
    verdicts = {item["id"]: we.check_negative(item) for item in items}
    kept = [item for item in items if verdicts[item["id"]][0]]
    dropped = {k: why for k, (ok, why) in verdicts.items() if not ok}
    # Before CON-001 (aether 2026-10-09-1) a failed contract carried no code.
    assert all(k.endswith("__failing_contract") for k in dropped), dropped
    per_template: dict[str, int] = {}
    for item in kept:
        per_template[item["template"]] = per_template.get(item["template"], 0) + 1
    assert len(per_template) == len(we.TEMPLATES) and min(per_template.values()) >= 2, per_template
    with tempfile.TemporaryDirectory() as tmp:
        path = pathlib.Path(tmp) / "tasks_wild_negatives.json"
        path.write_text(json.dumps({"version": "t", "tasks": kept}))
        assert not oracle_check.lint_manifest(path)
        args = oracle_check.adb.argparse.Namespace(aether_bin=binary, sandbox_deny="net,proc", aether_args=[])
        results = oracle_check.check_negatives(path, args)
    assert len(results) == len(kept) and all(r["ok"] for r in results), [r for r in results if not r["ok"]]


# --------------------------------------------------------------------------- #
# J3: rewording consistency
# --------------------------------------------------------------------------- #

def test_wordings_do_not_change_the_generated_tasks():
    tasks = we.generate(20, seed=7)
    # The original prompt and the rng stream are what they were before
    # paraphrases existed (seed 7's first task, recorded from that version).
    assert tasks[0]["prompt"] == ("Print the arithmetic mean of these 4 integers, rounded to exactly two "
                                  "decimal places, as `mean = <value>`: 19, 50, 83, 6.")
    assert tasks[0]["expected_stdout"] == "mean = 39.50\n"
    assert tasks[19]["id"] == "count_above_019"
    for t in tasks:
        ws = we.wordings(t)
        assert len(ws) == 5 and len(set(ws)) == 5 and ws[0] == t["prompt"], t["id"]


def _stub_run(reference_marker="REF"):
    return lambda code: (0, "ok\n") if code == reference_marker else (1, "")


def _tasks(n=3):
    return [{"id": f"t{i}", "template": "a" if i < 2 else "b", "prompt": f"p{i}",
             "paraphrases": [f"p{i}-1", f"p{i}-2"], "expected_stdout": "ok\n"} for i in range(n)]


def test_one_wording_asks_once_per_task_in_the_original_words():
    asked: list[str] = []

    def ask(prompt, task):
        asked.append(prompt)
        return "REF" if prompt != "p1" else "nope"

    results = we.score(_tasks(), 1, ask, _stub_run())
    assert asked == ["p0", "p1", "p2"]
    lines = we.report_score(results, 1)
    assert lines[:3] == ["[PASS] t0               rc=0", "[fail] t1               rc=1",
                         "[PASS] t2               rc=0"], lines
    assert lines[3] == "\nwild score: 2/3 (67%) on novel tasks"
    assert lines[4:6] == ["  a              1/2", "  b              1/1"]
    assert lines[-1] == "model calls: 3 tasks x 1 wording(s) = 3"


def test_rewordings_report_flips_against_one_reference():
    asked: list[str] = []

    def ask(prompt, task):
        asked.append(prompt)
        return "nope" if prompt == "p0-2" or prompt.startswith("p1") else "REF"

    results = we.score(_tasks(), 3, ask, _stub_run())
    assert len(asked) == 9
    assert [r["verdicts"] for r in results] == [[True, True, False], [False, False, False], [True, True, True]]
    lines = we.report_score(results, 3)
    assert lines[0] == "[PP.] t0               pass 2/3  1/2 wording(s) flip the verdict"
    assert lines[1].endswith("pass 0/3  consistent") and lines[2].endswith("pass 3/3  consistent")
    assert "  a              1/2  all wordings 2/6  same verdict in every wording 1/2" in lines
    assert "rewording: 2/3 tasks keep one verdict across 3 wordings" in lines
    assert lines[-1] == "model calls: 3 tasks x 3 wording(s) = 9"


# --------------------------------------------------------------------------- #
# J5: contract property tasks
# --------------------------------------------------------------------------- #

def test_property_tasks_have_contracts_inputs_and_three_broken_versions():
    tasks = we.generate_properties(8, seed=3)
    assert tasks == we.generate_properties(8, seed=3)
    assert {t["template"] for t in tasks} == {"clamp_into", "abs_val", "max_of_two", "gcd"}
    for t in tasks:
        assert t["kind"] == "property" and len(t["broken"]) >= 3, t["id"]
        assert any(c.startswith("@post") for c in t["contracts"]), t["id"]
        assert t["expected_stdout"] == f"checked {len(t['inputs'])}\n"
        assert we.wordings(t) == [t["prompt"]] and "no main" in t["prompt"]
        sig = t["reference"].splitlines()[0]
        assert all(src.splitlines()[0] == sig for src in t["broken"].values()), t["id"]


def test_property_program_wraps_the_function_in_a_checked_twin():
    task = we.generate_properties(1, seed=3)[0]
    program = we.property_program(task, "fn clampInto(v: Int, lo: Int, hi: Int) -> Int {\n    ret v;\n}\n")
    assert program.startswith("fn clampInto(v: Int, lo: Int, hi: Int) -> Int {\n    ret v;\n}\n\n@pre lo <= hi\n")
    assert "fn checkedClampInto(v: Int, lo: Int, hi: Int) -> Int {\n    ret clampInto(v, lo, hi);\n}" in program
    assert program.count("checkedClampInto(") == 1 + len(task["inputs"])
    assert program.endswith(f'    fx {{ println("checked {len(task["inputs"])}"); }}\n    ret;\n}}\n')


def test_property_class_and_score_count_contract_violations():
    task = we.generate_properties(1, seed=3)[0]
    ok = task["expected_stdout"]
    assert we.property_class(task, 0, ok, "") == "pass"
    assert we.property_class(task, 1, "", "[CON-001] Aether @post failed in checkedClampInto\n") == "contract_violation"
    assert we.property_class(task, 1, "", "Aether @post failed in checkedClampInto\n") == "other (rc=1)"
    seen_flags = []

    def run(program, flags):
        seen_flags.append(flags)
        return (0, ok, "") if "GOOD" in program else (1, "", "[CON-001] Aether @post failed in x\n")

    results = we.score([task], 1, lambda prompt, t: "GOOD" if t is task else "", run_full=run)
    assert results[0]["verdicts"] == [True] and results[0]["contract_violations"] == 0
    results = we.score([task], 1, lambda prompt, t: "fn clampInto() -> Int {}", run_full=run)
    assert results[0]["verdicts"] == [False] and results[0]["contract_violations"] == 1
    assert seen_flags == [we.SANDBOX_FLAGS] * 2, "model functions run under the sandbox"
    assert "contract violations: 1 (a property caught a wrong function)" in we.report_score(results, 1)


def test_real_aether_property_references_pass_and_broken_versions_are_caught():
    binary = real_aether_bin()
    we.AETHER_BIN = str(binary)
    version = we.subprocess.run([str(binary), "--version"], capture_output=True, text=True).stdout
    for task in we.generate_properties(len(we.PROPERTY_TEMPLATES) * 2, seed=7):
        assert we.check_property(task, task["reference"]) == "pass", task["id"]
        for name, src in task["broken"].items():
            got = we.check_property(task, src)
            if "CON-001" not in we.run_aether_full(we.property_program(task, src), we.SANDBOX_FLAGS)[2]:
                skip(f"this aether has no CON-001 (before 2026-10-09-1): {version.strip()}")
            assert got == "contract_violation", (task["id"], name, got)
    # Through score's default runners, as --mode score runs a model's answer.
    tasks = we.generate_properties(len(we.PROPERTY_TEMPLATES), seed=7)
    answers = {t["prompt"]: (t["reference"] if i % 2 == 0 else next(iter(t["broken"].values())))
               for i, t in enumerate(tasks)}
    results = we.score(tasks, 1, lambda prompt, t: answers[prompt])
    assert [r["verdicts"] for r in results] == [[True], [False], [True], [False]], results
    assert [r["contract_violations"] for r in results] == [0, 1, 0, 1], results


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
