// Smoke test for the molt:// WebUI pages under their strict CSP.
//
// Every page is served as HTML + /app.js with script-src 'self' (no
// 'unsafe-inline') and Trusted Types (see molt_webui_csp.h). A mistake
// there is a blank page or a dead button for users, and nothing short of a
// full Chromium build would show it — so this loads each page in jsdom the
// way Chromium would:
//
//   * the HTML and the app.js pieces are pulled out of the C++ sources;
//     the app.js composition is read from the data source's
//     IsAppJsRequest() branch, so the test can't drift from the code;
//   * inline <script> and on*= attributes never run (as under the CSP);
//   * Trusted Types are emulated: innerHTML/outerHTML/insertAdjacentHTML/
//     document.write throw on plain strings, and createPolicy() only
//     allows what the page's CSP allows (no policy, or 'molt-html' once);
//   * chrome.send() is stubbed and answers each request with a canned
//     response through cr.webUIResponse, like WebUIMessageHandler does.
//
// Then it checks: the script runs without throwing; no Trusted Types
// violation happens on load; every data-on-<event>="name" in the DOM and
// in the page script names a registered moltActions() function; the key
// controls of each page got a real event listener; and clicking every
// control in the initial DOM doesn't throw synchronously.
//
// Run: npm ci && node webui_smoke_test.js   (from tools/webui_checks)
'use strict';
const fs = require('fs');
const path = require('path');
const assert = require('assert');
const {JSDOM, VirtualConsole} = require('jsdom');

const WEBUI = path.join(__dirname, '../../src/chrome/browser/ui/webui/molt_ai');
const read = (f) => fs.readFileSync(path.join(WEBUI, f), 'utf8');

// All R"TAG(...)TAG" raw strings in a file, by constant name when named.
function rawStrings(src) {
  const out = {list: [], byName: {}};
  const re = /(?:(?:constexpr|const)\s+char\s+(\w+)\[\]\s*=\s*)?R"(HTML|JS)\(([\s\S]*?)\)\2"/g;
  let m;
  while ((m = re.exec(src))) {
    out.list.push({name: m[1] || null, kind: m[2], body: m[3]});
    if (m[1]) out.byName[m[1]] = m[3];
  }
  return out;
}

// The constants concatenated in the data source's IsAppJsRequest() branch.
function appJsParts(src) {
  // molt_ai_update_ui.cc (the CTO's reference page) picks with a ternary.
  const t = src.match(/url\.path\(\) == "\/app\.js" \? std::string\((k\w+)\)/);
  if (t) return [t[1]];
  const i = src.indexOf('IsAppJsRequest(url)) {');
  assert(i > 0, 'no IsAppJsRequest() branch');
  const branch = src.slice(i, src.indexOf('return;', i));
  return branch.match(/\b(?:molt_webui::|::molt_ai::|molt_ai::)?(k\w+)\b/g)
      .map((s) => s.replace(/^.*::/, ''))
      .filter((k) => k !== 'kAppJsPath');
}

function trustedTypesMode(src) {
  if (/TrustedTypesMode::kMoltHtmlPolicy/.test(src)) return 'molt-html';
  if (/TrustedTypesMode::kNoPolicies/.test(src)) return 'none';
  if (/"trusted-types;"/.test(src)) return 'none';  // update page
  throw new Error('page has no Trusted Types mode');
}

