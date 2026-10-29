# What you can say: MoltBrowser voice commands

Voice mode lets you use MoltBrowser by voice alone (the design is
[docs/design/TOUCHLESS_BROWSING.md](design/TOUCHLESS_BROWSING.md)). This page
lists the commands the browser understands directly, without its AI model:
they work in a moment, even on a slow machine. Anything else you say goes to
the on-device AI, which works out what you mean from the page.

Voice mode is being built: these commands are the first part, and a release
that can hear you is still to come.

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

The browser tries the forms in the order listed and takes the first that
fits all of what you said.

<!-- BEGIN GENERATED: tools/voice/voice_commands_doc.py --write -->
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
