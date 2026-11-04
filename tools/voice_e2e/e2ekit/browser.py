# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The browser under test: launched with a fresh profile and HOME, voice
mode on, the test log and the fixture sites; driven over the DevTools pipe
(--remote-debugging-pipe, the leak kit's client: only the process that
launched the browser can reach it, and it opens no port).

The kit never acts on a page itself: it navigates to a scenario's start
page, opens its extra tabs and reads the page's state, always without a
user gesture (Runtime.evaluate with userGesture false), so whatever
activation a page gets comes from voice mode.

Every page target is attached to; a recorder (RECORDER_JS) notes each page
event that shows an action (a trusted click, input, a player's change) with
its time, for no_action and for the latency from the end of speech to the
action.
"""

import base64
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
LEAK = os.path.join(REPO, "tools", "leak_test")
if LEAK not in sys.path:
    sys.path.insert(0, LEAK)

from leakkit import cdp as cdplib  # noqa: E402

from . import expect  # noqa: E402

RECORDER_JS = r"""(() => {
  if (window.__moltE2E) { return; }
  const E = window.__moltE2E = {events: []};
  const types = ['click', 'input', 'change', 'submit', 'keydown', 'play',
                 'pause', 'volumechange', 'ratechange', 'seeking',
                 'fullscreenchange'];
  for (const type of types) {
    window.addEventListener(type, (e) => {
      if (E.events.length < 5000) {
        const t = e.target || {};
        E.events.push({type, t: Date.now(), trusted: e.isTrusted,
                       target: String(t.id || t.tagName || '')});
      }
    }, {capture: true, passive: true});
  }
})();"""

EVENTS_JS = "JSON.stringify(window.__moltE2E ? window.__moltE2E.events : [])"
NAV_START_JS = "performance.timeOrigin"
VISIBLE_JS = "document.visibilityState"
AGENT_URL = "chrome://molt-ai-agent/"
# What the side panel shows (AI chat, Agent mode): never the active tab.
SIDE_PANEL_HOSTS = ("chrome://molt-ai-chat", "chrome://molt-ai-agent",
                    "chrome://read-later", "chrome-untrusted://")
TYPED_JS = """(async () => {
  for (let i = 0; i < 100 && typeof sendWithPromise !== 'function'; ++i) {
    await new Promise((r) => setTimeout(r, 50));
  }
  const r = await sendWithPromise('voiceCommand', %s);
  return r && r.ok ? 'ok' : String((r && r.error) || 'refused');
})()"""


class BrowserError(Exception):
    pass


def install_bundle(bundle_dir, browser_exe, log=print):
    """Copies the speech and voice helpers (scripts/bundle-speech.sh's
    output: <bundle>/speech, <bundle>/voice) next to the browser, where it
    looks for them on Linux (<exe dir>/speech, <exe dir>/voice). A copy, not
    a link: the browser takes only a bundled copy it can trust."""
    out = os.path.dirname(os.path.abspath(browser_exe))
    done = []
    for part in ("speech", "voice"):
        src = os.path.join(bundle_dir, part)
        if not os.path.isdir(src):
            continue
        dst = os.path.join(out, part)
        info_src = os.path.join(src, ".build-info")
        info_dst = os.path.join(dst, ".build-info")
        if os.path.exists(info_dst) and os.path.exists(info_src):
            with open(info_src, "rb") as a, open(info_dst, "rb") as b:
                if a.read() == b.read():
                    done.append(part)
                    continue
        if os.path.exists(dst):
            shutil.rmtree(dst)
        shutil.copytree(src, dst, symlinks=False)
        done.append(part)
        log("installed %s/ next to the browser" % part)
    return done


def seed_profile(profile_dir, download_dir):
    os.makedirs(os.path.join(profile_dir, "Default"), mode=0o700, exist_ok=True)
    prefs = {
        "browser": {"has_seen_welcome_page": True},
        "download": {"default_directory": download_dir, "prompt_for_download": False,
                     "directory_upgrade": True},
        "intl": {"accept_languages": "en-US,en"},
        "profile": {"exit_type": "Normal"},
    }
    with open(os.path.join(profile_dir, "Default", "Preferences"), "w") as f:
        json.dump(prefs, f)


def write_settings(home, settings):
    d = os.path.join(home, ".moltbrowser")
    os.makedirs(d, mode=0o700, exist_ok=True)
    with open(os.path.join(d, "settings.json"), "w") as f:
        json.dump(settings, f)


class Browser(object):
    def __init__(self, exe, run_dir, flags=(), env=None, log=print):
        self.exe = exe
        self.dir = run_dir
        self.flags = list(flags)
        self.env = dict(env or {})
        self.log = log
        self.proc = None
        self.cdp = None
        self.sessions = {}        # targetId -> sessionId
        self.windows = {}         # targetId -> windowId
        self.main_window = None
        self.agent_target = None
        self.download_dir = os.path.join(run_dir, "downloads")

    # ---- launch -------------------------------------------------------

    def launch(self, timeout=60):
        os.makedirs(self.download_dir, exist_ok=True)
        cmd_r, cmd_w = os.pipe()
        resp_r, resp_w = os.pipe()
        logf = open(os.path.join(self.dir, "browser.log"), "w")

        def remap():
            os.dup2(cmd_r, 3)
            os.dup2(resp_w, 4)

        args = [self.exe, "--remote-debugging-pipe"] + self.flags + ["about:blank"]
        try:
            self.proc = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=logf,
                                         stderr=logf, env=self.env, preexec_fn=remap,
                                         pass_fds=(3, 4), start_new_session=True)
        finally:
            logf.close()
        os.close(cmd_r)
        os.close(resp_w)
        self.cdp = cdplib.Cdp(cmd_w, resp_r)
        with open(os.path.join(self.dir, "browser-args.txt"), "w") as f:
            f.write("\n".join(args) + "\n")
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise BrowserError("the browser exited (%s); see %s/browser.log"
                                   % (self.proc.returncode, self.dir))
            try:
                pages = [t for t in self.cdp.targets() if t.get("type") == "page"]
                if pages:
                    self.cdp.call("Target.setDiscoverTargets", {"discover": True})
                    first = pages[0]["targetId"]
                    self._attach(first)
                    self.main_window = self.window_of(first)
                    return self
            except cdplib.CdpClosed:
                break
            except cdplib.CdpError:
                pass
            time.sleep(0.3)
        raise BrowserError("the browser didn't start; see %s/browser.log" % self.dir)

    def close(self):
        if self.cdp and not self.cdp.closed:
            try:
                self.cdp.call("Browser.close", timeout=10)
            except cdplib.CdpError:
                pass
        if self.proc:
            try:
                self.proc.wait(timeout=15)
            except subprocess.TimeoutExpired:
                try:
                    os.killpg(self.proc.pid, 9)
                except OSError:
                    pass
                self.proc.wait(timeout=10)

    # ---- targets ------------------------------------------------------

    def _attach(self, target_id):
        if target_id in self.sessions:
            return self.sessions[target_id]
        sid = self.cdp.attach(target_id)
        try:
            self.cdp.call("Page.addScriptToEvaluateOnNewDocument",
                          {"source": RECORDER_JS}, session=sid, timeout=10)
            self.cdp.evaluate(sid, RECORDER_JS, timeout=10)
        except cdplib.CdpError:
            pass
        self.sessions[target_id] = sid
        return sid

    def pages(self):
        """The page targets of the browser's windows (not the kit's own
        agent window), attached."""
        out = []
        for t in self.cdp.targets():
            if t.get("type") != "page" or t["targetId"] == self.agent_target:
                continue
            if (t.get("url") or "").startswith("devtools://"):
                continue
            self._attach(t["targetId"])
            out.append(t)
        return out

    def window_of(self, target_id):
        if target_id not in self.windows:
            try:
                r = self.cdp.call("Browser.getWindowForTarget", {"targetId": target_id})
                self.windows[target_id] = r.get("windowId")
            except cdplib.CdpError:
                self.windows[target_id] = None
        return self.windows[target_id]

    def main_pages(self):
        return [t for t in self.pages() if self.window_of(t["targetId"]) == self.main_window]

    def active(self):
        """The main window's active tab (its target info), or None: the
        visible page that isn't a side panel's."""
        pages = self.main_pages()
        if len(pages) == 1:
            return pages[0]
        visible = []
        for t in pages:
            try:
                if self.cdp.evaluate(self.sessions[t["targetId"]], VISIBLE_JS,
                                     timeout=5) == "visible":
                    visible.append(t)
            except cdplib.CdpError:
                continue
        tabs = [t for t in visible if not any(
            h in (t.get("url") or "") for h in SIDE_PANEL_HOSTS)]
        for group in (tabs, visible, pages):
            if group:
                return group[0]
        return None

    def navigate(self, url, timeout=30):
        t = self.active()
        if not t:
            raise BrowserError("no tab to navigate")
        err = self.cdp.navigate(self.sessions[t["targetId"]], url, timeout=timeout)
        if err:
            raise BrowserError("couldn't open %s: %s" % (url, err))

    def open_tab(self, url):
        r = self.cdp.call("Target.createTarget", {"url": url, "background": True})
        self._attach(r["targetId"])
        return r["targetId"]

    def activate(self, target_id):
        self.cdp.call("Target.activateTarget", {"targetId": target_id})

    def evaluate_active(self, expression, timeout=10):
        t = self.active()
        if not t:
            raise BrowserError("no tab")
        return self.cdp.evaluate(self.sessions[t["targetId"]], expression,
                                 timeout=timeout, user_gesture=False)

    # ---- typed mode -----------------------------------------------------

    def open_agent(self):
        """Agent mode's page in a window of its own, behind the browser's:
        its "Type what you would say" field runs a command in the last
        active window's active tab."""
        main = self.active()
        r = self.cdp.call("Target.createTarget",
                          {"url": AGENT_URL, "newWindow": True, "background": True})
        self.agent_target = r["targetId"]
        self._attach(self.agent_target)
        # Out of the way, so the browser's window is the last active one.
        try:
            wid = self.cdp.call("Browser.getWindowForTarget",
                                {"targetId": self.agent_target}).get("windowId")
            self.cdp.call("Browser.setWindowBounds",
                          {"windowId": wid, "bounds": {"windowState": "minimized"}})
        except cdplib.CdpError:
            pass
        if main:
            self.activate(main["targetId"])

    def type_command(self, text):
        if not self.agent_target:
            self.open_agent()
        sid = self.sessions[self.agent_target]
        r = self.cdp.evaluate(sid, TYPED_JS % json.dumps(text), timeout=15,
                              user_gesture=False)
        agent = [t for t in self.cdp.targets() if t["targetId"] == self.agent_target]
        if agent and not (agent[0].get("url") or "").startswith(AGENT_URL):
            raise BrowserError("the typed command acted in Agent mode's own window, "
                               "not the browser's (its window didn't become the last "
                               "active one): run typed mode with a window manager")
        return r

    # ---- the world (expect.py) -----------------------------------------

    def url(self):
        t = self.active()
        return t.get("url") if t else None

    def evaluate(self, expression):
        return self.evaluate_active(expression)

    def media(self):
        return self.evaluate_active(expect.MEDIA_JS)

    def tab_count(self):
        return len(self.main_pages())

    def window_count(self):
        return len({self.window_of(t["targetId"]) for t in self.pages()})

    def target_urls(self):
        return [t.get("url") or "" for t in self.cdp.targets()
                if t.get("targetId") != self.agent_target]

    def downloaded(self, name):
        p = os.path.join(self.download_dir, name)
        return os.path.isfile(p) and not os.path.exists(p + ".crdownload")

    def page_events(self, since):
        out = []
        for t in self.pages():
            try:
                raw = self.cdp.evaluate(self.sessions[t["targetId"]], EVENTS_JS, timeout=5)
                for ev in json.loads(raw or "[]"):
                    if isinstance(ev, dict) and ev.get("t", 0) / 1000.0 >= since:
                        out.append(ev)
            except (cdplib.CdpError, ValueError):
                continue
        return out

    def navigation_start(self):
        """When the active tab's document began loading (seconds)."""
        try:
            return float(self.evaluate_active(NAV_START_JS)) / 1000.0
        except (cdplib.CdpError, TypeError, ValueError, BrowserError):
            return None

    def screenshot(self, path):
        t = self.active()
        if not t:
            return False
        try:
            r = self.cdp.call("Page.captureScreenshot", {"format": "png"},
                              session=self.sessions[t["targetId"]], timeout=15)
            with open(path, "wb") as f:
                f.write(base64.b64decode(r.get("data", "")))
            return True
        except (cdplib.CdpError, ValueError, OSError):
            return False
