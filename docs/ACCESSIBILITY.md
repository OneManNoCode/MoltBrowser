# Accessibility of MoltBrowser's own UI

MoltBrowser's own pages and toolbar controls work with screen readers
(NVDA, JAWS, Narrator, VoiceOver, Orca), with the keyboard alone, and with
the operating system's voice control (Windows Voice Access, macOS Voice
Control). People who browse without touching a mouse or a keyboard depend
on all three (principle 7 of
[TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md)). The target is
WCAG 2.2 AA.

What this covers:

| Surface | Where |
|---|---|
| New Tab (`molt://home`) | `ui/webui/molt_ai/molt_home_ui.cc` |
| AI chat, full page (`molt://ai`) | `molt_ai_ui.cc` |
| AI chat, side panel (`molt://ai-chat`) | `molt_ai_chat_ui.cc`, `molt_ai_chat_ui_js.h` |
| Agent studio, side panel (`molt://ai-agent`) | `molt_ai_agent_ui.cc` |
| Automations (`molt://ai-automation`) | `molt_ai_automation_ui.cc` |
| AI settings (`molt://ai-settings`) | `molt_ai_settings_ui.cc` |
| Memory (`molt://memory`) | `molt_ai_memory_ui.cc` |
| Software update (`molt://ai-update`) | `molt_ai_update_ui.cc` |
| Bookmarks and import popover (`molt://import`) | `molt_import_ui.cc` |
| MoltNet popover (`molt://net`) | `molt_net_ui.cc` |
| Toolbar: AI mode, Agent mode, MoltNet, Bookmarks and import, Software update | `ui/views/toolbar/toolbar_view.cc` |

Paths are under `src/chrome/browser/` (WebUI pages under
`ui/webui/molt_ai/`).

## 1. What is checked automatically

`tools/webui_checks` runs in CI (`.github/workflows/webui-checks.yml`) on
every change to a page, and needs no Chromium checkout:

```bash
cd tools/webui_checks
npm ci
npm test                                  # everything below, and the other WebUI tests
node webui_a11y_test.js                   # accessibility only (about 20 s)
node webui_a11y_test.js molt://ai-chat    # one page
node check_contrast.js                    # colour contrast only
node check_contrast.js molt_net_ui.cc     # one page
```

Run `python3 tools/webui_checks/check_embedded_js.py .` and
`python3 tools/webui_checks/check_no_inline_handlers.py .` too (CLAUDE.md).

**`webui_a11y_test.js`** loads every page the way the smoke test does
(strict CSP, Trusted Types, `chrome.send` answered with canned data) in
jsdom, in its main states: empty (nothing from the browser yet), with
content, and the page's own states (a dialog or sheet open, an error
shown, an answer streamed, an agent step run, a menu opened by keyboard).
In each state:

- **axe-core** (pinned at an exact version in `package.json`) runs every
  rule it can evaluate without layout. A violation fails, and so does an
  "incomplete" (needs review) result. The experimental
  `focus-order-semantics` rule is on.
- **Checks axe doesn't make:**
  - exactly one `<h1>`;
  - everything that takes a click (`data-on-click`, `data-on-*` handlers)
    is a native control, or has an activatable role and a `tabindex`;
  - the words a control shows are in its accessible name (WCAG 2.5.3, so
    a voice control user can say what they see);
  - a visible control is never hidden from screen readers;
  - an open dialog is modal and named, holds focus, and leaves the rest of
    the page inert;
  - the page's live regions say what the page says (the state's own
    checks: an answer is announced once when it is complete, an agent
    step when it runs, an error assertively).
- **Over each page's `<style>`:** a `:focus-visible` style, a
  `prefers-reduced-motion` block that stops animations and transitions, a
  `forced-colors` block, and font sizes in relative units (`rem`/`em`).

**`check_contrast.js`** reads each page's CSS statically, in every theme
it has (dark, light, `prefers-color-scheme`), and checks each text colour
against the background it is set on, or the nearest ancestor's in its
selector, or the page's: 4.5:1 for text, 3:1 for large text. It can't
know every background an element really sits on, so it is a floor; the
build machine checks the rendered pages (§4).

**`markdown_renderer_test.js`** keeps the chat's Markdown output to an
allowlist: the only ARIA it may write is `role="heading"` with
`aria-level` 3 to 5, `aria-hidden="true"` and the "Copy code" label.

## 2. How the pages are built

For anyone changing a page. The tests above enforce most of it.

