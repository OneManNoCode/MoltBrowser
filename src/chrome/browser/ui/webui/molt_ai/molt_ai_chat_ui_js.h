// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The body of the <script> tag in the AI chat side panel.
// Extracted from molt_ai_chat_ui.cc as part of MEDIUM #11 from the
// 2026-05-20 code review — the parent file had grown to ~3950 lines
// with one huge inline raw-string blob containing the chat UI HTML,
// CSS, and ~3500 lines of JS. The JS body lives here so the .cc is
// reviewable per-feature.
//
// chat_ui.cc serves it as part of /app.js (after the shared
// moltActions()/moltHTML() prologue and before the page's action
// registrations); the HTML loads it with <script src="app.js">.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_UI_JS_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_UI_JS_H_

namespace molt_ai {

inline constexpr char kMoltChatJS[] = R"JS(
// ============================================================
// MoltBrowser AI Chat — WebUI JavaScript (Day 8)
//
// Served as part of /app.js under the strict molt WebUI CSP
// (molt_webui_csp.h): no inline handlers — markup names its handler
// with data-on-<event>="name" and the names are registered in
// molt_ai_chat_ui.cc (kChatPageActionsJs) — and every HTML sink goes
// through moltHTML(), the page's single Trusted Types policy.
// Model management, page content extraction, context management
// ============================================================

var isGenerating = false;
// Per-message origin metadata for backend context gating. Contract
// with the C++ side: 5th sendPrompt arg, one of 'chat' (typed free
// text — the default), 'page-action' (quick-action chips), or 'agent'
// (reserved; the Agent button uses the separate startWebAgent IPC and
// never enters sendMessage). Set by quickAction() right before it
// invokes sendMessage(); captured + reset inside sendMessage only once
// the message is actually committed, so the model-load auto-resend
// re-entry still sees the chip's flag (same pattern as pendingAutoSend).
var sendOrigin = 'chat';
var currentAiMessageEl = null;
var currentAiText = '';
var promptIdCounter = 0;

// Conversation history for multi-turn chat
var conversationHistory = [];
var MAX_HISTORY_MESSAGES = 16; // 8 user + 8 assistant exchanges

// Conversation persistence for the Recents drawer. `conversationId`
// stays null until the first autosave creates the record (per-profile,
// encrypted on disk by saveConversation; never saved in private windows).
// It is also the KV-cache reuse key sent with each local prompt.
var conversationId = null;
var conversationTitle = '';

// cr.sendWithPromise polyfill
var pendingCallbacks = {};

function rawSendWithPromise(method, args) {
  var id = method + '_' + (++promptIdCounter);
  return new Promise(function(resolve, reject) {
    pendingCallbacks[id] = {resolve: resolve, reject: reject};
    chrome.send(method, [id].concat(args));
  });
}

function sendWithPromise(method) {
  var args = Array.prototype.slice.call(arguments, 1);
  var p = rawSendWithPromise(method, args);
  if (method !== 'sendPrompt') return p;
  // Cloud first-use consent (P18): the backend refuses to send anything to a
  // provider the user hasn't approved and returns consent_required. Every
  // sendPrompt caller (chat, quick actions, slash commands) goes through
  // here, so the dialog is shown once, then the same request is re-sent.
  return p.then(function(r) {
    if (!r || !r.consent_required) return r;
    return askCloudConsent(r).then(function(ok) {
      if (!ok) {
        return {success: false,
                error: 'Nothing was sent to ' + r.provider_name + '.'};
      }
      return rawSendWithPromise('grantCloudConsent', [r.provider_id])
          .then(function() { return rawSendWithPromise('sendPrompt', args); });
    });
  });
}

window.cr = window.cr || {};
cr.webUIResponse = function(id, success, response) {
  var cb = pendingCallbacks[id];
  if (cb) {
    delete pendingCallbacks[id];
    if (success) cb.resolve(response);
    else cb.reject(response);
  }
};

var webUIListeners = {};
cr.addWebUiListener = function(eventName, callback) {
  if (!webUIListeners[eventName]) webUIListeners[eventName] = [];
  webUIListeners[eventName].push(callback);
};
cr.webUIListenerCallback = function(event) {
  var args = Array.prototype.slice.call(arguments, 1);
  var listeners = webUIListeners[event];
  if (listeners) {
    for (var i = 0; i < listeners.length; i++) {
      listeners[i].apply(null, args);
    }
  }
};

// ---- UI Helpers ----

function esc(t) {
  var d = document.createElement('div');
  d.textContent = t;
  return d.innerHTML;
}

// esc() for a double-quoted attribute value: esc() leaves quotes alone.
// Every HTML sink on this page goes through moltHTML() (Trusted Types),
// whose contract is that each interpolated value is escaped for where it
// lands — esc() in text, escAttr() inside attribute values.
function escAttr(t) {
  return esc(String(t == null ? '' : t)).replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
}

// An http(s) URL normalised for an href, or '' (javascript:, data:, ...).
function safeHttpHref(u) {
  try {
    var p = new URL(String(u));
    return (p.protocol === 'http:' || p.protocol === 'https:') ? p.href : '';
  } catch (e) {
    return '';
  }
}

// ---- Markdown Rendering ----
var codeBlockId = 0;

// Scheme allowlist for links surfaced in chat. http/https ONLY —
// this is the JS half of the two-layer validation (the C++
// 'openUrlInTab' handler re-checks the scheme before navigating).
// Rejects javascript:, data:, file:, chrome:, etc.
function isSafeHttpUrl(u) {
  return /^https?:\/\//i.test(u);
}

function renderMarkdown(text) {
  // Every fragment of markup this renderer generates (code, links) is
  // swapped for an opaque placeholder as soon as it is built, and the
  // placeholders are only expanded at the very end. Each later regex pass
  // therefore sees escaped text plus placeholders and never our own HTML,
  // so no pass can rewrite inside an attribute we emitted. (The previous
  // version re-scanned generated <a href="..."> markup with the bare-URL,
  // bold and italic rules, which let crafted model output close the href
  // and add live attributes in this privileged page.)
  var held = [];
  function hold(html) {
    held.push(html);
    return '\u0000' + (held.length - 1) + '\u0000';
  }
  // Escape HTML first. NUL is reserved for placeholders.
  var s = esc(String(text).replace(/\u0000/g, ''));
  // Code blocks (``` ... ```) with copy button
  s = s.replace(/```(\w*)\n([\s\S]*?)```/g, function(m, lang, code) {
    var id = 'cb-' + (++codeBlockId);
    var langLabel = lang ? '<span style="position:absolute;top:4px;left:8px;font-size:9px;color:var(--faint);text-transform:uppercase">' + lang + '</span>' : '';
    return hold('<div class="code-wrap">' + langLabel +
      '<button class="code-copy" data-on-click="copyCode" data-copy-target="' + id + '">Copy</button>' +
      '<pre id="' + id + '" style="background:var(--surface2);padding:' + (lang ? '22px 10px 10px' : '10px') + ';border-radius:8px;overflow-x:auto;font-size:12px;border:1px solid var(--border)"><code>' + code.trim() + '</code></pre></div>');
  });
  // Inline code (`...`)
  s = s.replace(/`([^`\n]+)`/g, function(m, code) {
    return hold('<code style="background:var(--surface2);padding:2px 6px;border-radius:4px;font-size:12px;color:var(--text)">' + code + '</code>');
  });
  // esc() (textContent->innerHTML) encodes & < > but NOT double-quotes,
  // so quote-escape a url before it lands in the href attribute.
  function attrUrl(u) { return u.replace(/"/g, '&quot;'); }
  // The anchor carries the chat-link class + data-molt-link marker so the
  // delegated click handler routes it to a real browser tab via
  // 'openUrlInTab' (which re-checks the scheme in C++).
  function link(url, body) {
    return hold('<a href="' + attrUrl(url) + '" class="chat-link" ' +
                'data-molt-link="1">' + body + '</a>');
  }
  // Markdown links [text](url) and bare urls, in ONE pass so a url that
  // has already become an anchor is never matched again. Markdown links
  // to anything but http/https stay as their literal text. A bare url
  // leaves trailing punctuation (.,;:!?) outside the link. Urls inside
  // code were held above and are not linkified.
  // Neither url form may span a placeholder (\u0000), or held markup would
  // be expanded inside the href.
  s = s.replace(/\[([^\]\n]+)\]\(([^)\s\u0000]+)\)|(https?:\/\/[^\s<\u0000]+[^\s<\u0000.,;:!?)\]])/gi,
    function(m, txt, url, bare) {
      if (bare) return link(bare, bare);
      if (!isSafeHttpUrl(url)) return m;
      return link(url, txt);
    });
  // Bold (**...**)
  s = s.replace(/\*\*([^*]+)\*\*/g, '<strong style="color:var(--text)">$1</strong>');
  // Italic (*...*)
  s = s.replace(/(?<!\*)\*([^*\n]+)\*(?!\*)/g, '<em>$1</em>');
  // Headers (### ... , ## ... , # ...)
  s = s.replace(/^### (.+)$/gm, '<div style="font-size:14px;font-weight:700;margin:10px 0 4px;color:var(--text)">$1</div>');
  s = s.replace(/^## (.+)$/gm, '<div style="font-size:15px;font-weight:700;margin:12px 0 4px;color:var(--text)">$1</div>');
  s = s.replace(/^# (.+)$/gm, '<div style="font-size:16px;font-weight:700;margin:14px 0 6px;color:var(--text)">$1</div>');
  // Unordered lists (- item or * item)
  s = s.replace(/^[\-\*] (.+)$/gm, '<div style="padding-left:16px;position:relative"><span style="position:absolute;left:4px;color:var(--muted)">•</span>$1</div>');
  // Ordered lists (1. item)
  s = s.replace(/^(\d+)\. (.+)$/gm, '<div style="padding-left:20px;position:relative"><span style="position:absolute;left:0;color:var(--muted);font-size:12px">$1.</span>$2</div>');
  // Line breaks (code blocks are held, so <pre> keeps its real newlines)
  s = s.replace(/\n/g, '<br>');
  // Expand placeholders. A held link may itself contain a held code span,
  // and a held fragment only ever refers to earlier ones, so this ends.
  function expand(str) {
    return str.replace(/\u0000(\d+)\u0000/g, function(m, i) {
      return held[+i] === undefined ? '' : expand(held[+i]);
    });
  }
  return expand(s);
}

function setStatus(state, text) {
  var el = document.getElementById('statusIndicator');
  el.className = 'status ' + state;
  el.textContent = text;
}

// ---- Theme (Gray default / Black / White) ----
// The persisted choice is applied pre-paint by a tiny inline script
// in <head>; this keeps the ⋯ menu check marks + storage in sync.
// 'gray' is the absence of the data-theme attribute; 'black' and
// 'light' set explicit var-override attributes.

function applyTheme(theme) {
  if (theme !== 'black' && theme !== 'light') theme = 'gray';
  if (theme === 'gray') {
    document.documentElement.removeAttribute('data-theme');
  } else {
    document.documentElement.setAttribute('data-theme', theme);
  }
  try { localStorage.setItem('moltTheme', theme); } catch (e) {}
  var checks = {
    gray: 'thCheckGray',
    black: 'thCheckBlack',
    light: 'thCheckLight'
  };
  for (var key in checks) {
    var el = document.getElementById(checks[key]);
    if (el) el.classList.toggle('visible', key === theme);
  }
}

// ⋯ menu entry point: apply + close the menu.
function setTheme(theme) {
  applyTheme(theme);
  closeOverflowMenu();
}

// Sync the menu check marks with whatever the pre-paint script applied
// (old persisted values like 'dark' collapse to the gray default).
applyTheme(document.documentElement.getAttribute('data-theme') || 'gray');

// ---- Minimal welcome message ----
// The static HTML ships a one-line welcome; this keeps it current
// with the loaded model. The full command list moved to /help.

function updateWelcomeMessage() {
  var el = document.getElementById('welcomeMsgText');
  if (!el) return;
  var active = null;
  for (var i = 0; i < allModels.length; i++) {
    if (allModels[i].is_loaded) { active = allModels[i]; break; }
  }
  // A selected cloud model is ready-by-selection (never is_loaded), so
  // fall back to it when no local model is loaded.
  if (!active && activeModelId) {
    for (var j = 0; j < allModels.length; j++) {
      if (allModels[j].is_cloud && allModels[j].model_id === activeModelId) {
        active = allModels[j];
        break;
      }
    }
  }
  el.textContent = active
      ? "Welcome! You're chatting with " +
        (active.display_name || active.model_id) + '.'
      : 'Welcome! Choose a model below to start.';
}

// Re-insert the welcome bubble into an empty transcript (New chat).
function ensureWelcomeMessage() {
  var m = document.getElementById('messages');
  if (!m || m.children.length) return;
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = moltHTML('<div class="sender">AI Assistant</div>' +
      '<div class="text"><span id="welcomeMsgText">Welcome!</span>' +
      '<div class="welcome-hint">Type /help for commands.</div></div>');
  m.appendChild(d);
  updateWelcomeMessage();
}

function addUserMessage(text, attachName) {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message user';
  d.innerHTML = moltHTML('<div class="sender">You</div>' +
      '<button class="msg-edit" title="Edit message" ' +
          'data-on-click="startEditMessage">&#9998;</button>' +
      '<div class="text"><span class="msg-text-body">' + esc(text) +
      '</span>' +
      (attachName
          ? '<div class="attached-note">[Attached: ' + esc(attachName) +
            ']</div>'
          : '') +
      '</div>');
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
}

// ---- Editable user messages (Claude-style) ----
// Hovering a user bubble shows a small pencil; clicking it swaps the
// bubble for an inline textarea with "Save & resubmit" / "Cancel".
// Saving truncates conversationHistory to just BEFORE that user turn,
// removes the bubble and everything after it from the DOM, autosaves
// the truncated conversation, then re-sends the edited text through
// the normal sendMessage pipeline.

function _userBubblePosFromEnd(msgEl) {
  var all = document.querySelectorAll('#messages .message.user');
  for (var i = 0; i < all.length; i++) {
    if (all[i] === msgEl) return all.length - 1 - i;
  }
  return -1;
}

function startEditMessage(btn) {
  if (isGenerating) return;
  var msgEl = btn.closest('.message.user');
  if (!msgEl || msgEl.querySelector('.msg-edit-area')) return;
  var textEl = msgEl.querySelector('.text');
  if (!textEl) return;
  var body = textEl.querySelector('.msg-text-body');
  var original = (body || textEl).textContent;
  var area = document.createElement('div');
  area.className = 'msg-edit-area';
  area.innerHTML = moltHTML('<textarea></textarea>' +
      '<div class="msg-edit-btns">' +
        '<button class="cancel-edit">Cancel</button>' +
        '<button class="save">Save &amp; resubmit</button>' +
      '</div>');
  var ta = area.querySelector('textarea');
  ta.value = original;
  textEl.style.display = 'none';
  btn.style.display = 'none';
  msgEl.appendChild(area);
  ta.focus();
  area.querySelector('.cancel-edit').onclick = function() {
    msgEl.removeChild(area);
    textEl.style.display = '';
    btn.style.display = '';
  };
  area.querySelector('.save').onclick = function() {
    if (isGenerating) return;
    var newText = ta.value.trim();
    if (!newText) return;
    _resubmitEditedMessage(msgEl, newText);
  };
}

function _resubmitEditedMessage(msgEl, newText) {
  // Map the DOM bubble to its conversationHistory entry by counting
  // user bubbles from the END — trimHistory drops old entries while
  // the DOM keeps them, so end-relative indexing stays correct.
  var posFromEnd = _userBubblePosFromEnd(msgEl);
  if (posFromEnd < 0) {
    addErrorMessage('Could not edit this message.');
    return;
  }
  var userIdxs = [];
  for (var i = 0; i < conversationHistory.length; i++) {
    if (conversationHistory[i].role === 'user') userIdxs.push(i);
  }
  if (posFromEnd >= userIdxs.length) {
    addErrorMessage(
        'This message is too old to edit (outside the history window).');
    return;
  }
  var histIdx = userIdxs[userIdxs.length - 1 - posFromEnd];
  // Mirror sendMessage's send guard BEFORE destroying state: when the
  // send can only end in an early-return error (no model loaded and
  // nothing downloaded, or a guard-triggered load is already in
  // flight), bail while the original turn is still intact in the
  // transcript and the conversation store.
  // A selected cloud model is ready-by-selection (no download/load), so
  // it never trips the "nothing downloaded" bail.
  var editCloudId = activeModelId || pickedModelId;
  var editIsCloud = editCloudId && allModels.some(function(mm) {
    return mm.is_cloud && mm.model_id === editCloudId;
  });
  if (!activeModelId && !editIsCloud && allModels.length) {
    var anyDownloaded = allModels.some(function(mm) {
      return mm.is_downloaded;
    });
    if (!anyDownloaded) {
      addErrorMessage(downloadingModelId
          ? 'Model is still downloading — it will load when finished.'
          : 'No model downloaded yet — pick one from the model menu below.');
      return;
    }
    if (pendingAutoSend) return;
  }
  conversationHistory = conversationHistory.slice(0, histIdx);
  // Remove the edited bubble and everything after it (AI replies,
  // action chips, error rows) from the transcript.
  var m = document.getElementById('messages');
  while (msgEl.nextSibling) m.removeChild(msgEl.nextSibling);
  m.removeChild(msgEl);
  updateContextBar();
  // Keep the stored conversation in sync with the truncation so the
  // Recents copy never contains the discarded tail.
  saveCurrentConversation();
  // Re-send through the normal pipeline (history/autosave/store all
  // behave exactly like a fresh send).
  var input = document.getElementById('chatInput');
  var draft = input ? input.value : '';
  if (input) input.value = newText;
  sendMessage();
  // sendMessage cleared the input on a successful dispatch — restore
  // whatever draft the user had typed there.
  if (input && !input.value && draft) {
    input.value = draft;
    input.dispatchEvent(new Event('input'));
  }
}

function startAiMessage() {
  var m = document.getElementById('messages');
  var d = document.createElement('div');
  d.className = 'message ai';
  d.innerHTML = moltHTML('<div class="sender">AI Assistant</div><div class="text"><span class="cursor"></span></div>');
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
  currentAiMessageEl = d.querySelector('.text');
  currentAiText = '';
  return d;
}

function appendToken(token) {
  if (!currentAiMessageEl) return;
  currentAiText += token;
  // A reply is text, never scanned for actions: page actions are
  // proposed by the browser after the reply (runPageActionProposal).
  var visible = currentAiText.trim();
  currentAiMessageEl.innerHTML = moltHTML(renderMarkdown(visible) + '<span class="cursor"></span>');
  var m = document.getElementById('messages');
  m.scrollTop = m.scrollHeight;
}

// Defense-in-depth scrub of chat-template control markers that leak
// into the visible reply when a model emits them as plain text. The
// real fix is C++-side (proper chat templating + EOG detection in
// BrowserAIRuntime); this just guarantees the rendered bubble is clean.
function scrubSpecialTokens(t) {
  return t
      .replace(/<\|start_header_id\|>[\s\S]*?<\|end_header_id\|>/g, '')
      .replace(/<\|(?:user|assistant|system|eot_id|end|im_start|im_end|endoftext)\|>/g, '')
      .replace(/<\/s>/g, '');
}

function finishAiMessage() {
  if (currentAiMessageEl) {
    var visibleText = scrubSpecialTokens(currentAiText).trim();
    currentAiMessageEl.innerHTML = moltHTML(renderMarkdown(visibleText));
    // Add copy response button
    var actions = document.createElement('div');
    actions.className = 'msg-actions';
    actions.innerHTML = moltHTML('<button class="msg-action" data-on-click="copyResponse">Copy response</button>');
    currentAiMessageEl.parentNode.appendChild(actions);
  }
  currentAiMessageEl = null;
  currentAiText = '';
  // Autosave the completed assistant turn (Recents drawer).
  saveCurrentConversation();
}

// Interim progress text in the streaming AI bubble ("Fetching…",
// "Scanning open tabs…"). Used by the slash commands, which later
// overwrite currentAiText with the final result and call
// finishAiMessage(). Creates the bubble if a stream isn't active.
function appendToAiMessage(text) {
  if (!currentAiMessageEl) startAiMessage();
  currentAiText += text;
  currentAiMessageEl.innerHTML =
      moltHTML(renderMarkdown(currentAiText) + '<span class="cursor"></span>');
  var m = document.getElementById('messages');
  if (m) m.scrollTop = m.scrollHeight;
}

// --------------------------------------------------------------
// Page actions the model proposes (docs/design/AGENT_ACTIONS.md §4).
//
// A reply is text: nothing in it is parsed or run, whatever it says.
// Old "[[ACTION ...]]" lines (in a reply, in page text the model
// echoed, in a saved conversation) are shown as text and do nothing.
//
// When the user typed a message that asks to act on the page, the
// browser's sendPrompt result carries `action_request`. After the
// reply, the page asks the browser for one proposal
// (proposeChatAction): the browser observes the page, has the
// on-device model choose one action in a fixed format it validates,
// and answers with its own description (`summary`) and, when the
// action must be approved even in Auto mode, a `reason` (another site,
// a form submission, a payment-card or one-time-code field). The page
// sends back only the proposal's id and the user's Allow
// (runChatProposal); no selector, URL or text from the model passes
// through it. Typed slash commands (/click, /navigate ...) are the
// user's own and keep using runMoltAction.
// --------------------------------------------------------------

// Session permission mode: 'ask' shows a confirm chip before every
// proposed page action (except scrolling); 'auto' runs a proposal at
// once unless the browser gives a reason to ask, which it does in both
// modes. Set by the mode chip; every new chat starts in 'ask'.
var actionAutoMode = false;

// Proposals that run without a chip in Ask mode: they only move the
// view.
var PROPOSAL_WITHOUT_CHIP = {SCROLL: 1};

// Set while a proposal is being chosen; Stop (cancelGeneration) calls it
// so the composer is never left waiting for an answer.
var abortPageActionProposal = null;

// |title| and |label| are browser-written text that quotes page labels
// and model text: they are set as text, never markup.
function _renderConfirmChip(title, label, onAllow, onDeny) {
  var msgs = document.getElementById('messages');
  if (!msgs) { onDeny(); return; }
  var d = document.createElement('div');
  d.className = 'molt-action-confirm';
  // Stylesheet for this chip is added inline so it can't be skinned
  // out by site CSS injection into our message area.
  d.innerHTML =
    moltHTML('<span class="ac-icon">⚡</span>' +
    '<div class="ac-body">' +
      '<div class="ac-title"></div>' +
      '<div class="ac-label"></div>' +
    '</div>' +
    '<div class="ac-buttons">' +
      '<button class="ac-deny">Deny</button>' +
      '<button class="ac-allow">Allow</button>' +
    '</div>');
  d.querySelector('.ac-title').textContent = title;
  var labelEl = d.querySelector('.ac-label');
  labelEl.textContent = label;
  // The user approves all of it: no ellipsis.
  labelEl.style.whiteSpace = 'normal';
  labelEl.style.wordBreak = 'break-word';
  msgs.appendChild(d);
  msgs.scrollTop = msgs.scrollHeight;
  // Single-use handlers; remove the chip on either path so the chat
  // stays clean.
  var settled = false;
  var settle = function(handler) {
    return function() {
      if (settled) return;
      settled = true;
      try { d.parentNode && d.parentNode.removeChild(d); } catch (e) {}
      handler();
    };
  };
  d.querySelector('.ac-allow').onclick = settle(onAllow);
  d.querySelector('.ac-deny').onclick = settle(onDeny);
}

// Show a real-time "🌐 Navigating to …" chip so the user sees the
// browser actually loading the page, not just a silent result chip.
// Returns the chip element (or null) so a caller can drop it again.
function appendNavigatingChip(url) {
  return appendStatusChip('🌐', 'Navigating to ' + url);
}

// A muted one-line status row with a spinner; |text| is shown as text.
function appendStatusChip(icon, text) {
  var m = document.getElementById('messages');
  if (!m) return null;
  var d = document.createElement('div');
  d.className = 'molt-navigating-chip';
  d.innerHTML = moltHTML('<span class="nav-icon"></span>' +
                '<span class="nav-label"></span>' +
                '<span class="nav-spinner"></span>');
  d.querySelector('.nav-icon').textContent = icon;
  d.querySelector('.nav-label').textContent = text;
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
  return d;
}

// Shows |label| while the browser chooses a proposal (|request()| asks
// for it). Resolves with the browser's answer, or null after Stop (a
// late answer is dropped). Never rejects.
function askBrowserForProposal(label, request) {
  return new Promise(function(resolve) {
    var row = appendStatusChip('⚡', label);
    var done = false;
    var finish = function(r) {
      if (done) return;
      done = true;
      abortPageActionProposal = null;
      if (row && row.parentNode) row.parentNode.removeChild(row);
      resolve(r);
    };
    // Stop: the browser cancels the decision; don't wait for its answer.
    abortPageActionProposal = function() { finish(null); };
    request().then(finish, function() { finish(null); });
  });
}

// After the reply to a typed page-action request (|token| is the
// reply's action_request): asks the browser for one proposal and shows
// it. Resolves once the proposal is chosen (or not); running it may
// still wait for the user's Allow. Never rejects.
function runPageActionProposal(token) {
  return askBrowserForProposal('Choosing a page action…', function() {
    return sendWithPromise('proposeChatAction', token);
  }).then(function(r) { handleChatProposal(r); });
}

// /fill ai and /paste (docs/design/AGENT_ACTIONS.md §4.7): the browser
// reads the page's text fields, has the on-device model choose values
// from the saved profile (|source| 'profile'; the browser reads it, it
// never comes through this page) or the pasted text ('paste', |pasted|)
// for at most 8 of them, checks the answer and describes every value
// itself. A form fill always comes with a reason, so it is shown with
// Allow / Deny in Ask first and Auto mode alike; Allow sends only the
// proposal's id. |hint| is what the user wrote after the command.
function runFormFillProposal(source, hint, pasted) {
  return askBrowserForProposal('Choosing what to fill…', function() {
    return sendWithPromise('proposeFormFill', source, hint, pasted);
  }).then(function(r) {
    if (r && typeof r === 'object' && r.status === 'none') {
      appendActionResult(false, 'Nothing filled', String(r.message || ''));
      return;
    }
    handleChatProposal(r, 'Nothing filled');
  });
}

// |failLabel| heads the browser's message when there is no proposal.
function handleChatProposal(r, failLabel) {
  if (!r || typeof r !== 'object') return;
  if (r.status === 'proposal') {
    var p = {id: Number(r.id) || 0, verb: String(r.verb || ''),
             summary: String(r.summary || ''),
             reason: String(r.reason || '')};
    if (!p.id || !p.summary) return;
    if (p.reason || (!actionAutoMode && !PROPOSAL_WITHOUT_CHIP[p.verb])) {
      askThenRunChatProposal(p, p.reason);
    } else {
      runChatProposal(p, false);
    }
  } else if (r.status === 'unavailable' || r.status === 'failed') {
    appendActionResult(false, failLabel || 'No page action',
                       String(r.message || ''));
  }
  // 'none' (no action fits the request) and 'cancelled': nothing to show.
}

function askThenRunChatProposal(p, reason) {
  _renderConfirmChip(reason ? 'Confirm: ' + reason : 'AI proposes a page action',
                     p.summary,
                     function() { runChatProposal(p, true); },
                     function() {
                       appendActionResult(false, p.summary, 'denied by user');
                     });
}

// |confirmed| is true only after the user tapped Allow on a chip for
// this proposal; the browser then acts on the reason it last showed.
function runChatProposal(p, confirmed) {
  var nav = (p.verb === 'NAVIGATE' || p.verb === 'SEARCH')
      ? appendStatusChip('🌐', p.summary) : null;
  sendWithPromise('runChatProposal', p.id, confirmed === true).then(
      function(r) {
        if (nav && nav.parentNode) nav.parentNode.removeChild(nav);
        if (r && r.needs_confirmation) {
          // The browser found (or the page changed to) something the user
          // must approve first, even in Auto mode.
          askThenRunChatProposal(p, String(r.reason || ''));
          return;
        }
        var ok = !!(r && r.success);
        appendActionResult(ok, p.summary,
                           r ? String((ok ? r.message : r.error) || '') : '');
      },
      function(err) {
        if (nav && nav.parentNode) nav.parentNode.removeChild(nav);
        appendActionResult(false, p.summary, String(err || 'failed'));
      });
}

function appendActionResult(ok, label, detail) {
  var m = document.getElementById('messages');
  if (!m) return;
  var d = document.createElement('div');
  d.className = 'molt-action-result ' + (ok ? 'ok' : 'fail');
  d.innerHTML = moltHTML((ok ? '&#10003; ' : '&#10007; ') + esc(label) +
                (detail ? ' <span class="detail">(' + esc(detail) + ')</span>' : ''));
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
}

// --------------------------------------------------------------
// Cloud honesty (P18): first-use consent per provider + a per-reply
// "Sent to <provider>" label. Local replies never get the label.
// --------------------------------------------------------------
function ensureCloudStyles() {
  if (document.getElementById('molt-cloud-styles')) return;
  var s = document.createElement('style');
  s.id = 'molt-cloud-styles';
  s.textContent =
      '.molt-cloud-consent .cc-body{white-space:normal}' +
      '.molt-cloud-consent ul{margin:4px 0 0 16px;padding:0}' +
      '.molt-cloud-consent li{margin:2px 0}' +
      '.molt-cloud-consent li.now{font-weight:600}' +
      '.message.ai.cloud-reply{flex-wrap:wrap}' +
      '.cloud-badge{flex-basis:100%;order:-1;font-size:10px;' +
      'color:var(--muted);margin:0 0 3px 4px}';
  document.head.appendChild(s);
}

// Resolves true when the user allows sending to r.provider_name.
// r.attached flags what THIS message would carry; the list shows every
// kind of context a cloud send can include so the choice is informed.
function askCloudConsent(r) {
  ensureCloudStyles();
  return new Promise(function(resolve) {
    var msgs = document.getElementById('messages');
    if (!msgs) { resolve(false); return; }
    var a = r.attached || {};
    var items = [
      ['history', 'Your message and the conversation so far'],
      ['page', 'Excerpts of the page you are viewing (when your message ' +
               'refers to it, or you use Summarize/Explain/Translate)'],
      ['tabs', 'Text from your open tabs (only with /ask-tabs)'],
      ['memory', 'Snippets from Memory, your browsing history (only when ' +
                 'Memory is on and your message refers to past reading)'],
      ['attachment', 'Documents you attach or load with /pdf'],
    ];
    var list = items.map(function(it) {
      var now = (it[0] === 'attachment') ? (a.attachment || a.pdf) : a[it[0]];
      return '<li' + (now ? ' class="now"' : '') + '>' + esc(it[1]) +
             (now ? ' — included in this message' : '') + '</li>';
    }).join('');
    var d = document.createElement('div');
    d.className = 'molt-action-confirm molt-cloud-consent';
    d.innerHTML = moltHTML(
        '<span class="ac-icon">☁️</span>' +
        '<div class="ac-body cc-body">' +
          '<div class="ac-title">Send to ' + esc(r.provider_name) + '?</div>' +
          '<div>This chat would leave your device: it goes to ' +
          esc(r.provider_name) + ' with your API key, under their privacy ' +
          'policy. Depending on what you ask, it can include:</div>' +
          '<ul>' + list + '</ul>' +
        '</div>' +
        '<div class="ac-buttons">' +
          '<button class="ac-deny">Cancel</button>' +
          '<button class="ac-allow">Allow</button>' +
        '</div>');
    msgs.appendChild(d);
    msgs.scrollTop = msgs.scrollHeight;
    var settle = function(ok) {
      return function() {
        try { d.parentNode && d.parentNode.removeChild(d); } catch (e) {}
        resolve(ok);
      };
    };
    d.querySelector('.ac-allow').onclick = settle(true);
    d.querySelector('.ac-deny').onclick = settle(false);
  });
}

// Fired by the backend right before a cloud reply streams in.
cr.addWebUiListener('ai-cloud-reply', function(providerName, model) {
  if (!currentAiMessageEl || !currentAiMessageEl.parentNode) return;
  ensureCloudStyles();
  var row = currentAiMessageEl.parentNode;
  if (row.querySelector('.cloud-badge')) return;
  row.classList.add('cloud-reply');
  var b = document.createElement('div');
  b.className = 'cloud-badge';
  b.textContent = '☁ Sent to ' + providerName +
                  (model ? ' · ' + model : '');
  row.insertBefore(b, row.firstChild);
});

// Bumped whenever a reply finishes, so sendMessage() can tell that a slash
// command already completed synchronously inside its dispatcher.
var genFinishEpoch = 0;

function setGenerating(val) {
  if (!val) genFinishEpoch++;
  isGenerating = val;
  var input = document.getElementById('chatInput');
  var sendBtn = document.getElementById('sendBtn');
  if (sendBtn) {
    // Stop replaces Send while generating.
    sendBtn.style.display = val ? 'none' : '';
    sendBtn.disabled = val || !(input && input.value.trim());
  }
  if (input) {
    input.disabled = val;
    // Collapse the autogrown textarea once its contents were sent.
    if (!input.value) input.style.height = 'auto';
  }
  var cancelBtn = document.getElementById('cancelBtn');
  if (cancelBtn) cancelBtn.className = 'cancel' + (val ? ' active' : '');
  var btns = document.querySelectorAll('#quickActions button');
  for (var i = 0; i < btns.length; i++) btns[i].disabled = val;
}

function updateContextBar() {
  var el = document.getElementById('contextBar');
  if (!el) return;
  var n = conversationHistory.length;
  if (n === 0) {
    el.textContent = '';
  } else {
    var chars = 0;
    for (var i = 0; i < n; i++) chars += conversationHistory[i].content.length;
    el.textContent = n + ' msgs \u00b7 ~' + Math.round(chars / 4) + ' tokens';
  }
}

// ---- Context Window Management ----

function trimHistory() {
  if (conversationHistory.length > MAX_HISTORY_MESSAGES) {
    conversationHistory = conversationHistory.slice(
        conversationHistory.length - MAX_HISTORY_MESSAGES);
  }
  // Every user turn is pushed and immediately followed by trimHistory,
  // so autosaving here persists the conversation even when the
  // generation that follows is interrupted.
  saveCurrentConversation();
}

// ---- Core Functions ----

// --------------------------------------------------------------
// Slash-command bridge to the active tab.
// Recognized forms (case-insensitive command, free-form remainder):
//   /click <selector>
//   /type <selector> <value...>
//   /scroll [pixels]            (default 600)
//   /navigate <url>
// Returns true if the text was a recognized action and was dispatched;
// the caller should skip the LLM path in that case.
// --------------------------------------------------------------
// --------------------------------------------------------------
// /help — prints the full slash-command list (the text that used to
// be the giant static welcome bubble). Purely local; no LLM call.
// --------------------------------------------------------------
var HELP_TEXT =
    "I'm your local AI assistant running entirely on this device.\n\n" +
    '**AI:** `/pdf`, `/bookmark <q>`, `/ask-tabs <q>`, `/history`, ' +
    '`/cluster <n> <q>`\n' +
    '**Reader:** `/simplify`, `/eli5`, `/summarize`, `/tldr`, ' +
    '`/chapters` (YouTube)\n' +
    '**Privacy:** `/trackers`, `/reputation`, `/hops`, `/tor status`, ' +
    '`/sandbox <url>`, `/js on|off`\n' +
    '**Actions:** `/triage list`, `/watch <url> <selector>`, `/receipt`, ' +
    '`/plan <task>`, `/fill`, `/fill ai`, `/paste`, `/click .button`\n' +
    '**Vault:** `/vault list`, `/vault find`, `/vault fill`\n' +
    '**Translate:** `/translate [lang] <text>`\n' +
    '**Digest:** `/digest` (last 24h briefing)\n\n' +
    'Or just send a message.';

function tryDispatchHelpCommand(text) {
  if (!/^\s*\/help\b/i.test(text)) return false;
  addUserMessage(text);
  var m = document.getElementById('messages');
  if (m) {
    var d = document.createElement('div');
    d.className = 'message ai';
    d.innerHTML = moltHTML('<div class="sender">AI Assistant</div>' +
        '<div class="text">' + renderMarkdown(HELP_TEXT) + '</div>');
    m.appendChild(d);
    m.scrollTop = m.scrollHeight;
  }
  return true;
}

// --------------------------------------------------------------
// URL of the page the active tab shows NOW, for the slash commands that act
// on "this page" when no URL is given (/pdf, /sandbox, /tor open, /js).
// Asked from the browser, like readActivePageText(): the side panel's last
// push (__moltLastTabContext) can lag behind the tab (a navigation or tab
// switch it hasn't reported yet), and acting on the previous page --
// turning JavaScript off for the wrong site, opening the wrong page in
// MoltNet -- is worse than a round trip. The push is used only when the
// native side can't name the active tab. Resolves '' if unknown.
// --------------------------------------------------------------
function readActiveTabUrl() {
  return sendWithPromise('getPageContext').then(function(ctx) {
    if (ctx && ctx.has_context) return ctx.url || '';
    var sp = window.__moltLastTabContext;
    return (sp && sp.url) || '';
  }).catch(function() { return ''; });
}

// |given| if non-empty, else the active tab's URL (readActiveTabUrl).
function urlOrActiveTab(given) {
  return given ? Promise.resolve(given) : readActiveTabUrl();
}

// --------------------------------------------------------------
// PDF chat: /pdf [url]
// With no arg, uses the active tab URL if it ends in .pdf. Otherwise
// fetches the supplied URL. The extracted text is stored as the next
// user turn's pre-pended context so any follow-up question grounds
// in the PDF.
// --------------------------------------------------------------
var pdfContext = null;  // {url, host, text} — primed by /pdf, consumed once
function tryDispatchPdfCommand(text) {
  var m = text.match(/^\s*\/pdf\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  addUserMessage(text);
  urlOrActiveTab(rest).then(function(url) {
    // With no argument, the active tab counts only if it shows a PDF.
    if (!rest && !/\.pdf(\?|#|$)/i.test(url)) url = '';
    if (!url) {
      addErrorMessage('Usage: /pdf <url>  (or open a .pdf tab first)');
      setGenerating(false);
      return;
    }
    if (!/^https?:\/\//i.test(url) && !/^file:\/\//i.test(url)) {
      url = 'https://' + url;
    }
    startAiMessage();
    appendToAiMessage('Fetching ' + url + '...');
    extractPdfIntoContext(url);
  });
  return true;
}

function extractPdfIntoContext(url) {
  sendWithPromise('extractPdfText', url).then(function(r) {
    if (!r.success) {
      currentAiText = '✗ ' + (r.error || 'PDF extraction failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var host = '';
    try { host = new URL(r.url).host; } catch (e) {}
    pdfContext = {url: r.url, host: host, text: r.text};
    var preview = (r.text || '').slice(0, 280);
    currentAiText = '✓ Loaded ' + r.char_count + ' chars from ' +
                    (host || r.url) + '. Ask me anything about it.\n\n' +
                    '"' + preview + (r.char_count > 280 ? '…' : '') + '"';
    finishAiMessage();
    setGenerating(false);
  });
}

// --------------------------------------------------------------
// Smart bookmarks: /bookmark <query> | /bm <query>
// Keyword-ranks the user's bookmarks and shows top hits with one-click
// open links.
// --------------------------------------------------------------
function tryDispatchBookmarksCommand(text) {
  var m = text.match(/^\s*\/(bookmark|bm)\b\s*(.*)$/i);
  if (!m) return false;
  var query = (m[2] || '').trim();
  if (!query) {
    addErrorMessage('Usage: /bookmark <query>');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Searching bookmarks...');
  sendWithPromise('searchBookmarks', query, 10).then(function(r) {
    var hits = (r && r.hits) || [];
    if (!hits.length) {
      currentAiText = 'No bookmark matched "' + query + '".';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    function esc(s) { return (s + '').replace(/&/g,'&amp;')
                                     .replace(/</g,'&lt;')
                                     .replace(/>/g,'&gt;')
                                     .replace(/"/g,'&quot;'); }
    var lines = ['Top bookmark matches:'];
    hits.forEach(function(h, i) {
      lines.push((i + 1) + '. ' + esc(h.title) +
                 ' — ' + esc(h.host) +
                 ' (score ' + h.score + ')' +
                 (h.parent ? '  · ' + esc(h.parent) : '') +
                 '\n   ' + esc(h.url));
    });
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Cross-tab Q&A: /ask-tabs <question> | /ask <question>
// Pulls innerText from every tab in this window, chunks it, ranks
// chunks against the question, and asks the LLM with the top-K
// chunks as grounding context. The standard chunk-ranker is reused.
// --------------------------------------------------------------
function tryDispatchAskTabsCommand(text) {
  var m = text.match(/^\s*\/(ask-tabs|asktabs|ask)\b\s*(.*)$/i);
  if (!m) return false;
  var question = (m[2] || '').trim();
  if (!question) {
    addErrorMessage('Usage: /ask-tabs <question>');
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading all open tabs...');
  sendWithPromise('extractAllTabsText', 4000).then(function(r) {
    var tabs = (r && r.tabs) || [];
    if (!tabs.length) {
      currentAiText = 'No tabs to read.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    // Build a combined text labeled per tab.
    var labeled = '';
    tabs.forEach(function(t) {
      if (!t.text) return;
      labeled += '\n\n[' + (t.title || t.url) + ']\n' + t.text;
    });
    // Chunk + rank with existing helpers.
    var chunks = chunkPageText(labeled, 600);
    var top = rankChunksByQuery(chunks, question, 6);
    var ctx = top.join('\n\n---\n\n');
    var prompt = 'Question: ' + question;
    var historyForPrompt = '';
    var pageCtx =
      'Cross-tab Q&A (' + tabs.length + ' tabs). Use only these snippets:\n' +
      ctx;
    currentAiText = '';
    // Restart the AI message bubble since we'll stream into it.
    startAiMessage();
    sendWithPromise('sendPrompt', prompt, historyForPrompt, pageCtx, '',
                    activeModelId || pickedModelId || '')
        .then(function(result) {
          finishAiMessage();
          setGenerating(false);
          if (result && !result.success && result.error) {
            addErrorMessage(result.error);
          }
        }).catch(function() {
          finishAiMessage();
          setGenerating(false);
        });
  });
  return true;
}

// --------------------------------------------------------------
// Sandbox tab: /sandbox <url>
// Opens the URL in an off-the-record window. Cookies and storage
// from that session vanish when the window closes.
// --------------------------------------------------------------
function tryDispatchSandboxCommand(text) {
  var m = text.match(/^\s*\/sandbox\b\s*(.*)$/i);
  if (!m) return false;
  addUserMessage(text);
  urlOrActiveTab((m[1] || '').trim()).then(function(url) {
    if (!url) {
      addErrorMessage('Usage: /sandbox <url>');
      setGenerating(false);
      return;
    }
    startAiMessage();
    sendWithPromise('openSandboxTab', url).then(function(r) {
      currentAiText = r.success
          ? '✓ Opened ' + r.url + ' in a sandbox window. ' +
            'Cookies/storage will be discarded on close.'
          : '✗ ' + (r.error || 'sandbox open failed');
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Privacy heatmap: /trackers
// Asks the active tab for a breakdown of third-party resources it
// loaded, grouped by ads / analytics / cdn / other. The JS probe is
// implemented in the C++ handler — we only render the result here.
// --------------------------------------------------------------
function tryDispatchTrackersCommand(text) {
  if (!/^\s*\/trackers\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Scanning active tab for third-party resources...');
  sendWithPromise('getTrackerBreakdown').then(function(r) {
    if (!r || r.error) {
      currentAiText = '✗ ' + (r && r.error || 'tracker scan failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var c = r.counts || {};
    var total = r.total_third_party || 0;
    var lines = [
      'Privacy heatmap for ' + (r.origin || 'this tab') + ':',
      '  First-party resources: ' + (c.first_party || 0),
      '  Third-party total:     ' + total +
        '   (across ' + (r.unique_third_party_hosts || 0) +
        ' unique hosts)',
      '    · ads:       ' + (c.ads || 0),
      '    · analytics: ' + (c.analytics || 0),
      '    · cdn:       ' + (c.cdn || 0),
      '    · other:     ' + (c.other || 0)
    ];
    if (r.top_hosts && r.top_hosts.length) {
      lines.push('');
      lines.push('Top third-party hosts:');
      r.top_hosts.forEach(function(h){
        lines.push('  [' + h.category + '] ' + h.host + ' × ' + h.count);
      });
    }
    if (total === 0) {
      lines.push('');
      lines.push('Nothing third-party loaded — clean page.');
    }
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Domain reputation: /reputation [host] | /rep [host]
// Uses Personal Vector Memory data to tell the user how many times
// they've visited this host, how much they've read, when first and
// last. Plus a tiny conservative risk-flag heuristic for sketchy
// TLDs / raw-IP hosts.
// --------------------------------------------------------------
function tryDispatchReputationCommand(text) {
  var m = text.match(/^\s*\/(reputation|rep)\b\s*(.*)$/i);
  if (!m) return false;
  var hostArg = (m[2] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Looking up domain reputation...');
  sendWithPromise('getDomainReputation', hostArg).then(function(r) {
    if (!r || r.error) {
      currentAiText = '✗ ' + (r && r.error || 'reputation lookup failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    function fmt(unix) {
      if (!unix) return '—';
      var d = new Date(unix * 1000);
      return d.toLocaleString();
    }
    var lines = ['Domain reputation for ' + (r.host || 'unknown') + ':'];
    if (r.first_visit_on_device) {
      lines.push('  • First visit on this device.');
    } else {
      lines.push('  • Visited ' + r.visit_count + ' time' +
                 (r.visit_count === 1 ? '' : 's') + ' before.');
      lines.push('  • First seen: ' + fmt(r.first_visit_unix));
      lines.push('  • Last seen:  ' + fmt(r.last_visit_unix));
      lines.push('  • Total content read: ' +
                 (r.total_words_read || 0).toLocaleString() + ' words');
    }
    if (r.risky_tld) {
      lines.push('  ⚠ TLD often associated with low-cost / abuse-heavy ' +
                 'registrations — be careful with credentials.');
    }
    if (r.is_ip_host) {
      lines.push('  ⚠ Raw IP address as host. Legitimate sites almost ' +
                 'always have a domain name.');
    }
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Connection path: /hops [url]
// v0: shows the URL structure, OTR session status, and a 3-hop
// you → ISP → destination diagram. Honest about not yet routing
// through Tor — that's the next batch.
// --------------------------------------------------------------
function tryDispatchHopsCommand(text) {
  var m = text.match(/^\s*\/hops\b\s*(.*)$/i);
  if (!m) return false;
  var url = (m[1] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Tracing connection path...');
  // We fan out two queries in parallel: the local connection-path
  // template and the live Tor circuit data. If Tor is up, we fold its
  // relays into the hop diagram. This is the same response shape
  // Phase B.2 will hit when routing through Tor — the visualizer
  // doesn't need to change when routing arrives.
  Promise.all([
    sendWithPromise('getConnectionPath', url),
    sendWithPromise('getTorCircuits').catch(function(){ return null; })
  ]).then(function(arr) {
    var r = arr[0];
    var torR = arr[1];
    if (!r || r.error) {
      currentAiText = '✗ ' + (r && r.error || 'connection path failed');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var lines = ['Connection path to ' + r.host + ':'];
    lines.push('');
    var hops = r.hops || [];
    // Pick a built circuit's relays to splice in between "Your ISP"
    // and the destination. The visualizer still labels these as
    // "would-route-through" until Phase B.2 actually pipes traffic.
    var torHops = null;
    if (torR && torR.circuits && torR.circuits.length) {
      for (var i = 0; i < torR.circuits.length; i++) {
        var c = torR.circuits[i];
        if (c.state === 'BUILT' && c.hops && c.hops.length >= 2) {
          torHops = c.hops;
          break;
        }
      }
    }
    if (torHops) {
      // Replace the middle "Your ISP" hop with the Tor relay chain.
      var newHops = [hops[0]];
      torHops.forEach(function(h, idx) {
        var role = idx === 0 ? 'guard'
                  : (idx === torHops.length - 1 ? 'exit' : 'middle');
        var name = h.nickname || h.fingerprint.slice(0, 8) + '…';
        var flag = h.country ? ccToFlag(h.country) + ' ' : '';
        var country = h.country ? ' (' + h.country + ')' : '';
        newHops.push({
          label: flag + 'Tor ' + role + ': ' + name + country,
          detail: (h.ip ? h.ip + '  ·  ' : '') + h.fingerprint
        });
      });
      newHops.push(hops[hops.length - 1]);
      hops = newHops;
    }
    for (var i2 = 0; i2 < hops.length; i2++) {
      var h2 = hops[i2];
      var marker = (i2 === 0) ? '●'
                  : (i2 === hops.length - 1 ? '○' : '─');
      lines.push('  ' + marker + ' ' + h2.label +
                 (h2.detail ? '  — ' + h2.detail : ''));
      if (i2 < hops.length - 1) lines.push('  │');
    }
    lines.push('');
    lines.push('Scheme: ' + r.scheme +
               (r.is_https ? '  (TLS-encrypted)' : '  (PLAINTEXT)'));
    if (r.is_anonymous_session) {
      lines.push('Session: ✓ Anonymous (sandbox tab).');
    }
    if (torHops) {
      lines.push('Tor:     ✓ live circuit shown above (relays from your ' +
                 'local Tor instance). Routing your tabs through them ' +
                 'lands in Phase B.2.');
    } else if (r.notes) {
      lines.push('');
      lines.push('Note: ' + r.notes);
    }
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// MoltNet (Tor): /tor [status|circuit|circuits|launch|open|close|help]
// Talks only to MoltNet's own Tor (SAFECOOKIE-authenticated control port
// on an auto-selected port). /tor open loads a URL in a MoltNet window —
// the isolated Tor profile — never in the regular or incognito profile.
// --------------------------------------------------------------
function renderTorCircuits(circuits) {
  if (!circuits || !circuits.length) {
    return 'No circuits yet — Tor is still bootstrapping. Try again in a few seconds.';
  }
  var lines = ['Live Tor circuits (' + circuits.length + '):'];
  circuits.forEach(function(c) {
    var hopStr = '';
    if (c.hops && c.hops.length) {
      hopStr = c.hops.map(function(h){
        var name = h.nickname ? h.nickname
                              : h.fingerprint.slice(0, 8) + '…';
        var flag = h.country ? ccToFlag(h.country) + ' ' : '';
        return flag + name +
               (h.country ? ' (' + h.country + ')' : '');
      }).join('  →  ');
    } else {
      hopStr = '(no path yet)';
    }
    lines.push('  [' + c.id + ' ' + c.state +
               (c.purpose ? ' · ' + c.purpose : '') + ']  ' + hopStr);
  });
  return lines.join('\n');
}

// ISO 3166-1 alpha-2 → emoji flag. Tor's GETINFO ip-to-country
// returns two-letter codes; we render each by mapping each letter to
// the corresponding Regional Indicator Symbol (U+1F1E6..U+1F1FF).
function ccToFlag(cc) {
  if (!cc || cc.length !== 2) return '';
  var A = 0x1F1E6;
  var hi = cc.toUpperCase().charCodeAt(0) - 65 + A;
  var lo = cc.toUpperCase().charCodeAt(1) - 65 + A;
  if (hi < A || lo < A) return '';
  return String.fromCodePoint(hi) + String.fromCodePoint(lo);
}

// The MoltNet exit-country selector moved out of the chat panel and
// into the browser toolbar (globe button menu). The
// getTorExitCountries / setTorExitCountry endpoints stay registered
// on the handler for the settings page and toolbar.

function tryDispatchTorCommand(text) {
  var m = text.match(/^\s*\/tor\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var parts = rest.split(/\s+/);
  var sub = (parts[0] || 'status').toLowerCase();
  addUserMessage(text);
  startAiMessage();
  if (sub === 'help') {
    currentAiText =
      'Tor commands:\n' +
      '  /tor status            — is Tor available? running? bundled or system?\n' +
      '  /tor circuit           — live circuit hops with country flags\n' +
      '  /tor launch            — start the bundled tor (no install needed)\n' +
      '  /tor open <url>        — open URL in a MoltNet (Tor) window\n' +
      '  /tor close             — close MoltNet windows and stop tor\n' +
      '\n' +
      'MoltBrowser ships with tor bundled inside the app. Run\n' +
      '/tor launch to start it, /tor open <url> to use it.\n' +
      'No external installation required.';
    finishAiMessage();
    setGenerating(false);
    return true;
  }
  if (sub === 'launch') {
    appendToAiMessage('Looking for tor binary and launching... ' +
                       '(bootstrap may take 15-30s)');
    sendWithPromise('launchTor').then(function(r) {
      if (r.success) {
        currentAiText = '✓ Tor launched.\n' +
                        '  Binary: ' + (r.binary_path || '?') + '\n' +
                        '  PID:    ' + (r.pid || '?') + '\n\n' +
                        'Use /tor open <url> to open a page in a MoltNet window.';
      } else {
        currentAiText = '✗ ' + (r.error || 'launch failed');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  if (sub === 'close' || sub === 'stop' || sub === 'kill') {
    sendWithPromise('stopTor').then(function() {
      currentAiText = '✓ Closed MoltNet windows and stopped MoltNet\'s tor.';
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  if (sub === 'open') {
    urlOrActiveTab(parts.slice(1).join(' ').trim()).then(function(url) {
      if (!url) {
        currentAiText = 'Usage: /tor open <url>';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      appendToAiMessage('Starting Tor (if needed) and opening ' + url +
                         ' in a MoltNet window...');
      sendWithPromise('openTorTab', url).then(function(r) {
        if (r.success) {
          currentAiText = '✓ Opened ' + r.url + ' in a MoltNet window.\n\n' +
                          'Only MoltNet windows use Tor; they have their own ' +
                          'cookies, discarded when the last one closes.';
        } else {
          currentAiText = '✗ ' + (r.error || 'open-tor-tab failed');
        }
        finishAiMessage();
        setGenerating(false);
      });
    });
    return true;
  }
  if (sub === 'circuit' || sub === 'circuits') {
    appendToAiMessage('Reading circuit list from Tor control port...');
    sendWithPromise('getTorCircuits').then(function(r) {
      currentAiText = renderTorCircuits(r && r.circuits);
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }
  // status (default)
  appendToAiMessage('Checking MoltNet\'s Tor...');
  sendWithPromise('getTorStatus').then(function(r) {
    if (!r.running) {
      var src = r.binary_source || 'none';
      var srcLine =
        src === 'bundled' ? '  Binary: ✓ bundled (' + r.binary_path + ')'
        : src === 'system' ? '  Binary: ⚠ system tor (' + r.binary_path + ')'
        : '  Binary: ✗ none found';
      currentAiText = '✗ Tor is not running yet.\n\n' +
                      srcLine + '\n\n' +
                      (r.install_hint || '');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var src = r.binary_source || 'unknown';
    var srcLabel =
      src === 'bundled' ? '✓ bundled (no install required)'
      : src === 'system' ? '⚠ system tor'
      : 'unknown';
    var lines = [
      '✓ Tor is running.',
      '  Source:       ' + srcLabel,
      '  Binary:       ' + (r.binary_path || '?'),
      '  Version:      ' + (r.version || 'unknown'),
      '  Control port: ' + r.control_port,
      '  SOCKS port:   ' + r.socks_port,
    ];
    // Pull circuits for a one-shot snapshot under the status.
    sendWithPromise('getTorCircuits').then(function(c) {
      lines.push('');
      lines.push(renderTorCircuits(c && c.circuits));
      lines.push('');
      lines.push('Only MoltNet windows are routed through Tor ' +
                 '(/tor open <url>).');
      currentAiText = lines.join('\n');
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Selective JS toggle: /js on | /js off [host]
// HostContentSettingsMap toggle. Reloads active tab so the change
// takes effect right now.
// --------------------------------------------------------------
function tryDispatchJsCommand(text) {
  var m = text.match(/^\s*\/js\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var parts = rest.split(/\s+/);
  var enabled;
  if (parts[0] === 'on' || parts[0] === 'enable') enabled = true;
  else if (parts[0] === 'off' || parts[0] === 'disable') enabled = false;
  else {
    addErrorMessage('Usage: /js on | /js off  [host]');
    setGenerating(false);
    return true;
  }
  addUserMessage(text);
  var given = parts[1] || '';
  (given ? Promise.resolve(given) : readActiveTabUrl().then(function(url) {
    try { return new URL(url).host; } catch (e) { return ''; }
  })).then(function(host) {
    if (!host) {
      addErrorMessage('No host. Try /js ' + parts[0] + ' example.com');
      setGenerating(false);
      return;
    }
    startAiMessage();
    sendWithPromise('setJsForDomain', host, enabled).then(function(r) {
      currentAiText = r.success
          ? '✓ JavaScript ' + (enabled ? 'enabled' : 'disabled') +
            ' for ' + host + '. Active tab reloaded.'
          : '✗ ' + (r.error || 'JS toggle failed');
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Tab Triage: /triage [close-inactive | close-domain <host> | list]
// /triage list  -> show all tabs in this window with a snippet.
// /triage close-inactive  -> close every non-active tab.
// /triage close-domain x.com  -> close every tab whose host == x.com.
// /triage bookmark-inactive -> bookmark every non-active tab to "Other".
// --------------------------------------------------------------
function tryDispatchTriageCommand(text) {
  var m = text.match(/^\s*\/triage\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Scanning open tabs...');
  sendWithPromise('listTabsInWindow').then(function(r) {
    var tabs = (r && r.tabs) || [];
    if (!rest || rest === 'list') {
      var lines = ['Open tabs in this window (' + tabs.length + '):'];
      for (var i = 0; i < tabs.length; i++) {
        var t = tabs[i];
        var prefix = (t.active ? '\u25CF ' : '  ') +
                     (t.pinned ? '\u{1F4CC} ' : '');
        lines.push(prefix + '[' + t.index + '] ' + (t.snippet || t.url));
      }
      currentAiText = lines.join('\n');
      finishAiMessage();
      setGenerating(false);
      return;
    }
    // Pick indices to act on.
    var picked = [];
    var action = 'close';
    var parts = rest.split(/\s+/);
    var verb = parts[0];
    if (verb === 'close-inactive') {
      action = 'close';
      tabs.forEach(function(t){ if (!t.active && !t.pinned) picked.push(t.index); });
    } else if (verb === 'bookmark-inactive') {
      action = 'bookmark';
      tabs.forEach(function(t){ if (!t.active) picked.push(t.index); });
    } else if (verb === 'pin-active') {
      action = 'pin';
      tabs.forEach(function(t){ if (t.active) picked.push(t.index); });
    } else if (verb === 'close-domain' && parts[1]) {
      action = 'close';
      var host = parts[1].toLowerCase();
      tabs.forEach(function(t){
        try {
          var u = new URL(t.url);
          if (u.host.toLowerCase().indexOf(host) !== -1) picked.push(t.index);
        } catch (e) {}
      });
    } else {
      currentAiText = 'Usage:\n' +
                      '  /triage list\n' +
                      '  /triage close-inactive\n' +
                      '  /triage close-domain <host>\n' +
                      '  /triage bookmark-inactive\n' +
                      '  /triage pin-active';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    if (!picked.length) {
      currentAiText = 'No tabs matched.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    sendWithPromise('triageActOnTabs',
                    {action: action, indices: picked}).then(function(rr) {
      currentAiText = rr.success
          ? '\u2713 ' + action + 'd ' + rr.affected_count + ' tab' +
            (rr.affected_count === 1 ? '' : 's')
          : '\u2717 ' + (rr.error || 'triage failed');
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Page Watcher: /watch <url> <selector> [interval-seconds] [name...]
// Creates a scheduled INTERVAL script that opens the URL, extracts the
// selector text, and fires an OS notification with the current value.
// --------------------------------------------------------------
function tryDispatchWatchCommand(text) {
  var m = text.match(/^\s*\/watch\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  if (!rest) {
    addErrorMessage('Usage: /watch <url> <css-selector> ' +
                    '[interval-seconds] [name]');
    return true;
  }
  // First token = url, second = selector, third (optional) = seconds,
  // remainder = display name.
  var parts = rest.split(/\s+/);
  if (parts.length < 2) {
    addErrorMessage('Usage: /watch <url> <css-selector> ' +
                    '[interval-seconds] [name]');
    return true;
  }
  var url = parts[0];
  if (!/^https?:\/\//i.test(url)) url = 'https://' + url;
  var sel = parts[1];
  var seconds = 900;
  var nameStart = 2;
  if (parts.length >= 3 && /^\d+$/.test(parts[2])) {
    seconds = parseInt(parts[2], 10);
    nameStart = 3;
  }
  var name = parts.slice(nameStart).join(' ') || '';
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Creating watcher...');
  sendWithPromise('createWatcher',
                  {url: url, selector: sel,
                   interval_seconds: seconds,
                   name: name}).then(function(r) {
    if (r.success) {
      currentAiText = '\u2713 Watching ' + url + '\n' +
                      'Selector: ' + sel + '\n' +
                      'Every ' + r.interval_seconds + 's\n' +
                      'Script id: ' + r.script_id;
    } else {
      currentAiText = '\u2717 ' + (r.error || 'watcher creation failed');
    }
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Form Filler: /fill  (autofill the active tab from the saved profile)
//              /profile  (open the encrypted profile editor)
// --------------------------------------------------------------
function tryDispatchFillCommand(text) {
  if (/^\s*\/profile\b/i.test(text)) {
    addUserMessage(text);
    openProfileEditor();
    setGenerating(false);
    return true;
  }
  if (!/^\s*\/fill\b/i.test(text)) return false;
  // "/fill ai" is tryDispatchFillAICommand's.
  if (/^\s*\/fill\s+ai\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Filling form...');
  sendWithPromise('runFormFill').then(function(r) {
    if (r.success) {
      currentAiText = '\u2713 Filled ' + r.filled + ' field' +
                      (r.filled === 1 ? '' : 's') +
                      ' (of ' + r.total + ' on this page)';
    } else {
      currentAiText = '\u2717 ' + (r.error || 'fill failed') +
                      '\n\nType /profile to set up your saved profile.';
    }
    finishAiMessage();
    setGenerating(false);
  }).catch(function(e) {
    currentAiText = '\u2717 ' + (e || 'fill failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// Render an inline profile editor in the messages area. Loads the
// encrypted profile via getMoltProfile and saves it back via
// saveMoltProfile. Keeps the WebUI surface area small — no separate
// settings page, no extension UI.
function openProfileEditor() {
  startAiMessage();
  appendToAiMessage('Loading your saved profile...');
  sendWithPromise('getMoltProfile').then(function(r) {
    var p = (r && r.profile) || {};
    var fields = [
      ['full_name',    'Full name'],
      ['first_name',   'First name'],
      ['last_name',    'Last name'],
      ['email',        'Email'],
      ['phone',        'Phone'],
      ['address_line1','Address line 1'],
      ['address_line2','Address line 2'],
      ['city',         'City'],
      ['state',        'State / Province'],
      ['zip',          'Postal code'],
      ['country',      'Country'],
      ['company',      'Company'],
      ['job_title',    'Job title'],
      ['website',      'Website']
    ];
    function esc(s){return (s+'').replace(/&/g,'&amp;').replace(/</g,'&lt;')
                                  .replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
    var rows = fields.map(function(f){
      return '<div class="profile-row">' +
             '<label>' + f[1] + '</label>' +
             '<input data-key="' + f[0] + '" type="text" value="' +
                escAttr(p[f[0]] || '') + '">' +
             '</div>';
    }).join('');
    currentAiText = '__PROFILE_EDITOR__';
    finishAiMessage();
    // sendMessage() marked the chat busy after dispatching /profile; the
    // editor is the whole answer, so give the composer back.
    setGenerating(false);
    // Replace the last AI message with the editor markup.
    var msgs = document.querySelectorAll('#messages .message.ai');
    var last = msgs[msgs.length - 1];
    if (!last) return;
    // A saved profile exists but couldn't be decrypted (keyring locked,
    // another machine's key, corrupt file). It is never overwritten
    // silently; say so instead of showing what looks like no profile.
    var unreadableNote = (r && r.unreadable)
        ? '<div class="profile-help">\u26a0 Your saved profile couldn\'t be ' +
          'decrypted (is your system keyring locked?). It has not been ' +
          'changed. Unlock the keyring and run <code>/profile</code> ' +
          'again to edit it.</div>'
        : '';
    last.querySelector('.text').innerHTML =
        moltHTML('<div class="profile-editor">' +
        unreadableNote +
        '<div class="profile-help">Encrypted at rest via your OS keychain. ' +
        'Used by <code>/fill</code> to autofill forms on http(s) pages.</div>' +
        rows +
        '<div class="profile-actions">' +
        '<button class="profile-save">Save profile</button>' +
        '<button class="profile-cancel">Cancel</button>' +
        '</div></div>');
    // opts.backupUnreadable: the user chose to start over; the unreadable
    // profile is renamed to a backup next to it, never deleted.
    function saveProfile(dict, opts) {
      sendWithPromise('saveMoltProfile', dict, opts || {}).then(function(rr){
        var box = last.querySelector('.text');
        var n = Object.keys(dict).length;
        if (rr && rr.success) {
          var msg = '\u2713 Profile saved (' + n + ' field' +
                    (n === 1 ? '' : 's') + ')';
          if (rr.backup) {
            msg += '. The profile that couldn\'t be decrypted was kept as ' +
                   '~/.moltbrowser/' + rr.backup + '.';
          }
          box.textContent = msg;
          return;
        }
        box.textContent = '\u2717 ' + ((rr && rr.error) || 'save failed');
        if (rr && rr.unreadable) {
          var actions = document.createElement('div');
          actions.className = 'profile-actions';
          var startOver = document.createElement('button');
          startOver.className = 'profile-save';
          startOver.textContent =
              'Start a new profile (keep the old one as a backup)';
          startOver.addEventListener('click', function() {
            startOver.disabled = true;
            saveProfile(dict, {backupUnreadable: true});
          });
          actions.appendChild(startOver);
          box.appendChild(actions);
        }
      });
    }
    last.querySelector('.profile-save').addEventListener('click', function(){
      var dict = {};
      last.querySelectorAll('input[data-key]').forEach(function(el){
        var v = el.value.trim();
        if (v) dict[el.getAttribute('data-key')] = v;
      });
      saveProfile(dict);
    });
    last.querySelector('.profile-cancel').addEventListener('click', function(){
      last.querySelector('.text').textContent = 'Profile editor closed.';
    });
  }).catch(function() {
    currentAiText = '✗ Could not load your saved profile.';
    finishAiMessage();
    setGenerating(false);
  });
}

// --------------------------------------------------------------
// AI-grouped history: /history [limit]
// Loads up to N (default 200, max 2000) recent documents from
// Personal Vector Memory and groups them by simple keyword overlap
// of their titles. Each cluster renders as a collapsible card
// with title-derived topic + count + list of pages.
//
// We do the clustering here in JS so the user can re-cluster on
// filter (by host, by date range) without an IPC round-trip.
// --------------------------------------------------------------
// --------------------------------------------------------------
// Reader 2.0: /simplify, /eli5, /summarize, /tldr
// Take the active page's captured innerText, wrap it in a per-mode
// framing prompt, and stream the LLM's rewrite into chat. All four
// modes share the same plumbing — only the prompt template differs.
// --------------------------------------------------------------
// Text of the page the active tab shows NOW, for the slash commands that
// read it (A7). The side panel's pushed text (__moltCurrentTabContext) is
// used only when it was read from that document: same URL and committed
// navigation entry as getPageContext reports, and its capture finished --
// the check sendMessage() makes. When the native side can't name the
// active tab, the push is the only (and current) source, as there.
// Otherwise the page is read again through getPageContent, which reports
// the document it read and drops the text if the tab moved on meanwhile.
// Resolves {url, title, text} with at most maxChars of text, or null.
function readActivePageText(maxChars) {
  return sendWithPromise('getPageContext').then(function(ctx) {
    var sp = window.__moltCurrentTabContext;
    var named = !!(ctx && ctx.has_context);
    var spCurrent = !!sp && !sp.text_pending && !!sp.text &&
        (!named || (sp.url === ctx.url &&
                    sp.nav_entry_id === ctx.nav_entry_id));
    if (spCurrent) {
      return {url: sp.url || '', title: sp.title || '',
              text: sp.text.slice(0, maxChars)};
    }
    if (!named) return null;
    return sendWithPromise('getPageContent', maxChars).then(function(r) {
      if (!r || !r.has_content || !r.content) return null;
      return {url: r.url || '', title: r.title || '',
              text: String(r.content).slice(0, maxChars)};
    });
  }).catch(function() { return null; });
}

function tryDispatchReaderCommand(text) {
  var m = text.match(/^\s*\/(simplify|eli5|summarize|tldr)\b\s*(.*)$/i);
  if (!m) return false;
  var mode = m[1].toLowerCase();
  var extra = (m[2] || '').trim();
  addUserMessage(text);
  // Cap input so we don't spend half the context window on boilerplate.
  // 12k chars ≈ ~3k tokens, leaves room for the generation budget on
  // a 4k-context bundled model.
  readActivePageText(12000).then(function(page) {
    if (!page || !page.text) {
      addErrorMessage('Open an http(s) page first — Reader needs page content.');
      setGenerating(false);
      return;
    }
    runReaderPrompt(mode, extra, page);
  });
  return true;
}

function runReaderPrompt(mode, extra, page) {
  var pageText = page.text;
  var framings = {
    simplify:
      'Rewrite the following web page in plain, accessible language ' +
      'without losing accuracy. Use short sentences. Keep numbers, ' +
      'names, and dates exact.' +
      (extra ? ' Focus especially on: ' + extra + '.' : ''),
    eli5:
      'Explain this web page like the reader is five years old. Use ' +
      'playful, concrete analogies. Avoid jargon entirely. Keep it ' +
      'short — under 200 words.' +
      (extra ? ' The reader is most curious about: ' + extra + '.' : ''),
    summarize:
      'Summarize this web page in 3-5 tight bullet points. Each bullet ' +
      'is one sentence and captures one fact, claim, or conclusion.' +
      (extra ? ' Bias the summary toward: ' + extra + '.' : ''),
    tldr:
      'Give a single-sentence TL;DR of this web page that a busy ' +
      'reader could absorb in two seconds.'
  };
  var prompt = framings[mode] +
               '\n\nPage title: ' + (page.title || '(untitled)') +
               '\n\nPage content:\n' + pageText;
  startAiMessage();
  sendWithPromise('sendPrompt', prompt, /*history=*/'', /*ctx=*/'',
                  /*origin=*/'', activeModelId || pickedModelId || '').then(
      function(result) {
        finishAiMessage();
        setGenerating(false);
        if (result && !result.success && result.error) {
          addErrorMessage(result.error);
        }
      }).catch(function() {
        finishAiMessage();
        setGenerating(false);
      });
}

// --------------------------------------------------------------
// Receipt extractor: /receipt
// LLM extracts a strict JSON {merchant,date,total,currency,items} from
// the active page (which should be a checkout confirmation or invoice
// view). We parse it client-side and append to a local CSV ledger
// via the new appendReceipt IPC. The ledger lives at
// ~/.moltbrowser/ledger.csv — plaintext so you can pull it into a
// spreadsheet without any tooling.
// --------------------------------------------------------------
function tryDispatchReceiptCommand(text) {
  if (!/^\s*\/receipt\b/i.test(text)) return false;
  addUserMessage(text);
  readActivePageText(10000).then(function(page) {
    if (!page || !page.text) {
      addErrorMessage('Open a receipt / invoice / order-confirmation page first.');
      setGenerating(false);
      return;
    }
    runReceiptPrompt(page);
  });
  return true;
}

function runReceiptPrompt(page) {
  startAiMessage();
  var schema =
    '{"merchant":"<string>","date":"YYYY-MM-DD",' +
    '"total":<number>,"currency":"<ISO 4217 code, default USD>",' +
    '"items":[{"name":"<string>","qty":<number>,"price":<number>}]}';
  var prompt =
    'You are a receipt extractor. Read the page below and respond with ' +
    'ONE JSON object matching this schema exactly:\n' + schema + '\n\n' +
    'Rules:\n' +
    '- If the page is not a receipt or invoice, respond with: ' +
    '{"error":"not a receipt"}\n' +
    '- Date must be ISO YYYY-MM-DD. If only the month and year are ' +
    'visible, use the first day.\n' +
    '- total is the grand total INCLUDING tax and shipping.\n' +
    '- If you can\'t find an item-level breakdown, return items: [].\n' +
    '- Respond with JSON only, no prose, no markdown fences.\n\n' +
    'Page title: ' + (page.title || '(untitled)') +
    '\nPage URL: ' + (page.url || '') +
    '\n\nPage content:\n' + page.text;
  sendWithPromise('sendPrompt', prompt, '', '', '',
                  activeModelId || pickedModelId || '').then(function() {
    // Grab whatever the LLM streamed into the current AI message bubble.
    // currentAiText holds the accumulated text.
    var jsonStr = currentAiText;
    // Best-effort JSON extraction: take the first balanced { ... } run.
    var first = jsonStr.indexOf('{');
    var last = jsonStr.lastIndexOf('}');
    if (first < 0 || last <= first) {
      currentAiText += '\n\n✗ Could not find JSON in the LLM response.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var slice = jsonStr.substring(first, last + 1);
    var data;
    try { data = JSON.parse(slice); }
    catch (e) {
      currentAiText += '\n\n✗ Invalid JSON: ' + e;
      finishAiMessage();
      setGenerating(false);
      return;
    }
    if (data.error) {
      currentAiText = '✗ ' + data.error;
      finishAiMessage();
      setGenerating(false);
      return;
    }
    sendWithPromise('appendReceipt', data, page.url || '').then(function(r) {
      if (r.success) {
        currentAiText = '✓ Saved receipt: ' +
            (data.merchant || '?') + ' ' +
            (data.date || '') + ' ' +
            (data.currency || 'USD') + ' ' +
            (typeof data.total === 'number' ? data.total.toFixed(2)
                                            : data.total) + '\n' +
            'Ledger: ' + r.path;
      } else {
        currentAiText = '✗ ' + (r.error || 'save failed');
      }
      finishAiMessage();
      setGenerating(false);
    });
  }).catch(function() {
    finishAiMessage();
    setGenerating(false);
  });
}

// --------------------------------------------------------------
// YouTube chapterizer: /chapters
// Injects a probe into the active tab that pulls the visible
// transcript (YouTube renders one in ytd-transcript-segment-renderer
// when the user has opened the transcript panel). We then ask the
// LLM to group those timestamped lines into chapters.
//
// If the transcript panel isn't open, we ask the user to open it
// rather than try to click "..." → "Show transcript" via brittle
// DOM scraping (that path changes every couple months).
// --------------------------------------------------------------
function isYouTubeUrl(u) {
  try {
    var h = new URL(u).hostname;
    return h === 'youtube.com' || /\.youtube\.com$/.test(h);
  } catch (e) {
    return false;
  }
}

function tryDispatchChaptersCommand(text) {
  if (!/^\s*\/chapters\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading transcript from the YouTube tab...');
  // We don't ship a generic JS-eval IPC (deliberately — it would be
  // a perpetual RCE-into-active-tab landmine the moment a future
  // commit accidentally wires the script through unescaped). Instead
  // we read the page's text (readActivePageText: the side panel's capture
  // when it is for the current document, else a fresh read): when the
  // transcript panel is open YouTube renders timestamped lines into the
  // DOM that show up in it. We then filter for "MM:SS" prefixes.
  // Code-review LOW #13 (removed dead `eval_for_chapters` branch that
  // referenced a never-implemented action type).
  readActivePageText(50000)
      .then(function(page) {
        if (!page || !isYouTubeUrl(page.url)) {
          currentAiText = '✗ Open a YouTube video first.';
          finishAiMessage();
          setGenerating(false);
          return;
        }
        var transcriptText = '';
        // Look for the numeric timestamp prefixes YouTube's transcript
        // uses.
        var lines = page.text.split('\n').filter(function(L){
          return /^\s*\d{1,2}:\d{2}/.test(L);
        });
        if (lines.length >= 5) transcriptText = lines.join('\n');
        if (!transcriptText) {
          currentAiText =
            '✗ No transcript found. Click the "..." menu under the ' +
            'video, choose "Show transcript", then re-run /chapters.';
          finishAiMessage();
          setGenerating(false);
          return;
        }
        currentAiText = '';
        var prompt =
          'Below are timestamped transcript lines from a YouTube video. ' +
          'Group them into 5-10 chapters. For each chapter, output one ' +
          'line of the form:\n' +
          '  MM:SS  Chapter title\n' +
          'Use the earliest timestamp in that chapter\'s range. Keep ' +
          'titles short (under 8 words). Output chapter lines only, ' +
          'one per line, no preamble.\n\n' +
          'Transcript:\n' + transcriptText.slice(0, 12000);
        sendWithPromise('sendPrompt', prompt, '', '', '',
                    activeModelId || pickedModelId || '').then(function() {
          finishAiMessage();
          setGenerating(false);
        });
      });
  return true;
}

// --------------------------------------------------------------
// Cluster Q&A: /cluster <n> <question>
// Chat against just the docs in cluster N from the most recent
// /history run. The docs (URL + title) are pre-pended to the prompt
// as grounding so the answer stays inside that topic.
// --------------------------------------------------------------
function tryDispatchClusterCommand(text) {
  var m = text.match(/^\s*\/cluster\b\s*(\d*)\s*(.*)$/i);
  if (!m) return false;
  var idx = parseInt(m[1] || '0', 10);
  var question = (m[2] || '').trim();
  var cls = window.__moltLastHistoryClusters;
  if (!cls || !cls.length) {
    addErrorMessage('Run /history first to build the cluster index.');
    return true;
  }
  if (isNaN(idx) || idx < 1 || idx > cls.length) {
    addErrorMessage('Usage: /cluster <n> <question>  (n between 1 and ' +
                    cls.length + ')');
    return true;
  }
  var cluster = cls[idx - 1];
  if (!question) {
    // No question — just dump the cluster's pages for the user.
    addUserMessage(text);
    startAiMessage();
    var lines = ['Cluster ' + idx + ' — ' + (cluster.label || '(untitled)') +
                 '  (' + cluster.docs.length + ' pages):'];
    cluster.docs.slice(0, 30).forEach(function(d){
      lines.push('  • ' + (d.title || d.host || d.url));
    });
    if (cluster.docs.length > 30) {
      lines.push('  … (' + (cluster.docs.length - 30) + ' more)');
    }
    lines.push('');
    lines.push('To chat against this cluster: /cluster ' + idx +
               ' <your question>');
    currentAiText = lines.join('\n');
    finishAiMessage();
    setGenerating(false);
    return true;
  }
  addUserMessage(text);
  startAiMessage();
  // Build a small grounding block from the cluster's docs. We only
  // include url + title (no snippets) — the LLM uses titles as a
  // pointer to its own memory of having seen the page, and a real
  // answer would need a memory-service query against just these doc
  // ids (not yet exposed via IPC — that's a B.2 follow-up for /cluster).
  var ground = cluster.docs.slice(0, 50).map(function(d){
    return '- ' + (d.title || d.host || d.url) + '  ' + d.url;
  }).join('\n');
  var prompt =
    'Below is a list of web pages the user has visited that belong to ' +
    'one topic cluster (' + (cluster.label || 'untitled') + '). ' +
    'Answer the user\'s question using only what you can confidently ' +
    'infer from these titles. If you can\'t answer with confidence, ' +
    'say so and recommend which pages to revisit.\n\n' +
    'Pages:\n' + ground + '\n\nQuestion: ' + question;
  sendWithPromise('sendPrompt', prompt, '', '', '',
                  activeModelId || pickedModelId || '').then(function() {
    finishAiMessage();
    setGenerating(false);
  }).catch(function() {
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Plan-a-task: /plan <task description>
// Asks the LLM to emit a multi-step plan in our action-token format,
// then parses it into a Script and saves it via the new savePlanScript
// IPC. The script is non-trusted by default — the user has to open
// it in the manager UI and click Run.
// --------------------------------------------------------------
function tryDispatchPlanCommand(text) {
  var m = text.match(/^\s*\/plan\b\s+(.+)$/i);
  if (!m) return false;
  var task = m[1].trim();
  addUserMessage(text);
  startAiMessage();
  var prompt =
    'You are a browser automation planner. Decompose the following ' +
    'task into a sequence of concrete browser steps using ONLY these ' +
    'action verbs:\n' +
    '  navigate <url>\n' +
    '  click <css-selector>\n' +
    '  type <css-selector> <text>\n' +
    '  wait-for <css-selector>\n' +
    '  scroll <pixels>\n' +
    '  extract <css-selector> as <var-name>\n' +
    '  notify <body>\n\n' +
    'Output one step per line, prefixed with "STEP: ". Then output ' +
    'a line "NAME: <short script name>" at the end. No prose, no ' +
    'markdown, no explanation.\n\n' +
    'Task: ' + task;
  sendWithPromise('sendPrompt', prompt, '', '', '',
                  activeModelId || pickedModelId || '').then(function() {
    // Parse currentAiText for STEP: and NAME: lines.
    var lines = currentAiText.split('\n');
    var steps = [];
    var name = task.slice(0, 60);
    lines.forEach(function(L){
      var sm = L.match(/^\s*STEP:\s*(.+)$/i);
      if (sm) steps.push(sm[1].trim());
      var nm = L.match(/^\s*NAME:\s*(.+)$/i);
      if (nm) name = nm[1].trim();
    });
    if (!steps.length) {
      currentAiText += '\n\n✗ Could not parse any STEP: lines from the LLM ' +
                       'plan. (Smaller models sometimes ignore the format; ' +
                       'try a more specific task.)';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    sendWithPromise('savePlanScript', {name: name, steps: steps, task: task})
        .then(function(r) {
          if (r.success) {
            currentAiText += '\n\n✓ Saved plan as script "' +
                             r.script_id + '"  (' + steps.length +
                             ' steps). Open the Automations manager ' +
                             'to review + run.';
          } else {
            currentAiText += '\n\n✗ ' + (r.error || 'save failed');
          }
          finishAiMessage();
          setGenerating(false);
        });
  }).catch(function() {
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Smart paste: /paste [hint]
// Reads the clipboard (navigator.clipboard.readText(), no prompt in
// privileged WebUI) and has the browser propose a form fill from it
// (runFormFillProposal): the on-device model picks values for the
// page's text fields, the browser checks and describes them, and
// nothing is filled before Allow. The pasted text goes to the
// on-device model only, as data, never to a cloud model.
//
// Optional [hint] text goes with the request:
//   /paste prefer phone over fax
// --------------------------------------------------------------
function tryDispatchPasteCommand(text) {
  var m = text.match(/^\s*\/paste\b\s*(.*)$/i);
  if (!m) return false;
  var hint = (m[1] || '').trim();
  addUserMessage(text);
  if (!navigator.clipboard || !navigator.clipboard.readText) {
    appendActionResult(false, 'Nothing filled',
                       'The clipboard isn\u2019t available here.');
    setGenerating(false);
    return true;
  }
  navigator.clipboard.readText().then(function(clip) {
    return runFormFillProposal('paste', hint, String(clip || ''));
  }, function() {
    appendActionResult(false, 'Nothing filled',
                       'Couldn\u2019t read the clipboard.');
  }).then(function() {
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// Daily digest: /digest
// Aggregates the user's last-24h activity from the local memory
// service and asks the LLM to compose a tight briefing. Pure
// client-side composition over existing IPCs (listMemoryDocs,
// listActiveAgents). Privacy: every data point is already on the
// user's disk; the digest never sends anything to the network.
// --------------------------------------------------------------
function tryDispatchDigestCommand(text) {
  if (!/^\s*\/digest\b/i.test(text)) return false;
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Reading your recent activity...');
  var nowUnix = Math.floor(Date.now() / 1000);
  var dayAgo = nowUnix - 86400;
  // Pull more docs than we strictly need so the 24-hour filter has
  // material on a heavy-browsing day.
  Promise.all([
    sendWithPromise('listMemoryDocs', 200),
    sendWithPromise('listActiveAgents').catch(function(){ return null; })
  ]).then(function(arr) {
    var memRes = arr[0] || {};
    var agentRes = arr[1] || {agents: []};
    var docs = (memRes.docs || []).filter(function(d){
      return d.visited_at_unix && d.visited_at_unix >= dayAgo;
    });
    var agents = (agentRes.agents || []);

    if (!docs.length && !agents.length) {
      currentAiText = 'Nothing in the last 24 hours yet — browse a few ' +
                       'pages or wait for watchers to fire.';
      finishAiMessage();
      setGenerating(false);
      return;
    }

    // Build a compact context blob for the LLM. Cap to keep prompt
    // budget reasonable on the bundled model (~3k tokens of input).
    var lines = [];
    lines.push('Pages visited in the last 24h: ' + docs.length);
    docs.slice(0, 80).forEach(function(d) {
      var ts = new Date(d.visited_at_unix * 1000).toISOString().slice(11, 16);
      lines.push('  [' + ts + '] ' + (d.title || d.host || d.url) +
                 '  (' + d.host + ')');
    });
    if (agents.length) {
      lines.push('');
      lines.push('Background automations currently in flight: ' +
                 agents.length);
      agents.forEach(function(a) {
        var step = a.total_steps ? (a.current_step + '/' + a.total_steps)
                                  : String(a.current_step);
        lines.push('  · ' + (a.script_name || a.script_id) +
                   '  step ' + step +
                   (a.status_note ? '  — ' + a.status_note : ''));
      });
    }
    var prompt =
      'Compose a tight daily-digest briefing for the user based on ' +
      'their local browsing + automation activity over the last 24 ' +
      'hours. Group pages into 2-4 themes by topic, summarize each ' +
      'theme in one sentence, then list any in-flight automations. ' +
      'No preamble, no markdown headers, just the briefing in plain ' +
      'paragraphs. Aim for under 200 words.\n\n' +
      'Activity:\n' + lines.join('\n');
    currentAiText = '';
    sendWithPromise('sendPrompt', prompt, '', '', '',
                    activeModelId || pickedModelId || '').then(function() {
      finishAiMessage();
      setGenerating(false);
    }).catch(function() {
      finishAiMessage();
      setGenerating(false);
    });
  });
  return true;
}

// --------------------------------------------------------------
// Password vault: /vault [list|find|fill|delete] (+ add/reveal/generate
// redirect to Settings)
//
// All credentials are OSCrypt-encrypted on disk
// (~/.moltbrowser/vault.enc). This page never receives a password:
// list/find return usernames + sites only, and /vault fill writes the
// secret straight into the active tab's login form from the browser
// process. Adding, revealing and generating passwords happen in
// molt://ai-settings → Passwords, so no secret ever lands in the chat
// transcript (which is autosaved and fed back to the model).
// --------------------------------------------------------------
function tryDispatchVaultCommand(text) {
  var m = text.match(/^\s*\/vault\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  var parts = rest.split(/\s+/);
  var sub = (parts[0] || 'help').toLowerCase();
  // Never echo a password typed after "/vault add" into the transcript.
  addUserMessage(sub === 'add' ? '/vault add …' : text);
  startAiMessage();

  function openVaultSettings(why) {
    currentAiText = why + '\n\nOpening Settings → Passwords…';
    finishAiMessage();
    setGenerating(false);
    openMoltSettings('passwords');
    return true;
  }

  function fmtTime(unix){
    if (!unix) return 'never';
    var d = new Date(unix * 1000);
    return d.toLocaleDateString();
  }

  if (sub === 'help' || sub === '') {
    currentAiText =
      'Vault commands:\n' +
      '  /vault list                       — list saved credentials\n' +
      '  /vault find                       — entries for active tab\'s site\n' +
      '  /vault fill                       — autofill active tab\n' +
      '  /vault fill <id>                  — autofill with specific entry\n' +
      '  /vault delete <id>                — remove an entry\n' +
      '  /vault add | reveal | generate    — open Settings → Passwords\n' +
      '\nAll entries are encrypted at rest via OSCrypt (Keychain on macOS).\n' +
      'Autofill only fills https pages for the saved site; a login saved\n' +
      'for another host on the same site asks you first.';
    finishAiMessage();
    setGenerating(false);
    return true;
  }

  if (sub === 'list') {
    sendWithPromise('vaultList').then(function(r) {
      var entries = (r && r.entries) || [];
      if (!entries.length) {
        currentAiText = 'Vault is empty. Use /vault add <host> <user> <pass>.';
      } else {
        var lines = ['Vault (' + entries.length + ' entries):'];
        entries.forEach(function(e){
          lines.push('  • ' + e.site_host + ' — ' + (e.username || '(no user)') +
                     '   [id ' + e.id.slice(0, 8) + '…]' +
                     '   last used: ' + fmtTime(e.last_used_unix));
        });
        currentAiText = lines.join('\n');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }

  if (sub === 'find') {
    sendWithPromise('vaultFindForActive').then(function(r) {
      var host = r && r.host || '(unknown)';
      var matches = (r && r.matches) || [];
      if (!matches.length) {
        currentAiText = 'No vault entries match ' + host + '.';
      } else {
        var lines = ['Matches for ' + host + ':'];
        matches.forEach(function(m){
          lines.push('  • ' + m.username + '   [id ' + m.id.slice(0, 8) +
                     '…]   last used: ' + fmtTime(m.last_used_unix) +
                     (m.match === 'same_site'
                          ? '   (saved for ' + m.site_host +
                            ' — asks before filling)'
                          : ''));
        });
        lines.push('');
        if (r.fillable) {
          lines.push('Fill with: /vault fill ' + matches[0].id.slice(0, 8));
        } else {
          lines.push('This page is not https, so it can\'t be autofilled.');
        }
        currentAiText = lines.join('\n');
      }
      finishAiMessage();
      setGenerating(false);
    });
    return true;
  }

  if (sub === 'fill') {
    var idArg = (parts[1] || '').trim();
    // Short-id lookup: user can pass any 8+ char prefix.
    var doFill = function(id) {
      sendWithPromise('vaultAutofill', id || '').then(function(r) {
        if (r.success) {
          currentAiText = '✓ Filled ' + r.filled + ' login form' +
              (r.filled === 1 ? '' : 's') + ' (' +
              r.total_password_fields + ' password field' +
              (r.total_password_fields === 1 ? '' : 's') + ' on page).';
        } else if (r.cancelled) {
          currentAiText = 'Autofill cancelled — nothing was filled.';
        } else {
          currentAiText = '✗ ' + (r.error || 'autofill failed');
        }
        finishAiMessage();
        setGenerating(false);
      });
    };
    if (!idArg) { doFill(''); return true; }
    // Resolve short id → full id.
    sendWithPromise('vaultList').then(function(r) {
      var hit = (r.entries || []).find(function(e){
        return e.id.indexOf(idArg) === 0;
      });
      if (!hit) {
        currentAiText = '✗ No entry with id prefix "' + idArg + '"';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      doFill(hit.id);
    });
    return true;
  }

  if (sub === 'add' || sub === 'reveal' || sub === 'show') {
    // Passwords are typed and shown only in Settings, never in chat.
    var input = document.getElementById('chatInput');
    if (input && /^\s*\/vault\s+add\b/i.test(input.value)) input.value = '';
    return openVaultSettings(sub === 'add'
        ? 'Passwords are added in Settings so they never appear in the ' +
          'chat transcript. Nothing was saved from this message.'
        : 'Saved passwords are revealed in Settings, after your ' +
          'system password / Touch ID (or a confirmation) — never in chat.');
  }

  if (sub === 'delete' || sub === 'remove' || sub === 'rm') {
    var idArg2 = (parts[1] || '').trim();
    if (!idArg2) {
      currentAiText = 'Usage: /vault delete <id-prefix>';
      finishAiMessage();
      setGenerating(false);
      return true;
    }
    sendWithPromise('vaultList').then(function(r) {
      var hit = (r.entries || []).find(function(e){
        return e.id.indexOf(idArg2) === 0;
      });
      if (!hit) {
        currentAiText = '✗ No entry matches.';
        finishAiMessage();
        setGenerating(false);
        return;
      }
      sendWithPromise('vaultDelete', hit.id).then(function(r2) {
        currentAiText = r2.success
            ? '✓ Deleted ' + hit.site_host + ' / ' + hit.username
            : '✗ ' + (r2.error || 'delete failed');
        finishAiMessage();
        setGenerating(false);
      });
    });
    return true;
  }

  if (sub === 'generate' || sub === 'gen') {
    return openVaultSettings('Strong passwords are generated in the ' +
        'Settings → Passwords "Add" form, so the new password goes ' +
        'straight into the vault instead of the chat transcript.');
  }

  currentAiText = 'Unknown /vault subcommand: ' + sub + '. Try /vault help.';
  finishAiMessage();
  setGenerating(false);
  return true;
}

// --------------------------------------------------------------
// Inline translate: /translate [lang] <text or selection>
// If no text supplied, uses the current text selection (via the
// runMoltAction eval shim). Default target language is English.
// --------------------------------------------------------------
function tryDispatchTranslateCommand(text) {
  var m = text.match(/^\s*\/translate\b\s*(.*)$/i);
  if (!m) return false;
  var rest = (m[1] || '').trim();
  // First token might be a target language code (en, es, fr, de, ...).
  var lang = 'English';
  var body = rest;
  var firstSpace = rest.indexOf(' ');
  var firstTok = firstSpace > 0 ? rest.slice(0, firstSpace) : rest;
  var langMap = {
    en: 'English', es: 'Spanish', fr: 'French', de: 'German',
    it: 'Italian', pt: 'Portuguese', ja: 'Japanese', zh: 'Chinese',
    ko: 'Korean', ar: 'Arabic', hi: 'Hindi', ru: 'Russian',
    tr: 'Turkish', nl: 'Dutch', sv: 'Swedish', pl: 'Polish'
  };
  if (firstTok && langMap[firstTok.toLowerCase()]) {
    lang = langMap[firstTok.toLowerCase()];
    body = rest.slice(firstSpace + 1).trim();
  }
  addUserMessage(text);
  startAiMessage();
  function doTranslate(src) {
    if (!src) {
      currentAiText = 'Nothing to translate. Use /translate [lang] <text>, ' +
                       'or select text on the page first.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var prompt =
      'Translate the following text to ' + lang + '. Preserve ' +
      'meaning and tone. Output the translation only, no preamble.\n\n' +
      'Text:\n' + src.slice(0, 8000);
    sendWithPromise('sendPrompt', prompt, '', '', '',
                    activeModelId || pickedModelId || '').then(function() {
      finishAiMessage();
      setGenerating(false);
    });
  }
  if (body) { doTranslate(body); return true; }
  // No text supplied — fall back to the active page's text (that of the
  // document the tab shows now, see readActivePageText). It's not the
  // live selection but gets us 80% there without needing a generic eval
  // IPC. The user can scope down by quoting a phrase.
  readActivePageText(4000).then(function(page) {
    if (!page || !page.text) {
      currentAiText = 'Usage: /translate [lang] <text>  (or open a page ' +
                       'with content first).';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    doTranslate(page.text);
  });
  return true;
}

// --------------------------------------------------------------
// Form filler with the on-device model: /fill ai [hint]
// The browser reads your saved profile itself and proposes one form
// fill (runFormFillProposal): at most 8 text fields, never a password
// field, every value shown before you tap Allow, in either mode.
// --------------------------------------------------------------
function tryDispatchFillAICommand(text) {
  var m = text.match(/^\s*\/fill\s+ai\b\s*(.*)$/i);
  if (!m) return false;
  addUserMessage(text);
  runFormFillProposal('profile', (m[1] || '').trim(), '').then(function() {
    setGenerating(false);
  });
  return true;
}

function tryDispatchHistoryCommand(text) {
  var m = text.match(/^\s*\/history\b\s*(\d*)$/i);
  if (!m) return false;
  var limit = parseInt(m[1] || '200', 10);
  addUserMessage(text);
  startAiMessage();
  appendToAiMessage('Loading your reading history...');
  sendWithPromise('listMemoryDocs', limit).then(function(r) {
    var docs = (r && r.docs) || [];
    if (!docs.length) {
      currentAiText = 'No memory documents yet — visit a few pages and ' +
                      'they\u2019ll show up here.';
      finishAiMessage();
      setGenerating(false);
      return;
    }
    var clusters = clusterDocsByTitleKeywords(docs);
    // Stash on window so /cluster <n> can refer back to a specific
    // group without another IPC + re-clustering round-trip.
    window.__moltLastHistoryClusters = clusters;
    renderHistoryClusters(clusters, docs.length);
    setGenerating(false);
  }).catch(function(e) {
    currentAiText = '\u2717 ' + (e || 'history load failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// Simple greedy keyword clusterer:
//   1. Build a per-doc token bag from the title (stop-words stripped).
//   2. For each doc, look at every existing cluster; if Jaccard
//      similarity with the cluster's keyword set >= threshold, join;
//      otherwise start a new cluster.
//   3. Cluster name = the top-2 keywords by frequency in the cluster.
//
// Fast (~O(n*k) where k is current cluster count, plus tiny constants),
// deterministic, and "good enough" for the kind of grouping a user
// can read off a glance. Doesn't need an LLM call.
function clusterDocsByTitleKeywords(docs) {
  var STOP = MOLT_STOP_WORDS;
  function tokens(s) {
    var toks = (s || '').toLowerCase().match(/[a-z0-9]{3,}/g) || [];
    var seen = {};
    var out = [];
    toks.forEach(function(t){
      if (STOP[t]) return;
      if (seen[t]) return;
      seen[t] = true;
      out.push(t);
    });
    return out;
  }
  function jaccard(a, b) {
    if (!a.length || !b.length) return 0;
    var setB = {};
    b.forEach(function(t){ setB[t] = true; });
    var inter = 0;
    a.forEach(function(t){ if (setB[t]) inter++; });
    var uni = a.length + b.length - inter;
    return uni ? inter / uni : 0;
  }
  var THRESH = 0.20;  // tuned by hand on a sample of 200 docs
  var clusters = [];
  for (var i = 0; i < docs.length; i++) {
    var d = docs[i];
    var ts = tokens(d.title || d.host || '');
    if (!ts.length) continue;
    var bestIdx = -1, bestScore = 0;
    for (var c = 0; c < clusters.length; c++) {
      var s = jaccard(ts, clusters[c].keyTokens);
      if (s > bestScore) { bestScore = s; bestIdx = c; }
    }
    if (bestIdx >= 0 && bestScore >= THRESH) {
      var cl = clusters[bestIdx];
      cl.docs.push(d);
      // Merge tokens with frequency tracking.
      ts.forEach(function(t){ cl.tokenFreq[t] = (cl.tokenFreq[t] || 0) + 1; });
      // Recompute the canonical key token set (top-by-freq).
      cl.keyTokens = Object.keys(cl.tokenFreq).sort(function(a,b){
        return cl.tokenFreq[b] - cl.tokenFreq[a];
      }).slice(0, 8);
    } else {
      var freq = {};
      ts.forEach(function(t){ freq[t] = 1; });
      clusters.push({
        docs: [d],
        tokenFreq: freq,
        keyTokens: ts.slice(0, 8)
      });
    }
  }
  // Pick a display label per cluster: top-2 most-frequent tokens.
  clusters.forEach(function(cl){
    var top = Object.keys(cl.tokenFreq).sort(function(a,b){
      return cl.tokenFreq[b] - cl.tokenFreq[a];
    }).slice(0, 2);
    cl.label = top.map(function(t){
      return t.charAt(0).toUpperCase() + t.slice(1);
    }).join(' \u00b7 ') || 'Other';
  });
  // Largest clusters first.
  clusters.sort(function(a, b){ return b.docs.length - a.docs.length; });
  return clusters;
}

function renderHistoryClusters(clusters, totalDocs) {
  function esc(s){return (s+'').replace(/&/g,'&amp;').replace(/</g,'&lt;')
                                .replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
  function timeAgo(unix) {
    if (!unix) return '';
    var d = Date.now()/1000 - unix;
    if (d < 60)      return Math.round(d) + 's ago';
    if (d < 3600)   return Math.round(d/60) + 'm ago';
    if (d < 86400)  return Math.round(d/3600) + 'h ago';
    return Math.round(d/86400) + 'd ago';
  }
  var html = '<div class="history-summary">' + totalDocs +
             ' pages in your local memory, grouped into ' +
             clusters.length + ' topic' +
             (clusters.length === 1 ? '' : 's') + '.</div>';
  clusters.forEach(function(cl, idx){
    var rows = cl.docs.map(function(d){
      var href = safeHttpHref(d.url);
      return '<li>' + (href ? '<a href="' + escAttr(href) + '" target="_blank">'
                            : '<a>') +
             esc(d.title || d.host || d.url) + '</a>' +
             '<span class="hist-meta">' + esc(d.host) + ' \u00b7 ' +
             timeAgo(d.visited_at_unix) + '</span></li>';
    }).join('');
    html += '<details class="history-cluster"' +
            (idx < 3 ? ' open' : '') + '>' +
            '<summary><span class="hist-label">' + esc(cl.label) + '</span>' +
            '<span class="hist-count">' + cl.docs.length + '</span></summary>' +
            '<ul>' + rows + '</ul></details>';
  });
  // Render in the most recent AI message slot.
  currentAiText = '__HISTORY__';
  finishAiMessage();
  var msgs = document.querySelectorAll('#messages .message.ai');
  var last = msgs[msgs.length - 1];
  if (last) last.querySelector('.text').innerHTML = moltHTML(html);
}

function tryDispatchActionCommand(text) {
  var m = text.match(/^\s*\/(click|type|select|hover|right-click|rclick|drag|scroll|navigate|nav|goto|wait|wait-for|waitfor)\b\s*(.*)$/i);
  if (!m) return false;
  var cmd = m[1].toLowerCase();
  var rest = (m[2] || '').trim();
  var action = null;
  // Normalize aliases.
  if (cmd === 'rclick') cmd = 'right-click';
  if (cmd === 'waitfor') cmd = 'wait-for';
  if (cmd === 'click' || cmd === 'hover' || cmd === 'right-click') {
    if (!rest) {
      addErrorMessage('Usage: /' + cmd + ' <css-selector>');
      return true;
    }
    action = {type: cmd, selector: rest};
  } else if (cmd === 'type' || cmd === 'select' || cmd === 'drag') {
    var sp = rest.indexOf(' ');
    if (sp < 0) {
      addErrorMessage('Usage: /' + cmd + ' <selector> <' +
                      (cmd === 'drag' ? 'target-selector' : 'value...') + '>');
      return true;
    }
    action = {type: cmd, selector: rest.slice(0, sp),
              value: rest.slice(sp + 1)};
  } else if (cmd === 'scroll') {
    action = {type: 'scroll', value: rest || '600'};
  } else if (cmd === 'navigate' || cmd === 'nav' || cmd === 'goto') {
    if (!rest) {
      addErrorMessage('Usage: /navigate <url>');
      return true;
    }
    if (!/^https?:\/\//i.test(rest)) rest = 'https://' + rest;
    action = {type: 'navigate', value: rest};
  } else if (cmd === 'wait') {
    action = {type: 'wait', value: rest || '1000'};
  } else if (cmd === 'wait-for') {
    var sp2 = rest.indexOf(' ');
    if (!rest) {
      addErrorMessage('Usage: /wait-for <selector> [timeout-ms]');
      return true;
    }
    if (sp2 < 0) {
      action = {type: 'wait-for', selector: rest, value: '5000'};
    } else {
      action = {type: 'wait-for', selector: rest.slice(0, sp2),
                value: rest.slice(sp2 + 1)};
    }
  }
  if (!action) return false;

  addUserMessage(text);
  // Show a transient "running" status as an AI message we'll update.
  startAiMessage();
  appendToAiMessage('Running ' + cmd + '...');

  // The user typed this command themselves: that is the confirmation the
  // browser otherwise asks for (cross-origin / credential-field actions).
  action.confirmed = true;
  sendWithPromise('runMoltAction', action).then(function(r) {
    var msg = r.success
        ? '\u2713 ' + (r.message || (cmd + ' ok'))
        : '\u2717 ' + (r.message || r.error || cmd + ' failed');
    currentAiText = msg;
    finishAiMessage();
    setGenerating(false);
  }).catch(function(err) {
    currentAiText = '\u2717 ' + (err || 'action failed');
    finishAiMessage();
    setGenerating(false);
  });
  return true;
}

// --------------------------------------------------------------
// P3: page text chunking + relevance ranking.
//
// Splits captured innerText into ~chunkSize-char chunks at sentence
// boundaries so chunks read naturally (LLMs reason better over
// complete sentences than mid-word slices). The ranker scores each
// chunk by unique-token overlap with the query — fast, deterministic,
// no embedding needed. Stop-words are ignored to make the scoring
// behave on common queries like "what does the page say about X".
// --------------------------------------------------------------
var MOLT_STOP_WORDS = {
  'a':1,'an':1,'the':1,'and':1,'or':1,'but':1,'if':1,'of':1,'to':1,
  'in':1,'on':1,'at':1,'is':1,'are':1,'was':1,'were':1,'be':1,'been':1,
  'do':1,'does':1,'did':1,'has':1,'have':1,'had':1,'this':1,'that':1,
  'it':1,'its':1,'for':1,'with':1,'as':1,'by':1,'from':1,'into':1,
  'what':1,'how':1,'why':1,'when':1,'where':1,'who':1,'which':1,
  'about':1,'me':1,'my':1,'i':1,'you':1,'your':1,'we':1,'our':1
};

function chunkPageText(text, chunkSize) {
  if (!text) return [];
  chunkSize = chunkSize || 600;
  // Sentence-ish split: period/!/? followed by whitespace, also
  // line breaks. Lookbehind is supported in every Chromium-era JS.
  var sentences = text.split(/(?<=[.!?])\s+|\n+/);
  var chunks = [];
  var current = '';
  for (var i = 0; i < sentences.length; i++) {
    var s = sentences[i].trim();
    if (!s) continue;
    if ((current.length + s.length + 1) > chunkSize && current) {
      chunks.push(current);
      current = s;
    } else {
      current += (current ? ' ' : '') + s;
    }
  }
  if (current) chunks.push(current);
  return chunks;
}

function rankChunksByQuery(chunks, query, topK) {
  topK = topK || 5;
  if (!chunks.length) return [];
  var qTokens = (query || '').toLowerCase().match(/[a-z0-9]{2,}/g) || [];
  var qSet = {};
  qTokens.forEach(function(t){ if (!MOLT_STOP_WORDS[t]) qSet[t] = true; });
  // If the query gave us no useful keywords (e.g. "summarize this"),
  // fall back to the head of the document — first paragraph is
  // usually the most informative.
  if (!Object.keys(qSet).length) return chunks.slice(0, topK);
  var scored = chunks.map(function(c, idx){
    var tokens = c.toLowerCase().match(/[a-z0-9]{2,}/g) || [];
    var hits = 0;
    var seen = {};
    for (var i = 0; i < tokens.length; i++) {
      if (qSet[tokens[i]] && !seen[tokens[i]]) {
        hits++;
        seen[tokens[i]] = true;
      }
    }
    return {chunk: c, score: hits, idx: idx};
  });
  scored.sort(function(a,b){ return b.score - a.score || a.idx - b.idx; });
  var hits = scored.filter(function(s){ return s.score > 0; })
                   .slice(0, topK);
  // If nothing matched at all, fall back to head — still useful.
  if (!hits.length) return chunks.slice(0, topK);
  return hits.map(function(s){ return s.chunk; });
}

// Direct-navigate intent: if the user types something that's obviously
// a navigation request — "open nasa.com", "go to github.com/x", or just
// a bare URL like "nasa.com" — skip the LLM entirely and navigate the
// active tab. The user typed the address, so it needs neither a model
// nor a confirmation (small models also tend to garble URLs).
function tryDispatchNavigateIntent(text) {
  var url = null;
  // Imperative: "open X" / "go to X" / "visit X" / "navigate to X" /
  // "browse to X" / "launch X"
  var m = text.match(
      /^(?:open|go\s+to|visit|navigate\s+to|browse\s+to|launch)\s+(.+)$/i);
  if (m) {
    url = m[1].trim();
  } else {
    // Bare URL or domain: "nasa.com", "https://nasa.com/foo",
    // "www.example.com/path"
    m = text.match(
        /^(https?:\/\/\S+|(?:www\.)?[a-z0-9-]+(?:\.[a-z0-9-]+)+(?:\/\S*)?)$/i);
    if (m) url = m[1].trim();
  }
  if (!url) return false;
  // Strip trailing punctuation
  url = url.replace(/[.,!?\)\]]+$/, '');
  // Reject if it contains spaces or placeholder markers — not a URL.
  if (/\s/.test(url) || /[<>]/.test(url)) return false;
  // Reject if user typed a /-slash-command — those are handled elsewhere.
  if (url.charAt(0) === '/') return false;
  // Auto-prepend https:// when no protocol was given.
  if (!/^https?:\/\//i.test(url)) url = 'https://' + url;
  // Validate the URL parses and has a hostname with a TLD.
  try {
    var u = new URL(url);
    if (!u.hostname || u.hostname.indexOf('.') < 0) return false;
  } catch (e) {
    return false;
  }
  // Render the same UX the LLM path would produce: user message,
  // navigating chip with spinner, then a result chip on completion.
  addUserMessage(text);
  appendNavigatingChip(url);
  // Typed by the user ("open nasa.com"), so it is already confirmed.
  sendWithPromise('runMoltAction',
                  {type: 'navigate', value: url, confirmed: true}).then(
      function(r) {
        appendActionResult(r && r.success, 'navigate = ' + url,
                           r ? (r.message || r.error || '') : '');
      }).catch(function(err) {
        appendActionResult(false, 'navigate = ' + url,
                           String(err || 'failed'));
      });
  return true;
}

function sendMessage() {
  // These two early-outs consume nothing — drop any chip-set origin
  // flag so a blocked page-action send can't mislabel the next typed
  // message. (The model-load auto-resend path below deliberately does
  // NOT reset it: its re-entry must still see the chip's flag.)
  if (isGenerating) { sendOrigin = 'chat'; return; }
  var input = document.getElementById('chatInput');
  var text = input.value.trim();
  if (!text) { sendOrigin = 'chat'; return; }

  // Direct-navigate intent (no LLM): "open X.com" / "go to X" / bare URL.
  if (tryDispatchNavigateIntent(text)) {
    sendOrigin = 'chat';
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    // This path skips setGenerating(), so re-sync the autogrow height
    // and the empty-input send-disabled state manually.
    input.dispatchEvent(new Event('input'));
    return;
  }

  // /help renders locally and synchronously — no generating state.
  // Commands that finish right away (e.g. /vault add) call
  // setGenerating(false) inside their dispatcher; only mark the chat busy
  // afterwards if the command is still running.
  var slashEpoch = genFinishEpoch;
  if (text.charAt(0) === '/' && tryDispatchHelpCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    input.dispatchEvent(new Event('input'));
    return;
  }

  // Slash-command shortcut: any `/click /type /scroll /navigate` line
  // bypasses the LLM and runs as a one-shot automation action on the
  // active tab. Lets the user drive page actions in plain language
  // without waiting on inference. Only the user's own commands use
  // runMoltAction; the model's actions are browser-validated proposals
  // (runPageActionProposal).
  if (text.charAt(0) === '/' && tryDispatchActionCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchPdfCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchBookmarksCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchAskTabsCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchSandboxCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTrackersCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchReputationCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchHopsCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTorCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchJsCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTriageCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchWatchCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchFillCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchPasteCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchDigestCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchVaultCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchTranslateCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchFillAICommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchHistoryCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchReaderCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchReceiptCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchChaptersCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchClusterCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }
  if (text.charAt(0) === '/' && tryDispatchPlanCommand(text)) {
    conversationHistory.push({role: 'user', content: text});
    trimHistory();
    input.value = '';
    if (genFinishEpoch === slashEpoch) setGenerating(true);
    return;
  }

  // Send guard: if no model is loaded yet but the user has picked one
  // in the model menu (or exactly one is downloaded), load it first —
  // pill spinner via loadModel — then re-run this send automatically.
  // A selected CLOUD model is ready-by-selection: no GGUF to download or
  // load, so skip the whole local load-guard and send straight through.
  var guardCloudId = activeModelId || pickedModelId;
  var guardIsCloud = guardCloudId && allModels.some(function(mm) {
    return mm.is_cloud && mm.model_id === guardCloudId;
  });
  // Only error when the load fails or nothing is downloaded at all.
  if (!activeModelId && !guardIsCloud && allModels.length) {
    var pickId = pickedModelId;
    // A pick that isn't on disk (e.g. its download is still running or
    // failed) can't be loaded — fall back to the downloaded-set logic.
    if (pickId) {
      var pm = allModels.find(function(mm) { return mm.model_id === pickId; });
      if (!pm || !pm.is_downloaded) pickId = null;
    }
    if (!pickId) {
      var dl = allModels.filter(function(mm) { return mm.is_downloaded; });
      if (dl.length === 1) {
        pickId = dl[0].model_id;
      } else if (dl.length === 0) {
        sendOrigin = 'chat';  // message not sent — drop any chip flag
        addErrorMessage(downloadingModelId
            ? 'Model is still downloading — it will load when finished.'
            : 'No model downloaded yet — pick one from the model menu below.');
        return;
      }
      // Multiple downloaded and no explicit pick: fall through and let
      // the backend lazy-load its configured default.
    }
    if (pendingAutoSend) return;  // a guard-triggered load is in flight
    if (pickId) {
      pendingAutoSend = true;
      loadModel(pickId, function(ok) {
        pendingAutoSend = false;
        if (ok) {
          sendMessage();
        } else {
          sendOrigin = 'chat';  // message not sent — drop any chip flag
          addErrorMessage(
              'Model failed to load — choose another from the model menu.');
        }
      });
      return;
    }
  }

  // Commit point: capture the per-message origin ('chat'|'page-action')
  // and reset the module flag so the NEXT typed message defaults back
  // to 'chat'. Done here — after the model-load guard — so the
  // auto-resend re-entry above still sees a chip-set flag.
  var origin = sendOrigin;
  sendOrigin = 'chat';

  // The active model id (local or 'provider:model' cloud id) — the C++
  // HandleSendPrompt reads it as args[5] to route cloud vs. local. May
  // be '' when unknown; the backend falls back to its loaded default.
  var sendModelId = activeModelId || pickedModelId || '';

  addUserMessage(text,
      (attachmentContext && attachmentContext.text)
          ? attachmentContext.name : null);
  conversationHistory.push({role: 'user', content: text});
  trimHistory();
  input.value = '';
  setGenerating(true);
  startAiMessage();

  // History = every message BEFORE the just-pushed user turn, as a
  // JSON array of {role:'user'|'assistant', content} objects. The C++
  // side (HandleSendPrompt) owns all chat templating now — no model
  // markers are ever assembled in JS. Empty string means no history.
  // conversationHistory is already capped at MAX_HISTORY_MESSAGES by
  // trimHistory() right after the push above.
  var historyForPrompt = conversationHistory.length > 1
      ? JSON.stringify(
            conversationHistory.slice(0, conversationHistory.length - 1))
      : '';

  // P3: build a richer page+memory context:
  //   - Active page: chunk the captured innerText (up to 50KB now)
  //     into ~600-char sentence-aware chunks, rank by keyword overlap
  //     with the user's query, take top-5. This beats blind head-
  //     truncation when the answer is deep in a long article.
  //   - Personal Vector Memory: query MemoryService for top-3 hits
  //     from the user's full browsing history so cross-page recall
  //     ("what was that REIT article I read last week") works.
  sendWithPromise('getPageContext').then(function(ctx) {
    var pageCtx = '';
    if (ctx && ctx.has_context) {
      pageCtx = ctx.title + ' (' + ctx.url + ')';
    }
    // Page text pushed by the side panel. Use it only if it was read from
    // the document the tab shows now (same committed navigation entry and
    // URL); otherwise it belongs to the previous page (A7). When the native
    // side can't name the active tab, the side panel's own push is the
    // only (and current) source.
    var sp = window.__moltCurrentTabContext;
    var spIsCurrent = !!sp && !(ctx && ctx.has_context &&
        (sp.url !== ctx.url || sp.nav_entry_id !== ctx.nav_entry_id));
    if (spIsCurrent && sp.text && sp.text.length > 0) {
      var chunks = chunkPageText(sp.text, 600);
      var top = rankChunksByQuery(chunks, text, 5);
      if (top.length) {
        pageCtx = (pageCtx ? (pageCtx + '\n\n') : '') +
                  'Active page content (most relevant excerpts):\n' +
                  top.map(function(c){ return '- ' + c; }).join('\n');
      }
    }
    // PDF chat: if /pdf was just used, prepend the top-K relevant
    // chunks from the loaded PDF to ground the answer in that document.
    if (pdfContext && pdfContext.text) {
      var pdfChunks = chunkPageText(pdfContext.text, 600);
      var pdfTop = rankChunksByQuery(pdfChunks, text, 6);
      if (pdfTop.length) {
        pageCtx = (pageCtx ? (pageCtx + '\n\n') : '') +
                  'Loaded PDF (' + (pdfContext.host || pdfContext.url) +
                  ') — most relevant excerpts:\n' +
                  pdfTop.map(function(c){ return '- ' + c; }).join('\n');
      }
    }
    // Uploaded attachment (paperclip): persists across turns until the
    // chip's ✕ is clicked. Mirrors the pdfContext consumption above —
    // chunk + keyword-rank so a 50K-char document doesn't blow the
    // context window; falls back to the head when ranking finds nothing.
    if (attachmentContext && attachmentContext.text) {
      var atChunks = chunkPageText(attachmentContext.text, 600);
      var atTop = rankChunksByQuery(atChunks, text, 6);
      var atBody = atTop.length ? atTop.join('\n')
                                : attachmentContext.text.slice(0, 4000);
      pageCtx = (pageCtx ? (pageCtx + '\n\n') : '') +
          "The user attached a document '" + attachmentContext.name +
          "'. Its extracted content:\n---\n" + atBody + '\n---\n';
    }
    // Personal Vector Memory grounding. Failures here are non-fatal
    // — we still want to send the prompt even if memory is empty/off.
    return sendWithPromise('queryMemory', text, 3).then(function(mem) {
      if (mem && mem.hits && mem.hits.length) {
        pageCtx += (pageCtx ? '\n\n' : '') + 'Relevant past reading:\n' +
            mem.hits.map(function(h){
              var snip = (h.snippet || '').replace(/\s+/g, ' ').slice(0, 200);
              return '- ' + (h.title || h.url) + ' (' + h.url + '): ' + snip;
            }).join('\n');
      }
      return sendWithPromise('sendPrompt', text, historyForPrompt, pageCtx,
                             origin, sendModelId, conversationId || '');
    }, function() {
      // queryMemory failed (no service yet, etc.) — proceed without it.
      return sendWithPromise('sendPrompt', text, historyForPrompt, pageCtx,
                             origin, sendModelId, conversationId || '');
    });
  }).then(function(result) {
    // Same scrub finishAiMessage applies to the visible bubble — the
    // stored history must not re-feed leaked template markers to the
    // model (or show them on Recents restore). Covers </s> too.
    var aiText = scrubSpecialTokens(currentAiText).trim();
    if (aiText) conversationHistory.push({role: 'assistant', content: aiText});
    finishAiMessage();
    updateContextBar();
    if (result && !result.success && result.error) {
      addErrorMessage(result.error);
    }
    // The message asked to act on the page: the browser now proposes one
    // action. The composer stays busy meanwhile; Stop ends it.
    var actionRequest = (result && result.success)
        ? Number(result.action_request) || 0 : 0;
    if (actionRequest > 0) {
      return runPageActionProposal(actionRequest).then(function() {
        setGenerating(false);
      });
    }
    setGenerating(false);
  }).catch(function(err) {
    finishAiMessage();
    setGenerating(false);
    updateContextBar();
    addErrorMessage('Error: ' + (err || 'Unknown error'));
  });
}

function addErrorMessage(text) {
  // Slim inline notice in the message flow (left red border) — errors
  // never take over the header.
  var m = document.getElementById('messages');
  if (!m) return;
  var d = document.createElement('div');
  d.className = 'message error';
  d.innerHTML = moltHTML('<div class="text">' + esc(text) + '</div>');
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
}

// --------------------------------------------------------------
// Attachments (paperclip). Pick a file → base64 (FileReader, data:
// prefix stripped) → sendWithPromise('extractAttachment', name,
// mime, b64) → C++ extracts text (pdf scraper / docx unzip / OCR /
// plain text). The extracted text lives in attachmentContext and is
// prepended to every following prompt until the chip's ✕ is clicked.
// --------------------------------------------------------------
var attachmentContext = null;  // {name, text, truncated}
var attachmentPending = false;
// Bumped by every ✕ / new pick so a stale extraction that resolves
// after the user removed the chip can't resurrect the attachment.
var attachmentSeq = 0;
var MAX_ATTACHMENT_BYTES = 20 * 1024 * 1024;  // friendly 20 MB cap

function _fmtBytes(n) {
  return n >= 1048576 ? (n / 1048576).toFixed(1) + ' MB'
                      : Math.max(1, Math.round(n / 1024)) + ' KB';
}

function _renderAttachChip(name, size, state, note) {
  var wrap = document.getElementById('attachChipWrap');
  if (!wrap) return;
  var status;
  if (state === 'loading') {
    status = '<span class="a-spin"></span>';
  } else if (state === 'ok') {
    status = '<span class="a-status ok">✓' +
             (note ? ' ' + esc(note) : '') + '</span>';
  } else {
    status = '<span class="a-status err">✗ ' +
             esc(note || 'failed') + '</span>';
  }
  // esc() (textContent→innerHTML) does not encode quotes, so anything
  // interpolated into an ATTRIBUTE also needs the `"` escape below —
  // a downloaded file named `x" onmouseover="...` must not be able to
  // inject attributes into this privileged WebUI page.
  var attrName = esc(name).replace(/"/g, '&quot;');
  wrap.innerHTML = moltHTML('<div class="attach-chip">' +
      '<span>📎</span>' +
      '<span class="a-name" title="' + attrName + '">' + esc(name) +
      '</span>' +
      '<span class="a-size">' + _fmtBytes(size) + '</span>' +
      status +
      '<button class="a-close" title="Remove attachment" ' +
          'data-on-click="clearAttachment">✕</button>' +
      '</div>');
  wrap.style.display = 'flex';
}

function clearAttachment() {
  attachmentSeq++;          // invalidate any in-flight extraction
  attachmentPending = false;
  attachmentContext = null;
  var wrap = document.getElementById('attachChipWrap');
  if (wrap) { wrap.style.display = 'none'; wrap.textContent = ''; }
}

function pickAttachment() {
  if (attachmentPending) return;
  var picker = document.createElement('input');
  picker.type = 'file';
  picker.accept = '.pdf,.docx,.txt,.md,.png,.jpg,.jpeg,.webp';
  picker.onchange = function(e) {
    var f = e.target.files && e.target.files[0];
    if (!f) return;
    attachmentContext = null;
    if (f.size > MAX_ATTACHMENT_BYTES) {
      _renderAttachChip(f.name, f.size, 'error',
                        'Too large — the limit is 20 MB.');
      return;
    }
    _renderAttachChip(f.name, f.size, 'loading');
    attachmentPending = true;
    var seq = ++attachmentSeq;
    var reader = new FileReader();
    reader.onload = function(ev) {
      if (seq !== attachmentSeq) return;  // ✕ clicked while reading
      // readAsDataURL yields "data:<mime>;base64,<payload>" — the
      // extractAttachment contract wants the raw base64 payload only.
      var b64 = String(ev.target.result || '');
      var comma = b64.indexOf(',');
      if (comma >= 0) b64 = b64.slice(comma + 1);
      sendWithPromise('extractAttachment', f.name, f.type || '', b64)
          .then(function(r) {
            if (seq !== attachmentSeq) return;  // ✕ clicked meanwhile
            attachmentPending = false;
            if (r && r.success) {
              attachmentContext =
                  {name: f.name, size: f.size, text: r.text || '',
                   truncated: !!r.truncated};
              _renderAttachChip(f.name, f.size, 'ok',
                                r.truncated ? '(truncated)' : '');
            } else {
              _renderAttachChip(f.name, f.size, 'error',
                                (r && r.error) || 'Extraction failed');
            }
          })
          .catch(function(err) {
            if (seq !== attachmentSeq) return;  // ✕ clicked meanwhile
            attachmentPending = false;
            _renderAttachChip(f.name, f.size, 'error',
                              String(err || 'Extraction failed'));
          });
    };
    reader.onerror = function() {
      if (seq !== attachmentSeq) return;
      attachmentPending = false;
      _renderAttachChip(f.name, f.size, 'error', 'Could not read file.');
    };
    reader.readAsDataURL(f);
  };
  picker.click();
}

// ---- Quick Actions with Page Content ----

// All chip sends go through the normal sendMessage pipeline but carry
// origin='page-action' so the backend can gate/dedupe its own page-
// context injection (the chips already embed their excerpt in the
// prompt itself).
function _sendQuickPrompt(prompt) {
  document.getElementById('chatInput').value = prompt;
  sendOrigin = 'page-action';
  sendMessage();
}

function quickAction(action) {
  if (isGenerating) return;
  if (action === 'summarize') {
    // Extract actual page content for better summarization
    sendWithPromise('getPageContent').then(function(result) {
      var prompt;
      if (result.has_content) {
        prompt = 'Summarize this page:\n\nTitle: ' +
                 (result.title || 'Unknown') + '\nURL: ' + (result.url || '') +
                 '\n\nContent:\n' + result.content.substring(0, 3000);
      } else {
        prompt = 'Summarize this page';
      }
      _sendQuickPrompt(prompt);
    }).catch(function() {
      _sendQuickPrompt('Summarize this page');
    });
  } else if (action === 'extract') {
    sendWithPromise('getPageContent').then(function(result) {
      var prompt;
      if (result.has_content) {
        prompt = 'Extract the key data, facts, and figures from this content:\n\n' +
                 result.content.substring(0, 3000);
      } else {
        prompt = 'Extract key data from this page';
      }
      _sendQuickPrompt(prompt);
    }).catch(function() {
      _sendQuickPrompt('Extract key data from this page');
    });
  } else if (action === 'explain') {
    sendWithPromise('getPageContent').then(function(result) {
      var prompt;
      if (result.has_content) {
        prompt = 'Explain this simply in plain language:\n\n' +
                 result.content.substring(0, 2000);
      } else {
        prompt = 'Explain this page simply';
      }
      _sendQuickPrompt(prompt);
    }).catch(function() {
      _sendQuickPrompt('Explain this page simply');
    });
  } else {
    _sendQuickPrompt('Translate this page to English');
  }
}

// ---- New Chat ----

// ---- Agent-action permission mode (Ask first / Auto) ----
// The picker is the mode chip at the left of the composer row (a
// Claude-Code-style chip that opens an upward menu). actionAutoMode
// decides whether a page action the browser proposes gets a confirm
// chip before it runs (handleChatProposal); the browser's own reasons to
// ask (another site, a form, payment fields) apply in both modes.

function setActionMode(mode) {
  actionAutoMode = (mode === 'auto');
  var chkAsk  = document.getElementById('ovCheckAsk');
  var chkAuto = document.getElementById('ovCheckAuto');
  if (chkAsk)  chkAsk.classList.toggle('visible', !actionAutoMode);
  if (chkAuto) chkAuto.classList.toggle('visible', actionAutoMode);
  // Keep the composer chip label in sync with the active mode.
  var icon = document.getElementById('modeChipIcon');
  var name = document.getElementById('modeChipName');
  if (icon) icon.innerHTML = moltHTML(actionAutoMode ? '&#9889;' : '&#128737;');
  if (name) name.textContent = actionAutoMode ? 'Auto' : 'Ask first';
  closeModeDropdown();
}

function toggleModeDropdown(ev) {
  if (ev) ev.stopPropagation();
  var chip = document.getElementById('modeChip');
  var dd = document.getElementById('modeChipDropdown');
  if (!chip || !dd) return;
  // stopPropagation keeps the outside-click closers from firing, so
  // explicitly close the other composer popup — never both at once.
  closeModelDropdown();
  dd.classList.toggle('open');
  chip.classList.toggle('open');
}

// Shared "close the model popup" used by both toggles above/below.
function closeModelDropdown() {
  var chip = document.getElementById('modelChip');
  var dd = document.getElementById('modelChipDropdown');
  if (dd) dd.classList.remove('open');
  if (chip) chip.classList.remove('open');
}

function closeModeDropdown() {
  var chip = document.getElementById('modeChip');
  var dd = document.getElementById('modeChipDropdown');
  if (dd) dd.classList.remove('open');
  if (chip) chip.classList.remove('open');
}

// Close the mode menu on outside click (mirrors the model dropdown).
document.addEventListener('click', function(e) {
  var wrap = document.querySelector('.mode-chip-wrap');
  if (wrap && !wrap.contains(e.target)) closeModeDropdown();
});

// Close the AI chat side panel from inside the WebUI. The panel has no
// native window controls (it renders its own header), so ask the C++
// handler to close the SidePanelUI. Fire-and-forget: closing tears down
// this page, so no promise is awaited.
function closeSidePanel() {
  try { chrome.send('closeSidePanel', []); } catch (e) {}
}

// ---- Settings (⋯ menu) ----
// The side-panel WebContents has no tab-strip delegate, so
// window.open('molt://ai-settings/') is silently dropped there. The
// C++ handler 'openMoltSettings' opens chrome://molt-ai-settings in a
// real browser tab instead. Contract: resolves {success: bool}.
function openMoltSettings(section) {
  // Optional section deep-links the settings page (e.g. 'providers' for
  // Connect AI Providers, 'import' for Import & Migration).
  sendWithPromise('openMoltSettings', section || '').then(function(r) {
    if (!r || !r.success) addErrorMessage('Could not open Settings.');
  }).catch(function() {
    addErrorMessage('Could not open Settings.');
  });
}

// Show a banner when a model is available but not yet selected/loaded.
function showModelSelectBanner() {
  var m = document.getElementById('messages');
  if (!m) return;
  // Avoid duplicates
  if (m.querySelector('.model-select-banner')) return;
  var d = document.createElement('div');
  d.className = 'model-select-banner';
  d.innerHTML =
    moltHTML('<span class="msb-icon">⬇</span>' +
    '<span class="msb-text">Choose a model below to start chatting</span>');
  m.insertBefore(d, m.firstChild);
}

function clearModelSelectBanner() {
  var b = document.querySelector('.model-select-banner');
  if (b && b.parentNode) b.parentNode.removeChild(b);
}

function newChat() {
  // Finish (and stop) any in-flight stream first so stray tokens
  // can't leak into the fresh transcript.
  if (isGenerating) cancelGeneration();
  if (currentAiMessageEl) finishAiMessage();
  currentAiMessageEl = null;
  currentAiText = '';
  pdfContext = null;
  clearAttachment();  // attachments are per-conversation
  conversationHistory = [];
  conversationId = null;   // next autosave creates a new Recents entry
  conversationTitle = '';
  setActionMode('ask'); // reset to safe default for new session
  var m = document.getElementById('messages');
  if (m) m.textContent = '';
  ensureWelcomeMessage();
  updateContextBar();
}

// ---- Cancel ----

function cancelGeneration() {
  chrome.send('cancelGeneration', []);
  // Stop while a page action is being chosen: the browser ends it; stop
  // waiting for its answer here.
  if (abortPageActionProposal) abortPageActionProposal();
}

// ---- Model Management ----

function toggleModelPanel() {
  var p = document.getElementById('modelPanel');
  if (!p) return;
  if (p.classList.contains('open')) {
    p.classList.remove('open');
  } else {
    p.classList.add('open');
    refreshModelList();
  }
}

// ---- Brand tiles: a small logo icon + maker name shown above each model
// name in the picker, so "who made it" reads at a glance (e.g. Meta ->
// "Llama 3.1 8B Instruct"). Glyphs are self-contained inline SVG (no network,
// CSP-safe) and hard-code their own fill/stroke so they render on any tile.
// Note: the Gemma model is labeled "Gemma" (its brand) NOT "Google", per the
// zero-"Google"-string product rule. Unknown makers fall back to a monogram.
var MOLT_BRANDS = {
  'Meta': {c:'#0866FF', t:'rgba(8,102,255,.15)', g:'<path d="M12 12.1C10.3 9.2 8.7 7.7 6.9 7.7 4.6 7.7 3 9.8 3 12.3s1.6 4.6 3.9 4.6c1.8 0 3.4-1.5 5.1-4.8M12 12.1c1.7-2.9 3.3-4.4 5.1-4.4 2.3 0 3.9 2.1 3.9 4.6s-1.6 4.6-3.9 4.6c-1.8 0-3.4-1.5-5.1-4.8" fill="none" stroke="#0866FF" stroke-width="2.1" stroke-linecap="round"/>'},
  'Qwen': {c:'#7C3AED', t:'rgba(124,58,237,.16)', g:'<path d="M12 2.7l8 4.6v9.4l-8 4.6-8-4.6V7.3z" fill="none" stroke="#7C3AED" stroke-width="1.8"/><path d="M8.3 9.2l3.7 2.1 3.7-2.1M12 11.3v4.5" fill="none" stroke="#7C3AED" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/>'},
  'Gemma': {c:'#4285F4', t:'rgba(66,133,244,.16)', g:'<path d="M12 3c.6 4.6 3.4 7.4 8 8-4.6.6-7.4 3.4-8 8-.6-4.6-3.4-7.4-8-8 4.6-.6 7.4-3.4 8-8z" fill="#4285F4"/>'},
  'Mistral AI': {c:'#FA500F', t:'rgba(250,80,15,.14)', g:'<g><rect x="3.5" y="5" width="17" height="3.1" rx="1" fill="#FFCC33"/><rect x="3.5" y="10.4" width="17" height="3.1" rx="1" fill="#FF7A1A"/><rect x="3.5" y="15.8" width="17" height="3.1" rx="1" fill="#F7101B"/></g>'},
  'Microsoft': {c:'#00A4EF', t:'rgba(0,164,239,.10)', g:'<g><rect x="3.6" y="3.6" width="7.4" height="7.4" fill="#F25022"/><rect x="13" y="3.6" width="7.4" height="7.4" fill="#7FBA00"/><rect x="3.6" y="13" width="7.4" height="7.4" fill="#00A4EF"/><rect x="13" y="13" width="7.4" height="7.4" fill="#FFB900"/></g>'},
  'OpenAI': {c:'#0FA47F', t:'rgba(15,164,127,.15)', g:'<g fill="none" stroke="#0FA47F" stroke-width="1.5"><ellipse cx="12" cy="12" rx="3.4" ry="8"/><ellipse cx="12" cy="12" rx="3.4" ry="8" transform="rotate(60 12 12)"/><ellipse cx="12" cy="12" rx="3.4" ry="8" transform="rotate(120 12 12)"/></g>'},
  'TinyLlama': {c:'#14B8A6', t:'rgba(20,184,166,.16)', g:'<path d="M8.6 4.4c-.6 0-1 .5-1 1.4l.3 3c-1.4.6-2.3 2-2.3 3.9v3.4c0 2.3 1.6 3.9 3.9 3.9h5c2.3 0 3.9-1.6 3.9-3.9v-3.4c0-1.9-.9-3.3-2.3-3.9l.3-3c0-.9-.4-1.4-1-1.4-.7 0-1.4.9-1.9 2.2-.9-.3-2-.3-3 0-.5-1.3-1.2-2.2-1.9-2.2z" fill="#14B8A6"/><circle cx="10" cy="13.2" r=".95" fill="#fff"/><circle cx="14" cy="13.2" r=".95" fill="#fff"/>'},
  // Cloud providers (bring-your-own-key) — for the picker's "Cloud" group.
  'Anthropic': {c:'#D97757', t:'rgba(217,119,87,.15)', g:'<g stroke="#D97757" stroke-width="2" stroke-linecap="round"><path d="M12 4v16M4 12h16M6.3 6.3l11.4 11.4M17.7 6.3L6.3 17.7"/></g>'},
  'Gemini': {c:'#4285F4', t:'rgba(66,133,244,.16)', g:'<path d="M12 3c.6 4.6 3.4 7.4 8 8-4.6.6-7.4 3.4-8 8-.6-4.6-3.4-7.4-8-8 4.6-.6 7.4-3.4 8-8z" fill="#4285F4"/>'},
  'Mistral': {c:'#FA500F', t:'rgba(250,80,15,.14)', g:'<g><rect x="3.5" y="5" width="17" height="3.1" rx="1" fill="#FFCC33"/><rect x="3.5" y="10.4" width="17" height="3.1" rx="1" fill="#FF7A1A"/><rect x="3.5" y="15.8" width="17" height="3.1" rx="1" fill="#F7101B"/></g>'}
};
function moltBrandColor(c) { var b = MOLT_BRANDS[c]; return b ? b.c : '#94a3b8'; }
function moltBrandTile(company) {
  var b = MOLT_BRANDS[company];
  if (!b) {
    var ch = (company || '?').charAt(0).toUpperCase();
    return '<span class="brandtile" style="background:rgba(148,163,184,.16);color:#94a3b8;' +
           'font-weight:700;font-size:13px">' + esc(ch) + '</span>';
  }
  return '<span class="brandtile" style="background:' + b.t + '">' +
         '<svg viewBox="0 0 24 24" width="17" height="17" aria-hidden="true">' + b.g + '</svg></span>';
}

function refreshModelList() {
  sendWithPromise('getModelStatus').then(function(info) {
    var list = document.getElementById('modelList');
    if (!list) return;
    list.textContent = '';
    // Same ordering as the picker: downloaded (loaded first) on top,
    // then not-downloaded; size ascending within each group.
    var models = sortModelsForPicker(info.models || []);

    models.forEach(function(m) {
      var card = document.createElement('div');
      card.className = 'model-card';
      card.id = 'model-' + m.model_id;

      var sizeMB = m.file_size_mb;
      var sizeStr = sizeMB > 1024 ? (sizeMB / 1024).toFixed(1) + ' GB' : sizeMB + ' MB';

      var statusBadge = '';
      var actionBtns = '';

      if (m.is_loaded) {
        statusBadge = '<span class="badge active">Active</span>';
        actionBtns = '';
      } else if (m.is_downloaded) {
        statusBadge = '<span class="badge downloaded">Ready</span>';
        actionBtns = '<button class="btn primary" data-on-click="loadModel" data-model-id="' + escAttr(m.model_id) + '">Load</button>' +
                     '<button class="btn danger" data-on-click="deleteModel" data-model-id="' + escAttr(m.model_id) + '">Delete</button>';
      } else {
        statusBadge = '<span class="badge unavailable">Not Downloaded</span>';
        actionBtns = '<button class="btn primary" data-on-click="downloadModel" data-model-id="' + escAttr(m.model_id) + '">Download (' + sizeStr + ')</button>';
      }

      card.innerHTML =
        moltHTML('<div class="card-head">' + moltBrandTile(m.company) +
          '<div class="chead-txt">' +
            '<div class="mco" style="color:' + moltBrandColor(m.company) + '">' +
              esc(m.company || 'Local') + '</div>' +
            '<div class="name">' + esc(m.display_name) + '</div>' +
          '</div>' +
        '</div>' +
        '<div class="meta">' + (m.quantization ? esc(m.quantization) + ' \u00b7 ' : '') + sizeStr + '</div>' +
        modelLicenseLine(m) +
        '<div style="margin-top:4px">' + statusBadge + '</div>' +
        (actionBtns ? '<div class="card-actions">' + actionBtns + '</div>' : '') +
        '<div class="progress-wrap" id="pw-' + escAttr(m.model_id) + '">' +
          '<div class="progress-bar"><div class="progress-fill" id="pf-' + escAttr(m.model_id) + '"></div></div>' +
          '<div class="progress-text" id="pt-' + escAttr(m.model_id) + '"></div>' +
        '</div>');

      list.appendChild(card);
    });
  });
}

function downloadModel(modelId) {
  // Show progress immediately
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.add('active');

  // Disable download buttons and show cancel
  var card = document.getElementById('model-' + modelId);
  if (card) {
    var btns = card.querySelectorAll('.card-actions .btn');
    for (var i = 0; i < btns.length; i++) btns[i].style.display = 'none';
    var cancelDiv = document.createElement('div');
    cancelDiv.className = 'card-actions';
    cancelDiv.id = 'cancel-wrap-' + modelId;
    cancelDiv.innerHTML = moltHTML('<button class="btn danger" data-on-click="cancelDownload">Cancel Download</button>');
    card.querySelector('.card-actions').parentNode.appendChild(cancelDiv);
  }

  sendWithPromise('downloadModel', modelId).then(function(r) {
    if (r.success) {
      refreshModelList();
    } else if (r.license_required) {
      // Llama / Gemma terms must be accepted before the download (S13).
      refreshModelList();
      askModelLicense(r).then(function(ok) {
        if (!ok) return;
        sendWithPromise('acceptModelLicense', modelId, r.license_id)
            .then(function(a) {
              if (a && a.success) downloadModel(modelId);
              else addErrorMessage('Could not record the license acceptance.');
            });
      });
    } else {
      addErrorMessage('Download failed: ' + (r.error || 'Unknown error'));
      refreshModelList();
    }
  }).catch(function(e) {
    addErrorMessage('Download error: ' + e);
    refreshModelList();
  });
}

function cancelDownload() {
  chrome.send('cancelDownload', []);
}

// A13: every local model shows the licence its weights come under, with a
// link to the full terms (opened in a real tab by the browser, which only
// accepts http(s)). Both values come from the C++ model registry; they are
// still escaped here like any other data.
function modelLicenseLine(m) {
  if (!m || !m.license_name) return '';
  var href = safeHttpHref(m.license_url);
  if (!href) {
    return '<div class="mlicense">Licence: ' + esc(m.license_name) + '</div>';
  }
  return '<div class="mlicense">Licence: <button type="button" ' +
         'class="mlicense-link" data-on-click="openModelLicense" ' +
         'data-license-url="' + escAttr(href) + '">' +
         esc(m.license_name) + '</button></div>';
}

function openModelLicense(url) {
  var href = safeHttpHref(url);
  if (href) sendWithPromise('openUrlInTab', href);
}

// Same, as a DOM node for the model menu rows: clicking it opens the terms
// without also selecting/downloading the row's model.
function modelLicenseNode(m) {
  if (!m || !m.license_name) return null;
  var href = safeHttpHref(m.license_url);
  var el = document.createElement(href ? 'button' : 'span');
  el.className = 'mlic';
  el.textContent = m.license_name;
  if (href) {
    el.type = 'button';
    el.title = 'Open the licence terms';
    el.addEventListener('click', function(ev) {
      ev.stopPropagation();
      openModelLicense(href);
    });
  }
  return el;
}

// License gate for models whose terms must be accepted (Llama, Gemma).
// Resolves true when the user accepts. The terms open in a real tab.
function askModelLicense(r) {
  return new Promise(function(resolve) {
    var msgs = document.getElementById('messages');
    if (!msgs) { resolve(false); return; }
    var d = document.createElement('div');
    d.className = 'molt-action-confirm';
    d.innerHTML = moltHTML(
        '<span class="ac-icon">\u{1F4DC}</span>' +
        '<div class="ac-body">' +
          '<div class="ac-title">License required</div>' +
          '<div class="ac-label" style="white-space:normal">' +
            'This model is distributed under the ' + esc(r.license_name) +
            '. Read the terms before downloading.</div>' +
        '</div>' +
        '<div class="ac-buttons">' +
          '<button class="ac-terms">Read terms</button>' +
          '<button class="ac-deny">Cancel</button>' +
          '<button class="ac-allow">Accept</button>' +
        '</div>');
    msgs.appendChild(d);
    msgs.scrollTop = msgs.scrollHeight;
    var settle = function(ok) {
      return function() {
        try { d.parentNode && d.parentNode.removeChild(d); } catch (e) {}
        resolve(ok);
      };
    };
    d.querySelector('.ac-terms').onclick = function() {
      sendWithPromise('openUrlInTab', r.license_url);
    };
    d.querySelector('.ac-allow').onclick = settle(true);
    d.querySelector('.ac-deny').onclick = settle(false);
  });
}

// onDone (optional) fires with true/false AFTER the model state has
// been re-fetched, so callers (the sendMessage guard) can rely on
// activeModelId being current. The user's pick is remembered in
// pickedModelId even when the load fails.
// Remember the user's last-selected model id so the next launch can
// auto-restore it instead of resetting the chip to "Choose model".
// Mirrors the moltTheme localStorage pattern (see applyTheme).
function rememberLastModel(id) {
  if (!id) return;
  try { localStorage.setItem('moltLastModel', id); } catch (e) {}
}

function loadModel(modelId, onDone) {
  pickedModelId = modelId;
  setStatus('loading', 'Loading ' + modelId + '...');
  // Remove the pulsing needs-selection state immediately — user made a choice
  var chip = document.getElementById('modelChip');
  if (chip) {
    chip.classList.remove('needs-selection');
    chip.classList.add('loading');
  }
  var nameEl = document.getElementById('modelChipName');
  if (nameEl) nameEl.textContent = 'Loading…';
  sendWithPromise('loadModel', modelId).then(function(r) {
    if (chip) chip.classList.remove('loading');
    if (r.success) {
      rememberLastModel(modelId);
      setStatus('ready', 'Model Ready');
      clearModelSelectBanner();
      refreshModelList();
      sendWithPromise('getModelStatus').then(function(rr) {
        allModels = rr.models || [];
        refreshModelChip();
        if (onDone) onDone(true);
      });
    } else {
      setStatus('error', 'Load failed');
      if (chip) chip.classList.add('needs-selection');
      refreshModelChip();
      if (onDone) onDone(false);
    }
  }).catch(function() {
    if (chip) {
      chip.classList.remove('loading');
      chip.classList.add('needs-selection');
    }
    setStatus('error', 'Load failed');
    refreshModelChip();
    if (onDone) onDone(false);
  });
}

// Select a cloud model (provider:model id). Unlike a local GGUF, there
// is nothing to download or load into memory — the backend short-circuits
// 'loadModel' for cloud ids to instant-ready. We set the active/picked
// ids optimistically so the chip + send-guard treat it as READY right
// away, with no download/load spinner.
function selectCloudModel(modelId) {
  activeModelId = modelId;
  pickedModelId = modelId;
  rememberLastModel(modelId);
  // Clear any leftover local-model needs-selection/loading affordances.
  var chip = document.getElementById('modelChip');
  if (chip) {
    chip.classList.remove('needs-selection');
    chip.classList.remove('loading');
    chip.classList.remove('downloading');
  }
  setStatus('ready', 'Model Ready');
  clearModelSelectBanner();
  // Tell the backend which model is active (instant-ready for cloud ids).
  // We don't gate the UI on the response — the chip is already usable.
  sendWithPromise('loadModel', modelId).then(function() {}, function() {});
  refreshModelChip();
  toggleModelDropdown();  // close the picker
}

function deleteModel(modelId) {
  sendWithPromise('deleteModel', modelId).then(function(r) {
    if (r.success) {
      refreshModelList();
    }
  });
}

// ---- Model Chip (compact side panel selector) ----
var allModels = [];
var activeModelId = null;
var downloadingModelId = null;
// The model the user last picked in the menu — survives a failed
// load so the sendMessage guard can retry it, unlike activeModelId
// which only ever mirrors the actually-loaded model.
var pickedModelId = null;
// True while the sendMessage guard is waiting on a loadModel round
// trip; blocks re-entrant guard loads from repeated Send presses.
var pendingAutoSend = false;

// Returns the currently-selected cloud model dict, or null. A cloud
// model is "active" purely by selection (is_loaded is always false for
// cloud) — activeModelId points at it after selectCloudModel().
function selectedCloudModel() {
  if (!activeModelId) return null;
  return allModels.find(function(m){
    return m.is_cloud && m.model_id === activeModelId;
  }) || null;
}

function refreshModelChip() {
  var nameEl = document.getElementById('modelChipName');
  if (!nameEl) return;
  // A loaded LOCAL model always wins and re-asserts activeModelId. If
  // none is loaded but the user picked a CLOUD model, keep that active
  // (cloud is ready-by-selection, never is_loaded). Otherwise clear.
  var active = allModels.find(function(m){ return m.is_loaded; });
  if (active) {
    activeModelId = active.model_id;
    nameEl.textContent = active.display_name || active.model_id;
  } else {
    var cloud = selectedCloudModel();
    if (cloud) {
      nameEl.textContent = cloud.display_name || cloud.model_id;
    } else {
      activeModelId = null;
      nameEl.textContent = 'Choose model';
    }
  }
  updateWelcomeMessage();
}

// Picker ordering: downloaded first (the loaded model at the very
// top), then not-downloaded; size ascending within each group.
function sortModelsForPicker(models) {
  return models.slice().sort(function(a, b) {
    var ra = a.is_loaded ? 0 : (a.is_downloaded ? 1 : 2);
    var rb = b.is_loaded ? 0 : (b.is_downloaded ? 1 : 2);
    if (ra !== rb) return ra - rb;
    return (a.file_size_mb || 0) - (b.file_size_mb || 0);
  });
}

function toggleModelDropdown(ev) {
  if (ev) ev.stopPropagation();
  var chip = document.getElementById('modelChip');
  var dd = document.getElementById('modelChipDropdown');
  if (!chip || !dd) return;
  closeModeDropdown();  // one composer popup at a time
  var willOpen = !dd.classList.contains('open');
  dd.classList.toggle('open');
  chip.classList.toggle('open');
  if (willOpen) renderModelChipDropdown();
}

function renderModelChipDropdown() {
  var dd = document.getElementById('modelChipDropdown');
  if (!dd) return;
  if (allModels.length === 0) {
    dd.innerHTML = moltHTML('<div style="padding:12px;color:var(--faint);font-size:11px;text-align:center">Loading\u2026</div>');
    sendWithPromise('getModelStatus').then(function(r){
      allModels = r.models || [];
      renderModelChipDropdown();
      refreshModelChip();
    });
    return;
  }
  dd.textContent = '';
  // Partition into local (on-device GGUF) and cloud (via user's key)
  // models. Cloud models carry is_cloud=true and provider!='' from the
  // backend; they need no download/load and are always ready-to-select.
  var localModels = [];
  var cloudModels = [];
  for (var li = 0; li < allModels.length; li++) {
    if (allModels[li].is_cloud) cloudModels.push(allModels[li]);
    else localModels.push(allModels[li]);
  }

  // ---- Cloud connect: the FIRST, most-prominent option so bring-your-own-
  // key frontier models are the top action in the picker. Opens the Connect
  // AI Providers settings (a manage entry once any provider is connected). ----
  var cloudCta = document.createElement('div');
  cloudCta.className = 'mcd-cloud-cta';
  cloudCta.innerHTML =
    moltHTML('<span class="mcc-icon">☁️</span>' +
    '<span class="mcc-text">' +
      (cloudModels.length ? 'Manage cloud providers' : 'Connect a cloud model') +
      '<span class="mcc-sub">OpenAI, Claude, Gemini &amp; more — your key</span>' +
    '</span>' +
    '<span class="mcc-arrow">→</span>');
  cloudCta.onclick = function() {
    toggleModelDropdown();
    openMoltSettings('providers');
  };
  dd.appendChild(cloudCta);

  // ---- Local group: existing download/load affordances ----
  if (localModels.length) {
    var lh = document.createElement('div');
    lh.className = 'mcd-header';
    lh.textContent = 'Local \u00b7 Private \ud83d\udd12';
    dd.appendChild(lh);
  }
  sortModelsForPicker(localModels).forEach(function(m) {
    var item = document.createElement('div');
    item.className = 'model-chip-item';
    var isActive = !!m.is_loaded;
    var isDownloading = downloadingModelId === m.model_id;
    var sizeMB = m.file_size_mb || 0;
    var sizeStr = sizeMB > 1024 ? (sizeMB / 1024).toFixed(1) + ' GB' : sizeMB + ' MB';
    var meta = (m.quantization ? m.quantization + ' \u00b7 ' : '') + sizeStr;
    // No pinned SHA-256 for this file yet (S13): say so before download.
    if (m.verified === false && !m.is_downloaded) {
      meta += ' \u00b7 unverified download';
    }
    // Right-hand affordance per state:
    //   loaded          \u2192 accent check (selected)
    //   downloading     \u2192 live % (see the download-progress listener)
    //   on disk         \u2192 subtle check; click loads
    //   not downloaded  \u2192 \u2b07 + size; click starts the download
    var right;
    if (isActive) {
      item.className += ' active';
      right = '<span class="mcheck on">\u2713</span>';
    } else if (isDownloading) {
      item.className += ' downloading';
      right = '<span class="mpct" id="chipRowPct-' + m.model_id + '">\u2026</span>';
    } else if (m.is_downloaded) {
      right = '<span class="mcheck disk" title="Downloaded \u2014 click to load">\u2713</span>';
    } else {
      right = '<span class="mget">\u2b07 ' + sizeStr + '</span>';
      // Single concurrent download: while one is running, other
      // download rows are disabled (handler enforces this too).
      if (downloadingModelId) item.className += ' disabled';
    }
    item.innerHTML =
      moltHTML(moltBrandTile(m.company) +
      '<div class="mmain">' +
        '<div class="mco" style="color:' + moltBrandColor(m.company) + '">' +
          esc(m.company || 'Local') + '</div>' +
        '<div class="mname">' + esc(m.display_name || m.model_id) + '</div>' +
        '<div class="msize">' + esc(meta) + '</div>' +
        (isDownloading
            ? '<div class="mrow-bar"><div class="mrow-fill" id="chipRowFill-' +
              escAttr(m.model_id) + '"></div></div>'
            : '') +
      '</div>' + right);
    var licNode = modelLicenseNode(m);
    var mainCol = item.querySelector('.mmain');
    if (licNode && mainCol) mainCol.appendChild(licNode);
    item.onclick = function(ev) {
      // The re-render below detaches the clicked row; without this the
      // document-level outside-click handler sees a detached target and
      // closes the menu we mean to keep open.
      if (ev) ev.stopPropagation();
      if (isActive || isDownloading) return;
      if (m.is_downloaded) {
        loadModel(m.model_id);
        toggleModelDropdown();
        return;
      }
      if (downloadingModelId) return;  // one download at a time
      pickedModelId = m.model_id;  // remember the pick for the send guard
      downloadingModelId = m.model_id;
      downloadModel(m.model_id);
      var chip = document.getElementById('modelChip');
      if (chip) chip.classList.add('downloading');
      // Keep the menu open so the row shows inline progress.
      renderModelChipDropdown();
    };
    dd.appendChild(item);
  });

  // ---- Cloud group: simplified rows (name + select check only) ----
  // No download/size/load affordances \u2014 a cloud model is instantly
  // usable via the user's API key. The "via your key" header is the
  // privacy signal that distinguishes it from the private local models.
  if (cloudModels.length) {
    var ch = document.createElement('div');
    ch.className = 'mcd-header';
    ch.textContent = 'Cloud \u00b7 via your key \u2601\ufe0f';
    dd.appendChild(ch);
    cloudModels.forEach(function(m) {
      var item = document.createElement('div');
      item.className = 'model-chip-item';
      var isActive = activeModelId === m.model_id;
      var right = isActive
          ? '<span class="mcheck on">\u2713</span>'
          : '<span class="mcheck disk" title="Click to use">\u2713</span>';
      if (isActive) item.className += ' active';
      item.innerHTML =
        moltHTML(moltBrandTile(m.provider) +
        '<div class="mmain">' +
          '<div class="mco" style="color:' + moltBrandColor(m.provider) + '">' +
            esc(m.provider || 'Cloud') + '</div>' +
          '<div class="mname">' + esc(m.display_name || m.model_id) + '</div>' +
          '<div class="msize">cloud \u00b7 via your key</div>' +
        '</div>' + right);
      item.onclick = function(ev) {
        if (ev) ev.stopPropagation();
        if (isActive) return;
        selectCloudModel(m.model_id);
      };
      dd.appendChild(item);
    });
  }

  if (!localModels.length && !cloudModels.length) {
    var none = document.createElement('div');
    none.className = 'mcd-header';
    none.textContent = 'Models';
    dd.appendChild(none);
  }
  // (Cloud connect/manage lives at the TOP of the dropdown now \u2014 see the
  // .mcd-cloud-cta entry above \u2014 so it's the first thing the user sees.)
  var foot = document.createElement('div');
  foot.className = 'mcd-footer';
  foot.textContent = 'Manage local models\u2026';
  foot.onclick = function() {
    toggleModelDropdown();
    toggleModelPanel();
  };
  dd.appendChild(foot);
}

function updateModelChipProgress(percent) {
  var fg = document.getElementById('modelChipProgressFg');
  var pct = document.getElementById('modelChipPct');
  if (fg) {
    var circumference = 2 * Math.PI * 7; // r=7 in side panel
    var offset = circumference - (percent / 100) * circumference;
    fg.style.strokeDasharray = circumference;
    fg.style.strokeDashoffset = offset;
  }
  if (pct) pct.textContent = Math.round(percent) + '%';
}

// Close dropdown on outside click
document.addEventListener('click', function(e) {
  var wrap = document.querySelector('.model-chip-wrap');
  if (wrap && !wrap.contains(e.target)) {
    var dd = document.getElementById('modelChipDropdown');
    var chip = document.getElementById('modelChip');
    if (dd) dd.classList.remove('open');
    if (chip) chip.classList.remove('open');
  }
});

// On startup, restore the user's last-used model so the picker no longer
// resets to "Choose model" every launch. Only acts when nothing is already
// active (a live-loaded local model always wins) and the remembered id is
// still available; a since-deleted / not-installed model silently falls
// back to "Choose model".
function autoSelectLastModel() {
  if (activeModelId) return;  // a loaded local or selected cloud already won
  var savedId = null;
  try { savedId = localStorage.getItem('moltLastModel'); } catch (e) {}
  if (!savedId) return;
  var m = allModels.find(function(x){ return x.model_id === savedId; });
  if (!m) return;  // model no longer installed/available -> Choose model
  if (m.is_cloud) {
    // Cloud is ready-by-selection: mirror selectCloudModel()'s state set
    // WITHOUT the dropdown toggle (the picker is closed on startup) and
    // without re-persisting (already stored).
    activeModelId = savedId;
    pickedModelId = savedId;
    sendWithPromise('loadModel', savedId).then(function(){}, function(){});
    refreshModelChip();
  } else if (m.is_downloaded) {
    // Local model present on disk -> load it into memory as if picked.
    loadModel(savedId);
  }
  // else: known but not downloaded -> leave the "Choose model" default.
}

// Init chip
sendWithPromise('getModelStatus').then(function(r){
  allModels = r.models || [];
  refreshModelChip();
  autoSelectLastModel();
});

// ---- Event Listeners ----

// Clickable links in chat responses. The side-panel WebContents has no
// AddNewContents delegate, so a plain <a target="_blank"> is swallowed.
// Intercept clicks on any http(s) anchor inside #messages (including the
// chat-link anchors built by renderMarkdown and the memory-docs list)
// and route them to a real browser tab via the 'openUrlInTab' IPC.
(function(){
  var messages = document.getElementById('messages');
  var target = messages || document;
  target.addEventListener('click', function(e) {
    var a = e.target.closest && e.target.closest('a.chat-link, a[href^="http"]');
    if (!a) return;
    var href = a.href || a.getAttribute('href');
    if (!isSafeHttpUrl(href)) return;
    e.preventDefault();
    sendWithPromise('openUrlInTab', href).then(function(r){
      if (!r || !r.success) addErrorMessage('Could not open link.');
    }).catch(function(){
      addErrorMessage('Could not open link.');
    });
  });
})();

// Token streaming
cr.addWebUiListener('ai-token', function(token, isDone) {
  if (token && token !== '') {
    appendToken(token);
  }
});

// Model status changes
cr.addWebUiListener('model-status', function(status, detail) {
  if (status === 'loading') {
    setStatus('loading', 'Loading model...');
  } else if (status === 'ready') {
    setStatus('ready', 'Model Ready');
    clearModelSelectBanner();
    // Pulse the model chip briefly to confirm selection
    var chip = document.getElementById('modelChip');
    if (chip) { chip.classList.add('model-ready-flash'); setTimeout(function(){ chip.classList.remove('model-ready-flash'); }, 1200); }
    // Lazy loads (backend auto-load on first send) reach here without
    // a loadModel round trip — re-fetch so the chip name and welcome
    // line reflect the newly loaded model.
    sendWithPromise('getModelStatus').then(function(r) {
      allModels = r.models || [];
      refreshModelChip();
    });
  } else if (status === 'error') {
    setStatus('error', 'Error: ' + (detail || 'Unknown'));
  }
});

// Download progress with speed and ETA
cr.addWebUiListener('download-progress', function(modelId, current, total, speed, eta) {
  var fill = document.getElementById('pf-' + modelId);
  var ptext = document.getElementById('pt-' + modelId);
  var pw = document.getElementById('pw-' + modelId);
  if (fill && total > 0) {
    var pct = Math.round((current / total) * 100);
    fill.style.width = pct + '%';
    if (pw) pw.classList.add('active');
    var info = pct + '% (' + Math.round(current / 1048576) + ' / ' + Math.round(total / 1048576) + ' MB)';
    if (speed > 0) {
      var mbps = (speed / 1048576).toFixed(1);
      info += ' \u00b7 ' + mbps + ' MB/s';
      if (eta > 0) {
        var mins = Math.floor(eta / 60);
        var secs = Math.round(eta % 60);
        info += ' \u00b7 ' + (mins > 0 ? mins + 'm ' : '') + secs + 's left';
      }
    }
    if (ptext) ptext.textContent = info;
  }
  // Update model pill progress ring + the inline row in the picker
  if (downloadingModelId === modelId && total > 0) {
    var pct2 = (current / total) * 100;
    updateModelChipProgress(pct2);
    var nameEl = document.getElementById('modelChipName');
    var info2 = allModels.find(function(m){return m.model_id === modelId;});
    if (nameEl && info2) nameEl.textContent = 'Downloading ' + (info2.display_name || modelId);
    var rowPct = document.getElementById('chipRowPct-' + modelId);
    if (rowPct) rowPct.textContent = Math.round(pct2) + '%';
    var rowFill = document.getElementById('chipRowFill-' + modelId);
    if (rowFill) rowFill.style.width = pct2 + '%';
  }
});

// Download complete
cr.addWebUiListener('download-complete', function(modelId, success) {
  var pw = document.getElementById('pw-' + modelId);
  if (pw) pw.classList.remove('active');
  // Reset chip download state
  if (downloadingModelId === modelId) {
    downloadingModelId = null;
    var chip = document.getElementById('modelChip');
    if (chip) chip.classList.remove('downloading');
    if (success) {
      sendWithPromise('getModelStatus').then(function(r){
        allModels = r.models || [];
        // Order matters: refreshModelChip() would overwrite the pill
        // with "Choose model", so run it before loadModel() sets the
        // "Loading…" state for the auto-load.
        refreshModelChip();
        loadModel(modelId);
        // Refresh the picker rows if it's still open.
        var dd = document.getElementById('modelChipDropdown');
        if (dd && dd.classList.contains('open')) renderModelChipDropdown();
      });
    } else {
      var dd2 = document.getElementById('modelChipDropdown');
      if (dd2 && dd2.classList.contains('open')) renderModelChipDropdown();
      refreshModelChip();
    }
  }
  if (success) {
    refreshModelList();
    // Close welcome overlay if it was open
    var wo = document.getElementById('welcomeOverlay');
    if (wo && wo.classList.contains('open')) {
      wo.classList.remove('open');
      setStatus('offline', 'Model ready \u2014 send a message to start');
    }
  }
});

// Keyboard: Enter sends, Shift+Enter inserts a newline (textarea).
document.getElementById('chatInput').addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    sendMessage();
  }
});

