"""S0 model check: times Kevin's models through the servers the sidecar will use.

Runs on Windows with the standard library only. Expects:
  Kev-4B     http://127.0.0.1:8008  (kev.serve, in WSL)
  Breeze     http://127.0.0.1:7860  (breeze_infer.api, in WSL)
  Gemma 4    http://127.0.0.1:11434 (Ollama, model kevin-gemma4-12b)
Parakeet is timed by parakeet_bench.py inside WSL, started from here.

  python bench.py kev | breeze | gemma | parakeet | vision IMAGE | quick | all
"all" runs Gemma generation nonstop in the background while timing the
others, to measure how much the brain slows the reflex path.
"""

import base64
import http.client
import json
import os
import statistics
import subprocess
import sys
import threading
import time
import uuid
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
GEMMA = os.environ.get("KEVIN_BRAIN", "kevin-gemma4-12b")
NUM_CTX = int(os.environ.get("KEVIN_NUM_CTX", "8192"))
WSL_PARAKEET = [
	"wsl.exe", "-d", "Ubuntu", "--",
	"/home/clara/kevin/parakeet/.venv/bin/python",
	"/mnt/b/halo/kevin/tools/s0/parakeet_bench.py",
]

STATE = """level: a30 (Halo), objective: evade the Covenant and find other survivors
player Chasm: health 0.8, shield 0.4, stance standing, facing north-east, weapon assault rifle
crosshair: rock (scenery\\rocks\\boulder_large) at 14.2 units
Kevin: health 1.0, shield 1.0, stance standing, 6.1 units behind the player, weapon assault rifle, ammo 32/240
policy: grumpy; mood: annoyed
nearby: grunt (enemy) at 2 o'clock 22 units; grunt (enemy) at 2 o'clock 24 units; elite (enemy) at 12 o'clock 31 units, not visible;
warthog (vehicle, empty) at 9 o'clock 12 units; marine (ally) at 6 o'clock 4 units; pile of bodies at 10 o'clock 9 units
last events: player_damaged by grunt 3 s ago; Kevin killed jackal 11 s ago
player said: "{utterance}"
"""

UTTERANCES = [
	"Kevin, jump", "Kevin go stand by that rock", "get down", "hold your fire until I say",
	"okay now light them up", "Kevin, get in the warthog, take the gun", "what are we supposed to be doing",
	"follow me", "stay here", "throw a grenade at those grunts", "Kevin can you jump?",
	"stop, not that rock, the other one", "reload", "go near those bodies over there",
	"what do you think of grunts", "cover me", "get out of the car", "shut up Kevin", "Kevin, crouch", "stand up",
]

ACTIONS = {
	"none": "Nothing to do; just talk or ignore",
	"jump": "Jump", "crouch": "Crouch or get down", "stand": "Stand up",
	"hold_fire": "Stop shooting until told", "free_fire": "Start shooting",
	"follow": "Follow the player", "wait_here": "Stay in place",
	"move_to": "Go to a place or object", "attack": "Attack a specific enemy",
	"grenade": "Throw a grenade", "enter_vehicle": "Get into a vehicle",
	"exit_vehicle": "Get out of a vehicle", "reload": "Reload",
	"cancel": "Stop or cancel the current task", "look_at": "Look at something",
}

QUESTIONS = {
	"action": {"type": "choice", "instructions": "Which action does the player want Kevin to take now?", "criteria": ACTIONS},
	"addressed": {"type": "noul", "instructions": "Is the player talking to Kevin?"},
	"needs_brain": {"type": "noul", "instructions": "Does this need a spoken reply or a judgement call, beyond doing an action?"},
}

KEVIN_VOICE = ("A gruff, gravelly male soldier in his thirties, sarcastic and impatient, "
	"speaking fast and clipped like he's in the middle of a firefight.")
BARKS = ["Moving.", "Contact, two o'clock!", "Shields are down, cover me!",
	"You're asking if I can jump? I'm a Spartan, you damn clown.",
	"Grunt down. Next.", "Fine. I'll shut up. But if a grunt sneaks up on us, I'm blaming you."]

