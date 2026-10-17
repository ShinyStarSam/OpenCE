"""S0: records samples to compare configurations by ear and by eye.

Uses the same servers as bench.py (and its helpers). Each mode writes
out/samples/<mode>_<name>.json, and Breeze modes write a WAV per line.

  python samples.py breeze NAME VARIANT   # VARIANT: full | cfg2 | cfg1 | clone
  python samples.py brain MODEL [NUM_CTX] # an Ollama model, e.g. kevin-gemma4-12b
  python samples.py kev NAME              # whatever Kev checkpoint is serving on 8008
"""

import http.client
import json
import os
import sys
import threading
import time
import uuid
import wave

import bench

SAMPLES = os.path.join(bench.OUT, "samples")

# Kevin's voice, then how each line should sound. The brain would pick the
# delivery along with the words.
LINES = [
	("Moving.", "Flat and bored."),
	("Contact, two o'clock!", "Shouting a warning over gunfire."),
	("Shields are down, cover me!", "Panicked, out of breath."),
	("You're asking if I can jump? I'm a Spartan, you damn clown.", "Incredulous and mocking."),
	("Ha! Did you see that grunt fly? That was beautiful.", "Laughing, delighted."),
	("Hey... we made it. Thanks for having my back out there.", "Quiet and sincere, almost embarrassed."),
	("Fine. I'll shut up. But if a grunt sneaks up on us, I'm blaming you.", "Sulking, muttering."),
	("(sighs) You drove the warthog into a ravine. Again.", "Weary and deadpan."),
]
REFERENCE_TEXT = "Alright, listen up. We move fast, we stay low, and nobody does anything stupid. Got it?"

# The player's lines for the brain, with what is going on at the time
BRAIN_TURNS = [
	("Kevin, can you jump?", "Calm moment, standing near the warthog."),
	("What are we supposed to be doing?", "Just landed; objective: evade the Covenant and find other survivors."),
	("Nice shot!", "Kevin just killed an elite with a headshot."),
	("Sorry about the warthog thing.", "Walking between fights."),
	("Shut up Kevin.", "Kevin was complaining about the heat."),
	("Do you remember the elite with the sword?", "Calm; that elite killed the player twice on the last level."),
	("I'm out of ammo.", "In a firefight with three grunts and a jackal."),
	("What do you think of the marines?", "Two marines are following them."),
]

BRAIN_SYSTEM = """You are Kevin, a Spartan-II supersoldier fighting beside the player, Chasm, on the ring world Halo.
You are foul-mouthed, sarcastic and impatient, but loyal underneath. You speak in one or two short sentences.
Start each reply with how to say it in square brackets, such as [annoyed] or [laughing], then the words.

What you remember:
- On the Pillar of Autumn, Chasm froze for a second at the first elite and you have never let it go.
- Chasm drove the warthog into a ravine on the last level. You were in the passenger seat.
- An elite with an energy sword killed Chasm twice near the lifeboat; Chasm finally stuck it with a plasma grenade.
- Chasm likes the pistol and you think that is showing off.
- You promised to teach Chasm to throw grenades properly.

Right now: level a30, the first landing on Halo. Kevin's health 1.0, shield 1.0, assault rifle 32/240.
Chasm's health 0.8, shield 0.4. Mood: annoyed. Policy: grumpy."""

# The action each utterance should get. Some have more than one fair answer.
KEV_EXPECTED = {
	"Kevin, jump": {"jump"}, "Kevin go stand by that rock": {"move_to"}, "get down": {"crouch"},
	"hold your fire until I say": {"hold_fire"}, "okay now light them up": {"free_fire", "attack"},
	"Kevin, get in the warthog, take the gun": {"enter_vehicle"}, "what are we supposed to be doing": {"none"},
	"follow me": {"follow"}, "stay here": {"wait_here"}, "throw a grenade at those grunts": {"grenade"},
	"Kevin can you jump?": {"none", "jump"}, "stop, not that rock, the other one": {"move_to", "cancel"},
	"reload": {"reload"}, "go near those bodies over there": {"move_to"}, "what do you think of grunts": {"none"},
	"cover me": {"free_fire", "attack", "follow"}, "get out of the car": {"exit_vehicle"}, "shut up Kevin": {"none"},
	"Kevin, crouch": {"crouch"}, "stand up": {"stand"},
}


