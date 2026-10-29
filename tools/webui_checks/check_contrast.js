// Colour contrast of the molt:// WebUI pages' CSS (docs/ACCESSIBILITY.md).
//
// jsdom paints nothing, so axe's colour-contrast rule can't run there.
// This reads each page's <style> statically instead, in every theme the
// page has (its :root tokens, :root[data-theme=...] overrides and
// prefers-color-scheme blocks), and checks:
//
//   * every rule that sets a text colour, against the rule's own
//     background when it has one, else the background of the nearest
//     ancestor in its selector that has one (".card .meta" sits on
//     ".card"), else the page's (translucent backgrounds blended over what
//     is below, every gradient stop);
//   * 4.5:1 for text (WCAG 1.4.3 AA), 3:1 for large text (1.5rem, or
//     1.17rem bold).
//
// It can't know what an element really sits on (a card inside a panel
// inside the page), so it is a floor, not a proof: the build machine runs
// axe's colour-contrast rule in Chromium on the rendered pages. Disabled
// controls are exempt (WCAG 1.4.3), and so are the rules listed in EXEMPT
// with the reason.
//
// Run: node check_contrast.js [page.cc ...]   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');

const WEBUI = path.join(__dirname, '../../src/chrome/browser/ui/webui/molt_ai');
const PAGES = ['molt_home_ui.cc', 'molt_ai_memory_ui.cc', 'molt_ai_update_ui.cc', 'molt_import_ui.cc',
               'molt_net_ui.cc', 'molt_ai_settings_ui.cc', 'molt_ai_agent_ui.cc',
               'molt_ai_automation_ui.cc', 'molt_ai_ui.cc', 'molt_ai_chat_ui.cc'];

// Selector substrings whose colours need no text contrast, and why.
const EXEMPT = [
  [':disabled', 'disabled controls (WCAG 1.4.3 exception)'],
  ['.disabled', 'disabled controls (WCAG 1.4.3 exception)'],
  ['[disabled]', 'disabled controls (WCAG 1.4.3 exception)'],
  ['.dim', 'browsers that are not installed: a disabled row'],
  ['::selection', 'selection highlight, the browser keeps text readable'],
  ['::-webkit-scrollbar', 'scrollbar parts, not text'],
  ['.highlight', 'search-match highlight, a mark over text that has its own colour'],
];

// Selector prefixes of rules for elements that sit on a container the
// selector doesn't name, per page: prefix -> the container's selector.
const CONTEXT = {
  'molt_ai_ui.cc': {'.welcome-': '.welcome-overlay', '.model-card': '.model-dialog',
                    '.mcd-': '.model-chip-dropdown', '.model-chip-item': '.model-chip-dropdown'},
  'molt_ai_chat_ui.cc': {'.welcome-': '.welcome-overlay', '.mcd-': '.model-chip-dropdown',
                         '.model-chip-item': '.model-chip-dropdown', '.om-': '.overflow-menu',
                         '.mode-item': '.mode-chip-dropdown', '.conv-': '.drawer',
                         '.drawer-': '.drawer', '.model-card': '.model-panel'},
  'molt_ai_agent_ui.cc': {'.sheet-': '.sheet', '.rmode': '.sheet', '.opt': '.sheet',
                          '.vfield': '.sheet', '.mini-modal': '.mini-modal', '.menu-item': '.card-menu'},
  'molt_ai_automation_ui.cc': {'.modal': '.modal'},
};

// ---- colours ---------------------------------------------------------
const NAMED = {white: [255, 255, 255, 1], black: [0, 0, 0, 1], transparent: [0, 0, 0, 0],
               red: [255, 0, 0, 1], gray: [128, 128, 128, 1], grey: [128, 128, 128, 1]};
