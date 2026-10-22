#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Builds and runs MoltBrowser's libFuzzer targets without a Chromium checkout.

The same *_fuzzer.cc files are fuzzer_test() targets in GN (see the BUILD.gn
next to each), where they build against the real //base, //url, //crypto and
llama.cpp. Here they build with the system clang
(-fsanitize=fuzzer,address,undefined) in one of these tiers:

  std        the product code is std-only; the harness's CHECK comes from
             tools/fuzz/stand_ins/base/check.h
  zlib       std plus the system zlib
  llama      std plus the vendored llama.cpp grammar parser
  stand_ins  the product code needs //base, //url or //crypto: it builds
             against the stand-ins in tools/fuzz/stand_ins/ (OpenSSL's
             libcrypto behind the crypto ones). They approximate Chromium's
             behaviour (see each header); a crash outside a stand-in is a
             real bug in MoltBrowser's code.

Usage:
  tools/fuzz/molt_fuzz.py list
  tools/fuzz/molt_fuzz.py check
  tools/fuzz/molt_fuzz.py build [--tier std,zlib] [--clean] [NAME ...]
  tools/fuzz/molt_fuzz.py run [--seconds N] [--runs N] [--seed N]
                              [--tier ...] [NAME ...] [-- LIBFUZZER_FLAGS]
  tools/fuzz/molt_fuzz.py smoke [NAME ...]    # what CI runs

