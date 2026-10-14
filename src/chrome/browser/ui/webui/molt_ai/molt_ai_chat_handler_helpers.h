// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Shared private helpers for the MoltAIChatHandler's per-feature .cc
// files. Anything that's used by more than one of the per-feature
// implementations (vault, voice, form fill, tor, etc.) lives here.
//
// Why a header and not the .cc's anonymous namespace: when we split
// molt_ai_chat_handler.cc into per-feature files (code-review #10),
// utilities like SecureClear and JsStringLiteral can't sit in any
// single TU's anonymous namespace anymore. Putting them in a header
// with `inline` linkage avoids ODR violations.

#ifndef CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_HELPERS_H_
#define CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_HELPERS_H_

#include <cstddef>
#include <string>
#include <string_view>

#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/values.h"

namespace molt_ai {
namespace chat_handler_helpers {

// ---- WebUI message argument validation ----
//
// chrome.send() arguments come straight from the WebUI renderer.
// base::Value::GetString() / GetDict() / GetInt() CHECK on a type
// mismatch and args[i] past the end is out of bounds, so one malformed
// message used to be enough to crash the whole browser. Every handler
// validates before touching |args|:
//
//   if (!HasCallbackArgs(args, 2u, __func__)) return;
//   const std::string callback_id = args[0].GetString();
//
// and reads every other argument with an is_*() check first (or with
// StringArg() below). When the callback id itself is missing there is
// nothing to resolve or reject, so the message is logged and dropped.

// Callback ids are "<method>_<counter>"; anything longer is not ours.
inline constexpr size_t kMaxCallbackIdLength = 256;

// True when |args| holds at least |min_size| values and args[0] is a
// plausible callback id string. Logs and returns false otherwise.
[[nodiscard]] inline bool HasCallbackArgs(const base::ListValue& args,
                                          size_t min_size,
                                          std::string_view handler) {
  if (!args.empty() && args.size() >= min_size && args[0].is_string() &&
      args[0].GetString().size() <= kMaxCallbackIdLength) {
    return true;
  }
  LOG(WARNING) << "[MoltAI] " << handler
               << ": dropping malformed WebUI message (" << args.size()
               << " args)";
  return false;
}

// args[index] when it exists and is a string, else |fallback|. Never
// CHECKs, whatever the renderer sent.
inline std::string StringArg(const base::ListValue& args,
                             size_t index,
                             std::string_view fallback = {}) {
  if (index < args.size() && args[index].is_string()) {
    return args[index].GetString();
  }
  return std::string(fallback);
}

// JSON-encode an arbitrary string so it can be safely embedded inside
// a JS source literal passed to ExecuteJavaScriptInIsolatedWorld.
// Handles backslash, double-quote, all control characters, and the
// JS-string-breaking U+2028 / U+2029 separators. Pair to JSQuote in
// automation_runner.cc.
inline std::string JsStringLiteral(const std::string& s) {
  std::string out;
  base::JSONWriter::Write(base::Value(s), &out);
  return out;
}

// Best-effort zero of a std::string's backing buffer before it goes
// out of scope. Used for transient buffers that briefly hold vault
// credentials before being handed off to the renderer via
// ExecuteJavaScriptInIsolatedWorld. The volatile pointer prevents
// the compiler from optimizing out the dead store. Not airtight (SSO,
// small-string-in-register cases, plus the renderer still holds the
// value during the eval) but reduces the in-memory plaintext window
// from "until heap reuse" to "until the next instruction".
// Code-review MEDIUM #7.
inline void SecureClear(std::string& s) {
  if (s.empty()) return;
  volatile char* p = const_cast<volatile char*>(s.data());
  for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
  s.clear();
}

}  // namespace chat_handler_helpers
}  // namespace molt_ai

#endif  // CHROME_BROWSER_UI_WEBUI_MOLT_AI_MOLT_AI_CHAT_HANDLER_HELPERS_H_
