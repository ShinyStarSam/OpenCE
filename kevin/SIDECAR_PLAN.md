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
reflex is waiting, and never while Kevin is speaking: in S0 nonstop brain
generation slowed Breeze from 0.7× to 1.5× real time, which stutters.
Brain prompt pieces that change rarely (profile, level briefing) are
built once and kept in the model's KV cache.

Live replies run with Gemma's thinking off; with it on, a short reply's
whole token budget goes to thinking. Background work (summaries, diary,
relationship reviews, dynamic profile) can think, since nothing waits on
it.

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

## What we take from chasm

chasm (`chasmlol/chasm`, AGPL-3.0) is the video's creator's own backend
for AI NPCs: brain, cloned voices, speech recognition, lore, actions and
memory, for any game that writes a bridge for it. It's read as
documentation only; nothing is copied. It has no reflex path: every turn
is push-to-talk, then the LLM.

| chasm system | What it does there | Kevin's version |
| --- | --- | --- |
| Witness memory (`witness.rs`) | Each event lists who was in range to see it; it becomes a narration line in each witness's history, batched, and written before that character's next turn | Kevin's history gets only what he saw (`kevin_saw`) or did. What happened out of his sight reaches him only if you tell him or he hears it on the radio. Bundled lines keep the history append-only, which keeps the brain's KV cache warm |
| Journals and skills (`journal.rs`, `skill_creator.rs`, `skill_executor.rs`) | After a save, each character writes a journal entry. A curator with no persona reads the entries and starts, changes or stops **habits**: one real game event plus the character's first-person intention, only for a settled, repeated intention. A habit fires without the LLM when its owner witnessed the event, with a cooldown, and plants the intention as a private impulse for a normal turn | **Kevin's habits**, from his diary: "you always charge in with the Warthog; I'll be on the gun before you ask". The curator's output is limited to real event types; firing checks `kevin_saw` and a cooldown. A habit can also set a tier-0 rule parameter, so a learned reflex runs in the game with no round trip |
| Save-aware stores (`save_sync.rs`) | Every store is copied at each save, keyed by the save, and restored on load. A store that didn't exist yet saves as empty, so a load also clears what came after | The same for every store, keyed by the checkpoint id the bridge sends (see "Saves and playthroughs") |
| Name correction after STT (`stt_vocab_boost.py`) | Parakeet can't be biased to a word list, so near-miss proper nouns in the transcript are snapped to the character and lore names: whole words only, phonetic key plus edit distance, high thresholds, and never onto an ordinary English word | Run on every transcript and partial before tier 1, so "Cotana" is "Cortana" for the keywords as well as the brain. The word list is the lore catalog's names and aliases, rebuilt when it changes |
| Warm-up on connect (`warmup.rs`) | A thrown-away TTS line (CUDA graph capture), a silent STT clip, a one-token LLM call with the real first prompt (fills the KV cache), the reranker's first pass | The same when the game connects, in the background, so Kevin's first line is as fast as the rest |
| Stack lifecycle (`stack_lifecycle.rs`) | The models start when the game's heartbeat appears and stop when it goes | The sidecar's process manager starts Parakeet, Kev, Breeze and the brain with the game and stops them after it |
| Action book (`action_book_injection.rs`) | Each action has keys, a meaning-match text, scopes and an execution body. Each turn offers only the actions whose constant, keyword or vector gate opens; the model's output is restricted to them | Tier 2 and the brain see only the actions that fit the moment, so prompts stay short and the grammar tight. Execution is a bridge command or a HaloScript line (`script`) |
| Scenario variants and macros (`scenario_variants.rs`, `macros.rs`) | The scene text in the prompt changes with game state (following, waiting, in combat); `{{macros}}` fill in live values | Variants for following, holding, in a vehicle, in combat, stealth; macros such as `{{objective}}`, `{{level}}`, `{{your_health}}`, `{{kevin_weapon}}` |
| Event reactions (`witness.rs` triggers) | Chosen event types make a witness speak unprompted, each type with its own chance and cooldown; reactions never stack or interrupt a reply in flight | Combat intensity's quiet-time talk, and the `trigger` flag, with a chance and cooldown per event type |
| Sentence streaming (`/api/game/v1/turn`) | Text and audio stream sentence by sentence; each subtitle is sent just before its audio | Kevin's lines, and the `subtitle` message ahead of each sentence's audio |
| Gamemaster pass | A persona-less pass after play updates how each character feels about the others | The relationship review (above), run as its own persona-less pass |
| Request tracing (`request_trace.rs`) | Every turn's requests and responses kept for a trace page | The **Logs** and **Latency** pages |