SYSTEM = ("You are Kevin, a Spartan-II supersoldier fighting beside the player, Chasm, on the ring world Halo. "
	"You are foul-mouthed, sarcastic and impatient, but loyal underneath. Keep every reply to one or two short "
	"sentences: you're in a firefight. ") + " ".join(
	f"Memory {i}: on an earlier level Chasm and Kevin fought through a Covenant ambush near a crashed lifeboat; "
	f"Chasm drove the warthog into a ravine and Kevin has not let it go." for i in range(40))


def pct(values, p):
	values = sorted(values)
	return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def summary(name, values, unit="ms"):
	if not values:
		return {"name": name, "n": 0}
	row = {"name": name, "n": len(values), "p50": round(statistics.median(values), 1),
		"p95": round(pct(values, 95), 1), "min": round(min(values), 1), "max": round(max(values), 1), "unit": unit}
	print(f"  {name:<34} p50 {row['p50']:>8} {unit}  p95 {row['p95']:>8}  min {row['min']:>8}  max {row['max']:>8}  (n={row['n']})")
	return row


def post_json(port, path, body, timeout=120):
	conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
	conn.request("POST", path, json.dumps(body), {"content-type": "application/json"})
	resp = conn.getresponse()
	data = resp.read()
	conn.close()
	if resp.status != 200:
		raise RuntimeError(f"{path} {resp.status}: {data[:300]!r}")
	return json.loads(data)


def gpu_memory():
	try:
		out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used,memory.total", "--format=csv,noheader,nounits"],
			capture_output=True, text=True, timeout=10).stdout.strip()
		used, total = (int(x) for x in out.split(","))
		return {"used_mib": used, "total_mib": total}
	except Exception as exc:
		return {"error": str(exc)}


def bench_kev(rounds=20):
	print("Kev (tier 2): 3 questions over a game state")
	wall_new, server_new, wall_same, server_same, answers = [], [], [], [], []
	for i in range(rounds):
		utterance = UTTERANCES[i % len(UTTERANCES)]
		state = STATE.format(utterance=utterance) + f"\nrequest {uuid.uuid4().hex[:8]}"
		for same in (False, True):
			start = time.perf_counter()
			result = post_json(8008, "/v1/systemone", {"state": state, "model": "kev-latest", "questions": QUESTIONS})
			elapsed = (time.perf_counter() - start) * 1000
			(wall_same if same else wall_new).append(elapsed)
			(server_same if same else server_new).append(result.get("latency_ms", elapsed))
			if not same:
				a = result["answers"]
				answers.append({"utterance": utterance, "action": a["action"]["choice"],
					"confidence": round(a["action"].get("confidence", 0), 2),
					"addressed": round(a["addressed"]["noul"], 2), "needs_brain": round(a["needs_brain"]["noul"], 2)})
	rows = [summary("new state, wall clock", wall_new), summary("new state, server time", server_new),
		summary("same state again, wall clock", wall_same), summary("same state again, server time", server_same)]
	for a in answers:
		print(f"    {a['utterance']!r:<48} -> {a['action']:<14} conf {a['confidence']:<5} addressed {a['addressed']:<5} brain {a['needs_brain']}")
	return {"latency": rows, "answers": answers}


