"""M2's check (kevin/BRIDGE_PLAN.md): "Kevin, jump" acted on within 250 ms
(p95) of the end of speech, and a bark playing within 300 ms, over 50
tries; and tier 1 picks the right action, or none, for a set of commands.

The sidecar (kevin/sidecar/kevin.py) runs here, against the game on --port
(or the fake game, --fake), with Parakeet in WSL. The speech is made once
with Windows' voices (kevin/sidecar/data/speech/) and heard as the game's
microphone would send it: a frame each 20 ms, with push to talk held from
a little before to a little after. With the real game it's given straight
to the sidecar's hearing (the microphone's own delay, about one tick, isn't
in the figures); with --fake it goes through the fake game and the bridge.

    python kevin/tools/m2_reflex_test.py [--port 2324] [--tries 50] [--fake]
"""

import argparse
import os
import queue
import sys
import threading
import time
import wave

TOOLS = os.path.dirname(os.path.abspath(__file__))
SIDECAR = os.path.join(TOOLS, "..", "sidecar")
sys.path.insert(0, TOOLS)
sys.path.insert(0, SIDECAR)
sys.path.insert(0, os.path.join(SIDECAR, "fakegame"))
import speech_lines  # noqa: E402
from kevin import Kevin  # noqa: E402
from latency.log import percentile  # noqa: E402

SPEECH = os.path.join(SIDECAR, "data", "speech")
JUMPS = ["Kevin, jump.", "Kevin, jump!", "Jump, Kevin.", "Kevin jump."]
# text: the action tier 1 should take, None for none (the brain's)
COMMANDS = {
    "Kevin, get down.": "crouch",
    "Kevin, stand up.": "stand",
    "Kevin, hold fire.": "hold_fire",
    "Open fire!": "fire",
    "Kevin, reload.": "reload",
    "Kevin, throw a grenade.": "grenade",
    "Stop.": "stop",
    "Kevin, can you jump?": None,
    "Don't jump.": None,
    "What are we doing here?": None,
}
VARIANTS = [(voice, rate) for voice in speech_lines.SAPI_VOICES for rate in (0, 2, 4)]
LEAD_SECONDS = 0.25      # push to talk down before speaking
HOLD_SECONDS = 0.35      # and up after


def speech_file(text, voice, rate):
    name = "".join(c if c.isalnum() else "_" for c in text.lower()).strip("_")
    path = os.path.join(SPEECH, f"{name}-{voice.split()[1].lower()}-{rate}.wav")
    if not os.path.exists(path):
        os.makedirs(SPEECH, exist_ok=True)
        speech_lines.sapi(text, path, voice, rate)
    with wave.open(path, "rb") as file:
        assert file.getframerate() == 16000 and file.getsampwidth() == 2 and file.getnchannels() == 1
        return file.readframes(file.getnframes())


