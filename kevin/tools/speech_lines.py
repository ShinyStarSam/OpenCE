"""Speech made from text, for Kevin's barks and the reflex tests: with
Breeze TTS 2 (Kevin's own voice, running in WSL on port 7860:
kevin/tools/s0/run_breeze.sh) or Windows' own voices (SAPI: David, Zira),
which need nothing running and are good enough to test hearing with.
"""

import http.client
import os
import subprocess
import uuid
import wave

SAPI_VOICES = ("Microsoft David Desktop", "Microsoft Zira Desktop")
KEVIN_VOICE = ("A gruff, gravelly male soldier in his thirties, sarcastic and impatient, "
               "speaking fast and clipped like he's in the middle of a firefight.")

_SAPI_SCRIPT = r"""
Add-Type -AssemblyName System.Speech
$s = New-Object System.Speech.Synthesis.SpeechSynthesizer
$s.SelectVoice($env:SAPI_VOICE)
$s.Rate = [int]$env:SAPI_RATE
$format = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
$s.SetOutputToWaveFile($env:SAPI_PATH, $format)
$s.Speak($env:SAPI_TEXT)
$s.Dispose()
"""


def sapi(text, path, voice=SAPI_VOICES[0], rate=0):
    """text spoken by a Windows voice into a 16 kHz WAV; rate -10 (slow) to 10."""
    environment = dict(os.environ, SAPI_VOICE=voice, SAPI_RATE=str(rate), SAPI_PATH=os.path.abspath(path),
                       SAPI_TEXT=text)
    subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", _SAPI_SCRIPT], env=environment,
                   check=True, capture_output=True, timeout=60)


def breeze_available(port=7860):
    try:
        connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2.0)
        connection.request("GET", "/")
        connection.getresponse().read()
        connection.close()
        return True
    except OSError:
        return False


def breeze(text, path, instruction=KEVIN_VOICE, seed=7, port=7860):
    """text in Kevin's voice (Breeze TTS 2) into a WAV at Breeze's rate."""
    fields = {"text": text, "instruction": instruction, "cfg_scale": "4", "seed": str(seed)}
    boundary = uuid.uuid4().hex
    body = b"".join(f"--{boundary}\r\nContent-Disposition: form-data; name=\"{key}\"\r\n\r\n{value}\r\n".encode()
                    for key, value in fields.items()) + f"--{boundary}--\r\n".encode()
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=300)
    connection.request("POST", "/v1/audio/speech", body,
                       {"content-type": f"multipart/form-data; boundary={boundary}"})
    response = connection.getresponse()
    if response.status != 200:
        raise RuntimeError(f"breeze {response.status}: {response.read()[:300]!r}")
    rate = int(response.getheader("X-Sample-Rate", "24000"))
    pcm = response.read()
    connection.close()
    with wave.open(path, "wb") as file:
        file.setnchannels(1)
        file.setsampwidth(2)
        file.setframerate(rate)
        file.writeframes(pcm)
