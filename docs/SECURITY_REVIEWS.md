# Security reviews

Adversarial reviews of MoltBrowser code, newest first. Each entry gives the
scope, the method, the findings by severity and the issues fixed. Issues
that aren't fixed are tracked privately until they are.

## 2026-10-31: round 42 (October 2026)

**Scope.** The code of round 42, which each of its four workstreams had
reviewed only on its own side; the places where they meet were looked at
first:

1. The listener's audio clock and the listening session's new rules
   (`voice_listener*`, `voice_listener_protocol*`'s `HelperAudioClock`,
   `voice_session*`): the helper's `audio_ms` against what the browser sent
   it (late, negative or wrapped lags, an epoch reset, audio never sent, a
   helper restarted), voice mode's own voice where the microphone has no
   echo canceller (its edges: the second after, a voice helper that
   restarts, the platform's voice taking over), and the browser's own
   words kept for as long as it speaks.
2. `~/.moltbrowser/settings.json`'s one writer (`common/molt_settings_file*`
   and its five writers): changes made outside its sequence, a symlinked or
   hard-linked file or directory, the read cap, and what molt://ai-settings'
   Save and Reset write for their page.
3. The browser's prompts by voice: a risky download kept through the
   download bubble's controller and without one, the password page
   action's observer (its lifetime, a tab switch, another tab's bubble),
   and the confirmation word heard "for sure" (a guesser's tries, and
   whether the prompt's own word decides it).
4. The on-device planner's steps in routines (`voice_routines*`,
   `voice_learning_loop.cc`, `agents/voice_agent_replay.*`,
   `WebAgent::SetReplayPicker()`): what a hostile page's labels put into a
   saved routine, the secret checks, the replay picker's reply against
   page names (quoting, the reply's format, names that fold together), and
   the site binding (a step changed within the confirmed sites, a click
   that navigates elsewhere mid-replay, drafts and the trust clamp).
5. Questions about the page (`page_questions*`, `voice_page_questions*`,
   `kAskPage`): the untrusted fence and the runtime's middle cut of a long
   message, the answer's wake phrase filter (punctuation, words run
   together, spelled forms, look-alike and invisible characters, an answer
   read in pieces), what a page can make the browser say after "The page
   says", the scorer's cost on hostile text, the time limit, cancellation
   and request ids, and the answer read as page text.
6. Round 41's fixes: a site said from its end (IDNs, IP literals, a final
   dot), the trust clamp on saves, how long a text takes to say for
   scripts the weights don't name, and when a navigation began
   (renderer-initiated, same-document, redirected).

