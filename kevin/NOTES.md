# Kevin: how the video's AI companion works

Source: chasm, "Giving Video Game NPC's Consciousness" (youtube.com/watch?v=EOfm3h4xYU8).
Notes are a summary of what the video shows and says; the video gives no code.

## Stack (from the video description; check each link before relying on it)

| Role | Model |
| --- | --- |
| Game | OpenCE, the Halo CE Xbox decompilation ported to PC (this repo) |
| Reflexes ("nervous system") | `jaredpalmer/kev-4b`, a small fast classifier model |
| Thinking ("brain") | `google/gemma-4-12B`, run locally, uncensored variant |
| Speech to text | `nvidia/parakeet-tdt-0.6b-v2` |
| Text to speech | `BreezeBlue/Breeze-TTS-2` (expressive: can whisper, shout, accents) |

Everything runs locally on one PC. An RTX 3090 (24 GB) should fit all four with a quantized 12B.

## Behaviour shown

- **Two layers.** Every event (the player speaks, Kevin is shot, an enemy appears)
  goes to the reflex model first. It acts at once for simple things (jump, crouch,
  stand, follow, hold fire, fire) and decides whether the event needs the LLM.
  Commands like "Kevin, jump" happen with no LLM call, so they're near instant.
- **Free will.** The brain can refuse an order ("can you jump?" gets a sarcastic
  no), then do it when told directly.
- **Eyes / spatial commands.** "Stand by this rock", "go near those bodies":
  Kevin works out world coordinates of what you're referring to and moves there.
- **Combat orders.** Hold fire until told, then engage; wait here; cover me;
  get in the vehicle (he takes the gunner seat of a Warthog).
- **Hears the game.** In-game voice lines (marines, Cortana) are fed into his
  audio input, so he reacts to what NPCs say.
- **Mission awareness.** Knows the current level objective and recent events,
  including his own death and respawn.
- **Radio.** When he's far from the player his voice switches to a radio effect.
- **Mood and voice control.** "Whisper" makes the TTS whisper; he stays in character.
- **Console injection.** Typing `kevin <text>` in the game console inserts a
  thought straight into his context ("you are a cat now", accents, personas).
- **Can turn hostile.** If the player threatens him he may decide to fire back.
- **Personality.** Foul-mouthed, sarcastic Spartan. The creator says a censored
  model wouldn't swear or fight back, so he used an uncensored one. A strong
  system prompt allowing profanity and in-game violence may be enough instead.

## Pieces to build

1. **Game bridge (C, in OpenCE).** A local socket in the game that streams state
   (player and Kevin position, nearby objects and enemies, damage, deaths,
   mission objective, dialogue being played) and accepts commands for one
   allied AI actor (move to, follow, jump, crouch, fire / hold fire, target,
   enter vehicle seat, play voice line). Plus the `kevin` console command.
2. **Sidecar (Python).** Mic + game audio capture → Parakeet STT → reflex
   classifier → (if needed) Gemma with memory and mission context → actions
   back to the game, speech to Breeze TTS → played in game, positional or
   radio-filtered.
3. **Perception.** Resolve "that rock" to coordinates: the game's object list
   and a ray from the player's view first; a vision pass on a screenshot later.
