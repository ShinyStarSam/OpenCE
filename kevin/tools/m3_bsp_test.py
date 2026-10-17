"""Checks that Kevin follows you across a structure BSP switch (M3), with
you playing: the game started with debug.kevin_bridge_test
(HALO_KEVIN_BRIDGE_TEST=1) and kevin.enabled.

It reads where the level switches BSPs from debug.txt (logged at the level's
start), drops you on the ground a little before the switch into the next BSP
(`test` you_to), and then you walk: it shows how far the switch is, and how
far Kevin is. Once the BSP has switched it waits for him to be brought to
you, and checks he's near you, alive, and in an encounter of the new BSP
(debug.txt). He's brought to you only when the switch left him far behind,
so whether he was is reported, not checked.

    python kevin/tools/m3_bsp_test.py [--port 2324] [--from 0]
"""

import argparse
import math
import os
import queue
import re
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(TOOLS, "..", "sidecar"))
from bridge.client import Bridge  # noqa: E402

DEBUG = os.path.join(TOOLS, "..", "..", "build", "windows", "debug.txt")
SWITCH = re.compile(r"kevin: bsp switch \d+ \((.*?)\) from (-?\d+) to (-?\d+), centre (\S+) (\S+) (\S+), "
                    r"forward (\S+) (\S+), extents (\S+) (\S+) (\S+)")
BEFORE = 10.0      # how far before the switch you're put


def switches():
    """The level's switches, from the last level start in debug.txt."""
    with open(DEBUG, encoding="utf-8", errors="replace") as file:
        lines = file.read().splitlines()
    start = max(index for index, line in enumerate(lines) if "kevin: the level has" in line)
    found = []
    for line in lines[start:start + 64]:
        match = SWITCH.search(line)
        if match:
            name, source, destination, *numbers = match.groups()
            x, y, z, fi, fj, ei, ej, ek = map(float, numbers)
            found.append({"name": name, "from": int(source), "to": int(destination), "centre": (x, y, z),
                          "forward": (fi, fj), "extents": (ei, ej, ek)})
    return found, lines[start:]


def distance(a, b):
    return math.dist(a[:2], b[:2])


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=2324)
    parser.add_argument("--from", dest="source", type=int, default=0, help="the BSP you cross from")
    arguments = parser.parse_args()

    found, _ = switches()
    going = next((switch for switch in found if switch["from"] == arguments.source), None)
    coming = next((switch for switch in found if switch["to"] == arguments.source), None)
    if not going:
        print(f"FAIL: no switch from BSP {arguments.source} in debug.txt")
        return 1

    events = queue.Queue()

    def on_message(message, now):
        if message.get("type") == "events":
            for event in message.get("events", []):
                events.put(event)

    bridge = Bridge(arguments.port, on_message=on_message, log=lambda *_: None)
    bridge.start()
    if not bridge.wait_connected(20):
        print("FAIL: no game")
        return 1
    bridge.send({"type": "urgent", "types": ["bsp_switch", "kevin_brought", "kevin_made", "kevin_died"]})

    def state():
        return bridge.state or {}

    def position(unit):
        point = (state().get(unit) or {}).get("position")
        return (point["x"], point["y"], point["z"]) if point else None

    deadline = time.monotonic() + 120
    while not position("you") and time.monotonic() < deadline:
        time.sleep(0.2)

    # a little before the switch, on the side away from it: past the switch
    # back, which is nearer the BSP you come from
    centre = going["centre"]
    if coming:
        away = (coming["centre"][0] - centre[0], coming["centre"][1] - centre[1])
    else:
        away = (-going["forward"][0], -going["forward"][1])
    length = math.hypot(*away) or 1.0
    away = (away[0] / length, away[1] / length)
    reach = (distance(coming["centre"], centre) if coming else 0.0) + BEFORE
    for extra in (0.0, 4.0, 8.0, -4.0):
        target = {"x": centre[0] + away[0] * (reach + extra), "y": centre[1] + away[1] * (reach + extra),
                  "z": centre[2]}
        replies = queue.Queue()
        bridge.send({"type": "test", "stimulus": "you_to", "point": target},
                    on_reply=lambda reply, now: replies.put(reply))
        reply = replies.get(timeout=3)
        if reply.get("ok"):
            break
    else:
        print("FAIL: no ground before the switch to put you on")
        return 1
    print(f"You're {reach + extra:.0f} units before the switch \"{going['name']}\" "
          f"(BSP {going['from']} to {going['to']}).")
    print("Walk a few steps so Kevin catches up, then walk into the switch (the distance counts down).")

    switched_at = None
    brought = None
    last_print = 0.0
    while True:
        try:
            event = events.get(timeout=0.25)
        except queue.Empty:
            event = None
        if event and event.get("event") == "bsp_switch" and event.get("to") == going["to"]:
            switched_at = time.monotonic()
            print(f"\nThe BSP switched to {going['to']} (tick {event['tick']}). Keep going; checking Kevin...")
        if event and event.get("event") == "kevin_brought" and switched_at:
            brought = event
        you, kevin = position("you"), position("kevin")
        now = time.monotonic()
        if not switched_at and you and now - last_print > 1.0:
            last_print = now
            kevin_text = f"Kevin {distance(you, kevin):.1f} from you" if kevin else "Kevin not made yet"
            print(f"  switch {distance(you, centre):5.1f} away; {kevin_text}", flush=True)
        if switched_at and now - switched_at > 15.0:
            break
        if not switched_at and now - deadline > 600:
            print("FAIL: no switch within 10 minutes")
            return 1

    kevin = state().get("kevin") or {}
    you = position("you")
    near = kevin and you and position("kevin") and distance(you, position("kevin")) < 8.0
    _, lines = switches()
    joined = [line for line in lines if "of the new structure bsp" in line or "brought to you" in line][-3:]
    results = [
        ("near you 15 s after", bool(near),
         f"{distance(you, position('kevin')):.1f} units" if near or position("kevin") else "no Kevin"),
        ("alive", bool(kevin.get("alive")), "alive" if kevin.get("alive") else "dead or gone"),
    ]
    print(f"     brought to you after the switch: {brought['why'] if brought else 'no (he walked across)'}")
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")
    print("debug.txt:")
    for line in joined:
        print("  " + line)
    bridge.stop()
    return 0 if all(ok for _, ok, _ in results) else 1


if __name__ == "__main__":
    sys.exit(main())