function parseColor(s) {
  s = s.trim().toLowerCase();
  if (NAMED[s]) return NAMED[s].slice();
  let m = s.match(/^#([0-9a-f]{3,8})$/);
  if (m) {
    let h = m[1];
    if (h.length === 3 || h.length === 4) h = h.split('').map((c) => c + c).join('');
    if (h.length !== 6 && h.length !== 8) return null;
    const n = (i) => parseInt(h.slice(i, i + 2), 16);
    return [n(0), n(2), n(4), h.length === 8 ? n(6) / 255 : 1];
  }
  m = s.match(/^rgba?\(([^)]*)\)$/);
  if (m) {
    const p = m[1].split(/[\s,/]+/).filter(Boolean);
    if (p.length < 3) return null;
    const ch = (v) => (v.endsWith('%') ? parseFloat(v) * 2.55 : parseFloat(v));
    const a = p.length > 3 ? (p[3].endsWith('%') ? parseFloat(p[3]) / 100 : parseFloat(p[3])) : 1;
    return [ch(p[0]), ch(p[1]), ch(p[2]), a];
  }
  m = s.match(/^hsla?\(([^)]*)\)$/);
  if (m) {
    const p = m[1].split(/[\s,/]+/).filter(Boolean);
    const h = parseFloat(p[0]) / 360, sat = parseFloat(p[1]) / 100, l = parseFloat(p[2]) / 100;
    const a = p.length > 3 ? parseFloat(p[3]) : 1;
    const q = l < 0.5 ? l * (1 + sat) : l + sat - l * sat, pp = 2 * l - q;
    const f = (t) => {
      t = (t + 1) % 1;
      if (t < 1 / 6) return pp + (q - pp) * 6 * t;
      if (t < 1 / 2) return q;
      if (t < 2 / 3) return pp + (q - pp) * (2 / 3 - t) * 6;
      return pp;
    };
    return [f(h + 1 / 3) * 255, f(h) * 255, f(h - 1 / 3) * 255, a];
  }
  return null;
}
function over(fg, bg) {
  const a = fg[3];
  return [fg[0] * a + bg[0] * (1 - a), fg[1] * a + bg[1] * (1 - a), fg[2] * a + bg[2] * (1 - a), 1];
}
function luminance(c) {
  const f = (v) => { v /= 255; return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4); };
  return 0.2126 * f(c[0]) + 0.7152 * f(c[1]) + 0.0722 * f(c[2]);
}
function contrast(a, b) {
  const la = luminance(a), lb = luminance(b);
  return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}
const hex = (c) => '#' + c.slice(0, 3).map((v) => Math.round(v).toString(16).padStart(2, '0')).join('');

