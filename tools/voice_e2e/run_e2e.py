#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Voice mode end to end (docs/VOICE_E2E.md): spoken scenarios against a
built MoltBrowser, through a virtual microphone.

  run_e2e.py render --data DIR --driver BIN --out SPEECH [--hashes FILE]
      Renders every utterance of the scenarios (tools/voice_bench's pinned
      voices and driver; DIR is a tools/voice_bench/fetch.py data dir).
  run_e2e.py check-speech --speech SPEECH --bundle BUNDLE --out FILE
      Runs every rendered utterance, clean and mixed, through the bundle's
      molt-speech: recognition accuracy per voice and mix.
  run_e2e.py run --browser CHROME --bundle BUNDLE --speech SPEECH --report DIR
      [--runs pulse:clean,pulse:music10,file:clean,typed]
      [--scenarios ID,...] [--tasks TASK,...] [--modes wake,always]
      [--no-loopback] [--no-sandbox] [--gate-latency]
      Runs the scenarios; writes DIR/report.json and DIR/report.md; exits
      0 when Phase 1's exit is met, 1 when not, 2 when it couldn't run.
  run_e2e.py serve [--port N]
      Serves the fixture sites (for a look by hand: map their names to
      127.0.0.1 and accept the certificate it prints the key of).
  run_e2e.py list
      The scenarios, their tasks and the commands they need.

Run with python3 -I from the repository.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

from e2ekit import KIT_VERSION  # noqa: E402
from e2ekit import scenarios as scen  # noqa: E402

SCENARIOS_DIR = os.path.join(HERE, "scenarios")


def load(args):
    scs = scen.load_dir(args.scenarios_dir)
    ids = [s for s in (getattr(args, "scenarios", "") or "").split(",") if s]
    tasks = [t for t in (getattr(args, "tasks", "") or "").split(",") if t]
    return scen.select(scs, ids, tasks)


def cmd_list(args):
    scs = load(args)
    counted = sum(len(s.modes) for s in scs
                  if s.counts and s.task not in ("phase2", "model"))
    phase2 = sum(len(s.modes) for s in scs if s.task == "phase2")
    model = sum(len(s.modes) for s in scs if s.task == "model")
    for s in scs:
        print("%-28s %-9s %-11s %s" % (s.id, s.task, ",".join(s.modes),
                                       ",".join(s.needs())))
    print("%d scenarios, %d runs of counted tasks per spoken run (and %d of Phase 2's, "
          "%d of the model's), %d utterances" % (len(scs), counted, phase2, model,
                                                 len(scen.utterances(scs))))
    return 0


def cmd_render(args):
    from e2ekit import fixtures, render
    scs = load(args)
    tts = render.DriverTts(os.path.abspath(args.driver), os.path.abspath(args.data))
    manifest = render.render_all(tts, os.path.abspath(args.out), scen.utterances(scs),
                                 fixtures.PAGE_AUDIO_TEXTS)
    print("rendered: %d texts, %d files" % (len(manifest["texts"]), len(manifest["files"])))
    if args.hashes:
        with open(args.hashes, encoding="utf-8") as f:
            recorded = json.load(f)
        same, different, missing, names = render.compare_hashes(manifest, recorded)
        print("against %s: %d the same, %d different, %d missing" % (
            args.hashes, same, different, missing))
        for n in names:
            print("  different: %s" % n)
        if args.strict and (different or missing):
            return 1
    if args.write_hashes:
        with open(args.write_hashes, "w", encoding="utf-8") as f:
            json.dump(render.set_digests(manifest), f, indent=1, sort_keys=True)
            f.write("\n")
    return 0


def cmd_check_speech(args):
    from e2ekit import speech_check
    scs = load(args)
    res = speech_check.run(os.path.abspath(args.speech), os.path.abspath(args.bundle),
                           scen.utterances(scs), conditions=args.conditions.split(","))
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(res, f, indent=1, sort_keys=True)
    print(speech_check.markdown(res))
    return 0


