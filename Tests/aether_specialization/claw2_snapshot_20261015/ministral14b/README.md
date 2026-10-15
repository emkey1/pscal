# Ministral 3 14B Reasoning Refinement Scaffold

Base model: `mistralai/Ministral-3-14B-Reasoning-2512`

Precision rule from Mike: **do not drop below 8-bit quantization**. This scaffold defaults to BF16 base weights + LoRA adapters. It intentionally does not use 4-bit/NF4/QLoRA.

## Practical context targets on one claw1

- 8K: default / realistic first target
- 16K: stretch target with gradient checkpointing and small batch
- 32K: experimental only
- 128K/256K: serving context, not realistic for local refinement on one node

## Layout on claw1

- Model: `$HOME/models/hf/mistralai/Ministral-3-14B-Reasoning-2512`
- Workdir: `$HOME/training/ministral14b`
- Datasets: `$HOME/datasets/openclaw-sft-v0`
- Outputs: `$HOME/training/ministral14b/outputs`

## Workflow

1. `scripts/download_ministral14b.sh`
2. `scripts/build_training_image.sh`
3. Put JSONL train/eval files in `$HOME/datasets/openclaw-sft-v0/`
4. `scripts/smoke_train_lora.sh`
5. If smoke passes, run a real LoRA job with `scripts/train_lora.sh`

## Dataset format

JSONL, one example per line:

```json
{"messages":[{"role":"system","content":"..."},{"role":"user","content":"..."},{"role":"assistant","content":"..."}]}
```

Keep early data small and high quality. Best first domains:
- finance/news synthesis
- report writing style
- queue/tool-routing decisions
- failure handling and missing-data disclosure
