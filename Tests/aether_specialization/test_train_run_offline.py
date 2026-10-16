#!/usr/bin/env python3
"""Offline tests for tools/aether_train_run.py and the training registry (no ssh, no GPU)."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools"))
import aether_train_run as tr  # noqa: E402

RUNS_DIR = REPO_ROOT / "Tests" / "aether_specialization" / "training" / "runs"
REQUIRED_FAMILY_KEYS = {"description", "image", "instruction_part", "response_part", "target_modules",
                        "chat_template_kwargs", "tokenizer_from_base", "serve", "leak_prefixes"}


def write_run(tmp: pathlib.Path, **overrides) -> pathlib.Path:
    run = {"tag": "t-run", "family": "qwen3_5", "base_model": "/storage/models/Base", "host": "clawX",
           "dataset": {"id": "ds-1", "aether_version": "2026-10-15-1"}} | overrides
    path = tmp / "run.json"
    path.write_text(json.dumps(run), encoding="utf-8")
    return path


class RegistryTests(unittest.TestCase):
    def test_every_family_is_complete(self):
        registry = json.loads(tr.FAMILIES_PATH.read_text(encoding="utf-8"))
        for name, family in registry["families"].items():
            with self.subTest(family=name):
                self.assertFalse(REQUIRED_FAMILY_KEYS - set(family), f"missing {REQUIRED_FAMILY_KEYS - set(family)}")
                for key in ("image", "dtype", "max_model_len", "gpu_memory_utilization", "extra_args"):
                    self.assertIn(key, family["serve"])

    def test_tracked_runs_load_and_name_known_families(self):
        runs = sorted(RUNS_DIR.glob("*.json"))
        self.assertTrue(runs)
        for path in runs:
            with self.subTest(run=path.name):
                run = tr.load_run(path)
                self.assertEqual(run["tag"], path.stem)
                self.assertIsNone(tr.PRIVATE.search(path.read_text(encoding="utf-8")))


class ConfigTests(unittest.TestCase):
    def test_defaults_fill_in(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = tr.load_run(write_run(pathlib.Path(tmp)))
        self.assertEqual(run["precision"], "4bit")
        self.assertEqual(run["lora"], {"r": 32, "alpha": 64, "dropout": 0.0})
        self.assertEqual(run["image"], "aether-unsloth-qwen3-coder-30b:base26-fla")
        self.assertEqual(run["paths"]["merged"], "/storage/aether-train/t-run/merged_16bit")
        self.assertEqual(run["score"]["variants"], ["base", "tuned"])

    def test_bad_values_are_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = pathlib.Path(tmp)
            for overrides, message in [({"family": "nope"}, "unknown family"),
                                       ({"precision": "3bit"}, "precision"),
                                       ({"tag": "Bad Tag"}, "tag"),
                                       ({"score": {"variants": ["guided"]}}, "variant")]:
                with self.subTest(overrides=overrides), self.assertRaises(SystemExit) as ctx:
                    tr.load_run(write_run(tmp, **overrides))
                self.assertIn(message, str(ctx.exception))


class ScriptTests(unittest.TestCase):
    def load(self, **overrides):
        with tempfile.TemporaryDirectory() as tmp:
            return tr.load_run(write_run(pathlib.Path(tmp), **overrides))

    def test_trainer_argv_carries_family_markers_and_recipe(self):
        argv = tr.trainer_argv(self.load())
        self.assertEqual(argv[argv.index("--instruction-part") + 1], "<|im_start|>user\\n")
        self.assertEqual(argv[argv.index("--response-part") + 1], "<|im_start|>assistant\\n")
        self.assertEqual(argv[argv.index("--lora-r") + 1], "32")
        self.assertEqual(argv[argv.index("--seed") + 1], argv[argv.index("--random-state") + 1])
        # Rendered like the served prompt (Qwen3.8 adds a reasoning system turn otherwise).
        self.assertEqual(json.loads(argv[argv.index("--chat-template-kwargs") + 1]), {"enable_thinking": False})
        self.assertNotIn("--no-load-in-4bit", argv)
        self.assertIn("--no-load-in-4bit", tr.trainer_argv(self.load(precision="16bit")))

    def test_mistral_tuned_is_served_with_the_base_tokenizer(self):
        run = self.load(family="mistral")
        tuned, base = tr.serve_command(run, "tuned"), tr.serve_command(run, "base")
        self.assertEqual(tuned[tuned.index("--tokenizer") + 1], "/storage/models/Base")
        self.assertNotIn("--tokenizer", base)
        self.assertNotIn("--tokenizer", tr.serve_command(self.load(), "tuned"))

    def test_thinking_switch_reaches_the_destination(self):
        dest = tr.destinations(self.load(), "base")["destinations"][0]
        self.assertEqual(dest["extra_body"], {"chat_template_kwargs": {"enable_thinking": False}})
        self.assertNotIn("extra_body", tr.destinations(self.load(family="chatml"), "base")["destinations"][0])

    def test_generated_scripts_parse_and_keep_home_expandable(self):
        run = self.load()
        for name, text in (("train.sh", tr.train_script(run)), ("score.sh", tr.score_script(run))):
            with self.subTest(script=name):
                self.assertEqual(subprocess.run(["bash", "-n"], input=text, text=True).returncode, 0)
                self.assertNotIn("'$HOME", text)
                self.assertIsNone(tr.PRIVATE.search(text))
        score = tr.score_script(run)
        for suite in run["score"]["suites"]:
            self.assertIn(f"{suite}=", score)
        self.assertIn("--aether-bin-sha256", score)
        self.assertNotIn('"$OUT/$TAGV.done" ] &&', score)  # a variant is skipped per suite, not by one marker
        self.assertIn("*2026-10-15-1*", score)
        newer = tr.score_script(self.load(score={"grader_version": "2026-10-15-2"}))
        self.assertIn("*2026-10-15-2*", newer)
        self.assertNotIn("*2026-10-15-1*", newer)


class ModelProbeTests(unittest.TestCase):
    def probe(self, files, index=None, incomplete=False):
        with tempfile.TemporaryDirectory() as tmp:
            d = pathlib.Path(tmp)
            for name, size in files.items():
                (d / name).write_bytes(b"x" * size)
            if index is not None:
                (d / "model.safetensors.index.json").write_text(json.dumps({"weight_map": index}))
            if incomplete:
                (d / ".cache/huggingface/download").mkdir(parents=True)
                (d / ".cache/huggingface/download/a.incomplete").write_bytes(b"")
            out = subprocess.run([sys.executable, "-c", tr.MODEL_PROBE, str(d)], capture_output=True, text=True)
            return out.stdout.strip()

    def test_states(self):
        index = {"a": "s1.safetensors", "b": "s2.safetensors"}
        self.assertEqual(self.probe({}), "model=missing")
        self.assertEqual(self.probe({"config.json": 2}, index), "model=partial")
        self.assertEqual(self.probe({"config.json": 2, "s1.safetensors": 4, "s2.safetensors": 0}, index), "model=partial")
        self.assertEqual(self.probe({"config.json": 2, "s1.safetensors": 4, "s2.safetensors": 4}, index), "model=ok")
        self.assertEqual(self.probe({"config.json": 2, "s1.safetensors": 4, "s2.safetensors": 4}, index, True),
                         "model=partial")
        self.assertEqual(self.probe({"config.json": 2, "m.safetensors": 4}), "model=ok")

    def test_waiter_script_parses(self):
        with tempfile.TemporaryDirectory() as tmp:
            run = tr.load_run(write_run(pathlib.Path(tmp)))
        text = tr.when_ready_script(run)
        self.assertEqual(subprocess.run(["bash", "-n"], input=text, text=True).returncode, 0)
        score = tr.score_when_ready_script(run)
        self.assertEqual(subprocess.run(["bash", "-n"], input=score, text=True).returncode, 0)
        self.assertIn("aether-train-t-run", score)
        self.assertIn("-ge 60", text)


class CheckGenerationsTests(unittest.TestCase):
    def run_check(self, sources, *extra, infra=0):
        report = {"destinations": [{"variants": [{
            "results": [{"attempts": [{"source_code": s}]} for s in sources],
            "summary": {"total_cases": len(sources), "first_attempt_exact": 0, "final_exact": 0,
                        "infra_failed": infra}}]}]}
        with tempfile.TemporaryDirectory() as tmp:
            script = pathlib.Path(tmp) / "check.py"
            script.write_text(tr.CHECK_GENERATIONS, encoding="utf-8")
            path = pathlib.Path(tmp) / "r.json"
            path.write_text(json.dumps(report), encoding="utf-8")
            return subprocess.run([sys.executable, str(script), str(path), '["thinking", "<think"]', *extra],
                                  capture_output=True, text=True).returncode

    def test_programs_pass(self):
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }", "fn main() -> Void { ret; }"]), 0)

    def test_reasoning_trace_fails(self):
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }", "Thinking Process: ..."]), 1)

    def test_all_empty_fails(self):
        self.assertEqual(self.run_check(["", "  "]), 1)

    def test_infra_failure_fails_only_when_complete_is_asked(self):
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }"], infra=1), 0)
        self.assertEqual(self.run_check(["fn main() -> Void { ret; }"], "--complete", infra=1), 1)


if __name__ == "__main__":
    unittest.main()
