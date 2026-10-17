"""S0: builds out/samples/kevin_loadouts.html from the samples and the
combined runs, for comparing configurations by ear and by eye.

  python make_page.py
"""

import html
import json
import os
import re
import statistics

HERE = os.path.dirname(os.path.abspath(__file__))
SAMPLES = os.path.join(HERE, "out", "samples")
GB = 1024


def load(name):
	path = os.path.join(SAMPLES, name)
	return json.load(open(path)) if os.path.exists(path) else None


def combined_total(name):
	"""Peak GPU memory seen in a combined run's log, in MiB, or None."""
	path = os.path.join(SAMPLES, f"combined_{name}.log")
	if not os.path.exists(path):
		return None
	text = open(path, encoding="utf-8", errors="replace").read()
	values = [int(v) for v in re.findall(r"(\d+) MiB", text)]
	values += [int(v) for v in re.findall(r'"used_mib": (\d+)', text)]
	return max(values) if values else None


def e(text):
	return html.escape(str(text))


# What each part takes on the card, in MiB, as measured (see S0 results)
PARTS = {
	"system": ("Windows and Halo (a30, 60 fps)", 1400),
	"breeze": ("Breeze, live", 8842),
	"gemma12": ("Gemma 4 12B", 7925),
	"gemmae4": ("Gemma 4 E4B", 4331),
	"kevlean": ("Kev-0.8B, lean", 2012),
	"kevmid": ("Kev-0.8B, mid", 4133),
	"parakeet": ("Parakeet, half", 1634),
}
COLOR = {"system": "sys", "breeze": "voice", "gemma12": "brain", "gemmae4": "brain", "kevlean": "kev", "kevmid": "kev",
	"parakeet": "ear"}

LOADOUTS = [
	{
		"id": "full", "name": "Full Kevin", "run": "max-brain",
		"parts": ["system", "breeze", "gemma12", "kevlean", "parakeet"],
		"voice": "Breeze, live: first sound 0.22 s, 0.7× real time",
		"brain": "Gemma 4 12B: first word 0.3 s (1.3 s for a new prompt)",
		"kev": "Kev-0.8B lean: 150–170 ms",
		"good": ["Breeze speaks every line live, with the delivery the brain picks",
			"The best brain: uses his memories and stays in character"],
		"cost": ["Command decisions take about 150 ms instead of 36 ms", "About 2.7 GB left over"],
	},
	{
		"id": "fast", "name": "Fast Kevin", "run": "fast-brain",
		"parts": ["system", "breeze", "gemmae4", "kevmid", "parakeet"],
		"voice": "Breeze, live: first sound 0.23 s, 0.7× real time",
		"brain": "Gemma 4 E4B: first word 0.1–0.4 s, about 100 tokens/s",
		"kev": "Kev-0.8B mid: 36 ms",
		"good": ["Breeze speaks every line live", "The fastest reflexes and the quickest replies", "About 4 GB left over"],
		"cost": ["A blunter brain: short, samey replies that rarely use his memories"],
	},
	{
		"id": "quiet", "name": "Quiet Kevin", "run": None,
		"parts": ["system", "gemma12", "kevmid", "parakeet"],
		"voice": "Breeze, pre-made lines only: play instantly",
		"brain": "Gemma 4 12B: first word 0.3 s (1.3 s for a new prompt)",
		"kev": "Kev-0.8B mid: 36 ms",
		"good": ["The best brain and the fastest reflexes",
			"Barks, callouts and reactions still in Breeze's voice, made between sessions",
			"About 9 GB left over"],
		"cost": ["Breeze isn't loaded while you play: his free replies show as subtitles, or he answers with the closest pre-made line"],
	},
	{
		"id": "max", "name": "Everything maxed", "run": None,
		"parts": ["system", "breeze", "gemma12", "kevmid", "parakeet"],
		"voice": "Breeze, live", "brain": "Gemma 4 12B", "kev": "Kev-0.8B mid: 36 ms",
		"good": ["Everything at its best"],
		"cost": ["About 23.4 GB: right where Halo froze before. Not safe, so not tested."],
		"unsafe": True,
	},
]

VOICES = [
	("full", "Full", "Guidance 4, Kevin's designed voice plus the delivery for each line"),
	("cfg2", "Lighter", "Guidance 2: follows the delivery less closely"),
	("cfg1", "Minimal", "Guidance 1: barely follows the voice and delivery"),
	("clone", "Cloned", "Copies one reference line in Kevin's voice; no delivery per line"),
]

