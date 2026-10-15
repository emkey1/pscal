#!/usr/bin/env python3
"""Render a model's chat template on a sample conversation and print it with
whitespace made visible, so response-masking markers can be read exactly."""
import sys
from transformers import AutoTokenizer

M = sys.argv[1]
tok = AutoTokenizer.from_pretrained(M, trust_remote_code=True)
msgs = [
    {"role": "system", "content": "SYS"},
    {"role": "user", "content": "USERMSG"},
    {"role": "assistant", "content": "ASSISTANTMSG"},
]
try:
    s = tok.apply_chat_template(msgs, tokenize=False)
except Exception:
    msgs = msgs[1:]  # some templates reject a system turn
    s = tok.apply_chat_template(msgs, tokenize=False)

print("---- rendered (repr, whitespace visible) ----")
print(repr(s))
print("---- around USERMSG ----")
i = s.find("USERMSG")
print(repr(s[max(0, i - 60):i]))
print("---- around ASSISTANTMSG ----")
j = s.find("ASSISTANTMSG")
print(repr(s[max(0, j - 60):j]))
