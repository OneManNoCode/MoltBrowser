#!/usr/bin/env python3
"""Fail if a molt:// WebUI page could need 'unsafe-inline' again.

The molt WebUI pages are served under a strict CSP (see
src/chrome/browser/ui/webui/molt_ai/molt_webui_csp.h): script-src 'self'
with no 'unsafe-inline', and Trusted Types. Under that policy an inline
handler attribute or an inline <script> body silently never runs, so a
regression shows up only as a dead button or a blank page. This script
scans the HTML and JS embedded in the WebUI C++ sources and fails on:

  * an on<event>= attribute in HTML markup, including markup built inside
    JS string literals ('<button onclick="...">') and
    setAttribute('on...', ...)
  * a <script> element without src=, or with an inline body
  * javascript: URLs in href/src/action attributes
  * 'unsafe-inline' in a script-src directive
  * on Trusted Types pages, an HTML sink (innerHTML / outerHTML /
    insertAdjacentHTML / document.write) that isn't fed by moltHTML(),
    the page's single documented policy — or any HTML sink at all on a
    page whose CSP allows no policy.

Usage: python3 tools/webui_checks/check_no_inline_handlers.py [repo_root]
"""
import os
import re
import sys

WEBUI_DIR = "src/chrome/browser/ui/webui/molt_ai"

# Pages whose conversion is owned by someone else and still pending.
# Listed explicitly so the remaining debt stays visible in CI output.
PENDING = {
    "molt_net_ui.cc": "MoltNet/Tor page, owned by the Tor work; still uses "
                      "inline handlers + 'unsafe-inline'",
}

# Page scripts (served as /app.js) and their Trusted Types mode:
#   "none"      trusted-types (no policies): no HTML sinks at all
#   "molt-html" every HTML sink must go through moltHTML(...)
# Keyed by source file -> {raw-string constant name: mode}. R"JS( strings
# not listed here are scripts injected into web pages (form fill, vault
# autofill, ...), which run under the page's own CSP, not ours.
PAGE_SCRIPTS = {
    "molt_home_ui.cc": {"kHomePageJs": "none"},
    "molt_ai_memory_ui.cc": {"kMemoryPageJs": "none"},
    "molt_ai_update_ui.cc": {"kUpdatePageJs": "none"},
    "molt_import_ui.cc": {"kImportPageJs": "molt-html"},
    "molt_ai_settings_ui.cc": {"kSettingsPageJs": "molt-html"},
    "molt_ai_agent_ui.cc": {"kAgentPageJs": "molt-html"},
    "molt_ai_automation_ui.cc": {"kAutomationPageJs": "molt-html"},
    "molt_ai_ui.cc": {"kMoltAIPageJs": "molt-html"},
    "molt_ai_chat_ui.cc": {"kChatPagePreludeJs": "molt-html"},
    "molt_ai_chat_ui_js.h": {"kMoltChatJS": "molt-html"},
    "molt_webui_csp.h": {"kMoltActionsJs": "none",
                         "kMoltTrustedHtmlJs": "none"},
}

RAW = re.compile(
    r'(?:(?:constexpr|const)\s+char\s+(\w+)\[\]\s*=\s*)?R"(HTML|JS)\((.*?)\)\2"',
    re.S)
TAG = re.compile(r"<[a-zA-Z][^<>]*>", re.S)
ON_ATTR = re.compile(r"\son[a-z]+\s*=", re.I)
JS_ON_ATTR = re.compile(r"(?<![\w.$])on[a-z]+\s*=\s*(?:\\?[\"']|[a-zA-Z(])")
JS_SET_ON = re.compile(r"setAttribute\(\s*[\"']on[a-z]+[\"']", re.I)
SCRIPT = re.compile(r"<script\b([^>]*)>(.*?)</script\s*>", re.S | re.I)
JS_URL = re.compile(r"\b(?:href|src|action|formaction)\s*=\s*\\?[\"']?\s*javascript:",
                    re.I)
# Only inside a C++ string literal ("script-src ... 'unsafe-inline' ..."),
# so comments that talk about the policy don't trip it.
UNSAFE_INLINE = re.compile(r"\"script-src[^;\"]*'unsafe-inline'")
HTML_SINK = re.compile(
    r"\.(innerHTML|outerHTML)\s*(\+?=)(?!=)\s*|"
    r"\.(insertAdjacentHTML)\s*\(\s*[^,]+,\s*|"
    r"\bdocument\.(write|writeln)\s*\(\s*")


