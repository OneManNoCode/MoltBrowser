// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.
//
// Copyright (c) 2024 The Brave Authors. All rights reserved.
// Modifications copyright 2026 GenEye AI Labs Inc.
//
// MoltShield's procedural cosmetic filter engine. Adapted from Brave's
// components/cosmetic_filters/resources/data/procedural_filters.ts and the
// procedural part of content_cosmetic.ts (brave-core, MPL-2.0): the operator
// table design and the operator semantics. This file stays under the MPL
// 2.0; it is self-contained and only included by cosmetic_filter_agent.cc.
//
// Changes from Brave's version:
//  * The input is MoltShield's vetted JSON (chrome/browser/molt_ai/shield/
//    adblock/procedural.rs): operator arguments arrive pre-parsed as
//    matchers ({text}/{exact}/{re, flags}) instead of strings parsed here;
//    regexes were checked browser-side for catastrophic-backtracking shapes
//    and are tested against at most MAX_REGEX_INPUT characters.
//  * :has-text() / :min-text-length() read textContent (as uBO does), not
//    innerText, which forces a synchronous layout per element.
//  * uBO semantics where Brave differed: :has() is relative (a descendant by
//    default), :upward(sel) starts at the parent, `~` means following
//    siblings, and a sibling/child step after an operator is resolved with
//    :scope instead of comparing one compound selector.
//  * Elements are marked with a per-document random attribute that a user
//    stylesheet (inserted by the agent) hides or styles, and unmarked when
//    they stop matching.
//  * The whole document is re-evaluated after DOM mutations (Brave only
//    re-checked added nodes, missing ads whose text arrives later),
//    throttled adaptively to the cost of the previous pass, with caps on
//    the elements any one step may produce.
//
// The script evaluates to a function, run once per document in an isolated
// world at document start (CosmeticFilterAgent): fn(token, filtersJson).
// `filtersJson` is passed as a string value and only ever JSON.parse()d --
// filter list text never becomes script.

#ifndef CHROME_RENDERER_MOLT_AI_SHIELD_PROCEDURAL_FILTERS_JS_H_
#define CHROME_RENDERER_MOLT_AI_SHIELD_PROCEDURAL_FILTERS_JS_H_

