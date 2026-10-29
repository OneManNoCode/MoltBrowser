// Regression test for the AI chat markdown renderer (security).
//
// The chat page is a privileged WebUI, and renderMarkdown() turns model
// output (which pages, PDFs and attachments can influence) into innerHTML.
// Its output is passed to moltHTML(), the page's pass-through Trusted
// Types policy (molt_webui_csp.h), so this test is what backs that
// policy: it pulls the shipped renderMarkdown() out of the C++ header,
// fuzzes it, and parses every result with an HTML5 parser to make sure it
// only ever emits the tags and attributes the renderer itself writes, and
// only http(s) links. The copy button carries no inline handler (the CSP
// runs none): data-on-click must name the registered copyCode action.
// molt://ai (molt_ai_ui.cc) has its own, older renderer; it is fuzzed the
// same way at the end.
//
// Run: npm ci && npm test   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const parse5 = require('parse5');

const HEADER = path.join(__dirname, '../../src/chrome/browser/ui/webui/molt_ai/molt_ai_chat_ui_js.h');
const src = fs.readFileSync(HEADER, 'utf8');
const from = src.indexOf('function isSafeHttpUrl(u)');
const to = src.indexOf('function setStatus(state, text) {');
assert(from > 0 && to > from, 'renderMarkdown() not found in ' + HEADER);

// The page's esc() serializes a text node: it escapes & < > and U+00A0.
const esc = (t) => String(t).replace(/&/g, '&amp;').replace(/</g, '&lt;')
                            .replace(/>/g, '&gt;').replace(/ /g, '&nbsp;');
const renderMarkdown = new Function('esc',
    'var codeBlockId = 0;\n' + src.slice(from, to) + '\nreturn renderMarkdown;')(esc);

const ALLOWED = {
  a: ['href', 'class', 'data-molt-link'],
  button: ['class', 'data-on-click', 'data-copy-target', 'aria-label'],
  pre: ['id', 'style'], code: ['style'], span: ['style', 'aria-hidden'],
  div: ['style', 'class', 'role', 'aria-level'],
  strong: ['style'], em: [], br: [],
};
function violations(html, allowedTags) {
  const table = allowedTags || ALLOWED;
  const bad = [];
  (function walk(n) {
    if (n.tagName) {
      const allowed = table[n.tagName];
      if (!allowed) bad.push('tag <' + n.tagName + '>');
      for (const at of n.attrs || []) {
        if (!allowed || !allowed.includes(at.name)) bad.push(n.tagName + ' attr ' + JSON.stringify(at.name));
        if (at.name === 'href' && !/^https?:\/\//i.test(at.value)) bad.push('href ' + at.value);
        if (at.name === 'data-on-click' && at.value !== 'copyCode') bad.push('data-on-click ' + at.value);
        if (at.name === 'data-copy-target' && !/^cb-\d+$/.test(at.value)) bad.push('data-copy-target ' + at.value);
        if (at.name === 'class' && !/^(code-wrap|code-copy|chat-link)$/.test(at.value)) bad.push('class ' + at.value);
        // Accessibility attributes: the renderer's own constants only.
        if (at.name === 'role' && at.value !== 'heading') bad.push('role ' + at.value);
        if (at.name === 'aria-level' && !/^[345]$/.test(at.value)) bad.push('aria-level ' + at.value);
        if (at.name === 'aria-hidden' && at.value !== 'true') bad.push('aria-hidden ' + at.value);
        if (at.name === 'aria-label' && at.value !== 'Copy code') bad.push('aria-label ' + at.value);
      }
    }
    (n.childNodes || []).forEach(walk);
  })(parse5.parseFragment(html));
  return bad;
}

// 1. Behaviour still works.
const cases = [
  ['see [docs](https://ex.com/a) now', 'see <a href="https://ex.com/a" class="chat-link" data-molt-link="1">docs</a> now'],
  ['go to https://ex.com/x.', 'go to <a href="https://ex.com/x" class="chat-link" data-molt-link="1">https://ex.com/x</a>.'],
  ['[x](javascript:alert(1))', '[x](javascript:alert(1))'],
];
for (const [input, want] of cases) assert.strictEqual(renderMarkdown(input), want, 'render ' + JSON.stringify(input));
assert(/<strong[^>]*>hi<\/strong> and <em>em<\/em>/.test(renderMarkdown('**hi** and *em*')), 'bold/italic');
assert(!/<a /.test(renderMarkdown('`https://in.code`')), 'urls inside code are not linked');
assert(/<pre[^>]*><code>a\nb<\/code><\/pre>/.test(renderMarkdown('```\na\nb\n```')), 'code block keeps newlines');

// 2. Fuzz: random mixes of the syntax that interacts across passes.
const T = ['https://a.co/', 'http://x.y/p?q=1&r=2', '**', '*', '"', "'", '[t](', '](', ')', ']', '[', '`', '```',
  '```js\n', 'onmouseover=alert(1)', '/', ' ', '\n', '# ', '- ', '1. ', '=', '<', '>', '&', '\u0000',
  'javascript:x', 'href=', ' ', 'x'];
let seed = Number(process.env.FUZZ_SEED || 20261014);
const rnd = (n) => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed % n; };
const N = Number(process.env.FUZZ_N || 60000);
for (let i = 0; i < N; i++) {
  let input = '';
  const len = 2 + rnd(24);
  for (let j = 0; j < len; j++) input += T[rnd(T.length)];
  const bad = violations(renderMarkdown(input));
  if (bad.length) {
    console.error('Disallowed markup for input ' + JSON.stringify(input) + ':\n  ' + bad.join('\n  '));
    process.exit(1);
  }
}
console.log('markdown renderer: behaviour checks passed; ' + N + ' fuzz inputs produced only allowed markup');

// molt://ai full-page chat renderer (molt_ai_ui.cc): same treatment.
{
  const UI = path.join(__dirname, '../../src/chrome/browser/ui/webui/molt_ai/molt_ai_ui.cc');
  const usrc = fs.readFileSync(UI, 'utf8');
  const ufrom = usrc.indexOf('function renderMarkdown(text) {');
  const uto = usrc.indexOf('function setStatus(cls, text) {');
  assert(ufrom > 0 && uto > ufrom, 'renderMarkdown() not found in ' + UI);
  // That page's esc(): text-node serialisation plus quote escaping.
  const uesc = (t) => esc(t).replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  const uRender = new Function('esc',
      'var codeBlockId = 0;\n' + usrc.slice(ufrom, uto) + '\nreturn renderMarkdown;')(uesc);
  const UALLOWED = Object.assign({}, ALLOWED);
  delete UALLOWED.a;  // this renderer makes no links
  assert(/<strong[^>]*>hi<\/strong>/.test(uRender('**hi**')), 'molt://ai bold');
  assert(/data-on-click="copyCode"/.test(uRender('```\nx\n```')), 'molt://ai copy button');
  for (let i = 0; i < N; i++) {
    let input = '';
    const len = 2 + rnd(24);
    for (let j = 0; j < len; j++) input += T[rnd(T.length)];
    const bad = violations(uRender(input), UALLOWED);
    if (bad.length) {
      console.error('molt://ai renderer: disallowed markup for input ' + JSON.stringify(input) +
                    ':\n  ' + bad.join('\n  '));
      process.exit(1);
    }
  }
  console.log('molt://ai renderer: ' + N + ' fuzz inputs produced only allowed markup');
}
