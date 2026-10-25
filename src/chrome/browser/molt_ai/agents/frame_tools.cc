// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/agents/frame_tools.h"

#include <algorithm>
#include <cmath>

#include "base/json/json_reader.h"
#include "base/strings/string_number_conversions.h"
#include "chrome/browser/molt_ai/agents/agent_tools.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"
#include "chrome/browser/molt_ai/automation/agent_run_draft.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"

namespace molt_ai {
namespace frame_tools {

namespace {

// Arguments are an object of at most agent_tools::kMaxToolArguments values,
// lists one level deep: anything deeper isn't what ReadArguments() writes.
constexpr size_t kMaxArgumentsDepth = 3;
// Larger whole numbers are never stored by ReadArguments() (+-10^15); an
// exact double holds them.
constexpr double kMaxStoredMs = 9007199254740991.0;  // 2^53 - 1

bool IsHostChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
         c == '.' || c == '_';
}

bool IsIpv6Char(char c) {
  return (c >= 'a' && c <= 'f') || (c >= '0' && c <= '9') || c == ':' ||
         c == '.';
}

// |s| without the characters that hide or reorder text.
std::string StripHidden(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const size_t n = hidden_chars::HiddenCharAt(s, i);
    if (n) {
      i += n;
      continue;
    }
    out.push_back(s[i]);
    ++i;
  }
  return out;
}

bool TextLooksSecret(std::string_view text) {
  return automation::agent_draft::TextLooksSecret(text) ||
         automation::agent_draft::TextLooksSecret(StripHidden(text));
}

bool NameLooksSecret(std::string_view name) {
  return automation::agent_draft::LabelSuggestsSecret(name) ||
         automation::agent_draft::LabelSuggestsSecret(StripHidden(name)) ||
         agent_safety::IsSensitive(agent_safety::ClassifySelector(name));
}

bool ValueLooksSecret(const base::Value& value) {
  switch (value.type()) {
    case base::Value::Type::STRING:
      return TextLooksSecret(value.GetString());
    case base::Value::Type::INTEGER:
      return TextLooksSecret(base::NumberToString(value.GetInt()));
    case base::Value::Type::DOUBLE: {
      // A whole number may be a card number or a code written as one.
      const double d = value.GetDouble();
      if (std::isfinite(d) && std::fabs(d) < 1e16 && d == std::floor(d)) {
        return TextLooksSecret(
            base::NumberToString(static_cast<int64_t>(d)));
      }
      return false;
    }
    case base::Value::Type::LIST:
      for (const base::Value& item : value.GetList()) {
        if (item.is_list() || item.is_dict() || ValueLooksSecret(item))
          return true;
      }
      return false;
    case base::Value::Type::DICT:
      // ReadArguments() never writes one.
      return true;
    case base::Value::Type::NONE:
    case base::Value::Type::BOOLEAN:
    case base::Value::Type::BINARY:
      return false;
  }
  return true;
}

}  // namespace

