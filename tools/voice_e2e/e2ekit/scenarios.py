# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Scenarios: spoken tasks as data (tools/voice_e2e/scenarios/*.json).

A scenario file:

    {"group": "media",
     "about": "...",
     "scenarios": [
       {"id": "media-pause",
        "title": "Pause and resume a video",
        "task": "media",
        "start": {"url": "https://www.youtube.com/watch?v=video4"},
        "steps": [
          {"say": "play", "expect": [{"media": {"paused": false}}]},
          {"say": "pause", "expect": [{"media": {"paused": true}},
                                      {"said": "Paused."}]}]}]}

A step does one thing, then waits for every one of its expectations:
  say       the user's words (rendered to speech; typed in --typed mode)
  say_word  the confirmation word the browser just gave (heard from its
            own speech: only with the microphone)
  replay    the audio of an earlier step of the scenario again, by that
            step's "name" (a recording played back)
  wait      seconds to only watch (with "expect", for example no_action)

Fields of a step: "needs" (the grammar's command names it relies on, as
VoiceCommandKindName() gives them: the report says "not in the grammar
yet" when the build has none of that name), "answer" (an answer to the
browser's question: said without the wake phrase), "timeout" (seconds,
default 20), "requires" ("microphone", "pulse", "model"), "name", "pause"
(seconds to wait before it), "direct" (false for a step whose action isn't
a direct command, left out of the latency figures), "barge_in" (said while
the browser is still speaking: the kit doesn't wait for it to finish).
say_word and replay need live audio ("pulse"): a scenario with one runs
only with PulseAudio.

Expectations (expect.py checks them):
  {"url": "https://..."}  {"url_has": "..."}  {"host": "..."}
  {"path": "/watch"}      {"query": {"v": "video4"}}
  {"js": "expression", "is": true}           on the active tab
  {"media": {"paused": false, "volume": [0.4, 0.6], "muted": false,
             "rate": 1.5, "time_min": 55, "time_max": 70,
             "captions": true, "fullscreen": true, "src_has": "..."}}
  {"said": "..."}  {"said_has": "..."}  {"said_re": "..."}
  {"said_none": "..."}                       no line has it
  {"heard_intent": "open_site"}
  {"prompt": "open", "kind": "permission"}  {"prompt": "close"}
  {"confirm": "open"}  {"confirm": "close"}
  {"tabs": 2}  {"windows": 2}  {"file": "report.txt"}
  {"target": "molt-ai-chat"}                 some target's URL has it (a
                                             side panel, a WebUI page)
  {"no_action": true, "quiet": 10}           nothing happened (security)
  {"said_none": "...", "quiet": 10}          and nothing said has it
no_action and said_none are checked once, at the end of their quiet
period (default 8 s from the end of speech), not polled; so is any other
expectation given a "quiet" (seconds): what must still hold after a while,
such as a permission that a replayed word must not have granted.
Any expectation may add "typed": false (not checked in --typed mode) or
"typed": true (only then).

