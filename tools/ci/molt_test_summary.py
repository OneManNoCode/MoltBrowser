#!/usr/bin/env python3
# MoltBrowser — a summary of the test launcher's results
# Copyright 2026 GenEye AI Labs Inc.
#
# Reads the --test-launcher-summary-output JSON files that
# tools/ci/molt_test_build.sh's "test" command writes (one per test binary)
# and prints a Markdown summary: per binary, how many tests passed, failed,
# crashed, timed out or didn't run, and each failure with the start of its
# output. It also writes <dir>/failures.md with each failure's whole
# snippet.
#
# Usage: molt_test_summary.py <report dir>

import json
import os
import sys

BINARIES = ('molt_ai_unittests', 'browser_tests')
PASS = {'SUCCESS'}
SKIP = {'SKIPPED', 'NOTRUN', 'EXCESSIVE_OUTPUT_SKIPPED'}


def last_runs(data):
    """Maps each test to its last run, across the launcher's iterations."""
    last = {}
    for iteration in data.get('per_iteration_data') or []:
        for name, runs in (iteration or {}).items():
            if runs:
                last[name] = runs[-1]
    return last


def summarize(dir_path, binary, lines, details):
    path = os.path.join(dir_path, binary + '.json')
    if not os.path.exists(path):
        lines.append('- **%s:** no results (not built, or it did not start; '
                     'see %s.log)' % (binary, binary))
        return 1
    try:
        with open(path, encoding='utf-8') as f:
            data = json.load(f)
    except (OSError, ValueError) as e:
        lines.append('- **%s:** unreadable results (%s)' % (binary, e))
        return 1
    last = last_runs(data)
    counts = {}
    failed = []
    for name, run in sorted(last.items()):
        status = run.get('status', 'UNKNOWN')
        counts[status] = counts.get(status, 0) + 1
        if status not in PASS and status not in SKIP:
            failed.append((name, status, run.get('output_snippet') or ''))
    passed = sum(counts.get(s, 0) for s in PASS)
    parts = ', '.join('%d %s' % (n, s.lower())
                      for s, n in sorted(counts.items()) if s not in PASS)
    lines.append('- **%s:** %d tests, %d passed%s' %
                 (binary, len(last), passed, ', ' + parts if parts else ''))
    for name, status, snippet in failed[:60]:
        first = next((l for l in snippet.splitlines()
                      if 'Failure' in l or 'error' in l.lower()
                      or 'Check failed' in l), '')
        lines.append('  - `%s`: %s%s' % (name, status.lower(),
                                         (' — ' + first.strip()[:200])
                                         if first else ''))
        details.append('## %s (%s, %s)\n\n```\n%s\n```\n' %
                       (name, binary, status.lower(), snippet[-6000:]))
    if len(failed) > 60:
        lines.append('  - ... and %d more' % (len(failed) - 60))
    return 1 if failed else 0


def main(argv):
    if len(argv) != 1:
        print('usage: molt_test_summary.py <report dir>')
        return 2
    dir_path = argv[0]
    lines = ['### Tests', '']
    details = ['# Test failures', '']
    rc = 0
    for binary in BINARIES:
        rc |= summarize(dir_path, binary, lines, details)
    print('\n'.join(lines))
    with open(os.path.join(dir_path, 'failures.md'), 'w',
              encoding='utf-8') as f:
        f.write('\n'.join(details))
    return 0 if rc == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
