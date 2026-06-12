#!/usr/bin/env python3
"""Run aether_doc_bench.py with an overridden guide path."""

from __future__ import annotations

import importlib.util
import pathlib
import sys


def main() -> int:
    if len(sys.argv) < 3:
        raise SystemExit(
            "usage: run_aether_doc_bench_with_doc.py <variant: full|small|none> /path/to/guide.md [bench args...]"
        )

    variant = sys.argv[1]
    override_path = pathlib.Path(sys.argv[2]).resolve()
    bench_args = sys.argv[3:]

    bench_path = pathlib.Path(__file__).with_name("aether_doc_bench.py")
    spec = importlib.util.spec_from_file_location("aether_doc_bench", bench_path)
    if spec is None or spec.loader is None:
        raise SystemExit(f"unable to load benchmark module from {bench_path}")

    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    if variant not in module.DOC_VARIANTS:
        raise SystemExit(f"unknown variant '{variant}', expected one of: {', '.join(sorted(module.DOC_VARIANTS))}")

    module.DOC_VARIANTS[variant] = override_path

    sys.argv = ["aether_doc_bench.py", *bench_args]
    return module.main()


if __name__ == "__main__":
    raise SystemExit(main())
