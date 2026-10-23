// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Page translation (molt_ai/translate/) on everything outside the browser:
// the page's answer to the extract script (any JavaScript value from its
// renderer, here any JSON), the model's replies to each batch (anything),
// and the answer to the write script.
//
// Checks:
//  * ParseExtraction() accepts at most kMaxSlots slots of known kinds,
//    each valid UTF-8 without NUL within kMaxSlotBytes, all within
//    kMaxPageBytes, and answers the same twice; a result for another job's
//    id is refused; ParseApplyResult() never counts more than was asked;
//  * the plan over the page (from the result, or straight from the input)
//    ends: every batch is at most kTranslatorMaxBatchTexts texts within
//    the piece budget (unless alone), there are at most 3 generations per
//    piece, and every piece is settled at the end;
//  * a batch's instruction is the same whatever the texts, and its user
//    turn reads back (JSON) to exactly the texts;
//  * ReadBatchReply() returns exactly as many strings as asked, each valid
//    UTF-8 without NUL; a well-formed reply is always read;
//  * each slot is written at most once, never with its own text, as valid
//    UTF-8 without NUL, keeping the whitespace around it, within the growth
//    cap; the write script carries the writes, as JSON, exactly.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/json/json_writer.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/translate/page_translate_io.h"
#include "chrome/browser/molt_ai/translate/page_translate_plan.h"
#include "chrome/browser/molt_ai/translate/page_translate_script.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_translation.h"
#include "chrome/services/molt_ai/inference/engine/stream_filters.h"

