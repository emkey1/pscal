#!/usr/bin/env python3
"""Shared corpus policy for the Aether specialization tools.

Every corpus tool must agree on these rules, so they live here and nowhere
else:

- which manifest items are canonical, and so may train (one definition for the
  raw exporter and the SFT builder alike);
- which goldens can never be training data (the golden backstop: heap
  pointers, raw array dumps, host paths, environment dumps);
- the oracle vocabulary (`python`, `reviewed`, `none`); an item whose golden
  has no oracle is never canonical;
- the board manifests that decontamination and the guide-contamination check
  test against.

This module never runs a corpus program; the only process it starts is
`aether --version`, to stamp what a dataset was verified against.
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import re
import subprocess
from typing import Any, Iterable

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
DEFAULT_BENCH_DIR = REPO_ROOT / "Tests" / "aether_doc_bench"

# --------------------------------------------------------------------------
# Golden backstop
# --------------------------------------------------------------------------

# A golden matching any of these is unusable as training data: the first two
# are process-specific memory dumps (`println(array)` prints an address), the
# rest are data from the machine that captured the golden. The repo is public,
# so the host-data patterns apply to every golden in the manifest, trained or
# not; the dump patterns apply to every golden that reaches a training record.
GOLDEN_BACKSTOP_PATTERNS: dict[str, re.Pattern[str]] = {
    "heap_pointer": re.compile(r"0x[0-9a-f]{6,}"),
    "array_dump": re.compile(r"ARRAY\(dims:"),
    "users_home": re.compile(r"/Users/"),
    "linux_home": re.compile(r"/home/"),
    "app_support": re.compile(r"Application Support"),
    "path_env": re.compile(r"PATH="),
}
HOST_DATA_PATTERNS = ("users_home", "linux_home", "app_support", "path_env")


def backstop_hits(text: Any, names: Iterable[str] | None = None) -> list[str]:
    """Names of the backstop patterns that `text` matches (empty if clean)."""
    if not isinstance(text, str) or not text:
        return []
    selected = list(names) if names is not None else list(GOLDEN_BACKSTOP_PATTERNS)
    return [name for name in selected if GOLDEN_BACKSTOP_PATTERNS[name].search(text)]


# --------------------------------------------------------------------------
# Oracle and the canonical-corpus definition
# --------------------------------------------------------------------------

# python   - the golden was computed independently by a Python reference
#            (tools/aether_corpus_gen.py templates, py_refs once W1-11 lands).
# reviewed - a person checked the golden when the item was added (the curated,
#            hand-authored corpus).
# none     - the golden is whatever the program printed (harvested idea-miner
#            output). Such an item is never canonical.
ORACLE_VALUES = ("python", "reviewed", "none")
HARVESTED_TAG_PREFIX = "harvested_"


def item_metadata(item: dict[str, Any]) -> dict[str, Any]:
    metadata = item.get("metadata")
    return metadata if isinstance(metadata, dict) else {}


def is_harvested(metadata: dict[str, Any]) -> bool:
    tags = metadata.get("tags")
    return isinstance(tags, list) and any(
        isinstance(tag, str) and tag.startswith(HARVESTED_TAG_PREFIX) for tag in tags
    )


def has_golden(item: dict[str, Any]) -> bool:
    stdout = item.get("stdout")
    return isinstance(stdout, str) and bool(stdout)


def canonical_exclusion(metadata: dict[str, Any]) -> str | None:
    """The one definition of a canonical corpus item; None means canonical.

    An item is canonical unless the manifest says `canonical: false` or its
    golden has no oracle. Both the raw exporter and the SFT builder use this,
    so the two can no longer disagree about what canonical Aether is; the
    pipeline flags (include_in_training, include_in_supervised) only opt an
    item out of one pipeline.
    """
    if metadata.get("oracle") == "none":
        return "no_oracle"
    if metadata.get("canonical") is False:
        return "not_canonical"
    return None


def sft_exclusion(item: dict[str, Any]) -> str | None:
    """Why a manifest item does not become an instruction-SFT record (None: it does).

    Support modules have no golden. Environment-dependent items keep their
    code in the raw corpus, but their golden cannot be reproduced, so it never
    becomes an "Exact stdout must be" prompt.
    """
    metadata = item_metadata(item)
    if not has_golden(item):
        return "no_expected_stdout"
    reason = canonical_exclusion(metadata)
    if reason:
        return reason
    if metadata.get("include_in_supervised") is False:
        return "not_supervised"
    if metadata.get("environment_dependent"):
        return "environment_dependent"
    return None


def raw_exclusion(item: dict[str, Any]) -> str | None:
    """Why a manifest item is not exported to the raw corpus (None: it is)."""
    metadata = item_metadata(item)
    if not has_golden(item):
        return "no_expected_stdout"
    reason = canonical_exclusion(metadata)
    if reason:
        return reason
    if metadata.get("include_in_training") is False:
        return "not_in_training"
    return None


def oracle_problem(metadata: dict[str, Any]) -> str | None:
    """A structural problem with an item's oracle metadata, or None."""
    oracle = metadata.get("oracle")
    if oracle is None:
        return "missing oracle"
    if oracle not in ORACLE_VALUES:
        return f"invalid oracle {oracle!r} (expected one of {', '.join(ORACLE_VALUES)})"
    if oracle == "none" and metadata.get("canonical") is not False:
        # A golden without an oracle is only what the program printed; such an
        # item can become canonical only once an oracle is recorded for it.
        return "oracle none requires canonical: false"
    return None


