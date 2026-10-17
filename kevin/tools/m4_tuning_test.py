"""M4's check (kevin/BRIDGE_PLAN.md), played: at accuracy 0 Kevin lands no
hits in a long fight, at 100 every shot lands, and in god mode he solos the
a30 opening.

Start the game with kevin.enabled, walk a few steps so Kevin is made, then
lead him into the fight. The phases follow one another: accuracy 0 (the rest
his own) until he has fired --shots rounds, then accuracy 100 until he
has fired as many, then god mode until you press Ctrl-C (stand back and let
him fight). It shows his shots and hits as they come (a hit is damage he
does to any unit, once a tick for each), and his deaths.

    python kevin/tools/m4_tuning_test.py [--port 2324] [--shots 60] [--phases 0,100,god]
"""

import argparse
import os
import queue
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(TOOLS, "..", "sidecar"))
from bridge.client import Bridge  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=2324)
    parser.add_argument("--shots", type=int, default=60)
    parser.add_argument("--phases", default="0,100,god", help="which, in order: 0, 100, god")
    arguments = parser.parse_args()

    deaths = queue.Queue()

    def on_message(message, now):
        if message.get("type") == "events":
            for event in message.get("events", []):
                if event.get("event") == "kevin_died":
                    deaths.put(event)

    bridge = Bridge(arguments.port, on_message=on_message, log=lambda *_: None)
    bridge.start()
    if not bridge.wait_connected(20):
        print("FAIL: no game")
        return 1
    bridge.send({"type": "urgent", "types": ["kevin_died"]})
    print("Waiting for Kevin (walk a few steps)...")
    while not (bridge.state or {}).get("kevin"):
        time.sleep(0.2)

    def counts():
        state = bridge.state or {}
        return state.get("kevin_shots", 0), state.get("kevin_hits", 0)

    def tune(message):
        replies = queue.Queue()
        bridge.send(dict(message, type="tuning"), on_reply=lambda reply, now: replies.put(reply))
        reply = replies.get(timeout=3)
        if not reply.get("ok"):
            raise RuntimeError(reply.get("reason"))

    results = []

    def phase(name, message, shots_wanted=None):
        tune(message)
        time.sleep(0.3)
        start_shots, start_hits = counts()
        died = 0
        while not deaths.empty():
            deaths.get_nowait()
        print(f"\n{name}: lead Kevin into a fight" + (f" until he has fired {shots_wanted} rounds." if shots_wanted
                                                      else "; Ctrl-C when you've seen enough."))
        last = None
        try:
            while True:
                shots, hits = counts()
                shots -= start_shots
                hits -= start_hits
                while not deaths.empty():
                    deaths.get_nowait()
                    died += 1
                line = f"  {shots} shots, {hits} hits, {died} deaths"
                if line != last:
                    print(line, flush=True)
                    last = line
                if shots_wanted and shots >= shots_wanted:
                    break
                time.sleep(0.25)
        except KeyboardInterrupt:
            pass
        return shots, hits, died

    for name in arguments.phases.split(","):
        if name == "0":
            shots, hits, _ = phase("Accuracy 0", {"preset": "kevin", "settings": {"accuracy": 0}}, arguments.shots)
            results.append(("accuracy 0: no hits", hits == 0, f"{hits} hits of {shots} shots"))
        elif name == "100":
            shots, hits, _ = phase("Accuracy 100", {"preset": "kevin", "settings": {"accuracy": 100}}, arguments.shots)
            results.append(("accuracy 100: every shot lands", shots > 0 and hits >= shots,
                            f"{hits} hits of {shots} shots ({100 * hits / max(shots, 1):.0f}%)"))
        elif name == "god":
            shots, hits, died = phase("God mode", {"preset": "god"})
            results.append(("god mode: never dies", died == 0, f"{died} deaths, {hits} hits of {shots} shots"))

    tune({"preset": "kevin"})
    bridge.stop()
    print()
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
