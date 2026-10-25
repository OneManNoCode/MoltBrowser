// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A workflow step's fallback locators (automation/automation_locators.h):
// the "locators" block of a .molt step (any file may be imported), the
// capture script's report and the probe script's report (both from a page,
// through a script in an isolated world a compromised renderer controls).
// The input is JSON: {"stored": ..., "capture": ..., "probe": ...,
// "selector": "...", "origin": "...", "hint": "..."}; any part may be
// missing or of any shape.
//
// Checks: no memory or UB error; a parsed block (stored or captured) is
// within every cap, writes back to the same block and parses back equal;
// the probe order is the recorded selector then the block's locators; a
// decision only ever names a valid, eligible candidate (the recorded kind,
// a credential field only for a credential step) and a locator that found
// it alone, and only on the recorded origin; a weak or conflicting choice
// is never reported as strong; the user's text is one clean line within
// its cap; the scripts carry page text only as JSON.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/json/json_reader.h"
#include "base/values.h"
#include "chrome/browser/molt_ai/automation/automation_locators.h"

namespace molt_ai::automation::locators {
namespace {

void CheckBlock(const StepLocators& s) {
  CHECK(!s.list.empty());
  CHECK_LE(s.list.size(), kMaxLocators);
  CHECK(s.origin.starts_with("https://") || s.origin.starts_with("http://"));
  CHECK_LE(s.origin.size(), kMaxOriginBytes);
  CHECK_EQ(s.element.tag == "input", !s.element.type.empty());
  for (size_t i = 0; i < s.list.size(); ++i) {
    const Locator& l = s.list[i];
    CHECK(l.by != By::kSelector) << "the runner's own entry, read from data";
    CHECK(!l.value.empty());
    CHECK(l.value.find("{{") == std::string::npos);
    CHECK_LE(l.value.size(),
             l.by == By::kPath ? kMaxPathBytes : 4 * kMaxLocatorTextChars);
    for (size_t j = 0; j < i; ++j)
      CHECK(!(s.list[j] == l)) << "a locator twice";
  }
  std::optional<StepLocators> back = Parse(ToValue(s));
  CHECK(back) << "a block that doesn't read back";
  CHECK(*back == s) << "a block that changes on a round trip";
}

void CheckLine(const std::string& s, size_t max_chars) {
  size_t chars = 0;
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    CHECK(c >= 0x20 && c != 0x7F) << "a control character for the user";
    CHECK(c != '"');
    if ((c & 0xC0) != 0x80)
      ++chars;
  }
  CHECK_LE(chars, max_chars);
}

}  // namespace
}  // namespace molt_ai::automation::locators

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::automation::locators;
  const std::string json(reinterpret_cast<const char*>(data), size);
  std::optional<base::Value> parsed =
      base::JSONReader::Read(json, base::JSON_PARSE_RFC);
  if (!parsed || !parsed->is_dict())
    return 0;
  const base::DictValue& in = parsed->GetDict();
  auto string_of = [&](const char* key) {
    const std::string* s = in.FindString(key);
    return s ? *s : std::string();
  };

  std::optional<StepLocators> stored;
  if (const base::Value* v = in.Find("stored")) {
    stored = Parse(*v);
    if (stored)
      CheckBlock(*stored);
  }
  if (const base::Value* v = in.Find("capture")) {
    std::optional<StepLocators> captured = FromCaptureReport(*v);
    if (captured)
      CheckBlock(*captured);
    if (!stored)
      stored = std::move(captured);
  }
  if (!stored)
    return 0;

  const std::string selector = string_of("selector");
  const std::vector<Locator> order = ProbeOrder(selector, *stored);
  CHECK_EQ(order.size(), stored->list.size() + (selector.empty() ? 0u : 1u));
  if (!selector.empty())
    CHECK(order[0].by == By::kSelector);

  const std::string hint = string_of("hint");
  const std::string script = BuildProbeScript("p", order, hint);
  CHECK(script.find("</script") == std::string::npos);

  const base::Value* probe_value = in.Find("probe");
  if (!probe_value)
    return 0;
  std::optional<Probe> probe = ParseProbe(*probe_value, order.size());
  if (!probe)
    return 0;
  CHECK_LE(probe->candidates.size(), kMaxProbeCandidates);
  for (const Candidate& c : probe->candidates)
    CheckLine(c.label, kMaxShownLabelChars);

  const std::string page_origin = string_of("origin");
  const Decision d = Decide(order, *probe, *stored, page_origin);
  if (d.strength == Strength::kNone)
    return 0;
  CHECK_EQ(page_origin, stored->origin) << "chose on another origin";
  CHECK_EQ(probe->origin, stored->origin);
  CHECK_LT(d.candidate, probe->candidates.size());
  CHECK_LT(d.locator, order.size());
  const Candidate& chosen = probe->candidates[d.candidate];
  CHECK(Eligible(*stored, chosen)) << "chose an ineligible element";
  CHECK(KindMatches(stored->element, chosen.kind));
  CHECK(chosen.field == stored->field) << "a credential field mismatch";
  CHECK(!probe->overflow[d.locator]);
  // The locator found it alone among the eligible ones.
  for (size_t c : probe->matches[d.locator]) {
    if (Eligible(*stored, probe->candidates[c]))
      CHECK_EQ(c, d.candidate);
  }
  // No locator before it found exactly one eligible element.
  for (size_t i = 0; i < d.locator; ++i) {
    if (probe->overflow[i])
      continue;
    size_t eligible = 0;  // the probe's indices are distinct
    for (size_t c : probe->matches[i]) {
      if (Eligible(*stored, probe->candidates[c]))
        ++eligible;
    }
    CHECK_NE(eligible, 1u) << "skipped an earlier locator";
  }
  CHECK_EQ(d.strength != Strength::kWeak, IsStrong(order[d.locator]));
  const std::string question =
      WeakMatchQuestion(hint, chosen, Describe(order[d.locator]), d.strength);
  CHECK(question.find('\n') == std::string::npos);
  return 0;
}
