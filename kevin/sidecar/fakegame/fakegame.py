"""A fake game that speaks Kevin's bridge protocol (kevin_bridge.c), so the
sidecar and its web UI can be built and tested with the game closed.

It listens on 127.0.0.1 like the game, takes one sidecar at a time, and
runs a small world at 30 ticks a second: you walk a loop, Kevin follows
three units behind, a few enemies stand along the way. Now and then Kevin
is hit (by an enemy, or by you), dies and is made again behind you, and the
game saves a checkpoint or reverts to one. It sends what the game sends
(`hello`, `state` every third tick, `events` in batches with urgent types at
once, `applied`, `action_result`, `pong`) and checks what it's sent the
same way, refusing with the same reasons.

Its microphone is what a test gives `say()`: while the sidecar asks to hear
(`hearing`), it's sent as the game sends it (`ptt` down, kind 2 frames, `ptt`
up). Kevin's voice (kind 3) is "played" in real time, with `voice` as it
starts and stops.

    python kevin/sidecar/fakegame/fakegame.py [--port 2325] [--seed 1]
"""

import argparse
import json
import math
import os
import random
import select
import socket
import struct
import sys
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import protocol  # noqa: E402

TICKS_PER_SECOND = 30
TICK = 1.0 / TICKS_PER_SECOND
STATE_TICKS = 3
EVENT_TICKS = 75
EVENT_BATCH = 16
SILENT_SECONDS = 5.0
SEND_LIMIT = 512 * 1024
MAXIMUM_PRESS_TICKS = 300
MAXIMUM_AIM_TICKS = 900
DEFAULT_PRESS_TICKS = 3
DEFAULT_AIM_TICKS = 60
MAXIMUM_URGENT_TYPES = 32
MAXIMUM_TYPE_LENGTH = 31
NEARBY_DISTANCE = 30.0
WITNESS_DISTANCE = 40.0
WITNESS_COSINE = 0.5
RESPAWN_TICKS = 10 * TICKS_PER_SECOND
HEARING_FRAME_BYTES = 2 * 320          # 20 ms at 16 kHz
VOICE_QUEUE_SAMPLES = 10 * 48000

FLAGS = ("crouch", "jump", "flashlight", "action", "reload", "fire", "fire_secondary", "grenade", "swap_weapons")

# object ids look like the game's datum indices: a salt above, an index below
KEVIN_INDEX = 1
YOUR_ID = 0xE4A50000
ENEMY_IDS = (0xE4C00010, 0xE4C10011, 0xE4C20012)


