#!/usr/bin/env bash
# Sets up the Python environments for Kevin's models inside WSL Ubuntu:
# Kev-4B (~/kevin/kev), Breeze TTS 2 (~/kevin/breeze-tts) and Parakeet
# (~/kevin/parakeet). Each has its own environment, since their torch and
# transformers versions conflict. Logs go to ~/kevin/<name>_sync.log.
set -u
export PATH="$HOME/.local/bin:$PATH"
cd "$HOME/kevin"

(cd kev &&
	uv sync --extra serve --python 3.12 &&
	uv pip install "flash-linear-attention==0.5.2") > kev_sync.log 2>&1
echo "kev_exit=$?" >> kev_sync.log

(cd breeze-tts &&
	uv venv -q --allow-existing --python 3.12 .venv &&
	uv pip install -p .venv -r requirements.txt) > breeze_sync.log 2>&1
echo "breeze_exit=$?" >> breeze_sync.log

mkdir -p parakeet
(cd parakeet &&
	uv venv -q --allow-existing --python 3.12 .venv &&
	uv pip install -p .venv "nemo_toolkit[asr]") > parakeet_sync.log 2>&1
echo "parakeet_exit=$?" >> parakeet_sync.log

tail -n 3 kev_sync.log breeze_sync.log parakeet_sync.log
