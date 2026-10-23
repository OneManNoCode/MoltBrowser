// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_script.h"

namespace molt_ai::page_translate {

// One function expression, so check_embedded_js.py parses it as a whole
// program; page_translate_io.cc appends "(<JSON arguments>)". Linear in the
// nodes it visits (capped); no regular expression runs on more than one
// text at a time, and none can backtrack.
const char kPageTranslateScript[] = R"JS((function moltPageTranslate(args) {
  'use strict';
  const KEY = '__moltPageTranslation';
  const HTML_NS = 'http://www.w3.org/1999/xhtml';
  const TEXT = 0, TITLE = 1, ALT = 2, PLACEHOLDER = 3;
  // Never read or written: code, scripts and styles, form controls (their
  // text is a value), embedded and media content.
  const SKIP = new Set([
    'script', 'style', 'noscript', 'template', 'code', 'pre', 'kbd',
    'samp', 'var', 'textarea', 'input', 'select', 'option', 'optgroup',
    'datalist', 'object', 'embed', 'iframe', 'frame', 'frameset', 'canvas',
    'audio', 'video', 'head',
  ]);
  // Fields whose placeholder is shown as text.
  const TEXT_FIELDS = new Set(
      ['text', 'search', 'email', 'url', 'tel', 'password', 'number']);
  // A write never lands inside these, whatever the page did since.
  const UNSAFE = 'script, style, noscript, template, textarea, iframe, ' +
      'object, embed';
  const LETTER = /\p{L}/u;
  const SPACES = /[ \t\n\r\f]+/g;
  const g = globalThis;
  const doc = document;

  function read(slot) {
    return slot.attr ? slot.target.getAttribute(slot.attr)
                     : slot.target.nodeValue;
  }

  function write(slot, text) {
    if (slot.attr) {
      slot.target.setAttribute(slot.attr, text);
    } else {
      slot.target.nodeValue = text;
    }
  }

  // Puts back what the last job wrote, where the page hasn't changed it
  // since, and forgets the job.
  function restore() {
    const state = g[KEY];
    let restored = 0;
    if (state && Array.isArray(state.slots)) {
      for (const slot of state.slots) {
        if (slot.written !== null && read(slot) === slot.written) {
          write(slot, slot.orig);
          restored++;
        }
      }
    }
    delete g[KEY];
    return restored;
  }

  function optedOut(el) {
    const translate = el.getAttribute('translate');
    return (translate !== null && translate.toLowerCase() === 'no') ||
        el.classList.contains('notranslate');
  }

  // contenteditable on |el| itself (isContentEditable covers the rest).
  function editableAttribute(el) {
    const value = el.getAttribute('contenteditable');
    return value !== null && value.toLowerCase() !== 'false';
  }

  // |el| is in editable content, whatever the page did since it was read.
  function inEditable(el) {
    if (el.isContentEditable === true) {
      return true;
    }
    const host = el.closest('[contenteditable]');
    return host !== null && editableAttribute(host);
  }

