"""Records a session between Kevin's sidecar and the game (or the fake game).

It sits between the two: listens where the sidecar connects, connects on to
the game, passes every byte through at once, and writes every message both
ways, with the time it passed, to a JSON-lines file:

    {"recording": 1, "started": "...", "game_port": 2324}
    {"t": 0.0123, "from": "game", "kind": 1, "message": {...}}
    {"t": 0.0480, "from": "sidecar", "kind": 1, "message": {...}}

A message that isn't JSON is kept as base64 (`"data"`). The recording ends
when either side closes.

    python kevin/sidecar/replay/recorder.py session.jsonl [--listen 2326] [--game-port 2324]
"""

import argparse
import base64
import datetime
import json
import os
import selectors
import socket
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import protocol  # noqa: E402


def entry(start, side, kind, payload):
    line = {"t": round(time.monotonic() - start, 4), "from": side, "kind": kind}
    if kind == protocol.KIND_JSON:
        try:
            line["message"] = json.loads(payload)
            return line
        except ValueError:
            pass
    line["data"] = base64.b64encode(payload).decode()
    return line


def record(path, listen_port, game_port, stop=None, ready=None, log=print):
    """Records one session into path; ready(port) is called once listening."""
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", listen_port))
    listener.listen(1)
    listener.settimeout(0.2)
    port = listener.getsockname()[1]
    log(f"recorder: waiting for the sidecar on 127.0.0.1:{port}")
    if ready:
        ready(port)
    sidecar = None
    while sidecar is None and not (stop and stop.is_set()):
        try:
            sidecar, _ = listener.accept()
        except socket.timeout:
            continue
    listener.close()
    if sidecar is None:
        return 0
    game = socket.create_connection(("127.0.0.1", game_port))
    for connection in (sidecar, game):
        connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        connection.setblocking(False)

    start = time.monotonic()
    count = 0
    peers = {sidecar: (game, "sidecar", protocol.Reader()), game: (sidecar, "game", protocol.Reader())}
    selector = selectors.DefaultSelector()
    selector.register(sidecar, selectors.EVENT_READ)
    selector.register(game, selectors.EVENT_READ)
    with open(path, "w", encoding="utf-8") as output:
        output.write(json.dumps({"recording": 1, "started": datetime.datetime.now().isoformat(timespec="seconds"),
                                 "game_port": game_port}) + "\n")
        log("recorder: recording")
        open_ = True
        while open_ and not (stop and stop.is_set()):
            for key, _ in selector.select(timeout=0.2):
                source = key.fileobj
                destination, side, reader = peers[source]
                try:
                    data = source.recv(65536)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    data = b""
                if not data:
                    open_ = False
                    break
                destination.setblocking(True)
                destination.sendall(data)
                destination.setblocking(False)
                reader.feed(data)
                for kind, payload in reader.messages():
                    output.write(json.dumps(entry(start, side, kind, payload), separators=(",", ":")) + "\n")
                    count += 1
    selector.close()
    sidecar.close()
    game.close()
    log(f"recorder: {count} messages in {path}")
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("path")
    parser.add_argument("--listen", type=int, default=protocol.GAME_PORT + 2)
    parser.add_argument("--game-port", type=int, default=protocol.GAME_PORT)
    arguments = parser.parse_args()
    try:
        record(arguments.path, arguments.listen, arguments.game_port)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
