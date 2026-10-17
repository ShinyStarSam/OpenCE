#!/usr/bin/env bash
# S0: for each Kev checkpoint and memory mode, start the server, note how much
# GPU memory it adds, record its answers with samples.py, and stop it. Runs in
# Git Bash on Windows; results go to out/samples/kev_<name>.json and the
# memory lines to out/samples/kev_memory.txt.
#
#   kev_samples.sh kev-0.8b-local:lean kev-4b-local:lean ...
S0="$(cd "$(dirname "$0")" && pwd)"
OUT="$S0/out/samples/kev_memory.txt"

used() { nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits; }

for spec in "$@"; do
	run="${spec%%:*}"
	mode="${spec#*:}"
	[ "$mode" = "full" ] && mode=""
	name="${run%-local}-${mode:-full}${KEV_NAME_SUFFIX:-}"
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "pkill -f '[k]ev\.serve|[k]ev_one_player'"
	sleep 3
	before=$(used)
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- env KEV_RUN="$run" KEV_PREFIX_CACHE="${KEV_PREFIX_CACHE:-1}" KEV_PREFIX_MAX_TOKENS="${KEV_PREFIX_MAX_TOKENS:-8192}" bash /mnt/b/halo/kevin/tools/s0/run_kev.sh $mode &
	until curl -sf http://127.0.0.1:8008/v1/models > /dev/null; do sleep 2; done
	loaded=$(used)
	python "$S0/samples.py" kev "$name" | tail -1
	after=$(used)
	echo "$name: before $before MiB, loaded $loaded MiB (+$((loaded - before))), after answering $after MiB (+$((after - before)))" | tee -a "$OUT"
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "pkill -f '[k]ev\.serve|[k]ev_one_player'"
	wait
done
