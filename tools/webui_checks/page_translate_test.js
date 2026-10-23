// The page-translation script (kPageTranslateScript in
// src/chrome/browser/molt_ai/translate/page_translate_script.cc), run in
// jsdom: what it reads from a page, how it writes translations back and how
// "Show original" restores the page.
//
// The script is one function expression in C++; this reads it from the
// source and calls it with JSON arguments the way page_translate_io.cc does
// (BuildExtractScript, BuildApplyScript, BuildRevertScript), so the test
// can't drift from the code. jsdom has no layout: checkVisibility(),
// getBoundingClientRect() and the computed white-space are stand-ins driven
// by attributes (data-offscreen, data-pre, inline display/visibility), as
// Chromium 152 would report them. In the browser the script runs in
// Translate's isolated world, where the page can't patch what it calls;
// here it shares the page's world, so the test covers its logic.
//
// Run: npm ci && node page_translate_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {JSDOM} = require('jsdom');

const SRC = fs.readFileSync(path.join(
    __dirname,
    '../../src/chrome/browser/molt_ai/translate/page_translate_script.cc'),
    'utf8');
const SCRIPT = (() => {
  const m = /kPageTranslateScript\[\] = R"JS\(([\s\S]*?)\)JS"/.exec(SRC);
  assert(m, 'kPageTranslateScript not found');
  return m[1];
})();

// page_translate_plan.h's caps, read from the source too.
const PLAN = fs.readFileSync(path.join(
    __dirname,
    '../../src/chrome/browser/molt_ai/translate/page_translate_plan.h'),
    'utf8');
function cap(name) {
  const m = new RegExp('k' + name + ' = (\\d+);').exec(PLAN);
  assert(m, name + ' not found');
  return Number(m[1]);
}
const CAPS = {
  slots: cap('MaxSlots'),
  slotChars: cap('MaxSlotChars'),
  pageChars: cap('MaxPageChars'),
  visited: cap('MaxVisitedNodes'),
};

const TEXT = 0, TITLE = 1, ALT = 2, PLACEHOLDER = 3;

function page(html, options = {}) {
  const dom = new JSDOM(html, {runScripts: 'outside-only'});
  const w = dom.window;
  // Chromium's checkVisibility(): false under display:none (or [hidden]) and,
  // with visibilityProperty, for a computed visibility of hidden; false for
  // display:contents (no box of its own).
  w.Element.prototype.checkVisibility = function(opts) {
    if (this.style.display === 'contents') return false;
    for (let e = this; e; e = e.parentElement) {
      if (e.hidden || e.style.display === 'none') return false;
    }
    if (opts && opts.visibilityProperty) {
      for (let e = this; e; e = e.parentElement) {
        if (e.style.visibility === 'visible') break;
        if (e.style.visibility === 'hidden') return false;
      }
    }
    return true;
  };
  w.Element.prototype.getBoundingClientRect = function() {
    return this.closest('[data-offscreen]') ?
        {top: 5000, bottom: 5020, left: 0, right: 100} :
        {top: 10, bottom: 30, left: 0, right: 100};
  };
  w.getComputedStyle = (el) => ({
    whiteSpace: el.closest('[data-pre]') ? 'pre-wrap' : 'normal',
    display: el.style.display || 'block',
  });
  const caps = Object.assign({}, CAPS, options.caps || {});
  const call = (args) =>
      JSON.parse(JSON.stringify(w.eval(SCRIPT + '(' + JSON.stringify(args) + ')')));
  return {
    w, doc: w.document,
    extract: (id = 'a1') => call({op: 'extract', id, caps}),
    apply: (writes, id = 'a1') => call({op: 'apply', id, writes}),
    revert: () => call({op: 'revert'}),
    raw: call,
  };
}

const texts = (r) => r.slots.map((s) => s[2]);

// ---- What is read -------------------------------------------------------

