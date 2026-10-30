# Touchless browsing: MoltBrowser by voice alone

Status: **proposal**, October 2026. Phase 0 is built, behind `chrome://flags/#molt-voice-mode`, and waits for the build machine (§10); §11 lists the owner's decisions that come before Phase 1.

The goal: a person who can't use a keyboard, mouse, touch screen or any other pointing device can use the whole browser with their voice. They hear what matters read back to them. If they have some sight, they see it large on screen.

## 1. Who it is for

**The person this is designed around:**
- They have lost the use of their arms and hands, so voice is their only input.
- They are partially sighted. Close to the screen they can read large text, and otherwise they rely on hearing.

Output is therefore both:
- **Speech:** what the browser heard, what it is doing, what it found, and the page itself when asked.
- **A screen built for low vision:** large captions, large numbered markers on everything that can be acted on, high contrast and magnification.

The same design serves many others:
- people with ALS, spinal cord injury, cerebral palsy, multiple sclerosis, tremor or repetitive strain injury;
- blind and low-vision people;
- anyone whose hands are busy.

Some of these users have **impaired or atypical speech**, so recognition has to adapt to them, not the other way round (§9, Phase 4).

## 2. What "done" looks like

The owner's example, end to end, with no hand anywhere:

> **User:** "Hey Molt, open YouTube."
> **Browser:** opens youtube.com. *"YouTube is open."*
> **User:** "Search for Bohemian Rhapsody."
> **Browser:** types into the site's search box and submits. *"20 results. 1: Bohemian Rhapsody, official video, Queen, 6 minutes. 2: Bohemian Rhapsody, live at Live Aid, Queen, 5 minutes. 3: ... Say a number or a name."* On screen, each result carries a large number.
> **User:** "Play the live one by Queen." *(or "the fourth one")*
> **Browser:** matches the words against the results' titles and channels and plays the video. *"Playing Bohemian Rhapsody, Live Aid 1985, by Queen."*
> **User:** "Hey Molt, louder." / "Pause." / "Skip ahead one minute." / "Full screen." / "Go back."

The same works when the user starts at google.com ("search Google for ..."), and on sites the browser has never seen before. On those it reads the page through the accessibility tree (§6.3).

**Core tasks** each phase has to cover (§10):
- **Go somewhere:** open a site, search, go back and forward, reload; open, close and switch tabs.
- **Read:** read the page, read headings and links, "what's on the screen?", summarize, find text, read the selection or the item under focus.
- **Choose:** by number, by name, by position ("the last one", "the next one"), with a readback when it's unclear.
- **Act on a page:** press buttons, follow links, fill forms by dictation, check boxes, choose options, scroll, zoom.
- **Media:** play, pause, volume, seek, speed, captions, full screen, next.
- **Run the browser itself:** settings, permission prompts, downloads, dialogs, the AI chat, MoltNet windows.
- **Recover:** "stop", "undo", "cancel", "what can I say?", "where am I?", "start over".

## 3. Principles

1. **Voice in; speech and screen out.** No step ever needs a pointer or a key. That includes the browser's own dialogs, permission prompts, error pages and first-run.
2. **Every state can be left by voice.** "Stop", "cancel" and "go back" always work, and the browser never waits in silence. If it can't do something, it says why and what the user can say instead.
3. **Fast for simple things.** Common commands are recognized by a small, fixed grammar without the language model, and start within about a second. Only open-ended requests go to the on-device planner (§6.2).
4. **The user decides, the browser confirms.** Before anything irreversible (buying, sending, posting, deleting, sharing data), the browser reads back what it will do and waits for a spoken "yes".
5. **Nothing leaves the device.** Wake phrase, recognition, voices and AI all run locally in sandboxed processes, like the rest of MoltBrowser's AI. No audio is stored unless the user asks.
6. **Pages can't give commands.** Audio a page plays, or text a page shows, is never taken as the user's instruction (§7).
7. **Works with the OS's assistive technology.** MoltBrowser's own UI stays fully accessible to screen readers and to OS voice control (Windows Voice Access, macOS Voice Control), for users who already rely on them.

## 4. What MoltBrowser has today

| Piece | Where | What it gives, and what's missing for touchless use |
|---|---|---|
| Speech to text | `molt_ai/voice/voice_service.*`: whisper.cpp's `whisper-cli` (bundled `ggml-tiny.en`), sandboxed (`helper_sandbox.h`) | Push-to-talk in the AI chat only, as one 30 s recording per request. No wake phrase, no streaming, English only. A process start per utterance is too slow for a conversation. |
| On-device language model | `chrome/services/molt_ai/inference` (llama.cpp, sandboxed), constrained JSON decoding | Turns open-ended requests into structured actions. |
| The web agent | `molt_ai/agents/`: element snapshot (40 labelled elements, `e1`...), actions (navigate, search, click, type, fill form, scroll, site tools), planner, vision (masked screenshots), site tools (WebMCP), `agent_safety` | The action vocabulary and its safety checks carry over. It acts through page scripts in an isolated world (`el.click()`), which don't count as a user gesture: a video won't start with sound, and full screen and some buttons refuse. |
| Real-click gates | `molt_ai/common/molt_user_click.h` | Trust changes, the agent's Allow and MoltNet settings need a physical click. A voice user can't give one: these need a voice equivalent that is just as hard to forge (§7.3). |
| MoltShield | `molt_ai/shield` | Removes consent banners, overlays and ads, so pages are simpler to read aloud and to choose from. |
| Chromium 156 | `chrome/services/speech/` (`SpeechRecognitionServiceImpl`, `AudioSourceFetcherImpl`); `content::TtsController` with `TtsEngineDelegate`; `WebContents::RequestAXTreeSnapshot()`; Blink's accessibility actions | The speech recognition service is the right home for an open-source recognizer: today it runs Google's closed SODA engine, which MoltBrowser doesn't ship. Text-to-speech can take a built-in engine of ours. The accessibility tree exposes every page's structure. An accessibility "click" counts as user activation (`AXObject::OnNativeClickAction()` calls `NotifyUserActivation()`), as it does for OS assistive technology. |

## 5. How it feels to use

### 5.1 Listening

