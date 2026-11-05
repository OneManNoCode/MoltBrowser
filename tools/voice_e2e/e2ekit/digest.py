# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""What explains the failed scenarios, as text for a CI job's log: the
reports' artifacts aren't always reachable from where the results are
read, the job log is.

For each run, the scenarios that failed or couldn't run: those that
couldn't run grouped by the first fatal line of their browser.log (one
crash prints once, with the names of the others it ended), those that
failed a check by its reason, each with:
  - the browser log's fatal lines and the stack after the first one;
  - the end of the browser log when nothing in it was fatal;
  - the voice test log's events (what was heard, said, the activity),
    clipped;
  - error.txt, the kit's own traceback.
The first |max_groups| groups in full, a line each after; |limit|
characters in all.
"""

import json
import os
import re

FATAL = ("FATAL", "Check failed", "DCHECK failed", "Received signal",
         "received signal", "[DanglingSignature]", "NOTREACHED",
         "AddressSanitizer", "Crash", "crashed", "SIGSEGV", "SIGABRT",
         "Sandbox", "seccomp")
# Lines every run prints that explain nothing.
NOISE = ("dbus/", "ALSA lib", "alsa_util.cc", "Failed to set realtime",
         "kill_posix.cc", "ui/gfx/linux/gpu_memory_buffer_support_x11.cc",
         "viz_main_impl.cc", "gl_surface_presentation_helper",
         "Fontconfig", "fontconfig")


def _read(path, limit=4 * 1024 * 1024):
    try:
        with open(path, "rb") as f:
            f.seek(0, os.SEEK_END)
            size = f.tell()
            f.seek(max(0, size - limit))
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def _normalized(line):
    line = re.sub(r"^\[\d+:\d+:\d+/\d+\.\d+:", "[", line.strip())
    line = re.sub(r"0x[0-9a-f]+", "0x", line)
    return re.sub(r"\d{3,}", "N", line)[:200]


def molt_lines(lines, limit=20):
    """MoltBrowser's own log lines ("[MoltVoice] microphone error ..."),
    each kind once."""
    out, seen = [], set()
    for line in lines:
        if "[Molt" not in line:
            continue
        key = _normalized(line)
        if key in seen:
            continue
        seen.add(key)
        out.append("  " + line[:400])
        if len(out) >= limit:
            break
    return out


def browser_log_digest(text, stack_lines=40, tail_lines=25):
    """(the first fatal line normalized or '', the digest's lines)."""
    lines = text.splitlines()
    own = molt_lines(lines)
    own = (["MoltBrowser's lines:"] + own) if own else []
    fatal = [i for i, l in enumerate(lines)
             if any(s in l for s in FATAL) and not any(n in l for n in NOISE)]
    if not fatal:
        tail = [l for l in lines if not any(n in l for n in NOISE)][-tail_lines:]
        return "", own + (["(nothing fatal; the end of browser.log:)"] + tail
                          if tail else ["(browser.log is empty)"])
    first = fatal[0]
    out = ["fatal lines (%d):" % len(fatal)]
    seen = set()
    for i in fatal:
        key = _normalized(lines[i])
        if key in seen:
            continue
        seen.add(key)
        out.append("  " + lines[i][:400])
        if len(seen) >= 12:
            break
    out.append("after the first:")
    out += ["  " + l[:400] for l in lines[first:first + stack_lines]]
    return _normalized(lines[first]), own + out


def voice_log_digest(path, limit_events=60):
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            raw = f.read().splitlines()
    except OSError:
        return ["(no voice test log)"]
    events = []
    for line in raw:
        try:
            ev = json.loads(line)
        except ValueError:
            continue
        if isinstance(ev, dict):
            events.append(ev)
    if not events:
        return ["(the voice test log is empty)"]
    t0 = events[0].get("t", 0)
    events = _fold_microphone(events)
    shown = events if len(events) <= limit_events else \
        events[:limit_events // 2] + [{"e": "...", "skipped": len(events) - limit_events}] + \
        events[-limit_events // 2:]
    for ev in shown:
        t = ev.get("t")
        rest = {k: v for k, v in ev.items() if k not in ("t", "e")}
        out.append("  %7s %-9s %s" % (
            "%+.1fs" % ((t - t0) / 1000.0) if isinstance(t, (int, float)) else "",
            ev.get("e", "?"),
            json.dumps(rest, ensure_ascii=False)[:1500 if ev.get("e") == "map" else 300]))
    return out


def _fold_microphone(events):
    """Each run of "mic" events (one about every five seconds) as one: how
    many, the loudest and quietest peaks, the samples, the echo modes."""
    out = []
    for ev in events:
        if ev.get("e") != "mic":
            out.append(ev)
            continue
        peak = ev.get("peak_db")
        samples = ev.get("samples")
        peak = peak if isinstance(peak, (int, float)) else -120
        samples = samples if isinstance(samples, int) else 0
        echo = str(ev.get("echo", ""))[:32]
        last = out[-1] if out else None
        if last is not None and last.get("e") == "mic" and "reports" in last:
            last["reports"] += 1
            last["loudest_db"] = max(last["loudest_db"], peak)
            last["quietest_db"] = min(last["quietest_db"], peak)
            last["samples"] += samples
            if echo not in last["echo"]:
                last["echo"].append(echo)
            continue
        out.append({"t": ev.get("t"), "e": "mic", "reports": 1,
                    "loudest_db": peak, "quietest_db": peak,
                    "samples": samples, "echo": [echo]})
    return out


def _results(report_dir):
    try:
        with open(os.path.join(report_dir, "report.json"), encoding="utf-8") as f:
            rep = json.load(f)
    except (OSError, ValueError):
        return []
    out = []
    for run in rep.get("runs") or []:
        label = run.get("label") or "?"
        for res in run.get("scenarios") or []:
            if res.get("status") in ("failed", "error"):
                out.append((label, res))
    return out


def build(report_dir, limit=150000, max_groups=30):
    groups = {}
    order = []
    for label, res in _results(report_dir):
        name = res["id"] + ("" if res.get("mode", "wake") == "wake" else "-" + res["mode"])
        run_dir = os.path.join(report_dir, "runs", label.replace("/", "-"), name)
        key_line, blog = browser_log_digest(_read(os.path.join(run_dir, "browser.log")))
        # A scenario that couldn't finish is grouped by the browser's crash;
        # one that failed a check by its own reason: a crash at exit, after
        # the check, would end every scenario's log alike.
        if res.get("status") == "error" and key_line:
            key = (label, key_line)
        else:
            key = (label, "failed: " + (res.get("reason") or "")[:60])
        if key not in groups:
            groups[key] = {"first": (name, res, run_dir, blog), "others": []}
            order.append(key)
        else:
            groups[key]["others"].append(name)
    out = []
    for n, key in enumerate(order):
        g = groups[key]
        name, res, run_dir, blog = g["first"]
        if n >= max_groups:
            # Past the first few, a line each: the logs are in the report.
            out.append("- %s: %s (%s): %s%s" % (
                key[0], name, res.get("status"), (res.get("reason") or "")[:200],
                " (and %d more alike)" % len(g["others"]) if g["others"] else ""))
            continue
        part = ["", "### %s: %s (%s)" % (key[0], name, res.get("status")),
                "why: " + (res.get("reason") or "")[:400]]
        if g["others"]:
            part.append("the same in %d more: %s" % (len(g["others"]),
                                                      ", ".join(g["others"][:40])))
        part.append("browser.log:")
        part += blog
        part.append("voice.jsonl:")
        part += voice_log_digest(os.path.join(run_dir, "voice.jsonl"))
        err = _read(os.path.join(run_dir, "error.txt"), 4000).strip()
        if err:
            part.append("error.txt:")
            part += ["  " + l for l in err.splitlines()[-15:]]
        out.append("\n".join(part))
    text = "\n".join(out) if out else "(no failed scenarios)"
    if len(text) > limit:
        text = text[:limit] + "\n[... %d characters more ...]" % (len(text) - limit)
    return text
