#!/usr/bin/env python3
# Copyright 2026 GenEye AI Labs Inc.
# Licensed under GPLv3. See LICENSE file.
"""Dry-run (or apply) a rebase of the MoltBrowser overlay onto a new Chromium.

For every upstream file MoltBrowser modifies (patches/series), 3-way merges
  base   = upstream@pin  (chromium_version.txt)
  ours   = src/<path>    (the overlay)
  theirs = upstream@NEW_TAG
with `git merge-file --diff3` into a staging dir, and reports:
  * clean merges, conflicts (with hunk counts), binary files,
  * patches that became empty (the change is upstream now),
  * upstream files that were deleted or moved (with candidates),
  * overlay copies identical to upstream@pin that NEW_TAG changed (left in
    src/ they would silently revert upstream),
  * DEPS-subproject files whose subproject moved,
  * first-party paths that NEW_TAG now also has (collisions),
  * API changes: identifiers that disappeared between pin and NEW_TAG from
    upstream headers that first-party code includes (or that our patches
    touch), and where first-party code / our patch hunks still use them,
    with likely renames. A heuristic; the compile is the real check.

src/ is never touched unless --apply. Results land in
.molt-chromium-cache/rebase/<NEW_TAG>/ (merged/, report.md, report.json).

Usage:
  tools/chromium/rebase.py 152.0.7977.168            # dry run
  tools/chromium/rebase.py 152.0.7977.168 --apply    # write merges to src/
        [--allow-conflicts]  with --apply: also write files that still have
                             conflict markers (resolve them by hand)
        [--out DIR] [--cache DIR] [--remote URL] [--offline] [--no-api-scan]
"""
import argparse
import difflib
import json
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import upstream as up  # noqa: E402

CODE_EXTS = (".h", ".hpp", ".cc", ".cpp", ".c", ".mm", ".m")
# Vendored third-party code is first-party to MoltBrowser but does not use
# Chromium APIs; skip it in the API scan.
VENDORED = ("third_party/llama_cpp/", "third_party/rust/")
INCLUDE_RE = re.compile(rb'^\s*#\s*(?:include|import)\s*"([^"]+)"', re.M)
COMMENT_RE = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\\n])*"|'
                        r"'(?:\\.|[^'\\\n])*'", re.S)
IDENT_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]{3,}\b")


def api_like(name):
    """Identifiers worth reporting: CamelCase, snake_case, kConstants,
    MACROS. Plain lowercase words are mostly noise."""
    if name.startswith("k") and len(name) > 1 and name[1].isupper():
        return True
    has_upper = any(c.isupper() for c in name)
    has_lower = any(c.islower() for c in name)
    return (has_upper and has_lower) or "_" in name.strip("_")


def identifiers(data):
    text = COMMENT_RE.sub(" ", data.decode("utf-8", errors="replace"))
    return {i for i in IDENT_RE.findall(text) if api_like(i)}


def merge3(path, base, ours, theirs, tmp):
    """git merge-file --diff3; returns (merged bytes, conflict count)."""
    files = []
    for name, data in (("ours", ours), ("base", base), ("theirs", theirs)):
        f = Path(tmp, name)
        f.write_bytes(data)
        files.append(str(f))
    proc = subprocess.run(
        ["git", "merge-file", "-p", "--diff3",
         "-L", "src/" + path + " (MoltBrowser)",
         "-L", "upstream@pin", "-L", "upstream@new"] + files,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=up.clean_git_env())
    if proc.returncode < 0 or proc.returncode > 127:
        raise RuntimeError("merge-file failed for %s: %s"
                           % (path, proc.stderr.decode(errors="replace")))
    return proc.stdout, proc.returncode


def added_lines(base, ours):
    """Lines our patch adds or changes (for the API scan of patched files)."""
    a = base.decode("utf-8", errors="replace").splitlines()
    b = ours.decode("utf-8", errors="replace").splitlines()
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    out = []
    for tag, _, _, j1, j2 in sm.get_opcodes():
        if tag in ("replace", "insert"):
            out.extend((j + 1, b[j]) for j in range(j1, j2))
    return out


def find_moves(path, base_oid, new_tree):
    name = path.rsplit("/", 1)[-1]
    exact = [p for p, e in new_tree.items() if e.oid == base_oid]
    if exact:
        return "moved", exact[:3]
    same_name = sorted(p for p in new_tree if p.rsplit("/", 1)[-1] == name)
    return "deleted-or-moved", same_name[:5]