namespace molt_ai::shield {

inline constexpr char kProceduralFiltersJs[] = R"JS(
(function(token, filtersJson) {
  'use strict';

  // Bounds. A pass is a full evaluation of every filter.
  const MAX_ELEMENTS = 5000;      // elements any one step may produce
  const MAX_REGEX_INPUT = 20000;  // characters a regex is tested against
  const MIN_DELAY_MS = 50;        // between passes after mutations
  const MAX_DELAY_MS = 2000;
  const DELAY_PER_PASS_MS = 8;    // delay = this x duration of last pass

  if (typeof token !== 'string' || !/^[a-z][a-z0-9]{7,63}$/.test(token) ||
      typeof filtersJson !== 'string') {
    return;
  }
  let input;
  try {
    input = JSON.parse(filtersJson);
  } catch (e) {
    return;
  }
  if (!Array.isArray(input)) return;

  const doc = document;
  const probe = doc.createDocumentFragment();

  // ---- Matchers ------------------------------------------------------------

  function compileMatcher(m) {
    if (!m || typeof m !== 'object') return null;
    if (typeof m.re === 'string') {
      let re;
      try {
        re = new RegExp(m.re, typeof m.flags === 'string' ? m.flags : '');
      } catch (e) {
        return null;
      }
      return (s) => re.test(
          s.length > MAX_REGEX_INPUT ? s.slice(0, MAX_REGEX_INPUT) : s);
    }
    if (typeof m.exact === 'string') {
      const t = m.exact;
      return (s) => s === t;
    }
    if (typeof m.text === 'string') {
      const t = m.text;
      return t === '' ? (s) => s.trim() === '' : (s) => s.includes(t);
    }
    return null;
  }

  // ---- DOM helpers ---------------------------------------------------------

  function validSelector(sel) {
    if (typeof sel !== 'string' || sel === '') return false;
    try {
      probe.querySelector(sel);
      return true;
    } catch (e) {
      return false;
    }
  }

  function qsa(root, sel) {
    const list = root.querySelectorAll(sel);
    const n = Math.min(list.length, MAX_ELEMENTS);
    const out = new Array(n);
    for (let i = 0; i < n; i++) out[i] = list[i];
    return out;
  }

  function text(el) {
    return el.textContent || '';
  }

  // ---- Operators -----------------------------------------------------------
  // Each compiles its argument once and returns el => [elements].

  // A css-selector step after another step (or inside :has()/:not()):
  // a leading combinator selects relatives, none filters the element.
  function cssStep(arg) {
    const c = arg[0];
    if (c === ' ' || c === '>') {
      const sel = ':scope' + (c === '>' ? ' ' : '') + arg;
      if (!validSelector(sel)) return null;
      return (el) => qsa(el, sel);
    }
    if (c === '+' || c === '~') {
      const sel = arg.slice(1).trim();
      if (!validSelector(sel)) return null;
      if (c === '+') {
        return (el) => {
          const s = el.nextElementSibling;
          return s && s.matches(sel) ? [s] : [];
        };
      }
      return (el) => {
        const out = [];
        for (let s = el.nextElementSibling; s && out.length < MAX_ELEMENTS;
             s = s.nextElementSibling) {
          if (s.matches(sel)) out.push(s);
        }
        return out;
      };
    }
    if (!validSelector(arg)) return null;
    return (el) => (el.matches(arg) ? [el] : []);
  }

  function xpathStep(expr) {
    let compiled;
    try {
      compiled = doc.createExpression(expr, null);
    } catch (e) {
      return null;
    }
    return (ctx) => {
      const r = compiled.evaluate(
          ctx, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
      const out = [];
      const n = Math.min(r.snapshotLength, MAX_ELEMENTS);
      for (let i = 0; i < n; i++) {
        const node = r.snapshotItem(i);
        if (node && node.nodeType === 1) out.push(node);
      }
      return out;
    };
  }

  function matchesCss(pseudo, arg) {
    if (!arg || typeof arg.prop !== 'string') return null;
    const prop = arg.prop;
    const test = compileMatcher(arg.value);
    if (!test) return null;
    return (el) => {
      const value = getComputedStyle(el, pseudo).getPropertyValue(prop);
      return test(value.trim()) ? [el] : [];
    };
  }

  const OPERATORS = {
    'has-text': (arg) => {
      const test = compileMatcher(arg);
      return test && ((el) => (test(text(el)) ? [el] : []));
    },
    'min-text-length': (arg) => {
      if (typeof arg !== 'number' || !(arg >= 0)) return null;
      return (el) => (text(el).length >= arg ? [el] : []);
    },
    'matches-attr': (arg) => {
      if (!arg || typeof arg !== 'object') return null;
      const testName = compileMatcher(arg.name);
      const testValue = arg.value == null ? null : compileMatcher(arg.value);
      if (!testName || (arg.value != null && !testValue)) return null;
      return (el) => {
        for (const attr of el.attributes) {
          if (testName(attr.name) && (!testValue || testValue(attr.value))) {
            return [el];
          }
        }
        return [];
      };
    },
    'matches-css': (arg) => matchesCss(null, arg),
    'matches-css-before': (arg) => matchesCss('::before', arg),
    'matches-css-after': (arg) => matchesCss('::after', arg),
    'upward': (arg) => {
      if (typeof arg === 'number') {
        if (!(arg >= 1 && arg <= 255)) return null;
        return (el) => {
          let e = el;
          for (let i = 0; i < arg && e; i++) e = e.parentElement;
          return e ? [e] : [];
        };
      }
      if (!arg || !validSelector(arg.css)) return null;
      const sel = arg.css;
      return (el) => {
        const p = el.parentElement;
        const e = p && p.closest(sel);
        return e ? [e] : [];
      };
    },
    'xpath': (arg) => (typeof arg === 'string' ? xpathStep(arg) : null),
    'has': (arg) => {
      const inner = compileSteps(arg);
      return inner && ((el) => (run(inner, [el]).length ? [el] : []));
    },
    'not': (arg) => {
      const inner = compileSteps(arg);
      return inner && ((el) => (run(inner, [el]).length ? [] : [el]));
    },
  };

  // Ops relative to given elements -> array of step functions, or null if
  // anything is malformed (the whole filter is then skipped).
  function compileSteps(ops) {
    if (!Array.isArray(ops) || ops.length === 0) return null;
    const steps = [];
    for (const op of ops) {
      if (!op || typeof op.type !== 'string') return null;
      let step = null;
      if (op.type === 'css-selector') {
        step = typeof op.arg === 'string' ? cssStep(op.arg) : null;
      } else if (Object.prototype.hasOwnProperty.call(OPERATORS, op.type)) {
        step = OPERATORS[op.type](op.arg);
      }
      if (!step) return null;
      steps.push(step);
    }
    return steps;
  }

  function run(steps, nodes) {
    for (const step of steps) {
      if (nodes.length === 0) break;
      const next = [];
      const seen = new Set();
      for (const node of nodes) {
        for (const el of step(node)) {
          if (!seen.has(el)) {
            seen.add(el);
            next.push(el);
          }
        }
        if (next.length >= MAX_ELEMENTS) break;
      }
      nodes = next;
    }
    return nodes;
  }

  // A filter's first op is resolved from the document.
  function compileFilter(f) {
    if (!f || !Array.isArray(f.selector) || f.selector.length === 0) {
      return null;
    }
    const first = f.selector[0];
    let start;
    let rest = f.selector;
    if (first && first.type === 'css-selector') {
      if (!validSelector(first.arg)) return null;
      const sel = first.arg;
      start = () => qsa(doc, sel);
      rest = f.selector.slice(1);
    } else if (first && first.type === 'xpath') {
      const step = typeof first.arg === 'string' ? xpathStep(first.arg) : null;
      if (!step) return null;
      start = () => step(doc);
      rest = f.selector.slice(1);
    } else {
      start = () => qsa(doc, '*');
    }
    const steps = rest.length ? compileSteps(rest) : [];
    if (!steps) return null;

    let mark = null;  // attribute token for hide/style; null for DOM actions
    let act = null;
    const a = f.action;
    if (a == null) {
      mark = 'h';
    } else if (a.type === 'style' && Number.isInteger(a.arg) && a.arg >= 0) {
      mark = 's' + a.arg;
    } else if (a.type === 'remove') {
      act = (el) => el.remove();
    } else if (a.type === 'remove-attr' && typeof a.arg === 'string') {
      const name = a.arg;
      act = (el) => {
        if (el.hasAttribute(name)) el.removeAttribute(name);
      };
    } else if (a.type === 'remove-class' && typeof a.arg === 'string') {
      const cls = a.arg;
      act = (el) => {
        if (el.classList.contains(cls)) el.classList.remove(cls);
      };
    } else {
      return null;
    }

    let path = null;
    if (f.path != null) {
      path = compileMatcher(f.path);
      if (!path) return null;
    }
    const watch = Array.isArray(f.watch) ?
        f.watch.filter((x) => typeof x === 'string') : [];
    return {start, steps, mark, act, path, watch, dead: false};
  }

  const filters = [];
  const watched = new Set();
  for (const f of input) {
    const c = compileFilter(f);
    if (!c) continue;
    filters.push(c);
    for (const w of c.watch) watched.add(w);
  }
  if (filters.length === 0) return 0;

  // ---- Marking -------------------------------------------------------------
  // marked: element -> Set of marks applied in the previous pass.

  let marked = new Map();

  function setMarks(el, marks) {
    if (marks.size === 0) {
      if (el.hasAttribute(token)) el.removeAttribute(token);
      return;
    }
    const value = Array.from(marks).sort().join(' ');
    if (el.getAttribute(token) !== value) el.setAttribute(token, value);
  }

  // ---- Passes --------------------------------------------------------------

  let timer = 0;
  let lastDuration = 0;

  function pass() {
    const t0 = performance.now();
    const where = location.pathname + location.search;
    const next = new Map();
    for (const f of filters) {
      if (f.dead || (f.path && !f.path(where))) continue;
      let found;
      try {
        found = run(f.steps, f.start());
      } catch (e) {
        f.dead = true;  // A filter that throws once is skipped from then on.
        continue;
      }
      for (const el of found) {
        if (f.act) {
          try {
            f.act(el);
          } catch (e) {
            f.dead = true;
            break;
          }
          continue;
        }
        let marks = next.get(el);
        if (!marks) next.set(el, (marks = new Set()));
        marks.add(f.mark);
      }
    }
    for (const [el, marks] of next) setMarks(el, marks);
    for (const el of marked.keys()) {
      if (!next.has(el)) setMarks(el, new Set());
    }
    marked = next;
    lastDuration = performance.now() - t0;
  }

  function schedule() {
    if (timer) return;
    const delay = Math.min(MAX_DELAY_MS,
        Math.max(MIN_DELAY_MS, lastDuration * DELAY_PER_PASS_MS));
    timer = setTimeout(() => {
      timer = 0;
      pass();
    }, delay);
  }

  function runSoon() {
    if (timer) {
      clearTimeout(timer);
      timer = 0;
    }
    pass();
  }

  pass();
  const options = {childList: true, subtree: true};
  if (watched.size) {
    options.attributes = true;
    options.attributeFilter = Array.from(watched);
  }
  new MutationObserver(schedule).observe(doc, options);
  doc.addEventListener('DOMContentLoaded', runSoon, {once: true});
  window.addEventListener('load', schedule, {once: true});
  return filters.length;  // For tests; the agent ignores it.
})
)JS";

}  // namespace molt_ai::shield

#endif  // CHROME_RENDERER_MOLT_AI_SHIELD_PROCEDURAL_FILTERS_JS_H_
