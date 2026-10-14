#!/usr/bin/env python3
"""Syntax-check the JavaScript embedded in MoltBrowser's C++ sources.

Every molt:// WebUI page ships its HTML/JS as C++ raw strings, so a JS
syntax error only shows up at runtime as a blank page. This script pulls
each script out of the raw strings and runs `node --check` on it, which
needs no Chromium checkout.

Checked:
  * every <script>...</script> block inside an R"HTML(...)HTML" string
  * every R"JS(...)JS" string that is a complete program

R"JS(" strings used as base::StringPrintf templates get each %s / %d
replaced with a placeholder value before the check. Any other R"JS(
string that is not a complete program goes in FRAGMENTS with the reason.

Usage: python3 tools/webui_checks/check_embedded_js.py [repo_root]
"""
import os
import re
import subprocess
import sys
import tempfile

ROOTS = ["src/chrome/browser/ui/webui/molt_ai", "src/chrome/browser/molt_ai",
         "src/chrome/browser/ui/views/toolbar"]
EXTS = (".cc", ".h", ".mm")
# path:ordinal -> why it is not a standalone program. Ordinal counts
# R"JS( strings within the file, starting at 1.
FRAGMENTS = {}

RAW = re.compile(r'R"(HTML|JS)\((.*?)\)\1"', re.S)
SCRIPT = re.compile(r"<script(?![^>]*\bsrc=)[^>]*>(.*?)</script>", re.S | re.I)


def node_check(code, label, failures):
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as f:
        f.write(code)
        tmp = f.name
    try:
        r = subprocess.run(["node", "--check", tmp], capture_output=True, text=True)
        if r.returncode != 0:
            msg = (r.stderr.strip().splitlines() or ["syntax error"])
            failures.append((label, "\n      ".join(msg[:6])))
    finally:
        os.unlink(tmp)


def main():
    repo = sys.argv[1] if len(sys.argv) > 1 else "."
    checked, failures = 0, []
    for root in ROOTS:
        for dirpath, _, files in os.walk(os.path.join(repo, root)):
            for name in sorted(files):
                if not name.endswith(EXTS):
                    continue
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, repo)
                src = open(path, encoding="utf-8").read()
                js_ordinal = 0
                for m in RAW.finditer(src):
                    kind, body = m.group(1), m.group(2)
                    line = src.count("\n", 0, m.start()) + 1
                    if kind == "JS":
                        js_ordinal += 1
                        key = f"{rel}:{js_ordinal}"
                        if key in FRAGMENTS:
                            continue
                        checked += 1
                        if re.search(r"%[sd]", body):  # StringPrintf template
                            body = re.sub(r"%[sd]", "null", body.replace("%%", "%"))
                        node_check(body, f"{rel}:{line} (R\"JS( #{js_ordinal})", failures)
                    else:
                        for i, s in enumerate(SCRIPT.finditer(body), 1):
                            checked += 1
                            node_check(s.group(1), f"{rel}:{line} (<script> #{i})", failures)
    for label, msg in failures:
        print(f"FAIL {label}\n      {msg}")
    print(f"{checked - len(failures)}/{checked} embedded scripts parse")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
