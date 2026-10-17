"""Tier 1: keyword reflexes (kevin/BRIDGE_PLAN.md, "Four reflex tiers").

A small grammar of imperatives, matched on every partial transcript and
fired the moment the words appear, before the sentence ends. A phrase
fires once per occurrence in an utterance: a pass that hears "jump" again
doesn't jump again, a second "jump" later in the sentence does.

What tier 1 leaves alone goes to the brain: a question ("can you jump?"),
and a phrase after a negation ("don't jump"), which may mean anything. A
phrase that is itself a negation ("don't shoot") waits for the final
transcript: a partial of "don't jump" can be heard as "don't shoot".
Words are matched as Parakeet spells them, with the forms it hears for
each, and longer phrases first ("hold fire" is not "fire").
"""

import difflib
import re

# action: (phrases, what is sent to the game, bark category)
# what is sent: a list of commands (dicts), or "release"
ACTIONS = {
    "jump": (["jump"], [{"type": "press", "flag": "jump"}], "ack"),
    "crouch": (["crouch", "get down", "duck", "stay down", "keep low"],
               [{"type": "hold", "flag": "crouch", "on": True}], "ack"),
    "stand": (["stand up", "get up"], [{"type": "hold", "flag": "crouch", "on": False}], "ack"),
    "hold_fire": (["hold fire", "hold your fire", "cease fire", "stop shooting", "don't shoot", "do not shoot"],
                   [{"type": "fire_mode", "mode": "hold"}], "ack"),
    "fire": (["open fire", "fire at will", "weapons free", "go loud", "light them up", "fire"],
             [{"type": "fire_mode", "mode": "free"}], "ack"),
    "reload": (["reload"], [{"type": "press", "flag": "reload"}], "ack"),
    "grenade": (["grenade", "frag", "throw a grenade", "nade"], [{"type": "press", "flag": "grenade"}], "ack"),
    "flashlight": (["flashlight", "light on", "light off", "lights"], [{"type": "press", "flag": "flashlight"}],
                   "ack"),
    "swap_weapons": (["swap weapons", "switch weapons", "swap guns", "switch guns"],
                     [{"type": "press", "flag": "swap_weapons"}], "ack"),
    # the corrections: whatever the sidecar laid on him, off at once
    "stop": (["stop", "cancel", "never mind", "nevermind", "forget it", "as you were"], "release", "ack"),
}

# forms Parakeet hears for a word, besides the word
FORMS = {
    "jump": {"jumps"},
    "crouch": {"crouched", "crutch"},
    "grenade": {"grenades", "grenaid"},
    "reload": {"reloads"},
    "nade": {"nate"},
    "don't": {"dont"},
}

NAMES = {"kevin", "kev", "kevvy"}
FILLERS = {"hey", "ok", "okay", "please", "uh", "um", "now", "and", "then", "just", "right", "alright", "yo", "go",
           "so", "quick", "quickly"}
QUESTION_WORDS = {"can", "could", "would", "will", "do", "does", "did", "are", "is", "should", "shall", "may",
                  "might", "why", "how", "what", "when", "where", "who", "won't", "wont", "can't", "cant"}
NEGATIONS = {"don't", "dont", "do not", "not", "never"}


def words(text):
    """Lowercase words, apostrophes kept ("don't")."""
    return re.findall(r"[a-z]+(?:'[a-z]+)?", text.lower().replace("’", "'"))


def word_matches(heard, wanted):
    if heard == wanted or heard in FORMS.get(wanted, ()):
        return True
    # (a misspelling of a long word, not another word: "grenaid")
    return len(wanted) >= 6 and difflib.SequenceMatcher(None, heard, wanted).ratio() >= 0.85


def phrases():
    """Every (phrase's words, action), longest first."""
    table = [(words(phrase), action) for action, (texts, _, _) in ACTIONS.items() for phrase in texts]
    return sorted(table, key=lambda entry: -len(entry[0]))


PHRASES = phrases()


def find(text):
    """The actions in a transcript, in order: [(action, start word, words)]."""
    heard = words(text)
    used = [False] * len(heard)
    found = []
    for phrase, action in PHRASES:
        size = len(phrase)
        for start in range(len(heard) - size + 1):
            if any(used[start:start + size]):
                continue
            if all(word_matches(heard[start + index], phrase[index]) for index in range(size)):
                for index in range(start, start + size):
                    used[index] = True
                found.append((action, start, heard))
    found.sort(key=lambda entry: entry[1])
    return [(action, start, heard) for action, start, heard in found if not passed_on(heard, start)]


def passed_on(heard, start):
    """Whether the phrase at start is the brain's: in a question, or after a negation."""
    before = heard[:start]
    # (the clause it's in: from the last name or comma-less start)
    for index in range(len(before) - 1, -1, -1):
        if before[index] in NAMES:
            before = before[index + 1:]
            break
    meaningful = [word for word in before if word not in FILLERS and word not in NAMES]
    if meaningful and meaningful[0] in QUESTION_WORDS:
        return True
    near = before[-3:]
    if any(word in NEGATIONS for word in near) or " ".join(near[-2:]) in NEGATIONS:
        return True
    return False


class Reflex:
    """Tier 1 over one utterance's partials: each action once per occurrence."""

    def __init__(self):
        self.utterance = None
        self.fired = {}

    def hear(self, utterance, text, final=True):
        """The actions to take now for this partial (or final) transcript."""
        if utterance != self.utterance:
            self.utterance = utterance
            self.fired = {}
        counts = {}
        new = []
        for action, start, heard in find(text):
            if not final and heard[start] in NEGATIONS | {"do"}:
                continue
            counts[action] = counts.get(action, 0) + 1
            if counts[action] > self.fired.get(action, 0):
                self.fired[action] = counts[action]
                new.append(action)
        return new
