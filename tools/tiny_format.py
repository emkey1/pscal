#!/usr/bin/env python3
"""Simple formatter for Clike source files."""

import argparse
import sys

INDENT = "    "

def format_code(src: str) -> str:
    indent = 0
    lines = []
    # ensure braces are on their own line
    tokens = (
        src.replace('{', '{\n')
           .replace('}', '}\n')
           .replace(';', ';\n')
        ).splitlines()
    for tok in tokens:
        line = tok.strip()
        if not line:
            continue
        if line.startswith('}'):
            indent = max(indent - 1, 0)
        lines.append(INDENT * indent + line)
        if line.endswith('{'):
            indent += 1
    return "\n".join(lines) + "\n"

def main() -> None:
    parser = argparse.ArgumentParser(description="Format Clike source")
    parser.add_argument('file', nargs='?', help='File to format (reads stdin if omitted)')
    args = parser.parse_args()
    if args.file:
        with open(args.file) as f:
            src = f.read()
    else:
        src = sys.stdin.read()
    sys.stdout.write(format_code(src))

if __name__ == '__main__':
    main()
