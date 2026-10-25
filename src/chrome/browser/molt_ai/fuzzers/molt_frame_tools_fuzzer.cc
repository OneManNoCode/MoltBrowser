// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Site tools in frames of other sites (agents/frame_tools.h): the pairs a
// profile stored (read back from its prefs, which aren't trusted), answers
// given on top of them, the web agent's question, and the check that keeps
// secrets from another site's frame's tool. The input is JSON: {"stored":
// [...the stored list...], "allow": [[main, frame, "session" | "always" |
// other], ...], "remove": [[main, frame], ...], "question": [main, frame,
// count], "arguments": "...the arguments' JSON..."}; any part may be
// missing.
//
// Checks: no memory or UB error; a stored list reads back to pairs within
// every cap, each of two different well-formed origins, "always", and
// writes back to a list that reads the same; answers keep the caps and
// never allow a malformed origin; the question shows no character that
// hides or reorders text; arguments pass only if they are a JSON object.

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/agents/frame_tools.h"
#include "chrome/browser/molt_ai/agents/hidden_chars.h"

namespace molt_ai::frame_tools {
namespace {

void CheckTable(const PermissionTable& table) {
  CHECK_LE(table.size(), 2 * kMaxPairs);
  std::map<std::string, size_t> per_main;
  size_t always = 0;
  size_t session = 0;
  for (const Pair& pair : table.List()) {
    CHECK(IsPairOrigin(pair.main_origin));
    CHECK(IsPairOrigin(pair.frame_origin));
    CHECK_NE(pair.main_origin, pair.frame_origin);
    CHECK(pair.grant == Grant::kSession || pair.grant == Grant::kAlways);
    CHECK_GE(pair.since_ms, 0);
    CHECK_EQ(table.Get(pair.main_origin, pair.frame_origin), pair.grant);
    CHECK_LE(++per_main[pair.main_origin], kMaxFramesPerSite);
    (pair.grant == Grant::kAlways ? always : session)++;
  }
  CHECK_LE(always, kMaxPairs);
  CHECK_LE(session, kMaxPairs);
}

}  // namespace
}  // namespace molt_ai::frame_tools

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::frame_tools;
  const std::string json(reinterpret_cast<const char*>(data), size);
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return 0;
  const base::DictValue& in = parsed->GetDict();

  // The stored list.
  PermissionTable table;
  if (const base::Value* stored = in.Find("stored")) {
    table = PermissionTable::FromStored(*stored);
    CheckTable(table);
    for (const Pair& pair : table.List())
      CHECK(pair.grant == Grant::kAlways);
    CHECK_LE(table.size(), kMaxPairs);
    // Written back, it reads the same.
    const PermissionTable again =
        PermissionTable::FromStored(base::Value(table.ToStored()));
    const std::vector<Pair> a = table.List();
    const std::vector<Pair> b = again.List();
    CHECK_EQ(a.size(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
      CHECK_EQ(a[i].main_origin, b[i].main_origin);
      CHECK_EQ(a[i].frame_origin, b[i].frame_origin);
      CHECK_EQ(a[i].since_ms, b[i].since_ms);
    }
  }

  // Answers on top.
  if (const base::ListValue* allow = in.FindList("allow")) {
    for (const base::Value& entry : *allow) {
      if (!entry.is_list() || entry.GetList().size() != 3 ||
          !entry.GetList()[0].is_string() || !entry.GetList()[1].is_string() ||
          !entry.GetList()[2].is_string()) {
        continue;
      }
      const std::string& main_origin = entry.GetList()[0].GetString();
      const std::string& frame_origin = entry.GetList()[1].GetString();
      const std::optional<Grant> grant =
          ParseAnswer(entry.GetList()[2].GetString());
      const bool ok = table.Allow(main_origin, frame_origin,
                                  grant.value_or(Grant::kNone), 1);
      if (ok) {
        CHECK(IsPairOrigin(main_origin) && IsPairOrigin(frame_origin));
        CHECK(table.Get(main_origin, frame_origin) != Grant::kNone);
      } else if (!IsPairOrigin(main_origin) || !IsPairOrigin(frame_origin)) {
        CHECK(table.Get(main_origin, frame_origin) == Grant::kNone);
      }
    }
    CheckTable(table);
  }
  if (const base::ListValue* remove = in.FindList("remove")) {
    for (const base::Value& entry : *remove) {
      if (entry.is_list() && entry.GetList().size() == 2 &&
          entry.GetList()[0].is_string() && entry.GetList()[1].is_string()) {
        table.Remove(entry.GetList()[0].GetString(),
                     entry.GetList()[1].GetString());
        CHECK(table.Get(entry.GetList()[0].GetString(),
                        entry.GetList()[1].GetString()) == Grant::kNone);
      }
    }
    CheckTable(table);
  }

  // The question.
  if (const base::ListValue* q = in.FindList("question");
      q && q->size() == 3 && (*q)[0].is_string() && (*q)[1].is_string() &&
      (*q)[2].is_int() && (*q)[2].GetInt() >= 0) {
    const std::string text =
        QuestionText((*q)[0].GetString(), (*q)[1].GetString(),
                     static_cast<size_t>((*q)[2].GetInt()));
    for (size_t i = 0; i < text.size(); ++i)
      CHECK_EQ(molt_ai::hidden_chars::HiddenCharAt(text, i), 0u);
  }

  // A call's arguments.
  if (const std::string* args = in.FindString("arguments")) {
    if (!ArgumentsLookSecret(*args)) {
      std::optional<base::Value> v =
          base::JSONReader::Read(*args, base::JSON_PARSE_RFC);
      CHECK(v && v->is_dict()) << "only an object passes";
    }
  }
  return 0;
}
