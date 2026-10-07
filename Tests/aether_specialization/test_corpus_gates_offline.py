#!/usr/bin/env python3
"""Offline tests for the corpus gates (no aether binary needed).

A tiny mock compiler stands in for aether: a source line `// mock-error CODE`
makes it reject the program with that code (plain and --diagnostics-json),
`// mock-warn CODE` makes it warn, and every `// mock-out TEXT` line is
printed as stdout. Run: python3 Tests/aether_specialization/test_corpus_gates_offline.py
"""

from __future__ import annotations

import os
import pathlib
import stat
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))

import aether_specialization_build_dataset as bd  # noqa: E402
import aether_specialization_corpus_policy as policy  # noqa: E402
import check_guide_contamination as contam  # noqa: E402

MOCK = r'''#!/usr/bin/env python3
import json, re, sys
args = sys.argv[1:]
if args == ["--version"]:
    print("Aether Compiler Version: 2099-01-01-1 (mock)")
    sys.exit(0)
src = open(args[-1]).read()
name = args[-1]
err = re.search(r"// mock-error (\S+)", src)
warn = re.search(r"// mock-warn (\S+)", src)
if err:
    code = err.group(1)
    if "--diagnostics-json" in args:
        sys.stderr.write("[\n" + json.dumps({"severity": "error", "code": code, "message": "m"}) + "\n]\n")
    else:
        sys.stderr.write(f"{name}:1: [{code}] mock error.\nhelp: see {code}\n")
    sys.exit(1)
if warn and "--diagnostics-json" not in args:
    sys.stderr.write(f"{name}:2: warning: [{warn.group(1)}] mock warning.\n")
for line in re.findall(r"// mock-out (.*)", src):
    print(line)
'''


class MockCompiler(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory()
        cls.bin = pathlib.Path(cls.tmp.name) / "aether"
        cls.bin.write_text(MOCK)
        cls.bin.chmod(cls.bin.stat().st_mode | stat.S_IXUSR)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def drill(self, kind: str, broken: str, diagnostic: str, expected: str = "ok\n") -> list[str]:
        item = {"id": "d", "kind": kind, "broken_source": broken, "diagnostic": diagnostic,
                "expected_stdout": expected}
        return bd.drill_problems(item, bd.probe_broken_source(self.bin, item))

    def test_repair_drill_still_rejected(self) -> None:
        self.assertEqual(self.drill("repair", "// mock-error FX-001\n", "[FX-001] x"), [])

    def test_repair_drill_obsolete(self) -> None:
        problems = self.drill("repair", "// mock-out ok\n", "[SYN-001] x")
        self.assertEqual(len(problems), 1)
        self.assertTrue(problems[0].startswith("drill obsolete"))

    def test_repair_drill_code_drift(self) -> None:
        self.assertEqual(self.drill("repair", "// mock-error BUILT-002\n", "[SCOPE-001] x"),
                         ["code drift SCOPE-001→BUILT-002"])

    def test_behavioral_drill(self) -> None:
        self.assertEqual(self.drill("behavioral", "// mock-out wrong\n", "Behavioral issue: x"), [])
        self.assertTrue(self.drill("behavioral", "// mock-out ok\n", "Behavioral issue: x")[0]
                        .startswith("drill obsolete"))
        self.assertTrue(self.drill("behavioral", "// mock-error X-1\n", "x")[0]
                        .startswith("behavioral drill no longer runs"))
        self.assertEqual(self.drill("behavioral", "// mock-warn ARR-001\n// mock-out wrong\n", "x"),
                         ["code drift none→ARR-001"])
        self.assertEqual(self.drill("behavioral", "// mock-warn ARR-001\n// mock-out wrong\n",
                                    "[ARR-001] y"), [])

    def test_missing_kind_fails(self) -> None:
        self.assertTrue(self.drill("", "// mock-error FX-001\n", "[FX-001] x")[0].startswith("invalid kind"))

    def test_stderr_names_sample(self) -> None:
        run = bd.run_aether(aether_bin=self.bin, source="// mock-error FX-001\n", files=None)
        self.assertTrue(run["stderr"].startswith("sample.aether:1: [FX-001]"))

    def test_identity_stamp(self) -> None:
        self.assertEqual(policy.aether_identity(self.bin)["aether_version"], "2099-01-01-1")


class Policy(unittest.TestCase):
    def test_backstop(self) -> None:
        self.assertEqual(policy.backstop_hits("ARRAY(dims:1, elements_at:0x10357bea0)"),
                         ["heap_pointer", "array_dump"])
        self.assertEqual(policy.backstop_hits("HOME = " + "/" + "Users/x"), ["users_home"])
        self.assertEqual(policy.backstop_hits("0xff 42 => 1764\n"), [])

    def test_selection(self) -> None:
        def item(**metadata):
            return {"repo_path": "x", "stdout": "1\n", "metadata": metadata}
        self.assertIsNone(policy.sft_exclusion(item(canonical=True, oracle="reviewed")))
        self.assertEqual(policy.sft_exclusion(item(canonical=False, oracle="none")), "no_oracle")
        self.assertEqual(policy.sft_exclusion(item(oracle="reviewed", environment_dependent=True)),
                         "environment_dependent")
        self.assertEqual(policy.sft_exclusion({"repo_path": "m", "metadata": {}}), "no_expected_stdout")
        self.assertEqual(policy.oracle_problem({"oracle": "none"}), "oracle none requires canonical: false")
        self.assertEqual(policy.oracle_problem({}), "missing oracle")


class Contamination(unittest.TestCase):
    def test_distinctive(self) -> None:
        self.assertTrue(contam.distinctive("clampSupport"))
        self.assertTrue(contam.distinctive("bench_math"))
        self.assertFalse(contam.distinctive("cube"))
        self.assertFalse(contam.distinctive("Base"))

    def test_runnable_program(self) -> None:
        self.assertIsNotNone(contam.runnable_program("fn main() -> Void { ret; }"))
        self.assertIn("fn main", contam.runnable_program("loop i in 0..2 { }"))
        self.assertIsNone(contam.runnable_program("fn helper() -> Int { ret 1; }"))


if __name__ == "__main__":
    unittest.main()