// ---- CSS -------------------------------------------------------------
// Rules as {media, selector, decls}, one level of @media; other at-rules
// (@keyframes, @font-face) are skipped.
function parseCss(css) {
  css = css.replace(/\/\*[\s\S]*?\*\//g, '');
  const rules = [];
  let i = 0;
  function block(media, end) {
    while (i < end) {
      const open = css.indexOf('{', i);
      if (open < 0 || open >= end) { i = end; return; }
      const head = css.slice(i, open).trim();
      // Find the matching close brace.
      let depth = 1, j = open + 1;
      while (j < css.length && depth) {
        if (css[j] === '{') depth++;
        else if (css[j] === '}') depth--;
        j++;
      }
      if (head.startsWith('@media')) {
        i = open + 1;
        block(head.slice(6).trim(), j - 1);
        i = j;
      } else if (head.startsWith('@')) {
        i = j;
      } else {
        const decls = {};
        for (const d of css.slice(open + 1, j - 1).split(';')) {
          const k = d.indexOf(':');
          if (k < 0) continue;
          decls[d.slice(0, k).trim().toLowerCase()] = d.slice(k + 1).trim();
        }
        for (const sel of head.split(',')) rules.push({media, selector: sel.trim(), decls});
        i = j;
      }
    }
  }
  block('', css.length);
  return rules;
}

// The page's themes: which rules apply, and the tokens.
function themesOf(rules) {
  const themes = [{name: 'default', attr: null, media: null}];
  for (const r of rules) {
    const m = r.selector.match(/^:root\[data-theme="?(\w+)"?\]/);
    if (m && !themes.some((t) => t.attr === m[1])) themes.push({name: m[1], attr: m[1], media: null});
    if (/prefers-color-scheme:\s*light/.test(r.media) && !themes.some((t) => t.media === 'light')) {
      themes.push({name: 'light (system)', attr: null, media: 'light'});
    }
  }
  return themes;
}
function applies(rule, theme) {
  if (rule.media) {
    if (/prefers-color-scheme:\s*light/.test(rule.media)) return theme.media === 'light';
    if (/forced-colors|prefers-reduced-motion|print/.test(rule.media)) return false;
  }
  const m = rule.selector.match(/^:root\[data-theme="?(\w+)"?\]/);
  if (m) return theme.attr === m[1];
  return true;
}
function stripTheme(sel) { return sel.replace(/^:root\[data-theme="?\w+"?\]\s*/, '').trim(); }

function tokensFor(rules, theme) {
  const t = {};
  for (const r of rules) {
    if (!applies(r, theme)) continue;
    if (stripTheme(r.selector) !== ':root' && stripTheme(r.selector) !== '') continue;
    for (const [k, v] of Object.entries(r.decls)) if (k.startsWith('--')) t[k] = v;
  }
  return t;
}
function resolve(value, tokens, depth = 0) {
  if (depth > 8) return value;
  return value.replace(/var\(\s*(--[\w-]+)\s*(?:,\s*([^()]*(?:\([^)]*\))?[^()]*))?\)/g, (m, name, fb) =>
    tokens[name] !== undefined ? resolve(tokens[name], tokens, depth + 1) : (fb || ''));
}
function colorsIn(value) {
  return Array.from(value.matchAll(/#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)|hsla?\([^)]*\)|\b(?:white|black|transparent)\b/g),
                    (m) => parseColor(m[0])).filter(Boolean);
}

// The page background in a theme: html/body background, else --bg / --ground.
function pageBackground(rules, theme, tokens) {
  let bg = null;
  for (const r of rules) {
    if (!applies(r, theme)) continue;
    const sel = stripTheme(r.selector);
    if (!/^(html|body|html,body|:root)$/.test(sel)) continue;
    const v = r.decls['background-color'] || r.decls.background;
    if (!v) continue;
    const cs = colorsIn(resolve(v, tokens)).filter((c) => c[3] > 0.5);
    if (cs.length) bg = cs[cs.length - 1];
  }
  for (const k of ['--bg', '--ground', '--bg-grad-b']) {
    if (bg) break;
    if (tokens[k]) bg = parseColor(resolve(tokens[k], tokens));
  }
  return bg ? over(bg, [255, 255, 255, 1]) : [10, 10, 10, 1];
}

function remOf(v) {
  if (!v) return null;
  const m = v.match(/([\d.]+)(rem|em|px)/);
  if (!m) return null;
  return m[2] === 'px' ? parseFloat(m[1]) / 16 : parseFloat(m[1]);
}

// The background a rule's text sits on, as layers from the top: the
// rule's own, then the nearest ancestor compound in its selector with one
// (".a .b .c" looks at ".a .b", then ".a"), then CONTEXT's.
function backgroundsFor(rule, sel, rules, theme, tokens, file) {
  const own = rule.decls['background-color'] || rule.decls.background;
  if (own && colorsIn(resolve(own, tokens)).length) return [colorsIn(resolve(own, tokens))];
  const layers = [];
  // ".x:hover" and ".x.copied" keep the background of ".x".
  const plain = sel.replace(/(:[\w-]+(\([^)]*\))?|\.(copied|on|open|active|sel|show))+$/, '');
  if (plain !== sel) {
    const hit = rules.filter((o) => applies(o, theme) && stripTheme(o.selector) === plain &&
                                    (o.decls['background-color'] || o.decls.background))
        .map((o) => colorsIn(resolve(o.decls['background-color'] || o.decls.background, tokens)))
        .filter((cs) => cs.length);
    if (hit.length) return [hit[hit.length - 1]];
  }
  const parts = sel.split(/\s+/);
  for (let n = parts.length - 1; n >= 1; n--) {
    const anc = parts.slice(0, n).join(' ');
    const hit = rules.filter((o) => applies(o, theme) && stripTheme(o.selector) === anc &&
                                    (o.decls['background-color'] || o.decls.background))
        .map((o) => colorsIn(resolve(o.decls['background-color'] || o.decls.background, tokens)))
        .filter((cs) => cs.length);
    if (hit.length) { layers.push(hit[hit.length - 1]); break; }
  }
  for (const [prefix, anc] of Object.entries(CONTEXT[file] || {})) {
    if (!sel.startsWith(prefix)) continue;
    const hit = rules.filter((o) => applies(o, theme) && stripTheme(o.selector) === anc &&
                                    (o.decls['background-color'] || o.decls.background))
        .map((o) => colorsIn(resolve(o.decls['background-color'] || o.decls.background, tokens)))
        .filter((cs) => cs.length);
    if (hit.length) layers.push(hit[hit.length - 1]);
  }
  return layers;
}