`check` verifies that every fuzzer here is a fuzzer_test() in GN with a
seed corpus, and the reverse. `smoke` runs the std, zlib and llama tiers
with a fixed seed and a fixed number of runs each. Output goes to
out/fuzz/: bin/, obj/, corpus/<name>/ (what runs found; the checked-in seed
corpus next to each fuzzer is only read) and artifacts/<name>/ (crashing
inputs). See docs/FUZZING.md.
"""

import argparse
import concurrent.futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "src")
STAND_INS = os.path.join(ROOT, "tools", "fuzz", "stand_ins")
OUT = os.path.join(ROOT, "out", "fuzz")

INFERENCE = "src/chrome/services/molt_ai/inference/fuzzers"
ENGINE = "src/chrome/services/molt_ai/inference/engine"
BROWSER = "src/chrome/browser/molt_ai/fuzzers"
MOLT_AI = "src/chrome/browser/molt_ai"
DOCUMENT_PARSER = "src/chrome/services/molt_ai/document_parser"
FINGERPRINTING = "src/third_party/blink/renderer/core/molt_fingerprinting"
LLAMA = "src/third_party/llama_cpp"

# name -> where the fuzzer lives (its .cc, BUILD.gn and corpus/<name>/), the
# product sources it links, its tier, the libFuzzer options it runs with
# (the same as in its GN fuzzer_test) and the smoke run's size.
FUZZERS = {
    # --- The inference service (page schemas, model output) ---------------
    "molt_bounded_json_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/bounded_json.cc"],
        tier="std",
        dict="dicts/json.dict",
        more_seeds=["corpus/molt_json_schema_grammar_fuzzer"],
        max_len=8192,
        smoke_runs=400000,
    ),
    "molt_json_schema_grammar_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/json_schema_grammar.cc",
                 f"{ENGINE}/bounded_json.cc"],
        tier="std",
        dict="dicts/json.dict",
        max_len=8192,
        smoke_runs=80000,
    ),
    "molt_json_schema_grammar_llama_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/json_schema_grammar.cc",
                 f"{ENGINE}/bounded_json.cc"],
        llama_sources=[f"{LLAMA}/src/llama-grammar.cpp",
                       "tools/fuzz/stand_ins/llama_grammar_deps.cc"],
        tier="llama",
        dict="dicts/json.dict",
        max_len=8192,
        smoke_runs=20000,
    ),
    "molt_stream_filters_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/stream_filters.cc"],
        tier="std",
        dict="dicts/model_output.dict",
        max_len=16384,
        smoke_runs=150000,
    ),
    "molt_grammar_tokens_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/grammar_tokens.cc", f"{ENGINE}/prompt_utils.cc",
                 f"{ENGINE}/stream_filters.cc"],
        tier="std",
        max_len=4096,
        smoke_runs=100000,
    ),
    "molt_prompt_utils_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/prompt_utils.cc"],
        tier="std",
        dict="dicts/model_output.dict",
        max_len=8192,
        smoke_runs=400000,
    ),
    "molt_embedding_utils_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/embedding_utils.cc"],
        tier="std",
        max_len=8192,
        smoke_runs=600000,
    ),
    "molt_image_input_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/image_input.cc"],
        tier="std",
        max_len=4096,
        smoke_runs=100000,
    ),
    "molt_context_budget_fuzzer": dict(
        dir=INFERENCE,
        sources=[f"{ENGINE}/context_budget.cc"],
        tier="std",
        max_len=4096,
        smoke_runs=400000,
    ),
    # --- The document parser service (files from sites and users) ---------
    "molt_pdf_text_scraper_fuzzer": dict(
        dir=f"{DOCUMENT_PARSER}/fuzzers",
        sources=[f"{DOCUMENT_PARSER}/pdf/pdf_text_scraper.cc"],
        tier="zlib",
        dict="dicts/pdf.dict",
        max_len=65536,
        smoke_runs=12000,
    ),
    "molt_docx_text_extractor_fuzzer": dict(
        dir=f"{DOCUMENT_PARSER}/fuzzers",
        sources=[f"{DOCUMENT_PARSER}/docx_text_extractor.cc"],
        tier="stand_ins",
        dict="dicts/docx.dict",
        max_len=65536,
    ),
    # --- The browser: what the service sends back -------------------------
    "molt_stream_validator_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/runtime/stream_validator.cc",
                 f"{ENGINE}/stream_filters.cc"],
        tier="std",
        dict="//chrome/services/molt_ai/inference/fuzzers/dicts/model_output.dict",
        max_len=16384,
        smoke_runs=350000,
    ),
    "molt_embedding_validator_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/runtime/embedding_validator.cc"],
        tier="std",
        max_len=8192,
        smoke_runs=500000,
    ),
    # --- The browser: what web pages pass to the built-in AI APIs ---------
    "molt_web_ai_translation_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/web_ai/web_ai_translation.cc",
                 f"{MOLT_AI}/web_ai/web_ai_policy.cc",
                 f"{MOLT_AI}/web_ai/web_ai_prompts.cc",
                 f"{ENGINE}/stream_filters.cc"],
        tier="std",
        dict="dicts/page_text.dict",
        max_len=16384,
        smoke_runs=20000,
    ),
    "molt_web_ai_policy_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/web_ai/web_ai_policy.cc"],
        tier="std",
        max_len=8192,
        smoke_runs=10000,
    ),
    "molt_web_ai_conversation_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/web_ai/web_ai_conversation.cc",
                 f"{MOLT_AI}/web_ai/web_ai_policy.cc",
                 f"{MOLT_AI}/web_ai/web_ai_prompts.cc",
                 f"{MOLT_AI}/runtime/model_image.cc",
                 f"{ENGINE}/image_input.cc"],
        tier="std",
        max_len=16384,
        smoke_runs=50000,
    ),
    "molt_web_ai_site_permissions_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/web_ai/web_ai_site_permissions.cc"],
        tier="std",
        max_len=8192,
        smoke_runs=50000,
    ),
    # --- The browser: page text ------------------------------------------
    "molt_dom_interpreter_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/dom/dom_interpreter.cc"],
        tier="std",
        dict="dicts/html.dict",
        max_len=65536,
        smoke_runs=12000,
    ),
    "molt_memory_snippet_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/memory/memory_snippet.cc",
                 f"{ENGINE}/stream_filters.cc"],
        tier="std",
        dict="dicts/page_text.dict",
        max_len=16384,
        smoke_runs=200000,
    ),
    # --- The browser: agents and workflows (//base stand-ins) -------------
    "molt_agent_actions_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/agents/agent_actions.cc",
                 f"{MOLT_AI}/agents/agent_tools.cc",
                 f"{MOLT_AI}/agents/chat_actions.cc",
                 f"{MOLT_AI}/security/agent_safety.cc",
                 f"{ENGINE}/json_schema_grammar.cc",
                 f"{ENGINE}/bounded_json.cc"],
        tier="stand_ins",
        dict="dicts/agent.dict",
        max_len=16384,
    ),
    "molt_agent_vision_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/agents/agent_vision.cc",
                 f"{MOLT_AI}/agents/agent_actions.cc",
                 f"{MOLT_AI}/agents/agent_tools.cc",
                 f"{MOLT_AI}/security/agent_safety.cc",
                 f"{MOLT_AI}/runtime/model_image.cc",
                 f"{ENGINE}/image_input.cc",
                 f"{ENGINE}/json_schema_grammar.cc",
                 f"{ENGINE}/bounded_json.cc"],
        tier="stand_ins",
        max_len=16384,
    ),
    "molt_agent_safety_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/security/agent_safety.cc"],
        tier="stand_ins",
        dict="dicts/page_text.dict",
        max_len=16384,
    ),
    "molt_automation_script_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/automation/automation_script.cc",
                 f"{MOLT_AI}/automation/automation_security.cc",
                 f"{MOLT_AI}/automation/automation_ai_steps.cc",
                 f"{MOLT_AI}/security/agent_safety.cc",
                 f"{ENGINE}/json_schema_grammar.cc",
                 f"{ENGINE}/bounded_json.cc"],
        tier="stand_ins",
        dict="dicts/agent.dict",
        max_len=16384,
    ),
    "molt_automation_ai_steps_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/automation/automation_ai_steps.cc",
                 f"{ENGINE}/json_schema_grammar.cc",
                 f"{ENGINE}/bounded_json.cc"],
        tier="stand_ins",
        dict="dicts/agent.dict",
        max_len=16384,
    ),
    # --- MoltNet ----------------------------------------------------------
    "molt_onion_location_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/tor/onion_util.cc"],
        tier="std",
        dict="dicts/tor.dict",
        max_len=8192,
        smoke_runs=1000000,
    ),
    "molt_torrc_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/tor/torrc_builder.cc",
                 f"{MOLT_AI}/tor/pluggable_transports.cc"],
        tier="std",
        dict="dicts/tor.dict",
        max_len=16384,
        smoke_runs=200000,
    ),
    "molt_tor_control_protocol_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/tor/tor_control_protocol.cc"],
        tier="stand_ins",
        dict="dicts/tor.dict",
        max_len=8192,
    ),
    # --- Fingerprinting protection (Blink, pure parts) --------------------
    "molt_farbling_fuzzer": dict(
        dir=FINGERPRINTING,
        sources=[f"{FINGERPRINTING}/farbling.cc",
                 f"{FINGERPRINTING}/fonts.cc",
                 f"{FINGERPRINTING}/font_allowlists.cc"],
        tier="stand_ins",
        max_len=8192,
    ),
    # --- In-app updates ---------------------------------------------------
    "molt_release_manifest_fuzzer": dict(
        dir=BROWSER,
        sources=[f"{MOLT_AI}/update/release_manifest.cc"],
        tier="stand_ins",
        dict="//chrome/services/molt_ai/inference/fuzzers/dicts/json.dict",
        max_len=16384,
    ),
}

# What each tier links beyond the fuzzer's own objects.
TIER_LDFLAGS = {
    "std": [],
    "zlib": ["-lz"],
    "llama": [],
    "stand_ins": ["-lcrypto", "-lz"],
}

SMOKE_TIERS = ("std", "zlib", "llama")
# smoke_runs (per fuzzer) is sized to about 12 s on a desktop core, so a
# CI runner takes 20-30 s; this is for the tiers CI doesn't run.
DEFAULT_SMOKE_RUNS = 50000
# A smoke run stops after its runs; this only stops one on a very slow
# runner (libFuzzer then exits cleanly: fewer inputs, same verdict on them).
SMOKE_GUARD_SECONDS = 60

CXX = os.environ.get("CXX", "clang++")
CC = os.environ.get("CC", "clang")

COMMON_FLAGS = [
    "-g",
    "-O1",
    "-fno-omit-frame-pointer",
    "-fsanitize=address,undefined",
    "-fno-sanitize-recover=all",
    # Bounds-checked operator[] and friends, like Chromium's hardened libc++.
    "-D_GLIBCXX_ASSERTIONS",
]
# MoltBrowser code builds as Chromium builds it: no exceptions.
MOLT_FLAGS = [
    "-std=c++20",
    "-fno-exceptions",
    "-Wall",
    "-Wextra",
    "-Wno-unused-parameter",
    "-Wno-missing-field-initializers",
]
# llama.cpp's own sources build with exceptions, as its BUILD.gn does.
LLAMA_CXX_FLAGS = ["-std=c++17", "-fexceptions", "-w"]
LLAMA_INCLUDES = [
    f"{LLAMA}/include",
    f"{LLAMA}/ggml/include",
    f"{LLAMA}/src",
    f"{LLAMA}/chromium",
]


def rel(path):
    return os.path.relpath(path, ROOT)


def fuzzer_source(name):
    return os.path.join(ROOT, FUZZERS[name]["dir"], name + ".cc")


def seed_dir(name):
    return os.path.join(ROOT, FUZZERS[name]["dir"], "corpus", name)


def resolve(name, path):
    """|path| as GN writes it: relative to the fuzzer's directory, or
    source-absolute ("//chrome/...", under src/)."""
    if path.startswith("//"):
        return os.path.join(SRC, path[2:])
    return os.path.join(ROOT, FUZZERS[name]["dir"], path)


def seed_dirs(name):
    """The fuzzer's own seed corpus, then any it shares (GN seed_corpuses)."""
    return [seed_dir(name)] + [resolve(name, d)
                               for d in FUZZERS[name].get("more_seeds", [])]