class Speaker:
    """Talks to the sidecar's hearing as the game's microphone would."""

    def __init__(self, kevin, fake=None):
        self.kevin = kevin
        self.fake = fake
        self.number = 1000000

    def say(self, samples):
        if self.fake:
            self.fake.say(bytes(2 * int(16000 * LEAD_SECONDS)) + samples, HOLD_SECONDS)
            return
        self.number += 1
        number = self.number
        samples = bytes(2 * int(16000 * LEAD_SECONDS)) + samples + bytes(2 * int(16000 * HOLD_SECONDS))
        start = time.monotonic()
        self.kevin._message({"type": "ptt", "down": True, "utterance": number}, start)
        frames = [samples[index:index + 640] for index in range(0, len(samples), 640)]
        for index, frame in enumerate(frames):
            due = start + (index + 1) * 0.02
            wait = due - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            self.kevin._hearing(number, index, frame, time.monotonic())
        self.kevin._message({"type": "ptt", "down": False, "utterance": number}, time.monotonic())


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=2324)
    parser.add_argument("--parakeet-port", type=int, default=8009)
    parser.add_argument("--tries", type=int, default=50)
    parser.add_argument("--fake", action="store_true", help="against the fake game, through its microphone")
    parser.add_argument("--gap", type=float, default=1.2, help="seconds between tries (he lands)")
    arguments = parser.parse_args()

    print("making the test speech (once)...")
    jumps = [speech_file(text, voice, rate) for text in JUMPS for voice, rate in VARIANTS]
    commands = [(text, expected, speech_file(text, voice, rate))
                for text, expected in COMMANDS.items() for voice, rate in VARIANTS[::3]]

    fake = None
    port = arguments.port
    if arguments.fake:
        import fakegame
        ports = queue.Queue()
        stop = threading.Event()
        fake = fakegame.FakeGame(0, seed=1, revert_seconds=0, log=lambda *_: None)
        threading.Thread(target=fake.run, kwargs={"stop": stop, "ready": ports.put}, daemon=True).start()
        port = ports.get(timeout=5)

    records = queue.Queue()
    kevin = Kevin(port, arguments.parakeet_port, log=lambda line: print("  " + line) if "heard" in line else None)
    kevin.on_record = records.put
    if not kevin.parakeet.available():
        print(f"FAIL: no Parakeet on 127.0.0.1:{arguments.parakeet_port}")
        return 1
    kevin.start()
    if not kevin.bridge.wait_connected(20):
        print(f"FAIL: no game on 127.0.0.1:{port}")
        return 1
    # (Kevin is made once the level has started and no cutscene plays)
    deadline = time.monotonic() + 180
    while not (kevin.bridge.state or {}).get("kevin") and time.monotonic() < deadline:
        time.sleep(0.5)
    if not (kevin.bridge.state or {}).get("kevin"):
        print("FAIL: no Kevin in the game")
        return 1
    # (the game turns hearing on as the sidecar asks; with the fake game, it must be on before it talks)
    time.sleep(0.5)
    speaker = Speaker(kevin, fake)
    failures = []

    def run(samples, timeout=4.0):
        """The records of one utterance."""
        while not records.empty():
            records.get_nowait()
        speaker.say(samples)
        got = []
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                got.append(records.get(timeout=0.2))
            except queue.Empty:
                if got:
                    break
        return got

    print(f"\"Kevin, jump\" {arguments.tries} times:")
    actions, barks, decided, last_word = [], [], [], []
    for index in range(arguments.tries):
        text = JUMPS[(index % len(jumps)) // len(VARIANTS)]
        got = [record for record in run(jumps[index % len(jumps)]) if record["action"] == "jump"]
        if not got or got[0]["applied"] is None:
            failures.append(f"try {index + 1}: no jump applied ({got[0]['refused'] if got else 'not heard'})")
            print(f"  try {index + 1}: no jump")
        else:
            figures = got[0]["figures"]
            actions.append(figures["action_ms"])
            # ("Jump, Kevin" is acted on before the sentence ends)
            if text.lower().rstrip(".!").endswith("jump"):
                last_word.append(figures["action_ms"])
            decided.append(figures.get("decided_ms", 0))
            if "bark_ms" in figures:
                barks.append(figures["bark_ms"])
            print(f"  try {index + 1}: jump {figures['action_ms']:.0f} ms after speech, "
                  f"bark {figures.get('bark_ms', float('nan')):.0f} ms (\"{got[0]['text']}\")")
        time.sleep(arguments.gap)

    print("the other commands:")
    right = 0
    for text, expected, samples in commands:
        got = run(samples)
        taken = [record["action"] for record in got]
        ok = taken == ([expected] if expected else [])
        right += ok
        if not ok:
            failures.append(f"\"{text}\": {taken or 'nothing'}, not {expected or 'nothing'}")
        print(f"  {'ok ' if ok else 'BAD'} \"{text}\": {', '.join(taken) or 'nothing'}")
        time.sleep(arguments.gap)
        if expected == "crouch":
            kevin.bridge.send({"type": "hold", "flag": "crouch", "on": False})
        if expected == "hold_fire":
            kevin.bridge.send({"type": "fire_mode", "mode": "free"})
    kevin.bridge.send({"type": "release"})
    time.sleep(0.3)
    kevin.stop()

    print()
    result = 0
    for name, values, target in (("jump after speech", actions, 250.0),
                                 ("jump after speech, \"jump\" said last", last_word, 250.0),
                                 ("bark after speech", barks, 300.0)):
        if not values:
            print(f"FAIL {name}: none")
            result = 1
            continue
        p50, p95 = percentile(values, 0.5), percentile(values, 0.95)
        verdict = "PASS" if p95 <= target else "FAIL"
        result |= verdict == "FAIL"
        print(f"{verdict} {name}: {len(values)} tries, p50 {p50:.0f} ms, p95 {p95:.0f} ms (target {target:.0f})")
    if decided:
        print(f"     (heard to decided: p50 {percentile(decided, 0.5):.0f} ms, p95 {percentile(decided, 0.95):.0f} ms)")
    print(f"{'PASS' if right == len(commands) else 'FAIL'} commands: {right} of {len(commands)} right")
    result |= right != len(commands)
    for failure in failures:
        print("  " + failure)
    return result


if __name__ == "__main__":
    sys.exit(main())
