// The page-translation script (kPageTranslateScript in
// src/chrome/browser/molt_ai/translate/page_translate_script.cc), run in
// jsdom: what it reads from a page, how it writes translations back and how
// "Show original" restores the page.
//
// The script is one function expression in C++; this reads it from the
// source and calls it with JSON arguments the way page_translate_io.cc does
// (BuildExtractScript, BuildApplyScript, BuildRevertScript), so the test
// can't drift from the code. jsdom has no layout: checkVisibility(),
// getBoundingClientRect() and the computed style (white-space, display,
// visibility) are stand-ins driven by attributes (data-offscreen, data-pre,
// inline display/visibility), as Chromium 152 would report them; so are
// designMode and isContentEditable, which jsdom lacks. jsdom doesn't let a
// form's named controls (or a document's named elements) stand in for
// their members as Chromium does; p.override() does. In the browser the
// script runs in Translate's isolated world, where the page can't patch
// what it calls; here it shares the page's world, so the test covers its
// logic.
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
  // Chromium's DOM methods and its layout and style are native: a form's
  // named controls never reach them. jsdom's selector engine reads an
  // element's members from script, so p.override() steps aside while it
  // runs, and the stand-ins below use the prototypes.
  let native = 0;
  for (const [proto, name] of [
           [w.Element.prototype, 'closest'], [w.Element.prototype, 'matches'],
           [w.Element.prototype, 'querySelector'],
           [w.Element.prototype, 'querySelectorAll'],
           [w.Document.prototype, 'querySelector'],
           [w.Document.prototype, 'querySelectorAll'],
           [w.Document.prototype, 'getElementById']]) {
    const fn = proto[name];
    proto[name] = function(...rest) {
      native++;
      try {
        return Reflect.apply(fn, this, rest);
      } finally {
        native--;
      }
    };
  }
  // What Chromium's [LegacyOverrideBuiltIns] does for a form's named
  // controls (and a document's named images, forms and embeds), in every
  // world: each of |names| on |obj| is now an element the page put there
  // (or |value|).
  const override = (obj, names, value) => {
    const decoy = (obj.ownerDocument || obj).createElement('em');
    for (const name of names) {
      let d;
      for (let o = Object.getPrototypeOf(obj); o && !d;
           o = Object.getPrototypeOf(o)) {
        d = Object.getOwnPropertyDescriptor(o, name);
      }
      Object.defineProperty(obj, name, {
        configurable: true,
        get() {
          if (native) return d ? (d.get ? d.get.call(obj) : d.value) : undefined;
          return value !== undefined ? value : decoy;
        },
      });
    }
  };
  const own = (proto, name) => Object.getOwnPropertyDescriptor(proto, name);
  const parentOf = (e) => own(w.Node.prototype, 'parentElement').get.call(e);
  const styleOf = (e) => own(w.HTMLElement.prototype, 'style').get.call(e);
  const hiddenOf = (e) => own(w.HTMLElement.prototype, 'hidden').get.call(e);
  const within = (e, sel) => w.Element.prototype.closest.call(e, sel) !== null;
  // Chromium's checkVisibility(): false under display:none (or [hidden]) and,
  // with visibilityProperty, for a computed visibility of hidden; false for
  // display:contents (no box of its own).
  w.Element.prototype.checkVisibility = function(opts) {
    if (styleOf(this).display === 'contents') return false;
    for (let e = this; e; e = parentOf(e)) {
      if (hiddenOf(e) || styleOf(e).display === 'none') return false;
    }
    if (opts && opts.visibilityProperty) {
      for (let e = this; e; e = parentOf(e)) {
        if (styleOf(e).visibility === 'visible') break;
        if (styleOf(e).visibility === 'hidden') return false;
      }
    }
    return true;
  };
  w.Element.prototype.getBoundingClientRect = function() {
    // No box, no rectangle.
    if (styleOf(this).display === 'contents') {
      return {top: 0, bottom: 0, left: 0, right: 0};
    }
    return within(this, '[data-offscreen]') ?
        {top: 5000, bottom: 5020, left: 0, right: 100} :
        {top: 10, bottom: 30, left: 0, right: 100};
  };
  w.getComputedStyle = (el) => {
    let visibility = 'visible';  // Inherited.
    for (let e = el; e; e = parentOf(e)) {
      if (styleOf(e).visibility) {
        visibility = styleOf(e).visibility;
        break;
      }
    }
    return {
      whiteSpace: within(el, '[data-pre]') ? 'pre-wrap' : 'normal',
      display: styleOf(el).display || 'block',
      visibility,
    };
  };
  // Chromium's Document.prototype.designMode and
  // HTMLElement.prototype.isContentEditable, which jsdom lacks.
  const modes = new WeakMap();
  Object.defineProperty(w.Document.prototype, 'designMode', {
    configurable: true,
    get() { return modes.get(this) || 'off'; },
    set(v) { modes.set(this, String(v).toLowerCase() === 'on' ? 'on' : 'off'); },
  });
  Object.defineProperty(w.HTMLElement.prototype, 'isContentEditable', {
    configurable: true,
    get() {
      for (let e = this; e; e = parentOf(e)) {
        const v = w.Element.prototype.getAttribute.call(e, 'contenteditable');
        if (v !== null) return v.toLowerCase() !== 'false';
      }
      return false;
    },
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
    override,
  };
}

