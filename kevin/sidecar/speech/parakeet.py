"""Parakeet TDT 0.6B v2, resident in WSL (kevin/tools/s0/parakeet_serve.py):
POST /transcribe with a WAV, {"text", "ms"} back."""

import http.client
import io
import json
import time
import wave


def wav_bytes(samples, rate):
    """16-bit mono samples (bytes) as a WAV file."""
    output = io.BytesIO()
    with wave.open(output, "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(samples)
    return output.getvalue()


class Parakeet:
    def __init__(self, port=8009, timeout=10.0):
        self.port = port
        self.timeout = timeout

    def available(self):
        try:
            connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=2.0)
            connection.request("GET", "/health")
            ok = connection.getresponse().status == 200
            connection.close()
            return ok
        except OSError:
            return False

    def transcribe(self, samples, rate=16000):
        """(text, the model's milliseconds, the round trip's milliseconds)."""
        body = wav_bytes(samples, rate)
        start = time.perf_counter()
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.timeout)
        try:
            connection.request("POST", "/transcribe", body, {"content-type": "audio/wav"})
            response = connection.getresponse()
            reply = json.loads(response.read())
        finally:
            connection.close()
        if "text" not in reply:
            raise RuntimeError(f"parakeet: {reply}")
        return reply["text"], reply.get("ms"), (time.perf_counter() - start) * 1000
