# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""MoltNet leak and uniformity test kit (docs/LEAK_TESTING.md).

Pure analysis code (pcapread, dissect, traffic, uniformity, report) uses only
the Python standard library and never needs root; the runtime pieces
(netns, services, cdp, harness) drive a built browser on a Linux machine.
"""

KIT_VERSION = '1.0'
REPORT_SCHEMA = 'moltleak-report/1'
