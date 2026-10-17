"""Kevin's sidecar: so far, the reflex slice (kevin/BRIDGE_PLAN.md, M2).

Hold the push to talk key (V) in the game and talk: the game sends what its
microphone hears, Parakeet transcribes it on a sliding window while you
talk, tier 1 fires a keyword's action the moment the word appears ("Kevin,
jump"), and Kevin acknowledges it with a bark from the cache. Talking over
him cuts him off. Every reflex's latency is logged (logs/latency-*.jsonl)
and summed up on exit.

    python kevin/sidecar/kevin.py [--port 2324] [--parakeet-port 8009]

Needs the game with kevin.enabled, and Parakeet running in WSL
(kevin/tools/s0/parakeet_serve.py).
"""

import argparse
import os
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bridge.client import Bridge  # noqa: E402
from latency.log import LatencyLog  # noqa: E402
from reflex import keywords  # noqa: E402
from speech.barks import Barks  # noqa: E402
from speech.hearing import Hearing  # noqa: E402
from speech.parakeet import Parakeet  # noqa: E402
import protocol  # noqa: E402

DATA = os.path.join(HERE, "data")
# tier 0 (kevin_bridge.c), as Kevin starts: he fires back after two hits by
# you within 3 seconds, for 3 seconds; he runs from grenades within 3 units
RULES = {"type": "rules", "hostile": {"on": True, "hits": 2, "seconds": 3, "fire_seconds": 3},
         "grenade_dive": {"on": True, "distance": 3}, "stealth": False, "hold_fire_until_engaged": False}
URGENT = ["reflex_fired", "player_shot_kevin", "kevin_died", "player_died"]
# a reflex's record is written this long after its utterance ends, complete or not
RECORD_WAIT_SECONDS = 2.0