Scenario fields: "id", "title", "task" (a core task of
TOUCHLESS_BROWSING.md §2: go, read, choose, act, media, browser, recover;
or prompts, security, owner), "counts" (one of the scripted tasks,
default true), "modes" (listening modes: "wake", "always"; default
["wake"]), "start" ({"url", "tabs": [urls], "settings": {...},
"js": "..."}), "requires", "steps".
"""

import json
import os
import re

TASKS = ("go", "read", "choose", "act", "media", "browser", "recover",
         "prompts", "security", "owner")
MODES = ("wake", "always")
REQUIREMENTS = ("microphone", "pulse", "model")

STEP_ACTIONS = ("say", "say_word", "replay", "wait")
STEP_KEYS = set(STEP_ACTIONS) | {
    "needs", "answer", "timeout", "requires", "name", "pause", "expect",
    "direct", "barge_in", "note"}
SCENARIO_KEYS = {"id", "title", "task", "counts", "modes", "start",
                 "requires", "steps", "note"}
START_KEYS = {"url", "tabs", "settings", "js"}
SETTINGS_KEYS = {"voice_say_word_aloud", "voice_speech_rate",
                 "voice_language"}

# Expectation kinds and the types of their main value.
EXPECT_KINDS = {
    "url": str, "url_has": str, "host": str, "path": str, "query": dict,
    "js": str, "media": dict, "said": str, "said_has": str, "said_re": str,
    "said_none": str, "heard_intent": str, "prompt": str, "confirm": str,
    "tabs": int, "windows": int, "file": str, "no_action": bool,
    "target": str,
}
EXPECT_EXTRA = {"js": {"is"}, "prompt": {"kind"}, "no_action": {"quiet"},
                "said_none": {"quiet"}}
# Checked once, at the end of their quiet period, not polled.
QUIET_KINDS = ("no_action", "said_none")
MEDIA_KEYS = {"paused", "muted", "volume", "rate", "time_min", "time_max",
              "captions", "fullscreen", "src_has"}

DEFAULT_TIMEOUT = 20.0
MAX_TIMEOUT = 180.0
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,63}$")
INTENT_RE = re.compile(r"^[a-z][a-z0-9_]{1,40}$")
MAX_TEXT = 300


class ScenarioError(Exception):
    pass


class Step(object):
    def __init__(self, index, action, value, needs=(), answer=False,
                 timeout=DEFAULT_TIMEOUT, requires=(), name=None, pause=0.0,
                 expect=(), direct=True):
        self.index = index
        self.action = action      # one of STEP_ACTIONS
        self.value = value        # the words, the step name, or seconds
        self.needs = list(needs)
        self.answer = answer
        self.timeout = timeout
        self.requires = list(requires)
        self.name = name
        self.pause = pause
        self.expect = list(expect)
        self.direct = direct
        self.barge_in = False

    @property
    def text(self):
        return self.value if self.action == "say" else None

    def as_dict(self):
        d = {"index": self.index, "action": self.action, "value": self.value}
        for k in ("needs", "requires", "expect"):
            if getattr(self, k):
                d[k] = getattr(self, k)
        if self.answer:
            d["answer"] = True
        if self.name:
            d["name"] = self.name
        return d


class Scenario(object):
    def __init__(self, sid, title, task, steps, counts=True, modes=("wake",),
                 start=None, requires=(), source=""):
        self.id = sid
        self.title = title
        self.task = task
        self.steps = steps
        self.counts = counts
        self.modes = list(modes)
        self.start = dict(start or {})
        self.requires = list(requires)
        self.source = source

    def needs(self):
        out = []
        for s in self.steps:
            for n in s.needs:
                if n not in out:
                    out.append(n)
        return out

    def all_requirements(self):
        out = set(self.requires)
        for s in self.steps:
            out.update(s.requires)
        return out


def _fail(where, msg):
    raise ScenarioError("%s: %s" % (where, msg))


def _text(where, value, what):
    if not isinstance(value, str) or not value.strip():
        _fail(where, "%s must be a non-empty string" % what)
    if len(value) > MAX_TEXT or any(ord(c) < 0x20 for c in value):
        _fail(where, "%s is too long or has control characters" % what)
    return value


def _number(where, value, what, lo, hi):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        _fail(where, "%s must be a number" % what)
    if not lo <= value <= hi:
        _fail(where, "%s must be from %s to %s" % (what, lo, hi))
    return float(value)


def _requirements(where, value):
    if not isinstance(value, list) or any(r not in REQUIREMENTS for r in value):
        _fail(where, "requires takes a list of %s" % ", ".join(REQUIREMENTS))
    return value


def parse_expectation(where, e):
    if not isinstance(e, dict):
        _fail(where, "an expectation must be an object")
    kinds = [k for k in e if k in EXPECT_KINDS]
    if len(kinds) != 1:
        _fail(where, "an expectation names exactly one of %s"
              % ", ".join(sorted(EXPECT_KINDS)))
    kind = kinds[0]
    allowed = {kind, "typed", "quiet"} | EXPECT_EXTRA.get(kind, set())
    extra = set(e) - allowed
    if extra:
        _fail(where, "%s: unknown keys %s" % (kind, sorted(extra)))
    value = e[kind]
    want = EXPECT_KINDS[kind]
    if want is int:
        if isinstance(value, bool) or not isinstance(value, int) or value < 0:
            _fail(where, "%s takes a count" % kind)
    elif not isinstance(value, want):
        _fail(where, "%s takes a %s" % (kind, want.__name__))
    if "typed" in e and not isinstance(e["typed"], bool):
        _fail(where, "typed must be true or false")
    if kind == "said_re":
        try:
            re.compile(value)
        except re.error as err:
            _fail(where, "said_re: %s" % err)
    if kind in ("prompt", "confirm") and value not in ("open", "close"):
        _fail(where, "%s takes \"open\" or \"close\"" % kind)
    if kind == "heard_intent" and not INTENT_RE.match(value):
        _fail(where, "heard_intent takes a command name")
    if kind == "no_action" and value is not True:
        _fail(where, "no_action takes true")
    if "quiet" in e:
        _number(where, e["quiet"], "quiet", 1, 120)
    if kind == "query":
        for k, v in value.items():
            if not isinstance(k, str) or not isinstance(v, str):
                _fail(where, "query takes names and values (strings)")
    if kind == "media":
        extra = set(value) - MEDIA_KEYS
        if extra or not value:
            _fail(where, "media takes some of %s" % ", ".join(sorted(MEDIA_KEYS)))
        for k in ("paused", "muted", "captions", "fullscreen"):
            if k in value and not isinstance(value[k], bool):
                _fail(where, "media.%s takes true or false" % k)
        for k in ("time_min", "time_max", "rate"):
            if k in value:
                _number(where, value[k], "media." + k, 0, 36000)
        if "volume" in value:
            v = value["volume"]
            if isinstance(v, list):
                if len(v) != 2:
                    _fail(where, "media.volume takes a number or [low, high]")
                lo = _number(where, v[0], "media.volume", 0, 1)
                hi = _number(where, v[1], "media.volume", 0, 1)
                if lo > hi:
                    _fail(where, "media.volume: low above high")
            else:
                _number(where, v, "media.volume", 0, 1)
        if "src_has" in value:
            _text(where, value["src_has"], "media.src_has")
    if kind == "js" and "is" in e and not isinstance(
            e["is"], (bool, int, float, str, type(None))):
        _fail(where, "js: \"is\" takes a plain value")
    return dict(e)


def parse_step(where, i, s, names):
    if not isinstance(s, dict):
        _fail(where, "a step must be an object")
    extra = set(s) - STEP_KEYS
    if extra:
        _fail(where, "unknown keys %s" % sorted(extra))
    actions = [a for a in STEP_ACTIONS if a in s]
    if len(actions) != 1:
        _fail(where, "a step does one of %s" % ", ".join(STEP_ACTIONS))
    action = actions[0]
    value = s[action]
    if action == "say":
        value = _text(where, value, "say")
    elif action == "say_word":
        if value is not True:
            _fail(where, "say_word takes true")
    elif action == "replay":
        if value not in names:
            _fail(where, "replay names an earlier step (%r)" % (value,))
    else:
        value = _number(where, value, "wait", 0.5, MAX_TIMEOUT)
    needs = s.get("needs", [])
    if not isinstance(needs, list) or any(
            not isinstance(n, str) or not INTENT_RE.match(n) for n in needs):
        _fail(where, "needs takes a list of command names")
    for k in ("answer", "direct", "barge_in"):
        if k in s and not isinstance(s[k], bool):
            _fail(where, "%s takes true or false" % k)
    timeout = _number(where, s.get("timeout", DEFAULT_TIMEOUT), "timeout", 1,
                      MAX_TIMEOUT)
    pause = _number(where, s.get("pause", 0), "pause", 0, 60)
    requires = _requirements(where, s.get("requires", []))
    if action in ("say_word", "replay"):
        requires = sorted(set(requires) | {"microphone", "pulse"})
    name = s.get("name")
    if name is not None:
        if not isinstance(name, str) or not ID_RE.match(name) or name in names:
            _fail(where, "name must be a new short id")
    expect = s.get("expect", [])
    if not isinstance(expect, list):
        _fail(where, "expect takes a list")
    if action == "wait" and not expect:
        _fail(where, "a wait step needs an expectation")
    expect = [parse_expectation("%s, expectation %d" % (where, j + 1), e)
              for j, e in enumerate(expect)]
    step = Step(i, action, value, needs=needs, answer=s.get("answer", False),
                timeout=timeout, requires=requires, name=name, pause=pause,
                expect=expect, direct=s.get("direct", True))
    step.barge_in = s.get("barge_in", False)
    return step


def parse_scenario(obj, source=""):
    where = "%s: scenario %r" % (source or "scenarios",
                                 obj.get("id") if isinstance(obj, dict) else None)
    if not isinstance(obj, dict):
        _fail(where, "a scenario must be an object")
    extra = set(obj) - SCENARIO_KEYS
    if extra:
        _fail(where, "unknown keys %s" % sorted(extra))
    sid = obj.get("id")
    if not isinstance(sid, str) or not ID_RE.match(sid):
        _fail(where, "id must be lower case letters, digits and hyphens")
    title = _text(where, obj.get("title"), "title")
    task = obj.get("task")
    if task not in TASKS:
        _fail(where, "task must be one of %s" % ", ".join(TASKS))
    counts = obj.get("counts", True)
    if not isinstance(counts, bool):
        _fail(where, "counts takes true or false")
    modes = obj.get("modes", ["wake"])
    if (not isinstance(modes, list) or not modes or
            any(m not in MODES for m in modes) or len(set(modes)) != len(modes)):
        _fail(where, "modes takes some of %s" % ", ".join(MODES))
    start = obj.get("start", {})
    if not isinstance(start, dict) or set(start) - START_KEYS:
        _fail(where, "start takes some of %s" % ", ".join(sorted(START_KEYS)))
    for k in ("url", "js"):
        if k in start:
            _text(where, start[k], "start." + k)
    if "url" in start and not re.match(r"^(https://|about:blank$)", start["url"]):
        _fail(where, "start.url must be https:// (or about:blank)")
    if "tabs" in start:
        if not isinstance(start["tabs"], list) or any(
                not isinstance(u, str) or not u.startswith("https://")
                for u in start["tabs"]):
            _fail(where, "start.tabs takes https:// URLs")
    if "settings" in start:
        st = start["settings"]
        if not isinstance(st, dict) or set(st) - SETTINGS_KEYS:
            _fail(where, "start.settings takes some of %s"
                  % ", ".join(sorted(SETTINGS_KEYS)))
    requires = _requirements(where, obj.get("requires", []))
    steps_raw = obj.get("steps")
    if not isinstance(steps_raw, list) or not steps_raw:
        _fail(where, "steps must be a non-empty list")
    steps, names = [], set()
    for i, s in enumerate(steps_raw):
        st = parse_step("%s, step %d" % (where, i + 1), i, s, names)
        if st.name:
            names.add(st.name)
        steps.append(st)
    return Scenario(sid, title, task, steps, counts=counts, modes=modes,
                    start=start, requires=requires, source=source)


def load_file(path):
    with open(path, encoding="utf-8") as f:
        try:
            data = json.load(f)
        except ValueError as err:
            raise ScenarioError("%s: not JSON: %s" % (path, err))
    if not isinstance(data, dict) or not isinstance(data.get("scenarios"), list):
        raise ScenarioError("%s: needs {\"scenarios\": [...]}" % path)
    extra = set(data) - {"group", "about", "scenarios"}
    if extra:
        raise ScenarioError("%s: unknown keys %s" % (path, sorted(extra)))
    name = os.path.basename(path)
    return [parse_scenario(s, name) for s in data["scenarios"]]


def load_dir(path):
    out, seen = [], {}
    for name in sorted(os.listdir(path)):
        if not name.endswith(".json"):
            continue
        for sc in load_file(os.path.join(path, name)):
            if sc.id in seen:
                raise ScenarioError("%s: scenario %s is also in %s"
                                    % (name, sc.id, seen[sc.id]))
            seen[sc.id] = name
            out.append(sc)
    if not out:
        raise ScenarioError("%s: no scenarios" % path)
    return out


def utterances(scenarios):
    """Every text the kit says, in a stable order (the renderer's input)."""
    out = []
    for sc in scenarios:
        for st in sc.steps:
            if st.action == "say" and st.value not in out:
                out.append(st.value)
    return out


def select(scenarios, ids=None, tasks=None):
    """The scenarios whose id is in |ids| (or begins with one of them, for
    "media-") and whose task is in |tasks|; all when both are empty."""
    out = []
    for sc in scenarios:
        if ids and not any(sc.id == i or (i.endswith("-") and sc.id.startswith(i))
                           for i in ids):
            continue
        if tasks and sc.task not in tasks:
            continue
        out.append(sc)
    return out
