#!/usr/bin/env python3
"""Deterministic fake model for testing the doc benchmark harness."""

from __future__ import annotations

import pathlib
import json
import re
import sys


def extract_task_ids(prompt: str) -> list[str]:
    matches = re.findall(r"^\s*Task ID:\s*(\S+)\s*$", prompt, re.MULTILINE)
    if not matches:
        raise SystemExit("unable to locate Task ID in prompt")
    return matches


PROGRAMS = {
    "hello_fx": """\
fn main() -> Void {
    fx {
        println("hello from benchmark");
    }
    ret;
}
""",
    "classify_scores": """\
@pure
fn classify(score: Int) -> Text {
    if score >= 90 {
        ret "ready";
    }
    if score >= 70 {
        ret "review";
    }
    ret "blocked";
}

fn main() -> Void {
    fx {
        println("95 => ", classify(95));
        println("72 => ", classify(72));
        println("10 => ", classify(10));
    }
    ret;
}
""",
    "type_and_method": """\
type Counter {
    value: Int;

    fn bump() -> Int {
        self.value = self.value + 1;
        ret self.value;
    }
}

fn main() -> Void {
    let counter = new Counter();
    counter.value = 0;
    fx {
        println(counter.bump());
        println(counter.bump());
    }
    ret;
}
""",
    "toon_inline_extract": """\
fn main() -> Void {
    if !has_toon() {
        fx {
            println("toon unavailable");
        }
        ret;
    }

    let doc: ToonDoc = toon_parse("{\\"name\\":\\"Aether\\",\\"score\\":42}");
    let root: ToonNode = toon_root(doc);
    let name: Text = toon_get_text(root, "name");
    let score: Int = toon_get_int(root, "score");
    fx {
        println(name, " ", score);
    }
    toon_close(doc);
    ret;
}
""",
}


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit("usage: mock_model.py PROMPT_FILE")
    prompt_path = pathlib.Path(sys.argv[1])
    prompt = prompt_path.read_text(encoding="utf-8")
    task_ids = extract_task_ids(prompt)
    if "\"results\"" in prompt or "Return exactly one JSON object" in prompt:
        payload = {
            "results": [
                {"task_id": task_id, "source_code": PROGRAMS[task_id]}
                for task_id in task_ids
            ]
        }
        sys.stdout.write(json.dumps(payload))
        return 0
    sys.stdout.write(PROGRAMS[task_ids[0]])
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
