#!/usr/bin/env python3
"""Deterministic generators for the scale suite's input files (W1-23).

The scale tasks read files of 100 KB to 5 MB. Checking those in would bloat
the repo and every report, so a task names a generator instead:

    "generated_files": {"orders.json": {"generator": "orders_json", "count": 85000, "seed": 7,
                                        "sha256": "<of the generated text>"}}

aether_doc_bench.load_tasks materialises the text (cached per process) and
checks the sha256, so a generator change can never silently move a task's
expected output. Every generator is a pure function of its arguments over a
fixed 31-bit LCG -- no `random`, no clock, no locale.

  python3 Tests/aether_doc_bench/scale_inputs.py orders_json count=10 seed=7   # print one
"""

from __future__ import annotations

import functools
import hashlib
import json
import sys
from typing import Any, Callable, Iterator

_M = 2147483648  # 2^31


def lcg(seed: int) -> Iterator[int]:
    """x' = (1103515245 x + 12345) mod 2^31 -- the same stream the scale prompts describe."""
    x = seed % _M
    while True:
        x = (1103515245 * x + 12345) % _M
        yield x


VOCAB = ("alpha", "bravo", "charlie", "delta", "echo", "foxtrot", "golf", "hotel", "india", "juliet",
         "kilo", "lima", "mike", "november", "oscar", "papa", "quebec", "romeo", "sierra", "tango",
         "uniform", "victor", "whiskey", "xray", "yankee", "zulu")


def text_corpus(seed: int, target_bytes: int, words_per_line: int = 9) -> str:
    """Lines of lowercase words from VOCAB, joined by single spaces, until the
    text reaches target_bytes (it ends with a newline)."""
    rng = lcg(seed)
    lines: list[str] = []
    size = 0
    while size < target_bytes:
        line = " ".join(VOCAB[next(rng) % len(VOCAB)] for _ in range(words_per_line))
        lines.append(line)
        size += len(line) + 1
    return "\n".join(lines) + "\n"


def word_keys(count: int) -> list[str]:
    """`count` distinct pronounceable keys: consonant-vowel syllables."""
    cons, vows = "bdfgklmnprstvz", "aeiou"
    keys: list[str] = []
    i = 0
    seen: set[str] = set()
    while len(keys) < count:
        n, parts = (i * 7919) % 343000, []
        for _ in range(3):
            parts.append(cons[n % len(cons)] + vows[(n // len(cons)) % len(vows)])
            n //= len(cons) * len(vows)
        key = "".join(parts)
        if key not in seen:
            seen.add(key)
            keys.append(key)
        i += 1
    return keys


def word_stream(seed: int, words: int, keys: int, words_per_line: int = 10) -> str:
    """`words` words drawn from `keys` distinct keys, skewed toward the low
    indexes (the smaller of two uniform draws), so the frequency table has a
    clear top."""
    rng = lcg(seed)
    pool = word_keys(keys)
    out: list[str] = []
    line: list[str] = []
    for _ in range(words):
        idx = min(next(rng) % keys, next(rng) % keys, next(rng) % keys)
        line.append(pool[idx])
        if len(line) == words_per_line:
            out.append(" ".join(line))
            line = []
    if line:
        out.append(" ".join(line))
    return "\n".join(out) + "\n"


REGIONS = ("north", "south", "east", "west", "central")


def orders_json(seed: int, count: int) -> str:
    """{"orders": [{"id", "region", "sku", "qty", "cents"}, ...]} -- one order per line."""
    rng = lcg(seed)
    rows = []
    for i in range(count):
        region = REGIONS[next(rng) % len(REGIONS)]
        sku = f"SKU-{next(rng) % 500:03d}"
        qty = 1 + next(rng) % 9
        cents = 100 + next(rng) % 99901
        rows.append(json.dumps({"id": i + 1, "region": region, "sku": sku, "qty": qty, "cents": cents},
                               separators=(", ", ": ")))
    return "{\"orders\": [\n" + ",\n".join(rows) + "\n]}\n"


GENERATORS: dict[str, Callable[..., str]] = {
    "text_corpus": text_corpus,
    "word_stream": word_stream,
    "orders_json": orders_json,
}


@functools.lru_cache(maxsize=16)
def _generate(name: str, frozen_args: str) -> str:
    return GENERATORS[name](**json.loads(frozen_args))


def generate(spec: dict[str, Any]) -> str:
    """The text one `generated_files` entry describes. Unknown keys other
    than generator and sha256 are the generator's arguments."""
    args = {k: v for k, v in spec.items() if k not in ("generator", "sha256")}
    name = spec["generator"]
    if name not in GENERATORS:
        raise KeyError(f"unknown scale input generator {name!r}")
    return _generate(name, json.dumps(args, sort_keys=True))


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def main(argv: list[str]) -> int:
    if not argv or argv[0] not in GENERATORS:
        print(__doc__)
        print("generators:", ", ".join(sorted(GENERATORS)))
        return 2
    args: dict[str, Any] = {}
    for item in argv[1:]:
        key, _, value = item.partition("=")
        args[key] = int(value) if value.lstrip("-").isdigit() else value
    text = generate({"generator": argv[0], **args})
    sys.stdout.write(text)
    sys.stderr.write(f"{len(text)} bytes sha256 {sha256_text(text)}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
