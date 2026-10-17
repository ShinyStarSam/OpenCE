"""M1's check of Kevin's bridge (port/linux/game/kevin_bridge.c).

Connects to the game on 127.0.0.1 (kevin.port, 2324), as the sidecar will,
and checks that:

- the game says hello, and sends `state` about 10 times a second;
- `ping` comes back as `pong`;
- `press jump` is `applied` on the next tick, timed over several tries;
- `hold crouch`, `fire_mode hold` and `aim_at you` are applied, and
  `release` clears them;
- bad messages are refused with a reason, not applied;
- events arrive (run it while Kevin takes damage or dies to see more).

Run it with the game in a campaign level with kevin.enabled:

    python kevin/tools/m1_bridge_test.py [--port 2324] [--seconds 10]

Then close it (or kill it) and debug.txt says Kevin is the game's AI again.
"""

import argparse
import json
import socket
import struct
import sys
import time

KIND_JSON = 1


class Bridge:
    def __init__(self, port):
        self.socket = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self.next_id = 1
        self.messages = []
        # the game's generation, from every message that says it (a revert
        # or Kevin's death changes it, and refuses commands of the last)
        self.generation = None

    def send(self, message):
        payload = json.dumps(message).encode()
        self.socket.sendall(struct.pack("<IB", len(payload) + 1, KIND_JSON) + payload)

    def send_raw(self, kind, payload):
        self.socket.sendall(struct.pack("<IB", len(payload) + 1, kind) + payload)

    def command(self, message):
        message = dict(message)
        message["id"] = self.next_id
        self.next_id += 1
        self.send(message)
        return message["id"]

    def read(self, timeout):
        """Every message that arrives within timeout seconds."""
        end = time.monotonic() + timeout
        received = []
        while True:
            while len(self.buffer) >= 5:
                length, kind = struct.unpack("<IB", self.buffer[:5])
                if len(self.buffer) < 4 + length:
                    break
                payload = self.buffer[5:4 + length]
                self.buffer = self.buffer[4 + length:]
                if kind == KIND_JSON:
                    message = json.loads(payload)
                    message["_received"] = time.monotonic()
                    if "generation" in message and message.get("type") in ("hello", "state", "events"):
                        self.generation = message["generation"]
                    received.append(message)
            left = end - time.monotonic()
            if left <= 0:
                return received
            self.socket.settimeout(left)
            try:
                data = self.socket.recv(65536)
            except socket.timeout:
                return received
            if not data:
                raise ConnectionError("the game closed the connection")
            self.buffer += data

    def wait_for(self, predicate, timeout):
        """The first message the predicate takes, keeping the rest."""
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            for message in self.read(min(0.05, max(0.0, end - time.monotonic()))):
                self.messages.append(message)
                if predicate(message):
                    return message
        return None