// Autogrow the composer textarea (1 → ~6 lines) and keep the send
// button disabled while the input is empty.
(function() {
  var input = document.getElementById('chatInput');
  if (!input) return;
  var sync = function() {
    input.style.height = 'auto';
    input.style.height = Math.min(input.scrollHeight, 120) + 'px';
    var sendBtn = document.getElementById('sendBtn');
    if (sendBtn && !isGenerating) sendBtn.disabled = !input.value.trim();
  };
  input.addEventListener('input', sync);
  sync();
})();

// ---- First-Run Experience ----

var isFirstRun = false;

function startFirstRunDownload() {
  var btn = document.getElementById('welcomeDownloadBtn');
  btn.disabled = true;
  btn.textContent = 'Downloading...';
  var wp = document.getElementById('welcomeProgress');
  wp.classList.add('active');
  downloadModel('tinyllama-1.1b');
}

function skipFirstRun() {
  document.getElementById('welcomeOverlay').classList.remove('open');
}

// Hook download progress into welcome overlay too
cr.addWebUiListener('download-progress', function(modelId, current, total, speed, eta) {
  var wo = document.getElementById('welcomeOverlay');
  if (wo && wo.classList.contains('open') && total > 0) {
    var pct = Math.round((current / total) * 100);
    document.getElementById('welcomePfill').style.width = pct + '%';
    var info = pct + '% \u2014 ' + Math.round(current / 1048576) + ' / ' +
      Math.round(total / 1048576) + ' MB';
    if (speed > 0) {
      info += ' \u00b7 ' + (speed / 1048576).toFixed(1) + ' MB/s';
      if (eta > 0) {
        var mins = Math.floor(eta / 60);
        var secs = Math.round(eta % 60);
        info += ' \u00b7 ' + (mins > 0 ? mins + 'm ' : '') + secs + 's left';
      }
    }
    document.getElementById('welcomePtext').textContent = info;
  }
});