def api_scan(chromium, base, new, kinds, results):
    base_tree, new_tree = chromium.tree(base), chromium.tree(new)
    first_party = [p for p, k in kinds.items() if k == up.FIRST_PARTY
                   and p.endswith(CODE_EXTS) and not p.startswith(VENDORED)]
    patched = [p for p, k in kinds.items() if k == up.PATCHED]

    # Headers first-party code includes, plus headers we patch.
    includers = {}
    for p in first_party:
        for inc in INCLUDE_RE.findall(up.overlay_bytes(p)):
            inc = inc.decode()
            if inc in base_tree and base_tree[inc].type == "blob":
                includers.setdefault(inc, set()).add(p)
    for p in patched:
        if p.endswith((".h", ".hpp")):
            includers.setdefault(p, set())
    changed, kept, removed_headers = [], [], {}
    for h in sorted(includers):
        e = new_tree.get(h)
        if e is None or e.type != "blob":
            _, moved_to = find_moves(h, base_tree[h].oid, new_tree)
            removed_headers[h] = {"included_by": sorted(includers[h]),
                                  "same_name_at_new_tag": moved_to}
        else:
            kept.append(h)
            if e.oid != base_tree[h].oid:
                changed.append(h)
    blobs = chromium.blobs([base_tree[h].oid for h in changed] +
                           [new_tree[h].oid for h in kept])

    # An identifier only counts as removed if no scanned header mentions it
    # at the new tag (otherwise it merely stopped being mentioned in one).
    still_there = set()
    for h in kept:
        still_there |= identifiers(blobs[new_tree[h].oid])
    removed_from = {}   # identifier -> [header]
    added_in = {}       # header -> identifiers added
    for h in changed:
        old = identifiers(blobs[base_tree[h].oid])
        cur = identifiers(blobs[new_tree[h].oid])
        added_in[h] = cur - old
        for ident in old - still_there:
            removed_from.setdefault(ident, []).append(h)

    # Where do first-party files and our patch hunks still use them?
    uses = {}
    pattern = None
    if removed_from:
        pattern = re.compile(r"\b(" + "|".join(
            map(re.escape, sorted(removed_from, key=len, reverse=True)))
            + r")\b")

    def scan(path, numbered_lines):
        for no, line in numbered_lines:
            code = COMMENT_RE.sub(" ", line)
            for m in pattern.finditer(code):
                uses.setdefault(m.group(1), []).append("%s:%d" % (path, no))

    if pattern:
        for p in first_party:
            text = up.overlay_bytes(p).decode("utf-8", errors="replace")
            scan(p, enumerate(text.splitlines(), 1))
        patched_code = [p for p in patched if p.endswith(CODE_EXTS)]
        base_bodies = chromium.blobs(base_tree[p].oid for p in patched_code)
        for p in patched_code:
            scan(p, added_lines(base_bodies[base_tree[p].oid],
                                up.overlay_bytes(p)))

    findings = []
    for ident in sorted(uses):
        headers = removed_from[ident]
        candidates = set()
        for h in headers:
            candidates |= added_in.get(h, set())
        renames = difflib.get_close_matches(ident, sorted(candidates), n=3,
                                            cutoff=0.75)
        findings.append({"symbol": ident, "removed_from": headers,
                         "used_at": sorted(set(uses[ident]))[:20],
                         "use_count": len(set(uses[ident])),
                         "possible_renames": renames})
    results["api"] = {
        "headers_scanned": len(includers),
        "headers_changed": len(changed),
        "headers_removed": removed_headers,
        "symbols_removed": len(removed_from),
        "findings": findings,
    }


