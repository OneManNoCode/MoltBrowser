// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Content-Security-Policy shared by the molt:// WebUI pages, plus the
// small JS bootstrap every page's script starts with.
//
// Each page is served by its own content::URLDataSource as two
// resources (same pattern as molt_ai_update_ui.cc):
//   /        the HTML — no inline <script>, no on*="..." attributes
//   /app.js  the page script, text/javascript, <script src="app.js">
// under
//   script-src 'self'                   no 'unsafe-inline', no eval
//   require-trusted-types-for 'script'  HTML sinks need TrustedHTML
//   trusted-types molt-html             pages that build HTML strings, or
//   trusted-types                       pages that never write HTML
// style-src keeps 'unsafe-inline': inline style attributes are pervasive
// in these pages and cannot run script. The URLDataSource defaults still
// apply to everything else (object-src 'none', child-src 'none',
// frame-ancestors 'none').
//
// tools/webui_checks/check_no_inline_handlers.py fails the build if an
// inline handler or inline script body comes back.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEBUI_CSP_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEBUI_CSP_H_

#include <optional>
#include <string>

#include "services/network/public/mojom/content_security_policy.mojom.h"
#include "url/gurl.h"

namespace molt_webui {

// Path of each page's script resource.
inline constexpr char kAppJsPath[] = "/app.js";

inline bool IsAppJsRequest(const GURL& url) {
  return url.path() == kAppJsPath;
}

inline std::string MimeTypeFor(const GURL& url) {
  return IsAppJsRequest(url) ? "text/javascript" : "text/html";
}

enum class TrustedTypesMode {
  // The page never assigns HTML strings: no policy may be created.
  kNoPolicies,
  // The page writes HTML only through the single 'molt-html' policy
  // created by kMoltTrustedHtmlJs (see below).
  kMoltHtmlPolicy,
};

// The directives molt pages override. std::nullopt means "keep the
// URLDataSource default" — callers fall through to
// content::URLDataSource::GetContentSecurityPolicy(directive).
inline std::optional<std::string> PageContentSecurityPolicy(
    network::mojom::CSPDirectiveName directive,
    TrustedTypesMode trusted_types) {
  switch (directive) {
    case network::mojom::CSPDirectiveName::ScriptSrc:
      return "script-src 'self';";
    case network::mojom::CSPDirectiveName::StyleSrc:
      return "style-src 'self' 'unsafe-inline';";
    case network::mojom::CSPDirectiveName::TrustedTypes:
      return trusted_types == TrustedTypesMode::kNoPolicies
                 ? "trusted-types;"
                 : "trusted-types molt-html;";
    case network::mojom::CSPDirectiveName::RequireTrustedTypesFor:
      return "require-trusted-types-for 'script';";
    default:
      return std::nullopt;
  }
}

// ---- Shared page-script prologue -------------------------------------
//
// Event wiring without inline handlers. Markup names its handler:
//   <button data-on-click="newChat">     <input data-on-input="filter">
// and the page registers the functions once:
//   moltActions({newChat: function(ev) { ... }, filter: ...});
// Every element gets a real addEventListener() listener — `this` is the
// element, event.stopPropagation() works as it did inline, and returning
// false calls preventDefault() like an inline handler's `return false`.
// Elements inserted later (innerHTML, appendChild) are bound by a
// MutationObserver before any user input can reach them. Only names
// registered through moltActions() can run, so markup can't name
// arbitrary code.
inline constexpr char kMoltActionsJs[] = R"JS(
var moltActions = (function() {
  'use strict';
  var EVENTS = ['click', 'dblclick', 'mousedown', 'mouseup', 'mouseover',
                'mouseout', 'mouseenter', 'mouseleave', 'contextmenu',
                'input', 'change', 'keydown', 'keyup', 'keypress', 'submit',
                'focus', 'blur', 'paste', 'dragstart', 'dragover',
                'dragleave', 'drop', 'load', 'error', 'scroll', 'toggle'];
  var ATTRS = EVENTS.map(function(t) { return 'data-on-' + t; });
  var SELECTOR = ATTRS.map(function(a) { return '[' + a + ']'; }).join(',');
  var table = Object.create(null);
  var bound = new WeakMap();
  var listeners = {};
  EVENTS.forEach(function(type) {
    listeners[type] = function(ev) {
      var name = this.getAttribute('data-on-' + type);
      var fn = name ? table[name] : null;
      if (!fn) return;
      if (fn.call(this, ev) === false) ev.preventDefault();
    };
  });
  function bindOne(el) {
    var done = bound.get(el);
    for (var i = 0; i < EVENTS.length; i++) {
      if (!el.hasAttribute(ATTRS[i])) continue;
      if (!done) { done = {}; bound.set(el, done); }
      if (done[EVENTS[i]]) continue;
      done[EVENTS[i]] = true;
      el.addEventListener(EVENTS[i], listeners[EVENTS[i]]);
    }
  }
  function bindTree(node) {
    if (node.nodeType === 1) bindOne(node);
    if (node.querySelectorAll) {
      var els = node.querySelectorAll(SELECTOR);
      for (var i = 0; i < els.length; i++) bindOne(els[i]);
    }
  }
  bindTree(document);
  new MutationObserver(function(records) {
    for (var i = 0; i < records.length; i++) {
      var r = records[i];
      if (r.type === 'attributes') {
        bindOne(r.target);
      } else {
        for (var j = 0; j < r.addedNodes.length; j++) {
          bindTree(r.addedNodes[j]);
        }
      }
    }
  }).observe(document.documentElement,
             {childList: true, subtree: true, attributes: true,
              attributeFilter: ATTRS});
  function register(fns) {
    for (var k in fns) {
      if (Object.prototype.hasOwnProperty.call(fns, k)) table[k] = fns[k];
    }
  }
  register.has = function(name) { return !!table[name]; };
  register.bindTree = bindTree;
  return register;
})();
)JS";

// Trusted Types: the one HTML policy on pages served with
// TrustedTypesMode::kMoltHtmlPolicy.
//
// With `require-trusted-types-for 'script'`, assigning a plain string to
// innerHTML / outerHTML / insertAdjacentHTML throws — including "" (clear
// with textContent = '' or replaceChildren() instead). 'molt-html' is the
// only policy name the CSP allows and it is created exactly once, here; a
// second createPolicy('molt-html') would throw.
//
// Its createHTML is a deliberate PASS-THROUGH: it does not sanitize. The
// contract is that moltHTML() is only ever called on markup the page
// builds itself from constant template text, with every dynamic value
// escaped by the page's esc()/attribute helpers — or, on the chat page,
// on renderMarkdown() output, which tools/webui_checks fuzzes to prove it
// only emits its own safe markup. Every HTML sink on a page is therefore
// greppable as `moltHTML(`; review each new call site like an eval.
inline constexpr char kMoltTrustedHtmlJs[] = R"JS(
var moltHTML = (function() {
  'use strict';
  var policy = (window.trustedTypes && window.trustedTypes.createPolicy)
      ? window.trustedTypes.createPolicy('molt-html', {
          createHTML: function(s) { return s; }
        })
      : null;
  return function(html) {
    return policy ? policy.createHTML(String(html)) : String(html);
  };
})();
)JS";

}  // namespace molt_webui

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEBUI_CSP_H_
