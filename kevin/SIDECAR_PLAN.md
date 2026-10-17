# Step 3: the sidecar, Kevin's character systems and web UI

The sidecar is the Python process that is Kevin's mind: speech, reflexes,
brain, memory, diary, profile, lore and a web UI to edit all of it. The
game side and protocol are in BRIDGE_PLAN.md.

Modelled on CHIM's server (HerikaServer), cut down to one character, and
local only.

## Rule one: nothing here slows down reflexes

Reflex tiers 0–2 (BRIDGE_PLAN.md) read only two things, both in memory
and both updated in the background:

- the current **policy** (obedient / grumpy / refusing / hostile), and
- the **bark cache**.

Profile, memory, diary and lore feed the **brain** (tier 3) only. Their
work runs between brain turns or in a background worker, never while a
reflex is waiting. Brain prompt pieces that change rarely (profile, level
briefing) are built once and kept in the model's KV cache.

## What we take from CHIM

| CHIM system | What it does there | Kevin's version |
| --- | --- | --- |
| NPC profile (`core_npc_master`) | Fields: prompt head, static bio, personality, relationships, occupation, appearance, skills, speech style, goals, emote moods, voice id, knowledge tags, lock flag | **Kevin's profile**, the same fields plus Kevin-only ones (below). Edited in the UI |
| Dynamic profile (`dynamic_profile_scheduler.php`) | Periodically rewrites chosen sections from recent events. Each section has its own action (personality, relationships, goals, skills, speech style…). It's gated by interval, minimum events and cooldown, and respects the lock | **Evolving Kevin:** his opinion of you, relationships, goals and running jokes update from what happens. Versioned with undo, and lockable per section |
| Memory (`memory_helper_vectordb.php`, `memory_ranking.php`) | Summaries of past events, retrieved by hybrid ranking: vector distance minus keyword rank, ties broken by recency | Same idea in SQLite: FTS5 for keywords, `sqlite-vec` for vectors, one combined score |
| Compacted history (`compact_context_history.php`) | Older dialogue squeezed into summaries to keep the prompt short | Rolling summary of the session; the last ~2 minutes kept verbatim |
| Diary (`diarylog.php`, `diary_book.php`, diary audio) | The character writes in-character diary entries from recent history. The UI shows them as a book, can read them aloud, and they're recalled as memory | **Kevin's diary**: entries at level end, after deaths and big fights, or when you ask. Book view in the UI, read aloud in his voice, indexed as memories |
| Oghma lore (`oghma_retrieval.php`, aliases, context rules, knowledge tags) | Lore article catalog. Matches entities in speech with aliases plus phonetic and edit-distance fuzzing, so STT mistakes still hit. Context rules force articles in, and knowledge tags limit what a character knows | **Halo lore** catalog with the same matching. It matters here: the video's STT already turned "Cortana" into "Cotana". Per-level briefings are forced in by level |
| Prompts manager, function editor | Edit prompt templates and the action catalog in the UI | Same: prompt templates and the action catalog's descriptions and enable flags |
| Connectors (LLM / STT / TTS / ITT pages, with tests) | Pick providers and test them | Model endpoints for brain, reflex, STT, TTS and vision, each with a test button |
| Logs, response queue, audit | See what was sent and returned | Logs plus **latency** per tier, the thing Kevin cares about most |
| Playthrough manager | Memory tied to saves, with rollback | Simpler: memory per playthrough, snapshot at each checkpoint (see "Saves") |

Left out: multiple NPCs, group conversations, the narrator, biography
CSV import, PostgreSQL, Apache/PHP, and the in-game settings hub. One
Python process with SQLite is enough for one character on one PC.

## Kevin's profile

Stored as a versioned record, one active at a time, with **presets**
("Kevin", "Cat Kevin", "German Kevin", "Fabulous Kevin") you can switch
between from the UI or the console.

