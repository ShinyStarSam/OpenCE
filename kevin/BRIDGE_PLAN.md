# Step 2: the game bridge

How the game and Kevin's sidecar talk. The game side exposes state, two
camera views and events, and takes commands for Kevin, one AI-driven
Spartan. The sidecar does the thinking (see NOTES.md).

Scope for v1: single-player campaign, Windows build, one Kevin. Nothing
here is built for more than one AI character.

**The priority is reactivity.** Kevin has to act as fast as a person
reacts, and talk while he does. Every design choice below is checked
against the latency budget first.

## Latency budget

Targets, measured from the moment you stop speaking (or the game event
happens) to Kevin visibly acting. Every stage is timestamped so these are
measured, not guessed (see "Measuring").

| Situation | Path | Target |
| --- | --- | --- |
| "Kevin, jump" / "get down" / "hold fire" | Streaming STT → keyword reflex → control overlay | **≤ 250 ms**, often before you finish the sentence |
| Kevin says "On it" / "Moving" after an order | Cached bark clip | ≤ 300 ms |
| Game event: enemy spotted, shields down, grenade nearby | In-game reflex rule, plus a cached bark | ≤ 1 tick (33 ms) for the action, ≤ 150 ms for the bark |
| "Go stand by that rock" | Streaming STT → reflex classifier with crosshair hints → command list | ≤ 400 ms until he starts moving |
| A real reply ("What are we doing?") | Brain, streamed tokens, TTS from the first sentence | First audio ≤ 1.2 s |
| You shoot Kevin | In-game reflex rule | Same tick |

## Four reflex tiers

The video's "nervous system before brain", split into tiers so each thing
goes to the fastest layer that can handle it. The brain never sits in the
path of an action that a faster tier can take.