def start_display(log):
    """An Xvfb display of the kit's own (the CI machine has no screen)."""
    if not shutil.which("Xvfb"):
        return None, None
    for n in range(91, 140):
        if not os.path.exists("/tmp/.X11-unix/X%d" % n) and \
                not os.path.exists("/tmp/.X%d-lock" % n):
            break
    proc = subprocess.Popen(["Xvfb", ":%d" % n, "-screen", "0", "1280x1024x24",
                             "-nolisten", "tcp", "-noreset"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                            start_new_session=True)
    deadline = time.time() + 15
    while time.time() < deadline and not os.path.exists("/tmp/.X11-unix/X%d" % n):
        time.sleep(0.2)
    log("Xvfb on :%d" % n)
    return ":%d" % n, proc


def cmd_run(args):
    from e2ekit import audio_path, browser, ears, fixtures, report, runner
    log = print
    scs = load(args)
    report_dir = os.path.abspath(args.report)
    os.makedirs(report_dir, exist_ok=True)
    speech = os.path.abspath(args.speech)
    bundle = os.path.abspath(args.bundle)
    exe = os.path.abspath(args.browser)
    if not os.path.isfile(exe):
        print("no browser at %s" % exe)
        return 2
    if not os.path.exists(os.path.join(speech, "manifest.json")):
        print("no rendered speech in %s: run render first" % speech)
        return 2
    if args.model_file:
        args.model_file = os.path.abspath(args.model_file)
        if not os.path.isfile(args.model_file) or not args.model_file.endswith(".gguf"):
            print("no model file (a .gguf) at %s" % args.model_file)
            return 2
    browser.install_bundle(bundle, exe, log=log)
    gen = os.path.join(report_dir, "media")
    page_clips = [os.path.join(speech, "page", n) for n in sorted(
        os.listdir(os.path.join(speech, "page")))] if os.path.isdir(
            os.path.join(speech, "page")) else []
    fixtures.write_media(gen, page_clips, log=log)
    cert, key, spki = fixtures.make_cert(os.path.join(report_dir, "cert"))
    server = fixtures.FixtureServer(cert, key, generated_dir=gen,
                                    request_log=open(os.path.join(report_dir, "requests.log"), "w")
                                    ).start()
    display, xvfb = (os.environ.get("DISPLAY"), None) if args.display == "current" \
        else start_display(log)
    pulse = None
    the_ears = None
    runs = []
    try:
        for spec in args.runs.split(","):
            # audio:condition, and for PulseAudio ":noloop" for a run
            # without the loopback of the browser's output into the
            # microphone (no echo for the echo canceller to remove), or
            # ":noaec" for one without Chrome-wide echo cancellation.
            audio, _, rest = spec.partition(":")
            cond, _, option = rest.partition(":")
            if option not in ("", "noloop", "noaec") or (option and audio != "pulse"):
                log("unknown run %s: skipped" % spec)
                continue
            loopback = not args.no_loopback and option != "noloop"
            cfg = runner.RunConfig(audio=audio, condition=cond or "clean",
                                   loopback=loopback,
                                   echo_cancellation=option != "noaec",
                                   sandbox=not args.no_sandbox,
                                   model_available=args.model,
                                   model_file=args.model_file,
                                   extra_flags=args.browser_flag or [],
                                   voices=[v for v in args.voices.split(",") if v] or None,
                                   speeds=[float(x) for x in args.speeds.split(",") if x]
                                   or None)
            if audio == "pulse" and pulse is not None and pulse.loopback != loopback:
                # The daemon has the loopback or hasn't: a new one.
                pulse.stop()
                pulse = None
            if audio == "pulse" and pulse is None:
                if not audio_path.pulse_available():
                    log("PulseAudio isn't installed: skipping %s" % spec)
                    continue
                pulse = audio_path.Pulse(report_dir, loopback=loopback,
                                         log=log).start()
            if audio in ("pulse", "file") and the_ears is None:
                # The kit's own recognizer: the confirmation word in the
                # browser's speech, and the digest's note of what voice
                # mode's microphone gave in a failed scenario.
                helper = os.path.join(bundle, "speech", "molt-speech")
                if os.path.exists(helper):
                    the_ears = ears.Ears(helper, os.path.join(bundle, "speech", "models"))
            r = runner.Runner(exe, speech, report_dir, server, spki, cfg,
                              pulse=pulse if audio == "pulse" else None,
                              ears=the_ears, display=display, log=log)
            runs.append(r.run(scs, modes=[m for m in args.modes.split(",") if m]))
    finally:
        if the_ears:
            the_ears.stop()
        if pulse:
            pulse.stop()
        server.stop()
        if xvfb:
            xvfb.terminate()
    meta = {"browser": exe, "bundle": bundle, "speech": speech,
            "kit_version": KIT_VERSION, "runs": args.runs,
            "speech_manifest_inputs": _manifest_inputs(speech),
            "speech_check": _speech_check_summary(os.path.join(report_dir,
                                                               "speech-check.json"))}
    rep = report.build(runs, meta, gate_latency=args.gate_latency)
    report.write(report_dir, rep)
    print("report: %s/report.md" % report_dir)
    for reason in rep["reasons"]:
        print("  " + reason)
    if not runs:
        return 2
    return 0 if rep["ok"] else 1


def _speech_check_summary(path):
    """What check-speech found per voice (left in the report directory by
    ci_run.sh): a wake phrase the recognizer misses explains a wake-mode
    run's "not heard"."""
    try:
        with open(path, encoding="utf-8") as f:
            res = json.load(f)
    except (OSError, ValueError):
        return None
    out = {}
    for name, s in res.get("sets", {}).items():
        out[name] = {"exact": s.get("exact"), "wer": s.get("wer")}
    for key, w in res.get("wake", {}).items():
        out.setdefault(key + "/clean", {})["woke"] = "%d/%d" % (w["woke"], w["n"])
    return out


def _manifest_inputs(speech):
    try:
        with open(os.path.join(speech, "manifest.json"), encoding="utf-8") as f:
            return json.load(f).get("inputs")
    except (OSError, ValueError):
        return None


def cmd_serve(args):
    from e2ekit import fixtures
    out = os.path.abspath(args.dir)
    cert, key, spki = fixtures.make_cert(os.path.join(out, "cert"))
    gen = os.path.join(out, "media")
    fixtures.write_media(gen, [])
    server = fixtures.FixtureServer(cert, key, generated_dir=gen, port=args.port).start()
    print("serving on 127.0.0.1:%d" % server.port)
    print("--host-resolver-rules=\"%s\"" % fixtures.host_rules(server.port))
    print("--ignore-certificate-errors-spki-list=%s" % spki)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        server.stop()
    return 0


def cmd_digest(args):
    from e2ekit import digest
    print(digest.build(os.path.abspath(args.report), limit=args.limit))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--scenarios-dir", default=SCENARIOS_DIR)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("list")
    s.add_argument("--scenarios", default="")
    s.add_argument("--tasks", default="")
    s = sub.add_parser("render")
    s.add_argument("--data", required=True, help="tools/voice_bench/fetch.py data dir")
    s.add_argument("--driver", required=True, help="tools/voice_bench's sherpa_bench")
    s.add_argument("--out", required=True)
    s.add_argument("--hashes", help="recorded hashes to compare with")
    s.add_argument("--write-hashes", help="write the hashes of this render here")
    s.add_argument("--strict", action="store_true", help="fail when hashes differ")
    s = sub.add_parser("check-speech")
    s.add_argument("--speech", required=True)
    s.add_argument("--bundle", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--conditions", default="clean,pink10,music10")
    s = sub.add_parser("run")
    s.add_argument("--browser", required=True)
    s.add_argument("--bundle", required=True)
    s.add_argument("--speech", required=True)
    s.add_argument("--report", required=True)
    s.add_argument("--runs", default="pulse:clean,pulse:music10,file:clean,typed")
    s.add_argument("--scenarios", default="")
    s.add_argument("--tasks", default="")
    s.add_argument("--modes", default="wake,always")
    s.add_argument("--display", choices=("xvfb", "current"), default="xvfb")
    s.add_argument("--no-loopback", action="store_true")
    s.add_argument("--no-sandbox", action="store_true")
    s.add_argument("--model", action="store_true",
                   help="an on-device AI model is installed (runs the model scenarios)")
    s.add_argument("--model-file",
                   help="a model file, named as the browser's model registry names it "
                        "and of its pinned size (its SHA-256 checked already), linked "
                        "into each scenario's home: the model scenarios run with it")
    s.add_argument("--gate-latency", action="store_true")
    s.add_argument("--browser-flag", action="append", help="one more browser flag")
    s.add_argument("--voices", default="",
                   help="the voices the scenarios rotate through (default all: "
                        "ljspeech,libritts-m,libritts-f)")
    s.add_argument("--speeds", default="", help="and the speeds (default 1,1.25)")
    s = sub.add_parser("digest", help="what explains the failed scenarios, for a CI log")
    s.add_argument("--report", required=True)
    s.add_argument("--limit", type=int, default=150000)
    s = sub.add_parser("serve")
    s.add_argument("--port", type=int, default=0)
    s.add_argument("--dir", default=os.path.join(os.getcwd(), "voice-e2e-serve"))
    args = p.parse_args(argv)
    return {"list": cmd_list, "render": cmd_render, "check-speech": cmd_check_speech,
            "run": cmd_run, "digest": cmd_digest, "serve": cmd_serve}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main())
