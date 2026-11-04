# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""The end-to-end spoken test kit for voice mode (docs/VOICE_E2E.md).

Pure parts (scenarios, expectations, the report, the test log reader, the
WAV renderer's planning) use only the standard library; the renderer's
audio and the mixes need numpy (tools/voice_bench/requirements.txt); the
runtime parts (the fixture server, PulseAudio, the browser) drive a built
MoltBrowser on Linux.
"""

KIT_VERSION = "1.0"
REPORT_SCHEMA = "molt-voice-e2e-report/1"