def dict_path(name):
    d = FUZZERS[name].get("dict")
    return resolve(name, d) if d else None


def obj_path(source, flags):
    """Objects are keyed by source and flags, so a source several fuzzers
    share with the same flags is compiled once."""
    digest = hashlib.sha1("\0".join(flags).encode()).hexdigest()[:10]
    stem = os.path.splitext(rel(source))[0]
    return os.path.join(OUT, "obj", digest, stem + ".o")


def compile_one(source, flags):
    """Compiles |source| unless its object is newer (headers aren't tracked:
    use --clean after editing one)."""
    out = obj_path(source, flags)
    if os.path.exists(out) and os.path.getmtime(out) > os.path.getmtime(source):
        return out, None
    os.makedirs(os.path.dirname(out), exist_ok=True)
    cmd = [CC if source.endswith(".c") else CXX] + flags
    cmd += ["-c", source, "-o", out]
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if proc.returncode != 0:
        return None, f"$ {' '.join(cmd)}\n{proc.stdout}{proc.stderr}"
    return out, None


def molt_flags(name):
    """Flags for a fuzzer and the MoltBrowser sources it links."""
    info = FUZZERS[name]
    flags = COMMON_FLAGS + ["-fsanitize=fuzzer-no-link"] + MOLT_FLAGS
    # Stand-ins first: base/check.h for every harness, and the //base, //url,
    # //crypto and zlib headers of the stand_ins tier.
    flags += ["-I", STAND_INS, "-I", SRC]
    if info["tier"] == "llama":
        for inc in LLAMA_INCLUDES:
            flags += ["-I", os.path.join(ROOT, inc)]
    return flags + info.get("cflags", [])


