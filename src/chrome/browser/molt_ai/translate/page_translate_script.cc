// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_script.h"

namespace molt_ai::page_translate {

// One function expression, so check_embedded_js.py parses it as a whole
// program; page_translate_io.cc appends "(<JSON arguments>)". Linear in the
// nodes it visits (capped); no regular expression runs on more than one
// text at a time, and none can backtrack. Every DOM member is called
// through its prototype: a form's named controls and a document's named
// elements take precedence over their own members, in every world.
const char kPageTranslateScript[] = R"JS((function moltPageTranslate(args) {
  'use strict';
  const KEY = '__moltPageTranslation';
  const HTML_NS = 'http://www.w3.org/1999/xhtml';
  const TEXT = 0, TITLE = 1, ALT = 2, PLACEHOLDER = 3;
  const ELEMENT_NODE = 1, TEXT_NODE = 3;
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

  // Every DOM member is reached through its prototype, never through the
  // object: a form's named controls (by name or id) take precedence over
  // the form's own members, and a document's named images, forms and
  // embeds over the document's ([LegacyOverrideBuiltIns], in every world,
  // isolated ones too), so `form.closest` or `document.body` can be an
  // element the page chose. Text nodes and the rest go the same way.
  function getter(proto, name) {
    const d = Object.getOwnPropertyDescriptor(proto, name);
    const get = d ? d.get : undefined;
    return get ? (self) => Reflect.apply(get, self, []) : () => undefined;
  }
  function method(proto, name) {
    const fn = proto[name];
    return typeof fn === 'function' ?
        (self, ...rest) => Reflect.apply(fn, self, rest) : null;
  }
  const N = Node.prototype, E = Element.prototype;
  const nodeType = getter(N, 'nodeType');
  const nodeValueOf = Object.getOwnPropertyDescriptor(N, 'nodeValue');
  const getNodeValue = (node) => Reflect.apply(nodeValueOf.get, node, []);
  const setNodeValue = (node, text) =>
      Reflect.apply(nodeValueOf.set, node, [text]);
  const parentNode = getter(N, 'parentNode');
  const parentElement = getter(N, 'parentElement');
  const nextSibling = getter(N, 'nextSibling');
  const firstChild = getter(N, 'firstChild');
  const isConnected = getter(N, 'isConnected');
  const getAttribute = method(E, 'getAttribute');
  const setAttribute = method(E, 'setAttribute');
  const closest = method(E, 'closest');
  const querySelector = method(E, 'querySelector');
  const rectOf = method(E, 'getBoundingClientRect');
  const checkVisibility = method(E, 'checkVisibility');
  const localName = getter(E, 'localName');
  const namespaceURI = getter(E, 'namespaceURI');
  const shadowRoot = getter(E, 'shadowRoot');
  const classList = getter(E, 'classList');
  const hasClass = method(DOMTokenList.prototype, 'contains');
  const isContentEditable = getter(HTMLElement.prototype, 'isContentEditable');
  const inputType = getter(HTMLInputElement.prototype, 'type');
  const shadowHost = getter(ShadowRoot.prototype, 'host');
  const D = Document.prototype;
  const docBody = getter(D, 'body'), docHead = getter(D, 'head');
  const docElement = getter(D, 'documentElement');
  const designMode = getter(D, 'designMode');

  // The job's state: only what an earlier call of this script stored on the
  // isolated world's global, never an element the page named.
  function state() {
    return Object.prototype.hasOwnProperty.call(g, KEY) ? g[KEY] : undefined;
  }

  function read(slot) {
    return slot.attr ? getAttribute(slot.target, slot.attr)
                     : getNodeValue(slot.target);
  }

  function write(slot, text) {
    if (slot.attr) {
      setAttribute(slot.target, slot.attr, text);
    } else {
      setNodeValue(slot.target, text);
    }
  }

  // Puts back what the last job wrote, where the page hasn't changed it
  // since, and forgets the job.
  function restore() {
    const last = state();
    let restored = 0;
    if (last && Array.isArray(last.slots)) {
      for (const slot of last.slots) {
        try {
          if (slot.written === null || read(slot) !== slot.written) {
            continue;
          }
          write(slot, slot.orig);
        } catch (e) {
          continue;
        }
        restored++;
      }
    }
    delete g[KEY];
    return restored;
  }

  function optedOut(el) {
    const translate = getAttribute(el, 'translate');
    return (translate !== null && translate.toLowerCase() === 'no') ||
        hasClass(classList(el), 'notranslate');
  }

  // contenteditable on |el| itself (isContentEditable covers the rest).
  function editableAttribute(el) {
    const value = getAttribute(el, 'contenteditable');
    return value !== null && value.toLowerCase() !== 'false';
  }

  // isContentEditable, which only HTML elements have.
  function editable(el) {
    return namespaceURI(el) === HTML_NS && isContentEditable(el) === true;
  }

  // |el| is in editable content, whatever the page did since it was read.
  function inEditable(el) {
    if (editable(el)) {
      return true;
    }
    const host = closest(el, '[contenteditable]');
    return host !== null && editableAttribute(host);
  }

  // checkVisibility(), where the browser has it.
  function visible(el, visibilityProperty) {
    return checkVisibility === null ||
        (visibilityProperty ? checkVisibility(el, {visibilityProperty: true})
                            : checkVisibility(el));
  }

