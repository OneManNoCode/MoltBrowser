// Accessibility check of the molt:// WebUI pages (docs/ACCESSIBILITY.md).
//
// Every page is loaded the way webui_smoke_test.js loads it (strict CSP,
// Trusted Types emulated, chrome.send stubbed), in its main states:
//
//   empty    the browser answers every request with {} (nothing yet)
//   content  the smoke test's canned answers, after its interactions
//   ...      page states below: a dialog open, an error shown, an answer
//            streamed, a step run
//
// and in each state:
//
//   * axe-core runs every rule it can evaluate without layout; any
//     violation, or any "needs review" result from those rules, fails;
//   * the checks axe doesn't make: exactly one <h1>; everything that
//     takes a click is a native control or has a role and tabindex (and
//     the shared Enter/Space handling); a control's visible words are in
//     its accessible name (WCAG 2.5.3, what voice control users say); an
//     open dialog is modal, named, holds focus and leaves the rest inert;
//     live regions say what the page says;
//
// and once per page, over its <style>: a :focus-visible style, a
// prefers-reduced-motion block that stops animations, a forced-colors
// block, and font sizes in relative units.
//
// The rules that need layout or rendering (colour contrast, target size
// and others; NEEDS_LAYOUT below) run on the build machine in a real
// Chromium; check_contrast.js covers the colour tokens here.
//
// Run: npm ci && node webui_a11y_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {PAGES, runPage} = require('./webui_smoke_test.js');

const AXE_SOURCE = fs.readFileSync(require.resolve('axe-core/axe.js'), 'utf8');
const WEBUI = path.join(__dirname, '../../src/chrome/browser/ui/webui/molt_ai');

// axe rules jsdom can't evaluate: it does no layout and paints nothing,
// so these would pass vacuously or come back "needs review". The build
// machine runs them in Chromium (docs/ACCESSIBILITY.md, "Build machine").
const NEEDS_LAYOUT = {
  'color-contrast': 'text colour over the painted background (WCAG 1.4.3)',
  'link-in-text-block': 'links told apart from text by more than colour (1.4.1)',
  'target-size': 'size and spacing of click targets (2.5.8)',
  'scrollable-region-focusable': 'overflowing regions reachable by keyboard (2.1.1)',
  'label-content-name-mismatch': 'visible label inside the accessible name (2.5.3); ' +
      'approximated here by checkLabelInName()',
  'css-orientation-lock': 'orientation media queries (1.3.4)',
};
const AXE_OPTIONS = {
  resultTypes: ['violations', 'incomplete'],
  rules: Object.assign(
      Object.fromEntries(Object.keys(NEEDS_LAYOUT).map((id) => [id, {enabled: false}])),
      // Experimental, but exactly what keyboard users need: anything in
      // the tab order has a role that says what it does.
      {'focus-order-semantics': {enabled: true}}),
};

// Roles a click handler may sit on when the element isn't a native
// control: the page gives them tabindex, and moltA11y's Enter/Space
// handling (molt_webui_csp.h) makes them work from the keyboard.
const ACTIVATABLE_ROLES = new Set([
  'button', 'link', 'menuitem', 'menuitemcheckbox', 'menuitemradio', 'tab',
  'option', 'switch', 'checkbox', 'radio', 'treeitem']);

function isNativeControl(el) {
  const tag = el.localName;
  if (tag === 'button' || tag === 'select' || tag === 'textarea' || tag === 'summary') return true;
  if (tag === 'input') return el.type !== 'hidden';
  if (tag === 'a' || tag === 'area') return el.hasAttribute('href');
  // A <label> forwards its click to its control.
  if (tag === 'label') return !!el.control;
  return false;
}

function makeShown(window) {
  return (el) => {
    for (let n = el; n && n.nodeType === 1; n = n.parentElement) {
      if (n.hidden) return false;
      const cs = window.getComputedStyle(n);
      if (cs.display === 'none' || cs.visibility === 'hidden') return false;
    }
    return true;
  };
}

function describe(el) {
  return el.outerHTML.replace(/\s+/g, ' ').slice(0, 90);
}

// Words a sighted user reads on a control: its text, minus aria-hidden
// parts, reduced to letters and digits.
function words(s) {
  return String(s || '').toLowerCase().normalize('NFKD')
      .replace(/[^\p{L}\p{N}]+/gu, ' ').trim();
}
function visibleWords(el) {
  let out = '';
  const walk = (n) => {
    if (n.nodeType === 3) { out += n.data; return; }
    if (n.nodeType !== 1 || n.getAttribute('aria-hidden') === 'true') return;
    for (const c of n.childNodes) walk(c);
    if (/^(div|p|br|li)$/.test(n.localName)) out += ' ';
  };
  walk(el);
  return words(out);
}

