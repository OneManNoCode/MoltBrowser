// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/memory/conversation_store.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

#include "base/base_paths.h"
#include "base/files/file_enumerator.h"
#include "base/files/file_util.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/strings/string_util.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/molt_ai/keys/molt_os_crypt.h"

namespace molt_ai {
namespace memory {

namespace {

constexpr char kMagic[] = "MOLTCONV1\n";
constexpr size_t kMagicLen = sizeof(kMagic) - 1;
constexpr base::FilePath::CharType kExtension[] = FILE_PATH_LITERAL(".mconv");

std::optional<base::DictValue> ParseDict(const std::string& json) {
  auto parsed = base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict()) return std::nullopt;
  return std::move(parsed->GetDict());
}

base::DictValue Error(const std::string& message) {
  base::DictValue r;
  r.Set("success", false);
  r.Set("error", message);
  return r;
}

}  // namespace

ConversationStore::ConversationStore(const base::FilePath& profile_dir)
    : dir_(profile_dir.AppendASCII("MoltAI").AppendASCII("Conversations")) {}

ConversationStore::~ConversationStore() = default;

// static
bool ConversationStore::IsValidId(const std::string& id) {
  if (id.empty() || id.size() > 64) return false;
  for (char c : id) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-';
    if (!ok) return false;
  }
  return true;
}

// static
base::FilePath ConversationStore::LegacyDir() {
  base::FilePath home;
  if (!base::PathService::Get(base::DIR_HOME, &home)) return base::FilePath();
  return home.AppendASCII(".moltbrowser").AppendASCII("conversations");
}

base::FilePath ConversationStore::PathFor(const std::string& id) const {
  return dir_.AppendASCII(id).AddExtension(kExtension);
}

bool ConversationStore::ReadDecrypted(const base::FilePath& path,
                                      std::string* json) const {
  std::optional<int64_t> size = base::GetFileSize(path);
  // Ciphertext is a little larger than the plaintext cap; allow headroom.
  if (!size || *size > static_cast<int64_t>(kMaxConversationBytes * 2)) {
    return false;
  }
  std::string contents;
  if (!base::ReadFileToString(path, &contents)) return false;
  if (contents.compare(0, kMagicLen, kMagic) != 0) return false;
  return molt_os_crypt::DecryptString(contents.substr(kMagicLen), json);
}

bool ConversationStore::WriteEncrypted(const base::FilePath& path,
                                       const std::string& json) const {
  // Any key will do here, the hard-coded Linux one included (unlike the
  // secret stores), but never no key: without one, nothing is written.
  std::string ciphertext;
  if (json.empty() || !molt_os_crypt::EncryptString(json, &ciphertext) ||
      ciphertext.empty()) {
    LOG(ERROR) << "[MoltAI] conversation not saved: encryption unavailable";
    return false;
  }
  if (!base::CreateDirectory(dir_)) return false;
#if !BUILDFLAG(IS_WIN)
  base::SetPosixFilePermissions(dir_, 0700);
#endif
  // Write to a temp file then rename, so a crash mid-autosave can never
  // truncate the only copy of a conversation.
  const base::FilePath tmp = path.AddExtension(FILE_PATH_LITERAL("tmp"));
  if (!base::WriteFile(tmp, std::string(kMagic) + ciphertext) ||
      !base::ReplaceFile(tmp, path, nullptr)) {
    base::DeleteFile(tmp);
    return false;
  }
  return true;
}

void ConversationStore::MigrateLegacy(const base::FilePath& legacy_dir) {
  if (legacy_dir.value().empty() || !base::DirectoryExists(legacy_dir)) {
    return;
  }
  int migrated = 0;
  base::FileEnumerator e(legacy_dir, /*recursive=*/false,
                         base::FileEnumerator::FILES);
  for (auto path = e.Next(); !path.value().empty(); path = e.Next()) {
    const std::string stem = path.BaseName().RemoveExtension().AsUTF8Unsafe();
    if (path.Extension() != FILE_PATH_LITERAL(".json") || !IsValidId(stem)) {
      continue;  // .tmp leftovers and anything unexpected stay put
    }
    std::optional<int64_t> size = base::GetFileSize(path);
    std::string contents;
    if (!size || *size > static_cast<int64_t>(kMaxConversationBytes) ||
        !base::ReadFileToString(path, &contents)) {
      continue;
    }
    std::optional<base::DictValue> d = ParseDict(contents);
    if (!d) continue;  // corrupt: leave it for the user, don't lose it
    const base::FilePath dest = PathFor(stem);
    if (!base::PathExists(dest)) {
      d->Set("id", stem);
      std::string json;
      if (!base::JSONWriter::Write(*d, &json) || !WriteEncrypted(dest, json)) {
        continue;  // keep the plaintext until it can be protected
      }
    }
    if (base::DeleteFile(path)) ++migrated;
  }
  // Remove the shared directory once it is empty (DeleteFile only removes
  // an empty directory).
  base::DeleteFile(legacy_dir);
  if (migrated) {
    LOG(INFO) << "[MoltAI] migrated " << migrated
              << " legacy conversations into the profile (encrypted)";
  }
}

