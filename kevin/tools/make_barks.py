"""Makes Kevin's bark cache (kevin/sidecar/data/barks/<category>/*.wav).

With Breeze TTS 2 running (kevin/tools/s0/run_breeze.sh) the lines are in
Kevin's voice; without it, in a Windows voice, as stand-ins.

    python kevin/tools/make_barks.py [--engine breeze|sapi] [--again]
"""

import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import speech_lines  # noqa: E402

BARKS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "sidecar", "data", "barks")

LINES = {
    "ack": ["On it.", "Moving.", "Yeah, yeah.", "Copy.", "Got it.", "Fine.", "Roger.", "Done."],
}


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--engine", choices=("breeze", "sapi"))
    parser.add_argument("--again", action="store_true", help="make lines that exist again")
    arguments = parser.parse_args()
    engine = arguments.engine or ("breeze" if speech_lines.breeze_available() else "sapi")
    print(f"making barks with {engine}")
    for category, lines in LINES.items():
        folder = os.path.join(BARKS, category)
        os.makedirs(folder, exist_ok=True)
        for index, text in enumerate(lines):
            path = os.path.join(folder, f"{index:02d}.wav")
            if os.path.exists(path) and not arguments.again:
                continue
            if engine == "breeze":
                speech_lines.breeze(text, path, seed=7 + index)
            else:
                speech_lines.sapi(text, path, voice=speech_lines.SAPI_VOICES[0], rate=2)
            print(f"  {category}/{index:02d}.wav: {text}")


if __name__ == "__main__":
    main()
