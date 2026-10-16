#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Rebuilds MoltBrowser's prepopulated_engines.json from upstream's.

third_party/search_engines_data is a DEPS subproject (a separate repo that the
GitHub mirror of chromium/src doesn't contain), so the patch series can't diff
it and src/ carries the whole file (patches/whole_files.txt). Our only change
is the "google" engine: id 1 is the default-search slot, and MoltBrowser puts
DuckDuckGo there with none of Google's extra endpoints (image search,
contextual search, side panel search, alternate URLs, new tab page).

On every rebase, on a machine with a synced checkout:

  tools/chromium/search_engines_overlay.py \\
      chromium/src/third_party/search_engines_data/resources/definitions/prepopulated_engines.json \\
      -o src/third_party/search_engines_data/resources/definitions/prepopulated_engines.json

--check verifies that a file is already the rewrite of upstream's.
"""

import argparse
import sys
from pathlib import Path

ENGINE_KEY = '    "google": {'

# The "google" element as MoltBrowser ships it. Keep "id": 1 and the
# SEARCH_ENGINE_GOOGLE type/resource: other code finds the default-search
# slot through them.
MOLT_GOOGLE_ENGINE = '''    "google": {
      "name": "DuckDuckGo",
      "keyword": "duckduckgo.com",
      "favicon_url": "https://duckduckgo.com/favicon.ico",
      "base_builtin_resource_id": "SEARCH_ENGINE_GOOGLE",
      "search_url": "https://duckduckgo.com/?q={searchTerms}",
      "suggest_url": "https://duckduckgo.com/ac/?q={searchTerms}&type=list",
      "type": "SEARCH_ENGINE_GOOGLE",
      "preconnect_to_search_url" : "ALLOWED",
      "prefetch_likely_navigations" : "ALLOWED",
      "id": 1
    },
'''


def rewrite(text):
    lines = text.splitlines(keepends=True)
    starts = [i for i, line in enumerate(lines) if line.rstrip("\n") == ENGINE_KEY]
    if len(starts) != 1:
        raise ValueError('expected exactly one line %r, found %d'
                         % (ENGINE_KEY, len(starts)))
    start = starts[0]
    # The element ends at the first line closing it at the same indentation.
    for end in range(start + 1, len(lines)):
        if lines[end].rstrip("\n") in ("    },", "    }"):
            break
    else:
        raise ValueError('unterminated "google" element')
    body = "".join(lines[start:end + 1])
    if '"id": 1\n' not in body or '"SEARCH_ENGINE_GOOGLE"' not in body:
        raise ValueError('"google" element no longer looks like id 1 / '
                         'SEARCH_ENGINE_GOOGLE; check upstream by hand')
    replacement = MOLT_GOOGLE_ENGINE
    if lines[end].rstrip("\n") == "    }":  # last element: no comma
        replacement = replacement[:-len("},\n")] + "}\n"
    return "".join(lines[:start]) + replacement + "".join(lines[end + 1:])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("upstream", help="upstream prepopulated_engines.json")
    ap.add_argument("-o", "--output", help="write here instead of stdout")
    ap.add_argument("--check", metavar="FILE",
                    help="exit 1 unless FILE equals the rewrite of upstream")
    args = ap.parse_args()

    try:
        result = rewrite(Path(args.upstream).read_text(encoding="utf-8"))
    except ValueError as e:
        print("search_engines_overlay: %s" % e, file=sys.stderr)
        return 2
    if args.check:
        if Path(args.check).read_text(encoding="utf-8") != result:
            print("search_engines_overlay: %s is not the rewrite of %s"
                  % (args.check, args.upstream), file=sys.stderr)
            return 1
        return 0
    if args.output:
        Path(args.output).write_text(result, encoding="utf-8")
    else:
        sys.stdout.write(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
