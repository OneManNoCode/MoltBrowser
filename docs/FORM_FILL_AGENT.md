# Form-Fill Agent — Engineering Spec

Status: draft for review · Author: AI pairing session 2026-07-20 · Owner: Raj

## 1. What it does

A privacy-first, on-device agent that fills web forms for the user.

- **Primary — agentic form fill.** On a form page ("Apply on Indeed", a Greenhouse/
  Lever/Workday application, a Google Form, a checkout address block) the user says
  *"fill this for me."* The agent reads the form, maps the user's saved profile +
  résumé + chat instructions onto the fields, shows an editable review, fills the
  fields, and **stops before submit** — the user submits themselves.
- **Foundation / sibling — page-grounded understanding.** The same live-page
  extraction that lets the agent *see* a form also powers page Q&A ("what does this
  Wikipedia page say about X", "what is this field asking for?"). Shipped as a
  by-product of the extractor.

Everything (perception, reasoning, fill data) stays on the device. No page content
or profile leaves the machine.

## 2. Reuse map — most of the machinery already exists

| Need | Already in-tree | File |
|---|---|---|
| Perceive → reason → act loop | `WebAgent` ReAct loop (`Start(goal,on_step,on_done)`, `IterateOnce`/`AskLLM`/`DispatchAction`) | `chrome/browser/molt_ai/agents/web_agent.h` |
| Multi-field fill primitive | `WebAgent::DoFillForm()` (selector=value pairs), `DoType()` (querySelector + set value + input/change events) | `agents/web_agent.cc` |
| Robust field setting | `AutomationRunner`: label-aware matching + selector fallbacks (`data-testid → id → name → aria-label → path`), React-safe event dispatch, per-step screenshots | `chrome/browser/molt_ai/automation/automation_runner.*` |
| Structured LLM extraction | `AI_EXTRACT` step (LLM → schema-typed JSON from current page) + `AI_DECIDE` | `automation/automation_script.h` |
| Action vocabulary + guards | `agent_engine` `ActionType{FILL_FORM, TYPE_TEXT, …}`, `AgentTaskOptions{allow_form_fill, allowed_domains}` | `agents/agent_engine.h` |
| Reasoning + page context | `BrowserAIRuntime::StreamChat/RunPrompt`, `GetPageContext()` → `PageContext` | `runtime/browser_ai_runtime.h` |
| Résumé → text | Attachment extraction (doc/image → on-device text → grounded answers) | AI chat side panel |
| Task routing slot | `prompt_router` `TaskType::FORM_FILL` (bump target model → `qwen3-8b`) | `runtime/prompt_router.cc` |
| Run history / trace | Automation run output + per-step screenshots/trace viewer | Agent studio |

## 3. What's new (the actual work)

1. **`FormExtractor`** — the critical new perception layer. JS injected via the
   existing `DOMContentBridge`/`EvalJS` that walks the target form and returns a
   structured schema: per field `{selector, label, type, options, required,
   current_value, autocomplete}`. `label` is resolved in priority order
   `<label for>` → `aria-label`/`aria-labelledby` → `placeholder` → nearest preceding
   text. `selector` is a stable, unique locator with the same fallback chain the
   recorder already produces. Richer than today's text `ObservePage()` snapshot.
2. **`MoltProfile`** — an on-device, OSCrypt-encrypted identity store (same idiom as
   `keys.enc`): name, email, phone, address, links (LinkedIn/GitHub/portfolio), work
   history, education, and a résumé (extracted text + file path for file-upload
   fields). Populated from (a) a settings form, (b) a parsed résumé upload, or (c)
   chat ("remember my email is …"). This is the *data the agent fills from* —
   distinct from `PersonaSystem` (which is AI tone/role, not autofill data).
3. **`FillPlanner`** — takes `{form schema + relevant profile subset + user
   instruction}`, asks the local LLM for a strict-JSON **FillPlan**:
   `[{selector, label, value, confidence, source, needs_user}]`. Never fabricates —
   an unconfident/missing match sets `needs_user=true` and leaves the field blank.
   Reuses the `AI_EXTRACT` parse-and-repair loop for malformed JSON.
4. **`SensitiveFieldGuard`** — hard blocklist that is *never* auto-filled: passwords
   (`type=password`), card number/CVV (`autocomplete=cc-number|cc-csc`), SSN/national
   ID, bank account, security-question answers. Detected by input type + autocomplete
   tokens + name/label regex. Surfaced for the user to fill manually. Enforces the
   platform's prohibited-action rules at the code level.
5. **Review & Apply UI** (side panel) — the FillPlan as an editable list (field →
   value, with source + confidence), with the matching fields highlighted on the
   page. User edits/skips, then **Apply** fills via `AutomationRunner`. **Submit is a
   separate, explicit user action** — the agent never clicks submit on its own.

## 4. Flow

```
 trigger ("fill this form")
   → FormExtractor            perceive: structured field schema of the target form
   → SensitiveFieldGuard      strip/mark password, card, SSN, bank fields
   → FillPlanner (local LLM)  plan:   map profile → fields → FillPlan (strict JSON)
   → Review UI (side panel)   review: user edits/approves; page highlights fields
   → AutomationRunner         act:    set each field (focus, value, input+change, React-safe)
   → STOP                     user reviews the page and clicks the site's Submit
```

Multiple forms on a page → pick the largest/most-field form, or prompt the user to
click the one they mean.

## 5. Data model

```cpp
struct FormField {
  std::string selector;        // stable unique locator (+ fallbacks)
  std::string label;           // resolved human label
  std::string type;            // text|email|tel|textarea|select|radio|checkbox|file|date
  std::vector<std::string> options;   // for select/radio
  bool required = false;
  std::string current_value;
  std::string autocomplete;    // browser autocomplete hint, if any
  bool sensitive = false;      // set by SensitiveFieldGuard
};

struct FillStep {
  std::string selector;
  std::string label;
  std::string value;
  float confidence = 0.f;      // planner's confidence 0..1
  std::string source;          // "profile.email" | "resume" | "instruction" | ""
  bool needs_user = false;     // no confident match — leave blank, surface in review
  bool applied = false;        // set after AutomationRunner acts
};

// FillPlan = std::vector<FillStep>
```

## 6. The reasoning step

- **Model:** `qwen3-8b` (strong structured-output / tool-use) via `StreamChat` with a
  JSON-only system prompt; fall back to whatever is loaded. `tinyllama-1.1b` is too
  weak for reliable structured output — planner requires a real model be loaded.
- **Input:** compact form schema + only the *relevant* profile facts + the user's
  instruction. Keep the profile subset minimal (privacy + context budget).
- **Output:** strict JSON FillPlan; parse-and-re-ask on malformed output.
- **Ambiguity policy:** never invent data. No confident profile match →
  `needs_user=true`, blank value, shown in review.

## 7. Safety & privacy

- 100% on-device reasoning; profile stored encrypted locally; nothing leaves the box.
- **Never auto-fill** passwords, card/CVV, SSN/national ID, bank account, security
  answers (SensitiveFieldGuard) — matches the platform's prohibited actions.
- **Never auto-submit.** Any irreversible click (submit/pay/confirm) is the user's.
- Respect `allowed_domains`; the review header names the domain being filled.
- First use on a site asks consent to use the saved profile there.
- Every fill logs as an automation run (reuse the trace viewer) — a visible audit of
  exactly what was written.

## 8. Phased delivery

- **Phase 0 — spike (days).** `FormExtractor` JS + render the extracted schema in the
  side panel (no fill). Prove perception on Indeed, Greenhouse, Lever, Workday,
  Google Forms.
- **Phase 1 — MVP.** `MoltProfile` settings form (manual entry) + `FillPlanner` +
  Review UI + Apply via `AutomationRunner` for text/email/tel/textarea/select. No
  file upload, no submit. → delivers the Indeed demo.
- **Phase 2.** Résumé upload → auto-populate `MoltProfile` (reuse attachment
  extraction) + file-upload fields (attach résumé) + radio/checkbox/date + multi-page
  forms (fill → next → fill).
- **Phase 3.** Page-grounded Q&A sibling (same extractor, prose mode) +
  "explain this field" + optional guarded submit.

## 9. Open decisions (need Raj's steer)

1. **Profile source first?** Manual settings form (predictable) vs résumé-parse
   (wow demo, noisier). *Rec: manual form in Phase 1, résumé-parse in Phase 2.*
2. **Submit policy?** Hard never-auto-submit (safest) vs guarded auto-submit behind a
   big confirm. *Rec: never-auto-submit for v1.*
3. **Blocklist breadth?** Hard-block passwords/cards/SSN always; also gate DOB / full
   address behind a per-site opt-in? *Rec: yes, opt-in for DOB/address.*
4. **Planner model?** Default to `qwen3-8b` and require a capable model be loaded, vs
   allow any. *Rec: require ≥ the 8B tier; refuse to plan on TinyLlama.*
```
