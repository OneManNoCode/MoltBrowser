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

#include <string>

#include "base/json/json_writer.h"
#include "base/values.h"

namespace molt_ai {
namespace chat_handler_helpers {

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
