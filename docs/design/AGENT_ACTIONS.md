# Structured model decisions in the agent and Agent mode

Status: implemented (October 2026), pending a compile on the build machine
and an end-to-end run with a real model. The web agent and Agent-mode
replay get every decision they take from the on-device model as a JSON
object decoded under a grammar (`PromptOptions::json_schema`,
docs/design/SANDBOXED_INFERENCE.md §9), and the browser reads it back
strictly. The page-facing checks are unchanged: `agent_safety` and
`automation_security` (CLAUDE.md "Agent mode") still decide what may happen.

| Decision | Code | Format |
|---|---|---|
| The web agent's next step | `agents/agent_actions.*`, `agents/web_agent.cc` | one action object (§2) |
| `ai_decide` step | `automation/automation_ai_steps.*`, `automation_runner.cc` | `{"decision": "SUCCESS" \| "FAIL"}` (§3.1) |
| `ai_extract` step | same | the step's fields, or any object within caps (§3.2) |
| Selector recovery (a recorded element is lost) | same | `{"selector": "..."}` (§3.3) |

Not covered: the AI chat's `[[ACTION ...]]` / slash-command actions
(`molt_ai_chat_handler.cc`) still parse free text; they go through the same
`agent_safety` checks.

## 1. Principles

1. **The grammar shapes, the browser decides.** The schema makes an honest
   model write one well-formed answer. It is not a security boundary: the
   reply comes from the sandboxed inference process, which the browser
   treats as hostile (§5.2 of SANDBOXED_INFERENCE.md), so every reply is
   parsed with `base::JSONReader` (RFC 8259, memory-safe) and validated
   again: exact keys, types, lengths in characters, characters, enum
   membership. A compromised service can return any JSON; the checks
   enforce what the grammar does (except key order and whitespace, which
   don't matter here) and more that it can't express: no control
   characters, no blank required values, no element twice.
2. **Schemas are ours and always supported.** Every schema is built from a
   fixed template with bounded enums and lengths. The unit tests run them,
   at their caps, through the service's real converter
   (`engine/json_schema_grammar.cc`), so `constraint_unsupported` means a
   bug or a misbehaving service. It stops the run (or the step); it never
   falls back to free text.
3. **No lax parser anywhere.** The old line parser (web agent), substring
   match (`ai_decide`) and raw-text fallback (`ai_extract`) are gone.
   Keeping one as a fallback would let a misbehaving service choose the
   weaker path by answering `constraint_unsupported`.
4. **An unusable answer is never acted on or stored.** It is asked for
   again a bounded number of times, then the run or the step fails with a
   fixed message.

## 2. The web agent

### 2.1 Observation: element ids

After each observation `WebAgent` runs `BuildElementSnapshotScript()` in
its isolated world. The script lists the page's visible interactive
elements (links, buttons, inputs, textareas, selects, checkboxes, radios,
ARIA widgets, contenteditable) and picks at most **40**: those in the
viewport first, then form fields, then buttons, then links further down,
returned in page order. It scans at most 4000 candidates.

For each it returns a selector **it builds itself**: `#id` when
`getElementById` resolves the id to that element (so `querySelector`
returns it too), else an `nth-of-type` path from `<html>` (at most 64
levels, 1024 bytes; otherwise the element is dropped). It also returns
the kind, the `<input>` type, `autocomplete` (64 chars) and a label
(aria-label, aria-labelledby, `<label>`, placeholder, text, title, alt,
name; 60 chars). What is typed in fields is never read (only a submit
button's value, which is its label).

`ParseElementSnapshot()` re-checks all of it (sizes, control characters,
a fixed kind vocabulary), classifies each field with
`agent_safety::ClassifyField`, and assigns ids `e1`...`e40`. An element is
a **text field** (TYPE / FILL_FORM target) if it is a `<textarea>` or a
text-like `<input>` (text, search, email, url, tel, number, date, time,
datetime-local, month, week) and not a password field (by type or
`autocomplete`). Payment-card and one-time-code fields stay text fields;
the probe asks the user before typing into them, as before.

The snapshot is kept only if it came from the page the run is on: the
committed origin must be an http(s) origin in the run's allowed set and
equal the `location.origin` the script read. Otherwise the element list is
empty (and CLICK / TYPE / FILL_FORM are left out of the schema).

The prompt gets one line per element inside the untrusted-page fence:

```
ELEMENTS:
e1 link "Home"
e2 input[search] "Search products" (text field)
e3 button "Go"
e4 input[password] "Password" (password field: the agent never types here)
```

Labels are page text: line breaks and control characters become spaces
and double quotes become single quotes, so a label can't start a line of
its own or close its quotes to fake another element.

### 2.2 Schema

`BuildActionSchema(observation)` is an `anyOf` with one object per action
`WebAgent::RunAction()` performs. Every key is required, no other key is
allowed, and `"reason"` comes first (the model states why before it
commits; reasoning is off under a constraint):

| Action | Keys after `reason`, `action` | Caps |
|---|---|---|
| NAVIGATE | `url` | 1-1024 chars |
| SEARCH | `query` | 1-256 chars |
| CLICK | `target` | enum of every observed id |
| TYPE | `target`, `text` | enum of text-field ids; text 0-500 chars |
| FILL_FORM | `fields`: [{`target`, `text`}] | 1 to min(8, text fields) items |
| SCROLL | `direction` | `"down"` \| `"up"` |
| OBSERVE | (none) | |
| DONE / ERROR | `result` | 1-1000 chars |

`reason` is 0-160 characters. CLICK is left out when nothing was observed,
TYPE and FILL_FORM when there is no text field (an empty enum would be
unsatisfiable). The largest schema (40 text fields) is under 3 KiB and
its grammar about 2.3 KiB; the service caps schemas at 32 KiB. The reply
budget is 512 tokens.

The grammar can't constrain URL schemes (`pattern` is refused); NAVIGATE's
`url` is checked by `DoNavigate` as before (http(s) only, origin scope,
confirmation), and a refused URL is a failed step the model sees in its
history.

### 2.3 Validation (`ParseAction`)

- One JSON object, nesting at most 4; anything around it, a second
  object, comments or trailing commas are refused.
- `action` is exactly one of the nine names; the object has exactly that
  action's keys, with the right types.
- Lengths in characters (code points, as the grammar's `maxLength` counts
  them). `url`, `query`, `result` must not be blank.
- No control characters (`\u0000`, ESC, DEL, ...), which the grammar
  allows as `\u` escapes; `text` and `result` may contain `\n` and `\t`,
  the others not even those (so a `reason` can't carry the old
  `ACTION:` / `TARGET:` lines).
- `target` exists in the observation the model was shown; for TYPE and
  FILL_FORM it is a text field (so never a password field). FILL_FORM has
  1-8 fields, no element twice. `direction` is `down` or `up`.

`WebAgent` then maps ids to the selectors from its own snapshot. The model
never supplies a selector.

### 2.4 Unusable replies

`InterpretReply()`:

| Reply | Outcome |
|---|---|
| valid action | act |
| `invalid_output` (incl. cut off at the budget), or valid JSON that `ParseAction` refuses | ask again, with a fixed note in the prompt (e.g. "it named an element that is not in ELEMENTS"); no step is taken or counted |
| the 3rd unusable reply for one step, or the 6th in a run | stop: "The model's replies couldn't be used as browser actions..." |
| `constraint_unsupported` | stop (the schema is ours; this is a bug or a misbehaving service) |
| failed generation (no model, crash, timeout) | stop with the runtime's message |
| cancelled | stop |

The note never repeats the reply. Before, an unreadable reply became an
OBSERVE step, so a model that couldn't follow the format spent all 20
steps observing.

### 2.5 Other changes in the loop

- SCROLL observes the page afterwards (the element list favours what is in
  view); the old pixel form is gone.
- The step trace (`Step`) describes elements (`button "Go"`) instead of
  selectors; FILL_FORM lists `element = text` pairs in `value`.

## 3. Agent mode (`AutomationRunner`)

All three use temperature 0.1 and `RunPromptAsync` as before. A failed
answer **stores nothing** and fails the step; `ai_decide` and
`ai_extract` now get one retry by default (one more inference). For those
two, an unsupported constraint and a refused step schema end the run on
that step without retries (`BlockStep`): retrying can't help.

### 3.1 `ai_decide`

Schema `{"decision": enum ["SUCCESS", "FAIL"]}`, 32 tokens. The reply must
be an object with that single key and one of those two strings. Anything
else is no decision: the step fails and `__ai_decision` (and the step's
`store_as`) are cleared, so a following IF can't take a branch the model
never chose. Before, the answer was lower-cased and substring-matched for
"success", "yes" or "true": "unsuccessful" and "not true" were SUCCESS,
and a failed generation was FAIL.

The decision is also stored under the step's `store_as` (the studio has
always offered "Save answer as"), and booleans now render as `true` /
`false` in `{{variables}}`.

### 3.2 `ai_extract`

**The step's schema.** `extra["schema"]` maps field names to type names:

```json
{"type": "ai_extract", "value": "the cheapest flight", "store_as": "flight",
 "extra": {"schema": {"price": "int", "airline": "string", "stops": "int[]"}}}
```

Type names (any case): `string` / `str` / `text`, `int` / `integer`,
`number` / `float` / `double` / `decimal`, `bool` / `boolean`, and any of
them followed by `[]` for a list. The studio doesn't write schemas; they
come from hand-written or imported `.molt` files, so the schema is
untrusted input. `ExtractSpecFromStep()`:

- refuses anything that isn't an object of type-name strings (a full JSON
  Schema, nested objects, lists, numbers, a JSON string holding an object);
