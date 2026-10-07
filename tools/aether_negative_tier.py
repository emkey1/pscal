#!/usr/bin/env python3
"""Moved: the negative tier is now part of tools/aether_oracle_check.py.

This shim keeps the old command line working; it runs the oracle check's
should_fail tier only (same as `aether_oracle_check.py --negatives-only`).
"""
from __future__ import annotations

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aether_oracle_check  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(aether_oracle_check.main(["--negatives-only", *sys.argv[1:]]))