Left out: picking which of several NPCs speaks, schedules and travel,
music, companion faces, and the profile-bundle import.

## What we take from SkyrimNet and its plugins

SkyrimNet (`MinLL/SkyrimNet-GamePlugin`) is a closed AI-NPC backend that
runs inside Skyrim's process; SeverActions and IntelEngine are plugins
built on it. None has a licence, so they're read as documentation only.

| Idea | Where | Kevin's version |
| --- | --- | --- |
| A model per job: a fast one picks actions, a stronger one talks | SkyrimNet | Already the tiers; the background passes (diary, habits, reviews) get their own model setting too |
| Actions as data: a description, typed parameters, eligibility rules (AND / OR of state checks), a priority, a cooldown, and an **event string** written to history when it fires | SkyrimNet | The action book's entries. The event string ("Kevin took the Warthog's gun") goes into his history, so the brain knows what he did |
| Nested action categories: the model picks an intent, then a cheaper prompt picks the action | SkyrimNet | Tier 2 picks among a few categories (move, fight, vehicle, stance, talk); only that category's actions go to the second step |
| Descriptions that say when to use an action, and when not, naming the one to use instead ("not for telling the player to wait; use ResumeFollowing") | SeverActions | How every action book entry is written |
| An action offered when it can't be used gets called anyway, so it must never be offered | SeverActions | Eligibility checks his state (in a vehicle, in a cutscene, already holding) before anything reaches a model |
| Triggers as data: when an event matches field conditions, do one of: a private thought, a narration line, a silent event in context, a diary entry, a profile update, a voice effect; with chance, cooldown, priority and an interrupt flag | SkyrimNet | Kevin's triggers, as files edited on a **Triggers** page with schema checks. They cover the reactions, habits' firing and combat-intensity talk |
| World knowledge scoped by conditions (a quest stage, a location), injected always or by meaning | SkyrimNet | Story-gated lore and level briefings as conditions on level, objective and script globals ("the Flood" from the moment you meet them on 343 Guilty Spark) |
| Voice effects as recipes of audio stages (pitch, EQ, distortion, reverb), previewable | SkyrimNet | The radio and the helmet sound as an effect on any line, not a separate TTS rendering: one bark cache instead of three; whisper stays a TTS style |
| Private thoughts that shape later lines without being heard | SkyrimNet | The `kevin` console command and trigger thoughts go in as thoughts |
| Earlier lines editable and deletable in the chat history | SkyrimNet | Fix a misheard transcript on the **Live** page, and the history he reads is corrected |
| An MCP server for AI assistants: query game data and state, render a prompt, check an action's eligibility, validate files | SkyrimNet | The sidecar runs one, so an assistant (Claude Code) can inspect Kevin, render his prompt and test actions while you play |
| Vision descriptions cached by the image's hash; a description can be locked | SkyrimNet | Kevin's album and view descriptions |
| Models pinned by checksum when downloaded | SkyrimNet | The process manager checks each model's checksum before starting it |
| Background LLM work in **lanes**: one request in flight per kind, each with a time limit, a minimum gap between calls, cleared on a revert, and a late reply on an old lane dropped | SeverActions | Diary, habits, relationship reviews, profile updates and bark rebuilds each run in a lane, so a stuck request never blocks the next |
| "How they see you" reassessed at milestones (the first real conversation, every hundred lines after, a change in the bond), at most once a game day | SeverActions | Kevin's opinion of you is rewritten at milestones (a level's end, a betrayal, a save), not on a timer |
| A history of banter topics, so they don't repeat | SeverActions | His idle talk keeps one |
| Bio blocks: personality snippets kept in a library and applied to a character, grouped in tabs, where some tabs allow one block at a time | SeverActions | Profile presets as blocks: accent, species, mood, voice. "You're a cat now" applies the cat block and removes it again; one accent at a time |
| Task awareness: the current task and its state, and past tasks with natural times ("just now", "earlier today") | IntelEngine | His prompt says what he's doing ("holding the bridge for you"), how he can stop, and what he did recently, with times like "a minute ago" |
| A safety net after each conversation: a short check finds commitments ("I'll take the gun when we reach the Warthog") the reply didn't act on | IntelEngine | **Promises** become tasks with a condition (a place, an objective, an event) and fire when it's met |
| Facts that expire, with natural time references | IntelEngine | Facts about the moment ("the Elite with the sword is still around") fade; facts about you don't |

