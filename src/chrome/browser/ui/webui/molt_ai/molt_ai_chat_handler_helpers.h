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
#include <optional>
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

// args[index] when it exists and is a bool, else nullopt.
inline std::optional<bool> BoolArg(const base::ListValue& args, size_t index) {
  if (index < args.size() && args[index].is_bool()) {
    return args[index].GetBool();
  }
  return std::nullopt;
}

// args[index] when it exists, is a string and is at most |max_bytes| long,
// else nullopt (an oversized string is refused, not cut).
inline std::optional<std::string> BoundedStringArg(const base::ListValue& args,
                                                   size_t index,
                                                   size_t max_bytes) {
  if (index < args.size() && args[index].is_string() &&
      args[index].GetString().size() <= max_bytes) {
    return args[index].GetString();
  }
  return std::nullopt;
}

// ---- The chat's window ----
//
// The window a chat acts in (its page actions and slash commands, the web
// agent, /fill, vault autofill, links from replies, the tab and privacy
// commands) is its own, never another one:
//  - a chat opened as a tab: the window holding that tab
//    (BrowserCollection::FindBrowserWithTab, |tab_owner|);
//  - the side panel: its WebContents is not a tab (views::WebView creates
//    it, and only a TabModel registers a WebContents as a tab, so
//    FindBrowserWithTab returns null for it); it is found by the window
//    that hosts it, FindBrowserWithWindow(GetTopLevelNativeWindow())
//    (|panel_host|).
// No other window: not the last active one, which can be another window
// (a page there would be the wrong page) and, in another profile, across
// MoltNet's boundary. |may_act(window)| is that boundary
// (molt_ai::tor::MayChatActInWindow for the chat's and the window's
// profiles), checked on the window found.
template <typename Window>
struct ChatWindowChoice {
  Window* window = nullptr;
  // The chat's window was found but is across MoltNet's boundary.
  bool crossed_moltnet = false;
};

template <typename Window, typename MayAct>
ChatWindowChoice<Window> ChooseChatWindow(Window* tab_owner,
                                          Window* panel_host,
                                          MayAct may_act) {
  ChatWindowChoice<Window> choice;
  Window* own = tab_owner ? tab_owner : panel_host;
  if (!own) {
    return choice;
  }
  if (!may_act(own)) {
    choice.crossed_moltnet = true;
    return choice;
  }
  choice.window = own;
  return choice;
}

// The page a chat action works on: the active tab of the chat's window,
// but never the chat itself (a chat opened as a tab is its window's active
// tab).
template <typename Contents>
Contents* ChatActionTarget(Contents* active_tab, const Contents* chat) {
  return active_tab == chat ? nullptr : active_tab;
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
