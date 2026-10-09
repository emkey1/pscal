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