| Field | Example | Used by |
| --- | --- | --- |
| Identity / prompt head | "You are Kevin, a Spartan-II…" | Brain |
| Static bio | Backstory that never changes | Brain |
| Personality | Foul-mouthed, sarcastic, loyal underneath | Brain, bark generation |
| Speech style | Short lines, swears, insults you affectionately | Brain, bark generation |
| Relationship with you | Your name, how he sees you (dynamic) | Brain |
| Goals | Survive, find survivors, outshine you (dynamic) | Brain |
| Skills | Weapons and vehicles he's good at | Brain, tier 2 |
| Appearance | Armour colour, voice description | Brain, bridge (armour colour) |
| Moods | Allowed moods and how each changes his voice | Brain, TTS style |
| Voice | TTS voice id, speed, default style | TTS |
| Knowledge tags | Which lore he knows (a Spartan knows the UNSC, not Forerunner secrets) | Lore retrieval |
| **Compliance** | Default policy, how easily he gets grumpy or refuses | Policy, tiers 0–2 |
| **Combat style** | Aggressive / cautious; fire discipline | Actor variant tuning, tier 0 rules |
| **Tier 0 rules** | Betrayal threshold, grenade dive, stealth mirroring | Bridge `rules` message |
| **Barks** | Lines per category, or "generate from personality" | Bark cache |
| **Profanity / content limits** | How much he swears; topics he won't go into | Brain |
| Lock flags | Per section: stop the dynamic profile changing it | Dynamic profile |

Editing a field takes effect on the next brain turn. Personality, speech
style or bark changes also queue a bark cache rebuild.

On "uncensored": the video used an uncensored model so Kevin would swear
and fight back. The plan is to get that from the profile and the system
prompt (profanity allowed, in-game violence allowed, he can turn on you)
with the normal Gemma model first. Switch models only if that falls short.
The content-limits field stays, so Kevin doesn't hand out real-world
harmful instructions just because he's in character.

## Memory, history and diary

- **Event log:** every game event, line of dialogue (yours and his),
  action and reflex, with game time, level and generation.
- **Working context:** the last ~2 minutes verbatim, plus a rolling
  summary of the rest of the session.
- **Memories:** a background worker turns closed segments (a fight, a
  conversation, a checkpoint-to-checkpoint stretch) into short first-person
  memory entries. They're stored with an embedding and FTS text and
  retrieved by hybrid score at brain time (top 5, recency-weighted).
- **Pinned memories:** ones you mark in the UI are always in context.
- **Diary:** written by the brain model in Kevin's voice at level end,
  after he dies, after big fights, or when you ask. Each entry is stored,
  shown in a book view, readable aloud, and indexed as a memory.
- **Embeddings:** a small local embedding model on the CPU, to keep the
  GPU free for reflexes.

## Long-term memory: remembering you across playthroughs

The basics above cover what happened recently. These are what make Kevin
remember you weeks later: who you are, what you did together, who you
fought, what you talked about.

### Taken from CHIM