BRAINS = [
	("brain_kevin-gemma4-12b.json", "Gemma 4 12B"),
	("brain_gemma4_e4b-it-qat.json", "Gemma 4 E4B"),
	("brain_gemma4_e2b-it-qat.json", "Gemma 4 E2B"),
]

KEVS = [("kev_kev-0.8b-mid.json", "Kev-0.8B"), ("kev_kev-4b-lean.json", "Kev-4B")]

STYLE = """
/* One column of evidence: budget bars first, then the samples to judge each part by */
:root {
  --bg: #eef1f3; --surface: #ffffff; --fg: #182129; --muted: #56636e; --line: #d3dae0;
  --accent: #9a6416; --danger: #b42318; --danger-bg: #fbe9e7;
  --sys: #8b97a2; --voice: #d08a2e; --brain: #3d6fa8; --kev: #2f8f83; --ear: #7b62b0; --free: #e2e7eb;
  --display: "Saira Condensed", "Arial Narrow", sans-serif;
  --body: "IBM Plex Sans", "Segoe UI", system-ui, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, Consolas, monospace;
}
@media (prefers-color-scheme: dark) { :root:not([data-theme="light"]) {
  --bg: #12171c; --surface: #1a2128; --fg: #e3e8ec; --muted: #98a5b0; --line: #2b343d;
  --accent: #e0a84a; --danger: #f97066; --danger-bg: #3a1d1a;
  --sys: #6c7884; --voice: #e0a04a; --brain: #6b9bd6; --kev: #4fb8aa; --ear: #a08bd6; --free: #252e36;
  color-scheme: dark } }
:root[data-theme="dark"] {
  --bg: #12171c; --surface: #1a2128; --fg: #e3e8ec; --muted: #98a5b0; --line: #2b343d;
  --accent: #e0a84a; --danger: #f97066; --danger-bg: #3a1d1a;
  --sys: #6c7884; --voice: #e0a04a; --brain: #6b9bd6; --kev: #4fb8aa; --ear: #a08bd6; --free: #252e36;
  color-scheme: dark }
body { background: var(--bg); color: var(--fg); font: 15px/1.55 var(--body); }
main { max-width: 1040px; margin: 0 auto; padding-block: 32px 64px;
  padding-left: max(16px, env(safe-area-inset-left, 0px)); padding-right: max(16px, env(safe-area-inset-right, 0px));
  display: grid; gap: 48px; }
h1, h2, h3 { font-family: var(--display); font-weight: 600; text-wrap: balance; margin: 0; letter-spacing: .01em; }
h1 { font-size: 2.6rem; line-height: 1.05; }
h2 { font-size: 1.7rem; }
h3 { font-size: 1.25rem; }
p { margin: 0; max-width: 68ch; }
section { display: grid; gap: 16px; min-width: 0; }
.eyebrow { font: 600 .75rem var(--mono); letter-spacing: .12em; text-transform: uppercase; color: var(--accent); }
.lede { color: var(--muted); }
.mono, td.num, .num { font-family: var(--mono); font-variant-numeric: tabular-nums; }
.bars { display: grid; gap: 14px; background: var(--surface); border: 1px solid var(--line); border-radius: 6px; padding: 20px; }
.bar-row { display: grid; grid-template-columns: 150px 1fr 70px; gap: 12px; align-items: center; }
.bar-row .label { font-weight: 600; }
.bar-row .total { font-family: var(--mono); text-align: right; font-variant-numeric: tabular-nums; }
.bar-row.unsafe .total { color: var(--danger); font-weight: 600; }
.track { position: relative; height: 26px; background: var(--free); border-radius: 3px; overflow: hidden; display: flex; }
.seg { height: 100%; border-right: 1px solid var(--surface); }
.seg.sys { background: var(--sys); } .seg.voice { background: var(--voice); } .seg.brain { background: var(--brain); }
.seg.kev { background: var(--kev); } .seg.ear { background: var(--ear); }
.zone { position: absolute; top: 0; bottom: 0; right: 0; width: calc(100% / 24);
  background: repeating-linear-gradient(135deg, transparent 0 5px, color-mix(in srgb, var(--danger) 45%, transparent) 5px 7px);
  border-left: 2px solid var(--danger); }
.axis { display: grid; grid-template-columns: 150px 1fr 70px; gap: 12px; }
.ticks { position: relative; height: 18px; font: .72rem var(--mono); color: var(--muted); }
.ticks span { position: absolute; transform: translateX(-50%); }
.ticks span:first-child { transform: none; } .ticks span:last-child { transform: translateX(-100%); }
.legend { display: flex; flex-wrap: wrap; gap: 8px 18px; font-size: .85rem; color: var(--muted); }
.legend i { display: inline-block; width: 11px; height: 11px; border-radius: 2px; margin-right: 6px; vertical-align: -1px; }
.cards { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 16px; }
.card { background: var(--surface); border: 1px solid var(--line); border-radius: 6px; padding: 18px 20px; display: grid; gap: 12px; align-content: start; }
.card.unsafe { border-color: var(--danger); background: var(--danger-bg); }
.card header { display: flex; justify-content: space-between; align-items: baseline; gap: 12px; }
.card .gb { font: 600 1.05rem var(--mono); }
.card.unsafe .gb { color: var(--danger); }
.spec { display: grid; grid-template-columns: auto 1fr; gap: 3px 14px; margin: 0; font-size: .9rem; }
.spec dt { color: var(--muted); } .spec dd { margin: 0; }
.card ul { margin: 0; padding-left: 18px; display: grid; gap: 3px; font-size: .92rem; }
.card .tag { font: 600 .7rem var(--mono); letter-spacing: .1em; text-transform: uppercase; }
.card .tag.good { color: var(--kev); } .card .tag.cost { color: var(--accent); } .card.unsafe .tag.cost { color: var(--danger); }
.scroll { overflow-x: auto; background: var(--surface); border: 1px solid var(--line); border-radius: 6px; }
table { border-collapse: collapse; width: 100%; font-size: .9rem; }
th, td { text-align: left; vertical-align: top; padding: 10px 12px; border-bottom: 1px solid var(--line); }
thead th { font: 600 .78rem var(--body); color: var(--muted); background: var(--surface); position: sticky; top: 0; }
thead th small { display: block; font: .72rem var(--mono); color: var(--muted); font-weight: 400; margin-top: 2px; }
tbody tr:last-child td { border-bottom: 0; }
.line-text { min-width: 210px; }
.line-text .delivery { color: var(--muted); font-size: .82rem; display: block; margin-top: 2px; }
audio { width: 190px; max-width: 100%; height: 34px; }
.timing { display: block; font: .72rem var(--mono); color: var(--muted); margin-top: 3px; }
.reply { min-width: 220px; }
.situation { color: var(--muted); font-size: .82rem; display: block; margin-top: 2px; }
.wrong { color: var(--danger); font-weight: 600; }
.right { color: var(--muted); }
.notes { display: grid; gap: 8px; color: var(--muted); font-size: .9rem; }
.notes p { max-width: 80ch; }
a { color: var(--accent); }
:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
@media (max-width: 720px) {
  .cards { grid-template-columns: 1fr; }
  .bar-row, .axis { grid-template-columns: 1fr 64px; }
  .bar-row .label { grid-column: 1 / -1; }
  .axis > :first-child { display: none; }
  h1 { font-size: 2.1rem; }
}
"""