let p = page(`<!doctype html><html><head><title>  Die
  Seite </title><style>p { color: red }</style></head><body>
<h1>Willkommen</h1>
<p>Lesen Sie <a href="/x">mehr</a> hier.</p>
<script>var x = "Hallo";</script>
<noscript>Kein Skript</noscript>
<code>const a = "Wert";</code><pre>Vorformatiert</pre>
<template><p>Vorlage</p></template>
<form><input type="text" placeholder="Suchen..." value="Eingabe">
<input type="submit" value="Absenden"><input type="hidden" value="geheim">
<input type="password" placeholder="Passwort"><textarea placeholder="Notiz">Entwurf</textarea>
<select><option>Erste Wahl</option></select><button>Senden</button></form>
<img src="a.png" alt="Ein Bild"><img src="b.png" alt="">
<div contenteditable="true">Bearbeitbar</div>
<span translate="no">Markenname</span><span class="notranslate">Firma</span>
<div hidden>Versteckt</div><div style="display:none">Auch versteckt</div>
<div style="visibility:hidden">Unsichtbar <span style="visibility:visible">Sichtbar</span></div>
<svg><text>Grafik</text></svg>
<div style="display:contents"><span>Inhalt</span></div>
<div data-offscreen><p>Unten</p></div>
<p>2026 - 12:30</p>
</body></html>`);
let r = p.extract();
assert.strictEqual(r.v, 1);
assert.strictEqual(r.id, 'a1');
assert.strictEqual(r.truncated, false);
assert.deepStrictEqual(r.slots, [
  [TITLE, true, ' Die Seite '],
  [TEXT, true, 'Willkommen'],
  [TEXT, true, 'Lesen Sie '],
  [TEXT, true, 'mehr'],
  [TEXT, true, ' hier.'],
  [PLACEHOLDER, true, 'Suchen...'],
  [PLACEHOLDER, true, 'Passwort'],
  [PLACEHOLDER, true, 'Notiz'],
  [TEXT, true, 'Senden'],
  [ALT, true, 'Ein Bild'],
  [TEXT, true, 'Sichtbar'],
  [TEXT, true, 'Inhalt'],
  [TEXT, false, 'Unten'],
]);
// Never read: scripts, styles, code, form values, hidden, editable,
// opted-out, SVG, digits.
for (const t of ['Hallo', 'color', 'Kein Skript', 'Wert', 'Vorformatiert',
                 'Vorlage', 'Eingabe', 'Absenden', 'geheim', 'Entwurf',
                 'Erste Wahl', 'Bearbeitbar', 'Markenname', 'Firma',
                 'Versteckt', 'Unsichtbar', 'Grafik', '2026']) {
  assert(!texts(r).some((s) => s.includes(t)), t + ' was read');
}

// Whitespace collapses where CSS collapses it, and only there.
p = page('<body><p>  Hallo\n    Welt  </p><div data-pre>  Zeile\n  zwei</div></body>');
assert.deepStrictEqual(texts(p.extract()), [' Hallo Welt ', '  Zeile\n  zwei']);

// An open shadow root's text, where it is shown.
p = page('<body><p>vorher</p><div id="host"><span>Licht</span></div><p>nachher</p></body>');
const shadow = p.doc.getElementById('host').attachShadow({mode: 'open'});
shadow.innerHTML = '<b>Schatten</b><slot></slot>';
assert.deepStrictEqual(texts(p.extract()),
                       ['vorher', 'Schatten', 'Licht', 'nachher']);

// Opted out as a whole, or edited as a whole: nothing.
p = page('<html translate="no"><body><p>Text</p></body></html>');
assert.deepStrictEqual(p.extract().slots, []);
p = page('<body><p>Text</p></body>');
p.doc.designMode = 'on';
assert.deepStrictEqual(p.extract().slots, []);

// ---- Caps ---------------------------------------------------------------

p = page('<body><p>kurz</p><p>' + 'x'.repeat(30) + '</p><p>Ende</p></body>',
         {caps: {slotChars: 20}});
r = p.extract();
// A text over the cap is skipped, not cut.
assert.deepStrictEqual(texts(r), ['kurz', 'Ende']);
assert.strictEqual(r.truncated, false);

p = page('<body><p>eins</p><p>zwei</p><p>drei</p></body>', {caps: {pageChars: 9}});
r = p.extract();
assert.deepStrictEqual(texts(r), ['eins', 'zwei']);
assert.strictEqual(r.truncated, true);

p = page('<body><p>eins</p><p>zwei</p><p>drei</p></body>', {caps: {slots: 2}});
r = p.extract();
assert.deepStrictEqual(texts(r), ['eins', 'zwei']);
assert.strictEqual(r.truncated, true);

p = page('<body>' + '<p>Wort</p>'.repeat(50) + '</body>', {caps: {visited: 21}});
r = p.extract();
assert.strictEqual(r.truncated, true);
assert(r.slots.length <= 10, String(r.slots.length));

