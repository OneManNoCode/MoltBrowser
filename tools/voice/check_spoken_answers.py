#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Checks that what voice mode says has a Spanish sentence.

Voice mode's English sentences are the keys of each language's catalog
(src/chrome/browser/molt_ai/voice/spoken_answers.h). This reads the string
literals of every file whose sentences voice mode says (adjacent literals
joined, as the compiler joins them), keeps those that are said or shown,
and fails when:

  - one has no entry in spoken_answers_es.cc;
  - an entry's placeholders ({0} ... {9}) differ from its English key's;
  - an English key is in the catalog twice;
  - a key no code says any more: it isn't a string literal of any of
    voice mode's sources (tests and the catalog itself don't count).

A literal is said or shown when it is an argument of a call that says it
(Say(), Status(), Tr(), Said(), SaidLater(), Localized(), ...), is
assigned to what is said, is returned by a function whose return is said
(FailureText(), PermissionPhrase()), or is a constant (constexpr char
kName[] = "..." or constexpr std::string_view kName = "...") whose name is
an argument of such a call. There are no exclusions: every file below is
checked whole.

  tools/voice/check_spoken_answers.py          # check (exit 1 on a fault)
  tools/voice/check_spoken_answers.py --list   # every sentence, and its
                                               # Spanish
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
MOLT_AI = os.path.join(ROOT, "src", "chrome", "browser", "molt_ai")
VOICE = os.path.join(MOLT_AI, "voice")
CATALOG = os.path.join(VOICE, "spoken_answers_es.cc")

# The files whose sentences voice mode says (relative to molt_ai/).
COVERED = (
    "voice/voice_controller_logic.cc",
    "voice/voice_controller.cc",
    "voice/voice_speech_text.cc",
    "voice/voice_prompts.cc",
    "voice/voice_browser_prompts.cc",
    "voice/voice_session.cc",
    "voice/voice_routines_logic.cc",
    "voice/voice_routines.cc",
    "voice/voice_routine_store.cc",
    "voice/voice_routine_sites.cc",
    "voice/voice_learning.cc",
    "voice/voice_learning_loop.cc",
    "voice/voice_reader.cc",
    "voice/page_reading.cc",
    "voice/page_questions.cc",
    "voice/voice_page_questions.cc",
    "voice/voice_browser_logic.cc",
    "voice/voice_media_controls.cc",
    "agents/voice_agent_speech.cc",
    "security/activation_policy.cc",
)

# Where a key may be written besides the covered files: a constant the
# covered files say (kWordOnScreen), a reason a covered file says
# (voice_listener.cc's), a field's or an item's kind the page map names
# ("payment card"; "button", said "the button": ROLE_KEY).
KEY_SOURCES = (
    "voice/voice_gesture_channel.h",
    "voice/voice_listener.cc",
    "voice/page_map.cc",
    "security/agent_safety.cc",
)
# "the button": "the " and an item's kind (ItemKindName()), said whole.
ROLE_KEY = re.compile(r"^the ([a-z][a-z ]*)$")

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
        elif text.startswith("R\"", i):
            # A raw string (a JSON schema): never said.
            close = text.find("(", i)
            delimiter = text[i + 2:close]
            j = text.find(")" + delimiter + "\"", close)
            i = n if j < 0 else j + len(delimiter) + 2
        elif text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
        elif text[i] == "'":
            # A character literal ('"' must not open a string).
            j = i + 1
            while j < n and text[j] != "'":
                j += 2 if text[j] == "\\" else 1
            out.append(" ")
            i = j + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def unescape(literal):
    return literal.replace('\\"', '"')


# Calls whose literal arguments are said or shown (the voice loop's
# Planner and the list readers, the catalog's functions), and assignments
# to what is said.
SPOKEN_CALLS = {"Say", "Status", "Tr", "AskToConfirm", "Scroll",
                "Localized", "ChunkSentences", "WaitStep", "Said",
                "SaidLater", "Translated"}
