#!/usr/bin/env python3
"""Call an OpenAI-compatible chat completions endpoint and print harness JSON."""

from __future__ import annotations

import argparse
import json
import urllib.error
import urllib.request
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prompt_file", help="path to the benchmark prompt file")
    parser.add_argument("--url", required=True, help="full /chat/completions URL")
    parser.add_argument("--model", required=True, help="served model name")
    parser.add_argument("--max-tokens", type=int, default=4000)
    parser.add_argument("--temperature", type=float, default=0.2)
    parser.add_argument("--timeout-seconds", type=int, default=900)
    parser.add_argument("--api-key", default="", help="optional bearer token")
    parser.add_argument("--stop", action="append", default=[], help="stop string; may be repeated")
    args = parser.parse_args()

    prompt = Path(args.prompt_file).read_text(encoding="utf-8")
    body = {
        "model": args.model,
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": args.max_tokens,
        "temperature": args.temperature,
    }
    if args.stop:
        body["stop"] = args.stop

    headers = {"Content-Type": "application/json"}
    if args.api_key:
        headers["Authorization"] = f"Bearer {args.api_key}"
    request = urllib.request.Request(
        args.url,
        data=json.dumps(body).encode("utf-8"),
        headers=headers,
        method="POST",
    )

    try:
        with urllib.request.urlopen(request, timeout=args.timeout_seconds) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", errors="replace")
        print(json.dumps({"raw_text": "", "stderr": f"http {exc.code}: {detail}"}))
        return 0
    except Exception as exc:
        print(json.dumps({"raw_text": "", "stderr": str(exc)}))
        return 0

    choices = payload.get("choices") or []
    raw_text = ""
    if choices:
        message = choices[0].get("message") or {}
        content = message.get("content")
        if isinstance(content, str):
            raw_text = content
        elif isinstance(content, list):
            parts: list[str] = []
            for item in content:
                if isinstance(item, dict) and item.get("type") == "text":
                    parts.append(str(item.get("text", "")))
            raw_text = "".join(parts)

    result = {"raw_text": raw_text}
    usage = payload.get("usage")
    if isinstance(usage, dict):
        result["usage"] = usage
    if payload.get("id") is not None:
        result["response_id"] = payload.get("id")
    result["model"] = args.model
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