class PeakMemory:
	"""Samples nvidia-smi while running, to catch what a model takes while it works."""

	def __init__(self):
		self.peak = 0
		self.stop = threading.Event()
		self.thread = threading.Thread(target=self.run, daemon=True)

	def run(self):
		while not self.stop.is_set():
			self.peak = max(self.peak, bench.gpu_memory().get("used_mib", 0))
			time.sleep(0.25)

	def __enter__(self):
		self.thread.start()
		return self

	def __exit__(self, *exc):
		self.stop.set()
		self.thread.join()


def tts(fields, files=None, save=None):
	boundary = uuid.uuid4().hex
	parts = [f"--{boundary}\r\nContent-Disposition: form-data; name=\"{k}\"\r\n\r\n{v}\r\n".encode() for k, v in fields.items()]
	for name, path in (files or {}).items():
		with open(path, "rb") as f:
			data = f.read()
		parts.append(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{name}\"; filename=\"{os.path.basename(path)}\"\r\n"
			f"Content-Type: audio/wav\r\n\r\n".encode() + data + b"\r\n")
	body = b"".join(parts) + f"--{boundary}--\r\n".encode()
	conn = http.client.HTTPConnection("127.0.0.1", 7860, timeout=300)
	start = time.perf_counter()
	conn.request("POST", "/v1/audio/speech", body, {"content-type": f"multipart/form-data; boundary={boundary}"})
	resp = conn.getresponse()
	if resp.status != 200:
		raise RuntimeError(f"tts {resp.status}: {resp.read()[:300]!r}")
	rate = int(resp.getheader("X-Sample-Rate", "24000"))
	first, pcm = None, bytearray()
	while True:
		chunk = resp.read1(65536)
		if not chunk:
			break
		if first is None:
			first = (time.perf_counter() - start) * 1000
		pcm.extend(chunk)
	total = (time.perf_counter() - start) * 1000
	conn.close()
	if save:
		with wave.open(save, "wb") as w:
			w.setnchannels(1)
			w.setsampwidth(2)
			w.setframerate(rate)
			w.writeframes(bytes(pcm))
	return round(first or total), round(total), round(len(pcm) / 2 / rate, 2)


def reference():
	"""One line in Kevin's designed voice, used as the voice to clone."""
	path = os.path.join(SAMPLES, "kevin_reference.wav")
	if not os.path.exists(path):
		tts({"text": REFERENCE_TEXT, "instruction": bench.KEVIN_VOICE + " Firm and in charge.", "cfg_scale": "4", "seed": "7"}, save=path)
	return path


def sample_breeze(name, variant):
	ref = reference() if variant == "clone" else None
	tts({"text": "Warming up.", "instruction": bench.KEVIN_VOICE, "cfg_scale": "4", "seed": "7"})
	lines = []
	with PeakMemory() as memory:
		for i, (text, delivery) in enumerate(LINES):
			fields = {"text": text, "seed": "7"}
			files = None
			if variant == "clone":
				fields.update({"cfg_scale": "1", "ref_text": REFERENCE_TEXT})
				files = {"ref_audio": ref}
			else:
				fields.update({"instruction": f"{bench.KEVIN_VOICE} {delivery}",
					"cfg_scale": {"full": "4", "cfg2": "2", "cfg1": "1"}[variant]})
			wav = f"breeze_{name}_{i}.wav"
			first, total, seconds = tts(fields, files, save=os.path.join(SAMPLES, wav))
			lines.append({"text": text, "delivery": delivery, "wav": wav, "first_audio_ms": first, "total_ms": total,
				"audio_s": seconds, "rtf": round(total / 1000 / max(seconds, 0.01), 2)})
			print(f"  {wav}: first audio {first} ms, {total} ms for {seconds} s  {text!r}")
	return {"variant": variant, "lines": lines, "gpu_peak_mib": memory.peak}


