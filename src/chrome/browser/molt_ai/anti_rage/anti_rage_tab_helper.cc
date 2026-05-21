// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/anti_rage/anti_rage_tab_helper.h"

#include "base/functional/callback_helpers.h"
#include "base/strings/utf_string_conversions.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "url/gurl.h"

namespace molt_ai {
namespace anti_rage {

namespace {

// Detector + chip JS. Runs in isolated world 1, idempotent via
// window.__moltAntiRageInstalled.
//
// Heuristics:
//   - Rage-click: 4+ pointer-down events on the same DOM node within
//     a 3s sliding window.
//   - Throttle: at most one chip per 30s, regardless of how rage-y
//     the user gets. We're suggesting a workflow, not nagging.
//   - Auto-dismiss: chip self-removes after 10s.
//   - Visibility-respecting: skips clicks on inputs, textareas,
//     contenteditable — typing is not rage.
constexpr char kDetectorJS[] = R"JS(
(function(){
  if (window.__moltAntiRageInstalled) return;
  window.__moltAntiRageInstalled = true;

  var clicks = [];      // ring of recent clicks
  var lastChipMs = 0;

  function isTypingTarget(el) {
    if (!el) return false;
    var t = (el.tagName || '').toUpperCase();
    if (t === 'INPUT' || t === 'TEXTAREA' || t === 'SELECT') return true;
    if (el.isContentEditable) return true;
    return false;
  }

  function showChip(reason) {
    var now = Date.now();
    if (now - lastChipMs < 30000) return;
    lastChipMs = now;

    // Build a self-contained chip with inline styles so we don't
    // collide with the host page's CSS.
    var chip = document.createElement('div');
    chip.setAttribute('id', '__molt_rage_chip');
    chip.style.cssText = [
      'position:fixed', 'bottom:16px', 'right:16px',
      'z-index:2147483647', 'max-width:320px',
      'background:#1f1f1f', 'color:#e8e8e8',
      'border:1px solid #3a86ff',
      'border-radius:10px', 'padding:10px 12px',
      'font:13px/1.4 -apple-system,BlinkMacSystemFont,Segoe UI,sans-serif',
      'box-shadow:0 6px 20px rgba(0,0,0,0.35)',
      'display:flex', 'align-items:flex-start', 'gap:10px',
      'opacity:0', 'transition:opacity 200ms ease-out'
    ].join(';');
    var icon = document.createElement('div');
    icon.style.cssText = 'font-size:18px;line-height:1;flex-shrink:0';
    icon.textContent = '🐉';  // dragon — "rage" in MoltBrowser-speak
    var body = document.createElement('div');
    body.style.cssText = 'flex:1';
    body.innerHTML =
      '<div style="font-weight:600;margin-bottom:2px">Page acting up?</div>' +
      '<div style="opacity:0.85">Open the MoltBrowser side panel and try ' +
      '<code style="background:#2a2a2a;padding:1px 4px;border-radius:3px">/simplify</code> ' +
      'for a clean reader view, or ' +
      '<code style="background:#2a2a2a;padding:1px 4px;border-radius:3px">/sandbox</code> ' +
      'to reload in a throwaway session.</div>';
    var close = document.createElement('button');
    close.textContent = '×';
    close.style.cssText = [
      'background:transparent', 'border:0', 'color:#888',
      'font-size:18px', 'line-height:1', 'cursor:pointer',
      'padding:0 0 0 4px', 'margin-top:-2px'
    ].join(';');
    close.onclick = function(){ if (chip.parentNode) chip.remove(); };
    chip.appendChild(icon);
    chip.appendChild(body);
    chip.appendChild(close);
    (document.body || document.documentElement).appendChild(chip);
    // Fade in.
    requestAnimationFrame(function(){ chip.style.opacity = '1'; });
    // Auto-dismiss after 10s.
    setTimeout(function(){
      if (!chip.parentNode) return;
      chip.style.opacity = '0';
      setTimeout(function(){ if (chip.parentNode) chip.remove(); }, 300);
    }, 10000);
  }

  document.addEventListener('pointerdown', function(e) {
    var tgt = e.target;
    if (!tgt || isTypingTarget(tgt)) return;
    var now = Date.now();
    // Window: last 3 seconds.
    clicks = clicks.filter(function(c){ return now - c.t < 3000; });
    clicks.push({t: now, tgt: tgt});
    // Count clicks on this exact node (or its ancestors up 2 levels).
    var hits = 0;
    for (var i = 0; i < clicks.length; i++) {
      var c = clicks[i];
      if (c.tgt === tgt) { hits++; continue; }
      // Cluster clicks on the same parent within depth 2 — covers
      // "click button text inside icon-wrapped link" patterns.
      var p = tgt;
      for (var d = 0; d < 2 && p; d++) {
        if (c.tgt === p) { hits++; break; }
        p = p.parentElement;
      }
    }
    if (hits >= 4) {
      showChip('rage-click');
      clicks = [];
    }
  }, /*useCapture=*/true);
})();
)JS";

}  // namespace

AntiRageTabHelper::AntiRageTabHelper(content::WebContents* contents)
    : content::WebContentsObserver(contents),
      content::WebContentsUserData<AntiRageTabHelper>(*contents) {}

AntiRageTabHelper::~AntiRageTabHelper() = default;

void AntiRageTabHelper::DocumentOnLoadCompletedInPrimaryMainFrame() {
  InjectDetector();
}

void AntiRageTabHelper::InjectDetector() {
  if (!web_contents() || !web_contents()->GetPrimaryMainFrame())
    return;
  if (!web_contents()->GetLastCommittedURL().SchemeIsHTTPOrHTTPS())
    return;
  web_contents()->GetPrimaryMainFrame()->ExecuteJavaScriptInIsolatedWorld(
      base::UTF8ToUTF16(std::string(kDetectorJS)),
      base::DoNothing(),
      /*world_id=*/1);
}

WEB_CONTENTS_USER_DATA_KEY_IMPL(AntiRageTabHelper);

}  // namespace anti_rage
}  // namespace molt_ai
