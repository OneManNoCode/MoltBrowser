// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Audit B7, end to end: runs the scriptlet bundles MoltShield builds with
// adblock-rust (real ones for a few sites, and one built from hostile
// filter arguments) in jsdom, the way CosmeticFilterAgent runs them in the
// page's main world. Each bundle must parse, run without throwing, and no
// filter argument may execute.
//
//   cargo test --release   (writes target/scriptlet_corpus.json)
//   (cd tools/webui_checks && npm ci) && node tools/shield_bench/scriptlets_test.js
'use strict';
const fs = require('fs');
const path = require('path');
const repo = process.argv[2] || path.join(__dirname, '..', '..');
const {JSDOM, VirtualConsole} = require(path.join(
    process.argv[3] || path.join(repo, 'tools/webui_checks/node_modules'),
    'jsdom'));

const corpusPath = path.join(repo, 'tools/shield_bench/target/scriptlet_corpus.json');
if (!fs.existsSync(corpusPath)) {
  console.log('scriptlets: no target/scriptlet_corpus.json (run cargo test ' +
              '--release in tools/shield_bench first); skipped');
  process.exit(0);
}

let failures = 0;
for (const {name, script} of JSON.parse(fs.readFileSync(corpusPath, 'utf8'))) {
  try {
    // Parses as one program (the IIFE wrapper of engine.rs).
    new Function(script);
    const vc = new VirtualConsole();  // scriptlets may log; keep quiet
    const dom = new JSDOM('<!DOCTYPE html><html><body><p>x</p></body></html>', {
      runScripts: 'outside-only',
      url: name.startsWith('https://') ? name : 'https://hostile.example/',
      virtualConsole: vc,
    });
    const w = dom.window;
    w.__moltPwned = 0;
    w.eval(script);
    // Touch the properties the hostile scriptlets were pointed at.
    for (let i = 0; i < 7; i++) {
      try { void w['p' + i]; } catch (e) { /* abort-on-property-read */ }
    }
    if (w.__moltPwned) throw new Error('a filter argument executed');
    console.log(`ok   ${name} (${script.length} bytes)`);
  } catch (e) {
    failures++;
    console.log(`FAIL ${name}: ${e.message}`);
  }
}
console.log(`scriptlets: ${failures ? 'FAILED' : 'all bundles parse, run, and ' +
            'no argument executes'}`);
process.exit(failures ? 1 : 0);