# --------------------------------------------------------------------------
# Board manifests (decontamination)
# --------------------------------------------------------------------------

# Every manifest a published board scores, plus tasks.json (still the
# harness default). tasks_traps and tasks_scale join automatically once they
# exist.
BOARD_MANIFESTS = (
    "tasks_v2_pos.json",
    "tasks_hard_v2.json",
    "tasks_hard_nontoon.json",
    "tasks_cs.json",
    "tasks_frontier.json",
    "tasks_frontier_algo.json",
    "tasks_frontier_spec.json",
    "tasks.json",
)
FUTURE_BOARD_MANIFESTS = ("tasks_traps.json", "tasks_scale.json")


def default_board_manifests(bench_dir: pathlib.Path = DEFAULT_BENCH_DIR) -> list[pathlib.Path]:
    missing = [name for name in BOARD_MANIFESTS if not (bench_dir / name).is_file()]
    if missing:
        raise SystemExit(f"board manifest(s) missing from {display_path(bench_dir)}: {', '.join(missing)}")
    paths = [bench_dir / name for name in BOARD_MANIFESTS]
    paths += [bench_dir / name for name in FUTURE_BOARD_MANIFESTS if (bench_dir / name).is_file()]
    return paths


def load_tasks(path: pathlib.Path) -> list[dict[str, Any]]:
    payload = json.loads(path.read_text(encoding="utf-8"))
    tasks = payload.get("tasks") if isinstance(payload, dict) else payload
    return [task for task in tasks or [] if isinstance(task, dict)]


# --------------------------------------------------------------------------
# Small shared helpers
# --------------------------------------------------------------------------


def display_path(path: pathlib.Path | str, root: pathlib.Path = REPO_ROOT) -> str:
    """A path safe to write into tracked or shareable output.

    Repo-relative when the path is inside the repo, otherwise only the file
    name, so a host's home directory never lands in a generated file.
    """
    candidate = pathlib.Path(path)
    try:
        return str(candidate.resolve().relative_to(root.resolve()))
    except ValueError:
        return candidate.name


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


_VERSION_RE = re.compile(r"\b(\d{4}-\d{2}-\d{2}-\d+)\b")


def aether_identity(aether_bin: pathlib.Path) -> dict[str, str]:
    """The language VERSION and binary sha256 a dataset was verified against."""
    version = "unknown"
    try:
        proc = subprocess.run(
            [str(aether_bin), "--version"], text=True, capture_output=True, timeout=20
        )
        text = (proc.stdout or proc.stderr or "").strip()
        match = _VERSION_RE.search(text)
        if match:
            version = match.group(1)
        elif text:
            version = text.splitlines()[0]
    except (OSError, subprocess.SubprocessError):
        pass
    sha = sha256_file(aether_bin) if aether_bin.is_file() else "unknown"
    return {"aether_version": version, "aether_sha256": sha}
