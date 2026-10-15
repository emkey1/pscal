from unsloth import FastLanguageModel
model, tokenizer = FastLanguageModel.from_pretrained(
    model_name='/storage/hf/hub/models--deepseek-ai--deepseek-coder-6.7b-instruct/snapshots/e5d64addd26a6a1db0f9b863abf6ee3141936807',
    max_seq_length=4096,
    load_in_4bit=False,
)
model.save_pretrained_gguf('/storage/deepseek6.7b-base-gguf', tokenizer, quantization_method='q8_0')
print('GGUF export done')
