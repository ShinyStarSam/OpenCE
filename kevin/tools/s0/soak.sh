#!/usr/bin/env bash
# S0 soak test: Halo on a30 at 60 fps (init.txt), then Full Kevin loaded next
# to it (Breeze decode+depth+codec, Kev-0.8B one-player, Gemma 4 12B,
# Parakeet half), then soak.py for SECONDS (default 600). Runs in Git Bash on
# Windows; stops everything it started afterwards.
S0="$(cd "$(dirname "$0")" && pwd)"
SECONDS_TO_RUN=${1:-600}
HALO_DIR='B:\halo\build\windows'
BRAIN=kevin-gemma4-12b
used() { nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits; }
stop_all() {
	MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c "pkill -f '[k]ev\.serve|[k]ev_one_player'; pkill -f '[b]reeze_infer.api'; pkill -f '[p]arakeet_serve'"
	curl -s http://127.0.0.1:11434/api/generate -d "{\"model\":\"$BRAIN\",\"keep_alive\":0}" > /dev/null
	powershell.exe -NoProfile -Command "Get-Process halo -ErrorAction SilentlyContinue | Stop-Process"
}

stop_all
sleep 5
echo "idle: $(used) MiB"
powershell.exe -NoProfile -Command "\$env:HALO_NO_VSYNC='1'; \$env:HALO_MAX_FPS='60'; Start-Process -FilePath '$HALO_DIR\\halo.exe' -WorkingDirectory '$HALO_DIR'" > /dev/null
sleep 40
echo "with Halo: $(used) MiB"
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash /mnt/b/halo/kevin/tools/s0/run_breeze.sh --fast-backbone-decode --fast-depth-decoder --fast-codec &
until curl -sf http://127.0.0.1:7860/health > /dev/null; do sleep 2; done
echo "with Breeze: $(used) MiB"
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- env KEV_RUN=kev-0.8b-local bash /mnt/b/halo/kevin/tools/s0/run_kev.sh one &
until curl -sf http://127.0.0.1:8008/v1/models > /dev/null; do sleep 2; done
echo "with Kev: $(used) MiB"
curl -s http://127.0.0.1:11434/api/chat -d "{\"model\":\"$BRAIN\",\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":false,\"think\":false,\"keep_alive\":\"30m\",\"options\":{\"num_ctx\":4096,\"num_predict\":1}}" > /dev/null
echo "with the brain: $(used) MiB"
MSYS_NO_PATHCONV=1 wsl.exe -d Ubuntu -- bash -c 'cd ~/kevin && exec ~/kevin/parakeet/.venv/bin/python /mnt/b/halo/kevin/tools/s0/parakeet_serve.py > ~/kevin/parakeet_serve.log 2>&1' &
until curl -sf http://127.0.0.1:8009/health > /dev/null; do sleep 2; done
echo "with Parakeet: $(used) MiB"
python "$S0/soak.py" "$SECONDS_TO_RUN"
echo "end: $(used) MiB"
stop_all
wait
