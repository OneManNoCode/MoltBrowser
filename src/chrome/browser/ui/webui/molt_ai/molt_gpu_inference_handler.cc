// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/ui/webui/molt_ai/molt_gpu_inference_handler.h"

#include <utility>

#include "base/functional/bind.h"
#include "chrome/browser/molt_ai/runtime/browser_ai_runtime.h"
#include "chrome/browser/ui/webui/molt_ai/molt_ai_chat_handler_helpers.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_ui.h"

namespace {

using molt_ai::chat_handler_helpers::BoundedStringArg;
using molt_ai::chat_handler_helpers::HasCallbackArgs;

constexpr size_t kMaxModeBytes = 8;

base::DictValue BuildState(const molt_ai::GpuInferenceState& state) {
  base::DictValue dict;
  dict.Set("available", state.available);
  dict.Set("automatic", state.automatic);
  dict.Set("disabledBySwitch", state.disabled_by_switch);
  dict.Set("offForSession", state.off_for_session);
  base::ListValue drivers;
  for (const molt_ai::GpuInferenceState::Driver& driver : state.turned_off) {
    base::DictValue row;
    row.Set("name", driver.name);
    row.Set("version", driver.driver_version);
    row.Set("since", static_cast<double>(driver.since_ms));
    row.Set("unidentified", driver.unidentified);
    drivers.Append(std::move(row));
  }
  dict.Set("drivers", std::move(drivers));
  return dict;
}

}  // namespace

MoltGpuInferenceHandler::MoltGpuInferenceHandler() = default;
MoltGpuInferenceHandler::~MoltGpuInferenceHandler() = default;

void MoltGpuInferenceHandler::RegisterMessages() {
  web_ui()->RegisterMessageCallback(
      "gpuInferenceGetState",
      base::BindRepeating(&MoltGpuInferenceHandler::HandleGetState,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "gpuInferenceSetMode",
      base::BindRepeating(&MoltGpuInferenceHandler::HandleSetMode,
                          base::Unretained(this)));
  web_ui()->RegisterMessageCallback(
      "gpuInferenceRetry",
      base::BindRepeating(&MoltGpuInferenceHandler::HandleRetry,
                          base::Unretained(this)));
}

bool MoltGpuInferenceHandler::HasUserClick() {
  content::RenderFrameHost* rfh = web_ui()->GetRenderFrameHost();
  return rfh && rfh->HasTransientUserActivation();
}

void MoltGpuInferenceHandler::Resolve(const std::string& callback_id,
                                      std::optional<std::string> error) {
  molt_ai::BrowserAIRuntime& runtime = molt_ai::BrowserAIRuntime::GetInstance();
  // Idempotent and non-blocking: reads the setting and the record from Local
  // State the first time.
  runtime.Initialize();
  base::DictValue state = BuildState(runtime.GetGpuInferenceState());
  if (error) state.Set("error", *error);
  ResolveJavascriptCallback(base::Value(callback_id),
                            base::Value(std::move(state)));
}

void MoltGpuInferenceHandler::HandleGetState(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  Resolve(args[0].GetString(), std::nullopt);
}

void MoltGpuInferenceHandler::HandleSetMode(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 2u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  const std::optional<std::string> mode =
      BoundedStringArg(args, 1, kMaxModeBytes);
  if (!mode || (*mode != "auto" && *mode != "off")) {
    Resolve(callback_id, std::string("Unknown setting."));
    return;
  }
  if (!HasUserClick()) {
    Resolve(callback_id, std::string("Click the setting to change it."));
    return;
  }
  molt_ai::BrowserAIRuntime& runtime = molt_ai::BrowserAIRuntime::GetInstance();
  runtime.Initialize();
  if (runtime.GetGpuInferenceState().available) {
    runtime.SetGpuInferenceAutomatic(*mode == "auto");
  }
  Resolve(callback_id, std::nullopt);
}

void MoltGpuInferenceHandler::HandleRetry(const base::ListValue& args) {
  if (!HasCallbackArgs(args, 1u, __func__)) return;
  AllowJavascript();
  const std::string callback_id = args[0].GetString();
  if (!HasUserClick()) {
    Resolve(callback_id, std::string("Click the button to try again."));
    return;
  }
  molt_ai::BrowserAIRuntime& runtime = molt_ai::BrowserAIRuntime::GetInstance();
  runtime.Initialize();
  if (runtime.GetGpuInferenceState().available) {
    runtime.RetryGpuInference();
  }
  Resolve(callback_id, std::nullopt);
}

// Embedded in app.js after the page's own script.
const char kGpuInferenceSettingsJs[] = R"JS(
// ---- Use the GPU for on-device AI (section 11.11 of SANDBOXED_INFERENCE) ----
(function() {
  'use strict';
  var section = document.getElementById('gpuSection');
  if (!section) return;

  function el(tag, cls, text) {
    var e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text !== undefined && text !== null) e.textContent = String(text);
    return e;
  }
  function fmtDate(ms) {
    var n = Number(ms);
    return n > 0 ? new Date(n).toLocaleDateString() : '';
  }

  function render(s) {
    if (!s || typeof s !== 'object') return;
    // Only builds with GPU inference show the section.
    section.hidden = s.available !== true;
    if (section.hidden) return;
    document.getElementById('gpuModeAuto').checked = s.automatic !== false;
    document.getElementById('gpuModeOff').checked = s.automatic === false;
    var notes = [];
    if (s.disabledBySwitch === true) {
      notes.push('MoltBrowser was started with --disable-gpu, so on-device ' +
                 'AI runs on the processor whatever this says.');
    }
    if (s.offForSession === true) {
      notes.push('The graphics processor failed twice since MoltBrowser ' +
                 'started, so models run on the processor until it ' +
                 'restarts.');
    }
    document.getElementById('gpuNotes').textContent = notes.join(' ');
    var drivers = Array.isArray(s.drivers) ? s.drivers : [];
    var box = document.getElementById('gpuTurnedOff');
    box.replaceChildren();
    drivers.forEach(function(d) {
      if (!d || typeof d !== 'object') return;
      var name = d.unidentified === true || !d.name ?
          'A graphics driver that failed before it could be identified' :
          String(d.name);
      var text = name + (d.version ? ' (driver ' + String(d.version) + ')' :
                                     '') +
          ' failed several times, so MoltBrowser turned the GPU off for it' +
          (fmtDate(d.since) ? ' on ' + fmtDate(d.since) : '') +
          '. Models run on the processor until the driver changes.';
      box.appendChild(el('div', 'desc gpu-turned-off', text));
    });
    var retry = document.getElementById('gpuRetryBtn');
    retry.hidden = !(drivers.length || s.offForSession === true);
    document.getElementById('gpuStatus').textContent =
        s.error ? String(s.error) : '';
  }

  function call() {
    return sendWithPromise.apply(null, arguments).then(render)
        .catch(function() { showToast('Couldn\'t update the GPU setting'); });
  }

  moltActions({
    gpuSetMode: function() {
      call('gpuInferenceSetMode', this.value === 'off' ? 'off' : 'auto');
    },
    gpuRetry: function() {
      call('gpuInferenceRetry').then(function() {
        showToast('The next model load tries the GPU again');
      });
    }
  });

  call('gpuInferenceGetState');
})();
)JS";
