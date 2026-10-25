// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site tools in frames of other sites (docs/design/AGENT_ACTIONS.md
// §2.7.14): which frames' tools the user let the agents see, and what a
// call to one may carry.
//
// A frame of another site (agent_tools::CrossOriginToolFrames(): embedded
// by the main site itself, with the `tools` policy delegated to it) offers
// its tools to the web agent and the AI chat only once the user has
// allowed that frame's origin on the main document's origin: a permission
// per (main origin, frame origin) pair, never for a frame origin
// everywhere, nor for every frame of a site. The user gives it in the web
// agent's question ("Not now" / "This session" / "Always on this site"),
// with a real click:
//  * "This session" lasts until the browser quits (the profile's service,
//    frame_tool_permissions_service.h); Incognito and Guest have only this;
//  * "Always" is kept in the regular profile's prefs, until the user
//    removes it (the agent studio), clears "Site settings" or resets the
//    profile;
//  * MoltNet windows never list another site's frame's tools at all.
// Allowing a pair only lets the tools be listed: every call of one still
// asks the user (agent_tools::CheckToolCall()), read-only or not, naming
// both sites, and refuses arguments that look like a secret
// (ArgumentsLookSecret()).
//
// Pure logic (base::Value), unit-tested in molt_ai_unittests and fuzzed
// (molt_frame_tools_fuzzer): the stored pairs are read back from the
// profile's prefs, which this code doesn't trust either.

#ifndef CHROME_BROWSER_MOLT_AI_AGENTS_FRAME_TOOLS_H_
#define CHROME_BROWSER_MOLT_AI_AGENTS_FRAME_TOOLS_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/values.h"

namespace molt_ai {
namespace frame_tools {

// A pair's origins: url::Origin::Serialize() of an http(s) origin.
inline constexpr size_t kMaxOriginBytes = 256;
// Pairs a profile keeps (each kind), frame origins per main origin, and
// entries of the stored list looked at.
inline constexpr size_t kMaxPairs = 256;
inline constexpr size_t kMaxFramesPerSite = 16;
inline constexpr size_t kMaxStoredEntriesRead = 512;

enum class Grant {
  kNone,
  // Until the browser quits.
  kSession,
  // Kept in the profile.
  kAlways,
};

// Whether |origin| may name a side of a pair: the form url::Origin::
// Serialize() gives an http(s) origin, "https://" or "http://", a host of
// lowercase letters, digits, '-' and '.' (not starting or ending with
// '.'), or a bracketed IPv6 literal, then an optional ":port" of 1-5
// digits; nothing else (no path, user info, query, upper case or
// whitespace), at most kMaxOriginBytes. An opaque origin ("null") never.
bool IsPairOrigin(std::string_view origin);

struct Pair {
  std::string main_origin;
  std::string frame_origin;
  Grant grant = Grant::kNone;
  // When the user allowed it (milliseconds since the Unix epoch).
  int64_t since_ms = 0;
};

// The pairs one profile knows of.
class PermissionTable {
 public:
  PermissionTable();
  ~PermissionTable();
  PermissionTable(const PermissionTable&);
  PermissionTable& operator=(const PermissionTable&);
  PermissionTable(PermissionTable&&);
  PermissionTable& operator=(PermissionTable&&);

  // kNone unless the pair was allowed.
  Grant Get(std::string_view main_origin, std::string_view frame_origin) const;

  // Allows a pair. False, and nothing changes, for origins that aren't
  // IsPairOrigin(), the same origin on both sides, kNone, or when the caps
  // are reached (kMaxPairs of that kind, kMaxFramesPerSite for the main
  // origin). A kSession answer for a pair already kAlways keeps kAlways.
  bool Allow(std::string_view main_origin,
             std::string_view frame_origin,
             Grant grant,
             int64_t now_ms);

  // The frame origins allowed on |main_origin|, sorted.
  std::vector<std::string> AllowedFrames(std::string_view main_origin) const;

  bool Remove(std::string_view main_origin, std::string_view frame_origin);
  void RemoveAll();
  // Every pair, sorted by main origin then frame origin.
  std::vector<Pair> List() const;
  size_t size() const { return pairs_.size(); }

  // The kAlways pairs as a profile stores them: [{"main": origin, "frame":
  // origin, "since": milliseconds (a double)}, ...], sorted.
  base::ListValue ToStored() const;
  // A table of the kAlways pairs of |stored|: entries that aren't exactly
  // that form (no other keys; origins IsPairOrigin() and different; since
  // a whole, non-negative number of milliseconds) are dropped, each pair is
  // kept once, at most kMaxStoredEntriesRead entries are looked at and the
  // caps hold.
  static PermissionTable FromStored(const base::Value& stored);

 private:
  size_t CountOf(Grant grant) const;
  size_t FramesOf(std::string_view main_origin) const;

  std::map<std::pair<std::string, std::string>, Pair> pairs_;
};

// The web agent's question before it lists the tools of a frame of
// |frame_origin| on a page of |main_origin| that has |tool_count| of them:
// "https://widget.example, a frame on https://shop.example, offers 3 tools.
// Let the agent see them on https://shop.example? It asks you before each
// call." The origins are the browser's (IsPairOrigin()), shown with
// characters that hide or reorder text replaced.
std::string QuestionText(std::string_view main_origin,
                         std::string_view frame_origin,
                         size_t tool_count);

// The user's answer to the question, as the page sends it: "not_now",
// "session" or "always". Anything else is no answer.
std::optional<Grant> ParseAnswer(std::string_view answer);

// Whether a call to another site's frame's tool must be refused because
// its arguments (|arguments_json|, the object agent_tools::ReadArguments()
// wrote) carry something that looks like a secret: an argument named like
// one (password, PIN, card number, code, key ...), or a string (or a
// string of a list) that looks like a password, key, card number, one-time
// code, SSN or IBAN, hidden characters ignored. Credentials stay on the
// page the user is on, never go to a frame of another site. True for
// anything but a JSON object, too.
bool ArgumentsLookSecret(std::string_view arguments_json);

}  // namespace frame_tools
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_AGENTS_FRAME_TOOLS_H_
