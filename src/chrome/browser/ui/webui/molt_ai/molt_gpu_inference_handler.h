// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// "Use the GPU for on-device AI" on molt://ai-settings (id="gpuSection";
// docs/design/SANDBOXED_INFERENCE.md section 11.11): Automatic or Off, and,
// when the lasting GPU record turned the GPU off for a driver that failed,
// which one and a "Try the GPU again" button. Shown only in builds with GPU
// inference (the state says "available"); the setting and the record are
// browser-wide, in Local State.
//
// Messages (every one validated with HasCallbackArgs() and the typed
// argument helpers; nothing from the page reaches a CHECK):
//   gpuInferenceGetState(cb)
//   gpuInferenceSetMode(cb, "auto" | "off")    needs a real click
//   gpuInferenceRetry(cb)                       needs a real click
// each resolving with the section state:
//   {available, automatic, disabledBySwitch, offForSession,
//    drivers: [{name, version, since, unidentified}], error?}
// The two that change something check HasTransientUserActivation() on the
// page's frame: the WebUI message pipe is frame-associated, so the click's
// activation reaches the browser first. A script can't flip them.
//
// The section's script is kGpuInferenceSettingsJs, appended to the page's
// app.js; it builds DOM with createElement/textContent only (a GPU name came
// from the inference service, sanitized, and is still treated as untrusted
// text) and wires its controls through moltActions().

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_GPU_INFERENCE_HANDLER_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_GPU_INFERENCE_HANDLER_H_

#include <optional>
#include <string>

#include "base/values.h"
#include "content/public/browser/web_ui_message_handler.h"

class MoltGpuInferenceHandler : public content::WebUIMessageHandler {
 public:
  MoltGpuInferenceHandler();
  MoltGpuInferenceHandler(const MoltGpuInferenceHandler&) = delete;
  MoltGpuInferenceHandler& operator=(const MoltGpuInferenceHandler&) = delete;
  ~MoltGpuInferenceHandler() override;

  // content::WebUIMessageHandler:
  void RegisterMessages() override;

 private:
  void HandleGetState(const base::ListValue& args);
  void HandleSetMode(const base::ListValue& args);
  void HandleRetry(const base::ListValue& args);

  // The page's frame has a transient user activation (a real click).
  bool HasUserClick();
  void Resolve(const std::string& callback_id,
               std::optional<std::string> error);
};

// The section's script, appended to molt://ai-settings' app.js after the
// page's own (it uses sendWithPromise(), showToast() and moltActions()).
extern const char kGpuInferenceSettingsJs[];

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_GPU_INFERENCE_HANDLER_H_
