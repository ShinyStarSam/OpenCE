"""The bark cache (kevin/BRIDGE_PLAN.md, "Barks: instant speech"): short
lines made ahead of time, kept in memory as the game plays them (48 kHz,
16-bit), so one starts as fast as the game can play it.

    kevin/sidecar/data/barks/<category>/<anything>.wav

(made by kevin/tools/make_barks.py; Breeze's output is under its
non-commercial licence, so the folder stays out of git). A category's
variants take turns at random, never the same one twice running.
"""

import array
import os
import random
import wave

RATE = 48000


def resampled(samples, rate):
    """16-bit samples (an array) at rate, as RATE, linearly."""
    if rate == RATE or not samples:
        return samples
    count = int(len(samples) * RATE / rate)
    step = rate / RATE
    last = len(samples) - 1
    output = array.array("h", bytes(2 * count))
    for index in range(count):
        position = index * step
        left = int(position)
        right = min(left + 1, last)
        fraction = position - left
        output[index] = int(samples[left] + (samples[right] - samples[left]) * fraction)
    return output


def load_wav(path):
    """A WAV's samples at RATE, mono, 16-bit, as bytes."""
    with wave.open(path, "rb") as file:
        channels, width, rate = file.getnchannels(), file.getsampwidth(), file.getframerate()
        frames = file.readframes(file.getnframes())
    if width != 2:
        raise ValueError(f"{path}: {8 * width}-bit; barks are 16-bit")
    samples = array.array("h")
    samples.frombytes(frames)
    if channels > 1:
        samples = array.array("h", (sum(samples[index:index + channels]) // channels
                                    for index in range(0, len(samples), channels)))
    return trimmed(resampled(samples, rate)).tobytes()


def trimmed(samples, quiet=300, margin=RATE // 100):
    """Silence before and after the line cut, but for 10 ms (a bark starts at once)."""
    loud = [index for index in range(len(samples)) if abs(samples[index]) >= quiet]
    if not loud:
        return samples
    return samples[max(0, loud[0] - margin):min(len(samples), loud[-1] + margin)]


class Barks:
    def __init__(self, folder, log=print):
        self.folder = folder
        self.lines = {}
        self.last = {}
        if os.path.isdir(folder):
            for category in sorted(os.listdir(folder)):
                directory = os.path.join(folder, category)
                if not os.path.isdir(directory):
                    continue
                for name in sorted(os.listdir(directory)):
                    if name.lower().endswith(".wav"):
                        try:
                            self.lines.setdefault(category, []).append((name, load_wav(os.path.join(directory, name))))
                        except (OSError, ValueError, EOFError, wave.Error) as error:
                            log(f"barks: {error}")
        count = sum(len(lines) for lines in self.lines.values())
        log(f"barks: {count} in {len(self.lines)} categories from {folder}" if count else
            f"barks: none in {folder} (kevin/tools/make_barks.py makes them)")

    def pick(self, category):
        """(name, samples) of a variant of category, or None."""
        lines = self.lines.get(category)
        if not lines:
            return None
        choices = [index for index in range(len(lines)) if index != self.last.get(category)] or [0]
        index = random.choice(choices)
        self.last[category] = index
        return lines[index]