| CHIM feature | How it works there | Kevin's version |
| --- | --- | --- |
| **Relationship system** (`ext/relationship_system`) | Affinity −100…100 per person, group or concept, with a type (platonic, professional…). Mapped to tiers: Hostile, Resentful, Cold, Wary, Neutral, Warm, Fond, Attached, Devoted. A small separate LLM reviews each turn and changes scores only on meaningful moments. The main model sees only the tier label, which keeps prompts short | **Kevin's relationships:** with you first, then recurring characters (Cortana, Keyes, Johnson), groups (marines, Grunts, Elites, the Flood) and concepts (Warthog driving). Reviewed by the reflex-sized model between turns, never during combat reflexes. The brain sees "Chasm: Fond (squadmate), down from Attached after the friendly fire on b30". Feeds the policy: Cold or worse makes him grumpier and slower to obey |
| **Relationship log** (`relationship_logs.php`) | Every change, with the reason | Same. Kevin can say *why* he's annoyed with you |
| **Memory categories** (`storeMemory(..., $category)`) | Summaries stored by kind | Categories: conversation, battle, death, vehicle, promise, joke, opinion, about-you |
| **Profile history** (`core_npc_master_history`) | Every profile version kept | Already in the plan (versions with undo). Kevin can also look back: "You used to be more fun" |
| **Dynamic lore** (`processor/dynamicoghma.php`) | Lore entries are created or updated as quest stages advance | **Story-gated knowledge:** Kevin learns about the Flood when you meet them, not before. No spoilers, and his knowledge grows with the campaign |
| **Quest log** (`questlog`) | Quest progress per playthrough | **Mission log:** per level, objectives met, how it went, how long it took, how many times you died |
| **Adventure log** (`ui/addons/adventurelog`) | A readable timeline over the event log | **Campaign log** page: the playthrough as a timeline you can scroll and search |
| **Soul Gaze gallery** (`soulgaze_gallery.php`) | Saved images from the vision action, with descriptions | **Kevin's album:** views he found memorable (first sight of the ring, a big explosion, your best kill) kept with his description, and recalled as visual memories |
| **Playthrough stats** (`playthrough_stats.php`) | Numbers per playthrough | **Service record:** see below |
| **Global vs playthrough data** (`lib/playthrough_policy.php`) | Each table is marked playthrough, global or mixed, so some things carry across playthroughs and others don't | Same split. *Global:* who you are, your relationship, facts about you, past playthrough summaries. *Playthrough:* events, missions, album, stats. So a new campaign starts fresh, but Kevin still knows you |

### Halo-specific additions

- **Service record:** kills by species and weapon, deaths and what
  killed each of you, times you saved each other, friendly fire both
  ways, vehicles wrecked, headshots, betrayals. Built straight from
  events with no LLM, so it's exact. Kevin can brag or hold a grudge with
  real numbers ("That's four times you've run me over").
- **Nemeses:** an enemy that kills Kevin or you more than once, or a
  memorable one (the Elite with the energy sword on a30), gets a record:
  where, what it did, whether it's dead. Kevin calls it out if it shows up
  again, and remembers when you finally got it.
