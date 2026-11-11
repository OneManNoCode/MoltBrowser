# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Runs scenarios against a browser and records what happened.

One run is one way of speaking: the audio path ("pulse", "file", or
"typed": the same words typed into Agent mode's voice field, to tell what
was misheard from what went wrong after) and a condition ("clean", or
"music10": music 10 dB under the speech). Each scenario gets a fresh
browser, profile and HOME; its voice and speed rotate through the
renderer's, so a run uses all of them.

For each step: say it (or type it), take the time its speech ended, then
poll its expectations until all hold or its timeout passes, and measure
from the end of speech to what voice mode heard, to the first action on
the page (a page event, a new document, a new tab) and to what it said.
A step that fails ends its scenario (the rest would only fail after it);
the report names why, with a screenshot of the tab.
"""

import json
import os
import re
import sys
import time
import traceback

from . import audio_path
from . import browser as browserlib
from . import expect
from . import render
from . import testlog
from .scenarios import QUIET_KINDS

SETTLE_S = 1.5        # quiet after the browser speaks (> kGestureQuietMs)
LISTEN_TIMEOUT_S = 45
IDLE_TIMEOUT_S = 60
# The longest wait for voice mode's follow-up window to close.
WINDOW_TIMEOUT_S = 30
# The longest wait for voice mode to answer what the kit said (the end of
# speech, a short command's wait, the answer).
REPLY_TIMEOUT_S = 10
POLL_S = 0.2
# File mode's fixed times (the recording can't wait on the browser): past
# voice mode's greeting ("Voice mode is on. Say Hey Molt, then what you
# want.", which ends about 7 s after the microphone opens, once the speech
# helper is ready), and past the usual answer before the next utterance:
# what is said over voice mode's own speech can be dropped ("I've stopped.
# Now say that again."). Nor may "Hey Molt" land on the tone voice mode
# plays as its follow-up window closes, 8 s after an answer
# (kFollowUpWindowMs): over the fake output device the echo canceller takes
# the phrase's start with it, which a pulse run avoids by waiting for the
# tone. A 12 s gap put that tone under the next "Hey Molt" for every answer
# of about 3 s ("Your name field. Say type, then your text."); at 14 s an
# answer of up to 4 s, most of them, ends its window well before it.
FILE_LEAD_S = 11.0
FILE_GAP_S = 14.0
# A file-mode step's events count from this long before its speech begins
# (the log's whole milliseconds, the browser's clock against the kit's).
FILE_STEP_SLACK_S = 0.3
DEFAULT_QUIET_S = 8.0
WORD_TRIES = 3


# What voice mode's microphone gave in a pulse run (--molt-voice-test-audio):
# raw 16 kHz mono 16-bit, in the scenario's run directory.
MIC_DUMP = "mic.pcm"
# How much of it the kit's recognizer hears back for a failed scenario.
MIC_DUMP_MAX_S = 90


def recording_levels(plan, index):
    """File mode: the levels onset_levels() gives for step |index|'s speech
    in the WAV the fake device played (plan["file_wav"], its first
    channel), each 100 ms from 0.5 s before its onset; [] otherwise."""
    import array
    import wave
    if not plan or not plan.get("file_wav") or index not in plan.get("times", {}):
        return []
    try:
        with wave.open(plan["file_wav"], "rb") as w:
            rate, channels = w.getframerate(), w.getnchannels()
            if w.getsampwidth() != 2:
                return []
            frames = w.readframes(w.getnframes())
    except (OSError, EOFError, wave.Error):
        return []
    samples = array.array("h")
    samples.frombytes(frames[:len(frames) // 2 * 2])
    if sys.byteorder != "little":
        samples.byteswap()
    first = samples[::channels] if channels > 1 else samples
    if sys.byteorder != "little":
        first.byteswap()
    a = plan["times"][index][0]
    return onset_levels(first.tobytes(), a * rate, rate=rate, search_s=1.0)[1]


def replay_lines(events, at_s, before_s=1.0, after_s=9.0, limit=40):
    """The replayed helper's events (Ears.replay()) from |before_s| before
    the failed step's speech (|at_s| into the dump) to |after_s| after:
    "27.15 wake", "27.60 partial 'He mold'", the time in the dump."""
    out = []
    for ms, kind, text in events:
        if not isinstance(ms, (int, float)) or not isinstance(kind, str):
            continue
        if not (at_s - before_s) * 1000 <= ms <= (at_s + after_s) * 1000:
            continue
        out.append("%.2f %s%s" % (ms / 1000.0, kind, " %r" % text[:80] if text else ""))
        if len(out) >= limit:
            break
    return out


def onset_levels(pcm, around, rate=16000, search_s=3.0, before_s=0.5, after_s=2.0,
                 step_s=0.1, onset_db=-30):
    """Around sample |around| of |pcm| (16-bit little-endian): where speech
    starts, the first 10 ms within |search_s| of it with a peak of
    |onset_db| or more, and each |step_s|'s peak in dBFS from |before_s|
    before it to |after_s| after (-120 for silence). (None, []) when nothing
    is that loud there. The dump starts a moment after the microphone
    opens, so a step's time is only roughly where its speech is."""
    import array
    import math
    samples = array.array("h")
    samples.frombytes(pcm[:len(pcm) // 2 * 2])
    if sys.byteorder != "little":
        samples.byteswap()
    level = 32768.0 * 10 ** (onset_db / 20.0)
    ten = rate // 100
    lo = max(0, int(around - search_s * rate))
    hi = min(len(samples), int(around + search_s * rate))
    onset = None
    for at in range(lo, hi, ten):
        if max((abs(x) for x in samples[at:at + ten]), default=0) >= level:
            onset = at
            break
    if onset is None:
        return None, []
    out = []
    step = int(step_s * rate)
    for at in range(max(0, onset - int(before_s * rate)),
                    min(len(samples), onset + int(after_s * rate)), step):
        peak = max((abs(x) for x in samples[at:at + step]), default=0)
        out.append(round(20 * math.log10(peak / 32768.0)) if peak else -120)
    return onset / float(rate), out


def mic_levels(pcm, rate=16000):
    """The peak of each second of |pcm| (16-bit little-endian), in dBFS
    (-120 for silence)."""
    import array
    import math
    samples = array.array("h")
    samples.frombytes(pcm[:len(pcm) // 2 * 2])
    if sys.byteorder != "little":
        samples.byteswap()
    out = []
    for at in range(0, len(samples), rate):
        peak = max((abs(x) for x in samples[at:at + rate]), default=0)
        out.append(round(20 * math.log10(peak / 32768.0)) if peak else -120)
    return out


# Chromium's Linux audio files whose VLOG(1) the pulse runs log.
AUDIO_VLOG_MODULES = ("audio_manager_linux", "audio_manager_pulse", "pulse_util",
                      "pulse_input", "audio_manager_alsa", "alsa_input",
                      "input_controller", "input_stream")


class RunConfig(object):
    def __init__(self, audio="pulse", condition="clean", loopback=True,
                 echo_cancellation=True,
                 voices=None, speeds=None, settle=SETTLE_S, sandbox=True,
                 extra_flags=(), model_available=False):
        if audio not in ("pulse", "file", "typed"):
            raise ValueError("audio is pulse, file or typed")
        if condition not in render.CONDITIONS:
            raise ValueError("condition is one of %s" % ", ".join(render.CONDITIONS))
        self.audio = audio
        self.condition = "clean" if audio == "typed" else condition
        self.loopback = loopback
        # False: the browser without Chrome-wide echo cancellation (the
        # microphone then has none on Linux).
        self.echo_cancellation = echo_cancellation
        self.voices = list(voices or [v[0] for v in render.VOICES])
        self.speeds = list(speeds or render.SPEEDS)
        self.settle = settle
        self.sandbox = sandbox
        self.extra_flags = list(extra_flags)
        self.model_available = model_available

    @property
    def label(self):
        if self.audio == "typed":
            return "typed"
        # A PulseAudio run without the speaker-to-microphone loopback.
        suffix = "-noloop" if self.audio == "pulse" and not self.loopback else ""
        if self.audio == "pulse" and not self.echo_cancellation:
            suffix += "-noaec"
        return "%s/%s%s" % (self.audio, self.condition, suffix)

    @property
    def typed(self):
        return self.audio == "typed"


# A page's own dialogs (alert(), confirm(), prompt(), leaving the page):
# while one is open its page runs no script, and a look at it (Runtime.
# evaluate) waits until the dialog closes or the look times out.
PAGE_DIALOG_KINDS = ("alert", "confirm", "prompt", "beforeunload")


class World(object):
    """What expect.py looks at: the browser and its test log."""

    def __init__(self, browser, log):
        self.browser = browser
        self.log = log

    def page_dialog_open(self):
        """Whether the test log shows a page's dialog open now."""
        shown = {}
        for e in self.log.events(0):
            if e.get("e") != "prompt":
                continue
            if e.get("op") == "open":
                shown[e.get("id")] = e.get("kind") in PAGE_DIALOG_KINDS
            elif e.get("op") == "close":
                shown.pop(e.get("id"), None)
        return any(shown.values())

    def url(self):
        return self.browser.url()

    def evaluate(self, expression):
        return self.browser.evaluate(expression)

    def media(self):
        return self.browser.media()

    def log_events(self, since):
        return self.log.events(since)

    def tab_count(self):
        return self.browser.tab_count()

    def window_count(self):
        return self.browser.window_count()

    def downloaded(self, name):
        return self.browser.downloaded(name)

    def page_events(self, since):
        return self.browser.page_events(since)

    def target_urls(self):
        return self.browser.target_urls()


def is_quiet(e):
    """Whether |e| is checked once, at the end of its quiet period."""
    return "quiet" in e or any(k in e for k in QUIET_KINDS)


def unmet(requirements, config, pulse_ok):
    """Why a scenario can't run in |config| (or "")."""
    if "microphone" in requirements and config.typed:
        return "needs the microphone (typed text never answers with a word)"
    if "listening" in requirements and config.typed:
        return ("needs voice mode listening in its window (a typed command "
                "runs in the last active window)")
    if "pulse" in requirements and (config.audio != "pulse" or not pulse_ok):
        return "needs live audio (PulseAudio)"
    if "model" in requirements and not config.model_available:
        return "needs an on-device AI model"
    return ""


def normalized(text):
    import voice_bench_lib as vbl  # tools/voice_bench, on the path via render
    return vbl.normalize(text)


def word_errors(ref, hyp):
    import voice_bench_lib as vbl
    r, h = normalized(ref).split(), normalized(hyp).split()
    return vbl.edit_distance(r, h), len(r)


# How the bundled recognizer writes "hey molt", apart or run together
# ("him old", "Himalt"): what precedes the command it heard.
WAKE_WORDS = ("hey", "hi", "hay", "a", "he", "him", "molt", "mold", "malt",
              "moult", "melt", "old", "alled", "himalt", "himold", "himaled",
              "himalled", "heymolt")


def strip_wake(text):
    words = normalized(text).split()
    i = 0
    while i < len(words) and i < 3 and words[i] in WAKE_WORDS:
        i += 1
    return " ".join(words[i:])


class Runner(object):
    def __init__(self, browser_exe, speech_dir, report_dir, server, spki, config,
                 pulse=None, ears=None, display=None, log=print, browser_factory=None,
                 player=None):
        self.exe = browser_exe
        self.speech = speech_dir
        self.report_dir = report_dir
        self.server = server
        self.spki = spki
        self.config = config
        self.pulse = pulse
        self.ears = ears
        self.display = display
        self.log = log
        self.browser_factory = browser_factory or browserlib.Browser
        # How an utterance reaches the browser in "pulse" runs (tests swap it).
        self.player = player

    # ---- the browser's launch -------------------------------------------

    def flags(self, log_path, wav=None, shields=True):
        f = ["--user-data-dir=%s", "--no-first-run", "--no-default-browser-check",
             "--password-store=basic", "--disable-search-engine-choice-screen",
             "--enable-features=MoltVoiceMode",
             "--molt-voice-test-log=" + log_path,
             "--host-resolver-rules=" + self.server_rules(),
             "--ignore-certificate-errors-spki-list=" + self.spki,
             "--autoplay-policy=document-user-activation-required",
             "--window-size=1280,900", "--lang=en-US"]
        if not self.config.sandbox:
            f.append("--no-sandbox")
        if not self.config.echo_cancellation:
            f.append("--disable-features=ChromeWideEchoCancellation")
        if not shields:
            # MoltShield off for this run: what a page shows that its
            # filters would hide (a YouTube ad's skip button) is there.
            f.append("--disable-molt-shield")
        if wav or self.config.audio == "pulse":
            # What voice mode's microphone gave, as its speech helper gets
            # it after the echo canceller (voice_test_audio.h), for the
            # digest: in file mode too, where the file is what was said.
            f.append("--molt-voice-test-audio=" +
                     os.path.join(os.path.dirname(log_path), MIC_DUMP))
        if wav:
            f += ["--use-fake-device-for-media-stream",
                  "--use-file-for-fake-audio-capture=%s%%noloop" % wav]
        elif self.config.audio == "pulse":
            # What Chromium's audio manager opens (PulseAudio or ALSA, which
            # device), for the digest's audio lines.
            f += ["--enable-logging=stderr", "--vmodule=" + ",".join(
                "%s=1" % m for m in AUDIO_VLOG_MODULES)]
        return f + self.config.extra_flags

    def server_rules(self):
        from . import fixtures
        return fixtures.host_rules(self.server.port)

    def env(self, run_dir):
        home = os.path.join(run_dir, "home")
        xdg = os.path.join(run_dir, "xdg")
        os.makedirs(home, mode=0o700, exist_ok=True)
        os.makedirs(xdg, mode=0o700, exist_ok=True)
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": home,
               "XDG_RUNTIME_DIR": xdg, "LANG": "en_US.UTF-8", "TZ": "UTC"}
        if self.display:
            env["DISPLAY"] = self.display
        if self.pulse and self.config.audio == "pulse":
            env.update(self.pulse.env())
        return env

    # ---- waiting on the browser -------------------------------------------

    def wait_listening(self, log, timeout=LISTEN_TIMEOUT_S):
        deadline = time.time() + timeout
        while time.time() < deadline:
            a = log.activity()
            if a and a.get("mic"):
                return a["ts"]
            time.sleep(POLL_S)
        return None

    def wait_greeting(self, log, timeout=LISTEN_TIMEOUT_S):
        """Until voice mode has said something (its greeting, "Voice mode is
        on...", which it says once its speech helper is ready)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if any(e["e"] == "said" for e in log.events(0)):
                return True
            time.sleep(POLL_S)
        return False

    def wait_waiting(self, log, timeout=WINDOW_TIMEOUT_S):
        """Until voice mode waits for the wake phrase (its follow-up window
        closed), as when the user says "Hey Molt" after a pause: the step is
        then heard by the wake phrase, not as a follow-up."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            a = log.activity()
            if a is not None and a.get("activity") == "listening" and a.get("waiting"):
                return True
            time.sleep(POLL_S)
        return False

    def wait_idle(self, log, timeout=IDLE_TIMEOUT_S):
        """Until voice mode has been listening (not thinking or speaking)
        for the settle time, and said nothing in it."""
        settle = self.config.settle
        deadline = time.time() + timeout
        while time.time() < deadline:
            a = log.activity()
            now = time.time()
            said = [e for e in log.events(now - settle) if e["e"] == "said"]
            if (a is None or a.get("activity") in ("listening", "off")) and not said and \
                    (a is None or now - a["ts"] >= settle):
                return True
            time.sleep(POLL_S)
        return False

    def wait_reply(self, log, since, timeout=REPLY_TIMEOUT_S):
        """Until voice mode says something, or starts to, after |since|."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            for e in log.events(since):
                if e["e"] == "said" or (e["e"] == "activity" and
                                        e.get("activity") in ("thinking", "speaking")):
                    return True
            time.sleep(POLL_S)
        return False

    def asked_again(self, log, since, timeout=REPLY_TIMEOUT_S):
        """Whether voice mode's first answer after |since| asks for the
        confirmation word again ("I didn't catch that. Say the word
        again.")."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            said = [e for e in log.events(since) if e["e"] == "said"]
            if said:
                return "say the word again" in (said[0].get("text") or "").lower()
            time.sleep(POLL_S)
        return False

    # ---- one scenario -------------------------------------------------------

    def voice_for(self, index, pinned=None):
        """The voice and speed of the |index|th scenario run: the voices in
        turn, or the scenario's own voice (Scenario.voice) when the run has
        it. The speed turns either way."""
        voices, speeds = self.config.voices, self.config.speeds
        voice = pinned if pinned in voices else voices[index % len(voices)]
        return voice, speeds[(index // len(voices)) % len(speeds)]

    def run_scenario(self, sc, mode, index):
        voice, speed = self.voice_for(index, sc.voice)
        res = {"id": sc.id, "title": sc.title, "task": sc.task, "mode": mode,
               "counts": sc.counts, "steps": [],
               "voice": None if self.config.typed else voice,
               "speed": None if self.config.typed else speed,
               "needs": sc.needs(), "status": "passed", "reason": ""}
        why = unmet(sc.all_requirements(), self.config, self.pulse is not None)
        if why:
            res.update(status="skipped", reason=why)
            return res
        name = sc.id + ("" if mode == "wake" else "-" + mode)
        run_dir = os.path.join(self.report_dir, "runs", self.config.label.replace("/", "-"),
                               name)
        os.makedirs(run_dir, exist_ok=True)
        log_path = os.path.join(run_dir, "voice.jsonl")
        testlog.fresh(log_path)
        testlog.fresh(os.path.join(run_dir, MIC_DUMP))  # the browser appends
        tlog = testlog.TestLog(log_path)
        t_start = time.time()
        browser = None
        plan = None
        try:
            plan = self.prepare_audio(sc, mode, voice, speed, run_dir)
            browserlib.seed_profile(os.path.join(run_dir, "profile"),
                                    os.path.join(run_dir, "downloads"))
            settings = {"voice_mode": not self.config.typed,
                        "voice_always_listening": mode == "always",
                        "voice_language": "en"}
            settings.update(sc.start.get("settings", {}))
            browserlib.write_settings(os.path.join(run_dir, "home"), settings)
            flags = self.flags(log_path, plan.get("file_wav"),
                               shields=sc.start.get("shields", True))
            flags[0] = flags[0] % os.path.join(run_dir, "profile")
            browser = self.browser_factory(self.exe, run_dir, flags, self.env(run_dir),
                                           log=self.log)
            browser.launch()
            world = World(browser, tlog)
            stream_start = None
            if not self.config.typed:
                stream_start = self.wait_listening(tlog)
                if stream_start is None:
                    raise RuntimeError("voice mode didn't start listening (no microphone "
                                       "line in the test log; see browser.log)")
            else:
                browser.open_agent()
            if self.config.audio == "pulse" and self.pulse:
                plan["rec_t0"] = self.pulse.start_recording(plan["recording"])
                if self.config.condition != "clean":
                    self.pulse.start_bed(plan["bed"])
            self.start_page(browser, sc)
            if self.config.audio == "pulse":
                # As a user does: speak once voice mode has said it's on.
                # The microphone is on before its speech helper is ready,
                # and what is said until then reaches no recognizer.
                if not self.wait_greeting(tlog):
                    raise RuntimeError("voice mode never said it was on (no said "
                                       "line in the test log; see browser.log)")
            if self.config.audio != "file":
                self.wait_idle(tlog)
            ok = True
            for i, step in enumerate(sc.steps):
                if not ok:
                    skipped = {"index": i, "action": step.action,
                               "status": "not run",
                               "detail": "an earlier step failed"}
                    if step.guards():
                        skipped["guard"] = True
                    res["steps"].append(skipped)
                    continue
                if self.config.audio == "pulse" and step.action == "say" and \
                        self.wake_for(step, mode) and not step.barge_in:
                    # Then the settle time, as a user waits for the tone
                    # voice mode plays as it starts waiting: said over the
                    # tone, the phrase's start is lost to the echo canceller.
                    if self.wait_waiting(tlog):
                        self.wait_idle(tlog)
                sres = self.run_step(sc, step, i, world, tlog, plan, stream_start, mode,
                                     run_dir)
                res["steps"].append(sres)
                if sres["status"] != "passed":
                    ok = False
                    res["status"] = sres["status"]
                    res["reason"] = "step %d (%s): %s" % (i + 1, sres.get("say") or step.action,
                                                          sres.get("detail", ""))
                elif self.config.audio != "file" and i + 1 < len(sc.steps) and \
                        not sc.steps[i + 1].barge_in:
                    self.wait_idle(tlog)
        except Exception as err:  # the scenario's own problem; the run goes on
            res.update(status="error", reason="%s: %s" % (type(err).__name__, err))
            with open(os.path.join(run_dir, "error.txt"), "w") as f:
                f.write(traceback.format_exc())
        finally:
            if self.pulse and self.config.audio == "pulse":
                self.pulse.stop_bed()
                self.pulse.stop_recording()
            if browser:
                browser.close()
            if res.get("status") in ("failed", "error"):
                self.note_microphone(res, run_dir, plan)
            elif self.config.audio == "file":
                # A passed file run's dump says nothing the file doesn't.
                try:
                    os.remove(os.path.join(run_dir, MIC_DUMP))
                except OSError:
                    pass
        res["duration_s"] = round(time.time() - t_start, 1)
        res["kinds"] = tlog.kinds()
        res["bad_log_lines"] = tlog.bad_lines
        res["log"] = os.path.relpath(log_path, self.report_dir)
        return res

    def start_page(self, browser, sc):
        start = sc.start
        if start.get("url") and start["url"] != "about:blank":
            browser.navigate(start["url"])
        main = browser.active()
        for url in start.get("tabs", []):
            browser.open_tab(url)
        if main and start.get("tabs"):
            browser.activate(main["targetId"])
        if start.get("js"):
            browser.evaluate_active(start["js"])

    # ---- audio ----------------------------------------------------------------

    def note_microphone(self, res, run_dir, plan=None):
        """For a failed scenario: what voice mode's microphone gave
        (its dump, voice_test_audio.h), each second's peak and what the
        kit's own recognizer makes of it, so the report tells a microphone
        that got nothing from audio voice mode's recognizer didn't make
        out. In file mode, also the recording's own levels at the failed
        step's speech, to set against the microphone's: what the
        browser's processing (echo canceller, noise suppressor) took."""
        path = os.path.join(run_dir, MIC_DUMP)
        try:
            with open(path, "rb") as f:
                pcm = f.read(MIC_DUMP_MAX_S * audio_path.PCM_RATE * 2)
        except OSError:
            return
        res["mic_levels"] = mic_levels(pcm)
        # Each 100 ms around the start of the failed step's speech: whether
        # its first words reached the helper (an echo canceller can hold
        # back the start of speech said over the browser's own sound).
        origin = None
        tlog = testlog.TestLog(os.path.join(run_dir, "voice.jsonl"))
        for ev in tlog.events(0):
            if ev["e"] == "activity" and ev.get("mic") is True:
                origin = ev["ts"]
                break
        # The step that failed (those after it weren't run).
        failing = next((st for st in res.get("steps", [])
                        if st.get("status") not in ("passed", "not run")
                        and st.get("said_at")), None)
        if origin is not None and failing:
            start = failing["said_at"][0]
            at_s, levels = onset_levels(pcm, (start - origin) * audio_path.PCM_RATE,
                                        rate=audio_path.PCM_RATE)
            if levels:
                res["mic_onset"] = {"step": failing.get("index", 0) + 1,
                                    "at_s": round(at_s, 2), "levels": levels}
                played = recording_levels(plan, failing.get("index"))
                if played:
                    res["mic_onset"]["played_levels"] = played
        if self.ears and pcm:
            try:
                res["mic_heard"] = (self.ears.transcribe(pcm) or "")[:600]
            except Exception as err:  # the ears are the kit's: a note only
                res["mic_heard"] = "(the kit's recognizer failed: %s)" % err
        at_s = (res.get("mic_onset") or {}).get("at_s")
        if self.ears and pcm and at_s is not None:
            # The failed step's speech again, through a helper that needs
            # the wake phrase: whether the helper loses a command said
            # right after it from the audio alone.
            try:
                events = self.ears.replay(pcm)
            except Exception as err:  # the ears are the kit's: a note only
                res["mic_replay"] = ["(the kit's helper failed: %s)" % err]
            else:
                res["mic_replay"] = replay_lines(events, at_s)

    def wake_for(self, step, mode):
        return mode == "wake" and not step.answer

    def prepare_audio(self, sc, mode, voice, speed, run_dir):
        """The WAVs this scenario plays (pulse, file), and in file mode the
        one WAV the fake device plays, with each step's times."""
        plan = {"wavs": {}, "times": {}, "voice_speed": (voice, speed)}
        if self.config.typed:
            return plan
        adir = os.path.join(run_dir, "audio")
        os.makedirs(adir, exist_ok=True)
        for i, step in enumerate(sc.steps):
            if step.action == "say":
                out = os.path.join(adir, "step%02d.wav" % (i + 1))
                plan["wavs"][i] = (out, audio_path.utterance_wav(
                    self.speech, out, voice, speed, step.value, self.wake_for(step, mode)))
        if self.config.audio == "pulse":
            plan["recording"] = os.path.join(run_dir, "browser-speech.pcm")
            if self.config.condition != "clean":
                bed = os.path.join(adir, "bed-%s.wav" % self.config.condition)
                kind, snr = render.CONDITIONS[self.config.condition]
                render.music_bed(bed, 600, snr, seed=sc.id)
                plan["bed"] = bed
        else:
            order = [i for i in sorted(plan["wavs"])]
            wav = os.path.join(adir, "scenario.wav")
            gaps = [sc.steps[i].file_gap or FILE_GAP_S for i in order]
            times = audio_path.scenario_wav(wav, [plan["wavs"][i][0] for i in order],
                                            self.config.condition, FILE_LEAD_S, gaps,
                                            sc.id)
            plan["file_wav"] = wav
            plan["times"] = dict(zip(order, times))
        return plan

    def say(self, step, i, plan, stream_start, world, tlog, mode):
        """Says step |i|; returns (the time its speech ended, what was said)."""
        if step.action == "wait":
            return time.time(), None
        if self.config.typed:
            text = ("Hey Molt, " if self.wake_for(step, mode) else "") + step.value
            t_end = time.time()
            r = world.browser.type_command(text)
            if r != "ok":
                raise RuntimeError("Agent mode refused the command: %s" % r)
            return t_end, text
        if self.config.audio == "file":
            a, b = plan["times"][i]
            t_end = stream_start + b
            wait = stream_start + a - time.time()
            if wait > 0:
                time.sleep(wait)
            return t_end, step.value
        if step.action == "say":
            wav = plan["wavs"][i][0]
        elif step.action == "replay":
            wav = plan["named"][step.value]
        else:
            wav = self.word_wav(step, i, plan, tlog, world)
        (self.player or self.pulse).play(wav)
        t_end = time.time()
        if step.action == "say_word" and self.asked_again(tlog, t_end):
            # "I didn't catch that. Say the word again.": once more, as a
            # user would.
            self.wait_idle(tlog)
            (self.player or self.pulse).play(wav)
            t_end = time.time()
        if step.name:
            plan.setdefault("named", {})[step.name] = wav
        return t_end, step.value if step.action == "say" else step.action

    def word_wav(self, step, i, plan, tlog, world):
        """The confirmation word the browser gave, heard from its speech as
        the user hears it; when it isn't clear, the kit says "repeat" (the
        browser says the prompt again, word and all) and listens again, as
        a user would, WORD_TRIES times in all."""
        opened = [e for e in tlog.events(0) if e["e"] == "prompt" and e.get("op") == "open"]
        since = opened[-1]["ts"] - 0.5 if opened else time.time() - 20
        if not self.ears:
            raise RuntimeError("no recognizer to hear the confirmation word with")
        voice, speed = plan.get("voice_speed", (self.config.voices[0], 1.0))
        heard_all = []
        guess = None
        for attempt in range(WORD_TRIES):
            if attempt:
                since = time.time()
                (self.player or self.pulse).play(
                    render.clip_path(self.speech, voice, speed, render.REPEAT))
                # Voice mode was idle before "repeat" too: wait for it to
                # answer (the prompt said again) before waiting for quiet,
                # or the kit says "repeat" again into the same utterance.
                self.wait_reply(tlog, since)
                self.wait_idle(tlog)
            pcm = audio_path.read_pcm_since(plan["recording"], plan["rec_t0"], since)
            heard = self.ears.transcribe(pcm)
            heard_all.append(heard)
            word = self.ears.find_word(heard)
            # Mangled twice ("say keptus", then "say keptus" again; "say kept
            # this", then "say active"): the one word both hearings sound
            # near. A wrong one fails the step (the browser refuses it).
            near = self.ears.sounds_near(heard)
            if not word and guess is not None and len(near & guess) == 1:
                word = next(iter(near & guess))
            guess = near
            if word:
                plan.setdefault("heard_browser", []).extend(heard_all)
                return render.clip_path(self.speech, voice, speed, word)
        plan.setdefault("heard_browser", []).extend(heard_all)
        raise StepFailed("didn't hear a confirmation word in the browser's speech "
                         "(heard %s)" % "; ".join(repr(h[:120]) for h in heard_all))

    # ---- one step -------------------------------------------------------------

    def run_step(self, sc, step, i, world, tlog, plan, stream_start, mode, run_dir):
        sres = {"index": i, "action": step.action, "status": "passed"}
        if step.guards():
            sres["guard"] = True
        if step.action == "say":
            sres["say"] = step.value
        if step.pause:
            time.sleep(step.pause)
        t0 = time.time()
        if self.config.audio == "file" and i in plan.get("times", {}):
            # What the browser does from when the step's speech begins is
            # the step's, however late the kit is to it: the file plays on
            # its own clock (a page's dialog holds the kit's looks at the
            # page until it closes).
            t0 = min(t0, stream_start + plan["times"][i][0] - FILE_STEP_SLACK_S)
        before = expect.snapshot(world)
        try:
            t_end, said = self.say(step, i, plan, stream_start, world, tlog, mode)
        except StepFailed as err:
            sres.update(status="failed", detail=str(err))
            self.screenshot(world, run_dir, i, sres)
            return sres
        sres["said_as"] = said
        if step.action == "say" and not self.config.typed:
            # When its speech played (wall clock), for the microphone notes.
            if self.config.audio == "file":
                a, b = plan["times"][i]
                sres["said_at"] = [round(stream_start + a, 3), round(stream_start + b, 3)]
            elif i in plan["wavs"]:
                length = plan["wavs"][i][1]
                sres["said_at"] = [round(t_end - length, 3), round(t_end, 3)]
        checks = [e for e in step.expect if expect.applies(e, self.config.typed)]
        polled = [e for e in checks if not is_quiet(e)]
        quiet = [e for e in checks if is_quiet(e)]
        deadline = t_end + step.timeout
        if self.config.audio == "file":
            nxt = [plan["times"][k][0] for k in plan["times"] if k > i]
            if nxt:
                deadline = min(deadline, stream_start + min(nxt))
        results = {}
        while True:
            for j, e in enumerate(polled):
                if results.get(j, (False,))[0]:
                    continue
                try:
                    results[j] = expect.check(e, world, t0, before)
                except Exception as err:  # a page gone mid-check
                    results[j] = (False, "%s: %s" % (type(err).__name__, err))
                if results[j][0]:
                    results[j] = (True, results[j][1], time.time())
            if all(results.get(j, (False,))[0] for j in range(len(polled))) and \
                    time.time() >= t_end:
                break
            if time.time() >= deadline:
                break
            time.sleep(POLL_S)
        if quiet:
            q = max(e.get("quiet", DEFAULT_QUIET_S) for e in quiet)
            left = t_end + q - time.time()
            if left > 0:
                time.sleep(left)
            for k, e in enumerate(quiet):
                if "no_action" in e:
                    results[len(polled) + k] = expect.quiet_check(before, world, t_end)
                else:
                    results[len(polled) + k] = expect.check(e, world, t0, before)
        sres["expectations"] = []
        for j, e in enumerate(polled + quiet):
            r = results.get(j, (False, "not checked"))
            kind = next(k for k in e if k not in ("typed", "is", "kind", "quiet"))
            sres["expectations"].append({"kind": kind, "want": e[kind], "ok": bool(r[0]),
                                         "detail": r[1]})
        heard = [e for e in tlog.events(t0) if e["e"] == "heard"]
        sres["heard"] = [h.get("text", "") for h in heard]
        sres["intents"] = [h.get("intent", "") for h in heard]
        sres["latency"] = self.latencies(world, tlog, t0, t_end, before, results, polled)
        if step.action == "say" and not self.config.typed and heard:
            errs, n = word_errors(step.value, strip_wake(heard[0].get("text", "")))
            sres["word_errors"] = [errs, n]
        if not all(x["ok"] for x in sres["expectations"]):
            sres["status"] = self.classify(step, sres, tlog)
            sres["detail"] = "; ".join(x["detail"] for x in sres["expectations"] if not x["ok"])
            self.screenshot(world, run_dir, i, sres)
        return sres

    def classify(self, step, sres, tlog):
        kinds = set(tlog.kinds())
        missing = [n for n in step.needs if kinds and n not in kinds]
        if missing:
            sres["missing_commands"] = missing
            return "not in grammar"
        if not self.config.typed and step.action == "say" and not sres["heard"]:
            sres["why"] = "not heard"
        elif sres.get("word_errors") and sres["word_errors"][0] > 0:
            sres["why"] = "misheard"
        return "failed"

    def latencies(self, world, tlog, t0, t_end, before, results, polled):
        out = {}
        heard = [e for e in tlog.events(t0) if e["e"] == "heard"]
        if heard:
            out["heard_ms"] = int(round((heard[0]["ts"] - t_end) * 1000))
        said = [e for e in tlog.events(t_end) if e["e"] == "said"]
        if said:
            out["said_ms"] = int(round((said[0]["ts"] - t_end) * 1000))
        acts = []
        try:
            if not world.page_dialog_open():
                acts = [ev["t"] / 1000.0 for ev in world.page_events(t_end - 0.3)
                        if ev.get("trusted", True)]
        except Exception:
            pass
        if world.url() != before.get("url"):
            nav = world.browser.navigation_start() if hasattr(world.browser,
                                                              "navigation_start") else None
            if nav:
                acts.append(nav)
        if acts:
            out["action_ms"] = int(round((min(acts) - t_end) * 1000))
        done = [r[2] for r in results.values() if len(r) > 2]
        if done and len(done) == len(polled):
            out["outcome_ms"] = int(round((max(done) - t_end) * 1000))
        return out

    def screenshot(self, world, run_dir, i, sres):
        path = os.path.join(run_dir, "step%02d.png" % (i + 1))
        try:
            if world.browser.screenshot(path):
                sres["screenshot"] = os.path.relpath(path, self.report_dir)
        except Exception:
            pass

    # ---- a run ----------------------------------------------------------------

    def run(self, scenarios, modes=None):
        out = {"label": self.config.label, "audio": self.config.audio,
               "condition": self.config.condition, "typed": self.config.typed,
               "loopback": self.config.loopback if self.config.audio == "pulse" else None,
               "started": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
               "scenarios": []}
        index = 0
        for sc in scenarios:
            for mode in sc.modes:
                if modes and mode not in modes:
                    continue
                self.log("[%s] %s (%s)" % (self.config.label, sc.id, mode))
                res = self.run_scenario(sc, mode, index)
                index += 1
                self.log("    %s%s" % (res["status"], (": " + res["reason"]) if res["reason"] else ""))
                out["scenarios"].append(res)
        out["finished"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        return out


class StepFailed(Exception):
    pass


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def safe_name(text):
    return re.sub(r"[^a-z0-9-]+", "-", text.lower()).strip("-")[:40]
