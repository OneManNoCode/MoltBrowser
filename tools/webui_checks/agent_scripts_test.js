// The page scripts the web agent and the AI chat type and fill with
// (agent_actions::BuildTypeScript / BuildFillFormScript in
// src/chrome/browser/molt_ai/agents/agent_actions.cc), run in jsdom on a
// form with a text input, a textarea and a password field.
//
// The scripts are printf templates in C++; this reads them from the source
// and fills in their %s the way the C++ does (JSON strings, and for the
// fill script a JSON list of {sel, val}), so the test can't drift from the
// code.
//
// Run: npm ci && node agent_scripts_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {JSDOM} = require('jsdom');

const SRC = fs.readFileSync(path.join(
    __dirname, '../../src/chrome/browser/molt_ai/agents/agent_actions.cc'), 'utf8');

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

const ORIGIN = 'https://shop.example';
const typeScript = (origin, sel, text) => fill(template('BuildTypeScript'), [
  JSON.stringify(origin), JSON.stringify(sel), JSON.stringify(text), JSON.stringify(text)]);
const fillScript = (origin, fields) => fill(template('BuildFillFormScript'), [
  JSON.stringify(origin),
  JSON.stringify(fields.map(([sel, val]) => ({sel, val})))]);

function page() {
  const dom = new JSDOM(
      '<form><input id="mail" type="email"><textarea id="addr"></textarea>' +
      '<input id="pw" type="password"></form>',
      {url: ORIGIN + '/checkout', runScripts: 'outside-only'});
  const w = dom.window;
  const events = [];
  for (const id of ['mail', 'addr', 'pw']) {
    const el = w.document.getElementById(id);
    el.addEventListener('input', () => events.push(id + ':input'));
    el.addEventListener('change', () => events.push(id + ':change'));
  }
  return {run: (js) => w.eval(js), events,
          value: (id) => w.document.getElementById(id).value};
}

// TYPE: a text input and a textarea (each through its own value setter,
// as React-style pages need), never a password field or another origin.
let p = page();
assert.strictEqual(p.run(typeScript(ORIGIN, '#mail', 'ada@example.com')), 'typed');
assert.strictEqual(p.value('mail'), 'ada@example.com');
assert.deepStrictEqual(p.events, ['mail:input', 'mail:change']);
p = page();
assert.strictEqual(p.run(typeScript(ORIGIN, '#addr', '1 Main St\nApt 2')), 'typed',
                   'typing into a textarea');
assert.strictEqual(p.value('addr'), '1 Main St\nApt 2');
assert.deepStrictEqual(p.events, ['addr:input', 'addr:change']);
p = page();
assert.strictEqual(p.run(typeScript(ORIGIN, '#pw', 'hunter2')), 'refused_password');
assert.strictEqual(p.value('pw'), '');
p = page();
assert.strictEqual(p.run(typeScript('https://evil.example', '#mail', 'x')), 'origin_changed');
assert.strictEqual(p.run(typeScript(ORIGIN, '#gone', 'x')), 'not_found');
assert.strictEqual(p.value('mail'), '');

// FILL_FORM (/fill ai, /paste and the agent): the input and the textarea,
// never the password field; a missing element is skipped.
p = page();
assert.strictEqual(p.run(fillScript(ORIGIN, [
  ['#mail', 'ada@example.com'], ['#addr', '1 Main St\nApt 2'], ['#pw', 'hunter2'],
  ['#gone', 'x']])), 2);
assert.strictEqual(p.value('mail'), 'ada@example.com');
assert.strictEqual(p.value('addr'), '1 Main St\nApt 2');
assert.strictEqual(p.value('pw'), '', 'never a password field');
p = page();
assert.strictEqual(p.run(fillScript('https://evil.example', [['#mail', 'x']])),
                   'origin_changed');
assert.strictEqual(p.value('mail'), '');
// Hostile values stay values.
p = page();
const hostile = '"); alert(1); ("</script> ';
assert.strictEqual(p.run(fillScript(ORIGIN, [['#addr', hostile]])), 1);
assert.strictEqual(p.value('addr'), hostile);

console.log('agent page scripts: type and fill (input, textarea, password, origin) ok');
