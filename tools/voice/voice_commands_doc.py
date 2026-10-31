#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Keeps docs/VOICE_COMMANDS.md in step with the voice grammar's tables.

The reference part of the page (between the GENERATED markers) is printed
by tools/voice/print_voice_commands.cc from the phrase tables in
src/chrome/browser/molt_ai/voice/, built here with the system C++ compiler
(the code is std-only). The printer also refuses an example that doesn't
parse to its command.

  tools/voice/voice_commands_doc.py          # check (exit 1 if stale)
  tools/voice/voice_commands_doc.py --write  # update the page
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
DOC = os.path.join(ROOT, "docs", "VOICE_COMMANDS.md")
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


def generated():
    with tempfile.TemporaryDirectory() as tmp:
        binary = os.path.join(tmp, "print_voice_commands")
        cmd = [compiler(), "-std=c++20", "-fno-exceptions", "-O1",
               "-I", os.path.join(ROOT, "src")] + SOURCES + ["-o", binary]
        build = subprocess.run(cmd, capture_output=True, text=True)
        if build.returncode != 0:
            sys.exit(f"voice_commands_doc: build failed\n$ {' '.join(cmd)}\n"
                     f"{build.stdout}{build.stderr}")
        run = subprocess.run([binary], capture_output=True, text=True)
        if run.returncode != 0:
            sys.exit(f"voice_commands_doc: {run.stderr.strip()}")
        return run.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--write", action="store_true",
                        help="update docs/VOICE_COMMANDS.md")
    args = parser.parse_args()
    with open(DOC, encoding="utf-8") as f:
        page = f.read()
    if BEGIN not in page or END not in page:
        sys.exit(f"voice_commands_doc: {DOC} lacks the GENERATED markers")
    head, rest = page.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    fresh = head + BEGIN + "\n" + generated() + "\n" + END + tail
    if fresh == page:
        print("voice_commands_doc: docs/VOICE_COMMANDS.md is up to date")
        return 0
    if args.write:
        with open(DOC, "w", encoding="utf-8") as f:
            f.write(fresh)
        print("voice_commands_doc: wrote docs/VOICE_COMMANDS.md")
        return 0
    print("voice_commands_doc: docs/VOICE_COMMANDS.md is out of date with "
          "the phrase tables; run tools/voice/voice_commands_doc.py --write",
          file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
