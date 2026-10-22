// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The shared safety rules for everything that drives a page
// (security/agent_safety.h) on page text, page script results, selectors
// from pages and workflow files, and the user's goal text.
//
// Checks:
//  * SanitizeUntrustedText() output can't forge the untrusted-content fence
//    or a chat-template control token: no run of three '<' or '>', no "<|"
//    or "|>", no "untrusted_page_content" in any case, no control
//    characters but \n and \t; FenceUntrustedText() has exactly one fence;
//  * ExtractExplicitURLs() returns http(s) pieces of the text without
//    blanks, quotes, brackets or trailing punctuation;
//  * ParseElementProbes() always returns |expected| probes;
//  * IsAllowedNavigationURL() only passes http(s);
//  * the field classifiers and BuildRunJsSource() hold their shape.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <string>
#include <string_view>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/security/agent_safety.h"
#include "url/gurl.h"

namespace molt_ai::agent_safety {
namespace {

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

size_t Count(std::string_view haystack, std::string_view needle) {
  size_t n = 0;
  for (size_t at = haystack.find(needle); at != std::string_view::npos;
       at = haystack.find(needle, at + 1)) {
    ++n;
  }
  return n;
}

void CheckSanitized(const std::string& text) {
  const std::string out = SanitizeUntrustedText(text);
  CHECK(out.find("<<<") == std::string::npos) << "a fence opener survived";
  CHECK(out.find(">>>") == std::string::npos) << "a fence closer survived";
  CHECK(out.find("<|") == std::string::npos);
  CHECK(out.find("|>") == std::string::npos);
  CHECK(Lower(out).find("untrusted_page_content") == std::string::npos);
  for (char ch : out) {
    const auto c = static_cast<unsigned char>(ch);
    CHECK(c >= 0x20 || c == '\n' || c == '\t');
    CHECK(c != 0x7F);
  }
  // The angle-bracket look-alikes are folded, so they can't stand in for
  // the fence either.
  for (const char* lookalike :
       {"\xEF\xBC\x9C", "\xEF\xBC\x9E", "\xE2\x80\xB9", "\xE2\x80\xBA",
        "\xC2\xAB", "\xC2\xBB", "\xE3\x80\x88", "\xE3\x80\x89"}) {
    CHECK(out.find(lookalike) == std::string::npos);
  }
  CHECK(SanitizeUntrustedText(out) == out) << "sanitizing twice changed it";

  const std::string fenced = FenceUntrustedText(text);
  CHECK(fenced.starts_with(kUntrustedBegin));
  CHECK(fenced.ends_with(kUntrustedEnd));
  CHECK(Count(fenced, kUntrustedBegin) == 1 && Count(fenced, kUntrustedEnd) == 1);
}

void CheckUrls(const std::string& text) {
  for (const std::string& url : ExtractExplicitURLs(text)) {
    const std::string lower = Lower(url);
    CHECK(lower.starts_with("http://") || lower.starts_with("https://"));
    CHECK(url.size() > (lower.starts_with("https://") ? 8u : 7u));
    CHECK(text.find(url) != std::string::npos);
    for (char c : url) {
      CHECK(c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '"' &&
            c != '\'' && c != '<' && c != '>' && c != '(' && c != ')' &&
            c != '[' && c != ']' && c != '{' && c != '}' && c != '`');
    }
    const char last = url.back();
    CHECK(last != '.' && last != ',' && last != ';' && last != ':' &&
          last != '!' && last != '?');
  }
}

}  // namespace
}  // namespace molt_ai::agent_safety

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::agent_safety;
  FuzzedDataProvider p(data, size);
  const std::string selector = p.ConsumeRandomLengthString(256);
  const std::string input_type = p.ConsumeRandomLengthString(32);
  const std::string autocomplete = p.ConsumeRandomLengthString(64);
  const std::string probe_json = p.ConsumeRandomLengthString(4096);
  const size_t expected = p.ConsumeIntegralInRange<size_t>(0, 16);
  const std::string text = p.ConsumeRemainingBytesAsString();

  CheckSanitized(text);
  CheckUrls(text);

  const FieldKind field = ClassifyField(input_type, autocomplete);
  CHECK(FieldKindFromString(FieldKindToString(field)) == field);
  CHECK(!FieldKindLabel(field).empty());
  if (Lower(input_type) == "password") CHECK(field == FieldKind::kPassword);
  const FieldKind by_selector = ClassifySelector(selector);
  CHECK(SelectorLooksSensitive(selector) == IsSensitive(by_selector));

  std::optional<base::Value> result = base::JSONReader::Read(probe_json);
  const std::vector<ElementProbe> probes = ParseElementProbes(
      result ? std::move(*result) : base::Value(), expected);
  CHECK(probes.size() == expected);
  for (const ElementProbe& probe : probes) {
    if (!probe.found) {
      CHECK(probe.field_kind == FieldKind::kOrdinary && !probe.submits_form &&
            probe.navigates_to.empty());
    }
  }
  BuildElementProbeScript({selector, text.substr(0, 64)});

  const GURL url(text.substr(0, 512));
  if (IsAllowedNavigationURL(url)) {
    CHECK(url.SchemeIs("http") || url.SchemeIs("https"));
  }

  const std::string js = BuildRunJsSource(selector, probe_json);
  CHECK(js.starts_with("{const vars=Object.freeze("));
  CHECK(js.ends_with("\n}"));
  return 0;
}
