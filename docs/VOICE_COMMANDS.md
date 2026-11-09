# What you can say: MoltBrowser voice commands

Voice mode lets you use MoltBrowser by voice alone (the design is
[docs/design/TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md)). This page
lists the commands the browser understands directly, without its AI model:
they work in a moment, even on a slow machine. Anything else you say goes to
the on-device AI, which works out what you mean from the page.

Voice mode is on by default (`chrome://flags/#molt-voice-mode` turns it
off). The microphone opens only when you start it.

## Turning voice mode on

1. Any of these turns listening on in a window:
   - the big **microphone on the home page** (the page a new window or tab
     opens on), under "Which site would you like me to open?". Voice mode
     says it is on, then asks which site to open: say the site's name
     alone, like "Facebook" or "YouTube", or "I want to go to Facebook".
     Well-known sites open at their official addresses (`www.facebook.com`
     for Facebook), from a built-in list; any other name is searched for
     on the web, never guessed as an address. "No thanks" or any other
     command ends the question;
   - the **Voice** button in the toolbar (it has a violet ring while the
     window listens; press it again to stop);
   - **Settings** (`molt://ai-settings`), "Voice mode": the switch listens
     in that window now, and **whenever MoltBrowser starts**, so a person
     who uses only their voice never needs to touch the computer after it
     is set up once. "Always listening" there makes every sentence a
     command, without "Hey Molt";
   - typing **"turn on voice mode"** in Agent mode's "Type what you would
     say" field.
2. The first time, your system may ask whether MoltBrowser may use the
   microphone: allow it. If it is refused, the browser says so aloud and
   on screen; allow it in the system's privacy settings, then turn voice
   mode on again.
3. You hear "Voice mode is on. Say Hey Molt, then what you want." A bar
   under the page shows what voice mode is doing (waiting for "Hey Molt",
   listening, thinking, speaking), what it heard as you speak, and what it
   is doing.

Voice mode can also be offered the first time MoltBrowser runs, so a
person who can only speak can start at once: with
`chrome://flags/#molt-voice-mode-at-first-run` also on (off until the
owner decides to ship it on), voice mode starts listening in the first window of the
browser's first run and, a few seconds after its own greeting, says "This
is MoltBrowser's voice mode, on because this is the browser's first run.
Say Hey Molt, then what you want, or what can I say. To turn it off, say
turn off voice mode." Settings are left as they were.

To stop listening, say **"turn off voice mode"** (the browser asks first,
because afterwards only the Voice button or settings turn it back on), or
press the Voice button. One window listens at a time: turning it on in
another window stops the first. Voice mode never listens in MoltNet
windows, and never opens the microphone for them.

Speech is recognized on this device by MoltBrowser's own speech engine,
sandboxed; nothing is recorded or sent anywhere. While you speak a command,
videos and music in your tabs are turned down. The browser cancels its own
sound (videos, pages) from the microphone, so a video saying "Hey Molt"
doesn't command it; while the browser itself is speaking, only "Hey Molt"
and "stop" are heard. A page reading aloud with the system's voices is
another matter: the browser can't cancel that voice, so while a page reads
aloud (and a second after) only "stop", "cancel" and "turn off voice mode"
are heard. The same goes for any tab's sound where the microphone has no
echo cancellation at all (the browser's turned off and the device has
none): while a tab plays sound, only those three are heard, so pause the
video yourself before you speak. On such a microphone the browser's own
voice is heard as it is too, so while it speaks, and a second after, only
those three are heard, "Hey Molt" included: say "stop", then what you want,
or wait until it has finished. Voice mode tells you so when it starts. Voice mode never cuts a page's reading
short: its own answer waits until the page has finished, and "stop" stops
only voice mode's speech. Each state has a short sound: a rising tone when it
heard "Hey Molt" and you paused (none when you go straight on, so it never
plays over your words), a tick when it took a command, a falling tone when it
goes back to waiting for "Hey Molt".

