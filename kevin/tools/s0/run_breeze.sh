#!/usr/bin/env bash
# Starts the Breeze TTS 2 streaming server in WSL on port 7860, logging to
# ~/kevin/breeze_serve.log. Eager mode (~7.7 GiB) by default; any extra
# arguments (e.g. --fast-all, ~14.4 GiB) are passed to the server.
cd "$HOME/kevin/breeze-tts" || exit 1
export HF_HUB_OFFLINE=1 PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True
# /tmp is in RAM in this distro; keep torch.compile's cache (30+ minutes to
# build the first time) on disk so it survives WSL restarts
export TORCHINDUCTOR_CACHE_DIR="$HOME/kevin/.cache/torchinductor" TRITON_CACHE_DIR="$HOME/kevin/.cache/triton"
exec .venv/bin/python -m breeze_infer.api "$HOME/kevin/models/breeze-tts-2" --port 7860 "$@" > "$HOME/kevin/breeze_serve.log" 2>&1
