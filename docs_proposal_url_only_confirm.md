# Docs proposal: confirm a changed workflow whose addresses come from values

The text below is for the caller to merge. CHANGELOG.md and
docs/design/AGENT_ACTIONS.md are not edited by this change.

## CHANGELOG.md, [Unreleased], ### Security

Add this to the end of the bullet **Workflows changed outside MoltBrowser
now need your review.**, after "(or in molt://ai-automation for Trusted or
Admin)." and before "Confirming keeps only what the review shows":

> A changed workflow whose every address is filled in from a value
> (`{{…}}`) has no site to confirm: click **Confirm it with no sites**. It
> then opens and acts on no page by itself until you add a step that opens
> a fixed address on a site, or set it to Trusted in molt://ai-automation.

## docs/design/AGENT_ACTIONS.md, §3.6.11 Not done

1. **Remove** this bullet:

   > - **A workflow whose steps name no static site** (only `{{url}}`) can't be
   >   confirmed; the user re-imports it or adds a "Go to page" step. Follow-up:
   >   confirm with no sites when `changed_outside` (CASUAL, empty whitelist).

2. **Add** to the end of "**Closed since** (the workflow-gaps change):":

   > - a file changed outside whose steps target no site
   >   (`HostsReferencedBy()` empty: every address they open has a
   >   `{{variable}}` in it, or none opens a page) is confirmed with none
   >   (`AutomationSecurity::MayConfirmWithoutSites()`: `sites_unconfirmed`
   >   and `changed_outside`, trust below TRUSTED, no voice step). getWorkflow
   >   and a save answer a draft with `confirmWithoutSites` (the browser
   >   decides, never the page); the studio's review then says so and shows
   >   **Confirm it with no sites**, which sends `confirmDraftSites` with an
   >   empty list, the click and the review token, as for sites (edits not
   >   saved yet are saved first). Trust is unchanged (the clamp made it
   >   CASUAL) and the whitelist becomes empty, whatever a page's save stored
   >   on the draft; the audit log says `outside_change_confirmed | no_sites`.
   >   Below TRUSTED an empty whitelist matches no host, so it opens and acts
   >   on no page by itself: each `{{variable}}` navigation is refused, the run
   >   window opens `about:blank`, and clicks, typing, key presses and site
   >   tools are refused on every page. It gains a site only from a step that
   >   opens a fixed address (`EffectivePolicy()` reads it), or any site when
   >   the user sets it Trusted in molt://ai-automation. Run by another
   >   workflow (`RUN_SCRIPT`, allowed once confirmed), its steps run under
   >   that run's policy and add no site to it, as for any confirmed child.
   >   It is the state of a workflow the user makes in the studio today (a
   >   blank or imported save: CASUAL, no whitelist). Never a draft from an
   >   agent run (§2.9.5: confirming says where the model may go; an agent
   >   draft always targets a site), a change left TRUSTED or ADMIN (a
   >   molt://ai-automation save of a stored workflow keeps its trust: with no
   >   site it would act on any), or a routine taught by voice: voice mode
   >   does check a routine's sites (`VoiceRoutineStore::Load()` gives it
   >   `EffectivePolicy()`, and the voice loop checks `AllowNavigation()`
   >   before it acts on or opens a page), but it acts on the tab in front of
   >   the user, a command that doesn't act on a page isn't site-checked, and
   >   a routine of the user's own steps counts as confirmed on
   >   `!sites_unconfirmed` alone (`RoutineSitesConfirmed()`), so a planted
   >   routine confirmed with no site could act wherever the user is
   >   (`VoiceRoutineStore::ConfirmSites()` refuses any changed routine
   >   anyway).

3. **Add** a bullet after the one above (or to "Existing gaps left as they
   are"):

   > - **A fixed host with a `{{variable}}` in its path or query**
   >   (`https://shop.example/search?q={{q}}`) is no site:
   >   `HostsReferencedBy()` skips an address with a `{{` anywhere in it. A
   >   change whose only addresses are like it is confirmed with none, and its
   >   "Go to page" is then refused below TRUSTED until a step opens a fixed
   >   address on that site (a "Go to page" step of its own for the same
   >   host). The studio's wording says "every address its steps open uses a
   >   value", which holds for it too.

4. **Add** to "**Existing gaps left as they are**":

   > molt://ai-automation's import keeps the file's `domain_whitelist`
   > (`ClampTrustForSave()` clears it only for a routine taught by voice), so
   > such an import may navigate to the sites its file lists; and the
   > studio's review lists at most 20 sites (`cleanSites()`), while a
   > confirmation needs exactly the sites the steps target, so a change whose
   > steps target more than 20 can't be confirmed in the studio.

## Optional, elsewhere in AGENT_ACTIONS.md §3.6

- **§3.6 file table, `automation/automation_security.{h,cc}` row:** add
  `MayConfirmWithoutSites()`.
- **§3.6.6 step 2**, after "and the sites must be exactly the ones its steps
  target": "(or none, when its steps target none and
  `MayConfirmWithoutSites()`: §3.6.11)".
- **§3.6.6 step 3**, after "(`outside_change_confirmed` in the audit log)":
  "(`outside_change_confirmed | no_sites` when it has none; its whitelist is
  then empty)".
- **§3.6.10 Tests:** `automation_security_unittest.cc`: a change with no
  site is confirmed with none (never with a site its steps don't target;
  once; then CASUAL with an empty whitelist: no navigation, click or typing
  at CASUAL or APPROVED, no run window page, runnable as a child adding no
  site, any site once TRUSTED, a fixed address added later allowing only its
  site); a fixed host with a `{{variable}}` in its query is no site, and is
  refused once confirmed; only a change is confirmed with none (not an agent
  draft, a voice routine, a change with a site, a non-draft, a TRUSTED or
  ADMIN change; a stored whitelist cleared). `molt_ai_agent_storage_unittest.cc`:
  getWorkflow's `confirmWithoutSites`, the confirmation with an empty list
  and the token, audited `no_sites`; refused for an agent draft and a voice
  routine with no site; a save answers whether it may still be confirmed
  with none. `molt_automation_script_fuzzer`: a clamped file is confirmed
  with no site exactly when its steps target none and it has no voice step,
  and then allows no step's address nor a run window page. The WebUI smoke
  test: the no-site panel, its button, the empty list with the token (saved
  first when edited), the toast, and no button without the browser's own
  `true` or for an agent draft.

## Release notes (§3.6.12), optional

> A changed workflow whose addresses are all filled in from values has no
> site to confirm; **Confirm it with no sites** confirms it, and it then
> opens no page by itself until you add a step that opens a fixed address,
> or set it to Trusted in molt://ai-automation.