// WCAG 2.5.3 Label in Name: what a voice control user reads on a control
// ("click Save schedule") has to be in its accessible name.
function checkLabelInName(doc) {
  const problems = [];
  const sel = 'button, a[href], input[type=button], input[type=submit], ' +
              '[role=button], [role=link], [role=tab], [role=menuitem], [role=switch], ' +
              '[role=checkbox], [role=option]';
  for (const el of doc.querySelectorAll(sel)) {
    const label = el.getAttribute('aria-label');
    if (label === null) continue;
    const seen = visibleWords(el);
    if (!seen) continue;  // icon only: the label is the name
    if (!(' ' + words(label) + ' ').includes(' ' + seen + ' ')) {
      problems.push(`label-in-name: "${seen}" is not in aria-label "${label}": ${describe(el)}`);
    }
  }
  return problems;
}

function checkDom(window, page, state) {
  const doc = window.document;
  const shown = makeShown(window);
  const problems = [];
  const h1 = Array.from(doc.querySelectorAll('h1')).filter(shown);
  if (h1.length !== 1) problems.push(`one-h1: ${h1.length} visible <h1> elements`);

  // Keyboard: whatever takes a click can be reached and used without a
  // pointer.
  const pointerOnly = (A11Y[page.name] || {}).pointerOnly || [];
  for (const el of doc.querySelectorAll('*')) {
    if (el === doc.documentElement || el === doc.body) continue;
    if (!window.__listenedTypes(el).has('click') && !el.hasAttribute('data-on-click') &&
        typeof el.onclick !== 'function') {
      continue;
    }
    if (isNativeControl(el)) continue;
    if (pointerOnly.some((sel) => el.matches(sel))) continue;
    const role = el.getAttribute('role');
    const tabindex = el.getAttribute('tabindex');
    if (!role || !ACTIVATABLE_ROLES.has(role) || tabindex === null || Number(tabindex) < 0) {
      problems.push(`keyboard: takes a click but is not a button, link or ` +
                    `focusable control: ${describe(el)}`);
    }
  }
  problems.push(...checkLabelInName(doc));

  // A custom-drawn switch or box whose real input is display:none: its
  // label shows, but neither the keyboard nor a screen reader can reach it.
  for (const el of doc.querySelectorAll('input, select, textarea')) {
    if (el.type === 'hidden' || el.type === 'file' || shown(el)) continue;
    const label = el.closest('label') ||
        (el.id ? doc.querySelector(`label[for="${el.id}"]`) : null);
    if (label && shown(label)) {
      problems.push(`hidden control: its label shows but the control is not rendered: ${describe(el)}`);
    }
  }

  // Open dialogs: modal, named, focus inside, the rest of the page inert.
  // A dialog under another one is inert itself: only the top one counts.
  for (const d of doc.querySelectorAll('[role=dialog], [role=alertdialog], dialog')) {
    if (!shown(d) || d.closest('[inert]')) continue;
    if (d.getAttribute('aria-modal') !== 'true') problems.push(`dialog: not aria-modal: ${describe(d)}`);
    if (!d.getAttribute('aria-label') && !d.getAttribute('aria-labelledby')) {
      problems.push(`dialog: no label: ${describe(d)}`);
    }
    if (!d.contains(doc.activeElement)) {
      problems.push(`dialog: focus is outside the open dialog (on ${describe(doc.activeElement)})`);
    }
    for (let n = d; n.parentElement && n !== doc.body; n = n.parentElement) {
      for (const sib of n.parentElement.children) {
        if (sib === n || /^(script|style)$/.test(sib.localName)) continue;
        if (sib.hasAttribute('data-molt-live') || !shown(sib)) continue;
        if (!sib.closest('[inert]')) problems.push(`dialog: ${describe(sib)} is not inert behind it`);
      }
    }
  }
  return problems;
}