def check(name, ok, detail=""):
    print(f"{'PASS' if ok else 'FAIL'}  {name}{('  ' + detail) if detail else ''}")
    return ok


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=2324)
    parser.add_argument("--seconds", type=float, default=10.0, help="how long to watch events at the end")
    parser.add_argument("--tries", type=int, default=20, help="press jump this many times")
    arguments = parser.parse_args()

    bridge = Bridge(arguments.port)
    results = []

    hello = bridge.wait_for(lambda m: m.get("type") == "hello", 2)
    results.append(check("hello", hello is not None and hello.get("protocol") == 1, json.dumps(hello)))
    if not hello:
        return 1
    generation = bridge.generation

    start = time.monotonic()
    states = []
    while time.monotonic() - start < 1.0:
        states += [m for m in bridge.read(0.1) if m.get("type") == "state"]
    rate = len(states) / (time.monotonic() - start)
    results.append(check("state about 10 a second", 7 <= rate <= 13, f"{rate:.1f} a second"))
    kevin = states[-1]["kevin"] if states else None
    results.append(check("state has Kevin", kevin is not None, json.dumps(kevin)[:200]))

    sent = time.monotonic()
    ping_id = bridge.command({"type": "ping", "t": 1.5})
    pong = bridge.wait_for(lambda m: m.get("type") == "pong" and m.get("id") == ping_id, 2)
    results.append(check("ping", pong is not None and pong.get("t") == 1.5,
                         f"{(pong['_received'] - sent) * 1000:.1f} ms" if pong else ""))

    # press jump: applied on the next tick
    latencies = []
    for _ in range(arguments.tries):
        bridge.wait_for(lambda m: m.get("type") == "state", 1)
        sent = time.monotonic()
        jump_id = bridge.command({"type": "press", "flag": "jump", "ticks": 2, "generation": bridge.generation})
        applied = bridge.wait_for(lambda m: m.get("id") == jump_id and m.get("type") in ("applied", "action_result"), 2)
        if not applied or applied.get("type") != "applied":
            results.append(check("press jump applied", False, json.dumps(applied)))
            break
        latencies.append((applied["_received"] - sent) * 1000)
        time.sleep(0.6)
    if latencies:
        latencies.sort()
        p50 = latencies[len(latencies) // 2]
        p95 = latencies[min(len(latencies) - 1, int(len(latencies) * 0.95))]
        # a command is read at the start of the next tick and applied in it,
        # so it waits at most a tick (33 ms) in the game, plus the frame
        results.append(check("press jump applied on the next tick", p95 <= 50,
                             f"{len(latencies)} tries, p50 {p50:.1f} ms, p95 {p95:.1f} ms"))

    for name, message in [
        ("hold crouch", {"type": "hold", "flag": "crouch", "on": True}),
        ("fire_mode hold", {"type": "fire_mode", "mode": "hold"}),
        ("aim_at you", {"type": "aim_at", "target": "you", "ticks": 60}),
    ]:
        command_id = bridge.command(message)
        reply = bridge.wait_for(lambda m, i=command_id: m.get("id") == i and m.get("type") in ("applied", "action_result"), 2)
        results.append(check(name, reply is not None and reply.get("type") == "applied", json.dumps(reply)))
    overlay = bridge.wait_for(lambda m: m.get("type") == "state", 1)
    results.append(check("state shows the overlay", overlay is not None and overlay.get("overlay") is True))
    time.sleep(1.0)
    release_id = bridge.command({"type": "release"})
    reply = bridge.wait_for(lambda m: m.get("id") == release_id, 2)
    results.append(check("release", reply is not None and reply.get("ok") is True, json.dumps(reply)))
    after = bridge.wait_for(lambda m: m.get("type") == "state" and m.get("overlay") is False, 1)
    results.append(check("state shows no overlay", after is not None))

    for name, message, reason in [
        ("unknown flag refused", {"type": "press", "flag": "dance"}, "unknown flag"),
        ("ticks out of range refused", {"type": "press", "flag": "jump", "ticks": 100000}, "ticks out of range"),
        ("stale generation refused", {"type": "press", "flag": "jump", "generation": bridge.generation + 1000}, "stale generation"),
        ("unknown type refused", {"type": "teleport_everyone"}, "unknown type"),
        ("bad target refused", {"type": "aim_at", "target": {"x": "a"}}, "target point out of the world"),
    ]:
        command_id = bridge.command(message)
        reply = bridge.wait_for(lambda m, i=command_id: m.get("id") == i, 2)
        results.append(check(name, reply is not None and reply.get("ok") is False and reply.get("reason") == reason,
                             json.dumps(reply)))
    bridge.send_raw(KIND_JSON, b"{not json")
    reply = bridge.wait_for(lambda m: m.get("type") == "action_result" and m.get("reason") == "not a JSON object", 2)
    results.append(check("bad JSON refused", reply is not None))
    bridge.send_raw(9, b"\x00\x01")
    reply = bridge.wait_for(lambda m: m.get("type") == "action_result" and m.get("reason") == "unknown message kind", 2)
    results.append(check("unknown kind refused", reply is not None))

    urgent_id = bridge.command({"type": "urgent", "types": ["kevin_damaged", "player_shot_kevin", "kevin_died"]})
    reply = bridge.wait_for(lambda m: m.get("id") == urgent_id, 2)
    results.append(check("urgent types", reply is not None and reply.get("ok") is True))

    print(f"watching events for {arguments.seconds:.0f} s (hurt Kevin, or let a checkpoint save)...")
    end = time.monotonic() + arguments.seconds
    while time.monotonic() < end:
        bridge.messages += bridge.read(0.2)
    for message in bridge.messages:
        if message.get("type") == "events":
            for event in message["events"]:
                print("  event", json.dumps(event)[:220])

    passed = sum(1 for ok in results if ok)
    print(f"{passed} of {len(results)} checks passed")
    return 0 if passed == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
