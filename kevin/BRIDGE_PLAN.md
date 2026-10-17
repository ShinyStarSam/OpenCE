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
| **2: classifier** | Sidecar, reflex model (Kev-4B, or our own small classifier) | Utterance + state + crosshair hints → one action with arguments ("stand by that rock" → `move_to` object 0x1F2A). Short cached prompt prefix, output constrained to the action schema, at most ~16 tokens | ≤ 100 ms |
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
  and radio styles. They're generated with Breeze TTS at startup and kept
  in RAM, so they play in under 50 ms.
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

All the models share one RTX 3090, and a long brain generation must not
delay a reflex.

- STT and the reflex model run in their own process and stay loaded. The
  brain runs in a separate llama.cpp/Ollama server.
- While a tier-1/2 decision or STT chunk is pending, brain generation
  pauses between tokens (the sidecar stops pulling from the stream).
- Fixed prompt prefixes stay in the KV cache, so the classifier only
  processes the new tokens.
- If contention still shows up in measurements, move STT to the CPU or
  quantize the reflex model further before anything else.

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

### Kevin is a Spartan

- Spawn: `object_create` the player's own biped tag
  (`player_information[0].player_unit`) beside you, give him the assault
  rifle and pistol, then `ai_attach_free` with a marine actor variant from
  the current map. Same team as you.
- He has the Spartan's shields, health, speed and jump. To tell you apart,
  use a different armour colour (the multiplayer colour change on the
  cyborg's shader), once that's checked.
- Respawn after death near you, out of sight (as co-op does), and again
  after level loads and checkpoint reverts.
- Risk: the cyborg's animation graph was made for a player, not an AI.
  Behaviours that ask for missing animations (dives, signals, surprise)
  may do nothing or look wrong. M0 checks this first. If it's bad, turn
  those behaviours off in the variant or map them to the animations the
  cyborg has.
- The actor variant sets reaction times and accuracy; see "Combat
  tuning".

### Combat tuning: from useless to solo-the-game

At map load the bridge copies the chosen marine actor variant (and its
actor definition) into its own memory and points Kevin's actor at the
copy. The sidecar can then change any of these at any time, from the web
UI, a preset or the brain:

| Setting | Range | Done by |
| --- | --- | --- |
| **Accuracy** | 0 = never hits … 100 = every shot lands | Variant: `projectile_error_angle`, `error_angle_modifier`, `target_track_fraction`, `target_lead_fraction`. At the ends, the aim overlay takes over: at 0 it aims just outside the target's bounding sphere every shot; at 100 it aims at the head marker with full lead, and the weapon's own spread is zeroed for Kevin's shots |
| **Damage dealt** | × 0 … × 10 | `weapon_damage_modifier`, plus a multiplier on damage whose owner is Kevin (hook in `object_cause_damage`) |
| **Damage taken** | × 0 (invulnerable) … × 10 | Multiplier in `unit_damage_aftermath` when the victim is Kevin |
| **Rate of fire / bursts** | Slow and hesitant … continuous | `rate_of_fire`, burst duration and separation, firing patterns |
| **Grenade frequency** | Never … whenever there's a target | Grenade chance and check time, the unlimited-grenades flag |
| **Grenade accuracy** | Wild … lands on the target | Trajectory solving: scatter added to the AI's throw, or an exact solve at 100 |
| **Cover use** | Never takes cover … always fights from cover | `cover_dive_chance`, `cover_max_distance`, `cover_damage_threshold`, hide and emerge shield fractions, hide times |
| **Aggression** | Hangs back … charges everything | Combat range, charge and pursuit settings, retreat and panic thresholds |
| **Reaction time** | Seconds … instant | First-burst delay, surprise delay, new-target pattern time |
| **Awareness** | Short-sighted … sees everything around him | Vision distance and angles, peripheral vision |
| **Melee** | Never … leaps at anything in range | Melee range, leap chance and range |
| **Dodging** | Never … dives from every grenade | Grenade dive chance, evasion chance, tier-0 grenade dive |

Presets: **Useless**, **Marine**, **Spartan**, **Master Chief**,
**God mode** (100 accuracy, invulnerable, × 10 damage, instant reactions,
perfect grenades: he can solo the campaign). Kevin can also be told in
game ("Kevin, stop carrying me") and the brain moves the sliders within
limits you set.

### Keeping up and cutscenes

- **Stuck detection:** Kevin hasn't made progress toward his goal or
  toward you for a few seconds → he tries a jump, then a new path, then a
  `stuck` event.
- **Catch-up teleport:** when he's far behind (or stuck) and neither of
  you can see the spot, he's teleported to a safe point near you
  (`teleported` event), as co-op respawns do.
- **Scripted moves:** level scripts that teleport you, drop you from a
  Pelican or change BSP bring Kevin along on the next safe tick.
- **Cutscenes:** `cinematic_start` / `cinematic_end` events. Kevin is
  hidden or frozen during the cutscene and placed beside you afterwards.
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
- **Kevin's POV:** after the player's window in `render_frame`, render one
  extra `render_window` with Kevin's head as the camera (his camera
  position and aiming vector, your FOV) into an offscreen target at
  `kevin.view_size`. No HUD or first-person weapon. Only drawn when asked,
  at most about once a second.
