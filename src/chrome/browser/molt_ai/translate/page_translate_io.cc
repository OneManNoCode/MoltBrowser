// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_io.h"

#include <utility>

#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/translate/page_translate_script.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai::page_translate {

namespace {

constexpr size_t kMaxRequestIdLength = 32;

// kPageTranslateScript called with |arguments|.
std::string Call(const base::DictValue& arguments) {
  std::string json;
  if (!base::JSONWriter::Write(arguments, &json)) {
    // Only strings, ints and lists go in, so this can't happen; a script
    // that does nothing is the safe answer if it ever did.
    json = "null";
  }
  return std::string(kPageTranslateScript) + "(" + json + ")";
}

base::DictValue Arguments(std::string_view op, std::string_view request_id) {
  base::DictValue arguments;
  arguments.Set("op", op);
  arguments.Set("id", request_id);
  return arguments;
}

// The result is a dict {v: 1, ...} for |request_id| (when given).
const base::DictValue* ResultFor(const base::Value& result,
                                 std::optional<std::string_view> request_id) {
  if (!result.is_dict()) {
    return nullptr;
  }
  const base::DictValue& dict = result.GetDict();
  if (dict.FindInt("v") != 1 || dict.FindBool("error").value_or(false)) {
    return nullptr;
  }
  if (request_id) {
    const std::string* id = dict.FindString("id");
    if (!id || *id != *request_id) {
      return nullptr;
    }
  }
  return &dict;
}

}  // namespace

bool IsValidRequestId(std::string_view id) {
  if (id.empty() || id.size() > kMaxRequestIdLength) {
    return false;
  }
  for (char c : id) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  return true;
}

std::string BuildExtractScript(std::string_view request_id) {
  base::DictValue arguments = Arguments("extract", request_id);
  base::DictValue caps;
  caps.Set("slots", static_cast<int>(kMaxSlots));
  caps.Set("slotChars", static_cast<int>(kMaxSlotChars));
  caps.Set("pageChars", static_cast<int>(kMaxPageChars));
  caps.Set("visited", static_cast<int>(kMaxVisitedNodes));
  arguments.Set("caps", std::move(caps));
  return Call(arguments);
}

std::string BuildApplyScript(std::string_view request_id,
                             const std::vector<SlotWrite>& writes) {
  base::DictValue arguments = Arguments("apply", request_id);
  base::ListValue list;
  for (const SlotWrite& write : writes) {
    if (write.slot >= kMaxSlots) {
      continue;
    }
    base::ListValue entry;
    entry.Append(static_cast<int>(write.slot));
    entry.Append(write.text);
    list.Append(std::move(entry));
  }
  arguments.Set("writes", std::move(list));
  return Call(arguments);
}

std::string BuildRevertScript() {
  base::DictValue arguments;
  arguments.Set("op", "revert");
  return Call(arguments);
}

std::optional<PageText> ParseExtraction(const base::Value& result,
                                        std::string_view request_id) {
  const base::DictValue* dict = ResultFor(result, request_id);
  if (!dict) {
    return std::nullopt;
  }
  const base::ListValue* slots = dict->FindList("slots");
  if (!slots || slots->size() > kMaxSlots) {
    return std::nullopt;
  }
  PageText page;
  page.truncated = dict->FindBool("truncated").value_or(false);
  page.slots.reserve(slots->size());
  size_t total = 0;
  for (size_t i = 0; i < slots->size(); ++i) {
    const base::Value& entry = (*slots)[i];
    if (!entry.is_list()) {
      return std::nullopt;
    }
    const base::ListValue& fields = entry.GetList();
    if (fields.size() != 3 || !fields[0].is_int() || !fields[1].is_bool() ||
        !fields[2].is_string()) {
      return std::nullopt;
    }
    const int kind = fields[0].GetInt();
    if (kind < 0 || kind > kMaxSlotKind) {
      return std::nullopt;
    }
    // Sized before it is copied, then again as it will be used.
    const std::string& raw = fields[2].GetString();
    if (raw.size() > kMaxSlotBytes) {
      return std::nullopt;
    }
    std::string text = molt_ai::ToDisplayText(raw);
    if (text.size() > kMaxSlotBytes) {
      return std::nullopt;
    }
    total += text.size();
    if (total > kMaxPageBytes) {
      return std::nullopt;
    }
    PageSlot slot;
    slot.kind = static_cast<SlotKind>(kind);
    slot.in_viewport = fields[1].GetBool();
    slot.text = std::move(text);
    page.slots.push_back(std::move(slot));
  }
  return page;
}

std::optional<size_t> ParseApplyResult(const base::Value& result,
                                       std::string_view request_id,
                                       size_t max) {
  const base::DictValue* dict = ResultFor(result, request_id);
  if (!dict) {
    return std::nullopt;
  }
  const std::optional<int> applied = dict->FindInt("applied");
  if (!applied || *applied < 0 || static_cast<size_t>(*applied) > max) {
    return std::nullopt;
  }
  return static_cast<size_t>(*applied);
}

std::optional<std::vector<std::string>> ReadBatchReply(std::string_view reply,
                                                       size_t count) {
  std::optional<base::Value> parsed =
      base::JSONReader::Read(reply, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict() || parsed->GetDict().size() != 1) {
    return std::nullopt;
  }
  const base::ListValue* list =
      parsed->GetDict().FindList(web_ai::kTranslatorBatchResponseKey);
  if (!list || list->size() != count) {
    return std::nullopt;
  }
  std::vector<std::string> translations;
  translations.reserve(count);
  for (size_t i = 0; i < list->size(); ++i) {
    const base::Value& item = (*list)[i];
    if (!item.is_string()) {
      return std::nullopt;
    }
    translations.push_back(molt_ai::ToDisplayText(item.GetString()));
  }
  return translations;
}

}  // namespace molt_ai::page_translate