| Tier | Where | What | Speed |
| --- | --- | --- | --- |
| **0: spinal** | In the game, `kevin_bridge.c`, every tick | Rules set by the sidecar, run with no round trip: hostile after you shoot him twice within 3 s; crouch when you crouch in stealth mode; dive from a grenade within 3 units; hold-fire mask. The engine's own AI already handles cover, dodging and returning fire | Same tick |
| **1: keyword** | Sidecar, on every *partial* STT result | A small grammar of imperatives: "jump", "crouch / get down", "stand up", "hold fire", "fire / go loud", "follow me", "wait / stay", "get in", "get out", "reload", "grenade", and the corrections "stop", "cancel", "no, the other one", which cancel the current task at once. Fuzzy-matched and fired as soon as the word appears, before the sentence ends | < 5 ms after STT |
| **2: classifier** | Sidecar, reflex model (Kev-0.8B, trained on Kevin's commands) | Utterance + state + crosshair hints → one action with arguments ("stand by that rock" → `move_to` object 0x1F2A). Short cached prompt prefix, output constrained to the action schema, at most ~16 tokens | ≤ 100 ms (S0: ~45 ms) |
| **3: brain** | Sidecar, Gemma 12B | Conversation, opinions, plans, questions, refusals, mood. Sets Kevin's **policy**, which tiers 0–2 read without waiting | Streams; first sentence ≤ ~800 ms |

How this keeps the video's free will without slowing reflexes down:

- An imperative ("Kevin, jump") goes straight to tier 1 and happens. A
  question ("can you jump?") goes to the brain, which can say no.
- The brain sets a **policy** in the background: `obedient`,
  `grumpy` (complies but complains), `refusing`, or `hostile`. Tiers 0–2
  check it on every decision. So a pissed-off Kevin ignores "jump" at once,
  with no LLM call in the way.
- Tiers 1 and 2 report what they did to the brain as events, so its next
  line can comment on it ("There. Satisfied?").

### Barks: instant speech

Speech is slow to generate, so short lines are made ahead of time.

- A **bark cache**: several variants per category (`ack`, `refuse`,
  `contact`, `shields_low`, `reloading`, `grenade`, `kill`, `hurt`,
  `revive`, `vehicle_in`, `man_down`…), each rendered in normal, whisper
  and radio styles. They're generated with Breeze TTS's batch endpoint
  (many lines per request, much faster than one by one), saved to disk,
  and kept in RAM, so they play in under 50 ms.
- The brain rewrites the cache in the background when Kevin's persona
  changes (the console "you're a cat now" gets cat barks within seconds),
  or just to keep lines from repeating.
- A bark plays at once; if the brain then has more to say, its line
  follows. A bark never waits for the brain.

### Callouts from geometry

"Grunt, two o'clock", "Elite behind you", "sniper up high", "Jackal on
the ridge" are worked out from positions, with no LLM: the enemy's
bearing relative to your facing (clock direction), height difference,
distance and species. They're spoken by joining pre-rendered fragments
("Grunt" + "two o'clock"), so they play as fast as a bark. Kevin's own
view lets him call out enemies you can't see.

### Combat intensity

A meter from recent shots fired, damage, enemies in view and kills sets
how much Kevin talks: barks and callouts only in a heavy fight, longer
lines and banter when it's quiet. After a quiet stretch an `idle` trigger
lets him start a conversation himself.

### Working ahead

- When you start speaking, the sidecar fetches memories, lore and the
  level briefing for the brain before the sentence is over.
- Tier 2 runs on partial transcripts while you're still talking. Its
  result is thrown away if the final transcript changes it.
- The brain's fixed prompt prefix and the TTS model stay warm.

### Keeping the GPU from getting in the way

All the models share one RTX 3090 with the game, and a long brain
generation must not delay a reflex or make Kevin's voice stutter. S0
measured what that takes (see "Hardware budget").

- Every model stays loaded in its own server: Parakeet, Kev and Breeze in
  WSL, the brain in Ollama on Windows.
- While a tier-1/2 decision or STT chunk is pending, brain generation
  pauses between tokens (the sidecar stops pulling from the stream).
- **Background brain work waits while Kevin speaks.** Breeze makes speech
  at 0.6–0.7× real time next to the game, but at about 1.5× (too slow)
  while the brain generates nonstop. Summaries, the diary and profile
  updates run only between Kevin's lines; his own replies are short
  enough not to matter.
- **Halo is capped with vsync off** (`display.vsync = false`,
  `display.max_fps`). With vsync on at 240 Hz, Halo took enough of the
  GPU that Breeze fell to 1.3× real time. Capped at 60–240 fps, Breeze
  stays at 0.7–0.9×; the higher the cap, the less margin.
- **A playback buffer for slow lines.** When Breeze runs slower than real
  time (r×), playing a line of L seconds waits (r − 1) × L first, so it
  never stutters. The sidecar measures r from the arriving audio and
  estimates L from the text. Splitting at sentences lets the buffer refill
  in natural pauses, and an instant bark ("Ugh.", "Yeah?") covers the
  wait.
- Fixed prompt prefixes stay in the KV cache, so the classifier only
  processes the new tokens.
- Parakeet stays on the GPU: on the CPU it takes 1.5–4.4 s per pass.

## What the engine already gives us

Found in the OpenCE sources:

| Need | Existing code |
| --- | --- |
| **The per-tick input hook** | `actor_unit_control` (`source/ai/actors.c:4133`) turns the actor's output into `unit_control_data` (throttle, facing, aim, control flags, trigger) and calls `unit_control` once a tick. A hook just before that call is where tier 0 and tier 1 actions land |
| Control flags | `units.h:104`: crouch, jump, action, reload, primary/secondary trigger, throw grenade, swap weapons, flashlight |
| A localhost script console | `source/networking/telnet_console.c`, off by default (`debug.telnet_console`, port 2323). 128-char lines, one client. Fine for the M0 prototype only |
| The Spartan biped tag | `game_globals->player_information[0].player_unit`, what `player_spawn` uses (`source/game/players.c:1436`) |
| Give a unit an AI brain with no map encounter | `ai_scripting_attach_free(unit, actor_variant)` (`source/ai/ai_script.c:3513`) → `actor_create_for_unit`, which accepts any living biped (`actors.c:2523`), so a cyborg works |
| Multi-step tasks | Command lists, run by the "obey" action (`source/ai/action_obey.c`). Atoms: go_to, go_to_and_face, crouch, shoot, grenade, vehicle seat, jumps, look at, targeting on/off, wait, teleport |
| Start a command list | `ai_scripting_command_list(ai_reference, index)` (`ai_script.c:3923`) |
| Follow / attack / teams | `ai_scripting_follow_target_players`, `_follow_target_unit`, `_prefer_target`, `_set_team`, `_allegiance` |
| Iterate actors and objects | `actor_iterator_new/next` (`actors.c:1117`), `object_iterator_new` (`objects.c:302`) |
| What the player is looking at | `collision_test_vector` (physics, line 478) from the camera |
| Several views a frame | `render_frame` loops over `render_window`s, each with its own `render_camera` (`source/render/render.c:594`) |
| Mission objective text | `hud_messaging_get_objective()` |
| Damage and death | `unit_damage_aftermath` (`units.c:6955`), `unit_died` (`units.c:2743`) |
| NPC dialogue | `unit_speak` (`unit_dialogue.c:607`) |
| Play a voice in the game's mix | `voice_audio_play(speaker, seq, opus_packet, len, gain, pan)` (`port/linux/src/voice_audio.h`), used for voice chat |
| Hook typed console commands | `hs_compile_and_evaluate_command` (`source/hs/hs.c:~15400`), next to the port's `bringto` and `votekick` |
| A per-frame hook | `main.c:3242`, where `telnet_console_process()` is called |

## Design

### Game side: one new port module

`port/linux/game/kevin_bridge.c` (+ `.h`), compiled like the port's other
game-side files. Changes inside `source/` are a few one-line hooks, each
marked `port:` as the project's rules ask.

- **Settings** (`port/linux/src/port_config.c`): `kevin.enabled` (false),
  `kevin.port` (2324), `kevin.view_size` (512x288).
- **Socket:** 127.0.0.1 only, one client, non-blocking, `TCP_NODELAY`.
  Read **every tick** just before the AI update (not once a frame after
  it), so a command that arrives is applied on the next tick. Messages are
  length-prefixed: 4-byte length, 1-byte kind (JSON, Opus audio, JPEG
  image), payload.
- **Events are pushed the moment they happen**, not batched into the
  10 Hz state.

### Two ways to act

1. **Control overlay (reflexes).** The hook in `actor_unit_control` merges
   an overlay into Kevin's `unit_control_data` every tick:
   - *press*: set jump / crouch / reload / grenade / action flags for N
     ticks;
   - *hold*: keep crouch on until cleared;
   - *mask*: clear the primary trigger and grenade flags (**hold fire**,
     which settles that open question);
   - *steer*: override throttle and facing for a short dodge or a step
     toward a point;
   - *aim*: override the aiming vector (look at you, snap to a target).

   The AI keeps running underneath. When the overlay ends, it takes over
   again with no state change. This is the path for anything that has to
   feel instant.
2. **Runtime command lists (tasks).** For multi-step jobs the engine
   already does well: walk to a point with pathing, get into a gunner
   seat, go and face something. At map load the bridge copies
   `scenario->ai_command_lists` into its own array with 4 extra slots and
   points the tag block at the copy. A task writes atoms and points into a
   free slot and calls `ai_scripting_command_list`. Tasks can be slower to
   start; reflexes never use them.

Tier 0 rules run in the same hook, from state the bridge already has
(recent damage sources, your stance, nearby grenades), with parameters
the sidecar sets.

A task can change pace while it runs ("hurry up", "slow down": walk or
run) and be cancelled ("stop", "never mind") without touching what's
scheduled. While a task, a cutscene or a vehicle ride holds him, `state`
carries a **busy reason** ("holding the bridge", "in the Warthog's
gun"), so the brain knows what he's doing and doesn't offer actions that
would fight it.

### Places

At map load the bridge indexes the level's named places: trigger volumes
(designers' names such as the bridge or a hangar), cutscene flags, named
scenario objects and devices, and the encounters' areas. "Go to the
bridge", "back to the lifeboat" resolve against it by fuzzy match, as
IntelEngine indexes Skyrim's locations at load.

Spatial words resolve from the geometry and your crosshair, not names:
"up there" (the crosshair's hit point, higher than him), "behind that
rock" (the far side of the object or surface from the nearest enemies),
"take cover" (the nearest firing position or surface out of the enemies'
sight), "outside" (the nearest exit from the cluster he is in), "over
there" (the crosshair's hit point). They're answered as a world point
and surface, which the task then walks to.

### Kevin is a Spartan driven by the engine's AI

Kevin is one more of the AI's units, not a player (M0, as built:
`port/linux/game/kevin_bot.c`). With `kevin.enabled`, a campaign level
played alone on this computer gets him:

- **Made behind you** once the level has started and no cutscene plays:
  the player's biped (`game_globals->player_information[0].player_unit`,
  as `player_spawn` makes yours), in his own armour colour, with the
  scenario's first starting profile (`player_add_equipment`), on your
  trail: a spot you stood on a moment ago, so never inside rock or past a
  wall (M3).
- **Made again** there 10 seconds after he dies, and at once when a revert
  takes his unit away (a cutscene's skip reverts to a saved game; a
  checkpoint from before he was made has no Kevin in it).
- **His actor:** his own actor variant and actor definition, the same on
  every map whatever actors it has (`kevin_tuning.c`, M4), on your team:
  in the first encounter of your side on the BSP loaded (its team set to
  yours or an ally's, or its squads only human soldiers), or of none on a
  map with none. Kept active and never dormant.

The first prototype made him co-op player 2. That gave co-op's respawns
and cutscene handling for free, but split the screen, gave him a sound
listener of his own, and needed the local player count to leave him out
for every HUD and split-screen check. As an AI unit none of that applies;
what co-op did for him the bridge does instead (below, "Keeping up").

What M0 built on top of the AI, all in `kevin_bot.c` with one-line hooks
in `actors.c`, `actor_moving.c`, `actor_combat.c` and
`actor_perception.c`:

- **Following:** when the AI sends him to a firing position far from you,
  he walks to you instead, starting past 5 units and stopping inside 3;
  he runs (combat movement) and moves any way from where he faces.
- **Look and aim:** a critically damped spring toward where he means to
  look (the way he goes, up or down the slope, a glance at you or around,
  the AI's target in combat), with a turn-speed cap and his body kept
  within 1 radian of his aim, so his view never whips or flips.
- **Combat:** he prefers the enemy you aim at and those near you, is told
  of them every half second, takes his own spot ahead of you toward his
  target when the AI has no firing position near you, fires bursts more
  often and further (his weapon's reach, not the marine's range), and
  changes weapon by range (assault rifle near, pistol far), swapping
  instead of reloading when an enemy is close and reloading when none is.
- **Brought back** when he is stuck away from you or falls outside the
  level.

The actor variant sets reaction times and accuracy; see "Combat tuning".

### Combat tuning: from useless to solo-the-game

Kevin's actor variant and actor definition are his own, made in code
(`port/linux/game/kevin_tuning.c`) and added to the tag table at each
level's start as `kevin\actor variant` and `kevin\actor`, so he is the
same Spartan on every map, marines or none. His numbers began as the
campaign marine's (a soldier of the same training) and differ where he is
a Spartan: full weapon damage (a marine's is scaled to 0.6), his body's own
health and shields. The sidecar changes settings (`tuning`) at any time,
from the web UI, a preset or the brain, each worked into his tags from
that baseline, 50 (or × 1) being his own. Built in M4: accuracy, damage
dealt and taken, rate of fire, reaction, grenades, cover, aggression.
Still to come: grenade accuracy, awareness, melee, dodging.

At the ends of accuracy the AI's aim is set aside: for an AI unit the
weapon aims each round at the AI's target itself (`actor_aim_projectile`,
whatever the unit points at), so Kevin's rounds there go from his gun the
tuning's way, without the weapon's spread (hooks in `weapons.c`).


| Setting | Range | Done by |
| --- | --- | --- |
| **Accuracy** | 0 = never hits … 100 = every shot lands | Variant: `projectile_error_angle`, `target_track_fraction`, `target_lead_fraction`. At the ends his aim and rounds are the tuning's: at 0 just outside his target's bounding sphere (of the vehicle it rides in, if it rides), at 100 its head, led by his round's speed, with no spread |
| **Damage dealt** | × 0 … × 10 | A multiplier on damage whose owner is Kevin (hook in `object_cause_damage`) |
| **Damage taken** | × 0 (invulnerable) … × 10 | The same hook, when the victim is Kevin |
| **Rate of fire / bursts** | Slow and hesitant … continuous | `rate_of_fire`, burst duration and separation, firing patterns |
| **Grenade frequency** | Never … whenever there's a target | Grenade chance and check time, the unlimited-grenades flag |
| **Grenade accuracy** | Wild … lands on the target | Trajectory solving: scatter added to the AI's throw, or an exact solve at 100 |
| **Cover use** | Never takes cover … always fights from cover | `cover_dive_chance`, `cover_max_distance`, `cover_damage_threshold`, hide and emerge shield fractions, hide times |
| **Aggression** | Hangs back … charges everything | Combat range, charge and pursuit settings, retreat and panic thresholds |
| **Reaction time** | Seconds … instant | First-burst delay, surprise delay, new-target pattern time |
| **Awareness** | Short-sighted … sees everything around him | Vision distance and angles, peripheral vision |
| **Melee** | Never … leaps at anything in range | Melee range, leap chance and range |
| **Dodging** | Never … dives from every grenade | Grenade dive chance, evasion chance, tier-0 grenade dive |

Presets: **useless**, **kevin** (his own, and what he is again when the
sidecar goes), **veteran**, **master_chief**, **god** (100 accuracy,
invulnerable, × 10 damage, instant reactions, grenades without end: he can
solo the campaign). Kevin can also be told in
game ("Kevin, stop carrying me") and the brain moves the sliders within
limits you set.

### Keeping up and cutscenes

- **Stuck detection:** Kevin hasn't made progress toward his goal or
  toward you for a few seconds → he tries a jump, then a new path, then
  nudges toward the goal in shorter and shorter steps, and last gives up
  with a `stuck` event, so he can say he can't get there instead of
  standing silent (IntelEngine's stuck recovery).
- **Departure check:** after an order to go somewhere, the bridge checks
  that he actually set off, and escalates the same way when he didn't.
- **Catch-up teleport:** when he's far behind (or stuck) and neither of
  you can see the spot, he's teleported to a safe point near you
  (`teleported` event). M0 has the first part: after 5 seconds stuck more
  than 10 units from you, or at once outside the level, he is put behind
  you on your ground.
- **Scripted moves:** the levels move only the players (teleports of
  `player1`, BSP switches); the bridge brings Kevin along on the next safe
  tick. After a BSP switch his encounter may be on the old BSP and stop
  being updated, so the bridge moves him to an encounter of the new one.
- **Cutscenes:** `cinematic_start` / `cinematic_end` events. He isn't made
  while one plays, and the bridge places him beside you after it.
  He stays quiet, receives the cutscene's dialogue as context, and can
  comment once it's over.

### Fail-safe

- A hotkey in game and a button in the web UI hand Kevin straight back to
  the normal game AI: the overlay, tasks and tuning overrides are cleared.
- If the sidecar disconnects or stops answering, the bridge does the
  same, and the game carries on with Kevin fighting as a plain AI. He's
  picked up again when the sidecar reconnects.

### Two points of view

The sidecar asks for a view; the bridge answers with a JPEG plus hints.
Vision feeds tier 2 and the brain; it's never on the reflex path (tier 2
uses the hint list, which is instant).

- **Your POV:** read back the finished frame at present time (in
  `d3d8_gl.c`) with an async pixel buffer object, downscale to
  `kevin.view_size`, JPEG-encode on a worker thread.
- **Kevin's POV (built):** drawn only when a frame is asked for
  (`kevin_view_request`; `debug.kevin_view_every` asks on a timer):
  one extra `render_window` from his eyes along his aim, your field of
  view, no HUD and not his own body or weapon, into the top left quarter
  of the screen's target before your view covers it. It's read back
  through a pixel buffer object and a fence, so nothing waits for the GPU;
  the frame arrives a frame or more later. About 1 ms of the processor's
  time to draw (`port/linux/game/kevin_view.c`).
- **Labels with each image**, as CHIM's vision sends them
  (`HerikaEyes.cpp`): the objects in that exact frame, worked out on the
  game's tick when the frame is drawn and frozen with it: id, tag name,
  type, team, alive, distance, and its screen position (0–1, from the top
  left). Only objects he (or you) has a line of sight to, plus whatever is
  under the crosshair; sorted crosshair first, then nearest the centre,
  then nearest; at most 12. So "that one" or "that rock" maps to an
  object id and world point that the vision model can name.
- JPEG encoding: `stb_image_write` (public domain), vendored in
  `port/third_party/` with a README.
- Check: what per-window state `render_player_frame` assumes, and how the
  D3D8-on-GL layer switches render targets.

### Protocol (v1)

Every message carries `generation` (bumped on level load, checkpoint
revert and Kevin's death; stale commands are dropped) and timestamps for
measuring.

Game → sidecar:

- `state`, 10 Hz: game time, level, objective, your
  position/facing/stance/health/shield, crosshair hit (point + object),
  Kevin's position/facing/stance/health/shield/weapon/ammo/vehicle
  seat/alive, distance between you, nearby objects within ~30 units (id,
  tag name, type, team, position, alive).
- Events, as below ("Events and what Kevin saw"), with a `trigger`
  flag: `damaged` (by whom),
  `shields_down`, `died`, `respawned`, `kill`, `enemy_seen`,
  `grenade_near`, `player_damaged`, `player_died`, `player_shot_kevin`,
  `dialogue` (speaker + sound tag name), `checkpoint`,
  `checkpoint_revert`, `level_loaded`, `objective_changed`,
  `cinematic_start`, `cinematic_end`, `stuck`, `teleported`,
  `console` (text after `kevin`), `reflex_fired` (a tier-0
  rule acted), `action_result`, `applied` (the tick a command took
  effect, for measuring).
- While the sidecar asks to hear (`hearing`): `ptt` {down, utterance} at
  once as the push to talk key goes down and up, and between, what the
  game's microphone hears (kind 2: the utterance and frame numbers, then
  16-bit samples at 16 kHz, 20 ms a frame), starting 300 ms before the
  press so the first word isn't clipped.
- `voice` {line, playing} as Kevin's voice starts and stops (handed to the
  mixer).
- `view` replies: which POV, the JPEG, the labels.
- `snapshot`, at each checkpoint: your health, shield, weapons and ammo,
  grenades, the level and objective, deaths so far. The data behind the
  brain's description of how you're doing (the FNV bridge sends one like
  it on every save), with no image.

Sidecar → game:

- Reflexes (control overlay): `press` {flag, ticks}, `hold` {flag, on},
  `fire_mode` free / hold, `dodge` {direction}, `aim_at` {you | object |
  point}
- Tasks (command lists or AI scripting): `move_to` {point | object id},
  `follow`, `wait_here`, `attack` {target}, `grenade` {target},
  `enter_vehicle` {vehicle, seat}, `exit_vehicle`, `set_hostile` on/off
- `rules` {hostile {on, hits, seconds, fire_seconds}, grenade_dive {on,
  distance}, stealth, hold_fire_until_engaged}: tier 0's parameters, each
  member optional and all checked before any is taken; `set_hostile` {on}
  ends (or starts) his firing back; `test` {stimulus} with
  `debug.kevin_bridge_test`, for the scripted check; `policy`: obedient / grumpy /
  refusing / hostile
- `tuning` {any combat tuning setting}, `preset` {name}
- `teleport_near_player`, `release` (hand Kevin back to the game AI)
- `request_view` {player | kevin}
- `script` {line}: a HaloScript line from the action book (enter a
  vehicle, an AI command list, look at, crouch), so a new action needs no
  C. Only functions on the bridge's allowed list are run, and only from
  the local socket: whatever another process sends is untrusted.
- `urgent` {event types}: which events are sent at once (below)
- Kevin's voice (kind 3: his line's number, then 16-bit samples at
  48 kHz; plain samples, since the socket is local), up to 10 s queued, a
  new line cutting off the last; `voice_stop` (stop at once: barge-in, or
  "shut up")
- `hearing` {on}: the game's microphone open, for push to talk
- `subtitle` {text, duration}: Kevin's line on screen
- `gesture` {name}: a gesture animation while he talks

### Events and what Kevin saw

From the chasm FNV bridge's event log, cut down to one witness:

- Each event has an id, a type, a one-line summary, game time, level, the
  objects involved, and **`kevin_in_range`**, **`kevin_saw`** (his facing
  and a line-of-sight ray at the time) and **`kevin_did`** (he caused it,
  so it isn't something he "saw").
- Events go out in batches, every 2.5 seconds or when a batch is full. The
  types the sidecar marks `urgent` (shots at him, a grenade near, you
  shooting him, his death) go out at once.
- A burst (twenty shots, a long firefight) becomes one aggregate: the
  first event goes at once, the rest are merged into one with
  `memory_only`, so a reaction fires once.
- `checkpoint` and `checkpoint_revert` carry the checkpoint's id, so the
  sidecar rolls its stores back to the right one. The game doesn't wait
  for the sidecar (Halo saves checkpoints too often for that).
- Every command names its target by object id. The bridge checks that the
  id is still that object, alive, and in range before acting, and answers
  with `action_result` either way: a sent command isn't proof it happened.

### Voice and hearing

- **Kevin's voice:** barks and brain lines go out as 48 kHz samples;
  the bridge plays them through voice chat's mixer (`voice_audio.c`) with
  gain and pan from
  Kevin's position relative to the camera. As the FNV bridge plays its
  NPCs' lines: one stream per line, silence where a frame is late rather
  than a stutter, frames of an old line dropped, and a line that never
  finishes cleared after a timeout.
- **Can Kevin hear you?** As CHIM's spatial hearing: within a set
  distance and in sight he hears you directly; further, or round a wall,
  over the radio. That decides whether his reply is spoken in the room or
  in the radio style, and the radio voice plays at its own volume, not
  from his position.
- **Whisper and shout:** speak quietly (the mic level, or while you're
  crouched) and Kevin whispers back and keeps his lines short; shout and
  he raises his voice. As CHIM's conversation modes.
- **Looking at him:** your crosshair resting on Kevin for a moment sends a
  `looked_at` event (CHIM's gaze dwell), so he can react, and an
  utterance while you look at him is for him.
- **Push-to-talk by default:** the game's own push-to-talk binding
  (`controls.push_to_talk`, V) sends `ptt_down` / `ptt_up` the moment
  it's pressed, so the sidecar opens the mic with no delay. **Open mic**
  is an option: voice activity detection starts and ends speech, and a
  line counts as meant for Kevin if it uses his name, your crosshair is
  on him, or you were just talking to him.
- **Recognition:** Parakeet on a sliding window, re-run every ~120 ms
  while you talk, so tier 1 sees partial words. Parakeet isn't a
  streaming model, but at half precision a pass takes 70–110 ms on the
  3090 next to everything else (S0), for any window up to 4 s. Whether
  partials are stable enough is checked in the reflex slice (M2).
- **Echo cancellation** on the mic input, so speakers don't feed Kevin's
  voice and the game back into STT.
- **Barge-in:** if you start talking while Kevin speaks, he stops
  (`voice_cut`) and listens.
- **Game dialogue** (Cortana, Keyes, marines): the `dialogue` event
  carries the line's text from a table made once offline, keyed by sound
  tag and permutation, so it's exact and instant with no audio capture.
  The maps have no subtitles: `sound_permutation.subtitle_data`
  (`source/sound/sound_definitions.h:91`) is empty in every campaign map,
  and the Xbox engine has no code that shows subtitles (S0). The table is
  made by running Parakeet over every dialogue sound. About 180 story
  lines also exist as text in the level scripts' string constants
  (`scenario.hs_string_constants`, Bungie's debug prints, lowercase, with
  speaker labels such as `cortana:`); where a script plays a sound next to
  one, that text replaces Parakeet's, and the scripts' names (Foehammer,
  Keyes) correct its spelling. The table is made from your own copy of
  the game and stays local, out of git.
- **Kevin looks at you while he talks** (aim overlay on your head), with
  a gesture now and then.
- **Subtitles:** Kevin's lines appear on screen (`subtitle`), and the web
  UI offers the same feed as a page you can add to OBS.

### Console

`kevin <text>` typed in the game console is caught in
`hs_compile_and_evaluate_command` and sent as a `console` event. The
sidecar puts it into Kevin's context as a thought.

`say <text>` in the console, or the chat box in the web UI, talks to
Kevin by text instead of the mic.

## Measuring

Reactivity is the requirement, so it's measured from day one.

- Timestamps on every stage: speech start/end (VAD), each STT partial,
  tier decision, message sent, message received in game, tick applied
  (`applied` event).
- The sidecar logs a per-stage breakdown for every action and keeps
  rolling p50 / p95 numbers against the budget table.
- A debug overlay in game (setting `kevin.debug_overlay`) shows the last
  action, its tier and its total latency.
- Every milestone from M2 on lists the latency it has to hit.

### Record, replay and a fake game

- **Recording:** the sidecar can record a session: every message from the
  game, your mic audio and the timestamps.
- **Replay:** a recording is played back into the sidecar with the same
  timing, so reflexes, prompts and latency can be tuned without playing.
  A set of recordings doubles as regression tests ("Kevin, jump" still
  fires under 250 ms; "stop" still cancels).
- **Fake game:** a small Python program that speaks the bridge protocol
  with a simple simulated world, so the sidecar and web UI can be built
  and tested before the bridge exists or with the game closed.

## Pointers taken from CHIM / HerikaServer

CHIM (Skyrim AI followers): a thin game plugin (`CHIM/Plugin`) and a
server that owns models, prompts and memory (`HerikaServer`). What Kevin
borrows, single-character only:

| CHIM idea | For Kevin |
| --- | --- |
| Thin game client; the server owns models, prompts, memory | `kevin_bridge.c` stays thin; the sidecar owns the rest |
| Events logged with game time; only some start an AI turn; combat barks are context | The `trigger` flag; tiers decide what reaches the brain |
| Action catalog with validation | The command list above; the bridge rejects invalid ones |
| Action results fed back (`funcret`) | `action_result` and `reflex_fired` events |
| Stale replies dropped (`interaction_generation`) | `generation` |
| Image-to-text with hints | The two views with object hints |
| Memory per playthrough; evolving profile | Memory keyed by save; Kevin's opinion of you updated over time |
| Lore knowledge base | One briefing file per level, plus a short lore file |
| Voice filter presets | Normal / whisper / radio styles |

CHIM's game plugin (`Dwemer-Dynamics/CHIM`, GPL-3.0: read as
documentation only, never copied) adds:

| CHIM plugin idea | For Kevin |
| --- | --- |
| Vision with the actors in the frame, their screen positions and ids (`HerikaEyes.cpp`) | The labels sent with each view |
| Spatial hearing: distance, line of sight, indoor and outdoor ranges (`SpatialAwareness`) | Direct speech or radio |
| Conversation modes: whisper, close, shout | Whisper and shout |
| Gaze dwell: the crosshair resting on an actor | `looked_at` |
| Actions target the live reference id, checked before acting | Object ids, checked, with `action_result` |
| Requests tagged with the load they belong to; work from an older load is dropped | `generation` |
| A successful reply isn't proof the right actor acted | `action_result` for every command |

What CHIM doesn't have, and Kevin needs: a reflex path. In CHIM, every
turn goes through the server and an LLM. That's fine for Skyrim
conversations; in a firefight it would be far too slow.

## Pointers taken from SkyrimNet and its plugins

SkyrimNet (`MinLL/SkyrimNet-GamePlugin`, the public files of a closed
backend that runs inside Skyrim's process), SeverActions
(`Severause/SeverActions`) and IntelEngine
(`galanx/IntelEngine-GamePlugin`), plugins built on it. None has a
licence, so they're read as documentation only.

| Idea | For Kevin |
| --- | --- |
| Action eligibility worked out while you're still speaking | The bridge keeps the `state` fields eligibility needs current, so tier 2 never waits on the game |
| A busy reason that other systems respect | `state`'s busy reason |
| Named places indexed at load; spatial words resolved from geometry (IntelEngine) | "Places" |
| Stuck recovery that escalates and ends by saying so; departure checks (IntelEngine) | "Keeping up" |
| Pace changes and cancelling mid-task (IntelEngine) | Tasks |
| An MCP server over the game's data and state | The sidecar's MCP server (SIDECAR_PLAN.md), with the bridge's `state` and object queries behind it |

## Pointers taken from chasm and its FNV bridge

chasm (`chasmlol/chasm`, AGPL-3.0) is the video's creator's backend for
AI NPCs, and Chasm Bridge FNV (`chasmlol/chasm-bridge-fnv`, no licence)
its Fallout: New Vegas game side. Both are read as documentation only;
nothing is copied. Neither has a reflex path or anything for Halo.

| Idea | For Kevin |
| --- | --- |
| Events with who witnessed them, and who was in range but didn't see | `kevin_in_range`, `kevin_saw`, `kevin_did` |
| Events batched; trigger types flushed at once; bursts aggregated, the first firing alone | Batches, `urgent`, `memory_only` aggregates |
| Save events with ids, so the backend's stores roll back | `checkpoint` / `checkpoint_revert` with ids |
| A heartbeat with the game's state every ~100 ms | The `state` message |
| Streamed speech: one buffer per line, silence-filled, stale chunks dropped, stuck replies recovered | Kevin's voice playback |
| Actions as script bodies from an action book, run by the game | `script`, limited to allowed functions |
| A player snapshot at each save, as data, not a picture | `snapshot` |
| The backend's model stack started when the game connects and stopped when it exits | The sidecar's process manager (SIDECAR_PLAN.md) |

The rest (witness memory, habits, warm-up, the action book's gates,
scenario variants) is the sidecar's; see SIDECAR_PLAN.md.

## Milestones

| # | Result | Check |
| --- | --- | --- |
| P0 | Fork OpenCE on GitHub; game-side changes on a `kevin` branch; `kevin/` tracked. **Done** | Upstream merges cleanly into the branch |
| S0 | **Model check:** download and verify every model and its licence; time each alone and all together with Halo running; check whether the maps' dialogue sounds have subtitle data. **Done:** see "Hardware budget" | A table of load size, latency and VRAM per model; a go / no-go for each |
| M0 | **Done.** Kevin as an AI-driven Spartan (`kevin_bot.c`): made behind you, made again after he dies, following, look and aim, combat and weapon choice, brought back when stuck; his view drawn off screen on request (`kevin_view.c`). (A first version made him co-op player 2; see "Kevin is a Spartan") | Kevin, armed, follows you through the a30 opening while you play, gets into the Warthog's gun seat, comes back after dying; frames of his view arrive with no cost to yours |
| M1 | **Done.** `kevin_bridge.c`: settings, socket read every tick, framing, `state`, events with `kevin_in_range` / `kevin_saw` / `kevin_did`, batches and `urgent` types, checkpoint ids, the control overlay hook, `applied` timestamps and `action_result`, the fail-safe; the fake game and recorder | Python sends `press jump`; `applied` comes back on the next tick (≤ 33 ms in game); killing the sidecar hands Kevin back to the AI (met: `applied` on the next tick, p50 16 ms, p95 32 ms over 20 tries at 60 fps; silence and a closed socket both hand him back; `kevin/tools/m1_bridge_test.py`) |
| M2 | **Done.** **Reflex vertical slice:** push-to-talk from the game key → sliding-window Parakeet → tier-1 keywords including stop / cancel → overlay; bark cache with `ack` lines; latency logging; replay tests | "Kevin, jump" ≤ 250 ms p95, bark ≤ 300 ms, over 50 tries (met: in the game at 60 fps, jump p50 62 ms, p95 149 ms after speech ends; the bark the same; 20 of 20 commands right, questions and "don't" left alone; `kevin/tools/m2_reflex_test.py`, speech from Windows' voices given to the hearing, so the microphone's own delay of about a tick isn't counted) |
| M3 | **Done.** Kevin across level loads, BSP switches and checkpoint reverts (made again or moved to the new BSP's encounter), generations, tier-0 rules (hostile, grenade dive, stealth crouch, hold fire) | Each rule fires the same tick in a scripted test (met: `kevin/tools/m3_rules_test.py` in the game, each `reflex_fired` on its cause's tick, none with the rules off; he fires back and crouches in `state`, and runs from grenades, seen in play. Across a30's BSP switch both ways, `kevin/tools/m3_bsp_test.py`: he joined an encounter of the new BSP and was beside you after it) |
| M4 | **Done.** **Combat tuning:** his own variant and actor definition on every map, eight settings, damage multipliers, the aim extremes, presets | At 0 accuracy Kevin lands no hits in a long fight; at 100 every shot lands; God mode solos the a30 opening (`kevin/tools/m4_tuning_test.py`, played on d20, which has no marines: at 100, 60 hits of 60 shots; at 0, none on his target and 5 of 60 on grunts beside it; god mode, no deaths through a30's opening and d20's first fights) |
| M5 | Tasks via runtime command lists: `move_to`, `follow`, `wait_here`, `attack`, `enter_vehicle`; tier-2 classifier with crosshair hints, trained first on Kevin's commands (the training part of SIDECAR_PLAN.md S14, brought forward: untrained Kev picked the right action about half the time from real speech in S0) | "Stand by that rock" moving ≤ 400 ms p95; 95% of a held-out set of spoken commands get the right action |
| M6 | Keeping up: stuck detection, catch-up teleport, scripted moves, cutscene events | Kevin is beside you after every cutscene and Pelican drop through a30 and a50 |
| M7 | The rest of the events: dialogue text from the offline table (Parakeet, corrected by the scripts' lines), objective, checkpoints, level load | Cortana's lines arrive as exact text through the a30 opening |
| M8 | Callouts from geometry and combat intensity | Clock directions match the screen; Kevin stays brief in fights |
| M9 | Your POV: readback, JPEG, labels; HaloScript `script` actions with the allowed list; `snapshot` | JPEGs match the screen; no frame-time spike; a label's object id is the object at that spot |
| M10 | Kevin's POV: the frame is drawn and read back (done); JPEG, downscaling and labels | JPEGs from Kevin's eyes with labels; frame-time cost measured |
| M11 | Voice playback: streamed, positional, radio by hearing, whisper and shout, barge-in, echo cancellation, open-mic mode, `looked_at`, look-at-you and gestures, subtitles | Walk away from Kevin; interrupt him mid-line; subtitles match his lines |
| M12 | `kevin <text>` and `say <text>` console commands | Typed lines reach the brain |
| M13 | Brain and character systems (step 3, SIDECAR_PLAN.md): policy, conversation, profile, memory, diary, lore, web UI | Talking to Kevin in game; first audio ≤ 1.2 s |

## Open questions

- **His encounter after a BSP switch** (M3): he's moved to the first
  encounter of the new BSP with squads, when his is on the old one (seen
  on a30). Levels with many BSPs (a10) still to be played through.
- **Where he is put:** only on your trail, where you stood (M3). a30's
  start has ground behind you inside a rock that the game's collision tests
  find no wall in, so the level's ground is no proof of room; he isn't made
  until you've walked a little.
- **Shooting Kevin:** whether the AI turns on you after friendly fire, as
  marines do, before tier 0's own betrayal rule; tier 0 should decide.
- **Weapon spread at 100 accuracy:** where the weapon's own error is
  applied to a shot, so it can be zeroed for Kevin alone (M4).
- **Parakeet partials:** answered in M2: stable enough for tier 1 on the
  test set, with passes back to back (heard to decided p50 86 ms, p95
  110 ms). The one partial that misled was "don't jump" heard as "don't
  shoot", so a phrase that is a negation waits for the final transcript.
  To check with real voices and a noisy room.
- **Reflex model accuracy:** Kev-0.8B is fast enough (S0) but untrained;
  how many of Kevin's own examples it needs to be reliable (M5).
- **Memory in later levels:** the loadout leaves about 1 GB below the
  point where Windows pages GPU memory and Halo freezes, measured on a30.
  Check the heaviest levels; if it's short, run Kev and Parakeet in one
  process or trim Kev's buffers further.
- **Overlay vs AI:** does the AI fight the overlay (e.g. re-crouching or
  firing the tick after the mask ends)? The overlay may need to hold for
  a few ticks or set the actor's own state too. (M1)
- **Misses at accuracy 0:** aimed to the side of his target, a round can
  still hit the one beside it (5 of 60 on d20). Over its head may do
  better.
- **Patching the command-list block:** check nothing else holds the old
  address or validates the count (tag validator, saved games, BSP
  switches).
- **Extra render window:** side effects on your view and its cost.
- **Checkpoint reverts and saves:** Kevin's unit can vanish in a revert;
  M0 makes him again at once. What the sidecar keeps across a revert is
  its rollback setting (SIDECAR_PLAN.md).

## Hardware budget (RTX 3090, 24 GB)

Measured in S0 (`kevin/tools/s0/`), each model's share of the card next
to Halo on a30, and its speed with everything loaded:

| Model | Setup | GPU memory | Speed | Verdict |
| --- | --- | --- | --- | --- |
| Windows and Halo | a30, 2560×1440, capped | 1.4 GB | | |
| Breeze TTS 2 | `--fast-backbone-decode --fast-depth-decoder --fast-codec`, guidance 4 | 8.8 GB | First sound 0.2 s, 0.6–0.7× real time | **Go**, live |
| Gemma 4 12B QAT Q4_0 | Ollama, `num_ctx` 4096, thinking off for live replies | 7.9 GB | First word 0.2–0.4 s (1.3 s for a new prompt), 67–75 tokens/s | **Go** |
| Kev-0.8B | `run_kev.sh one`: CUDA graphs sized for one player (`kev_one_player.py`) | 2.9 GB | ~45 ms per decision | **Go**, needs training |
| Parakeet TDT 0.6B v2 | Half precision, resident (`parakeet_serve.py`) | 1.6 GB | 70–110 ms per pass | **Go** |
| **All together** | | **22.4–22.8 GB** | | 10-minute soak: no errors, memory flat, every line faster than real time |

Windows starts paging GPU memory at about 23.7 GB used, and the game then
freezes, so the total has to stay well under that.

Choices that were measured and set aside:

- **Kev-4B**: 10 GB and no more accurate than Kev-0.8B untrained (15 of
  20 commands each).
- **Kev-0.8B in its stock modes**: mid 4.2 GB at ~40 ms; lean 2.0 GB but
  ~150 ms (no CUDA graphs).
- **Breeze's full fast path** (`--fast-all`, 14.4 GB): leaves no room for
  the brain. Adding the text-encoder and prefill graphs ran out of memory.
  Breeze's eager mode (7.6 GB) makes speech 3.6× slower than real time.
  Cloning a reference voice adds about 3 GB of graphs.
- **Gemma 4 E4B and E2B**: 4.3 and ~2 GB and quicker, but terse; they
  rarely used Kevin's memories.
- **Gemma with thinking on**: spends its whole reply budget thinking.
  Thinking stays on only for background work (diary, summaries,
  relationship reviews), which isn't in a hurry.
