// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_learning_store.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"

namespace molt_ai {
namespace voice {

namespace {

constexpr char kVersion[] = "version";
constexpr char kLearn[] = "learn";
constexpr char kWords[] = "words";
constexpr char kCorrections[] = "corrections";
constexpr char kHints[] = "hints";
constexpr char kPhrase[] = "phrase";
constexpr char kHeard[] = "heard";
constexpr char kMeant[] = "meant";
constexpr char kUses[] = "uses";
constexpr char kLast[] = "last";
constexpr char kHost[] = "host";
constexpr char kSearch[] = "search";
constexpr char kList[] = "list";
constexpr char kPlayer[] = "player";
constexpr char kDeclinedRoutines[] = "declined_routines";
// A script id, at most (AutomationStorage's).
constexpr size_t kMaxRoutineIdBytes = 128;

// Unix seconds after this (year 33658) are refused.
constexpr double kMaxTime = 1e12;
// How deep the file may nest (it needs 3).
constexpr size_t kMaxDepth = 8;

std::optional<int64_t> TimeOf(const base::DictValue& d) {
  const base::Value* value = d.Find(kLast);
  if (!value) {
    return std::nullopt;
  }
  if (value->is_int()) {
    return value->GetInt() >= 0 ? std::optional<int64_t>(value->GetInt())
                                : std::nullopt;
  }
  if (!value->is_double()) {
    return std::nullopt;
  }
  const double t = value->GetDouble();
  if (!std::isfinite(t) || t < 0 || t > kMaxTime || std::floor(t) != t) {
    return std::nullopt;
  }
  return static_cast<int64_t>(t);
}

const std::string* StringOf(const base::DictValue& d, const char* key) {
  return d.FindString(key);
}

std::string StringOr(const base::DictValue& d, const char* key) {
  const std::string* value = d.FindString(key);
  return value ? *value : std::string();
}

// A script id as AutomationStorage takes it (a file name): not empty, at
// most kMaxRoutineIdBytes, no path or extension separator, no control
// character.
bool IsRoutineId(std::string_view id) {
  if (id.empty() || id.size() > kMaxRoutineIdBytes) {
    return false;
  }
  for (char c : id) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (c == '/' || c == '\\' || c == ':' || c == '.' || u < 0x20 ||
        u == 0x7f) {
      return false;
    }
  }
  return true;
}

}  // namespace

LearnedData::LearnedData() = default;
LearnedData::~LearnedData() = default;
LearnedData::LearnedData(const LearnedData&) = default;
LearnedData& LearnedData::operator=(const LearnedData&) = default;
LearnedData::LearnedData(LearnedData&&) = default;
LearnedData& LearnedData::operator=(LearnedData&&) = default;

std::string SerializeLearning(const LearnedData& data) {
  base::DictValue root;
  root.Set(kVersion, kLearningFormatVersion);
  root.Set(kLearn, data.learn);
  base::ListValue words;
  base::ListValue corrections;
  for (const VocabularyEntry& entry : data.vocabulary.entries()) {
    base::DictValue d;
    if (entry.correction()) {
      d.Set(kHeard, entry.heard);
      d.Set(kMeant, entry.phrase);
    } else {
      d.Set(kPhrase, entry.phrase);
    }
    d.Set(kUses, entry.uses);
    d.Set(kLast, static_cast<double>(entry.last_used));
    (entry.correction() ? corrections : words).Append(std::move(d));
  }
  root.Set(kWords, std::move(words));
  root.Set(kCorrections, std::move(corrections));
  base::ListValue hints;
  for (const SiteHint& hint : data.hints.hints()) {
    base::DictValue d;
    d.Set(kHost, hint.host);
    d.Set(kSearch, hint.search_field);
    d.Set(kList, hint.result_list);
    d.Set(kPlayer, hint.player);
    d.Set(kLast, static_cast<double>(hint.last_used));
    hints.Append(std::move(d));
  }
  root.Set(kHints, std::move(hints));
  base::ListValue declined;
  for (const std::string& id : data.declined_routines) {
    declined.Append(id);
  }
  root.Set(kDeclinedRoutines, std::move(declined));
  std::string out;
  if (!base::JSONWriter::Write(root, &out)) {
    return std::string();
  }
  return out;
}

LearnedData ParseLearning(std::string_view json, int64_t now_unix) {
  LearnedData data;
  if (json.empty() || json.size() > kMaxLearningFileBytes) {
    return data;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC, kMaxDepth);
  if (!parsed || !parsed->is_dict()) {
    return data;
  }
  const base::DictValue& root = parsed->GetDict();
  if (root.FindInt(kVersion) != kLearningFormatVersion) {
    return data;
  }
  data.learn = root.FindBool(kLearn).value_or(true);
  auto each = [&root](const char* key, auto&& take) {
    const base::ListValue* list = root.FindList(key);
    if (!list) {
      return;
    }
    for (const base::Value& item : *list) {
      if (const base::DictValue* d = item.GetIfDict()) {
        take(*d);
      }
    }
  };
  auto entry_of = [](const base::DictValue& d,
                     const char* phrase_key) -> std::optional<VocabularyEntry> {
    const std::string* phrase = StringOf(d, phrase_key);
    const std::optional<int> uses = d.FindInt(kUses);
    const std::optional<int64_t> last = TimeOf(d);
    if (!phrase || !uses || !last) {
      return std::nullopt;
    }
    VocabularyEntry entry;
    entry.phrase = *phrase;
    entry.uses = *uses;
    entry.last_used = *last;
    return entry;
  };
  each(kCorrections, [&](const base::DictValue& d) {
    std::optional<VocabularyEntry> entry = entry_of(d, kMeant);
    const std::string* heard = StringOf(d, kHeard);
    if (entry && heard && !heard->empty()) {
      entry->heard = *heard;
      data.vocabulary.Restore(std::move(*entry));
    }
  });
  each(kWords, [&](const base::DictValue& d) {
    if (std::optional<VocabularyEntry> entry = entry_of(d, kPhrase)) {
      data.vocabulary.Restore(std::move(*entry));
    }
  });
  each(kHints, [&](const base::DictValue& d) {
    const std::optional<int64_t> last = TimeOf(d);
    if (!last) {
      return;
    }
    SiteHint hint;
    hint.host = StringOr(d, kHost);
    hint.search_field = StringOr(d, kSearch);
    hint.result_list = StringOr(d, kList);
    hint.player = StringOr(d, kPlayer);
    hint.last_used = *last;
    data.hints.Restore(std::move(hint));
  });
  if (const base::ListValue* declined = root.FindList(kDeclinedRoutines)) {
    std::vector<std::string> ids;
    for (const base::Value& item : *declined) {
      if (item.is_string()) {
        ids.push_back(item.GetString());
      }
    }
    AddDeclinedRoutines(data.declined_routines, ids);
  }
  data.vocabulary.Age(now_unix);
  data.hints.Age(now_unix);
  return data;
}

bool AddDeclinedRoutines(std::vector<std::string>& declined,
                         const std::vector<std::string>& ids) {
  bool changed = false;
  for (const std::string& id : ids) {
    if (!IsRoutineId(id) ||
        std::find(declined.begin(), declined.end(), id) != declined.end()) {
      continue;
    }
    declined.push_back(id);
    changed = true;
  }
  if (declined.size() > kMaxDeclinedRoutines) {
    declined.erase(
        declined.begin(),
        declined.end() - static_cast<std::ptrdiff_t>(kMaxDeclinedRoutines));
  }
  return changed;
}

}  // namespace voice
}  // namespace molt_ai