def bar(loadout):
	total = sum(PARTS[p][1] for p in loadout["parts"])
	measured = combined_total(loadout["run"]) if loadout["run"] else None
	segs = "".join(f'<div class="seg {COLOR[p]}" style="width:{PARTS[p][1] / (24 * GB) * 100:.2f}%" title="{e(PARTS[p][0])}: '
		f'{PARTS[p][1] / GB:.1f} GB"></div>' for p in loadout["parts"])
	cls = "bar-row unsafe" if loadout.get("unsafe") else "bar-row"
	return total, measured, (f'<div class="{cls}"><span class="label">{e(loadout["name"])}</span>'
		f'<div class="track" role="img" aria-label="{e(loadout["name"])}: {total / GB:.1f} of 24 GB">{segs}<div class="zone"></div></div>'
		f'<span class="total">{total / GB:.1f} GB</span></div>')


def card(loadout, total, measured):
	cls = "card unsafe" if loadout.get("unsafe") else "card"
	note = (f"measured next to Halo: {measured / GB:.1f} GB, {(measured + PARTS['parakeet'][1]) / GB:.1f} GB while Parakeet listens"
		if measured else "added up from the parts' measured sizes")
	good = "".join(f"<li>{e(x)}</li>" for x in loadout["good"])
	cost = "".join(f"<li>{e(x)}</li>" for x in loadout["cost"])
	return (f'<article class="{cls}" id="{loadout["id"]}"><header><h3>{e(loadout["name"])}</h3>'
		f'<span class="gb">{total / GB:.1f} GB</span></header>'
		f'<dl class="spec"><dt>Voice</dt><dd>{e(loadout["voice"])}</dd><dt>Brain</dt><dd>{e(loadout["brain"])}</dd>'
		f'<dt>Commands</dt><dd>{e(loadout["kev"])}</dd><dt>Hearing</dt><dd>Parakeet, half precision: about 80 ms</dd>'
		f'<dt>Memory</dt><dd class="mono">{e(note)}</dd></dl>'
		f'<div><span class="tag good">Gets you</span><ul>{good}</ul></div>'
		f'<div><span class="tag cost">Costs you</span><ul>{cost}</ul></div></article>')


