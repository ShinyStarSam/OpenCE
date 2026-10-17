#!/usr/bin/env bash
# S0: loads one whole configuration next to the running game and times it
# with bench.py all (each model alone, then while the brain talks nonstop).
# Runs in Git Bash on Windows. Stops before the bench if GPU memory passes
# LIMIT_MIB, since near 24 GB Windows starts paging and the game freezes.
#
#   combined.sh NAME BRAIN KEV_RUN KEV_MODE
#   combined.sh max-brain kevin-gemma4-12b kev-0.8b-local lean
S0="$(cd "$(dirname "$0")" && pwd)"
NAME="$1" BRAIN="$2" KEV_RUN="$3" KEV_MODE="$4"
LIMIT_MIB=${LIMIT_MIB:-22500}
OUT="$S0/out/samples/combined_$NAME.log"
used() { nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits; }
stop_all() {
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "pkill -f '[k]ev\.serve|[k]ev_one_player'; pkill -f '[b]reeze_infer.api'"
	curl -s http://127.0.0.1:11434/api/generate -d "{\"model\":\"$BRAIN\",\"keep_alive\":0}" > /dev/null
}

stop_all
sleep 5
{
	echo "== $NAME: brain $BRAIN, Kev $KEV_RUN $KEV_MODE, Breeze decode+depth+codec, Parakeet half"
	echo "start: $(used) MiB"
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash /mnt/b/halo/kevin/tools/s0/run_breeze.sh --fast-backbone-decode --fast-depth-decoder --fast-codec &
	until curl -sf http://127.0.0.1:7860/health > /dev/null; do sleep 2; done
	echo "with Breeze: $(used) MiB"
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- env KEV_RUN="$KEV_RUN" bash /mnt/b/halo/kevin/tools/s0/run_kev.sh $KEV_MODE &
	until curl -sf http://127.0.0.1:8008/v1/models > /dev/null; do sleep 2; done
	echo "with Kev: $(used) MiB"
	curl -s http://127.0.0.1:11434/api/chat -d "{\"model\":\"$BRAIN\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":false,\"think\":false,\"keep_alive\":\"30m\",\"options\":{\"num_ctx\":4096,\"num_predict\":1}}" > /dev/null
	echo "with the brain: $(used) MiB"
	if [ "$(used)" -gt "$LIMIT_MIB" ]; then
		echo "over $LIMIT_MIB MiB before Parakeet; stopping"
	else
		KEVIN_BRAIN="$BRAIN" KEVIN_NUM_CTX=4096 PARAKEET_HALF=1 python "$S0/bench.py" all
	fi
	echo "end: $(used) MiB"
} > "$OUT" 2>&1
stop_all
wait
grep -E "^==|MiB|p50|kevin:|stopping" "$OUT"