def line_of(src, pos):
    return src.count("\n", 0, pos) + 1


def check_html(rel, src, start, body, problems):
    for t in TAG.finditer(body):
        if ON_ATTR.search(t.group(0)):
            problems.append((rel, line_of(src, start + t.start()),
                             "inline handler: " + t.group(0)[:100]))
        if JS_URL.search(t.group(0)):
            problems.append((rel, line_of(src, start + t.start()),
                             "javascript: URL: " + t.group(0)[:100]))
    for s in SCRIPT.finditer(body):
        attrs, inner = s.group(1), s.group(2)
        if not re.search(r"\bsrc\s*=", attrs) or inner.strip():
            problems.append((rel, line_of(src, start + s.start()),
                             "inline <script> (move it to app.js)"))
    if re.search(r"<script(?![^>]*\bsrc=)[^>]*>", body, re.I) and \
            not SCRIPT.search(body):
        problems.append((rel, line_of(src, start), "unterminated inline <script>"))


def check_js(rel, src, start, body, problems):
    for m in JS_ON_ATTR.finditer(body):
        problems.append((rel, line_of(src, start + m.start()),
                         "inline handler in JS-built markup: " +
                         body[m.start():m.start() + 60].split("\n")[0]))
    for m in JS_SET_ON.finditer(body):
        problems.append((rel, line_of(src, start + m.start()),
                         "setAttribute of an on* handler"))
    for m in JS_URL.finditer(body):
        problems.append((rel, line_of(src, start + m.start()),
                         "javascript: URL in JS-built markup"))
    for s in SCRIPT.finditer(body):
        problems.append((rel, line_of(src, start + s.start()),
                         "<script> element built in JS"))


def check_sinks(rel, src, start, body, mode, problems):
    for m in HTML_SINK.finditer(body):
        rest = body[m.end():m.end() + 9]
        if mode == "molt-html" and rest.startswith("moltHTML("):
            continue
        what = (m.group(1) or m.group(3) or "document." + (m.group(4) or ""))
        why = ("HTML sink on a page with no Trusted Types policy"
               if mode == "none" else "HTML sink not fed by moltHTML(...)")
        problems.append((rel, line_of(src, start + m.start()),
                         f"{why}: {what} {body[m.start():m.end() + 30].strip()[:80]}"))


def main():
    repo = sys.argv[1] if len(sys.argv) > 1 else "."
    root = os.path.join(repo, WEBUI_DIR)
    problems, scanned, page_scripts_seen = [], 0, set()
    for name in sorted(os.listdir(root)):
        if not name.endswith((".cc", ".h")):
            continue
        if name in PENDING:
            print(f"SKIP {WEBUI_DIR}/{name}: {PENDING[name]}")
            continue
        rel = f"{WEBUI_DIR}/{name}"
        src = open(os.path.join(root, name), encoding="utf-8").read()
        for m in UNSAFE_INLINE.finditer(src):
            problems.append((rel, line_of(src, m.start()),
                             "'unsafe-inline' in script-src"))
        page = PAGE_SCRIPTS.get(name, {})
        for m in RAW.finditer(src):
            const, kind, body = m.group(1), m.group(2), m.group(3)
            start = m.start(3)
            scanned += 1
            if kind == "HTML":
                check_html(rel, src, start, body, problems)
            elif const in page:
                page_scripts_seen.add((name, const))
                check_js(rel, src, start, body, problems)
                check_sinks(rel, src, start, body, page[const], problems)
            else:
                # Injected or helper JS: still must not build inline handlers
                # if it ever ends up in our markup.
                check_js(rel, src, start, body, problems)
    for name, consts in PAGE_SCRIPTS.items():
        for const in consts:
            if (name, const) not in page_scripts_seen and \
                    os.path.exists(os.path.join(root, name)):
                problems.append((f"{WEBUI_DIR}/{name}", 1,
                                 f"page script {const} not found (update "
                                 "PAGE_SCRIPTS in this checker)"))
    for rel, line, msg in problems:
        msg = " ".join(msg.split())
        print(f"FAIL {rel}:{line}: {msg}")
    print(f"{scanned} embedded HTML/JS strings scanned, "
          f"{len(problems)} problem(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
