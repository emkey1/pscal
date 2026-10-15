#!/usr/bin/env python3
import argparse, json, os
from pathlib import Path
import yaml
import torch
from datasets import load_dataset
from transformers import AutoModelForCausalLM, AutoModelForImageTextToText, AutoTokenizer
from trl import SFTConfig
from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training
from trl import SFTTrainer


def read_cfg(path):
    with open(path, 'r') as f:
        return yaml.safe_load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config', required=True)
    args = ap.parse_args()
    cfg = read_cfg(args.config)
    if cfg.get('load_in_4bit'):
        raise SystemExit('Refusing to run: load_in_4bit is forbidden by OpenClaw precision policy.')
    max_steps_env = os.environ.get('OPENCLAW_TRAIN_MAX_STEPS')
    if max_steps_env:
        cfg['max_steps'] = int(max_steps_env)
        cfg['num_train_epochs'] = 1
    model_name = cfg['model_name_or_path']
    tokenizer_kwargs = {'trust_remote_code': True}
    # Mistral/Ministral 3 tokenizers warn unless this compatibility fix is explicit.
    tokenizer_kwargs['fix_mistral_regex'] = True
    tokenizer = AutoTokenizer.from_pretrained(model_name, **tokenizer_kwargs)
    if tokenizer.pad_token is None:
        tokenizer.pad_token = tokenizer.eos_token
    load_kwargs = {
        'torch_dtype': torch.bfloat16,
        'device_map': 'auto',
        'trust_remote_code': True,
    }
    # Optional 8-bit path only. Never 4-bit.
    if cfg.get('load_in_8bit'):
        load_kwargs['load_in_8bit'] = True
    try:
        model = AutoModelForCausalLM.from_pretrained(model_name, **load_kwargs)
    except Exception as exc:
        print(f"AutoModelForCausalLM failed ({exc}); falling back to AutoModelForImageTextToText")
        model = AutoModelForImageTextToText.from_pretrained(model_name, **load_kwargs)
    model.config.use_cache = False
    if cfg.get('load_in_8bit'):
        model = prepare_model_for_kbit_training(model)
    if cfg.get('gradient_checkpointing', True):
        model.gradient_checkpointing_enable()
    lora = LoraConfig(
        r=int(cfg.get('lora_r', 16)),
        lora_alpha=int(cfg.get('lora_alpha', 32)),
        lora_dropout=float(cfg.get('lora_dropout', 0.05)),
        bias='none',
        task_type='CAUSAL_LM',
        target_modules=list(cfg.get('target_modules') or []),
    )
    model = get_peft_model(model, lora)
    model.print_trainable_parameters()

    ds = load_dataset('json', data_files={'train': cfg['train_file'], 'eval': cfg['eval_file']})
    def format_example(ex):
        msgs = ex.get('messages') or []
        return tokenizer.apply_chat_template(msgs, tokenize=False, add_generation_prompt=False)

    out = cfg['output_dir']
    train_args = SFTConfig(
        output_dir=out,
        per_device_train_batch_size=int(cfg.get('per_device_train_batch_size', 1)),
        gradient_accumulation_steps=int(cfg.get('gradient_accumulation_steps', 8)),
        learning_rate=float(cfg.get('learning_rate', 1e-4)),
        num_train_epochs=float(cfg.get('num_train_epochs', 1)),
        max_steps=int(cfg.get('max_steps', -1)),
        logging_steps=int(cfg.get('logging_steps', 5)),
        save_steps=int(cfg.get('save_steps', 100)),
        eval_steps=int(cfg.get('eval_steps', 100)),
        eval_strategy='steps',
        bf16=True,
        fp16=False,
        report_to=['tensorboard'],
        gradient_checkpointing=bool(cfg.get('gradient_checkpointing', True)),
        remove_unused_columns=True,
        max_length=int(cfg.get('max_seq_length', 8192)),
        packing=False,
    )
    trainer = SFTTrainer(
        model=model,
        processing_class=tokenizer,
        train_dataset=ds['train'],
        eval_dataset=ds['eval'],
        args=train_args,
        formatting_func=format_example,
    )
    trainer.train()
    trainer.save_model(out)
    tokenizer.save_pretrained(out)
    Path(out, 'openclaw_precision_policy.json').write_text(json.dumps({
        'base_precision': 'bf16' if not cfg.get('load_in_8bit') else 'int8/bf16 mixed',
        'load_in_8bit': bool(cfg.get('load_in_8bit')),
        'load_in_4bit': False,
        'max_seq_length': int(cfg.get('max_seq_length', 8192)),
    }, indent=2))

if __name__ == '__main__':
    main()
