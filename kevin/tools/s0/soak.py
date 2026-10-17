"""S0 soak test: Kevin's whole loop, nonstop for a while, next to the game.

Expects every server loaded (soak.sh starts them): Parakeet on 8009, Kev on
8008, Breeze on 7860, the brain in Ollama. Every few seconds it plays the
player: one of your lines (spoken by Breeze in another voice, made once) goes
through Parakeet, Kev decides what to do, and every other turn Gemma answers
and Breeze speaks the answer. Meanwhile it samples GPU memory each second and
checks Halo answers each 15 s. Writes out/samples/soak.json and prints a
summary.

  python soak.py [SECONDS]     # default 600
"""

import http.client
import json
import os
import subprocess
import sys
import threading
import time

import bench
import samples

YOU = ["Kevin, get in the warthog and take the gun", "throw a grenade at those grunts", "follow me",
	"what are we supposed to be doing", "Kevin, crouch", "do you remember the elite with the sword",
	"hold your fire until I say", "nice shot", "go stand by that rock", "shut up Kevin"]
YOUR_VOICE = "A young man in his twenties speaking casually into a gaming headset, slightly distracted."
PERIOD_S = 6


def your_lines():
	"""Your lines as WAVs, in a voice other than Kevin's, made once."""
	paths = []
	for i, text in enumerate(YOU):
		path = os.path.join(samples.SAMPLES, f"soak_you_{i}.wav")
		if not os.path.exists(path):
			samples.tts({"text": text, "instruction": YOUR_VOICE, "cfg_scale": "4", "seed": "11"}, save=path)
		paths.append(path)
	return paths


def transcribe(path):
	with open(path, "rb") as f:
		data = f.read()
	conn = http.client.HTTPConnection("127.0.0.1", 8009, timeout=60)
	start = time.perf_counter()
	conn.request("POST", "/transcribe", data, {"content-type": "audio/wav"})
	result = json.loads(conn.getresponse().read())
	conn.close()
	return result["text"], (time.perf_counter() - start) * 1000


def halo_responding():
	out = subprocess.run(["powershell.exe", "-NoProfile", "-Command",
		"$p = Get-Process halo -ErrorAction SilentlyContinue; if ($p) { $p.Responding } else { 'gone' }"],
		capture_output=True, text=True, timeout=30).stdout.strip()
	return out


def main():
	seconds = int(sys.argv[1]) if len(sys.argv) > 1 else 600
	paths = your_lines()
	memory, halo, turns, errors = [], [], [], []
	stop = threading.Event()
	start = time.time()

	def watch_memory():
		while not stop.is_set():
			memory.append((round(time.time() - start, 1), bench.gpu_memory().get("used_mib", 0)))
			time.sleep(1)

	def watch_halo():
		while not stop.is_set():
			halo.append((round(time.time() - start, 1), halo_responding()))
			stop.wait(15)

	threads = [threading.Thread(target=f, daemon=True) for f in (watch_memory, watch_halo)]
	for t in threads:
		t.start()

	i = 0
	while time.time() - start < seconds:
		began = time.time()
		turn = {"t": round(began - start, 1), "you": YOU[i % len(YOU)]}
		try:
			text, turn["parakeet_ms"] = transcribe(paths[i % len(paths)])
			turn["heard"] = text
			state = bench.STATE.format(utterance=text) + f"\nturn {i}"
			t = time.perf_counter()
			result = bench.post_json(8008, "/v1/systemone", {"state": state, "model": "kev-latest", "questions": bench.QUESTIONS})
			turn["kev_ms"] = (time.perf_counter() - t) * 1000
			turn["action"] = result["answers"]["action"]["choice"]
			if i % 2 == 0:
				messages = [{"role": "system", "content": samples.BRAIN_SYSTEM}, {"role": "user", "content": text}]
				first, total, rate, reply, _ = samples.brain_chat(messages, 4096)
				turn.update({"brain_first_ms": first or total, "brain_total_ms": total, "brain_tok_s": rate, "kevin": reply})
				words = reply.split("]", 1)[-1].strip() or reply
				first_audio, voice_total, audio_s = samples.tts(
					{"text": words, "instruction": bench.KEVIN_VOICE, "cfg_scale": "4", "seed": str(i)})
				turn.update({"breeze_first_ms": first_audio, "breeze_total_ms": voice_total, "audio_s": audio_s,
					"rtf": round(voice_total / 1000 / max(audio_s, 0.01), 2)})
		except Exception as exc:
			errors.append({"t": turn["t"], "error": repr(exc)[:300]})
			turn["error"] = repr(exc)[:300]
		turns.append(turn)
		print(f"[{turn['t']:6.1f}s] {turn.get('heard', '?')!r} -> {turn.get('action', '?')}"
			+ (f", Kevin: {turn.get('kevin', '')[:60]!r} (RTF {turn.get('rtf')})" if "kevin" in turn else ""), flush=True)
		i += 1
		time.sleep(max(0, PERIOD_S - (time.time() - began)))

	stop.set()
	for t in threads:
		t.join(timeout=40)

	def stats(key):
		values = [t[key] for t in turns if key in t]
		return bench.summary(key, values, "x" if key == "rtf" else "ms") if values else {"name": key, "n": 0}

	used = [m for _, m in memory]
	first_minute = [m for s, m in memory if s < 60]
	last_minute = [m for s, m in memory if s > seconds - 60]
	report = {
		"seconds": seconds, "turns": len(turns), "errors": errors,
		"memory": {"start_mib": used[0] if used else None, "peak_mib": max(used) if used else None,
			"first_minute_avg_mib": round(sum(first_minute) / max(len(first_minute), 1)),
			"last_minute_avg_mib": round(sum(last_minute) / max(len(last_minute), 1)), "timeline": memory},
		"halo": {"checks": len(halo), "not_responding": [h for h in halo if h[1] != "True"]},
		"latency": [stats(k) for k in ("parakeet_ms", "kev_ms", "brain_first_ms", "breeze_first_ms", "rtf")],
		"slow_voice_lines": sum(1 for t in turns if t.get("rtf", 0) > 1),
		"log": turns,
	}
	print(f"memory: start {report['memory']['start_mib']} MiB, peak {report['memory']['peak_mib']} MiB, "
		f"first minute {report['memory']['first_minute_avg_mib']} MiB, last minute {report['memory']['last_minute_avg_mib']} MiB")
	print(f"Halo: {len(halo)} checks, {len(report['halo']['not_responding'])} not responding")
	print(f"voice lines slower than real time: {report['slow_voice_lines']}; errors: {len(errors)}")
	with open(os.path.join(samples.SAMPLES, "soak.json"), "w") as f:
		json.dump(report, f, indent=2)


if __name__ == "__main__":
	main()
