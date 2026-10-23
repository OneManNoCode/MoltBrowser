// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// What crosses into and out of the page and the model during a page
// translation: the calls to kPageTranslateScript and the checks on what
// they return, and the reading of the model's batch replies. Everything
// read here is untrusted: the script's results come from the page's
// renderer (a compromised one can send anything), the replies from the
// model (which the page's text can steer). Pure logic over base::Value, no
// content/, so molt_ai_unittests tests it and molt_page_translate_fuzzer
// fuzzes it. Design: docs/design/PAGE_TRANSLATION.md §4.1-§4.3, §6.

#ifndef CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_IO_H_
#define CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_IO_H_

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "chrome/browser/molt_ai/translate/page_translate_plan.h"

namespace base {
class Value;
}  // namespace base

namespace molt_ai::page_translate {

// A job's id in the isolated world: 1-32 of [0-9a-f], so the script's state
// for one job is never taken for another's.
bool IsValidRequestId(std::string_view id);

// The scripts, as JavaScript source. |request_id| must pass
// IsValidRequestId(); every value goes in as JSON (base::JSONWriter), so
// page and model text can't become code.
std::string BuildExtractScript(std::string_view request_id);
std::string BuildApplyScript(std::string_view request_id,
                             const std::vector<SlotWrite>& writes);
std::string BuildRevertScript();

// The extract script's result for |request_id|, checked: a dict
// {v: 1, id, truncated, slots}, at most kMaxSlots slots, each exactly
// [kind 0..kMaxSlotKind, bool, string]. Each text is made valid UTF-8
// without NUL and must then fit kMaxSlotBytes, and all of them
// kMaxPageBytes. Anything else, or {error: true}: nullopt (nothing is
// translated).
std::optional<PageText> ParseExtraction(const base::Value& result,
                                        std::string_view request_id);

// The apply script's count of slots written for |request_id| (0 to
// |max|), or nullopt: the page lost the job's state (-1), or the result is
// malformed.
std::optional<size_t> ParseApplyResult(const base::Value& result,
                                       std::string_view request_id,
                                       size_t max);

// The translations in a batch reply decoded under
// web_ai::TranslatorBatchResponseSchema(count): strict JSON (RFC 8259,
// base::JSONReader), an object whose "translations" is a list of exactly
// |count| strings, each made valid UTF-8 without NUL. Other members, a
// different count or anything that isn't a string: nullopt, and the batch
// is tried again in halves (TranslationPlan::RejectBatch()).
std::optional<std::vector<std::string>> ReadBatchReply(std::string_view reply,
                                                       size_t count);

}  // namespace molt_ai::page_translate

#endif  // CHROME_BROWSER_MOLT_AI_TRANSLATE_PAGE_TRANSLATE_IO_H_
