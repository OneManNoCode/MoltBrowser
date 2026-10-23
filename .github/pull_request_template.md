## What and why

<!-- What does this change, and why is it needed? Link issues: "Fixes #123". -->

## How it was tested

<!-- Builds, tests (molt_ai_unittests, tools/webui_checks), manual steps, platforms. -->

## Checklist

- [ ] Commits are signed off (`git commit -s`, see CONTRIBUTING.md)
- [ ] Changes to upstream Chromium files are made in `src/`, and
      `tools/chromium/overlay_to_patches.py` + `tools/chromium/verify_patches.py`
      were re-run (see docs/REBASING.md)
- [ ] Anything this needs on the build machine (a compile, tests, device
      checks) is listed in docs/BUILD_MACHINE_CHECKLIST.md
- [ ] No secrets, keys or personal data in the diff
- [ ] New network access, data collection or third-party downloads are
      called out above (downloads must be pinned by SHA-256)
- [ ] Security-sensitive? Say so here, and report vulnerabilities privately
      (SECURITY.md), not in this PR
