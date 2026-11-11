# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Reads voice mode's test log (--molt-voice-test-log, written by
voice_test_log.cc): one JSON object per line, appended as it happens.

  {"t": ms since the epoch, "w": window, "e": "start", "kinds": [...]}
  {"e": "heard", "text", "source": "mic"|"typed", "clean", "over_voice"?, "intent"}
  {"e": "said", "text"}
  {"e": "activity", "activity", "mic", "waiting"}
  {"e": "prompt", "op": "open"|"close", "id", "kind"}
  {"e": "confirm", "op": "open"|"close", "what"}
  {"e": "map", "items", "lists", "media", "selection", "selected_text",
   "labels"}
  {"e": "mic", "peak_db", "samples", "dropped", "echo"}
  {"e": "dropped", "text", "reason"}
  {"e": "helper", "event": "wake"|"speech_start"|"speech_end", "lag_ms",
   "note"}  (a wake's note: "taken", or why it was set aside)
  {"e": "earcon", "kind"}
  {"e": "duck", "on", "tabs"}

The browser writes it; the kit reads it as untrusted input all the same
(a line too long, not JSON or not an object is skipped and counted).
"""

import json
import math
import os
import threading

MAX_LINE = 64 * 1024
EVENTS = ("start", "heard", "said", "activity", "prompt", "confirm", "map",
          "mic", "dropped", "helper", "earcon", "duck")


class TestLog(object):
    def __init__(self, path):
        self.path = path
        self._pos = 0
        self._partial = b""
        self._events = []
        self._bad = 0
        self._lock = threading.Lock()

    @property
    def bad_lines(self):
        return self._bad

    def poll(self):
        """Reads what was appended since the last call."""
        try:
            with open(self.path, "rb") as f:
                f.seek(self._pos)
                data = f.read()
                self._pos = f.tell()
        except OSError:
            return
        if not data:
            return
        data = self._partial + data
        lines = data.split(b"\n")
        self._partial = lines.pop()
        if len(self._partial) > MAX_LINE:
            self._partial = b""
            self._bad += 1
        with self._lock:
            for raw in lines:
                ev = parse_line(raw)
                if ev is None:
                    if raw.strip():
                        self._bad += 1
                    continue
                self._events.append(ev)

    def events(self, since=0.0, window=None):
        """Events at or after |since| (seconds since the epoch). The log's
        times are whole milliseconds, so an event in the same millisecond
        as |since| counts, even when |since| is later within it."""
        self.poll()
        # (A microsecond of slack for the float product: 8.001 * 1000.)
        since = math.floor(since * 1000 + 1e-3) / 1000.0
        with self._lock:
            return [e for e in self._events if e["ts"] >= since and
                    (window is None or e.get("w") == window)]

    def last(self, kind, window=None):
        for e in reversed(self.events(0.0, window)):
            if e["e"] == kind:
                return e
        return None

    def kinds(self):
        """The grammar's command names, from the first window's start line
        (empty before it is written)."""
        for e in self.events():
            if e["e"] == "start" and isinstance(e.get("kinds"), list):
                return [k for k in e["kinds"] if isinstance(k, str)]
        return []

    def activity(self, window=None):
        """The last activity line (or None)."""
        return self.last("activity", window)


def parse_line(raw):
    """One event, or None for a line that isn't one."""
    if not raw or len(raw) > MAX_LINE:
        return None
    try:
        ev = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, ValueError):
        return None
    if not isinstance(ev, dict) or ev.get("e") not in EVENTS:
        return None
    t = ev.get("t")
    if isinstance(t, bool) or not isinstance(t, (int, float)):
        return None
    ev["ts"] = t / 1000.0
    for k in ("text", "intent", "source", "op", "kind", "activity", "what"):
        if k in ev and not isinstance(ev[k], str):
            ev[k] = str(ev[k])
    return ev


def fresh(path):
    """Removes an old log at |path| (the browser appends)."""
    try:
        os.remove(path)
    except FileNotFoundError:
        pass
