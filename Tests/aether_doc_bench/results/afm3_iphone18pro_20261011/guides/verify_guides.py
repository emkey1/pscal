#!/usr/bin/env python3
"""Run every ```aether block of a guide draft on the frozen compiler and compare its
stdout with the ```text block that follows it. Exit 1 on any mismatch.

usage: verify_guides.py GUIDE.md...     (AETHER_ROOT defaults to ~/aether-6659679)

Each block runs in an empty directory holding the aether repo's stub modules
(tests/guide_modules/, so `use "score_utils";` resolves) and the data files in
FIXTURES below, which are the files the guides say exist. Also prints each guide's
size and its estimated on-device token count (chars / 3.02, the ratio measured on
the card and v6 prompts for this model's tokenizer).
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.environ.get("AETHER_ROOT", os.path.expanduser("~/aether-6659679"))
BIN = os.path.join(ROOT, "build", "aether")
FIXTURES = {
    "trip.json": '{"city":"Oslo","open":true,"stops":[{"id":7,"tags":["a","b"]},{"id":9}]}\n',
}
BLOCK = re.compile(r"```aether\n(.*?)```\n(?:\s*```text\n(.*?)```)?", re.S)

def run(src):
    d = tempfile.mkdtemp(prefix="guide_verify_")
    try:
        mods = os.path.join(ROOT, "tests", "guide_modules")
        for m in os.listdir(mods):
            shutil.copy(os.path.join(mods, m), d)
        for name, body in FIXTURES.items():
            open(os.path.join(d, name), "w").write(body)
        open(os.path.join(d, "block.aether"), "w").write(src)
        p = subprocess.run([BIN, "--no-cache", "block.aether"], cwd=d, capture_output=True, text=True, timeout=60)
        return p.returncode, p.stdout, p.stderr
    finally:
        shutil.rmtree(d, ignore_errors=True)

bad = 0
for path in sys.argv[1:]:
    text = open(path).read()
    blocks = BLOCK.findall(text)
    print(f"{os.path.basename(path)}: {len(text)} chars, ~{round(len(text) / 3.02)} device tokens, {len(blocks)} programs")
    for i, (src, want) in enumerate(blocks, 1):
        rc, out, err = run(src)
        if want == "":
            status = "NO EXPECTED OUTPUT BLOCK"
        elif rc != 0:
            status = f"rc={rc}: {err.strip().splitlines()[0] if err.strip() else ''}"
        elif out != want:
            status = "OUTPUT DIFFERS\n--- got\n" + out + "--- want\n" + want
        else:
            status = "ok"
        if status != "ok":
            bad += 1
        print(f"  block {i}: {status}")
sys.exit(1 if bad else 0)