  function extract() {
    restore();
    const caps = args.caps;
    const job = {id: args.id, slots: []};
    const out = [];
    let total = 0;
    let visited = 0;
    let truncated = false;
    const height = g.innerHeight;
    const width = g.innerWidth;
    const about = new Map();
    const owners = new Map();

    // The nearest ancestor of |el| that isn't display: contents (the one
    // whose box holds |el|'s text), across shadow roots; null if none.
    function boxOwner(el) {
      const chain = [];
      let owner = null;
      for (let node = el;;) {
        if (owners.has(node)) {
          owner = owners.get(node);
          break;
        }
        chain.push(node);
        let up = parentElement(node);
        if (up === null) {
          const parent = parentNode(node);
          up = parent instanceof ShadowRoot ? shadowHost(parent) : null;
        }
        if (up === null) {
          break;
        }
        if (g.getComputedStyle(up).display !== 'contents') {
          owner = up;
          break;
        }
        node = up;
      }
      for (const node of chain) {
        owners.set(node, owner);
      }
      return owner;
    }

    // Layout facts of an element whose text or attribute is read. An
    // element with display: contents has no box of its own
    // (checkVisibility() is false, its rectangle empty): its text is laid
    // out in its box owner's, with the element's own visibility.
    function facts(el) {
      let f = about.get(el);
      if (!f) {
        const style = g.getComputedStyle(el);
        const spaces = style.whiteSpaceCollapse || style.whiteSpace || '';
        let box = el;
        let shown = visible(el, true);
        if (!shown && style.display === 'contents') {
          box = boxOwner(el);
          shown = box !== null && style.visibility === 'visible' &&
              visible(box, false);
        }
        const rect = shown ? rectOf(box) : null;
        f = {
          shown: shown,
          inView: rect !== null && rect.bottom > 0 && rect.right > 0 &&
              rect.top < height && rect.left < width,
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
      if (job.slots.length >= caps.slots ||
          total + text.length > caps.pageChars) {
        truncated = true;
        return false;
      }
      total += text.length;
      job.slots.push({
        target: target,
        attr: attr,
        orig: attr ? getAttribute(target, attr) : getNodeValue(target),
        written: null,
      });
      out.push([kind, inView, text]);
      return true;
    }

    function attribute(el, name, kind) {
      const value = getAttribute(el, name);
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
      if (visible(el, false)) {
        return true;
      }
      // display: contents has no box of its own; its children may.
      return g.getComputedStyle(el).display === 'contents';
    }

    // Whether to look inside |el|. Reads its alt or placeholder on the way.
    let open = true;
    function accept(el) {
      if (namespaceURI(el) !== HTML_NS || optedOut(el) || !rendered(el)) {
        return false;
      }
      const name = localName(el);
      if (name === 'textarea' ||
          (name === 'input' && TEXT_FIELDS.has(inputType(el)))) {
        open = attribute(el, 'placeholder', PLACEHOLDER);
        return false;
      }
      // An editable ancestor was refused already, so its own attribute is
      // enough here.
      if (SKIP.has(name) || editable(el) || editableAttribute(el)) {
        return false;
      }
      if (name === 'img') {
        open = attribute(el, 'alt', ALT);
      }
      return true;
    }

    function readText(node) {
      const raw = getNodeValue(node);
      const parent = parentElement(node);
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

    const root = docBody(doc);
    const html = docElement(doc);
    if (root && html && designMode(doc) !== 'on' && !optedOut(html)) {
      // The title, shown in the tab: first, as if on screen.
      const head = docHead(doc);
      const title = head ? querySelector(head, 'title') : null;
      const text = title ? firstChild(title) : null;
      if (title && !optedOut(title) && text !== null &&
          nextSibling(text) === null && nodeType(text) === TEXT_NODE) {
        const raw = getNodeValue(text);
        if (raw.length <= caps.slotChars && LETTER.test(raw)) {
          add(TITLE, true, raw.replace(SPACES, ' '), text, null);
        }
      }
      // Depth first, in page order; an open shadow root's content comes
      // where it is shown, before the host's own children. The stack holds
      // at most a few nodes per level.
      const stack = [root];
      while (stack.length && open) {
        const node = stack.pop();
        const next = node !== root ? nextSibling(node) : null;
        if (next) {
          stack.push(next);
        }
        if (++visited > caps.visited) {
          truncated = true;
          break;
        }
        const type = nodeType(node);
        if (type === TEXT_NODE) {
          open = readText(node);
          continue;
        }
        if (type !== ELEMENT_NODE || !accept(node)) {
          continue;
        }
        const child = firstChild(node);
        if (child) {
          stack.push(child);
        }
        const shadow = shadowRoot(node);
        const shadowChild = shadow ? firstChild(shadow) : null;
        if (shadowChild) {
          stack.push(shadowChild);
        }
      }
    }
    g[KEY] = job;
    return {v: 1, id: args.id, truncated: truncated, slots: out};
  }

  // A text node is checked from its parent; an attribute from its
  // element's parent, since the element itself may be a textarea, whose
  // placeholder is shown (unlike its text).
  function writable(slot) {
    const parent = parentElement(slot.target);
    return isConnected(slot.target) === true && parent !== null &&
        closest(parent, UNSAFE) === null &&
        !inEditable(slot.attr ? slot.target : parent);
  }

  function apply() {
    const job = state();
    if (!job || job.id !== args.id || !Array.isArray(job.slots)) {
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
          index >= job.slots.length || typeof text !== 'string') {
        continue;
      }
      const slot = job.slots[index];
      // Whatever a slot's checks run into, the others still go, and the
      // count stays exact (so "Show original" knows there is something to
      // undo).
      try {
        if (slot.written !== null || read(slot) !== slot.orig ||
            !writable(slot)) {
          continue;
        }
        write(slot, text);
      } catch (e) {
        continue;
      }
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
