// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/automation/automation_recorder.h"

#include <algorithm>
#include <cstdint>  // for int64_t — required on Linux under -fmodules
#include <ctime>
#include <optional>
#include <vector>

#include "base/base64.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "components/viz/common/frame_sinks/copy_output_result.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_widget_host_view.h"
#include "content/public/browser/web_contents.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "ui/gfx/codec/png_codec.h"
#include "ui/gfx/geometry/rect.h"
#include "ui/gfx/geometry/size.h"
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

  function uniq(sel){ try { return document.querySelectorAll(sel).length === 1; } catch(e){ return false; } }
  function goodId(v){ return v && /^[A-Za-z][\w-]*$/.test(v); }
  // Build an ORDERED list of selectors, UNIQUE ones first. A selector value
  // shared across the page (e.g. Nike's data-testid="link" on every nav link)
  // is only kept as a weak fallback, never the primary target — that was the
  // root cause of "Selector did not match" on replay.
  function buildSelectors(el) {
    var strong = [];  // verified to match exactly ONE element
    var weak = [];    // useful but not unique — kept as ordered fallbacks
    function add(sel){
      if (!sel) return;
      if (uniq(sel)) { if (strong.indexOf(sel) < 0) strong.push(sel); }
      else if (weak.indexOf(sel) < 0) weak.push(sel);
    }
    var tag = (el.tagName || '').toLowerCase();
    if (goodId(el.id)) add('#' + CSS.escape(el.id));
    // Attribute selectors, each tried bare, tag-scoped, and id-ancestor-scoped
    // so a value shared across the page can still resolve to a unique node.
    var attrs = ['data-testid','data-test','data-cy','data-qa','name','aria-label'];
    for (var i = 0; i < attrs.length; i++) {
      var v = el.getAttribute && el.getAttribute(attrs[i]);
      if (!v) continue;
      var b = '[' + attrs[i] + '=' + quote(v) + ']';
      add(b);
      add(tag + b);
      var anc = el.closest && el.closest('[id]');
      if (anc && anc !== el && goodId(anc.id)) add('#' + CSS.escape(anc.id) + ' ' + b);
    }
    // Deterministic structural path, anchored to the nearest id ancestor.
    var node = el, parts = [];
    while (node && node.nodeType === 1 && node !== document.body && parts.length < 6) {
      if (goodId(node.id)) { parts.unshift('#' + CSS.escape(node.id)); node = null; break; }
      var t = node.tagName.toLowerCase(), idx = 1, sib = node.previousElementSibling;
      while (sib) { if (sib.tagName === node.tagName) idx++; sib = sib.previousElementSibling; }
      parts.unshift(t + ':nth-of-type(' + idx + ')');
      node = node.parentElement;
    }
    if (parts.length) add(parts.join(' > '));
    var outSel = strong.concat(weak);
    return outSel.length ? outSel : (tag ? [tag] : ['*']);
  }

  // The "accessible name" of an element — recorded as a matching anchor so the
  // runner can find "the link that says Kids" or "the Arrival airport field"
  // even when the selectors drift. Prefer aria-label / placeholder over raw
  // textContent, which on wrapper elements concatenates a label with its
  // instruction text (e.g. "Aéroport d'arrivéeÀ Entrer les trois...").
  function visibleText(el){
    var tag=(el.tagName||'').toLowerCase();
    var role=(attr(el,'role')||'').toLowerCase();
    var isField = tag==='input'||tag==='select'||tag==='textarea'||
                  role==='combobox'||role==='textbox'||role==='searchbox';
    var t;
    if (isField) {
      t = txt(attr(el,'aria-label')) || txt(attr(el,'placeholder')) ||
          txt(el.value) || txt(attr(el,'title'));
    } else {
      t = txt(attr(el,'aria-label')) || txt(el.textContent) || txt(el.value) ||
          txt(attr(el,'title')) || txt(attr(el,'alt'));
    }
    return t ? t.slice(0, 60) : '';
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
    var t = visibleText(el);
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
      '[role="menuitem"],[role="combobox"],[role="textbox"],[role="searchbox"],' +
      'input,select,textarea,[onclick],summary,label';
  // Bounding rect of an element in viewport CSS px, so C++ can crop a
  // record-time thumbnail of exactly what was clicked.
  function elRect(el){
    try { var r = el.getBoundingClientRect();
      return {x: Math.round(r.left), y: Math.round(r.top),
              w: Math.round(r.width), h: Math.round(r.height)}; }
    catch(e){ return null; }
  }
  document.addEventListener('click', function(e) {
    var el = (e.target.closest && e.target.closest(kActionable)) || e.target;
    if (!rateLimit(el, 50)) return;
    var sels = buildSelectors(el);
    send({type: 'click', target: sels[0] || '', selector_fallbacks: sels.slice(1),
          text: visibleText(el),
          rect: elRect(el), dpr: window.devicePixelRatio || 1,
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
      // Record scroll as a RESOLUTION-INDEPENDENT fraction of the scrollable
      // height (0..1), not an absolute pixel offset — so replay lands at the
      // same relative position at any window size.
      var max = Math.max(1, (document.documentElement.scrollHeight ||
                             document.body.scrollHeight || 0) - window.innerHeight);
      var frac = Math.min(1, Math.max(0, window.scrollY / max));
      send({type: 'scroll', value: String(frac),
            description: 'Scroll ' + Math.round(frac*100) + '% down the page'});
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

  // ---- Recording indicator ----
  // The Agent side panel now shows the live step list + a "Recording" banner,
  // so the old in-page REC chip is redundant (and could show a stale count).
  // Remove any leftover overlay from a previous build and don't paint one.
  (function(){ var el = document.getElementById('__moltRecOverlay'); if (el) el.remove(); })();

  console.log('[MoltAutomation] recorder injected');
})();
)JS";

// Encode a captured thumbnail bitmap to a PNG data: URI. Runs on a MayBlock
// pool thread. Returns "" on failure.
std::string EncodeThumbDataUri(const SkBitmap& bitmap) {
  if (bitmap.drawsNothing())
    return std::string();
  std::optional<std::vector<uint8_t>> png =
      gfx::PNGCodec::EncodeBGRASkBitmap(bitmap, /*discard_transparency=*/false);
  if (!png)
    return std::string();
  return "data:image/png;base64," + base::Base64Encode(png.value());
}

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

void AutomationRecorder::DidFinishNavigation(
    content::NavigationHandle* navigation_handle) {
  if (!is_recording_ || !navigation_handle) {
    return;
  }
  content::NavigationHandle* nav = navigation_handle;
  // Only real, committed, primary-main-frame navigations to a NEW document.
  if (!nav->IsInPrimaryMainFrame() || !nav->HasCommitted() ||
      nav->IsSameDocument() || nav->IsErrorPage()) {
    return;
  }
  // Only BROWSER-initiated navigations — typing a URL in the omnibox, a
  // bookmark, or history. Renderer-initiated navigations (clicking a link or a
  // button) are already captured as a CLICK step, so recording them again here
  // would double them up. This is what makes "Go to example.com" the first
  // recorded step when the user starts on the omnibox.
  if (nav->IsRendererInitiated()) {
    return;
  }
  GURL u = nav->GetURL();
  if (!u.is_valid() || !u.SchemeIsHTTPOrHTTPS()) {
    return;
  }
  Step step;
  step.type = StepType::NAVIGATE;
  step.target = u.spec();
  step.description = "Go to " + std::string(u.host());
  DeduplicateAndAppend(std::move(step));
  seen_hosts_.insert(std::string(u.host()));
  // Keep capturing on the freshly-loaded page.
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
  // Visible-text anchor ("Kids") — stored in extra so the runner can match the
  // element by text when the CSS selectors drift.
  if (auto* txt = step_json.FindString("text"); txt && !txt->empty()) {
    base::DictValue e;
    e.Set("text", *txt);
    s.extra = base::Value(std::move(e));
  }
  // Record-time thumbnail: remember the clicked element's rect so we can crop a
  // snapshot of exactly what was clicked after the step is appended.
  bool is_click = (s.type == StepType::CLICK);
  int rx = 0, ry = 0, rw = 0, rh = 0;
  if (const base::DictValue* r = step_json.FindDict("rect")) {
    rx = r->FindInt("x").value_or(0);
    ry = r->FindInt("y").value_or(0);
    rw = r->FindInt("w").value_or(0);
    rh = r->FindInt("h").value_or(0);
  }
  DeduplicateAndAppend(std::move(s));
  if (is_click && rw > 0 && rh > 0 && !steps_.empty()) {
    CaptureElementThumb(static_cast<int>(steps_.size()) - 1, rx, ry, rw, rh);
  }
}

void AutomationRecorder::CaptureElementThumb(int index, int x, int y, int w,
                                             int h) {
  content::RenderWidgetHostView* view =
      web_contents() ? web_contents()->GetRenderWidgetHostView() : nullptr;
  if (!view || !view->IsSurfaceAvailableForCopy())
    return;
  gfx::Rect bounds(view->GetViewBounds().size());  // viewport, DIP, origin 0,0
  gfx::Rect src(x - 40, y - 40, w + 80, h + 80);   // element + a little context
  src.Intersect(bounds);
  if (src.IsEmpty())
    return;
  constexpr int kMaxWidth = 280;
  gfx::Size out = src.size();
  if (out.width() > kMaxWidth) {
    out = gfx::Size(kMaxWidth,
                    std::max(1, src.height() * kMaxWidth / src.width()));
  }
  viz::CopyOutputBitmapWithMetadata empty;
  auto on_copied = base::BindOnce(
      [](base::WeakPtr<AutomationRecorder> self, int index,
         const content::CopyFromSurfaceResult& result) {
        if (!self)
          return;
        if (!result.has_value() || result.value().bitmap.drawsNothing())
          return;
        SkBitmap bitmap = result.value().bitmap;
        base::ThreadPool::PostTaskAndReplyWithResult(
            FROM_HERE, {base::MayBlock(), base::TaskPriority::BEST_EFFORT},
            base::BindOnce(&EncodeThumbDataUri, std::move(bitmap)),
            base::BindOnce(&AutomationRecorder::OnThumbReady, self, index));
      },
      weak_factory_.GetWeakPtr(), index);
  view->CopyFromSurface(
      src, out, base::TimeDelta(),
      mojo::WrapCallbackWithDefaultInvokeIfNotRun(std::move(on_copied),
                                                  base::OwnedRef(empty)));
}

void AutomationRecorder::OnThumbReady(int index, std::string data_uri) {
  if (data_uri.empty() || index < 0 ||
      index >= static_cast<int>(steps_.size())) {
    return;
  }
  base::DictValue e = steps_[index].extra.is_dict()
                          ? steps_[index].extra.GetDict().Clone()
                          : base::DictValue();
  e.Set("ref_shot", std::move(data_uri));
  steps_[index].extra = base::Value(std::move(e));
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
