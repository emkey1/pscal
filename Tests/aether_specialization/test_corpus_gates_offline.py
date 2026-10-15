#!/usr/bin/env python3
"""Offline tests for the corpus gates (no aether binary needed).

A tiny mock compiler stands in for aether: a source line `// mock-error CODE`
makes it reject the program with that code (plain and --diagnostics-json),
`// mock-warn CODE` makes it warn, and every `// mock-out TEXT` line is
printed as stdout. Run: python3 Tests/aether_specialization/test_corpus_gates_offline.py
"""

from __future__ import annotations

import hashlib
import json
import os
import pathlib
import shutil
import stat
import subprocess
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


BUILD_DATASET = REPO_ROOT / "tools" / "aether_specialization_build_dataset.py"
VALIDATE_CORPUS = REPO_ROOT / "tools" / "aether_specialization_validate_corpus.py"


class GateCheckMode(unittest.TestCase):
    """build_dataset.py --check-gates end to end on a tiny corpus and the mock."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self._tmp.name)
        self.bin = self.root / "aether"
        self.bin.write_text(MOCK)
        self.bin.chmod(self.bin.stat().st_mode | stat.S_IXUSR)
        self.corpus = self.root / "corpus"
        self.corpus.mkdir()
        (self.root / "fixtures").mkdir()
        self.items: list[dict] = []
        self.add_corpus("01_hello", "// mock-out hello\n", "hello\n")
        self.add_corpus("02_board_twin", "// mock-out board answer\n", "board answer\n")
        self.add_corpus("90_support", "// a module with no golden\n", "")
        self.write("seeds.json", {"pairs": [
            {"id": "seed_one", "prompt": "p", "solution": "// mock-out one\n", "expected_stdout": "one\n"}]})
        self.drills = [{"id": "drill_one", "kind": "repair", "diagnostic": "[FX-001] x",
                        "broken_source": "// mock-error FX-001\n", "fixed_source": "// mock-out fixed\n",
                        "expected_stdout": "fixed\n"}]
        self.write("board.json", {"tasks": [
            {"id": "t1", "expected_stdout": "board answer\n", "reference_solution": "// mock-out board answer\n"}]})

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def write(self, name: str, payload: dict) -> pathlib.Path:
        path = self.root / name
        path.write_text(json.dumps(payload), encoding="utf-8")
        return path

    def add_corpus(self, name: str, source: str, stdout: str) -> None:
        path = self.corpus / name
        path.write_text(source, encoding="utf-8")
        self.items.append({
            "repo_path": str(path),
            "sha256": hashlib.sha256(source.encode("utf-8")).hexdigest(),
            "stdout": stdout,
            "metadata": {"canonical": True, "oracle": "reviewed"},
        })

    def check_gates(self, *extra: str) -> tuple[int, str, dict]:
        manifest = self.write("manifest.json", {"retired": [], "items": self.items})
        repairs = self.write("drills.json", {"pairs": self.drills})
        report = self.root / "report.json"
        proc = subprocess.run(
            [sys.executable, str(BUILD_DATASET), "--check-gates", "--aether-bin", str(self.bin),
             "--corpus-manifest", str(manifest), "--corpus-dir", str(self.corpus),
             "--fixtures-dir", str(self.root / "fixtures"),
             "--instruction-manifest", str(self.root / "seeds.json"), "--repair-manifest", str(repairs),
             "--exclude-benchmark-tasks", str(self.root / "board.json"), "--report-json", str(report),
             *extra],
            cwd=self.root, text=True, capture_output=True, timeout=120,
        )
        output = proc.stdout + proc.stderr
        return proc.returncode, output, json.loads(report.read_text()) if report.exists() else {}

    def test_all_gates_pass_and_nothing_is_written(self) -> None:
        rc, output, report = self.check_gates()
        self.assertEqual(rc, 0, output)
        self.assertIn("gates: PASS (0 of 7 failed)", output)
        self.assertIn("no dataset written", output)
        self.assertEqual(report["mode"], "check_gates")
        self.assertEqual(report["status"], "ok")
        self.assertEqual([step["step"] for step in report["preflight"]], ["corpus_layout", "recapture_check"])
        # the record whose stdout is a board task's is dropped, the rest built and run
        self.assertEqual(report["benchmark_overlap"]["dropped"], ["02_board_twin"])
        self.assertEqual(report["records"], {"instruction": 2, "corpus_instruction": 1, "repair": 1})
        self.assertEqual(sorted(path.name for path in self.root.rglob("*.jsonl")), [])

    def test_drill_code_drift_fails(self) -> None:
        self.drills[0]["diagnostic"] = "[SCOPE-001] x"
        rc, output, report = self.check_gates()
        self.assertEqual(rc, 1, output)
        self.assertIn("FAIL  drill polarity and codes: 1 drills, 1 with problems", output)
        self.assertIn("drill_one: code drift SCOPE-001→FX-001", output)
        self.assertIn("gates: FAIL (1 of 7 failed)", output)
        self.assertEqual(report["status"], "failed")

    def test_obsolete_drill_and_wrong_stdout_fail(self) -> None:
        self.drills[0]["broken_source"] = "// mock-out fixed\n"
        self.items[0]["stdout"] = "goodbye\n"
        rc, output, _ = self.check_gates("--skip-recapture-check")
        self.assertEqual(rc, 1, output)
        self.assertIn("drill_one: drill obsolete", output)
        self.assertIn("01_hello: stdout differs from its expected stdout", output)
        self.assertIn("recapture_expected.py --check pre-flight was skipped", output)

    def test_preflight_failures_are_reported(self) -> None:
        # a no-golden support file that no longer runs (the ENTRY-001 case),
        # and a stray file beside the corpus
        self.add_corpus("91_no_entry", "// mock-error ENTRY-001\n", "")
        (self.corpus / "stray_file").write_text("x\n")
        rc, output, report = self.check_gates()
        self.assertEqual(rc, 1, output)
        self.assertIn("FAIL  preflight: corpus layout", output)
        self.assertIn("disk_not_manifest: stray_file", output)
        self.assertIn("FAIL  preflight: goldens against this binary", output)
        self.assertRegex(output, r"FAIL\s+\S*91_no_entry rc=1")
        # the record gates still ran and passed: every problem is reported at once
        self.assertIn("PASS  records:", output)
        self.assertEqual(report["status"], "failed")

    def test_recapture_report_is_reused(self) -> None:
        version = "Aether Compiler Version: 2099-01-01-1 (mock)"
        report = {"aether_version": version, "ok": 2, "drifted": [], "environment_dependent_drifted": [],
                  "source_drift": [], "run_failed": [], "missing": []}
        path = self.write("recapture.json", report)
        rc, output, gate_report = self.check_gates("--recapture-report", str(path))
        self.assertEqual(rc, 0, output)
        self.assertIn("PASS  preflight: goldens against this binary (recapture_expected --check), "
                      "read from recapture.json", output)
        self.assertEqual(gate_report["preflight"][-1]["from_report"], "recapture.json")

        report["run_failed"] = [{"repo_path": "corpus/91_no_entry", "returncode": 1, "stderr": "[ENTRY-001]"}]
        self.write("recapture.json", report)
        rc, output, _ = self.check_gates("--recapture-report", str(path))
        self.assertEqual(rc, 1, output)
        self.assertIn("FAIL  corpus/91_no_entry rc=1", output)

        # a report from another binary is not evidence about this one
        report.update(run_failed=[], aether_version="Aether Compiler Version: 2000-01-01-1")
        self.write("recapture.json", report)
        rc, output, _ = self.check_gates("--recapture-report", str(path))
        self.assertEqual(rc, 1, output)
        self.assertIn("not this binary", output)

    def test_real_build_still_needs_outputs(self) -> None:
        proc = subprocess.run([sys.executable, str(BUILD_DATASET), "--aether-bin", str(self.bin)],
                              text=True, capture_output=True, timeout=60)
        self.assertEqual(proc.returncode, 2)
        self.assertIn("--instruction-jsonl and --repair-jsonl are required", proc.stderr)

    def test_instrument_check_calls_the_flag(self) -> None:
        script = (REPO_ROOT / "tools" / "instrument_check.sh").read_text()
        self.assertIn("aether_specialization_build_dataset.py --check-gates", script)
        self.assertNotIn("SKIPPED+=(build_gates)", script)


@unittest.skipUnless(shutil.which("git"), "git not available")
class ValidatorIgnoresRunArtefacts(unittest.TestCase):
    def test_gitignored_artefact_is_not_a_corpus_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            subprocess.run(["git", "init", "-q", str(root)], check=True)
            (root / ".gitignore").write_text("corpus/app_log.txt\n")
            corpus = root / "corpus"
            corpus.mkdir()
            (corpus / "01_hello").write_text("// mock-out hello\n")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"items": [{
                "repo_path": str(corpus / "01_hello"), "stdout": "hello\n",
                "metadata": {"canonical": True, "oracle": "reviewed"}}]}))

            def validate() -> tuple[int, dict]:
                proc = subprocess.run(
                    [sys.executable, str(VALIDATE_CORPUS), "--strict", "--manifest", str(manifest),
                     "--corpus-dir", str(corpus), "--fixtures-dir", str(root)],
                    text=True, capture_output=True, timeout=60)
                return proc.returncode, json.loads(proc.stdout)

            (corpus / "app_log.txt").write_text("written by a corpus program\n")
            rc, report = validate()
            self.assertEqual(rc, 0, report["errors"])
            self.assertEqual(report["warnings"]["ignored_run_artefacts"], ["app_log.txt"])
            (corpus / "not_ignored").write_text("x\n")
            rc, report = validate()
            self.assertEqual(rc, 1)
            self.assertEqual(report["errors"]["disk_not_manifest"], ["not_ignored"])


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
