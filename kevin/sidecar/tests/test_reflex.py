"""Tier 1 and the reflex slice (kevin/BRIDGE_PLAN.md, M2).

- the keyword grammar on transcripts, as Parakeet writes them;
- with Parakeet running (kevin/tools/s0/parakeet_serve.py): "Kevin, jump"
  said to the fake game's microphone makes the sidecar press jump and bark,
  through the recorder, and the recording replayed into a new sidecar makes
  it press jump again (skipped without Parakeet).

    python -m unittest discover -s kevin/sidecar/tests -v
"""

import os
import queue
import sys
import tempfile
import threading
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SIDECAR = os.path.dirname(HERE)
TOOLS = os.path.join(SIDECAR, "..", "tools")
sys.path.insert(0, SIDECAR)
sys.path.insert(0, TOOLS)
sys.path.insert(0, os.path.join(SIDECAR, "fakegame"))
sys.path.insert(0, os.path.join(SIDECAR, "replay"))

import fakegame  # noqa: E402
import recorder  # noqa: E402
import replayer  # noqa: E402
from kevin import Kevin  # noqa: E402
from reflex import keywords  # noqa: E402
from speech.parakeet import Parakeet  # noqa: E402


def quiet(*_):
    pass


def actions(text):
    return [action for action, _, _ in keywords.find(text)]


class GrammarTest(unittest.TestCase):
    def test_imperatives(self):
        self.assertEqual(actions("Kevin, jump."), ["jump"])
        self.assertEqual(actions("Jump, Kevin!"), ["jump"])
        self.assertEqual(actions("Kevin, get down."), ["crouch"])
        self.assertEqual(actions("Stand up."), ["stand"])
        self.assertEqual(actions("Kevin, throw a grenade."), ["grenade"])
        self.assertEqual(actions("Kevin, swap weapons and reload."), ["swap_weapons", "reload"])

    def test_longest_phrase_first(self):
        self.assertEqual(actions("Hold fire."), ["hold_fire"])
        self.assertEqual(actions("Open fire!"), ["fire"])
        self.assertEqual(actions("Stop shooting."), ["hold_fire"])
        self.assertEqual(actions("Kevin, do not shoot."), ["hold_fire"])

    def test_left_to_the_brain(self):
        self.assertEqual(actions("Kevin, can you jump?"), [])
        self.assertEqual(actions("Hey Kevin, could you reload?"), [])
        self.assertEqual(actions("Don't jump."), [])
        self.assertEqual(actions("Never reload in a fight."), [])
        self.assertEqual(actions("Stand by that rock."), [])
        self.assertEqual(actions("What are we doing here?"), [])

    def test_each_occurrence_once(self):
        reflex = keywords.Reflex()
        self.assertEqual(reflex.hear(1, "Kevin", False), [])
        self.assertEqual(reflex.hear(1, "Kevin, jump", False), ["jump"])
        self.assertEqual(reflex.hear(1, "Kevin, jump.", True), [])
        self.assertEqual(reflex.hear(1, "Kevin, jump, jump", True), ["jump"])
        self.assertEqual(reflex.hear(2, "Jump.", True), ["jump"])

    def test_negations_wait_for_the_final(self):
        reflex = keywords.Reflex()
        self.assertEqual(reflex.hear(1, "Don't shoot", False), [])
        self.assertEqual(reflex.hear(1, "Don't jump.", True), [])
        reflex = keywords.Reflex()
        self.assertEqual(reflex.hear(2, "Don't shoot", False), [])
        self.assertEqual(reflex.hear(2, "Don't shoot.", True), ["hold_fire"])


def in_thread(target, **arguments):
    ports = queue.Queue()
    stop = threading.Event()
    results = []
    thread = threading.Thread(
        target=lambda: results.append(target(stop=stop, ready=ports.put, **arguments)), daemon=True)
    thread.start()
    return thread, ports.get(timeout=5), stop, results


@unittest.skipUnless(Parakeet().available(), "Parakeet isn't running (kevin/tools/s0/parakeet_serve.py)")
class HearingTest(unittest.TestCase):
    def test_jump_heard_recorded_and_replayed(self):
        import m2_reflex_test
        speech = m2_reflex_test.speech_file("Kevin, jump.", "Microsoft David Desktop", 0)
        folder = tempfile.mkdtemp()
        game = fakegame.FakeGame(0, seed=1, revert_seconds=0, log=quiet)
        game_thread, game_port, game_stop, _ = in_thread(game.run)
        path = os.path.join(folder, "session.jsonl")
        record_thread, record_port, _, _ = in_thread(
            recorder.record, path=path, listen_port=0, game_port=game_port, log=quiet)

        records = queue.Queue()
        kevin = Kevin(record_port, logs=folder, log=quiet)
        kevin.on_record = records.put
        kevin.start()
        self.assertTrue(kevin.bridge.wait_connected(5))
        deadline = time.monotonic() + 15
        while not (kevin.bridge.state or {}).get("kevin") and time.monotonic() < deadline:
            time.sleep(0.1)
        time.sleep(0.3)
        game.say(bytes(8000) + speech)
        record = records.get(timeout=10)
        self.assertEqual(record["action"], "jump")
        self.assertIsNotNone(record["applied"], record)
        self.assertIsNotNone(record["bark_playing"], record)
        self.assertLess(record["figures"]["action_ms"], 250)
        kevin.stop()
        record_thread.join(timeout=10)
        game_stop.set()
        game_thread.join(timeout=5)

        # the recording has the microphone's frames, and plays back into a new sidecar
        _, lines = replayer.load(path)
        self.assertTrue(any(line["from"] == "game" and line["kind"] == 2 for line in lines))
        replay_thread, replay_port, _, results = in_thread(replayer.replay, path=path, port=0, log=quiet)
        again = Kevin(replay_port, logs=folder, log=quiet)
        again.start()
        replay_thread.join(timeout=30)
        again.stop()
        self.assertGreaterEqual(results[0]["replayed_sidecar"]["press"], 1, results[0])


if __name__ == "__main__":
    unittest.main()