function checkPage(file) {
  const src = fs.readFileSync(path.join(WEBUI, file), 'utf8');
  const html = (src.match(/R"HTML\(([\s\S]*?)\)HTML"/) || [])[1] || '';
  const css = Array.from(html.matchAll(/<style>([\s\S]*?)<\/style>/g), (m) => m[1]).join('\n');
  const rules = parseCss(css);
  const problems = [];
  let checked = 0;
  for (const theme of themesOf(rules)) {
    const tokens = tokensFor(rules, theme);
    const base = pageBackground(rules, theme, tokens);
    // A theme's own rule for a selector replaces the default one there.
    const themed = new Set(rules.filter((r) => applies(r, theme) && (r.media || /^:root\[/.test(r.selector)))
                                .map((r) => stripTheme(r.selector)));
    for (const r of rules) {
      if (!applies(r, theme)) continue;
      const sel = stripTheme(r.selector);
      if (!r.media && !/^:root\[/.test(r.selector) && theme.name !== 'default' && themed.has(sel) &&
          rules.some((o) => o !== r && applies(o, theme) && stripTheme(o.selector) === sel &&
                     (o.media || /^:root\[/.test(o.selector)) && o.decls.color)) {
        continue;
      }
      const colorDecl = r.decls.color;
      if (!colorDecl || /inherit|currentcolor|initial|unset/i.test(colorDecl)) continue;
      if (/transparent/.test(r.decls['-webkit-text-fill-color'] || '')) continue;
      if (EXEMPT.some(([s]) => sel.includes(s))) continue;
      const fgs = colorsIn(resolve(colorDecl, tokens));
      if (fgs.length !== 1) continue;
      // Layers top-down; each stop of the top layer over the next ones.
      const layers = backgroundsFor(r, sel, rules, theme, tokens, file);
      let under = base;
      for (let li = layers.length - 1; li >= 1; li--) under = over(layers[li][0], under);
      const bgs = layers.length ? layers[0].map((c) => over(c, under)) : [base];
      const size = remOf(r.decls['font-size']);
      const bold = /^(bold|[6-9]00)$/.test((r.decls['font-weight'] || '').trim());
      const large = size !== null && (size >= 1.5 || (size >= 1.17 && bold));
      const need = large ? 3 : 4.5;
      let worst = null;
      for (const b of bgs) {
        const back = b;
        const ratio = contrast(over(fgs[0], back), back);
        if (!worst || ratio < worst.ratio) worst = {ratio, back};
      }
      checked++;
      if (worst.ratio + 1e-9 < need) {
        problems.push(`${file} [${theme.name}] ${sel}: ${hex(over(fgs[0], worst.back))} on ` +
                      `${hex(worst.back)} is ${worst.ratio.toFixed(2)}:1, needs ${need}:1`);
      }
    }
  }
  return {problems, checked};
}

function main() {
  // The formula against WCAG's own reference pairs.
  const near = (x, y) => Math.abs(x - y) < 0.01;
  if (!near(contrast(parseColor('#767676'), parseColor('#ffffff')), 4.54) ||
      !near(contrast(parseColor('#000'), parseColor('#fff')), 21) ||
      !near(contrast(over(parseColor('rgba(0,0,0,0.5)'), parseColor('#fff')), parseColor('#fff')), 3.98)) {
    throw new Error('check_contrast: the contrast formula is broken');
  }
  const files = process.argv.slice(2).length ? process.argv.slice(2) : PAGES;
  let all = [];
  let checked = 0;
  for (const f of files) {
    const r = checkPage(f);
    all = all.concat(r.problems);
    checked += r.checked;
  }
  if (all.length) {
    console.error(all.join('\n'));
    console.error(`check_contrast: ${all.length} colour pairs below WCAG AA`);
    process.exit(1);
  }
  console.log(`check_contrast: ${checked} colour pairs in ${files.length} pages meet WCAG AA`);
}

module.exports = {parseColor, contrast, parseCss};
if (require.main === module) main();
