#!/usr/bin/env python3
"""Small HTTP server for Qwen3-4B-Base generation on Spark."""

from __future__ import annotations

import argparse
import json
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import torch
from transformers import AutoModelForCausalLM, AutoTokenizer
from transformers.generation.stopping_criteria import StoppingCriteria, StoppingCriteriaList


OUTPUT_END_MARKER = "__AETHER_BENCH_END__"


class TokenSequenceStopper(StoppingCriteria):
    def __init__(self, stop_ids: list[int]) -> None:
        super().__init__()
        self.stop_ids = stop_ids
        self.stop_len = len(stop_ids)

    def __call__(self, input_ids: torch.LongTensor, scores: torch.FloatTensor, **kwargs: object) -> bool:
        if self.stop_len == 0 or input_ids.shape[1] < self.stop_len:
            return False
        tail = input_ids[0, -self.stop_len :].tolist()
        return tail == self.stop_ids


class ModelRunner:
    def __init__(self, model_id: str) -> None:
        self.model_id = model_id
        self.tokenizer = AutoTokenizer.from_pretrained(model_id)
        self.stop_ids = self.tokenizer.encode(OUTPUT_END_MARKER, add_special_tokens=False)
        self.model = AutoModelForCausalLM.from_pretrained(
            model_id,
            dtype=torch.bfloat16,
            device_map={"": 0},
        )
        self.lock = threading.Lock()

    def generate(self, prompt: str, max_new_tokens: int, temperature: float) -> dict[str, object]:
        with self.lock:
            encoded = self.tokenizer(prompt, return_tensors="pt")
            encoded = {key: value.to(self.model.device) for key, value in encoded.items()}
            prompt_tokens = int(encoded["input_ids"].shape[1])
            do_sample = temperature > 0.0
            outputs = self.model.generate(
                **encoded,
                max_new_tokens=max_new_tokens,
                temperature=temperature if do_sample else None,
                do_sample=do_sample,
                pad_token_id=self.tokenizer.eos_token_id,
                stopping_criteria=StoppingCriteriaList([TokenSequenceStopper(self.stop_ids)]),
            )
            generated_ids = outputs[0][prompt_tokens:]
            text = self.tokenizer.decode(generated_ids, skip_special_tokens=True)
            completion_tokens = int(generated_ids.shape[0])
            return {
                "raw_text": text,
                "model": self.model_id,
                "usage": {
                    "prompt_tokens": prompt_tokens,
                    "completion_tokens": completion_tokens,
                    "total_tokens": prompt_tokens + completion_tokens,
                },
            }


def build_handler(runner: ModelRunner) -> type[BaseHTTPRequestHandler]:
    class Handler(BaseHTTPRequestHandler):
        def _send_json(self, status: int, payload: dict[str, object]) -> None:
            body = json.dumps(payload, ensure_ascii=True).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:  # noqa: N802
            if self.path == "/health":
                self._send_json(200, {"ok": True, "model": runner.model_id})
                return
            self._send_json(404, {"ok": False, "error": "not_found"})

        def do_POST(self) -> None:  # noqa: N802
            if self.path != "/generate":
                self._send_json(404, {"ok": False, "error": "not_found"})
                return
            length = int(self.headers.get("Content-Length", "0"))
            raw_body = self.rfile.read(length)
            try:
                payload = json.loads(raw_body.decode("utf-8"))
                prompt = payload["prompt"]
                max_new_tokens = int(payload.get("max_new_tokens", 2048))
                temperature = float(payload.get("temperature", 0.0))
                if not isinstance(prompt, str):
                    raise TypeError("prompt must be a string")
            except Exception as exc:  # pragma: no cover - defensive endpoint handling
                self._send_json(400, {"ok": False, "error": str(exc)})
                return

            try:
                result = runner.generate(
                    prompt=prompt,
                    max_new_tokens=max_new_tokens,
                    temperature=temperature,
                )
            except Exception as exc:  # pragma: no cover - defensive endpoint handling
                self._send_json(500, {"ok": False, "error": str(exc)})
                return

            self._send_json(200, result)

        def log_message(self, format: str, *args: object) -> None:  # noqa: A003
            return

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-id", default="Qwen/Qwen3-4B-Base")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=18081)
    args = parser.parse_args()

    runner = ModelRunner(args.model_id)
    server = ThreadingHTTPServer((args.host, args.port), build_handler(runner))
    print(f"listening on http://{args.host}:{args.port} model={args.model_id}", flush=True)
    server.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
