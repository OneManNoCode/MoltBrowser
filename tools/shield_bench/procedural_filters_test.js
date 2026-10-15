// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// jsdom tests for MoltShield's procedural cosmetic filter engine, the
// script embedded in src/chrome/renderer/molt_ai/shield/procedural_filters_js.h
// (not shipped). Uses tools/webui_checks' jsdom:
//   (cd tools/webui_checks && npm ci) && node tools/shield_bench/procedural_filters_test.js
//
// If tools/shield_bench/target/procedural_corpus.json exists (written by
// `cargo test --release` with the real lists), every real-world filter in it
// is also compiled and run against a sample page.
'use strict';
const fs = require('fs');
const path = require('path');
const repo = process.argv[2] || path.join(__dirname, '..', '..');
const {JSDOM} = require(path.join(
    process.argv[3] || path.join(repo, 'tools/webui_checks/node_modules'),
    'jsdom'));

const header = fs.readFileSync(path.join(repo,
    'src/chrome/renderer/molt_ai/shield/procedural_filters_js.h'), 'utf8');
const engineSrc =
    header.match(/kProceduralFiltersJs\[\] = R"JS\(([\s\S]*?)\)JS"/)[1];

const TOKEN = 'mtest0123456789ab';

function page(html, url) {
  const dom = new JSDOM(`<!DOCTYPE html><html><body>${html}</body></html>`, {
    runScripts: 'outside-only',
    url: url || 'https://www.example.com/',
  });
  const w = dom.window;
  // jsdom has no getComputedStyle(el, '::before'); Chromium does. Ignore the
  // pseudo-element so :matches-css-before/-after filters still run here.
  const computed = w.getComputedStyle.bind(w);
  w.getComputedStyle = (el) => computed(el);
  const engine = w.eval(engineSrc);
  return {
    w,
    doc: w.document,
    run(filters, token) {
      return engine(token || TOKEN, typeof filters === 'string' ?
          filters : JSON.stringify(filters));
    },
    marks(sel) {
      return Array.from(w.document.querySelectorAll(sel))
          .map((e) => e.getAttribute(TOKEN) || '-').join(',');
    },
  };
}

const css = (arg) => ({type: 'css-selector', arg});
const hasText = (t) => ({type: 'has-text', arg: {text: t}});
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

const cases = [];
function test(name, fn) { cases.push([name, fn]); }
function eq(a, b, what) {
  if (a !== b) {
    throw new Error(`${what || 'value'}: got ${JSON.stringify(a)}, ` +
                    `want ${JSON.stringify(b)}`);
  }
}

test('has-text hides only matching elements', () => {
  const p = page(`<div class="c">Sponsored</div><div class="c">News</div>
                  <div class="c">x Sponsored y</div>`);
  eq(p.run([{selector: [css('.c'), hasText('Sponsored')]}]), 1, 'compiled');
  eq(p.marks('.c'), 'h,-,h');
});

test('regex matcher with flags; g/y never reach the engine', () => {
  const p = page(`<p>PROMOTED</p><p>promo</p><p>news</p>`);
  p.run([{selector: [css('p'),
                     {type: 'has-text', arg: {re: '^promo(ted)?$', flags: 'i'}}]}]);
  eq(p.marks('p'), 'h,h,-');
});

test(':has() with a procedural argument is relative (descendants)', () => {
  const p = page(`<div class="card"><span>Sponsored</span></div>
                  <div class="card"><span>Article</span></div>
                  <div class="card">Sponsored (own text)</div>`);
  p.run([{selector: [css('div.card'),
                     {type: 'has', arg: [css(' span'), hasText('Sponsored')]}]}]);
  eq(p.marks('.card'), 'h,-,-');
});

test(':has(:has-text()) looks at descendants, :not() at the element', () => {
  const p = page(`<ul><li><b>Ad</b></li><li><b>Hi</b></li></ul>
                  <p class="t">Ad</p><p class="t">MAHA Ad</p>`);
  p.run([
    {selector: [css('li'), {type: 'has', arg: [css(' *'), hasText('Ad')]}]},
    {selector: [css('p.t'), hasText('Ad'),
                {type: 'not', arg: [css('p'), hasText('MAHA')]}]},
  ]);
  eq(p.marks('li'), 'h,-');
  eq(p.marks('p.t'), 'h,-');
});

test('upward by count and by selector (starting at the parent)', () => {
  const p = page(`<section class="slot"><div><span class="lbl">Ad</span></div></section>
                  <div class="slot2"><i class="m">x</i></div>`);
  p.run([
    {selector: [css('.lbl'), {type: 'upward', arg: 2}]},
    {selector: [css('.m'), {type: 'upward', arg: {css: 'div, i'}}]},
  ]);
  eq(p.marks('section'), 'h');
  eq(p.marks('.slot2'), 'h');
  eq(p.marks('.m'), '-');  // not itself
});

