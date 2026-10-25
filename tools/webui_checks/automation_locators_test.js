// The fallback locators' page scripts (the library in
// src/chrome/browser/molt_ai/automation/automation_locators.cc: capture at
// record time, the probe and the pin at replay), run in jsdom.
//
// The library and the scripts' printf templates are read from the C++ source
// and filled in the way the C++ does (JSON values), so the test can't drift
// from the code. In the browser they run in an isolated world; here they
// share the page's world, so the test covers their logic only. jsdom has no
// layout: getBoundingClientRect() is stubbed (an element with data-hidden,
// or inside one, has no box), and innerText falls back to textContent.
//
// Run: npm ci && node automation_locators_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {JSDOM} = require('jsdom');

const SRC = fs.readFileSync(path.join(
    __dirname,
    '../../src/chrome/browser/molt_ai/automation/automation_locators.cc'),
    'utf8');

const LIBRARY = (() => {
  const m = /kLibraryJs\[\] = R"JS\(([\s\S]*?)\)JS"/.exec(SRC);
  assert(m, 'kLibraryJs not found');
  return m[1];
})();

// The R"JS(...)JS" template inside the C++ function |name|.
function template(name) {
  const at = SRC.indexOf('std::string ' + name + '(');
  assert(at >= 0, name + ' not found');
  const m = /R"JS\(([\s\S]*?)\)JS"/.exec(SRC.slice(at));
  assert(m, name + ': no R"JS( template');
  return m[1];
}

function fill(tpl, args) {
  let i = 0;
  const out = tpl.replace(/%s/g, () => args[i++]);
  assert.strictEqual(i, args.length, 'argument count');
  return out;
}

// As the C++ builds them.
const captureScript = (selector) =>
    LIBRARY + fill(template('BuildCaptureScript'), [JSON.stringify(selector)]);
const captureListScript = (selectors) =>
    LIBRARY + fill(template('BuildCaptureListScript'), [JSON.stringify(selectors)]);
const probeScript = (id, order, hint) =>
    LIBRARY + fill(template('BuildProbeScript'), [
      JSON.stringify(id),
      JSON.stringify(order.map((l) => ({by: l.by, role: l.role || '',
                                        name: l.name || '', value: l.value}))),
      JSON.stringify(hint || '')]);
const pinScript = (id, index, token) =>
    fill(template('BuildPinScript'),
         [JSON.stringify(id), String(index),
          JSON.stringify(/^[A-Za-z0-9]{1,64}$/.test(token) ? token : '')]);
// PinnedCheck(token), as the runner's click script embeds it.
const pinnedCheck = (token) =>
    '(globalThis.__moltLocate && globalThis.__moltLocate.token === ' +
    JSON.stringify(token) + ' && globalThis.__moltLocate.pinned === el)';

function page(html, url = 'https://shop.example/cart') {
  const dom = new JSDOM(html, {url, runScripts: 'outside-only'});
  const w = dom.window;
  w.Element.prototype.getBoundingClientRect = function() {
    const hidden = this.closest && this.closest('[data-hidden]');
    return hidden ? {width: 0, height: 0, left: 0, top: 0}
                  : {width: 10, height: 10, left: 0, top: 0};
  };
  return {
    w, doc: w.document,
    run: (js) => JSON.parse(JSON.stringify(w.eval(js))),
  };
}

const STORE = `
  <div id="main">
    <h1>Cart</h1>
    <div class="row">
      <button data-testid="add-btn" id="ember1234" type="button">Add to cart</button>
      <a href="/help">Help</a>
    </div>
    <form>
      <label for="em">Email</label>
      <input id="em" name="email" type="email" autocomplete="email">
      <label>Password <input name="pw" type="password"></label>
      <input name="code" autocomplete="one-time-code" aria-label="Code">
      <button type="submit">Pay now</button>
    </form>
  </div>`;

// ---- Capture ------------------------------------------------------------

{
  const p = page(STORE);
  const r = p.run(captureScript('[data-testid="add-btn"]'));
  assert.strictEqual(r.origin, 'https://shop.example');
  assert.deepStrictEqual([r.tag, r.type, r.role, r.input_type],
                         ['button', '', 'button', '']);
  assert.deepStrictEqual(r.list, [
    {by: 'role', role: 'button', value: 'Add to cart'},
    // A generated-looking id (digits) is no identity.
    {by: 'attr', name: 'data-testid', value: 'add-btn'},
    {by: 'text', value: 'Add to cart'},
    {by: 'path', value: '#main > div:nth-of-type(1) > button:nth-of-type(1)'},
  ]);
  assert.strictEqual(p.run(captureScript('#nothing')), null);
  assert.strictEqual(p.run(captureScript('!!bad selector')), null);
}