// ---- Copy Code / Response ----

function copyCode(id, btn) {
  var pre = document.getElementById(id);
  if (!pre) return;
  var text = pre.textContent;
  navigator.clipboard.writeText(text).then(function() {
    btn.textContent = 'Copied!';
    btn.classList.add('copied');
    setTimeout(function() { btn.textContent = 'Copy'; btn.classList.remove('copied'); }, 1500);
  });
}

function copyResponse(btn) {
  var msg = btn.closest('.message');
  if (!msg) return;
  var textEl = msg.querySelector('.text');
  if (!textEl) return;
  navigator.clipboard.writeText(textEl.innerText).then(function() {
    btn.textContent = 'Copied!';
    setTimeout(function() { btn.textContent = 'Copy response'; }, 1500);
  });
}

// ---- Search Conversation ----

function toggleSearch() {
  var bar = document.getElementById('searchBar');
  if (bar.classList.contains('open')) {
    bar.classList.remove('open');
    clearSearch();
  } else {
    bar.classList.add('open');
    document.getElementById('searchInput').focus();
  }
}

function doSearch() {
  clearSearch();
  var query = document.getElementById('searchInput').value.trim().toLowerCase();
  if (!query) { document.getElementById('searchCount').textContent = ''; return; }
  var msgs = document.querySelectorAll('#messages .message .text');
  var count = 0;
  for (var i = 0; i < msgs.length; i++) {
    var el = msgs[i];
    var text = el.textContent.toLowerCase();
    if (text.indexOf(query) >= 0) {
      count++;
      highlightTextNodes(el, query);
      if (count === 1) el.scrollIntoView({behavior:'smooth', block:'center'});
    }
  }
  document.getElementById('searchCount').textContent = count + ' match' + (count !== 1 ? 'es' : '');
}

