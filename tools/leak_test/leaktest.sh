#!/bin/bash
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
#
# MoltNet leak and uniformity test kit. See docs/LEAK_TESTING.md.
#
#   sudo tools/leak_test/leaktest.sh run --browser /path/to/moltbrowser
#   tools/leak_test/leaktest.sh report <run dir>
#   tools/leak_test/leaktest.sh diff <run A>/report.json <run B>/report.json
#   sudo tools/leak_test/leaktest.sh traffic --browser /path/to/moltbrowser
#   tools/leak_test/leaktest.sh hosts [--write]
#
# Python runs isolated (-I): no user site-packages, no PYTHON* variables,
# nothing imported from the current directory.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec python3 -I "$here/leaktest.py" "$@"
