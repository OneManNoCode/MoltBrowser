#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Tests for check_spoken_answers.py: it passes on the tree as it is, and
fails on a copy of the Spanish catalog changed each way it must catch.

  python3 -I tools/voice/check_spoken_answers_test.py -v
"""

import contextlib
import importlib.util
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))


def load_checker():
    spec = importlib.util.spec_from_file_location(
        "check_spoken_answers", os.path.join(HERE, "check_spoken_answers.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class CheckSpokenAnswersTest(unittest.TestCase):

    def setUp(self):
        self.check = load_checker()
        with open(self.check.CATALOG, encoding="utf-8") as f:
            self.catalog = f.read()

    def run_on(self, text):
        """The checker's exit code and its complaints with |text| as the
        catalog."""
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "spoken_answers_es.cc")
            with open(path, "w", encoding="utf-8") as f:
                f.write(text)
            self.check.CATALOG = path
            saved = sys.argv
            sys.argv = ["check_spoken_answers.py"]
            err = io.StringIO()
            try:
                with contextlib.redirect_stderr(err), \
                        contextlib.redirect_stdout(io.StringIO()):
                    code = self.check.main()
            finally:
                sys.argv = saved
        return code, err.getvalue()

    def mutated(self, old, new):
        self.assertIn(old, self.catalog)
        return self.catalog.replace(old, new, 1)

    def test_the_tree_passes(self):
        self.assertEqual(self.run_on(self.catalog), (0, ""))

    def test_a_sentence_with_no_spanish(self):
        code, err = self.run_on(self.mutated(
            '{"Kept the file.", "He conservado el archivo."},', ""))
        self.assertEqual(code, 1)
        self.assertIn("no Spanish for voice/voice_prompts.cc: 'Kept the "
                      "file.'", err)

    def test_placeholders_that_differ(self):
        code, err = self.run_on(self.mutated('"Reproduciendo {0}."',
                                             '"Reproduciendo."'))
        self.assertEqual(code, 1)
        self.assertIn("placeholders differ: 'Playing {0}.'", err)
        # The same placeholder twice where the key has it once differs too.
        code, err = self.run_on(self.mutated('"Reproduciendo {0}."',
                                             '"Reproduciendo {0} {0}."'))
        self.assertEqual(code, 1)

    def test_a_key_no_code_says(self):
        code, err = self.run_on(self.mutated(
            '{"Kept the file.",',
            '{"Kept the old file.", "x"},\n    {"Kept the file.",'))
        self.assertEqual(code, 1)
        self.assertIn("no code says 'Kept the old file.'", err)

    def test_a_key_twice(self):
        code, err = self.run_on(self.mutated(
            '{"Kept the file.",',
            '{"Kept the file.", "y"},\n    {"Kept the file.",'))
        self.assertEqual(code, 1)
        self.assertIn("in the catalog twice: 'Kept the file.'", err)

    def test_every_file_is_covered_whole(self):
        # No exclusions: each covered file exists and is read whole.
        for name in self.check.COVERED + self.check.KEY_SOURCES:
            self.assertTrue(
                os.path.isfile(os.path.join(self.check.MOLT_AI, name)), name)
        self.assertFalse(hasattr(self.check, "without_functions"))


if __name__ == "__main__":
    unittest.main()