- **Hints with each image:** the objects in that view (id, tag name, type,
  team, distance, screen position), so "that rock" maps to an object id
  and world point.
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
- Events, pushed at once, with a `trigger` flag: `damaged` (by whom),
  `shields_down`, `died`, `respawned`, `kill`, `enemy_seen`,
  `grenade_near`, `player_damaged`, `player_died`, `player_shot_kevin`,
  `dialogue` (speaker + sound tag name), `checkpoint`,
  `checkpoint_revert`, `level_loaded`, `objective_changed`,
  `cinematic_start`, `cinematic_end`, `stuck`, `teleported`, `ptt_down`,
  `ptt_up`, `console` (text after `kevin`), `reflex_fired` (a tier-0
  rule acted), `action_result`, `applied` (the tick a command took
  effect, for measuring).
- `view` replies: which POV, the JPEG, the hints.

Sidecar → game:

- Reflexes (control overlay): `press` {flag, ticks}, `hold` {flag, on},
  `fire_mode` free / hold, `dodge` {direction}, `aim_at` {you | object |
  point}
- Tasks (command lists or AI scripting): `move_to` {point | object id},
  `follow`, `wait_here`, `attack` {target}, `grenade` {target},
  `enter_vehicle` {vehicle, seat}, `exit_vehicle`, `set_hostile` on/off
- `rules`: tier-0 rule parameters; `policy`: obedient / grumpy /
  refusing / hostile
- `tuning` {any combat tuning setting}, `preset` {name}
- `teleport_near_player`, `release` (hand Kevin back to the game AI)
- `request_view` {player | kevin}
- `voice` (20 ms Opus frames at 48 kHz), `voice_end`, `voice_cut` (stop
  at once, e.g. when you shout "shut up")
- `subtitle` {text, duration}: Kevin's line on screen
- `gesture` {name}: a gesture animation while he talks

### Voice and hearing

- **Kevin's voice:** barks and brain lines go out as Opus frames; the
  bridge plays them through `voice_audio_play` with gain and pan from
  Kevin's position relative to the camera. Past a set distance the
  sidecar uses the radio style.
- **Push-to-talk by default:** the game's own push-to-talk binding
  (`controls.push_to_talk`, V) sends `ptt_down` / `ptt_up` the moment
  it's pressed, so the sidecar opens the mic with no delay. **Open mic**
  is an option: voice activity detection starts and ends speech, and a
  line counts as meant for Kevin if it uses his name, your crosshair is
  on him, or you were just talking to him.
- **Recognition:** Parakeet on a sliding window, re-run every ~120 ms
  while you talk, so tier 1 sees partial words. Parakeet isn't a
  streaming model, but it's fast enough on the 3090 to run this way;
  check that in the reflex slice (M2).
- **Echo cancellation** on the mic input, so speakers don't feed Kevin's
  voice and the game back into STT.
- **Barge-in:** if you start talking while Kevin speaks, he stops
  (`voice_cut`) and listens.
- **Game dialogue** (Cortana, Keyes, marines): the `dialogue` event
  carries the line's text, read from the sound's subtitle data
  (`sound_permutation.subtitle_data`, `source/sound/sound_definitions.h:91`).
  If a map's lines have no subtitle data, the text comes from a table made
  once offline by running Parakeet over every dialogue sound in the maps.
  Either way it's exact and instant, with no audio capture.
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

What CHIM doesn't have, and Kevin needs: a reflex path. In CHIM, every
turn goes through the server and an LLM. That's fine for Skyrim
conversations; in a firefight it would be far too slow.

## Milestones

