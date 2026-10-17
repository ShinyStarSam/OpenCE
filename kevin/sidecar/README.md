# Kevin's sidecar

The program that is Kevin's mind (`kevin/SIDECAR_PLAN.md`). It talks to the
game through the bridge (`port/linux/game/kevin_bridge.c`,
`kevin/BRIDGE_PLAN.md`): TCP on 127.0.0.1, port `kevin.port` (2324), one
connection at a time. Every message, both ways, is a 4-byte length (little
endian, of what follows), a 1-byte kind (1: JSON) and the payload;
`protocol.py` frames and reads them.

Python 3.12, standard library only so far.

## The fake game

`fakegame/fakegame.py` listens as the game does and speaks the same
messages, with the same checks: `hello`, `state` every third tick (30
ticks a second), `events` in batches (urgent types at once), `applied`,
`action_result` and `pong`. Its world is small: you walk a loop, Kevin
follows behind you, three enemies stand along it. Kevin is hit now and then,
by them or by you, dies and is made again, and the game saves a checkpoint
every 30 seconds and reverts to one every 2 minutes.

```bash
python kevin/sidecar/fakegame/fakegame.py --port 2325
```

`--seed` picks the run of events, and `--revert-seconds 0` turns the
reverts off. The bridge's own check runs against it as against the game:

```bash
python kevin/tools/m1_bridge_test.py --port 2325
```

## Recording and replaying

`replay/recorder.py` sits between a sidecar and the game (or the fake
game): it listens where the sidecar connects, connects on to the game,
passes every byte through, and writes each message both ways with its time
to a JSON-lines file.

```bash
python kevin/sidecar/replay/recorder.py session.jsonl --listen 2326 --game-port 2324
```

Point the sidecar at port 2326 instead of 2324. The recording ends when
either side closes.

`replay/replayer.py` plays the game's side of a recording back into a
sidecar, with the same timing, and counts what the sidecar sends by type
against what it sent when it was recorded. It doesn't answer the sidecar.

```bash
python kevin/sidecar/replay/replayer.py session.jsonl --port 2325
```

`--speed 2` plays it twice as fast.

## Tests

```bash
python -m unittest discover -s kevin/sidecar/tests -v
```

The bridge's check passes against the fake game, directly and through the
recorder, and the recording replays with every message sent close to its
time.
