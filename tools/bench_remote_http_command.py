#!/usr/bin/env python3
"""Call a remote benchmark generation endpoint and print harness JSON."""

from __future__ import annotations

import argparse
import json
import sys
import urllib.error
import urllib.request
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prompt_file", help="path to the benchmark prompt file")
    parser.add_argument("--url", required=True, help="remote /generate endpoint URL")
    parser.add_argument("--max-new-tokens", type=int, default=3000)
    parser.add_argument("--temperature", type=float, default=0.2)
    parser.add_argument("--timeout-seconds", type=int, default=300)
    args = parser.parse_args()

    prompt = Path(args.prompt_file).read_text(encoding="utf-8")
    body = json.dumps(
        {
            "prompt": prompt,
            "max_new_tokens": args.max_new_tokens,
            "temperature": args.temperature,
        }
    ).encode("utf-8")

    request = urllib.request.Request(
        args.url,
        data=body,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        with urllib.request.urlopen(request, timeout=args.timeout_seconds) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        print(
            json.dumps(
                {
                    "raw_text": "",
                    "stderr": f"http {exc.code}: {detail}",
                }
            )
        )
        return 0
    except Exception as exc:
        print(json.dumps({"raw_text": "", "stderr": str(exc)}))
        return 0

    result = {
        "raw_text": str(payload.get("raw_text", "")),
    }
    usage = payload.get("usage")
    if isinstance(usage, dict):
        result["usage"] = usage
    if payload.get("model") is not None:
        result["model"] = payload.get("model")
    if payload.get("response_id") is not None:
        result["response_id"] = payload.get("response_id")
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
