import sys, json, importlib.util
_real_find_spec = importlib.util.find_spec
def _patched_find_spec(name, package=None):
    if name == "kernels" or name.startswith("kernels."):
        return None
    return _real_find_spec(name, package)
importlib.util.find_spec = _patched_find_spec

from unsloth import FastLanguageModel

base_model_id = sys.argv[1]
adapter_dir = sys.argv[2]

model, tokenizer = FastLanguageModel.from_pretrained(
    attn_implementation="sdpa",
    model_name=base_model_id,
    max_seq_length=1408,
    dtype=None,
    load_in_4bit=False,
    load_in_8bit=True,
    local_files_only=True,
    device_map="cuda:0",
    gpu_memory_utilization=0.92,
)
from peft import PeftModel
model = PeftModel.from_pretrained(model, adapter_dir)

qc = getattr(model.config, "quantization_config", None)
print("=== quantization_config type:", type(qc))
if qc is not None:
    d = qc.to_dict() if hasattr(qc, "to_dict") else dict(qc)
    for k, v in d.items():
        print(f"  {k!r}: {type(v)} = {v!r}"[:200])
        try:
            json.dumps({k: v})
        except TypeError as e:
            print(f"    ^^ NOT JSON SERIALIZABLE: {e}")

print("\n=== attempting config.save_pretrained with quantization_config present (expect crash) ===")
try:
    model.config.save_pretrained("/tmp/diag_test_with_qc")
    print("UNEXPECTED: succeeded with quantization_config present")
except TypeError as e:
    print(f"CONFIRMED CRASH: {e}")

print("\n=== attempting with quantization_config cleared to None ===")
model.config.quantization_config = None
try:
    model.config.save_pretrained("/tmp/diag_test_no_qc")
    print("SUCCESS: save_pretrained works with quantization_config=None")
except Exception as e:
    print(f"STILL FAILS: {e}")