namespace molt_ai::page_translate {
namespace {

constexpr char kId[] = "c0ffee";

bool IsAsciiWs(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
         c == '\v';
}

std::string_view LeadingWs(std::string_view s) {
  size_t n = 0;
  while (n < s.size() && IsAsciiWs(s[n])) ++n;
  return s.substr(0, n);
}

std::string_view TrailingWs(std::string_view s) {
  size_t n = 0;
  while (n < s.size() && IsAsciiWs(s[s.size() - 1 - n])) ++n;
  return s.substr(s.size() - n);
}

void CheckText(const std::string& text) {
  CHECK(IsValidUtf8(text));
  CHECK(text.find('\0') == std::string::npos);
}

void CheckPage(const PageText& page) {
  CHECK(page.slots.size() <= kMaxSlots);
  size_t total = 0;
  for (const PageSlot& slot : page.slots) {
    CHECK(static_cast<int>(slot.kind) >= 0 &&
          static_cast<int>(slot.kind) <= kMaxSlotKind);
    CHECK(slot.text.size() <= kMaxSlotBytes);
    CheckText(slot.text);
    total += slot.text.size();
  }
  CHECK(total <= kMaxPageBytes);
}

// The JSON a built script passes to kPageTranslateScript.
base::Value ArgumentsOf(const std::string& script) {
  const std::string prefix = std::string(kPageTranslateScript) + "(";
  CHECK(script.size() > prefix.size() + 1);
  CHECK(script.compare(0, prefix.size(), prefix) == 0);
  CHECK(script.back() == ')');
  std::optional<base::Value> value = base::JSONReader::Read(
      std::string_view(script).substr(prefix.size(),
                                      script.size() - prefix.size() - 1),
      base::JSON_PARSE_RFC);
  CHECK(value && value->is_dict()) << "the arguments are not JSON";
  return std::move(*value);
}

void CheckWriteScript(const std::vector<SlotWrite>& writes) {
  const base::Value args = ArgumentsOf(BuildApplyScript(kId, writes));
  const base::ListValue* list = args.GetDict().FindList("writes");
  CHECK(list);
  CHECK(list->size() == writes.size());
  for (size_t i = 0; i < writes.size(); ++i) {
    const base::ListValue& entry = (*list)[i].GetList();
    CHECK(entry.size() == 2);
    CHECK(static_cast<size_t>(entry[0].GetInt()) == writes[i].slot);
    CHECK(entry[1].GetString() == writes[i].text);
  }
}

size_t TranslatedPieces(const std::string& text) {
  size_t n = 0;
  for (const web_ai::TranslationPiece& piece :
       web_ai::SplitForTranslation(text, web_ai::kTranslatorPieceTokens)) {
    n += piece.translate ? 1 : 0;
  }
  return n;
}

}  // namespace
}  // namespace molt_ai::page_translate

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::page_translate;
  namespace web_ai = molt_ai::web_ai;
  FuzzedDataProvider p(data, size);

  // The extract script's result, as the renderer could send it.
  std::optional<PageText> page;
  if (std::optional<base::Value> value = base::JSONReader::Read(
          p.ConsumeRandomLengthString(1 << 18), base::JSON_PARSE_RFC, 64)) {
    page = ParseExtraction(*value, kId);
    if (page) {
      CheckPage(*page);
      std::optional<PageText> again = ParseExtraction(*value, kId);
      CHECK(again && again->slots.size() == page->slots.size());
      CHECK(!ParseExtraction(*value, "badd"));
    }
    if (std::optional<size_t> applied = ParseApplyResult(*value, kId, 16)) {
      CHECK(*applied <= 16);
    }
  }
  // Otherwise a page straight from the input.
  if (!page) {
    page.emplace();
    const size_t slots = p.ConsumeIntegralInRange<size_t>(0, 48);
    for (size_t i = 0; i < slots; ++i) {
      PageSlot slot;
      slot.kind = static_cast<SlotKind>(
          p.ConsumeIntegralInRange<int>(0, kMaxSlotKind));
      slot.in_viewport = p.ConsumeBool();
      slot.text = molt_ai::ToDisplayText(p.ConsumeRandomLengthString(4096));
      if (slot.text.size() <= kMaxSlotBytes) {
        page->slots.push_back(std::move(slot));
      }
    }
  }

  const web_ai::TranslatorPair pair = *web_ai::ResolveTranslatorPair("de", "en");
  const std::string instruction =
      web_ai::BuildTranslatorBatchPrompt(pair, {"x"}).system_prompt;
  TranslationPlan plan(*page);
  std::vector<bool> written(page->slots.size(), false);
  size_t generations = 0;
  while (std::optional<Batch> batch = plan.NextBatch()) {
    ++generations;
    // Halving a batch makes at most 2k-1 generations of k texts, and a text
    // that fails in company goes once more alone.
    CHECK(generations <= 3 * plan.unit_count()) << "the plan doesn't end";
    const size_t n = batch->texts.size();
    CHECK(n >= 1 && n <= web_ai::kTranslatorMaxBatchTexts);
    CHECK(batch->units.size() == n);
    uint32_t tokens = 0;
    for (const std::string& text : batch->texts) {
      tokens = web_ai::AddTokens(tokens, web_ai::TranslatorBatchTextTokens(text));
    }
    CHECK(n == 1 || tokens <= web_ai::kTranslatorPieceTokens);

    // Page text only in the user turn, and exactly.
    const web_ai::TaskPrompt prompt =
        web_ai::BuildTranslatorBatchPrompt(pair, batch->texts);
    CHECK(prompt.system_prompt == instruction);
    std::optional<base::Value> sent =
        base::JSONReader::Read(prompt.user_message, base::JSON_PARSE_RFC);
    CHECK(sent && sent->is_list() && sent->GetList().size() == n);
    for (size_t i = 0; i < n; ++i) {
      CHECK(sent->GetList()[i].GetString() == batch->texts[i]);
    }

    std::vector<SlotWrite> writes;
    switch (p.ConsumeIntegralInRange<int>(0, 3)) {
      case 0: {
        // A well-formed reply, with text from the input.
        base::ListValue list;
        for (const std::string& text : batch->texts) {
          list.Append(p.ConsumeBool()
                          ? text + "!"
                          : molt_ai::ToDisplayText(p.ConsumeRandomLengthString(512)));
        }
        base::DictValue reply;
        reply.Set(web_ai::kTranslatorBatchResponseKey, std::move(list));
        std::string json;
        CHECK(base::JSONWriter::Write(reply, &json));
        std::optional<std::vector<std::string>> read = ReadBatchReply(json, n);
        CHECK(read && read->size() == n) << "a well-formed reply was refused";
        writes = plan.AcceptReply(*batch, *read);
        break;
      }
      case 1: {
        // Anything the model could write.
        std::optional<std::vector<std::string>> read =
            ReadBatchReply(p.ConsumeRandomLengthString(4096), n);
        if (read) {
          CHECK(read->size() == n);
          for (const std::string& t : *read) CheckText(t);
          writes = plan.AcceptReply(*batch, *read);
        } else {
          writes = plan.RejectBatch(*batch);
        }
        break;
      }
      case 2:
        writes = plan.RejectBatch(*batch);
        break;
      default:
        // The wrong number of translations counts as a rejection.
        writes = plan.AcceptReply(*batch, std::vector<std::string>(n + 1));
        break;
    }
    for (const SlotWrite& w : writes) {
      CHECK(w.slot < page->slots.size());
      CHECK(!written[w.slot]) << "a slot written twice";
      written[w.slot] = true;
      const std::string& original = page->slots[w.slot].text;
      CHECK(w.text != original);
      CheckText(w.text);
      CHECK(std::string_view(w.text).starts_with(LeadingWs(original)));
      CHECK(std::string_view(w.text).ends_with(TrailingWs(original)));
      CHECK(w.text.size() <= kMaxGrowthFactor * original.size() +
                                 kMaxGrowthSlack * TranslatedPieces(original));
    }
    if (!writes.empty()) {
      CheckWriteScript(writes);
    }
  }
  CHECK(plan.finished());
  CHECK(plan.settled_units() == plan.unit_count());
  CHECK(plan.failed_units() <= plan.unit_count());
  return 0;
}
