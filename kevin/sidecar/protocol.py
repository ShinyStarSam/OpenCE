"""Kevin's bridge protocol, version 1 (port/linux/game/kevin_bridge.c).

TCP on 127.0.0.1, one client. Every message, both ways: a 4-byte length
(little endian, of what follows), a 1-byte kind, then the payload:

- kind 1, both ways: JSON;
- kind 2, from the game: what the microphone hears while push to talk is
  held: the utterance and the frame's sequence in it (4 bytes each, little
  endian), then 16-bit samples at 16 kHz (20 ms a frame);
- kind 3, to the game: Kevin's voice: his line (4 bytes), then 16-bit
  samples at 48 kHz. A new line cuts off the one playing.
"""

import json
import struct

PROTOCOL = 1
GAME_PORT = 2324
KIND_JSON = 1
KIND_HEARING = 2
KIND_VOICE = 3
HEARING_RATE = 16000
VOICE_RATE = 48000
HEADER = struct.Struct("<IB")
# the longest message the game takes from the sidecar (kevin_bridge.c's
# KEVIN_MAXIMUM_INCOMING)
MAXIMUM_INCOMING = 16 * 1024


def frame(kind, payload):
    return HEADER.pack(len(payload) + 1, kind) + payload


def frame_json(message):
    return frame(KIND_JSON, json.dumps(message, separators=(",", ":")).encode())


def frame_voice(line, samples):
    """Kevin's voice: samples, bytes of 16-bit little-endian at 48 kHz."""
    return frame(KIND_VOICE, struct.pack("<I", line & 0xFFFFFFFF) + samples)


def frame_hearing(utterance, sequence, samples):
    return frame(KIND_HEARING, struct.pack("<II", utterance & 0xFFFFFFFF, sequence & 0xFFFFFFFF) + samples)


def parse_hearing(payload):
    """(utterance, sequence, samples as bytes) of a kind 2 message."""
    utterance, sequence = struct.unpack_from("<II", payload)
    return utterance, sequence, payload[8:]


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