bool IsPairOrigin(std::string_view origin) {
  if (origin.size() > kMaxOriginBytes)
    return false;
  std::string_view rest;
  if (origin.starts_with("https://")) {
    rest = origin.substr(8);
  } else if (origin.starts_with("http://")) {
    rest = origin.substr(7);
  } else {
    return false;
  }
  std::string_view host;
  std::string_view port;
  if (rest.starts_with("[")) {
    const size_t close = rest.find(']');
    if (close == std::string_view::npos || close < 3)
      return false;
    for (char c : rest.substr(1, close - 1)) {
      if (!IsIpv6Char(c))
        return false;
    }
    host = rest.substr(0, close + 1);
    rest = rest.substr(close + 1);
    if (!rest.empty() && rest[0] != ':')
      return false;
    port = rest.empty() ? std::string_view() : rest.substr(1);
    if (!rest.empty() && port.empty())
      return false;
  } else {
    const size_t colon = rest.find(':');
    host = rest.substr(0, colon);
    if (colon != std::string_view::npos) {
      port = rest.substr(colon + 1);
      if (port.empty())
        return false;
    }
    if (host.empty() || host.front() == '.' || host.back() == '.' ||
        host.find("..") != std::string_view::npos) {
      return false;
    }
    for (char c : host) {
      if (!IsHostChar(c))
        return false;
    }
  }
  if (port.size() > 5)
    return false;
  for (char c : port) {
    if (c < '0' || c > '9')
      return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// PermissionTable
// ---------------------------------------------------------------------------

PermissionTable::PermissionTable() = default;
PermissionTable::~PermissionTable() = default;
PermissionTable::PermissionTable(const PermissionTable&) = default;
PermissionTable& PermissionTable::operator=(const PermissionTable&) = default;
PermissionTable::PermissionTable(PermissionTable&&) = default;
PermissionTable& PermissionTable::operator=(PermissionTable&&) = default;

Grant PermissionTable::Get(std::string_view main_origin,
                           std::string_view frame_origin) const {
  auto it = pairs_.find(
      std::make_pair(std::string(main_origin), std::string(frame_origin)));
  return it == pairs_.end() ? Grant::kNone : it->second.grant;
}

size_t PermissionTable::CountOf(Grant grant) const {
  size_t n = 0;
  for (const auto& [key, pair] : pairs_) {
    if (pair.grant == grant)
      ++n;
  }
  return n;
}

size_t PermissionTable::FramesOf(std::string_view main_origin) const {
  size_t n = 0;
  for (const auto& [key, pair] : pairs_) {
    if (key.first == main_origin)
      ++n;
  }
  return n;
}

bool PermissionTable::Allow(std::string_view main_origin,
                            std::string_view frame_origin,
                            Grant grant,
                            int64_t now_ms) {
  if (grant == Grant::kNone || !IsPairOrigin(main_origin) ||
      !IsPairOrigin(frame_origin) || main_origin == frame_origin) {
    return false;
  }
  auto key = std::make_pair(std::string(main_origin), std::string(frame_origin));
  auto it = pairs_.find(key);
  if (it != pairs_.end()) {
    if (it->second.grant == Grant::kAlways || it->second.grant == grant)
      return true;
    // kSession to kAlways.
    if (CountOf(Grant::kAlways) >= kMaxPairs)
      return false;
    it->second.grant = grant;
    it->second.since_ms = now_ms < 0 ? 0 : now_ms;
    return true;
  }
  if (CountOf(grant) >= kMaxPairs ||
      FramesOf(main_origin) >= kMaxFramesPerSite) {
    return false;
  }
  Pair pair;
  pair.main_origin = key.first;
  pair.frame_origin = key.second;
  pair.grant = grant;
  pair.since_ms = now_ms < 0 ? 0 : now_ms;
  pairs_.emplace(std::move(key), std::move(pair));
  return true;
}

std::vector<std::string> PermissionTable::AllowedFrames(
    std::string_view main_origin) const {
  std::vector<std::string> out;
  for (const auto& [key, pair] : pairs_) {
    if (key.first == main_origin && pair.grant != Grant::kNone)
      out.push_back(key.second);
  }
  return out;
}

bool PermissionTable::Remove(std::string_view main_origin,
                             std::string_view frame_origin) {
  return pairs_.erase(std::make_pair(std::string(main_origin),
                                     std::string(frame_origin))) > 0;
}

void PermissionTable::RemoveAll() {
  pairs_.clear();
}

std::vector<Pair> PermissionTable::List() const {
  std::vector<Pair> out;
  out.reserve(pairs_.size());
  for (const auto& [key, pair] : pairs_)
    out.push_back(pair);
  return out;
}

base::ListValue PermissionTable::ToStored() const {
  base::ListValue list;
  for (const auto& [key, pair] : pairs_) {
    if (pair.grant != Grant::kAlways)
      continue;
    base::DictValue entry;
    entry.Set("main", pair.main_origin);
    entry.Set("frame", pair.frame_origin);
    entry.Set("since", static_cast<double>(pair.since_ms));
    list.Append(std::move(entry));
  }
  return list;
}

// static
PermissionTable PermissionTable::FromStored(const base::Value& stored) {
  PermissionTable table;
  if (!stored.is_list())
    return table;
  const base::ListValue& list = stored.GetList();
  const size_t n = std::min(list.size(), kMaxStoredEntriesRead);
  for (size_t i = 0; i < n; ++i) {
    if (!list[i].is_dict())
      continue;
    const base::DictValue& entry = list[i].GetDict();
    if (entry.size() != 3)
      continue;
    const std::string* main_origin = entry.FindString("main");
    const std::string* frame_origin = entry.FindString("frame");
    const base::Value* since = entry.Find("since");
    if (!main_origin || !frame_origin || !since)
      continue;
    // A whole number of milliseconds, as ToStored() writes it (a double; an
    // int if it was written by hand and small).
    double ms = -1;
    if (since->is_int())
      ms = since->GetInt();
    else if (since->is_double())
      ms = since->GetDouble();
    if (!std::isfinite(ms) || ms < 0 || ms > kMaxStoredMs ||
        ms != std::floor(ms)) {
      continue;
    }
    // A pair stored twice keeps its first entry; Allow() checks the rest.
    if (table.Get(*main_origin, *frame_origin) != Grant::kNone)
      continue;
    table.Allow(*main_origin, *frame_origin, Grant::kAlways,
                static_cast<int64_t>(ms));
  }
  return table;
}

// ---------------------------------------------------------------------------
// The question and the calls
// ---------------------------------------------------------------------------

std::string QuestionText(std::string_view main_origin,
                         std::string_view frame_origin,
                         size_t tool_count) {
  const std::string main_text = agent_tools::VisibleText(main_origin);
  const std::string frame_text = agent_tools::VisibleText(frame_origin);
  const std::string count =
      tool_count == 1 ? std::string("1 tool")
                      : base::NumberToString(tool_count) + " tools";
  return frame_text + ", a frame on " + main_text + ", offers " + count +
         ". Let the agent see them on " + main_text +
         "? It asks you before each call.";
}

std::optional<Grant> ParseAnswer(std::string_view answer) {
  if (answer == "not_now")
    return Grant::kNone;
  if (answer == "session")
    return Grant::kSession;
  if (answer == "always")
    return Grant::kAlways;
  return std::nullopt;
}

bool ArgumentsLookSecret(std::string_view arguments_json) {
  std::optional<base::DictValue> args = base::JSONReader::ReadDict(
      arguments_json, base::JSON_PARSE_RFC, kMaxArgumentsDepth);
  if (!args)
    return true;
  for (const auto [name, value] : *args) {
    if (NameLooksSecret(name) || ValueLooksSecret(value))
      return true;
  }
  return false;
}

}  // namespace frame_tools
}  // namespace molt_ai
