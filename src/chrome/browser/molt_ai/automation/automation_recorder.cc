// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_recorder.h"

#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <ctime>
#include <optional>

#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace automation {

namespace {

// Self-contained recorder JS: attaches capture-phase listeners and posts
// each event back to C++ via window.chrome.send('automationStep', [json]).
// Robust selector heuristic: data-testid -> id -> name -> aria-label ->
// CSS path with nth-of-type. Posts at most one event per ~50ms per element
// to avoid event storms during typing.
const char kRecorderJS[] = R"JS(
(function() {
  if (window.__moltRecorder) return;
  window.__moltRecorder = true;

  function quote(s) {
    return '"' + String(s).replace(/[\\"]/g, function(c){return '\\'+c;}) + '"';
  }

  function buildSelectors(el) {
    var out = [];
    var attrs = ['data-testid', 'data-test', 'data-cy', 'data-qa'];
    for (var i = 0; i < attrs.length; i++) {
      var v = el.getAttribute && el.getAttribute(attrs[i]);
      if (v) out.push('[' + attrs[i] + '=' + quote(v) + ']');
    }
    if (el.id) out.push('#' + CSS.escape(el.id));
    if (el.name) out.push('[name=' + quote(el.name) + ']');
    var aria = el.getAttribute && el.getAttribute('aria-label');
    if (aria) out.push('[aria-label=' + quote(aria) + ']');
    var role = el.getAttribute && el.getAttribute('role');
    if (role && el.textContent) {
      var t = el.textContent.trim().slice(0, 60);
      out.push('[role=' + quote(role) + ']'
               + ' /* ' + (t || '') + ' */');
    }
    // CSS path fallback (parent>parent>...>tag:nth-of-type(n))
    var node = el;
    var parts = [];
    while (node && node.nodeType === 1 && node !== document.body
           && parts.length < 5) {
      var tag = node.tagName.toLowerCase();
      var idx = 1;
      var sib = node.previousElementSibling;
      while (sib) {
        if (sib.tagName === node.tagName) idx++;
        sib = sib.previousElementSibling;
      }
      parts.unshift(tag + ':nth-of-type(' + idx + ')');
      node = node.parentElement;
    }
    if (parts.length) out.push(parts.join(' > '));
    return out;
  }

  // Maintain an in-page queue. The C++ recorder polls this queue every
  // 400ms and drains it via ExecuteJavaScriptInIsolatedWorld. The JSON
  // string is what crosses the bridge — keeps the protocol stable.
  if (!window.__moltStepQueue) window.__moltStepQueue = [];
  window.__moltDrainSteps = function() {
    var out = window.__moltStepQueue;
    window.__moltStepQueue = [];
    return JSON.stringify(out);
  };

  function send(step) {
    try {
      step.url = location.href;
      step.ts  = Date.now();
      window.__moltStepQueue.push(step);
      // Cap queue so a misbehaving page can't grow it without bound.
      if (window.__moltStepQueue.length > 500) {
        window.__moltStepQueue.splice(0, window.__moltStepQueue.length - 500);
      }
    } catch (e) { /* swallow */ }
  }

  // Per-element debounce so dragging or holding a key doesn't flood.
  var lastSent = new WeakMap();
  function rateLimit(el, ms) {
    var now = Date.now();
    var last = lastSent.get(el) || 0;
    if (now - last < (ms || 50)) return false;
    lastSent.set(el, now);
    return true;
  }

  // ---- Human-readable naming so steps read like plain English ----
  function txt(s){ return (s||'').replace(/\s+/g,' ').trim(); }
  function attr(el,n){ try { return el.getAttribute ? el.getAttribute(n) : ''; } catch(e){ return ''; } }
  // A short, quoted name for a clickable element (its visible text/label).
  function friendlyName(el){
    var t = txt(attr(el,'aria-label')) || txt(el.textContent) ||
            txt(el.value) || txt(attr(el,'title')) || txt(attr(el,'alt')) ||
            txt(attr(el,'placeholder'));
    if (t) return '"' + t.slice(0,40) + '"';
    var tag = (el.tagName||'').toLowerCase();
    return 'the ' + (tag==='a' ? 'link' : (tag==='button' ? 'button' : (tag||'element')));
  }
  // The human label of an input field (associated <label>, aria-label,
  // placeholder, or name).
  function fieldLabel(el){
    try {
      var lab = '';
      if (el.id && window.CSS && CSS.escape) {
        var l = document.querySelector('label[for="'+CSS.escape(el.id)+'"]');
        if (l) lab = txt(l.textContent);
      }
      if (!lab && el.closest) { var p = el.closest('label'); if (p) lab = txt(p.textContent); }
      lab = lab || txt(attr(el,'aria-label')) || txt(attr(el,'placeholder')) ||
            txt(el.name) || txt(el.id);
      return lab ? ('the "'+lab.slice(0,32)+'" field') : 'the field';
    } catch(e){ return 'the field'; }
  }

  // Climb to the nearest actionable ancestor so clicking an icon/label INSIDE
  // a button records the button (not the inner <span>/<svg>).
  var kActionable = 'a,button,[role="button"],[role="link"],[role="tab"],' +
      '[role="menuitem"],input[type="submit"],input[type="button"],' +
      'input[type="checkbox"],input[type="radio"],[onclick],summary,label';
  document.addEventListener('click', function(e) {
    var el = (e.target.closest && e.target.closest(kActionable)) || e.target;
    if (!rateLimit(el, 50)) return;
    var sels = buildSelectors(el);
    send({type: 'click', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          description: 'Click ' + friendlyName(el)});
  }, true);

  document.addEventListener('change', function(e) {
    var el = e.target;
    if (el.tagName !== 'INPUT' && el.tagName !== 'TEXTAREA'
        && el.tagName !== 'SELECT') return;
    // Checkboxes/radios are clicks, not typed text — the click handler covers
    // them.
    if (el.type === 'checkbox' || el.type === 'radio') return;
    var sels = buildSelectors(el);
    var isSel = el.tagName === 'SELECT';
    var v = isSel
        ? (el.options[el.selectedIndex] && el.options[el.selectedIndex].value)
        : el.value;
    // Never record the literal password.
    var shown = (el.type === 'password') ? '••••••' : String(v || '');
    send({type: 'type', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          value: v || '',
          description: (isSel ? 'Choose "' : 'Type "') + shown.slice(0,40) +
                       '" in ' + fieldLabel(el)});
  }, true);

  document.addEventListener('submit', function(e) {
    var el = e.target;
    var sels = buildSelectors(el);
    send({type: 'click', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          description: 'Submit the form'});
  }, true);

  // Throttled scroll capture (every 500ms)
  var scrollThrottle = null;
  document.addEventListener('scroll', function() {
    if (scrollThrottle) return;
    scrollThrottle = setTimeout(function() {
      send({type: 'scroll', value: String(window.scrollY),
            description: 'Scroll to ' + window.scrollY});
      scrollThrottle = null;
    }, 500);
  }, true);

  // Track URL changes for SPAs.
  var lastUrl = location.href;
  setInterval(function() {
    if (location.href !== lastUrl) {
      lastUrl = location.href;
      send({type: 'navigate', target: location.href,
            description: 'URL changed (SPA)'});
    }
  }, 600);

  // ---- Recording overlay banner ----
  // A small fixed-position chip in the bottom right so the user always
  // sees that recording is active and how many steps were captured.
  function renderOverlay() {
    var el = document.getElementById('__moltRecOverlay');
    if (!el) {
      el = document.createElement('div');
      el.id = '__moltRecOverlay';
      el.style.cssText = [
        'position:fixed','right:14px','bottom:14px','z-index:2147483647',
        'padding:8px 12px','border-radius:999px',
        'background:rgba(220,38,38,0.95)','color:white',
        'font-family:-apple-system,system-ui,sans-serif','font-size:13px',
        'font-weight:600','box-shadow:0 4px 12px rgba(0,0,0,0.25)',
        'pointer-events:none','user-select:none','letter-spacing:0.2px',
      ].join(';');
      el.innerHTML = '<span style="display:inline-block;width:8px;height:8px;'
                      + 'border-radius:50%;background:white;margin-right:8px;'
                      + 'animation:moltRecPulse 1.4s ease-in-out infinite"></span>'
                      + '<span id="__moltRecCount">REC</span>';
      var style = document.createElement('style');
      style.textContent = '@keyframes moltRecPulse{'
                          + '0%,100%{opacity:0.4}50%{opacity:1}}';
      document.head && document.head.appendChild(style);
      (document.body || document.documentElement).appendChild(el);
    }
    var c = document.getElementById('__moltRecCount');
    if (c) c.textContent = 'REC · ' + window.__moltStepQueue.length;
  }
  setInterval(renderOverlay, 600);
  renderOverlay();

  console.log('[MoltAutomation] recorder injected');
})();
)JS";

}  // namespace

AutomationRecorder::AutomationRecorder(content::WebContents* contents)
    : content::WebContentsObserver(contents) {}

AutomationRecorder::~AutomationRecorder() = default;

void AutomationRecorder::Start(StepCapturedCallback on_step_captured) {
  is_recording_ = true;
  steps_.clear();
  seen_hosts_.clear();
  on_step_captured_ = std::move(on_step_captured);
  // Capture an initial NAVIGATE step for the current URL so replays
  // always start from a known location.
  if (web_contents()) {
    GURL u = web_contents()->GetLastCommittedURL();
    if (u.is_valid() && u.SchemeIsHTTPOrHTTPS()) {
      Step nav;
      nav.type = StepType::NAVIGATE;
      nav.target = u.spec();
      nav.description = "Open " + std::string(u.host());
      DeduplicateAndAppend(std::move(nav));
      seen_hosts_.insert(std::string(u.host()));
    }
  }
  InjectRecorderJS();

  // Drive the page-side queue from C++ at 400ms cadence.
  poll_timer_.Start(FROM_HERE, base::Milliseconds(400),
                    base::BindRepeating(&AutomationRecorder::PollPageQueue,
                                         weak_factory_.GetWeakPtr()));
}

Script AutomationRecorder::Stop() {
  is_recording_ = false;
  poll_timer_.Stop();
  Script s;
  s.id = "";
  s.name = "Recorded automation";
  s.created_at_unix = static_cast<int64_t>(std::time(nullptr));
  s.ai_model = "tinyllama-1.1b";
  s.steps = std::move(steps_);
  s.security.domain_whitelist.assign(seen_hosts_.begin(), seen_hosts_.end());
  s.security.require_approval_for = {"form_submit", "payment", "login"};
  s.security.trust = TrustLevel::CASUAL;
  on_step_captured_.Reset();
  return s;
}

void AutomationRecorder::PollPageQueue() {
  if (!is_recording_ || !web_contents() ||
      !web_contents()->GetPrimaryMainFrame()) {
    return;
  }
  // Drain returns a JSON-stringified array. We re-inject the recorder JS
  // first call (idempotent at the page level via window.__moltRecorder).
  std::u16string js = base::UTF8ToUTF16(std::string(
      "(window.__moltDrainSteps && window.__moltDrainSteps()) || '[]'"));
  web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      js,
      base::BindOnce(
          [](base::WeakPtr<AutomationRecorder> self, base::Value v) {
            if (!self || !self->is_recording_) return;
            if (!v.is_string()) return;
            std::optional<base::Value> parsed =
                base::JSONReader::Read(v.GetString(), /*options=*/0);
            if (!parsed || !parsed->is_list()) return;
            for (auto& entry : parsed->GetList()) {
              if (entry.is_dict())
                self->OnStepFromInjectedJS(entry.GetDict());
            }
          },
          weak_factory_.GetWeakPtr()),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void AutomationRecorder::DocumentOnLoadCompletedInPrimaryMainFrame() {
  if (is_recording_)
    InjectRecorderJS();
}

void AutomationRecorder::InjectRecorderJS() {
  if (!web_contents() || !web_contents()->GetPrimaryMainFrame())
    return;
  web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kRecorderJS)),
      base::DoNothing(),
      content::ISOLATED_WORLD_ID_CONTENT_END);
}