def number(value):
    """JSON numbers only (true and false are not numbers to the game)."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)


class Refused(Exception):
    pass


class Unit:
    def __init__(self, unit_id, tag, team, position, weapon="assault rifle", rounds=60):
        self.id = unit_id
        self.tag = tag
        self.team = team
        self.position = list(position)
        self.facing = 0.0
        self.aim = [1.0, 0.0, 0.0]
        self.health = 1.0
        self.shield = 1.0
        self.alive = True
        self.weapon = weapon
        self.rounds = rounds

    def state(self):
        return {
            "unit": self.id,
            "position": {"x": round(self.position[0], 2), "y": round(self.position[1], 2), "z": round(self.position[2], 2)},
            "facing": round(self.facing, 3),
            "aim": {"i": round(self.aim[0], 3), "j": round(self.aim[1], 3), "k": round(self.aim[2], 3)},
            "health": round(self.health, 3),
            "shield": round(self.shield, 3),
            "alive": self.alive,
            "vehicle": None,
            "weapon": self.weapon,
            "rounds": self.rounds,
        }


class FakeGame:
    def __init__(self, port, seed=1, revert_seconds=120.0, log=print):
        self.port = port
        self.random = random.Random(seed)
        self.revert_ticks = int(revert_seconds * TICKS_PER_SECOND) if revert_seconds > 0 else 0
        self.log = log
        self.listener = None
        self.client = None
        self.reader = None
        self.sending = b""
        self.last_message = 0.0
        self.silent = False

        self.tick = 0
        self.generation = 1
        self.event_serial = 0
        self.events = []
        self.event_ticks = 0
        self.state_ticks = 0
        self.urgent = []
        self.saved = None
        # ticks since the last revert (the game time itself goes back in one)
        self.revert_counter = 0

        # hearing: what say() was given, sent a frame each 20 ms
        self.hearing = False
        self.speech_lock = threading.Lock()
        self.speeches = []
        self.speech = None             # [frames, next frame, started]
        self.utterance = 0
        self.hearing_sequence = 0
        # Kevin's voice: samples waiting, and his line
        self.voice_samples = 0
        self.voice_line = 0
        self.voice_playing = False

        self.you = Unit(YOUR_ID, "cyborg", 1, (20.0, 0.0, 0.0), "pistol", 12)
        self.kevin = None
        self.kevin_salt = 0xE4B6
        self.dead_ticks = RESPAWN_TICKS
        self.enemies = [Unit(enemy_id, tag, 3, position, "plasma pistol", -1)
                        for enemy_id, tag, position in zip(
                            ENEMY_IDS,
                            ("grunt", "elite", "jackal"),
                            ((0.0, 22.0, 0.0), (-22.0, 0.0, 0.0), (0.0, -24.0, 0.0)))]
        self.release_overlay()

    # ---- the overlay

    def release_overlay(self, why=None):
        busy = getattr(self, "hold_flags", None) or getattr(self, "hold_fire", False) or getattr(self, "aim_target", None)
        busy = busy or any(getattr(self, "press_ticks", {}).values())
        self.press_ticks = {flag: 0 for flag in FLAGS}
        self.hold_flags = set()
        self.hold_fire = False
        self.aim_target = None
        self.aim_ticks = 0
        self.pending = []
        if why and busy:
            self.log(f"fake game: Kevin is the game's AI again ({why})")

    def overlay_active(self):
        return bool(self.hold_flags or self.hold_fire or self.aim_target)

    # ---- sending

    def send(self, message):
        if not self.client:
            return
        data = protocol.frame_json(message)
        if len(self.sending) + len(data) > SEND_LIMIT:
            self.disconnect("not reading")
            return
        self.sending += data

    def flush(self):
        while self.client and self.sending:
            try:
                sent = self.client.send(self.sending)
            except BlockingIOError:
                return
            except OSError as error:
                self.disconnect(str(error))
                return
            self.sending = self.sending[sent:]

    def result(self, command_id, ok, reason):
        self.send({"type": "action_result", "id": command_id, "ok": ok, "tick": self.tick, "reason": reason})

    # ---- events

    def witness(self, position):
        """Whether Kevin was in range of a point, and saw it (in front of him)."""
        if not self.kevin or not self.kevin.alive:
            return False, False
        offset = [position[i] - self.kevin.position[i] for i in range(3)]
        distance = math.sqrt(sum(c * c for c in offset))
        if distance > WITNESS_DISTANCE:
            return False, False
        if distance < 0.5:
            return True, True
        cosine = sum(offset[i] * self.kevin.aim[i] for i in range(3)) / distance
        return True, cosine >= WITNESS_COSINE

    def event(self, kind, summary, unit=None, kevin_did=False, **fields):
        self.event_serial += 1
        event = {"id": self.event_serial, "event": kind, "tick": self.tick, "summary": summary,
                 "object": unit.id if unit else None}
        in_range = saw = False
        if unit:
            event["position"] = unit.state()["position"]
            if unit is self.kevin:
                in_range = saw = True
            else:
                in_range, saw = self.witness(unit.position)
        event.update({"kevin_in_range": in_range or kevin_did, "kevin_saw": saw or kevin_did, "kevin_did": kevin_did})
        event.update(fields)
        self.log(f"fake game: event {json.dumps(event)}")
        self.events.append(event)
        if len(self.events) >= EVENT_BATCH or kind in self.urgent:
            self.flush_events()

    def flush_events(self):
        if self.events:
            self.send({"type": "events", "tick": self.tick, "generation": self.generation, "events": self.events})
        self.events = []
        self.event_ticks = 0

    def new_generation(self, why):
        self.generation += 1
        self.release_overlay(why)
        self.event("generation", why, generation=self.generation)

    # ---- commands

    def command(self, payload):
        try:
            message = json.loads(payload)
        except (ValueError, UnicodeDecodeError):
            message = None
        if not isinstance(message, dict):
            raise Refused(None, "not a JSON object")
        command_id = message.get("id")
        if "id" in message:
            if not number(command_id) or command_id < 0 or command_id > 2147483647:
                raise Refused(None, "id is not a whole number")
            command_id = int(command_id)
        else:
            command_id = -1
        kind = message.get("type")
        if not isinstance(kind, str):
            raise Refused(command_id, "no type")
        kind = kind[:MAXIMUM_TYPE_LENGTH]
        if "generation" in message and (not number(message["generation"]) or int(message["generation"]) != self.generation):
            raise Refused(command_id, "stale generation")

        if kind == "ping":
            t = message.get("t")
            self.send({"type": "pong", "id": command_id, "t": round(t, 3) if number(t) else 0.0, "tick": self.tick})
            return
        if kind == "release":
            self.release_overlay("the sidecar asked")
            self.result(command_id, True, "released")
            return
        if kind == "urgent":
            types = message.get("types")
            if not isinstance(types, list):
                raise Refused(command_id, "types is not an array")
            if len(types) > MAXIMUM_URGENT_TYPES or not all(isinstance(t, str) for t in types):
                raise Refused(command_id, "too many types, or one isn't a string")
            self.urgent = [t[:MAXIMUM_TYPE_LENGTH] for t in types]
            self.result(command_id, True, "urgent types set")
            return
        if kind == "request_view":
            self.result(command_id, True, "view asked for")
            return
        if kind == "hearing":
            on = message.get("on", True)
            if not isinstance(on, bool):
                raise Refused(command_id, "on is not true or false")
            self.hearing = on
            self.result(command_id, True, "hearing" if on else "not hearing")
            return
        if kind == "voice_stop":
            self.voice_samples = 0
            self.result(command_id, True, "voice stopped")
            return

        if not self.kevin or not self.kevin.alive:
            raise Refused(command_id, "no Kevin")
        if kind in ("press", "hold"):
            flag = message.get("flag")
            if not isinstance(flag, str) or flag[:MAXIMUM_TYPE_LENGTH] not in FLAGS:
                raise Refused(command_id, "unknown flag")
            if kind == "press":
                ticks = self.ticks(message, MAXIMUM_PRESS_TICKS, DEFAULT_PRESS_TICKS, command_id)
                self.press_ticks[flag] = ticks
            else:
                on = message.get("on", True)
                if not isinstance(on, bool):
                    raise Refused(command_id, "on is not true or false")
                if on:
                    self.hold_flags.add(flag)
                else:
                    self.hold_flags.discard(flag)
            self.pending.append(command_id)
            return
        if kind == "fire_mode":
            mode = message.get("mode")
            if mode not in ("free", "hold"):
                raise Refused(command_id, "mode is not free or hold")
            self.hold_fire = mode == "hold"
            self.pending.append(command_id)
            return
        if kind == "aim_at":
            ticks = self.ticks(message, MAXIMUM_AIM_TICKS, DEFAULT_AIM_TICKS, command_id)
            target = message.get("target")
            if target == "you":
                self.aim_target = "you"
            elif number(target):
                if target < 0 or target > 4294967295 or self.object_by_id(int(target)) is None:
                    raise Refused(command_id, "no such object")
                self.aim_target = int(target)
            elif isinstance(target, dict):
                point = [target.get(axis) for axis in "xyz"]
                if not all(number(c) and -10000 <= c <= 10000 for c in point):
                    raise Refused(command_id, "target point out of the world")
                self.aim_target = point
            else:
                raise Refused(command_id, "target is not you, an object id or a point")
            self.aim_ticks = ticks
            self.pending.append(command_id)
            return
        raise Refused(command_id, "unknown type")

    def ticks(self, message, maximum, default, command_id):
        if "ticks" not in message:
            return default
        ticks = message["ticks"]
        if not number(ticks) or ticks < 1 or ticks > maximum:
            raise Refused(command_id, "ticks out of range")
        return int(ticks)

    def object_by_id(self, object_id):
        for unit in [self.you, self.kevin] + self.enemies:
            if unit and unit.id == object_id:
                return unit
        return None

    # ---- the connection

    def disconnect(self, why):
        if self.client:
            self.client.close()
            self.log(f"fake game: the sidecar is gone ({why})")
        self.client = None
        self.sending = b""
        self.release_overlay("the sidecar is gone")
        self.hearing = False
        self.voice_samples = 0

    # ---- hearing and Kevin's voice

    def say(self, samples, hold_seconds=0.3):
        """Has the microphone hear samples (16-bit, 16 kHz, bytes) with push to
        talk held, and held hold_seconds more; the time (monotonic) the push
        to talk goes down is returned through the list given back, once it does."""
        samples = samples + bytes(2 * int(16000 * hold_seconds))
        frames = [samples[index:index + HEARING_FRAME_BYTES].ljust(HEARING_FRAME_BYTES, b"\0")
                  for index in range(0, len(samples), HEARING_FRAME_BYTES)]
        started = []
        with self.speech_lock:
            self.speeches.append((frames, started))
        return started

    def hear(self):
        if self.speech is None:
            with self.speech_lock:
                if not self.speeches or not self.hearing or not self.client:
                    return
                frames, started = self.speeches.pop(0)
            self.utterance += 1
            self.hearing_sequence = 0
            now = time.monotonic()
            self.speech = [frames, 0, now]
            started.append(now)
            self.send({"type": "ptt", "down": True, "utterance": self.utterance, "tick": self.tick,
                       "ms": int(now * 1000)})
        frames, next_frame, started = self.speech
        due = min(len(frames), int((time.monotonic() - started) / 0.02) + 1)
        while next_frame < due:
            self.send_raw(protocol.frame_hearing(self.utterance, self.hearing_sequence, frames[next_frame]))
            self.hearing_sequence += 1
            next_frame += 1
        self.speech[1] = next_frame
        if next_frame >= len(frames):
            self.speech = None
            self.send({"type": "ptt", "down": False, "utterance": self.utterance, "tick": self.tick,
                       "ms": int(time.monotonic() * 1000)})

    def voice_message(self, payload):
        if len(payload) < 4 or (len(payload) - 4) % 2:
            self.result(-1, False, "voice is a line number and 16-bit samples")
            return
        (line,) = struct.unpack_from("<I", payload)
        if line != self.voice_line:
            self.voice_line = line
            self.voice_samples = 0
        self.voice_samples = min(VOICE_QUEUE_SAMPLES, self.voice_samples + (len(payload) - 4) // 2)

    def speak(self):
        playing = self.voice_samples > 0
        self.voice_samples = max(0, self.voice_samples - 48000 // TICKS_PER_SECOND)
        if playing != self.voice_playing:
            self.voice_playing = playing
            self.send({"type": "voice", "line": self.voice_line, "playing": playing, "tick": self.tick,
                       "ms": int(time.monotonic() * 1000)})

    def send_raw(self, data):
        if not self.client:
            return
        if len(self.sending) + len(data) > SEND_LIMIT:
            self.disconnect("not reading")
            return
        self.sending += data

    def accept(self):
        readable, _, _ = select.select([self.listener], [], [], 0)
        if not readable:
            return
        client, _ = self.listener.accept()
        if self.client:
            client.close()
            self.log("fake game: a second sidecar refused")
            return
        client.setblocking(False)
        client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.client = client
        self.reader = protocol.Reader(protocol.MAXIMUM_INCOMING)
        self.last_message = time.monotonic()
        self.silent = False
        self.log("fake game: the sidecar is connected")
        self.send({"type": "hello", "protocol": protocol.PROTOCOL, "game": "fakegame", "tick": self.tick,
                   "generation": self.generation, "kevin": self.kevin.id if self.kevin else None, "level": "fake"})

    def receive(self):
        while self.client:
            try:
                data = self.client.recv(65536)
            except BlockingIOError:
                break
            except OSError as error:
                self.disconnect(str(error))
                return
            if not data:
                self.disconnect("closed")
                return
            self.reader.feed(data)
        if not self.client:
            return
        try:
            for kind, payload in self.reader.messages():
                self.last_message = time.monotonic()
                self.silent = False
                if kind == protocol.KIND_VOICE:
                    self.voice_message(payload)
                    continue
                if kind != protocol.KIND_JSON:
                    self.result(-1, False, "unknown message kind")
                    continue
                try:
                    self.command(payload)
                except Refused as refusal:
                    command_id, reason = refusal.args
                    self.result(-1 if command_id is None else command_id, False, reason)
        except ValueError as error:
            self.disconnect(str(error))

    # ---- the world

    def spawn_kevin(self):
        self.kevin_salt += 1
        behind = [self.you.position[0] - 3.0 * math.cos(self.you.facing),
                  self.you.position[1] - 3.0 * math.sin(self.you.facing), 0.0]
        self.kevin = Unit((self.kevin_salt << 16) | KEVIN_INDEX, "cyborg", 1, behind)
        self.new_generation("Kevin was made")
        self.event("kevin_made", "Kevin was made behind you.", self.kevin)

    def simulate(self):
        # you walk a loop of radius 20, one turn a minute or so
        angle = self.tick * TICK * 0.1
        self.you.position = [20.0 * math.cos(angle), 20.0 * math.sin(angle), 0.0]
        self.you.facing = angle + math.pi / 2
        self.you.aim = [math.cos(self.you.facing), math.sin(self.you.facing), 0.0]

        if not self.kevin or not self.kevin.alive:
            self.dead_ticks += 1
            if self.dead_ticks >= RESPAWN_TICKS:
                self.dead_ticks = 0
                self.spawn_kevin()
            return

        # Kevin follows three units behind you
        behind = [self.you.position[0] - 3.0 * math.cos(self.you.facing),
                  self.you.position[1] - 3.0 * math.sin(self.you.facing)]
        for axis in range(2):
            self.kevin.position[axis] += (behind[axis] - self.kevin.position[axis]) * 0.2
        self.kevin.facing = self.you.facing

        # the overlay: presses and holds, his aim
        flags = {flag for flag, ticks in self.press_ticks.items() if ticks > 0} | self.hold_flags
        for flag in self.press_ticks:
            if self.press_ticks[flag] > 0:
                self.press_ticks[flag] -= 1
        self.kevin.position[2] = 0.5 if "jump" in flags else 0.0
        target = None
        if self.aim_target is not None and self.aim_ticks > 0:
            self.aim_ticks -= 1
            if self.aim_target == "you":
                target = self.you.position
            elif isinstance(self.aim_target, list):
                target = self.aim_target
            else:
                unit = self.object_by_id(self.aim_target)
                target = unit.position if unit else None
        elif self.aim_target is not None:
            self.aim_target = None
        if target is None:
            target = [self.kevin.position[0] + math.cos(self.kevin.facing), self.kevin.position[1] + math.sin(self.kevin.facing), 0.0]
        offset = [target[i] - self.kevin.position[i] for i in range(3)]
        length = math.sqrt(sum(c * c for c in offset)) or 1.0
        self.kevin.aim = [c / length for c in offset]
        if not self.hold_fire and self.kevin.rounds > 0 and self.random.random() < 0.05:
            self.kevin.rounds -= 1

        # the commands that took effect on this tick
        for command_id in self.pending:
            self.send({"type": "applied", "id": command_id, "tick": self.tick})
        if self.pending:
            self.flush()
        self.pending = []

        # now and then he's hit, by an enemy or by you; his shield recharges
        if self.random.random() < 1.0 / (8 * TICKS_PER_SECOND):
            by_you = self.random.random() < 0.25
            attacker = self.you if by_you else self.random.choice(self.enemies)
            lost = 0.2 + self.random.random() * 0.3
            shield = max(0.0, self.kevin.shield - lost)
            self.kevin.health = max(0.0, self.kevin.health - max(0.0, lost - self.kevin.shield))
            self.kevin.shield = shield
            fields = {"shield": round(self.kevin.shield, 3), "health": round(self.kevin.health, 3),
                      "lost": round(lost, 3), "attacker": attacker.id}
            if by_you:
                self.event("player_shot_kevin", "You shot Kevin.", self.kevin, **fields)
            else:
                self.event("kevin_damaged", "Kevin was hit.", self.kevin, **fields)
            if self.kevin.health <= 0.0:
                self.kevin.alive = False
                self.event("kevin_died", "Kevin died.", self.kevin)
                self.new_generation("Kevin died")
                self.dead_ticks = 0
        else:
            self.kevin.shield = min(1.0, self.kevin.shield + 0.002)

    def checkpoints(self):
        if self.tick % (30 * TICKS_PER_SECOND) == 0:
            self.saved = self.tick
            self.event("checkpoint", "The game saved a checkpoint.", checkpoint=self.tick)
        self.revert_counter += 1
        if self.revert_ticks and self.revert_counter >= self.revert_ticks and self.saved:
            # a revert brings back the checkpoint's game time, as the game's does
            self.revert_counter = 0
            self.tick = self.saved
            self.event("checkpoint_revert", "The game went back to a checkpoint.", checkpoint=self.tick)
            self.new_generation("checkpoint revert")

    def send_state(self):
        nearby = []
        for unit in self.enemies:
            if math.dist(unit.position, self.you.position) <= NEARBY_DISTANCE:
                nearby.append({"id": unit.id, "tag": unit.tag, "team": unit.team, "alive": unit.alive,
                               "position": unit.state()["position"]})
        self.send({"type": "state", "tick": self.tick, "generation": self.generation,
                   "overlay": self.overlay_active(), "you": self.you.state(),
                   "kevin": self.kevin.state() if self.kevin else None,
                   "crosshair": None, "nearby": nearby})

    # ---- the loop

    def step(self):
        self.tick += 1
        self.accept()
        self.receive()
        if self.client and not self.silent and time.monotonic() - self.last_message > SILENT_SECONDS:
            self.silent = True
            self.release_overlay("the sidecar is silent")
        self.hear()
        self.speak()
        self.simulate()
        self.checkpoints()
        self.event_ticks += 1
        if self.event_ticks >= EVENT_TICKS:
            self.flush_events()
        self.state_ticks += 1
        if self.client and self.state_ticks >= STATE_TICKS:
            self.state_ticks = 0
            self.send_state()
        self.flush()

    def run(self, stop=None, ready=None):
        """Runs until stop is set; ready(port) is called once listening."""
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", self.port))
        self.port = self.listener.getsockname()[1]
        self.listener.listen(1)
        self.log(f"fake game: listening on 127.0.0.1:{self.port}")
        if ready:
            ready(self.port)
        next_tick = time.monotonic()
        try:
            while not (stop and stop.is_set()):
                next_tick += TICK
                self.step()
                wait = next_tick - time.monotonic()
                if wait > 0:
                    time.sleep(wait)
                else:
                    next_tick = time.monotonic()
        finally:
            if self.client:
                self.client.close()
            self.listener.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--port", type=int, default=protocol.GAME_PORT + 1)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--revert-seconds", type=float, default=120.0, help="revert to the last checkpoint this often; 0 never")
    arguments = parser.parse_args()
    try:
        FakeGame(arguments.port, arguments.seed, arguments.revert_seconds).run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
