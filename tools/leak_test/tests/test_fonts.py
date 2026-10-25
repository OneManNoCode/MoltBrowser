#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The kit's copies of MoltNet's bundled font set (expectations.py and
pages/static/fp.js) match the set itself (tools/fonts/moltnet_fonts.json,
through the generator that writes Blink's table).

  python3 -I -m unittest discover -s tools/leak_test/tests -t tools/leak_test
"""

import json
import os
import re
import sys
import unittest

from leakkit import expectations as ex
from leakkit import uniformity

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
FONTS = os.path.join(REPO, 'tools', 'fonts')
FP_JS = os.path.join(HERE, '..', 'pages', 'static', 'fp.js')


def _fonts_model():
    sys.path.insert(0, FONTS)
    try:
        import generate_tables
        import manifest
        data = manifest.load(manifest.MANIFEST)
        return data, generate_tables.model(data)
    finally:
        sys.path.remove(FONTS)


def _js_array(text, name):
    body = re.search(r'const %s = \[(.*?)\];' % name, text, re.S).group(1)
    return re.findall(r"'([^']*)'", body)


def _js_object(text, name):
    body = re.search(r'const %s = \{(.*?)\};' % name, text, re.S).group(1)
    return dict(re.findall(r"'([^']*)': '([^']*)'", body))


class BundledFontSetTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.data, cls.model = _fonts_model()
        with open(FP_JS, encoding='utf-8') as f:
            cls.fp_js = f.read()

    def test_names_match_the_set(self):
        names = tuple(name for name, _ in self.model['visibility'])
        self.assertEqual(ex.MOLTNET_FONTS, names)
        self.assertEqual(tuple(_js_array(self.fp_js, 'MOLTNET_FONTS')), names)

    def test_generic_map_matches_the_set(self):
        generics = _js_object(self.fp_js, 'MOLTNET_GENERICS')
        self.assertEqual(set(generics), set(ex.MOLTNET_GENERIC_MAP))
        for generic, family in generics.items():
            self.assertEqual(self.data['generic'][generic], family, generic)

    def test_local_names_are_faces_of_the_set(self):
        unique = set()
        for entry in self.data['files']:
            for face in entry['faces']:
                unique.add(face['postscript_name'])
                unique.add(face['full_name'])
        for name in _js_array(self.fp_js, 'LOCAL_BUNDLED'):
            self.assertIn(name, unique)

    def test_rules(self):
        rules = {(r[0], r[1]): r for r in ex.FULL_RULES}
        visible = rules[('fonts.visible', 'subset')]
        ok = uniformity.check_rule(
            {'fonts.visible': ['Arial', 'noto sans', 'Noto Sans CJK JP']},
            visible, 'Windows')
        self.assertEqual(ok['status'], 'PASS')
        for os_name in ('Linux', 'macOS', 'Windows'):
            leak = uniformity.check_rule(
                {'fonts.visible': ['Arial', 'DejaVu Sans']}, visible, os_name)
            self.assertEqual(leak['status'], 'FAIL', os_name)
        whole = rules[('fonts.moltnet_set', 'eq')]
        self.assertEqual(uniformity.check_rule(
            {'fonts.moltnet_set': list(ex.MOLTNET_FONTS)}, whole)['status'],
            'PASS')
        self.assertEqual(uniformity.check_rule(
            {'fonts.moltnet_set': list(ex.MOLTNET_FONTS[1:])}, whole)[
                'status'], 'FAIL')
        generic = rules[('fonts.generic_map', 'eq')]
        bad = dict(ex.MOLTNET_GENERIC_MAP, **{'system-ui': False})
        self.assertEqual(uniformity.check_rule(
            {'fonts.generic_map': bad}, generic)['status'], 'FAIL')
        for key in ('fonts.fallback_widths', 'fonts.local_bundled'):
            self.assertIn(key, ex.DIFF_KEYS)

    def test_manifest_is_json(self):
        with open(os.path.join(FONTS, 'moltnet_fonts.json'),
                  encoding='utf-8') as f:
            self.assertEqual(json.load(f)['format'], 1)


if __name__ == '__main__':
    unittest.main()
