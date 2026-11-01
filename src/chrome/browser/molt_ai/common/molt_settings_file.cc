// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/common/molt_settings_file.h"

#include <optional>
#include <string>
#include <utility>

#include "base/base_paths.h"
#include "base/check.h"
#include "base/files/file_util.h"
#include "base/files/important_file_writer.h"
#include "base/functional/bind.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/no_destructor.h"
#include "base/path_service.h"
#include "base/task/lazy_thread_pool_task_runner.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "chrome/browser/molt_ai/common/molt_regular_file.h"

namespace molt_ai {

namespace {

// One sequence for every change (made lazily, and per test environment in
// unit tests). A change posted before shutdown is written.
base::LazyThreadPoolSequencedTaskRunner g_settings_task_runner =
    LAZY_THREAD_POOL_SEQUENCED_TASK_RUNNER_INITIALIZER(
        base::TaskTraits(base::MayBlock(),
                         base::TaskPriority::USER_VISIBLE,
                         base::TaskShutdownBehavior::BLOCK_SHUTDOWN));

// Set before any task reads it (tests).
std::optional<base::FilePath>& PathForTesting() {
  static base::NoDestructor<std::optional<base::FilePath>> path;
  return *path;
}

}  // namespace

std::optional<base::FilePath> MoltSettingsFilePath() {
  if (PathForTesting()) {
    return PathForTesting();
  }
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) {
    return std::nullopt;
  }
  return home.Append(FILE_PATH_LITERAL(".moltbrowser"))
      .Append(FILE_PATH_LITERAL("settings.json"));
}

void SetMoltSettingsFilePathForTesting(std::optional<base::FilePath> path) {
  PathForTesting() = std::move(path);
}

scoped_refptr<base::SequencedTaskRunner> MoltSettingsFileTaskRunner() {
  return g_settings_task_runner.Get();
}

MoltSettingsRead ReadMoltSettingsFileAt(const base::FilePath& path,
                                        base::DictValue* out) {
  *out = base::DictValue();
  // Only a regular file is opened (a FIFO would hold this sequence).
  const MoltFileRead read = ReadMoltFile(path, kMaxMoltSettingsFileBytes,
                                         MoltFileLinks::kFollowedWithinHome);
  switch (read.kind) {
    case MoltFileKind::kMissing:
      return MoltSettingsRead::kMissing;
    case MoltFileKind::kNotRegular:
      return MoltSettingsRead::kNotAFile;
    case MoltFileKind::kTooBig:
    case MoltFileKind::kUnreadable:
      return MoltSettingsRead::kNotAnObject;
    case MoltFileKind::kRegular:
      break;
  }
  std::optional<base::Value> parsed =
      base::JSONReader::Read(read.contents, base::JSON_ALLOW_TRAILING_COMMAS);
  if (!parsed || !parsed->is_dict()) {
    return MoltSettingsRead::kNotAnObject;
  }
  *out = std::move(parsed->GetDict());
  return MoltSettingsRead::kObject;
}

std::optional<base::DictValue> ReadMoltSettings() {
  const std::optional<base::FilePath> path = MoltSettingsFilePath();
  if (!path) {
    return std::nullopt;
  }
  base::DictValue settings;
  switch (ReadMoltSettingsFileAt(*path, &settings)) {
    case MoltSettingsRead::kMissing:
    case MoltSettingsRead::kObject:
      return settings;
    case MoltSettingsRead::kNotAnObject:
    case MoltSettingsRead::kNotAFile:
      return std::nullopt;
  }
  return std::nullopt;
}

void MergeMoltSettings(base::DictValue& settings,
                       const base::DictValue& changes) {
  for (const auto [key, value] : changes) {
    if (value.is_none()) {
      settings.Remove(key);
    } else {
      settings.Set(key, value.Clone());
    }
  }
}

void MergeMoltSettingsForm(base::DictValue& settings,
                           const base::DictValue& form,
                           const base::DictValue& fields) {
  for (const auto [key, value] : form) {
    const base::Value* field = fields.Find(key);
    if (!field) {
      continue;
    }
    const bool numbers = (field->is_int() || field->is_double()) &&
                         (value.is_int() || value.is_double());
    const bool same_scalar =
        field->type() == value.type() && (value.is_bool() || value.is_string());
    if (numbers || same_scalar) {
      settings.Set(key, value.Clone());
    }
  }
}

bool UpdateMoltSettingsFileAt(const base::FilePath& path,
                              MoltSettingsUpdate update) {
  base::DictValue settings;
  const MoltSettingsRead read = ReadMoltSettingsFileAt(path, &settings);
  if (read != MoltSettingsRead::kMissing && read != MoltSettingsRead::kObject) {
    // Left for the user to fix.
    return false;
  }
  if (!update(settings)) {
    return false;
  }
  std::string json;
  if (!base::JSONWriter::WriteWithOptions(
          settings, base::JSONWriter::OPTIONS_PRETTY_PRINT, &json)) {
    return false;
  }
  // Never a file the readers would refuse (and every later change with
  // them): the change is refused instead, the file left as it was.
  if (json.size() > kMaxMoltSettingsFileBytes) {
    return false;
  }
  const base::FilePath dir = path.DirName();
  if (!base::DirectoryExists(dir) && !base::CreateDirectory(dir)) {
    return false;
  }
  // The file itself, or the regular file a link to it leads to (the link
  // stays); nothing written when something else is there now.
  const std::optional<base::FilePath> target =
      MoltFileWritePath(path, MoltFileLinks::kFollowedWithinHome);
  if (!target) {
    return false;
  }
  // A temporary file next to it, then a rename: never half a file.
  return base::ImportantFileWriter::WriteFileAtomically(*target, json);
}

bool UpdateMoltSettings(MoltSettingsUpdate update) {
  // Every change on the one sequence: none drops another's keys.
  DCHECK(MoltSettingsFileTaskRunner()->RunsTasksInCurrentSequence());
  const std::optional<base::FilePath> path = MoltSettingsFilePath();
  return path && UpdateMoltSettingsFileAt(*path, update);
}

bool SetMoltSettings(const base::DictValue& changes) {
  return UpdateMoltSettings([&changes](base::DictValue& settings) {
    MergeMoltSettings(settings, changes);
    return true;
  });
}

void PostSetMoltSettings(base::DictValue changes,
                         base::OnceCallback<void(bool)> done) {
  if (!done) {
    // No reply: a thread pool task without a sequence may post this too.
    MoltSettingsFileTaskRunner()->PostTask(
        FROM_HERE,
        base::BindOnce(
            [](const base::DictValue& changes) { SetMoltSettings(changes); },
            std::move(changes)));
    return;
  }
  MoltSettingsFileTaskRunner()->PostTaskAndReplyWithResult(
      FROM_HERE,
      base::BindOnce(
          [](const base::DictValue& changes) {
            return SetMoltSettings(changes);
          },
          std::move(changes)),
      std::move(done));
}

}  // namespace molt_ai