def llama_flags(source):
    flags = COMMON_FLAGS + ["-fsanitize=fuzzer-no-link"]
    flags += ["-std=c11", "-w"] if source.endswith(".c") else LLAMA_CXX_FLAGS
    for inc in LLAMA_INCLUDES:
        flags += ["-I", os.path.join(ROOT, inc)]
    return flags


def units(name):
    """(source, flags) of everything |name| links."""
    info = FUZZERS[name]
    out = [(fuzzer_source(name), molt_flags(name))]
    out += [(os.path.join(ROOT, s), molt_flags(name)) for s in info["sources"]]
    out += [(os.path.join(ROOT, s), llama_flags(s))
            for s in info.get("llama_sources", [])]
    return out


def build(names, jobs):
    """Compiles and links |names|; returns the names that failed."""
    tasks = {}
    for name in names:
        for source, flags in units(name):
            tasks[(source, tuple(flags))] = (source, flags)

    objects = {}
    errors = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        futures = {pool.submit(compile_one, src, flags): key
                   for key, (src, flags) in tasks.items()}
        for f in concurrent.futures.as_completed(futures):
            out, err = f.result()
            if err:
                errors[futures[f]] = err
            else:
                objects[futures[f]] = out

    failed = []
    for name in names:
        keys = [(source, tuple(flags)) for source, flags in units(name)]
        bad = [errors[k] for k in keys if k in errors]
        if bad:
            print(f"FAILED to compile {name}:\n" + "\n".join(bad),
                  file=sys.stderr)
            failed.append(name)
            continue
        os.makedirs(os.path.join(OUT, "bin"), exist_ok=True)
        binary = os.path.join(OUT, "bin", name)
        cmd = [CXX] + COMMON_FLAGS + ["-fsanitize=fuzzer"]
        cmd += [objects[k] for k in keys]
        cmd += TIER_LDFLAGS[FUZZERS[name]["tier"]]
        cmd += FUZZERS[name].get("ldflags", []) + ["-o", binary]
        proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if proc.returncode != 0:
            print(f"FAILED to link {name}:\n$ {' '.join(cmd)}\n"
                  f"{proc.stdout}{proc.stderr}", file=sys.stderr)
            failed.append(name)
            continue
        print(f"built {rel(binary)}", flush=True)
    return failed