void AutomationRecorder::OnStepFromInjectedJS(
    const base::DictValue& step_json) {
  if (!is_recording_)
    return;
  // Track host for whitelist.
  if (auto* url = step_json.FindString("url")) {
    GURL g(*url);
    if (g.is_valid() && !g.host().empty())
      seen_hosts_.insert(std::string(g.host()));
  }
  // Build a Step from the JSON.
  Step s;
  if (auto* t = step_json.FindString("type"))
    s.type = StepTypeFromString(*t);
  if (auto* t = step_json.FindString("target"))
    s.target = *t;
  if (const base::ListValue* fbs =
          step_json.FindList("selector_fallbacks")) {
    for (const auto& v : *fbs)
      if (v.is_string()) s.selector_fallbacks.push_back(v.GetString());
  }
  if (auto* v = step_json.FindString("value"))
    s.value = *v;
  if (auto* d = step_json.FindString("description"))
    s.description = *d;
  DeduplicateAndAppend(std::move(s));
}

void AutomationRecorder::DeduplicateAndAppend(Step step) {
  // Dedup: collapse adjacent SCROLL events; collapse adjacent TYPE events
  // on the same selector (keep latest value).
  if (!steps_.empty()) {
    Step& last = steps_.back();
    if (step.type == StepType::SCROLL && last.type == StepType::SCROLL) {
      last.value = step.value;
      last.description = step.description;
      return;
    }
    if (step.type == StepType::TYPE && last.type == StepType::TYPE &&
        last.target == step.target) {
      last.value = step.value;
      return;
    }
  }
  steps_.push_back(std::move(step));
  if (on_step_captured_)
    on_step_captured_.Run(steps_.back(), static_cast<int>(steps_.size()));
}

}  // namespace automation
}  // namespace molt_ai
