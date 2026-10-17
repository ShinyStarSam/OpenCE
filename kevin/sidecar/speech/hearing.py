"""Hearing you: an utterance at a time (push to talk), transcribed by
Parakeet on a sliding window while you talk, so tier 1 sees words before the
sentence ends.

Parakeet isn't a streaming model, but a pass over up to 4 seconds takes
70-110 ms on the 3090 next to everything else (S0). So passes run back to
back while you talk, each over the utterance's latest WINDOW_SECONDS, and
on_partial(utterance, text, info) gets each one's words. A last pass, with
info["final"], follows the release. Voice activity is a frame's level
against the room's: when you started and stopped speaking is kept, for the
latency log.
"""

import array
import math
import threading
import time

RATE = 16000
FRAME = 320                   # 20 ms
WINDOW_SECONDS = 4.0
MINIMUM_SECONDS = 0.3         # less is not worth a pass
# a frame is speech this far over the room's level, and no quieter than this
SPEECH_OVER_ROOM = 3.0
SPEECH_FLOOR = 300.0


def level(samples):
    """A frame's root mean square (16-bit samples as an array)."""
    if not samples:
        return 0.0
    return math.sqrt(sum(value * value for value in samples) / len(samples))


class Utterance:
    def __init__(self, number, started):
        self.number = number
        self.started = started
        self.samples = array.array("h")
        self.times = []           # each frame's arrival
        self.ended = None
        self.speech_start = None
        self.speech_end = None
        self.passed = 0           # samples the last pass covered
        self.text = ""
        self.final = False


class Hearing:
    def __init__(self, parakeet, on_partial=None, on_end=None, log=print):
        self.parakeet = parakeet
        self.on_partial = on_partial
        self.on_end = on_end
        self.log = log
        self.lock = threading.Condition()
        self.current = None
        self.room = 100.0         # the room's level, from frames heard outside speech
        self.stopping = False
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        with self.lock:
            self.stopping = True
            self.lock.notify_all()

    # ---------- from the bridge (or a test's recordings)

    def begin(self, number, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            if self.current and self.current.ended is None:
                self.current.ended = now
            self.current = Utterance(number, now)
            self.lock.notify_all()

    def hear(self, number, samples, now=None):
        """16-bit little-endian samples (bytes) of utterance number."""
        now = time.monotonic() if now is None else now
        frame = array.array("h")
        frame.frombytes(samples[:len(samples) & ~1])
        with self.lock:
            utterance = self.current
            if utterance is None or utterance.number != number or utterance.ended is not None:
                return
            utterance.samples.extend(frame)
            utterance.times.append((len(utterance.samples), now))
            loudness = level(frame)
            if loudness >= max(SPEECH_FLOOR, self.room * SPEECH_OVER_ROOM):
                if utterance.speech_start is None:
                    utterance.speech_start = now
                utterance.speech_end = now
            elif utterance.speech_start is None:
                self.room = 0.9 * self.room + 0.1 * max(loudness, 30.0)
            self.lock.notify_all()

    def end(self, number, now=None):
        now = time.monotonic() if now is None else now
        with self.lock:
            if self.current and self.current.number == number and self.current.ended is None:
                self.current.ended = now
                self.lock.notify_all()

    # ---------- the passes

    def _next(self):
        """(utterance, samples, the time the last came, final) to pass over next, or None to stop."""
        with self.lock:
            while True:
                if self.stopping:
                    return None
                utterance = self.current
                if utterance and not utterance.final:
                    fresh = len(utterance.samples) > utterance.passed
                    enough = len(utterance.samples) >= MINIMUM_SECONDS * RATE and utterance.speech_start is not None
                    if utterance.ended is not None and (not fresh or not enough):
                        utterance.final = True
                        if enough or utterance.passed:
                            return utterance, None, None, True
                        continue
                    if fresh and enough:
                        count = len(utterance.samples)
                        window = utterance.samples[max(0, count - int(WINDOW_SECONDS * RATE)):count]
                        arrived = utterance.times[-1][1]
                        utterance.passed = count
                        return utterance, window.tobytes(), arrived, utterance.ended is not None
                self.lock.wait(0.5)

    def _run(self):
        while True:
            job = self._next()
            if job is None:
                return
            utterance, samples, arrived, final = job
            if samples is not None:
                started = time.monotonic()
                try:
                    text, model_ms, round_trip_ms = self.parakeet.transcribe(samples, RATE)
                except Exception as error:  # a pass lost is a partial missed, not the hearing
                    self.log(f"hearing: parakeet failed ({error})")
                    continue
                utterance.text = text.strip()
                info = {"final": final, "audio_end": arrived, "pass_started": started,
                        "pass_finished": time.monotonic(), "model_ms": model_ms, "round_trip_ms": round_trip_ms,
                        "seconds": len(samples) / 2 / RATE}
                if self.on_partial:
                    self.on_partial(utterance, utterance.text, info)
            if final:
                with self.lock:
                    utterance.final = True
                if self.on_end:
                    self.on_end(utterance)