class Kevin:
    def __init__(self, game_port=protocol.GAME_PORT, parakeet_port=8009, barks=os.path.join(DATA, "barks"),
                 logs=os.path.join(HERE, "logs"), log=print):
        self.log = log
        self.lock = threading.RLock()
        self.bridge = Bridge(game_port, on_message=self._message, on_hearing=self._hearing,
                             on_connect=self._connect, log=log)
        self.parakeet = Parakeet(parakeet_port)
        self.hearing = Hearing(self.parakeet, on_partial=self._partial, on_end=self._utterance_end, log=log)
        self.reflex = keywords.Reflex()
        self.barks = Barks(barks, log=log)
        self.latency = LatencyLog(logs)
        self.records = []           # reflexes not yet written
        self.line = 0               # Kevin's last line
        self.speaking = False
        self.barked = set()         # utterances acknowledged
        self.ptt = {}               # utterance -> {"down": t, "up": t}
        self.on_record = None       # (tests) each record as it's written
        self.stopping = threading.Event()
        self.finisher = threading.Thread(target=self._finish_records, daemon=True)

    def start(self):
        if not self.parakeet.available():
            self.log(f"kevin: no Parakeet on 127.0.0.1:{self.parakeet.port} yet (kevin/tools/s0/parakeet_serve.py)")
        self.bridge.start()
        self.finisher.start()

    def stop(self):
        self.stopping.set()
        self.hearing.stop()
        self.bridge.stop()

    # ---------- the game

    def _connect(self, hello):
        self.bridge.send({"type": "urgent", "types": URGENT})
        self.bridge.send(RULES)
        self.bridge.send({"type": "hearing", "on": True})

    def _message(self, message, now):
        kind = message.get("type")
        if kind == "ptt":
            utterance = message.get("utterance")
            if message.get("down"):
                self.ptt[utterance] = {"down": now}
                self.hearing.begin(utterance, now)
                # (barge-in: talking over Kevin stops him)
                if self.speaking:
                    self.bridge.send({"type": "voice_stop"})
            else:
                self.ptt.setdefault(utterance, {})["up"] = now
                self.hearing.end(utterance, now)
        elif kind == "events":
            for event in message.get("events", []):
                if event.get("event") == "reflex_fired":
                    self.log(f"kevin: tier 0 {event.get('rule')}: {event.get('summary')}")
        elif kind == "voice":
            with self.lock:
                self.speaking = bool(message.get("playing"))
                if self.speaking:
                    for record in self.records:
                        if record.get("bark_line") == message.get("line") and record.get("bark_playing") is None:
                            record["bark_playing"] = now

    def _hearing(self, utterance, sequence, samples, now):
        self.hearing.hear(utterance, samples, now)

    # ---------- tier 1

    def _partial(self, utterance, text, info):
        actions = self.reflex.hear(utterance.number, text, info["final"])
        decided = time.monotonic()
        if info["final"] and text:
            self.log(f"kevin: heard \"{text}\"")
        for action in actions:
            self.act(action, utterance, text, info, decided)

    def act(self, action, utterance, text, info, decided):
        _, commands, bark = keywords.ACTIONS[action]
        record = {"utterance": utterance.number, "action": action, "text": text, "final": info["final"],
                  "audio_end": info["audio_end"], "pass_started": info["pass_started"],
                  "pass_finished": info["pass_finished"], "decided": decided, "sent": None, "applied": None,
                  "refused": None, "bark": None, "bark_line": None, "bark_sent": None, "bark_playing": None,
                  "waiting": 0}
        if commands == "release":
            commands = [{"type": "release"}]
        with self.lock:
            self.records.append(record)
        for command in commands:
            def reply(message, now, record=record):
                with self.lock:
                    record["waiting"] = record.get("waiting", 0) - 1
                    if message.get("type") == "applied" or message.get("ok"):
                        record["applied"] = record["applied"] or now
                    else:
                        record["refused"] = message.get("reason")
            with self.lock:
                record["waiting"] += 1
            command_id, sent = self.bridge.send(command, on_reply=reply)
            with self.lock:
                if command_id is None:
                    record["waiting"] -= 1
                    record["refused"] = "no game"
                record["sent"] = record["sent"] or sent
        self.log(f"kevin: {action} (\"{text}\")")
        if bark and utterance.number not in self.barked:
            self.barked.add(utterance.number)
            self.say_bark(bark, record)

    def say_bark(self, category, record=None):
        picked = self.barks.pick(category)
        if not picked:
            return None
        name, samples = picked
        with self.lock:
            self.line += 1
            line = self.line
            if record is not None:
                record["bark"] = f"{category}/{name}"
                record["bark_line"] = line
                record["bark_sent"] = time.monotonic()
        self.bridge.send_voice(line, samples)
        return line

    # ---------- the latency log

    def _utterance_end(self, utterance):
        with self.lock:
            for record in self.records:
                if record["utterance"] == utterance.number:
                    record["speech_start"] = utterance.speech_start
                    record["speech_end"] = utterance.speech_end
                    record["ptt_down"] = self.ptt.get(utterance.number, {}).get("down", utterance.started)
                    record["ptt_up"] = self.ptt.get(utterance.number, {}).get("up", utterance.ended)
                    record["heard"] = utterance.text
                    record["ended"] = time.monotonic()

    def _finish_records(self):
        while not self.stopping.wait(0.05):
            now = time.monotonic()
            done = []
            with self.lock:
                for record in self.records:
                    if "ended" not in record:
                        continue
                    complete = record["waiting"] <= 0 and (record["bark_line"] is None or record["bark_playing"])
                    if complete or now - record["ended"] > RECORD_WAIT_SECONDS:
                        done.append(record)
                for record in done:
                    self.records.remove(record)
            for record in done:
                written = self.latency.write({key: value for key, value in record.items() if key != "waiting"})
                figures = written["figures"]
                self.log("kevin: " + record["action"] + " " +
                         ", ".join(f"{name} {value:.0f}" for name, value in figures.items()))
                if self.on_record:
                    self.on_record(written)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=int(os.environ.get("HALO_KEVIN_PORT", protocol.GAME_PORT)))
    parser.add_argument("--parakeet-port", type=int, default=8009)
    arguments = parser.parse_args()
    kevin = Kevin(arguments.port, arguments.parakeet_port)
    kevin.start()
    try:
        while True:
            time.sleep(1.0)
    except KeyboardInterrupt:
        pass
    kevin.stop()
    for line in kevin.latency.summary_lines():
        print(line)


if __name__ == "__main__":
    main()
