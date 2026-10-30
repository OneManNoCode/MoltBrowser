# What you can say: MoltBrowser voice commands

Voice mode lets you use MoltBrowser by voice alone (the design is
[docs/design/TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md)). This page
lists the commands the browser understands directly, without its AI model:
they work in a moment, even on a slow machine. Anything else you say goes to
the on-device AI, which works out what you mean from the page.

Voice mode is a preview, behind a flag.

## Turning voice mode on

1. Turn on `chrome://flags/#molt-voice-mode` and restart the browser.
2. Then, any of these turns listening on in a window:
   - the **Voice** button in the toolbar (it has a violet ring while the
     window listens; press it again to stop);
   - **Settings** (`molt://ai-settings`), "Voice mode": the switch listens
     in that window now, and **whenever MoltBrowser starts**, so a person
     who uses only their voice never needs to touch the computer after it
     is set up once. "Always listening" there makes every sentence a
     command, without "Hey Molt";
   - typing **"turn on voice mode"** in Agent mode's "Type what you would
     say" field.
3. The first time, your system may ask whether MoltBrowser may use the
   microphone: allow it. If it is refused, the browser says so aloud and
   on screen; allow it in the system's privacy settings, then turn voice
   mode on again.
4. You hear "Voice mode is on. Say Hey Molt, then what you want." A bar
   under the page shows what voice mode is doing (waiting for "Hey Molt",
   listening, thinking, speaking), what it heard as you speak, and what it
   is doing.

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
video yourself before you speak. Voice mode never cuts a page's reading
short: its own answer waits until the page has finished, and "stop" stops
only voice mode's speech. Each state has a short sound: a rising tone when it
heard "Hey Molt", a tick when it took a command, a falling tone when it
goes back to waiting for "Hey Molt".

You can also type the commands, without the microphone, in Agent mode's
"Type what you would say": the browser does it in the tab you are on and
says the answer aloud with your system's voices.

On Linux, spoken answers need speech-dispatcher, and the browser started
with `--enable-speech-dispatcher`; without them it answers in the panel
and the bar only.

## What it does

- **"Open YouTube"** opens the site. Six sites are known by name: YouTube,
  Google, DuckDuckGo, Wikipedia, Brave Search and Bing. A spoken address
  ("example dot com") opens that address. Any other name is searched for
  with your default search engine, and the browser tells you so: it never
  guesses a web address.
- **"Search for ..."** types into the page's own search box and submits it;
  on a page without one, it uses the site's search page, or your default
  search engine. Then it reads the results: how many, the first five with
  their channel or site and length, and "Say a number or a name". "Be brief"
  reads only how many; "tell me everything" reads all of them.
- **Choosing:** "the fourth one", "number four", "four" right after a list,
  "the live one by Queen". If several fit, the browser reads them back
  numbered ("Did you mean 1: ..., 2: ...?") and you answer with a number or
  a name. "Play" or "open" a video opens it and plays it with sound.
- **Before anything that can't be undone,** such as buying, paying,
  deleting, sending, posting, subscribing, signing out or sending a form,
  the browser asks "Say yes to press ..." and does it only if your next
  words are "yes".
- **When a page tries something after the browser clicks for you,** such
  as opening another site in a new window, downloading a file or opening
  another app, the browser asks first: "The page tried to open a new
  window to example.com. Say yes to open it, or no to cancel." It happens
  only if your next words are "yes". A page that tries to go full screen
  (unless you said "full screen" or "play") or to change your clipboard is
  stopped; a permission it asks for, such as your location, is turned down,
  because voice mode can't answer permission questions yet. The browser
  tells you each time.
- **Typing:** choose a field ("click the search box"), then "type ...",
  "delete the last word", "clear the field", "read it back", "submit".
  Password, card and code fields are never typed into or read out.