// A field: its label, never what is typed in it.
{
  const p = page(STORE);
  const em = p.doc.getElementById('em');
  em.value = 'typed-secret@example.com';
  const r = p.run(captureScript('#em'));
  assert.deepStrictEqual([r.tag, r.type, r.role, r.input_type, r.autocomplete],
                         ['input', 'text', 'textbox', 'email', 'email']);
  assert.deepStrictEqual(r.list, [
    {by: 'role', role: 'textbox', value: 'Email'},
    {by: 'label', value: 'Email'},
    {by: 'attr', name: 'id', value: 'em'},
    {by: 'attr', name: 'name', value: 'email'},
    {by: 'path', value: '#main > form:nth-of-type(1) > input:nth-of-type(1)'},
  ]);
  assert(!JSON.stringify(r).includes('typed-secret'), 'the value is never read');
  // A password field reports its type; the C++ classifies it.
  p.doc.querySelector('[name=pw]').value = 'hunter2';
  const pw = p.run(captureScript('[name=pw]'));
  assert.strictEqual(pw.input_type, 'password');
  assert.strictEqual(pw.type, 'password');
  assert.deepStrictEqual(pw.list[0], {by: 'label', value: 'Password'});
  assert(!JSON.stringify(pw).includes('hunter2'));
  const code = p.run(captureScript('[name=code]'));
  assert.strictEqual(code.autocomplete, 'one-time-code');
}

// Several fields at once (a form fill), in order; a miss or a bad selector
// is null.
{
  const p = page(STORE);
  const r = p.run(captureListScript(['#em', '#none', '!!', '[name=pw]']));
  assert.strictEqual(r.length, 4);
  assert.strictEqual(r[0].list[1].value, 'Email');
  assert.strictEqual(r[1], null);
  assert.strictEqual(r[2], null);
  assert.strictEqual(r[3].input_type, 'password');
}

// Page text over the cap is dropped, not cut; hostile text stays data.
{
  const long = 'x'.repeat(121);
  const hostile = '"]); alert(1); //';
  const p = page(`<div id="box"><button name="${long}" data-qa='${hostile}'
      title="t">${long}</button></div>`);
  const r = p.run(captureScript('button'));
  assert(!r.list.some((l) => l.value === long), 'over the cap: dropped');
  assert(r.list.some((l) => l.name === 'data-qa' && l.value === hostile));
  assert(r.list.some((l) => l.name === 'title' && l.value === 't'));
  assert(r.list.length <= 8);
}

// A path is anchored at an id or <html>, or not kept.
{
  let html = '<section>';
  for (let i = 0; i < 20; i++) html += '<div>';
  html += '<button>Deep</button>';
  for (let i = 0; i < 20; i++) html += '</div>';
  html += '</section>';
  const r = page(html).run(captureScript('button'));
  assert(!r.list.some((l) => l.by === 'path'), 'too deep to anchor');
  const shallow = page('<p><a href="/x">Go</a></p>').run(captureScript('a'));
  assert.deepStrictEqual(shallow.list.find((l) => l.by === 'path'),
                         {by: 'path', value: 'html > body:nth-of-type(1) > ' +
                                             'p:nth-of-type(1) > a:nth-of-type(1)'});
}

// ---- Probe --------------------------------------------------------------

// The page changed: the test id is gone and the button moved, but the role
// and name still find it; the old path finds the link that took its place.
{
  const recorded = page(STORE).run(captureScript('[data-testid="add-btn"]'));
  const changed = page(`
    <div id="main"><h1>Cart</h1>
      <div class="row"><a href="/help">Help</a>
        <button type="button">Add to cart</button></div></div>`);
  const order = [{by: 'selector', value: '[data-testid="add-btn"]'}]
      .concat(recorded.list);
  const r = changed.run(probeScript('p1', order, 'Add to cart'));
  assert.strictEqual(r.origin, 'https://shop.example');
  assert.deepStrictEqual(r.matches.map((m) => m.length), [0, 1, 0, 1, 1]);
  const button = r.matches[1][0];
  assert.deepStrictEqual(r.candidates[button],
      {tag: 'button', type: '', role: 'button', input_type: '',
       autocomplete: '', label: 'Add to cart'});
  assert.strictEqual(r.matches[3][0], button, 'its text');
  // The path names each step's tag: it still finds the button.
  assert.strictEqual(r.matches[4][0], button, 'its path');
  assert.strictEqual(r.candidates.length, 1);
  assert.deepStrictEqual(r.over, [false, false, false, false, false]);
}

