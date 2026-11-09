# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""How the kit picks the confirmation word out of what its recognizer made
of the browser's speech (the transcripts are ones molt-speech gave here)."""

import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))

from e2ekit import ears  # noqa: E402


class FindWordTest(unittest.TestCase):
    def setUp(self):
        self.e = ears.Ears("molt-speech", "models")

    def test_the_word_itself(self):
        self.assertEqual(self.e.find_word(
            "prompts.test wants to know your location. To allow it, say harbor. "
            "To block it, say no."), "harbor")
        self.assertEqual(self.e.find_word("To allow it, say tigers."), "tiger")

    def test_a_mangled_word_after_say(self):
        self.assertEqual(self.e.find_word("To allow it, say Harbot."), "harbor")
        self.assertEqual(self.e.find_word("To allow it, say tarbon."), "harbor")
        self.assertEqual(self.e.find_word("To allow it, say Rick it."), "rocket")
        self.assertEqual(self.e.find_word("to allow it say tamboo"), "bamboo")
        # Not only after the last "say": the whole prompt heard (the
        # spoken suite's twenty-fourth build).
        self.assertEqual(self.e.find_word(
            "to know your location, to allow it, say Micaroni, to block it, say now"),
            "macaroni")
        self.assertEqual(self.e.find_word(
            "to show nowifications, to allow it, say Netkin, to blark it, say no."),
            "napkin")

    def test_nothing_rather_than_a_guess(self):
        # A wrong word refuses the prompt: when unsure, none.
        self.assertIsNone(self.e.find_word("To allow it, say point shoe."))
        self.assertIsNone(self.e.find_word("To allow it, say the word on your screen."))
        self.assertIsNone(self.e.find_word("example.com wants to use your camera"))
        self.assertIsNone(self.e.find_word(""))

    def test_distance(self):
        self.assertEqual(ears.letter_distance("harbor", "harbour"), 1)
        self.assertEqual(ears.letter_distance("", "abc"), 3)
        self.assertEqual(ears.letter_distance("kitten", "sitting"), 3)


if __name__ == "__main__":
    unittest.main()