const PAGES = [
  {name: 'molt://home', file: 'molt_home_ui.cc',
   listeners: ['#searchForm:submit', '.card.ai:click', '.card.net:click']},
  {name: 'molt://memory', file: 'molt_ai_memory_ui.cc',
   listeners: ['#search-btn:click', '#forget-domain-btn:click',
               '#wipe-btn:click', '#q:keydown'],
   responses: {
     memStats: {docs: 2, chunks: 9},
     memListRecent: {docs: [
       {doc_id: 1, url: 'https://example.com/a"><img src=x>', title: '<b>t</b>',
        visited_at: 1700000000, word_count: 5},
       {doc_id: 2, url: 'javascript:alert(1)', title: 'js', visited_at: 0}]},
   },
   after(window) {
     const links = window.document.querySelectorAll('#results a[target=_blank]');
     assert.strictEqual(links.length, 1, 'only the http(s) result is a link');
     assert(/^https:\/\/example\.com\//.test(links[0].href), links[0].href);
     assert.strictEqual(window.document.querySelectorAll('#results img, #results b').length, 0,
                        'result text is not parsed as HTML');
   }},
  {name: 'molt://ai-update', file: 'molt_ai_update_ui.cc', listeners: []},
  {name: 'molt://import', file: 'molt_import_ui.cc',
   listeners: ['#tab-bm:click', '#tab-im:click', '#sw:click', '#go:click',
               '#manage:click', '#bmsearch:input'],
   responses: {
     getBookmarks: {bookmarks: [
       {title: '<img src=x onerror=alert(1)>', url: 'https://a.example/', host: 'a.example'}]},
     getImportableBrowsers: {browsers: [
       {id: 'chrome', display_name: 'Chrome', installed: true},
       {id: 'evil<svg>', display_name: '<b>x</b>', installed: false}]},
   },
   after(window) {
     const doc = window.document;
     assert(window.__sent.includes('getBookmarks'), 'bookmarks requested on load');
     assert.strictEqual(doc.querySelectorAll('#bmlist img[onerror], #bmlist b').length, 0);
     assert(doc.querySelector('#implist .flag svg'), 'browser logo rendered via moltHTML');
   }},
  {name: 'molt://ai-settings', file: 'molt_ai_settings_ui.cc',
   listeners: ['#maxTokens:input', '#temperature:input', '#backBtn:click',
               '#moltnetEnabled:change', '#routingMode:change', '#newCircuitBtn:click',
               '#exitCountry:change', '#importAllBtn:click', '#saveBtn:click',
               '#resetBtn:click'],
   noClick: ['#backBtn'],
   responses: {
     getSettings: {max_tokens: 512, temperature: 0.7, top_p: 0.9, top_k: 40},
     resetSettings: {max_tokens: 512, temperature: 0.7, top_p: 0.9, top_k: 40},
     getMoltnetExitCountries: {available: [{code: 'de', name: 'Germany'}], selected: ''},

     getImportableBrowsers: {browsers: [
       {id: 'chrome', display_name: '<img src=x>', installed: true, has_passwords_store: true},
       {id: 'firefox', display_name: 'Firefox', installed: true}]},
     getProviders: {providers: [
       {id: 'openai', display_name: 'OpenAI', default_base_url: 'https://api.openai.com/v1',
        custom_base_url: false, connected: true, base_url: '', enabled_models: ['gpt-x']},
       {id: 'custom', display_name: 'Custom', default_base_url: '', custom_base_url: true,
        connected: false, base_url: '', enabled_models: []}]},
     vaultList: {success: true, secure_storage: false, message: 'No system keyring found',
                 entries: [{id: 'a1', site_host: 'github.com', username: '<b>me</b>'},
                           {id: 'b2', site_host: 'example.com', username: 'x'}]},
     vaultRevealEntry: (args) => ({success: true, id: args[1], password: 'pw-' + args[1], notes: ''}),
     vaultAdd: {success: true, id: 'c3', site_host: 'new.example'},
   },
   listenersExtra: ['#vaultAddBtn:click', '#vaultGenBtn:click'],
   async after(window) {
     const doc = window.document;
     const tick = () => new Promise((r) => setTimeout(r, 10));
     assert.strictEqual(doc.querySelectorAll('#vaultList b').length, 0, 'usernames are text');
     assert.strictEqual(doc.getElementById('vaultStorageWarn').style.display, '', 'keyring warning shown');
     const shows = Array.from(doc.querySelectorAll('#vaultList button')).filter((b) => b.textContent === 'Show');
     assert.strictEqual(shows.length, 2);
     shows[0].click(); await tick();
     shows[1].click(); await tick();
     const revealed = Array.from(doc.querySelectorAll('#vaultList input')).filter((i) => i.value);
     assert.strictEqual(revealed.length, 1, 'at most one password on the page');
     assert.strictEqual(revealed[0].value, 'pw-b2');
     doc.getElementById('vaultGenBtn').click();
     assert.strictEqual(doc.getElementById('vaultPass').value.length, 20, 'generated password');
     const t = doc.getElementById('temperature');
     t.value = '1.3';
     t.dispatchEvent(new window.Event('input'));
     assert.strictEqual(doc.getElementById('tempVal').textContent, '1.3');
     assert.strictEqual(doc.querySelectorAll('#browserList img').length, 0, 'browser names are text');
     assert(doc.querySelector('#browserList svg'), 'browser logos rendered');
     window.cr.webUIListenerCallback('moltnet-status', {status: 'connected', relays: [
       {country: 'de', relay_id: '"><img src=x onerror=alert(1)>', ip: '1.2.3.4'}]});
     const circuit = doc.getElementById('circuitDisplay');
     assert.strictEqual(circuit.querySelectorAll('img').length, 0, 'relay id stays in the title');
     assert(/img src=x/.test(circuit.querySelector('[title]').getAttribute('title')));
   }},
  {name: 'molt://ai-agent', file: 'molt_ai_agent_ui.cc',
   responses: {
     listWorkflows: {workflows: [
       {id: 'w1', name: '<img src=x onerror=alert(1)>', steps: 3, trigger: {type: 'manual'}}]},
     getRuns: {runs: []},
   },
   after(window) {
     assert.strictEqual(window.document.querySelectorAll('#libList img[onerror]').length, 0,
                        'workflow names are text');
   }},
  {name: 'molt://ai-automation', file: 'molt_ai_automation_ui.cc',
   listeners: ['#new-blank-btn:click', '#sample-btn:click', '#import-btn:click',
               '#import-input:change', '#refresh-btn:click', '#search:input',
               '#audit-btn:click', '#modal-bg:click'],
   noClick: ['#import-btn'],
   responses: {
     listScripts: {scripts: [{
       id: 's1', name: '<img src=x onerror=alert(1)>',
       security: {trust: 'x" onmouseover="alert(1)'},
       trigger: {type: 'cron', expression: '0 9 * * *'},
       steps: [{type: 'click', target: '" autofocus onfocus="alert(1)', value: "'><b>"}],
       variables: {'a"b': 'c'},
       stats: {runs: 1, ai_tokens_total: '<i>9</i>',
               history: [{ok: true, dur_ms: 5, ts: 1, ai_tokens: '<i>x</i>'}]},
     }]},
     getAuditForScript: {entries: [{ts: '<b>t</b>', event: 'run_failed', extra: '<i>e</i>'}]},
     auditTail: {lines: ['a', 'b']},
   },
   after(window) {
     const doc = window.document;
     const card = doc.querySelector('#list > *');
     assert(card, 'script card rendered');
     card.querySelector('.script-summary').click();
     const bad = doc.querySelectorAll('#list img, #list i, #list b:not(.keep), [onmouseover], [onfocus]');
     const injected = Array.from(bad).filter((e) => /^(IMG|I)$/.test(e.tagName) ||
         e.hasAttribute('onmouseover') || e.hasAttribute('onfocus'));
     assert.strictEqual(injected.length, 0, 'script data stays text: ' +
         injected.map((e) => e.outerHTML).join(' '));
   }},
  {name: 'molt://ai', file: 'molt_ai_ui.cc',
   listeners: ['#modelChip:click', '#sendBtn:click', '#cancelBtn:click',
               '#searchInput:input', '#modelOverlay:click', '#welcomeDownloadBtn:click'],
   noClick: ['#welcomeDownloadBtn'],
   responses: {
     initChat: {model_loaded: false, is_first_run: false, total_ram_gb: 16, cpu_cores: 8,
                models: [{model_id: 'm1', is_downloaded: true}]},
     getModelStatus: {models: [
       {model_id: 'm1" data-x="1', display_name: '<b>bold</b>', company: 'Meta',
        is_downloaded: true, file_size_mb: 100},
       {model_id: 'm2', display_name: 'Two', company: '<i>Co</i>', file_size_mb: 2048},
       {model_id: 'openai:<x>', is_cloud: true, provider: 'OpenAI', display_name: '<img src=x>'}]},
     sendPrompt: {success: true},
   },
   after(window) {
     const doc = window.document;
     window.startAiMsg();
     window.appendTok('**hi** <img src=x onerror=alert(1)> ```js\nconsole.log(1)\n```');
     window.finishAiMsg();
     const last = doc.querySelector('#messages .message:last-child');
     assert(last.querySelector('strong'), 'markdown rendered');
     assert.strictEqual(last.querySelectorAll('img').length, 0, 'model output stays text');
     const copy = last.querySelector('button.code-copy');
     assert(copy && copy.getAttribute('data-on-click') === 'copyCode', 'copy button wired');
     window.toggleModelPanel();
     return new Promise((r) => setTimeout(r, 20)).then(() => {
       assert.strictEqual(doc.querySelectorAll('#modelList b, #modelList i, [data-x]').length, 0,
                          'model metadata stays text');
       const load = doc.querySelector('#modelList [data-on-click=loadModel]');
       assert(load && load.getAttribute('data-model-id') === 'm1" data-x="1', 'model id round-trips');
       const input = doc.getElementById('searchInput');
       input.value = 'hi';
       input.dispatchEvent(new window.Event('input'));
       assert(doc.querySelector('#messages .highlight'), 'search highlights text');
     });
   }},
  {name: 'molt://ai-chat', file: 'molt_ai_chat_ui.cc',
   extraFiles: ['molt_webui_csp.h', 'molt_ai_chat_ui_js.h'],
   preScripts: ['kChatThemeJs'],
   listeners: ['#sendBtn:click', '#modelChip:click', '#modeChip:click', '#micBtn:click',
               '#attachBtn:click', '#cancelBtn:click', '#drawerSearch:input',
               '#searchInput:input', '#drawerScrim:click', '#agentBtn:click'],
   noClick: ['#attachBtn', '#welcomeDownloadBtn', '#micBtn'],
   responses: {
     initChat: {model_loaded: false, is_first_run: false, total_ram_gb: 16, cpu_cores: 8,
                models: [{model_id: 'm1', is_downloaded: true, display_name: 'M1'}]},
     getModelStatus: {models: [
       {model_id: 'm1" data-x="1', display_name: '<b>bold</b>', company: 'Meta',
        is_downloaded: true, file_size_mb: 100},
       {model_id: 'openai:<x>', is_cloud: true, provider: 'OpenAI', display_name: '<img src=x>'}]},
     listConversations: {success: true, conversations: [
       {id: 'c1', title: '<img src=x onerror=alert(1)>', updated_at: 1}]},
     vaultList: {entries: [{id: 'abcdef1234567890', site_host: '<b>gh</b>', username: 'me'}]},
     vaultFindForActive: {host: 'github.com', fillable: true, matches: [
       {id: 'abcdef1234567890', site_host: 'gist.github.com', username: 'me', match: 'same_site'}]},
     vaultAutofill: {success: false, cancelled: true},
     openMoltSettings: {success: true},
     sendPrompt: {success: true},
   },
   async after(window) {
     const doc = window.document;
     const tick = () => new Promise((r) => setTimeout(r, 15));
     window.startAiMessage();
     window.appendToken('**hi** <img src=x onerror=alert(1)> [l](https://ex.com/"q) ```js\nx()\n```');
     window.finishAiMessage();
     const last = doc.querySelector('#messages .message.ai:last-child');
     assert(last.querySelector('strong'), 'markdown rendered');
     assert.strictEqual(last.querySelectorAll('img').length, 0, 'model output stays text');
     const copy = last.querySelector('button.code-copy');
     assert(copy && /^cb-\d+$/.test(copy.getAttribute('data-copy-target')), 'copy button wired');
     copy.click();
     last.querySelector('[data-on-click=copyResponse]').click();
     // Recents drawer with a hostile title.
     window.toggleDrawer();
     await tick();
     assert.strictEqual(doc.querySelectorAll('#convList img').length, 0, 'chat titles stay text');
     // /vault commands never carry a password into the page.
     const input = doc.getElementById('chatInput');
     for (const cmd of ['/vault list', '/vault find', '/vault fill', '/vault add a.com me hunter2']) {
       input.value = cmd;
       window.sendMessage();
       await tick();
     }
     assert(!/hunter2/.test(doc.getElementById('messages').textContent),
            '/vault add never echoes the password');
     assert(window.__sent.includes('openMoltSettings'), '/vault add opens Settings');
     assert(!window.__sent.includes('vaultAdd'), 'chat never sends vaultAdd');
     // Tab-context contract still installed (from app.js now).
     window.__moltSetTabContext({url: 'https://example.com/a.pdf', title: 'Doc (PDF)'});
     assert.strictEqual(doc.querySelector('#tabContext .tab-context-icon').textContent, '\u{1F4C4}');
     // Model dropdown + panel render hostile metadata as text.
     window.toggleModelPanel();
     await tick();
     assert.strictEqual(doc.querySelectorAll('#modelList b, [data-x]').length, 0, 'model metadata stays text');
   }},
];

function makeTrustedTypes(window, mode, violations) {
  class TrustedHTML {
    constructor(s) { this.s = s; }
    toString() { return this.s; }
  }
  const created = new Set();
  window.TrustedHTML = TrustedHTML;
  window.trustedTypes = {
    createPolicy(name, rules) {
      if (mode === 'none' || name !== 'molt-html' || created.has(name)) {
        const msg = `Trusted Types: createPolicy('${name}') is not allowed by the CSP`;
        violations.push(msg);
        throw new window.TypeError(msg);
      }
      created.add(name);
      return {createHTML: (s) => new TrustedHTML(rules.createHTML(s))};
    },
  };
  const guard = (label, v) => {
    if (v instanceof TrustedHTML || window.__ttOff) return String(v);
    const msg = `Trusted Types violation: ${label} assigned a plain string`;
    violations.push(msg + ': ' + JSON.stringify(String(v)).slice(0, 80));
    throw new window.TypeError(msg);
  };
  for (const proto of [window.Element.prototype, window.ShadowRoot.prototype]) {
    for (const prop of ['innerHTML', 'outerHTML']) {
      const d = Object.getOwnPropertyDescriptor(proto, prop);
      if (!d || !d.set) continue;
      Object.defineProperty(proto, prop, {
        configurable: true, enumerable: d.enumerable, get: d.get,
        set(v) { d.set.call(this, guard(prop, v)); },
      });
    }
  }
  const iah = window.Element.prototype.insertAdjacentHTML;
  window.Element.prototype.insertAdjacentHTML = function(where, v) {
    return iah.call(this, where, guard('insertAdjacentHTML', v));
  };
  const write = window.document.write;
  window.document.write = function(v) { return write.call(this, guard('document.write', v)); };
}

async function runPage(page) {
  const src = read(page.file);
  const raw = rawStrings(src);
  const htmlEntry = raw.list.find((r) => r.kind === 'HTML');
  assert(htmlEntry, page.name + ': no HTML');
  const html = htmlEntry.body;
  assert(!/<script(?![^>]*\bsrc=)[^>]*>\s*\S/i.test(html), page.name + ': inline <script> left');
  assert(/<script src="app\.js"><\/script>/.test(html), page.name + ': no <script src="app.js">');

  const constants = Object.assign({}, raw.byName);
  for (const f of page.extraFiles || ['molt_webui_csp.h']) Object.assign(constants, rawStrings(read(f)).byName);
  const parts = (page.preScripts || []).concat(appJsParts(src));
  const js = parts.map((p) => {
    assert(p in constants, `${page.name}: app.js part ${p} not found`);
    return constants[p];
  }).join('');

  const errors = [];
  const violations = [];
  const vc = new VirtualConsole();
  vc.on('jsdomError', (e) => {
    if (/Not implemented: (navigation|window\.scrollTo|HTMLMediaElement)/.test(e.message)) return;
    errors.push('jsdomError: ' + e.message + (e.detail ? ' ' + (e.detail.stack || e.detail) : ''));
  });
  const dom = new JSDOM(html, {runScripts: 'outside-only', pretendToBeVisual: true,
                               url: 'chrome://' + page.name.slice(7) + '/', virtualConsole: vc});
  const {window} = dom;
  makeTrustedTypes(window, trustedTypesMode(src), violations);

  const listened = new WeakMap();
  const ael = window.EventTarget.prototype.addEventListener;
  window.EventTarget.prototype.addEventListener = function(type, fn, opts) {
    if (!listened.has(this)) listened.set(this, new Set());
    listened.get(this).add(type);
    return ael.call(this, type, fn, opts);
  };
  window.confirm = () => false;
  window.alert = () => {};
  window.prompt = () => null;
  window.scrollTo = () => {};
  Object.defineProperty(window.navigator, 'clipboard', {configurable: true,
      value: {writeText: () => Promise.resolve(), readText: () => Promise.resolve('')}});
  window.HTMLElement.prototype.scrollIntoView = function() {};
  window.matchMedia = window.matchMedia || (() => ({matches: false, addListener() {}, addEventListener() {}}));

  const sent = [];
  window.__sent = sent;
  const responses = page.responses || {};
  window.chrome = {send(method, args) {
    sent.push(method);
    const id = args && args[0];
    if (typeof id !== 'string') return;
    let r = responses[method];
    if (typeof r === 'function') r = r(args);
    setTimeout(() => {
      if (window.cr && typeof window.cr.webUIResponse === 'function') {
        window.cr.webUIResponse(id, true, r === undefined ? {} : r);
      }
    }, 0);
  }};
  window.addEventListener('error', (e) => errors.push('error event: ' + e.message));
  const rejections = [];
  const onRej = (reason) => rejections.push(String(reason && reason.stack || reason));
  process.on('unhandledRejection', onRej);

  try {
    window.eval(js);
  } catch (e) {
    errors.push('app.js threw on load: ' + (e.stack || e));
  }
  await new Promise((r) => setTimeout(r, 60));

  // data-on-* names must all be registered.
  const doc = window.document;
  const used = new Set();
  for (const el of doc.querySelectorAll('*')) {
    for (const a of el.getAttributeNames()) {
      if (a.startsWith('data-on-')) used.add(el.getAttribute(a));
    }
  }
  for (const m of js.matchAll(/data-on-[a-z]+=\\?["']([A-Za-z_$][\w$]*)/g)) used.add(m[1]);
  if (used.size) {
    assert(typeof window.moltActions === 'function', page.name + ': data-on-* used but no moltActions');
    for (const name of used) {
      if (!window.moltActions.has(name)) errors.push(`data-on-* names unregistered action "${name}"`);
    }
  }
  // Key controls got a listener.
  for (const spec of (page.listeners || []).concat(page.listenersExtra || [])) {
    const [sel, type] = spec.split(':');
    const el = doc.querySelector(sel);
    if (!el) { errors.push(`listener check: ${sel} not found`); continue; }
    const types = listened.get(el);
    if (!types || !types.has(type)) errors.push(`${sel} has no ${type} listener`);
  }
  if (page.after) {
    try { await page.after(window); } catch (e) { errors.push('after(): ' + (e.stack || e)); }
  }
  // Click every clickable control in the initial DOM; nothing may throw.
  if (!page.skipClickAll) {
    // Everything that has a click listener (direct or via moltActions).
    const clickable = Array.from(doc.querySelectorAll('*')).filter((el) => {
      const t = listened.get(el);
      return t && t.has('click');
    });
    for (const el of clickable) {
      if ((page.noClick || []).some((sel) => el.matches(sel))) continue;
      try { el.click(); } catch (e) { errors.push(`click ${el.outerHTML.slice(0, 60)}: ${e}`); }
    }
    await new Promise((r) => setTimeout(r, 60));
  }
  process.off('unhandledRejection', onRej);
  const problems = errors.concat(violations, rejections.map((r) => 'unhandled rejection: ' + r));
  window.__ttOff = true;  // jsdom's own teardown clears the body
  window.close();
  return {problems, sent};
}

(async () => {
  let failed = 0;
  for (const page of PAGES) {
    const {problems, sent} = await runPage(page);
    if (problems.length) {
      failed++;
      console.error(`FAIL ${page.name}\n  ` + problems.join('\n  '));
    } else {
      console.log(`ok   ${page.name} (${sent.length} chrome.send calls)`);
    }
  }
  if (failed) process.exit(1);
  console.log(`webui smoke test: ${PAGES.length} pages load under strict CSP + Trusted Types`);
})();
