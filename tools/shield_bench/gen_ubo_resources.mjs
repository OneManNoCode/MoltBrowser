#!/usr/bin/env node
// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Generates MoltShield's bundled scriptlet + redirect resources
// (src/chrome/browser/molt_ai/shield/adblock/data/ubo_resources.json) from a
// pinned uBlock Origin release, in the JSON format adblock-rust's
// `Engine::use_resources` expects (a `Vec<adblock::resources::Resource>`).
//
// uBO ships its scriptlets as ES modules (src/js/resources/*.js) that register
// functions via `registerScriptlet`. adblock-rust can't parse that format
// itself (its resource_assembler only reads the pre-2023 `/// name.js`
// format), so this script imports the modules under Node and serializes each
// registered function with `fn.toString()`. Function-style resources are what
// adblock-rust invokes with JSON-escaped arguments (`name("a", "b")`), which
// is what closes the old scriptlet-argument injection hole.
//
// uBlock Origin is GPLv3, like MoltBrowser. Not shipped by itself: the output
// JSON is embedded into the browser by the molt_adblock crate.
//
// Usage:
//   node tools/shield_bench/gen_ubo_resources.mjs [--tag 1.75.0] [--out PATH]
//
// Needs network access to raw.githubusercontent.com. Run from anywhere.

import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import url from 'node:url';

const args = process.argv.slice(2);
function argValue(name, fallback) {
  const i = args.indexOf(name);
  return i >= 0 && i + 1 < args.length ? args[i + 1] : fallback;
}

const TAG = argValue('--tag', '1.75.0');
const HERE = path.dirname(url.fileURLToPath(import.meta.url));
const REPO = path.resolve(HERE, '..', '..');
const OUT = argValue('--out', path.join(
    REPO, 'src/chrome/browser/molt_ai/shield/adblock/data/ubo_resources.json'));
const RAW = `https://raw.githubusercontent.com/gorhill/uBlock/${TAG}/`;

// adblock-rust PermissionMask bit granted to uBO's own ("trusted") lists so
// they may use `trusted-*` scriptlets. Must match kTrustedListPermission in
// molt_adblock's engine.rs.
const TRUSTED_PERMISSION = 1;

async function fetchText(rel) {
  const res = await fetch(RAW + rel);
  if (!res.ok) throw new Error(`${res.status} fetching ${rel}`);
  return await res.text();
}

async function fetchBytes(rel) {
  const res = await fetch(RAW + rel);
  if (!res.ok) throw new Error(`${res.status} fetching ${rel}`);
  return Buffer.from(await res.arrayBuffer());
}

// Mirrors `rel` (and, recursively, every relative module it imports) into
// `root`.
async function mirrorModule(root, rel, seen) {
  if (seen.has(rel)) return;
  seen.add(rel);
  const text = await fetchText(rel);
  const dst = path.join(root, rel);
  fs.mkdirSync(path.dirname(dst), {recursive: true});
  fs.writeFileSync(dst, text);
  const re = /\b(?:import|export)\s+(?:[^'";]*?\s+from\s+)?['"](\.[^'"]+)['"]/g;
  let m;
  const pending = [];
  while ((m = re.exec(text)) !== null) {
    const dep = path.posix.normalize(
        path.posix.join(path.posix.dirname(rel), m[1]));
    pending.push(mirrorModule(root, dep, seen));
  }
  await Promise.all(pending);
}

const MIME_BY_EXT = {
  css: 'text/css', gif: 'image/gif', html: 'text/html',
  js: 'application/javascript', json: 'application/json',
  mp3: 'audio/mp3', mp4: 'video/mp4', png: 'image/png',
  txt: 'text/plain', xml: 'text/xml',
};

async function main() {
  const tmp = fs.mkdtempSync(path.join(os.tmpdir(), 'ubo-res-'));
  try {
    // ---- Scriptlets ----
    await mirrorModule(tmp, 'src/js/resources/scriptlets.js', new Set());
    const mod = await import(url.pathToFileURL(
        path.join(tmp, 'src/js/resources/scriptlets.js')).href);
    const resources = [];
    const names = new Set();
    let skipped = 0;
    for (const s of mod.builtinScriptlets) {
      const src = s.fn.toString();
      // adblock-rust only invokes function-style resources it can name.
      if (!/^function\s+[^()\s{}]+\s*\(/.test(src)) { skipped++; continue; }
      const isFn = s.name.endsWith('.fn');
      const entry = {
        name: s.name,
        aliases: s.aliases || [],
        kind: {mime: isFn ? 'fn/javascript' : 'application/javascript'},
        content: Buffer.from(src, 'utf8').toString('base64'),
      };
      if (s.dependencies && s.dependencies.length) {
        entry.dependencies = s.dependencies;
      }
      if (s.requiresTrust) entry.permission = TRUSTED_PERMISSION;
      resources.push(entry);
      names.add(s.name);
    }

    // ---- Redirect (web accessible) resources ----
    await mirrorModule(tmp, 'src/js/redirect-resources.js', new Set());
    const redirects = (await import(url.pathToFileURL(
        path.join(tmp, 'src/js/redirect-resources.js')).href)).default;
    for (const [name, details] of redirects) {
      if (names.has(name)) continue;
      const ext = name.slice(name.lastIndexOf('.') + 1);
      const mime = MIME_BY_EXT[ext];
      if (!mime) { skipped++; continue; }
      let bytes;
      try {
        bytes = await fetchBytes(`src/web_accessible_resources/${name}`);
      } catch (e) {
        skipped++;
        continue;
      }
      const alias = details && details.alias;
      resources.push({
        name,
        aliases: Array.isArray(alias) ? alias : (alias ? [alias] : []),
        kind: {mime},
        content: bytes.toString('base64'),
      });
      names.add(name);
    }

    resources.sort((a, b) => a.name.localeCompare(b.name));
    fs.mkdirSync(path.dirname(OUT), {recursive: true});
    fs.writeFileSync(OUT, JSON.stringify(resources, null, 0) + '\n');
    console.log(`uBO ${TAG}: wrote ${resources.length} resources ` +
                `(${skipped} skipped) -> ${path.relative(REPO, OUT)}`);
  } finally {
    fs.rmSync(tmp, {recursive: true, force: true});
  }
}

main().catch(e => { console.error(e); process.exit(1); });
