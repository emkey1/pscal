FROM vllm/vllm-openai:v0.20.2
ENV PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PYTHONUNBUFFERED=1
RUN python3 -m pip install --no-cache-dir -U \
    "transformers>=5.0.0.dev0" \
    datasets accelerate peft trl safetensors sentencepiece pyyaml tensorboard
WORKDIR /workspace