Left out: in-process hosting (Kevin's models need the GPU in WSL),
scheduled meetings at game times, gossip chains, faction politics, the
story Dungeon Master, and NPC-to-NPC scenes.

### chasm itself as Kevin's brain (an option for M13)

chasm runs as a separate program, so the sidecar could call it instead of
building the brain: the sidecar keeps streaming STT, tiers 0–2, barks and
the game socket, and a tier-3 turn becomes a call to chasm's
`/api/game/v1/turn`. Its licence doesn't reach a program that only talks
to it over HTTP. Against it, for now:

- it is experimental, and its interfaces may change;
- its speech is its own (Qwen3-TTS, Parakeet), not Breeze;
- its event log and witness memory take events only as the FNV bridge's
  files (`control/gameevents/`), not over HTTP, so Kevin would have to
  write that layout or chasm grow an endpoint for it.

Decide at M13 against what the sidecar has by then.

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
- **What he saw:** only events with `kevin_saw` or `kevin_did` go into
  his history, bundled into one narration line when they go quiet or the
  bundle gets big, and always before his next turn (chasm's witness
  memory). The rest stay in the event log, for the campaign log and the
  service record.
- **Working context:** the last ~2 minutes verbatim, plus a rolling
  summary of the rest of the session.
- **Memories:** a background worker turns closed segments (a fight, a
  conversation, a checkpoint-to-checkpoint stretch) into short first-person
  memory entries. They're stored with an embedding and FTS text and
  retrieved by hybrid score at brain time (top 5, recency-weighted).
- **Pinned memories:** ones you mark in the UI are always in context.
- **Diary:** written by the brain model in Kevin's voice at level end,
  after he dies, after big fights, or when you ask. Each entry is stored,
  shown in a book view, readable aloud, and indexed as a memory. Append
  only: an entry is never rewritten.
- **Habits:** after each diary entry, a curator pass with no persona reads
  the new entries and starts, changes or stops a habit only when an
  intention is clearly settled and repeated (chasm's skills, above). A
  habit fires when he saw its event, with a cooldown, as a private
  impulse for his next turn, or sets a tier-0 rule. The **Habits** page
  shows them with the entries they came from.
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
- **Name correction:** before anything reads a transcript, near-miss
  names in it are snapped to the catalog's names and aliases, behind high
  thresholds and a guard that never changes an ordinary English word
  (chasm's STT vocabulary correction).
- **Level briefings:** one per campaign level: setting, objective in
  plain words, allies, enemies, what came before. Forced into context by
  level name, and updated when `objective_changed` arrives.

## Saves and playthroughs

Halo CE has one checkpoint slot per profile plus save-and-quit, simpler
than Skyrim.

- A **playthrough** is a named memory set. Start one fresh, or continue
  one.
- At each `checkpoint` (with its id) the sidecar copies every store:
  world facts, memories, diary, habits, relationships, promises. A store
  that didn't exist yet is saved as empty, so a rollback also clears what
  came after (as chasm's save sync does).
- On `checkpoint_revert`, world facts roll back to that checkpoint's
  copy, but by default Kevin keeps his memories of what happened ("you
  got us killed").
- An option in the UI turns full rollback on instead: every store goes
  back, and a habit he formed in a discarded stretch is gone.

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
| **Habits** | Each habit with its trigger, intention, cooldown and the diary entries it came from; turn on or off, edit, run the curator now |
| **Triggers** | Trigger files with schema checks: event conditions, the response, chance, cooldown, priority; test one against a recorded event |
| **Voice effects** | The radio, helmet and other recipes, with a preview on any line |
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
  examples) to train Kev-0.8B for tier 2.
- The first training comes early, with bridge milestone M5: untrained
  Kev-0.8B picked the right action for about half of the spoken commands
  in S0. A starting set is written by hand and with Gemma, spoken through
  Breeze in a few voices and transcribed by Parakeet, so it has Parakeet's
  real mistakes in it ("Ghost stand by that rock").

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
    brain/       prompt assembly, scenario variants, macros, Gemma client, action book
    vision/      view requests, image + hints → description
    memory/      event log, what he saw, summaries, hybrid search, diary, habits
    lore/        catalog, matching, briefings
    profile/     profile, presets, versions, dynamic profile
    combat/      tuning settings, presets, brain limits
    callouts/    clock directions, fragments, combat intensity
    web/         FastAPI app, templates, static
    latency/     timestamps, rolling stats
    stack/       process manager (start with the game, stop after), warm-up, checksums
    lanes/       background LLM work, one request in flight per kind
    triggers/    trigger files, matching, responses
    mcp/         MCP server for AI assistants
    replay/      recorder, replayer, regression set
    fakegame/    simulated world speaking the bridge protocol
    tests/       character test scenarios and runner
  tools/
    dialogue_transcripts.py   offline table of dialogue text (Parakeet, corrected by the scripts' lines)
    s0/                       S0: model setup, timing, samples, soak test
    wsl_setup.sh              the models' Python environments in WSL
  data/          kevin.db (SQLite), presets, lore, briefings
```

Python 3.12 (installed). SQLite with FTS5 and `sqlite-vec`. The brain is
served by Ollama on Windows. Parakeet, Kev and Breeze each run as a
resident server in WSL (Ubuntu, `kevin/tools/wsl_setup.sh`); the sidecar
talks to all four over local HTTP. The S0 scripts that start them
(`run_breeze.sh`, `run_kev.sh one`, `parakeet_serve.py`) are where the
sidecar's process manager starts from. If memory gets short, Kev and
Parakeet can share one process.

## Milestones

These follow bridge milestone M2 (the reflex vertical slice), which builds
the first parts of `speech/` and `reflex/`.

| # | Result | Check |
| --- | --- | --- |
| S1 | Sidecar skeleton, SQLite schema, event log, web UI shell with the **Live** and **Latency** pages; the process manager and warm-up | Live page shows game state and events from the bridge; the models start with the game, and the first line is as fast as the rest |
| S2 | **Profile** page with fields, presets and versions; brain prompt built from it; **Prompts** page with preview | Change personality in the UI; the next reply reflects it |
| S3 | Brain turn: Gemma streaming, sentence-level TTS, action calls through the catalog, `action_result` fed back; policy updates | Conversation in game; first audio ≤ 1.2 s; "can you jump?" can be refused |
| S4 | **Barks** page and generated bark cache; rebuild on persona change | Console "you're a cat" → cat barks within ~10 s |
| S5 | Memory: summaries worker, hybrid search, pinned memories, **Memories** page | Kevin brings up something from an earlier level unprompted when it's relevant |
| S6 | **Diary**: automatic and on-request entries, book view, read aloud; **habits** from it, with the curator and the **Habits** page | An entry after a30 that matches what happened; a habit formed from repeated behaviour fires when he sees its event |
| S7 | **Lore** catalog, fuzzy matching, name correction of transcripts, level briefings, **Lore** page with test box | "What's Cotana saying?" pulls in the Cortana article, and the keyword tier sees "Cortana" |
| S8 | Dynamic profile with locks and undo | Shoot near Kevin a lot; his opinion of you visibly sours |
| S9 | **Actions** (the action book with its gates), **Models**, **Logs**, **Playthroughs** pages; every store copied at each checkpoint and rolled back on revert | Revert a checkpoint: world facts roll back, memories stay; with full rollback on, a habit formed after it is gone |
| S10 | Relationships with tiers, change log and policy link; service record and nemeses; **Relationships** and **Service record** pages | Run Kevin over twice; the tier drops, the log says why, and he mentions the count |
| S11 | Facts about you, promises and jokes, story-gated lore, mission log, **About you** and **Campaign log** pages | Tell him your name and a preference; he still uses both in a new playthrough |
| S12 | Album, past-playthrough summaries, session openers, importance scoring, consolidation, recall reinforcement | After a long playthrough, recall stays on point and the prompt stays within its budget |
| S13 | **Combat**, **Voice**, **Subtitles** and **Recordings** pages | Every slider changes Kevin in game within a second |
| S14 | Teaching Kevin: misfire marking, keyword and example updates, retraining tier 2 from corrections (its first training comes earlier, with bridge M5) | A corrected command works the next time |
| S15 | Character test set and **Tests** page | A deliberately bad prompt edit fails the set |