- **States:** *off*, *waiting* (listening only for the wake phrase), *listening* (a command is being spoken), *thinking*, *speaking*. Each has a distinct sound (an "earcon") and a large on-screen indicator.
- **Wake phrase** (default "Hey Molt"; the user can choose another). It works while a video plays: the browser removes its own sound from the microphone signal (echo cancellation with what the browser plays as the reference, §6.1), and ducks media volume while listening.
- **Follow-up window:** after the browser answers, it keeps listening for about 8 s without the wake phrase, and as long as it is waiting for an answer to its own question. This makes "the fourth one" or "yes" natural.
- **Always-listening mode**, for users alone at home: every utterance is a command. It is safer with speaker verification turned on (§7).
- **Barge-in:** the user can interrupt the browser's speech ("stop", "next", or a new command).
- **End of speech:** voice activity detection decides when the user has finished, adjusted for slower speakers.

### 5.2 What the user can say

Five layers, tried in this order:

1. **Direct commands** in a fixed grammar, with no language model:
   - go to, search, back, forward, reload;
   - new, close and switch tabs;
   - scroll, zoom;
   - play, pause, louder, quieter, mute, skip, full screen;
   - read, next heading, next link;
   - stop, help, what can I say.
   Many phrasings are accepted ("open", "go to", "take me to").
2. **Choosing** from what is on screen:
   - by number: "the fourth one", "number 12", "click 7";
   - by name: "the live one by Queen", "the sign-in button";
   - by position: "the first video", "the last link", "the one below".
3. **Dictation** into the focused field: "type ...", "new line", "delete the last word", "read it back", "submit". Punctuation is spoken or automatic.
4. **Tasks** in plain language, handled by the planner: "find a train from Paris to Lyon tomorrow morning", "reply to the last email from Ana saying I'll be late". Each step is spoken, with confirmation before anything irreversible.
5. **Questions about the page:** "what does this page say about prices?", "summarize this article", "what's in this picture?" (the on-device vision model).

"What can I say?" lists what fits the current page.

**Implementation note (Phase 0, round 37): the voice loop.** `molt_ai/voice/voice_controller_logic.h` decides, `voice_controller.h` does it in a window (§6.2, §6.5, §6.6). What each layer does today:
1. **Direct commands** become steps: browser commands (back, forward, reload, home, zoom, full screen, tabs), the media session (pause, resume, seek, next), the tab's mute, accessibility actions on the page (press, focus, set text, submit, scroll, a volume slider's increment and decrement). A command that needs something the page lacks says so and what to say instead ("I can't find a volume control on this page. Say mute or unmute.").
   - **Sites** (`voice_sites.h`): a small built-in table (YouTube, Google, DuckDuckGo, Wikipedia, Brave Search, Bing) with search pages for "search YouTube for ...". A spoken address becomes `https://` and the address, only when it reads as a host name. Any other name is searched for with the default search engine, and the browser says so: it never makes up a domain. Every URL passes `agent_safety::IsAllowedNavigationURL()` before it loads.
   - **"Search for X"** types into the page's own search field (a search box, `type=search`, or a field whose name says "search") and submits it; without one, the known site's search page, else the default engine.
