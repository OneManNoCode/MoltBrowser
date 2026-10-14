#!/usr/bin/env python3
"""Syntax-check the JavaScript embedded in MoltBrowser's C++ sources.

Every molt:// WebUI page ships its HTML/JS as C++ raw strings, so a JS
syntax error only shows up at runtime as a blank page. This script pulls
each script out of the raw strings and runs `node --check` on it, which
needs no Chromium checkout.

Checked:
  * every <script>...</script> block inside an R"HTML(...)HTML" string
  * every R"JS(...)JS" string that is a complete program
  * every molt WebUI page script as actually served: the /app.js a data
    source builds by concatenating constants (shared prologue + page
    script) is reassembled and checked as one program, and every
    <script src="..."> in a page's HTML must be a resource its data
    source serves (molt pages run no inline script; see
    molt_webui_csp.h)

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
NAMED_JS = re.compile(r'char\s+(k\w+)\[\]\s*=\s*R"JS\((.*?)\)JS"', re.S)
WEBUI = "src/chrome/browser/ui/webui/molt_ai"
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


def check_served_scripts(repo, failures):
    """Reassemble each page's served scripts and check them as one program."""
    root = os.path.join(repo, WEBUI)
    consts = {}
    files = sorted(f for f in os.listdir(root) if f.endswith(EXTS))
    for name in files:
        src = open(os.path.join(root, name), encoding="utf-8").read()
        for m in NAMED_JS.finditer(src):
            consts[m.group(1)] = m.group(2)
    checked = 0
    for name in files:
        src = open(os.path.join(root, name), encoding="utf-8").read()
        rel = f"{WEBUI}/{name}"
        served = set()
        # if (molt_webui::IsAppJsRequest(url)) { ... Run(... kA + kB ...); return; }
        # and if (url.path() == "/x.js") { ... }
        for m in re.finditer(r'if \((?:molt_webui::IsAppJsRequest\(url\)|'
                             r'url\.path\(\) == "(/[\w.-]+\.js)")\) \{(.*?)return;',
                             src, re.S):
            path = m.group(1) or "/app.js"
            parts = [p.split("::")[-1] for p in
                     re.findall(r"\b(?:[\w]+::)*k\w+", m.group(2))]
            parts = [p for p in parts if p in consts]
            served.add(path)
            checked += 1
            node_check("".join(consts[p] for p in parts),
                       f"{rel} ({path} = {' + '.join(parts)})", failures)
        # molt_ai_update_ui.cc style: url.path() == "/app.js" ? kJs : kHtml
        for m in re.finditer(r'url\.path\(\) == "(/[\w.-]+\.js)" \? std::string\((k\w+)\)',
                             src):
            served.add(m.group(1))
            checked += 1
            node_check(consts[m.group(2)], f"{rel} ({m.group(1)} = {m.group(2)})",
                       failures)
        for m in RAW.finditer(src):
            if m.group(1) != "HTML":
                continue
            for s in re.finditer(r'<script\b[^>]*\bsrc="([^"]+)"', m.group(2)):
                want = "/" + s.group(1).lstrip("/")
                if want not in served:
                    failures.append((f"{rel}", f'<script src="{s.group(1)}"> '
                                     "is not served by this data source"))
    return checked


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
    checked += check_served_scripts(repo, failures)
    for label, msg in failures:
        print(f"FAIL {label}\n      {msg}")
    print(f"{checked - len(failures)}/{checked} embedded scripts parse")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