def tts_stream(text, instruction=KEVIN_VOICE, save=None):
	boundary = uuid.uuid4().hex
	fields = {"text": text, "instruction": instruction, "cfg_scale": "4", "seed": "7"}
	body = b"".join(
		f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode() for k, v in fields.items()
	) + f"--{boundary}--\r\n".encode()
	conn = http.client.HTTPConnection("127.0.0.1", 7860, timeout=120)
	start = time.perf_counter()
	conn.request("POST", "/v1/audio/speech", body, {"content-type": f"multipart/form-data; boundary={boundary}"})
	resp = conn.getresponse()
	if resp.status != 200:
		raise RuntimeError(f"tts {resp.status}: {resp.read()[:300]!r}")
	rate = int(resp.getheader("X-Sample-Rate", "24000"))
	first, pcm = None, bytearray()
	while True:
		chunk = resp.read1(65536) if hasattr(resp, "read1") else resp.read(4096)
		if not chunk:
			break
		if first is None:
			first = (time.perf_counter() - start) * 1000
		pcm.extend(chunk)
	total = (time.perf_counter() - start) * 1000
	conn.close()
	seconds = len(pcm) / 2 / rate
	if save:
		with wave.open(save, "wb") as w:
			w.setnchannels(1)
			w.setsampwidth(2)
			w.setframerate(rate)
			w.writeframes(bytes(pcm))
	return first or total, total, seconds


def bench_breeze(rounds=2):
	print("Breeze TTS 2: streaming lines in Kevin's designed voice")
	os.makedirs(OUT, exist_ok=True)
	ttfa, rtf, per_line = [], [], []
	tts_stream("Warming up.")
	for r in range(rounds):
		for i, text in enumerate(BARKS):
			save = os.path.join(OUT, f"kevin_line_{i}.wav") if r == 0 else None
			first, total, seconds = tts_stream(text, save=save)
			ttfa.append(first)
			rtf.append(total / 1000 / max(seconds, 0.01))
			per_line.append({"text": text, "ttfa_ms": round(first), "total_ms": round(total), "audio_s": round(seconds, 2)})
	rows = [summary("time to first audio", ttfa), summary("real-time factor (lower is faster)", rtf, "x")]
	print(f"    saved sample lines to {OUT}")
	return {"latency": rows, "lines": per_line[:len(BARKS)]}


def ollama_chat(messages, num_predict=60, stream=True):
	body = {"model": GEMMA, "messages": messages, "stream": stream, "keep_alive": "30m", "think": False,
		"options": {"num_ctx": NUM_CTX, "num_predict": num_predict, "temperature": 0.8}}
	conn = http.client.HTTPConnection("127.0.0.1", 11434, timeout=300)
	start = time.perf_counter()
	conn.request("POST", "/api/chat", json.dumps(body), {"content-type": "application/json"})
	resp = conn.getresponse()
	first, text, final = None, [], {}
	for line in resp:
		if not line.strip():
			continue
		msg = json.loads(line)
		piece = msg.get("message", {}).get("content", "")
		if piece and first is None:
			first = (time.perf_counter() - start) * 1000
		text.append(piece)
		if msg.get("done"):
			final = msg
	conn.close()
	tps = final.get("eval_count", 0) / max(final.get("eval_duration", 1) / 1e9, 1e-9)
	return first, (time.perf_counter() - start) * 1000, tps, "".join(text).strip(), final


def bench_gemma(rounds=8):
	print("Gemma 4 12B (brain): long system prompt, short reply, streamed")
	ollama_chat([{"role": "system", "content": SYSTEM}, {"role": "user", "content": "Ready?"}], num_predict=5)
	ttft_cached, ttft_cold, tps, replies = [], [], [], []
	for i in range(rounds):
		question = UTTERANCES[(i * 3) % len(UTTERANCES)]
		first, total, rate, text, _ = ollama_chat([{"role": "system", "content": SYSTEM}, {"role": "user", "content": question}])
		ttft_cached.append(first)
		tps.append(rate)
		replies.append({"user": question, "kevin": text})
	for i in range(3):
		cold_system = f"[session {uuid.uuid4().hex}] " + SYSTEM
		first, *_ = ollama_chat([{"role": "system", "content": cold_system}, {"role": "user", "content": "Kevin, status?"}])
		ttft_cold.append(first)
	rows = [summary("first token, prompt prefix cached", ttft_cached), summary("first token, new prompt", ttft_cold),
		summary("generation speed", tps, "tok/s")]
	for r in replies:
		print(f"    you: {r['user']!r}\n    kevin: {r['kevin']!r}")
	return {"latency": rows, "replies": replies}


