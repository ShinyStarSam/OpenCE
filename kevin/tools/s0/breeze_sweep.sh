#!/usr/bin/env bash
# S0: measures Breeze TTS 2 with different fast-path switches, one at a time,
# alone on the GPU. For each setup: start the server, wait for it, record GPU
# memory, time Kevin's lines with bench.py, stop it. Results are appended to
# out/breeze_sweep.txt. With arguments, runs only the named setups:
#
#   breeze_sweep.sh fast-all depth
S0="$(cd "$(dirname "$0")" && pwd)"
OUT="$S0/out/breeze_sweep.txt"
mkdir -p "$S0/out"
pkill -f '[b]reeze_infer.api'
pkill -f '[k]ev\.serve'
sleep 5

run() {
	local name="$1"
	shift
	echo "=== $name: $*" | tee -a "$OUT"
	bash "$S0/run_breeze.sh" "$@" &
	# --fast-all compiles for 30+ minutes the first time (with 4 CPUs)
	for _ in $(seq 1 1080); do
		curl -sf -o /dev/null http://127.0.0.1:7860/health && break
		grep -q Traceback "$HOME/kevin/breeze_serve.log" && break
		sleep 5
	done
	if ! curl -sf -o /dev/null http://127.0.0.1:7860/health; then
		echo "  failed to start:" | tee -a "$OUT"
		grep -E "Error|error" "$HOME/kevin/breeze_serve.log" | grep -v POSIX | tail -3 | tee -a "$OUT"
	else
		nvidia-smi --query-gpu=memory.used --format=csv,noheader | sed 's/^/  GPU memory used: /' | tee -a "$OUT"
		python3 "$S0/bench.py" breeze 2>&1 | grep -E "first audio|real-time" | tee -a "$OUT"
	fi
	pkill -f '[b]reeze_infer.api'
	sleep 8
}

nvidia-smi --query-gpu=memory.used --format=csv,noheader | sed 's/^/baseline GPU memory used: /' | tee -a "$OUT"
setup() {
	case "$1" in
	eager) run eager ;;
	decode) run decode --fast-backbone-decode ;;
	decode+codec) run decode+codec --fast-backbone-decode --fast-codec ;;
	depth) run depth --fast-backbone-decode --fast-depth-decoder --fast-codec ;;
	quick-all) run quick-all --fast-backbone-decode --fast-codec --fast-text-encoder --fast-backbone-prefill ;;
	fast-all) run fast-all --fast-all ;;
	*) echo "unknown setup $1" ;;
	esac
}

if [ $# -eq 0 ]; then
	set -- eager decode decode+codec depth quick-all fast-all
fi
for name in "$@"; do
	setup "$name"
done
echo "=== done" | tee -a "$OUT"
