"""S0: times Parakeet TDT 0.6B v2 on the GPU the way tier 1 will use it.

Runs inside WSL with ~/kevin/parakeet/.venv. Given a WAV of speech, it
transcribes windows of 0.5-4 s (the sliding window re-run while you talk)
and the whole clip, and prints a RESULT line of JSON for bench.py.

  python parakeet_bench.py speech.wav [half]

"half" runs the model in fp16, which roughly halves its GPU memory.
"""

import json
import os
import statistics
import sys
import time

import numpy as np
import soundfile as sf
import torch

MODEL = os.path.expanduser("~/kevin/models/parakeet-tdt-0.6b-v2/parakeet-tdt-0.6b-v2.nemo")
RATE = 16000


def load_audio(path):
	audio, rate = sf.read(path, dtype="float32")
	if audio.ndim > 1:
		audio = audio.mean(axis=1)
	if rate != RATE:
		positions = np.linspace(0, len(audio) - 1, int(len(audio) * RATE / rate))
		audio = np.interp(positions, np.arange(len(audio)), audio).astype("float32")
	return audio


def main():
	import nemo.collections.asr as nemo_asr

	start = time.perf_counter()
	half = "half" in sys.argv[2:]
	cpu = "cpu" in sys.argv[2:]
	# halved on the CPU so the full-precision copy never reaches the GPU
	model = nemo_asr.models.ASRModel.restore_from(MODEL, map_location="cpu" if half or cpu else "cuda")
	model.eval()
	if half and not cpu:
		model = model.half().cuda()
	load_s = time.perf_counter() - start
	sync = (lambda: None) if cpu else torch.cuda.synchronize
	if cpu:
		torch.set_num_threads(os.cpu_count())
	vram_mib = torch.cuda.memory_allocated() / 2**20
	parameter_mib = sum(p.numel() * p.element_size() for p in model.parameters()) / 2**20
	print(f"  parameters {parameter_mib:.0f} MiB")

	audio = load_audio(sys.argv[1])
	clip_s = len(audio) / RATE

	def transcribe(samples):
		with torch.inference_mode():
			out = model.transcribe([samples], batch_size=1, verbose=False)
		first = out[0]
		return first.text if hasattr(first, "text") else str(first)

	for _ in range(3):
		transcribe(audio[: RATE * 2])

	rows = []
	for window in (0.5, 1.0, 2.0, 4.0):
		samples = audio[: int(RATE * min(window, clip_s))]
		times = []
		for _ in range(20):
			sync()
			t = time.perf_counter()
			transcribe(samples)
			sync()
			times.append((time.perf_counter() - t) * 1000)
		times.sort()
		row = {"name": f"{window:.1f} s window", "p50": round(statistics.median(times), 1),
			"p95": round(times[int(0.95 * (len(times) - 1))], 1), "n": len(times), "unit": "ms"}
		rows.append(row)
		print(f"  {row['name']:<34} p50 {row['p50']:>8} ms  p95 {row['p95']:>8}")

	text = transcribe(audio)
	peak_mib = torch.cuda.max_memory_reserved() / 2**20
	print(f"  load {load_s:.1f} s, {vram_mib:.0f} MiB allocated after load, {peak_mib:.0f} MiB peak reserved, clip {clip_s:.1f} s")
	print(f"  full clip: {text!r}")
	print("RESULT " + json.dumps({"load_s": round(load_s, 1), "vram_mib": round(vram_mib), "latency": rows, "transcript": text}))


if __name__ == "__main__":
	main()
