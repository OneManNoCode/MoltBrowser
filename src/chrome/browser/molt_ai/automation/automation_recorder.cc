// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_recorder.h"

#include <ctime>

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

  function send(step) {
    try {
      step.url = location.href;
      step.ts  = Date.now();
      // chrome.send is provided by the WebUI host. For non-WebUI tabs we
      // post to the parent via a custom event the C++ side captures.
      if (window.chrome && window.chrome.send) {
        window.chrome.send('automationStep', [step]);
      } else {
        document.dispatchEvent(new CustomEvent(
            '__moltAutomationStep', {detail: JSON.stringify(step)}));
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

  document.addEventListener('click', function(e) {
    var el = e.target;
    if (!rateLimit(el, 50)) return;
    var sels = buildSelectors(el);
    send({type: 'click', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          description: 'Click ' + (el.textContent||'').trim().slice(0,40)});
  }, true);

  document.addEventListener('change', function(e) {
    var el = e.target;
    if (el.tagName !== 'INPUT' && el.tagName !== 'TEXTAREA'
        && el.tagName !== 'SELECT') return;
    var sels = buildSelectors(el);
    var v = el.tagName === 'SELECT'
        ? el.options[el.selectedIndex] && el.options[el.selectedIndex].value
        : el.value;
    send({type: 'type', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          value: v || '',
          description: 'Type into ' + (el.name||el.id||el.tagName)});
  }, true);

  document.addEventListener('submit', function(e) {
    var el = e.target;
    var sels = buildSelectors(el);
    send({type: 'click', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          description: 'Submit form'});
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
}

Script AutomationRecorder::Stop() {
  is_recording_ = false;
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