def run(name, seconds, runs, seed, extra):
    """Runs |name| over its seed corpus and out/fuzz/corpus/<name>/."""
    info = FUZZERS[name]
    binary = os.path.join(OUT, "bin", name)
    work = os.path.join(OUT, "corpus", name)
    artifacts = os.path.join(OUT, "artifacts", name)
    os.makedirs(work, exist_ok=True)
    os.makedirs(artifacts, exist_ok=True)
    cmd = [binary, f"-seed={seed}",
           f"-max_len={info.get('max_len', 4096)}",
           f"-timeout={info.get('timeout', 25)}",
           f"-rss_limit_mb={info.get('rss_limit_mb', 2560)}",
           f"-artifact_prefix={artifacts}/",
           "-print_final_stats=1"]
    if seconds:
        cmd.append(f"-max_total_time={seconds}")
    if runs:
        cmd.append(f"-runs={runs}")
    d = dict_path(name)
    if d:
        cmd.append(f"-dict={d}")
    cmd += info.get("options", []) + extra
    cmd.append(work)
    cmd += [d for d in seed_dirs(name) if os.path.isdir(d)]
    shown = " ".join(rel(c) if c.startswith(ROOT) else c for c in cmd)
    print(f"== {name}: {shown}", flush=True)
    start = time.time()
    proc = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    elapsed = time.time() - start
    if proc.returncode != 0:
        print(proc.stderr[-30000:], file=sys.stderr)
        print(f"== {name}: FAILED (exit {proc.returncode}) after "
              f"{elapsed:.0f}s; the input is in {rel(artifacts)}/", flush=True)
        return False
    for line in proc.stderr.splitlines():
        if line.startswith(("stat::number_of_executed_units",
                            "stat::peak_rss_mb", "Done ")) or " DONE " in line:
            print("   " + line)
    print(f"== {name}: ok ({elapsed:.0f}s)", flush=True)
    return True


def select(names, tiers):
    names = names or sorted(FUZZERS)
    unknown = [n for n in names if n not in FUZZERS]
    if unknown:
        sys.exit(f"unknown fuzzer(s): {', '.join(unknown)}")
    if tiers:
        names = [n for n in names if FUZZERS[n]["tier"] in tiers]
    return names