async function runAxe(window) {
  window.__ttOff = true;  // axe builds its own nodes
  try {
    if (!window.axe) {
      // jsdom has no hit testing; axe asks for it to find modal overlays.
      window.document.elementFromPoint = () => null;
      window.document.elementsFromPoint = () => [];
      // jsdom's selector engine matches label[for="null"] against every
      // <label> without a for attribute; axe asks exactly that for a field
      // with no id, and would see every such label as the field's.
      for (const proto of [window.Element.prototype, window.Document.prototype]) {
        const qsa = proto.querySelectorAll;
        proto.querySelectorAll = function(sel) {
          if (sel === 'label[for="null"]') {
            return Array.from(qsa.call(this, 'label[for]'))
                .filter((l) => l.getAttribute('for') === 'null');
          }
          return qsa.call(this, sel);
        };
      }
      window.eval(AXE_SOURCE);
    }
    // While a modal dialog is open the page behind it is inert, so it has
    // no main landmark or <h1> to a screen reader, which is the point.
    const modal = Array.from(window.document.querySelectorAll('[aria-modal=true]'))
        .some(makeShown(window));
    const options = modal ? Object.assign({}, AXE_OPTIONS, {rules: Object.assign({},
        AXE_OPTIONS.rules, {'landmark-one-main': {enabled: false},
                            'page-has-heading-one': {enabled: false}})}) : AXE_OPTIONS;
    const r = await window.axe.run(window.document, options);
    const out = [];
    for (const v of r.violations) {
      out.push(`axe ${v.id} (${v.impact}): ${v.help}\n      ` +
               v.nodes.slice(0, 6).map((n) => n.target.join(' ') + ' ' +
                   (n.failureSummary || '').replace(/\s+/g, ' ').slice(0, 160))
                   .join('\n      '));
    }
    for (const v of r.incomplete) {
      out.push(`axe needs review ${v.id}: ${v.help}\n      ` +
               v.nodes.slice(0, 6).map((n) => n.target.join(' ') + ' ' +
                   JSON.stringify(n.any.concat(n.all, n.none).map((c) => c.message)))
                   .join('\n      '));
    }
    return out;
  } finally {
    window.__ttOff = false;
  }
}

