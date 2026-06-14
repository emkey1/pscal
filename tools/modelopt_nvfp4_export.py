#!/usr/bin/env python3
"""Quantize a merged HF checkpoint to NVFP4 using Model Optimizer."""

from __future__ import annotations

import argparse
import copy
import json
import pathlib
import time

import torch
from modelopt.torch.export import export_hf_checkpoint
import modelopt.torch.quantization as mtq
from transformers import AutoModelForCausalLM, AutoTokenizer


QFORMAT_PRESETS = {
    "nvfp4": "NVFP4_DEFAULT_CFG",
    "nvfp4_default": "NVFP4_DEFAULT_CFG",
    "nvfp4_experts_only": "NVFP4_EXPERTS_ONLY_CFG",
    "nvfp4_mlp_only": "NVFP4_MLP_ONLY_CFG",
    "nvfp4_omlp_only": "NVFP4_OMLP_ONLY_CFG",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-path", required=True)
    parser.add_argument("--export-dir", required=True)
    parser.add_argument(
        "--calibration-jsonl",
        action="append",
        default=[],
        help="JSONL files with benchmark/training prompts.",
    )
    parser.add_argument("--max-samples", type=int, default=192)
    parser.add_argument("--max-seq-length", type=int, default=1024)
    parser.add_argument("--qformat", default="nvfp4_experts_only")
    parser.add_argument("--device-map", default="cuda:0")
    parser.add_argument("--trust-remote-code", action="store_true")
    return parser.parse_args()


def load_calibration_texts(paths: list[str], limit: int) -> list[str]:
    texts: list[str] = []
    for path_str in paths:
        path = pathlib.Path(path_str)
        if not path.exists():
            raise FileNotFoundError(f"Calibration file not found: {path}")
        with path.open("r", encoding="utf-8") as handle:
            for line in handle:
                line = line.strip()
                if not line:
                    continue
                record = json.loads(line)
                for message in record.get("messages", []):
                    if message.get("role") == "user":
                        content = str(message.get("content", "")).strip()
                        if content:
                            texts.append(content)
                            break
                if len(texts) >= limit:
                    return texts
    return texts[:limit]


def resolve_qformat(name: str) -> tuple[str, dict]:
    preset_name = QFORMAT_PRESETS.get(name.lower(), name)
    if not hasattr(mtq, preset_name):
        raise ValueError(f"Unknown qformat preset: {name}")
    return preset_name, copy.deepcopy(getattr(mtq, preset_name))


def build_forward_loop(tokenizer, texts: list[str], max_seq_length: int):
    def forward_loop(model):
        model.eval()
        model_device = next(model.parameters()).device
        for text in texts:
            batch = tokenizer(
                text,
                return_tensors="pt",
                truncation=True,
                max_length=max_seq_length,
            )
            batch = {key: value.to(model_device) for key, value in batch.items()}
            with torch.inference_mode():
                model(**batch)

    return forward_loop


def main() -> int:
    args = parse_args()
    model_path = pathlib.Path(args.model_path)
    export_dir = pathlib.Path(args.export_dir)
    export_dir.mkdir(parents=True, exist_ok=True)

    texts = load_calibration_texts(args.calibration_jsonl, args.max_samples)
    if not texts:
        raise ValueError("No calibration texts were loaded.")

    preset_name, quant_cfg = resolve_qformat(args.qformat)
    print(f"Loading model from {model_path}")
    print(f"Using quantization preset {preset_name}")
    print(
        f"Calibration samples={len(texts)} max_seq_length={args.max_seq_length} device_map={args.device_map}"
    )

    tokenizer = AutoTokenizer.from_pretrained(
        str(model_path),
        trust_remote_code=args.trust_remote_code,
    )
    if tokenizer.pad_token is None and tokenizer.eos_token is not None:
        tokenizer.pad_token = tokenizer.eos_token

    model = AutoModelForCausalLM.from_pretrained(
        str(model_path),
        dtype=torch.bfloat16,
        device_map=args.device_map,
        trust_remote_code=args.trust_remote_code,
        low_cpu_mem_usage=True,
    )

    forward_loop = build_forward_loop(tokenizer, texts, args.max_seq_length)

    start = time.time()
    model = mtq.quantize(model, quant_cfg, forward_loop=forward_loop)
    quantized_at = time.time()
    print(f"Quantization completed in {quantized_at - start:.1f}s")

    with torch.inference_mode():
        export_hf_checkpoint(model, export_dir=str(export_dir))
    exported_at = time.time()
    print(f"Export completed in {exported_at - quantized_at:.1f}s")

    tokenizer.save_pretrained(str(export_dir))
    metadata = {
        "model_path": str(model_path),
        "export_dir": str(export_dir),
        "qformat": preset_name,
        "max_samples": len(texts),
        "max_seq_length": args.max_seq_length,
        "quantize_seconds": quantized_at - start,
        "export_seconds": exported_at - quantized_at,
    }
    (export_dir / "aether_nvfp4_metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(metadata, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