2. **Choosing** (`voice_candidates.h`): from the list the browser read last, else the page's main list, else every numbered item. "The fourth one" is a position; a number is the position the browser read out, or, while numbers are shown, the number on screen (the list is then read with those numbers, so what is heard is what is seen). When two or more fit, the close ones are read back, numbered 1, 2, 3, and the answer picks among them ("the Live Aid one", "two"), or from the whole list if it names another.
3. **Dictation** goes to the field the user chose (or the focused one, or the page's only one): "type" adds to its text, "delete the last word", "clear", "read it back", "submit" (a search field submits at once; any other form asks first). Never in a password, card or code field.
4. **Tasks** and anything else the grammar leaves `kUnknown` reach a planner hook (`VoiceController::SetPlannerHook()`), empty until the planner comes; the browser says it can't do that yet. Routines say they come later.
5. **Questions about the page:** not yet.

"What can I say?" reads examples from the grammar's own table (`CommandInfos()`) that fit the page: choosing when there is a list, media when there is a player, dictation in a field, yes and no while a question waits.

### 5.3 Numbers on everything

"Show numbers" (or on by default in voice mode) puts a large number on every link, button, field, result and video, as Windows Voice Access and macOS Voice Control do. The browser also groups the page's repeated items: search results, videos, products, emails, comments. It reads them as one numbered list, each item with its title and the parts that tell them apart (channel, author, duration, price, date).

**Implementation note (round 37, built; not switched on yet):** `chrome/browser/ui/views/molt_ai/voice/`. The voice controller calls `VoiceOverlay` (`voice_overlay.h`, one per tab): `ShowNumbers(map)`, `HideNumbers()`, `SetCurrentItem(map, number)`, and a callback when the numbers hide by themselves.
- **Where they are drawn:** a view laid over the page's view in each contents area (`ContentsContainerView`, the split-view slot, which already lays the enterprise watermark, the capture border and the Glic overlays over the page; small hooks marked `MoltBrowser:`). It paints to its own layer, takes no events or focus, and is invisible to assistive technology. A child widget, as the find bar uses, would add a window to activate and an accessibility root for nothing: the overlay is never interactive.
- **Coordinates:** the page map's bounds are Blink's physical pixels relative to the page's viewport, zoom and frame offsets included; they become DIPs by the screen's scale factor, from the page view's origin. On macOS the trees leave out pinch zoom, so no numbers are drawn while a page is pinch-zoomed there.
- **Placing badges** (`voice_overlay_layout.h`, pure and unit-tested): a badge goes on the item's top-left corner, else beside it (left, above, the other corners, right, below, centred), clamped inside the viewport and next to its item, never over another badge. Where none fits, it shrinks in two steps to a floor (14 DIPs of text at the default size). If that leaves items without a badge, the badges are placed again with the items closer together than a badge starting smaller, and the plan that numbers more items is kept. An item with no room even then gets no badge, and the plan counts it, so the controller can suggest zooming in. The current item is placed first. At most 500 badges.
- **Following the page:** the page controller applies Blink's location and scroll changes to its trees, so bounds stay current as the page scrolls, and tells subscribers about every change. While numbers are shown, the overlay maps the page again at most ten times a second; if every number still names the same item (items added after the last are fine), the badges move with them, else they hide and the callback says the page changed. They also hide on navigation (a jump to a fragment aside) and when the tab is hidden.
- **Readability:** black on yellow badges with a black border, at least 7:1; a thick yellow ring between black lines around the current item; the OS's colours in forced-colours mode; sizes from the user's text settings (`docs/ACCESSIBILITY.md` §5).

### 5.4 Speaking back

- **Verbosity levels:**
  - *brief*: confirmations and counts;
  - *normal*: plus the top items of a list;
  - *full*: the page read like a screen reader.
- **Rate and voice** are adjustable by voice ("speak faster").
- **Secrets are never read aloud:** passwords, one-time codes and card numbers ("password field, filled from your vault").
- **Private content** (mail, messages) is read aloud only after asking whether others can hear, unless headphones are connected.

### 5.5 The screen, for low vision

- **A caption bar** in large type:
  - what was heard ("Heard: play the live one by Queen");
  - what is happening ("Playing: ...");
  - the state.
- **Large number markers**, and a thick focus ring around the current item.
- **Magnification** that follows the item being read or acted on: "zoom in", "zoom out", "bigger text".
- **High-contrast and dark themes**, and Chromium's forced colours.
- **Reading mode:** the article alone, in large text, read aloud with the sentence highlighted.

**Implementation note (round 37, built; not switched on yet):** `VoiceOverlay::SetCaption(state, heard, status)` and `HideCaption()` (or the same for a window), `voice_caption_bar.h`.
- **Where:** a bar under the page in each contents area; the window's caption shows in the area with the active tab and stays across tab switches. It takes its height from the page area, as the new tab page's footer does, so it covers nothing; a tab-modal dialog's scrim doesn't cover it.
- **Fixed height:** two lines (the state with what was heard; what is happening), whatever they say. The page is resized only when the bar is shown or hidden, never per utterance, so a page (in MoltNet too) sees no resize tied to what the user says. It takes at most a third of the page area; its text shrinks to fit, down to a floor.
- **Text:** what was heard and the status pass `SpeechSafeText()` and are capped at 200 characters; long lines end with an ellipsis.
- **Assistive technology:** a polite live region named "Voice mode" (role status) with the state as its description; never focused, so the OS magnifier stays with the page.
- **Not yet:** magnification that follows the item being read, and reading mode.

### 5.6 When things go wrong

- **Unclear choices:** the browser asks ("Did you mean the official video or the live one?"), with the candidates numbered.
- **Misheard words:** "no, I said ..." corrects the last request, and "undo" reverses the last action where the browser can.
- **Failures:** something that fails is said plainly, with a next step ("The site wants you to prove you're human. I can switch to its audio challenge and play it; you say the answer.").
- **Errors and crashes:** the browser reopens what was open and says so. In voice mode it never leaves the user in a silent state.

## 6. Architecture

```
 microphone ──► Speech service (sandboxed utility process)
                 echo cancellation (reference: what the browser plays)
                 voice activity detection ─► wake phrase ─► streaming recognition
                 [optional] speaker verification
                       │ text + timing + confidence
                       ▼
 Browser process:  Voice controller
                   ├─ Intent layer: direct-command grammar ──► action
                   │                 └─ otherwise: on-device planner (constrained JSON)
                   ├─ Page map: accessibility trees (page + browser UI) → numbered items
                   ├─ Matcher: spoken reference → item (ordinal, text, sound-alike, model)
                   ├─ Actions: accessibility actions (click, focus, set value, scroll),
                   │           browser commands, media session
                   ├─ Safety: agent_safety, confirmations, voice gesture (§7)
                   └─ Output: TTS (TtsController + MoltBrowser voice) and the overlay
```

### 6.1 Speech service

- **Process:** a sandboxed utility process.
- **Chromium's speech service:** `chrome/services/speech/` already captures the microphone inside the service (`AudioSourceFetcherImpl`) and streams results over Mojo. MoltBrowser would add an open-source recognizer next to the SODA one, which it never ships.
- **Pipeline:**
  1. capture at 16 kHz;
  2. echo cancellation (WebRTC's, already in MoltBrowser), with the browser's own output as the reference;
  3. voice activity detection;
  4. wake-phrase spotting;
  5. streaming recognition with partial results (shown live in the caption bar);
  6. final text with per-word confidence.
- **Speaker verification** (optional): an on-device voice embedding enrolled from about 30 s of speech, which rejects other voices.
- **Recognition biased toward the screen:** the names in the current page map (titles, channels, button labels) and the personal vocabulary (§6.8) are passed to the recognizer as context words. That way "Bohemian Rhapsody" is recognized as written when it is on screen.
- **Engine:** see §9 for the choice. Model files reach the service as read-only handles, like the inference service's.
- **A possible bonus:** the same recognizer could answer the Web Speech API (`SpeechRecognition`) for web pages locally. Today that API is unavailable or Google-backed. In MoltNet it would first need its own fingerprinting review, since a page could time the recognizer or compare its results.

**Implementation note (round 38): the speech helper.** The engine side of the pipeline above is `molt-speech`, a resident helper process the browser starts once per voice session (`chrome/browser/molt_ai/voice/speech_helper/`; the protocol between them is [SPEECH_HELPER_PROTOCOL.md](SPEECH_HELPER_PROTOCOL.md)). The browser side (the microphone, echo cancellation, starting and watching the helper, the listening states) is the voice session's.
- **A helper, not a utility service, for now:** sherpa-onnx and ONNX Runtime are big CMake projects built with exceptions, and sherpa-onnx ends its process on many errors ([VOICE_BENCH.md](../VOICE_BENCH.md) §5.3). A helper keeps all of that out of Chromium's `-fno-exceptions` code and reuses the helper sandbox and its limits. A later round may move it into a GN-built utility service; its std-only core (the frames, CONFIG, the listening states) is GN code already.
- **Engines,** as VOICE_BENCH.md §6 recommends: the GigaSpeech keyword spotter for the wake phrase (threshold 0.15, the phrase cut into the spotter's word pieces as its own SentencePiece model cuts it), Silero VAD for the end of speech (0.5 s by default), and Kroko EN for streaming recognition, with modified beam search so the page's names (sent as hotwords, cleaned again in the helper) bias it. Everything runs on the CPU, one thread each by default, with ONNX Runtime built so its threads never spin-wait.
- **Listening:** the spotter and the VAD run all the time; the recognizer only while someone speaks after a wake phrase (or in every utterance when the browser turns the wake phrase off, for the follow-up window and always-listening mode). The recognizer hears the wake phrase too, so "Hey Molt, open YouTube" in one breath works; the grammar drops the phrase as it does from typed commands.
- **Built** from pinned source tarballs by `scripts/bundle-speech.sh` (one static executable per OS) and bundled with its three models (about 80 MB) under `speech/`, opt-in (`MOLT_BUNDLE_SPEECH=1`) until voice listening ships.
- **Measured here** (a shared 4-core server, synthetic voices; SPEECH_HELPER_PROTOCOL.md §8): the final text 0.63 s after the user stops speaking (0.5 s of it the silence the VAD waits for) and the first words 1.1 s after they start; recognition as accurate as the bench's own decoder (11.8% WER, 68% of commands exactly right; with the page's names as hotwords, 75% of the names on screen right against 70%); the wake phrase heard in 73 to 77 of 100 clips, with no false wake-up in an hour of chatter, commands and page audio; about 4% of one core while waiting for the wake phrase and 6% while recognizing everything, under 200 MB; every malformed message refused, also inside the Linux helper sandbox. Measuring it found the VAD's end of speech drifting later over a session; it now starts afresh after each stretch of speech.
- **Not yet:** the microphone and echo cancellation (the voice session), speaker verification, a per-user wake threshold, models handed over as read-only handles (the helper reads its bundled, read-only model directory by path; §5.1 of the bench has what a handle-based version needs), and builds on macOS and Windows (scripted, not run yet).

### 6.2 Intent layer

1. **The direct-command grammar first:** a small parser, deterministic and unit-tested, with synonyms and number words. It handles most commands in milliseconds.
2. **Choosing:** when the utterance is a choice, the matcher (§6.4) resolves it against the current page map.
3. **Everything else goes to the planner:** the existing on-device model with constrained JSON output, given the page map and the conversation so far. It returns one action or a short plan, which is spoken and shown before it runs.
4. **Context:** the last list the browser read, the last item chosen, and the current field. This lets "the next one", "play it again" and "read it back" work.

**Implementation note (Phase 0, built):** `molt_ai/voice/voice_command.h`, `ParseVoiceCommand(utterance, ParseContext)`, std-only and unit-tested.
- **Patterns, per language:** `voice_command_phrases_en.cc` holds ordered patterns (words, alternatives, optional groups and typed slots: a number, a length of time, a volume, a site, a choice, free text). They are matched left to right without backtracking, and the first that takes the whole utterance wins. `spoken_phrases_en.cc` holds the words: numbers, fillers, politeness, wake greetings, address words. A new language adds two files with the same tables. `docs/VOICE_COMMANDS.md` is generated from these tables, and its examples are checked to parse.
- **Clean-up:** the wake phrase (with sound-alikes), fillers ("um") and politeness are dropped. "No. 4", "#4" and "50%" become words. Over 1,000 bytes, or invalid UTF-8, is refused, never cut down.
- **Context:** "yes" and "no" count only after a yes/no question, the dictation edits only in a focused field, and a bare number or short description only right after a list. Everything else goes to the planner as `kUnknown`, with the text as heard.
- **Trust stays with the browser:** a site comes out as host-like text ("youtube", "example.com"), never a URL. The browser resolves it and checks it with `agent_safety`. A choice is resolved by the matcher (§6.4), and an action still passes §7's confirmations.
- **Not yet:** spoken punctuation in dictation, a site's name on its own ("YouTube" alone), and other languages.

**Implementation note (round 37): from command to action.** `PlanUtterance(text, map, environment, state)` in `voice_controller_logic.h` is std-only and unit-tested on hand-built pages; it returns the steps to run, the words to say and the next conversation state.
- **Context** (`ContextFor()`): a yes/no question is pending only on the page it was asked on and for 30 s; a list holds only on its page (the controller changes the page id at every committed navigation and every tab switch); a field is being typed into only on its page.
- **The conversation state:** the last list read (which list, to find again in a newer map of the same page), the choices offered after an unclear one and the verb to apply, the current entry for "the next one", a pending confirmation and the steps it holds, the field typed into, the heading and link cursors, numbers shown, verbosity and speech rate, and what was said last for "repeat".
- **Steps after a page loads:** a navigation or submit is followed by a wait; the controller resumes when the navigation has committed, the page has stopped loading and its accessibility tree has been quiet for 0.5 s (a page whose tree never rests is read 4 s after the commit; at most 15 s in all), then plans the follow-up: "YouTube is open", the results read, the page's title and its list, or the video of a page opened to play. A site that plays what it opens (YouTube does) gets 1.5 s to start before the browser clicks the video, which would pause it.
- **Confirmations:** pressing anything whose name says buy, purchase, pay, order, checkout, delete, remove, send, post, publish, reply, comment, submit, confirm, subscribe, unsubscribe, donate, transfer, sign out or log out, a link to a sign-out address, anything on a page with a payment-card field (agent_safety treats card fields as sensitive), and submitting a form that isn't a search. The browser says "Say yes to press Buy now, or no to cancel." and runs the held steps only on a "yes" in that context; any other command drops the question.
- **Page text is read, never obeyed:** it reaches only the spoken words and the matcher, after `SpeechSafeText()`. The steps come from the grammar's command and the map's structure; a page's "say yes to buy" is read as the entry it is.
- **MoltNet:** voice mode doesn't start in a MoltNet window (§7.4): no accessibility mode is taken, and a command only hears "Voice mode is off in MoltNet windows."

### 6.3 Page map

- **Source:** `WebContents::RequestAXTreeSnapshot()` gives the page's accessibility tree: roles, names, states, values, headings, landmarks, links and media, across frames and shadow DOM. This is the same structure screen readers use.
- **Items:** the map numbers what can be acted on and detects repeated structures (lists of results, cards, rows) to read as one list with their distinguishing fields.
- **The browser UI too:** the views have accessibility trees, so the browser's own bubbles, prompts, menus and settings get numbers and names.
- **Fallback:** where the accessibility tree is poor (canvas apps, unlabeled controls), the vision model describes the screenshot (masked as today), and the existing element snapshot fills gaps.
- **Shared with the agent:** this replaces its 40-element snapshot with a richer, more robust one.

**Implementation (round 36, Phase 0).** In `chrome/browser/molt_ai/voice/`:
- **Source:** live trees rather than snapshots. `VoicePageController` (`voice_page_controller.*`, one per tab) holds a scoped accessibility mode on its tab while voice mode is on, and keeps each frame's `ui::AXTree` from `AccessibilityEventReceived()` and, since round 37, the location and scroll changes Blink sends apart (`AccessibilityLocationChangesReceived()`), in the browser process. Actions need the renderer's accessibility to stay on anyway, and a snapshot's node ids would not be the live ones. The mode is `kWebContents | kExtendedProperties`: the second adds checked states, placeholders, heading levels, list positions, HTML tags, autocomplete and default actions. It has no `kNativeAPIs` (no platform tree), `kInlineTextBoxes`, `kHTML` or `kScreenReader`. The reasons are in the controller's `.cc`.
- **Joining frames:** `ax_page_source.*` walks the top-level tree and joins each frame's tree under its host (child tree ids), each tree at most once. Only the tab's own primary-page frames are looked up.
- **The map:** `page_map.*` is std-only, over a flat node list.
  - **Items:** everything that can be acted on and isn't hidden is numbered in reading order, on screen or not, so scrolling never changes a number. Off-screen items are marked; the overlay will number only the visible ones.
  - **Lists:** semantic lists, feeds, list boxes and table rows, and repeated structures. A repeated structure is a container whose children (at least 3, not inline text, each holding an action) have similar shapes: the roles and depths of their content, ignoring wrapper divs and custom elements, and how many links, headings and buttons they hold.
  - **Entries:** each has a primary action (the link in or around its heading), a title (that link's visible text, not a long `aria-label`), a channel or site, and typed fields: duration, counts and date (`page_map_text.*`, English for now).
  - **Ranking:** lists are ranked by prominence (entries, their size, titles, fields and headings, the main landmark; navigation counts against), so "the results" is `lists[0]`.
  - **Text and secrets:** all text is cleaned for speech (no control, bidi or invisible characters, capped). Credential fields are marked (agent_safety's classification plus their labels), and their values are never kept.
  - **Bounds:** the work is capped (20,000 nodes, depth 256, a work budget).
  - **Tests:** unit tests with hand-built YouTube-, Google- and Wikipedia-like trees; `molt_page_map_fuzzer`.
- **Not yet:** the browser UI's own tree, speech, and the agent acting through `PageNodeHandle`s instead of its isolated-world scripts. The numbers overlay (round 37) draws `PageItem::bounds` (§5.3).

### 6.4 Matching what was said to what is on screen

- **Ordinals and numbers first:** "fourth", "number four", "4", "the last".
- **Then names:**
  - normalized text matching of the words against each item's title and its distinguishing fields;
  - a phonetic comparison, so recognition errors still match ("Bohemian rap city" → "Bohemian Rhapsody");
  - artist or channel cues ("by Queen").
- **The planner as last resort:** for descriptions ("the live one", "the shortest", "the one from 1985"), the on-device model ranks the candidates.
- **Thresholds:** below a confidence threshold, or when two items are close, the browser asks.
- **Tests:** every scorer is a pure function with tests built from real result lists and real recognition errors.

**Implementation note (Phase 0, built):** `molt_ai/voice/spoken_choice.h`, `MatchSpokenChoice(ChoiceRef, candidates, MatchOptions)`, std-only and unit-tested. The page map fills `ChoiceCandidate`s: position, on-screen number, a coarse role, the title and typed fields (author, source, date, duration, counts, price, details). The header documents the role mapping from the accessibility tree.
- **References:** a position ("the fourth one", "the second to last video"), a number ("number 12": the number shown on screen, else the position), a relation to the current item ("the next one"), or a description with "by" and "from" cues ("the live one by Queen"). A description can also carry an ordinal ("the second live one"). Role words narrow the items only when some item has that role.
- **Scoring:** words are folded (case, punctuation, Latin accents) and compared exactly (1.0), as plurals (0.95), or by sound with MoltBrowser's own phonetic key (`phonetic_key.h`: 13 sound classes, a banded edit distance weighted for close sounds, at most 0.9). Words are also joined across two or three words on either side, for a recognizer's splits and merges ("rap city" / "rhapsody"). Each word is weighed by how few items it matches: a word every item shares counts 0.25, one no item has 0.5, and a "by" word at least 0.6. A sound-alike counts 0.6 of its score when another item has the word as spelled, because the recognizer is biased toward on-screen words (§6.1).
- **Outcome:** below 0.5 nothing matches. A lead of 0.15 over the next item is unique. Otherwise the items within 0.15 are ambiguous (at most five, read back to the user). The ranked items and their scores always come with the outcome. Descriptions about what items don't say ("the shortest one") score low or tie, and go to the user or the planner.
- **Bounds:** at most 1,000 items, 12 fields, a 1 KiB title and 256-byte fields, and 12 description words. 500 items of about 300 bytes are matched in about 4.4 ms on one desktop x86-64 core (target: 5 ms); a page of 20 results takes well under a millisecond. Non-Latin text only matches exactly.

### 6.5 Actions

- **On pages:** accessibility actions (default action/click, focus, set value, scroll into view, increment and decrement). Blink treats these as user interaction, as for OS assistive technology, so media starts with sound, full screen works, and pages see a trusted click.
- **Text entry:** set-value or the input method path, with the page's input events.
- **Browser commands:** Chromium's command IDs (back, reload, new tab, zoom, find) and the media session for playback control.
- **Safety:** every action still passes `agent_safety` (http(s) only, credential fields, untrusted-text fencing) and §7's confirmations.

**Implementation (round 36, Phase 0).** `VoicePageController` acts on the map's `PageNodeHandle`s through `RenderFrameHost::AccessibilityPerformAction()`:
- **Press:** `kDoDefault`. Blink's `AXObject::OnNativeClickAction()` notifies user activation, then dispatches the element's pointer, mouse and click events. The browser accepts the activation, so unmuted media and full screen are allowed afterwards.
- **Focus** and **scroll into view:** `kFocus` and `kScrollToMakeVisible`.
- **Set text:** `kSetValue` (`AXNodeObject::OnNativeSetValueAction()`), which fires the `input` and `change` events forms listen to. It is refused for protected and credential fields, read-only, disabled and non-text fields. The text is cleaned first (`TypedText()`).
- **Submit:** presses the form's submit input when there is one. Otherwise it focuses the field, waits until its frame reports the focus there (key events travel apart from accessibility actions), and sends Enter to the frame's widget, as the keyboard does.
- **Media:**
  - **Play:** the element's default action. The page's player starts with sound: the click carries user activation. The click goes to the `<video>` itself, so this works where the site's player plays on a click (YouTube's does). A bare `<video controls>` doesn't play from it, because Blink's controls see only clicks on their own buttons: press the controls' play button, which is on the map as a button inside the video. Chromium's accessibility media actions (`kResumeMedia`, `kSuspendMedia`) do nothing in web pages.
  - **Pause and resume:** the tab's media session, as the media keys do. Resume plays as triggered by the user, so the sound stays on.
- **Stale targets:** before each action the handle is found again. Its frame must be this tab's, active, in the primary page and an http(s) document, and the node must be in that frame's current tree. A handle from an earlier document is refused.
- **Turning it off:** `Stop()` releases the mode and drops the trees.
- **Tests:** the browser tests (`voice_page_controller_browsertest.cc`, pages in `chrome/test/data/molt_voice/`) drive a video site from search to sound, a search engine's results, a sign-in form and a cross-site frame through the controller alone. They run on the build machine.

### 6.6 Speaking and showing

- **Voices:**
  - Phase 1 uses `content::TtsController` with the platform's voices (Windows SAPI/OneCore, macOS, speech-dispatcher on Linux).
  - Phase 2 adds a bundled neural voice in the speech service, registered through `TtsEngineDelegate`, so it sounds the same on every OS.
  - In MoltNet the voice list pages see through `speechSynthesis.getVoices()` is then the same everywhere, which closes a fingerprinting surface.
- **The overlay:** MoltBrowser views above the page (the numbers and the focus ring) and below it (the caption bar). They are drawn by the browser, so pages can't read or fake them (§5.3, §5.5).

**Implementation note (Phase 0, round 37): spoken answers.** `molt_ai/voice/voice_speech.h` speaks through `content::TtsController` with the platform's voices, at the user's rate ("speak faster" and "slower" step it by 1.25, from 0.5 to 2.5).
- **Interruptible:** a new command or "stop" silences voice mode's speech, current and queued (it leaves a page's own speech alone when voice mode isn't speaking); each answer interrupts whatever was being said.
- **Utterances belong to the profile**, not the tab, so a navigation doesn't cut "YouTube is open" short. Long readings are spoken in chunks of at most 1,200 characters, so stopping is quick.
- **Verbosity:** brief reads a list's count, normal the first five results (ten headings or links), full all of them with their dates, views and details.
- **Platforms:** Windows and macOS need nothing. On Linux, Chromium's speech needs speech-dispatcher (`libspeechd.so.2`) and the `--enable-speech-dispatcher` switch; without them voice mode is silent and only the caption shows what it says. Whether MoltBrowser's Linux launcher should pass the switch is the owner's call: it also gives pages `speechSynthesis` voices, a fingerprinting surface in MoltNet (see Voices above).
- **Showing:** `VoiceController::Observer` gives the overlay the caption (state, what was heard, what is happening), what is said, "show numbers" and the item acted on; the agent panel's voice field shows the same as text.
- **Typed commands until the speech service:** with `chrome://flags/#molt-voice-mode` on, the Agent mode panel (`chrome://molt-ai-agent`) has a "Type a voice command" field for keyboard users and the build machine. It sends what was typed to its window's controller (at most 1,000 bytes, checked in the handler); the answer is spoken and listed as text.

### 6.7 Site skills

- **Built in:** for the sites people use most (YouTube and other video sites, the major search engines, Wikipedia, webmail, news, maps), declarative "skills" name the search box, the result list, the player and the next page, so these flows don't depend on guessing.
- **WebMCP:** where a site offers WebMCP tools, they are used as the agent already does.
- **When a site changes:** skills are data, versioned and tested against stored copies of the pages. A broken skill falls back to the generic page map.

### 6.8 Learning from use
The navigator gets better for each user from what they say and what worked, all on the device:
- **Routines taught by voice.** After a task succeeds, "remember this as *my news*" saves it as a workflow in the automation engine's format, under its trust policy. "Hey Molt, my news" then replays it without the language model. The agent can already save a run as a draft workflow; voice mode adds naming and starting it by voice.
- **Experience as examples.** For a new request, the planner is shown the most similar saved routines for the site as worked examples. This follows the published Agent Workflow Memory method, which reports large gains on web-task benchmarks.
- **Personal words.** Names the user says often (artists, contacts, sites) and their corrections ("no, I said ...") are kept as a personal vocabulary. The recognizer is biased toward them, and toward the names on the current page (§6.1).
- **Site hints.** When the generic page map works on a site, the parts it used (search box, result list, player) are remembered for that site. A hint that fails is dropped.
- **The user's own voice** (Phase 4): adaptation to impaired or atypical speech from samples the user chooses to record.

Everything learned is listed in settings, can be deleted by voice, and stays on the device. Nothing is learned in MoltNet windows.

## 7. Trust and safety for voice

Voice is a new way into the browser, so it gets the same care as clicks.

### 7.1 Threats
- **Page audio:** a page, ad or video that speaks "Hey Molt, open settings ..." through the speakers.
- **Other people and devices:** someone else in the room, a TV, a voice assistant.
- **Replay:** a recording of the user's own voice.
- **Text injection:** a page's text that tries to steer the planner ("ignore the user and ..."). The agent's untrusted-text fencing already handles this.
- **Misrecognition** that leads to a wrong irreversible action.
- **Reading out the wrong thing:** speaking private information aloud in front of others.

### 7.2 Defenses
- **Echo cancellation** against everything the browser plays removes most page audio from the microphone. While a page plays audio, commands also need a slightly higher confidence.
- **Speaker verification** (optional) accepts only the enrolled voice.
- **Page content never becomes a command.** Text from pages goes to the planner only inside the untrusted fence, as today.
- **Confirmations:** buying, sending, posting, deleting, changing account or security settings, sharing location or files, and anything the agent's probe already asks about are read back first. They need a spoken "yes" heard by the speech service.
- **Secrets:** never spoken. They are typed only from the vault (`molt_ai/vault`), never by dictation; the user says "use my saved password".
- **Rate limits:** sensitive actions are capped per minute, and a run of failed confirmations stops the voice session.

### 7.3 A voice gesture in place of a real click
Today several decisions count only with a physical click: the page's transient user activation, consumed by `ConsumeUserClick()`.

In voice mode, the browser gives them a **voice gesture** instead. It is a one-time capability the speech service mints when it hears the user confirm a specific prompt, which the browser has spoken and shown. It works like this:
- The prompt shows a random confirmation word (for example "say *harbor* to allow"). A recording or a page's audio can't predict it.
- The capability is bound to that prompt and expires in seconds.
- Pages can't mint it: it comes from the speech service, not from a renderer.

The design and threat model get their own security review before they ship (Phase 3).

### 7.4 MoltNet
- **On-device:** all speech stays local.
- **Pages don't hear the user:** they never receive the user's voice through voice mode. They only see the text the browser types and the clicks it makes.
- **Assistive technology can be noticed:** the overlay isn't part of the page, but a page can sometimes tell that assistive technology is acting, as with a screen reader in any browser. Accessibility actions differ in small ways from a mouse and keyboard, and keeping the accessibility tree costs measurable time. Voice-mode users would stand out from other MoltNet users. So voice mode is off in MoltNet windows by default, and turning it on there says so; making its actions indistinguishable is a Phase 3 item.

## 8. Privacy

- **Audio:** processed in memory and dropped. No recordings are kept.
- **Transcripts:** a history of commands is off by default. If the user turns it on, it is kept locally like AI memory and can be deleted by voice.
- **No new network traffic:** model files are downloaded like the others (pinned hashes), and nothing else.

## 9. Engines and models

| Need | Option | License | Notes |
|---|---|---|---|
| Wake phrase | sherpa-onnx keyword spotting (open vocabulary: the phrase is text, no training) | Apache-2.0 (library); models per model card | Any phrase the user chooses; English and Chinese models today. Measured: misses 7–15% of clean wake phrases and more in noise, mostly for particular voices, so the threshold adapts per user. |
| | openWakeWord | Apache-2.0 code, **non-commercial** pre-trained models (CC BY-NC-SA 4.0) | Not usable as is. |
| Voice activity | Silero VAD (through sherpa-onnx, or in whisper.cpp) | MIT | Small; handles slow speech with tuning. |
| Streaming recognition | sherpa-onnx streaming transducers: Kroko EN (a Zipformer), or NeMo FastConformer | Apache-2.0 (library); Kroko CC BY-SA 4.0, FastConformer CC BY 4.0 | True streaming with partial results; multilingual models exist. Measured: Kroko is as accurate on commands as anything but Whisper small.en, at 4–6% of a core; the Zipformers trained on audiobooks only miss modern names. |
| | whisper.cpp (already bundled) | MIT | Accurate, but not streaming. Keep it for long dictation and for languages the streaming models lack. |
| Speaker verification | sherpa-onnx speaker embeddings | Apache-2.0 | Optional. |
| Voices | Platform voices through `TtsController` | — | Phase 1: no download; the voices differ per OS. |
| | Kokoro-82M (through sherpa-onnx) | Apache-2.0 | 54 voices, 8 languages, 24 kHz; natural. Measured: 0.7–1.5 s to the first sound on a fast CPU, so for fast machines only. |
| | Piper | GPL-3.0 (since its move to OHF-Voice; compatible with MoltBrowser's GPLv3); voices per model card | Very fast and light (first sound in under 0.15 s, measured); many languages. |

Models and open-source agents for acting on pages (checked October 2026):

| Role | Option | License | Notes |
|---|---|---|---|
| Planner over the page map (text) | Qwen3.5-4B, Gemma 4 E4B (already in MoltBrowser's model list) | Apache-2.0 | Fast enough on a CPU with constrained JSON; the page map gives them numbered items, as browser-use does. |
| Screenshot navigator (fallback for pages with poor accessibility) | MolmoWeb 4B and 8B (Ai2, March 2026) | Reported Apache-2.0; confirm on the model card | Strongest open web-agent model reported; open training data; screenshots only. llama.cpp support for its architecture (Molmo2) is unconfirmed. |
| | Fara-7B (Microsoft) | MIT | Built on Qwen2.5-VL, which llama.cpp runs; reported 73.5% on WebVoyager. |
| | UI-TARS-1.5-7B (ByteDance) | Apache-2.0 | Built on Qwen2.5-VL. |
| Agent designs to learn from | browser-use (Python), Stagehand (TypeScript) | MIT | Numbered interactive elements; cached actions. Ideas only: neither runs inside a browser process. |
| | BrowserOS (a Chromium fork with an agent) | AGPL-3.0 | Closest open product; not voice-first; its agent runs as a separate server and uses local model servers. Ideas only. |

No maintained open-source voice-first browser exists: the voice tools for disabled users are closed (Windows Voice Access, macOS Voice Control, Talon, commercial extensions) or research prototypes. MoltBrowser's navigator is built natively: the accessibility page map, the existing agent and its safety checks, the sandboxed models, and learning from use (§6.8). The screenshot navigators are compared on the §12 task set in Phase 2.

**Recommendation:** sherpa-onnx in the speech service, for the wake phrase, voice activity, streaming recognition, speaker verification and the bundled neural voice. It is one Apache-2.0 library that also runs on Android and iOS. Keep whisper.cpp for long-form dictation.
- **What it costs:** ONNX Runtime (MIT) as a new vendored dependency, sandboxed like llama.cpp.
- **Measured (Phase 0):** [VOICE_BENCH.md](../VOICE_BENCH.md) has latency, accuracy, CPU use, memory and size against whisper.cpp, from a server VM; the build machine re-runs the kit on the minimum hardware before the choice is final. The numbers keep sherpa-onnx and change two model choices:
  - **Kroko EN for streaming,** not the LibriSpeech Zipformers: those were trained on audiobooks and miss modern names ("you tube" for YouTube), while Kroko gets 70% of the spoken commands exactly right (Whisper small.en 78%, the bundled Whisper tiny.en 68%).
  - **A Piper voice for the bundled neural voice on modest hardware,** Kokoro only on fast machines. sherpa-onnx 2.x reads English through a lexicon only and skips unknown words, so either needs MoltBrowser's own text normalization in front of it.
  - Echo cancellation with the browser's output as the reference is a requirement: without it, recognizers transcribe the browser's own voice.
- **Downloads:** model files come from the projects' GitHub releases with pinned hashes, as for the other models.

## 10. Phases

Rounds are this project's working rounds. Each phase ends with its exit criteria met on the build machine with real microphones, so the build machine is a prerequisite.

### Phase 0: Foundations (about 2 rounds)
- **Engine spike:** measurements of sherpa-onnx and whisper.cpp (latency, accuracy on a spoken-command test set, CPU, memory, size) on x86-64 and arm64. Recorded here, then the owner's choice (§11).
- **Page map from the accessibility tree:** numbered items, repeated-structure detection, the browser UI's tree. Also used by the agent.
- **Actions through accessibility actions,** including the check that media starts with sound and full screen works.
- **Spoken output** through `TtsController`.
- **An accessibility audit and fixes of MoltBrowser's own pages:** the chat, the agent studio, settings and the MoltNet panel.
- **The test harness of §12.**

**Exit:** a scripted run (text in, no microphone yet) opens YouTube, searches, reads the results and plays the fourth by number and by name.

**Status (round 37): built; the exit run waits for the build machine.**
- **Done:** the page map and accessibility actions (round 36, §6.3, §6.5); the grammar and the matcher (round 36, §6.2, §6.4); the accessibility audit of MoltBrowser's pages (round 36, `docs/ACCESSIBILITY.md`); spoken output through `TtsController` and the voice loop that joins them (round 37, §5.2, §6.2, §6.6); the numbers overlay and caption bar (round 37's overlay workstream, §5.3, §5.5).
- **The exit criterion** is `VoiceControllerBrowserTest.TheOwnersScenario` (`chrome/browser/molt_ai/voice/voice_controller_browsertest.cc`): typed utterances open the fixture video site by name, search it, hear the 20 results (captured from a fake TTS platform), play the fourth with sound, pause, go back, and play "the live one by Queen" after the browser asks which of three. It runs on the build machine (`docs/BUILD_MACHINE_CHECKLIST.md` §5.1), then by hand on real YouTube with each OS's voices (§7.3).
- **Still open:** the engine measurements (`tools/voice_bench/`, finishing); the browser UI's own tree in the page map; the §12 synthetic speech set (Phase 1, with the speech service).

### Phase 1: Voice mode (about 3 to 4 rounds)
- **Voice:** the speech service with echo cancellation, voice activity, the wake phrase and streaming recognition.
- **Voice mode, by voice alone:** turned on and off by voice, and offered at first run. It can also start with the browser at login, so the user never needs to touch the machine after setup.
- **Commands and choices:** the direct-command grammar, choosing by number and name, the numbers overlay, the caption bar, and the follow-up window.
- **Media and reading:** media control, reading the page, headings and links.
- **Sites:** skills for YouTube, Google, DuckDuckGo and Wikipedia.

**Exit:**
- The owner's song scenario and 50 scripted spoken tasks pass with at least 90% success, in a quiet room and with music playing.
- A direct command starts acting within 1.5 s of the end of speech on the minimum hardware.

### Phase 2: Conversation and tasks (about 4 to 6 rounds)
- **Planning:** open-ended tasks through the planner, with spoken steps and confirmations.
- **Dialog:** disambiguation, correction and undo.
- **Forms:** dictation and form filling with the vault.
- **Questions:** summaries and questions about pages.
- **More site skills:** webmail, news, shopping, maps.
- **A bundled neural voice.**
- **Learning (§6.8):** routines saved and started by voice, personal vocabulary and corrections, site hints. A comparison of the screenshot navigators on the task set.

**Exit:**
- 30 multi-step tasks (mail, shopping cart up to the confirmation, a form) succeed without a hand.
- Every irreversible step is confirmed.

### Phase 3: Trust and the browser's own UI (about 2 to 3 rounds, partly alongside Phase 2)
- **Voice gestures** in place of real clicks (§7.3).
- **Speaker verification.**
- **The defenses** against page audio and replay, with measurements.
- **Every browser prompt and dialog operable by voice:** permissions, downloads, saved passwords, MoltNet.
- **Voice mode in MoltNet windows:** actions that look like a mouse and keyboard, and an accessibility tree that costs a page nothing it can measure; until then, off there by default (§7.4).
- **A security review** (red team) of the voice channel.

**Exit:** no page audio or replay in the test set triggers an action, and every browser prompt is answerable by voice.

### Phase 4: Low vision, languages and reach (ongoing)
- **Low vision:** magnification that follows focus, reading mode with highlighting, contrast themes.
- **Speech:** more languages, personal vocabulary and aliases, and adaptation for impaired speech (per-user fine-tuning or adaptation on the device).
- **Android:** a voice-first MoltBrowser for Android, where hands-free use matters most. The same speech service and page map apply.

## 11. Decisions for the owner

1. **Engine:** sherpa-onnx as recommended (§9), after the Phase 0 measurements, or whisper.cpp only (no wake phrase or neural voice without more work).
2. **The wake phrase:** "Hey Molt", or another, and whether users can change it.
3. **Languages** for the first release (English only, or which others).
4. **Minimum hardware** for voice mode, for example 4 cores and 8 GB of RAM, with the planner possibly slower on less.
5. **Defaults:**
   - Is voice mode offered at first run?
   - Is always-listening allowed?
   - Which verbosity is the default?
6. **User research partners:** organizations of disabled people to co-design and test with, from Phase 1 on. This matters more than any technical choice.
7. **Cloud services:** none, consistent with MoltBrowser's privacy promise (recommended), or an opt-in for languages the device can't do.

## 12. Testing and measurement

- **Synthetic speech set:**
  - spoken commands rendered with several voices, speeds and accents;
  - mixed with noise, music and the page's own audio at several levels;
  - fed through Chromium's fake audio capture (`--use-file-for-fake-audio-capture`).
  - Measured: recognition accuracy and command success.
- **End-to-end runs** against stored copies of real result pages and players, and local fixture sites. Measured: success, latency from end of speech to action, and the time to complete each scenario.
- **Unit tests:** the grammar, the matcher and the page-map heuristics, against real lists and real recognition errors.
- **Security tests:** page audio that speaks commands, replayed confirmations, text injection, run on every change to the voice channel.
- **Accessibility checks** (axe-core) on MoltBrowser's own pages in the WebUI test suite.
- **Sessions with users with disabilities** each phase. Their success and their words decide what is next.

## 13. Risks

- **Latency on modest machines:** the planner is slow on a CPU, so the direct-command grammar has to cover the common cases.
- **Recognition errors,** especially with atypical speech, accents and noise. Mitigations: confirmations, sound-alike matching, a personal vocabulary, and adaptation later on.
- **Sites change.** Skills are data with tests, and the generic page map is the fallback.
- **OS setup a touchless user can't do alone:** the first microphone permission, installing, and starting at login. It needs either one-time help or the OS's own voice control. MoltBrowser can then start at login with voice mode on and recover by itself after a crash.
- **CAPTCHAs:** the browser may offer a site's audio challenge, but it doesn't solve challenges.
- **Security:** a voice channel that acts with user activation is powerful. Phase 3's review comes before it is widely offered.
- **Verification:** none of this can be verified without a Chromium build and real devices.

## References (checked October 2026)
- sherpa-onnx: https://github.com/k2-fsa/sherpa-onnx (keyword spotting: https://k2-fsa.github.io/sherpa/onnx/kws/)
- Kokoro-82M: https://huggingface.co/hexgrad/Kokoro-82M
- Piper: https://github.com/OHF-Voice/piper1-gpl
- openWakeWord models' license: https://huggingface.co/davidscripka/openwakeword
- whisper.cpp: https://github.com/ggml-org/whisper.cpp