// Wraps each case-insensitive occurrence of `query` inside the TEXT nodes
// of `root` in a <span class="highlight">. Works on text nodes only, so a
// match can never land inside markup or an attribute (a regex over
// innerHTML could, and could break out of an href).
function highlightTextNodes(root, query) {
  var re = new RegExp(query.replace(/[.*+?^${}()|[\]\\]/g, '\\$&'), 'gi');
  var walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  var nodes = [];
  while (walker.nextNode()) nodes.push(walker.currentNode);
  for (var i = 0; i < nodes.length; i++) {
    var node = nodes[i];
    var text = node.nodeValue;
    re.lastIndex = 0;
    var m = re.exec(text);
    if (!m) continue;
    var frag = document.createDocumentFragment();
    var from = 0;
    while (m) {
      frag.appendChild(document.createTextNode(text.slice(from, m.index)));
      var mark = document.createElement('span');
      mark.className = 'highlight';
      mark.textContent = m[0];
      frag.appendChild(mark);
      from = m.index + m[0].length;
      m = re.exec(text);
    }
    frag.appendChild(document.createTextNode(text.slice(from)));
    node.parentNode.replaceChild(frag, node);
  }
}

function clearSearch() {
  var highlights = document.querySelectorAll('.highlight');
  for (var i = 0; i < highlights.length; i++) {
    var span = highlights[i];
    span.replaceWith(span.textContent);
  }
}

