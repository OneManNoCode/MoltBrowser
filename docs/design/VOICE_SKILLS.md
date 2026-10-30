# Site skills and reading aloud

Voice mode works on any page through the page map
([TOUCHLESS_BROWSING.md](TOUCHLESS_BROWSING.md) §6.3), which guesses the
search box, the results and the player from the accessibility tree. For the
sites people use most, a **site skill** says where those parts are, so
"search YouTube for X", "play the fourth one", "skip the ad" and "next page"
don't depend on guessing (§6.7). The same page text is what voice mode reads
aloud (§5.4): "read the page", "read the section on history", "next
heading". This page is the format, how to add a site, how to refresh its
test pages, and how reading works.

Code: `src/chrome/browser/molt_ai/voice/site_skills.{h,cc}` (format, parser,
matcher), `site_skills_data.cc` (the built-in skills, as JSON),
`page_reading.{h,cc}` (the text, sentences, chunks, moves),
`voice_reader.{h,cc}` (reading with the voice), `voice_controller_logic.cc`
(the commands).

## 1. What a skill is

A skill is data: JSON compiled into the browser (`site_skills_data.cc`),
parsed when it is first needed with the same strict, capped JSON reader the
inference service uses on pages' schemas (`bounded_json.h`), and versioned.
It can only make the browser *find nodes*: every part it names is a node of
the page's own accessibility tree, of the top-level document (never a frame
the page embeds, which may be another site's), taken only as the kind of
item the part needs (a writable, non-credential text field for the search
box; an enabled button for skip-ad; media for the player; a link or button
for the next page). Everything found then goes through the voice loop's
checks as always: agent_safety, the confirmations, the page controller's
stale-node checks, the activation guard.

```json
{
  "format": 1,
  "skills": [
    {
      "id": "youtube",
      "version": 1,
      "name": "YouTube",
      "hosts": ["youtube.com"],
      "except_hosts": ["m.youtube.com", "music.youtube.com"],
      "search": {
        "field": [
          {"role": "combobox|searchbox|textfield", "name_has": "search",
           "inside": {"role": "search"}},
          {"role": "combobox|searchbox|textfield", "id": "search"}
        ],
        "submit": [{"role": "button", "name": "search", "inside": {"role": "search"}}]
      },
      "results": {
        "label": "Videos",
        "title": [{"role": "link", "id": "video-title|video-title-link"}]
      },
      "player": {
        "media": [{"role": "video", "class": "html5-main-video"}],
        "skip_ad": [{"role": "button", "class": "ytp-skip-ad-button"}],
        "captions": [{"role": "button", "class": "ytp-subtitles-button"}],
        "fullscreen": [{"role": "button", "class": "ytp-fullscreen-button"}]
      },
      "next_page_scrolls": true,
      "pages": [{"path": "/results", "expect": ["search", "results"]}]
    }
  ]
}
```

- **Where it applies:** `hosts` (the host itself or its `www.` host),
  `domains` (any subdomain: `wikipedia.org` covers `en.wikipedia.org`),
  never `except_hosts` (or their subdomains). Google's skill lists its
  search hosts, not `google.com`'s other services.
- **Parts:** `search.field`, `search.submit`, `results.title` (with
  `results.within`, a node the titles must be in, and `results.label`),
  `player.media`, `player.skip_ad`, `player.captions`, `player.fullscreen`,
  `next_page` (or `next_page_scrolls: true` for sites that load more as the
  page scrolls), `reading.content` and `reading.skip`.