- checks the size first (O(1)): 1 to **16** fields;
- names: 1-64 bytes, `[A-Za-z_][A-Za-z0-9_]*`, not starting with `__`
  (the runner's own `__ai_decision`; `__proto__` would set the prototype
  of RUN_JS's `vars` object literal);
- refuses the whole schema on any bad entry, with a fixed message that
  never repeats the name.

It becomes `{"type": "object", "properties": {...}, "required": [all],
"additionalProperties": false}`, fields sorted by name, each
`{"type": [T, "null"]}` (null = not on the page); strings at most 256
characters; lists at most 16 items of strings of at most 128 characters
(never null items). The worst case (16 lists with 64-byte names) stays
well inside the converter's caps; the tests check it. The prompt lists the
fields ("- price: whole number, or null"). Budget: 1024 tokens.

**Without a schema** (every step the studio records) the answer stays
what it was, a JSON object of the model's choosing, so workflows that
read its fields in later steps or in RUN_JS keep working. The schema is
`{"type": "object"}`, the converter's "any value" form with an object at
the top: containers nested at most 3 deep (the top object included),
anything in them. The grammar can't constrain key names or counts, so the
browser checks the whole object before storing it and refuses it, never
trims it, when:

| Check | Cap |
|---|---|
| reply size (before parsing) | 8 KiB |
| container depth, top object included | 3 (the reader's own limit is 4) |
| keys, all objects together | 64 |
| values, containers and list items included | 256 |
| one string | 1000 characters |
| a key | 1-64 bytes of ASCII letters, digits, `_`, `-` and inner spaces, not starting with `__` (at any level: `__proto__`, `__ai_decision`) |
| a number | finite |

The top level must be an object (not a list, string or number). Strings
are cleaned and integral doubles become ints, as with a schema. Before,
the step stored whatever JSON the model wrote (any shape, any keys, any
size), or its raw text when it didn't parse.

**Validation and coercion with a schema** (`InterpretExtract`): exactly
the spec's keys; each value null or of its type; integers as ints (an
integral double from the reader becomes an int when it fits, else stays a
double); finite numbers; list sizes. Strings are cleaned before they are stored: `\r`
dropped, other control characters except `\n` and `\t` turned into
spaces, ends trimmed. Too long, wrong type, missing or extra key: the step
fails.

**Why nothing is stored on failure.** Variables are typed into pages
(`{{flight}}` in a TYPE step), put into prompts and URLs. Raw model text
there means whatever the page persuaded the model to write. A failed step
stops the run (after its retry) instead; the Runs timeline says why.

### 3.3 Selector recovery

`AskLLMForSelector` (after the recorded selectors and the text anchor
fail) asks for `{"selector": "..."}`, 1-200 characters. The reply must be
that single key, one line, no control characters; ends are trimmed.
Anything else is no selector (the step fails as before). A selector found
this way still goes through the element probe and the workflow policy.

## 4. What this changes for prompt injection

Constrained output removes a class of tricks; it does not make the model
trustworthy.

**What it stops:**

- **Extra steps in one reply.** The grammar ends after one object and the
  parser refuses a second one or any trailing text. Page text that makes
  the model write "...then NAVIGATE https://evil" gets at most a string
  inside `reason`, which is never executed.
- **Text outside the format.** There is no lax parser to pick an
  `ACTION:` line out of prose or a reason; `reason` can't contain a line
  break.
- **Inventing actions or keys.** Only the nine actions exist, each with
  exactly its keys (no `RUN_JS`, no `selector`).
- **Choosing arbitrary elements.** The model picks an id from the
  observation, not a CSS selector; typing targets only text fields, never
  a password field (grammar, parser, probe and the write script all
  enforce it).
- **Flipping a decision with wording.** `ai_decide` reads two exact
  values; "unsuccessful" is not SUCCESS.
- **Smuggling text into variables.** `ai_extract` stores only a checked
  object (typed fields with a schema; bounded, cleaned values under plain
  keys without one), never raw output, and never a `__proto__` or
  `__ai_decision` key.
- **Oversized output.** Every string has a cap (typed text 500, URLs
  1024, results 1000, extracted strings 256, or 1000 in a free-form
  object), and a free-form object is capped as a whole.

**What it doesn't change:**

- The model still chooses *which* allowed action, element and text, and
  page text can still steer that choice: click the element labelled "Next"
  that is really "Delete", type attacker text into a search field, open a
  URL the page suggests, finish with a result the page wrote, or decide
  SUCCESS because the page says so. Labels are page text and a page can
  mislabel its elements.
- A compromised inference service can still return any valid-looking
  action; the parser checks its shape, not its intent.
- URL schemes, origins, form submits, other sites, password, payment and
  one-time-code fields, and workflow trust are not the grammar's job. They
  stay with `agent_safety` (http(s) only, origin scope, credential-field
  detection, untrusted-text fencing), the element probe and the user's
  confirmations (web agent), and `automation_security` (whitelist, trust
  levels, submit and sensitive-field rules, Approved / Casual imports).
  None of those changed, and every action still goes through them.

## 5. Not done

- **The chat's `[[ACTION ...]]` actions** still parse free text
  (`molt_ai_chat_handler.cc`, outside this change).
- **Re-observing after TYPE / FILL_FORM.** The ids stay those of the last
  observation; the probe checks the live element at action time.
- **Label drift.** An id is resolved to its selector without checking that
  the element still has the label the model saw.
- **Frames and shadow DOM.** The snapshot lists the main document only, as
  the old selectors did.
- **A studio editor for extract schemas.** Only `.molt` files carry them.

## 6. Tests

**In `molt_ai_unittests`:**

- `agents/agent_actions_unittest.cc`: the snapshot parser (refs, field
  classification, malformed and oversized entries, the 40 cap, labels that
  try to forge prompt lines, UTF-8-safe truncation); the schema (refs per
  action, branches dropped without targets, accepted by the service's
  converter empty and at the caps); every action parsed; hostile replies
  (prose around JSON, two objects, comments, invalid UTF-8, nesting 10,000
  deep, unknown actions, extra / missing keys, wrong types, unknown ids,
  CSS selectors as targets, typing into password fields / buttons / links /
  checkboxes, duplicate and too many fields, length caps at and over the
  limit counted in characters (multi-byte and escaped), control
  characters, blank values, the old format inside a field); the retry /
  stop table.
- `automation/automation_ai_steps_unittest.cc`: the decision schema and
  exact reading (the old substring cases refused); every outcome; the step
  schema's conversion (every type name, the exact JSON Schema, hostile
  names and types, a full JSON Schema refused, 16 fields at the caps with
  64-byte names accepted by the service's converter, 100,000 fields
  refused, a schema arriving through `Script::FromJSONString`); typed
  extraction, coercion, cleaning, mismatches and caps; free-form objects
  without a schema (stored with nesting, coercion and cleaning at every
  level; each cap at and over its limit: keys across nested objects,
  values, string characters, key bytes, reply bytes, depth; hostile
  replies: other top-level types, prose, trailing text, two objects,
  nesting 10,000 deep, `__proto__` / `__ai_decision` / `__x` keys at any
  level, keys with quotes, NUL, line breaks, dots, `{{`, non-ASCII or
  edge spaces, a number too big for a double); selector replies.

