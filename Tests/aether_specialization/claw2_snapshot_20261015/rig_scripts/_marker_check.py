from transformers import AutoTokenizer
tok = AutoTokenizer.from_pretrained("/workspace/model-mirror", use_fast=True, local_files_only=True)
msgs = [
    {"role": "system", "content": "SYS"},
    {"role": "user", "content": "USER_PROMPT"},
    {"role": "assistant", "content": "ASSISTANT_BODY"},
]
text = tok.apply_chat_template(msgs, tokenize=False, add_generation_prompt=False)
print("----- RENDERED CHAT -----")
print(text)
print("----- MARKER CHECKS -----")
print("HAS_USER_MARKER:", "<|im_start|>user\n" in text)
print("HAS_ASSISTANT_MARKER:", "<|im_start|>assistant\n" in text)
