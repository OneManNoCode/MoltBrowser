// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_voice_learning_handler.h"

#include <optional>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/common/molt_user_click.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_hints.h"
#include "chrome/browser/molt_ai/voice/voice_learning.h"
#include "chrome/browser/molt_ai/voice/voice_vocabulary.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/web_ui.h"

namespace {

using molt_ai::chat_handler_helpers::BoolArg;
using molt_ai::chat_handler_helpers::BoundedStringArg;
using molt_ai::chat_handler_helpers::HasCallbackArgs;
using molt_ai::voice::RoutineSummary;
using molt_ai::voice::SiteHint;
using molt_ai::voice::VocabularyEntry;
using molt_ai::voice::VoiceLearning;

constexpr char kClickToChange[] = "Click the setting to change it.";
constexpr char kClickToForget[] = "Click the button to forget everything.";
// A key the page sends back: an id, a phrase or a host, at most.
constexpr size_t kMaxKeyBytes = 256;

}  // namespace

MoltVoiceLearningHandler::MoltVoiceLearningHandler() = default;
MoltVoiceLearningHandler::~MoltVoiceLearningHandler() = default;

void MoltVoiceLearningHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "voiceLearningGetState",
      base::BindRepeating(&MoltVoiceLearningHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceLearningSetEnabled",
      base::BindRepeating(&MoltVoiceLearningHandler::HandleSetEnabled,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceLearningForget",
      base::BindRepeating(&MoltVoiceLearningHandler::HandleForget,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "voiceLearningForgetAll",
      base::BindRepeating(&MoltVoiceLearningHandler::HandleForgetAll,
                          base::Unretained(this)));
}

void MoltVoiceLearningHandler::OnJavascriptDisallowed() {
  weak_factory_.InvalidateWeakPtrs();
}

VoiceLearning* MoltVoiceLearningHandler::Learning() {
  return VoiceLearning::Get(Profile::FromWebUI(web_ui()));
}

void MoltVoiceLearningHandler::Resolve(std::string callback_id,
                                       std::optional<std::string> error) {
  VoiceLearning* learning = Learning();
  if (!learning) {
    ResolveNow(std::move(callback_id), std::move(error));
    return;
  }
  learning->WhenLoaded(base::BindOnce(&MoltVoiceLearningHandler::ResolveNow,
                                      weak_factory_.GetWeakPtr(),
                                      std::move(callback_id),
                                      std::move(error)));
}

void MoltVoiceLearningHandler::ResolveNow(std::string callback_id,
                                          std::optional<std::string> error) {
  if (!IsJavascriptAllowed()) {
    return;
  }
  Profile* profile = Profile::FromWebUI(web_ui());
  VoiceLearning* learning = Learning();
  base::DictValue state;
  state.Set("available",
            molt_ai::voice::VoiceModeEnabled() && learning != nullptr);
  state.Set("moltnet", molt_ai::tor::IsMoltNetProfile(profile));
  state.Set("enabled", learning && learning->setting());
  base::ListValue routines;
  base::ListValue words;
  base::ListValue corrections;
  base::ListValue hints;
  if (learning) {
    for (const RoutineSummary& routine : learning->routines()) {
      base::DictValue r;
      r.Set("id", routine.id);
      r.Set("name", routine.name);
      r.Set("steps", routine.steps);
      r.Set("draft", routine.needs_sites_confirmed);
      routines.Append(std::move(r));
    }
    for (const VocabularyEntry& entry : learning->vocabulary().entries()) {
      base::DictValue e;
      if (entry.correction()) {
        e.Set("heard", entry.heard);
        e.Set("meant", entry.phrase);
        corrections.Append(std::move(e));
      } else {
        e.Set("phrase", entry.phrase);
        e.Set("uses", entry.uses);
        words.Append(std::move(e));
      }
    }
    for (const SiteHint& hint : learning->hints().hints()) {
      base::DictValue h;
      h.Set("host", hint.host);
      h.Set("search", hint.search_field);
      h.Set("list", hint.result_list);
      h.Set("player", hint.player);
      hints.Append(std::move(h));
    }
  }
  state.Set("routines", std::move(routines));
  state.Set("words", std::move(words));
  state.Set("corrections", std::move(corrections));
  state.Set("hints", std::move(hints));
  if (error) {
    state.Set("error", *error);
  }
  ResolveJavascriptCallback(base::Value(std::move(callback_id)),
                            base::Value(std::move(state)));
}

void MoltVoiceLearningHandler::HandleGetState(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  Resolve(args[0].GetString(), std::nullopt);
}

void MoltVoiceLearningHandler::HandleSetEnabled(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::optional<bool> enabled = BoolArg(args, 1);
  VoiceLearning* learning = Learning();
  if (!enabled || !learning) {
    Resolve(args[0].GetString(), std::nullopt);
    return;
  }
  if (!molt_ai::ConsumeUserClick(true, web_ui()->GetRenderFrameHost())) {
    Resolve(args[0].GetString(), kClickToChange);
    return;
  }
  learning->SetEnabled(*enabled);
  Resolve(args[0].GetString(), std::nullopt);
}

void MoltVoiceLearningHandler::HandleForget(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 3u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  const std::optional<std::string> kind = BoundedStringArg(args, 1, 16);
  const std::optional<std::string> key =
      BoundedStringArg(args, 2, kMaxKeyBytes);
  VoiceLearning* learning = Learning();
  if (!kind || !key || key->empty() || !learning) {
    Resolve(callback_id, std::nullopt);
    return;
  }
  // Forgetting only takes away what was learned: no click needed.
  if (*kind == "routine") {
    learning->DeleteRoutine(
        std::string(), *key,
        base::BindOnce(
            [](base::WeakPtr<MoltVoiceLearningHandler> self,
               std::string callback_id, bool deleted) {
              if (self) {
                self->Resolve(std::move(callback_id),
                              deleted ? std::nullopt
                                      : std::optional<std::string>(
                                            "That routine is already gone."));
              }
            },
            weak_factory_.GetWeakPtr(), callback_id));
    return;
  }
  if (*kind == "word" || *kind == "correction") {
    learning->ForgetWord(*key);
  } else if (*kind == "hint") {
    learning->ForgetHint(*key);
  }
  Resolve(callback_id, std::nullopt);
}

void MoltVoiceLearningHandler::HandleForgetAll(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) {
    return;
  }
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  VoiceLearning* learning = Learning();
  if (!learning) {
    Resolve(callback_id, std::nullopt);
    return;
  }
  // Everything at once, routines too: only with the click itself.
  if (!molt_ai::ConsumeUserClick(true, web_ui()->GetRenderFrameHost())) {
    Resolve(callback_id, kClickToForget);
    return;
  }
  learning->ForgetEverything(base::BindOnce(
      &MoltVoiceLearningHandler::Resolve, weak_factory_.GetWeakPtr(),
      callback_id, std::optional<std::string>()));
}