- **Facts about you:** things you tell him ("my name's…", "I hate the
  Library", "I always take the sniper") extracted after conversations and
  kept as editable facts. Global, so they carry into the next playthrough.
- **Promises and running jokes:** "you owe me one", nicknames, bits that
  land. Kept until they're resolved or go stale, so he brings them back
  at the right time.
- **Past playthroughs:** when a playthrough ends, it's condensed into a
  one-page summary that stays global. On a replay Kevin can say "Last time
  you drove us off this bridge."
- **Session openers:** at the start of each session the brain gets the
  last diary entry, open promises and the relationship tier, so his first
  line picks up where you left off.

### Keeping it useful as it grows

CHIM retrieves memories but doesn't say much about pruning them. Kevin
needs to stay fast and on point after dozens of hours, so we add:

- **Importance:** each memory gets a 1–5 score when it's written. Firsts,
  deaths, betrayals and promises score high; routine fights score low.
  Retrieval weights importance alongside relevance and recency.
- **Consolidation:** at session end, a background pass merges duplicates
  ("we fought Grunts again"), compresses old low-importance memories into
  per-level summaries, and keeps high-importance ones word for word.
- **Recall reinforcement:** memories that get recalled and used gain
  weight; ones never recalled fade (but are never deleted unless you
  delete them).
- **Budget:** the brain gets a fixed share of the prompt for long-term
  context: relationship tier, top facts, open promises, nemesis if
  present, and the top memories. The memory system has to fit inside it.

## Lore and level briefings

- **Lore articles:** short entries on species, factions, characters,
  weapons, vehicles, places. Each has a canonical name, aliases including
  likely STT mishearings, tags, and knowledge tags. Write them ourselves
  or with Gemma's help. Don't paste wiki text: Halopedia is CC BY-SA, so
  anything imported from it needs attribution and the same licence.
- **Matching:** exact name → alias → phonetic key → bounded edit distance
  on the transcript's word windows, as Oghma does. Top 2–3 articles go
  into the prompt.
- **Level briefings:** one per campaign level: setting, objective in
  plain words, allies, enemies, what came before. Forced into context by
  level name, and updated when `objective_changed` arrives.

## Saves and playthroughs

Halo CE has one checkpoint slot per profile plus save-and-quit, simpler
than Skyrim.

- A **playthrough** is a named memory set. Start one fresh, or continue
  one.
- At each `checkpoint` the sidecar snapshots its world facts (where
  you are, what's done).
- On `checkpoint_revert`, world facts roll back to that snapshot, but
  Kevin keeps his memories of what happened ("you got us killed").
- An option in the UI turns full rollback on instead.

## Web UI

Served by the sidecar on 127.0.0.1 only. FastAPI on the back end; plain
HTML with htmx on the front, so there's no build step. A websocket feeds
the live pages.

| Page | What's on it |
| --- | --- |
| **Live** | Game state, Kevin's mood and policy, the live transcript (you, Kevin, game dialogue), recent events, the last few actions with their tier and latency, a chat box to talk to him by text, a box to inject a thought (like the `kevin` console command), an emergency "shut up", and **Release Kevin** (hand him back to the game AI) |
| **Combat** | Every combat tuning slider (accuracy, damage dealt and taken, rate of fire, grenade frequency and accuracy, cover, aggression, reaction time, awareness, melee, dodging), the presets from Useless to God mode, and the limits within which the brain may change them |
| **Subtitles** | Kevin's lines as a clean page to add to OBS |
| **Profile** | Every profile field, presets (switch / duplicate / export / import), version history with diff and undo, lock toggles, dynamic profile settings and "run now" |
| **Barks** | Categories and lines, play each in normal / whisper / radio, regenerate from personality, cache status |
| **Memories** | Search (same hybrid ranking the brain uses), edit, pin, delete; a "why was this recalled" view for each brain turn |
| **Diary** | Book view, read aloud, write an entry now, edit or delete |
| **Relationships** | Kevin's affinity with you and others, tier, type, the change log with reasons; edit scores by hand |
| **About you** | Facts Kevin knows about you, promises and running jokes; edit, add or delete |
| **Campaign log** | The playthrough as a searchable timeline: missions, fights, deaths, conversations, album shots |
| **Service record** | Stats and nemeses, per playthrough and all-time |
| **Album** | Kevin's saved views with his descriptions |
| **Lore** | Articles, aliases, knowledge tags, level briefings; a test box: type a sentence, see which articles match and why |
| **Prompts** | Brain, diary, memory summary, dynamic profile, vision and bark-generation templates, with a preview of the fully built prompt |
| **Actions** | The action catalog: descriptions the brain sees, enable flags, tier-1 keyword phrases and tier-2 examples |
| **Models** | Endpoints and settings for brain, reflex, STT, TTS, vision and embeddings, each with a test button and measured speed |
| **Voice** | Push-to-talk (default) or open mic, mic device, echo cancellation, Kevin's volume and radio distance |
| **Latency** | p50 / p95 per tier and per stage against the budget, a timeline of recent actions, GPU memory and load; mark any action as a misfire or misunderstanding (see "Teaching Kevin") |
| **Recordings** | Recorded sessions; replay one into the sidecar; run the replay regression set |
| **Tests** | The character test set: scenarios, last results, run now |
| **Logs** | Brain requests and responses, vision requests with their images, errors |
| **Playthroughs** | List, start, rename, export / import, rollback setting |

## Teaching Kevin

- On the **Latency** or **Logs** page, mark a reflex that misfired, a
  command he misunderstood or an action he shouldn't have taken, and give
  the right answer.
- Each correction becomes an example: tier-1 phrases are added to the
  keyword grammar, and tier-2 examples go into the classifier's prompt.
- Once there are enough, they're used (with Gemma-labelled synthetic
  examples) to train a small classifier of our own for tier 2.

## Character tests

- A fixed set of scenarios, each a short event history and a prompt:
  "you shot him", "he just died", "quiet moment on b30", "you ask him to
  jump while he's Cold toward you", "he meets the Flood".
- Each is run through the brain after every prompt or profile change and
  checked: in character, consistent with his memories and relationship
  tier, the right policy and actions, within the latency budget.
- Checked by rules where possible (policy, actions, length) and by Gemma
  as a judge for tone.

## Layout and stack

```
kevin/
  sidecar/
    bridge/      socket client, framing, generation
    speech/      VAD, Parakeet, Breeze TTS, Opus, bark cache
    reflex/      tier 1 keywords, tier 2 classifier, policy
    brain/       prompt assembly, Gemma client, action calls
    vision/      view requests, image + hints → description
    memory/      event log, summaries, hybrid search, diary
    lore/        catalog, matching, briefings
    profile/     profile, presets, versions, dynamic profile
    combat/      tuning settings, presets, brain limits
    callouts/    clock directions, fragments, combat intensity
    web/         FastAPI app, templates, static
    latency/     timestamps, rolling stats
    replay/      recorder, replayer, regression set
    fakegame/    simulated world speaking the bridge protocol
    tests/       character test scenarios and runner
  tools/
    dialogue_transcripts.py   offline table of dialogue text (if subtitles are missing)
    model_check.py            S0: load and time every model
  data/          kevin.db (SQLite), presets, lore, briefings
```

Python 3.12 (installed). SQLite with FTS5 and `sqlite-vec`. The brain is
served by llama.cpp or Ollama (installed). STT and the reflex model run
in-process (or in a second process if the GPU scheduling needs it), with
TTS in its own worker.

## Milestones

These follow bridge milestone M2 (the reflex vertical slice), which builds
the first parts of `speech/` and `reflex/`.

| # | Result | Check |
| --- | --- | --- |
| S1 | Sidecar skeleton, SQLite schema, event log, web UI shell with the **Live** and **Latency** pages | Live page shows game state and events from the bridge |
| S2 | **Profile** page with fields, presets and versions; brain prompt built from it; **Prompts** page with preview | Change personality in the UI; the next reply reflects it |
| S3 | Brain turn: Gemma streaming, sentence-level TTS, action calls through the catalog, `action_result` fed back; policy updates | Conversation in game; first audio ≤ 1.2 s; "can you jump?" can be refused |
| S4 | **Barks** page and generated bark cache; rebuild on persona change | Console "you're a cat" → cat barks within ~10 s |
| S5 | Memory: summaries worker, hybrid search, pinned memories, **Memories** page | Kevin brings up something from an earlier level unprompted when it's relevant |
| S6 | **Diary**: automatic and on-request entries, book view, read aloud | An entry after a30 that matches what happened |
| S7 | **Lore** catalog, fuzzy matching, level briefings, **Lore** page with test box | "What's Cotana saying?" pulls in the Cortana article |
| S8 | Dynamic profile with locks and undo | Shoot near Kevin a lot; his opinion of you visibly sours |
| S9 | **Actions**, **Models**, **Logs**, **Playthroughs** pages; checkpoint snapshot and revert | Revert a checkpoint: world facts roll back, memories stay |
| S10 | Relationships with tiers, change log and policy link; service record and nemeses; **Relationships** and **Service record** pages | Run Kevin over twice; the tier drops, the log says why, and he mentions the count |
| S11 | Facts about you, promises and jokes, story-gated lore, mission log, **About you** and **Campaign log** pages | Tell him your name and a preference; he still uses both in a new playthrough |
| S12 | Album, past-playthrough summaries, session openers, importance scoring, consolidation, recall reinforcement | After a long playthrough, recall stays on point and the prompt stays within its budget |
| S13 | **Combat**, **Voice**, **Subtitles** and **Recordings** pages | Every slider changes Kevin in game within a second |
| S14 | Teaching Kevin: misfire marking, keyword and example updates, then a trained tier-2 classifier | A corrected command works the next time |
| S15 | Character test set and **Tests** page | A deliberately bad prompt edit fails the set |
