---
date: 2026-05-12
title: A form filler that lives on your machine, not in Google's profile graph
slug: form-filler
tags: [forms, privacy, oscrypt]
---

Chrome's autofill is excellent — and it lives in your Google profile.
For people who don't want every shipping address and phone number
they've ever typed sitting in a sync graph, the only alternatives have
been: type it every time, or a password manager bolted onto autofill.

We just shipped a third option: **a form filler whose entire universe
is one local file**.

## The shape

Type `/profile` in the side panel. An inline editor pops up:

```
Full name        ┃ Raj
Email            ┃ raj@geneye.ai
Phone            ┃ +1 555 0100
Address line 1   ┃ 123 Main St
City             ┃ San Francisco
…
Save profile  /  Cancel
```

Hit save. The dict gets JSON-serialized, encrypted with OSCrypt
(macOS Keychain / Windows DPAPI / libsecret on Linux) and dropped at
`~/.moltbrowser/profile.enc`. Nothing leaves the box, no account is
required, no sync ever happens.

Then on any web form, type `/fill`. Done.

```
✓ Filled 7 fields (of 9 on this page)
```

## How the matcher works

For v1, no LLM round-trip. The matcher is dumb-and-fast: for each
profile key (`email`, `phone`, `address_line1`, …) we keep a small
list of regex patterns. For each visible `<input>` / `<textarea>` /
`<select>` on the page, we build an identity string from
`name | id | placeholder | autocomplete | aria-label | <label>` and
test it against every pattern until one hits.

```js
email:          [/e[-_ ]?mail/i, /^email/i, /username.*email/i],
phone:          [/phone/i, /tel(ephone)?/i, /mobile/i, /cell/i],
address_line1:  [/address[-_ ]?(line)?[-_ ]?1/i, /street[-_ ]?address/i, ...],
```

Hits get filled, with the `input` and `change` events dispatched so
React/Vue/Angular SPAs see the change. We skip hidden / disabled /
file / password / radio / checkbox controls, and we **don't overwrite
existing values** — if the user already typed something, the matcher
leaves it alone.

Every filled control gets `data-molt-filled="<key>"` set so a future
"highlight what got filled" UI can paint a halo.

## Why no LLM yet

For ~80% of forms, the heuristic is enough. When it fails — funky
field names, no label, custom widgets — we can route through the LLM
for selector→profile-key mapping using the same prompt template the
selector-recovery path uses for the automation runner. That's the v2
delta. Shipping v1 first because it's deterministic, fast (sub-50ms
on big pages), and most importantly debuggable.

## What this is **not**

- Not a password manager. We skip `type="password"` deliberately.
- Not a payment vault. Card numbers belong in a real PCI-compliant
  flow, not in `~/.moltbrowser/`.
- Not synced. By design. If you want the same profile on three
  machines, write your own sync script — the file format is JSON.

## Try it

```
git pull && ./scripts/build.sh && open chromium/src/out/MoltBrowser/MoltBrowser.app
```

Open the side panel, type `/profile`, fill it once. Then `/fill` on
your next signup / checkout / contact form.

— GenEye AI Labs
