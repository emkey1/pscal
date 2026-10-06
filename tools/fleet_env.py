"""Private fleet facts (hosts, endpoints) for the PBuild tools.

This repo is public, so it carries no tailnet addresses, host FQDNs or
credential locations. They live in an untracked overlay,
~/.config/pscal/fleet.env (override the path with $PSCAL_FLEET_ENV), as
shell-style `export NAME=value` lines: shell drivers `source` it and Python
tools read it through get(). A variable set in the environment wins over the
file. Copy the file to every machine that runs these tools.
"""

from __future__ import annotations

import os
import pathlib
import re
import shlex

_LINE = re.compile(r"^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$")
_REF = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)\}")
_cache: dict[str, str] | None = None


def path() -> pathlib.Path:
    return pathlib.Path(
        os.environ.get("PSCAL_FLEET_ENV", "~/.config/pscal/fleet.env")
    ).expanduser()


def _file_values() -> dict[str, str]:
    global _cache
    if _cache is None:
        _cache = {}
        try:
            text = path().read_text(encoding="utf-8")
        except OSError:
            return _cache
        for line in text.splitlines():
            m = _LINE.match(line)
            if not m or line.lstrip().startswith("#"):
                continue
            parts = shlex.split(m.group(2), comments=True)
            _cache[m.group(1)] = parts[0] if parts else ""
    return _cache


def get(name: str, default: str | None = None) -> str | None:
    """The value of NAME from the environment, else the overlay, else default."""
    value = os.environ.get(name)
    if value:
        return value
    return _file_values().get(name) or default


def expand(text: str) -> str:
    """Replace each ${NAME} in text with get(NAME); fail loudly if one is unset."""

    def sub(m: re.Match[str]) -> str:
        value = get(m.group(1))
        if value is None:
            raise SystemExit(
                f"{m.group(1)} is not set: export it or add it to {path()} "
                "(the private fleet overlay; see tools/fleet_env.py)"
            )
        return value

    return _REF.sub(sub, text)