When the recognizer gets one word of a command wrong ("close this tip",
"Joe my downloads", "read the tile") or leaves a scrap of "Hey Molt" in
front of it ("halt. Play the live one"), voice mode takes it as the
command it sounds like, but only when nothing else fits (not a routine's
or a site's name) and when exactly one command fits. A swapped word works
only for commands that are safe on a guess: moving around, reading,
media, scrolling, zooming, tabs; never an answer to a question, a choice
from a list, sending a form, a setting, a routine or turning voice mode
off, and never the start of free text ("take hello there" isn't typed).
When only the scrap is dropped, the rest is what you said, so a press
works too ("Kalt, click empty the cart."), and asks first, as it would in
full, when it buys, deletes or sends; still never turning voice mode off,
forgetting what it learned, a protected setting, opening a downloaded
file, closing a window or removing a bookmark. Never while it waits for a
"yes" or "no". English only for now.

You can also type the commands, without the microphone, in Agent mode's
"Type what you would say": the browser does it in the tab you are on and
says the answer aloud with your system's voices.

On Linux, spoken answers need speech-dispatcher, and the browser started
with `--enable-speech-dispatcher`; without them it answers in the panel
and the bar only.

## What it does

- **"Open YouTube"** opens the site. The search engines (YouTube, Google,
  DuckDuckGo, Wikipedia, Brave Search, Bing) and 38 popular sites
  (Facebook, Instagram, WhatsApp, X, Amazon, Netflix, Gmail and others)
  are known by name, at their official addresses. A spoken address
  ("example dot com") opens that address. Any other name is searched for
  with your default search engine, and the browser tells you so: it never
  guesses a web address.
- **"Search for ..."** types into the page's own search box and submits it;
  on a page without one, it uses the site's search page, or your default
  search engine. Then it reads the results: how many, the first five with
  their channel or site and length, and "Say a number or a name". "Be brief"
  reads only how many; "tell me everything" reads all of them.
- **On YouTube, Google, DuckDuckGo, Bing and Wikipedia** the browser knows
  where the search box, the results, the player's buttons and the next page
  are, so "search YouTube for ..." (while you are on YouTube) types into
  YouTube's own box, the results are read as the site shows them ("1: Queen
  – Bohemian Rhapsody, Queen Official, 6 minutes"), "skip the ad" presses
  the player's skip button once it shows, and "next page" goes to the next
  page of results (on YouTube, which loads more as you scroll, it scrolls).
  If one of these sites changes so that the browser can't find a part any
  more, it finds it the way it does on any other site.
- **Choosing:** "the fourth one", "number four", "four" right after a list,
  "the live one by Queen". If several fit, the browser reads them back
  numbered ("Did you mean 1: ..., 2: ...?") and you answer with a number or
  a name. "Play" or "open" a video opens it and plays it with sound.
- **Before anything that can't be undone,** such as buying, paying,
  deleting, sending, posting, subscribing, signing out or sending a form,
  the browser asks "Say yes to press ..." and does it only if your next
  words are "yes". Ticking a box or choosing an option never asks ("check
  send me the newsletter"): only the button that sends the form does.
- **Lists to choose from** (a country, a size): say the list's name
  ("click country") and the browser reads its options; then "choose
  France". "Choose France" alone works too when only one list on the page
  has France.
- **When a page tries something after the browser clicks for you,** such
  as opening another site in a new window, downloading a file or opening
  another app, the browser asks first: "The page tried to open a new
  window to example.com. Say yes to open it, or no to cancel." It happens
  only if your next words are "yes". A page that tries to go full screen
  (unless you said "full screen" or "play") or to change your clipboard is
  stopped. The browser tells you each time. A permission it asks for, such
  as your location, is read to you like any other (below).
- **Questions from the browser and from pages** are read to you and
  answered by voice:
  - a permission ("example.com wants to use your camera. To allow it, say
    harbor. To block it, say no.");
  - a page's message, question or request for text ("example.com asks:
    Empty the cart? Say yes or no.");
  - leaving a page with unsaved changes ("Say leave or stay");
  - a downloaded file that may be harmful ("To keep it, say harbor. To
    delete it, say discard.");
  - saving a password ("Say save, never, or not now"). Your password is
    never read out.

  To allow a permission or keep a risky file, say the word the browser
  gives you. It is a new word each time, so a recording or a sound from a
  page can't say it for you; say it once it has finished speaking, in a
  quiet moment. A different word turns the request down. If what it heard
  could be one of two words, it says "I didn't catch that. Say the word
  again." and keeps waiting. "No" always refuses.

  The word is meant for you alone. The browser says it aloud only when
  nothing else can hear it: its own voice is speaking, your microphone
  has echo cancellation, and no page is using a microphone. Otherwise it
  shows the word in large letters at the end of the bar under the page
  and says "To allow it, say the word on your screen." While your screen
  or a window is shared (Cast's screen mirroring included), it gives no
  word at all ("I can't take the word while your screen is shared"):
  allow it with the mouse or keyboard, or say "no". If a page starts using
  a microphone after the word was said, or the screen is shared after it
  was shown, the browser changes the word and tells you; saying the old
  one then does nothing. When the page's own text was read out after the
  question (an answer about the page, a reading), the browser reads its
  question again before it takes your word or your "yes" ("That was the
  page's text. Here is my question: ..."), so a page can't pose as the
  browser. If you can't
  see the word, turn on "Say the confirmation word aloud even when
  another program could hear it" in Settings, "Voice mode": the browser
  then always says it, and a page or program listening could hear it
  too. A risky file is kept
  just as the downloads button's warning keeps it. When a page wants text, say "answer" and your answer, then
  "yes".
- **Typing:** choose a field ("click the search box"), then "type ...",
  "delete the last word", "clear the field", "read it back", "submit".
  Password, card and code fields are never typed into or read out.
  Punctuation is said as words: "type see you at noon comma Ana period"
  types "see you at noon, Ana." ("comma", "period" or "full stop",
  "question mark", "exclamation mark", "colon", "semicolon", "new line",
  "new paragraph", "open quote" and "close quote", "open paren" and "close
  paren", "hyphen", "dash", "apostrophe", "ellipsis"). "The word comma"
  types the word itself. After a period, a question mark or a new line,
  the next word starts with a capital.
- **Media:** "play", "pause", "resume", "louder" and "quieter" (with the
  player's volume control), "mute", "skip ahead one minute", "full screen",
  "turn on captions", "skip the ad". These work on a player in a frame of
  another site too (an embedded video), and "next video" where the player
  says it has one.
  - **The volume to a level:** "volume 50 percent", "set the volume to
    half", "maximum volume", "volume 3" (on a scale of ten). The browser
    sets the video's own volume and says "Volume 50 percent."; when the tab
    is muted it adds "The tab is muted: say unmute to hear it."
  - **The playback speed:** "speed 1.5", "play at double speed", "half
    speed", "speed one and a half", from a quarter to 4 times; "faster"
    and "slower" step through 0.25, 0.5, 0.75, normal, 1.25, 1.5, 1.75 and
    2 times (while the browser reads a page aloud, they are its reading
    speed, as before); "normal speed". It says "Speed: 1.5 times." or
    "Normal speed."; a player that refuses a speed is heard as "This player
    doesn't go to that speed. Speed: 2 times."
  - **Captions:** the player's captions button when it has one; otherwise
    the video's own captions or subtitles ("Captions on.", "Captions off.",
    or "I can't find captions for this video.").
  - With no video or audio on the page: "There's no video or audio on this
    page."
- **Undo:** "undo" puts back what voice mode did last, and says so:
  - a tab it closed is reopened ("Undone: Weather is open again.");
  - what it typed, cleared or deleted in a field is put back ("Undone:
    Search says Bohemian Rhapsody again.", or "Undone: Search is empty
    again.");
  - a page it opened (a site, a search, a link, a result) goes back to the
    page before ("Undone: back to the page before.", then the page's
    title); after "go back", it goes forward again;
  - a bookmark it added is removed, one it removed put back ("Undone: the
    bookmark is removed.", "Undone: the bookmark is back.").

  Only the last thing counts. If the page it was on has changed since:
  "I can't undo the last thing I did any more: the page changed since."
  While the page it opened is still loading: "Wait until the page has
  opened, then say undo." With nothing to undo: "There's nothing I can
  undo." What you did yourself, and what can't be taken back (a form sent,
  a purchase), is never undone.
- **Start over:** "start over" forgets the list being chosen from, the
  question the browser asked and the field being typed into, closes a menu
  that is open, and listens again: "Starting over. What would you like to
  do?" (with "A question from the browser is still waiting for your
  answer." when a permission or a page's dialog still waits). While a video
  or a song plays, "start over" plays it from the beginning, as before.
- **"Where am I?"** says the site and the page, the tab, and what you are
  in: "You're in the list of bookmarks, at 2 of 5.", "You're typing in
  Search.", "Something is playing.", "The browser's menu is open: say what
  to press, or close the menu.", "A question from the browser is waiting
  for your answer." **"What can I say?"** puts first what fits: the menu,
  the list, a reading, a player, and in a field the punctuation words.
- **Find on the page:** "find opening hours on this page" says where it is
  first and, when it is in more places, "Say next match to go on."; "next
  match" and "previous match" go through them ("Match 2 of 3: the link
  Opening hours."). A word finds its plural, and a word the recognizer
  split or joined finds it as the page writes it ("time table" finds
  "Timetable").
- **A site's name on its own:** "YouTube" (or "Wikipedia", "Google",
  "DuckDuckGo", "Bing", "Brave Search") opens it, saying "Opening YouTube."
  then "YouTube is open."; so does the name of a site you opened by voice
  earlier in this window ("BBC" after "open bbc dot co dot uk"). Any other
  name on its own goes to the on-device AI as before.
- **Reading aloud:** "read the page" reads the page's text, not its menus:
  the article, from its title on, without the navigation, the search box,
  forms, tables or ads in frames. "Read the section on history" reads that
  section; "read the selection" what you selected; "read from here" from
  the item you are on. "Next heading" and "previous heading" say the
  heading, "next paragraph" reads the next paragraph; while the browser
  reads, they jump there and go on reading. "Stop", "pause" or any new
  command stops it where it is, and "continue reading" goes on from there.
  "Faster" and "slower" while it reads change its speed, which it keeps.
  The bar under the page shows the sentence being read, and the page
  scrolls along.
- **Reading lists:** the title, the headings, the links, the results, "next
  link", "next item", "where am I", "what's on the screen".
- **Questions about the page:** "what is this page about?", "read me the
  main points", "what does it say about the battery?", "who wrote this?",
  "what's the price?". The on-device AI answers from the page's text (the
  same text "read the page" reads), in a few sentences, starting with "The
  page says". If the page doesn't say, the browser tells you so. "Tell me
  more" goes on with the next part of the page. It needs an on-device AI
  model: without one, the browser says so and where to download one
  (settings). Answering can take a few seconds on a slower computer; the
  browser says "Let me look at the page" and, if it takes longer, "Still
  working on it". "Stop", or anything else you say, stops it. The answer
  is only said: nothing on the page is clicked or opened because of it, and
  if a page's text tries to make the AI say a voice command, the browser
  doesn't say the answer and tells you why. Nothing is kept, and it never
  happens in MoltNet windows.
- **Routines:** do something by voice ("open YouTube", "search for news",
  "play the first one"), then say "remember this as my news". The
  browser reads back what it will keep and saves it on your "yes". Later,
  "run my news", or just "my news", does it again, saying each step; it
  still asks before anything that can't be undone, and "stop" ends it.
  A choice made by position ("the first one") picks the same position
  next time; one made by name picks by name. A text that looks like a
  password or a code isn't kept: the browser asks for it at each run.
  "What are my routines?" lists them; "forget my news" deletes one after
  your "yes". Routines are listed in Agent mode too, which can't run them.
- **Routines belong to your profile:** a routine is kept in the browser
  profile you taught it in. Another profile on the same computer (someone
  else's, or your work profile) doesn't hear it listed, can't run or
  forget it, and doesn't see it in Agent mode; each profile can have a
  "my news" of its own. Saying "remember this as my news" when this
  profile already has one asks first ("You already have a routine called
  my news. ... Say yes to replace it"): "yes" replaces it, "no" keeps the
  old one and asks you for another name. Each name runs exactly one
  routine. A routine you import, copy or rename in Agent mode under a
  name you already use is called "my news (2)" (say "my news two").
- **Routines from before this update** were shared by every profile. The
  first time you ask about routines, or run one of them by name, the
  browser asks: "2 routines were made before routines belonged to a
  profile: my news and morning mail. Say yes to keep them here". "Yes"
  moves them into this profile (and no other profile has them after
  that); "no" leaves them for another profile and isn't asked again
  here. Agent mode shows them too, to keep some and not others.
- **Routines with the AI's steps:** when you asked the on-device AI to do
  something ("find me a train to Lyon"), what it did is part of "this" too:
  the pages it opened, what it clicked, typed or chose. Saving such a
  routine says which sites it uses, and the first time you run it, you
  confirm them: say the word the browser shows ("say harbor"), or confirm
  them in Agent mode. Running it again repeats the AI's steps exactly, on
  those sites only, without asking the AI again; it asks before anything
  that can't be undone, and stops at a step it can't find ("I couldn't
  find Search on this page."). Text the AI typed that looks like a
  password or a code isn't kept; some steps can't be kept at all (a site's
  own tool, a card number), and then "remember this" says so. While
  "Learn from what I say" is off, the AI's steps aren't kept.
- **Learning your words:** names you say often, and your corrections
  ("no, I said Queen" after the browser heard "Green"), help it hear you
  next time. Nothing is learned in MoltNet or private windows, from what
  you dictate into a field, or while the browser reads a page aloud.
  Settings, "What voice mode learned", lists everything and deletes it;
  "Learn from what I say" turns it off. By voice: "forget the word
  Queen", "forget everything you learned".
- **The browser itself:**
  - **Bookmarks:** "bookmark this page" ("Bookmarked Cats - VideoSite.",
    or "This page is already bookmarked."), "remove this bookmark"
    ("Removed this page's bookmark. Say undo to put it back."), "open my
    bookmarks" (the bookmark manager opens, and the browser reads them: "5
    bookmarks. 1: BBC News, bbc.co.uk, 2: ... Say a number or a name."),
    then a number or a name opens one; "open the bookmark news" opens it
    at once, or asks which when several fit ("I can't find trains in your
    bookmarks." when none does).
  - **History:** "show my history" (the history page opens, and the
    newest pages are read to choose from), "open the weather from my
    history", "reopen the closed tab" ("Reopened Weather.", or "There's no
    closed tab to reopen."). Private windows keep no history: "Private
    windows keep no history."
  - **Downloads:** "show my downloads" (the downloads page opens, and they
    are read, newest first), "open the last download", "open download 3".
    A file that could run programs on your computer (a program, an
    installer, a script, a shortcut, a disk image) is never opened by
    voice: "tool.bat can run programs on your computer, so I don't open it
    by voice. Open it from the downloads page with the mouse or
    keyboard." Nor is a file blocked as dangerous, or one still
    downloading.
  - **Settings:** "open settings", "open privacy settings", "open the AI
    settings", "open voice mode settings" (voice mode's section), "open
    MoltNet settings". The browser can read a settings page to you, but
    changing a setting there needs the mouse or keyboard: "I can read
    settings to you, but changing one there needs the mouse or keyboard."
    Voice mode's own settings it changes by voice: "speak faster", "be
    brief", and "turn on always listening", which asks first ("With
    always listening on, everything you say is a command, without Hey
    Molt, also what others nearby say. Say yes to turn on always
    listening, or no to cancel."); "turn off always listening" at once.
    Settings that weaken a protection stay a click in settings: "say the
    word aloud" and "trust this workflow" only hear where to click.
  - **Windows:** "new window" and "new private window" open one, and voice
    mode stays in this window ("New window. I'm still listening in this
    one: say switch to the other window to use it by voice."); "new
    MoltNet window" opens one, where voice mode never listens. "Switch to
    the other window" brings that window forward and voice mode with it
    ("I'm listening in this window now: <its page>."); never to a MoltNet
    window. "Close this window" asks first, says how many tabs it has, and
    moves voice mode to your other window; closing the only window closes
    MoltBrowser, and the browser says so first.
  - **The AI chat:** "ask the AI how far away is the moon" asks the
    on-device AI ("Let me think.", then "The AI says: ..."). Its answer is
    read to you and nothing is done because of it; an answer holding
    something that sounds like a voice command is left out ("The AI's
    answer held something that sounds like a voice command, so I left it
    out."). Without an on-device AI model, the browser says how to get one.
    "Open the AI chat" opens the chat beside the page.
  - **The browser's own buttons and menus:** "open the menu" opens the
    browser's menu and reads its items ("12 menu items. 1: New tab, ...");
    say a number or a name ("click Settings", "History") to press one, and
    a submenu is read when it opens. "Close the menu" or "cancel" closes
    it. "Show the browser's buttons" reads the toolbar, the tabs, the
    bookmarks bar and the side panel's buttons to press by number or name.
    Never a permission's request, a download's warning or the password
    bubble (they have their own spoken prompts), and never the Voice
    button ("turn off voice mode" asks first). Pressing the menu's "Exit"
    asks first.
- **"Stop"** stops the browser speaking and whatever it was doing; a new
  command does too. "Repeat" says the last answer again.

## How to talk to it

- **Start with the wake phrase** ("Hey Molt", or the one you chose), unless
  the browser is already listening: right after it answers, or when it has
  asked you something.
- **Say it your way.** "Please", "could you", "I want to", "um" and "thank
  you" are fine. Upper or lower case and punctuation don't matter.
- **Numbers** can be said or written as digits: "the fourth one", "the 4th
  one", "number four", "No. 4". Where a number is expected, sound-alikes such
  as "number for" or "tab too" count too. "The last one", "the second to
  last" and "the one before last" work as well.
- **Times** are said as you would: "30 seconds", "a minute and a half",
  "1 minute 30 seconds".
- **Addresses** can be spoken: "example dot com", "w w w dot bbc dot co dot
  uk". A site's name works too: "open YouTube".
- **Choosing from a list**: after the browser reads a list, or with numbers
  shown on the page, say a number ("four"), a position ("the last one") or
  what the item says ("the live one by Queen"). If two items fit, the
  browser asks which one you meant.
- **If it heard you wrong**, say "no, I said ..." and what you meant:
  the whole command ("no, I said open YouTube") or only the words it
  misheard ("no, I said Queen"). It does what you meant and, when the two
  sound alike, remembers the correction for next time.
  "Cancel", "stop" and "go back" always work.
- **"What can I say?"** lists what fits the page you are on.

A command that would buy, send, post, delete or share something is read back
first and waits for your "yes". Text a web page shows or plays is never taken
as a command.

## Reading the lists below

Each command shows a few ways to say it:

- `(open | go to)`: say one of these.
- `[the | a]`: these words may be left out.
- `{site}`: a site's name or spoken address. `{address}`: a spoken address
  only ("example dot com").
- `{query}`, `{text}`, `{name}`: your own words (a search, text to type, a
  routine's name, a section's name).
- `{seconds}`: a length of time; `{seconds?}` may be left out.
- `{level}`: a volume, from 0 to 10, as a percentage, or "half" or "max".
- `{speed}`: a playback speed: "1.5", "one point five", "one and a half",
  "2x", "two times", "double", "half".
- `{pages?}`: how far to scroll ("a bit", "half a page", "a page", "3
  pages"), or nothing.
- `{position}`: a position or number on its own ("the fourth one", "number
  12", "the next one"). `{number}`: the same, or a bare number ("four").
- `{choice}`: anything that picks an item: a position, a number, or what it
  says ("the live one by Queen", "the sign-in button").
- `{pick}`: a choice said on its own right after a list: a number or a
  short description.
- `{tab}`: a tab by number, position or what it shows ("the YouTube tab").

Words are shown the way the browser compares them: lower case, without
apostrophes ("whats" is "what's", "dont" is "don't"). The browser tries the
forms in the order listed and takes the first that fits all of what you
said.

<!-- BEGIN GENERATED: tools/voice/voice_commands_doc.py --write -->

## Going places

**Open a site by its name or its address.** For example: "open YouTube".

- `(open | go to | take me to | visit | navigate to | launch | load | bring up | pull up) {site} (in | and) [a] new tab`
- `(open | go to | take me to | visit | navigate to | launch | load | bring up | pull up) {site}`
- `hoping {site}`
- `{address}`
- `{site}` (when the browser has asked which site to open)

**Search: on the site you are on (or the search engine when it has no search box), the web, or a site you name.** For example: "search for Bohemian Rhapsody".

- `search (the web | the internet | online | the net) for {query}`
- `(web search | internet search | search online for) {query}`
- `search (this site | this website | here) for {query}`
- `search for {query} (on this site | on this website | here)`
- `search {site} for {query}`
- `(search for | search | look up | look for) {query} (on | in) {site}`
- `(search for | search | look up | look for) {query}`

**Find words on this page.** For example: "find opening hours on this page".

- `find [the word] {query} (on | in) (this | the) page`
- `fine [the word] {query} (on | in) (this | the) page`
- `(find on page | find in page | find on this page) {query}`
- `search (this | the) page for {query}`

**The next place the words were found.** For example: "next match".

- `[go to | jump to | show | show me] [the] (next | following) (match | occurrence | hit | one i found)`
- `find (next | again | the next one)`

**The place before, where the words were found.** For example: "previous match".

- `[go to | jump to | show | show me] [the] previous (match | occurrence | hit)`
- `find (previous | the previous one)`

**Go back to the previous page.** For example: "go back".

- `(go | navigate | take me) back [a page | one page | to the (previous | last) page]`
- `(back | previous page | last page | go to the previous page)`
- `feel back`

**Go forward again.** For example: "go forward".

- `(go | navigate) forward [a page | one page]`
- `forward`
- `feel forward`

**Load the page again.** For example: "reload".

- `(reload | refresh) [the | this] [page | tab | site | website | it]`
- `load [the | this] page again`
- `(roulette | roledge | rolete)`

**Stop loading the page.** For example: "stop loading".

- `stop loading [the | this] [page | site | tab]`
- `stop the page [from] loading`

**Go to your home page.** For example: "go home".

- `(go home | take me home | home | home page | homepage | go to [the] home page | open [the] home page)`

**The next page of results.** For example: "next page".

- `[go to] [the] next page [of results]`
- `(more results | show more results | load more results)`

**Open a new tab.** For example: "open a new tab".

- `(open | new | start | create) [a] new tab`
- `(open | start | create) [a] new tip`
- `(new tab | open [a] tab | open another tab)`

**Close this tab, or a tab by number or name.** For example: "close this tab".

- `close [this | the | current] tab`
- `close (this | the) ted`
- `close tab {tab}`
- `close [the] {tab} tab`

**Switch tabs: by number, position or name, or the next one.** For example: "switch to tab 2".

- `[go to | switch to | move to] [the] next tab`
- `[go to | switch to | move to] [the] previous tab`
- `[go to | switch to | move to] [the] previous tomb`
- `(switch | go | change | move | jump) to (tab | tip) {tab}`
- `tab {tab}`
- `(switch | go | change | move | jump) to [the] {tab} tab`
- `switch to {tab}`

## Viewing

**Scroll down: a bit, half a page, a page, or several.** For example: "scroll down".

- `page down`
- `(scroll | go | move) down {pages?}`
- `(scroll | keep scrolling | scroll more | down) {pages?}`

**Scroll up.** For example: "scroll up a page".

- `page up`
- `(scroll | go | move) up {pages?}`
- `up {pages?}`

**Scroll left.** For example: "scroll left".

- `scroll left`

**Scroll right.** For example: "scroll right".

- `scroll right`

**Go to the top of the page.** For example: "scroll to the top".

- `(scroll | roll | go | jump | move | go back) [up] to the (top | beginning | start) [of the page]`
- `(scroll | roll) [up] to the tar`
- `(top of [the] page | page top | scroll top | back to top)`

**Go to the bottom of the page.** For example: "scroll to the bottom".

- `(scroll | roll | girl | go | jump | move) [down] to the (bottom | end) [of the page]`
- `(bottom of [the] page | page bottom | scroll bottom)`
- `(scroll | roll) [down] to the borrow`

**Make everything bigger.** For example: "zoom in".

- `(zoom in | zoom in more | magnify | enlarge | increase [the] zoom | (bigger | larger) [text] | make (it | the text | text | everything | the page) (bigger | larger))`

**Make everything smaller.** For example: "zoom out".

- `(zoom out | zoom out more | shrink | decrease [the] zoom | smaller [text] | make (it | the text | text | everything | the page) smaller)`

**Back to the normal size.** For example: "reset zoom".

- `((reset | rosetta | bosetta | for setta | normal | default) zoom | zoom (reset | normal | to normal | back to normal) | (actual | normal | original | default) size | reset [the] zoom level)`
- `(resetzum | rosetzum | resetzoom)`

## Media

**Play.** For example: "play".

- `(play | hit play | press play | start playing | start playback) [it | this | that | the video | the song | the music | the media | the movie]`

**Pause.** For example: "pause".

- `stop (the video | the music | the song | playback | playing | the movie | the audio)`
- `(pause | hold on) [it | this | that | the video | the music | the song | playback | the movie | the audio | reading | the reading]`
- `(pores | paws) [it | this | that | the video | the music | the song]`

**Carry on playing.** For example: "resume".

- `(resume | unpause | continue | keep playing | continue playing | carry on) [it | playback | the video | the music | the song | playing]`

**Louder.** For example: "louder".

- `(louder | volume up | turn it up | turn up the volume | turn the volume up | turn up | more volume | make it louder | crank it up | pump it up | (increase | raise) [the] volume | a (bit | little) louder | much louder)`

**Quieter.** For example: "quieter".

- `(quieter | softer | volume down | turn it down | turn down the volume | turn the volume down | turn down | less volume | make it quieter | lower | (decrease | lower | reduce) [the] volume | a (bit | little) quieter | much quieter)`

**Set the volume: 0 to 10, a percentage, half or max.** For example: "set the volume to 5".

- `[set | turn | change | put] [the] volume [to | at] {level}`
- `{level} volume`
- `said [the] volume (to | at) {level}`

**Mute the sound.** For example: "mute".

- `mute [it | this | everything | the (sound | audio | video | tab | volume | music)]`
- `((sound | audio) off | turn (off | down) the sound | turn the sound off)`

**Turn the sound back on.** For example: "unmute".

- `unmute [it | this | everything | the (sound | audio | video | tab | volume | music)]`
- `((sound | audio) on | turn on the sound | turn the sound [back] on)`

**Skip ahead, by a step or by how long you say.** For example: "skip ahead one minute".

- `(go | skip | jump | move | seek) (ahead | forward | forwards) [by] {seconds}`
- `(skip | jump | seek) (ahead | forward | forwards) [by] {seconds?}`
- `fast forward [by] {seconds?}`
- `(skip | jump) [by] {seconds?}`
- `forward {seconds}`

**Go back in the video.** For example: "go back 10 seconds".

- `(go | skip | jump | move | seek) (back | backward | backwards) [by] {seconds}`
- `(skip | jump | seek) (back | backward | backwards) [by] {seconds?}`
- `rewind [by] {seconds?}`
- `back {seconds}`

**Play from the beginning.** For example: "play from the beginning".

- `(restart | replay | play (it | this | that) again | play again | watch again | from the (beginning | start | top) | start from the (beginning | start) | play from the (beginning | start) | restart the video | rewind to the (beginning | start))`

**The next video or song.** For example: "next video".

- `[play | skip to | go to] [the] next (video | song | track | episode | clip)`
- `skip (this | the) (video | song | track | episode | clip)`

**The previous video or song.** For example: "previous song".

- `[play | go to | go back to] [the] previous (video | song | track | episode | clip)`

**Full screen.** For example: "full screen".

- `[go | enter | make it | switch to | turn on | go to] (full screen | fullscreen) [mode]`
- `maximize [the video | it | the player]`

**Leave full screen.** For example: "exit full screen".

- `(exit | leave | close | quit | end | turn off | get out of | cancel) (full screen | fullscreen) [mode]`
- `((small | normal | regular) screen | minimize [the video | it | the player])`

**Show captions.** For example: "turn on captions".

- `(turn on | show | enable | switch on | start | put on) [the] (captions | subtitles | closed captions | cc | subtitle)`
- `(captions | subtitles | closed captions | cc) on`
- `turn on [the] catchens`

**Hide captions.** For example: "captions off".

- `(turn off | hide | disable | switch off | stop | remove | take off) [the] (captions | subtitles | closed captions | cc | subtitle)`
- `((captions | subtitles | closed captions | cc) off | no (captions | subtitles))`
- `(captions | subtitles) (of | wolf | both)`

**Play faster.** For example: "speed up".

- `(speed up | faster | play faster | go faster | speed it up | increase [the] [playback] speed)`

**Play slower.** For example: "slow down".

- `(slow down | slower | play slower | go slower | slow it down | (decrease | reduce) [the] [playback] speed)`

**Back to normal speed.** For example: "normal speed".

- `((normal | regular | default | usual) [playback] speed | reset [the] [playback] speed | speed (normal | to normal | back to normal) | play at normal speed)`

**Set the playback speed: 1.5, one and a half, double, half, from a quarter to 4 times.** For example: "speed 1.5".

- `[set | change | put | turn] [the] [playback | video] speed [to | at] {speed}`
- `(play | watch | listen | go) at {speed} [speed]`
- `(play | watch | listen) [it] {speed} (faster | speed)`
- `{speed} speed`

**Skip the ad, once the player lets you.** For example: "skip the ad".

- `skip [the | this | these] (ad | ads | advert | adverts | advertisement | advertisements | commercial)`
- `skip (the | this) ed`

## Reading

**Read the page aloud.** For example: "read the page".

- `read [the | this] [whole | entire] (page | article | story | text | post | content) [aloud | out loud | to me]`
- `(read | start reading | begin reading) [it | this | everything | all | everything on the page] [aloud | out loud | to me]`

**Read on from where you are.** For example: "read from here".

- `(read from here | read on | read the rest [of the page] | read from this point | (continue | keep | resume | carry on) reading)`

**Read the headings.** For example: "read the headings".

- `(read | list | say | tell me | what are) [the | all] [the] (headings | section headings | sections | subheadings)`
- `headings`

**Read the links.** For example: "read the links".

- `(read | list | say | tell me | what are) [the | all] [the] links`

**Read the list of results again.** For example: "read the results".

- `(read | list | say | tell me | what are) [the | all] [the] (results | search results | options | choices | items | list | videos | stories | articles | products) [again]`
- `what are my (options | choices)`

**Read the page's title.** For example: "read the title".

- `(read | say | tell me) the (title | page title | title of the page | name of the page | page name)`
- `read the tile`
- `(what is | whats) the (title | page title | title of the page | name of (this | the) page)`

**Read the selected text.** For example: "read the selection".

- `read [the | my] (selection | selected text | highlighted text | highlighted part | highlight)`
- `read (what is | whats | what i) (selected | highlighted)`

**Describe what is on the screen.** For example: "what's on the screen".

- `((whats | what is) on [the | my | this | a] (screen | page) | what do you see | what can you see | describe [the | this] (screen | page | view) | (whats | what is) here)`

**Move to the next heading.** For example: "next heading".

- `[go to | jump to | skip to | move to | read] [the] next (heading | section | header)`

**Move to the previous heading.** For example: "previous heading".

- `[go to | jump to | skip to | move to | read] [the] previous (heading | section | header)`

**Move to the next link.** For example: "next link".

- `[go to | jump to | move to | read] [the] next link`

**Move to the previous link.** For example: "previous link".

- `[go to | jump to | move to | read] [the] previous link`

**Move to the next item of the list.** For example: "next item".

- `[go to | jump to | move to | read] [the] next (item | result | entry | element)`
- `next`

**Move to the previous item.** For example: "previous item".

- `[go to | jump to | move to | read] [the] previous (item | result | entry | element)`
- `previous`

**Read the next paragraph.** For example: "next paragraph".

- `[go to | jump to | move to | read] [the] next paragraph`

**Read the paragraph before.** For example: "previous paragraph".

- `[go to | jump to | move to | read] [the] previous paragraph`

**Read one section of the page, by its heading.** For example: "read the section on history".

- `(read | read me | go to | jump to | skip to | move to) [the] (section | part) (on | about | called | named | titled) {query}`
- `(read | read me | go to | jump to | skip to | move to) [the] {query} (section | part)`

## Questions about the page

**Ask about the page you are on: what it is about, its main points, what it says about something, who wrote it, the price. The on-device AI answers from the page's text, briefly, and says when the page doesn't say. "Tell me more" goes on with the next passages.** For example: "what is this page about".

- `(tell me more | what else does (it | the page | this page | the article | this article) say)`
- `(read | tell | give | list | say) [me | us] the (main | key | important | most important) (points | ideas | facts | takeaways | things)`
- `(what are | whats | what is) the (main | key | important | most important) (points | ideas | facts | takeaways | point | idea)`
- `(main points | key points | key takeaways | takeaways)`
- `what (does | do | did) (it | this | that | they | the page | this page | the article | this article | the story | this story | the post | this post | the text | the site | this site) (say | tell me | tell us | mention | write) (about | on | regarding) {query}`
- `(does | do | did) (it | this | the page | this page | the article | this article | the story | this story | the post | this post | the text) (say | mention | tell me) (anything | something) (about | on | regarding) {query}`
- `(does | do) (it | the page | this page | the article | this article | the text) (mention | talk about | cover | discuss) {query}`
- `(tell me | read me) what (it | this | the page | this page | the article | this article) says (about | on | regarding) {query}`
- `(what is | whats) (this | the | this whole | the whole) (page | article | story | post | site | website | text | document | video | piece | essay | thread) about`
- `(what is | whats) (it | this | that) [all] about`
- `what (does | do) (it | this | the page | this page | the article | this article | the story | this story | the post | this post | the text) say`
- `(summarize | summarise | sum up | recap) [it | this | that | the (page | article | story | post | text | document | video) | this (page | article | story | post | text | document | video)]`
- `(give me | read me | tell me) [a | the] (summary | short summary | quick summary | gist | overview) [of (it | this | that | the (page | article | story | post | text) | this (page | article | story | post | text))]`
- `(whats | what is) the (gist | summary) [of (it | this | that | the (page | article | story | post | text) | this (page | article | story | post | text))]`
- `(summary | tldr | tl dr)`
- `who (wrote | authored | published | posted) (this | it | that | the (page | article | story | post | piece) | this (page | article | story | post | piece))`
- `(who is | whos) the (author | writer | publisher) [of (this | it | the (page | article | story | post | piece) | this (page | article | story | post | piece))]`
- `who is (this | it) (by | from | written by)`
- `(what is | whats) the (price | cost) [of (it | this | this one | that | that one | the (product | item | thing) | this (product | item | thing))]`
- `how much (is | does | do) (it | this | this one | that | that one | the (product | item) | this (product | item)) [cost | costs]`
- `what does (it | this | that) cost`
- `when was (this | it | that | the (page | article | story | post | piece) | this (page | article | story | post | piece)) (written | published | posted | updated | last updated | released)`
- `(ask the page | ask this page | ask the article | ask this article) {query}`
- `according to (this | the) (page | article | story | post | site) {query}`

## Choosing

**Choose from what is on screen: by number, by position, or by name, with what to do (open, play, click, select, read, describe).** For example: "play the fourth one".

- `(play | watch | listen to) {choice}`
- `(open | go to | follow | visit | show me | show | bring up) {position}`
- `(click | click on | press | tap | tap on | push | hit | activate | check | uncheck | tick | untick | toggle) {choice}`
- `(select | choose | pick | focus | focus on) {choice}`
- `read {choice}`
- `(describe | tell me about | tell me more about | what is | whats | what about | more about | more info on | details of | details on) {choice}`
- `(open | go to | follow | visit | show me | show | bring up) {choice}` (right after the browser read a list, or with numbers shown)
- `(open | go to | follow | visit | show me | show | bring up) {choice}`
- `{number}` (right after the browser read a list, or with numbers shown)
- `{position}`
- `{pick}` (right after the browser read a list, or with numbers shown)

**Put numbers on everything you can choose.** For example: "show numbers".

- `((show | joe) [me] [the] (numbers | labels | number labels) | (numbers | labels) on | (number | label) everything | turn on [the] (numbers | labels))`

**Take the numbers away.** For example: "hide numbers".

- `(hide [the] (numbers | labels) | (numbers | labels) off | (remove | clear) [the] (numbers | labels) | turn off [the] (numbers | labels) | no numbers)`

## Typing

**Type into the selected field.** For example: "type hello world".

- `(type | write | dictate | tight) {text}`
- `(take | tech) {text}` (while you are typing in a field)

**Start a new line.** For example: "new line".

- `(new line | newline | next line | line break | new paragraph)` (while you are typing in a field)

**Delete the last word.** For example: "delete the last word".

- `(delete | remove | erase | scratch | undo) [the] (last | previous) word` (while you are typing in a field)
- `delete that word` (while you are typing in a field)
- `(delete | remove | erase) the list word` (while you are typing in a field)
- `to leap the (last | list) word` (while you are typing in a field)

**Clear the field.** For example: "clear the field".

- `(clear | erase | empty | delete | remove) [the | this] (field | box | text box | text | input | search box | entry)` (while you are typing in a field)
- `(clear | erase | delete) [it | that | everything | all]` (while you are typing in a field)

**Read back what you typed.** For example: "read it back".

- `read (it | that | this | what i (typed | wrote | said)) back` (while you are typing in a field)
- `read back [what i (typed | wrote)]` (while you are typing in a field)
- `(what did i (type | write) | read what i (typed | wrote))` (while you are typing in a field)

**Send what you typed.** For example: "submit".

- `(submit | send | enter | go | search | done | send it | submit it | send the message)` (while you are typing in a field)
- `(press | hit) (enter | return)`
- `submit [the] form`

## Talking to the browser

**Answer a question: yes.** For example: "yes".

- `(yes | yeah | yep | yup | ya | sure | correct | confirm | confirmed | affirmative | absolutely | of course | okay | ok | alright | all right | right | do it | go ahead | go for it | thats right | thats correct | that is right | please do) [do it | go ahead | thats right | thats it | i do | it is | i am | confirm | please do]` (when the browser has asked a yes-or-no question)

**Answer a question: no.** For example: "no".

- `(no | nope | nah | negative | wrong | incorrect | no way) [dont | dont do it | do not | i dont | its not | it isnt | it is not | not that one | cancel]` (when the browser has asked a yes-or-no question)
- `(dont | do not) [do it | do that]` (when the browser has asked a yes-or-no question)
- `now` (when the browser has asked a yes-or-no question)

**Cancel what you asked for.** For example: "cancel".

- `(cancel | never mind | nevermind | forget it | forget that | forget about it | cancel that | abort | scratch that | stop that request)`
- `(no | no thanks | no thank you | nothing | none | never mind | not now)` (when the browser has asked which site to open)

**Stop: speaking, and whatever is running.** For example: "stop".

- `stop [it | that | everything | all | now]`

**Stop speaking.** For example: "stop talking".

- `(stop | quit) (talking | speaking | reading | reading aloud)`
- `(shut up | be quiet | quiet | silence | hush | enough)`

**Undo what voice mode did last: reopen the tab it closed, put back what it typed, go back from a page it opened, or put back a bookmark.** For example: "undo".

- `undo [that | it | this | the last (thing | action | step | change) | what you did]`

**Start over: forget the list, the question and what was being done, and listen again. While something plays, play it from the start.** For example: "start over".

- `(start over | start again | begin again | lets start over | lets start again | start from scratch | start afresh | reset | reset everything | clear the conversation | new conversation)`
- `start of`

**Say that again.** For example: "repeat".

- `(repeat | repeat that | repeat it | say again | say it again | say that again | come again | pardon | pardon me | what did you say | once more | sorry what | what was that)`

**Hear what you can say.** For example: "what can I say".

- `(help | help me | i need help | what can i say | what can i do | what do i say | what can you do | what commands are there | show commands | list commands | commands | show me what i can say | how does this work)`

**Hear where you are.** For example: "where am I".

- `(where am i | where are we | what (page | site | website | tab) (is this | am i on | are we on) | what is this (page | site | website) | whats this (page | site | website))`

**Speak faster.** For example: "speak faster".

- `(speak | talk | read) (faster | quicker | more quickly)`
- `speed up (your voice | your speech | speech | talking)`

**Speak slower.** For example: "speak slower".

- `(speak | talk | read) (slower | more slowly | slowly)`
- `slow down (your voice | your speech | speech | talking)`

**How much the browser says: brief, normal or full, more or less.** For example: "be brief".

- `(be brief | brief mode | brief | short answers | shorter answers | keep it short | verbosity brief | brief verbosity)`
- `(normal verbosity | verbosity normal | normal detail | default verbosity)`
- `(full verbosity | verbosity full | full detail | be verbose | verbose mode | tell me everything)`
- `(more detail | more details | say more | more verbose)`
- `(less detail | fewer details | say less | less verbose)`

**Correct what was heard.** For example: "no, I said YouTube".

- `[no | nope | wrong] i (said | meant | asked for) {text}`
- `(no | nope) (not that | thats wrong | wrong) i (said | meant) {text}`
- `i didnt say that i said {text}`

## The browser itself

**Bookmark this page.** For example: "bookmark this page".

- `(bookmark | bookmarked) (this | it | this page | the page | this site | this tab | that)`
- `(add | save | put) (this | it | this page | the page | this site | this tab) (to | in | as) [a | my | the] (bookmarks | bookmark)`
- `(add | make | create) a bookmark [for this page | for this site | here]`

**Remove this page's bookmark.** For example: "remove this bookmark".

- `(remove | delete) (this | the | that) bookmark [for this page | of this page]`
- `(remove | delete | take) (this | it | this page | the page | this site) (from | out of) [my | the] bookmarks`
- `unbookmark [this | it | this page | the page | this site]`

**Hear your bookmarks, to open one by number or name.** For example: "open my bookmarks".

- `(open | show | read | list | tell me) [me] [all] my bookmarks`
- `(open | show) [me] [the] (bookmarks | bookmark manager | bookmarks page)`
- `(what are | whats) my bookmarks`
- `my bookmarks`

**Open a bookmark by its name.** For example: "open the bookmark news".

- `(open | go to | load) [the | my] bookmark [called | named | for] {name}`
- `(open | go to | load) [the | my] book mark [called | named | for] {name}`
- `(open | go to | load) {name} (from | in) [my | the] bookmarks`

**Hear the pages you visited, to open one by number or name.** For example: "show my history".

- `(open | show | read | list | tell me) [me] my [browsing] history`
- `(open | show) [me] [the] [browsing] history [page]`
- `(what | which) (pages | sites) did i (visit | open | look at) [recently]`
- `my history`
- `to my history`

**Reopen the tab you closed last.** For example: "reopen the closed tab".

- `(reopen | restore | bring back | undo close | reuben) [the] [last] [closed | close] (tab | tip)`
- `(reopen | restore | bring back | open again) the [last] tab i [just] closed`
- `open [the] [last] (closed | close) (tab | tip)`

**Open a page from your history by its site or title.** For example: "open the weather from my history".

- `(open | go to | load | find) {query} (from | in) [my | the] history`

**Hear your downloads, newest first, to open one.** For example: "show my downloads".

- `(open | show | read | list | tell me) [me] [all] my downloads`
- `(open | show) [me] [the] downloads [page | folder]`
- `what (did i | have i) download [recently]`
- `my downloads`
- `(to | draw | sure) my downloads`

**Open a downloaded file: the last one, or by its number. A file that can run programs isn't opened by voice.** For example: "open the last download".

- `open [the] (last | latest | newest | most recent | last downloaded | latest downloaded) (download | file | downloaded file)`
- `open [the] download [number] {number}`
- `open [the] {position} (download | downloaded file)`

**Open settings: the browser's, or privacy, AI, voice mode or MoltNet settings.** For example: "open settings".

- `(open | show | go to) [me] [the | my] (privacy | security | privacy and security) settings`
- `(open | show | go to) [me] [the] (ai | molt ai | assistant) settings`
- `(open | show | go to) [me] [the] (voice mode | voice | voice control) settings`
- `(open | show | go to) [me] [the] (moltnet | molt net | mold net | tor) settings`
- `(open | show | go to) [me] [the] [browser | browsers | molt browser | moltbrowser] settings`
- `(browser | browsers) settings`

**Always listening on (after your yes) or off: every sentence is a command, without Hey Molt.** For example: "turn on always listening".

- `(turn | switch) on (always listening | always listen | listening all the time)`
- `(turn | switch) always listening on`
- `(always listening on | always listen [to me] | listen all the time)`
- `(turn | switch) off (always listening | listening all the time)`
- `(turn | switch) always listening off`
- `(always listening off | stop always listening | stop listening all the time | only listen after hey molt)`

**Settings that weaken a protection (saying the confirmation word aloud, a workflow's trust) are changed with a click in settings only: the browser says where.** For example: "say the word aloud".

- `[turn on | switch on | enable | always] say [the] [confirmation] word (aloud | out loud) [always | anyway]`
- `(turn on | switch on | enable) [the] (say the word aloud | confirmation word aloud)`
- `(trust | approve | allow) (this | the | that | my) (workflow | routine | automation) [on (all | every) (site | sites)]`
- `(change | raise) the trust of (this | the | that | my) (workflow | routine | automation)`

**Open a new window, a private window or a MoltNet window. Voice mode stays in this window, and never listens in MoltNet.** For example: "new window".

- `(open | new | start | create) [a] [new] (private | incognito) window`
- `(private | incognito) window`
- `(open | new | start | create) [a] [new] (moltnet | molt net | mold net | moldnet | moldenet | moldenate | mold the | tor) window`
- `(moltnet | molt net | mold net | moldnet | moldenet) window`
- `(open | start | create) [a] new window`
- `(new window | open [a] window | open another window)`
- `[open] [a] new (windjo | winjo | windel)`
- `[open] [a] new (private | incognito) (windjo | winjo | windel)`
- `[open] [a] new (molten at | molten | moldnet | moltnet) (window | windjo | winjo)`

**Close this window, after your yes; voice mode moves to another window.** For example: "close this window".

- `close [this | the | current] window`

**Switch to your other window; voice mode comes with you.** For example: "switch to the other window".

- `(switch | go | move | change | jump) to [the] (other | next | previous | last) window`
- `(other | next) window`

**Ask the on-device AI a question. Its answer is read to you, and nothing is done because of it.** For example: "ask the AI how far away is the moon".

- `ask (the ai | ai | the assistant | the ai assistant | molt ai | the chat | the ai chat | the eye | the a chat) [about | whether | if] {query}`
- `as (the ai | the assistant | the ai assistant) {query}`

**Open the AI chat beside the page.** For example: "open the AI chat".

- `(open | show | start) [me] [the] (ai chat | ai | chat | assistant | ai assistant | ai panel | chat panel | a chat | achat | eye chat | ai checked)`

**Hear the browser's own buttons (the toolbar, the tabs, the bookmarks bar, the side panel), to press one by number or name.** For example: "show the browser's buttons".

- `(show | read | list | tell me) [me] [the] (browser | browsers | toolbar | toolbars | browser toolbar) (buttons | controls)`
- `(what are | whats on) the (browser | browsers | toolbar) (buttons | controls | toolbar)`
- `(browser buttons | browsers buttons | toolbar buttons | show [me] the toolbar)`

**Open the browser's menu, then say what to click.** For example: "open the menu".

- `(open | show) [me] [the] (app | main | browser | browsers | chrome | moltbrowser) menu`
- `open the menu`
- `(app menu | browser menu | main menu | browsers menu)`

**Close the browser's menu.** For example: "close the menu".

- `(close | exit | leave | hide | dismiss) [the] [app | main | browser] menu`

## Routines

**Save what you just did as a routine.** For example: "remember this as my news".

- `(remember | save) (this | that | it) as [a routine | routine] [called | named] {name}`
- `save this routine as {name}`

**Run a routine by its name.** For example: "run my news".

- `(run | start | do | play) [the | my] routine [called | named] {name}`
- `run {name}`

**Delete a routine.** For example: "forget my news".

- `(forget | delete | remove) [the | my] routine [called | named] {name}`
- `forget {name}`

**Hear your routines.** For example: "what are my routines".

- `(what | which) (are | is) my routines`
- `(what | which) routines (do i have | are there | have i got | have i saved)`
- `(list | read | tell me | say) [all] [the | my] routines`
- `(my routines | routines)`

**Forget a word or a correction the browser learned from you.** For example: "forget the word Queen".

- `(forget | delete | remove) the (word | name | phrase | correction) {name}`

**Forget everything the browser learned from you: routines, words, corrections and site hints.** For example: "forget everything you learned".

- `(forget | delete | erase) (everything | all) [that] you (learned | learnt | know) [about me | from me]`
- `(forget | delete | erase) (everything | all) [that] youve (learned | learnt) [about me | from me]`
- `(forget | delete | erase) (everything | all) [that] you have (learned | learnt) [about me | from me]`

## Voice mode

**Start listening (typed in the Agent mode panel).** For example: "turn on voice mode".

- `(turn | switch) on voice [mode | control | commands]`
- `(turn | switch) voice [mode | control] on`
- `voice [mode | control] on`
- `(start | enter) voice [mode | control | commands]`
- `start listening [to me]`

**Stop listening. The browser asks first: afterwards the Voice button in the toolbar, or settings, turns it back on.** For example: "turn off voice mode".

- `(turn | switch | shut) (off | down) voice [mode | control | commands]`
- `(turn | switch) voice [mode | control] off`
- `voice [mode | control] off`
- `(stop | quit | exit | end | leave | close) voice [mode | control | commands]`
- `stop listening [to me]`

<!-- END GENERATED -->

## Not yet

- **Other languages.** English, and Spanish ([VOICE_COMMANDS.es.md](VOICE_COMMANDS.es.md),
  chosen in settings, "Voice mode", "Language"): Spanish commands typed in
  Agent mode work and are answered in Spanish with the computer's Spanish
  voice, but voice mode can't listen in Spanish until a Spanish speech
  model it can ship is found. More languages add their own phrase tables.
- **A site's name on its own** opens only the sites the browser knows by
  name and those you opened by voice in the same window since it opened;
  the browser doesn't keep a list of the sites you open across restarts.
  Say "open", then the name, for any other.
- **Opening a downloaded program by voice:** a file that could run code
  (a program, an installer, a script) isn't opened by voice at all, not
  even with a confirmation word; open it from the downloads page.
- **Changing settings on a settings page by voice:** voice mode can read
  a settings page but not change its switches; it changes its own speed,
  detail and always listening by voice. The settings that weaken a
  protection stay a click on purpose.
- **The browser's own buttons:** the toolbar, the tabs, the bookmarks bar,
  the side panel and the open menu are read and pressed by voice; other
  windows of the browser (a bubble, a dialog) only through their own
  spoken prompts. Undo is one step: the last thing voice mode did.
- **Routines with a site's own tool:** when the on-device AI used a tool a
  site offers, that step can't be kept in a routine yet. Nor can a step
  whose button or link has the same name as another on the page when the
  routine runs: the routine stops there rather than guess.
- **Renaming a routine by voice,** or choosing which of the routines from
  before this update to keep one by one: do these in Agent mode. Two
  routines with the same name (after editing their files) run by neither
  name until one is renamed there.
- **Telling your voice from others':** anyone who hears a confirmation
  word ("say harbor") can say it. Recognizing your own voice is planned.
- **Choosing your own wake phrase**, and a wake phrase tuned to your voice:
  "Hey Molt" for now. If the browser keeps missing your "Hey Molt", turn on
  "Always listening" in settings.
- **Interrupting the browser mid-sentence with a new command** works
  without "Hey Molt" when the browser speaks with its own voice and the
  microphone has echo cancellation (its own voice is then removed from
  what the microphone hears), for what stays with what it is reading or
  doing: a number, "next", "louder", "pause", "go back", "read the
  headings". A command that goes to another site, searches, types, sends,
  closes a tab or runs a routine stops the browser instead, and it says
  "I've stopped. Now say that again." With "Hey Molt" first, any command
  interrupts it. With the system's voices, which the browser uses when its
  own isn't installed, start with "Hey Molt" (or say "stop"). With its own
  voice and no echo cancellation, say "stop" first.
- **While reading aloud,** the sentence being read is shown in the bar
  under the page, but not highlighted on the page itself.
- **The playback speed** is the video's own: a site's player that keeps
  its own speed setting may show its old speed in its menu.
- **Questions about the page** use only the page's text: not its
  pictures, videos, tables or the parts in frames. A question about a
  picture ("what's in this picture?") comes later, and so does a
  conversation about the answer: each question stands on its own, apart
  from "tell me more". Answers start once they are complete, not word by
  word. The same goes for "ask the AI": each question stands on its own,
  and the answer isn't added to the AI chat's conversation.
