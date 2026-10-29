// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_session_settings.h"

#include <optional>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/path_service.h"
#include "base/values.h"

namespace molt_ai {
namespace voice {

namespace {

// settings.json is at most a few KiB; anything bigger isn't ours.
constexpr size_t kMaxSettingsBytes = 1 << 20;

std::optional<base::FilePath> SettingsPath() {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return std::nullopt;
  }
  return home.Append(base::FilePath::FromUTF8Unsafe(".moltbrowser"))
      .Append(base::FilePath::FromUTF8Unsafe("settings.json"));
}

enum class ReadResult {
  kMissing,
  kObject,
  kNotAnObject,
};

ReadResult ReadSettings(const base::FilePath& path, base::DictValue* out) {
  if (!base::PathExists(path)) {
    return ReadResult::kMissing;
  }
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(path, &contents, kMaxSettingsBytes)) {
    return ReadResult::kNotAnObject;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(contents, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return ReadResult::kNotAnObject;
  }
  *out = std::move(parsed->GetDict());
  return ReadResult::kObject;
}

}  // namespace

VoiceModeSettings ReadVoiceModeSettings() {
  VoiceModeSettings settings;
  const std::optional<base::FilePath> path = SettingsPath();
  base::DictValue values;
  if (!path || ReadSettings(*path, &values) != ReadResult::kObject) {
    return settings;
  }
  settings.start_with_browser =
      values.FindBool(kVoiceModeSettingsKey).value_or(false);
  settings.always_listening =
      values.FindBool(kVoiceAlwaysListeningSettingsKey).value_or(false);
  return settings;
}

bool WriteVoiceModeSettings(const VoiceModeSettings& settings) {
  const std::optional<base::FilePath> path = SettingsPath();
  if (!path) {
    return false;
  }
  base::DictValue values;
  if (ReadSettings(*path, &values) == ReadResult::kNotAnObject) {
    return false;
  }
  values.Set(kVoiceModeSettingsKey, settings.start_with_browser);
  values.Set(kVoiceAlwaysListeningSettingsKey, settings.always_listening);
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          values, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
    return false;
  }
  const base::FilePath dir = path->DirName();
  if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
    return false;
  }
  return base::WriteFile(*path, json);
}

}  // namespace voice
}  // namespace molt_ai
