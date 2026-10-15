// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// jsdom regression test for the MoltShield consent killer script embedded
// in src/chrome/browser/molt_ai/consent/consent_killer_tab_helper.cc (not
// shipped). Uses tools/webui_checks' jsdom:
//   (cd tools/webui_checks && npm ci) && node tools/shield_bench/consent_killer_test.js
const fs = require('fs');
const path = require('path');
const repo = process.argv[2] || path.join(__dirname, '..', '..');
const {JSDOM} = require(path.join(
    process.argv[3] || path.join(repo, 'tools/webui_checks/node_modules'),
    'jsdom'));

const src = fs.readFileSync(path.join(repo,
    'src/chrome/browser/molt_ai/consent/consent_killer_tab_helper.cc'), 'utf8');
const killer = src.match(/kKillerJS\[\] = R"JS\(([\s\S]*?)\)JS"/)[1];

function makeDom(html, mode) {
  const dom = new JSDOM(`<!DOCTYPE html><html><body>${html}</body></html>`,
                        {runScripts: 'outside-only', pretendToBeVisual: true});
  const w = dom.window;
  // jsdom has no layout: treat everything not display:none as visible.
  w.Element.prototype.getClientRects = function() {
    let e = this;
    while (e && e.nodeType === 1) {
      if (w.getComputedStyle(e).display === 'none') return [];
      e = e.parentNode && e.parentNode.host ? e.parentNode.host : e.parentNode;
    }
    return [{}];
  };
  w.clicked = [];
  w.document.addEventListener('click', (ev) => {
    w.clicked.push(ev.composedPath()[0].textContent.trim());
  }, true);
  return {dom, w, run() { w.eval(`var __moltConsentMode = '${mode || 'page'}';\n` + killer); }};
}

const cases = [];
function test(name, fn) { cases.push([name, fn]); }
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

test('OneTrust: reject, not settings', async () => {
  const t = makeDom(`<div id="onetrust-banner-sdk">We use cookies
    <button id="onetrust-pc-btn-handler">Cookie Settings</button>
    <button id="onetrust-reject-all-handler">Reject All</button></div>`);
  t.run();
  return t.w.clicked.join('|') === 'Reject All';
});
test('Cookiebot: Deny, never Customize', async () => {
  const t = makeDom(`<div id="CybotCookiebotDialog">This website uses cookies
    <button id="CybotCookiebotDialogBodyLevelButtonCustomize">Customize</button>
    <button id="CybotCookiebotDialogBodyButtonDecline">Deny</button></div>`);
  t.run();
  return t.w.clicked.join('|') === 'Deny';
});
test('generic cookie dialog by text, skipping Manage settings', async () => {
  const t = makeDom(`<div role="dialog"><p>We and our partners use cookies</p>
    <button>Manage settings</button><button>Accept all</button>
    <button>Reject all</button></div>`);
  t.run();
  return t.w.clicked.join('|') === 'Reject all';
});
test('non-consent dialog is left alone', async () => {
  const t = makeDom(`<div role="dialog"><p>Subscribe to our newsletter!</p>
    <button>No thanks</button><button>Reject</button></div>`);
  t.run();
  await sleep(300);
  return t.w.clicked.length === 0;
});
test('reject-looking button outside any consent container is left alone', async () => {
  const t = makeDom(`<main><p>cookie recipes</p><button>Reject all</button></main>`);
  t.run();
  await sleep(300);
  return t.w.clicked.length === 0;
});
test('open shadow root (Usercentrics-style) vendor button', async () => {
  const t = makeDom(`<div id="usercentrics-root"></div>`);
  const host = t.w.document.getElementById('usercentrics-root');
  const sr = host.attachShadow({mode: 'open'});
  sr.innerHTML = `<div>We use cookies <button data-testid="uc-more-button">Settings</button>
    <button data-testid="uc-deny-all-button">Deny</button></div>`;
  t.run();
  return t.w.clicked.join('|') === 'Deny';
});
test('open shadow root text fallback', async () => {
  const t = makeDom(`<div id="usercentrics-root"></div>`);
  const sr = t.w.document.getElementById('usercentrics-root').attachShadow({mode: 'open'});
  sr.innerHTML = `<div>Privacy settings: cookies <button>Customize</button>
    <button>Reject all</button></div>`;
  t.run();
  return t.w.clicked.join('|') === 'Reject all';
});
test('late banner caught by MutationObserver', async () => {
  const t = makeDom(`<p>content</p>`);
  t.run();
  await sleep(100);
  const d = t.w.document.createElement('div');
  d.innerHTML = `<div class="qc-cmp2-container">We value your privacy
    <button mode="secondary">MORE OPTIONS</button><button mode="secondary">DISAGREE</button>
    <button mode="primary">AGREE</button></div>`;
  t.w.document.body.appendChild(d);
  await sleep(400);
  return t.w.clicked.join('|') === 'DISAGREE';
});
test('CMP document (e.g. Sourcepoint iframe): whole-page text fallback', async () => {
  const t = makeDom(`<div class="message"><button>Options</button><button>Reject all</button></div>`,
                    'cmp-document');
  t.run();
  return t.w.clicked.join('|') === 'Reject all';
});
test('repurposed vendor id that accepts is not clicked', async () => {
  const t = makeDom(`<div id="onetrust-banner-sdk">cookies
    <button id="onetrust-reject-all-handler">Accept All</button></div>`);
  t.run();
  await sleep(300);
  return t.w.clicked.length === 0;
});
test('hidden reject button is not clicked', async () => {
  const t = makeDom(`<div role="dialog">cookies <button style="display:none">Reject all</button>
    <button>Only necessary</button></div>`);
  t.run();
  return t.w.clicked.join('|') === 'Only necessary';
});
test('runs once per page (idempotent) and stops after a click', async () => {
  const t = makeDom(`<div role="dialog">cookies <button>Reject all</button></div>`);
  t.run(); t.run();
  const d = t.w.document.createElement('div');
  d.innerHTML = `<div role="dialog">cookies <button>Decline all</button></div>`;
  t.w.document.body.appendChild(d);
  await sleep(400);
  return t.w.clicked.join('|') === 'Reject all';
});

(async () => {
  let failed = 0;
  for (const [name, fn] of cases) {
    let ok = false;
    try { ok = await fn(); } catch (e) { console.log(e); }
    console.log((ok ? 'ok   ' : 'FAIL ') + name);
    if (!ok) failed++;
  }
  console.log(`${cases.length - failed}/${cases.length} consent killer cases pass`);
  process.exit(failed ? 1 : 0);
})();
