"""Latency, measured from day one (kevin/BRIDGE_PLAN.md, "Measuring").

Each reflex is a record of its stages' times (time.monotonic(), seconds),
written as a JSON line once it's complete, with the milliseconds between
stages worked out. The targets (the latency budget) are measured from when
you stopped speaking: "speech end", the last frame of the utterance loud
enough to be speech, as it came from the game. A reflex fired before you
finished has a negative figure.

Rolling p50 / p95 of each figure are kept for the summary.
"""

import json
import os
import threading
import time

# figure: (from stage, to stage)
FIGURES = {
    "action_ms": ("speech_end", "applied"),
    "bark_ms": ("speech_end", "bark_playing"),
    "decided_ms": ("audio_end", "decided"),
    "pass_ms": ("pass_started", "pass_finished"),
    "to_game_ms": ("sent", "applied"),
    "bark_start_ms": ("bark_sent", "bark_playing"),
}
TARGETS = {"action_ms": 250.0, "bark_ms": 300.0}


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def figures(record):
    out = {}
    for name, (start, end) in FIGURES.items():
        if record.get(start) is not None and record.get(end) is not None:
            out[name] = round((record[end] - record[start]) * 1000, 1)
    return out


class LatencyLog:
    def __init__(self, folder=None, keep=200):
        self.lock = threading.Lock()
        self.keep = keep
        self.values = {name: [] for name in FIGURES}
        self.file = None
        if folder:
            os.makedirs(folder, exist_ok=True)
            self.path = os.path.join(folder, time.strftime("latency-%Y%m%d-%H%M%S.jsonl"))
            self.file = open(self.path, "a", encoding="utf-8")

    def write(self, record):
        """A complete record: its figures added, kept and written."""
        record = dict(record)
        record["figures"] = figures(record)
        with self.lock:
            for name, value in record["figures"].items():
                values = self.values[name]
                values.append(value)
                del values[:-self.keep]
            if self.file:
                self.file.write(json.dumps(record, separators=(",", ":")) + "\n")
                self.file.flush()
        return record

    def summary(self):
        """{figure: (count, p50, p95, target)}"""
        with self.lock:
            return {name: (len(values), percentile(values, 0.5), percentile(values, 0.95), TARGETS.get(name))
                    for name, values in self.values.items() if values}

    def summary_lines(self):
        lines = []
        for name, (count, p50, p95, target) in self.summary().items():
            verdict = "" if target is None else ("  (met)" if p95 <= target else f"  (over {target:.0f})")
            lines.append(f"{name}: {count} times, p50 {p50:.0f} ms, p95 {p95:.0f} ms{verdict}")
        return lines