// The page's own CSS: focus, motion, forced colours, relative text.
function checkCss(page) {
  const src = fs.readFileSync(path.join(WEBUI, page.file), 'utf8');
  const html = (src.match(/R"HTML\(([\s\S]*?)\)HTML"/) || [])[1] || '';
  const css = Array.from(html.matchAll(/<style>([\s\S]*?)<\/style>/g), (m) => m[1]).join('\n');
  const problems = [];
  if (!/:focus-visible\s*[,{][^}]*outline\s*:\s*(?!none|0\b)/.test(css)) {
    problems.push('css: no :focus-visible rule with a visible outline');
  }
  if (/[{;\s]animation\s*:|transition\s*:|scroll-behavior\s*:\s*smooth/.test(css)) {
    const rm = css.match(/@media\s*\(prefers-reduced-motion:\s*reduce\)\s*\{([\s\S]*?\})\s*\}/);
    if (!rm || !/animation[^;]*:\s*none|animation-duration/.test(rm[1]) ||
        !/transition[^;]*:\s*none|transition-duration/.test(rm[1])) {
      problems.push('css: animations and transitions are not stopped under ' +
                    'prefers-reduced-motion: reduce');
    }
  }
  if (!/@media\s*\(forced-colors:\s*active\)/.test(css)) {
    problems.push('css: no forced-colors (high contrast) rules');
  }
  // Text sizes in rem, in the page's CSS and in every style its scripts
  // write (the page file and the script files it is served with).
  const files = [page.file].concat((page.extraFiles || []).filter((f) => f !== 'molt_webui_csp.h'));
  for (const f of files) {
    const text = Array.from(fs.readFileSync(path.join(WEBUI, f), 'utf8')
        .matchAll(/R"(HTML|JS)\(([\s\S]*?)\)\1"/g), (m) => m[2]).join('\n');
    const px = Array.from(text.matchAll(/font-size\s*:\s*[^;}"'\n]*?\b\d+(?:\.\d+)?px/g));
    if (px.length) {
      problems.push(`css: ${px.length} font sizes in px in ${f} (use rem): ` +
                    px.slice(0, 5).map((m) => m[0].trim()).join('; '));
    }
  }
  return problems;
}

const tick = (ms = 20) => new Promise((r) => setTimeout(r, ms));
// What the polite / assertive announcers (moltA11y.announce) say now.
const announced = (window, role = 'status') => {
  const r = window.document.querySelector(`[data-molt-live="${role}"]`);
  return r ? r.textContent : '';
};

// Per page: |states| beyond empty and content, and |pointerOnly|: click
// targets that only add a pointer shortcut to something the keyboard
// already has (a dialog's backdrop closes it; Escape does too, which the
// dialog state checks).
const keydown = (window, el, key, opts = {}) => el.dispatchEvent(
    new window.KeyboardEvent('keydown', Object.assign({key, bubbles: true, cancelable: true}, opts)));

// Records what moltA11y.announce() is asked to say.
function recordAnnouncements(window) {
  const said = [];
  const say = window.moltA11y.announce;
  window.moltA11y.announce = (t, o) => { said.push([t, !!(o && o.assertive)]); say(t, o); };
  return said;
}

const A11Y = {
  'molt://ai-chat': {
    pointerOnly: [
      '#drawerScrim',  // the drawer's backdrop; Escape closes it too
      '#messages',     // routes clicks on its links to a tab (they are links)
      '.conv-item'],   // the row opens the chat for a pointer; its title is the button
    states: [
      {name: 'answer streamed', async run(window) {
        await tick(40);
        window.__said = recordAnnouncements(window);
        window.startAiMessage();
        for (const t of ['# Plan\n', 'Step ', 'one.']) window.appendToken(t);
        window.__midway = window.__said.length;
        window.finishAiMessage();
        await tick(100);
      }, check(window) {
        assert.strictEqual(window.__midway, 0, 'nothing is read while it streams');
        assert.deepStrictEqual(window.__said.map((s) => s[0]),
            ['AI Assistant: Plan Step one.'], 'the answer is read once');
        const h = window.document.querySelector('#messages .message.ai:last-child [role=heading]');
        assert(h && h.getAttribute('aria-level') === '3', 'a heading in the answer is a heading');
      }},
      {name: 'chats drawer by keyboard', async run(window) {
        const doc = window.document;
        await tick(40);
        const btn = doc.getElementById('drawerBtn');
        btn.focus();
        btn.click();
        await tick(40);
      }, check(window) {
        const doc = window.document;
        const btn = doc.getElementById('drawerBtn');
        assert.strictEqual(btn.getAttribute('aria-expanded'), 'true');
        assert.strictEqual(doc.getElementById('drawer').getAttribute('aria-label'), 'Chats');
        const del = doc.querySelector('#convList .conv-del');
        assert(/^Delete chat: /.test(del.getAttribute('aria-label')));
        keydown(window, doc.activeElement, 'Escape');
        assert(!doc.getElementById('drawer').classList.contains('open'), 'Escape closes');
        assert.strictEqual(doc.activeElement, btn, 'focus back on the Chats button');
        assert.strictEqual(btn.getAttribute('aria-expanded'), 'false');
      }},
      {name: 'more-options menu by keyboard', async run(window) {
        const doc = window.document;
        await tick(40);
        const btn = doc.getElementById('overflowBtn');
        btn.focus();
        btn.click();
        doc.querySelector('#overflowMenu [data-arg=light]').focus();
      }, check(window) {
        const doc = window.document;
        const btn = doc.getElementById('overflowBtn');
        assert.strictEqual(btn.getAttribute('aria-expanded'), 'true');
        assert.strictEqual(doc.querySelector('#overflowMenu [data-arg=gray]').getAttribute('aria-pressed'), 'true');
        keydown(window, doc.activeElement, 'Escape');
        assert(!doc.getElementById('overflowMenu').classList.contains('open'));
        assert.strictEqual(doc.activeElement, btn, 'focus back on the menu button');
      }},
      {name: 'page action proposed', async run(window) {
        await tick(40);
        window.__said = recordAnnouncements(window);
        window._renderConfirmChip('AI proposes a page action', 'Click button "Buy"',
                                  () => {}, () => {});
      }, check(window) {
        const chip = window.document.querySelector('#messages .molt-action-confirm');
        assert.strictEqual(chip.getAttribute('role'), 'group');
        assert(window.__said.some(([t, assertive]) => assertive && /Click button "Buy"/.test(t)),
               'the proposal is read out at once');
        assert.notStrictEqual(window.document.activeElement.className, 'ac-allow',
                              'focus never jumps to Allow');
      }},
      {name: 'error shown', async run(window) {
        await tick(40);
        window.addErrorMessage('Could not load conversation.');
      }, check(window) {
        const err = window.document.querySelector('#messages .message.error');
        assert.strictEqual(err.getAttribute('role'), 'alert');
      }},
      {name: 'agent run', async run(window) {
        await tick(40);
        window.__said = recordAnnouncements(window);
        window.startWebAgent('Find the cheapest flight');
        const fire = (e, d) => window.cr.webUIListenerCallback(e, d);
        fire('agent-step', {number: 1, action: 'NAVIGATE', target: 'flights.example', success: true});
        fire('agent-plan', {steps: [{goal: 'Open the site', done_when: 'it loads'},
                                    {goal: 'Search', done_when: 'results show'}], current: 2});
        fire('agent-confirm', {id: 3, action: 'CLICK', message: 'Click "Book" on flights.example?'});
        await tick(40);
      }, check(window) {
        const said = window.__said.map((s) => s[0]);
        assert(said.includes('Step 1: NAVIGATE flights.example'), said.join(' | '));
        assert(said.includes('Plan step 2 of 2: Search'), said.join(' | '));
        assert(window.__said.some(([t, a]) => a && /Agent needs your OK/.test(t)));
        const bar = window.document.getElementById('agentBar');
        assert.strictEqual(bar.getAttribute('role'), 'region');
      }},
      {name: 'first-run welcome', responses: {
        initChat: {model_loaded: false, is_first_run: true, total_ram_gb: 16, cpu_cores: 8, models: []}},
      async run(window) {
        await tick(60);
      }, check(window) {
        const doc = window.document;
        const wo = doc.getElementById('welcomeOverlay');
        assert.strictEqual(wo.getAttribute('role'), 'dialog');
        assert(wo.contains(doc.activeElement), 'focus in the welcome');
        keydown(window, doc.activeElement, 'Escape');
        assert(!wo.classList.contains('open'), 'Escape skips');
        assert.strictEqual(doc.activeElement.id, 'chatInput');
      }},
    ],
  },
  'molt://ai': {
    pointerOnly: ['#modelOverlay'],  // the dialog's backdrop; Escape too
    states: [
      {name: 'answer streamed', async run(window) {
        await tick(40);
        window.__said = recordAnnouncements(window);
        window.startAiMsg();
        for (const t of ['The ', 'answer ', 'is ', '**42**.']) window.appendTok(t);
        window.__midway = window.__said.length;
        window.finishAiMsg();
        await tick(100);
      }, check(window) {
        assert.deepStrictEqual(window.__said.map((s) => s[0]),
            ['MoltBrowser AI is answering', 'MoltBrowser AI: The answer is 42.'],
            'one notice when it starts, the answer once when it is done');
        assert.strictEqual(window.__midway, 1, 'nothing per token');
        assert.strictEqual(announced(window), 'MoltBrowser AI: The answer is 42.');
        assert(!window.document.querySelector('#messages [aria-busy]'), 'not busy any more');
      }},
      {name: 'first-run welcome', responses: {
        initChat: {model_loaded: false, is_first_run: true, total_ram_gb: 16, cpu_cores: 8, models: []}},
      async run(window) {
        await tick(40);
      }, check(window) {
        const doc = window.document;
        const wo = doc.getElementById('welcomeOverlay');
        assert.strictEqual(wo.getAttribute('role'), 'dialog');
        assert(wo.contains(doc.activeElement), 'focus in the welcome');
        keydown(window, doc.activeElement, 'Escape');
        assert(!wo.classList.contains('open'), 'Escape skips the set-up');
        assert.strictEqual(doc.activeElement.id, 'prompt', 'focus goes to the message field');
      }},
      {name: 'model menu by keyboard', async run(window) {
        const doc = window.document;
        await tick(40);
        const chip = doc.getElementById('modelChip');
        chip.focus();
        chip.click();
        await tick(40);
      }, check(window) {
        const doc = window.document;
        const chip = doc.getElementById('modelChip');
        assert.strictEqual(chip.getAttribute('aria-expanded'), 'true');
        const picks = doc.querySelectorAll('#modelChipDropdown button.mpick');
        assert(picks.length >= 2, 'each model is a button');
        picks[0].focus();
        keydown(window, picks[0], 'Escape');
        assert.strictEqual(chip.getAttribute('aria-expanded'), 'false');
        assert.strictEqual(doc.activeElement, chip, 'focus back on the model button');
      }},
      {name: 'error shown', async run(window) {
        await tick(40);
        window.addErr('The model could not be loaded.');
      }, check(window) {
        const err = window.document.querySelector('#messages [role=alert]');
        assert(err && /could not be loaded/.test(err.textContent));
      }},
    ]},
  'molt://ai-settings': {states: [
    {name: 'provider key form opened by keyboard', async run(window) {
      const doc = window.document;
      await tick(40);
      const btn = doc.querySelector('#providersList [aria-label="Connect Custom"]');
      btn.focus();
      btn.click();
      // An empty key: the error is an alert.
      doc.querySelector('#pbody-custom [aria-label="Connect Custom with this key"]').click();
    }, check(window) {
      const doc = window.document;
      const btn = doc.querySelector('#providersList [aria-label="Connect Custom"]');
      assert.strictEqual(btn.getAttribute('aria-expanded'), 'true');
      assert.strictEqual(btn.getAttribute('aria-controls'), 'pbody-custom');
      const err = doc.querySelector('#pbody-custom .provider-err');
      assert.strictEqual(err.getAttribute('role'), 'alert');
      assert.strictEqual(err.textContent, 'Enter an API key.');
      // The switches are real, focusable checkboxes.
      const sw = doc.getElementById('autoLoadModel');
      sw.focus();
      assert.strictEqual(doc.activeElement, sw);
    }},
  ]},
  'molt://ai-agent': {
    // The sheets' backdrops: a click outside closes them, as Escape does.
    pointerOnly: ['#runScrim', '#schedScrim', '#miniScrim', '.opt'],
    states: [
      {name: 'run sheet open', async run(window) {
        const doc = window.document;
        await tick(40);
        const run = doc.querySelector('#libList .card [aria-label^="Run "]');
        run.focus();
        run.click();
        await tick(40);
      }, check(window) {
        const doc = window.document;
        const sheet = doc.getElementById('runSheet');
        assert.strictEqual(sheet.getAttribute('role'), 'dialog');
        assert.strictEqual(sheet.getAttribute('aria-labelledby'), 'runTitle');
        const modes = doc.querySelectorAll('#runBody .rmode[aria-pressed]');
        assert.strictEqual(modes.length, 2, 'the run modes are toggle buttons');
        modes[1].click();
        assert.strictEqual(modes[1].getAttribute('aria-pressed'), 'true');
        assert.strictEqual(modes[0].getAttribute('aria-pressed'), 'false');
        // Escape closes it; focus goes back to the Run button.
        keydown(window, doc.activeElement, 'Escape');
        assert(!doc.getElementById('runScrim').classList.contains('open'), 'Escape closes');
        assert(/^Run /.test(doc.activeElement.getAttribute('aria-label') || ''),
               'focus is back on Run: ' + doc.activeElement.outerHTML.slice(0, 80));
        assert(!doc.querySelector('.app[inert]'), 'the page is usable again');
      }},
      {name: 'schedule sheet open', async run(window) {
        const doc = window.document;
        await tick(40);
        doc.querySelector('#libList .card [aria-label^="Schedule "]').click();
        await tick(20);
      }, check(window) {
        const doc = window.document;
        const radios = doc.querySelectorAll('#schedBody input[type=radio][name=schedMode]');
        assert.strictEqual(radios.length, 4, 'each schedule option has a radio');
        assert.strictEqual(radios[0].labels.length, 1, 'labelled by its title');
        radios[2].checked = true;
        radios[2].dispatchEvent(new window.Event('change', {bubbles: true}));
        const sel = doc.querySelector('#schedBody .opt.sel');
        assert.strictEqual(sel.getAttribute('data-mode'), 'daily', 'the radio picks the option');
        assert.strictEqual(doc.getElementById('dailyTime').getAttribute('aria-label'), 'Time of day');
      }},
      {name: 'delete confirm open', async run(window) {
        const doc = window.document;
        await tick(40);
        const kebab = doc.querySelector('#libList .card .kebab');
        kebab.focus();
        kebab.click();
        assert.strictEqual(kebab.getAttribute('aria-expanded'), 'true');
        assert(doc.activeElement.classList.contains('menu-item'), 'focus moves into the menu');
        Array.from(doc.querySelectorAll('.card-menu .menu-item'))
            .find((b) => /Delete/.test(b.textContent)).click();
        await tick(80);
      }, check(window) {
        const doc = window.document;
        const box = doc.getElementById('miniModal');
        assert.strictEqual(box.getAttribute('role'), 'alertdialog');
        assert.strictEqual(box.getAttribute('aria-describedby'), 'miniMsg');
      }},
      {name: 'card menu closed by Escape', async run(window) {
        const doc = window.document;
        await tick(40);
        const kebab = doc.querySelector('#libList .card .kebab');
        kebab.focus();
        kebab.click();
        keydown(window, doc.activeElement, 'Escape');
      }, check(window) {
        const doc = window.document;
        assert(!doc.querySelector('.card-menu'), 'menu closed');
        assert(doc.activeElement.classList.contains('kebab'), 'focus back on its button');
        assert.strictEqual(doc.activeElement.getAttribute('aria-expanded'), 'false');
      }},
      {name: 'a run reads out its steps', async run(window) {
        await tick(40);
        const fire = (e, d) => window.cr.webUIListenerCallback(e, d);
        const say = window.moltA11y.announce;
        window.__said = [];
        window.moltA11y.announce = (t, o) => { window.__said.push(t); say(t, o); };
        fire('run-progress', {id: 'w1', stepIndex: 0, total: 3, message: 'Open example.com'});
        fire('run-progress', {id: 'w1', stepIndex: 0, total: 3, message: 'Open example.com'});
        fire('run-progress', {id: 'w1', stepIndex: 1, total: 3, message: 'Click Buy'});
        fire('run-complete', {id: 'w1', ok: false, failedStepIndex: 1,
                              failedStep: 'Click Buy', message: 'element not found'});
        await tick(100);
      }, check(window) {
        assert.deepStrictEqual(window.__said.slice(0, 2),
            ['Step 1 of 3: Open example.com', 'Step 2 of 3: Click Buy'],
            'each step is read once');
        assert(/^Run stopped: Failed at step 2/.test(announced(window, 'alert')),
               announced(window, 'alert'));
        const bar = window.document.querySelector('#libList .card [role=progressbar]');
        assert(bar, 'the run shows a progressbar');
      }},
      {name: 'recording', async run(window) {
        const doc = window.document;
        await tick(40);
        doc.getElementById('recordBtn').click();
        window.cr.webUIListenerCallback('recorder-step',
            {index: 0, type: 'click', title: 'Click “Search”'});
        await tick(100);
      }, check(window) {
        const doc = window.document;
        assert.strictEqual(announced(window), 'Step 1 recorded: Click “Search”');
        assert.strictEqual(doc.activeElement.localName, 'h2', 'focus on the view heading');
        assert(/Recording/.test(doc.activeElement.textContent));
        assert.strictEqual(doc.querySelector('#recList .drop').getAttribute('aria-label'),
                           'Remove step 1');
      }},
    ]},
  'molt://ai-automation': {
    pointerOnly: [
      // The row toggles the card for a pointer; its name is the button.
      '.script-summary',
      // The (unused) confirm dialog's backdrop; Escape closes it too.
      '#modal-bg'],
    states: [
      {name: 'card opened by keyboard, list rebuilt', async run(window) {
        const doc = window.document;
        await tick(40);
        const btn = doc.querySelector('#list .name-btn');
        btn.focus();
        btn.click();  // what Enter or Space on the button does
        assert.strictEqual(btn.getAttribute('aria-expanded'), 'true');
        doc.querySelector('#list [data-field="target"]').focus();
        window.refresh();  // the list is rebuilt from the browser's answer
        await tick(40);
      }, check(window) {
        const doc = window.document;
        const card = doc.querySelector('#list .script-card');
        assert(card.classList.contains('open'), 'the card stays open');
        assert.strictEqual(card.querySelector('.name-btn').getAttribute('aria-expanded'), 'true');
        assert.strictEqual(doc.activeElement.getAttribute('data-field'), 'target',
                           'focus comes back to the field');
        const detail = doc.getElementById(
            card.querySelector('.name-btn').getAttribute('aria-controls'));
        assert(detail && card.contains(detail), 'the button controls the details');
      }},
      {name: 'error shown', async run(window) {
        await tick();
        window.showToast('Save failed: disk full', true);
      }, check(window) {
        const t = window.document.getElementById('toast');
        assert.strictEqual(t.getAttribute('role'), 'alert');
      }},
    ]},
  'molt://net': {states: [
    {name: 'error shown', async run(window) {
      await tick(40);
      window.apply({running: false, error: 'Tor couldn\u2019t start.'});
      const err = window.document.getElementById('err');
      const node = err.firstChild;
      window.render();
      assert.strictEqual(err.firstChild, node, 're-rendering does not repeat the alert');
    }, check(window) {
      const doc = window.document;
      assert.strictEqual(doc.getElementById('err').getAttribute('role'), 'alert');
      assert.strictEqual(doc.getElementById('st').getAttribute('role'), 'status');
      const pressed = doc.querySelectorAll('#modes [aria-pressed=true]');
      assert.strictEqual(pressed.length, 1);
      assert.strictEqual(pressed[0].getAttribute('data-mode'), 'direct');
      const rows = doc.querySelectorAll('#list button.row[aria-pressed=true]');
      assert.strictEqual(rows.length, 1, 'one exit country is chosen');
    }},
  ]},
  'molt://import': {states: [
    {name: 'import tab by keyboard', async run(window) {
      const doc = window.document;
      await tick();
      const bm = doc.getElementById('tab-bm');
      bm.focus();
      bm.dispatchEvent(new window.KeyboardEvent('keydown', {key: 'ArrowRight', bubbles: true}));
      doc.getElementById('sw').click();
    }, check(window) {
      const doc = window.document;
      const im = doc.getElementById('tab-im');
      assert.strictEqual(doc.activeElement, im, 'arrow key moves to the Import tab');
      assert.strictEqual(im.getAttribute('aria-selected'), 'true');
      assert.strictEqual(im.tabIndex, 0);
      assert.strictEqual(doc.getElementById('tab-bm').tabIndex, -1);
      assert(!doc.getElementById('view-im').hidden);
      assert.strictEqual(doc.getElementById('sw').getAttribute('aria-checked'), 'true');
      const rows = doc.querySelectorAll('#implist button.row');
      assert.strictEqual(rows.length, 2);
      assert.strictEqual(rows[0].getAttribute('aria-label'), 'Import from Chrome');
      assert(rows[1].disabled, 'a browser that is not installed is unavailable');
    }},
  ]},
  'molt://memory': {states: [
    {name: 'error shown', async run(window) {
      window.document.getElementById('forget-domain-btn').click();
    }, check(window) {
      const t = window.document.getElementById('toast');
      assert.strictEqual(t.getAttribute('role'), 'alert');
      assert.strictEqual(t.textContent, 'Type a domain first');
    }},
    {name: 'search results', async run(window) {
      const doc = window.document;
      doc.getElementById('q').value = 'miso';
      doc.getElementById('search-btn').click();
      await tick(40);
    }, check(window) {
      const doc = window.document;
      assert.strictEqual(doc.getElementById('results-status').textContent, '2 results');
      assert.strictEqual(doc.getElementById('results-title').localName, 'h2');
    }},
  ]},
  'molt://ai-update': {states: [
    {name: 'error shown', async run(window) {
      await tick();
      window.cr.webUIListenerCallback('update-state', {
        state: 'error', error: 'The download did not match the signed manifest.',
        current_version: '0.2.5'});
    }, check(window) {
      const st = window.document.getElementById('status');
      assert.strictEqual(st.getAttribute('role'), 'alert', 'a failure is an alert');
      assert(/signed manifest/.test(st.textContent));
    }},
    {name: 'downloading', async run(window) {
      await tick();
      const s = {state: 'downloading', latest_version: '0.2.6', percent: 40};
      window.cr.webUIListenerCallback('update-state', s);
      const st = window.document.getElementById('status');
      const node = st.firstChild;
      window.cr.webUIListenerCallback('update-state', Object.assign({}, s, {percent: 41}));
      assert.strictEqual(st.firstChild, node, 'progress alone does not rewrite the status');
    }, check(window) {
      const bar = window.document.getElementById('bar');
      assert.strictEqual(bar.getAttribute('aria-valuenow'), '41');
      assert.strictEqual(window.document.getElementById('status').getAttribute('role'), 'status');
    }},
  ]},
};

// Each state loads the page afresh. |run| drives it there (like the smoke
// test's after()); |check| makes the state's own assertions.
function statesFor(page) {
  const states = [
    {name: 'empty', page: Object.assign({}, page, {
      responses: {}, after: null, listeners: [], clickGated: [], skipClickAll: true})},
    {name: 'content', page: Object.assign({}, page, {skipClickAll: true})},
  ];
  for (const s of (A11Y[page.name] || {}).states || []) {
    states.push({name: s.name, page: Object.assign({}, page, {
      after: s.run, check: s.check, skipClickAll: true,
      responses: Object.assign({}, page.responses, s.responses || {})})});
  }
  return states;
}

async function main() {
  const only = process.argv[2];
  let failed = 0;
  let checkedStates = 0;
  for (const page of PAGES) {
    if (only && page.name !== only) continue;
    const pageProblems = checkCss(page);
    for (const state of statesFor(page)) {
      checkedStates++;
      const problems = [];
      const {problems: loadProblems} = await runPage(state.page, {
        async done(window) {
          problems.push(...await runAxe(window));
          problems.push(...checkDom(window, page, state.name));
          if (state.page.check) {
            try { await state.page.check(window); } catch (e) { problems.push('check(): ' + (e.stack || e)); }
          }
        },
      });
      // The smoke test owns script errors in its own states; here they'd
      // mean the state never got set up.
      problems.unshift(...loadProblems);
      pageProblems.push(...problems.map((p) => `[${state.name}] ${p}`));
    }
    if (pageProblems.length) {
      failed++;
      console.error(`FAIL ${page.name}\n  ` + pageProblems.join('\n  '));
    } else {
      console.log(`ok   ${page.name}`);
    }
  }
  if (failed) process.exit(1);
  const n = only ? 1 : PAGES.length;
  console.log(`webui a11y test: ${n} page${n === 1 ? '' : 's'} pass axe-core and the a11y checks ` +
              `in ${checkedStates} states`);
}

main();