def sample_brain(model, num_ctx):
	bench.GEMMA = model
	turns = []
	for user, situation in BRAIN_TURNS:
		messages = [{"role": "system", "content": BRAIN_SYSTEM + "\n\nSituation: " + situation}, {"role": "user", "content": user}]
		first, total, rate, text, final = brain_chat(messages, num_ctx)
		turns.append({"user": user, "situation": situation, "kevin": text, "first_token_ms": round(first or total),
			"total_ms": round(total), "tok_s": round(rate, 1)})
		print(f"  you: {user!r}\n  kevin ({round(first or total)} ms): {text!r}")
	ps = json.loads(http_get(11434, "/api/ps"))
	vram = [m.get("size_vram", 0) for m in ps.get("models", []) if m.get("name", "").startswith(model)]
	return {"model": model, "num_ctx": num_ctx, "turns": turns, "vram_mib": round(vram[0] / 2**20) if vram else None}


def brain_chat(messages, num_ctx):
	body = {"model": bench.GEMMA, "messages": messages, "stream": True, "keep_alive": "10m", "think": False,
		"options": {"num_ctx": num_ctx, "num_predict": 70, "temperature": 0.8}}
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
	rate = final.get("eval_count", 0) / max(final.get("eval_duration", 1) / 1e9, 1e-9)
	return first, (time.perf_counter() - start) * 1000, rate, "".join(text).strip(), final


def http_get(port, path):
	conn = http.client.HTTPConnection("127.0.0.1", port, timeout=30)
	conn.request("GET", path)
	data = conn.getresponse().read()
	conn.close()
	return data


def sample_kev(name):
	answers, times = [], []
	for utterance, expected in KEV_EXPECTED.items():
		state = bench.STATE.format(utterance=utterance) + f"\nrequest {uuid.uuid4().hex[:8]}"
		start = time.perf_counter()
		result = bench.post_json(8008, "/v1/systemone", {"state": state, "model": "kev-latest", "questions": bench.QUESTIONS})
		times.append((time.perf_counter() - start) * 1000)
		a = result["answers"]
		choice = a["action"]["choice"]
		answers.append({"utterance": utterance, "action": choice, "right": choice in expected,
			"expected": sorted(expected), "confidence": round(a["action"].get("confidence", 0), 2),
			"addressed": round(a["addressed"]["noul"], 2), "needs_brain": round(a["needs_brain"]["noul"], 2)})
		print(f"  {utterance!r:<46} -> {choice:<14} {'ok' if choice in expected else 'WRONG (' + '/'.join(sorted(expected)) + ')'}")
	right = sum(a["right"] for a in answers)
	print(f"  {right}/{len(answers)} right")
	return {"name": name, "right": right, "of": len(answers), "latency": bench.summary("new state, wall clock", times[1:]),
		"answers": answers}


def main():
	if len(sys.argv) < 3:
		sys.exit(__doc__)
	mode, name = sys.argv[1], sys.argv[2]
	os.makedirs(SAMPLES, exist_ok=True)
	before = bench.gpu_memory()
	if mode == "breeze":
		result = sample_breeze(name, sys.argv[3] if len(sys.argv) > 3 else "full")
	elif mode == "brain":
		result = sample_brain(name, int(sys.argv[3]) if len(sys.argv) > 3 else 8192)
	elif mode == "kev":
		result = sample_kev(name)
	else:
		sys.exit(__doc__)
	result["gpu_before"] = before
	result["gpu_after"] = bench.gpu_memory()
	path = os.path.join(SAMPLES, f"{mode}_{name.replace(':', '_')}.json")
	with open(path, "w") as f:
		json.dump(result, f, indent=2)
	print(f"saved {path}")


if __name__ == "__main__":
	main()
