#!/usr/bin/env python3
"""Run Unsloth QLoRA SFT for Aether on Qwen3-Coder-30B-A3B-Instruct."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import random
import re
from pathlib import Path

# Match the existing Qwen training harness workaround.
_real_find_spec = importlib.util.find_spec


def _patched_find_spec(name: str, package: str | None = None):  # type: ignore[override]
    if name == "kernels" or name.startswith("kernels."):
        return None
    return _real_find_spec(name, package)


importlib.util.find_spec = _patched_find_spec

import torch
from datasets import Dataset
from unsloth import FastLanguageModel
from transformers import AutoTokenizer, EarlyStoppingCallback
from trl import SFTConfig, SFTTrainer


TARGET_MODULES = [
    "q_proj",
    "k_proj",
    "v_proj",
    "o_proj",
    "gate_proj",
    "up_proj",
    "down_proj",
]


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


def round_up(value: int, multiple: int) -> int:
    if multiple <= 0:
        return value
    return ((value + multiple - 1) // multiple) * multiple


def materialize_chat_text(tokenizer: AutoTokenizer, messages: list[dict]) -> str:
    rendered = tokenizer.apply_chat_template(
        messages,
        tokenize=False,
        add_generation_prompt=False,
    )
    if not isinstance(rendered, str):
        raise TypeError("chat template did not return text")
    return rendered


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


def load_supervised_records(path: Path) -> list[dict]:
    if not path.exists():
        return []
    rows = read_jsonl(path)
    records: list[dict] = []
    for row in rows:
        verification = row.get("verification") or {}
        if verification.get("returncode") != 0:
            continue
        records.append(
            {
                "id": row.get("id", ""),
                "kind": row.get("kind", ""),
                "messages": row.get("messages", []),
            }
        )
    return records


def load_reference_records(path: Path | None) -> list[dict]:
    if path is None or not path.exists():
        return []
    payload = read_json(path)
    records: list[dict] = []
    for item in payload.get("items", []):
        ref_path = str(item.get("path", "reference"))
        content = str(item.get("content", "")).strip()
        if not content:
            continue
        for idx, chunk in enumerate(chunk_reference_text(content), start=1):
            records.append(
                {
                    "id": f"{slugify(ref_path)}_chunk_{idx}",
                    "kind": "reference_corpus",
                    "text": f"### REFERENCE\n{ref_path}\n\n{chunk}".strip() + "\n",
                }
            )
    return records


def load_raw_corpus_records(path: Path | None) -> list[dict]:
    if path is None or not path.exists():
        return []
    payload = read_json(path)
    records: list[dict] = []
    for item in payload.get("items", []):
        corpus_path = str(item.get("path", "corpus"))
        content = str(item.get("content", "")).strip()
        if not content:
            continue
        records.append(
            {
                "id": f"raw_{slugify(corpus_path)}",
                "kind": "raw_aether_corpus",
                "text": content + "\n",
            }
        )
    return records


def attach_text(records: list[dict], tokenizer: AutoTokenizer) -> list[dict]:
    enriched: list[dict] = []
    for record in records:
        text = record.get("text")
        if not isinstance(text, str):
            text = materialize_chat_text(tokenizer, record["messages"])
        enriched.append(
            {
                "id": record["id"],
                "kind": record["kind"],
                "messages": record.get("messages", []),
                "text": text,
            }
        )
    return enriched


def compute_dynamic_max_seq_length(
    records: list[dict],
    tokenizer: AutoTokenizer,
    *,
    margin_tokens: int,
    round_multiple: int,
    min_seq_length: int,
    max_seq_length_cap: int,
) -> tuple[int, int]:
    longest = 0
    for record in records:
        token_count = len(tokenizer(record["text"], add_special_tokens=False)["input_ids"])
        if token_count > longest:
            longest = token_count
    suggested = round_up(longest + margin_tokens, round_multiple)
    suggested = max(suggested, min_seq_length)
    suggested = min(suggested, max_seq_length_cap)
    return suggested, longest


def build_datasets(
    *,
    instruction_jsonl: Path,
    repair_jsonl: Path,
    corpus_json: Path | None,
    reference_json: Path | None,
    seed: int,
    eval_cases: int,
    tokenizer: AutoTokenizer,
    include_raw_corpus: bool,
    include_reference: bool,
) -> tuple[list[dict], list[dict]]:
    supervised = load_supervised_records(instruction_jsonl) + load_supervised_records(repair_jsonl)
    raw_corpus = load_raw_corpus_records(corpus_json) if include_raw_corpus else []
    reference_records = load_reference_records(reference_json) if include_reference else []

    rng = random.Random(seed)

    if supervised:
        # Hold the eval split out of the instruction-style records so eval_loss and
        # load_best_model_at_end track instruction-following, not raw continuation.
        # (v6 held out raw corpus and therefore selected the most over-specialized,
        # least instruction-following checkpoint -> no-guide accuracy collapsed to 0.)
        pool = list(supervised)
        rng.shuffle(pool)
        holdout = min(max(1, eval_cases), len(pool) - 1)
        eval_records = pool[:holdout]
        train_records = pool[holdout:] + raw_corpus + reference_records
    else:
        # Legacy fallback: no supervised data, hold out from raw corpus.
        if len(raw_corpus) < 2:
            raise SystemExit(
                "need supervised records (instruction/repair) or at least two raw corpus records"
            )
        pool = list(raw_corpus)
        rng.shuffle(pool)
        holdout = min(max(1, eval_cases), len(pool) - 1)
        eval_records = pool[:holdout]
        train_records = pool[holdout:] + reference_records

    return attach_text(train_records, tokenizer), attach_text(eval_records, tokenizer)


def export_merged_16bit(model, tokenizer, output_dir: Path) -> Path:
    """Dequantize the 4-bit base, merge the LoRA, and write a 16-bit HF checkpoint.

    This is the vLLM-loadable / Model-Optimizer-loadable artifact. NVFP4 export
    (modelopt_nvfp4_export.py) consumes exactly this directory. ~57 GiB for the
    30B base, so point --merged-output-dir at /storage (NFS), not / (constrained).
    """
    output_dir.mkdir(parents=True, exist_ok=True)
    # Unsloth's 8-bit loading path (FastBaseModel.from_pretrained) monkeypatches a
    # get_loading_attributes lambda directly onto the BitsAndBytesConfig instance;
    # unsloth_zoo's merged_16bit save calls config.save_pretrained() (which JSON-dumps
    # quantization_config) BEFORE its own _remove_quantization_config cleanup runs, so
    # that lambda crashes json.dumps with "Object of type function is not JSON
    # serializable". Strip it before saving; no-op when absent (4-bit/16-bit runs).
    quant_config = getattr(model.config, "quantization_config", None)
    if quant_config is not None and hasattr(quant_config, "get_loading_attributes"):
        try:
            delattr(quant_config, "get_loading_attributes")
        except (AttributeError, TypeError):
            pass
    model.save_pretrained_merged(str(output_dir), tokenizer, save_method="merged_16bit")
    return output_dir


def export_gguf_variants(model, tokenizer, output_dir: Path, methods: list[str]) -> list[str]:
    completed: list[str] = []
    if not methods:
        return completed
    gguf_dir = output_dir / "gguf"
    gguf_dir.mkdir(parents=True, exist_ok=True)
    for method in methods:
        target = gguf_dir / method
        target.parent.mkdir(parents=True, exist_ok=True)
        model.save_pretrained_gguf(str(target), tokenizer, quantization_method=method)
        completed.append(method)
    return completed


def export_only(
    *,
    model_id: str,
    adapter_dir: Path,
    export_dir: Path,
    methods: list[str],
    max_seq_length: int,
    dtype: torch.dtype | None,
    load_in_4bit: bool,
    load_in_8bit: bool,
    local_files_only: bool,
    device_map: str,
    gpu_memory_utilization: float,
    export_merged: bool = False,
    merged_output_dir: Path | None = None,
) -> list[str]:
    model, tokenizer = FastLanguageModel.from_pretrained(
        attn_implementation="sdpa",
        model_name=str(adapter_dir),
        max_seq_length=max_seq_length,
        dtype=dtype,
        load_in_4bit=load_in_4bit,
        load_in_8bit=load_in_8bit,
        local_files_only=local_files_only,
        device_map=device_map,
        gpu_memory_utilization=gpu_memory_utilization,
    )
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token
    tokenizer.padding_side = "right"
    export_dir.mkdir(parents=True, exist_ok=True)
    if export_merged:
        merged_dir = merged_output_dir if merged_output_dir is not None else (export_dir / "merged_16bit")
        export_merged_16bit(model, tokenizer, merged_dir)
    return export_gguf_variants(model, tokenizer, export_dir, methods)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model-id", default="unsloth/Qwen3-Coder-30B-A3B-Instruct")
    parser.add_argument("--instruction-jsonl", type=Path, required=True)
    parser.add_argument("--repair-jsonl", type=Path, required=True)
    parser.add_argument("--corpus-json", type=Path)
    parser.add_argument("--reference-json", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--adapter-dir", type=Path)
    parser.add_argument("--seed", type=int, default=3407)
    parser.add_argument("--eval-cases", type=int, default=12)
    # v7 defaults: short + gentle. 8 epochs at 2e-4 over ~130 tiny records (v6)
    # over-specialized the model and eroded base instruction-following.
    parser.add_argument("--epochs", type=float, default=3.0)
    parser.add_argument("--batch-size", type=int, default=1)
    parser.add_argument("--grad-accum", type=int, default=8)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--weight-decay", type=float, default=0.0)
    parser.add_argument("--warmup-ratio", type=float, default=0.03)
    parser.add_argument("--logging-steps", type=int, default=1)
    parser.add_argument("--eval-steps", type=int, default=5)
    parser.add_argument("--save-steps", type=int, default=5)
    parser.add_argument("--early-stopping-patience", type=int, default=2)
    parser.add_argument("--load-best-model-at-end", action="store_true", default=True)
    parser.add_argument("--no-load-best-model-at-end", dest="load_best_model_at_end", action="store_false", help="export the final-step checkpoint instead of the best-eval-loss one")
    parser.add_argument("--margin-tokens", type=int, default=64)
    parser.add_argument("--round-multiple", type=int, default=128)
    parser.add_argument("--min-seq-length", type=int, default=1024)
    parser.add_argument("--max-seq-length-cap", type=int, default=32768)
    parser.add_argument("--max-seq-length", type=int, default=0, help="override auto-sized max_seq_length when > 0")
    parser.add_argument("--dtype", default="auto", choices=["auto", "bfloat16", "float16"])
    parser.add_argument("--load-in-4bit", action="store_true", default=True)
    parser.add_argument("--no-load-in-4bit", dest="load_in_4bit", action="store_false")
    parser.add_argument("--load-in-8bit", action="store_true", default=False,
                         help="bitsandbytes 8-bit quantization; mutually exclusive with "
                              "--load-in-4bit (pass --no-load-in-4bit alongside this)")
    parser.add_argument("--local-files-only", action="store_true", default=True)
    parser.add_argument("--allow-network-download", dest="local_files_only", action="store_false")
    parser.add_argument("--device-map", default="cuda:0")
    parser.add_argument("--gpu-memory-utilization", type=float, default=0.92)
    parser.add_argument("--lora-r", type=int, default=16)
    parser.add_argument("--lora-alpha", type=int, default=32)
    parser.add_argument("--lora-dropout", type=float, default=0.0)
    parser.add_argument("--random-state", type=int, default=3407)
    parser.add_argument("--target-modules", default="",
                        help="comma-separated LoRA target module names; empty uses the default set. "
                             "For hybrid-attention archs (qwen3_5) add the linear_attn projections.")
    parser.add_argument("--max-steps", type=int, default=-1,
                        help="cap optimizer steps, overriding epochs when > 0 (smoke tests)")
    parser.add_argument("--gradient-checkpointing", default="unsloth",
                        choices=["unsloth", "true", "false"],
                        help="gradient checkpointing mode; 'false' disables it (needs more VRAM, "
                             "but avoids Unsloth's offload-recompute path)")
    parser.add_argument(
        "--export-gguf",
        default="",
        help="comma-separated GGUF methods to export after training; empty disables (v7 targets NVFP4, not GGUF)",
    )
    # v7: instruction-only supervision. Raw corpus and the reference guide are NOT
    # language-modeled as bare completions by default -- doing so taught the model to
    # echo Aether/guide text and is what drove no-guide accuracy to 0. Each corpus
    # program already appears as the assistant response inside the instruction JSONL.
    parser.add_argument("--include-raw-corpus", action="store_true", default=False,
                        help="also train on bare corpus source as plain completions (off by default)")
    parser.add_argument("--include-reference", action="store_true", default=False,
                        help="also train on reference-guide chunks as plain completions (off by default)")
    parser.add_argument("--train-on-responses-only", dest="train_on_responses_only",
                        action="store_true", default=True,
                        help="mask the prompt; compute loss on assistant Aether only (default)")
    parser.add_argument("--no-train-on-responses-only", dest="train_on_responses_only",
                        action="store_false")
    parser.add_argument("--instruction-part", default="<|im_start|>user\n",
                        help="prompt-turn marker for response masking (default: ChatML/Qwen; "
                             "Granite uses '<|start_of_role|>user<|end_of_role|>'). Supports \\n escapes.")
    parser.add_argument("--response-part", default="<|im_start|>assistant\n",
                        help="assistant-turn marker for response masking (default: ChatML/Qwen). Supports \\n escapes.")
    parser.add_argument("--export-merged-16bit", dest="export_merged_16bit",
                        action="store_true", default=True,
                        help="write a merged 16-bit HF checkpoint for vLLM / NVFP4 (default)")
    parser.add_argument("--no-export-merged-16bit", dest="export_merged_16bit",
                        action="store_false")
    parser.add_argument("--merged-output-dir", type=Path, default=None,
                        help="where to write the merged 16-bit checkpoint; defaults to <output-dir>/final/merged_16bit. "
                             "Point at /storage for the 30B (~57 GiB) to avoid filling /.")
    args = parser.parse_args()

    if args.load_in_8bit and args.load_in_4bit:
        raise SystemExit("--load-in-8bit requires --no-load-in-4bit (both quantizations cannot be active)")

    random.seed(args.seed)
    torch.manual_seed(args.seed)

    dtype = None
    if args.dtype == "bfloat16":
        dtype = torch.bfloat16
    elif args.dtype == "float16":
        dtype = torch.float16

    methods = [method.strip() for method in args.export_gguf.split(",") if method.strip()]
    if args.adapter_dir is not None:
        exported = export_only(
            model_id=args.model_id,
            adapter_dir=args.adapter_dir,
            export_dir=args.output_dir,
            methods=methods,
            max_seq_length=args.max_seq_length if args.max_seq_length > 0 else args.min_seq_length,
            dtype=dtype,
            load_in_4bit=args.load_in_4bit,
            load_in_8bit=args.load_in_8bit,
            local_files_only=args.local_files_only,
            device_map=args.device_map,
            gpu_memory_utilization=args.gpu_memory_utilization,
            export_merged=args.export_merged_16bit,
            merged_output_dir=args.merged_output_dir,
        )
        if exported:
            (args.output_dir / "gguf_exports.json").write_text(
                json.dumps({"methods": exported}, indent=2),
                encoding="utf-8",
            )
        return 0

    tokenizer = AutoTokenizer.from_pretrained(
        args.model_id,
        use_fast=True,
        local_files_only=args.local_files_only,
    )
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token
    tokenizer.padding_side = "right"

    train_records, eval_records = build_datasets(
        instruction_jsonl=args.instruction_jsonl,
        repair_jsonl=args.repair_jsonl,
        corpus_json=args.corpus_json,
        reference_json=args.reference_json,
        seed=args.seed,
        eval_cases=args.eval_cases,
        tokenizer=tokenizer,
        include_raw_corpus=args.include_raw_corpus,
        include_reference=args.include_reference,
    )

    all_records = train_records + eval_records
    auto_max_seq_length, longest_record = compute_dynamic_max_seq_length(
        all_records,
        tokenizer,
        margin_tokens=args.margin_tokens,
        round_multiple=args.round_multiple,
        min_seq_length=args.min_seq_length,
        max_seq_length_cap=args.max_seq_length_cap,
    )
    max_seq_length = args.max_seq_length if args.max_seq_length > 0 else auto_max_seq_length

    model, unsloth_tokenizer = FastLanguageModel.from_pretrained(
        attn_implementation="sdpa",
        model_name=args.model_id,
        max_seq_length=max_seq_length,
        dtype=dtype,
        load_in_4bit=args.load_in_4bit,
        load_in_8bit=args.load_in_8bit,
        local_files_only=args.local_files_only,
        device_map=args.device_map,
        gpu_memory_utilization=args.gpu_memory_utilization,
    )
    if unsloth_tokenizer.pad_token is None:
        unsloth_tokenizer.pad_token = unsloth_tokenizer.eos_token
    unsloth_tokenizer.padding_side = "right"

    target_modules = [m.strip() for m in args.target_modules.split(",") if m.strip()] or TARGET_MODULES
    print(f"LoRA target modules ({len(target_modules)}): {target_modules}", flush=True)
    gc_value = {"unsloth": "unsloth", "true": True, "false": False}[args.gradient_checkpointing]
    print(f"use_gradient_checkpointing = {gc_value!r}", flush=True)
    model = FastLanguageModel.get_peft_model(
        model,
        r=args.lora_r,
        target_modules=target_modules,
        lora_alpha=args.lora_alpha,
        lora_dropout=args.lora_dropout,
        bias="none",
        use_gradient_checkpointing=gc_value,
        random_state=args.random_state,
        use_rslora=False,
        loftq_config=None,
    )

    train_dataset = Dataset.from_list(train_records)
    eval_dataset = Dataset.from_list(eval_records)
    # Newer TRL (transformers 5.x) treats a `messages` column as a conversational
    # dataset and re-templates it, which conflicts with our pre-rendered `text`
    # (RuntimeError: Could not infer dtype of dict). Keep only `text` so SFTTrainer
    # tokenizes the strings we already built via materialize_chat_text.
    train_dataset = train_dataset.remove_columns([c for c in train_dataset.column_names if c != "text"])
    eval_dataset = eval_dataset.remove_columns([c for c in eval_dataset.column_names if c != "text"])

    args.output_dir.mkdir(parents=True, exist_ok=True)

    trainer = SFTTrainer(
        model=model,
        tokenizer=unsloth_tokenizer,
        train_dataset=train_dataset,
        eval_dataset=eval_dataset,
        dataset_text_field="text",
        max_seq_length=max_seq_length,
        args=SFTConfig(
            output_dir=str(args.output_dir),
            num_train_epochs=args.epochs,
            max_steps=args.max_steps,
            per_device_train_batch_size=args.batch_size,
            per_device_eval_batch_size=1,
            gradient_accumulation_steps=args.grad_accum,
            learning_rate=args.learning_rate,
            weight_decay=args.weight_decay,
            warmup_ratio=args.warmup_ratio,
            logging_steps=args.logging_steps,
            eval_steps=args.eval_steps,
            save_steps=args.save_steps,
            eval_strategy="steps",
            save_strategy="steps",
            save_total_limit=2,
            load_best_model_at_end=args.load_best_model_at_end,
            metric_for_best_model="eval_loss",
            greater_is_better=False,
            bf16=True,
            optim="adamw_8bit",
            lr_scheduler_type="linear",
            seed=args.seed,
            report_to="none",
            packing=False,
        ),
    )

    if args.train_on_responses_only:
        # Compute loss on the assistant Aether only, not on the system/user prompt.
        # Qwen3-Coder uses the ChatML turn markers below. The trainer previously
        # computed loss over the whole sequence (prompt included).
        try:
            from unsloth.chat_templates import train_on_responses_only
        except Exception as exc:  # pragma: no cover - depends on remote image
            raise SystemExit(
                f"train_on_responses_only unavailable ({exc}); "
                "rerun with --no-train-on-responses-only to disable prompt masking"
            )
        trainer = train_on_responses_only(
            trainer,
            instruction_part=args.instruction_part.encode().decode("unicode_escape"),
            response_part=args.response_part.encode().decode("unicode_escape"),
        )

    trainer.add_callback(
        EarlyStoppingCallback(
            early_stopping_patience=args.early_stopping_patience,
            early_stopping_threshold=0.0,
        )
    )

    metadata = {
        "model_id": args.model_id,
        "instruction_jsonl": str(args.instruction_jsonl),
        "repair_jsonl": str(args.repair_jsonl),
        "corpus_json": str(args.corpus_json) if args.corpus_json else None,
        "reference_json": str(args.reference_json) if args.reference_json else None,
        "train_records": len(train_records),
        "eval_records": len(eval_records),
        "eval_case_ids": [record["id"] for record in eval_records],
        "longest_tokenized_record": longest_record,
        "max_seq_length": max_seq_length,
        "auto_max_seq_length": auto_max_seq_length,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "grad_accum": args.grad_accum,
        "learning_rate": args.learning_rate,
        "load_in_4bit": args.load_in_4bit,
        "load_in_8bit": args.load_in_8bit,
        "local_files_only": args.local_files_only,
        "dtype": args.dtype,
        "device_map": args.device_map,
        "gpu_memory_utilization": args.gpu_memory_utilization,
        "target_modules": target_modules,
        "lora_r": args.lora_r,
        "lora_alpha": args.lora_alpha,
        "lora_dropout": args.lora_dropout,
        "learning_rate": args.learning_rate,
        "train_on_responses_only": args.train_on_responses_only,
        "include_raw_corpus": args.include_raw_corpus,
        "include_reference": args.include_reference,
        "eval_holdout_source": "supervised" if load_supervised_records(args.instruction_jsonl) or load_supervised_records(args.repair_jsonl) else "raw_corpus",
        "export_merged_16bit": args.export_merged_16bit,
        "merged_output_dir": str(args.merged_output_dir) if args.merged_output_dir is not None else str(args.output_dir / "final" / "merged_16bit"),
    }
    (args.output_dir / "run_metadata.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")

    trainer.train()
    final_dir = args.output_dir / "final"
    trainer.save_model(str(final_dir))
    unsloth_tokenizer.save_pretrained(str(final_dir))

    if args.export_merged_16bit:
        merged_dir = args.merged_output_dir if args.merged_output_dir is not None else (final_dir / "merged_16bit")
        export_merged_16bit(model, unsloth_tokenizer, merged_dir)
        (args.output_dir / "merged_export.json").write_text(
            json.dumps({"merged_16bit": str(merged_dir)}, indent=2),
            encoding="utf-8",
        )

    exported = export_gguf_variants(model, unsloth_tokenizer, final_dir, methods)
    if exported:
        (args.output_dir / "gguf_exports.json").write_text(
            json.dumps({"methods": exported}, indent=2),
            encoding="utf-8",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
