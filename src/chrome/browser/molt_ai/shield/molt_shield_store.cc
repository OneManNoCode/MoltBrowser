// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/shield/molt_shield_store.h"

#include <algorithm>
#include <utility>

#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/json/values_util.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/threading/scoped_blocking_call.h"
#include "crypto/sha2.h"

namespace molt_ai::shield {

namespace {

constexpr char kStateFile[] = "state.json";
constexpr char kEngineFile[] = "engine.dat";
constexpr char kManifestFile[] = "engine.json";
constexpr char kListsDir[] = "lists";
constexpr int64_t kMaxJsonBytes = 1024 * 1024;

std::string StringOr(const base::DictValue& d, std::string_view key) {
  const std::string* s = d.FindString(key);
  return s ? *s : std::string();
}

base::Time TimeOr(const base::DictValue& d, std::string_view key) {
  return base::ValueToTime(d.Find(key)).value_or(base::Time());
}

std::optional<base::DictValue> ReadJsonDict(const base::FilePath& path) {
  std::optional<std::string> text = ReadFileCapped(path, kMaxJsonBytes);
  if (!text) {
    return std::nullopt;
  }
  return base::JSONReader::ReadDict(*text, base::JSON_PARSE_RFC);
}

bool WriteJsonDict(const base::FilePath& path, const base::DictValue& dict) {
  std::optional<std::string> json = base::WriteJson(dict);
  return json && WriteFileAtomic(path, *json);
}

}  // namespace

ListState::ListState() = default;
ListState::ListState(const ListState&) = default;
ListState& ListState::operator=(const ListState&) = default;
ListState::~ListState() = default;

ShieldSettings::ShieldSettings() = default;
ShieldSettings::ShieldSettings(const ShieldSettings&) = default;
ShieldSettings& ShieldSettings::operator=(const ShieldSettings&) = default;
ShieldSettings::~ShieldSettings() = default;

StoreState::StoreState() = default;
StoreState::StoreState(const StoreState&) = default;
StoreState& StoreState::operator=(const StoreState&) = default;
StoreState::~StoreState() = default;

EngineManifest::EngineManifest() = default;
EngineManifest::EngineManifest(const EngineManifest&) = default;
EngineManifest& EngineManifest::operator=(const EngineManifest&) = default;
EngineManifest::~EngineManifest() = default;

ListSpec::ListSpec() = default;
ListSpec::ListSpec(const ListSpec&) = default;
ListSpec& ListSpec::operator=(const ListSpec&) = default;
ListSpec::~ListSpec() = default;

base::DictValue SerializeState(const StoreState& state) {
  base::DictValue settings;
  settings.Set("enabled", state.settings.enabled);
  settings.Set("cosmetic", state.settings.cosmetic);
  settings.Set("consent_killer", state.settings.consent_killer);
  base::DictValue list_enabled;
  for (const auto& [id, enabled] : state.settings.list_enabled) {
    list_enabled.Set(id, enabled);
  }
  settings.Set("list_enabled", std::move(list_enabled));
  settings.Set("last_update_check",
               base::TimeToValue(state.settings.last_update_check));

  base::DictValue lists;
  for (const auto& [id, list] : state.lists) {
    base::DictValue l;
    l.Set("etag", list.etag);
    l.Set("last_modified", list.last_modified);
    l.Set("sha256", list.sha256_hex);
    l.Set("size", base::Int64ToValue(list.size));
    l.Set("title", list.title);
    l.Set("version", list.version);
    l.Set("rule_lines", static_cast<int>(list.rule_lines));
    l.Set("fetched_at", base::TimeToValue(list.fetched_at));
    l.Set("checked_at", base::TimeToValue(list.checked_at));
    l.Set("last_error", list.last_error);
    lists.Set(id, std::move(l));
  }

  base::DictValue root;
  root.Set("version", 1);
  root.Set("settings", std::move(settings));
  root.Set("lists", std::move(lists));
  return root;
}

StoreState ParseState(const base::DictValue& root) {
  StoreState state;
  if (const base::DictValue* s = root.FindDict("settings")) {
    state.settings.enabled = s->FindBool("enabled").value_or(true);
    state.settings.cosmetic = s->FindBool("cosmetic").value_or(true);
    state.settings.consent_killer =
        s->FindBool("consent_killer").value_or(true);
    if (const base::DictValue* le = s->FindDict("list_enabled")) {
      for (const auto [id, value] : *le) {
        if (value.is_bool()) {
          state.settings.list_enabled[id] = value.GetBool();
        }
      }
    }
    state.settings.last_update_check = TimeOr(*s, "last_update_check");
  }
  if (const base::DictValue* lists = root.FindDict("lists")) {
    for (const auto [id, value] : *lists) {
      const base::DictValue* l = value.GetIfDict();
      if (!l) {
        continue;
      }
      ListState list;
      list.etag = StringOr(*l, "etag");
      list.last_modified = StringOr(*l, "last_modified");
      list.sha256_hex = StringOr(*l, "sha256");
      list.size = base::ValueToInt64(l->Find("size")).value_or(0);
      list.title = StringOr(*l, "title");
      list.version = StringOr(*l, "version");
      list.rule_lines =
          static_cast<uint32_t>(std::max(0, l->FindInt("rule_lines").value_or(0)));
      list.fetched_at = TimeOr(*l, "fetched_at");
      list.checked_at = TimeOr(*l, "checked_at");
      list.last_error = StringOr(*l, "last_error");
      state.lists[id] = std::move(list);
    }
  }
  return state;
}

base::DictValue SerializeManifest(const EngineManifest& manifest) {
  base::DictValue hashes;
  for (const auto& [id, sha] : manifest.list_hashes) {
    hashes.Set(id, sha);
  }
  base::DictValue root;
  root.Set("format", manifest.format_key);
  root.Set("lists", std::move(hashes));
  root.Set("engine_sha256", manifest.engine_sha256);
  return root;
}

std::optional<EngineManifest> ParseManifest(const base::DictValue& root) {
  const std::string* format = root.FindString("format");
  const base::DictValue* lists = root.FindDict("lists");
  const std::string* engine_sha = root.FindString("engine_sha256");
  if (!format || !lists || !engine_sha) {
    return std::nullopt;
  }
  EngineManifest manifest;
  manifest.format_key = *format;
  manifest.engine_sha256 = *engine_sha;
  for (const auto [id, value] : *lists) {
    if (!value.is_string()) {
      return std::nullopt;
    }
    manifest.list_hashes[id] = value.GetString();
  }
  return manifest;
}

base::FilePath ListPath(const base::FilePath& dir, std::string_view id) {
  return dir.AppendASCII(kListsDir).AppendASCII(base::StrCat({id, ".txt"}));
}

base::FilePath StatePath(const base::FilePath& dir) {
  return dir.AppendASCII(kStateFile);
}

base::FilePath EnginePath(const base::FilePath& dir) {
  return dir.AppendASCII(kEngineFile);
}

base::FilePath ManifestPath(const base::FilePath& dir) {
  return dir.AppendASCII(kManifestFile);
}

StoreState LoadState(const base::FilePath& dir) {
  std::optional<base::DictValue> dict = ReadJsonDict(StatePath(dir));
  return dict ? ParseState(*dict) : StoreState();
}

bool SaveState(const base::FilePath& dir, const StoreState& state) {
  return WriteJsonDict(StatePath(dir), SerializeState(state));
}

std::optional<EngineManifest> LoadManifest(const base::FilePath& dir) {
  std::optional<base::DictValue> dict = ReadJsonDict(ManifestPath(dir));
  return dict ? ParseManifest(*dict) : std::nullopt;
}

bool SaveManifest(const base::FilePath& dir, const EngineManifest& manifest) {
  return WriteJsonDict(ManifestPath(dir), SerializeManifest(manifest));
}

std::optional<std::string> ReadFileCapped(const base::FilePath& path,
                                          int64_t max_bytes) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(path, &contents,
                                         static_cast<size_t>(max_bytes))) {
    return std::nullopt;
  }
  return contents;
}

bool WriteFileAtomic(const base::FilePath& path, std::string_view data) {
  base::ScopedBlockingCall scoped_blocking_call(FROM_HERE,
                                                base::BlockingType::MAY_BLOCK);
  if (!base::CreateDirectory(path.DirName())) {
    return false;
  }
  return base::ImportantFileWriter::WriteFileAtomically(path, data);
}

std::string Sha256Hex(std::string_view data) {
  return base::HexEncodeLower(crypto::SHA256HashString(data));
}

}  // namespace molt_ai::shield
