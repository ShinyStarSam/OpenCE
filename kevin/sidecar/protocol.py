"""Kevin's bridge protocol, version 1 (port/linux/game/kevin_bridge.c).

TCP on 127.0.0.1, one client. Every message, both ways: a 4-byte length
(little endian, of what follows), a 1-byte kind, then the payload. Kind 1
is JSON, the only kind version 1 uses.
"""

import json
import struct

PROTOCOL = 1
GAME_PORT = 2324
KIND_JSON = 1
HEADER = struct.Struct("<IB")
# the longest message the game takes from the sidecar (kevin_bridge.c's
# KEVIN_MAXIMUM_INCOMING)
MAXIMUM_INCOMING = 16 * 1024


def frame(kind, payload):
    return HEADER.pack(len(payload) + 1, kind) + payload


def frame_json(message):
    return frame(KIND_JSON, json.dumps(message, separators=(",", ":")).encode())


class Reader:
    """Splits a byte stream into (kind, payload) messages."""

    def __init__(self, maximum=None):
        self.buffer = b""
        self.maximum = maximum

    def feed(self, data):
        self.buffer += data

    def messages(self):
        """The whole messages waiting; ValueError on a length out of range."""
        while len(self.buffer) >= HEADER.size:
            length, kind = HEADER.unpack_from(self.buffer)
            if length < 1 or (self.maximum is not None and length > self.maximum + 1):
                raise ValueError(f"a message of a wrong size ({length})")
            if len(self.buffer) < 4 + length:
                return
            payload = self.buffer[HEADER.size:4 + length]
            self.buffer = self.buffer[4 + length:]
            yield kind, payload