  function extract() {
    restore();
    const caps = args.caps;
    const state = {id: args.id, slots: []};
    const out = [];
    let total = 0;
    let visited = 0;
    let truncated = false;
    const height = g.innerHeight;
    const width = g.innerWidth;
    const about = new Map();

    // Layout facts of an element whose text or attribute is read.
    function facts(el) {
      let f = about.get(el);
      if (!f) {
        const box = el.getBoundingClientRect();
        const style = g.getComputedStyle(el);
        const spaces = style.whiteSpaceCollapse || style.whiteSpace || '';
        f = {
          shown: typeof el.checkVisibility !== 'function' ||
              el.checkVisibility({visibilityProperty: true}),
          inView: box.bottom > 0 && box.right > 0 && box.top < height &&
              box.left < width,
          collapse: spaces === 'collapse' || spaces === 'normal' ||
              spaces === 'nowrap',
        };
        about.set(el, f);
      }
      return f;
    }

    // Adds a slot; false once a cap is reached (the rest of the page stays
    // as it is).
    function add(kind, inView, text, target, attr) {
      if (state.slots.length >= caps.slots ||
          total + text.length > caps.pageChars) {
        truncated = true;
        return false;
      }
      total += text.length;
      state.slots.push({
        target: target,
        attr: attr,
        orig: attr ? target.getAttribute(attr) : target.nodeValue,
        written: null,
      });
      out.push([kind, inView, text]);
      return true;
    }

    function attribute(el, name, kind) {
      const value = el.getAttribute(name);
      if (value === null || value.length > caps.slotChars ||
          !LETTER.test(value)) {
        return true;
      }
      const f = facts(el);
      if (!f.shown) {
        return true;
      }
      return add(kind, f.inView, value.replace(SPACES, ' '), el, name);
    }

    function rendered(el) {
      if (typeof el.checkVisibility !== 'function' || el.checkVisibility()) {
        return true;
      }
      // display: contents has no box of its own; its children may.
      return g.getComputedStyle(el).display === 'contents';
    }

    // Whether to look inside |el|. Reads its alt or placeholder on the way.
    let open = true;
    function accept(el) {
      if (el.namespaceURI !== HTML_NS || optedOut(el) || !rendered(el)) {
        return false;
      }
      const name = el.localName;
      if (name === 'textarea' ||
          (name === 'input' && TEXT_FIELDS.has(el.type))) {
        open = attribute(el, 'placeholder', PLACEHOLDER);
        return false;
      }
      // An editable ancestor was refused already, so its own attribute is
      // enough here.
      if (SKIP.has(name) || el.isContentEditable === true ||
          editableAttribute(el)) {
        return false;
      }
      if (name === 'img') {
        open = attribute(el, 'alt', ALT);
      }
      return true;
    }

    function readText(node) {
      const raw = node.nodeValue;
      const parent = node.parentElement;
      if (!parent || raw.length > caps.slotChars || !LETTER.test(raw)) {
        return true;
      }
      const f = facts(parent);
      if (!f.shown) {
        return true;
      }
      return add(TEXT, f.inView, f.collapse ? raw.replace(SPACES, ' ') : raw,
                 node, null);
    }

    const root = doc.body;
    const html = doc.documentElement;
    if (root && html && doc.designMode !== 'on' && !optedOut(html)) {
      // The title, shown in the tab: first, as if on screen.
      const title = doc.head ? doc.head.querySelector('title') : null;
      if (title && !optedOut(title) && title.childNodes.length === 1 &&
          title.firstChild.nodeType === Node.TEXT_NODE) {
        const raw = title.firstChild.nodeValue;
        if (raw.length <= caps.slotChars && LETTER.test(raw)) {
          add(TITLE, true, raw.replace(SPACES, ' '), title.firstChild, null);
        }
      }
      // Depth first, in page order; an open shadow root's content comes
      // where it is shown, before the host's own children. The stack holds
      // at most a few nodes per level.
      const stack = [root];
      while (stack.length && open) {
        const node = stack.pop();
        if (node !== root && node.nextSibling) {
          stack.push(node.nextSibling);
        }
        if (++visited > caps.visited) {
          truncated = true;
          break;
        }
        if (node.nodeType === Node.TEXT_NODE) {
          open = readText(node);
          continue;
        }
        if (node.nodeType !== Node.ELEMENT_NODE || !accept(node)) {
          continue;
        }
        if (node.firstChild) {
          stack.push(node.firstChild);
        }
        if (node.shadowRoot && node.shadowRoot.firstChild) {
          stack.push(node.shadowRoot.firstChild);
        }
      }
    }
    g[KEY] = state;
    return {v: 1, id: args.id, truncated: truncated, slots: out};
  }

  function writable(slot) {
    const el = slot.attr ? slot.target : slot.target.parentElement;
    return slot.target.isConnected && el !== null && !el.closest(UNSAFE) &&
        !inEditable(el);
  }

  function apply() {
    const state = g[KEY];
    if (!state || state.id !== args.id) {
      return {v: 1, id: args.id, applied: -1};
    }
    let applied = 0;
    for (const entry of Array.isArray(args.writes) ? args.writes : []) {
      if (!Array.isArray(entry) || entry.length !== 2) {
        continue;
      }
      const index = entry[0];
      const text = entry[1];
      if (!Number.isInteger(index) || index < 0 ||
          index >= state.slots.length || typeof text !== 'string') {
        continue;
      }
      const slot = state.slots[index];
      if (slot.written !== null || read(slot) !== slot.orig ||
          !writable(slot)) {
        continue;
      }
      write(slot, text);
      slot.written = text;
      applied++;
    }
    return {v: 1, id: args.id, applied: applied};
  }

  try {
    switch (args && args.op) {
      case 'extract':
        return extract();
      case 'apply':
        return apply();
      case 'revert':
        return {v: 1, reverted: restore()};
    }
  } catch (e) {
    // Fall through: the browser treats it as an unreadable page.
  }
  return {v: 1, error: true};
}))JS";

}  // namespace molt_ai::page_translate