def voice_table():
	runs = {v: load(f"breeze_{v}.json") for v, _, _ in VOICES}
	head = "".join(
		f'<th>{e(label)}<small>{e(desc)}</small><small>first sound {statistics.median(l["first_audio_ms"] for l in runs[v]["lines"]):.0f} ms, '
		f'{statistics.median(l["rtf"] for l in runs[v]["lines"]):.2f}× real time</small></th>'
		for v, label, desc in VOICES if runs[v])
	rows = []
	for i, line in enumerate(runs["full"]["lines"]):
		cells = []
		for v, _, _ in VOICES:
			if not runs[v]:
				continue
			l = runs[v]["lines"][i]
			cells.append(f'<td><audio controls preload="none" src="{e(l["wav"])}"></audio>'
				f'<span class="timing">{l["audio_s"]:.1f} s line, ready in {l["total_ms"] / 1000:.1f} s</span></td>')
		rows.append(f'<tr><td class="line-text">“{e(line["text"])}”<span class="delivery">{e(line["delivery"])}</span></td>{"".join(cells)}</tr>')
	return f'<div class="scroll"><table><thead><tr><th>Line and delivery</th>{head}</tr></thead><tbody>{"".join(rows)}</tbody></table></div>'


def brain_table():
	runs = [(load(f), name) for f, name in BRAINS]
	runs = [(r, n) for r, n in runs if r]
	head = "".join(
		f'<th>{e(n)}<small>{r["vram_mib"] / GB:.1f} GB · first word {statistics.median(t["first_token_ms"] for t in r["turns"][1:]):.0f} ms · '
		f'{statistics.median(t["tok_s"] for t in r["turns"]):.0f} tokens/s</small></th>' for r, n in runs)
	rows = []
	for i, turn in enumerate(runs[0][0]["turns"]):
		cells = "".join(f'<td class="reply">{e(r["turns"][i]["kevin"])}</td>' for r, _ in runs)
		rows.append(f'<tr><td class="line-text">“{e(turn["user"])}”<span class="situation">{e(turn["situation"])}</span></td>{cells}</tr>')
	return f'<div class="scroll"><table><thead><tr><th>You say</th>{head}</tr></thead><tbody>{"".join(rows)}</tbody></table></div>'


def kev_table():
	runs = [(load(f), name) for f, name in KEVS]
	runs = [(r, n) for r, n in runs if r]
	head = "".join(f'<th>{e(n)}<small>{r["right"]} of {r["of"]} right</small></th>' for r, n in runs)
	rows = []
	for i, a in enumerate(runs[0][0]["answers"]):
		cells = "".join(
			f'<td class="{"right" if r["answers"][i]["right"] else "wrong"}">{e(r["answers"][i]["action"])}</td>' for r, _ in runs)
		rows.append(f'<tr><td>“{e(a["utterance"])}”</td><td class="mono">{e(" / ".join(a["expected"]))}</td>{cells}</tr>')
	return (f'<div class="scroll"><table><thead><tr><th>You say</th><th>Right answer</th>{head}</tr></thead>'
		f'<tbody>{"".join(rows)}</tbody></table></div>')