def bench_vision(path):
	print(f"Gemma 4 12B vision: describe {os.path.basename(path)}")
	with open(path, "rb") as f:
		image = base64.b64encode(f.read()).decode()
	prompt = ("You are Kevin's eyes. In one or two sentences, say what is in view that matters in a fight: "
		"enemies, allies, vehicles, cover, and where they are.")
	times, texts = [], []
	for _ in range(3):
		first, total, _, text, _ = ollama_chat([{"role": "user", "content": prompt, "images": [image]}], num_predict=80)
		times.append(total)
		texts.append(text)
	rows = [summary("image to description, total", times)]
	print(f"    {texts[-1]!r}")
	return {"latency": rows, "description": texts[-1]}


def bench_parakeet():
	print("Parakeet TDT 0.6B v2 (in WSL): sliding-window transcription")
	wav = os.path.join(OUT, "kevin_line_3.wav")
	wsl_wav = "/mnt/" + wav[0].lower() + wav[2:].replace("\\", "/")
	extra = os.environ.get("PARAKEET_ARGS", "half" if os.environ.get("PARAKEET_HALF") else "").split()
	proc = subprocess.run(WSL_PARAKEET + [wsl_wav] + extra, capture_output=True, text=True,
		env={**os.environ, "MSYS_NO_PATHCONV": "1"})
	sys.stdout.write(proc.stdout)
	if proc.returncode:
		sys.stdout.write(proc.stderr[-3000:])
		return {"error": proc.stderr[-1000:]}
	line = [l for l in proc.stdout.splitlines() if l.startswith("RESULT ")]
	return json.loads(line[-1][7:]) if line else {"error": "no result"}


def brain_load(stop):
	while not stop.is_set():
		try:
			ollama_chat([{"role": "system", "content": SYSTEM},
				{"role": "user", "content": "Tell me a long war story about the warthog in the ravine."}], num_predict=200)
		except Exception:
			time.sleep(0.5)


def main():
	mode = sys.argv[1] if len(sys.argv) > 1 else "all"
	report = {"time": time.strftime("%Y-%m-%d %H:%M:%S"), "mode": mode, "gpu_before": gpu_memory()}
	if mode == "kev":
		report["kev"] = bench_kev()
	elif mode == "breeze":
		report["breeze"] = bench_breeze()
	elif mode == "gemma":
		report["gemma"] = bench_gemma()
	elif mode == "parakeet":
		report["parakeet"] = bench_parakeet()
	elif mode == "vision":
		report["vision"] = bench_vision(sys.argv[2])
	elif mode == "quick":
		# a short check of the reflex path, the voice and the brain, all already loaded
		report["quick"] = {"kev": bench_kev(5), "breeze": bench_breeze(1), "gemma": bench_gemma(2)}
	elif mode == "all":
		print("== Everything loaded; each model alone first")
		report["alone"] = {"kev": bench_kev(10), "breeze": bench_breeze(1), "gemma": bench_gemma(4), "parakeet": bench_parakeet()}
		report["gpu_all_loaded"] = gpu_memory()
		print(f"GPU memory with everything loaded: {report['gpu_all_loaded']}")
		print("== Same again while Gemma generates nonstop (the brain talking during a fight)")
		stop = threading.Thread, threading.Event()
		event = stop[1]
		loader = threading.Thread(target=brain_load, args=(event,), daemon=True)
		loader.start()
		time.sleep(3)
		report["under_brain_load"] = {"kev": bench_kev(10), "breeze": bench_breeze(1), "parakeet": bench_parakeet()}
		event.set()
		report["gpu_peak_after"] = gpu_memory()
	else:
		sys.exit(__doc__)
	os.makedirs(OUT, exist_ok=True)
	path = os.path.join(OUT, f"s0_{mode}_{time.strftime('%Y%m%d_%H%M%S')}.json")
	with open(path, "w") as f:
		json.dump(report, f, indent=2)
	print(f"report: {path}")


if __name__ == "__main__":
	main()
