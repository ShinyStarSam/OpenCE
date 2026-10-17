"""The sidecar's end of the bridge (port/linux/game/kevin_bridge.c).

Connects to the game (or the fake game), reads its messages on a thread of
its own and hands them on: JSON to on_message(message, t), what the
microphone hears to on_hearing(utterance, sequence, samples, t), where t is
time.monotonic() as the message came in. Commands go out with an id and the
generation last heard, so the game refuses one meant for a Kevin who is
gone; whoever sent one can wait for its `applied` or `action_result`. A ping
each second keeps the game from handing Kevin back to the AI for silence.
"""

import json
import os
import socket
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import protocol  # noqa: E402

PING_SECONDS = 1.0


class Bridge:
    def __init__(self, port=protocol.GAME_PORT, on_message=None, on_hearing=None, on_connect=None, log=print):
        self.port = port
        self.on_message = on_message
        self.on_hearing = on_hearing
        self.on_connect = on_connect
        self.log = log
        self.socket = None
        self.send_lock = threading.Lock()
        self.next_id = 1
        self.generation = None
        self.hello = None
        self.state = None
        self.connected = threading.Event()
        self.stopping = threading.Event()
        # id -> (callback, sent at)
        self.waiting = {}
        self.waiting_lock = threading.Lock()
        self.threads = []

    # ---------- the connection

    def start(self):
        """Connects (and connects again whenever the game goes) on threads of its own."""
        for target in (self._run, self._ping):
            thread = threading.Thread(target=target, daemon=True)
            thread.start()
            self.threads.append(thread)

    def stop(self):
        self.stopping.set()
        if self.socket:
            try:
                self.socket.close()
            except OSError:
                pass

    def wait_connected(self, timeout=None):
        return self.connected.wait(timeout)

    def _run(self):
        announced = False
        while not self.stopping.is_set():
            try:
                connection = socket.create_connection(("127.0.0.1", self.port), timeout=1.0)
            except OSError:
                if not announced:
                    self.log(f"bridge: waiting for the game on 127.0.0.1:{self.port}")
                    announced = True
                time.sleep(0.5)
                continue
            announced = False
            connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            connection.settimeout(None)
            self.socket = connection
            self._read(connection)
            self.connected.clear()
            self.socket = None
            self.generation = None
            with self.waiting_lock:
                self.waiting.clear()
            if not self.stopping.is_set():
                self.log("bridge: the game is gone")

    def _read(self, connection):
        reader = protocol.Reader()
        while not self.stopping.is_set():
            try:
                data = connection.recv(65536)
            except OSError:
                return
            if not data:
                return
            now = time.monotonic()
            reader.feed(data)
            try:
                for kind, payload in reader.messages():
                    if kind == protocol.KIND_JSON:
                        self._message(json.loads(payload), now)
                    elif kind == protocol.KIND_HEARING and self.on_hearing:
                        utterance, sequence, samples = protocol.parse_hearing(payload)
                        self.on_hearing(utterance, sequence, samples, now)
            except ValueError as error:
                self.log(f"bridge: {error}; closing")
                connection.close()
                return

    def _message(self, message, now):
        kind = message.get("type")
        if "generation" in message:
            self.generation = message["generation"]
        if kind == "hello":
            self.hello = message
            self.connected.set()
            self.log(f"bridge: connected to {message.get('game')} in {message.get('level') or 'no level'}")
            if self.on_connect:
                self.on_connect(message)
        elif kind == "state":
            self.state = message
        elif kind in ("applied", "action_result") and message.get("id") is not None:
            with self.waiting_lock:
                waiter = self.waiting.pop(message["id"], None)
            if waiter:
                waiter[0](message, now)
        if self.on_message:
            self.on_message(message, now)

    def _ping(self):
        while not self.stopping.wait(PING_SECONDS):
            if self.connected.is_set():
                self.send({"type": "ping", "t": round(time.monotonic(), 3)})

    # ---------- sending

    def _send_bytes(self, data):
        connection = self.socket
        if not connection:
            return False
        with self.send_lock:
            try:
                connection.sendall(data)
            except OSError:
                return False
        return True

    def send(self, message, on_reply=None):
        """Sends a JSON command with an id (and the generation, for Kevin's);
        on_reply(message, t) is called with its `applied` or `action_result`.
        Returns (id, the time it was sent), or (None, None) with no game."""
        message = dict(message)
        with self.waiting_lock:
            message_id = self.next_id
            self.next_id += 1
            if on_reply:
                self.waiting[message_id] = (on_reply, time.monotonic())
        message["id"] = message_id
        if message["type"] in ("press", "hold", "fire_mode", "aim_at") and self.generation is not None:
            message.setdefault("generation", self.generation)
        sent = time.monotonic()
        if not self._send_bytes(protocol.frame_json(message)):
            with self.waiting_lock:
                self.waiting.pop(message_id, None)
            return None, None
        return message_id, sent

    def send_voice(self, line, samples):
        """Kevin's voice: 48 kHz 16-bit samples (bytes), in messages the game takes."""
        step = 4800 * 2
        for start in range(0, len(samples), step):
            if not self._send_bytes(protocol.frame_voice(line, samples[start:start + step])):
                return False
        return True