- **Controls are real.** A `data-on-click` goes on a `<button>` or an
  `<a href>`. A control that can't be one gets a role and `tabindex="0"`;
  `moltA11y` (below) makes Enter and Space work on it.
- **Names.** Icon-only buttons have an `aria-label` that starts with any
  words they show. Every field has a `<label>` (or `aria-label`). Images
  have a text alternative; decoration is `aria-hidden="true"`.
- **Structure.** `<html lang="en">`, a `<title>` that names the page,
  `<header>`/`<nav>`/`<main>` landmarks, one `<h1>` (visually hidden where
  the design has none) and headings in order.
- **`moltA11y`** (`kMoltA11yJs` in `molt_webui_csp.h`, served ahead of
  the page's own script by the pages with dialogs: the two AI chats and
  the agent studio; the smaller pages do the same with a few lines of
  their own):
  - `moltA11y.announce(text)` reads text out through a polite live region;
    `{assertive: true}` for errors. The regions exist from page load (a
    region that appears together with its text is often not read).
  - `moltA11y.openDialog(el, {label | labelledBy, focus, onEscape})` makes
    `el` a modal dialog: role, `aria-modal`, a name, focus moved in, the
    rest of the page `inert`. Escape closes the top dialog.
    `moltA11y.closeDialog(el)` puts focus back where it was.
  - `moltA11y.reducedMotion()` and `moltA11y.scrollBehavior()` for script
    animations and smooth scrolling.

  It writes no HTML and adds no Trusted Types policy; CSP and Trusted
  Types rules are unchanged.
- **Live updates.** A streamed AI answer is announced once, when it is
  complete (at most the first 2,000 characters; the whole answer stays
  readable in the page), never token by token. Agent steps, a run's
  result, a copy, a saved setting are announced politely; errors use
  `role="alert"`. Status text is written only when it changes, so a
  periodic refresh doesn't read the same thing again.
- **Menus and disclosures** are buttons with `aria-expanded`; toggles use
  `aria-pressed` or a checkbox with `role="switch"`; tabs follow the ARIA
  tabs pattern (arrow keys, Home, End). A menu closes on Escape and gives
  focus back to its button.
- **Focus.** Every page has a visible `:focus-visible` ring (light blue on
  dark pages, dark blue on light). Hidden views, closed drawers and sheets
  are out of the tab order (`visibility: hidden` or `inert`), and controls
  that appear on hover appear on focus too.
