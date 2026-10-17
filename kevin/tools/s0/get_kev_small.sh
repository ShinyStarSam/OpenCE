#!/usr/bin/env bash
# Downloads Kev-0.8B and its Qwen3.5-0.8B base into ~/kevin/models and makes
# a local run (kev-0.8b-local) whose head.pt points at the local base.
set -e
cd "$HOME/kevin/kev"
export HF_HUB_DISABLE_TELEMETRY=1
M="$HOME/kevin/models"
REV=$(.venv/bin/python -c "import torch,sys; from huggingface_hub import hf_hub_download as h; print(torch.load(h('jaredpalmer/kev-0.8b','head.pt'),map_location='cpu').get('base_revision') or '')")
.venv/bin/hf download jaredpalmer/kev-0.8b --local-dir "$M/kev-0.8b"
.venv/bin/hf download Qwen/Qwen3.5-0.8B-Base ${REV:+--revision "$REV"} --local-dir "$M/qwen3.5-0.8b-base"
rm -rf "$M/kev-0.8b-local" && cp -r "$M/kev-0.8b" "$M/kev-0.8b-local"
.venv/bin/python - "$M" <<'PY'
import json, sys, torch
m = sys.argv[1]
d, base = f"{m}/kev-0.8b-local", f"{m}/qwen3.5-0.8b-base"
meta = torch.load(f"{d}/head.pt", map_location="cpu")
meta["base"], meta["base_revision"] = base, None
torch.save(meta, f"{d}/head.pt")
c = json.load(open(f"{d}/adapter_config.json"))
c["base_model_name_or_path"] = base
json.dump(c, open(f"{d}/adapter_config.json", "w"), indent=2)
print("kev-0.8b-local ready")
PY