// ---- Conversation rendering (shared by import + Recents switch) ----

function renderConversationDOM(messages) {
  var m = document.getElementById('messages');
  if (!m) return;
  m.textContent = '';
  for (var i = 0; i < messages.length; i++) {
    var msg = messages[i];
    if (msg.role === 'user') {
      addUserMessage(msg.content);
    } else {
      var d = document.createElement('div');
      d.className = 'message ai';
      // History stores the raw assistant text. It is only ever shown:
      // "[[ACTION ...]]" lines in a conversation saved by an older version
      // (or imported) render as text and run nothing.
      var visible = scrubSpecialTokens(String(msg.content || '')).trim();
      d.innerHTML = moltHTML('<div class="sender">AI Assistant</div><div class="text">' + renderMarkdown(visible) + '</div>' +
        '<div class="msg-actions"><button class="msg-action" data-on-click="copyResponse">Copy response</button></div>');
      m.appendChild(d);
    }
  }
  m.scrollTop = m.scrollHeight;
  updateContextBar();
}

// ---- Recents drawer + conversation store ----
// One JSON file per conversation on the C++ side; the JS generates
// ids, derives titles from the first user message and autosaves
// after every user turn (trimHistory) and assistant turn
// (finishAiMessage).

var allConversations = [];

function toggleDrawer() {
  var d = document.getElementById('drawer');
  if (d && d.classList.contains('open')) closeDrawer();
  else openDrawer();
}

