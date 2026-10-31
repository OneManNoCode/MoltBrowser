#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Checks that what the voice loop says has a Spanish sentence.

The voice loop's English sentences are the keys of each language's
catalog (src/chrome/browser/molt_ai/voice/spoken_answers.h). This reads the
string literals of the files the catalog covers (adjacent literals joined,
as the compiler joins them), keeps those that are said or shown (they have
a letter and aren't in IGNORED, below), and fails when one has no entry in
spoken_answers_es.cc, or when an entry's placeholders differ from its
English sentence's.

  tools/voice/check_spoken_answers.py          # check (exit 1 if missing)
  tools/voice/check_spoken_answers.py --list   # every sentence, and its
                                               # Spanish
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
VOICE = os.path.join(ROOT, "src", "chrome", "browser", "molt_ai", "voice")
CATALOG = os.path.join(VOICE, "spoken_answers_es.cc")

# The files whose sentences the catalog covers, and in each the functions
# left out for now (their files' owners move them: TOUCHLESS_BROWSING.md
# §4.1 lists them).
COVERED = {
    "voice_controller_logic.cc": (
        # Learning (§6.8): the routines' and learned words' sentences.
        "void RememberRoutine(",
        "void RunRoutine(",
        "void ListRoutines(",
        "void ForgetRoutine(",
        "void ForgetWord(",
        "void ForgetLearning(",
        # The browser's prompts (voice_prompts.h says them).
        "void ReadPrompt(",
        "void PromptGone(",
        "bool AnswerBrowserPrompt(",
    ),
    "voice_speech_text.cc": (),
}

# Literals that are never said or shown as they are.
IGNORED = {
    "\\\"", "\"", ", ", ": ", ". ", "; ", "; or ", "?", ",", ".", " ", "the ",
}

LITERAL = re.compile(r'"((?:[^"\\\n]|\\.)*)"')
PLACEHOLDER = re.compile(r"\{\d\}")


def strip_comments(text):
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text.startswith("R\"", i):
            j = text.find(")\"", i)
            i = n if j < 0 else j + 2
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def without_functions(text, names):
    """|text| without the bodies of the functions whose heads are |names|."""
    for name in names:
        start = text.find(name)
        while start >= 0:
            brace = text.find("{", start)
            depth = 0
            end = brace
            while end < len(text):
                if text[end] == '"':
                    end += 1
                    while end < len(text) and text[end] != '"':
                        end += 2 if text[end] == "\\" else 1
                elif text[end] == "{":
                    depth += 1
                elif text[end] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                end += 1
            text = text[:start] + text[end + 1:]
            start = text.find(name)
    return text


# Calls whose literal arguments are said or shown (the voice loop's
# Planner and the list readers), and assignments to what is said.
SPOKEN_CALLS = {"Say", "Status", "Tr", "AskToConfirm", "Scroll",
                "Localized", "ChunkSentences", "WaitStep"}
SPOKEN_ASSIGNMENTS = re.compile(r"\b(said|said\.text|preface)\s*\+?=")
# Functions whose returned literals are said.
SPOKEN_RETURNS = ("std::string_view FailureText(StepFailure failure) {",)


def enclosing_calls(text, at):
    """The names of the calls whose parentheses enclose |at|, and the
    statement's start."""
    names = []
    depth = 0
    i = at - 1
    while i >= 0:
        c = text[i]
        if c == '"':
            # Over a literal, back to its opening quote.
            i -= 1
            while i >= 0 and not (text[i] == '"' and text[i - 1] != "\\"):
                i -= 1
        elif c == ")":
            depth += 1
        elif c == "(":
            if depth == 0:
                j = i
                while j > 0 and text[j - 1] == " ":
                    j -= 1
                k = j
                while k > 0 and (text[k - 1].isalnum() or text[k - 1] == "_"):
                    k -= 1
                names.append(text[k:j])
            else:
                depth -= 1
        elif c in ";{}" and depth == 0:
            break
        i -= 1
    return names, i + 1


def returned_ranges(text):
    ranges = []
    for head in SPOKEN_RETURNS:
        start = text.find(head)
        if start >= 0:
            end = text.find("\n}\n", start)
            ranges.append((start, len(text) if end < 0 else end))
    return ranges


def sentences(path, skipped):
    with open(path, encoding="utf-8") as f:
        text = strip_comments(f.read())
    text = without_functions(text, skipped)
    returns = returned_ranges(text)
    found = []
    # Adjacent literals ("a" "b") are one.
    for match in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+', text):
        parts = LITERAL.findall(match.group(0))
        literal = "".join(parts).replace('\\"', '"')
        if literal in IGNORED or not re.search(r"[A-Za-z]{2}", literal):
            continue
        names, statement = enclosing_calls(text, match.start())
        head = text[statement:match.start()]
        spoken = (any(name in SPOKEN_CALLS for name in names) or
                  SPOKEN_ASSIGNMENTS.search(head) is not None or
                  any(a <= match.start() < b for a, b in returns))
        if spoken:
            found.append(literal)
    return found


def catalog():
    with open(CATALOG, encoding="utf-8") as f:
        text = f.read()
    entries = {}
    pattern = re.compile(
        r'\{\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+),\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\}')
    for match in pattern.finditer(text):
        english = "".join(LITERAL.findall(match.group(1)))
        spanish = "".join(LITERAL.findall(match.group(2)))
        entries[english.replace('\\"', '"')] = spanish.replace('\\"', '"')
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--list", action="store_true")
    parser.add_argument("--ignore-missing", action="store_true",
                        help="report, but exit 0")
    args = parser.parse_args()
    entries = catalog()
    missing = []
    bad = []
    for name, skipped in COVERED.items():
        for sentence in sentences(os.path.join(VOICE, name), skipped):
            spanish = entries.get(sentence)
            if args.list:
                print(f"{name}: {sentence!r} -> {spanish!r}")
            if spanish is None:
                missing.append((name, sentence))
    for english, spanish in entries.items():
        if sorted(PLACEHOLDER.findall(english)) != sorted(
                PLACEHOLDER.findall(spanish)):
            bad.append(english)
    for english in bad:
        print(f"check_spoken_answers: placeholders differ: {english!r}",
              file=sys.stderr)
    for name, sentence in sorted(set(missing)):
        print(f"check_spoken_answers: no Spanish for {name}: {sentence!r}",
              file=sys.stderr)
    if bad or (missing and not args.ignore_missing):
        return 1
    print(f"check_spoken_answers: {len(entries)} Spanish sentences; "
          f"{len(set(missing))} missing")
    return 0


if __name__ == "__main__":
    sys.exit(main())
