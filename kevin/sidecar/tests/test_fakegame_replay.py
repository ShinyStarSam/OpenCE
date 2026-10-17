"""The fake game, the recorder and the replayer, together.

- kevin/tools/m1_bridge_test.py passes against the fake game, directly and
  through the recorder;
- the recording replays: every game message is sent, close to its time.

    python -m unittest discover -s kevin/sidecar/tests -v
"""

import os
import queue
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SIDECAR = os.path.dirname(HERE)
CLIENT = os.path.join(SIDECAR, "..", "tools", "m1_bridge_test.py")
sys.path.insert(0, SIDECAR)
sys.path.insert(0, os.path.join(SIDECAR, "fakegame"))
sys.path.insert(0, os.path.join(SIDECAR, "replay"))

import fakegame  # noqa: E402
import protocol  # noqa: E402
import recorder  # noqa: E402
import replayer  # noqa: E402


def quiet(*_):
    pass


def in_thread(target, **arguments):
    """Runs target in a thread; returns the thread, the port it's ready on and its stop event."""
    ports = queue.Queue()
    stop = threading.Event()
    results = []
    thread = threading.Thread(
        target=lambda: results.append(target(stop=stop, ready=ports.put, **arguments)), daemon=True)
    thread.start()
    return thread, ports.get(timeout=5), stop, results


def run_client(port):
    return subprocess.run([sys.executable, "-I", CLIENT, "--port", str(port), "--tries", "3", "--seconds", "1"],
                          capture_output=True, text=True, timeout=120)


class FakeGameTest(unittest.TestCase):
    def setUp(self):
        game = fakegame.FakeGame(0, seed=1, revert_seconds=0, log=quiet)
        self.game_thread, self.game_port, self.game_stop, _ = in_thread(game.run)

    def tearDown(self):
        self.game_stop.set()
        self.game_thread.join(timeout=5)

    def test_client_passes(self):
        completed = run_client(self.game_port)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)

    def test_record_and_replay(self):
        path = os.path.join(tempfile.mkdtemp(), "session.jsonl")
        record_thread, record_port, record_stop, counts = in_thread(
            recorder.record, path=path, listen_port=0, game_port=self.game_port, log=quiet)
        completed = run_client(record_port)
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        record_thread.join(timeout=10)
        self.assertGreater(counts[0], 50)

        header, lines = replayer.load(path)
        self.assertEqual(header["game_port"], self.game_port)
        self.assertTrue(any(line["from"] == "sidecar" for line in lines))

        replay_thread, replay_port, _, results = in_thread(replayer.replay, path=path, port=0, log=quiet)
        client = socket.create_connection(("127.0.0.1", replay_port))
        reader = protocol.Reader()
        received = 0
        client.settimeout(30)
        while True:
            data = client.recv(65536)
            if not data:
                break
            reader.feed(data)
            received += sum(1 for _ in reader.messages())
        client.close()
        replay_thread.join(timeout=10)
        result = results[0]
        self.assertEqual(result["sent"], result["game_messages"])
        self.assertEqual(received, result["game_messages"])
        self.assertLess(result["worst_offset"], 0.05)


if __name__ == "__main__":
    unittest.main()