function openDrawer() {
  var d = document.getElementById('drawer');
  var s = document.getElementById('drawerScrim');
  if (d) d.classList.add('open');
  if (s) s.classList.add('open');
  refreshConvList();
}

function closeDrawer() {
  var d = document.getElementById('drawer');
  var s = document.getElementById('drawerScrim');
  if (d) d.classList.remove('open');
  if (s) s.classList.remove('open');
}

function refreshConvList() {
  sendWithPromise('listConversations').then(function(r) {
    allConversations = (r && r.conversations) || [];
    renderConvList();
  }).catch(function() {
    allConversations = [];
    renderConvList();
  });
}

function renderConvList() {
  var list = document.getElementById('convList');
  if (!list) return;
  var q = '';
  var si = document.getElementById('drawerSearch');
  if (si) q = si.value.trim().toLowerCase();
  list.textContent = '';
  var shown = 0;
  for (var i = 0; i < allConversations.length; i++) {
    var c = allConversations[i];
    var title = c.title || 'Untitled chat';
    if (q && title.toLowerCase().indexOf(q) < 0) continue;
    shown++;
    var row = document.createElement('div');
    row.className = 'conv-item' + (c.id === conversationId ? ' active' : '');
    row.innerHTML = moltHTML('<span class="conv-title">' + esc(title) + '</span>' +
                    '<button class="conv-del" title="Delete chat">🗑</button>');
    (function(id) {
      row.onclick = function() { loadConversationById(id); };
      row.querySelector('.conv-del').onclick = function(ev) {
        ev.stopPropagation();
        deleteConversationById(id);
      };
    })(c.id);
    list.appendChild(row);
  }
  if (!shown) {
    list.innerHTML = moltHTML('<div class="conv-empty">' +
        (q ? 'No matching chats' : 'No conversations yet') + '</div>');
  }
}