base::DictValue ConversationStore::List(size_t max_entries) const {
  base::DictValue result;
  result.Set("success", true);
  struct Entry {
    double updated_at = 0.0;
    base::DictValue dict;
  };
  std::vector<Entry> entries;
  if (base::DirectoryExists(dir_)) {
    base::FileEnumerator e(dir_, /*recursive=*/false,
                           base::FileEnumerator::FILES);
    for (auto path = e.Next(); !path.value().empty(); path = e.Next()) {
      if (path.Extension() != kExtension) continue;
      const std::string stem =
          path.BaseName().RemoveExtension().AsUTF8Unsafe();
      if (!IsValidId(stem)) continue;
      std::string json;
      if (!ReadDecrypted(path, &json)) continue;
      std::optional<base::DictValue> d = ParseDict(json);
      if (!d) continue;  // skip corrupt files rather than failing the list
      const std::string* title = d->FindString("title");
      Entry entry;
      entry.updated_at = d->FindDouble("updated_at").value_or(0.0);
      int message_count = 0;
      if (const base::ListValue* messages = d->FindList("messages")) {
        message_count = static_cast<int>(messages->size());
      }
      entry.dict.Set("id", stem);
      entry.dict.Set("title", title ? *title : std::string());
      entry.dict.Set("updated_at", entry.updated_at);
      entry.dict.Set("message_count", message_count);
      entries.push_back(std::move(entry));
    }
  }
  std::stable_sort(entries.begin(), entries.end(),
                   [](const Entry& a, const Entry& b) {
                     return a.updated_at > b.updated_at;
                   });
  if (entries.size() > max_entries) entries.resize(max_entries);
  base::ListValue conversations;
  for (auto& entry : entries) conversations.Append(std::move(entry.dict));
  result.Set("conversations", std::move(conversations));
  return result;
}

base::DictValue ConversationStore::Save(const std::string& id,
                                        const std::string& title,
                                        const std::string& payload_json) const {
  if (!IsValidId(id)) return Error("invalid conversation id");
  if (payload_json.size() > kMaxConversationBytes) {
    return Error("history_json exceeds 16 MiB cap");
  }
  // Parse first and re-serialize inside the wrapper so the stored JSON is
  // always valid, whatever the renderer sent. Accept the legacy bare array
  // and the v2 envelope that carries an attached document (the old code
  // rejected the envelope, so chats with an attachment never saved).
  auto parsed = base::JSONReader::Read(payload_json, base::JSON_PARSE_RFC);
  if (!parsed) return Error("history_json is not valid JSON");
  base::DictValue wrapper;
  if (parsed->is_list()) {
    wrapper.Set("messages", std::move(parsed->GetList()));
  } else if (parsed->is_dict() && parsed->GetDict().FindList("messages")) {
    base::DictValue& env = parsed->GetDict();
    wrapper.Set("messages", std::move(*env.FindList("messages")));
    if (base::DictValue* att = env.FindDict("attachment")) {
      wrapper.Set("attachment", std::move(*att));
    }
  } else {
    return Error("history_json must be a list or {messages: [...]}");
  }
  wrapper.Set("id", id);
  wrapper.Set("title", title);
  wrapper.Set("updated_at", base::Time::Now().InMillisecondsFSinceUnixEpoch());

  std::string serialized;
  if (!base::JSONWriter::Write(wrapper, &serialized)) {
    return Error("failed to serialize conversation");
  }
  if (!WriteEncrypted(PathFor(id), serialized)) {
    return Error("failed to write conversation file");
  }
  base::DictValue r;
  r.Set("success", true);
  return r;
}

base::DictValue ConversationStore::Load(const std::string& id) const {
  if (!IsValidId(id)) return Error("invalid conversation id");
  std::string json;
  if (!ReadDecrypted(PathFor(id), &json)) {
    return Error("conversation not found");
  }
  std::optional<base::DictValue> d = ParseDict(json);
  if (!d) return Error("conversation file is corrupt");
  base::DictValue result;
  result.Set("id", id);
  const std::string* title = d->FindString("title");
  result.Set("title", title ? *title : std::string());
  // Hand the page back the shape it saved: a bare array, or the v2
  // envelope when a document was attached.
  std::string history_json = "[]";
  base::ListValue* messages = d->FindList("messages");
  base::DictValue* attachment = d->FindDict("attachment");
  if (messages && attachment) {
    base::DictValue env;
    env.Set("v", 2);
    env.Set("messages", std::move(*messages));
    env.Set("attachment", std::move(*attachment));
    base::JSONWriter::Write(env, &history_json);
  } else if (messages) {
    base::JSONWriter::Write(*messages, &history_json);
  }
  result.Set("history_json", history_json);
  result.Set("success", true);
  return result;
}

base::DictValue ConversationStore::Delete(const std::string& id) const {
  if (!IsValidId(id)) return Error("invalid conversation id");
  if (!base::DeleteFile(PathFor(id))) {
    return Error("failed to delete conversation");
  }
  base::DictValue r;
  r.Set("success", true);
  return r;
}

}  // namespace memory
}  // namespace molt_ai
