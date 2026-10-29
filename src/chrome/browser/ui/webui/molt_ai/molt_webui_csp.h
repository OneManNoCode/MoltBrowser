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

// Any script resource a page serves (app.js, plus e.g. the chat page's
// pre-paint theme.js). Everything else a molt page serves is HTML.
inline std::string MimeTypeFor(const GURL& url) {
  return url.path().ends_with(".js") ? "text/javascript" : "text/html";
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

// Accessibility helpers shared by the pages that need them
// (docs/ACCESSIBILITY.md). The page never writes HTML here.
//
//   moltA11y.announce(text)           read out politely by screen readers
//   moltA11y.announce(text, {assertive: true})   read out at once (errors)
//   moltA11y.openDialog(el, {labelledBy, label, focus, onEscape})
//       |el| becomes a modal dialog: role, aria-modal and a name; focus
//       moves into it and everything else on the page is inert (unreachable
//       by Tab, pointer and screen reader) until
//   moltA11y.closeDialog(el)          which puts focus back where it was.
//   moltA11y.reducedMotion()          the user asked for less motion
//   moltA11y.scrollBehavior()         'smooth', or 'auto' for less motion
//
// Enter and Space activate an element that isn't a native control but has
// an activatable role (role="button" and the like) and a tabindex, as they
// do a <button>. Pages use real buttons and links wherever they can; this
// is for the rare control that can't be one.
//
// The live regions (role=status, role=alert) exist from the start: screen
// readers ignore a region that appears together with its text. They carry
// data-molt-live and are never made inert, so a message is heard while a
// dialog is open too.
inline constexpr char kMoltA11yJs[] = R"JS(
var moltA11y = (function() {
  'use strict';
  var HIDDEN_STYLE = 'position:absolute;width:1px;height:1px;margin:-1px;' +
      'padding:0;border:0;overflow:hidden;clip:rect(0 0 0 0);' +
      'clip-path:inset(50%);white-space:nowrap';
  var regions = {};
  function region(role) {
    var r = regions[role];
    if (r && r.isConnected) return r;
    r = document.createElement('div');
    r.setAttribute('role', role);
    r.setAttribute('aria-atomic', 'true');
    r.setAttribute('data-molt-live', role);
    r.style.cssText = HIDDEN_STYLE;
    (document.body || document.documentElement).appendChild(r);
    regions[role] = r;
    return r;
  }
  var timers = {};
  // The text is set a moment after the region is emptied, so the same
  // message twice in a row is read twice.
  function announce(text, opts) {
    var role = opts && opts.assertive ? 'alert' : 'status';
    var r = region(role);
    var msg = String(text == null ? '' : text).replace(/\s+/g, ' ').trim();
    r.textContent = '';
    clearTimeout(timers[role]);
    if (!msg) return;
    timers[role] = setTimeout(function() { r.textContent = msg; }, 60);
  }

  var FOCUSABLE = 'a[href],area[href],button,input,select,textarea,iframe,' +
      'summary,[tabindex],[contenteditable="true"],[contenteditable=""]';
  function canFocus(el) {
    if (el.disabled || el.type === 'hidden' ||
        el.getAttribute('tabindex') === '-1') {
      return false;
    }
    for (var n = el; n && n.nodeType === 1; n = n.parentElement) {
      if (n.hidden || n.hasAttribute('inert')) return false;
      var cs = window.getComputedStyle(n);
      if (cs.display === 'none' || cs.visibility === 'hidden') return false;
    }
    return true;
  }
  function firstFocusable(root) {
    var all = root.querySelectorAll(FOCUSABLE);
    for (var i = 0; i < all.length; i++) {
      if (canFocus(all[i])) return all[i];
    }
    return null;
  }

  var open = [];
  function entryFor(el) {
    for (var i = open.length - 1; i >= 0; i--) {
      if (open[i].el === el) return i;
    }
    return -1;
  }
  function openDialog(el, opts) {
    opts = opts || {};
    if (entryFor(el) >= 0) closeDialog(el, {keepFocus: true});
    var role = el.getAttribute('role');
    if (role !== 'dialog' && role !== 'alertdialog') {
      el.setAttribute('role', 'dialog');
    }
    el.setAttribute('aria-modal', 'true');
    if (opts.labelledBy) el.setAttribute('aria-labelledby', opts.labelledBy);
    if (opts.label) el.setAttribute('aria-label', opts.label);
    if (!el.hasAttribute('tabindex')) el.setAttribute('tabindex', '-1');
    var d = {el: el, opener: document.activeElement, onEscape: opts.onEscape,
             made: [], lifted: []};
    // A dialog opened over another one: its own ancestors come back out
    // of the first one's inert set while it is open.
    for (var a = el; a && a.nodeType === 1; a = a.parentElement) {
      if (!a.hasAttribute('inert')) continue;
      for (var k = 0; k < open.length; k++) {
        if (open[k].made.indexOf(a) >= 0) {
          a.removeAttribute('inert');
          d.lifted.push(a);
          break;
        }
      }
    }
    for (var n = el; n.parentElement && n !== document.body;
         n = n.parentElement) {
      var sibs = n.parentElement.children;
      for (var i = 0; i < sibs.length; i++) {
        var s = sibs[i];
        if (s === n || s.hasAttribute('inert') ||
            s.hasAttribute('data-molt-live') ||
            s.tagName === 'SCRIPT' || s.tagName === 'STYLE') {
          continue;
        }
        s.setAttribute('inert', '');
        d.made.push(s);
      }
    }
    open.push(d);
    var target = opts.focus || el.querySelector('[autofocus]') ||
                 firstFocusable(el) || el;
    target.focus();
  }
  function closeDialog(el, opts) {
    var i = entryFor(el);
    if (i < 0) return false;
    var d = open.splice(i, 1)[0];
    d.made.forEach(function(s) { s.removeAttribute('inert'); });
    d.lifted.forEach(function(s) { s.setAttribute('inert', ''); });
    if (opts && opts.keepFocus) return true;
    var back = d.opener;
    if (back && back.isConnected && back.focus && canFocus(back)) {
      back.focus();
    } else if (opts && opts.fallback) {
      opts.fallback.focus();
    }
    return true;
  }
  function isOpen(el) { return entryFor(el) >= 0; }

  document.addEventListener('keydown', function(e) {
    if (e.defaultPrevented) return;
    if (e.key === 'Escape' && open.length) {
      var top = open[open.length - 1];
      if (top.onEscape) {
        e.preventDefault();
        top.onEscape(e);
      }
      return;
    }
    if ((e.key !== 'Enter' && e.key !== ' ') ||
        e.altKey || e.ctrlKey || e.metaKey) {
      return;
    }
    var t = e.target;
    if (!t || t.nodeType !== 1 || t.isContentEditable) return;
    if (/^(BUTTON|INPUT|SELECT|TEXTAREA|SUMMARY)$/.test(t.tagName)) return;
    if (t.tagName === 'A' && t.hasAttribute('href')) return;
    var role = t.getAttribute('role');
    if (!/^(button|link|menuitem|menuitemcheckbox|menuitemradio|tab|option|switch|checkbox|radio|treeitem)$/.test(role || '')) {
      return;
    }
    if (role === 'link' && e.key === ' ') return;
    e.preventDefault();
    t.click();
  });

  function reducedMotion() {
    return !!(window.matchMedia &&
              window.matchMedia('(prefers-reduced-motion: reduce)').matches);
  }

  region('status');
  region('alert');
  return {
    announce: announce,
    openDialog: openDialog,
    closeDialog: closeDialog,
    isOpen: isOpen,
    reducedMotion: reducedMotion,
    scrollBehavior: function() { return reducedMotion() ? 'auto' : 'smooth'; },
  };
})();
)JS";

}  // namespace molt_webui

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_WEBUI_CSP_H_