def check():
    """Every fuzzer here is a fuzzer_test() in its dir's BUILD.gn, with its
    source and a seed corpus; every *_fuzzer.cc in those dirs is here."""
    ok = True
    gn_blocks = {}
    for d in sorted({info["dir"] for info in FUZZERS.values()}):
        gn = os.path.join(ROOT, d, "BUILD.gn")
        if os.path.exists(gn):
            with open(gn, encoding="utf-8") as f:
                text = f.read()
            # Simple string variables (_json_dict = "//...").
            variables = dict(re.findall(r'^(_\w+)\s*=\s*"([^"]+)"', text,
                                        re.M))
            for name, body in re.findall(
                    r'^fuzzer_test\("([a-z0-9_]+)"\)\s*\{(.*?)^\}', text,
                    re.M | re.S):
                for var, value in variables.items():
                    body = re.sub(rf"\b{var}\b", f'"{value}"', body)
                gn_blocks[name] = body
    gn_names = set(gn_blocks)
    for d in sorted({info["dir"] for info in FUZZERS.values()}):
        for f in sorted(os.listdir(os.path.join(ROOT, d))):
            if f.endswith("_fuzzer.cc") and f[:-3] not in FUZZERS:
                print(f"{d}/{f}: not listed in molt_fuzz.py")
                ok = False
    for name, info in sorted(FUZZERS.items()):
        if name not in gn_names:
            print(f"{name}: no fuzzer_test() in {info['dir']}/BUILD.gn")
            ok = False
        else:
            # The GN target runs with the same options and inputs.
            body = gn_blocks[name]
            gn_dict = re.search(r'\bdict\s*=\s*"([^"]+)"', body)
            if (gn_dict.group(1) if gn_dict else None) != info.get("dict"):
                print(f"{name}: GN dict differs from molt_fuzz.py's")
                ok = False
            if f'"max_len={info.get("max_len", 4096)}"' not in body:
                print(f"{name}: GN max_len differs from molt_fuzz.py's")
                ok = False
            for corpus in [f"corpus/{name}"] + info.get("more_seeds", []):
                if f'"{corpus}"' not in body:
                    print(f"{name}: GN seed corpus lacks {corpus}")
                    ok = False
        if not os.path.exists(fuzzer_source(name)):
            print(f"{name}: missing {rel(fuzzer_source(name))}")
            ok = False
        if not os.path.isdir(seed_dir(name)) or not os.listdir(seed_dir(name)):
            print(f"{name}: no seed corpus in {rel(seed_dir(name))}")
            ok = False
        if dict_path(name) and not os.path.exists(dict_path(name)):
            print(f"{name}: missing dictionary {rel(dict_path(name))}")
            ok = False
        if info["tier"] not in TIER_LDFLAGS:
            print(f"{name}: unknown tier {info['tier']}")
            ok = False
    print("check: ok" if ok else "check: FAILED")
    return ok


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    sub.add_parser("check")
    for cmd in ("build", "run", "smoke"):
        p = sub.add_parser(cmd)
        p.add_argument("names", nargs="*")
        p.add_argument("--tier", default="",
                       help="comma-separated tiers to include")
        p.add_argument("--jobs", type=int, default=os.cpu_count() or 2)
        p.add_argument("--clean", action="store_true",
                       help="rebuild every object")
        if cmd != "build":
            p.add_argument("--seconds", type=int, default=0,
                           help="-max_total_time per fuzzer")
            p.add_argument("--runs", type=int, default=0,
                           help="-runs per fuzzer")
            p.add_argument("--seed", type=int, default=1)
            p.add_argument("--no-build", action="store_true")
    args, extra = parser.parse_known_args()
    extra = [e for e in extra if e != "--"]
    if extra and args.cmd not in ("run", "smoke"):
        parser.error(f"unrecognized arguments: {' '.join(extra)}")

    if args.cmd == "list":
        for name, info in sorted(FUZZERS.items()):
            print(f"{name:48} {info['tier']:10} {info['dir']}")
        return 0
    if args.cmd == "check":
        return 0 if check() else 1

    tiers = [t for t in args.tier.split(",") if t]
    if args.cmd == "smoke" and not tiers:
        tiers = list(SMOKE_TIERS)
    names = select(args.names, tiers)
    if args.clean and os.path.isdir(os.path.join(OUT, "obj")):
        shutil.rmtree(os.path.join(OUT, "obj"))
    if args.cmd == "build" or not args.no_build:
        if build(names, args.jobs):
            return 1
    if args.cmd == "build":
        return 0

    failed = []
    for name in names:
        seconds, runs = args.seconds, args.runs
        if args.cmd == "smoke":
            # A fixed seed and a fixed number of runs: the same inputs on
            # every machine. The time cap only guards a very slow runner.
            runs = runs or FUZZERS[name].get("smoke_runs", DEFAULT_SMOKE_RUNS)
            seconds = seconds or SMOKE_GUARD_SECONDS
        if not run(name, seconds, runs, args.seed, extra):
            failed.append(name)
    if failed:
        print(f"FAILED: {', '.join(failed)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