| # | Result | Check |
| --- | --- | --- |
| P0 | Fork OpenCE on GitHub; game-side changes on a `kevin` branch; `kevin/` tracked | Upstream merges cleanly into the branch |
| S0 | **Model check:** download and verify every model and its licence; time each alone and all together with Halo running; check whether the maps' dialogue sounds have subtitle data | A table of load size, latency and VRAM per model; a go / no-go for each |
| M0 | Prototype over the telnet console, no C changes: spawn a Spartan Kevin on a30, follow, the map's own command lists, enter a vehicle. Settles the animation risk | Manual, on a30 |
| M1 | `kevin_bridge.c`: settings, socket read every tick, framing, `state` and pushed events, the control overlay hook, `applied` timestamps, the fail-safe; the fake game and recorder | Python sends `press jump`; `applied` comes back on the next tick (≤ 33 ms in game); killing the sidecar hands Kevin back to the AI |
| M2 | **Reflex vertical slice:** push-to-talk from the game key → sliding-window Parakeet → tier-1 keywords including stop / cancel → overlay; bark cache with `ack` lines; latency logging; replay tests | "Kevin, jump" ≤ 250 ms p95, bark ≤ 300 ms, over 50 tries |
| M3 | Spawn / respawn as a Spartan, generations, tier-0 rules (hostile, grenade dive, stealth crouch, hold fire) | Each rule fires the same tick in a scripted test |
| M4 | **Combat tuning:** private variant copy, every setting, damage multipliers, the aim overlay extremes, presets | At 0 accuracy Kevin lands no hits in a long fight; at 100 every shot lands; God mode solos the a30 opening |
| M5 | Tasks via runtime command lists: `move_to`, `follow`, `wait_here`, `attack`, `enter_vehicle`; tier-2 classifier with crosshair hints | "Stand by that rock" moving ≤ 400 ms p95 |
| M6 | Keeping up: stuck detection, catch-up teleport, scripted moves, cutscene events | Kevin is beside you after every cutscene and Pelican drop through a30 and a50 |
| M7 | The rest of the events: dialogue text from subtitles or the offline table, objective, checkpoints, level load | Cortana's lines arrive as exact text through the a30 opening |
| M8 | Callouts from geometry and combat intensity | Clock directions match the screen; Kevin stays brief in fights |
| M9 | Your POV: readback, JPEG, hints | JPEGs match the screen; no frame-time spike |
| M10 | Kevin's POV: extra render window | JPEGs from Kevin's eyes; frame-time cost measured |
| M11 | Voice playback: positional, radio, barge-in, echo cancellation, open-mic mode, look-at-you and gestures, subtitles | Walk away from Kevin; interrupt him mid-line; subtitles match his lines |
| M12 | `kevin <text>` and `say <text>` console commands | Typed lines reach the brain |
| M13 | Brain and character systems (step 3, SIDECAR_PLAN.md): policy, conversation, profile, memory, diary, lore, web UI | Talking to Kevin in game; first audio ≤ 1.2 s |

## Open questions

- **Spartan animations under AI** (M0).
- **Subtitle data in the maps:** whether the Xbox maps fill
  `subtitle_data` for dialogue, or the offline table is needed (S0).
- **Weapon spread at 100 accuracy:** where the weapon's own error is
  applied to a shot, so it can be zeroed for Kevin alone (M4).
- **Parakeet on a sliding window:** is it fast and stable enough for
  partials, or does tier 1 need a dedicated streaming model or a
  keyword-spotting model on raw audio? (M2)
- **Reflex model:** whether Kev-4B (unverified; the link is from the
  video description) hits ≤ 100 ms, or a small classifier trained on
  Gemma-labelled examples does better. (M4)
- **Overlay vs AI:** does the AI fight the overlay (e.g. re-crouching or
  firing the tick after the mask ends)? The overlay may need to hold for
  a few ticks or set the actor's own state too. (M1)
- **Actor variant per map:** which marine variant each level has; pick one
  at map load.
- **Patching the command-list block:** check nothing else holds the old
  address or validates the count (tag validator, saved games, BSP
  switches).
- **Extra render window:** side effects on your view and its cost.
- **Checkpoint reverts and saves:** Kevin's unit index may change or
  vanish; the bridge has to find or respawn him.

## Hardware budget (RTX 3090, 24 GB)

Rough figures, to check once the model files are in hand: Gemma 12B at
Q4 ~8 GB (it also handles the views), the reflex model ~3–4 GB (less if
it's a small classifier), Parakeet ~2 GB, the TTS model a few GB. It
should fit beside the game. Ollama and LM Studio are already installed and
can serve the brain.
