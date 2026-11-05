#!/usr/bin/env python3
# MoltBrowser — a summary of the test launcher's results
# Copyright 2026 GenEye AI Labs Inc.
#
# Reads the --test-launcher-summary-output JSON files that
# tools/ci/molt_test_build.sh's "test" command writes (one per test binary)
# and prints a Markdown summary: per binary, how many tests passed, failed,
# crashed, timed out or didn't run, and each failure with its first error.
# It also writes <dir>/failures.md, and failures-<binary>.md per binary,
# with the output around each failure's errors.
#
# Usage: molt_test_summary.py <report dir>

import json
import os
import re
import sys

BINARIES = ('molt_ai_unittests', 'browser_tests')
PASS = {'SUCCESS'}
SKIP = {'SKIPPED', 'NOTRUN', 'EXCESSIVE_OUTPUT_SKIPPED'}


def last_runs(data):
    """Maps each test to its last run, across the launcher's iterations. A
    test that failed or crashed before passing on a retry is "FLAKY", with
    the failing run's output: a flake is still a failure to fix."""
    last = {}
    for iteration in data.get('per_iteration_data') or []:
        for name, runs in (iteration or {}).items():
            if not runs:
                continue
            final = runs[-1]
            bad = [r for r in runs[:-1]
                   if r.get('status') not in PASS and r.get('status') not in SKIP]
            if final.get('status') in PASS and bad:
                final = dict(bad[-1], status='FLAKY')
            last[name] = final
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
    # Failures with the same first error (a fixture's shared setup, say)
    # are one section: its first test's output, then the others' names.
    groups = {}
    for name, status, snippet in failed:
        first = first_error(snippet)
        key = (normalized(first) or name) + ' | ' + freed_at(snippet)
        groups.setdefault(key, []).append((name, status, snippet, first))
    for name, status, snippet, first in [g[0] for g in groups.values()][:80]:
        lines.append('  - `%s`: %s%s' % (name, status.lower(),
                                         (' — ' + first.strip()[:200])
                                         if first else ''))
    if len(groups) > 80:
        lines.append('  - ... and %d more' % (len(groups) - 80))
    # The details most worth the log's room first: crashes and failures,
    # then timeouts and flakes, then failures at exit (a dangling pointer
    # found at shutdown usually ends many tests alike).
    order = {'CRASH': 0, 'FAILURE': 1, 'TIMEOUT': 2, 'FLAKY': 3,
             'FAILURE_ON_EXIT': 4}
    for group in sorted(groups.values(),
                        key=lambda g: order.get(g[0][1], 5)):
        name, status, snippet, _ = group[0]
        others = [g[0] for g in group[1:]]
        lines_of_others = ('\nThe same first error in %d more: %s\n' %
                           (len(others), ', '.join(others))) if others else ''
        details.append('## %s (%s, %s)\n%s\n```\n%s\n```\n' %
                       (name, binary, status.lower(), lines_of_others,
                        clip(snippet)))
    return 1 if failed else 0


STRONG = ('FATAL', 'Check failed', 'DCHECK failed', 'Failure',
          'Assertion', 'Sanitizer', 'Timeout', '[DanglingSignature]',
          'received signal')
RELEASED = 'Memory was released on:'
# base's dangling raw_ptr reports: at exit (RELEASED), and when the pointer
# is released (its signature line names where the memory was freed and
# where the pointer was released).
DANGLING = (RELEASED, '[DanglingSignature]', '[DanglingPtr](1/3)')


def first_error(snippet):
    """The line that names the failure: a fatal error or a test's failure,
    else any error line."""
    lines = snippet.splitlines()
    return (next((l for l in lines if any(s in l for s in STRONG)), '') or
            next((l for l in lines if 'error' in l.lower()), ''))


def freed_at(snippet):
    """For a dangling raw_ptr report (base's CheckDanglingRawPtrBufferEmpty),
    the first frame past the allocator where the memory was freed: which
    object the pointer outlived."""
    for line in snippet.splitlines():
        if '[DanglingSignature]' in line:
            return line.split('[DanglingSignature]', 1)[1].strip()
    at = snippet.find(RELEASED)
    if at < 0:
        return ''
    for line in snippet[at:].splitlines()[1:60]:
        frame = re.sub(r'^#\d+ 0x[0-9a-f]+ ', '', line.strip())
        if not line.strip().startswith('#'):
            continue
        if not re.search(r'partition_alloc|allocator|operator delete|'
                         r'CollectStackTrace|StackTrace::|'
                         r'DanglingRawPtr|std::__|base::internal', frame):
            return frame
    return ''


def normalized(line):
    """The first error without what differs from run to run: process and
    thread IDs, addresses, timings."""
    line = re.sub(r'^\[\d+:\d+:', '[', line.strip())
    line = re.sub(r'0x[0-9a-f]+', '0x', line)
    return re.sub(r'\(\d+ ms\)', '', line)


def clip(snippet, before=1500, after=4500, tail=1500, limit=14000):
    """What explains the failure: the output around the first error and
    around each dangling raw_ptr report, and the end."""
    if len(snippet) <= limit:
        return snippet
    anchors = [m.start() for anchor in DANGLING
               for m in re.finditer(re.escape(anchor), snippet)]
    first = first_error(snippet)
    if first:
        anchors.append(snippet.find(first))
    windows = sorted((max(0, a - before), a + after) for a in anchors if a >= 0)
    windows.append((max(0, len(snippet) - tail), len(snippet)))
    merged = []
    for start, end in sorted(windows):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    out, shown, last = [], 0, 0
    for start, end in merged:
        if shown >= limit:
            break
        end = min(end, start + limit - shown)
        if start > last:
            out.append('[... %d characters ...]' % (start - last))
        out.append(snippet[start:end])
        shown += end - start
        last = end
    if last < len(snippet):
        out.append('[... %d characters ...]' % (len(snippet) - last))
    return '\n'.join(out)


def main(argv):
    if len(argv) != 1:
        print('usage: molt_test_summary.py <report dir>')
        return 2
    dir_path = argv[0]
    lines = ['### Tests', '']
    details = ['# Test failures', '']
    rc = 0
    for binary in BINARIES:
        mine = ['# %s failures' % binary, '']
        rc |= summarize(dir_path, binary, lines, mine)
        # One file per binary too, so the job log can give each its share.
        with open(os.path.join(dir_path, 'failures-%s.md' % binary), 'w',
                  encoding='utf-8') as f:
            f.write('\n'.join(mine))
        details.extend(mine[2:])
    print('\n'.join(lines))
    with open(os.path.join(dir_path, 'failures.md'), 'w',
              encoding='utf-8') as f:
        f.write('\n'.join(details))
    return 0 if rc == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