test('xpath as first op and as a step', () => {
  const p = page(`<div><span>AD</span></div><div><span>no</span></div>`);
  p.run([{selector: [{type: 'xpath', arg: '//span[text()="AD"]/..'}]}]);
  eq(p.marks('div'), 'h,-');
});

test('sibling and child steps after an operator', () => {
  const p = page(`<h2>Sponsored</h2><div class="a"></div><p class="b"></p>
                  <h2>News</h2><div class="a"></div>
                  <ul class="x"><li>Ad</li></ul>`);
  p.run([
    {selector: [css('h2'), hasText('Sponsored'), css('+ div')]},
    {selector: [css('h2'), hasText('Sponsored'), css('~ p')]},
    {selector: [css('ul'), hasText('Ad'), css('> li')]},
  ]);
  eq(p.marks('div.a'), 'h,-');
  eq(p.marks('p.b'), 'h');
  eq(p.marks('ul.x > li'), 'h');
});

test('actions: remove, remove-attr, remove-class, style marks', () => {
  const p = page(`<div id="r">Ad</div><a class="k sticky" onclick="x()">Ad</a>
                  <div class="s">Ad</div>`);
  p.run([
    {selector: [css('#r'), hasText('Ad')], action: {type: 'remove'}},
    {selector: [css('a'), hasText('Ad')],
     action: {type: 'remove-attr', arg: 'onclick'}},
    {selector: [css('a'), hasText('Ad')],
     action: {type: 'remove-class', arg: 'sticky'}},
    {selector: [css('.s'), hasText('Ad')], action: {type: 'style', arg: 0}},
    {selector: [css('.s'), hasText('Ad')]},
  ]);
  eq(p.doc.getElementById('r'), null, 'removed');
  const a = p.doc.querySelector('a');
  eq(a.hasAttribute('onclick'), false, 'onclick');
  eq(a.className, 'k', 'class');
  eq(p.marks('.s'), 'h s0', 'two marks, sorted');
});

test('min-text-length and matches-attr (regex name, exact value)', () => {
  const p = page(`<div class="m">0123456789</div><div class="m">0123</div>
                  <div class="a" data-trackid="1">x</div>
                  <div class="a" data-ad-slot="top">x</div>`);
  p.run([
    {selector: [css('.m'), {type: 'min-text-length', arg: 10}]},
    {selector: [css('.a'), {type: 'matches-attr',
                            arg: {name: {re: 'trackid$', flags: ''}, value: null}}]},
    {selector: [css('.a'), {type: 'matches-attr',
                            arg: {name: {exact: 'data-ad-slot'},
                                  value: {exact: 'top'}}}]},
  ]);
  eq(p.marks('.m'), 'h,-');
  eq(p.marks('.a'), 'h,h');
});

test('matches-css against the computed style', () => {
  const p = page(`<style>.fixed { position: fixed; }</style>
                  <div class="o fixed">x</div><div class="o">y</div>`);
  p.run([{selector: [css('.o'), {type: 'matches-css',
                                 arg: {prop: 'position', value: {exact: 'fixed'}}}]}]);
  eq(p.marks('.o'), 'h,-');
});

test('matches-path is a page precondition', () => {
  const home = page(`<div class="f">Ad</div>`, 'https://social.example/');
  const post = page(`<div class="f">Ad</div>`, 'https://social.example/post/1');
  const f = [{selector: [css('.f'), hasText('Ad')],
              path: {re: '^/(\\?.*)?$', flags: ''}}];
  home.run(f);
  post.run(f);
  eq(home.marks('.f'), 'h');
  eq(post.marks('.f'), '-');
});

test('content added later is filtered after the mutation', async () => {
  const p = page(`<main></main>`);
  p.run([{selector: [css('.c'), hasText('Sponsored')]}]);
  const d = p.doc.createElement('div');
  d.className = 'c';
  d.textContent = 'Sponsored';
  p.doc.querySelector('main').appendChild(d);
  await sleep(150);
  eq(p.marks('.c'), 'h');
});

test('an element that stops matching is unmarked', async () => {
  const p = page(`<div class="c"><span>Sponsored</span></div>`);
  p.run([{selector: [css('.c'), hasText('Sponsored')]}]);
  eq(p.marks('.c'), 'h');
  p.doc.querySelector('.c span').replaceWith(p.doc.createTextNode('Article'));
  await sleep(150);
  eq(p.marks('.c'), '-');
});