function filterConvList() { renderConvList(); }

function genConversationId() {
  return 'c' + Date.now().toString(36) + '-' +
         Math.random().toString(36).slice(2, 8);
}

function deriveConversationTitle() {
  for (var i = 0; i < conversationHistory.length; i++) {
    if (conversationHistory[i].role === 'user') {
      var t = (conversationHistory[i].content || '')
                  .replace(/\s+/g, ' ').trim();
      if (t.length > 40) t = t.slice(0, 40) + '…';
      if (t) return t;
    }
  }
  return 'New chat';
}

function saveCurrentConversation() {
  if (!conversationHistory.length) return;
  var isNew = !conversationId;
  if (isNew) {
    conversationId = genConversationId();
    conversationTitle = deriveConversationTitle();
  }
  var json;
  // Persist any attached document alongside the messages using a
  // backward-compatible envelope so a reopened chat stays grounded in the
  // file. With no attachment the payload stays a bare array (identical to
  // the legacy on-disk format), so old saves and other readers are
  // unaffected. attachmentContext.text is already capped server-side.
  var payload = (attachmentContext && attachmentContext.text)
      ? {v: 2, messages: conversationHistory, attachment: attachmentContext}
      : conversationHistory;
  try { json = JSON.stringify(payload); } catch (e) { return; }
  sendWithPromise('saveConversation', conversationId, conversationTitle, json)
      .then(function() {
        // A brand-new conversation should appear in an open drawer.
        var d = document.getElementById('drawer');
        if (isNew && d && d.classList.contains('open')) refreshConvList();
      })
      .catch(function() { /* Persistence is best-effort. */ });
}

function loadConversationById(id) {
  if (isGenerating) return;
  sendWithPromise('loadConversation', id).then(function(r) {
    if (!r || !r.success) {
      addErrorMessage('Could not load conversation.');
      return;
    }
    var msgs = [];
    var restoredAttachment = null;
    try {
      // Accept either the legacy bare-array format or the v2 envelope
      // {v, messages, attachment} written by saveCurrentConversation.
      var parsed = JSON.parse(r.history_json || '[]');
      if (Array.isArray(parsed)) {
        msgs = parsed;
      } else if (parsed && Array.isArray(parsed.messages)) {
        msgs = parsed.messages;
        restoredAttachment = parsed.attachment || null;
      }
    } catch (e) {}
    if (!Array.isArray(msgs)) msgs = [];
    conversationId = r.id || id;
    conversationTitle = r.title || '';
    conversationHistory = msgs;
    pdfContext = null;
    // Restore the saved attachment (and its chip) so follow-up questions in
    // a reopened chat stay grounded in the file, instead of silently
    // dropping it. A brand-new chat still starts clean (newChat clears it).
    if (restoredAttachment && restoredAttachment.text) {
      clearAttachment();
      attachmentSeq++;
      attachmentContext = restoredAttachment;
      _renderAttachChip(restoredAttachment.name,
                        restoredAttachment.size ||
                            (restoredAttachment.text || '').length,
                        'ok',
                        restoredAttachment.truncated ? '(truncated)' : '');
    } else {
      clearAttachment();
    }
    currentAiMessageEl = null;
    currentAiText = '';
    renderConversationDOM(msgs);
    closeDrawer();
  }).catch(function() {
    addErrorMessage('Could not load conversation.');
  });
}

function deleteConversationById(id) {
  sendWithPromise('deleteConversation', id).then(function() {
    // Deleting the active conversation detaches it: the transcript
    // stays on screen but the next autosave creates a fresh entry.
    if (id === conversationId) conversationId = null;
    refreshConvList();
  }).catch(function() { refreshConvList(); });
}

// ---- Header overflow menu (⋯) ----

function toggleOverflowMenu(e) {
  if (e) e.stopPropagation();
  var menu = document.getElementById('overflowMenu');
  if (!menu) return;
  var willOpen = !menu.classList.contains('open');
  menu.classList.toggle('open', willOpen);
  if (willOpen) {
    // Exclude the whole .overflow-wrap (menu + its ⋯ button): this
    // runs in the capture phase, so if it closed on the button the
    // button's own onclick would immediately re-open the menu.
    var wrap = menu.parentNode;
    var close = function(ev) {
      if (!wrap.contains(ev.target)) {
        menu.classList.remove('open');
        document.removeEventListener('click', close, true);
      }
    };
    document.addEventListener('click', close, true);
  }
}

function closeOverflowMenu() {
  var menu = document.getElementById('overflowMenu');
  if (menu) menu.classList.remove('open');
}

// ---- Import Chat History ----

function importChat() {
  var input = document.createElement('input');
  input.type = 'file';
  input.accept = '.json';
  input.onchange = function(e) {
    var file = e.target.files[0];
    if (!file) return;
    var reader = new FileReader();
    reader.onload = function(ev) {
      try {
        var data = JSON.parse(ev.target.result);
        if (data.messages && Array.isArray(data.messages)) {
          conversationHistory = data.messages;
          // An import becomes its own conversation on the next save.
          conversationId = null;
          conversationTitle = '';
          renderConversationDOM(data.messages);
          addSystemMessage('Imported ' + data.messages.length + ' messages from ' + file.name);
        } else {
          addErrorMessage('Invalid chat export file');
        }
      } catch (err) {
        addErrorMessage('Failed to parse file: ' + err.message);
      }
    };
    reader.readAsText(file);
  };
  input.click();
}

// ---- Export Chat History ----

function exportChat() {
  if (conversationHistory.length === 0) {
    addErrorMessage('No conversation to export');
    return;
  }
  var data = JSON.stringify({
    exported_at: new Date().toISOString(),
    messages: conversationHistory
  }, null, 2);
  sendWithPromise('exportHistory', data).then(function(r) {
    if (r.success) {
      addSystemMessage('Chat exported to: ' + r.filename);
    } else {
      addErrorMessage('Export failed');
    }
  });
}

function addSystemMessage(text) {
  // One-line, small, centered notice in the flow.
  var m = document.getElementById('messages');
  if (!m) return;
  var d = document.createElement('div');
  d.className = 'message system';
  d.innerHTML = moltHTML('<div class="text">' + esc(text) + '</div>');
  m.appendChild(d);
  m.scrollTop = m.scrollHeight;
}

// ---- Keyboard Shortcuts ----
// Cmd/Ctrl+Shift+S = Summarize page
// Cmd/Ctrl+Shift+E = Explain page
// Cmd/Ctrl+Shift+X = Export chat
// Cmd/Ctrl+Shift+N = New chat

document.addEventListener('keydown', function(e) {
  var mod = e.metaKey || e.ctrlKey;
  if (!mod || !e.shiftKey) return;
  if (e.key === 'S' || e.key === 's') {
    e.preventDefault();
    quickAction('summarize');
  } else if (e.key === 'E' || e.key === 'e') {
    e.preventDefault();
    quickAction('explain');
  } else if (e.key === 'X' || e.key === 'x') {
    e.preventDefault();
    exportChat();
  } else if (e.key === 'N' || e.key === 'n') {
    e.preventDefault();
    newChat();
  }
});

// Cmd/Ctrl+F for search
document.addEventListener('keydown', function(e) {
  if ((e.metaKey || e.ctrlKey) && (e.key === 'f' || e.key === 'F') && !e.shiftKey) {
    e.preventDefault();
    toggleSearch();
  }
  if (e.key === 'Escape') {
    var dr = document.getElementById('drawer');
    if (dr && dr.classList.contains('open')) { closeDrawer(); return; }
    var om = document.getElementById('overflowMenu');
    if (om && om.classList.contains('open')) { closeOverflowMenu(); return; }
    var bar = document.getElementById('searchBar');
    if (bar && bar.classList.contains('open')) toggleSearch();
  }
});

// --------------------------------------------------------------
// Voice mode (Tier 5).
// Click the mic, the button goes red, speak. Click again (or wait
// 30s) and we stop, encode the captured audio as a 16-kHz mono WAV,
// base64 it, and ship to the transcribeAudio IPC which hands off
// to bundled whisper.cpp. The result lands in the chat input box
// — the user can review and edit before sending.
//
// We do the resampling ourselves rather than asking the browser
// for 16k input, because most macOS mics report 44.1k or 48k and
// the WebAudio constraint isn't reliably honored across hardware.
//
// NOTE (2026-07 side-panel fix): getUserMedia is KEPT. The side
// panel's "Mic permission denied: NotSupportedError" was a hosting
// bug — AiChatSidePanelWebView is a bare views::WebView whose
// default WebContentsDelegate denies media requests. The native fix
// forwards Request/CheckMediaAccessPermission to
// MediaCaptureDevicesDispatcher (same path as tab-hosted chrome://
// pages, which auto-ALLOW mic); no JS change is needed here.
// --------------------------------------------------------------
var voiceState = {
  recording: false,
  ctx: null,
  source: null,
  processor: null,
  stream: null,
  chunks: [],       // Float32Array slices captured in real time
  inputRate: 0,
  startedAt: 0,
  maxMs: 30000,     // hard cap so a forgotten mic doesn't run forever
  // ---- Live dictation state ----
  session: 0,        // bumped per recording; stale IPC results are dropped
  liveTimer: null,   // ~2.75s interval driving interim transcribes
  inFlight: false,   // serialize transcribeAudio calls — never overlap
  finalQueued: false,// stop() arrived while an interim was in flight
  failed: false,     // an interim transcribe errored; stop polling quietly
  prefix: '',        // composer text present when dictation started
  transcript: '',    // newest whisper transcript for this session
  lastApplied: null  // exact composer value we last wrote (edit detection)
};
var VOICE_LIVE_TICK_MS = 2750;  // interim transcribe cadence (~2.5-3s)
var VOICE_MIN_SECONDS = 0.7;    // below this total -> 'Recording too short.'

function _wavBytesFromPCM(pcm16, sampleRate) {
  // pcm16 is Int16Array of mono samples.
  var byteLen = 44 + pcm16.length * 2;
  var ab = new ArrayBuffer(byteLen);
  var dv = new DataView(ab);
  function w(off, str){
    for (var i = 0; i < str.length; i++) dv.setUint8(off + i, str.charCodeAt(i));
  }
  w(0, 'RIFF');
  dv.setUint32(4, byteLen - 8, true);
  w(8, 'WAVE');
  w(12, 'fmt ');
  dv.setUint32(16, 16, true);          // fmt chunk size
  dv.setUint16(20, 1, true);           // PCM format
  dv.setUint16(22, 1, true);           // mono
  dv.setUint32(24, sampleRate, true);
  dv.setUint32(28, sampleRate * 2, true);
  dv.setUint16(32, 2, true);           // block align
  dv.setUint16(34, 16, true);          // bits per sample
  w(36, 'data');
  dv.setUint32(40, pcm16.length * 2, true);
  for (var i = 0; i < pcm16.length; i++) {
    dv.setInt16(44 + i * 2, pcm16[i], true);
  }
  return new Uint8Array(ab);
}

function _resampleTo16k(floatBuf, fromRate) {
  if (fromRate === 16000) return floatBuf;
  var ratio = fromRate / 16000;
  var outLen = Math.floor(floatBuf.length / ratio);
  var out = new Float32Array(outLen);
  for (var i = 0; i < outLen; i++) {
    // Cheap linear interp; whisper is forgiving.
    var srcIdx = i * ratio;
    var s0 = Math.floor(srcIdx);
    var s1 = Math.min(s0 + 1, floatBuf.length - 1);
    var t = srcIdx - s0;
    out[i] = floatBuf[s0] * (1 - t) + floatBuf[s1] * t;
  }
  return out;
}

function _floatToInt16(buf) {
  var out = new Int16Array(buf.length);
  for (var i = 0; i < buf.length; i++) {
    var s = Math.max(-1, Math.min(1, buf[i]));
    out[i] = s < 0 ? s * 0x8000 : s * 0x7FFF;
  }
  return out;
}

function _bytesToBase64(bytes) {
  // Chunk to avoid blowing the call-stack on a long recording.
  var chunkSize = 0x8000;
  var s = '';
  for (var i = 0; i < bytes.length; i += chunkSize) {
    s += String.fromCharCode.apply(null,
        bytes.subarray(i, Math.min(i + chunkSize, bytes.length)));
  }
  return btoa(s);
}

// Concatenate the captured chunks WITHOUT consuming them — live
// dictation re-transcribes the growing cumulative buffer each tick.
function _mergedVoiceSamples() {
  var total = 0;
  for (var i = 0; i < voiceState.chunks.length; i++)
    total += voiceState.chunks[i].length;
  var merged = new Float32Array(total);
  var off = 0;
  for (var j = 0; j < voiceState.chunks.length; j++) {
    merged.set(voiceState.chunks[j], off);
    off += voiceState.chunks[j].length;
  }
  return merged;
}

function _voiceWavB64(merged) {
  var resampled = _resampleTo16k(merged, voiceState.inputRate || 16000);
  var pcm16 = _floatToInt16(resampled);
  var wav = _wavBytesFromPCM(pcm16, 16000);
  return _bytesToBase64(wav);
}

// Subtle status line above the composer: pulsing dot + 'Listening…'
// while recording, amber 'Transcribing…' during the final pass.
// state: 'listening' | 'transcribing' | anything else = hidden.
function _setDictationHint(state) {
  var h = document.getElementById('dictationHint');
  if (!h) return;
  var t = document.getElementById('dictationHintText');
  if (state === 'listening') {
    h.className = 'dictation-hint on';
    if (t) t.textContent = 'Listening…';
  } else if (state === 'transcribing') {
    h.className = 'dictation-hint on transcribing';
    if (t) t.textContent = 'Transcribing…';
  } else {
    h.className = 'dictation-hint';
  }
}

// Replace the dictation region of the composer with the newest
// transcript: value = prefix + transcript. Text the user had typed
// BEFORE dictation started lives in the prefix; if they edit the box
// between interim results, fold that edit into the prefix instead of
// clobbering it.
function _applyDictation(newText) {
  var st = voiceState;
  var input = document.getElementById('chatInput');
  if (!input) { st.transcript = newText; return; }
  var cur = input.value;
  if (st.lastApplied !== null && cur !== st.lastApplied) {
    if (st.transcript && cur.length >= st.transcript.length &&
        cur.slice(cur.length - st.transcript.length) === st.transcript) {
      // Our previous transcript is still the tail — the user edited
      // the part before it; keep that as the new prefix.
      st.prefix = cur.slice(0, cur.length - st.transcript.length);
    } else {
      // Transcript tail is gone — treat the whole box as new prefix.
      st.prefix = cur ? cur.replace(/\s+$/, '') + ' ' : '';
    }
  }
  st.transcript = newText;
  input.value = st.prefix + newText;
  st.lastApplied = input.value;
  // Programmatic value writes don't fire 'input': nudge the
  // autogrow/send-enable sync on every live update.
  input.dispatchEvent(new Event('input'));
}

function _resetMicButton() {
  var btn = document.getElementById('micBtn');
  if (!btn) return;
  btn.classList.remove('recording');
  btn.classList.remove('transcribing');
  btn.textContent = '🎙';
  btn.title = 'Hold or click to record (local Whisper)';
}

// Tear the dictation session down (final pass done, error, too short).
function _finishDictation(session) {
  if (session !== voiceState.session) return;
  if (voiceState.liveTimer) {
    clearInterval(voiceState.liveTimer);
    voiceState.liveTimer = null;
  }
  voiceState.chunks = [];
  voiceState.finalQueued = false;
  _setDictationHint('off');
  _resetMicButton();
  var input = document.getElementById('chatInput');
  if (input) input.focus();
}

// One transcribe of the CUMULATIVE buffer over the stateless
// transcribeAudio IPC (fresh whisper-cli per call — repeat-callable).
// Interim calls update the composer quietly; the final call also tears
// the session down and surfaces errors. Strictly serialized via
// voiceState.inFlight — a new call is never issued while one runs.
function _transcribePass(isFinal) {
  var st = voiceState;
  var session = st.session;
  var b64 = _voiceWavB64(_mergedVoiceSamples());
  st.inFlight = true;
  sendWithPromise('transcribeAudio', b64).then(function(r) {
    st.inFlight = false;
    if (session !== st.session) {
      // A newer session took over; if ITS stop queued a final pass
      // behind this stale call, run it now that the pipe is free
      // (otherwise that session would hang in 'Transcribing…').
      if (st.finalQueued && !st.recording) {
        st.finalQueued = false;
        _transcribePass(true);
      }
      return;
    }
    if (!r || !r.success) {
      if (isFinal) {
        addErrorMessage('Transcribe: ' + ((r && r.error) || 'unknown') +
                        ((r && r.install_hint) ? '\n' + r.install_hint : ''));
        _finishDictation(session);
      } else {
        // Whisper unavailable/failing — stop the live loop quietly;
        // the final pass on stop surfaces the full error once.
        st.failed = true;
        if (st.finalQueued) { st.finalQueued = false; _transcribePass(true); }
      }
      return;
    }
    _applyDictation((r.text || '').trim());
    if (isFinal) {
      _finishDictation(session);
    } else if (st.finalQueued) {
      // Stop arrived while this interim was in flight — run the queued
      // full-buffer final pass now that the pipe is free.
      st.finalQueued = false;
      _transcribePass(true);
    }
  }, function(err) {
    st.inFlight = false;
    if (session !== st.session) return;
    if (isFinal) {
      addErrorMessage('Transcribe failed: ' + err);
      _finishDictation(session);
    } else {
      st.failed = true;
      if (st.finalQueued) { st.finalQueued = false; _transcribePass(true); }
    }
  });
}

// Interval tick while recording: ship the cumulative audio unless a
// call is already in flight (skip that tick — never overlap IPCs) or
// there isn't enough audio yet to be worth a whisper spawn.
function _liveDictationTick() {
  var st = voiceState;
  if (!st.recording || st.inFlight || st.failed) return;
  var total = 0;
  for (var i = 0; i < st.chunks.length; i++) total += st.chunks[i].length;
  if (total < (st.inputRate || 16000) * VOICE_MIN_SECONDS) return;
  _transcribePass(false);
}

function toggleMic() {
  if (voiceState.recording) {
    _stopRecording();
  } else {
    _startRecording();
  }
}

function _startRecording() {
  var btn = document.getElementById('micBtn');
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    addErrorMessage('Mic API unavailable in this context.');
    return;
  }
  navigator.mediaDevices.getUserMedia({audio: {
    channelCount: 1,
    noiseSuppression: true,
    echoCancellation: true
  }}).then(function(stream) {
    voiceState.stream = stream;
    voiceState.chunks = [];
    voiceState.startedAt = Date.now();
    // New dictation session: invalidates any stale in-flight result.
    voiceState.session++;
    voiceState.failed = false;
    voiceState.finalQueued = false;
    voiceState.transcript = '';
    voiceState.lastApplied = null;
    // Preserve whatever the user already typed: live transcripts are
    // always written as prefix + newest transcript.
    var input0 = document.getElementById('chatInput');
    voiceState.prefix = (input0 && input0.value)
        ? input0.value.replace(/\s+$/, '') + ' ' : '';
    // Use the legacy ScriptProcessorNode — universally supported.
    // AudioWorklet would be cleaner but adds complexity for a v1.
    voiceState.ctx = new (window.AudioContext || window.webkitAudioContext)();
    voiceState.inputRate = voiceState.ctx.sampleRate;
    voiceState.source = voiceState.ctx.createMediaStreamSource(stream);
    voiceState.processor = voiceState.ctx.createScriptProcessor(4096, 1, 1);
    voiceState.processor.onaudioprocess = function(e) {
      var chan = e.inputBuffer.getChannelData(0);
      // Copy because the underlying buffer is reused next frame.
      voiceState.chunks.push(new Float32Array(chan));
      if (Date.now() - voiceState.startedAt > voiceState.maxMs) {
        _stopRecording();
      }
    };
    voiceState.source.connect(voiceState.processor);
    voiceState.processor.connect(voiceState.ctx.destination);
    voiceState.recording = true;
    // Live dictation: every ~2.75s transcribe the cumulative buffer and
    // replace the composer's dictation region with the newest
    // transcript. Ticks are skipped while a transcribe is in flight.
    if (voiceState.liveTimer) clearInterval(voiceState.liveTimer);
    voiceState.liveTimer =
        setInterval(_liveDictationTick, VOICE_LIVE_TICK_MS);
    _setDictationHint('listening');
    if (btn) {
      btn.classList.add('recording');
      btn.textContent = '⏺';
      btn.title = 'Recording... click to stop (auto-stops at 30s)';
    }
  }, function(err) {
    addErrorMessage('Mic permission denied: ' + err);
  });
}