- **Motion and display.** `prefers-reduced-motion` stops animations
  (including the home page's), transitions and smooth scrolling. A
  `forced-colors` block keeps borders, focus rings and selected states
  visible in high-contrast modes. Text sizes are in `rem`, so the user's
  font size and zoom apply.
- **Colours.** Filled buttons with white text use the `*-fill` tokens
  (4.5:1 and more); `check_contrast.js` checks every new colour pair.

**Toolbar (native views).** Through `GetViewAccessibility()`:

- *AI mode* and *Agent mode* are toggle buttons named "AI mode" and
  "Agent mode" (no emoji), pressed while their side panel is open; the
  tooltip is the description.
- *MoltNet* is named by its state, starting with the word it shows:
  "MoltNet", "Tor", "Tor, exit country Germany" or "Tor, warning"; the
  tooltip, which explains the state, is the description. It opens a
  dialog (`has popup: dialog`).
- *Bookmarks and import* (icon only) is named "Bookmarks and import" and
  opens a dialog; *Software update* is named by its tooltip.

## 3. Manual checks

Run on a release build, on every page in the table above, in the dark and
the light theme. Windows builds are paused (CLAUDE.md), so the Windows
items wait for a Windows build.

### Keyboard (every OS)

- [ ] Tab and Shift+Tab reach every control in reading order, with a
  visible focus ring, and never get stuck; nothing invisible takes focus
  (closed drawer, hidden view, closed sheet).
- [ ] Enter and Space work every button; arrow keys move between tabs
  (import popover) and radio options (agent studio schedule).
- [ ] A dialog, sheet or menu takes focus when it opens, keeps it inside
  while it is open, closes on Escape and gives focus back to what opened
  it. In the AI chat: the chats drawer, the model menu, the more-options
  menu, the first-run welcome.
- [ ] F6 moves between the toolbar, the side panel and the page. In the
  toolbar (Alt+Shift+T on Windows and Linux; F6 or Tab from the address
  bar on macOS), the arrow keys reach AI mode, Agent mode, MoltNet,
  Bookmarks and import, and Software update, and Enter or Space works
  each one.
- [ ] At 200% zoom (Ctrl/Cmd +) and with a 24 px default font size
  (settings, Appearance): no text is cut off or overlaps, nothing needs
  horizontal scrolling in the side panel at its default width, every
  control is still reachable.

### macOS

- [ ] **VoiceOver** (Cmd+F5): each page's title and landmarks are read
  (VO+U rotor: headings, landmarks, form controls); every button and field
  has a sensible name; in the AI chat, a streamed answer is read once when
  it is complete, not word by word; an agent run reads each step; an error
  is read at once.
- [ ] VoiceOver on the toolbar: "AI mode, toggle button" and its state
  ("selected" while the side panel is open), "Agent mode", "MoltNet" or
  "Tor, exit country …", "Bookmarks and import". Accessibility Inspector
  (Xcode) shows the same names and roles.
- [ ] **Voice Control** (System Settings, Accessibility): "Show names"
  labels every control with the words it shows; "Click AI mode", "Click
  Agent mode", "Click MoltNet", "Click Send" work; "Show numbers" numbers
  every control on each page.
- [ ] **Reduce motion** (Accessibility, Display): the home page's
  background, the chat's typing indicator and the progress animations
  stop. **Increase contrast** keeps every control's border visible.

### Linux

- [ ] **Orca** (Super+Alt+S): the same as VoiceOver above (structural
  navigation with H, D, B, F; the answer read once; the toolbar names and
  the toggle's "pressed" state). Accerciser shows the names and roles.
- [ ] GNOME's **Reduce animation** setting (`gtk-enable-animations`
  off) or `--force-prefers-reduced-motion`: animations stop.
- [ ] DevTools, Rendering: "Emulate CSS media feature forced-colors:
  active" and "prefers-color-scheme: light/dark": borders, focus rings and
  selected states stay visible.

### Windows (when Windows builds resume)

- [ ] **NVDA** and **JAWS** (browse mode and focus mode): headings,
  landmarks, form fields; a streamed answer read once; errors read at once;
  dialogs announced with their name; the toolbar's toggle buttons read as
  "pressed" / "not pressed".
- [ ] **Narrator** (Ctrl+Win+Enter): the same, and the scan mode headings
  list.
- [ ] **Voice Access**: "Show labels"/"Show numbers" on every page; "Click
  AI mode", "Click Agent mode", "Click Send" work.
- [ ] **Contrast themes** (Settings, Accessibility): every page stays
  usable; focus rings, selected tabs and modes, and toggles are visible.
- [ ] Inspect.exe or Accessibility Insights: names, roles and states of
  the toolbar buttons.

## 4. Build machine

These need layout or a real browser, which jsdom doesn't have. They are in
[BUILD_MACHINE_CHECKLIST.md](BUILD_MACHINE_CHECKLIST.md) §7.3, tagged
"a11y-webui".

- **axe rules that need layout**, skipped in jsdom (`NEEDS_LAYOUT` in
  `webui_a11y_test.js`): `color-contrast` (WCAG 1.4.3), `link-in-text-block`
  (1.4.1), `target-size` (2.5.8), `scrollable-region-focusable` (2.1.1),
  `label-content-name-mismatch` (2.5.3; approximated in jsdom),
  `css-orientation-lock` (1.3.4). Run axe in the built browser: open each
  page, DevTools console, paste `axe.min.js` from
  `tools/webui_checks/node_modules/axe-core/` (the page's CSP blocks
  loading it as a script; the console runs it), then
  `await axe.run({runOnly: ['wcag2a','wcag2aa','wcag21a','wcag21aa','wcag22aa','best-practice']})`
  and read `violations` (verify the command: the pages' Trusted Types
  rule may refuse parts of axe; Lighthouse doesn't depend on it). Or
  Lighthouse's accessibility audit (DevTools, Lighthouse, "Accessibility"
  only) on the full-page surfaces (`molt://ai`,
  `molt://home`, `molt://ai-settings`, `molt://ai-automation`,
  `molt://memory`). The side-panel pages and the popovers can be opened as
  tabs (`molt://ai-chat`, `molt://ai-agent`, `molt://net`,
  `molt://import`) to run axe on them.
- **Contrast on the rendered pages**, in the dark and light themes, which
  `check_contrast.js` only bounds from below.
- **200% zoom and forced colours** on the rendered pages (§3).
- **Native views**: `chrome://accessibility` (the browser's own tree) and
  the platform inspectors show the toolbar's names, roles and states
  (§3).
- **Screen readers and voice control** on each OS (§3).