// Embedded in app.js after the page's own script.
const char kVoiceLearningSettingsJs[] = R"JS(
// ---- What voice mode learned ----
(function() {
  'use strict';
  var section = document.getElementById('voiceLearningSection');
  if (!section) return;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function str(v) { return typeof v === 'string' ? v : ''; }
  function list(v) { return Array.isArray(v) ? v.slice(0, 500) : []; }

  // One row: what was learned (text only) and its Forget button.
  function row(text, detail, kind, key, label) {
    var r = el('div', 'voice-learned');
    r.style.cssText = 'display:flex;gap:10px;align-items:center;margin-top:6px';
    r.appendChild(el('span', null, text));
    if (detail) r.appendChild(el('span', 'desc', detail));
    var forget = el('button', 'btn secondary', 'Forget');
    forget.type = 'button';
    forget.style.cssText = 'font-size:.75rem;padding:4px 12px;margin-left:auto';
    forget.setAttribute('data-kind', kind);
    forget.setAttribute('data-key', key);
    forget.setAttribute('aria-label', 'Forget ' + label);
    forget.setAttribute('data-on-click', 'voiceLearningForget');
    r.appendChild(forget);
    return r;
  }

  function fill(id, rows, empty) {
    var box = document.getElementById(id);
    box.replaceChildren();
    if (!rows.length) {
      box.appendChild(el('div', 'desc', empty));
      return;
    }
    rows.forEach(function(r) { box.appendChild(r); });
  }

  function render(s) {
    if (!s || typeof s !== 'object') return;
    section.hidden = s.available !== true;
    document.getElementById('voiceLearningEnabled').checked = s.enabled === true;
    var routines = list(s.routines).filter(function(r) {
      return r && typeof r === 'object' && str(r.id) && str(r.name);
    }).map(function(r) {
      var steps = Number(r.steps) || 0;
      var detail = steps + (steps === 1 ? ' step' : ' steps') +
          (r.draft === true ? ', its sites to confirm in Agent mode' : '');
      return row(r.name, detail, 'routine', r.id, 'the routine ' + r.name);
    });
    fill('voiceLearningRoutines', routines,
         'None yet. Do something by voice, then say "remember this as", and a name.');
    var words = list(s.words).filter(function(w) {
      return w && typeof w === 'object' && str(w.phrase);
    }).map(function(w) {
      var uses = Number(w.uses) || 0;
      return row(w.phrase, uses > 1 ? ('said ' + uses + ' times') : '', 'word',
                 w.phrase, 'the word ' + w.phrase);
    });
    fill('voiceLearningWords', words, 'None yet.');
    var corrections = list(s.corrections).filter(function(c) {
      return c && typeof c === 'object' && str(c.heard) && str(c.meant);
    }).map(function(c) {
      return row('"' + c.heard + '" means "' + c.meant + '"', '', 'correction',
                 c.heard, 'the correction of ' + c.heard);
    });
    fill('voiceLearningCorrections', corrections, 'None yet.');
    var hints = list(s.hints).filter(function(h) {
      return h && typeof h === 'object' && str(h.host);
    }).map(function(h) {
      var parts = [];
      if (str(h.search)) parts.push('search box "' + h.search + '"');
      if (str(h.list)) parts.push('results "' + h.list + '"');
      if (str(h.player)) parts.push('player "' + h.player + '"');
      return row(h.host, parts.join(', '), 'hint', h.host,
                 'the hints for ' + h.host);
    });
    fill('voiceLearningHints', hints, 'None yet.');
    var said = s.error ? String(s.error) : '';
    var status = document.getElementById('voiceLearningStatus');
    if (status.textContent !== said) status.textContent = said;
  }

  function call() {
    return sendWithPromise.apply(null, arguments).then(render)
        .catch(function() { showToast('Couldn\'t update what voice mode learned'); });
  }

  moltActions({
    voiceLearningSetEnabled: function() {
      call('voiceLearningSetEnabled', !!this.checked);
    },
    voiceLearningForget: function() {
      call('voiceLearningForget', this.getAttribute('data-kind') || '',
           this.getAttribute('data-key') || '');
    },
    voiceLearningForgetAll: function() {
      call('voiceLearningForgetAll');
    }
  });

  call('voiceLearningGetState');
})();
)JS";