SPOKEN_ASSIGNMENTS = re.compile(r"\b(said|said\.text|preface)\s*\+?=")
# Functions whose returned literals are said.
SPOKEN_RETURNS = (
    "std::string_view FailureText(StepFailure failure) {",
    "std::string PermissionPhrase(",
)
# A constant's declaration up to its literal.
CONSTANT = re.compile(
    r"constexpr\s+(?:char\s+(k\w+)\[\]|std::string_view\s+(k\w+))\s*=\s*$")


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


def literals(text):
    """Each (joined literal, start offset) of |text| (comments stripped)."""
    for match in re.finditer(r'(?:"(?:[^"\\\n]|\\.)*"\s*)+', text):
        yield unescape("".join(LITERAL.findall(match.group(0)))), match.start()


def spoken_constants(text):
    """The names of |text|'s constants that are arguments of a spoken
    call."""
    names = set()
    for match in re.finditer(r"\bk[A-Z]\w*\b", text):
        calls, _ = enclosing_calls(text, match.start())
        if any(name in SPOKEN_CALLS for name in calls):
            names.add(match.group(0))
    return names


def sentences(path):
    with open(path, encoding="utf-8") as f:
        text = strip_comments(f.read())
    returns = returned_ranges(text)
    constants = spoken_constants(text)
    found = []
    for literal, start in literals(text):
        if literal in IGNORED or not re.search(r"[A-Za-z]{2}", literal):
            continue
        names, statement = enclosing_calls(text, start)
        head = text[statement:start]
        constant = CONSTANT.search(head)
        constant_name = (constant.group(1) or constant.group(2)
                         if constant else None)
        spoken = (any(name in SPOKEN_CALLS for name in names) or
                  SPOKEN_ASSIGNMENTS.search(head) is not None or
                  any(a <= start < b for a, b in returns) or
                  (constant_name is not None and constant_name in constants))
        if spoken:
            found.append(literal)
    return found


def all_literals(path):
    with open(path, encoding="utf-8") as f:
        text = strip_comments(f.read())
    return {literal for literal, _ in literals(text)}


def catalog():
    with open(CATALOG, encoding="utf-8") as f:
        text = f.read()
    entries = {}
    duplicates = []
    pattern = re.compile(
        r'\{\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+),\s*((?:"(?:[^"\\\n]|\\.)*"\s*)+)\}')
    for match in pattern.finditer(strip_comments(text)):
        english = unescape("".join(LITERAL.findall(match.group(1))))
        spanish = unescape("".join(LITERAL.findall(match.group(2))))
        if english in entries:
            duplicates.append(english)
        entries[english] = spanish
    return entries, duplicates


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args()
    entries, duplicates = catalog()
    missing = []
    said = set()
    for name in COVERED:
        for sentence in sentences(os.path.join(MOLT_AI, name)):
            said.add(sentence)
            spanish = entries.get(sentence)
            if args.list:
                print(f"{name}: {sentence!r} -> {spanish!r}")
            if spanish is None:
                missing.append((name, sentence))
    # Every literal of voice mode's sources, for keys no code says.
    written = set()
    for name in COVERED + KEY_SOURCES:
        written |= all_literals(os.path.join(MOLT_AI, name))
    bad = []
    for english, spanish in entries.items():
        if sorted(PLACEHOLDER.findall(english)) != sorted(
                PLACEHOLDER.findall(spanish)):
            bad.append(english)
    def is_written(english):
        role = ROLE_KEY.match(english)
        return english in written or (role is not None and
                                      role.group(1) in written)

    stale = sorted(english for english in entries if not is_written(english))
    for english in bad:
        print(f"check_spoken_answers: placeholders differ: {english!r}",
              file=sys.stderr)
    for english in duplicates:
        print(f"check_spoken_answers: in the catalog twice: {english!r}",
              file=sys.stderr)
    for english in stale:
        print(f"check_spoken_answers: no code says {english!r}",
              file=sys.stderr)
    for name, sentence in sorted(set(missing)):
        print(f"check_spoken_answers: no Spanish for {name}: {sentence!r}",
              file=sys.stderr)
    if bad or duplicates or stale or missing:
        return 1
    print(f"check_spoken_answers: {len(entries)} Spanish sentences, "
          f"{len(said)} said in {len(COVERED)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
