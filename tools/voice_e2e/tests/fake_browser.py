#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""A stand-in browser for the runner's tests: it speaks the DevTools pipe
(NUL-terminated JSON on fds 3 and 4) as the runner uses it, keeps a toy web
of the fixture sites' state, writes voice mode's test log as
voice_test_log.cc does, and runs a toy voice loop over what it is told:
typed commands through Agent mode's page (Runtime.evaluate of TYPED_JS),
and "heard" ones from <run dir>/fake-mic.txt, where the tests' player
writes what it plays.

It knows a handful of commands (the tests' scenarios), and leaves out
"bookmark_page" from its grammar's names, so a scenario needing it reports
"not in the grammar yet". "louder" does nothing, so a step fails.
"""

import json
import os
import re
import sys
import threading
import time
import urllib.parse

# How long the fake's follow-up window stays open after a spoken reply.
FOLLOW_UP_S = 1.0

KINDS = ["unknown", "refused", "open_site", "search", "choose", "pause", "play",
         "volume_up", "back", "stop", "yes", "no"]
PNG_1X1 = ("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAA"
           "AABJRU5ErkJggg==")


class Fake(object):
    def __init__(self, argv):
        self.log_path = None
        self.profile = None
        for a in argv:
            if a.startswith("--molt-voice-test-log="):
                self.log_path = a.split("=", 1)[1]
            elif a.startswith("--user-data-dir="):
                self.profile = a.split("=", 1)[1]
        self.run_dir = os.path.dirname(self.profile) if self.profile else "."
        home = os.environ.get("HOME", "")
        try:
            with open(os.path.join(home, ".moltbrowser", "settings.json")) as f:
                self.settings = json.load(f)
        except (OSError, ValueError):
            self.settings = {}
        self.targets = {"T1": {"url": "about:blank", "window": 1}}
        self.order = ["T1"]
        self.active = "T1"
        self.sessions = {}
        self.events = []
        self.media = None
        self.cart = 0
        self.nav_ms = time.time() * 1000
        self.wfd = 4
        self.lock = threading.Lock()
        self.started = False
        self.closing = False

    # ---- the test log ------------------------------------------------------

    def line(self, event, **fields):
        if not self.log_path:
            return
        d = {"t": int(time.time() * 1000), "w": 1, "e": event}
        d.update(fields)
        with open(self.log_path, "a") as f:
            f.write(json.dumps(d) + "\n")

    def start_log(self):
        if not self.started:
            self.started = True
            self.line("start", kinds=KINDS)

    # ---- the pipe -------------------------------------------------------------

    def send(self, msg):
        data = json.dumps(msg).encode("utf-8") + b"\0"
        with self.lock:
            while data:
                n = os.write(self.wfd, data)
                data = data[n:]

    def serve(self):
        if self.settings.get("voice_mode"):
            self.start_log()
            always = self.settings.get("voice_always_listening")
            self.waiting = not always
            self.window = 0
            self.line("activity", activity="listening", mic=True, waiting=not always)
            # As the browser does once its speech helper is ready.
            self.line("said", text="Voice mode is on. I'm listening." if always else
                      "Voice mode is on. Say Hey Molt, then what you want.")
            threading.Thread(target=self.listen, daemon=True).start()
        buf = b""
        while not self.closing:
            chunk = os.read(3, 1 << 16)
            if not chunk:
                break
            buf += chunk
            while b"\0" in buf:
                raw, buf = buf.split(b"\0", 1)
                msg = json.loads(raw.decode("utf-8"))
                try:
                    result = self.handle(msg)
                    self.send({"id": msg["id"], "result": result or {}})
                except Exception as err:  # reported as a DevTools error
                    self.send({"id": msg["id"], "error": {"message": str(err)}})

    def listen(self):
        mic = os.path.join(self.run_dir, "fake-mic.txt")
        pos = 0
        while not self.closing:
            try:
                with open(mic) as f:
                    f.seek(pos)
                    data = f.read()
                    pos = f.tell()
            except OSError:
                data = ""
            for text in data.splitlines():
                if text.strip():
                    time.sleep(0.3)  # the recognizer's end of speech
                    self.utterance(text.strip(), True)
            time.sleep(0.05)

    def handle(self, msg):
        m, p = msg["method"], msg.get("params", {})
        sid = msg.get("sessionId")
        if m == "Target.getTargets":
            return {"targetInfos": [{"targetId": t, "type": "page",
                                     "url": self.targets[t]["url"]} for t in self.order]}
        if m == "Target.attachToTarget":
            s = "S" + p["targetId"]
            self.sessions[s] = p["targetId"]
            return {"sessionId": s}
        if m in ("Target.setDiscoverTargets", "Page.enable", "Runtime.enable",
                 "Page.addScriptToEvaluateOnNewDocument"):
            return {}
        if m == "Browser.getWindowForTarget":
            return {"windowId": self.targets[p["targetId"]]["window"]}
        if m == "Browser.close":
            self.closing = True
            threading.Timer(0.1, lambda: os._exit(0)).start()
            return {}
        if m == "Target.createTarget":
            tid = "T%d" % (len(self.order) + 1)
            self.targets[tid] = {"url": p["url"], "window": 2 if p.get("newWindow") else 1}
            self.order.append(tid)
            if not p.get("background"):
                self.active = tid
            return {"targetId": tid}
        if m == "Target.activateTarget":
            self.active = p["targetId"]
            return {}
        if m == "Page.navigate":
            tid = self.sessions[sid]
            self.go(p["url"], tid)
            threading.Timer(0.05, lambda: self.send(
                {"method": "Page.loadEventFired", "sessionId": sid, "params": {}})).start()
            return {"frameId": "F"}
        if m == "Page.captureScreenshot":
            return {"data": PNG_1X1}
        if m == "Runtime.evaluate":
            return {"result": {"type": "object", "value": self.evaluate(
                self.sessions.get(sid), p["expression"])}}
        raise ValueError("unknown method %s" % m)

    # ---- the toy web ---------------------------------------------------------

    def go(self, url, tid=None):
        tid = tid or self.active
        self.targets[tid]["url"] = url
        self.nav_ms = time.time() * 1000
        if "/watch" in url:
            self.media = {"paused": True, "muted": False, "volume": 0.7, "rate": 1,
                          "time": 0, "src": "song.wav", "captions": False,
                          "fullscreen": False}
        else:
            self.media = None

    def evaluate(self, tid, expr):
        if "__moltE2E ? window.__moltE2E.events" in expr:
            return json.dumps(self.events)
        if expr.startswith("(() => {\n  if (window.__moltE2E)"):
            return None
        if expr == "document.visibilityState":
            return "visible" if tid == self.active else "hidden"
        if expr == "performance.timeOrigin":
            return self.nav_ms
        if "document.querySelector('video, audio')" in expr:
            return self.media
        if "sendWithPromise('voiceCommand'" in expr:
            text = json.loads(re.search(r"sendWithPromise\('voiceCommand', (.*)\);", expr).group(1))
            threading.Thread(target=self.utterance, args=(text, False), daemon=True).start()
            return "ok"
        if expr == "shop.items().length":
            return self.cart
        raise ValueError("the fake can't evaluate %r" % expr[:80])

    # ---- the toy voice loop ---------------------------------------------------

    def say(self, text):
        self.line("said", text=text)

    def utterance(self, text, mic):
        self.start_log()
        # Whether voice mode waited for the wake phrase when this was said
        # (the tests check the kit waited for the follow-up window to close).
        was_waiting = getattr(self, "waiting", False)
        self.window = getattr(self, "window", 0) + 1
        words = re.sub(r"^(hey molt,?\s*)", "", text, flags=re.I).strip()
        low = words.lower()
        intent = "unknown"
        if low.startswith("open youtube"):
            intent = "open_site"
        elif low.startswith("search for"):
            intent = "search"
        elif low.startswith("play the") or low.startswith("click"):
            intent = "choose"
        elif low in ("pause", "play", "louder", "go back", "stop"):
            intent = {"pause": "pause", "play": "play", "louder": "volume_up",
                      "go back": "back", "stop": "stop"}[low]
        self.line("heard", text=text, source="mic" if mic else "typed", clean=True,
                  intent=intent, waiting=was_waiting)
        self.line("activity", activity="thinking", mic=mic, waiting=False)
        time.sleep(0.1)
        if intent == "open_site":
            self.go("https://www.youtube.com/")
            self.say("YouTube is open.")
        elif intent == "search":
            q = words[len("search for "):]
            self.go("https://www.youtube.com/results?search_query=" + urllib.parse.quote(q))
            self.say("20 results. 1: Queen – Bohemian Rhapsody, Queen Official, 6 minutes.")
        elif intent == "choose" and "fourth" in low:
            self.go("https://www.youtube.com/watch?v=video4")
            self.events.append({"type": "click", "t": int(time.time() * 1000),
                                "trusted": True, "target": "video"})
            self.media["paused"] = False
            self.say("Playing Queen - Bohemian Rhapsody (Live Aid 1985).")
        elif intent == "choose" and "add kettle" in low:
            self.cart += 1
            self.events.append({"type": "click", "t": int(time.time() * 1000),
                                "trusted": True, "target": "kettle"})
            self.say("Pressed Add kettle to cart.")
        elif intent == "pause" and self.media:
            self.media["paused"] = True
            self.events.append({"type": "pause", "t": int(time.time() * 1000),
                                "trusted": True, "target": "VIDEO"})
            self.say("Paused.")
        elif intent == "volume_up":
            pass  # the step fails, as the tests want
        else:
            self.say("I can't do that yet.")
        self.line("activity", activity="speaking", mic=mic, waiting=False)
        time.sleep(0.2)
        self.line("activity", activity="listening", mic=mic, waiting=False)
        if mic and not self.settings.get("voice_always_listening"):
            # The follow-up window, then the wake phrase again.
            self.waiting = False
            window = self.window

            def close():
                time.sleep(FOLLOW_UP_S)
                if window == self.window and not self.closing:
                    self.waiting = True
                    self.line("activity", activity="listening", mic=True, waiting=True)
            threading.Thread(target=close, daemon=True).start()


def main():
    Fake(sys.argv[1:]).serve()


if __name__ == "__main__":
    main()