const texts = (r) => r.slots.map((s) => s[2]);

// Every member the script reaches on a node, an element or a document.
const MEMBERS = [
  'closest', 'getAttribute', 'setAttribute', 'querySelector',
  'getBoundingClientRect', 'checkVisibility', 'firstChild', 'nextSibling',
  'parentNode', 'parentElement', 'nodeType', 'nodeValue', 'isConnected',
  'localName', 'namespaceURI', 'shadowRoot', 'classList',
  'isContentEditable', 'type', 'textContent', 'childNodes', 'tagName',
];

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

// ---- Text in display: contents --------------------------------------

// An element with display: contents has no box (checkVisibility() is false,
// its rectangle empty); its own text is shown in its box owner's box, with
// the element's own visibility.
p = page(`<body><div style="display:contents">Direkt <b>fett</b></div>
<div style="display:none"><span style="display:contents">Versteckt</span></div>
<div style="visibility:hidden"><span style="display:contents">Unsichtbar</span></div>
<div style="visibility:hidden"><span style="display:contents;visibility:visible">Sichtbar</span></div>
<div><span style="display:contents"><span style="display:contents">Tief</span></span></div>
<div data-offscreen><span style="display:contents">Unten</span></div></body>`);
r = p.extract();
assert.deepStrictEqual(r.slots, [
  [TEXT, true, 'Direkt '],
  [TEXT, true, 'fett'],
  [TEXT, true, 'Sichtbar'],
  [TEXT, true, 'Tief'],
  [TEXT, false, 'Unten'],
]);
// In a shadow root: the host holds the box.
p = page('<body><div id="host"></div></body>');
p.doc.getElementById('host').attachShadow({mode: 'open'}).innerHTML =
    '<span style="display:contents">Schatten</span>';
assert.deepStrictEqual(texts(p.extract()), ['Schatten']);
p = page('<body><div id="host" style="display:none"></div></body>');
p.doc.getElementById('host').attachShadow({mode: 'open'}).innerHTML =
    '<span style="display:contents">Schatten</span>';
assert.deepStrictEqual(texts(p.extract()), []);

// ---- A textarea's placeholder -------------------------------------------

// Its placeholder is written back (its text is never read or written).
p = page('<body><textarea id="t" placeholder="Notiz">Entwurf</textarea></body>');
assert.deepStrictEqual(texts(p.extract()), ['Notiz']);
assert.strictEqual(p.apply([[0, 'Note']]).applied, 1);
let area = p.doc.getElementById('t');
assert.strictEqual(area.getAttribute('placeholder'), 'Note');
assert.strictEqual(area.value, 'Entwurf');
assert.strictEqual(p.revert().reverted, 1);
assert.strictEqual(area.getAttribute('placeholder'), 'Notiz');
// But not once the page moved it into a script or editable content.
for (const into of ['script', 'div contenteditable', 'template']) {
  p = page('<body><textarea id="t" placeholder="Notiz"></textarea>' +
           '<' + into + ' id="x"></' + into.split(' ')[0] + '></body>');
  p.extract();
  area = p.doc.getElementById('t');
  const x = p.doc.getElementById('x');
  (x.content || x).appendChild(area);
  assert.strictEqual(p.apply([[0, 'Note']]).applied, 0, into);
  assert.strictEqual(area.getAttribute('placeholder'), 'Notiz', into);
}

