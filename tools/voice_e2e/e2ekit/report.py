# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""report.json and report.md, and the exit code against Phase 1's exit
(docs/design/TOUCHLESS_BROWSING.md §10):

  - the scripted tasks (scenarios with "counts") pass at SUCCESS_TARGET or
    better in every spoken run (quiet and with music), the owner's
    scenario included;
  - no security scenario acts (a failure there fails the report whatever
    the rate), and each gets to its checks (one whose earlier step failed
    fails it too, reported as such: it acted only if a check failed);
  - a direct command acts within LATENCY_TARGET_MS of the end of speech:
    the median is reported against it, and gates only with --gate-latency
    (the runner isn't the minimum hardware).

A scenario a build has no command for yet ("not in grammar": a step's
"needs" names a command the build's grammar doesn't list) counts as not
passed, and is listed apart. Skipped scenarios (typed text can't say a
confirmation word; file capture can't replay one) count for neither.
"""

import json
import os

from . import KIT_VERSION, REPORT_SCHEMA

SUCCESS_TARGET = 0.9
LATENCY_TARGET_MS = 1500


def _median(xs):
    xs = sorted(xs)
    if not xs:
        return None
    n = len(xs)
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2.0


def _pct(xs, p):
    xs = sorted(xs)
    if not xs:
        return None
    k = min(len(xs) - 1, max(0, int(round(p / 100.0 * (len(xs) - 1)))))
    return xs[k]


def security_outcome(s):
    """A failed security scenario's "acted" when one of its checks failed
    (the steps that watch for nothing happening, Step.guards(): "guard" in
    the step's result), or it has none (each step is one then); else
    "unchecked": a step before a check failed, so that check didn't run,
    and those that ran held."""
    steps = s["steps"]
    if not any(st.get("guard") for st in steps):
        return "acted"
    for st in steps:
        if st.get("guard") and st.get("status") not in ("passed", "not run"):
            return "acted"
    return "unchecked"


def summarize_run(run):
    scs = run["scenarios"]
    ran = [s for s in scs if s["status"] != "skipped"]
    counted = [s for s in ran if s["counts"]]
    passed = [s for s in counted if s["status"] == "passed"]
    missing = [s for s in counted if s["status"] == "not in grammar"]
    errors = [s for s in ran if s["status"] == "error"]
    by_task = {}
    for s in counted:
        t = by_task.setdefault(s["task"], {"passed": 0, "total": 0})
        t["total"] += 1
        t["passed"] += s["status"] == "passed"
    security = [s for s in ran if s["task"] == "security"]
    owner = [s for s in ran if s["task"] == "owner"]
    lat, heard_lat, said_lat, outcome = [], [], [], []
    errs = words = exact = recognized = 0
    misheard = []
    for s in ran:
        for st in s["steps"]:
            la = st.get("latency") or {}
            if st.get("status") == "passed" and st.get("action") == "say":
                if "action_ms" in la and la["action_ms"] >= -300:
                    lat.append(la["action_ms"])
                if "outcome_ms" in la:
                    outcome.append(la["outcome_ms"])
            if "heard_ms" in la:
                heard_lat.append(la["heard_ms"])
            if "said_ms" in la:
                said_lat.append(la["said_ms"])
            we = st.get("word_errors")
            if we:
                recognized += 1
                errs += we[0]
                words += we[1]
                exact += we[0] == 0
                if we[0]:
                    misheard.append({"scenario": s["id"], "say": st.get("say"),
                                     "heard": (st.get("heard") or [""])[0]})
    total = len(counted)
    known = total - len(missing)
    return {
        "scenarios": len(scs), "ran": len(ran), "skipped": len(scs) - len(ran),
        "counted": total, "passed": len(passed), "not_in_grammar": len(missing),
        "errors": len(errors),
        "success_rate": round(len(passed) / total, 4) if total else None,
        "success_rate_known_commands": round(len(passed) / known, 4) if known else None,
        "by_task": by_task,
        "owner": [{"id": s["id"], "mode": s["mode"], "status": s["status"]} for s in owner],
        "security": {"ran": len(security),
                     "passed": sum(1 for s in security if s["status"] == "passed"),
                     "acted": [s["id"] for s in security if s["status"] == "failed"
                               and security_outcome(s) == "acted"],
                     "unchecked": [s["id"] for s in security if s["status"] == "failed"
                                   and security_outcome(s) == "unchecked"]},
        "latency_ms": {"action_median": _median(lat), "action_p90": _pct(lat, 90),
                       "action_n": len(lat),
                       "within_target": round(sum(1 for x in lat if x <= LATENCY_TARGET_MS)
                                              / len(lat), 3) if lat else None,
                       "heard_median": _median(heard_lat), "said_median": _median(said_lat),
                       "outcome_median": _median(outcome)},
        "recognition": {"steps": recognized,
                        "wer": round(errs / words, 4) if words else None,
                        "exact": round(exact / recognized, 4) if recognized else None},
        "misheard": misheard,
        "missing_commands": sorted({c for s in counted for st in s["steps"]
                                    for c in st.get("missing_commands", [])}),
    }


def verdict(runs, gate_latency=False):
    """(ok, reasons)."""
    reasons = []
    for run in runs:
        s = run["summary"]
        label = run["label"]
        if s["counted"] and (s["success_rate"] or 0) < SUCCESS_TARGET:
            reasons.append("%s: %d of %d tasks passed (%.0f%%, the target is %.0f%%)"
                           % (label, s["passed"], s["counted"], 100 * (s["success_rate"] or 0),
                              100 * SUCCESS_TARGET))
        for o in s["owner"]:
            if o["status"] != "passed":
                reasons.append("%s: the owner's scenario (%s) %s" % (label, o["mode"], o["status"]))
        if s["security"]["acted"]:
            reasons.append("%s: security scenarios acted: %s"
                           % (label, ", ".join(s["security"]["acted"])))
        if s["security"].get("unchecked"):
            reasons.append("%s: security scenarios that didn't get to their checks "
                           "(a step before them failed; the checks that ran held): %s"
                           % (label, ", ".join(s["security"]["unchecked"])))
        if s["errors"]:
            reasons.append("%s: %d scenarios couldn't run (errors)" % (label, s["errors"]))
        med = s["latency_ms"]["action_median"]
        if gate_latency and med is not None and med > LATENCY_TARGET_MS:
            reasons.append("%s: median %d ms from the end of speech to the action (target %d)"
                           % (label, med, LATENCY_TARGET_MS))
    return not reasons, reasons


def build(runs, meta, gate_latency=False):
    for run in runs:
        run["summary"] = summarize_run(run)
    ok, reasons = verdict(runs, gate_latency)
    return {"schema": REPORT_SCHEMA, "kit_version": KIT_VERSION, "meta": meta,
            "targets": {"success": SUCCESS_TARGET, "latency_ms": LATENCY_TARGET_MS,
                        "latency_gates": gate_latency},
            "ok": ok, "reasons": reasons, "runs": runs}


def _rate(x):
    return "-" if x is None else "%.0f%%" % (100 * x)


def _ms(x):
    return "-" if x is None else "%d ms" % x


def markdown(report):
    out = ["# Voice mode end to end", ""]
    meta = report.get("meta", {})
    out.append("Browser: `%s`. Bundle: `%s`. Kit %s." % (
        meta.get("browser", "?"), meta.get("bundle", "?"), report.get("kit_version")))
    out.append("")
    out.append("**%s**" % ("Phase 1's exit is met." if report["ok"] else "Not met:"))
    for r in report["reasons"]:
        out.append("- %s" % r)
    out += ["", "| Run | Tasks passed | Without the missing commands | Not in the grammar yet "
            "| Owner's scenario | Security | Action latency (median, p90) | Recognition |",
            "|---|---|---|---|---|---|---|---|"]
    for run in report["runs"]:
        s = run["summary"]
        owner = ", ".join("%s %s" % (o["mode"], o["status"]) for o in s["owner"]) or "-"
        sec = "%d/%d" % (s["security"]["passed"], s["security"]["ran"])
        lat = s["latency_ms"]
        rec = s["recognition"]
        out.append("| %s | %d/%d (%s) | %s | %d | %s | %s | %s, %s | %s |" % (
            run["label"], s["passed"], s["counted"], _rate(s["success_rate"]),
            _rate(s["success_rate_known_commands"]), s["not_in_grammar"], owner, sec,
            _ms(lat["action_median"]), _ms(lat["action_p90"]),
            "-" if rec["wer"] is None else "WER %s, %s exact" % (_rate(rec["wer"]),
                                                                _rate(rec["exact"]))))
    out += ["", "Latency is from the end of the utterance to the first action on the page "
            "(a trusted event, or a new document's start), for passed direct commands; "
            "the target is %d ms on the minimum hardware, and this machine isn't it." %
            LATENCY_TARGET_MS, ""]
    check = meta.get("speech_check")
    if check:
        out += ["The rendered speech through the bundle's recognizer (check-speech): "
                "utterances heard exactly, word error rate, and how often \"Hey Molt\" "
                "woke it. A voice whose wake phrase doesn't wake it fails every "
                "wake-mode step as \"not heard\".", "",
                "| Voice, speed, condition | Exact | WER | Woke |", "|---|---|---|---|"]
        for name in sorted(check):
            c = check[name]
            out.append("| %s | %s | %s | %s |" % (name, _rate(c.get("exact")),
                                                  _rate(c.get("wer")), c.get("woke", "-")))
        out.append("")
    for run in report["runs"]:
        s = run["summary"]
        out += ["## %s" % run["label"], ""]
        tasks = ", ".join("%s %d/%d" % (t, v["passed"], v["total"])
                          for t, v in sorted(s["by_task"].items()))
        out.append("By task: %s." % (tasks or "none"))
        lat = s["latency_ms"]
        out.append("Heard (median from the end of speech): %s; said: %s; outcome: %s."
                   % (_ms(lat["heard_median"]), _ms(lat["said_median"]),
                      _ms(lat["outcome_median"])))
        if s["missing_commands"]:
            out.append("Not in the grammar yet: %s." % ", ".join(
                "`%s`" % c for c in s["missing_commands"]))
        out.append("")
        out += ["| Scenario | Mode | Voice | Result | Why |", "|---|---|---|---|---|"]
        for sc in run["scenarios"]:
            out.append("| %s | %s | %s | %s | %s |" % (
                sc["id"], sc["mode"],
                "%s@%s" % (sc["voice"], sc["speed"]) if sc.get("voice") else "typed",
                sc["status"], _cell(sc.get("reason", ""))))
        failed = [sc for sc in run["scenarios"] if sc["status"] not in ("passed", "skipped")]
        if failed:
            out += ["", "### What went wrong", ""]
            for sc in failed:
                out.append("**%s** (%s): %s" % (sc["id"], sc["mode"], _cell(sc.get("reason", ""))))
                for st in sc["steps"]:
                    if st.get("status") in ("passed", "not run"):
                        continue
                    heard = "; ".join(st.get("heard") or []) or "(nothing)"
                    line = "- step %d, said %r, heard %r, understood as %s" % (
                        st["index"] + 1, st.get("say") or st.get("action"), heard,
                        ", ".join(st.get("intents") or []) or "-")
                    if st.get("why"):
                        line += " (%s)" % st["why"]
                    out.append(line)
                    for x in st.get("expectations", []):
                        if not x["ok"]:
                            out.append("  - %s %s: %s" % (x["kind"], json.dumps(x["want"]),
                                                          _cell(x["detail"])))
                    if st.get("screenshot"):
                        out.append("  - screenshot: [%s](%s)" % (st["screenshot"],
                                                                st["screenshot"]))
                out.append("")
        if s["misheard"]:
            out += ["", "### Misheard", "", "| Scenario | Said | Heard |", "|---|---|---|"]
            for m in s["misheard"]:
                out.append("| %s | %s | %s |" % (m["scenario"], _cell(m["say"] or ""),
                                                 _cell(m["heard"])))
        out.append("")
    return "\n".join(out) + "\n"


def _cell(text):
    return str(text).replace("|", "\\|").replace("\n", " ")[:300]


def write(report_dir, report):
    os.makedirs(report_dir, exist_ok=True)
    with open(os.path.join(report_dir, "report.json"), "w", encoding="utf-8") as f:
        json.dump(report, f, indent=1, sort_keys=True)
    with open(os.path.join(report_dir, "report.md"), "w", encoding="utf-8") as f:
        f.write(markdown(report))
