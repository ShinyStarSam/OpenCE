#!/usr/bin/env bash
# S0: how Halo's frame rate and resolution affect Kevin's models. Loads
# Breeze (decode+depth+codec), Kev-0.8B (one-player graphs) and Gemma 4 12B
# once, then for each setting restarts Halo on a30 (init.txt) and runs
# bench.py quick. Runs in Git Bash on Windows; a summary line per setting
# goes to out/samples/halo_settings.txt.
#
#   halo_settings.sh 60:native 240:1920x1080 ...
S0="$(cd "$(dirname "$0")" && pwd)"
OUT="$S0/out/samples/halo_settings.txt"
HALO_DIR='B:\halo\build\windows'
BRAIN=kevin-gemma4-12b
used() { nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits; }
busy() {
	local total=0
	for _ in 1 2 3 4 5; do
		total=$((total + $(nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits)))
		sleep 1
	done
	echo $((total / 5))
}
stop_halo() { powershell.exe -NoProfile -Command "Get-Process halo -ErrorAction SilentlyContinue | Stop-Process"; sleep 3; }
stop_models() {
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "pkill -f '[k]ev\.serve|[k]ev_one_player'; pkill -f '[b]reeze_infer.api'"
	curl -s http://127.0.0.1:11434/api/generate -d "{\"model\":\"$BRAIN\",\"keep_alive\":0}" > /dev/null
}

stop_halo
stop_models
sleep 5
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash /mnt/b/halo/kevin/tools/s0/run_breeze.sh --fast-backbone-decode --fast-depth-decoder --fast-codec &
until curl -sf http://127.0.0.1:7860/health > /dev/null; do sleep 2; done
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- env KEV_RUN=kev-0.8b-local bash /mnt/b/halo/kevin/tools/s0/run_kev.sh one &
until curl -sf http://127.0.0.1:8008/v1/models > /dev/null; do sleep 2; done
curl -s http://127.0.0.1:11434/api/chat -d "{\"model\":\"$BRAIN\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":false,\"think\":false,\"keep_alive\":\"30m\",\"options\":{\"num_ctx\":4096,\"num_predict\":1}}" > /dev/null
echo "== $(date '+%Y-%m-%d %H:%M') models loaded: $(used) MiB" | tee -a "$OUT"

for spec in "$@"; do
	fps="${spec%%:*}"
	res="${spec#*:}"
	stop_halo
	powershell.exe -NoProfile -Command "\$env:HALO_NO_VSYNC='1'; \$env:HALO_MAX_FPS='$fps'; \$env:HALO_RESOLUTION='$res'; Start-Process -FilePath '$HALO_DIR\\halo.exe' -WorkingDirectory '$HALO_DIR'" > /dev/null
	sleep 40
	log=$(KEVIN_NUM_CTX=4096 python "$S0/bench.py" quick 2>&1)
	kev=$(echo "$log" | grep -m1 "new state, wall clock" | awk '{print $6}')
	first=$(echo "$log" | grep -m1 "time to first audio" | awk '{print $6}')
	rtf=$(echo "$log" | grep -m1 "real-time factor" | awk '{print $7}')
	tps=$(echo "$log" | grep -m1 "generation speed" | awk '{print $4}')
	echo "Halo ${fps} fps at ${res}: GPU busy $(busy)%, memory $(used) MiB; Kev ${kev} ms, Breeze first sound ${first} ms, ${rtf}x real time; Gemma ${tps} tok/s" | tee -a "$OUT"
done

stop_halo
stop_models
wait
