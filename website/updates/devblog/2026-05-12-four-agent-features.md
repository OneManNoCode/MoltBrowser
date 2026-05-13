---
date: 2026-05-12
title: Four features that make the browser feel a little more alive
slug: four-agent-features
tags: [automation, side-panel, privacy, agent-inbox]
commit: bd52774
---

We shipped four features today, all built on the same playbook: combine
the side-panel chat, the automation engine, and the per-tab observer
hooks we already had, and aim at a real annoyance.

## 1. Universal cookie-modal killer

Every browser session, every day, you're clicking past cookie banners.
We baked a `WebContentsObserver` into every tab that, ~600 ms after
load, runs a 3-stage script in an isolated world. It tries:

1. Curated CSS selectors for the eleven biggest CMP vendors (OneTrust,
   Cookiebot, TrustArc, Quantcast, Didomi, CookieYes, Osano,
   Sourcepoint, Usercentrics, Klaro, generic patterns).
2. A text-based fallback that scans visible buttons for
   "reject all" / "decline all" / "only necessary".
3. A retry pass at 1.8s and 4s for CMPs that lazy-mount their banner.

It's idempotent (`window.__moltConsentKillerInstalled`) so it never
fires twice on the same page. Only runs on http(s). Three lines in
`tab_helpers.cc` to attach the helper.

## 2. Tab triage

Open the side panel and type `/triage list` — every tab in the window
appears with a one-line snippet. From there:

```
/triage close-inactive       # close everything except the active tab
/triage close-domain x.com   # close every Twitter tab
/triage bookmark-inactive    # rescue them to "Other Bookmarks"
/triage pin-active           # pin the current tab
```

Backed by two new WebUI IPCs (`listTabsInWindow`, `triageActOnTabs`)
that talk straight to the side-panel-owning Browser's `TabStripModel`.
Bookmarks land via the existing `BookmarkModelFactory`.

## 3. Page watchers

```
/watch https://flights.example.com/sfo-bali  .price  900  Bali flight
```

Builds a `Script` with an `INTERVAL` trigger and four steps —
`NAVIGATE`, `WAIT_FOR`, `EXTRACT`, `NOTIFY` — and saves it via the
existing `AutomationStorage`. The scheduler picks it up on its next
tick (or immediately, we call `Reschedule()`). Every 900 s you get an
OS notification with the current price.

The script ID is a hash of `url|selector` so re-running `/watch` with
the same arguments updates the existing watcher in place rather than
proliferating duplicates.

## 4. Agent inbox

The watchers in (3) run silently in a hidden popup window. Until
today, you couldn't see what they were doing. Now:

- New process-wide `AgentInboxRegistry` (UI-thread-only, no locking
  needed).
- `BackgroundRunHolder` registers an ID on start, updates
  `current_step` from the runner's per-step callback, and marks
  `Finish` on completion (pruned 30 s later).
- A 3-second poller in the side panel renders a "Running agents" tray
  with a pulsing dot, script name, step counter, and status note.
- Hides when there's nothing to show.

The tray is small (~150 LoC of HTML + CSS) but it changes the
emotional read of background automation from "did anything happen?"
to "I can see what's happening."

## What's next

I'm starting work on three follow-ups: **PDF chat** (chat with any
PDF the same way you chat with any web page), **Form filler agent**
(encrypted local profile, one-click autofill, no Google sync), and
**AI-grouped history** (replace the dumb history list with topic
clusters). Each will land as its own commit and its own post here.

— GenEye AI Labs
