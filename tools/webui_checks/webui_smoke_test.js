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

// promoteTrust calls the Agent studio makes ([id, level] each).
const agentTrustCalls = [];

// A draft saved from an AI agent run (docs/design/AGENT_ACTIONS.md §2.9), as
// getWorkflow returns it, and the confirmDraftSites calls (JSON of [id,
// sites]).
const DRAFT_WORKFLOW = {
  id: 'd1', name: 'Buy socks', variables: {pin: ''},
  security: {trust: 'casual', sites_unconfirmed: true},
  draftSites: ['shop.example', '<b>evil</b>.example', 7, ''],
  steps: [
    {type: 'navigate', target: 'https://shop.example/', description: 'Open shop.example'},
    {type: 'type', target: '#pin', value: '{{pin}}', description: 'Type your “PIN” (asked at each run)',
     extra: {review: 'value_withheld', text: 'PIN', host: 'shop.example'}},
    {type: 'type', target: '#cc', description: 'Enter the payment card yourself (not saved)',
     extra: {review: 'secret_field', secret: 'vault', secret_kind: 'payment_card'}},
    {type: 'click', target: '#x', description: 'Click “<img src=x onerror=alert(1)>”',
     extra: {review: '__proto__', text: '<img src=x onerror=alert(1)>'}},
  ],
};
const draftConfirmCalls = [];

// URLs the model pickers asked the browser to open (openUrlInTab).
const openedUrls = [];

// saveMoltProfile calls from the /profile editor ([dict, options] each).
const profileSaves = [];

// The molt://ai-chat reader commands (A7): the active tab shows
// example.com/b (entry 2); getPageContent reads it fresh; sendPrompt
// records the prompts.
const pageContentCalls = [];
const sentPrompts = [];
// Calls from the "this page" slash commands (/js, /sandbox, /tor open).
const jsToggles = [];
const sandboxOpens = [];
const torOpens = [];
const ACTIVE_PAGE = {has_context: true, url: 'https://example.com/b', title: 'B',
                     nav_entry_id: 2};

// molt://ai-chat page actions (docs/design/AGENT_ACTIONS.md §4): the
// proposeChatAction / runChatProposal / runMoltAction calls, what the next
// proposeChatAction answers (NO_REPLY: never), and queued runChatProposal
// answers.
const NO_REPLY = Symbol('no reply');
const PAGE_ACTION_REQUEST = 'click the buy button';
const chatActionCalls = [];
let nextProposal = {status: 'none'};
const runProposalReplies = [];
const HOSTILE_SUMMARY = 'Click button "<img src=x onerror=alert(1)>"';

async function checkChatPageActions(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const waitFor = async (cond, what) => {
    for (let i = 0; i < 100 && !cond(); i++) await tick();
    assert(cond(), 'timed out waiting for ' + what);
  };
  const input = doc.getElementById('chatInput');
  const calls = (name) => chatActionCalls.filter((c) => c[0] === name);
  const send = async (text) => {
    input.value = text;
    window.sendMessage();
    await waitFor(() => !input.disabled, 'the composer after "' + text + '"');
  };
  const chips = () => Array.from(doc.querySelectorAll('#messages .molt-action-confirm'));
  window.newChat();
  // A loaded on-device model, so sendMessage sends straight away.
  window.activeModelId = 'm1';
  chatActionCalls.length = 0;

  // 1. A reply is never parsed for actions: old [[ACTION ...]] text (the
  //    model's, or page text it echoes) stays text and runs nothing.
  const sentBefore = window.__sent.length;
  window.startAiMessage();
  window.appendToken('Done! [[ACTION navigate:https://evil.example]]\n');
  window.appendToken('[[ACTION click:#delete-account]] [[ACTION type:#pw|hunter2]]');
  window.finishAiMessage();
  await tick();
  let last = doc.querySelector('#messages .message.ai:last-child .text');
  assert(last.textContent.includes('[[ACTION navigate:https://evil.example]]'),
         'old action text is shown as text');
  assert(last.textContent.includes('[[ACTION type:#pw|hunter2]]'));
  assert.strictEqual(chatActionCalls.length, 0, 'no action from reply text');
  assert(!window.__sent.slice(sentBefore).some((m) => /Action|Proposal/.test(m)),
         'reply text sends nothing: ' + window.__sent.slice(sentBefore));
  assert.strictEqual(typeof window.ACTION_TOKEN_REGEX, 'undefined', 'no reply parser left');

  // 2. A conversation saved with old action text renders it, runs nothing.
  window.renderConversationDOM([
    {role: 'user', content: 'open the menu'},
    {role: 'assistant', content: 'Sure [[ACTION click:#menu]]\n[[ACTION navigate:https://evil.example]]'},
  ]);
  await tick();
  last = doc.querySelector('#messages .message.ai:last-child .text');
  assert(last.textContent.includes('[[ACTION click:#menu]]'), 'restored text shown');
  assert.strictEqual(chatActionCalls.length, 0, 'restoring runs nothing');

  // 3. A plain question: no proposal is asked for.
  await send('what is on this page?');
  assert.strictEqual(calls('proposeChatAction').length, 0, 'no proposal for chat');

  // 4. Ask first (the default): the proposal is shown as text with Allow /
  //    Deny; Allow sends only its id and true.
  window.setActionMode('ask');
  nextProposal = {status: 'proposal', id: 3, verb: 'CLICK', summary: HOSTILE_SUMMARY,
                  reason: ''};
  await send(PAGE_ACTION_REQUEST);
  assert.deepStrictEqual(calls('proposeChatAction'), [['proposeChatAction', 7]]);
  await waitFor(() => chips().length === 1, 'the proposal chip');
  assert.strictEqual(chips()[0].querySelector('.ac-label').textContent, HOSTILE_SUMMARY);
  assert.strictEqual(chips()[0].querySelector('.ac-title').textContent,
                     'AI proposes a page action');
  assert.strictEqual(doc.querySelectorAll('#messages img').length, 0, 'summary is text');
  assert.strictEqual(calls('runChatProposal').length, 0, 'nothing runs before Allow');
  assert(!doc.querySelector('#messages .molt-navigating-chip'), 'status row removed');
  chips()[0].querySelector('.ac-allow').click();
  await waitFor(() => calls('runChatProposal').length === 1, 'runChatProposal');
  assert.deepStrictEqual(calls('runChatProposal')[0], ['runChatProposal', 3, true]);
  await waitFor(() => doc.querySelector('#messages .molt-action-result.ok'), 'result row');
  assert.strictEqual(chips().length, 0);

  // 5. Auto: runs at once (not confirmed); when the browser asks, the
  //    reason is shown; Deny sends nothing more.
  window.setActionMode('auto');
  chatActionCalls.length = 0;
  runProposalReplies.push({success: false, needs_confirmation: true,
                           reason: 'It opens https://evil.example<b>x</b>'});
  nextProposal = {status: 'proposal', id: 4, verb: 'CLICK', summary: 'Click link "Docs"',
                  reason: ''};
  await send(PAGE_ACTION_REQUEST);
  await waitFor(() => chips().length === 1, 'the confirmation chip');
  assert.deepStrictEqual(calls('runChatProposal'), [['runChatProposal', 4, false]]);
  assert.strictEqual(chips()[0].querySelector('.ac-title').textContent,
                     'Confirm: It opens https://evil.example<b>x</b>');
  assert.strictEqual(doc.querySelectorAll('#messages b').length, 0, 'reason is text');
  chips()[0].querySelector('.ac-deny').click();
  await tick();
  assert.strictEqual(calls('runChatProposal').length, 1, 'Deny runs nothing');
  assert(/denied by user/.test(doc.getElementById('messages').textContent));

  // 6. Auto, but the browser gives a reason up front: asked before anything.
  chatActionCalls.length = 0;
  nextProposal = {status: 'proposal', id: 5, verb: 'NAVIGATE',
                  summary: 'Open https://evil.example/', reason: 'Opens a different site: https://evil.example'};
  await send(PAGE_ACTION_REQUEST);
  await waitFor(() => chips().length === 1, 'the up-front chip');
  assert.strictEqual(calls('runChatProposal').length, 0, 'nothing runs before Allow');
  chips()[0].querySelector('.ac-allow').click();
  await waitFor(() => calls('runChatProposal').length === 1, 'runChatProposal');
  assert.deepStrictEqual(calls('runChatProposal')[0], ['runChatProposal', 5, true]);

  // 6b. A site tool, still in Auto mode: the browser always gives a reason,
  //     so the chip comes first; Allow sends only the id and true; the
  //     summary and the site's answer are text.
  chatActionCalls.length = 0;
  const TOOL_SUMMARY = 'Run the site\'s tool "search" in the frame ' +
      'https://shop.example/w with q = "<b>shoes</b>"';
  const TOOL_REASON = 'Runs the site\'s tool "search" on https://shop.example ' +
      '(the site says it only reads)';
  runProposalReplies.push({success: true, message:
      'The site\'s tool answered: "<img src=x onerror=alert(2)>"'});
  nextProposal = {status: 'proposal', id: 6, verb: 'CALL_TOOL',
                  summary: TOOL_SUMMARY, reason: TOOL_REASON};
  await send(PAGE_ACTION_REQUEST);
  await waitFor(() => chips().length === 1, 'the tool chip');
  assert.strictEqual(calls('runChatProposal').length, 0,
                     'no tool runs before Allow, in Auto mode too');
  assert.strictEqual(chips()[0].querySelector('.ac-label').textContent, TOOL_SUMMARY);
  assert.strictEqual(chips()[0].querySelector('.ac-title').textContent,
                     'Confirm: ' + TOOL_REASON);
  assert.strictEqual(doc.querySelectorAll('#messages b').length, 0, 'summary is text');
  chips()[0].querySelector('.ac-allow').click();
  await waitFor(() => calls('runChatProposal').length === 1, 'runChatProposal');
  assert.deepStrictEqual(calls('runChatProposal')[0], ['runChatProposal', 6, true]);
  await waitFor(() => /The site's tool answered/.test(
      doc.getElementById('messages').textContent), 'the tool\'s answer');
  assert.strictEqual(doc.querySelectorAll('#messages img').length, 0,
                     'the answer is text');

  // 7. No proposal possible: the browser's message, as text.
  nextProposal = {status: 'unavailable', message: 'Page actions are chosen by the <i>on-device</i> model'};
  await send(PAGE_ACTION_REQUEST);
  await waitFor(() => /chosen by the <i>on-device<\/i> model/.test(
      doc.getElementById('messages').textContent), 'the unavailable note');
  assert.strictEqual(doc.querySelectorAll('#messages i').length, 0);

  // 8. Stop while the proposal is being chosen: the composer is usable
  //    again without waiting for the browser.
  nextProposal = NO_REPLY;
  input.value = PAGE_ACTION_REQUEST;
  window.sendMessage();
  await waitFor(() => input.disabled &&
                    doc.querySelector('#messages .molt-navigating-chip'), 'choosing');
  window.cancelGeneration();
  await waitFor(() => !input.disabled, 'the composer after Stop');
  assert(!doc.querySelector('#messages .molt-navigating-chip'), 'status row gone');
  assert(window.__sent.includes('cancelGeneration'));

  // 9. The user's own slash command still goes to runMoltAction, confirmed.
  chatActionCalls.length = 0;
  await send('/click #buy');
  assert.strictEqual(JSON.stringify(calls('runMoltAction')),
                     '[["runMoltAction",{"type":"click","selector":"#buy","confirmed":true}]]');
  assert.strictEqual(calls('proposeChatAction').length, 0);
  window.setActionMode('ask');
  nextProposal = {status: 'none'};
}