**Run outside the repo** (standalone, against `//base` stand-ins):

- Both test files above, with the real converter (49 tests), also under
  ASan and UBSan.
- Mutation checks: 46 single-point mutations of the parsers, validators,
  caps and outcome rules, free-form objects included; all are caught. Two
  survived at first (`\r` in the middle of extracted text, and the
  free-form string cap counted in bytes) and got a test each.
- Grammar agreement: each schema through the real converter into a GBNF
  recognizer (the one in `json_schema_grammar_unittest.cc`); hand-picked
  replies accepted or refused by the grammar match the browser parsers,
  including the caps at their limits.
- llama.cpp: every schema at its caps (40 text fields, no elements,
  buttons only; 16 extract fields with 64-byte names; the free-form
  object; decision; selector) through the converter into the vendored b11641
  `llama_grammar_init_impl`, which accepts each grammar, then sample
  replies fed code point by code point with `llama_grammar_accept`:
  accepted and refused as expected.
- The snapshot script in jsdom (spec `CSS.escape` polyfill, fake layout):
  kinds, labels, hidden / disabled / hidden-input elements skipped,
  selectors that resolve to the described element, duplicate and hostile
  ids, the 40 cap and its priority order, deep nesting and huge tag names
  dropped.
- Syntax of `web_agent.cc`, `agent_actions.cc`, `automation_ai_steps.cc`
  and the changed `AutomationRunner` functions (verbatim, with the real
  header) with `-fno-exceptions`, against declaration stand-ins with the
  152 signatures.
