"""M3's check (kevin/BRIDGE_PLAN.md): each of tier 0's rules fires on the
tick of its cause.

Against the game started with debug.kevin_bridge_test (HALO_KEVIN_BRIDGE_TEST=1),
or the fake game (--fake). Each rule is set, its cause made to happen with
`test` (a hit on Kevin by you, a grenade beside him, your crouch, your
shot), and its `reflex_fired` event has to carry the tick the `test` was
taken on, and what he does is checked in `state`: he fires back, runs from
the grenade (in the game; the fake game doesn't move him), crouches. With
the rules off, the same causes fire nothing.

When he fires back at you he does so for 0.3 seconds, so you take a few
rounds.

    python kevin/tools/m3_rules_test.py [--port 2324] [--fake]
"""

import argparse
import os
import queue
import sys
import threading
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
SIDECAR = os.path.join(TOOLS, "..", "sidecar")
sys.path.insert(0, SIDECAR)
sys.path.insert(0, os.path.join(SIDECAR, "fakegame"))
from bridge.client import Bridge  # noqa: E402

OFF = {"type": "rules", "hostile": {"on": False}, "grenade_dive": {"on": False}, "stealth": False,
       "hold_fire_until_engaged": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=2324)
    parser.add_argument("--fake", action="store_true")
    arguments = parser.parse_args()

    port = arguments.port
    if arguments.fake:
        import fakegame
        ports = queue.Queue()
        stop = threading.Event()
        game = fakegame.FakeGame(0, seed=1, revert_seconds=0, log=lambda *_: None, test=True)
        threading.Thread(target=game.run, kwargs={"stop": stop, "ready": ports.put}, daemon=True).start()
        port = ports.get(timeout=5)

    fired = queue.Queue()

    def on_message(message, now):
        if message.get("type") == "events":
            for event in message.get("events", []):
                if event.get("event") == "reflex_fired":
                    fired.put(event)

    bridge = Bridge(port, on_message=on_message, log=lambda *_: None)
    bridge.start()
    if not bridge.wait_connected(20):
        print(f"FAIL: no game on 127.0.0.1:{port}")
        return 1
    deadline = time.monotonic() + 180
    while not (bridge.state or {}).get("kevin") and time.monotonic() < deadline:
        time.sleep(0.2)
    if not (bridge.state or {}).get("kevin"):
        print("FAIL: no Kevin in the game")
        return 1

    def command(message):
        replies = queue.Queue()
        bridge.send(message, on_reply=lambda reply, now: replies.put(reply))
        return replies.get(timeout=3)

    def stimulus(name):
        reply = command({"type": "test", "stimulus": name})
        if not reply.get("ok"):
            raise RuntimeError(f"test {name} refused: {reply.get('reason')} (is debug.kevin_bridge_test on?)")
        return reply["tick"]

    def drain():
        events = []
        while True:
            try:
                events.append(fired.get_nowait())
            except queue.Empty:
                return events

    def expect(rule, tick, timeout=2.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                event = fired.get(timeout=0.1)
            except queue.Empty:
                continue
            if event.get("rule") == rule:
                return event
        return None

    results = []

    def kevin_state():
        return (bridge.state or {}).get("kevin") or {}

    def watch(test, seconds):
        """Whether test(Kevin's state) held at some state within seconds."""
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            if test(kevin_state()):
                return True
            time.sleep(0.02)
        return False

    def check(name, ok, detail):
        results.append(ok)
        print(f"{'PASS' if ok else 'FAIL'} {name}: {detail}")

    for reply in (command({"type": "urgent", "types": ["reflex_fired"]}), command(OFF)):
        assert reply.get("ok"), reply
    time.sleep(0.2)
    drain()

    # with the rules off, nothing
    stimulus("you_hit_kevin")
    time.sleep(0.1)
    stimulus("you_hit_kevin")
    stimulus("grenade")
    stimulus("you_crouch")
    stimulus("you_stand")
    stimulus("you_fire")
    time.sleep(0.5)
    stray = drain()
    check("rules off", not stray, f"{len(stray)} fired" + (f" ({[e['rule'] for e in stray]})" if stray else ""))

    # hostile: the second hit within 3 s
    command({"type": "rules", "hostile": {"on": True, "hits": 2, "seconds": 3, "fire_seconds": 0.3}})
    time.sleep(3.2)
    drain()
    first = stimulus("you_hit_kevin")
    early = expect("hostile", first, timeout=0.4)
    second = stimulus("you_hit_kevin")
    event = expect("hostile", second)
    check("hostile", early is None and event is not None and event["tick"] == second,
          "not after one hit; " + (f"fired on tick {event['tick']}, the second hit's {second}" if event else "never fired"))
    shot = watch(lambda kevin: kevin.get("firing"), 0.5)
    check("hostile: he fires", shot, "his trigger held" if shot else "never fired")
    command({"type": "set_hostile", "on": False})

    # grenade dive
    command({"type": "rules", "hostile": {"on": False}, "grenade_dive": {"on": True, "distance": 3}})
    time.sleep(0.8)
    drain()
    start = dict(kevin_state().get("position", {}))
    tick = stimulus("grenade")
    event = expect("grenade_dive", tick)
    check("grenade_dive", event is not None and event["tick"] == tick,
          f"fired on tick {event['tick']}, the grenade's {tick}" if event else "never fired")
    if not arguments.fake:
        time.sleep(0.7)
        # (the test's grenade is 1.5 units along x from him)
        moved = start.get("x", 0.0) - kevin_state().get("position", {}).get("x", 0.0)
        check("grenade_dive: he runs", moved > 0.5, f"{moved:.2f} units away from it")

    # stealth: he crouches as you do
    command({"type": "rules", "grenade_dive": {"on": False}, "stealth": True})
    time.sleep(0.8)
    drain()
    tick = stimulus("you_crouch")
    event = expect("stealth_crouch", tick)
    check("stealth_crouch", event is not None and event["tick"] == tick,
          f"fired on tick {event['tick']}, your crouch's {tick}" if event else "never fired")
    crouched = watch(lambda kevin: kevin.get("crouching"), 0.5)
    check("stealth_crouch: he crouches", crouched, "crouching" if crouched else "never crouched")
    stimulus("you_stand")

    # hold fire until engaged
    command({"type": "rules", "stealth": False, "hold_fire_until_engaged": True})
    time.sleep(0.5)
    drain()
    tick = stimulus("you_fire")
    event = expect("engaged", tick)
    check("engaged", event is not None and event["tick"] == tick,
          f"fired on tick {event['tick']}, your shot's {tick}" if event else "never fired")

    command(OFF)
    command({"type": "release"})
    bridge.stop()
    print(f"{sum(results)} of {len(results)} passed")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
