// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The script MoltBrowser runs in a page's Translate isolated world
// (ISOLATED_WORLD_ID_TRANSLATE) to read its text and write translations
// back. Never the page's main world: the page can't see or patch what it
// calls, and its state (the text nodes read, their original values) stays
// in the isolated world. Design: docs/design/PAGE_TRANSLATION.md §4.1.
//
// kPageTranslateScript is one function expression; page_translate_io.h
// calls it with a JSON argument {op, id, caps, writes}:
//  * "extract": restores any earlier translation of this document, then
//    reads the visible text in page order and remembers each slot. Returns
//    {v: 1, id, truncated, slots: [[kind, inViewport, text], ...]}.
//  * "apply": writes [[slot, text], ...] as text (Text.nodeValue, or the
//    alt / placeholder attribute), each only if the slot still holds what
//    was read, is still in the document, and isn't inside a script, style
//    or editable content. Returns {v: 1, id, applied} (-1: no such job).
//  * "revert": puts back every slot that still holds its translation, and
//    forgets the job. Returns {v: 1, reverted}.
// jsdom-tested in tools/webui_checks/page_translate_test.js.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_SCRIPT_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_SCRIPT_H_

namespace molt_ai::page_translate {

extern const char kPageTranslateScript[];

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_SCRIPT_H_
