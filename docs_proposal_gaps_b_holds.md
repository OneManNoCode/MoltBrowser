# Docs proposal: Snooze and Skip are holds kept in the record

Proposed text for the caller to merge (this change edits neither
`CHANGELOG.md` nor `docs/design/AGENT_ACTIONS.md`). Section 1 is the
CHANGELOG entry and section 2 is §3.6.11. Section 3 lists sentences in other
§3.6 sections that this change makes stale, with replacements.

## 1. CHANGELOG.md, `## [Unreleased]` → `### Fixed`

Add after the "An interval schedule saved in molt://ai-automation runs."
entry:

```markdown
- **Snooze and Skip in molt://ai-automation hold every schedule.** On a
  cron schedule ("weekdays at 9"), Skip next ran the very time it was
  meant to skip, and Snooze 1h didn't hold the run back and was then
  forgotten. Both now hold cron and interval schedules alike. Snooze 1h
  runs nothing for the hour, then the schedule goes on at its own next
  time; an hourly or daily interval is no longer pushed a whole interval
  past the hour. Skip next skips exactly the next run, and pressing it
  again skips the one after. While a hold lasts, the card says "Held
  until …" next to the next run. A hold applies to this copy of
  MoltBrowser (all its profiles), not to another browser that shares
  `~/.moltbrowser`. Saving the same schedule again keeps the hold, and a
  new schedule ends it. The two buttons are shown only for cron and
  interval schedules. They no longer save the card's copy of the
  workflow, so pressing them on a card that was open while the workflow
  was edited elsewhere no longer undoes that edit.
```

## 2. docs/design/AGENT_ACTIONS.md §3.6.11 "Not done"

### 2a. Under "**Closed since** (the workflow-gaps change):"

Add this bullet after the interval bullet (the one ending "…re-saving one
multiplied it by 3600);"):

```markdown
  - molt://ai-automation's Snooze and Skip are holds in this browser's
    record: the entry's `held_until`, set by `WorkflowRecord::Hold()`
    through `AutomationStorage::HoldWith()`. Holds are read into
    `Trigger::held_until_unix`, are never written to or read from a file,
    and stay outside the digest. Cron and interval schedules both honour
    a hold:
    - `NextFireTime()` gives the first time at or after the hold, and an
      interval keeps its phase (whole intervals after its last fire), so
      the times a hold covers are never caught up;
    - a timer's reload that finds the workflow held doesn't fire it;
    - `ClaimFired()` refuses a fire before the hold, and a claimed fire
      ends it.

    Snooze (`snoozeScript`, a minute to an hour; the button sends an
    hour) holds from the click. Skip (`skipNextFire`) holds until one
    second past the first run strictly after now: for an overdue interval
    that is its next time to come, not the time already passed. A second
    Skip skips the run after that. A hold only rises.

    A save that keeps the schedule keeps the hold (same type, expression,
    time zone and end: a page's save, a trust change, a confirmation). A
    save that changes the schedule ends it, and so does a whole write
    (`Save()`, `SaveNew()`). A hold covers this browser's profiles, which
    share the record, and no other browser.

    Both are refused for:
    - a routine taught by voice ("script not found");
    - an unconfirmed draft (in `PromoteTrust()`'s words);
    - a workflow that doesn't repeat;
    - an interval never started.

    The buttons are shown only for cron and interval schedules. They send
    their own message and no longer save the card's copy, which may be
    stale. molt://ai-automation's `saveScript` never moves a stored last
    fire later: the page's value is capped at the stored one. No page has
    a reason to move it, and a fire time far ahead would stall the
    schedule.
```

### 2b. Under "**Existing gaps left as they are** (separate changes):"

