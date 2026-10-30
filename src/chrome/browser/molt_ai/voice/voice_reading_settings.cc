// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_reading_settings.h"

#include <cmath>
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
#include "chrome/browser/molt_ai/voice/voice_controller_logic.h"

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

// The settings as a dictionary: nullopt when the file exists but isn't a
// JSON object (it is left for the user to fix); empty when it is missing.
std::optional<base::DictValue> ReadSettings(const base::FilePath& path) {
  if (!base::PathExists(path)) {
    return base::DictValue();
  }
  std::string contents;
  if (!base::ReadFileToStringWithMaxSize(path, &contents, kMaxSettingsBytes)) {
    return std::nullopt;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(contents, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return std::nullopt;
  }
  return std::move(parsed->GetDict());
}

}  // namespace

double ReadVoiceSpeechRate() {
  const std::optional<base::FilePath> path = SettingsPath();
  if (!path) {
    return 1.0;
  }
  const std::optional<base::DictValue> values = ReadSettings(*path);
  if (!values) {
    return 1.0;
  }
  const std::optional<double> rate =
      values->FindDouble(kVoiceSpeechRateSettingsKey);
  if (!rate || !std::isfinite(*rate) || *rate < kMinSpeechRate ||
      *rate > kMaxSpeechRate) {
    return 1.0;
  }
  return *rate;
}

bool WriteVoiceSpeechRate(double rate) {
  const std::optional<base::FilePath> path = SettingsPath();
  if (!path || !std::isfinite(rate) || rate < kMinSpeechRate ||
      rate > kMaxSpeechRate) {
    return false;
  }
  std::optional<base::DictValue> values = ReadSettings(*path);
  if (!values) {
    return false;
  }
  values->Set(kVoiceSpeechRateSettingsKey, rate);
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          *values, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
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