// molt://ai-chat /fill ai and /paste (docs/design/AGENT_ACTIONS.md §4.7):
// the browser proposes a form fill and describes every value; the page
// sends only the command's source, the user's words and the pasted text,
// never a selector or a value, and nothing runs before Allow, in either
// mode. What the next proposeFormFill answers (NO_REPLY: never).
let nextFormFill = {status: 'none', message: 'Nothing matched.'};
const FILL_REASON = 'Fills 2 fields on https://shop.example with your saved profile';
const FILL_SUMMARY = 'Fill 2 fields: input[email] "Email" = "<img src=x onerror=alert(1)>"; ' +
                     'textarea "Address" = "1 Main St"';
async function checkChatFormFill(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const waitFor = async (cond, what) => {
    for (let i = 0; i < 100 && !cond(); i++) await tick();
    assert(cond(), 'timed out waiting for ' + what);
  };
  const input = doc.getElementById('chatInput');
  const calls = (name) => chatActionCalls.filter((c) => c[0] === name);
  const send = async (text) => {
    input.value = text;
    window.sendMessage();
    await waitFor(() => !input.disabled, 'the composer after "' + text + '"');
  };
  const chips = () => Array.from(doc.querySelectorAll('#messages .molt-action-confirm'));
  const setClipboard = (text) => Object.defineProperty(window.navigator, 'clipboard', {
    configurable: true,
    value: {writeText: () => Promise.resolve(), readText: () => Promise.resolve(text)},
  });
  const sentBefore = window.__sent.length;
  const promptsBefore = sentPrompts.length;
  window.newChat();
  chatActionCalls.length = 0;

  // 1. /fill ai (Auto mode): the browser's proposal is shown with Allow /
  //    Deny anyway; Allow sends only the id and true.
  window.setActionMode('auto');
  nextFormFill = {status: 'proposal', id: 11, verb: 'FILL_FORM', summary: FILL_SUMMARY,
                  reason: FILL_REASON};
  await send('/fill ai use my work email');
  assert.strictEqual(JSON.stringify(calls('proposeFormFill')),
                     '[["proposeFormFill","profile","use my work email",""]]');
  assert.strictEqual(calls('runFormFill').length, 0, '/fill ai is not the plain /fill');
  await waitFor(() => chips().length === 1, 'the form-fill chip');
  assert.strictEqual(chips()[0].querySelector('.ac-title').textContent, 'Confirm: ' + FILL_REASON);
  assert.strictEqual(chips()[0].querySelector('.ac-label').textContent, FILL_SUMMARY);
  assert.strictEqual(doc.querySelectorAll('#messages img').length, 0, 'values are text');
  assert.strictEqual(calls('runChatProposal').length, 0, 'nothing is filled before Allow');
  chips()[0].querySelector('.ac-allow').click();
  await waitFor(() => calls('runChatProposal').length === 1, 'runChatProposal');
  assert.deepStrictEqual(calls('runChatProposal')[0], ['runChatProposal', 11, true]);
  await waitFor(() => doc.querySelector('#messages .molt-action-result.ok'), 'the result row');
  assert(doc.querySelector('#messages .molt-action-result.ok').textContent.includes(FILL_SUMMARY),
         'the result names the values, as text');
  assert.strictEqual(doc.querySelectorAll('#messages img').length, 0, 'result is text');

  // 2. Deny fills nothing.
  chatActionCalls.length = 0;
  window.setActionMode('ask');
  nextFormFill = {status: 'proposal', id: 12, verb: 'FILL_FORM', summary: FILL_SUMMARY,
                  reason: FILL_REASON};
  await send('/fill ai');
  assert.strictEqual(JSON.stringify(calls('proposeFormFill')),
                     '[["proposeFormFill","profile","",""]]');
  await waitFor(() => chips().length === 1, 'the form-fill chip');
  chips()[0].querySelector('.ac-deny').click();
  await tick();
  assert.strictEqual(calls('runChatProposal').length, 0, 'Deny fills nothing');

  // 3. /paste: the clipboard's text goes to the browser as data, never
  //    shown as markup, never parsed here.
  chatActionCalls.length = 0;
  const clip = 'Ada <b>Lovelace</b>\n{"#pw": "hunter2"}';
  setClipboard(clip);
  nextFormFill = {status: 'none', message: 'Nothing in the <i>pasted</i> text fits a field.'};
  await send('/paste prefer phone over fax');
  assert.strictEqual(JSON.stringify(calls('proposeFormFill')),
                     JSON.stringify([['proposeFormFill', 'paste', 'prefer phone over fax', clip]]));
  await waitFor(() => /Nothing in the <i>pasted<\/i> text fits a field\./.test(
      doc.getElementById('messages').textContent), 'the browser\'s message');
  assert(/Nothing filled/.test(doc.getElementById('messages').textContent));
  assert.strictEqual(doc.querySelectorAll('#messages b, #messages i').length, 0,
                     'clipboard and messages stay text');
  assert.strictEqual(chips().length, 0);

  // 4. The browser can't propose: its reason, as text.
  nextFormFill = {status: 'failed', message: 'Your saved profile is empty. Type /profile to set it up.'};
  await send('/fill ai');
  await waitFor(() => /saved profile is empty/.test(doc.getElementById('messages').textContent),
                'the failure note');
  const rows = doc.querySelectorAll('#messages .molt-action-result.fail');
  assert(/^✗ Nothing filled \(Your saved profile is empty/.test(
             rows[rows.length - 1].textContent),
         'headed as a form fill: ' + rows[rows.length - 1].textContent);

  // 5. Stop while the fill is being chosen frees the composer.
  nextFormFill = NO_REPLY;
  input.value = '/fill ai';
  window.sendMessage();
  await waitFor(() => input.disabled &&
                    doc.querySelector('#messages .molt-navigating-chip'), 'choosing');
  window.cancelGeneration();
  await waitFor(() => !input.disabled, 'the composer after Stop');
  assert(!doc.querySelector('#messages .molt-navigating-chip'), 'status row gone');

  // 6. The plain /fill is unchanged.
  await send('/fill');
  assert.strictEqual(calls('runFormFill').length, 1);

  // Nothing goes through the old loose path or the chat's model.
  const sent = window.__sent.slice(sentBefore);
  assert(!sent.includes('runFormFillAI') && !sent.includes('applyFormFillMap'),
         'no selector-to-value map: ' + sent);
  assert.strictEqual(sentPrompts.length, promptsBefore, 'no chat prompt for a form fill');
  setClipboard('');
  window.setActionMode('ask');
  nextFormFill = {status: 'none', message: 'Nothing matched.'};
}

// molt://ai-chat's agent bar and its plan (docs/design/AGENT_ACTIONS.md
// §2.8): the steps are model text, written from page text, so they render
// as text only; the current step is marked; a rewrite, the "no plan" note
// and junk events don't break the bar; nothing is shown once the run is
// over, and a new run starts without the old plan. The goals the page
// sent to startWebAgent (which never answers here: the run stays open).
const agentGoals = [];
const HOSTILE_PLAN_STEP = '<img src=x onerror=alert(1)>Search flights';
async function checkChatAgentPlan(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const fire = (event, value) => window.cr.webUIListenerCallback(event, value);
  const items = () => Array.from(doc.querySelectorAll('#agentPlan li'));
  const title = () => doc.querySelector('#agentPlan .agent-plan-title');
  const steps = [
    {goal: HOSTILE_PLAN_STEP, done_when: 'results <b>listed</b>'},
    {goal: 'Sort by price', done_when: 'cheapest first'},
  ];

  window.startWebAgent('Find the cheapest flight');
  await tick();
  assert.deepStrictEqual(agentGoals, ['Find the cheapest flight']);
  const plan = doc.getElementById('agentPlan');
  assert(plan, 'the bar has a plan box');
  assert(!plan.classList.contains('shown'), 'no plan before one is written');

  // The first plan: text only, the first step current.
  fire('agent-plan', {event: 'planned', steps, current: 1, replans: 0, note: ''});
  await tick();
  assert(plan.classList.contains('shown'));
  assert.strictEqual(title().textContent, 'PLAN');
  assert.strictEqual(items().length, 2);
  assert.strictEqual(items()[0].querySelector('.agent-plan-goal').textContent,
                     HOSTILE_PLAN_STEP);
  assert.strictEqual(items()[0].querySelector('.agent-plan-check').textContent,
                     'Done when: results <b>listed</b>');
  assert(items()[0].classList.contains('current'));
  assert.strictEqual(items()[0].getAttribute('aria-current'), 'step');
  assert(!items()[1].classList.contains('current'));
  assert.strictEqual(doc.querySelectorAll('#agentBar img, #agentBar b').length, 0,
                     'plan text stays text');

  // The next step.
  fire('agent-plan', {event: 'advanced', steps, current: 2, replans: 0, note: ''});
  await tick();
  assert(items()[0].classList.contains('done'));
  assert(items()[1].classList.contains('current'));
  assert.strictEqual(items()[0].getAttribute('aria-current'), null);

  // A step row of a REPLAN, as text.
  fire('agent-step', {number: 3, action: 'REPLAN', target: 'plan step 2',
                      reason: '<b>the page changed</b>', success: true, plan_step: 2});
  await tick();
  const rows = doc.querySelectorAll('#agentSteps .agent-step');
  assert.strictEqual(rows[rows.length - 1].querySelector('.agent-step-badge').textContent,
                     'REPLAN');
  assert.strictEqual(doc.querySelectorAll('#agentBar b').length, 0);

  // A rewritten plan replaces the old one.
  fire('agent-plan', {event: 'replanned', current: 1, replans: 1, note: '',
                      steps: [{goal: 'Open the cart', done_when: 'the cart is open'}]});
  await tick();
  assert.strictEqual(title().textContent, 'PLAN (rewritten once)');
  assert.strictEqual(items().length, 1);
  fire('agent-plan', {event: 'replanned', current: 1, replans: 2, note: '',
                      steps: [{goal: 'x', done_when: 'y'}]});
  await tick();
  assert.strictEqual(title().textContent, 'PLAN (rewritten 2 times)');

  // No usable plan: the browser's note, as text.
  fire('agent-plan', {event: 'none', steps: [], current: 0, replans: 2,
                      note: 'The model couldn\'t write a <i>usable</i> plan'});
  await tick();
  assert.strictEqual(title().textContent, 'NO PLAN');
  assert.strictEqual(items().length, 0);
  assert.strictEqual(plan.querySelector('.agent-plan-note').textContent,
                     'The model couldn\'t write a <i>usable</i> plan');
  assert.strictEqual(doc.querySelectorAll('#agentBar i').length, 0);

  // Junk doesn't break the bar; at most six steps are shown.
  for (const junk of [null, 'x', 7, {steps: 'nope'}, {steps: [null, 5, {goal: {}}]},
                      {steps: Array(20).fill({goal: 'g', done_when: 'd'}), current: 'x'}]) {
    fire('agent-plan', junk);
    await tick();
  }
  assert.strictEqual(items().length, 6);
  assert.strictEqual(doc.querySelectorAll('#agentPlan .current').length, 0);

  // Once the run is over, nothing more is shown; a new run starts clean.
  doc.getElementById('agentStopBtn').click();
  await tick();
  assert(window.__sent.includes('cancelWebAgent'));
  fire('agent-plan', {event: 'planned', steps, current: 1, replans: 0, note: ''});
  await tick();
  assert.strictEqual(items().length, 6, 'a finished run ignores plan events');
  window.startWebAgent('Again');
  await tick();
  assert(!plan.classList.contains('shown'), 'a new run has no plan yet');
  assert.strictEqual(items().length, 0);
  doc.getElementById('agentStopBtn').click();
  await tick();
}

// molt://ai-chat's "Save as automation" (docs/design/AGENT_ACTIONS.md §2.9):
// offered only after a run that ended with DONE and that the browser says
// can be saved (save_run); otherwise the browser's note, as text; saving is
// sent from the click; the outcome is text, with "Review in Agent mode"
// only after a save; junk offers nothing; a new run clears the old offer.
// Queued answers to startWebAgent (empty: it never answers), the
// saveAgentRun calls ([run_id] each) and the next answer to one.
const agentRunReplies = [];
const agentSaveCalls = [];
let nextAgentSave = {success: false, error: 'not set'};
async function checkChatAgentSave(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const box = () => doc.getElementById('agentSave');
  const labels = () => Array.from(box().querySelectorAll('button')).map((b) => b.textContent);
  const button = (label) => Array.from(box().querySelectorAll('button')).find(
      (b) => b.textContent === label);
  const run = async (reply) => {
    agentRunReplies.push(reply);
    window.startWebAgent('Buy blue socks');
    await tick();
    await tick();
  };

  // A run that can be saved: the offer, sent only from the click.
  await run({success: true, result: 'Bought', save_run: 7});
  assert(box().classList.contains('shown'), 'save offered');
  assert.deepStrictEqual(labels(), ['Save as automation', 'Not now']);
  assert.strictEqual(agentSaveCalls.length, 0, 'nothing saved before the click');
  nextAgentSave = {success: true, id: 'agent_1', name: 'n',
                   message: 'Saved as a draft <img src=x onerror=alert(1)>'};
  button('Save as automation').click();
  assert.deepStrictEqual(agentSaveCalls.pop(), [7], 'sent from the click');
  await tick();
  assert(/Saved as a draft <img/.test(box().textContent), 'the browser\'s words, as text');
  assert.strictEqual(doc.querySelectorAll('#agentBar img').length, 0);
  assert.deepStrictEqual(labels(), ['Review in Agent mode', 'Close']);
  button('Review in Agent mode').click();
  assert(window.__sent.includes('openAgentStudio'), 'opens Agent mode');
  button('Close').click();
  assert(!box().classList.contains('shown'));
  assert(!doc.getElementById('agentBar').classList.contains('active'), 'bar closed');

  // A save the browser refuses: its error as text, nothing to review.
  await run({success: true, result: 'Done', save_run: 8});
  nextAgentSave = {success: false, error: 'This run can no longer be <b>saved</b>.'};
  button('Save as automation').click();
  assert.deepStrictEqual(agentSaveCalls.pop(), [8]);
  await tick();
  assert(/can no longer be <b>saved<\/b>/.test(box().textContent));
  assert.strictEqual(doc.querySelectorAll('#agentBar b').length, 0);
  assert.deepStrictEqual(labels(), ['Close']);

  // Not saveable: the note only. Stopped or failed runs, and junk ids:
  // nothing, whatever the reply claims.
  await run({success: true, result: 'Done', save_note: 'It used a site\'s <i>tool</i>'});
  assert.deepStrictEqual(labels(), []);
  assert.strictEqual(box().textContent, 'It used a site\'s <i>tool</i>');
  assert.strictEqual(doc.querySelectorAll('#agentBar i').length, 0);
  for (const reply of [{success: false, result: 'Stopped', save_run: 9},
                       {success: true, result: 'Done', save_run: 'x'},
                       {success: true, result: 'Done', save_run: -1},
                       {success: true, result: 'Done', save_run: 1.5},
                       {success: true, result: 'Done', save_run: {}},
                       {success: true, result: 'Done', save_note: 42},
                       null]) {
    await run(reply);
    assert(!box().classList.contains('shown'), 'no offer for ' + JSON.stringify(reply));
  }
  // "Not now" closes the offer; a new run starts without one.
  await run({success: true, result: 'Done', save_run: 10});
  button('Not now').click();
  assert(!box().classList.contains('shown'));
  await run({success: true, result: 'Done', save_run: 11});
  agentRunReplies.length = 0;
  window.startWebAgent('Again');
  await tick();
  assert(!box().classList.contains('shown'), 'a new run clears the offer');
  doc.getElementById('agentStopBtn').click();
  await tick();
  assert(!box().classList.contains('shown'), 'a stopped run offers nothing');
  assert.strictEqual(agentSaveCalls.length, 0);
}

// shieldSetGlobal calls from molt://ai-settings ([key, value] each). The
// reply after turning tracking-parameter stripping off also reports its
// chrome://flags kill switch as off.
const shieldGlobalCalls = [];
function shieldSetGlobal(args) {
  shieldGlobalCalls.push([args[1], args[2]]);
  if (args[1] !== 'removeParam') return SHIELD_STATE;
  return Object.assign({}, SHIELD_STATE, {
    removeParam: args[2],
    hooksOff: ['molt-shield-websocket', 'molt-shield-removeparam'],
  });
}

// molt://ai-settings "Sites using on-device AI": hostile site strings stay
// text, Remove sends exactly the key it was given, the switch and "Remove
// all" send their messages, and a change elsewhere refreshes the list.
async function checkWebAISites(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const section = doc.getElementById('webAiSection');
  assert(section, 'section present');
  assert.strictEqual(section.querySelectorAll('img, b').length, 0, 'site names stay text');
  const rows = (id) => Array.from(doc.querySelectorAll('#' + id + ' .web-ai-site'));
  assert.strictEqual(rows('webAiAllowed').length, 1);
  assert.strictEqual(rows('webAiBlocked').length, 1);
  assert.strictEqual(rows('webAiPaused').length, 1);
  assert.strictEqual(section.querySelectorAll('button[data-site]').length, 3,
                     'rows without a string key or with an unknown kind are not shown');
  assert(rows('webAiBlocked')[0].textContent.includes('<img src=x onerror=alert(1)>'));
  assert(rows('webAiPaused')[0].textContent.includes('http://<b>paused</b>.example'));
  assert(/can ask again/.test(rows('webAiPaused')[0].textContent), 'pause end shown');
  assert(doc.getElementById('webAiEnabled').checked, 'switch shows on');
  assert.strictEqual(doc.getElementById('webAiMemoryNote').style.display, 'none');

  // Remove: the exact key goes back to the browser.
  webAiCalls.length = 0;
  rows('webAiBlocked')[0].querySelector('button').click();
  await tick();
  assert.deepStrictEqual(webAiCalls.pop(), ['webAiSitesRemove', WEB_AI_HOSTILE_SITE]);

  // The switch, with an error from the browser shown as text.
  webAiState = Object.assign({}, webAiState, {enabled: false, error: '<b>disk full</b>'});
  const toggle = doc.getElementById('webAiEnabled');
  toggle.click();
  await tick();
  assert.deepStrictEqual(webAiCalls.pop(), ['webAiSitesSetEnabled', false]);
  assert(!toggle.checked, 'switch shows the saved value');
  assert.strictEqual(doc.getElementById('webAiStatus').textContent, '<b>disk full</b>');
  assert.strictEqual(section.querySelectorAll('b').length, 0);

  // A change elsewhere (another tab's prompt): the list is fetched again.
  webAiState = {enabled: false, available: true, persistent: false, sites: []};
  window.cr.webUIListenerCallback('web-ai-sites-changed');
  await tick();
  assert.deepStrictEqual(webAiCalls.pop(), ['webAiSitesGetState']);
  assert.strictEqual(section.querySelectorAll('button[data-site]').length, 0);
  assert.strictEqual(doc.getElementById('webAiMemoryNote').style.display, '',
                     'in-memory note shown for a Guest session');
  assert.strictEqual(doc.getElementById('webAiClearBtn').disabled, true);
  assert(/No sites allowed/.test(doc.getElementById('webAiAllowed').textContent));

  // Remove all.
  webAiState = {enabled: true, available: true, persistent: true, sites: WEB_AI_SITES};
  window.cr.webUIListenerCallback('web-ai-sites-changed');
  await tick();
  assert.strictEqual(doc.getElementById('webAiClearBtn').disabled, false);
  webAiState = {enabled: true, available: true, persistent: true, sites: []};
  doc.getElementById('webAiClearBtn').click();
  await tick();
  assert.deepStrictEqual(webAiCalls.pop(), ['webAiSitesClear']);
  assert.strictEqual(section.querySelectorAll('button[data-site]').length, 0);

  // Garbage answers don't break the section.
  for (const junk of [null, 'x', 7, {sites: 'nope'}, {sites: [{}]}]) {
    webAiState = junk;
    window.cr.webUIListenerCallback('web-ai-sites-changed');
    await tick();
  }
  webAiState = {enabled: true, available: true, persistent: true, sites: WEB_AI_SITES};
}

// molt://ai-settings "Use the GPU for on-device AI"
// (docs/design/SANDBOXED_INFERENCE.md section 11.11). The driver names come
// from the inference service (sanitized by the browser, still untrusted
// text here); rows of the wrong shape are skipped.
const GPU_DRIVERS = [
  {name: 'AMD Radeon <b>RX</b> 6600 <img src=x onerror=alert(1)>',
   version: '25.2.3', since: 1760000000000, unidentified: false},
  {name: '', version: '', since: 1760000000000, unidentified: true},
  null, 'junk',
];
const gpuCalls = [];
let gpuState = {available: true, automatic: true, disabledBySwitch: false,
                offForSession: false, drivers: GPU_DRIVERS};
function gpuInference(args) {
  gpuCalls.push([args[0].replace(/_\d+$/, '')].concat(args.slice(1)));
  return gpuState;
}

async function checkGpuSection(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const section = doc.getElementById('gpuSection');
  assert(section, 'GPU section present');
  assert.strictEqual(section.hidden, false, 'shown in a build with GPU inference');
  assert.strictEqual(section.querySelectorAll('img, b').length, 0, 'driver names stay text');
  const rows = Array.from(section.querySelectorAll('.gpu-turned-off'));
  assert.strictEqual(rows.length, 2, 'one line per turned-off driver');
  assert(rows[0].textContent.includes('<b>RX</b>'), rows[0].textContent);
  assert(rows[0].textContent.includes('driver 25.2.3'));
  assert(/before it could be identified/.test(rows[1].textContent));
  assert.strictEqual(doc.getElementById('gpuRetryBtn').hidden, false, 'retry offered');
  assert(doc.getElementById('gpuModeAuto').checked);

  // Off, then Automatic: the value goes to the browser (which checks the
  // click and its argument), and the page shows what it answers.
  gpuState = Object.assign({}, gpuState, {automatic: false});
  doc.getElementById('gpuModeOff').click();
  await tick();
  assert.deepStrictEqual(gpuCalls.pop(), ['gpuInferenceSetMode', 'off']);
  assert(doc.getElementById('gpuModeOff').checked);
  // A refused change (no real click): the answer's value and its error.
  gpuState = Object.assign({}, gpuState, {error: '<b>Click the setting to change it.</b>'});
  doc.getElementById('gpuModeAuto').click();
  await tick();
  assert.deepStrictEqual(gpuCalls.pop(), ['gpuInferenceSetMode', 'auto']);
  assert(doc.getElementById('gpuModeOff').checked, 'shows the saved value');
  assert.strictEqual(doc.getElementById('gpuStatus').textContent,
                     '<b>Click the setting to change it.</b>');
  assert.strictEqual(section.querySelectorAll('b').length, 0);

  // Retry: nothing turned off any more, the button goes.
  gpuState = {available: true, automatic: true, disabledBySwitch: true,
              offForSession: false, drivers: []};
  doc.getElementById('gpuRetryBtn').click();
  await tick();
  assert.deepStrictEqual(gpuCalls.pop(), ['gpuInferenceRetry']);
  assert.strictEqual(section.querySelectorAll('.gpu-turned-off').length, 0);
  assert.strictEqual(doc.getElementById('gpuRetryBtn').hidden, true);
  assert(/--disable-gpu/.test(doc.getElementById('gpuNotes').textContent));

  // A build without GPU inference: the section hides.
  gpuState = {available: false, automatic: true, drivers: GPU_DRIVERS};
  doc.getElementById('gpuModeOff').click();
  await tick();
  assert.strictEqual(section.hidden, true, 'hidden without GPU inference');

  // Garbage answers don't break it.
  for (const junk of [null, 'x', 7, {available: true, drivers: 'nope'},
                      {available: true, drivers: [{}]}]) {
    gpuState = junk;
    doc.getElementById('gpuModeAuto').click();
    await tick();
  }
  gpuState = {available: true, automatic: true, disabledBySwitch: false,
              offForSession: false, drivers: GPU_DRIVERS};
}

// Model licence display (A13), shared by molt://ai and molt://ai-chat: the
// licence name is text, only an http(s) licence URL becomes a link, the
// link opens through openUrlInTab, and in the model menu it doesn't also
// select or download the row's model.
async function checkModelLicences(window) {
  const doc = window.document;
  const tick = () => new Promise((r) => setTimeout(r, 15));
  const links = doc.querySelectorAll('#modelList [data-on-click=openModelLicense]');
  assert.strictEqual(links.length, 1, 'only the http(s) licence is a link');
  assert.strictEqual(links[0].textContent, '<img src=x onerror=alert(1)>',
                     'licence name stays text');
  assert(/^https:\/\/example\.com\/terms/.test(links[0].getAttribute('data-license-url')));
  assert(Array.from(doc.querySelectorAll('#modelList .mlicense'))
             .some((d) => d.textContent === 'Licence: MIT License'),
         'licence without a usable link is still named');
  assert.strictEqual(doc.querySelectorAll('#modelList img').length, 0);
  openedUrls.length = 0;
  links[0].click();
  await tick();
  assert.strictEqual(openedUrls.length, 1, 'licence link opens a tab');
  assert(/^https:\/\/example\.com\/terms/.test(openedUrls[0]), openedUrls[0]);

  window.allModels = [];
  window.renderModelChipDropdown();
  await tick();
  const rowLinks = doc.querySelectorAll('#modelChipDropdown button.mlic');
  assert.strictEqual(rowLinks.length, 1, 'model menu row links its licence');
  assert.strictEqual(doc.querySelectorAll('#modelChipDropdown img').length, 0);
  const sentBefore = window.__sent.length;
  rowLinks[0].click();
  await tick();
  assert.strictEqual(openedUrls.length, 2);
  assert(!window.__sent.slice(sentBefore).some(
             (m) => m === 'downloadModel' || m === 'loadModel'),
         'licence click does not pick the model');
}

// molt://ai-settings MoltShield section state, with hostile strings where
// list/tab metadata comes from the network or pages.
const SHIELD_STATE = {
  enabled: true, cosmetic: true, consentKiller: false, removeParam: true, ready: true,
  updating: false, lastUpdateCheck: 1760000000000, engine: 'adblock-rust-0.13.3/molt-1',
  stats: {checked: 1200, blocked: 300, ads: 120, redirects: 4, websockets: 1,
          cosmetic: 9, scriptlets: 2},
  lists: [
    {id: 'easylist', title: 'EasyList <b>', enabled: true, cached: true, rules: 77000,
     version: '202610142210', fetchedAt: 1760000000000, checkedAt: 1760000000000, error: ''},
    {id: 'fanboy-annoyance', title: "Fanboy's Annoyance", enabled: false, cached: false,
     rules: 0, version: '', fetchedAt: 0, checkedAt: 0, error: 'HTTP 503'},
  ],
  tabs: [{key: 7, site: 'example.com', title: '<img src=x onerror=alert(1)>',
          blocked: 12, shieldsUp: true}],
  exceptions: ['https://[*.]broken.example'],
  // Kill switches: one hook turned off in chrome://flags.
  disabledByCommandLine: false, hooksOff: ['molt-shield-websocket'],
};

// molt://ai-settings "Sites using on-device AI". Keys are canonical origins
// in the browser, but the page must not rely on that: hostile keys and
// display names, rows of the wrong shape, a kind it doesn't know.
const WEB_AI_HOSTILE_SITE = 'https://x.example"><img src=x onerror=alert(1)>';
const WEB_AI_SITES = [
  {site: 'https://allowed.example', display: 'allowed.example', kind: 'allowed',
   since: 1760000000000, until: 0},
  {site: WEB_AI_HOSTILE_SITE, display: '<img src=x onerror=alert(1)>', kind: 'blocked',
   since: 1760000000000, until: 0},
  {site: 'http://paused.example', display: 'http://<b>paused</b>.example', kind: 'paused',
   since: 1760000000000, until: 1760604800000},
  {site: 42, display: 'not a string key', kind: 'allowed'},
  {site: 'https://odd.example', display: 'odd kind', kind: '<b>other</b>'},
  null,
];
const webAiCalls = [];
let webAiState = {enabled: true, available: true, persistent: true, sites: WEB_AI_SITES};
function webAiSites(args) {
  webAiCalls.push([args[0].replace(/_\d+$/, '')].concat(args.slice(1)));
  return webAiState;
}

// molt://memory: what memSemanticStatus answers (the test changes it), and
// the [query, top_k] of each memSearch.
let memorySemanticStatus = {
  available: true,
  model: {state: 'not_installed', name: '<img src=x onerror=alert(1)>',
          license_name: '<b>Apache</b>', license_url: 'javascript:alert(1)',
          size_bytes: 640000000, received: 0, total: 0, reason: '', error: ''},
  index: {model_active: false, migrating: false, done: 0, total: 0, pending: 0,
          background_allowed: true, model_failed: false},
};
const memorySearches = [];
// molt://memory/?q= (the omnibox's @memory shortcut): what memInitialQuery
// answers (the browser parsed the URL) and the searches the page made.
const MEMORY_URL_QUERY = '<img src=x onerror=alert(1)> miso "soup"';
const memoryUrlSearches = [];

const PAGES = [
  {name: 'molt://home', file: 'molt_home_ui.cc',
   listeners: ['#searchForm:submit', '.card.ai:click', '.card.net:click']},
  {name: 'molt://memory', file: 'molt_ai_memory_ui.cc',
   listeners: ['#search-btn:click', '#forget-domain-btn:click',
               '#wipe-btn:click', '#q:keydown', '#mem-enabled:change',
               '#model-download-btn:click', '#model-cancel-btn:click',
               '#model-remove-btn:click'],
   responses: {
     memStats: {docs: 2, chunks: 9},
     memListRecent: {docs: [
       {doc_id: 1, url: 'https://example.com/a"><img src=x>', title: '<b>t</b>',
        visited_at: 1700000000, word_count: 5},
       {doc_id: 2, url: 'javascript:alert(1)', title: 'js', visited_at: 0}]},
     memSemanticStatus: () => memorySemanticStatus,
     memSearch(args) {
       memorySearches.push(Array.from(args).slice(1));  // a Node array
       return {model: true, ok: true, hits: [
         {url: 'https://example.com/r"><img src=x>', title: '<b>Miso soup</b>',
          snippet: '… a <img src=x onerror=alert(1)> MISO-glazed soup, misoyaki …',
          score: 0.71, visited_at: 1700000000},
         {url: 'javascript:alert(1)', title: '<i>js</i>', snippet: 'plain', score: 0.2}]};
     },
     memModelRemove: {success: true},
   },
   async after(window) {
     const doc = window.document;
     const tick = () => new Promise((r) => setTimeout(r, 30));
     const links = doc.querySelectorAll('#results a[target=_blank]');
     assert.strictEqual(links.length, 1, 'only the http(s) result is a link');
     assert(/^https:\/\/example\.com\//.test(links[0].href), links[0].href);
     assert.strictEqual(doc.querySelectorAll('#results img, #results b').length, 0,
                        'result text is not parsed as HTML');
     assert(window.__sent.includes('memSemanticStatus'), 'model status asked on load');

     // The model card: not installed, hostile name/licence stay text, a
     // javascript: licence URL is not a link, only Download is offered.
     assert(!doc.getElementById('search-model').hidden);
     assert.strictEqual(doc.getElementById('model-name').textContent,
                        '<img src=x onerror=alert(1)>');
     assert.strictEqual(doc.querySelectorAll('#search-model img, #search-model a').length, 0);
     assert(!doc.getElementById('model-download-btn').hidden);
     assert(/640 MB/.test(doc.getElementById('model-download-btn').textContent));
     assert(doc.getElementById('model-cancel-btn').hidden);
     assert(doc.getElementById('model-remove-btn').hidden);
     assert(/matching words/.test(doc.getElementById('engine').textContent));

     // Search by meaning: one result per page with title, URL, date and a
     // snippet whose matching words are <mark>ed as text.
     doc.getElementById('q').value = '  miso soup ';
     doc.getElementById('search-btn').click();
     await tick();
     assert.deepStrictEqual(memorySearches[memorySearches.length - 1], ['miso soup', 20]);
     const hits = doc.querySelectorAll('#results .hit');
     assert.strictEqual(hits.length, 2);
     assert.strictEqual(doc.querySelectorAll('#results a[target=_blank]').length, 1);
     assert.strictEqual(doc.querySelectorAll('#results img, #results b, #results i').length, 0);
     assert.strictEqual(hits[0].querySelector('.hit-title').textContent, '<b>Miso soup</b>');
     assert.strictEqual(hits[0].querySelector('.hit-url').textContent,
                        'https://example.com/r"><img src=x>');
     assert.strictEqual(hits[0].querySelector('.hit-snippet').textContent,
                        '… a <img src=x onerror=alert(1)> MISO-glazed soup, misoyaki …');
     assert.deepStrictEqual(
         Array.from(hits[0].querySelectorAll('.hit-snippet mark')).map((m) => m.textContent),
         ['MISO', 'soup', 'misoyaki']);
     assert.strictEqual(hits[1].querySelector('.hit-title').textContent, '<i>js</i>');
     assert(/by meaning/.test(doc.getElementById('timing').textContent));

     // Enter in the box searches too.
     doc.getElementById('q').dispatchEvent(
         new window.KeyboardEvent('keydown', {key: 'Enter', bubbles: true}));
     await tick();
     assert.strictEqual(memorySearches.length, 2, 'Enter searches');

     // Installed and building the index in the background: Remove offered,
     // progress shown on the engine line.
     memorySemanticStatus = Object.assign({}, memorySemanticStatus, {
       model: Object.assign({}, memorySemanticStatus.model,
                            {state: 'installed', license_url: 'https://example.com/lic'}),
       index: {model_active: false, migrating: true, done: 1200, total: 3400,
               pending: 0, background_allowed: false, model_failed: false}});
     window.loadSemantic(false);
     await tick();
     assert(!doc.getElementById('model-remove-btn').hidden);
     assert(doc.getElementById('model-download-btn').hidden);
     assert(/1,200 of 3,400/.test(doc.getElementById('engine').textContent),
            doc.getElementById('engine').textContent);
     assert(/paused/.test(doc.getElementById('engine').textContent));
     const lic = doc.querySelectorAll('#model-license a');
     assert.strictEqual(lic.length, 1, 'an https licence URL is a link');
     assert.strictEqual(lic[0].textContent, '<b>Apache</b>');

     // Downloading: progress bar and Cancel.
     memorySemanticStatus = Object.assign({}, memorySemanticStatus, {
       model: Object.assign({}, memorySemanticStatus.model,
                            {state: 'downloading', received: 320000000, total: 640000000})});
     window.loadSemantic(false);
     await tick();
     assert(!doc.getElementById('model-progress').hidden);
     assert.strictEqual(doc.getElementById('model-progress-bar').style.width, '50%');
     assert(!doc.getElementById('model-cancel-btn').hidden);

     // Not offered (e.g. not pinned yet): the reason as text, no buttons.
     memorySemanticStatus = Object.assign({}, memorySemanticStatus, {
       model: Object.assign({}, memorySemanticStatus.model,
                            {state: 'not_offered', reason: '<b>not here</b>'})});
     window.loadSemantic(false);
     await tick();
     assert.strictEqual(doc.getElementById('model-state').textContent, '<b>not here</b>');
     for (const id of ['model-download-btn', 'model-cancel-btn', 'model-remove-btn']) {
       assert(doc.getElementById(id).hidden, id);
     }
     // No memory in this profile (off-the-record): no model card.
     memorySemanticStatus = {available: false};
     window.loadSemantic(false);
     await tick();
     assert(doc.getElementById('search-model').hidden);
     // No settings answer (no memory): no @memory tip either.
     assert(doc.getElementById('omnibox-tip').hidden, '@memory tip hidden');
   }},
  // Opened from the omnibox's @memory shortcut: the browser hands over the
  // query it read from ?q=; the page searches it at once and shows it only
  // as the search box's value.
  {name: 'molt://memory/?q', file: 'molt_ai_memory_ui.cc',
   listeners: ['#search-btn:click', '#q:keydown'],
   responses: {
     memInitialQuery: {query: MEMORY_URL_QUERY},
     memGetSettings: {available: true, enabled: true, paused: false,
                      retention_days: 90, show_opt_in_notice: false},
     memStats: {docs: 2, chunks: 9},
     memListRecent: {docs: [
       {doc_id: 1, url: 'https://example.com/recent', title: 'Recent',
        visited_at: 1700000000, word_count: 5}]},
     memSemanticStatus: {available: true, model: {state: 'not_offered'},
                         index: {model_active: false}},
     memSearch(args) {
       memoryUrlSearches.push(Array.from(args).slice(1));
       return {model: false, ok: true, hits: [
         {url: 'https://example.com/miso', title: 'Miso soup',
          snippet: 'miso soup', score: 0.5, visited_at: 1700000000}]};
     },
   },
   async after(window) {
     const doc = window.document;
     await new Promise((r) => setTimeout(r, 30));
     assert(window.__sent.includes('memInitialQuery'), 'asks the browser');
     assert.deepStrictEqual(memoryUrlSearches[0], [MEMORY_URL_QUERY, 20],
                            'searched at once');
     assert.strictEqual(doc.getElementById('q').value, MEMORY_URL_QUERY);
     assert.strictEqual(doc.querySelectorAll('img').length, 0,
                        'the query is never markup');
     assert.strictEqual(doc.getElementById('results-title').textContent,
                        'Search results');
     assert.strictEqual(doc.querySelectorAll('#results .hit').length, 1);
     assert(!window.__sent.includes('memListRecent'),
            'recent pages never replace the results');
     assert(!doc.getElementById('omnibox-tip').hidden, '@memory tip shown');
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
  {name: 'molt://net', file: 'molt_net_ui.cc',
   listeners: ['#openWindowBtn:click', '#newIdentityBtn:click', '#settingsBtn:click'],
   noClick: ['#settingsBtn'],
   responses: {
     'moltnet.getStatus': {running: true, starting: false, selected: '', mode: 'multi_hop',
                           routing: 'tor', in_moltnet_window: true,
                           proxy_externally_controlled: false},
     'moltnet.getExitCountries': {available: [
       {code: 'de', name: '<img src=x onerror=alert(1)>'}], selected: ''},
     'moltnet.getCircuit': {connected: true, hops: [
       {role: 'guard', country: 'DE', country_name: '<b>Germany</b>', ip: '192.0.2.1',
        nickname: '<img src=x onerror=alert(1)>', fingerprint: 'A'.repeat(40)},
       {role: 'middle', country: 'NL', ip: '192.0.2.2', nickname: 'm', fingerprint: 'B'.repeat(40)},
       {role: 'exit', country: 'SE', ip: '192.0.2.3', nickname: 'e', fingerprint: 'C'.repeat(40)}]},
     'moltnet.openWindow': {running: true, routing: 'not_routed',
                            error: '<img src=x onerror=alert(1)>'},
     'moltnet.newIdentity': {running: true, routing: 'tor', in_moltnet_window: true},
     'moltnet.setMode': {running: false, routing: 'not_routed'},
     'moltnet.setExitCountry': {running: true, selected: 'de', routing: 'tor', in_moltnet_window: true},
     'moltnet.verifyExit': {ok: true, ip: '192.0.2.3', is_tor: true},
   },
   async after(window) {
     const doc = window.document;
     const tick = () => new Promise((r) => setTimeout(r, 15));
     await tick();
     assert.strictEqual(doc.getElementById('st').textContent, 'Private via Tor',
                        'status reflects this window\'s routing');
     assert(!/Your IP|origin/i.test(doc.body.textContent), 'no real-IP display');
     assert(!window.__sent.some((m) => /ipify/i.test(m)), 'no IP lookup');
     assert.strictEqual(doc.querySelectorAll('img, b').length, 0, 'relay/country data stays text');
     const verify = Array.from(doc.querySelectorAll('.hbtn')).find((b) => b.textContent === 'Verify');
     assert(verify, 'exit can be verified from a MoltNet window');
     verify.click(); await tick();
     assert(/Tor confirmed/.test(doc.getElementById('routeWrap').textContent));
     doc.getElementById('openWindowBtn').click(); await tick();
     assert.strictEqual(doc.getElementById('err').textContent, '<img src=x onerror=alert(1)>',
                        'errors are shown as text');
     doc.querySelector('#list .row:nth-child(2)').click(); await tick();
     assert(window.__sent.includes('moltnet.setExitCountry'));
   }},
  {name: 'molt://ai-settings', file: 'molt_ai_settings_ui.cc',
   listeners: ['#maxTokens:input', '#temperature:input', '#backBtn:click',
               '#moltnetEnabled:change', '#newCircuitBtn:click',
               '#bridgeMode:change', '#bridgeLines:input', '#saveBridgesBtn:click',
               '#exitCountry:change', '#importAllBtn:click', '#saveBtn:click',
               '#resetBtn:click'],
   noClick: ['#backBtn'],
   responses: {
     getSettings: {max_tokens: 512, temperature: 0.7, top_p: 0.9, top_k: 40},
     resetSettings: {max_tokens: 512, temperature: 0.7, top_p: 0.9, top_k: 40},
     getMoltnetExitCountries: {available: [{code: 'de', name: 'Germany'}], selected: ''},
     // Linux without transport packages or a built-in bridge list.
     getMoltnetBridges: {mode: 'custom', custom_lines: '<img src=x onerror=alert(1)>',
                         support: {obfs4: 'Need obfs4proxy <b>x</b>.',
                                   snowflake: 'Need snowflake-client.',
                                   none: 'Install obfs4proxy/snowflake-client to use bridges.',
                                   built_in: 'No built-in list.'}},
     setMoltnetBridges: {ok: false, error: '<b>Line 1: bad</b>'},

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
     // MoltShield section (script: kMoltShieldSettingsJs).
     shieldGetState: SHIELD_STATE,
     shieldSetGlobal,
     shieldSetListEnabled: SHIELD_STATE,
     shieldUpdateNow: SHIELD_STATE,
     shieldSetTabShields: SHIELD_STATE,
     shieldRemoveException: SHIELD_STATE,
     shieldReportBreakage: {report: 'MoltShield breakage report\nSite: <b>x</b>\n'},
     // "Sites using on-device AI" (script: kWebAISitesSettingsJs).
     webAiSitesGetState: webAiSites,
     webAiSitesRemove: webAiSites,
     webAiSitesClear: webAiSites,
     webAiSitesSetEnabled: webAiSites,
     // "Use the GPU for on-device AI" (script: kGpuInferenceSettingsJs).
     gpuInferenceGetState: gpuInference,
     gpuInferenceSetMode: gpuInference,
     gpuInferenceRetry: gpuInference,
   },
   extraFiles: ['molt_webui_csp.h',
                '../../../molt_ai/shield/molt_shield_settings_handler.cc',
                'molt_web_ai_sites_handler.cc',
                'molt_gpu_inference_handler.cc'],
   listenersExtra: ['#vaultAddBtn:click', '#vaultGenBtn:click',
                    '#shieldUpdateBtn:click', '#webAiClearBtn:click',
                    '#webAiEnabled:change', '#gpuModeAuto:change',
                    '#gpuModeOff:change', '#gpuRetryBtn:click'],
   async after(window) {
     const doc = window.document;
     const tick = () => new Promise((r) => setTimeout(r, 10));
     await checkWebAISites(window);
     await checkGpuSection(window);
     // MoltShield: hostile list/tab metadata renders as text, never markup.
     assert.strictEqual(doc.querySelectorAll('#shieldSection img, #shieldSection b').length, 0);
     assert(doc.getElementById('shieldTabs').textContent.includes('<img src=x onerror=alert(1)>'));
     assert.strictEqual(doc.querySelectorAll('#shieldLists input[type=checkbox]').length, 2);
     assert.strictEqual(doc.querySelectorAll('#shieldGlobals input[type=checkbox]').length, 4);
     assert(doc.querySelector('#shieldGlobals .shield-killswitch').textContent
                .includes('molt-shield-websocket'), 'kill-switch notice shown');
     assert.strictEqual(doc.querySelectorAll('#shieldSection input:disabled').length, 0,
                        'controls stay usable unless --disable-molt-shield');
     // "Strip tracking parameters": a per-profile switch next to the others.
     const removeParamRow = () => Array.from(doc.querySelectorAll('#shieldGlobals .field'))
         .find((f) => /Strip tracking parameters/.test(f.textContent));
     let rp = removeParamRow();
     assert(rp, 'tracking-parameter switch shown');
     assert(rp.querySelector('input').checked, 'on by default');
     rp.querySelector('input').click();
     await tick();
     assert.strictEqual(JSON.stringify(shieldGlobalCalls.pop()), '["removeParam",false]',
                        'switch sends shieldSetGlobal(removeParam, false)');
     rp = removeParamRow();
     assert(!rp.querySelector('input').checked, 'shows the new value');
     assert(rp.querySelector('input').disabled,
            'disabled while #molt-shield-removeparam is off in chrome://flags');
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
     assert.strictEqual(doc.getElementById('bridgeLines').style.display, 'block',
                        'custom bridge lines shown for custom mode');
     // Missing pluggable transports: the hint names what the selected
     // bridges need, as text.
     const hint = doc.getElementById('bridgeHint');
     assert.strictEqual(hint.style.display, 'block', 'transport hint shown');
     assert.strictEqual(hint.textContent,
                        'Install obfs4proxy/snowflake-client to use bridges.');
     const lines = doc.getElementById('bridgeLines');
     lines.value = 'Bridge obfs4 192.0.2.1:443 ' + 'A'.repeat(40) + ' cert=x iat-mode=0';
     lines.dispatchEvent(new window.Event('input'));
     assert.strictEqual(hint.textContent, 'Need obfs4proxy <b>x</b>.');
     assert.strictEqual(doc.querySelectorAll('#bridgeHint b').length, 0, 'hint is text');
     lines.value += '\nsnowflake 192.0.2.3:80 ' + 'B'.repeat(40);
     lines.dispatchEvent(new window.Event('input'));
     assert.strictEqual(hint.textContent, 'Need obfs4proxy <b>x</b>. Need snowflake-client.');
     const mode = doc.getElementById('bridgeMode');
     mode.value = 'snowflake';
     mode.dispatchEvent(new window.Event('change'));
     assert.strictEqual(hint.textContent, 'No built-in list. Need snowflake-client.');
     mode.value = 'none';
     mode.dispatchEvent(new window.Event('change'));
     assert.strictEqual(hint.style.display, 'none', 'no hint without bridges');
     mode.value = 'custom';
     mode.dispatchEvent(new window.Event('change'));
     doc.getElementById('saveBridgesBtn').click(); await tick();
     assert(window.__sent.includes('setMoltnetBridges'), 'bridges saved via setMoltnetBridges');
     assert.strictEqual(doc.querySelectorAll('#bridgeStatus b').length, 0, 'bridge errors are text');
     window.cr.webUIListenerCallback('moltnet-status', {status: 'connected', relays: [
       {country: 'de', relay_id: '"><img src=x onerror=alert(1)>', ip: '1.2.3.4'}]});
     const circuit = doc.getElementById('circuitDisplay');
     assert.strictEqual(circuit.querySelectorAll('img').length, 0, 'relay id stays in the title');
     assert(/img src=x/.test(circuit.querySelector('[title]').getAttribute('title')));
   }},
  {name: 'molt://ai-agent', file: 'molt_ai_agent_ui.cc',
   responses: {
     listWorkflows: {workflows: [
       {id: 'w1', name: '<img src=x onerror=alert(1)>', stepCount: 3, trust: 'casual',
        host: 'example.com', trigger: {type: 'manual'}},
       {id: 'd1', name: 'Buy socks', stepCount: 4, trust: 'casual', draft: true,
        host: 'shop.example', trigger: {type: 'manual'}}]},
     getRuns: {runs: []},
     getWorkflow: (args) => args[1] === 'd1' ? DRAFT_WORKFLOW : {
       id: 'w1', name: 'W', variables: {}, security: {trust: 'casual'},
       steps: [{type: 'navigate', target: 'https://www.example.com/'},
               {type: 'click', target: '#buy', description: 'Click "Buy"'}]},
     promoteTrust: (args) => { agentTrustCalls.push([args[1], args[2]]); return {ok: true, trust: args[2]}; },
     confirmDraftSites: (args) => {
       draftConfirmCalls.push(JSON.stringify(args.slice(1)));
       return {ok: true, trust: 'casual'};
     },
   },
   async after(window) {
     const doc = window.document;
     const tick = () => new Promise((r) => setTimeout(r, 15));
     assert.strictEqual(doc.querySelectorAll('#libList img[onerror]').length, 0,
                        'workflow names are text');
     // Trust level on the card; the card menu offers the trust action.
     const card = doc.querySelector('#libList .card');
     assert(/not trusted/.test(card.querySelector('.csub').textContent), 'trust shown on the card');
     card.querySelector('.kebab').click();
     const item = Array.from(card.querySelectorAll('.menu-item')).find(
         (m) => m.textContent.includes('Trust this workflow to submit forms on example.com'));
     assert(item, 'card menu offers "Trust this workflow…"');
     item.click();
     assert.deepStrictEqual(agentTrustCalls.pop(), ['w1', 'approved'], 'sent from the click');
     await tick();
     // Workflow detail: panel with the action and a one-line explanation.
     window.openEditor('w1');
     await tick();
     const panel = doc.getElementById('edTrust');
     assert(/only on the sites its steps use/.test(panel.textContent), 'explains what changes');
     const btn = Array.from(panel.querySelectorAll('button')).find(
         (b) => b.textContent === 'Trust this workflow to submit forms on example.com');
     assert(btn, 'editor trust button');
     btn.click();
     assert.deepStrictEqual(agentTrustCalls.pop(), ['w1', 'approved']);
     await tick();
     assert(/Trusted to submit forms on example\.com/.test(panel.textContent), 'panel updates');
     // A run stopped by trust names the step and offers "Trust & re-run".
     window.doRun('w1', {q: 'x'}, 'auto');
     window.cr.webUIListenerCallback('run-complete', {
       id: 'w1', ok: false, failedStepIndex: 1, failedStep: 'Click "Buy"', needsTrust: true,
       message: 'Blocked: submitting a form needs a trusted workflow.'});
     const done = doc.querySelector('#libList .card .run-done');
     assert(done && done.textContent.includes('Stopped at step 2 “Click "Buy"”'), 'blocked step named');
     const rerun = Array.from(doc.querySelectorAll('#libList button')).find(
         (b) => b.textContent === 'Trust & re-run');
     assert(rerun, '"Trust & re-run" offered');
     const before = window.__sent.filter((m) => m === 'runWorkflow').length;
     rerun.click();
     assert.deepStrictEqual(agentTrustCalls.pop(), ['w1', 'approved']);
     await tick();
     assert.strictEqual(window.__sent.filter((m) => m === 'runWorkflow').length, before + 1,
                        're-run after trusting');
     // A draft from an AI agent run: "Review" instead of Run / Edit /
     // Schedule and no trust item; the editor's review panel lists the
     // browser's sites as text, points at the steps that need the user,
     // won't run, and "Confirm" sends exactly the sites shown, from the
     // click; afterwards it is an ordinary untrusted workflow.
     window.showView('library');
     await window.loadLibrary();
     await tick();
     const draftCard = doc.querySelectorAll('#libList .card')[1];
     assert(/draft · review it/.test(draftCard.querySelector('.csub').textContent),
            'draft badge');
     assert.deepStrictEqual(
         Array.from(draftCard.querySelectorAll('.card-actions button')).map((b) => b.textContent),
         ['\u{1F50D} Review', '⋯'], 'a draft is reviewed, not run');
     draftCard.querySelector('.kebab').click();
     assert(!Array.from(draftCard.querySelectorAll('.menu-item')).some(
         (m) => /[Tt]rust/.test(m.textContent)), 'no trust item for a draft');
     draftCard.querySelector('.kebab').click();
     draftCard.querySelector('.card-actions .run').click();
     await tick();
     const draftPanel = doc.getElementById('edTrust');
     assert(/Draft from an AI agent run/.test(draftPanel.textContent), 'review panel');
     assert(/1 value asked at each run, 1 card or code field you enter yourself/.test(
         draftPanel.textContent), 'counts what needs the user');
     assert.deepStrictEqual(
         Array.from(draftPanel.querySelectorAll('.draft-site')).map((s) => s.textContent),
         ['shop.example', '<b>evil</b>.example'], 'sites as text, junk dropped');
     assert.strictEqual(draftPanel.querySelectorAll('b').length, 0);
     const notes = doc.querySelectorAll('#edSteps .review-note');
     assert.strictEqual(notes.length, 2, 'two steps need the user (junk codes ignored)');
     assert(/asked for it at each run/.test(notes[0].textContent));
     assert(/never saves those/.test(notes[1].textContent));
     assert.strictEqual(doc.querySelectorAll('#edSteps img[onerror]').length, 0);
     const runsBefore = window.__sent.filter((m) => m === 'runWorkflow').length;
     window.runWorkflowFlow('d1');
     await tick();
     assert.strictEqual(window.__sent.filter((m) => m === 'runWorkflow').length, runsBefore,
                        'a draft never runs');
     assert(!window.__sent.includes('saveSchedule'));
     doc.getElementById('draftConfirmBtn').click();
     assert.strictEqual(draftConfirmCalls.pop(),
                        '["d1",["shop.example","<b>evil</b>.example"]]', 'sent from the click');
     await tick();
     assert(!/Draft from an AI agent run/.test(draftPanel.textContent), 'confirmed');
     assert(/Not trusted/.test(draftPanel.textContent), 'now an untrusted workflow');
     // Leave an empty editor for the click-everything pass below: it clicks
     // controls in DOM order, so a populated step list would see its later
     // buttons clicked after "Delete step" had already detached them.
     window.openEditorDraft({name: 'x', steps: [], variables: {}});
     window.showView('library');
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
        is_downloaded: true, file_size_mb: 100,
        license_name: '<img src=x onerror=alert(1)>',
        license_url: 'https://example.com/terms"><b>x</b>'},
       {model_id: 'm2', display_name: 'Two', company: '<i>Co</i>', file_size_mb: 2048,
        license_name: 'MIT License', license_url: 'javascript:alert(1)'},
       {model_id: 'openai:<x>', is_cloud: true, provider: 'OpenAI', display_name: '<img src=x>'}]},
     sendPrompt: {success: true},
     openUrlInTab: (args) => { openedUrls.push(args[1]); return {success: true}; },
   },
   async after(window) {
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
     await new Promise((r) => setTimeout(r, 20));
     assert.strictEqual(doc.querySelectorAll('#modelList b, #modelList i, [data-x]').length, 0,
                        'model metadata stays text');
     const load = doc.querySelector('#modelList [data-on-click=loadModel]');
     assert(load && load.getAttribute('data-model-id') === 'm1" data-x="1', 'model id round-trips');
     await checkModelLicences(window);
     const input = doc.getElementById('searchInput');
     input.value = 'hi';
     input.dispatchEvent(new window.Event('input'));
     assert(doc.querySelector('#messages .highlight'), 'search highlights text');
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
        is_downloaded: true, file_size_mb: 100,
        license_name: '<img src=x onerror=alert(1)>',
        license_url: 'https://example.com/terms"><b>x</b>'},
       {model_id: 'm2', display_name: 'Two', company: 'Microsoft', file_size_mb: 2048,
        license_name: 'MIT License', license_url: 'javascript:alert(1)'},
       {model_id: 'openai:<x>', is_cloud: true, provider: 'OpenAI', display_name: '<img src=x>'}]},
     openUrlInTab: (args) => { openedUrls.push(args[1]); return {success: true}; },
     listConversations: {success: true, conversations: [
       {id: 'c1', title: '<img src=x onerror=alert(1)>', updated_at: 1}]},
     vaultList: {entries: [{id: 'abcdef1234567890', site_host: '<b>gh</b>', username: 'me'}]},
     vaultFindForActive: {host: 'github.com', fillable: true, matches: [
       {id: 'abcdef1234567890', site_host: 'gist.github.com', username: 'me', match: 'same_site'}]},
     vaultAutofill: {success: false, cancelled: true},
     openMoltSettings: {success: true},
     sendPrompt: (args) => {
       sentPrompts.push(args[1]);
       // A request typed in the composer (origin 'chat') to act on the
       // page: the browser's reply asks for a proposal.
       return args[1] === PAGE_ACTION_REQUEST && args[4] === 'chat'
           ? {success: true, action_request: 7} : {success: true};
     },
     proposeChatAction: (args) => {
       chatActionCalls.push(['proposeChatAction'].concat(args.slice(1)));
       return nextProposal;
     },
     runChatProposal: (args) => {
       chatActionCalls.push(['runChatProposal'].concat(args.slice(1)));
       return runProposalReplies.length ? runProposalReplies.shift()
                                        : {success: true, message: ''};
     },
     runMoltAction: (args) => {
       chatActionCalls.push(['runMoltAction'].concat(args.slice(1)));
       return {success: true, message: 'ok'};
     },
     proposeFormFill: (args) => {
       chatActionCalls.push(['proposeFormFill'].concat(args.slice(1)));
       return nextFormFill;
     },
     runFormFill: (args) => {
       chatActionCalls.push(['runFormFill'].concat(args.slice(1)));
       return {success: true, filled: 1, total: 1};
     },
     startWebAgent: (args) => {
       agentGoals.push(args[1]);
       return agentRunReplies.length ? agentRunReplies.shift() : NO_REPLY;
     },
     saveAgentRun: (args) => { agentSaveCalls.push([args[1]]); return nextAgentSave; },
     getPageContext: ACTIVE_PAGE,
     setJsForDomain: (args) => { jsToggles.push(args.slice(1)); return {success: true}; },
     openSandboxTab: (args) => { sandboxOpens.push(args[1]); return {success: true, url: args[1]}; },
     openTorTab: (args) => { torOpens.push(args[1]); return {success: true, url: args[1]}; },
     getPageContent: (args) => {
       pageContentCalls.push(args[1]);
       return {has_content: true, url: ACTIVE_PAGE.url, title: 'B', nav_entry_id: 2,
               content: 'fresh text of page B'};
     },
     // profile.enc exists but can't be decrypted: a plain save is refused,
     // "start over" keeps it as a backup.
     getMoltProfile: {profile: {}, unreadable: true},
     saveMoltProfile: (args) => {
       profileSaves.push(args.slice(1));
       return args[2] && args[2].backupUnreadable
           ? {success: true, backup: 'profile.enc.unreadable-20261016T090000Z<b>'}
           : {success: false, unreadable: true, error: '<b>could not be decrypted</b>'};
     },
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
     // A command that finishes synchronously (/vault add opens Settings and
     // is done) must not leave the chat marked busy.
     assert(!input.disabled, 'chat input usable again after /vault add');
     assert(doc.getElementById('sendBtn').style.display !== 'none',
            'Send button back after /vault add');
     // /profile with an unreadable profile.enc: the editor says so, a save
     // is refused without touching it, and only an explicit "start over"
     // asks for the backup path.
     input.value = '/profile';
     window.sendMessage();
     await tick();
     const editor = doc.querySelector('#messages .profile-editor');
     assert(editor, '/profile opens the editor');
     assert(/couldn't be decrypted/.test(editor.textContent), 'unreadable profile flagged');
     assert(!input.disabled, 'composer usable again after /profile');
     editor.querySelector('input[data-key=full_name]').value = 'Ada';
     editor.querySelector('.profile-save').click();
     await tick();
     // (Objects from the page's realm: compare as JSON.)
     assert.strictEqual(JSON.stringify(profileSaves[0]), '[{"full_name":"Ada"},{}]',
                        'plain save first');
     const startOver = Array.from(doc.querySelectorAll('#messages button')).find(
         (b) => /Start a new profile/.test(b.textContent));
     assert(startOver, 'refused save offers "start over"');
     assert.strictEqual(doc.querySelectorAll('#messages b').length, 0, 'errors are text');
     startOver.click();
     await tick();
     assert.strictEqual(JSON.stringify(profileSaves[1]),
                        '[{"full_name":"Ada"},{"backupUnreadable":true}]');
     assert(/kept as ~\/\.moltbrowser\/profile\.enc\.unreadable-/.test(
                doc.getElementById('messages').textContent), 'backup named');
     assert.strictEqual(doc.querySelectorAll('#messages b').length, 0, 'backup name is text');
     // Reader commands (A7): text pushed for the previous page is never
     // used; the current document's text is read instead.
     const runCmd = async (cmd) => {
       input.value = cmd;
       window.sendMessage();
       await tick();
       await tick();
     };
     window.__moltSetTabContext({url: 'https://example.com/a', title: 'A',
                                 text: 'stale text of page A', nav_entry_id: 1});
     await runCmd('/summarize');
     assert.strictEqual(pageContentCalls.pop(), 12000, 'stale push: page read again');
     assert(/fresh text of page B/.test(sentPrompts[sentPrompts.length - 1]) &&
            !/page A/.test(sentPrompts[sentPrompts.length - 1]),
            '/summarize uses the current page, never the stale push');
     // Pushed text for the document shown now is used as is.
     window.__moltSetTabContext({url: ACTIVE_PAGE.url, title: 'B',
                                 text: 'pushed text of page B', nav_entry_id: 2});
     await runCmd('/tldr');
     assert.strictEqual(pageContentCalls.length, 0, 'current push: no extra read');
     assert(/pushed text of page B/.test(sentPrompts[sentPrompts.length - 1]));
     // Same document, capture still running: read it now.
     window.__moltSetTabContext({url: ACTIVE_PAGE.url, title: 'B', text: '',
                                 nav_entry_id: 2, text_pending: true});
     await runCmd('/translate');
     assert.strictEqual(pageContentCalls.pop(), 4000);
     assert(/fresh text of page B/.test(sentPrompts[sentPrompts.length - 1]));
     window.__moltSetTabContext({url: 'https://example.com/a', title: 'A',
                                 text: 'stale receipt of page A', nav_entry_id: 1});
     await runCmd('/receipt');
     assert.strictEqual(pageContentCalls.pop(), 10000);
     assert(/Page URL: https:\/\/example\.com\/b/.test(sentPrompts[sentPrompts.length - 1]) &&
            !/page A/.test(sentPrompts[sentPrompts.length - 1]), '/receipt reads page B');
     // /chapters checks the URL of the document it read, not the push's.
     window.__moltSetTabContext({url: 'https://www.youtube.com/watch?v=x', title: 'V',
                                 text: '0:01 a\n0:02 b\n0:03 c\n0:04 d\n0:05 e',
                                 nav_entry_id: 1});
     const promptsBefore = sentPrompts.length;
     await runCmd('/chapters');
     assert.strictEqual(sentPrompts.length, promptsBefore, 'no prompt for a non-YouTube page');
     assert(/Open a YouTube video first/.test(doc.getElementById('messages').textContent));
     assert(!input.disabled, 'composer usable after the reader commands');
     // "This page" commands with no URL (/js, /sandbox, /tor open, /pdf) act
     // on the page the active tab shows now, never the side panel's last
     // push, which can be the previous page.
     window.__moltLastTabContext = {url: 'https://stale.example/old.pdf', title: 'Old'};
     await runCmd('/js off');
     assert.strictEqual(JSON.stringify(jsToggles.pop()), '["example.com",false]',
                        '/js off acts on the active tab');
     await runCmd('/sandbox');
     assert.strictEqual(sandboxOpens.pop(), ACTIVE_PAGE.url, '/sandbox opens the active tab');
     await runCmd('/tor open');
     assert.strictEqual(torOpens.pop(), ACTIVE_PAGE.url, '/tor open opens the active tab');
     await runCmd('/pdf');
     assert(!window.__sent.includes('extractPdfText'),
            '/pdf ignores a stale .pdf push when the active tab is not a PDF');
     assert(!input.disabled, 'composer usable after a /pdf usage error');
     await runCmd('/js maybe');
     assert(!input.disabled, 'composer usable after a /js usage error');
     // Tab-context contract still installed (from app.js now).
     window.__moltSetTabContext({url: 'https://example.com/a.pdf', title: 'Doc (PDF)'});
     assert.strictEqual(doc.querySelector('#tabContext .tab-context-icon').textContent, '\u{1F4C4}');
     // Model dropdown + panel render hostile metadata as text.
     window.toggleModelPanel();
     await tick();
     assert.strictEqual(doc.querySelectorAll('#modelList b, [data-x]').length, 0, 'model metadata stays text');
     await checkModelLicences(window);
     await checkChatPageActions(window);
     await checkChatFormFill(window);
     await checkChatAgentPlan(window);
     await checkChatAgentSave(window);
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
    if (r === NO_REPLY) return;  // a request the browser never answers
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