// ---- Named controls and named elements ----------------------------------

// A form's controls named after the members the script calls (or a
// document's named elements) don't change what it reads or writes: it
// reaches every member through the prototypes.
const FORM_PAGE = `<head><title>Titel</title></head><body>
<form id="f" class="c">Formular <b>fett</b><input id="i" placeholder="Feld">
<img id="m" alt="Bild"></form><p>Danach</p></body>`;
const FORM_TEXTS = ['Titel', 'Formular ', 'fett', 'Feld', 'Bild', 'Danach'];
p = page(FORM_PAGE);
const plain = p.extract();
assert.deepStrictEqual(texts(plain), FORM_TEXTS);
for (const value of [undefined, null, 7, 'x', () => { throw new Error('no'); }]) {
  p = page(FORM_PAGE);
  const form = p.doc.getElementById('f');
  p.override(form, MEMBERS, value);
  p.override(p.doc, ['body', 'head', 'documentElement', 'designMode'], value);
  r = p.extract();
  assert.deepStrictEqual(r, plain, String(value));
  assert.deepStrictEqual(
      p.apply([[0, 'Title'], [1, 'Form '], [2, 'bold'], [3, 'Field'],
               [4, 'Picture'], [5, 'After']]),
      {v: 1, id: 'a1', applied: 6}, String(value));
  assert.strictEqual(p.doc.querySelector('b').textContent, 'bold');
  assert.strictEqual(p.doc.getElementById('i').getAttribute('placeholder'),
                     'Field');
  assert.deepStrictEqual(p.revert(), {v: 1, reverted: 6}, String(value));
  assert.strictEqual(p.doc.querySelector('b').textContent, 'fett');
}
// The job's state is the script's own, never an element the page named
// (the window's named properties, which jsdom lacks, emulated as Chromium
// has them: writable data properties on the window's prototype chain).
p = page('<body><p id="__moltPageTranslation">eins</p></body>');
Object.defineProperty(Object.getPrototypeOf(p.w), '__moltPageTranslation', {
  configurable: true,
  writable: true,
  value: p.doc.getElementById('__moltPageTranslation'),
});
assert.strictEqual(p.apply([[0, 'one']]).applied, -1);
assert.deepStrictEqual(p.revert(), {v: 1, reverted: 0});
assert.deepStrictEqual(texts(p.extract()), ['eins']);
assert.strictEqual(p.apply([[0, 'one']]).applied, 1);

// Whatever a slot's checks run into (here a patched prototype, which a page
// can't do to the isolated world), the other slots still go, and the count
// stays exact, so "Show original" knows what there is to undo.
p = page('<body><p id="a">eins</p><p id="b">zwei</p></body>');
p.extract();
const restoreClosest = p.w.Element.prototype.closest;
const bad = p.doc.getElementById('a');
p.w.Element.prototype.closest = function(sel) {
  if (this === bad) throw new Error('page code');
  return restoreClosest.call(this, sel);
};
assert.strictEqual(p.apply([[0, 'one'], [1, 'two']]).applied, 1);
p.w.Element.prototype.closest = restoreClosest;
assert.strictEqual(p.doc.getElementById('b').textContent, 'two');
assert.strictEqual(p.revert().reverted, 1);

// Unknown calls are refused.
p = page('<body><p>eins</p></body>');
assert.deepStrictEqual(p.raw({op: 'eval', id: 'a1'}), {v: 1, error: true});
assert.deepStrictEqual(p.raw(null), {v: 1, error: true});

console.log('page_translate_test: ok');
