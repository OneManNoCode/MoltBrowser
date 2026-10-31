#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Keeps docs/VOICE_COMMANDS.md in step with the voice grammar's tables.

The reference part of the pages (between the GENERATED markers) is printed
by tools/voice/print_voice_commands.cc from the phrase tables in
src/chrome/browser/molt_ai/voice/, built here with the system C++ compiler
(the code is std-only): docs/VOICE_COMMANDS.md from English's tables and
docs/VOICE_COMMANDS.es.md from Spanish's. The printer also refuses an
example that doesn't parse to its command.

  tools/voice/voice_commands_doc.py          # check both (exit 1 if stale)
  tools/voice/voice_commands_doc.py --write  # update the pages
  tools/voice/voice_commands_doc.py --language es  # one page only
"""

import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
VOICE = os.path.join(ROOT, "src", "chrome", "browser", "molt_ai", "voice")
DOCS = {
    "en": os.path.join(ROOT, "docs", "VOICE_COMMANDS.md"),
    "es": os.path.join(ROOT, "docs", "VOICE_COMMANDS.es.md"),
}
BEGIN = "<!-- BEGIN GENERATED: tools/voice/voice_commands_doc.py --write -->"
END = "<!-- END GENERATED -->"
SOURCES = [
    os.path.join(ROOT, "tools", "voice", "print_voice_commands.cc"),
] + [
    os.path.join(VOICE, name + ".cc") for name in (
        "phonetic_key", "phonetic_key_es", "spoken_choice", "spoken_numbers",
        "spoken_phrases", "spoken_phrases_en", "spoken_phrases_es",
        "spoken_text", "voice_command", "voice_command_phrases_en",
        "voice_command_phrases_es")
]


def compiler():
    for candidate in (os.environ.get("CXX"), "c++", "clang++", "g++"):
        if candidate and shutil.which(candidate):
            return candidate
    sys.exit("voice_commands_doc: no C++ compiler (set CXX)")


def build(tmp):
    binary = os.path.join(tmp, "print_voice_commands")
    cmd = [compiler(), "-std=c++20", "-fno-exceptions", "-O1",
           "-I", os.path.join(ROOT, "src")] + SOURCES + ["-o", binary]
    built = subprocess.run(cmd, capture_output=True, text=True)
    if built.returncode != 0:
        sys.exit(f"voice_commands_doc: build failed\n$ {' '.join(cmd)}\n"
                 f"{built.stdout}{built.stderr}")
    return binary


def generated(binary, language):
    run = subprocess.run([binary, language], capture_output=True, text=True,
                         encoding="utf-8")
    if run.returncode != 0:
        sys.exit(f"voice_commands_doc: {run.stderr.strip()}")
    return run.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--write", action="store_true",
                        help="update the pages")
    parser.add_argument("--language", choices=["all"] + sorted(DOCS),
                        default="all", help="which page (default: both)")
    args = parser.parse_args()
    languages = sorted(DOCS) if args.language == "all" else [args.language]
    stale = []
    with tempfile.TemporaryDirectory() as tmp:
        binary = build(tmp)
        for language in languages:
            doc = DOCS[language]
            name = os.path.relpath(doc, ROOT)
            with open(doc, encoding="utf-8") as f:
                page = f.read()
            if BEGIN not in page or END not in page:
                sys.exit(f"voice_commands_doc: {name} lacks the GENERATED "
                         "markers")
            head, rest = page.split(BEGIN, 1)
            _, tail = rest.split(END, 1)
            fresh = (head + BEGIN + "\n" + generated(binary, language) +
                     "\n" + END + tail)
            if fresh == page:
                print(f"voice_commands_doc: {name} is up to date")
            elif args.write:
                with open(doc, "w", encoding="utf-8") as f:
                    f.write(fresh)
                print(f"voice_commands_doc: wrote {name}")
            else:
                stale.append(name)
    for name in stale:
        print(f"voice_commands_doc: {name} is out of date with the phrase "
              "tables; run tools/voice/voice_commands_doc.py --write",
              file=sys.stderr)
    return 1 if stale else 0


if __name__ == "__main__":
    sys.exit(main())
