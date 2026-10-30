// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Test-only: a stored copy of a site's page (chrome/test/data/molt_voice/
// skills/) as the accessibility tree Blink would give the page map, so the
// unit tests run the page map, the site skills and reading on the same
// pages the browser tests load. It reads the small, well-formed HTML the
// fixtures are written in, and maps it as Blink's AXNodeObject does at 156
// for what they use:
//   - roles: the element's role attribute, else its tag (a[href] link,
//     button, input by type, textarea, select, option, h1-h6, ul/ol, li,
//     main, nav, header, footer, aside, form, search, section with a name,
//     article, table, tr, td, th, video, audio, img, p, time); anything
//     else is a generic container;
//   - ignored nodes: a generic container is ignored unless it is a custom
//     element (a '-' in its tag), focusable (tabindex), has a role, or is a
//     block that holds text directly; a span or other inline element is
//     ignored. Ignored nodes keep no tag, id or class, as Blink sends none
//     for them, so a skill can't lean on them here either;
//   - hidden: hidden, aria-hidden="true", style="display:none" and the
//     content of script, style, template and head (but the title) are
//     left out;
//   - names: aria-label, else (links, buttons, headings, list items,
//     options, cells, tabs, menu items) the text inside, else alt (img),
//     title, placeholder; values from value= and a textarea's text;
//   - states: disabled, aria-pressed/checked, aria-expanded, required,
//     readonly, tabindex and the controls' focusability, a password field
//     protected; the focused node is the one with autofocus.
// Bounds are laid out as a column of 24-pixel lines, 1000 pixels wide, so
// the first 33 lines are on screen (800 pixels).
//
// Std-only.

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_HTML_FIXTURE_TREE_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_HTML_FIXTURE_TREE_H_

#include <string_view>

#include "chrome/browser/molt_ai/voice/page_map.h"

namespace molt_ai {
namespace voice {
namespace test {

// |html|'s accessibility tree, as the page map's input (tree 0).
PageTree PageTreeFromHtml(std::string_view html);

}  // namespace test
}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_HTML_FIXTURE_TREE_H_
