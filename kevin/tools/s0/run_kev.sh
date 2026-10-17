#!/usr/bin/env bash
# Starts the Kev-4B server in WSL on port 8008, logging to ~/kevin/kev_serve.log.
# Pass "lean" to trade some speed for GPU memory: no CUDA graphs, one cached
# state of at most 8k tokens, and PyTorch's expandable allocator segments.
# "mid" is the same with CUDA graphs kept. KEV_RUN picks the checkpoint folder;
# KEV_PREFIX_CACHE and KEV_PREFIX_MAX_TOKENS override the cache. "one" is mid
# with the CUDA graph buffers sized for one player (kev_one_player.py).
cd "$HOME/kevin/kev" || exit 1
export HF_HUB_OFFLINE=1
SERVE=(-m kev.serve)
[ "${1:-}" = "one" ] && SERVE=(/mnt/b/halo/kevin/tools/s0/kev_one_player.py)
case "${1:-}" in
lean | mid | one)
	export KEV_PREFIX_CACHE=${KEV_PREFIX_CACHE:-1} KEV_PREFIX_MAX_TOKENS=${KEV_PREFIX_MAX_TOKENS:-8192}
	export PYTORCH_CUDA_ALLOC_CONF=expandable_segments:True
	[ "$1" = "lean" ] && export KEV_CUDA_GRAPHS=0
	;;
esac
exec .venv/bin/python "${SERVE[@]}" --run "$HOME/kevin/models/${KEV_RUN:-kev-4b-local}" --port 8008 > "$HOME/kevin/kev_serve.log" 2>&1