function _stopRecording() {
  var btn = document.getElementById('micBtn');
  if (!voiceState.recording) return;
  voiceState.recording = false;
  if (voiceState.liveTimer) {
    clearInterval(voiceState.liveTimer);
    voiceState.liveTimer = null;
  }
  try { voiceState.processor.disconnect(); } catch(e) {}
  try { voiceState.source.disconnect(); } catch(e) {}
  try { voiceState.ctx.close(); } catch(e) {}
  if (voiceState.stream) {
    voiceState.stream.getTracks().forEach(function(t){ t.stop(); });
  }
  var total = 0;
  for (var i = 0; i < voiceState.chunks.length; i++)
    total += voiceState.chunks[i].length;

  // "Too short" only when the WHOLE take is under ~0.7s, measured at
  // the raw device rate. (The old `< 1600 raw samples` check was ~33ms
  // at 48kHz despite its 0.1s comment.)
  if (total < (voiceState.inputRate || 16000) * VOICE_MIN_SECONDS) {
    _finishDictation(voiceState.session);
    addErrorMessage('Recording too short.');
    return;
  }

  if (btn) {
    btn.classList.remove('recording');
    btn.classList.add('transcribing');
    btn.textContent = '…';
    btn.title = 'Transcribing locally...';
  }
  _setDictationHint('transcribing');

  // Final full-buffer pass replaces the composer text one last time,
  // then tears the session down. If an interim call is still in
  // flight, queue the final behind it — the IPC is never overlapped.
  if (voiceState.inFlight) {
    voiceState.finalQueued = true;
  } else {
    _transcribePass(true);
  }
}

// ---- Initialization ----

(function init() {
  setStatus('loading', 'Initializing...');

  sendWithPromise('initChat').then(function(info) {
    // Apply settings from backend
    if (info.max_history_messages) MAX_HISTORY_MESSAGES = info.max_history_messages;

    // Update hardware bar
    if (info.has_gpu) {
      document.getElementById('hwGpu').textContent = info.gpu_backend.toUpperCase();
    }
    document.getElementById('hwRam').textContent = info.total_ram_gb + 'GB RAM';
    document.getElementById('hwCores').textContent = info.cpu_cores + ' cores';

    // Check model status
    if (info.model_loaded) {
      setStatus('ready', 'Model Ready');
    } else if (info.is_first_run) {
      // Show first-run welcome overlay
      isFirstRun = true;
      setStatus('offline', 'Setup required');
      document.getElementById('welcomeOverlay').classList.add('open');
    } else {
      var downloaded = (info.models || []).filter(function(m) { return m.is_downloaded; });
      if (downloaded.length > 0) {
        setStatus('offline', 'Model available \u2014 choose below to start');
        showModelSelectBanner();
        // Pulse the model pill to draw attention
        var chip = document.getElementById('modelChip');
        if (chip) chip.classList.add('needs-selection');
      } else {
        setStatus('error', 'No models \u2014 download one from the model picker');
      }
    }
  }).catch(function() {
    setStatus('error', 'Failed to initialize');
  });

  // Agent Inbox poller — refreshes the running-agents tray every 3s.
  // Cheap IPC (just a snapshot of an in-memory map) so polling at this
  // cadence is fine. Hides the tray when nothing's running so it
  // doesn't take up real estate.
  function renderAgents(agents) {
    var tray = document.getElementById('agentInbox');
    if (!tray) return;
    if (!agents || !agents.length) {
      tray.style.display = 'none';
      tray.textContent = '';
      return;
    }
    var rows = ['<div class="agent-inbox-header">' +
                'Running agents (' + agents.length + ')</div>'];
    agents.forEach(function(a) {
      var done = a.finished_at_unix && a.finished_at_unix > 0;
      var cls = !done ? '' : (a.succeeded ? ' done-ok' : ' done-err');
      var step = a.total_steps
          ? (a.current_step + '/' + a.total_steps)
          : (a.current_step + '');
      var note = a.status_note || '';
      if (done) note = a.succeeded ? 'Completed' : 'Failed';
      // Cheap text-escape: avoid breaking the DOM if a script name
      // contains stray < or &.
      function esc(s) { return (s + '').replace(/&/g,'&amp;')
                                       .replace(/</g,'&lt;')
                                       .replace(/>/g,'&gt;')
                                       .replace(/"/g,'&quot;'); }
      rows.push(
        '<div class="agent-row">' +
          '<div class="agent-spinner' + cls + '"></div>' +
          '<div class="agent-name" title="' + esc(a.start_url) + '">' +
            esc(a.script_name) + '</div>' +
          '<div class="agent-progress">step ' + esc(step) + '</div>' +
          '<div class="agent-note">' + esc(note) + '</div>' +
        '</div>');
    });
    tray.innerHTML = moltHTML(rows.join(''));
    tray.style.display = 'flex';
  }
  function pollAgents() {
    sendWithPromise('listActiveAgents').then(function(r) {
      renderAgents(r && r.agents);
    }).catch(function() {});
  }
  pollAgents();
  setInterval(pollAgents, 3000);
})();

// ============================================================
// WebAgent UI — autonomous browsing agent in the side panel
// Streams step-by-step progress via the 'agent-step' WebUI event, and the
// agent's plan and its current step via 'agent-plan'.
// Triggered by the "Browse" button next to the chat input.
// ============================================================
(function(){
  var agentRunning = false;

  // Inject agent-specific styles.
  var css = `
.agent-bar{display:none;flex-direction:column;gap:0;background:var(--surface);
  border:1px solid var(--border);border-radius:12px;padding:10px 12px 8px;
  width:100%;max-width:calc(72ch + 32px);margin:0 auto 8px}
.agent-bar.active{display:flex}
.agent-bar-hdr{display:flex;align-items:center;justify-content:space-between;margin-bottom:8px}
.agent-bar-title{font-size:11px;font-weight:700;color:var(--muted);letter-spacing:0.5px}
.agent-stop-btn{font-size:10px;padding:2px 8px;border-radius:6px;
  background:transparent;border:1px solid var(--err);color:var(--err);cursor:pointer}
.agent-stop-btn:hover{background:rgba(240,112,112,0.12)}
.agent-steps{display:flex;flex-direction:column;gap:3px;max-height:220px;overflow-y:auto;
  scrollbar-width:thin}
.agent-step{display:flex;gap:6px;align-items:flex-start;padding:4px 5px;border-radius:5px}
.agent-step.ok{background:var(--surface2)}.agent-step.fail{background:rgba(240,112,112,0.08)}
.agent-step-num{font-size:9px;color:var(--faint);min-width:18px;text-align:right;padding-top:2px}
.agent-step-badge{font-size:8px;font-weight:700;padding:1px 5px;border-radius:5px;
  white-space:nowrap;min-width:52px;text-align:center;background:var(--bg)}
.badge-NAVIGATE,.badge-SEARCH{color:var(--muted)}
.badge-CLICK{color:var(--ok)}
.badge-TYPE,.badge-FILL_FORM{color:var(--muted)}
.badge-SCROLL,.badge-OBSERVE{color:var(--faint)}
.badge-DONE{color:var(--ok)}
.badge-ERROR{color:var(--err)}
.badge-REPLAN{color:var(--muted)}
.agent-plan{display:none;margin:0 0 6px;padding:6px 8px;border-radius:6px;
  background:var(--surface2)}
.agent-plan.shown{display:block}
.agent-plan-title{font-size:9px;font-weight:700;letter-spacing:0.5px;color:var(--muted);
  margin-bottom:3px}
.agent-plan-steps{margin:0;padding-left:18px;display:flex;flex-direction:column;gap:2px}
.agent-plan-step{font-size:10px;color:var(--muted);word-break:break-word}
.agent-plan-step.done{color:var(--faint);text-decoration:line-through}
.agent-plan-step.current{color:var(--text);font-weight:600}
.agent-plan-check{font-size:9px;color:var(--faint);font-weight:400}
.agent-plan-note{font-size:10px;color:var(--muted);word-break:break-word}
.agent-step-body{flex:1;min-width:0}
.agent-step-target{font-size:10px;color:var(--text);white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.agent-step-reason{font-size:9px;color:var(--faint);margin-top:1px}
.agent-spinner-sm{width:7px;height:7px;border:1.5px solid var(--accent);
  border-top-color:transparent;border-radius:50%;animation:spin 0.8s linear infinite;
  margin-top:3px;flex-shrink:0}
.agent-result{font-size:11px;color:var(--text);padding:5px 8px;background:var(--surface2);
  border-radius:5px;margin-top:4px;border-left:2px solid var(--ok)}
.agent-result.fail{background:rgba(240,112,112,0.08);border-color:var(--err);color:var(--err)}
.agent-confirm{display:flex;flex-direction:column;gap:6px;padding:7px 8px;border-radius:6px;
  background:var(--surface2);border-left:2px solid var(--accent);margin:2px 0}
.agent-confirm-title{font-size:9px;font-weight:700;letter-spacing:0.5px;color:var(--muted);
  text-transform:uppercase}
.agent-confirm-msg{font-size:11px;color:var(--text);word-break:break-word}
.agent-confirm-btns{display:flex;gap:6px;justify-content:flex-end}
.agent-confirm-btns button{font-size:10px;padding:3px 10px;border-radius:6px;cursor:pointer;
  border:1px solid var(--border);background:transparent;color:var(--text)}
.agent-confirm-btns .agent-allow{background:var(--accent);border-color:var(--accent);color:#fff}
.agent-save{display:none;flex-wrap:wrap;gap:6px;align-items:center;margin-top:6px}
.agent-save.shown{display:flex}
.agent-save button{font-size:10px;padding:3px 10px;border-radius:6px;cursor:pointer;
  border:1px solid var(--border);background:transparent;color:var(--text)}
.agent-save .agent-save-btn{background:var(--accent);border-color:var(--accent);color:#fff}
.agent-save button:disabled{opacity:0.6;cursor:default}
.agent-save-note{flex-basis:100%;font-size:10px;color:var(--muted);word-break:break-word}
.agent-save-note.ok{color:var(--text)}
  `;
  var s = document.createElement('style');
  s.textContent = css;
  document.head.appendChild(s);

  // Build the agent bar DOM once.
  function ensureBar() {
    if (document.getElementById('agentBar')) return;
    var bar = document.createElement('div');
    bar.id = 'agentBar';
    bar.className = 'agent-bar';
    bar.innerHTML =
      moltHTML('<div class="agent-bar-hdr">' +
        '<span class="agent-bar-title">&#129302; AGENT RUNNING</span>' +
        '<button class="agent-stop-btn" id="agentStopBtn">&#9632; Stop</button>' +
      '</div>' +
      '<div class="agent-plan" id="agentPlan"></div>' +
      '<div class="agent-steps" id="agentSteps"></div>' +
      '<div id="agentResult" style="display:none"></div>' +
      '<div class="agent-save" id="agentSave"></div>');
    var inputArea = document.getElementById('inputArea') ||
                    document.querySelector('.input-area');
    if (inputArea && inputArea.parentNode)
      inputArea.parentNode.insertBefore(bar, inputArea);
    else
      document.body.appendChild(bar);

    document.getElementById('agentStopBtn').addEventListener('click', function() {
      chrome.send('cancelWebAgent', []);
      agentRunning = false;
      finishBar(false, 'Cancelled by user');
    });
  }

  // Show bar and reset its contents.
  function startBar(goal) {
    ensureBar();
    agentRunning = true;
    document.getElementById('agentBar').classList.add('active');
    document.getElementById('agentSteps').innerHTML =
      moltHTML('<div class="agent-step ok">' +
        '<div class="agent-spinner-sm"></div>' +
        '<div class="agent-step-body"><div class="agent-step-target">Goal: ' +
          goal.replace(/[<>&]/g, function(c){return {'<':'&lt;','>':'&gt;','&':'&amp;'}[c];}) +
        '</div></div></div>');
    document.getElementById('agentResult').style.display = 'none';
    // A new run has no plan yet.
    var plan = document.getElementById('agentPlan');
    if (plan) {
      while (plan.firstChild) plan.removeChild(plan.firstChild);
      plan.classList.remove('shown');
    }
    var hdr = document.querySelector('.agent-bar-title');
    if (hdr) hdr.textContent = '\u{1F916} AGENT RUNNING';
    var stopBtn = document.getElementById('agentStopBtn');
    if (stopBtn) stopBtn.style.display = '';
    // A new run: the last one's save offer is gone.
    clearSave();
  }

  // ---- "Save as automation" (docs/design/AGENT_ACTIONS.md §2.9) ----
  // After a run that ended with DONE the browser says whether it can be
  // saved as a draft workflow (save_run, the run's id) or why not
  // (save_note, its own fixed words). Saving happens only on the button's
  // click (the browser checks for the gesture); the draft is reviewed in
  // Agent mode before it can run. Text only, DOM nodes only.
  var saveOffered = false;
  function saveBox() { return document.getElementById('agentSave'); }
  function clearSave() {
    saveOffered = false;
    var box = saveBox();
    if (!box) return;
    while (box.firstChild) box.removeChild(box.firstChild);
    box.classList.remove('shown');
  }
  function saveNote(text, ok) {
    var note = document.createElement('div');
    note.className = 'agent-save-note' + (ok ? ' ok' : '');
    note.textContent = String(text || '').substring(0, 400);
    return note;
  }
  function saveButton(label, primary, onClick) {
    var b = document.createElement('button');
    b.type = 'button';
    if (primary) b.className = 'agent-save-btn';
    b.textContent = label;
    b.addEventListener('click', onClick);
    return b;
  }
  function collapseBar() {
    var bar = document.getElementById('agentBar');
    if (bar) bar.classList.remove('active');
  }
  function offerSave(r) {
    clearSave();
    var box = saveBox();
    if (!box || !r || typeof r !== 'object') return;
    var runId = Number(r.save_run);
    if (Number.isInteger(runId) && runId > 0) {
      saveOffered = true;
      var save = saveButton('Save as automation', true, function() {
        saveRun(runId, save);
      });
      box.appendChild(save);
      box.appendChild(saveButton('Not now', false, function() {
        clearSave();
        collapseBar();
      }));
      box.classList.add('shown');
    } else if (typeof r.save_note === 'string' && r.save_note) {
      box.appendChild(saveNote(r.save_note, false));
      box.classList.add('shown');
    }
  }
  function showSaved(res) {
    var box = saveBox();
    if (!box) return;
    while (box.firstChild) box.removeChild(box.firstChild);
    var ok = !!(res && typeof res === 'object' && res.success === true);
    var text = ok ? res.message
                  : ((res && typeof res === 'object' && res.error) ||
                     'Couldn’t save the run.');
    box.appendChild(saveNote(text, ok));
    if (ok) {
      box.appendChild(saveButton('Review in Agent mode', true, function() {
        chrome.send('openAgentStudio', []);
      }));
    }
    box.appendChild(saveButton('Close', false, function() {
      clearSave();
      collapseBar();
    }));
    box.classList.add('shown');
  }
  function saveRun(runId, btn) {
    btn.disabled = true;
    btn.textContent = 'Saving…';
    sendWithPromise('saveAgentRun', runId)
      .then(showSaved)
      .catch(function() { showSaved(null); });
  }

  // Append one step row.
  cr.addWebUiListener('agent-step', function(step) {
    if (!agentRunning) return;
    var steps = document.getElementById('agentSteps');
    if (!steps) return;
    // Remove placeholder spinner.
    var ph = steps.querySelector('.agent-spinner-sm');
    if (ph && ph.parentNode) ph.parentNode.remove();

    var badge = step.action || '?';
    var target = (step.target || '').substring(0, 70);
    var reason = (step.reason || '').substring(0, 90);

    var row = document.createElement('div');
    row.className = 'agent-step ' + (step.success ? 'ok' : 'fail');
    // `badge` is the model's raw ACTION verb, so it is only ever text.
    var badgeClass = /^[A-Z_]{1,24}$/.test(badge) ? badge : 'OTHER';
    row.innerHTML =
      moltHTML('<div class="agent-step-num">' + (Number(step.number) || 0) + '</div>' +
      '<span class="agent-step-badge badge-' + badgeClass + '"></span>' +
      '<div class="agent-step-body">' +
        '<div class="agent-step-target">' +
          target.replace(/[<>&]/g, function(c){return {'<':'&lt;','>':'&gt;','&':'&amp;'}[c];}) +
        '</div>' +
        (reason
          ? '<div class="agent-step-reason">' +
              reason.replace(/[<>&]/g, function(c){return {'<':'&lt;','>':'&gt;','&':'&amp;'}[c];}) +
            '</div>'
          : '') +
      '</div>');
    row.querySelector('.agent-step-badge').textContent = badge;
    steps.appendChild(row);
    steps.scrollTop = steps.scrollHeight;
  });

  // The agent's plan (#molt-agent-planner) and its current step. The model
  // wrote it from the goal and the pages the agent read, so a page can
  // shape it: every step is shown as text (DOM nodes + textContent), never
  // markup, and the plan never changes what the agent may do.
  // update: {event, steps: [{goal, done_when}], current, replans, note}.
  function renderPlan(update) {
    var box = document.getElementById('agentPlan');
    if (!box || !update || typeof update !== 'object') return;
    while (box.firstChild) box.removeChild(box.firstChild);
    var steps = Array.isArray(update.steps) ? update.steps.slice(0, 6) : [];
    var current = Number(update.current) || 0;
    var replans = Number(update.replans) || 0;
    var title = document.createElement('div');
    title.className = 'agent-plan-title';
    box.appendChild(title);
    if (!steps.length) {
      // No usable plan: the browser's note; the agent goes on without one.
      title.textContent = 'NO PLAN';
      var note = document.createElement('div');
      note.className = 'agent-plan-note';
      note.textContent = String(update.note || '').substring(0, 300);
      box.appendChild(note);
      box.classList.add('shown');
      return;
    }
    title.textContent = replans > 0
        ? 'PLAN (rewritten ' + (replans === 1 ? 'once' : replans + ' times') + ')'
        : 'PLAN';
    var list = document.createElement('ol');
    list.className = 'agent-plan-steps';
    steps.forEach(function(s, i) {
      var n = i + 1;
      var item = document.createElement('li');
      item.className = 'agent-plan-step' +
          (n < current ? ' done' : n === current ? ' current' : '');
      if (n === current) item.setAttribute('aria-current', 'step');
      var goal = document.createElement('div');
      goal.className = 'agent-plan-goal';
      goal.textContent = String((s && s.goal) || '').substring(0, 200);
      var check = document.createElement('div');
      check.className = 'agent-plan-check';
      check.textContent =
          'Done when: ' + String((s && s.done_when) || '').substring(0, 200);
      item.appendChild(goal);
      item.appendChild(check);
      list.appendChild(item);
    });
    box.appendChild(list);
    box.classList.add('shown');
  }

  cr.addWebUiListener('agent-plan', function(update) {
    if (!agentRunning) return;
    renderPlan(update);
  });

  // A frame of another site on the page offers tools: the agent lists
  // them only if the user allows that site on this page. "Not now" only
  // leaves them out; the run goes on whatever the answer. The browser
  // takes "This session" / "Always" only from the click itself.
  function frameToolsChip(req, steps) {
    var row = document.createElement('div');
    row.className = 'agent-confirm';
    row.innerHTML =
      moltHTML('<div class="agent-confirm-title">Another site\'s tools</div>' +
      '<div class="agent-confirm-msg"></div>' +
      '<div class="agent-confirm-btns">' +
        '<button class="agent-deny" data-answer="not_now">Not now</button>' +
        '<button class="agent-allow" data-answer="session">This session' +
        '</button>' +
      '</div>');
    row.querySelector('.agent-confirm-msg').textContent =
        String(req.message || '').substring(0, 300);
    if (req.remember === true) {
      var always = document.createElement('button');
      always.className = 'agent-allow';
      always.setAttribute('data-answer', 'always');
      always.textContent = 'Always here';
      row.querySelector('.agent-confirm-btns').appendChild(always);
    }
    var answered = false;
    row.querySelectorAll('button[data-answer]').forEach(function(b) {
      b.addEventListener('click', function() {
        if (answered) return;
        answered = true;
        if (row.parentNode) row.parentNode.removeChild(row);
        chrome.send('webAgentFrameTools',
                    [Number(req.id) || 0, b.getAttribute('data-answer')]);
      });
    });
    steps.appendChild(row);
    steps.scrollTop = steps.scrollHeight;
  }

  // The agent paused on a sensitive action (leaving the task's sites,
  // submitting a form, a payment / one-time-code field). Nothing happens
  // until the user answers; Deny stops the run. Shown in every permission
  // mode — Auto mode never skips these.
  cr.addWebUiListener('agent-confirm', function(req) {
    if (!agentRunning || !req) return;
    var steps = document.getElementById('agentSteps');
    if (!steps) return;
    if (req.action === 'FRAME_TOOLS') {
      frameToolsChip(req, steps);
      return;
    }
    var row = document.createElement('div');
    row.className = 'agent-confirm';
    row.innerHTML =
      moltHTML('<div class="agent-confirm-title">Agent needs your OK</div>' +
      '<div class="agent-confirm-msg"></div>' +
      '<div class="agent-confirm-btns">' +
        '<button class="agent-deny">Deny</button>' +
        '<button class="agent-allow">Allow</button>' +
      '</div>');
    // The message quotes page text / URLs — text only, never markup.
    row.querySelector('.agent-confirm-msg').textContent =
        String(req.message || '').substring(0, 300);
    var answered = false;
    function answer(allowed) {
      if (answered) return;
      answered = true;
      if (row.parentNode) row.parentNode.removeChild(row);
      chrome.send('webAgentConfirm', [Number(req.id) || 0, allowed === true]);
    }
    row.querySelector('.agent-allow').addEventListener('click', function() {
      answer(true);
    });
    row.querySelector('.agent-deny').addEventListener('click', function() {
      answer(false);
    });
    steps.appendChild(row);
    steps.scrollTop = steps.scrollHeight;
  });

  // Persist the agent outcome in the transcript — the bar itself
  // auto-collapses after 10 s, so without this the result vanishes
  // from the UI entirely. Success renders as a normal AI bubble (and
  // joins conversationHistory so follow-up questions can reference
  // it); failure renders as the standard inline error row.
  function appendAgentOutcome(success, result) {
    // Success bubbles render through renderMarkdown both live (below) and
    // on Recents restore (renderConversationDOM), so the persisted string
    // must carry the same '**Agent result:**' markup the live bubble uses
    // — otherwise the bold header is lost after a reload.
    var successText = '**Agent result:** ' + (result || '(no result)');
    var text = success
        ? successText
        : 'Agent stopped: ' + (result || 'Cancelled');
    if (!success) {
      if (typeof addErrorMessage === 'function') addErrorMessage(text);
      return;
    }
    var m = document.getElementById('messages');
    if (m) {
      var d = document.createElement('div');
      d.className = 'message ai';
      d.innerHTML = moltHTML('<div class="sender">AI Assistant</div>' +
          '<div class="text">' +
          (typeof renderMarkdown === 'function'
              ? renderMarkdown(successText)
              : successText.replace(/[<>&]/g, function(c){
                  return {'<':'&lt;','>':'&gt;','&':'&amp;'}[c]; })) +
          '</div>');
      m.appendChild(d);
      m.scrollTop = m.scrollHeight;
    }
    if (typeof conversationHistory !== 'undefined') {
      conversationHistory.push({role: 'assistant', content: text});
      if (typeof trimHistory === 'function') trimHistory();
      if (typeof saveCurrentConversation === 'function')
        saveCurrentConversation();
    }
  }

  // Finish the bar (success or failure). |r| is the browser's answer to
  // startWebAgent (absent when the user stopped the run).
  function finishBar(success, result, r) {
    agentRunning = false;
    // An unanswered approval chip is moot once the run is over.
    document.querySelectorAll('#agentSteps .agent-confirm').forEach(
        function(el) { el.remove(); });
    var hdr = document.querySelector('.agent-bar-title');
    if (hdr) hdr.textContent = success ? '✅ AGENT DONE' : '❌ AGENT STOPPED';
    var stopBtn = document.getElementById('agentStopBtn');
    if (stopBtn) stopBtn.style.display = 'none';
    var resultEl = document.getElementById('agentResult');
    if (resultEl) {
      resultEl.className = 'agent-result' + (success ? '' : ' fail');
      resultEl.textContent = (result || '(no result)').substring(0, 400);
      resultEl.style.display = 'block';
    }
    appendAgentOutcome(success, result);
    offerSave(success ? r : null);
    // Auto-collapse the bar after 10 s, unless it offers to save the run
    // (then "Not now" closes it).
    setTimeout(function() {
      if (saveOffered || agentRunning) return;
      collapseBar();
    }, 10000);
  }

  // Public entry point: start an agent task.
  window.startWebAgent = function(goal) {
    if (!goal || !goal.trim()) return;
    if (agentRunning) {
      if (typeof addErrorMessage === 'function')
        addErrorMessage('An agent run is already in progress — stop it first.');
      return;
    }
    startBar(goal);
    sendWithPromise('startWebAgent', goal)
      .then(function(r) { finishBar(r && r.success, r && r.result, r); })
      .catch(function(e) { finishBar(false, String(e)); });
  };

  // Empty-input nudge: transient muted hint line above the composer
  // (static #agentHint div in the HTML). Auto-hides after 4 s.
  var agentHintTimer = null;
  function showAgentHint() {
    var h = document.getElementById('agentHint');
    if (!h) return;
    h.style.display = 'block';
    if (agentHintTimer) clearTimeout(agentHintTimer);
    agentHintTimer = setTimeout(function() {
      h.style.display = 'none';
      agentHintTimer = null;
    }, 4000);
  }

  // Wire the "Agent" button (id="agentBtn") added in the HTML.
  function wireAgentBtn() {
    var btn = document.getElementById('agentBtn');
    if (!btn) return;
    btn.addEventListener('click', function() {
      var inp = document.getElementById('chatInput') ||
                document.querySelector('textarea');
      var goal = inp ? inp.value.trim() : '';
      if (!goal) {
        // No silent no-op: focus the input and explain what to type.
        if (inp) inp.focus();
        showAgentHint();
        return;
      }
      // Preflight the silent-failure preconditions with clear inline
      // notices (the goal stays in the composer so the user can fix
      // the precondition and retry):
      // (1) No model loaded — the C++ agent otherwise burns 20 silent
      //     OBSERVE steps because RunPrompt's 'No model loaded' error
      //     text is discarded by WebAgent::AskLLM.
      if (typeof activeModelId !== 'undefined' && !activeModelId) {
        addErrorMessage('The agent needs a loaded model — pick one from ' +
                        'the model menu below, then press Agent again.');
        return;
      }
      // (2) Active tab isn't a regular web page (chrome://, molt://,
      //     about:blank) — the agent acts on the active tab and can't
      //     drive internal pages. Only gate when the native side has
      //     told us what the active tab is; if unknown, let the C++
      //     handler decide (its failure resolves visibly via finishBar).
      var tabCtx = window.__moltCurrentTabContext;
      if (tabCtx && tabCtx.url &&
          (tabCtx.url.indexOf('chrome://') === 0 ||
           tabCtx.url.indexOf('molt://') === 0 ||
           tabCtx.url === 'about:blank')) {
        addErrorMessage('The agent works on the active web page — open a ' +
                        'regular site in the current tab first.');
        return;
      }
      if (inp) {
        inp.value = '';
        // Re-sync autogrow + send-disabled after the programmatic clear.
        inp.dispatchEvent(new Event('input'));
      }
      window.startWebAgent(goal);
    });
  }
  // The chat JS normally runs as an end-of-body script (before
  // DOMContentLoaded), but wire immediately if the document is already
  // interactive so a late-injected copy can't silently miss the event.
  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', wireAgentBtn);
  } else {
    wireAgentBtn();
  }
})();
)JS";

}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_UI_JS_H_
