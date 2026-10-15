#!/usr/bin/env python3
"""Offline tests for --suite properties in tools/aether_wild_eval.py.

A property task gives the model a fixed signature; the harness puts @post
properties on the model's function and calls it on generated inputs, so no
expected answer is needed. A failed property is CON-001 and the attempt sorts
as contract_violation.

The first tests need no compiler (program assembly, determinism, input
coverage). The last runs every reference and broken version through a real
binary and needs $AETHER_BIN (skipped when unset).

Run standalone:  python3 Tests/aether_doc_bench/test_wild_eval_properties.py
(also collects under pytest via the test_* functions.)
"""

from __future__ import annotations

import json
import os
import pathlib
import sys

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_wild_eval as w  # noqa: E402


class Skipped(Exception):
    """Raised by a test that cannot run here (no real aether binary)."""


def skip(reason: str) -> None:
    try:
        import pytest  # noqa: F401
    except ImportError:
        raise Skipped(reason)
    pytest.skip(reason)


def test_same_seed_same_tasks():
    assert json.dumps(w.generate_properties(10, 4)) == json.dumps(w.generate_properties(10, 4))
    assert json.dumps(w.generate_properties(10, 4)) != json.dumps(w.generate_properties(10, 5))


def test_every_template_has_properties_inputs_and_breaks():
    tasks = w.generate_properties(len(w.PROPERTY_TEMPLATES), 1)
    assert {t["template"] for t in tasks} == {f.__name__[2:] for f in w.PROPERTY_TEMPLATES}
    for t in tasks:
        assert t["posts"] and len(t["calls"]) >= 5 and len(t["broken"]) >= 2
        assert t["signature"] in t["prompt"] and t["expected_stdout"] == f"checked {len(t['calls'])}\n"


def test_posts_go_above_existing_annotations():
    t = w.generate_properties(1, 2)[0]
    prog = w.wrap_property(t, t["reference"])
    head = prog.index("@post(")
    assert head < prog.index("@pure") < prog.index(f"fn {t['function']}(")
    assert prog.count("@post(") == len(t["posts"])
    assert prog.count(f"{t['function']}(") == len(t["calls"]) + 1
    assert prog.rstrip().endswith("ret;\n}")


def test_a_model_main_is_dropped():
    t = w.generate_properties(2, 2)[1]
    src = t["reference"] + "fn main() -> Void {\n    fx { if 1 > 0 { println(\"x\"); } }\n    ret;\n}\n"
    prog = w.wrap_property(t, src)
    assert prog.count("fn main(") == 1 and "println(\"x\")" not in prog


def test_classification_uses_the_bench_classifier():
    t = w.generate_properties(1, 2)[0]
    ok = lambda src: (0, t["expected_stdout"], "")  # noqa: E731
    con = lambda src: (1, "", f"[CON-001] Aether @post failed in {t['function']}\n")  # noqa: E731
    syn = lambda src: (1, "", "prog.aether:2: [SYN-001] parser error\n")  # noqa: E731
    assert w.run_property(t, t["reference"], run=ok) == "pass"
    assert w.run_property(t, t["reference"], run=con) == "contract_violation"
    assert w.run_property(t, t["reference"], run=syn) == "coded_error"


def test_real_compiler_references_pass_and_breaks_are_caught():
    aether = os.environ.get("AETHER_BIN")
    if not aether or not pathlib.Path(aether).exists():
        skip("AETHER_BIN not set: needs a real aether binary")
    w.AETHER_BIN = os.path.abspath(aether)
    caught = 0
    for t in w.generate_properties(10, 6):
        assert w.run_property(t, t["reference"]) == "pass", t["id"]
        for name, src in t["broken"].items():
            assert w.run_property(t, src) == "contract_violation", (t["id"], name)
            caught += 1
    assert caught >= 3


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