// ---- Writing back, as text only ----------------------------------------

const ORIGINAL = `<head><title>Titel</title></head><body><p id="p">Hallo <b>Welt</b></p>
<img id="i" alt="Bild"><input id="f" placeholder="Suche"></body>`;
p = page(ORIGINAL);
const before = p.doc.documentElement.outerHTML;
r = p.extract();
assert.deepStrictEqual(texts(r), ['Titel', 'Hallo ', 'Welt', 'Bild', 'Suche']);
const hostile = '<img src=x onerror="window.pwned=1"><script>window.pwned=2</script>';
r = p.apply([[0, 'Title'], [1, hostile], [2, 'World'], [3, 'Picture'],
             [4, 'Search']]);
assert.deepStrictEqual(r, {v: 1, id: 'a1', applied: 5});
assert.strictEqual(p.doc.title, 'Title');
assert.strictEqual(p.doc.getElementById('p').textContent, hostile + 'World');
// Text, not markup: still one <b> and no new elements.
assert.strictEqual(p.doc.getElementById('p').children.length, 1);
assert.strictEqual(p.doc.querySelectorAll('img').length, 1);
assert.strictEqual(p.w.pwned, undefined);
assert.strictEqual(p.doc.getElementById('i').getAttribute('alt'), 'Picture');
assert.strictEqual(p.doc.getElementById('f').getAttribute('placeholder'), 'Search');
// Each slot once.
assert.strictEqual(p.apply([[2, 'again']]).applied, 0);

// "Show original" puts back exactly what was there.
assert.deepStrictEqual(p.revert(), {v: 1, reverted: 5});
assert.strictEqual(p.doc.documentElement.outerHTML, before);
// The job is gone.
assert.strictEqual(p.apply([[0, 'x']]).applied, -1);
assert.deepStrictEqual(p.revert(), {v: 1, reverted: 0});

// What the page changed is the page's: not overwritten, not reverted.
p = page('<body><p id="a">eins</p><p id="b">zwei</p><p id="c">drei</p></body>');
p.extract();
p.doc.getElementById('a').firstChild.nodeValue = 'neu';
assert.strictEqual(p.apply([[0, 'one'], [1, 'two']]).applied, 1);
assert.strictEqual(p.doc.getElementById('a').textContent, 'neu');
p.doc.getElementById('b').firstChild.nodeValue = 'auch neu';
assert.strictEqual(p.revert().reverted, 0);
assert.strictEqual(p.doc.getElementById('b').textContent, 'auch neu');

// A text the page removed or moved into a script or editable content
// after it was read is not written.
p = page('<body><p id="a">eins</p><p id="b">zwei</p><div id="e" contenteditable></div></body>');
p.extract();
const a = p.doc.getElementById('a').firstChild;
const b = p.doc.getElementById('b').firstChild;
const script = p.doc.createElement('script');
script.appendChild(a);
p.doc.body.appendChild(script);
p.doc.getElementById('e').appendChild(b);
assert.strictEqual(p.apply([[0, 'alert(1)'], [1, 'two']]).applied, 0);
assert.strictEqual(a.nodeValue, 'eins');
p = page('<body><p id="a">eins</p></body>');
p.extract();
p.doc.getElementById('a').remove();
assert.strictEqual(p.apply([[0, 'one']]).applied, 0);

// Malformed writes are skipped; another job's id writes nothing.
p = page('<body><p>eins</p><p>zwei</p></body>');
p.extract('b2');
assert.strictEqual(p.apply([[0, 'one']], 'a1').applied, -1);
assert.strictEqual(p.apply([[1.5, 'x'], [-1, 'x'], [2, 'x'], ['0', 'x'],
                            [0, 7], [0], 'x', null, [1, 'two']], 'b2').applied, 1);
assert.strictEqual(p.apply('nope', 'b2').applied, 0);

// Translating again (another language) starts from the original.
p = page('<body><p>eins</p></body>');
p.extract('c1');
p.apply([[0, 'one']], 'c1');
assert.deepStrictEqual(texts(p.extract('c2')), ['eins']);
assert.strictEqual(p.doc.body.textContent, 'eins');

// Unknown calls are refused.
p = page('<body><p>eins</p></body>');
assert.deepStrictEqual(p.raw({op: 'eval', id: 'a1'}), {v: 1, error: true});
assert.deepStrictEqual(p.raw(null), {v: 1, error: true});

console.log('page_translate_test: ok');
