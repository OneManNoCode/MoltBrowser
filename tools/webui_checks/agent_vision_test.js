// The web agent's screenshot geometry script (agent_vision::
// BuildGeometryScript in src/chrome/browser/molt_ai/agents/agent_vision.cc),
// run in jsdom: it reports how many frame elements it found, in the
// document and its open shadow roots, in view or not. The browser compares
// that with the frames the document owns (FoundEveryFrame()) and takes no
// screenshot when the script missed one, as it must miss one inside a
// closed shadow root (which it can't read, so it can't mask).
//
// jsdom has no layout (every box is empty), so this covers the counting
// and the result's shape, not the masks' geometry.
//
// Run: npm ci && node agent_vision_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {JSDOM} = require('jsdom');

const SRC = fs.readFileSync(path.join(
    __dirname, '../../src/chrome/browser/molt_ai/agents/agent_vision.cc'), 'utf8');

// The R"JS(...)JS" template inside the C++ function |name|.
function template(name) {
  const at = SRC.indexOf('std::string ' + name + '(');
  assert(at >= 0, name + ' not found');
  const m = /R"JS\(([\s\S]*?)\)JS"/.exec(SRC.slice(at));
  assert(m, name + ': no R"JS( template');
  return m[1];
}

// printf's %s, in order.
function fill(tpl, args) {
  let i = 0;
  const out = tpl.replace(/%s/g, () => args[i++]);
  assert.strictEqual(i, args.length, 'argument count');
  return out;
}

const geometryScript = (selectors) =>
    fill(template('BuildGeometryScript'), [JSON.stringify(selectors)]);

function run(html, setup) {
  const dom = new JSDOM(html, {url: 'https://shop.example/checkout',
                               runScripts: 'outside-only'});
  if (setup) setup(dom.window);
  // As the browser gets it: JSON values, not the page's objects.
  return JSON.parse(JSON.stringify(dom.window.eval(geometryScript(['#buy']))));
}

// Frames anywhere in the document, an open shadow root included, and
// out of view (jsdom lays nothing out).
let r = run('<button id="buy">Buy</button>' +
            '<iframe src="about:blank"></iframe>' +
            '<object data="x.png"></object><embed src="x.pdf">' +
            '<div id="host"></div>',
            (w) => {
              // (The parser drops a <frame> outside a frameset.)
              w.document.body.appendChild(w.document.createElement('frame'));
              const root = w.document.getElementById('host')
                               .attachShadow({mode: 'open'});
              root.innerHTML = '<p><iframe src="about:blank"></iframe></p>';
            });
assert.strictEqual(r.over, false);
assert.strictEqual(r.origin, 'https://shop.example');
assert.strictEqual(r.frames, 4 + 1, 'document and open shadow root frames');
assert.deepStrictEqual(r.masks, []);
assert.deepStrictEqual(r.boxes, [null]);

// A frame inside a closed shadow root can't be found: the count leaves it
// out, which is what the browser's comparison catches.
r = run('<div id="host"></div><iframe src="about:blank"></iframe>', (w) => {
  const root = w.document.getElementById('host').attachShadow({mode: 'closed'});
  root.innerHTML = '<iframe src="about:blank"></iframe>';
});
assert.strictEqual(r.over, false);
assert.strictEqual(r.frames, 1, 'the closed shadow root is not walked');

// No frames.
r = run('<p>text</p>');
assert.strictEqual(r.frames, 0);

console.log('agent vision geometry: frames counted, closed shadow roots ' +
            'left to the browser ok');
