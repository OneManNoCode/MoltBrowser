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

    def test_a_say_run_into_the_word(self):
        # The spoken suite's forty-first build (speakers): "say mango"
        # heard as "same ingo", then as "same mingo", on each "repeat".
        self.assertEqual(self.e.find_word(
            "Press share my location. Prompts Dark test once to know your "
            "location to allow it same ingo, to bark it, say no"), "mango")
        self.assertEqual(self.e.find_word(
            "Prompts Dark test once to know your location, to allow it same "
            "mingo to blark it, say no"), "mango")
        # Run into the whole word (the speakers runs of the thirteenth and
        # thirty-fifth builds).
        self.assertEqual(self.e.find_word(
            "know your location to allow it, sameingo to bark it, say no"), "mango")
        self.assertEqual(self.e.find_word(
            "now your location to allow it to Samingo to block it, say no"), "mango")
        self.assertEqual(self.e.find_word("To allow it, say sandwich."), "sandwich")
        self.assertIsNone(self.e.find_word(
            "To allow it, same the word on your screen. To block it, say no."))

    def test_nothing_rather_than_a_guess(self):
        # A wrong word refuses the prompt: when unsure, none.
        self.assertIsNone(self.e.find_word("To allow it, say point shoe."))
        # Two letters off, but not the one word it sounds like: the spoken
        # suite's forty-first build (headphones) said "tiger" for "binger",
        # which is as near "banjo" by sound, and the browser turned it
        # down; "tarbon" is nearer "dragon" by sound than "harbor".
        self.assertIsNone(self.e.find_word(
            "Press share my location. Prompts dark test warns to know your "
            "location, to allow it, say binger, to block it, say no"))
        self.assertIsNone(self.e.find_word("To allow it, say tarbon."))
        self.assertIsNone(self.e.find_word("To allow it, say the word on your screen."))
        self.assertIsNone(self.e.find_word("example.com wants to use your camera"))
        self.assertIsNone(self.e.find_word(""))

    def test_a_word_nearest_by_sound(self):
        # The spoken suite's twenty-seventh build: each heard the same way
        # on every "repeat", and three letters from the list's nearest word.
        self.assertIsNone(self.e.find_word(
            "to know your location. To allow it, say keptus, to block it, say no"))
        self.assertEqual(self.e.nearest_by_sound(
            "to know your location. To allow it, say keptus, to block it, say no"),
            "cactus")
        self.assertEqual(self.e.nearest_by_sound(
            "to know your location to allow it, say Bingja, to blark it, say now"),
            "banjo")
        # "Limit": "helmet" with its "h" lost after "say", nearer than
        # "lemon" (whose "n" is one sound off).
        self.assertEqual(self.e.nearest_by_sound(
            "to show notifications, to allow it, saying limit, to blark it, say no"),
            "helmet")
        self.assertEqual(self.e.nearest_by_sound("to allow it, said Benjo"), "banjo")
        # The thirty-second build (headphones): "fossil" heard as "farsal",
        # then as "pharsal" on the "repeat": the same sounds both times.
        for heard in ("to allow it, say farsal, to block it, say no.",
                      "to allow it, say pharsal, to blark it, say no"):
            self.assertEqual(self.e.nearest_by_sound(heard), "fossil")
        # Nothing near, or more than one as near: none.
        self.assertIsNone(self.e.nearest_by_sound("To allow it, say the word on your screen."))
        self.assertIsNone(self.e.nearest_by_sound("example.com wants to use your camera"))
        self.assertIsNone(self.e.nearest_by_sound("To allow it, say no."))
        self.assertIsNone(self.e.nearest_by_sound(""))

    def test_the_words_two_hearings_leave(self):
        # The spoken suite on build 37 (headphones): "cactus" heard as
        # "kept this", then as "active", which is as near "guitar".
        first = self.e.sounds_near(
            "Press share of my location. Prompt start test once to know your "
            "location, to allow it. Say kept this, to block it, say n")
        second = self.e.sounds_near(
            "Prompt Dark test ones to know your location. To allow it, say "
            "active, to blark it, say no")
        self.assertIn("cactus", first)
        self.assertIn("guitar", second)
        self.assertEqual(first & second, {"cactus"})
        # The same mangling twice: that word.
        self.assertEqual(self.e.sounds_near("To allow it, say farsal.") &
                         self.e.sounds_near("To allow it, say pharsal."), {"fossil"})
        # The prompt without the word, which build 38's speakers run read
        # as "tornado", "turtle" or "yogurt", and said "yogurt": nothing.
        self.assertEqual(self.e.sounds_near(
            "prompts.test wants to know your location. To allow it, say the "
            "word on your screen. To block it, say no."), set())
        # Nothing after a "say", or too short to tell: nothing.
        self.assertEqual(self.e.sounds_near("example.com wants to use your camera"), set())
        self.assertEqual(self.e.sounds_near("To allow it, say no."), set())
        self.assertEqual(self.e.sounds_near(""), set())

    def test_sound_key(self):
        self.assertEqual(ears.sound_key("cactus"), "kts")
        self.assertEqual(ears.sound_key("keptus"), "kpts")
        self.assertEqual(ears.sound_key("Lemon"), "lmn")
        self.assertEqual(ears.sound_key("harbor"), "rpr")
        self.assertEqual(ears.sound_key("pharsal"), "frsl")
        self.assertEqual(ears.sound_key("Philip"), "flp")

    def test_distance(self):
        self.assertEqual(ears.letter_distance("harbor", "harbour"), 1)
        self.assertEqual(ears.letter_distance("", "abc"), 3)
        self.assertEqual(ears.letter_distance("kitten", "sitting"), 3)


if __name__ == "__main__":
    unittest.main()
