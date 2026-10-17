"""Plays the game's side of a recording (recorder.py) back into a sidecar,
with the same timing, so reflexes, prompts and latency can be tuned without
playing.

It listens where the sidecar connects, waits for one, then sends each of
the game's messages at the time it came in the recording (measured from
the first). It doesn't answer the sidecar: what the sidecar sends is kept
and counted by type against what the recorded sidecar sent, so a replay
shows where its behaviour changed.

    python kevin/sidecar/replay/replayer.py session.jsonl [--port 2325] [--speed 1]
"""

import argparse
import base64
import collections
import json
import os
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import protocol  # noqa: E402


def load(path):
    """A recording's lines, header first."""
    with open(path, encoding="utf-8") as recording:
        lines = [json.loads(line) for line in recording if line.strip()]
    if not lines or lines[0].get("recording") != 1:
        raise ValueError(f"{path} is not a recording")
    return lines[0], lines[1:]


def payload_of(line):
    if "message" in line:
        return json.dumps(line["message"], separators=(",", ":")).encode()
    return base64.b64decode(line["data"])


def replay(path, port, speed=1.0, stop=None, ready=None, log=print):
    """Replays into the first sidecar that connects. Returns what happened:
    the game messages sent, when (seconds from the first, as sent and as
    recorded), and the sidecar's messages by type, then and now."""
    _, lines = load(path)
    game_lines = [line for line in lines if line["from"] == "game"]
    recorded_sidecar = collections.Counter(
        line.get("message", {}).get("type", "?") for line in lines if line["from"] == "sidecar")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", port))
    listener.listen(1)
    listener.settimeout(0.2)
    log(f"replayer: {len(game_lines)} game messages; waiting for the sidecar on 127.0.0.1:{listener.getsockname()[1]}")
    if ready:
        ready(listener.getsockname()[1])
    sidecar = None
    while sidecar is None and not (stop and stop.is_set()):
        try:
            sidecar, _ = listener.accept()
        except socket.timeout:
            continue
    listener.close()
    if sidecar is None:
        return None
    sidecar.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    sidecar.settimeout(0.0)

    reader = protocol.Reader()
    replayed_sidecar = collections.Counter()
    timing = []
    first = game_lines[0]["t"] if game_lines else 0.0
    start = time.monotonic()
    closed = False

    def drain():
        nonlocal closed
        while not closed:
            try:
                data = sidecar.recv(65536)
            except (BlockingIOError, InterruptedError, socket.timeout):
                return
            except OSError:
                data = b""
            if not data:
                closed = True
                return
            reader.feed(data)
            for kind, payload in reader.messages():
                try:
                    replayed_sidecar[json.loads(payload).get("type", "?")] += 1
                except ValueError:
                    replayed_sidecar["?"] += 1

    for line in game_lines:
        due = (line["t"] - first) / speed
        while not closed and time.monotonic() - start < due:
            drain()
            time.sleep(min(0.002, max(0.0, due - (time.monotonic() - start))))
        if closed or (stop and stop.is_set()):
            break
        sidecar.settimeout(None)
        sidecar.sendall(protocol.frame(line["kind"], payload_of(line)))
        sidecar.settimeout(0.0)
        timing.append((time.monotonic() - start, due))
    linger = time.monotonic() + 0.5
    while not closed and time.monotonic() < linger:
        drain()
        time.sleep(0.01)
    sidecar.close()

    worst = max((abs(sent - due) for sent, due in timing), default=0.0)
    log(f"replayer: sent {len(timing)} of {len(game_lines)} game messages, at most {worst * 1000:.1f} ms off their time")
    for kind in sorted(set(recorded_sidecar) | set(replayed_sidecar)):
        log(f"replayer:   sidecar {kind}: recorded {recorded_sidecar[kind]}, now {replayed_sidecar[kind]}")
    return {"sent": len(timing), "game_messages": len(game_lines), "worst_offset": worst,
            "recorded_sidecar": recorded_sidecar, "replayed_sidecar": replayed_sidecar}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("path")
    parser.add_argument("--port", type=int, default=protocol.GAME_PORT + 1)
    parser.add_argument("--speed", type=float, default=1.0)
    arguments = parser.parse_args()
    try:
        replay(arguments.path, arguments.port, arguments.speed)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