- **Media:** "play", "pause", "resume", "louder" and "quieter" (with the
  player's volume control), "mute", "skip ahead one minute", "full screen",
  "turn on captions".
- **Reading:** the title, the headings, the links, the results, "next
  heading", "next link", "next item", "where am I", "what's on the screen".
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
- **If it heard you wrong**, say "no, I said ..." and what you meant.
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
  routine's name).
- `{seconds}`: a length of time; `{seconds?}` may be left out.
- `{level}`: a volume, from 0 to 10, as a percentage, or "half" or "max".
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

- `(open | go to | take me to | visit | navigate to | launch | load | bring up | pull up) {site} in [a] new tab`
- `(open | go to | take me to | visit | navigate to | launch | load | bring up | pull up) {site}`
- `{address}`

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
- `(find on page | find in page | find on this page) {query}`
- `search (this | the) page for {query}`

**Go back to the previous page.** For example: "go back".

- `(go | navigate | take me) back [a page | one page | to the (previous | last) page]`
- `(back | previous page | last page | go to the previous page)`

**Go forward again.** For example: "go forward".

- `(go | navigate) forward [a page | one page]`
- `forward`

**Load the page again.** For example: "reload".

- `(reload | refresh) [the | this] [page | tab | site | website | it]`
- `load [the | this] page again`

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
- `(new tab | open [a] tab | open another tab)`

**Close this tab, or a tab by number or name.** For example: "close this tab".

- `close [this | the | current] tab`
- `close tab {tab}`
- `close [the] {tab} tab`

**Switch tabs: by number, position or name, or the next one.** For example: "switch to tab 2".

- `[go to | switch to | move to] [the] next tab`
- `[go to | switch to | move to] [the] previous tab`
- `(switch | go | change | move | jump) to tab {tab}`
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

- `(scroll | go | jump | move | go back) [up] to the (top | beginning | start) [of the page]`
- `(top of [the] page | page top | scroll top | back to top)`

**Go to the bottom of the page.** For example: "scroll to the bottom".

- `(scroll | go | jump | move) [down] to the (bottom | end) [of the page]`
- `(bottom of [the] page | page bottom | scroll bottom)`

**Make everything bigger.** For example: "zoom in".

- `(zoom in | zoom in more | magnify | enlarge | increase [the] zoom | (bigger | larger) [text] | make (it | the text | text | everything | the page) (bigger | larger))`

**Make everything smaller.** For example: "zoom out".

- `(zoom out | zoom out more | shrink | decrease [the] zoom | smaller [text] | make (it | the text | text | everything | the page) smaller)`

**Back to the normal size.** For example: "reset zoom".

- `((reset | normal | default) zoom | zoom (reset | normal | to normal | back to normal) | (actual | normal | original | default) size | reset [the] zoom level)`

## Media

**Play.** For example: "play".

- `(play | hit play | press play | start playing | start playback) [it | this | that | the video | the song | the music | the media | the movie]`

**Pause.** For example: "pause".

- `stop (the video | the music | the song | playback | playing | the movie | the audio)`
- `(pause | hold on) [it | this | that | the video | the music | the song | playback | the movie | the audio]`

**Carry on playing.** For example: "resume".

- `(resume | unpause | continue | keep playing | continue playing | carry on) [it | playback | the video | the music | the song | playing]`

**Louder.** For example: "louder".

- `(louder | volume up | turn it up | turn up the volume | turn the volume up | turn up | more volume | make it louder | crank it up | pump it up | (increase | raise) [the] volume | a (bit | little) louder | much louder)`

**Quieter.** For example: "quieter".

- `(quieter | softer | volume down | turn it down | turn down the volume | turn the volume down | turn down | less volume | make it quieter | lower | (decrease | lower | reduce) [the] volume | a (bit | little) quieter | much quieter)`

**Set the volume: 0 to 10, a percentage, half or max.** For example: "set the volume to 5".

- `[set | turn | change | put] [the] volume [to | at] {level}`

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

**Play from the beginning.** For example: "start over".

- `(restart | start over | replay | play (it | this | that) again | play again | watch again | from the (beginning | start | top) | start from the (beginning | start) | play from the (beginning | start) | restart the video | rewind to the (beginning | start))`

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

**Hide captions.** For example: "captions off".

- `(turn off | hide | disable | switch off | stop | remove | take off) [the] (captions | subtitles | closed captions | cc | subtitle)`
- `((captions | subtitles | closed captions | cc) off | no (captions | subtitles))`

**Play faster.** For example: "speed up".

- `(speed up | faster | play faster | go faster | speed it up | increase [the] [playback] speed)`

**Play slower.** For example: "slow down".

- `(slow down | slower | play slower | go slower | slow it down | (decrease | reduce) [the] [playback] speed)`

**Back to normal speed.** For example: "normal speed".

- `((normal | regular | default | usual) [playback] speed | reset [the] [playback] speed | speed (normal | to normal | back to normal) | play at normal speed)`

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
- `(what is | whats) the (title | page title | title of the page | name of (this | the) page)`

**Read the selected text.** For example: "read the selection".

- `read [the | my] (selection | selected text | highlighted text | highlighted part | highlight)`
- `read (what is | whats | what i) (selected | highlighted)`

**Describe what is on the screen.** For example: "what's on the screen".

- `((whats | what is) on [the | my | this] (screen | page) | what do you see | what can you see | describe [the | this] (screen | page | view) | (whats | what is) here)`

**Move to the next heading.** For example: "next heading".

- `[go to | jump to | move to | read] [the] next (heading | section | header)`

**Move to the previous heading.** For example: "previous heading".

- `[go to | jump to | move to | read] [the] previous (heading | section | header)`

**Move to the next link.** For example: "next link".

- `[go to | jump to | move to | read] [the] next link`

**Move to the previous link.** For example: "previous link".

- `[go to | jump to | move to | read] [the] previous link`

**Move to the next item of the list.** For example: "next item".

- `[go to | jump to | move to | read] [the] next (item | result | entry | paragraph | element)`
- `next`

**Move to the previous item.** For example: "previous item".

- `[go to | jump to | move to | read] [the] previous (item | result | entry | paragraph | element)`
- `previous`

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

- `(show [me] [the] (numbers | labels | number labels) | (numbers | labels) on | (number | label) everything | turn on [the] (numbers | labels))`

**Take the numbers away.** For example: "hide numbers".

- `(hide [the] (numbers | labels) | (numbers | labels) off | (remove | clear) [the] (numbers | labels) | turn off [the] (numbers | labels) | no numbers)`

## Typing

**Type into the selected field.** For example: "type hello world".

- `(type | write | dictate) {text}`

**Start a new line.** For example: "new line".

- `(new line | newline | next line | line break | new paragraph)` (while you are typing in a field)

**Delete the last word.** For example: "delete the last word".

- `(delete | remove | erase | scratch | undo) [the] (last | previous) word` (while you are typing in a field)
- `delete that word` (while you are typing in a field)

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

**Cancel what you asked for.** For example: "cancel".

- `(cancel | never mind | nevermind | forget it | forget that | forget about it | cancel that | abort | scratch that | stop that request)`

**Stop: speaking, and whatever is running.** For example: "stop".

- `stop [it | that | everything | all | now]`

**Stop speaking.** For example: "stop talking".

- `(stop | quit) (talking | speaking | reading | reading aloud)`
- `(shut up | be quiet | quiet | silence | hush | enough)`

**Undo the last action.** For example: "undo".

- `undo [that | it | this | the last (thing | action | step | change) | what you did]`

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

- **Other languages.** English only for now; the phrase tables are laid out
  for more.
- **Spoken punctuation** in typing ("comma", "period"): the recognizer adds
  punctuation itself.
- **A site's name on its own** ("YouTube" alone) doesn't open it: say "open
  YouTube". A spoken address on its own ("example dot com") does.
- **A routine's name on its own** ("my news") is looked up among your
  routines before the AI is asked; "run my news" always runs it.
- **Choosing your own wake phrase**, and a wake phrase tuned to your voice:
  "Hey Molt" for now. If the browser keeps missing your "Hey Molt", turn on
  "Always listening" in settings.
- **Interrupting the browser mid-sentence with a new command** works with
  "Hey Molt" (or "stop"); a command without "Hey Molt" while it speaks is
  not heard, because the system voices it speaks with can't be cancelled
  from the microphone yet.
- **Still to come in voice mode:** the on-device AI for anything these
  commands don't cover, routines, setting the volume to a level, the
  playback speed, reading the page's text and the selection, undo, and
  questions about the page. Until then the browser says so when you ask
  for them.
