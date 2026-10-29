#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Turns run_bench.py's results into Markdown tables (stdout or --out).

  python3 -I tools/voice_bench/report.py --dir DATA [--results DIR]
      [--out report.md] [--json summary.json]
"""

import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import catalog  # noqa: E402
import fetch  # noqa: E402
import voice_bench_lib as vbl  # noqa: E402

CONDS = ["clean", "pink20", "pink10", "pink5", "babble10", "music10", "echo0", "echo0-aec"]


def fmt_pct(x, digits=1):
    return "–" if x is None else ("%.*f%%" % (digits, 100 * x))


def fmt_ms(x):
    return "–" if x is None else ("%d" % round(x))


def fmt_f(x, d=2):
    return "–" if x is None else ("%.*f" % (d, x))


def fmt_mb(b):
    return "–" if b is None else ("%.1f" % (b / 1e6))


def table(header, rows, align=None):
    out = ["| " + " | ".join(header) + " |",
           "|" + "|".join((align or ["---"] * len(header))[i] if align else "---"
                          for i in range(len(header))) + "|"]
    for r in rows:
        out.append("| " + " | ".join(str(c) for c in r) + " |")
    return "\n".join(out)


class Report:
    def __init__(self, data, results, work):
        self.data, self.results, self.work = data, results, work
        self.pages = vbl.load_pages()
        self.utts = {u["id"]: u for u in vbl.load_testset(pages=self.pages)}
        self.summary = {}
        self.page_text = {pid: vbl.normalize(" ".join(t for it in p["items"] for t in it))
                          for pid, p in self.pages.items()}

    def jsonl(self, *parts):
        p = os.path.join(self.results, *parts)
        if not os.path.exists(p):
            return None
        return vbl.read_jsonl(p)

    def ref_for(self, rec_id):
        """The utterance (or a synthetic one for real speech) a clip id
        scores against."""
        uid = rec_id.split("@", 1)[0]
        if uid in self.utts:
            return self.utts[uid]
        return None

    def set_refs(self, set_name):
        """{clip id: utterance dict} for a set, including real speech."""
        path = os.path.join(self.work, "sets", set_name + ".tsv")
        refs = {}
        if not os.path.exists(path):
            return refs
        with open(path, encoding="utf-8") as f:
            for line in f:
                r = line.rstrip("\n").split("\t")
                if len(r) < 2:
                    continue
                if r[1].startswith("sc:") or r[1].startswith("ls:"):
                    text = r[1][3:]
                    refs[r[0]] = {"id": r[0], "category": "real", "accepted": [vbl.normalize(text)],
                                  "entities": [], "page": None}
                elif r[1] in self.utts:
                    refs[r[0]] = self.utts[r[1]]
        return refs

    def score_file(self, recs, refs):
        scores = []
        for r in recs or []:
            if r.get("event") != "utt" or r["id"] not in refs:
                continue
            s = vbl.score_utterance(refs[r["id"]], r.get("text", ""))
            s["category"] = refs[r["id"]]["category"]
            s["utt"] = refs[r["id"]]
            scores.append(s)
        return scores

    # ------------------------------------------------------------ sections

    def machine(self):
        p = os.path.join(self.results, "system.json")
        if not os.path.exists(p):
            return "(no system.json)"
        with open(p) as f:
            info = json.load(f)
        lines = []
        for line in info.get("lscpu", "").splitlines():
            k = line.split(":", 1)[0].strip()
            if k in ("Model name", "CPU(s)", "Thread(s) per core", "Hypervisor vendor",
                     "Virtualization type", "L3 cache"):
                lines.append("- %s: %s" % (k, line.split(":", 1)[1].strip()))
            if k == "Flags":
                flags = line.split(":", 1)[1].split()
                keep = [f for f in flags if f.startswith(("avx", "amx", "fma", "f16c", "sse4",
                                                          "vnni", "bmi"))]
                lines.append("- SIMD flags: %s" % " ".join(keep))
        if info.get("sysctl"):
            lines += ["- " + line for line in info["sysctl"].splitlines()]
        lines.append("- %s, Python %s, %s logical CPUs, %s" % (
            info.get("platform"), info.get("python"), info.get("cpu_count"),
            info.get("meminfo", "")))
        lines.append("- driver: `%s`" % info.get("driver", ""))
        return "\n".join(lines)

    def sizes(self):
        p = os.path.join(self.results, "sizes.json")
        if not os.path.exists(p):
            return "", ""
        with open(p) as f:
            sz = json.load(f)
        self.summary["sizes"] = sz
        rows = []
        for rid, libs in sorted(sz["runtimes"].items()):
            core = {k: v for k, v in libs.items() if "cxx-api" not in k
                    and not k.endswith((".lib",)) and "providers_shared" not in k}
            ort = sum(v for k, v in core.items() if "onnxruntime" in k)
            sh = sum(v for k, v in core.items() if "sherpa-onnx" in k or "kaldi" in k
                     or "ssentencepiece" in k or "utf8proc" in k or "kissfft" in k
                     or "fst" in k)
            rows.append((rid, fmt_mb(ort), fmt_mb(sh), fmt_mb(ort + sh)))
        rt = table(["release archive", "ONNX Runtime (MB)", "sherpa-onnx (MB)", "total (MB)"],
                   rows)
        mrows = []
        for mid, m in sorted(sz["models"].items(), key=lambda kv: (kv[1]["group"], kv[0])):
            ops = m.get("onnx_ops") or []
            mrows.append((m["group"].lower(), mid, fmt_mb(m["bytes"]),
                          "ORT format" if m.get("ort_format") else len(ops)))
        # The operators a reduced ONNX Runtime build would need for one
        # plausible bundle: wake phrase, VAD, a streaming recognizer, a voice.
        bundle = ["kws-gigaspeech-3.3m", "silero-vad", "zipformer-en-2023-06-26",
                  "piper-ljspeech-medium"]
        union = set()
        for b in bundle:
            union |= set(sz["models"].get(b, {}).get("onnx_ops") or [])
        self.summary["bundle_ops"] = {"models": bundle, "ops": sorted(union)}
        note = ("Distinct ONNX operators used by %s together: %d (%s)." %
                (", ".join(bundle), len(union), ", ".join(o.split(":")[1] for o in sorted(union))))
        return rt, table(["kind", "model", "files used (MB)", "distinct ONNX ops"],
                         mrows) + "\n\n" + note

    def accuracy(self):
        offline = [(m, "offline", e) for m, e in catalog.OFFLINE.items()]
        stream = [(m, "stream", e) for m, e in catalog.STREAM.items()]
        rows_clean, rows_cond_wer, rows_cond_cmd, rows_real, rows_cat = [], [], [], [], []
        acc = {}
        for mid, kind, e in offline + stream:
            per = {}
            for s in ["asr-clean-a", "asr-clean-b"] + ["asr-%s-a" % c for c in CONDS[1:]] + \
                    ["real-sc", "real-ls"]:
                recs = self.jsonl(kind, mid, s + ".jsonl")
                if recs is None:
                    continue
                per[s] = self.score_file(recs, self.set_refs(s))
            if not per:
                continue
            acc[mid] = {s: vbl.aggregate(v) for s, v in per.items()}
            clean = per.get("asr-clean-a", []) + per.get("asr-clean-b", [])
            if clean:
                a = vbl.aggregate(clean)
                rows_clean.append((e["label"], kind, a["n"], fmt_pct(a["wer"]),
                                   fmt_pct(a["command_acc"]), fmt_pct(a["entity_acc"])))
                cats = []
                for c in vbl.CATEGORIES:
                    sc = [x for x in clean if x["category"] == c]
                    cats.append(fmt_pct(vbl.aggregate(sc)["command_acc"], 0) if sc else "–")
                rows_cat.append([e["label"]] + cats)
            wr, cr = [e["label"]], [e["label"]]
            for c in CONDS:
                s = "asr-%s-a" % c
                a = vbl.aggregate(per[s]) if s in per else None
                wr.append(fmt_pct(a["wer"]) if a else "–")
                cr.append(fmt_pct(a["command_acc"], 0) if a else "–")
            rows_cond_wer.append(wr)
            rows_cond_cmd.append(cr)
            sc = vbl.aggregate(per["real-sc"]) if "real-sc" in per else None
            ls = vbl.aggregate(per["real-ls"]) if "real-ls" in per else None
            rows_real.append((e["label"], sc["n"] if sc else "–",
                              fmt_pct(sc["command_acc"]) if sc else "–",
                              fmt_pct(ls["wer"]) if ls else "–"))
        self.summary["accuracy"] = acc
        return {
            "clean": table(["model", "kind", "clips", "WER", "command acc.", "entity acc."],
                           rows_clean),
            "cats": table(["model"] + list(vbl.CATEGORIES), rows_cat),
            "cond_wer": table(["model"] + CONDS, rows_cond_wer),
            "cond_cmd": table(["model"] + CONDS, rows_cond_cmd),
            "real": table(["model", "Speech Commands words", "word correct",
                           "LibriSpeech + JFK WER (4 clips)"], rows_real),
        }

    def offline_timing(self):
        rows = []
        tim = {}
        for mid, e in catalog.OFFLINE.items():
            res = {}
            for t in (1, 2, 4):
                runs = []
                for rep in range(3):
                    recs = self.jsonl("timing", mid, "t%d.r%d.jsonl" % (t, rep))
                    if not recs:
                        continue
                    utt = [r for r in recs if r.get("event") == "utt"]
                    load = next((r for r in recs if r.get("event") == "load"), {})
                    end = next((r for r in recs if r.get("event") == "end"), {})
                    if not utt:
                        continue
                    runs.append({
                        "decode_ms": vbl.median([r["decode_ms"] for r in utt]),
                        "rtf": vbl.median([r["decode_ms"] / 1000 / r["audio_s"] for r in utt]),
                        "audio_s": vbl.median([r["audio_s"] for r in utt]),
                        "load_ms": load.get("load_ms"),
                        "peak_rss_kb": end.get("peak_rss_kb"),
                        "rss_after_load_kb": load.get("rss_after_load_kb")})
                if runs:
                    res[t] = {k: vbl.median([r[k] for r in runs]) for k in runs[0]}
                    res[t]["runs"] = len(runs)
            if not res:
                continue
            tim[mid] = res
            first = res[min(res)]
            rows.append((e["label"],
                         fmt_ms(res.get(1, {}).get("decode_ms")),
                         fmt_ms(res.get(2, {}).get("decode_ms")),
                         fmt_ms(res.get(4, {}).get("decode_ms")),
                         fmt_f(res.get(1, {}).get("rtf"), 3),
                         fmt_f(res.get(4, {}).get("rtf"), 3),
                         fmt_ms(first.get("load_ms")),
                         fmt_mb((first.get("peak_rss_kb") or 0) * 1024 or None),
                         fmt_ms(500 + (res.get(2, {}).get("decode_ms") or 0))))
        self.summary["offline_timing"] = tim
        return table(["model", "decode ms, 1 thr", "2 thr", "4 thr", "RTF 1 thr", "RTF 4 thr",
                      "load ms", "peak RSS MB", "end of speech → text, ms (2 thr)"], rows)

    def stream_timing(self):
        rows = []
        tim = {}
        for mid, e in catalog.STREAM.items():
            res = {}
            for t in (1, 2):
                runs = []
                for rep in range(3):
                    recs = self.jsonl("stimes", mid, "t%d.r%d.jsonl" % (t, rep))
                    if not recs:
                        continue
                    utt = [r for r in recs if r.get("event") == "utt"]
                    load = next((r for r in recs if r.get("event") == "load"), {})
                    end = next((r for r in recs if r.get("event") == "end"), {})
                    if not utt:
                        continue
                    runs.append({
                        "first_partial_ms": vbl.median([r.get("first_partial_ms") for r in utt]),
                        "first_partial_p90": vbl.percentile([r.get("first_partial_ms")
                                                             for r in utt], 90),
                        "final_ms": vbl.median([r.get("final_after_speech_ms") for r in utt]),
                        "final_p90": vbl.percentile([r.get("final_after_speech_ms")
                                                     for r in utt], 90),
                        "vad_end_ms": vbl.median([r.get("vad_end_after_speech_ms")
                                                  for r in utt]),
                        "endpoint_ms": vbl.median([r.get("endpoint_after_speech_ms")
                                                   for r in utt]),
                        "rtf": vbl.median([r["compute_ms"] / 1000 / r["audio_s"] for r in utt]),
                        "load_ms": load.get("load_ms"),
                        "peak_rss_kb": end.get("peak_rss_kb")})
                if runs:
                    res[t] = {k: vbl.median([r[k] for r in runs]) for k in runs[0]}
                    res[t]["runs"] = len(runs)
            if not res:
                continue
            tim[mid] = res
            for t, r in sorted(res.items()):
                rows.append((e["label"], t, fmt_ms(r["first_partial_ms"]),
                             fmt_ms(r["first_partial_p90"]), fmt_ms(r["vad_end_ms"]),
                             fmt_ms(r["final_ms"]), fmt_ms(r["final_p90"]),
                             fmt_ms(r["endpoint_ms"]), fmt_f(r["rtf"], 3), fmt_ms(r["load_ms"]),
                             fmt_mb((r["peak_rss_kb"] or 0) * 1024 or None)))
        self.summary["stream_timing"] = tim
        return table(["model", "threads", "first partial ms", "p90", "VAD end ms",
                      "final text ms", "p90", "built-in endpoint ms", "RTF", "load ms",
                      "peak RSS MB"], rows)

    def listen(self):
        rows, out = [], {}
        base = os.path.join(self.results, "listen")
        if not os.path.isdir(base):
            return ""
        labels = {"kws": "wake phrase (Zipformer KWS 3.3M) + VAD"}
        for mid, e in catalog.STREAM.items():
            labels[mid] = e["label"] + " + VAD"
        for eng in sorted(os.listdir(base)):
            for variant in ("t1", "t2", "t2-nospin"):
                vals, late = [], []
                for rep in range(3):
                    recs = self.jsonl("listen", eng, "%s.r%d.jsonl" % (variant, rep))
                    for r in recs or []:
                        if r.get("event") == "listen":
                            vals.append(r["cpu_pct_of_one_core"])
                            late.append(r["max_late_ms"])
                if vals:
                    out.setdefault(eng, {})[variant] = vbl.median(vals)
                    rows.append((labels.get(eng, eng), variant, fmt_f(vbl.median(vals), 1),
                                 fmt_ms(vbl.median(late))))
        self.summary["listen"] = out
        return table(["listening", "threads", "CPU, % of one core (median)",
                      "worst lag behind real time, ms"], rows)

    def bias(self):
        rows = []
        out = {}
        for mid, e in catalog.STREAM.items():
            for s in ("asr-clean-a", "asr-pink10-a"):
                refs = self.set_refs(s)
                cfgs = [("beam", s + ".beam.jsonl"), ("hot 1.5", s + ".hot1.5.jsonl"),
                        ("hot 3.0", s + ".hot3.0.jsonl")]
                res = {}
                errs = {}
                for name, fn in cfgs:
                    recs = self.jsonl("bias", mid, fn)
                    if recs is None:
                        continue
                    errs[name] = (recs[0].get("hotword_errors", 0) if recs else 0)
                    sc = self.score_file(recs, refs)
                    on, off = [], []
                    for x in sc:
                        u = x["utt"]
                        for (name_e, forms), hit in zip(u["entities"], self._entity_hits(x)):
                            onscreen = vbl.contains_words(self.page_text[u["page"]], forms)
                            (on if onscreen else off).append(hit)
                    agg = vbl.aggregate(sc)
                    res[name] = {"wer": agg["wer"], "cmd": agg["command_acc"],
                                 "ent": agg["entity_acc"],
                                 "on": (sum(on) / len(on)) if on else None, "n_on": len(on),
                                 "off": (sum(off) / len(off)) if off else None,
                                 "n_off": len(off)}
                if not res:
                    continue
                out.setdefault(mid, {})[s] = res
                for name, r in res.items():
                    rows.append((e["label"], s.replace("asr-", "").replace("-a", ""), name,
                                 fmt_pct(r["wer"]), fmt_pct(r["cmd"]),
                                 "%s (%d)" % (fmt_pct(r["on"]), r["n_on"]),
                                 "%s (%d)" % (fmt_pct(r["off"]), r["n_off"]),
                                 errs.get(name, 0)))
        self.summary["bias"] = out
        return table(["model", "set", "decoding", "WER", "command acc.",
                      "names on screen right (n)", "names not on screen right (n)",
                      "hotword encode errors"], rows)

    @staticmethod
    def _entity_hits(score):
        u = score["utt"]
        return [vbl.contains_words(score["hyp"], forms) for _, forms in u["entities"]]

    def kws(self):
        rows, out = [], {}
        base = os.path.join(self.results, "kws")
        if not os.path.isdir(base):
            return ""
        for kid, e in catalog.KWS.items():
            for phrase in catalog.WAKE_PHRASES:
                tag = phrase.replace(" ", "_")
                d = os.path.join(base, kid, tag)
                if not os.path.isdir(d):
                    continue
                for th in sorted(os.listdir(d)):
                    r = {}
                    fr = []
                    delays = []
                    for c in ("clean", "pink10", "babble10", "music10"):
                        recs = self.jsonl("kws", kid, tag, th, "wake-%s.jsonl" % c)
                        if not recs:
                            continue
                        rows_set = self._set_index("wake-%s-%s" % (tag, c))
                        utt = [x for x in recs if x.get("event") == "kws"]
                        miss = 0
                        for x in utt:
                            hits = [h for h in x["hits"]]
                            if not hits:
                                miss += 1
                            else:
                                pe = rows_set.get(x["id"])
                                if pe is not None:
                                    delays.append(1000 * (hits[0]["at_s"] - pe))
                        r["fr_" + c] = miss / len(utt) if utt else None
                        fr.append((miss, len(utt)))
                    fa = {}
                    for group, sets in (("speech", ["asr-clean-a", "asr-clean-b", "asr-pink10-a",
                                                    "asr-babble10-a", "asr-music10-a"]),
                                        ("echo", ["asr-echo0-a"]),
                                        ("page", ["fa-page"]), ("real", ["fa-real-sc"])):
                        n_hits, secs = 0, 0.0
                        for s in sets:
                            recs = self.jsonl("kws", kid, tag, th, s + ".jsonl")
                            for x in recs or []:
                                if x.get("event") == "kws":
                                    n_hits += len(x["hits"])
                                    secs += x["audio_s"]
                        fa[group] = (n_hits, secs)
                    r["fa"] = fa
                    r["delay_ms"] = vbl.median(delays)
                    out.setdefault(kid, {}).setdefault(phrase, {})[th] = r
                    tot_miss = sum(m for m, _ in fr)
                    tot = sum(n for _, n in fr)

                    def per_hour(g):
                        n, s = fa.get(g, (0, 0))
                        return "–" if not s else "%.1f (%d in %.1f h)" % (n / (s / 3600), n,
                                                                          s / 3600)
                    rows.append((e["label"], phrase, th,
                                 fmt_pct(r.get("fr_clean")), fmt_pct(r.get("fr_pink10")),
                                 fmt_pct(r.get("fr_babble10")), fmt_pct(r.get("fr_music10")),
                                 fmt_pct(tot_miss / tot if tot else None),
                                 per_hour("speech"), per_hour("echo"), per_hour("page"),
                                 per_hour("real"), fmt_ms(r["delay_ms"])))
        self.summary["kws"] = out
        return table(["model", "phrase", "threshold", "FR clean", "FR pink 10 dB",
                      "FR babble", "FR music", "FR all", "FA/h non-wake speech",
                      "FA/h browser speech (echo0)", "FA/h page audio",
                      "FA/h real speech", "delay ms"], rows)

    def _set_index(self, set_name):
        """{clip id: end of the wake phrase (s)} for a wake set."""
        path = os.path.join(self.work, "sets", set_name + ".tsv")
        out = {}
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                for line in f:
                    r = line.rstrip("\n").split("\t")
                    if len(r) >= 9:
                        out[r[0]] = float(r[8])
        return out

    def tts(self):
        rows, out = [], {}
        szp = os.path.join(self.results, "sizes.json")
        sizes = json.load(open(szp))["models"] if os.path.exists(szp) else {}
        for tid, e in catalog.TTS.items():
            res = {}
            for t in (1, 2, 4):
                runs = []
                for rep in range(3):
                    recs = self.jsonl("tts", tid, "t%d.r%d.jsonl" % (t, rep))
                    if not recs:
                        continue
                    items = [r for r in recs if r.get("event") == "tts"]
                    load = next((r for r in recs if r.get("event") == "load"), {})
                    end = next((r for r in recs if r.get("event") == "end"), {})
                    if not items:
                        continue
                    run = {"load_ms": load.get("load_ms"), "peak_rss_kb": end.get("peak_rss_kb")}
                    for kind in ("short", "result", "list"):
                        its = [r for r in items if r["id"].startswith(kind)]
                        run["first_" + kind] = vbl.median([r["first_audio_ms"] for r in its])
                    run["rtf"] = vbl.median([r["gen_ms"] / 1000 / r["audio_s"] for r in items])
                    runs.append(run)
                if runs:
                    res[t] = {k: vbl.median([r[k] for r in runs]) for k in runs[0]}
            if not res:
                continue
            out[tid] = res
            for t, r in sorted(res.items()):
                rows.append((e["label"], t, fmt_ms(r["first_short"]), fmt_ms(r["first_result"]),
                             fmt_ms(r["first_list"]), fmt_f(r["rtf"], 3), fmt_ms(r["load_ms"]),
                             fmt_mb((r["peak_rss_kb"] or 0) * 1024 or None),
                             fmt_mb(sizes.get(tid, {}).get("bytes"))))
        self.summary["tts"] = out
        return table(["voice", "threads", "first audio ms: short", "one result",
                      "list of three", "RTF", "load ms", "peak RSS MB", "model MB"], rows)

    def lexicon_coverage(self):
        """How many words of real page text each TTS lexicon lacks: since
        sherpa-onnx 2.0 those words are skipped (not spelled, not guessed)."""
        import gen_audio
        lexicons = {}
        p = os.path.join(self.data, "a", "tts-lexicon-en-us", "lexicon-en-us.txt")
        if os.path.exists(p):
            lexicons["lexicon-en-us.txt (Piper/VITS, Matcha)"] = set(gen_audio.read_new_lexicon(p))
        p = os.path.join(self.data, "a", "tts-kokoro-v1_0-int8", "lexicon-us-en.txt")
        if os.path.exists(p):
            lexicons["lexicon-us-en.txt (Kokoro v1.0)"] = set(gen_audio.read_old_lexicon(p))
        if not lexicons:
            return ""
        corpora = {
            "test-set commands": [u["text"] for u in self.utts.values()],
            "result titles and channels": [t for pg in self.pages.values()
                                           for it in pg["items"] for t in it],
            "docs prose (400 sentences)": gen_audio.docs_sentences(400),
        }
        rows, out = [], {}
        for lname, lex in lexicons.items():
            for cname, texts in corpora.items():
                words = [w for t in texts for w in gen_audio.tts_words(t)]
                miss = [w for w in words if w not in lex]
                examples = sorted(set(miss), key=lambda w: -miss.count(w))[:8]
                out.setdefault(lname, {})[cname] = {"words": len(words), "missing": len(miss)}
                rows.append((lname, cname, len(words), fmt_pct(len(miss) / len(words)
                                                               if words else None),
                             ", ".join(examples)))
        self.summary["lexicon_coverage"] = out
        return table(["lexicon", "text", "words", "skipped (not in lexicon)",
                      "most frequent skipped words"], rows)

    def whisper_cpp(self):
        """whisper-bench timings (median of runs) and, with real models,
        whisper-cli per request."""
        base = os.path.join(self.results, "whispercpp-bench")
        if not os.path.isdir(base):
            return ""
        rows, out = [], {}
        for model in ("tiny.en", "base.en", "small.en"):
            for t in (1, 2, 4):
                runs = []
                for rep in range(3):
                    p = os.path.join(base, model, "t%d.r%d.json" % (t, rep))
                    if os.path.exists(p):
                        with open(p) as f:
                            runs.append(json.load(f))
                runs = [r for r in runs if r.get("returncode") == 0 and r.get("timings")]
                if not runs:
                    continue

                def med(key, field="per_run_ms"):
                    return vbl.median([r["timings"].get(key, {}).get(field) for r in runs])
                enc, dec, bat, load = med("encode"), med("decode"), med("batchd"), med("load",
                                                                                     "total_ms")
                # One short command: a 30 s encoder window, then about 12
                # decoder steps of a 5-wide beam (whisper-cli's default).
                est = None if enc is None or bat is None else (load or 0) + enc + 12 * bat
                out.setdefault(model, {})[t] = {"encode_ms": enc, "decode_token_ms": dec,
                                                "beam5_step_ms": bat, "load_ms": load,
                                                "weights": runs[0].get("weights"),
                                                "estimate_per_request_ms": est}
                rows.append(("whisper.cpp %s" % model, runs[0].get("weights"), t, fmt_ms(load),
                             fmt_ms(enc), fmt_f(dec, 1), fmt_f(bat, 1), fmt_ms(est)))
        self.summary["whisper_cpp"] = out
        text = table(["engine", "weights", "threads", "load ms", "encode ms (30 s window)",
                      "decode ms/token", "5-beam step ms",
                      "≈ per request ms (load + encode + 12 steps)"], rows)
        tr = []
        for model in ("tiny.en", "base.en", "small.en"):
            for s in ("asr-clean-a", "asr-pink10-a", "real-sc"):
                recs = self.jsonl("whispercpp", model, s + ".jsonl")
                if not recs:
                    continue
                sc = self.score_file(recs, self.set_refs(s))
                a = vbl.aggregate(sc)
                ms = vbl.median([r.get("process_ms") for r in recs if r.get("event") == "utt"])
                tr.append(("whisper-cli %s" % model, s, a["n"], fmt_pct(a["wer"]),
                           fmt_pct(a["command_acc"]), fmt_ms(ms)))
        if tr:
            text += "\n\n" + table(["engine", "set", "clips", "WER", "command acc.",
                                    "median ms per request (new process)"], tr)
        return text

    def licences(self):
        rows = []
        for a in fetch.load_manifest():
            rows.append((a["id"], a["license"], a.get("license_note", "")))
        return table(["artifact", "licence", "source / note"], rows)


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--dir", default=fetch.default_dir())
    p.add_argument("--work", default="")
    p.add_argument("--results", default="")
    p.add_argument("--out", default="")
    p.add_argument("--json", default="")
    args = p.parse_args(argv)
    data = os.path.abspath(args.dir)
    rep = Report(data, os.path.abspath(args.results or os.path.join(data, "results")),
                 os.path.abspath(args.work or os.path.join(data, "work")))
    acc = rep.accuracy()
    rt, models = rep.sizes()
    parts = [
        "# Voice bench results", "",
        "## Machine", "", rep.machine(), "",
        "## Recognition accuracy, synthetic speech (clean, two voices per utterance)", "",
        acc["clean"], "",
        "### Command accuracy by category (clean)", "", acc["cats"], "",
        "### WER by condition (the 'a' voices)", "", acc["cond_wer"], "",
        "### Command accuracy by condition", "", acc["cond_cmd"], "",
        "## Real speech", "", acc["real"], "",
        "## Per-utterance recognizers: decode time (median of 3 runs)", "",
        rep.offline_timing(), "",
        "## Streaming recognizers with Silero VAD (0.5 s), simulated real time", "",
        rep.stream_timing(), "",
        "## Listening all the time (real time, 30 s of page audio)", "", rep.listen(), "",
        "## Biasing toward the page's names", "", rep.bias(), "",
        "## Wake phrase", "", rep.kws(), "",
        "## Voices", "", rep.tts(), "",
        "### Words the voices skip", "", rep.lexicon_coverage(), "",
        "## whisper.cpp, as MoltBrowser ships it", "", rep.whisper_cpp(), "",
        "## Size on disk", "", rt, "", models, "",
        "## Licences", "", rep.licences(), "",
    ]
    text = "\n".join(parts) + "\n"
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(rep.summary, f, indent=1, default=str)
    return 0


if __name__ == "__main__":
    sys.exit(main())
