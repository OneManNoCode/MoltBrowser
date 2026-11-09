# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Expectations: whether a step's outcome is there yet, and why not.

check(expectation, world, since) looks at |world| (the runner's view of
the browser, or a test's fake) and returns (ok, detail). The world gives:

  url()                    the active tab's URL
  evaluate(expression)     a value from the active tab (no user gesture)
  media()                  the active tab's player, as MEDIA_JS reads it
  log_events(since)        the test log's events since |since| (seconds)
  tab_count(), window_count()
  downloaded(name)         whether the download directory holds |name|
  page_events(since)       what the page recorder saw since |since|
  target_urls()            every target's URL (pages, side panels, WebUI)

A no_action expectation isn't polled: the runner takes a snapshot() before
the step and calls quiet_check(before, world, since) at the end of its
quiet period.
"""

import re
import time
import urllib.parse

# The player of the active tab: the first <video> or <audio> of the page,
# else what a frame's player reported to the page (fixtures/kit.js), as
# a plain object.
MEDIA_JS = r"""(() => {
  const m = document.querySelector('video, audio');
  if (!m) { return window.__moltFrameMedia || null; }
  const track = Array.from(m.textTracks || []).find(
      t => t.kind === 'captions' || t.kind === 'subtitles');
  return {paused: m.paused, muted: m.muted, volume: m.volume,
          rate: m.playbackRate, time: m.currentTime, src: m.currentSrc || m.src,
          captions: !!(track && track.mode === 'showing') ||
                    document.documentElement.dataset.captions === 'on',
          fullscreen: !!document.fullscreenElement};
})()"""

# Page events that are actions (the browser did something on the page).
ACTION_EVENTS = ("click", "input", "change", "submit", "keydown",
                 "volumechange", "ratechange", "seeking", "pause",
                 "fullscreenchange")

VOLUME_SLACK = 0.02
RATE_SLACK = 0.01


def _said_lines(world, since):
    return [e.get("text", "") for e in world.log_events(since)
            if e.get("e") == "said"]


def _norm(text):
    return re.sub(r"\s+", " ", text or "").strip()


def _media(world, want, before=None):
    got = world.media()
    if not isinstance(got, dict):
        return False, "no player on the page"
    problems = []
    for k in ("paused", "muted", "captions", "fullscreen"):
        if k in want and bool(got.get(k)) != want[k]:
            problems.append("%s is %s" % (k, str(bool(got.get(k))).lower()))
    if "volume" in want:
        v = got.get("volume")
        w = want["volume"]
        lo, hi = (w[0], w[1]) if isinstance(w, list) else (
            w - VOLUME_SLACK, w + VOLUME_SLACK)
        if not isinstance(v, (int, float)) or not lo <= v <= hi:
            problems.append("volume is %s" % v)
    if "rate" in want:
        r = got.get("rate")
        if not isinstance(r, (int, float)) or abs(r - want["rate"]) > RATE_SLACK:
            problems.append("rate is %s" % r)
    t = got.get("time")
    if "time_min" in want and (not isinstance(t, (int, float)) or t < want["time_min"]):
        problems.append("time is %s" % t)
    if "time_max" in want and (not isinstance(t, (int, float)) or t > want["time_max"]):
        problems.append("time is %s" % t)
    if "seeked" in want:
        # How far the player moved from where it would be had nothing
        # happened since the step began: its time then, plus the time
        # since at its rate while it played.
        lo, hi = want["seeked"]
        b = (before or {}).get("media")
        bt = b.get("time") if isinstance(b, dict) else None
        if not isinstance(bt, (int, float)) or not isinstance(t, (int, float)):
            problems.append("no player time before the step")
        else:
            rate = b.get("rate") if isinstance(b.get("rate"), (int, float)) else 1.0
            played = 0.0 if b.get("paused") else (time.time() - before["t"]) * rate
            moved = t - (bt + played)
            if not lo <= moved <= hi:
                problems.append("moved %+d s" % int(round(moved)))
    if "src_has" in want and want["src_has"] not in str(got.get("src") or ""):
        problems.append("src is %s" % got.get("src"))
    if problems:
        return False, "player: " + ", ".join(problems)
    return True, "player as expected"


def check(e, world, since, before=None):
    """(ok, detail) for expectation |e| now; |before| is the step's
    snapshot()."""
    kind = next(k for k in e if k not in ("typed", "is", "kind", "quiet"))
    value = e[kind]
    if kind in ("url", "url_has", "host", "path", "query"):
        url = world.url() or ""
        parts = urllib.parse.urlsplit(url)
        if kind == "url":
            ok = url == value
        elif kind == "url_has":
            ok = value in url
        elif kind == "host":
            ok = parts.hostname == value
        elif kind == "path":
            ok = parts.path == value
        else:
            q = dict(urllib.parse.parse_qsl(parts.query, keep_blank_values=True))
            ok = all(q.get(k) == v for k, v in value.items())
        return ok, "the tab is at %s" % url
    if kind == "js":
        want = e.get("is", True)
        try:
            got = world.evaluate(value)
        except Exception as err:  # the page's script error, or no page
            return False, "%s failed: %s" % (value, err)
        return got == want, "%s is %r" % (value, got)
    if kind == "media":
        return _media(world, value, before)
    if kind in ("said", "said_has", "said_re", "said_none"):
        lines = [_norm(t) for t in _said_lines(world, since)]
        joined = "\n".join(lines)
        if kind == "said":
            ok = _norm(value) in lines
        elif kind == "said_has":
            ok = _norm(value) in " ".join(lines)
        elif kind == "said_re":
            # Each line on its own: in file mode the step begins while
            # the line before it (the greeting, the last step's answer) is
            # still being said.
            ok = re.search(value, joined, re.MULTILINE) is not None
        else:
            ok = _norm(value) not in " ".join(lines)
        shown = " | ".join(lines)
        return ok, "said: %s" % (shown[:400] if shown else "(nothing)")
    if kind == "heard_intent":
        heard = [ev for ev in world.log_events(since) if ev.get("e") == "heard"]
        intents = [ev.get("intent") for ev in heard]
        return value in intents, "understood as %s" % (", ".join(
            str(i) for i in intents) or "(nothing heard)")
    if kind in ("prompt", "confirm"):
        evs = [ev for ev in world.log_events(since) if ev.get("e") == kind]
        for ev in evs:
            if ev.get("op") == value and (
                    kind != "prompt" or "kind" not in e or ev.get("kind") == e["kind"]):
                return True, "%s %s" % (kind, value)
        return False, "%s events: %s" % (kind, ", ".join(
            "%s%s" % (ev.get("op"), " " + ev["kind"] if ev.get("kind") else "")
            for ev in evs) or "none")
    if kind == "tabs":
        n = world.tab_count()
        return n == value, "%s tabs" % n
    if kind == "windows":
        n = world.window_count()
        return n == value, "%s windows" % n
    if kind == "file":
        return bool(world.downloaded(value)), "download %s %s" % (
            value, "is there" if world.downloaded(value) else "is missing")
    if kind == "target":
        urls = world.target_urls()
        return any(value in u for u in urls), "targets: %s" % ", ".join(urls)[:400]
    if kind == "no_action":
        return True, "checked after its quiet period"
    raise ValueError("unknown expectation %r" % kind)


def applies(e, typed):
    """Whether |e| is checked in this mode."""
    return e.get("typed", typed) == typed


def snapshot(world):
    """What a no_action expectation compares with, and the player as the
    step began (media "seeked")."""
    media = None
    dialog = getattr(world, "page_dialog_open", None)
    try:
        # Not while a page's dialog is open: the look would wait for it.
        if not (dialog and dialog()):
            media = world.media()
    except Exception:  # no page, or one going away: no player then
        media = None
    return {"url": world.url(), "tabs": world.tab_count(),
            "windows": world.window_count(), "media": media, "t": time.time()}


def quiet_check(before, world, since):
    """(ok, detail): nothing happened since |since| (and |before|)."""
    problems = []
    url = world.url()
    if url != before.get("url"):
        problems.append("the tab went to %s" % url)
    if world.tab_count() != before.get("tabs"):
        problems.append("%s tabs, was %s" % (world.tab_count(), before.get("tabs")))
    if world.window_count() != before.get("windows"):
        problems.append("%s windows, was %s" % (world.window_count(),
                                                before.get("windows")))
    acted = [ev for ev in world.page_events(since)
             if ev.get("type") in ACTION_EVENTS and ev.get("trusted", True)]
    if acted:
        problems.append("the page saw %s" % ", ".join(
            sorted({ev["type"] for ev in acted})))
    for ev in world.log_events(since):
        if ev.get("e") == "prompt" and ev.get("op") == "close":
            problems.append("a prompt was answered")
            break
    heard = [ev for ev in world.log_events(since) if ev.get("e") == "heard"
             and ev.get("intent") not in ("unknown", "refused", None)]
    detail = "heard %s" % ", ".join(
        "%r as %s" % (ev.get("text"), ev.get("intent")) for ev in heard) if heard else ""
    if problems:
        return False, "; ".join(problems + ([detail] if detail else []))
    return True, "nothing happened" + ("; " + detail if detail else "")
