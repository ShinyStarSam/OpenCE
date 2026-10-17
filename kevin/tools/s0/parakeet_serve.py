"""S0: keeps Parakeet TDT 0.6B v2 loaded (half precision) and transcribes
over HTTP, the way the sidecar will hear you. Runs inside WSL with
~/kevin/parakeet/.venv.

  python parakeet_serve.py [PORT]     # default 8009

POST /transcribe with a WAV body returns {"text": ..., "ms": ...}.
GET /health returns ok once the model is loaded.
"""

import io
import json
import sys
import time
from http.server import BaseHTTPRequestHandler, HTTPServer

import numpy as np
import soundfile as sf
import torch

from parakeet_bench import MODEL, RATE


def load_model():
	import nemo.collections.asr as nemo_asr
	# halved on the CPU so the full-precision copy never reaches the GPU
	model = nemo_asr.models.ASRModel.restore_from(MODEL, map_location="cpu")
	model.eval()
	model = model.half().cuda()
	# the first pass sets up kernels (over a second); do it before the first request
	with torch.inference_mode():
		model.transcribe([np.zeros(RATE, dtype="float32")], batch_size=1, verbose=False)
	return model


def to_16k(audio, rate):
	if audio.ndim > 1:
		audio = audio.mean(axis=1)
	if rate != RATE:
		positions = np.linspace(0, len(audio) - 1, int(len(audio) * RATE / rate))
		audio = np.interp(positions, np.arange(len(audio)), audio)
	return audio.astype("float32")


def main():
	port = int(sys.argv[1]) if len(sys.argv) > 1 else 8009
	model = load_model()

	class Handler(BaseHTTPRequestHandler):
		def log_message(self, *args):
			pass

		def reply(self, status, body):
			data = json.dumps(body).encode()
			self.send_response(status)
			self.send_header("content-type", "application/json")
			self.send_header("content-length", str(len(data)))
			self.end_headers()
			self.wfile.write(data)

		def do_GET(self):
			self.reply(200, {"ok": True}) if self.path == "/health" else self.reply(404, {"error": "not found"})

		def do_POST(self):
			if self.path != "/transcribe":
				return self.reply(404, {"error": "not found"})
			length = int(self.headers.get("content-length", 0))
			if not 0 < length <= 16 * 2**20:
				return self.reply(413, {"error": "send a WAV of at most 16 MB"})
			audio, rate = sf.read(io.BytesIO(self.rfile.read(length)), dtype="float32")
			start = time.perf_counter()
			with torch.inference_mode():
				out = model.transcribe([to_16k(audio, rate)], batch_size=1, verbose=False)
			torch.cuda.synchronize()
			text = out[0].text if hasattr(out[0], "text") else str(out[0])
			self.reply(200, {"text": text, "ms": round((time.perf_counter() - start) * 1000, 1)})

	# one request at a time, as one player talks at a time
	server = HTTPServer(("127.0.0.1", port), Handler)
	print(f"parakeet listening on 127.0.0.1:{port}", flush=True)
	server.serve_forever()


if __name__ == "__main__":
	main()