// The recorded selector must agree with the step's text anchor.
{
  const p = page('<button id="b">Delete account</button>');
  let r = p.run(probeScript('p2', [{by: 'selector', value: '#b'}], 'Save'));
  assert.deepStrictEqual(r.matches, [[]]);
  r = p.run(probeScript('p2', [{by: 'selector', value: '#b'}], 'delete'));
  assert.deepStrictEqual(r.matches, [[0]]);
  r = p.run(probeScript('p2', [{by: 'selector', value: '#b'}], ''));
  assert.deepStrictEqual(r.matches, [[0]]);
  // A selector that isn't one finds nothing.
  r = p.run(probeScript('p2', [{by: 'selector', value: '<<'}], ''));
  assert.deepStrictEqual(r.matches, [[]]);
}

// Hidden elements don't count; labels, attributes and roles match exactly
// (case and spaces aside for text), attribute values are compared as data.
{
  const hostile = '"] , * , [x="';
  const p = page(`
    <button data-hidden>Pay</button>
    <button data-cy='${hostile}'>Pay</button>
    <div role="button" aria-label="  pay ">x</div>
    <label for="q">Search   terms</label><input id="q">`);
  const r = p.run(probeScript('p3', [
    {by: 'text', value: 'pay'},
    {by: 'attr', name: 'data-cy', value: hostile},
    {by: 'attr', name: 'onclick', value: 'x'},
    {by: 'role', role: 'button', value: 'Pay'},
    {by: 'label', value: 'search terms'},
    {by: 'role', role: 'b"]', value: 'x'},
  ], ''));
  assert.strictEqual(r.matches[0].length, 1, 'the hidden one is skipped');
  assert.strictEqual(r.matches[1].length, 1);
  assert.deepStrictEqual(r.matches[2], [], 'an attribute not on the list');
  assert.strictEqual(r.matches[3].length, 2, 'both buttons named Pay');
  assert.strictEqual(r.matches[4].length, 1);
  assert.deepStrictEqual(r.matches[5], [], 'a role that is not one');
}

// Too many matches, or no room left in the candidate table, flag the
// locator: it then identifies nothing.
{
  let html = '';
  for (let i = 0; i < 40; i++) html += '<button>Same</button>';
  html += '<button>Other</button>';
  const p = page(html);
  const r = p.run(probeScript('p4', [{by: 'text', value: 'same'},
                                     {by: 'text', value: 'other'}], ''));
  assert.strictEqual(r.matches[0].length, 8);
  assert.deepStrictEqual(r.over, [true, false]);
  let html2 = '';
  for (let i = 0; i < 40; i++) html2 += `<button name="b${i}">B</button>`;
  const order = [];
  for (let i = 0; i < 40; i++) order.push({by: 'attr', name: 'name', value: 'b' + i});
  const r2 = page(html2).run(probeScript('p5', order, ''));
  assert(r2.candidates.length <= 32);
  assert.strictEqual(r2.over[31], false);
  assert.strictEqual(r2.over[32], true, 'no room: flagged');
  assert.deepStrictEqual(r2.matches[32], []);
}

// ---- Pin ----------------------------------------------------------------

{
  const p = page('<button>A</button><button>B</button>');
  p.run(probeScript('p6', [{by: 'text', value: 'b'}], ''));
  assert.strictEqual(p.run(pinScript('p6', 0, 'Tok123')),
                     '[data-molt-pick="Tok123"]');
  const b = p.doc.querySelectorAll('button')[1];
  assert.strictEqual(b.getAttribute('data-molt-pick'), 'Tok123');
  // The runner's scripts accept only the pinned element itself: a decoy
  // the page gave the same attribute is refused.
  const check = (el) => p.w.eval('(function(el){return ' + pinnedCheck('Tok123') +
                                 ';})')(el);
  assert.strictEqual(check(b), true);
  const a = p.doc.querySelectorAll('button')[0];
  a.setAttribute('data-molt-pick', 'Tok123');
  assert.strictEqual(check(a), false);
  // Another probe's id, an index out of range, a bad token: nothing.
  assert.strictEqual(p.run(pinScript('other', 0, 'Tok123')), '');
  assert.strictEqual(p.run(pinScript('p6', 5, 'Tok123')), '');
  assert.strictEqual(p.run(pinScript('p6', 0, 'a"b')), '');
  // A new pin clears the old marks.
  p.run(pinScript('p6', 0, 'Tok456'));
  assert.deepStrictEqual(
      Array.from(p.doc.querySelectorAll('[data-molt-pick]')).map(
          (e) => e.getAttribute('data-molt-pick')),
      ['Tok456']);
  // A detached element isn't pinned.
  b.remove();
  assert.strictEqual(p.run(pinScript('p6', 0, 'Tok789')), '');
}

// The library can run twice in one world (a var, not a const).
{
  const p = page('<button>A</button>');
  p.run(captureScript('button'));
  p.run(captureScript('button'));
  p.run(probeScript('p7', [{by: 'text', value: 'a'}], ''));
}

console.log('automation locators: capture (roles, labels, attributes, ' +
            'text, paths; values never read), probe, pin ok');
