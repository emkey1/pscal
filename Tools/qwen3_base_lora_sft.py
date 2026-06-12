#!/usr/bin/env python3
"""Run a simple LoRA SFT pass for Aether specialization on Qwen3-4B-Base."""

from __future__ import annotations

import argparse
import importlib.util
import json
import random
import re
from pathlib import Path

# The Spark training venv currently has an optional `kernels` package installed
# that crashes the latest transformers import path on startup. This training
# harness does not need that package, so hide it from availability probes.
_real_find_spec = importlib.util.find_spec


def _patched_find_spec(name: str, package: str | None = None):  # type: ignore[override]
    if name == "kernels" or name.startswith("kernels."):
        return None
    return _real_find_spec(name, package)


importlib.util.find_spec = _patched_find_spec

import torch
from datasets import Dataset
from peft import LoraConfig, get_peft_model
from transformers import AutoModelForCausalLM, AutoTokenizer, TrainerCallback, TrainingArguments
from trl import SFTTrainer


def read_jsonl(path: Path) -> list[dict]:
    rows: list[dict] = []
    with path.open("r", encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                rows.append(json.loads(line))
    return rows


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def format_messages(record: dict) -> str:
    parts: list[str] = []
    for message in record.get("messages", []):
        role = str(message.get("role", "")).strip().upper()
        content = str(message.get("content", "")).strip()
        if not role or not content:
            continue
        parts.append(f"### {role}\n{content}")
    return "\n\n".join(parts).strip() + "\n"


def chunk_reference_text(text: str, *, max_chars: int = 6000) -> list[str]:
    sections: list[str] = []
    current: list[str] = []
    current_len = 0
    for line in text.splitlines():
        is_heading = line.startswith("#")
        line_len = len(line) + 1
        if current and ((is_heading and current_len >= max_chars // 2) or current_len + line_len > max_chars):
            sections.append("\n".join(current).strip() + "\n")
            current = []
            current_len = 0
        current.append(line)
        current_len += line_len
    if current:
        sections.append("\n".join(current).strip() + "\n")
    return [section for section in sections if section.strip()]


def slugify(value: str) -> str:
    return re.sub(r"[^A-Za-z0-9._-]+", "_", value).strip("._-") or "reference"


def load_records(instruction_jsonl: Path, repair_jsonl: Path) -> list[dict]:
    records = read_jsonl(instruction_jsonl) + read_jsonl(repair_jsonl)
    filtered: list[dict] = []
    for record in records:
        verification = record.get("verification") or {}
        if verification.get("returncode") != 0:
            continue
        text = format_messages(record)
        filtered.append(
            {
                "id": record.get("id"),
                "kind": record.get("kind"),
                "text": text,
            }
        )
    return filtered


def load_reference_records(reference_json: Path | None) -> list[dict]:
    if reference_json is None or not reference_json.exists():
        return []
    payload = read_json(reference_json)
    records: list[dict] = []
    for item in payload.get("items", []):
        path = str(item.get("path", "reference"))
        content = str(item.get("content", "")).strip()
        if not content:
            continue
        for idx, chunk in enumerate(chunk_reference_text(content), start=1):
            records.append(
                {
                    "id": f"{slugify(path)}_chunk_{idx}",
                    "kind": "reference_corpus",
                    "text": f"### REFERENCE\n{path}\n\n{chunk}".strip() + "\n",
                }
            )
    return records


class SaveMetadataCallback(TrainerCallback):
    def __init__(self, path: Path, metadata: dict) -> None:
        self.path = path
        self.metadata = metadata

    def on_train_begin(self, args, state, control, **kwargs):  # noqa: ANN001
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.path.write_text(json.dumps(self.metadata, indent=2), encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-id", default="Qwen/Qwen3-4B-Base")
    parser.add_argument("--instruction-jsonl", type=Path, required=True)
    parser.add_argument("--repair-jsonl", type=Path, required=True)
    parser.add_argument("--reference-json", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--learning-rate", type=float, default=2e-4)
    parser.add_argument("--weight-decay", type=float, default=0.0)
    parser.add_argument("--warmup-ratio", type=float, default=0.03)
    parser.add_argument("--epochs", type=float, default=6.0)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--grad-accum", type=int, default=8)
    parser.add_argument("--max-seq-len", type=int, default=8192)
    parser.add_argument("--lora-r", type=int, default=32)
    parser.add_argument("--lora-alpha", type=int, default=64)
    parser.add_argument("--lora-dropout", type=float, default=0.05)
    parser.add_argument("--train-split", type=float, default=0.9)
    parser.add_argument("--save-steps", type=int, default=20)
    parser.add_argument("--logging-steps", type=int, default=1)
    args = parser.parse_args()

    random.seed(args.seed)
    torch.manual_seed(args.seed)

    records = load_records(args.instruction_jsonl, args.repair_jsonl)
    reference_records = load_reference_records(args.reference_json)
    if len(records) < 2:
        raise SystemExit("need at least two verified records for train/eval split")

    random.shuffle(records)
    split_index = max(1, min(len(records) - 1, int(len(records) * args.train_split)))
    train_records = records[:split_index]
    eval_records = records[split_index:]
    random.shuffle(reference_records)
    train_records = train_records + reference_records

    train_dataset = Dataset.from_list(train_records)
    eval_dataset = Dataset.from_list(eval_records)

    tokenizer = AutoTokenizer.from_pretrained(args.model_id, use_fast=True)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token
    tokenizer.padding_side = "right"
    tokenizer.model_max_length = args.max_seq_len

    model = AutoModelForCausalLM.from_pretrained(
        args.model_id,
        torch_dtype=torch.bfloat16,
        low_cpu_mem_usage=True,
    )
    model.config.use_cache = False

    lora_config = LoraConfig(
        r=args.lora_r,
        lora_alpha=args.lora_alpha,
        lora_dropout=args.lora_dropout,
        bias="none",
        task_type="CAUSAL_LM",
        target_modules=[
            "q_proj",
            "k_proj",
            "v_proj",
            "o_proj",
            "gate_proj",
            "up_proj",
            "down_proj",
        ],
    )
    model = get_peft_model(model, lora_config)

    training_args = TrainingArguments(
        output_dir=str(args.output_dir),
        num_train_epochs=args.epochs,
        per_device_train_batch_size=args.batch_size,
        per_device_eval_batch_size=1,
        gradient_accumulation_steps=args.grad_accum,
        learning_rate=args.learning_rate,
        weight_decay=args.weight_decay,
        warmup_ratio=args.warmup_ratio,
        bf16=True,
        logging_steps=args.logging_steps,
        save_steps=args.save_steps,
        eval_strategy="steps",
        eval_steps=args.save_steps,
        save_strategy="steps",
        report_to="none",
        seed=args.seed,
    )

    metadata = {
        "model_id": args.model_id,
        "instruction_jsonl": str(args.instruction_jsonl),
        "repair_jsonl": str(args.repair_jsonl),
        "reference_json": str(args.reference_json) if args.reference_json else None,
        "train_records": len(train_records),
        "eval_records": len(eval_records),
        "supervised_train_records": len(records[:split_index]),
        "reference_train_records": len(reference_records),
        "max_seq_len": args.max_seq_len,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "grad_accum": args.grad_accum,
        "learning_rate": args.learning_rate,
        "lora_r": args.lora_r,
        "lora_alpha": args.lora_alpha,
        "lora_dropout": args.lora_dropout,
    }

    trainer = SFTTrainer(
        model=model,
        args=training_args,
        train_dataset=train_dataset,
        eval_dataset=eval_dataset,
        processing_class=tokenizer,
        formatting_func=lambda record: record["text"],
    )
    trainer.add_callback(SaveMetadataCallback(args.output_dir / "run_metadata.json", metadata))
    trainer.train()
    trainer.save_model(str(args.output_dir / "final"))
    tokenizer.save_pretrained(str(args.output_dir / "final"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