Delete this clause (the bullet then starts at "a run's limit is checked
between steps;"):

```markdown
  molt://ai-automation's Snooze and Skip don't hold back a cron schedule's
  timer in any browser (they are written as a future last fire, which cron
  ignores; holds kept in the record are the planned fix);
```

Optionally add one caveat to the same bullet: "a hold is on this
browser's clock: setting the clock back during one makes it last longer by
the same amount;".

## 3. Other §3.6 sentences this change makes stale (suggested replacements)

- **§3.6.1, the JSON example:** add a hold to `wf_1.molt`:
  `{"digest": "<64 lowercase hex>", "fired": 1763200000, "held_until": 1763203600}`.
  After the `fired` bullet, add: "and `held_until` (left out when 0), a
  Snooze or Skip made in this browser (`Hold()`). It only rises, and it
  ends with a fire claimed at or after it, or with a write that doesn't
  keep the schedule (`Set()`; `SetKeepingHold()` keeps it). It is never
  read from or written to a file. A malformed one reads as none and the
  entry stays. An older build ignores it, so a downgrade ends holds."
- **§3.6.3, Reads:** "A browser-made write sets `fired` to what it wrote
  when that is later, or 0: molt://ai-automation's Snooze and Skip (a fire
  time in the future) hold, and a schedule saved afresh starts over" →
  "A browser-made write sets `fired` to what it wrote when that is later,
  or 0: a schedule saved afresh starts over (molt://ai-automation never
  sends a later one: its Snooze and Skip are holds, `HoldWith()`)". In the
  next sentence, "a page opened before the last fire, Snooze or Skip" →
  "a page opened before the last fire", and "and ends no snooze" → "and
  keeps the schedule's hold". Add `HoldWith()` (record only, the file not
  written) to the list of writes.
- **§3.6.5, Scheduler:** "(a Skip, a Snooze, a clock that was ahead: a
  cron timer fires through those, and the next cron time after either is
  the same)" → "(a clock that was ahead: a cron timer fires through it)".
  Add: "A hold (`Trigger::held_until_unix`) is honoured by `NextFireTime()`
  (the first time at or after it, an interval's phase kept), by `IsDue()`
  on the timer's reload, and by `ClaimFired()`, which refuses a fire
  before it. The times a hold covered aren't caught up." "(not fired or
  snoozed here since it was listed: by a catch-up, a Snooze)" → "(not
  fired or held here since it was listed)".
- **§3.6.5, molt://ai-automation:** add: "Snooze and Skip are messages of
  their own (`snoozeScript`, `skipNextFire`). Each records a hold and
  doesn't rewrite the workflow. They are shown only for cron and interval
  schedules. A save never moves a stored last fire later."
- **§3.6.10, Tests:**
  - `workflow_record_unittest.cc`: a hold only rises and survives a
    restart; a claim before it is refused and one at it ends it; a write
    ends it unless it keeps the schedule; a move's copy carries it, and a
    failed write's restore puts it back; a malformed `held_until` reads as
    none and the entry stays.
  - `automation_storage_unittest.cc`: a hold is recorded, not written (the
    file, its digest and the vouching are unchanged); it is read,
    computed and set under one lock, so a reschedule waiting for the lock
    ends it; only a save of the same schedule keeps it (a page's save, a
    trust change, an `Update()` of something else), while a new schedule
    or a whole write ends it and a failed write puts it back; it is never
    claimed through; each user data directory keeps its own.
  - `automation_scheduler_unittest.cc`: a held trigger's next fire is its
    first time at or after the hold; a Snooze made here holds an interval
    and it runs at its next time after the hold (75 min, not 85); a cron
    snoozed after its last listing doesn't fire at its time and fires at
    its first match after the hold; a skipped interval keeps its phase; an
    overdue interval's Skip holds past its next time (no catch-up); a
    stale page's save keeps a Snooze.
    Also replace "a Snooze saved here holds for the whole snooze" with "a
    Snooze or Skip made here holds (cron and interval)".
  - `molt_ai_automation_storage_unittest.cc`: a page can't move the last
    fire later; Snooze holds without writing the file; Skip holds past the
    next run only (cron, interval, overdue interval, none to come); both
    refuse what they can't hold.
  - `molt_ai_agent_storage_unittest.cc`: "an editor opened before a Snooze
    keeps it" (now the hold).
- **§3.6.12, release notes:** add the CHANGELOG sentence from section 1,
  shortened: "Snooze and Skip in molt://ai-automation now hold cron
  schedules too, and Snooze no longer pushes an interval a whole interval
  past the hour. A hold applies to this browser only."
