// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/translate/page_translate_io.h"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/translate/page_translate_script.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace molt_ai::page_translate {
namespace {

constexpr char kId[] = "0123abcd";

base::Value Json(std::string_view json) {
  std::optional<base::Value> value = base::JSONReader::Read(json);
  EXPECT_TRUE(value.has_value()) << json;
  return value ? std::move(*value) : base::Value();
}

// The arguments a built script passes to kPageTranslateScript.
base::DictValue ArgumentsOf(const std::string& script) {
  const std::string prefix = std::string(kPageTranslateScript) + "(";
  EXPECT_EQ(script.substr(0, prefix.size()), prefix);
  EXPECT_EQ(script.back(), ')');
  std::optional<base::Value> value = base::JSONReader::Read(
      std::string_view(script).substr(prefix.size(),
                                       script.size() - prefix.size() - 1));
  EXPECT_TRUE(value && value->is_dict());
  return value && value->is_dict() ? std::move(value->GetDict())
                                   : base::DictValue();
}

std::string Extraction(std::string_view slots,
                       std::string_view extra = "\"truncated\":false") {
  return "{\"v\":1,\"id\":\"" + std::string(kId) + "\"," + std::string(extra) +
         ",\"slots\":" + std::string(slots) + "}";
}

TEST(PageTranslateIoTest, RequestIds) {
  EXPECT_TRUE(IsValidRequestId("0"));
  EXPECT_TRUE(IsValidRequestId(kId));
  EXPECT_TRUE(IsValidRequestId(std::string(32, 'f')));
  EXPECT_FALSE(IsValidRequestId(""));
  EXPECT_FALSE(IsValidRequestId(std::string(33, 'f')));
  EXPECT_FALSE(IsValidRequestId("ABCD"));
  EXPECT_FALSE(IsValidRequestId("12\"x"));
}

TEST(PageTranslateIoTest, ScriptsCarryTheirArgumentsAsJson) {
  base::DictValue args = ArgumentsOf(BuildExtractScript(kId));
  EXPECT_EQ(*args.FindString("op"), "extract");
  EXPECT_EQ(*args.FindString("id"), kId);
  const base::DictValue* caps = args.FindDict("caps");
  ASSERT_TRUE(caps);
  EXPECT_EQ(caps->FindInt("slots"), static_cast<int>(kMaxSlots));
  EXPECT_EQ(caps->FindInt("slotChars"), static_cast<int>(kMaxSlotChars));
  EXPECT_EQ(caps->FindInt("pageChars"), static_cast<int>(kMaxPageChars));
  EXPECT_EQ(caps->FindInt("visited"), static_cast<int>(kMaxVisitedNodes));

  // Model text that looks like code stays a string.
  const std::string hostile =
      "\"); alert(1); (\"</script><img src=x onerror=alert(2)>\\ ";
  args = ArgumentsOf(BuildApplyScript(kId, {{3, hostile}, {kMaxSlots, "x"}}));
  EXPECT_EQ(*args.FindString("op"), "apply");
  const base::ListValue* writes = args.FindList("writes");
  ASSERT_TRUE(writes);
  // A slot past the cap is dropped.
  ASSERT_EQ(writes->size(), 1u);
  const base::ListValue& entry = (*writes)[0].GetList();
  ASSERT_EQ(entry.size(), 2u);
  EXPECT_EQ(entry[0].GetInt(), 3);
  EXPECT_EQ(entry[1].GetString(), hostile);

  args = ArgumentsOf(BuildRevertScript());
  EXPECT_EQ(*args.FindString("op"), "revert");
  EXPECT_FALSE(args.FindString("id"));
}

TEST(PageTranslateIoTest, ParseExtraction) {
  std::optional<PageText> page = ParseExtraction(
      Json(Extraction("[[0,true,\"Hallo\"],[1,true,\"Titel\"],"
                      "[2,false,\"Ein Bild\"],[3,false,\"Suchen\"]]",
                      "\"truncated\":true")),
      kId);
  ASSERT_TRUE(page);
  ASSERT_EQ(page->slots.size(), 4u);
  EXPECT_TRUE(page->truncated);
  EXPECT_EQ(page->slots[0].kind, SlotKind::kText);
  EXPECT_TRUE(page->slots[0].in_viewport);
  EXPECT_EQ(page->slots[0].text, "Hallo");
  EXPECT_EQ(page->slots[1].kind, SlotKind::kTitle);
  EXPECT_EQ(page->slots[2].kind, SlotKind::kAlt);
  EXPECT_FALSE(page->slots[2].in_viewport);
  EXPECT_EQ(page->slots[3].kind, SlotKind::kPlaceholder);

  // Empty, and NUL replaced.
  page = ParseExtraction(Json(Extraction("[[0,false,\"a\\u0000b\"]]")), kId);
  ASSERT_TRUE(page);
  EXPECT_EQ(page->slots[0].text.find('\0'), std::string::npos);
  page = ParseExtraction(Json(Extraction("[]")), kId);
  ASSERT_TRUE(page);
  EXPECT_TRUE(page->slots.empty());
}

TEST(PageTranslateIoTest, ParseExtractionFailsClosed) {
  const char* const kBad[] = {
      "[]",
      "{}",
      "{\"v\":1,\"error\":true}",
      "{\"v\":2,\"id\":\"0123abcd\",\"slots\":[]}",
      "{\"v\":1,\"id\":\"ffff\",\"slots\":[]}",
      "{\"v\":1,\"id\":\"0123abcd\"}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":{}}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[0,true]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[0,true,\"a\",1]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[4,true,\"a\"]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[-1,true,\"a\"]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[0.5,true,\"a\"]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[0,1,\"a\"]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[[0,true,5]]}",
      "{\"v\":1,\"id\":\"0123abcd\",\"slots\":[\"a\"]}",
  };
  for (const char* bad : kBad) {
    EXPECT_FALSE(ParseExtraction(Json(bad), kId)) << bad;
  }
  EXPECT_FALSE(ParseExtraction(base::Value(), kId));
}

TEST(PageTranslateIoTest, ParseExtractionEnforcesTheCaps) {
  const std::string max(kMaxSlotBytes, 'a');
  EXPECT_TRUE(ParseExtraction(
      Json(Extraction("[[0,false,\"" + max + "\"]]")), kId));
  EXPECT_FALSE(ParseExtraction(
      Json(Extraction("[[0,false,\"" + max + "a\"]]")), kId));

  // In all.
  std::string slots = "[";
  size_t bytes = 0;
  while (bytes + max.size() <= kMaxPageBytes) {
    slots += (bytes ? "," : "") + std::string("[0,false,\"") + max + "\"]";
    bytes += max.size();
  }
  EXPECT_TRUE(ParseExtraction(Json(Extraction(slots + "]")), kId));
  EXPECT_FALSE(
      ParseExtraction(Json(Extraction(slots + ",[0,false,\"a\"]]")), kId));

  // How many.
  std::string many = "[";
  for (size_t i = 0; i <= kMaxSlots; ++i) {
    many += (i ? "," : "") + std::string("[0,false,\"a\"]");
  }
  EXPECT_FALSE(ParseExtraction(Json(Extraction(many + "]")), kId));
}

TEST(PageTranslateIoTest, ParseApplyResult) {
  EXPECT_EQ(ParseApplyResult(Json("{\"v\":1,\"id\":\"0123abcd\",\"applied\":3}"),
                             kId, 5),
            3u);
  EXPECT_EQ(ParseApplyResult(Json("{\"v\":1,\"id\":\"0123abcd\",\"applied\":0}"),
                             kId, 5),
            0u);
  // The page lost the job, or more than was asked, or someone else's.
  EXPECT_FALSE(ParseApplyResult(
      Json("{\"v\":1,\"id\":\"0123abcd\",\"applied\":-1}"), kId, 5));
  EXPECT_FALSE(ParseApplyResult(
      Json("{\"v\":1,\"id\":\"0123abcd\",\"applied\":6}"), kId, 5));
  EXPECT_FALSE(ParseApplyResult(
      Json("{\"v\":1,\"id\":\"aaaa\",\"applied\":1}"), kId, 5));
  EXPECT_FALSE(ParseApplyResult(Json("{\"v\":1,\"error\":true}"), kId, 5));
  EXPECT_FALSE(ParseApplyResult(base::Value(), kId, 5));
}

TEST(PageTranslateIoTest, ReadBatchReply) {
  std::optional<std::vector<std::string>> t =
      ReadBatchReply("{\"translations\":[\"Hello\",\"Line\\none\"]}", 2);
  ASSERT_TRUE(t);
  EXPECT_EQ(*t, (std::vector<std::string>{"Hello", "Line\none"}));
  EXPECT_TRUE(ReadBatchReply(" {\"translations\": [\"a\"]} \n", 1));

  // Exactly one translation per text, and nothing else.
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[\"a\"]}", 2));
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[\"a\",\"b\"]}", 1));
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[\"a\"],\"note\":\"x\"}", 1));
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[1]}", 1));
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[[\"a\"]]}", 1));
  EXPECT_FALSE(ReadBatchReply("{\"translation\":\"a\"}", 1));
  EXPECT_FALSE(ReadBatchReply("[\"a\"]", 1));
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[\"a\"]} trailing", 1));
  EXPECT_FALSE(ReadBatchReply("{\"translations\":[\"a\",]}", 1));
  EXPECT_FALSE(ReadBatchReply("Here you go: {\"translations\":[\"a\"]}", 1));
  EXPECT_FALSE(ReadBatchReply("", 1));

  // NUL never reaches the page.
  t = ReadBatchReply("{\"translations\":[\"a\\u0000b\"]}", 1);
  ASSERT_TRUE(t);
  EXPECT_EQ((*t)[0].find('\0'), std::string::npos);
}

}  // namespace
}  // namespace molt_ai::page_translate