**Method.** Each candidate traced from its attacker (a web page or the
frames it embeds, the page text the on-device model answers from, a
speech or voice helper a hostile input took over, the page of
molt://ai-settings, a file in the settings directory) through MoltBrowser's
code and the upstream Chromium 156 code at the pinned revision (the
download bubble's controller, the embedded test server's handlers). The
answer filter was probed with the voice's own text front end
(`voice_said_aloud.h`) to compare what is written with what is said. Fixes
tested with the std-only unit tests built standalone (system gtest, ASan
and UBSan), the settings file's tests on the `//base` stand-ins, the page
questions, voice controller, prompts and learning fuzzers, and
`molt_fuzz.py check`; the changed C++ was compiled against Chromium 156's
headers; the browser test runs on the build machine.

**Findings.** 15: 5 low and 10 informational (defence in depth,
usability, or what later phases of the design cover). Fixed: three of the
low and one of the informational; the others are tracked privately.

**Fixed.**

- molt://ai-settings' Save writes only the settings its form shows, each
  with a value of its kind; the switches other sections change (voice
  mode's, which take a real click, and the built-in AI APIs') are left as
  they are (`common/molt_settings_file.cc`'s `MergeMoltSettingsForm()`).
- A change that would make `settings.json` bigger than its read cap is
  refused and the file left as it was: such a file would be refused by
  every reader and every later change (`UpdateMoltSettingsFileAt()`).
- An answer about the page that holds voice mode's wake phrase run into
  one word ("HeyMolt", which a voice says as two), after "O.K." (said
  "okay"), or written with Greek or Cyrillic letters that look like Latin
  ones isn't said (`voice/page_questions.cc`'s `HoldsWakePhrase()`).
- When a navigation began is remembered for the tab's main frame however
  many navigations its frames start: a page that kept its frames
  navigating could make the page a voice click led to count from when it
  committed, and "go back" then skipped the user's page
  (`security/activation_policy.cc`'s `NavigationStarts`).

**Tests added.**

- `molt_settings_file_unittest.cc`: a change too big to read back is
  refused and later ones still work; a form writes only its own fields,
  of their kinds.
- `page_questions_unittest.cc`: the wake phrase run together and with
  look-alike letters is found; words that only begin like a greeting
  aren't.
- `activation_policy_unittest.cc`: navigation starts keep the main
  frame's. `activation_guard_browsertest.cc`: "go back" from a slowly
  served page returns to the user's page while the user's page keeps its
  frames navigating.

## 2026-10-30: round 41 (October 2026)

**Scope.** The code of round 41, which each of its four workstreams had
reviewed only on its own side; the places where they meet were looked at
first:

1. Voice mode's learning (`voice_learning*`, `voice_routines*`,
   `voice_vocabulary*`, `voice_hints*`, the learning settings section and
   the automation runner's, scheduler's and Agent studio's refusals):
   routines kept as Agent-mode workflows and read back as untrusted, an
   edited or imported routine against the workflow trust policy, a
   routine replayed on a page that changed or is hostile, the learning
   file's parser and caps, who can teach a correction and what a
   correction can rewrite, and the settings section's rendering and
   argument checks, MoltNet and off-the-record profiles.
2. Site skills and reading aloud (`site_skills*`, `page_reading*`,
   `voice_reader*`, `ax_page_source.cc`'s id, class and selection): the
   five sites' skills against what their users can put on a page (a wiki
   editor's ids, classes and roles, video titles, result snippets), page
   text read aloud against the listening session's checks for the
   browser's own words (with the bundled voice and the platform's, as the
   voice says the text, after a gap, and when the user talks over it),
   the sentence splitter's bounds, the selection read, and the reading
   speed setting.
3. Prompts and the voice gesture (`voice_gesture*`, `voice_prompts*`,
   `voice_browser_prompts*`, `voice_dialog_manager*` and its hook in
   `BrowserWebContentsDelegate::GetJavaScriptDialogManager()`,
   `voice_session_logic`'s `HeardCleanly()`): minting, replay and binding
   when prompts race or are replaced, a page's dialogs from frames,
   background tabs and loops, text dictated into `prompt()`, "leave" on
   `beforeunload`, permission prompts and their "allow this time", a
   frame's requests, dangerous downloads, and the pass-through dialog
   manager's methods and lifetime.
4. The activation guard's round 41 changes: full screen across a
   browsing context group, the history entries voice mode's "go back"
   skips, prompts left to voice mode while it acts, and the video
   picture-in-picture hook, including automatic picture-in-picture on a
   tab switch.
5. The joins: the order in which a site's parts are found (skill, hint,
   generic map) and the skill's search button, learning's check for what
   is heard while the page is read, and a routine's sites confirmed with
   the voice gesture.

**Method.** Each candidate traced from its attacker (a web page or a frame
it embeds, a user of one of the skills' sites, page text read aloud, a
file brought into the automations folder by an import, a page's audio)
through MoltBrowser's code and the upstream Chromium 156 code at the
pinned revision (`WebContentsImpl`'s dialog paths and its suppression of
other origins' frames' dialogs, `TabModalDialogManager`, the permission
request manager, the download item model). What the browser's voice says
for page text was measured with the voice's own text front end
(`voice_said_aloud.h`) against the reading chunks' sizes. Fixes tested
with the std-only unit tests built standalone (system gtest, ASan and
UBSan), the learning and automation tests on the `//base` stand-ins, the
page reading, learning, automation script, prompts and voice controller
fuzzers, and `molt_fuzz.py check`; the changed C++ was compiled against
Chromium 156's headers; the browser tests run on the build machine.

**Findings.** 18: 6 low and 12 informational (defence in depth,
usability, or what later phases of the design cover). Fixed: five of the
low and one of the informational; the others are tracked privately.

**Fixed.**

- A site name too long to say in full is said from its end, in whole
  labels, so its registrable domain is always said; before, its start
  was kept, so a long host that began with another site's name was said
  as that site (permission prompts and the other prompts read by voice,
  the activation guard's questions, a routine's sites;
  `security/activation_policy.cc`'s `SiteForSpeech()`).
- A routine taught by voice that a save brings in (an import, in the
  Agent studio or `molt://ai-automation`, or a copy) is saved as a draft
  whose sites the user confirms first, by the voice gesture or in the
  Agent studio: voice mode's replay checks every step against the
  routine's sites but not its trust level, so an imported routine would
  otherwise have run on its sites unasked
  (`AutomationSecurity::ClampTrustForSave()`).
- A routine's sites are confirmed by voice only when the question says
  every one of them in full; otherwise in the Agent studio, which shows
  them (`voice/voice_routines.cc`'s `RoutineSitesSayable()`).
- Reading aloud sizes its pieces by how long they take to say, not by
  their length in characters: numbers said in words, words voices spell
  out, symbols and pictures said by name and ideographs count for what
  they take to say (`voice/page_reading.cc`'s `SpokenLength()`), so a
  piece is said within the time the listening session remembers the
  browser's words.
- The first history entry an action leads to is judged by when its
  navigation began: a page whose server answered after the action's
  window was taken for one the page added with the action's activation,
  and voice mode's "go back" then skipped the user's own page
  (`security/activation_guard.cc`).
- Nothing is learned from what is said while any of the page's text is
  read, and just after: "continue reading", moving on by heading, and
  the results, links, headings, title and screen read out counted too
  little, and a command said over a reading stops the reader before
  learning looks (`voice/voice_vocabulary.cc`'s `ReadsPageText()`).

**Tests added.**

- `activation_policy_unittest.cc`: long site names said from their end,
  in whole labels. `activation_guard_browsertest.cc`: "go back" from what
  a slowly served page adds returns to the user's page.
- `automation_security_unittest.cc`: a voice routine brought in by a
  save is a draft with no sites, one edited where it lives keeps what it
  had, and its sites once confirmed are what it may use.
  `voice_routines_unittest.cc`: an imported routine needs its sites
  confirmed; sites confirmed by voice only when all are said.
- `page_reading_unittest.cc`: spoken lengths, and a sentence short to read
  but long to say read in pieces of the rate's size.
- `voice_vocabulary_unittest.cc`: every reading of the page counts.

## 2026-10-30: round 40 (October 2026)

**Scope.** The code of round 40, which only its authors had reviewed:

1. Voice mode's own voice, the `molt-voice` helper (`voice/voice_helper/`,
   `scripts/bundle-speech.sh`'s voice parts): the text front end on page
   text (bounds, quadratic paths, UTF-8, numbers of any length, the
   lexicon loader and its caps, the units and the short first unit), the
   frame reader and writer, the helper's limits and exit rules, and the
   supply chain (the voice's and lexicon's pins, the licences shipped,
   nothing fetched at build or run time).
2. The browser's side of that voice (`voice_speech*`): the helper's output
   read as hostile (`VoiceStreamChecker`: audio caps, ids, order), what a
   helper a hostile input took over could make the browser play or keep
   doing, the reader and writer threads, restarts and giving up, playback
   through the audio service, and MoltNet windows.
3. Interrupting that voice without the wake phrase
   (`voice_session_logic.*`): who can speak such a command (a page's
   audio, with Chrome-wide echo cancellation, the platform's or none; a
   bystander), the filter for the browser's own words heard back against
   text a page writes to be heard differently from how it reads, and its
   interplay with the rules for other speakers and missing echo
   cancellation.
4. The activation guard (`security/activation_*` and its hooks in
   `chrome_content_browser_client.cc`, `browser_web_contents_delegate.cc`
   and `chrome_download_manager_delegate.cc`): the features of Chromium
   156 that use or check user activation and which of them it covers, the
   window and what ends it, the decisions with no one to ask, a page
   racing its requests against one question, and each hook's place in
   156's code.
5. The helper sandbox's signal rules and the review's later fixes
   (`common/helper_sandbox_linux.*`, the restarts, the error lines, the
   hardening flags, the wake model's reload), on the real kernel.
6. Questions the agent's page-map path asks about forms, and how the
   planner's questions are spoken.

**Method.** Each candidate traced from its attacker (a web page or a frame
it embeds, page text read aloud by the browser, a page's audio, a helper
that a hostile input took over) through MoltBrowser's code and the
upstream Chromium 156 code at the pinned revision (the popup blocker's
dispositions, the download manager's checks, the external protocol
handler's flood gate, the browser delegate's picture-in-picture, pointer
lock and protocol handler paths) and ONNX Runtime 1.28.2's thread
affinity. The echo filter was checked by feeding the voice's own words
for page-like text, as its text front end says them, back to the session
logic. On Linux the sandbox was exercised against the real kernel
(Landlock ABI 7 and seccomp-bpf, and the seccomp filter alone) as an
unprivileged user: the helper sandbox's tests, a confined program
changing another process's scheduling, and the bundled `molt-speech` (21
of 21 protocol checks) and `molt-voice` (16 of 16, and its speaking
checks) under the resident sandbox after the fixes. Fixes tested with
the std-only unit tests built standalone (system gtest, ASan and UBSan),
the agent page map's unit tests on the `//url` stand-ins, the voice
listener fuzzer (four minutes) and the agent page map fuzzer, which now
checks the quoted labels (two minutes), `molt_fuzz.py check`; the changed C++ was
compiled against Chromium 156's headers; the browser tests run on the
build machine.

**Findings.** 17: 8 low and 9 informational (wording, defence in depth,
licence notices, or what later phases of the design cover). Fixed: seven
of the low and four of the informational (one in part); the others are
tracked privately.

**Fixed.**

- On Linux a helper changes only its own scheduling: `setpriority()` and
  `ioprio_set()` of another process or group, or of all the user's
  processes, and `sched_setaffinity()`, `sched_setscheduler()`,
  `sched_setparam()`, `sched_setattr()`, `migrate_pages()` and
  `move_pages()` of another process are refused by the seccomp filter on
  every kernel (Landlock has no rule for them). A process may otherwise
  lower its user's other processes' priority, make them `SCHED_IDLE` or
  pin them to one CPU, which an unprivileged one can't undo
  (`common/helper_sandbox_linux.cc`).
- While voice mode's own voice speaks, what the browser said is compared
  with what the recognizer heard both as written and as the voice says
  it (`voice/voice_said_aloud.h`, the voice helper's own text front end:
  "example dot com" for "example.com", "second" for "2nd", camel case
  apart), so its own words heard back, however a page wrote them, aren't
  taken for a command (`voice/voice_session_logic.cc`).
- The browser's own reading of a sound-alike of the wake phrase ("Hey
  Mold", "HeyMolt", "Hey Moltbrowser") is recognized as its own, like the
  wake phrase itself: a word with the wake name's phonetic key, or one
  beginning with the name right after "hey" (`voice_session_logic.cc`).
- The voice helper is ended, and restarted with the usual fallback, when
  it sends nothing for 20 s for a request while nothing is left to play,
  so voice mode never stays speaking (`voice/voice_speech_bundled.cc`,
  `StoppedAnswering()` in `voice_speech_protocol.cc`).
- The activation guard asks about at most one of a page's requests per
  agent or voice action: what the page tries after that, until the next
  action, is refused and said, so a question can't be swapped for another
  just as the user answers (`security/activation_policy.cc`,
  `activation_guard.cc`).
- A video's picture-in-picture window, which stays over every app, is
  refused from an agent's or voice mode's action and said, whatever the
  action was for (`BrowserWebContentsDelegate::EnterPictureInPicture()`,
  `ActivationGuard::MayEnterPictureInPicture()`); a document's was already
  asked about as a new window.
- The web agent's click questions put the page's label between quotes
  with its own quotes and dashes made plain, so it can't close them and
  continue in the browser's words (`agents/agent_page_map.cc`'s
  `QuotedLabel()`, `web_agent.cc`); spoken, the label is "the one labelled
  ..." in one phrase (`agents/voice_agent_speech.cc`).
- A form whose action the element snapshot reads and that goes to
  another origin is asked about as "submits a form to <origin>"; on the
  page map's path a submit input outside any form (tied to one with
  `form=`) is asked about as a form submission (`agent_page_map.cc`).
- The bundled voice ships CMUdict's licence notice for its lexicon
  (`voice/voice_helper/LICENSES.md`).

**Tests added.**

- `helper_sandbox_unittest.cc`: the seccomp filter's verdicts for the
  scheduling calls; on the real kernel, a confined child can't change
  another process's priority, policy or affinity and still changes its
  own.
- `voice_said_aloud_unittest.cc`; `voice_session_logic_unittest.cc`: the
  browser's own words heard back as spoken, sound-alikes of the wake
  phrase read out, and words that only share sounds with it. The voice
  listener fuzzer builds the new code.
- `voice_speech_protocol_unittest.cc`: `StoppedAnswering()`.
  `voice_speech_browsertest.cc`: a stand-in helper that never answers is
  ended and the platform's voice speaks.
- `activation_policy_unittest.cc`: one question per action, and a
  video's picture-in-picture refused. `activation_guard_browsertest.cc`: a
  page's download after its asked-about new window is refused and said; a
  video popped out after a voice press is refused and said, one popped out
  with the page's own gesture isn't.
- `agent_page_map_unittest.cc`: quoted labels, a cross-origin form's
  question, submit inputs outside a form; `voice_agent_speech_unittest.cc`:
  the spoken label.

## 2026-10-30: rounds 38 and 39 (October 2026)

**Scope.** The code of rounds 38 and 39 (touchless browsing's listening,
the web agent on the accessibility page map and voice mode's planner
fallback), which only its authors had reviewed, without the Chromium
rebase's mechanical changes, in this order of priority:

1. The speech helper (`voice/speech_helper/`, `scripts/bundle-speech.sh`):
   the frame reader and the CONFIG parser (bounds, integer overflow,
   partial reads), the hotwords' cleaning against sherpa-onnx's hotword
   syntax and spelling, the output caps and the stall rules, and the
   supply chain (the pins, the `git archive` fallback, extraction, and the
   checks that CMake fetches nothing).
2. The helper sandbox's resident mode (`common/helper_resident*`, and the
   resident parts of `helper_sandbox_linux.*`, `seatbelt_profile.*` and
   `helper_sandbox_win_policy.*`): whether a helper a hostile input took
   over can write anything, reach the network or a local service, start a
   process, read outside its models, outlive the browser or pass its memory
   and CPU caps; the restarts and their backoff.
3. The listening session (`voice_session*`, `voice_listener*`,
   `voice_audio*`, `voice_listener_protocol*`): the helper's output (caps,
   UTF-8, the order of events, RESET epochs, and what a hijacked helper's
   text is to the browser); page audio and page speech against voice mode
   (with echo cancellation and without, `speechSynthesis`, the wake phrase,
   the follow-up window, and "stop", "cancel" and "turn off voice mode"
   said by a page or by the browser reading a page); the hotwords taken
   from page text; when the microphone opens and closes (MoltNet windows,
   stopping, closing the window); the settings handler, the toolbar
   button and the real-click rule.
4. The agent on the page map (`agents/agent_page_map*`, `web_agent.cc`,
   `voice_agent_planner*`, `voice_agent_speech*`, `voice_page_controller.cc`'s
   `Inspect()`, `OptionsOf()`, `SelectOption()` and holders): every
   agent_safety rule on the new path (origins per frame and the frames
   between them, credential fields, the fence, the questions before a
   click), page-controlled names, a target that changes between the check
   and the action, an option's default action, holders that never release
   the accessibility mode, and the planner's questions and results read
   aloud.
5. The rebase's merges of `quota_database.{cc,h}`, `mouse_event.cc` and
   `media_stream_video_track.cc`: MoltBrowser's hooks still in place and
   still correct after upstream's changes.

**Method.** Each candidate traced from its attacker (a web page or a frame
it embeds, page audio and page speech, the page text the browser reads
aloud, a helper that a hostile input took over, a compromised renderer)
through MoltBrowser's code, sherpa-onnx 2.0.1's hotword encoder and
keyword parser (`EncodeHotwords()`, `EncodeBase()`), ONNX Runtime 1.28.2's
dependency fetching, and the upstream Chromium 156 code at the pinned
revision (Blink's accessibility serialization of forms, buttons and input
types, `HTMLOptionElement`'s default action). On Linux the helper sandbox
was exercised against the real kernel (Landlock ABI 7 and seccomp-bpf):
the resident mode's tests, a confined program sending a datagram to a
named socket outside its paths, and the bundled speech helper itself
under the resident sandbox (the protocol checks of
`speech_helper_check.py`, 21 of 21). Fixes tested with the std-only unit
tests built standalone (system gtest, ASan and UBSan), the helper
sandbox's tests on the real kernel, the agent page map's unit tests
built standalone on the `//url` stand-ins, the voice listener fuzzer
(four minutes after the fixes), `molt_fuzz.py check`; the changed C++ was
compiled against Chromium 156's headers; the tests that need Chromium run
on the build machine. The fixes made later in the round were tested the
same way, and also with the seccomp filter alone (as on a kernel whose
Landlock doesn't scope signals): the helper sandbox's tests in a child
confined by it, and the speech helper's 21 protocol checks under it; the
speech helper's own unit tests and fuzzer, and its Linux build checked
with `readelf`.

**Findings.** 16: 9 low and 7 informational (wording, defence in depth,
performance, or what later phases of the design cover). Fixed: six of
the low and three of the informational; the others are tracked
privately.

**Fixed.**

- On Linux a helper's UNIX sockets are stream or seqpacket sockets only:
  connect() was already refused, but a datagram socket reaches a named
  socket without it, as a logger reaches the system log. This holds for
  the OCR and voice helpers and the resident speech helper alike
  (`common/helper_sandbox_linux.cc`).
- Where the microphone has no echo canceller (Chrome-wide echo
  cancellation turned off, and a device without its own), a tab's sound
  reaches the recognizer as it is. A tab playing sound then counts as
  another source's speech, as a page's `speechSynthesis` does: only
  "stop", "cancel" and "turn off voice mode" count while it plays and a
  second after. The session also looks at the tabs' sound at each of the
  helper's events (`voice/voice_session_logic.cc`, `voice_session.cc`).
- The page's names that bias recognition (hotwords) leave out any phrase
  the grammar reads as a yes ("Yes", "OK", "Sure, go ahead"), so page
  text never biases the answer that confirms an action
  (`voice/voice_listener_protocol.cc`).
- The web agent's page map lists, and acts in, the page's own frame and
  frames of its origin that only frames of that origin embed: a frame of
  the page's origin inside another site's frame (an ad that embeds one of
  the page's URLs of its choosing, with its buttons) is left out like the
  frame around it, and refused at action time (`agents/web_agent.cc`,
  `agent_page_map.cc`, `voice/voice_page_controller.cc`).
- On Linux the helpers' seccomp filter keeps their signals to the helper
  itself, its threads and its process group on every kernel, as Landlock
  does from ABI 6 (Linux 6.12): `kill()`, `tgkill()` and the `sigqueue`
  calls name the helper's own process ID, which the child writes into its
  copy of the filter after `fork()`; `tkill()` and `pidfd_send_signal()`
  are refused (glibc's `raise()`, `abort()` and `pthread_kill()` use
  `tgkill()`); a file's owner for `SIGIO` and `SIGURG` can only be the
  helper; and `prlimit64()` changes only its own limits
  (`common/helper_sandbox_linux.cc`).
- The voice listener counts a speech helper that ran a minute after it was
  ready as a fresh start however it ended (a limit, a protocol error, a
  stall), not only when it exited, so failures hours apart don't add up
  to giving up (`voice/voice_listener.cc`, `HelperFailureCount` in
  `voice_listener_protocol.cc`).
- The speech helper exits after an `error` line (SPEECH_HELPER_PROTOCOL.md
  §3.7): the browser now ends it at the first, passed on once to the log,
  and refuses any line after one, so a helper that writes errors and
  carries on is restarted with the backoff like a crash
  (`voice/voice_listener.cc`, `voice_listener_protocol.cc`).
- The speech helper's frame reader and CONFIG parser are built with the
  program's hardening (stack protector, `_FORTIFY_SOURCE`, stack clash
  protection where the compiler has it, a position-independent
  executable; Control Flow Guard on Windows), whatever the toolchain's
  defaults (`voice/speech_helper/CMakeLists.txt`).
- The speech helper rebuilds its wake-phrase spotter only when the wake
  phrases change, not at every CONFIG, which carries them each time the
  follow-up window opens or closes: such a CONFIG took 0.8 s here, during
  which the helper read no audio, and now takes 0.1 ms
  (`voice/speech_helper/listener.cc`).

**Tests added.**

- `helper_sandbox_unittest.cc`: the seccomp filter's verdicts for each
  socket type and flag; a confined short-lived helper can't send a
  datagram to a named socket outside its scratch directory.
  `helper_resident_unittest.cc`: the same for a resident helper. The
  stand-in helper (`helper_sandbox_test_helper_posix.cc`) has a `dgram`
  action for them.
- `voice_session_logic_unittest.cc`: a video's wake phrase and command,
  and an answer in the follow-up window, without an echo canceller; the
  same with one. The voice listener fuzzer drives the new input and checks
  that only the commands that always count are taken while a tab plays
  sound without one.
- `voice_listener_protocol_unittest.cc`: yes-phrases left out of a page's
  hotwords, other names kept.
- `agent_page_map_unittest.cc`: a frame of the page's origin inside
  another site's frame is refused at action time.
  `agent_page_map_browsertest.cc`: the same widget listed when the page
  embeds it, and left out when another site's frame does.
- `helper_sandbox_unittest.cc`: the seccomp filter's verdicts for each
  signal call and owner command, its own-ID slots; a child confined by the
  seccomp filter alone (as on a kernel without Landlock's signal scoping)
  can't signal its parent in any of those ways, still signals itself and
  still ends with `SIGABRT` on `abort()`; the stand-in helper (new
  `signals` and `abort` actions) under the short-lived sandbox.
  `helper_resident_unittest.cc`: the same under the resident one.
- `voice_listener_protocol_unittest.cc`: nothing after an `error`; the
  failure count, its fresh start after a stable run and its giving up.
  The voice listener fuzzer checks that nothing follows an accepted
  `error`. `voice_session_browsertest.cc`: a helper that writes `error`
  lines and carries on is ended three times, then voice mode gives up.
- `speech_helper/listener_unittest.cc`: CONFIGs with the same wake phrases
  keep the spotter (a wake phrase heard across one still wakes it); new
  ones rebuild it.

## 2026-10-29: PR #80 (October 2026)

**Scope.** The code merged in pull request #80 (rounds 36 and 37, touchless
browsing's Phase 0), which only its authors had reviewed, in this order of
priority:

1. The page map and the accessibility actions (`page_map*`,
   `ax_page_source*`, `voice_page_controller*`): what a page, or a
   compromised renderer, can send (updates a tree can't take, the reset
   loop, a frame claiming another frame's tree, huge trees and strings,
   location changes with hostile bounds and offset containers); whether a
   page can get the browser to press, type into or focus a node of another
   frame, tab or origin, or of the browser's own interface; stale handles;
   the Enter fallback's wait for the focus; and fields that hide a
   password, card number or one-time code behind other roles, labels or
   `contenteditable`.
2. The voice loop (`voice_controller*`, `voice_sites*`,
   `voice_candidates*`, `voice_speech*`): confirmations (names that avoid
   the trigger words, a question that outlives its page or tab, a "yes"
   from elsewhere), spoken addresses and the default engine, page text
   that tries to steer the loop, the waits and the controller's lifetime
   across window and tab closes and crashes, and MoltNet's refusal.
3. The numbers overlay and the caption bar (`ui/views/molt_ai/voice/`, the
   `ContentsContainerView` hooks read from their import commit): hostile
   bounds, drawing outside the page, the caption's page text, and what a
   page can observe.
4. The agent panel's voice field (`molt_ai_agent_voice_handler.*`),
   `kMoltA11yJs`, and the pages' new dialog and live-region code under the
   strict CSP and Trusted Types.
5. The grammar and the matcher (`voice_command*`, `spoken_*`): linear time,
   the wake phrase, the `whole_query` fallback, and the characters kept in
   what was heard.
6. The `#molt-voice-mode` flag entries, `configs/windows-x64.gn`'s DCHECK
   switch, and the voice bench fetcher's archive extraction and pinning.

**Method.** Each candidate traced from its attacker (a web page, a frame a
page embeds such as an ad or another site's widget, a compromised
renderer, whatever the browser hears or is typed into the voice field)
through MoltBrowser's code and the upstream Chromium 156 code at the
pinned revision: `RenderFrameHostImpl::HandleAXEvents()` and
`GetAXTreeData()` (the tree data the browser sets: tree, parent and
focused tree ids), `AXTree::Unserialize()` and
`RelativeToTreeBoundsInternal()`, `AXTreeManager`'s and
`BrowserAccessibility`'s own check of a child tree's parent, and the
contents area's layout. Page maps were built from hand-made trees of pages
that embed a frame larger and more prominent than their own content. Fixes
tested with the std-only unit tests built standalone (system gtest, ASan
and UBSan, `-fno-exceptions`), the page map, grammar, matcher and voice
loop fuzzers (a few minutes each, after the fixes), `molt_fuzz.py check`,
the WebUI checks and their `npm test`; the changed C++ was compiled
against Chromium 156's headers; the tests that need Chromium run on the
build machine.

**Findings.** 22: 1 medium, 11 low and 10 informational (by design,
already correct, or covered by this round's listening work). Fixed: the
medium and six of the low; the others are tracked privately.

**Fixed.**

- Voice mode's map of a page puts the page before the frames it embeds,
  so an ad or another site's widget can't take its place: the frames'
  trees share part of the node budget and of the numbers, the page's
  lists are looked for first and come before the frames' however
  prominent, its media before theirs, and what the browser picks by
  itself ("next page", "louder", full screen, captions) is the page's own
  first. "Search for ..." types only into the top-level document's own
  search box, never one in a frame. The page's own tree is always kept,
  and the frames' trees are capped in nodes together
  (`voice/ax_page_source.cc`, `page_map.cc`, `voice_controller_logic.cc`,
  `voice_page_controller.cc`).
- A frame's tree joins the page only under a node of the tree the browser
  records as the frame's parent, as Chromium's own accessibility does
  (`voice/ax_page_source.cc`).
- A node whose bounds would be measured through offset containers that
  lead back to each other, or through a chain of them longer than the walk
  is deep, gets no bounds instead of being measured
  (`voice/ax_page_source.cc`).
- A tree voice mode keeps starts empty, and takes only a whole tree then;
  tree data alone (which the browser sends when the focus moves, as when
  a tab becomes active again) waits for it instead of being reported as
  an error, which builds that fail fast on accessibility errors make a
  crash (`voice/ax_page_source.cc`, `voice_page_controller.cc`).
- After "Which one?", an answer counts only while the choices read out
  still name the same items; when the page changed meanwhile (a frame
  before them grew, for one), the browser asks again
  (`voice/voice_controller_logic.cc`).
- What was heard, and the words a command carries, are cleaned in the
  grammar as page text is: Hangul fillers, private-use characters,
  noncharacters and U+FFF9 to U+FFFD are dropped too
  (`voice/spoken_text.cc`).

**Tests added.**

- `ax_page_source_unittest.cc`: a frame claiming a sibling's tree, and a
  tree with no recorded parent; offset containers in loops (two nodes, a
  node leading into a loop, a node without a size whose child or nearest
  sized ancestor loops, the root) and a very long chain of them; the
  frames' share of the node budget; what an empty tree takes.
- `page_map_unittest.cc`: a frame with a list more prominent than the
  page's, and a video before the page's; the frames' share of the numbers.
- `voice_controller_logic_unittest.cc`: "search for", "next page", "play"
  and "louder" on a page with an ad frame; an answer to "Which one?" after
  a frame before the choices grew.
- `voice_page_controller_browsertest.cc`: the page's tree kept when the
  frames' trees fill their caps (in number and in nodes); tree data alone
  after voice mode starts again, then the whole tree.
- `spoken_text_unittest.cc`, `voice_command_unittest.cc`: the characters
  dropped from what was heard; the grammar, page map and voice loop
  fuzzers check the new rules (with a new grammar seed).

## 2026-10-29: PR #79 (October 2026)

**Scope.** The code merged in pull request #79 (round 35), which only its
authors had reviewed, in this order of priority:

1. The OCR and voice helpers' limits on every OS: the launcher's watchdog
   over a helper's scratch directory (races with a helper's next write;
   hard links, open-but-deleted files, files renamed out and back, sparse
   and preallocated files, names that change during a scan, the check after
   the helper exits); on Linux the seccomp rules (the thread-only `clone`,
   `clone3`/`openat2` ENOSYS fallbacks, other ways to get unaccounted
   memory, `O_TMPFILE` and extended attributes) and the resource limits
   (`RLIMIT_DATA`, `RLIMIT_NOFILE`); on macOS the Seatbelt write rule and
   the footprint watchdog; on Windows the handle-based scanner (alternate
   streams, compressed and sparse files, a file held with deny-sharing) and
   the scratch directory's security descriptor; and whether one helper
   run's limits or kill can reach another run or the browser.
2. MoltNet's cameras: the renderer-local `ImageCapture` service (lifetime,
   a track that ends or is cloned, the 2-second timeout, JPEG encoding on a
   worker, the pan-tilt-zoom permission hook); `VideoTrackAdapter`'s 30 fps
   grid (the clock's lifetime, ticks after the source stops, many tracks at
   different rates, a page that adds and removes tracks quickly, the memory
   the newest frame holds); the GPU readback (the worker task, coalescing, a
   readback that finishes after the adapter is gone, the black-frame rule);
   the colour conversion (odd sizes, strides, refused formats, integer
   overflow in the Q14 arithmetic); and the camera menu.
3. The per-site shares of sign-ins and HTTP/1.1 records (the entry a cache
   must keep): whether sites can evict each other in a loop or keep another
   from signing in, the cost of the passes over all sites, the use counter,
   and every way an entry leaves.
4. The down-mix layout exclusion, and the leak kit's camera probes and
   expectations.

**Method.** Each candidate traced from its attacker (a web page, a site a
page embeds, a server, a compromised renderer, a helper that a hostile OCR
or voice input took over, a device's reported modes) through MoltBrowser's
code and the upstream Chromium 156 code at the pinned revision, with the
upstream files' edits read from their import commit. The per-site share
rule's eviction was traced for loops and for a site locked out of signing
in: a site that holds none, or at least two fewer than the site that holds
the most, always makes room for an entry a cache must keep, by taking that
site's least recently used entry, so the keepers converge to equal counts
and a site is never pushed below the mean; the passes over the sites are
bounded by the cache's total. The camera's colour coefficients were
recomputed and the Q14 arithmetic bounded against 8-bit inputs (no 32-bit
overflow); the grid's clock was traced against the adapter's and the
tracks' lifetimes (its ticks and readbacks keep the adapter alive, so
neither outlives it) and for ticks after the source stops (the grid holds a
second, then stops). On Linux the helper sandbox was exercised against the
real kernel with the shipped code: a helper cannot hard-link a file from
outside its scratch directory into it (the link is refused `EXDEV`), so it
gains no write handle out of the one place it may write. The leak kit's
Python suites and `molt_fuzz.py check` pass.

**Findings.** 4: 1 low, tracked privately, and 3 informational (by design
or already correct).

**Tests added.**

- The helper sandbox's write-confinement test now also checks that a helper
  can't hard-link a file from outside its scratch directory into it,
  besides deleting, renaming, making directories and symbolic links, so the
  scratch directory stays the only place a helper's writes can reach
  (`chrome/browser/molt_ai/common/helper_sandbox_unittest.cc`).

## 2026-10-29: PR #78 (October 2026)

**Scope.** The code merged in pull request #78 (round 34), which only its
authors had reviewed, in this order of priority:

1. MoltNet's per-site shares of the fixed-size network caches: the
   containers (`MoltSiteShares`, `MoltSiteLRU`, the site a network key
   counts against) and their use in the TLS session cache, the HTTP
   authentication cache, the server properties, the CORS preflight cache
   and the in-memory compression dictionaries; each site's record on every
   way an entry leaves (erased, cleared, swapped, shrunk, under memory
   pressure, dropped with a nonce); whether a site, or a group of sites,
   can evict, keep out or observe another site's entries or grow memory
   without bound, and whether any of it runs in a regular profile; and
   every caller at Chromium 156 of `HttpAuthCache::Add()`, which may now
   return null, and of `ServerInfoMap::GetOrPut()`, which may now return
   `end()`.
2. The totals of partitioned-cookie partitions without a nonce (their
   limits and purge, the counts on every insertion and deletion); a
   network context made again refusing its partition's nonces from the
   start (`molt_refused_nonces`: who fills it, and whether a page can
   reach it); MoltNet's context without QUIC.
3. Clear-Site-Data in the request's partition: the storage key it names
   for requests of frames, workers, keepalive requests after their page
   went, redirects and sandboxed top-level pages.
4. MoltNet's cameras: the menu that stands in for a device's modes,
   `VideoTrackAdapter`'s conversion (sizes, crops, frames the CPU can't
   read, odd sizes, rotation, frame rates), `applyConstraints()` and a
   second `getUserMedia()`, what a page can learn of the device's own
   modes, and sizes or formats the conversion doesn't expect.
5. The browser's decision on the CPU-independent processing of a
   renderer's microphone streams: every path a processing config takes to
   the audio service, a renderer that sends none, other profiles.
6. Microphone channels and down-mixing: the layouts and channel counts a
   renderer or a device brings to the processing, and the checks they
   could reach.
7. IIRFilterNode's floor, `setSinkId()` with a device MoltNet doesn't list,
   the OCR run slots, the Linux helpers' file-size and core-dump limits,
   and the leak kit's `--out` group check and its test fixtures.

**Method.** Each candidate traced from its attacker (a web page, the sites
a page moves its tab through, a third party it embeds, a site's server, a
compromised renderer, a device's reported layout, another account in the
user's group) through MoltBrowser's code and the upstream Chromium 156 code
at the pinned revision: HTTP authentication (`HttpAuthController`, the
login prompt's `AddAuthCacheEntry()` and reload, Blink's rule for
credentials in subresource URLs), `HttpNetworkTransaction`'s retry when a
server requires HTTP/1.1 and `SpdySession`'s record of it, the audio
service's `InputController` and `AudioProcessor`, media's `ChannelMixer`,
`StorageKey` and `CookiePartitionKey`, the camera's capture and
`VideoTrackAdapter`. Fixes tested with standalone harnesses (g++
ASan/UBSan, clang `-fno-exceptions`; each pasted hook block checked against
the tree): the HTTP authentication cache's and the HTTP/1.1 records' hooks
at round 34 and now, past a total that sixteen sites filled; the down-mix
of every layout it takes with Chromium 156's `ChannelMixer`; the shares'
std-only unit tests (16) and 13 mutants of the new rule, each caught. The
leak kit's test suite passes as root, as an unprivileged account, and as
one whose primary group is `users` or `daemon`; the tools' own test suites
and `molt_fuzz.py check` pass; the changed C++ was compiled against
Chromium 156's headers.

**Findings.** 12: 3 low, 9 informational. Fixed: two of the low, and two
informational.

**Fixed.**

- MoltNet's per-site shares of the fixed-size network caches: what the
  user signs in to a site with (an HTTP authentication entry) and a
  server's demand for HTTP/1.1 are kept also once other sites fill the
  cache's total between them. Such an entry of a site holding none, or at
  least two fewer than the site that holds the most, takes the place of
  that site's least recently used entry, so the sign-in prompt doesn't
  come back on every request, and a request to a server that requires
  HTTP/1.1 is sent over HTTP/1.1; the sites that keep such entries end up
  holding as many as each other, give or take one, and a site holding
  fewer entries than another keeps its own
  (`net/base/molt_site_share_policy.h`, `molt_site_lru_cache.h`,
  `net/http/http_auth_cache.cc`, `http_server_properties.cc`).
- The CPU-independent processing down-mixes only layouts media's
  `ChannelMixer` takes: a microphone that reported the deprecated
  `STEREO_AND_KEYBOARD_MIC` layout fails its processed streams, as at
  Chromium 156 (`media/base/molt_portable_downmix.cc`).
- The leak kit's `--out` group tests use fake groups whose ids no real
  user's group has, so they pass for a user whose primary group is `users`
  or `daemon` (`tools/leak_test/tests/test_rundir.py`).

## 2026-10-28: PR #77 (October 2026)

**Scope.** The code merged in pull request #77 (round 33), which only its
authors had reviewed, in this order of priority:

1. Cookies under a page's nonce: the cookie store's totals for each nonce
   partition and each nonce, kept on every path that stores or deletes a
   cookie; the eviction inside a partition (iterators and partitions while
   it deletes), a refused nonce, the size entry dropped with a partition's
   last cookie; whether a page or a site can evict or refuse another's
   cookies, or leave its own unable to be stored, and whether any of it
   runs in a regular profile.
2. The network context's drop of what a nonce keys: who can ask for it,
   its cast to the cookie store, and each cache it drops from (socket
   pools, HTTP/2 sessions, TLS sessions, HTTP authentication entries,
   server properties, CORS preflight results): the nonce's idle entries
   only, never a connection in use, safely while it iterates.
3. The quota manager's refusal of buckets for a closed nonce group; the
   tab helper's bound of 256 storage keys a group, pages that share a
   nonce, and the timers after a page goes, at tab and profile shutdown.
4. Deletions of one storage key: the listing of a time range through the
   quota database's index by key, and Clear-Site-Data on a response the
   browser can't place in a document.
5. Subnormal floats in the portable audio code: the scopes that turn the
   flush-to-zero modes off (restored on every path, or leaking to other
   work on the thread), regular profiles, the floors in the resampler,
   WSOLA, Web Audio and the mixer, and WebRTC's noise floor.
6. MoltNet's microphones: the capture bit the platform code reads (what a
   renderer of any profile gains by setting it, Android included), the
   generic labels and groups wherever a page reads them, the PulseAudio
   source choice (locks, server errors, which source it picks), the
   Windows category condition.
7. The OCR and voice helpers: on Windows the less-privileged AppContainer
   (its package SID, the security descriptors built from SIDs, the
   scratch directory's rights and label, handle inheritance, the job, the
   mitigations, the environment, the check before launch, every failure,
   the read of the helper's output file) and the installer's grant; on
   every OS the hand-off to Chromium's data decoder (sizes, deadlines,
   processes per image and page), the PNM writer, the TIFF refusal, the
   PDF page loop and its deadline, the WAV check and its rewrite.
8. The leak kit and tools: the previous review's fixes (the `--out`
   directory reached without links, reads from the run directory's
   descriptor, the run's ownership, root's own name-service files and the
   launcher's checks of them), `rebase.py`'s rule for files the release
   tarballs lack, and the iOS packaging script.

**Method.** Each candidate traced from its attacker (web page, a
third-party or credentialless frame, another site's page, a site's server,
a compromised renderer or helper, a process of the user who runs the leak
kit as root) through MoltBrowser's code and the upstream Chromium 156 code
at the pinned revision: the cookie store, the network context and the
caches it drops from, the quota manager and database, the Clear-Site-Data
handler with its filter builder and storage keys, PageImpl's nonce, the
audio service's mixer, the PulseAudio and WASAPI input streams, and the
sandbox library's AppContainer, job and attribute list. A PulseAudio 16.1
server showed what it reports for a virtual microphone (a remapped null
sink's monitor: a filter whose master device is a monitor). Fixes tested
with standalone harnesses (system gtest, g++ ASan/UBSan): the PulseAudio
source choice, 6 tests, 3 of which the previous code fails; the
Clear-Site-Data policy's unit tests, and the hook's choice with 156's
matching for a response to another site's page, the origin's own page, a
credentialless frame and a request with no partition, where the previous
hook deleted the origin's first-party storage for another site's page;
and 7 mutants of the two fixes, each caught by the tests. The leak kit's
test suite as root and unprivileged, the tools' own test suites and
`molt_fuzz.py check` pass; the changed C++ was compiled against Chromium
156's headers.

**Findings.** 12: 2 low, 10 informational. Fixed: the two low, and two informational (OCR runs at once; on Linux, the size of the files a helper writes).

**Fixed.**

- MoltNet's microphones on Linux: a stream records from the microphone
  under a PulseAudio or PipeWire filter source only when the server says
  which it is. Otherwise (the filter's recording stream isn't listed, its
  master isn't, the chain loops, or a sink's monitor is under it: a
  virtual microphone that plays applications' output) it records the
  source it asked for, never another device in its place
  (`media/audio/pulse/molt_capture_source.cc`).
- MoltNet's Clear-Site-Data on a response the browser can't place in a
  document or worker (a keepalive request's after its page went) names
  the key a document of its origin would have where the request was made:
  in the request's cookie partition, under a credentialless frame's nonce,
  or the origin's own key for a request with no partition. A response to a
  request another site's page made of the origin deletes what the origin
  keeps under that site, never its first-party storage
  (`content/browser/browsing_data/molt_one_key_deletion_policy.h`,
  `storage_partition_impl.cc`).
- OCR: at most two runs (image attachments and scanned PDFs) are under way
  at once; another gets "busy" at once. Each run holds a decoder or
  document parser process and up to 128 MiB of decoded image in the
  browser, so files sent faster than they are read can't make it hold an
  unbounded number. A run's slot comes back when it answers, or when its
  callback is dropped unrun (`chrome/browser/molt_ai/ocr/ocr_service.cc`).
- The OCR and voice helpers on Linux write no file larger than 64 MiB
  (RLIMIT_FSIZE; past it the helper ends) and no core dump, so one that a
  hostile input took over can't fill the disk before its deadline
  (`chrome/browser/molt_ai/common/helper_sandbox_linux.cc`).

## 2026-10-28: PRs #75 and #76 (October 2026)

**Scope.** The code merged in pull requests #75 (round 31: the rebase onto
Chromium 156.0.8078.74 and the previous review's fixes, which no one else
had reviewed) and #76 (round 32), in this order of priority:

1. The rebase and its tools: the 3-way merges of the DEPS-subproject whole
   files and the commits they record (`tools/chromium/subprojects.py`,
   `rebase.py`, the lag check of `verify_patches.py`), the nine patches
   merged with upstream changes, the release tarball's pinned SHA-256, the
   tarballs' safe extraction and a release not pinned yet; the search
   engine definitions rebuilt from 156 and the MoltNet locale tables
   regenerated at .74; the 50 identical copies dropped from `src/`; and
   the compile check moving its work directory to a new pin and keeping a
   result only when the working tree held still.
2. The previous review's fixes: the leak kit's writes as root through the
   run directory's descriptor, a sandboxed page's nonce storage counted to
   its own site, a copy owed to a populated namespace dropped outside a
   message, the Background Fetch valve's marker.
3. A page's nonce-keyed state: its cookies deleted by partition and again
   a minute later, the clipboard salt derived for a key with a nonce, and
   a deletion of one storage key that reads that key's usage only, in
   each kind of profile.
4. `sessionStorage` jobs that wait only for their own namespaces, and a
   copy's wait of at most a second for its sites' turns: order,
   reentrancy, a namespace that goes during a job, a wait that never ends.
5. Background Fetch downloads taken up again while their parameters are
   asked for, in every profile.
6. MoltNet's microphone: the echo canceller of each mode, the platform
   effects masked, the output mix computed with the portable code while a
   MoltNet stream listens (and who can ask for it), the per-call portable
   checks of the audio math.
7. The leak kit and scripts: the `--out` directory's checks, the
   microphone page's playback cases, the iOS packaging script's signing.
8. The agent studio's step buttons.

**Method.** Each candidate traced from its attacker (web page, a group of
sites, a compromised renderer, a process of the user who runs the leak kit
as root, another local account, the download sources of the build tools)
through MoltBrowser's code and the upstream Chromium 156 code at the pinned
revision. The rebase was checked by comparing the lines each of the 591
patches adds and removes before and after it (the same for all), diffing
the nine merged files with 156.0.8078.74's, and comparing every DEPS whole
file's import with its subproject at 156.0.8078.48, and that with .74 (the
subprojects' mirrors and the release tarballs); the pinned SHA-256 with the
bucket's published hashes; and the search engine file with the rewrite of
156's. The namespace job order was exercised by a randomized standalone
test (system g++, ASan/UBSan) that checks that jobs sharing a namespace
never overlap and start in the order they came, and that every job ends.
The leak kit's paths as root were exercised with links, hard links and
swapped directories (standalone checks had the harness read a file only
root may read into its log, and a launch mount a file in a directory its
user can't reach), and its fixes tested with new tests and a mutant set
each catches, run as root and unprivileged; the tools' own test suites
and `molt_fuzz.py check` pass.

**Findings.** 18: 4 low, 14 informational. Fixed: the four low.

**Fixed.**

- The leak test kit, which runs as root, also reads the run directory it
  gives its user only from that directory's descriptor (its onion
  services' addresses, which must be addresses; MoltNet's torrc and Tor's
  consensus, inside the run directory only), mounts into a launch only
  its own `resolv.conf` and `nsswitch.conf` (made in its root-owned
  directory; the launcher refuses a link or another account's file), and
  when a run ends gives its user no file with more than one link. The
  `--out` directory is reached from `/` a component at a time without
  following a link on the way, but one only root could have put there
  (`tools/leak_test/leakkit/rundir.py`, `harness.py`, `launch_in_ns.sh`).

## 2026-10-28: PR #74 (October 2026)

**Scope.** The code merged in pull request #74 (round 30) that the
previous review didn't cover, in this order of priority:

1. MoltNet's Background Fetch with no total for all sites: each site's
   share, the memory valve told of each download past the floor, the
   in-memory download service with no limits of its own (in every
   off-the-record profile), the scheduler's new per-site queues and its
   hooks, starvation, counts that could drift or overflow, the controllers'
   lifetime; whether one site can hold unbounded memory or tasks before the
   valve acts, or end an identity at will.
2. Storage under a page's nonce: the tab helper's tracking (frames as they
   are made and as they commit, the page going, pages in the back/forward
   cache and prerendered ones), the bound of 32 groups per site, the drop
   in the storage service and the quota manager and who can ask for it,
   closed groups.
3. DOM storage in short turns: the usage walk and the memory dump, a tab's
   `sessionStorage` copied or deleted a chunk a task, control messages held
   and the order they are replayed in, reentrancy, a namespace or area that
   goes during a job, a message never replayed.
4. Per-site records forgotten: whether a site can reset its write
   allowance or commit rate by emptying its storage, or make records come
   and go to cost processor time; keepalive recorders and quota marks.
5. Local Network Access for Background Fetch requests made without the
   worker's factory, and fetches taken up after a restart, in every
   profile: the factory, the wait for a fetch's parameters, its deadline
   and the second request for them, with the previous review's restart; a
   permission prompt out of context; downloads made to fail for other
   sites.
6. MoltNet's microphone processing: the per-instance scope in WebRTC (other
   instances on one thread, components made later or on other threads),
   the setting carried over Mojo from either kind of profile, the residual
   echo estimator left out, the portable math calls and WebRTC's other
   vector code (the signal processing library, PFFFT).
7. The leak test kit: the microphone launch's flags and input file, and
   how the harness handles what pages answer.
8. The build tools: the compile check's result cache (stale or planted
   results), the HSTS generator built and run on the host, the paths
   `clean` removes.

**Method.** Each candidate traced from its attacker (web page, a group of
sites, a compromised renderer, a local process of the user who runs the
leak kit, a source edited during a check) through MoltBrowser's code and
the upstream Chromium 156 code at the pinned revision, and WebRTC's and
PFFFT's at the revisions it pins (read from the release's source tarball);
fixes tested with the leak kit's test suite and new tests that put links,
hard links, FIFOs and a moved run directory where the harness writes,
with a mutant set each test catches, run as root and unprivileged; the
nonce policy's unit tests built standalone (system gtest, g++
ASan/UBSan) with mutants they catch; the namespace harness of the
previous round (`SessionStorageImpl`'s and `SessionStorageNamespaceImpl`'s
copy and deletion verbatim, 156's and this branch's, clang ASan/UBSan)
with new tests of a copy owed to a populated namespace, in a message and
outside one, and mutants they catch, and a unit test of the storage
service's per-site host, which needs a Chromium build; the compile
check's test suite with a new test that edits a source while it
compiles, and mutants it catches; and the compile check against Chromium
156's headers.

**Findings.** 14: 3 low, 11 informational. Fixed: the three low and one
informational.

**Fixed.**

- The leak test kit: the harness, which runs as root, writes into the run
  directory it gives the invoking user only through a descriptor of that
  directory opened when it is made: no symbolic link is followed at any
  step of a path, only regular files with a single link are written or
  emptied, and owners and modes are set on the open file
  (`tools/leak_test/leakkit/rundir.py`). That covers the microphone
  launch's input file, every launch's and server's log, the profile's
  settings and the run's manifest; the servers' certificates and the test
  extension are made in the root-owned directory under `/run` and copied
  in.
- MoltNet: a page its server sandboxed (an opaque origin) counts toward the
  bound of the site its origin came from on how many pages keep storage
  under their nonce, rather than toward one bound that every site's
  sandboxed pages shared.
- MoltNet's DOM storage: a tab's `sessionStorage` deletion makes the
  copies still owed to tabs it opened, in a task of its own after its
  sites' turns. A copy owed to a namespace that was populated meanwhile is
  now dropped there without reporting a message, none being handled, as
  Chromium drops it while handling one (`session_storage_impl.cc`).
- The compile check: a file's result is stored under the key made from
  what preprocessing read, and the compile reads the working tree again.
  A result is now stored only if no C-family file of the working tree
  changed (size, modification time or inode) from before its key was made
  to the end of its compile (`ccheck/check.py`, `ccheck/sources.py`).

## 2026-10-27: PR #73 (October 2026)

**Scope.** The code merged in pull request #73 (rounds 28 and 29) that
the previous review didn't cover, in this order of priority:

1. MoltNet's DOM storage on threads of its own: the storage service's new
   binding methods, who can call them and for which partitions, the
   threads' lifetime and shutdown; the fair scheduler's charge in
   processor time and the control pipe's weight; notifications and frees
   split into short tasks (their order, an area that goes in the middle,
   reentrancy); unused empty areas dropped in their site's turn, and a
   binding that arrives meanwhile; and the in-memory HTTP cache's sweep in
   steps, its monotonic stamps, the table of sites made once with a seeded
   hash, its cap on sites, and the sweep's position kept across steps.
2. The previous review's fixes: what a resumed Background Fetch request
   carries, in every profile and on every path a download is resumed or
   started again, a later browser session included; the refusal of name
   lookups and host resolvers for MoltNet; and when the Background Fetch
   scheduler looks at the registrations waiting to become active.
3. MoltNet's processor requirement: every way into MoltNet; the JPEG XL
   decoder's portable mode (which processes turn it on, and when);
   half-float conversions, time-stretched media audio and its window's
   cosine.
4. The compile check's changes to sources: Windows paths and time
   functions, `raw_ptr` fields, special members defined out of the class
   body in headers, and the virtual hooks moved into `.cc` files, in
   component builds too.
5. The build tools as a supply chain: what the compile check and the
   static GN check download, how it is checked and unpacked, and what of
   it runs on the machine; the Linux repository tool's check of rpm
   signatures.
6. Regular profiles: that each MoltNet-only path is taken only in MoltNet.

**Method.** Each candidate traced from its attacker (web page,
third-party frame, a site's server, a group of sites, a download source,
the build machine's network) through MoltBrowser's code and the upstream
Chromium 156 code at the pinned revision, and V8's and jxl-rs's at the
revisions it pins; fixes tested with a standalone harness (system gtest,
g++ ASan/UBSan) that compiles the download service's changed functions
verbatim against stand-ins with 156's shapes, with a mutant set each test
catches, a unit test on the download service's real request parameters,
a DLL and an executable linked with Chromium's clang-cl and lld-link as a
Windows component build links them, the tools' own test suites and a
fetch of the pinned release's tarball, and the compile check against
Chromium 156's headers.

**Findings.** 13: 3 low, 10 informational. Fixed: the three low and two
informational.

**Fixed.**

- Background Fetch, every profile: the download service keeps a page's
  fetch across browser sessions, but not its initiator or network
  isolation, which Background Fetch gives back when the download starts
  again in a later session. A download whose item the browser kept from
  the earlier session was then resumed without them, as a request the
  browser makes itself (first-party at its destination, with
  credentials); it now starts again from the beginning with them, as a
  request with a body already did. A download started afresh in a later
  session keeps its first party on redirects, as in its first session.
  Other clients' downloads are unchanged.
- The build tools: Chromium's source tarball, which the static GN check
  and the compile check stream and whose scripts and DEPS files run on the
  machine, is used only if it matches a SHA-256 pinned in the repository,
  and none of its files is written outside its directory. The compile
  check builds its cxxbridge with the dependency versions the crate's lock
  file pins, not the newest ones crates.io has.
- Builds: the Blob registry's delegate is exported, so in a Windows
  component build libraries other than the storage one link against its
  method defined in the `.cc` file; and the web AI translation header's
  include of the inference engine's `raw_ptr` exclusion is allowed by its
  DEPS rules, which checkdeps had refused.

## 2026-10-26: PR #72 and round 28 (October 2026)

**Scope.** The code merged in pull request #72 (round 27) and round 28's
MoltNet changes, in this order of priority:

1. Background Fetch scheduled per site in MoltNet: the scheduler's
   containers while it activates and starts requests, the controllers'
   lifetime when a registration is aborted, a job finishes or its service
   worker goes away, starvation and whether one site can keep another's
   fetches from starting, the in-memory download service's limits, and
   regular profiles staying as Chromium has them.
2. DOM storage in MoltNet: the mode without a database, the write
   allowance, the storage service's sequence taken in turn per site
   (reentrancy, tasks of an area or site that is gone, the order callers
   rely on, a task that never runs), the per-item and per-area overhead
   in the totals, the groups of storage keys (nonces, opaque top-level
   sites), and the restore of a refused write's old value, which runs in
   every profile.
3. The in-memory HTTP cache's list per site: the second link in every
   entry on each doom, eviction and destruction path, the walks during
   eviction and the idle sweep, the check on a new entry's key, and sparse
   data's child entries.
4. The quota system's usage counted per site: after deletions, eviction
   and the end of an identity, and what a quota answer reads.
5. MoltNet's network context and sockets outside its own session: the
   socket factory with a session of its own, the WebRTC socket manager,
   raw sockets and multicast DNS, and the rest of the network context's
   interface at Chromium 156 (name lookups and host resolvers,
   WebTransport, proxy lookups, preconnects), with the browser features
   that ask for them.
6. What a Background Fetch request carries in every profile (initiator,
   network isolation, credentials, `Sec-Fetch-*`, `Origin` and `Referer`,
   SameSite and third-party cookie decisions, CORS, Local Network
   Access), on each path: the download service of regular profiles, the
   in-memory one of private profiles, registrations restored at startup,
   requests resumed or retried, and MoltNet's one-off key for a retried
   download.

**Method.** Each candidate traced from its attacker (web page, third-party
frame, a site and a server working together, a group of sites, local
user) through MoltBrowser's code and the upstream Chromium 156 code at the
pinned revision, through to the network service's request, header and
cookie decisions; fixes tested with standalone harnesses (system gtest,
g++ ASan/UBSan, clang `-fno-exceptions`) that compile the new header and
the changed scheduler function verbatim against stand-ins with 156's
shapes, with a mutant set each test catches, unit tests on Chromium's
real download request and network context, and the compile check against
Chromium 156's headers.

**Findings.** 10: 1 medium, 2 low, 7 informational. Fixed: the medium and
both low.

**Fixed.**

- Background Fetch, every profile: a request whose response stops early
  and is resumed carries its first request's initiator, network isolation
  and credentials mode, and keeps its first party on redirects. In a
  regular profile the download item that resumes it used none of them,
  so the resumed request was made as one the browser starts itself:
  first-party at its destination, with credentials whatever the fetch
  asked for. Other downloads resume as before.
- MoltNet: the network context looks up no name itself. Its name lookups
  and host resolvers, which some browser pages and services ask for, used
  the context's own host resolver outside Tor; they are now refused, so
  only Tor resolves names for MoltNet.
- MoltNet's Background Fetch scheduler looks at the registrations waiting
  to become active only when one is made or one finishes, not on every
  request's start and end, so a site keeping many waiting doesn't add to
  the work every site's requests cause in the browser.

## 2026-10-26: PRs #70 and #71 (October 2026)

**Scope.** The code merged in pull requests #70 and #71 (rounds 25 and
26), in this order of priority:

1. The end of a MoltNet identity: every way a request, a socket write or
   a name lookup could still leave once the end reaches a network context
   (URL loader factories, WebSockets, WebTransport, preconnects,
   prefetches, host resolution, the SOCKS refusal of writes and
   handshakes), contexts made after the end and after a network service
   restart, the browser's wait for every context and its 5-second
   deadline, the requests the browser holds for pages (keepalive,
   `fetchLater`, Background Fetch, downloads, Save Page As), New Identity
   while an end is under way, and a new identity's starting state.
2. The per-site budgets: the in-memory HTTP cache's shares, the
   `localStorage` and `sessionStorage` totals, cache groups and commit
   rates, the Background Fetch buffer counted against blob shares and its
   site keys, the storage quota share, and the memory valve: integer
   ranges, map growth, entry and area lifetimes, thread affinity, stale
   callbacks, what one site can make another see or lose, and who can
   make the valve end an identity.
3. The certificate-fetch pools and the split connection budget, and their
   checks against Tor's capacity.
4. The de-Google changes: the component registration guards and their
   switch, the on-device model manifest asked for on demand, the feature
   overrides, and the traffic-capture kit (its TLS key, the namespace set
   up as root, the parsing of what pages send).
5. Portable math and display capture in MoltNet: other ways to a picker or
   a capture (workers, frames, Permissions Policy, extensions), and the
   remaining C library calls a page can observe.
6. The one-click rule on every MoltBrowser page, with the handlers PR #71
   added.

**Method.** Each candidate traced from its attacker (web page, a third
party's script, a group of sites, a compromised WebUI script, local
environment) through MoltBrowser's code and the upstream Chromium 156 code
at the pinned revision; fixes tested with standalone harnesses (system
gtest, g++ ASan/UBSan, clang `-fno-exceptions`) that compile the hooked
code verbatim against stand-ins with 156's shapes, a mutant set each new
test catches, the earlier rounds' harnesses re-run on the changed code,
and the compile check against Chromium 156's headers.

**Findings.** 9: 1 medium, 2 low, 6 informational. Fixed: the medium and
both low.

**Fixed.**

- A credentialless iframe's storage key has a nonce and, in Chromium, its
  own origin's site as top-level site wherever it is embedded. MoltNet's
  per-site storage budgets now count such a key with the other keys of
  its nonce only (the credentialless frames of one page), not with that
  site: the storage quota share, and the `localStorage` and
  `sessionStorage` totals, cache groups and commit rates. A
  `localStorage` or `sessionStorage` area under an opaque top-level site
  counts on its own.
- MoltNet's in-memory HTTP cache counts a new entry's key against its
  site's share and the total like the data written into it: the site
  makes room from its own unused entries first, and an entry that still
  doesn't fit isn't made, so entries a site holds open can't take it past
  its share or the cache past its total.
- The `localStorage` and `sessionStorage` totals forget an area a page
  cleared safely when its site's other areas were forgotten first (their
  data deleted, for example by a `Clear-Site-Data` header): the site's
  entry, already gone, is no longer looked up.

## 2026-10-25: PRs #67-#69 (October 2026)

**Scope.** The code merged in pull requests #67 to #69 (rounds 22 to 24),
in this order of priority:

1. MoltNet's routing and isolation: the per-site socket pools, the
   connection and SOCKS handshake budgets shared through Tor's capacity,
   certificate fetches that carry their site, Background Fetch and download
   isolation (retries included), the end of an identity (the navigation
   throttle, the renderer shutdown, the refuse-all proxy configuration and
   its confirmation) and New Identity, keepalive and blob limits per site,
   one BrowsingInstance per renderer process, and the Incognito policies
   MoltNet now follows.
2. Memory safety and thread affinity in the new browser and network code:
   budgets and counts, raw pointers to pools, gates and the shared Tor
   capacity, the certificate-fetch thread-local, blob budget keys, the
   per-thread font cache invalidation, and checks reachable from renderer
   input.
3. Agent mode: fallback locators (the probe, the pin, weak matches and the
   question to the user), site-tool steps in recorded workflows, the tools of
   frames of other sites (the per-pair permission, counting without
   listing, the per-call question, the secret check on arguments), and the
   new WebUI handlers.
4. On-device AI: audio input for the Prompt API (the browser's checks, the
   service's re-checks, the projector allow-list read from the GGUF header,
   the local llama.cpp patch), the packed Vulkan shaders and the pipeline
   cache data, and the Windows GPU warm-up before the sandbox locks down.
5. MoltNet's bundled fonts: the read-only handles sent to renderers, the
   manifest, fetch and pin scripts, the SFNT reader in `tools/fonts`, and
   what happens when the set is missing, damaged or late.
6. A check of the new uniform values against cheap consistency probes
   (`navigator.deviceMemory` against the heap and data limits, the audio
   clock against `performance.now()`, the bundled fonts against each OS's
   text rendering).

**Method.** Each candidate traced from its attacker (web page, a group of
sites, a compromised WebUI script, an imported workflow, model or font
file, local environment) through MoltBrowser's code, the vendored llama.cpp
and the upstream Chromium 156 code at the pinned revision; fixes tested
with standalone harnesses (system gtest, `-fno-exceptions`) and a mutant
each new test catches, the Python tool tests, a libFuzzer run of every new
parser's target, and the compile check against Chromium 156's headers.

**Findings.** 7: 1 low, 6 informational. Fixed: the low and 1 of the
informational.

**Fixed.**

- Every MoltBrowser page that takes a click as the user's approval
  consumes the page's user activation (`molt_ai/common/molt_user_click.h`):
  the AI chat's agent "Allow", running a confirmed chat proposal, saving
  an agent run, the Agent studio's and the automation page's trust and
  "Confirm sites" buttons, the GPU setting and its "Try the GPU again" on
  `molt://ai-settings`, and the onion page's "Open in a MoltNet window",
  as the answers to a run's questions and to another site's frame's tools
  already did: one click allows at most one of these, whatever the page's
  script sends after it.
- The MoltNet popover's settings that decide where MoltNet traffic goes
  (exit country, on/off, mode) each need, and use up, one real click on the
  page, so a script there can't pin an exit country. Opening a MoltNet
  window and New Identity only add privacy and stay ungated. The page
  sends exactly one such message per click and none on load. The same
  settings in `molt://ai-settings` (MoltNet switch, exit country, bridges)
  and the AI chat handler's exit-country message follow the same rule.
- The font manifest's checks match whole values (a value with a final
  newline no longer passes).

## 2026-10-23: PRs #47-#52 (October 2026)

**Scope.** The code merged in pull requests #47 to #52, in this order of
priority:

1. Page translation on the device (PR #48): reading a page's text in
   Translate's isolated world and writing the model's translations back
   (markup and attribute injection, nodes the page moves or swaps,
   named-property shadowing, `translate="no"`, the work a hostile page can
   cause, "Show original"), Google's translate script and language list,
   and translation being off in MoltNet windows.
2. The web agent's planner (PR #47): the plan parser, the step numbers in
   actions, REPLAN, what a plan written from page text can change, and
   the plan's display.
3. MoltNet (PRs #49 and #51): the New Identity sequence, certificate
   fetches, HTTP authentication schemes, captive portals, the pinned
   preferences and the SOCKS username of sandboxed pages.
4. GPU inference (PR #52): the Linux sandbox additions and the switch that
   enables them, the exception boundary around llama.cpp, how GPU failures
   are told apart from model crashes, and the shader build tools.
5. The address bar's full-width `@ai` and `@memory` (PR #47) and the
   follow-ups of PR #50.

**Method.** Each candidate traced from its attacker (web page, model
output, model file, network, local user) through MoltBrowser's code, the
vendored llama.cpp and the upstream Chromium 152 code at the pinned
revision; fixes tested with the standalone stand-in harnesses (system
gtest, GCC `-Werror`, clang ASan/UBSan, `-fno-exceptions`) with a mutant
that each new test catches, a syntax pass of the browser code against
declaration stand-ins with the 152 signatures, and the libFuzzer target of
the code touched.

**Findings.** 8: 2 medium, 2 low, 4 informational. Fixed: 1 of the low.

**Fixed.**

- On-device AI: the browser accepts a GPU failure report (the inference
  service's `kBackendFailed`) only from a model that runs on the GPU. From
  a model on the CPU, where llama.cpp doesn't throw, it is a protocol
  violation that counts against the model like a crash, so the crash-loop
  guard applies and the GPU stays available to other models.

**Fixed since.** The other seven (both medium, the other low and the four
informational), in a follow-up the same day:

- MoltNet: the connections of each MoltNet identity carry a SOCKS password
  of their own, random for each MoltNet network context, so Tor never puts
  a new identity on an earlier one's circuits, after New Identity or once
  the last MoltNet window has closed, whether or not Tor has applied New
  Identity's request for new circuits (which it still gets).
- GPU inference on Linux (off by default): the inference service's GPU
  broker grants the sysfs files libdrm reads about the GPU, each by name,
  instead of the GPU's sysfs directory recursively.
- Page translation: the script reaches every DOM member through its
  prototype, so a page's named form controls or named elements can't stand
  in for them, and a slot whose check fails no longer ends the write; the
  15-minute deadline also ends a job waiting for a write the page doesn't
  answer; and a translation asked for in a MoltNet window no longer adds
  its language to that window's language list.
- The agent planner shows the soft hyphen, the Hangul fillers, the
  variation selectors and the tag characters as U+FFFD too.

Two functional issues found on the way are fixed as well: a textarea's
placeholder is now written back, and text directly inside a
`display: contents` element is translated.

## 2026-10-23: MoltNet's network isolation and linkability

**Scope.** MoltNet, MoltBrowser's Tor mode, against the claims in
`docs/MOLTNET_COMPLIANCE.md`:

1. Traffic that could leave a MoltNet window, or be caused by one, outside
   Tor: name resolution, UDP and QUIC, fetches the browser makes for a page
   (certificates, reporting, favicons, downloads, captive-portal checks,
   ...), workers and keepalive requests, extensions, internal pages, and
   MoltBrowser's own features (AI chat and agent, automation, memory,
   translation).
2. State that could link a MoltNet identity to the regular profile, one
   MoltNet session to the next, or one site to another within a session:
   network state and caches, settings and per-site exceptions, Tor circuit
   isolation and New Identity, MoltBrowser's own stores.
3. The Chromium 152 behaviour the compliance document relies on, and the
   coverage of the leak-test kit (`tools/leak_test/`).

**Method.** Each candidate traced from its trigger (page script, user
action, background task) through MoltBrowser's code and the upstream
Chromium 152 code at the pinned revision to the socket or the shared
state; Tor's control-port behaviour checked against Tor 0.4.9.11; fixes
tested with the standalone stand-in harnesses (gtest, ASan/UBSan,
`-fno-exceptions`) and mutants, and with new leak-kit cases checked against
its recorded sample run.

**Findings.** 9: 4 medium, 5 low. Fixed: 3 of the medium and 3 of the low.

**Fixed.**

- New Identity: the old identity's windows close even if a page asks
  "Leave site?", Tor is asked for new circuits only once the old profile is
  destroyed, and the new window opens only once Tor confirms that it has
  switched (`650 SIGNAL NEWNYM`), which can be up to ten seconds later. No
  other MoltNet window opens meanwhile.
- A MoltNet page always gets the prompt before an external application
  opens: the regular profile's "Always allow" exemptions are no longer read.
- Pages served sandboxed (opaque top-level origins) get per-site Tor
  circuits.
- MoltNet pages start at 100% zoom and never take the regular profile's
  zoom levels, also with fingerprinting protection switched off.
- The Tor control connection waits with `poll()` instead of `select()` on
  POSIX, and frames replies and asynchronous events separately.

**Fixed since.** The other three (the remaining medium and two low), in a
follow-up the same day:

- Certificate fetches made while checking a MoltNet site's certificate
  (missing intermediate certificates, revocation checks) each get a
  one-off network key: never cached, never merged with another fetch, and
  on a Tor circuit of their own.
- MoltNet windows don't check for captive portals and never open a
  network sign-in page.
- MoltNet windows offer sites Basic and Digest authentication only, never
  NTLM or Negotiate (Kerberos).

## 2026-10-22: PRs #26-#45 (October 2026)

**Scope.** The code merged in pull requests #26 to #45, in this order of
priority:

1. What any web page reaches: the built-in AI APIs (`molt_ai/web_ai/`:
   the manager and its binder, sessions, the consent prompt and remembered
   answers, limits and the scheduler, image input, what a page can learn),
   the web agent's site tools (WebMCP), page content entering the agent and
   the chat (screenshots, element snapshots, chat proposals, fencing) and
   the MoltNet fingerprinting changes (scrollbars, viewport lengths, text
   metrics, zoom letterboxing).
2. What a hostile model or a prompt-injected page reaches: every place a
   model reply becomes an action, a URL, markup, a file or a stored record.
3. Crafted input: `molt://memory/?q=`, the omnibox's `@memory` / `@ai`
   handling, imported workflows, and the build provenance and release
   verification tools.
4. The WebUI pages added or changed in those pull requests.

**Method.** Code reading with an attacker named for each candidate (web
page, model, file, network, local user), tracing input to outcome; the
upstream Chromium 152 code the changes rely on, read at the pinned
revision; regression tests against the standalone stand-in harnesses
(gtest), jsdom tests of the page scripts, the provenance test suite, and
the libFuzzer targets of the code touched.

**Findings.** 14: 1 medium, 8 low, 5 informational. Fixed: the medium and
6 of the low.

**Fixed.**

- The web agent's screenshots: the browser counts the frames the observed
  document owns and takes no screenshot when the page script that finds
  what to mask didn't find all of them.
- The built-in AI APIs: `create()` answers every user-specific unavailable
  reason alike; the consent bar ignores Allow and Block for half a second
  after it appears; a document's pending `create()` calls are bounded.
- The web agent's page scripts read a form's controls, attributes and
  action through the prototypes' accessors (site tools, the click probe).
- `molt_provenance.py verify` no longer reports success for files it
  doesn't cover, and compares a given statement file with its signed copy.

**Fixed since:** the remaining findings, in the follow-up to this review:

- The web agent takes no screenshot of a page with a frame of another
  origin anywhere in it (the browser counts them), nor while a frame of the
  tab has just navigated or gone away.
- The built-in AI APIs: how long `availability()` and `create()` take no
  longer depends on other sites' calls (device facts kept per site);
  sessions check the switches, the policies and the site's answer on every
  call and end when one turns them off; the consent bar ignores repeated
  rapid presses, as Chromium's dialogs do.
- MoltNet's uniform text rendering on Linux applies whatever the desktop's
  anti-aliasing setting.
- The release tools check names, versions and digests as whole ASCII
  values.
- The address bar's `@ai` and `@memory` shortcuts also match their
  full-width forms, as an East Asian input method types them (PR #47).