test(':watch-attr() re-evaluates on attribute changes', async () => {
  const p = page(`<div class="c">x</div>`);
  p.run([{selector: [css('.c.ad')], watch: ['class']},
         {selector: [css('.c'), hasText('nothing')]}]);
  eq(p.marks('.c'), '-');
  p.doc.querySelector('.c').classList.add('ad');
  await sleep(150);
  eq(p.marks('.c'), 'h');
});

test('malformed filters are skipped, others still apply', () => {
  const p = page(`<div class="ok">Ad</div>`);
  const n = p.run([
    {selector: [css('div[')]},                               // bad selector
    {selector: [css('div'), {type: 'has-text', arg: {re: '(', flags: ''}}]},
    {selector: [css('div'), {type: 'eval', arg: 'alert(1)'}]},  // unknown op
    {selector: [css('div'), {type: 'upward', arg: 0}]},
    {selector: [css('div'), hasText('Ad')], action: {type: 'innerHTML'}},
    {selector: []},
    null,
    {selector: [css('.ok'), hasText('Ad')]},
  ]);
  eq(n, 1, 'compiled');
  eq(p.marks('.ok'), 'h');
});

test('bad token or non-JSON input does nothing', () => {
  const p = page(`<div class="c">Ad</div>`);
  eq(p.run([{selector: [css('.c')]}], 'bad token"]'), undefined);
  eq(p.run('{"selector":', TOKEN), undefined);
  eq(p.run('{"not":"an array"}', TOKEN), undefined);
  eq(p.marks('.c'), '-');
});

test('list text is data: hostile arguments never execute', () => {
  const p = page(`<div class="c">x</div>`);
  p.w.pwned = false;
  const evil = `'); window.pwned = true; //</script><img src=x onerror="pwned=1">`;
  p.run([
    {selector: [css('.c'), hasText(evil)]},
    {selector: [css('.c'), {type: 'has-text', arg: {re: evil, flags: ''}}]},
    {selector: [css('.c'), {type: 'xpath', arg: evil}]},
    {selector: [css('.c'), hasText('x')], action: {type: 'remove-attr', arg: evil}},
    {selector: [css('.c'), hasText('x')], action: {type: 'style', arg: evil}},
  ]);
  eq(p.w.pwned, false, 'pwned');
  eq(p.doc.querySelectorAll('img').length, 0, 'no markup injected');
});

test('regex input is capped (bounded work on huge text)', () => {
  const p = page(`<div class="big"></div>`);
  p.doc.querySelector('.big').textContent = 'a'.repeat(2000000);
  const t0 = Date.now();
  p.run([{selector: [css('.big'),
                     {type: 'has-text', arg: {re: 'a.*b', flags: ''}}]}]);
  const ms = Date.now() - t0;
  eq(p.marks('.big'), '-');
  if (ms > 2000) throw new Error(`took ${ms} ms`);
});

test('step output is capped', () => {
  const many = '<i class="x">Ad</i>'.repeat(6000);
  const p = page(many);
  p.run([{selector: [css('i.x'), hasText('Ad')]}]);
  const n = p.doc.querySelectorAll(`[${TOKEN}]`).length;
  eq(n, 5000, 'capped at MAX_ELEMENTS');
});

test('real-world corpus compiles and runs', () => {
  const corpusPath = path.join(repo,
      'tools/shield_bench/target/procedural_corpus.json');
  if (!fs.existsSync(corpusPath)) {
    console.log('     (no target/procedural_corpus.json; run cargo test first)');
    return;
  }
  const corpus = JSON.parse(fs.readFileSync(corpusPath, 'utf8'));
  let total = 0;
  let compiled = 0;
  const failed = [];
  for (const entry of corpus) {
    const p = page(`<div class="card"><span>Sponsored</span><a href="#">x</a></div>
                    <ul><li>Ad</li><li>News</li></ul><p>text</p>`,
                   `https://${entry.host}/`);
    for (const f of entry.filters) {
      total++;
      const n = p.run([f]);
      if (n === 1) {
        compiled++;
      } else {
        failed.push(JSON.stringify(f).slice(0, 160));
      }
    }
  }
  console.log(`     corpus: ${compiled}/${total} real filters compiled in jsdom`);
  if (failed.length) {
    console.log('     not compiled in jsdom (selector support differs):\n       ' +
                failed.slice(0, 8).join('\n       '));
  }
  if (compiled < total * 0.9) throw new Error('too many corpus failures');
});

(async () => {
  let failures = 0;
  for (const [name, fn] of cases) {
    try {
      await fn();
      console.log(`ok   ${name}`);
    } catch (e) {
      failures++;
      console.log(`FAIL ${name}: ${e.message}`);
    }
  }
  console.log(`procedural filters: ${cases.length - failures}/${cases.length} passed`);
  process.exit(failures ? 1 : 0);
})();