def main():
	bars, cards = [], []
	for loadout in LOADOUTS:
		total, measured, row = bar(loadout)
		bars.append(row)
		cards.append(card(loadout, total, measured))
	ticks = "".join(f'<span style="left:{g / 24 * 100:.2f}%">{g}</span>' for g in range(0, 25, 4))
	legend = "".join(f'<span><i style="background:var(--{c})"></i>{e(t)}</span>' for c, t in
		[("sys", "Windows and Halo"), ("voice", "Breeze"), ("brain", "Brain"), ("kev", "Kev"), ("ear", "Parakeet"), ("free", "Free")])
	page = f"""<title>Kevin Loadouts</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Mono:wght@400;600&family=IBM+Plex+Sans:wght@400;600&family=Saira+Condensed:wght@600&display=swap">
<style>{STYLE}</style>
<main>
<header style="display:grid;gap:10px">
  <span class="eyebrow">S0 · RTX 3090, 24 GB · Halo running a30</span>
  <h1>Kevin Loadouts</h1>
  <p class="lede">Four ways to fit Kevin on one card next to the game, with Halo capped at 60 fps. The bars are to scale; the striped end is where Windows starts paging GPU memory and Halo freezes. Below them, listen to the voice options, read each brain's replies to the same moments, and check how well each Kev model follows commands.</p>
</header>

<section aria-labelledby="budget">
  <h2 id="budget">Memory on the card</h2>
  <div class="bars">
    {''.join(bars)}
    <div class="axis"><span></span><div class="ticks">{ticks}</div><span></span></div>
    <div class="legend">{legend}<span><i style="background:var(--danger)"></i>Halo freezes</span></div>
  </div>
</section>

<section aria-labelledby="loadouts">
  <h2 id="loadouts">The loadouts</h2>
  <div class="cards">{''.join(cards)}</div>
</section>

<section aria-labelledby="voice">
  <h2 id="voice">Voice: Breeze at four settings</h2>
  <p class="lede">Every column ran live in the decode, depth and codec setup (8.8 GB). Lower guidance is what a lighter Breeze sounds like; it saves no memory, only a little time. Breeze's normal mode sounds the same as Full but makes audio 3.6× slower than it plays, so it only suits lines made ahead.</p>
  {voice_table()}
</section>

<section aria-labelledby="brain">
  <h2 id="brain">Brain: the same moments, three sizes</h2>
  <p class="lede">Same character sheet, memories and situation for each. The square brackets are the delivery he picked, which goes to Breeze with the words.</p>
  {brain_table()}
</section>

<section aria-labelledby="commands">
  <h2 id="commands">Commands: Kev as it comes, untrained</h2>
  <p class="lede">Twenty spoken commands in the middle of a30. Both sizes get 15 right before any training on Kevin's own commands; the bigger one makes different mistakes, not fewer. Kev-0.8B decides in 36 ms (mid) or 150 ms (lean).</p>
  {kev_table()}
</section>

<section aria-labelledby="notes" class="notes">
  <h2 id="notes" style="color:var(--fg)">How these were measured</h2>
  <p>Every part's size is what it added on the card (nvidia-smi) when loaded next to Halo on a30. Full Kevin and Fast Kevin were also loaded all at once next to the game and timed, first with each model alone, then while the brain talked nonstop.</p>
  <p>Halo must be capped at 60 fps (vsync off, display.max_fps = 60). At 240 fps on your monitor it took enough of the card that Breeze slowed to 1.3× real time and stuttered, and Gemma halved its speed.</p>
  <p>While the brain writes nonstop, Breeze slows to about 1.5× real time in every loadout, so background work such as the diary and summaries has to wait while Kevin speaks. His own replies are short enough that this doesn't matter for them.</p>
  <p>Parakeet hears in about 0.1 s per pass on the GPU at half precision (1.2 GB of weights); on the CPU it takes 1.5 to 4.4 s, too slow even for push-to-talk.</p>
  <p>Cloning Breeze's voice from a reference line adds about 3 GB of extra graphs on top of the 8.8 GB, so a live setup should use one way of voicing him, not both.</p>
</section>
</main>
"""
	path = os.path.join(SAMPLES, "kevin_loadouts.html")
	with open(path, "w", encoding="utf-8") as f:
		f.write(page)
	print(path)


if __name__ == "__main__":
	main()