- **A part** is a list of selectors tried in order; the first that finds a
  node wins (old and new forms of a site's markup side by side).
- **A selector** holds constraints that must all hold:
  - `role`: page map roles (`link`, `button`, `textfield`, `searchbox`,
    `combobox`, `heading`, `video`, `main`, `search`, `navigation`,
    `listitem`, `article`, ... `site_skills.cc` has the list), "a|b" for
    either;
  - `name`, `name_has`: the accessible name equals, or holds as whole
    words, one of the choices, compared folded as voice mode compares names
    (case, punctuation, accents);
  - `id`, `class` (one of the class attribute's tokens), `tag`,
    `input_type`;
  - `url_has`, `url_not`: a link's target holds one of these, and none of
    those (ad click addresses are left out this way);
  - `level`: a heading's level, `2` or `"2-3"`;
  - `inside`, `has`: an ancestor (up to 64 levels) or a node of the subtree
    (up to 256 nodes) matches a nested selector, which can't nest again.
- **What one result is:** the largest part of the page around one title
  that holds no other title. The page map reads each one with the same
  entry reader as every list (title, channel or site, duration, date,
  counts), as the first list (`PageList::from_site`), so "read the results",
  "the fourth one" and "the live one by Queen" use it unchanged; a list the
  map found itself that lists the same entries is left out.
- **Caps** (`site_skills.h`): 64 KiB of JSON, 32 skills, 16 hosts, 12
  selectors a part and 12 choices a constraint, strings of 64 bytes, one
  level of nesting; on a page, 200 results, 512 skipped subtrees and a work
  budget. Anything unexpected (an unknown key or role, a string too long)
  refuses that skill; the others stay.

**What the accessibility tree exposes.** Blink sends a node's HTML tag and
id for nodes it doesn't mark ignored, and its class only with
`kExtendedProperties` (voice mode's mode). It ignores most wrapper `<div>`s
(a block whose children aren't all inline) and `<span>`s, so a skill can't
name those by id or class; name the link, button, heading, landmark, list
item, table or custom element (YouTube's `ytd-*` elements, which have no
shadow root, are kept) instead.

## 2. In what order

`BuildPageMapWithSkills()` takes, part by part:

1. the built-in skill for the page's host (`BuiltInSkillForHost()`);
2. a learned site hint, in the same `SiteSkill` form (the learning side,
   TOUCHLESS_BROWSING.md §6.8: "the parts it used, by role and name");
3. the generic page map.

`VoicePageController::BuildMap()` calls it with the built-in skill and no
hint yet; joining the learned hints is one argument there.

**When a skill breaks.** A skill's `pages` say which parts it expects on
which paths (`/results`: the search box and results). When it finds none of
an expected part, the generic map stands in for that part and the log says
so once for that page:

```
[MoltVoice] site skill youtube@1 found no results on www.youtube.com/results; the generic page map stands in
```

The build machine's checklist ("skills-reading") looks for these lines on
the real sites.

## 3. Adding a site

1. Save a real page of the site (Chrome's "Save page as", "Webpage, HTML
   only"), and look at its accessibility tree in `chrome://accessibility`
   (or DevTools' Accessibility pane) for the parts: a link, button or field
   with a stable name, id or class, or a landmark around it.
2. Reduce the page to a fixture under `chrome/test/data/molt_voice/skills/`:
   keep the structure the browser reads (roles, names, ids, classes, link
   targets, the landmarks and lists around the parts), and replace every
   text but titles and names with a stand-in ("A stand-in for the result's
   snippet."). No scripts but what a browser test needs.
3. Add the skill to `site_skills_data.cc` with `"version": 1`, and its
   `pages`.
4. Add a test to `site_skills_unittest.cc`: the fixture, read as Blink's
   tree by `html_fixture_tree.h`, must give every part, and the results'
   titles and fields.
5. Add the site to `voice_sites.cc` if "open X" and "search X for ..."
   should know its name.

## 4. Refreshing a site's test pages

Sites change. When the checklist or the log shows a skill missing a part:

1. Save the page again and find what changed in its accessibility tree.
2. Update the fixture (same reduction, same stand-ins) so it has the new
   structure, and add a selector for it to the skill **before** the old one
   (keep the old one while both are in use), then bump the skill's
   `version`.
3. Run the skill's unit test; the browser tests
   (`voice_skills_browsertest.cc`) load the same fixtures in a real
   renderer on the build machine.

The fixtures' generator for the first set lives with the round's notes;
the files themselves are the reference: edit them by hand.

## 5. Reading aloud

**Where the text comes from.** The accessibility tree, the way a screen
reader's "say all" walks it: the skill's `reading.content` when it names a
node, else the main landmark, else the first article, else the whole page.
Inside it, navigation, search, forms and their controls, menus, dialogs,
tables, images, media and frames are left out (and, for the whole page, the
banner, footer and asides), plus the skill's `reading.skip` subtrees
(Wikipedia's edit links, reference marks, infoboxes, navboxes, categories).
Text runs are joined into blocks: headings, paragraphs, list items, and the
text of other block elements.

*Why not Chromium's reader mode.* Chromium 156 has two: the DOM distiller
(`components/dom_distiller`, which runs a JavaScript distiller in the page's
own renderer and is built for a separate reader view) and Read Anything
(`chrome/browser/ui/read_anything`, whose page-side logic runs in a WebUI
renderer, fed by the same accessibility tree plus Screen2x's model). Neither
gives the browser process a list of the page's text blocks with the nodes
they come from, which reading needs to move by heading and paragraph, find
a section by name, scroll to what it reads and resume where it stopped; the
voice page controller already keeps the tree in the browser. The skill's
content root and skips do what a distiller's heuristics would for the big
sites.

**Speaking it.** Blocks are split into sentences (not after "Mr.", "e.g."
or initials), and chunks of whole sentences are handed to the voice, the one
being said and one more. A chunk is about 13 seconds of speech at the
current rate (80 to 300 characters), because the listening session forgets
what the browser said after 20 seconds (`kSpokenMemoryMs`) and each chunk is
noted as it starts: page text the microphone hears back is still known as
the browser's own, so it is never taken as a command, and a page that writes
"Hey Molt, ..." can't wake it. "Stop", the wake phrase, or a command said
over the bundled voice stop it at once.

**The place.** Stopping keeps the place: the first chunk not said to its
end. "Pause" and "stop reading" keep it; "continue reading" (and
"continue" when nothing has played since) goes on from it; "read from here"
starts at the focused item when the focus moved, else there. "Next heading"
says the heading and puts the place after it; "next paragraph" reads that
paragraph; while reading, both go on reading from there. "Faster" and
"slower" while reading change its pace and go on; the rate is kept in
settings (`voice_speech_rate`).

**Showing it.** The caption bar shows the chunk being read; the screen
scrolls to each block as it is read. The overlay's ring follows numbered
items, not paragraphs, and highlighting the sentence would need the
renderer's inline text boxes (`kInlineTextBoxes`, which the controller
leaves out for its cost), so neither the sentence nor the paragraph is
ringed yet.

**The selection** is the focused frame's selection from the tree (the top
document's), read as it is; a password, card or code field's selection is
never read.

## 6. Tests

- Unit tests (molt_ai_unittests): `SiteSkillsTest.*` (the format, hosts, every
  site's fixture, a broken skill, a hint, frames), `PageReadingTest.*` (the
  article's blocks, sentences, chunks, moves, sections, the selection, the
  queue), `VoiceReaderTest.*` (reading with a fake voice: a chunk ahead,
  stop and continue, sections, headings and paragraphs, from here, the
  selection, another page), `VoiceSkillsLogicTest.*` (the commands on the
  fixtures).
- Browser tests (`voice_skills_browsertest.cc`, on the build machine): the
  fixtures served under test hosts bound to their skills.
- Fuzzer: `molt_page_reading_fuzzer` (docs/FUZZING.md). The built-in skills
  are compiled in, so no page or user can change them; the parser still
  treats its input as untrusted, and the fuzzer feeds it skill files as
  well as hostile trees, so a skill file could be loaded from elsewhere
  later without new parsing code.