def write_report(out, meta, items, results):
    by = {}
    for path, item in sorted(items.items()):
        by.setdefault(item["status"], []).append((path, item))
    lines = ["# Rebase dry run: %s -> %s" % (meta["from"], meta["to"]), "",
             "- base: chromium/src@%s (%s)" % (meta["base_commit"],
                                               meta["from"]),
             "- new:  chromium/src@%s (%s)" % (meta["new_commit"], meta["to"]),
             ""]
    order = [("conflict", "Conflicts (resolve by hand)"),
             ("deleted-or-moved", "Upstream file deleted or moved"),
             ("moved", "Upstream file moved (same content elsewhere)"),
             ("clean", "Clean 3-way merges"),
             ("empty", "Clean, patch now empty (change is upstream)"),
             ("binary-ours", "Binary, upstream changed: our file kept"),
             ("unchanged", "Upstream unchanged: our file kept"),
             ("refresh", "Identical copies upstream changed (would REVERT "
                         "upstream; refresh or drop)"),
             ("identical-unchanged", "Identical copies, upstream unchanged"),
             ("deps-changed", "DEPS-subproject file: subproject moved, "
                              "check by hand"),
             ("deps-unchanged", "DEPS-subproject file: subproject unchanged"),
             ("collision", "First-party paths that now exist upstream")]
    lines.append("| Result | Files |")
    lines.append("|---|---|")
    for key, title in order:
        if by.get(key):
            lines.append("| %s | %d |" % (title, len(by[key])))
    lines.append("")
    for key, title in order:
        if not by.get(key) or key in ("unchanged", "identical-unchanged",
                                      "deps-unchanged"):
            continue
        lines.append("## %s (%d)" % (title, len(by[key])))
        lines.append("")
        for path, item in by[key]:
            extra = ""
            if item.get("conflicts"):
                extra = " - %d conflict hunk(s)" % item["conflicts"]
            if item.get("candidates"):
                extra = " - candidates: " + ", ".join(item["candidates"])
            lines.append("- `%s`%s" % (path, extra))
        lines.append("")
    api = results.get("api")
    if api:
        lines.append("## Upstream API changes that touch MoltBrowser code")
        lines.append("")
        lines.append("Scanned %d upstream headers that first-party code "
                     "includes or that we patch; %d changed, %d were removed "
                     "or moved, %d identifiers disappeared from the changed "
                     "ones." % (api["headers_scanned"], api["headers_changed"],
                                len(api["headers_removed"]),
                                api["symbols_removed"]))
        lines.append("")
        if api["headers_removed"]:
            lines.append("Included headers that no longer exist at the new "
                         "tag:")
            lines.append("")
            for h, info in sorted(api["headers_removed"].items()):
                users = info["included_by"]
                moved = info["same_name_at_new_tag"]
                lines.append("- `%s` (included by %d file(s), e.g. `%s`)%s"
                             % (h, len(users), users[0] if users else "-",
                                "; same name now at " + ", ".join(
                                    "`%s`" % m for m in moved)
                                if moved else "; no file of that name"))
            lines.append("")
        if api["findings"]:
            lines.append("| Removed symbol | From | Used in MoltBrowser code "
                         "| Possible rename |")
            lines.append("|---|---|---|---|")
            for f in api["findings"]:
                lines.append("| `%s` | %s | %d use(s), e.g. `%s` | %s |" % (
                    f["symbol"], ", ".join("`%s`" % h
                                           for h in f["removed_from"][:2]),
                    f["use_count"], f["used_at"][0],
                    ", ".join("`%s`" % r for r in f["possible_renames"])
                    or "-"))
            lines.append("")
    (out / "report.md").write_text("\n".join(lines))
    (out / "report.json").write_text(json.dumps(
        {"meta": meta, "files": items, "api": results.get("api")},
        indent=1, sort_keys=True))
    return by


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("new_tag", help="Chromium tag (e.g. 152.0.7977.168) "
                                    "or 40-hex commit")
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--allow-conflicts", action="store_true")
    ap.add_argument("--out")
    ap.add_argument("--cache", default=str(up.DEFAULT_CACHE))
    ap.add_argument("--remote", default=up.DEFAULT_REMOTE)
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--no-api-scan", action="store_true")
    args = ap.parse_args()

    pin = up.read_pin()
    chromium = up.Upstream(args.cache, args.remote, args.offline)
    base = chromium.resolve(pin.revision)
    new = chromium.resolve(args.new_tag)
    new_version = chromium.version(new)
    base_tree, new_tree = chromium.tree(base), chromium.tree(new)
    out = Path(args.out) if args.out else (
        Path(args.cache) / "rebase" / args.new_tag)
    up.rmtree(out / "merged")
    (out / "merged").mkdir(parents=True, exist_ok=True)

    kinds = up.classify(chromium, base)
    tracked = [p for p, k in kinds.items()
               if k in (up.PATCHED, up.IDENTICAL_KIND)]
    oids = [base_tree[p].oid for p in tracked]
    oids += [new_tree[p].oid for p in tracked
             if p in new_tree and new_tree[p].type == "blob"]
    blobs = chromium.blobs(oids)

    items = {}
    tmp = up.scratch_dir("molt-merge-")
    try:
        for path, kind in sorted(kinds.items()):
            if kind == up.FIRST_PARTY:
                if path in new_tree:
                    items[path] = {"kind": kind, "status": "collision"}
                continue
            if kind == up.DEPS_FILE:
                old_link = chromium.gitlink_for(base, path)
                new_link = chromium.gitlink_for(new, path)
                same = old_link == new_link
                items[path] = {"kind": kind,
                               "status": "deps-unchanged" if same
                               else "deps-changed",
                               "subproject": [old_link, new_link]}
                continue
            b = base_tree[path]
            n = new_tree.get(path)
            item = {"kind": kind}
            items[path] = item
            if n is None or n.type != "blob":
                item["status"], item["candidates"] = find_moves(
                    path, b.oid, new_tree)
                continue
            ours = up.overlay_bytes(path)
            if kind == up.IDENTICAL_KIND:
                if n.oid == b.oid:
                    item["status"] = "identical-unchanged"
                else:
                    item["status"] = "refresh"
                    merged = blobs[n.oid]
                    dst = out / "merged" / path
                    dst.parent.mkdir(parents=True, exist_ok=True)
                    dst.write_bytes(merged)
                continue
            if n.oid == b.oid:
                item["status"] = "unchanged"
                continue
            base_data, theirs = blobs[b.oid], blobs[n.oid]
            if up.is_binary(base_data) or up.is_binary(ours) or \
                    up.is_binary(theirs):
                item["status"] = "binary-ours"
                continue
            merged, conflicts = merge3(path, base_data, ours, theirs, tmp)
            if conflicts:
                item["status"], item["conflicts"] = "conflict", conflicts
            elif merged == theirs:
                item["status"] = "empty"
            else:
                item["status"] = "clean"
            dst = out / "merged" / path
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(merged)
    finally:
        up.rmtree(tmp)

    results = {}
    if not args.no_api_scan:
        api_scan(chromium, base, new, kinds, results)

    meta = {"from": pin.version, "to": new_version, "tag": args.new_tag,
            "base_commit": base, "new_commit": new}
    by = write_report(out, meta, items, results)

    def n(key):
        return len(by.get(key, []))
    patched_total = sum(1 for k in kinds.values() if k == up.PATCHED)
    print("Rebase %s -> %s (%s)" % (pin.version, new_version, new))
    print("Patched upstream files: %d" % patched_total)
    print("  clean merges:          %d (+%d now empty, %d upstream-unchanged,"
          " %d binary kept)" % (n("clean"), n("empty"), n("unchanged"),
                                n("binary-ours")))
    print("  CONFLICTS:             %d" % n("conflict"))
    print("  deleted/moved:         %d" % (n("deleted-or-moved") + n("moved")))
    print("Identical copies:        %d changed upstream (refresh/drop), "
          "%d unchanged" % (n("refresh"), n("identical-unchanged")))
    print("DEPS whole files:        %d need a manual look"
          % n("deps-changed"))
    print("First-party collisions:  %d" % n("collision"))
    if "api" in results:
        api = results["api"]
        print("API scan:                %d removed symbols still used by "
              "MoltBrowser code, %d included headers gone"
              % (len(api["findings"]), len(api["headers_removed"])))
    for path, item in by.get("conflict", []):
        print("  conflict: %s (%d hunks)" % (path, item["conflicts"]))
    print("Report: %s" % (out / "report.md"))

    if not args.apply:
        print("Dry run: src/ was not modified.")
        return 0

    blocking = n("conflict") + n("deleted-or-moved") + n("moved")
    if blocking and not args.allow_conflicts:
        up.die("%d files need manual work; fix them or re-run with "
               "--apply --allow-conflicts to write conflict markers into src/"
               % blocking)
    written = 0
    for path, item in items.items():
        src_file = out / "merged" / path
        if item["status"] in ("clean", "empty", "conflict", "refresh") and \
                src_file.exists():
            dst = up.OVERLAY_DIR / path
            mode = os.stat(dst).st_mode
            dst.write_bytes(src_file.read_bytes())
            os.chmod(dst, mode)
            written += 1
    up.write_pin(new_version, new)
    print("Wrote %d merged files into src/ and moved chromium_version.txt to "
          "%s (%s)." % (written, new_version, new))
    print("Next: resolve conflict markers (grep -rn '^<<<<<<<' src/), fix "
          "deleted/moved files, then\n  tools/chromium/overlay_to_patches.py "
          "&& tools/chromium/verify_patches.py\nand compile on the build "
          "machine (docs/REBASING.md).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
