import sys, traceback
import importlib.util
_real_find_spec = importlib.util.find_spec
def _patched_find_spec(name, package=None):
    if name == "kernels" or name.startswith("kernels."):
        return None
    return _real_find_spec(name, package)
importlib.util.find_spec = _patched_find_spec

from unsloth import FastLanguageModel

model_id = sys.argv[1]
print(f"=== Attempting load_in_8bit=True for {model_id} ===", flush=True)
try:
    model, tokenizer = FastLanguageModel.from_pretrained(
        attn_implementation="sdpa",
        model_name=model_id,
        max_seq_length=1408,
        dtype=None,
        load_in_4bit=False,
        load_in_8bit=True,
        local_files_only=True,
        device_map="cuda:0",
        gpu_memory_utilization=0.92,
    )
    print("=== from_pretrained OK, attempting get_peft_model ===", flush=True)
    model = FastLanguageModel.get_peft_model(
        model,
        r=32,
        target_modules=["q_proj","k_proj","v_proj","o_proj","gate_proj","up_proj","down_proj"],
        lora_alpha=64,
        lora_dropout=0.0,
        bias="none",
        use_gradient_checkpointing="unsloth",
        random_state=3407,
        use_rslora=False,
        loftq_config=None,
    )
    print("=== get_peft_model OK ===", flush=True)
    import torch
    inputs = tokenizer(text="Hello, this is a smoke test.", return_tensors="pt").to("cuda:0")
    with torch.no_grad():
        out = model(**inputs)
    print("=== forward pass OK, logits shape:", out.logits.shape, "===", flush=True)
    print("SMOKE_TEST_RESULT=SUCCESS", flush=True)
except Exception as exc:
    print("SMOKE_TEST_RESULT=FAILURE", flush=True)
    traceback.print_exc()
    sys.exit(1)
