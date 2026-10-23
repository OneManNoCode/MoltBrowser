// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The built-in AI APIs' per-site permissions (web_ai/web_ai_site_permissions.h):
// the stored form read back from the profile's prefs (any JSON a damaged or
// hand-edited Preferences file holds: NaN, infinite, negative or huge
// times, any counts, any keys), and the table driven by page origins and
// the user's answers in any order, at any (clock) time.
//
// Checks: no memory or UB error (double -> int conversions); every record
// read back is clean (an answer with a time in [0, now], or prompt history
// with 1..kMaxStoredDismissals closes) and survives a store/load round
// trip; the table never holds more than kMaxSites sites, only plausible
// keys, and a site the user answered keeps its answer until removed; a
// paused site reads as blocked; List() orders allowed, blocked, paused;
// the prompt's input protection agrees with a reference (a press counts
// only kPromptInputProtectionMs after the prompt was shown and after the
// previous press), whatever the clock does.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_site_permissions.h"

namespace molt_ai::web_ai {
namespace {

std::optional<double> StoredDouble(FuzzedDataProvider* p) {
  switch (p->ConsumeIntegralInRange<int>(0, 6)) {
    case 0:
      return std::nullopt;
    case 1:
      return NAN;
    case 2:
      return p->ConsumeBool() ? INFINITY : -INFINITY;
    case 3:
      return p->ConsumeFloatingPoint<double>();
    case 4:
      return static_cast<double>(kMaxStoredTimeMs) +
             p->ConsumeIntegralInRange<int>(-2, 2);
    default:
      return static_cast<double>(
          p->ConsumeIntegralInRange<int64_t>(-1000, int64_t{1} << 42));
  }
}

std::string SiteKey(FuzzedDataProvider* p) {
  switch (p->ConsumeIntegralInRange<int>(0, 3)) {
    case 0:
      return p->ConsumeRandomLengthString(64);
    case 1:
      return "https://a" + std::to_string(p->ConsumeIntegralInRange<int>(0, 600)) +
             ".example";
    default:
      return std::string(p->ConsumeBool() ? "https://" : "http://") +
             std::string(1, static_cast<char>(
                                'a' + p->ConsumeIntegralInRange<int>(0, 3)));
  }
}

void CheckRecord(const SiteRecord& r, int64_t now) {
  const int64_t clamped_now = std::clamp<int64_t>(now, 0, kMaxStoredTimeMs);
  if (r.answer != SiteConsent::kAsk) {
    CHECK(r.answered_ms >= 0 && r.answered_ms <= clamped_now);
    CHECK(r.dismissals == 0);
  } else {
    CHECK(r.dismissals >= 1 && r.dismissals <= kMaxStoredDismissals);
    CHECK(r.last_dismissal_ms >= 0 && r.last_dismissal_ms <= kMaxStoredTimeMs);
  }
}

}  // namespace
}  // namespace molt_ai::web_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace molt_ai::web_ai;
  FuzzedDataProvider p(data, size);
  int64_t now = p.ConsumeIntegralInRange<int64_t>(-1000, int64_t{1} << 54);

  // Loading the stored dictionary.
  ClassifyStoredVersion(p.ConsumeBool() ? std::optional<int>()
                                        : p.ConsumeIntegral<int>());
  SitePermissionTable table;
  const size_t stored_count = p.ConsumeIntegralInRange<size_t>(0, 32);
  for (size_t i = 0; i < stored_count; ++i) {
    StoredSite stored;
    switch (p.ConsumeIntegralInRange<int>(0, 3)) {
      case 0:
        break;
      case 1:
        stored.answer = kStoredAllow;
        break;
      case 2:
        stored.answer = kStoredBlock;
        break;
      default:
        stored.answer = p.ConsumeRandomLengthString(8);
        break;
    }
    stored.answered_ms = StoredDouble(&p);
    if (p.ConsumeBool()) stored.dismissals = p.ConsumeIntegral<int>();
    stored.last_dismissal_ms = StoredDouble(&p);
    const std::optional<SiteRecord> record = RecordFromStored(stored, now);
    if (!record) continue;
    CheckRecord(*record, now);
    // Store and load again: the same record.
    const std::optional<SiteRecord> again =
        RecordFromStored(StoredFromRecord(*record), now);
    CHECK(again && again->answer == record->answer &&
          again->answered_ms == record->answered_ms &&
          again->dismissals == record->dismissals &&
          again->last_dismissal_ms == record->last_dismissal_ms);
    const std::string key = SiteKey(&p);
    CHECK(table.Insert(key, *record) == IsPlausibleSiteKey(key));
  }

  // The prompt's input protection and its reference.
  PromptInputProtection guard;
  std::optional<int64_t> shown;
  std::optional<int64_t> last_press;

  // The user and pages, over time.
  for (int step = 0; step < 2048 && p.remaining_bytes() > 0; ++step) {
    now += p.ConsumeIntegralInRange<int64_t>(-1000, kEmbargoMs);
    const std::string key = SiteKey(&p);
    switch (p.ConsumeIntegralInRange<int>(0, 6)) {
      case 6: {
        // Small steps too: presses come milliseconds apart.
        now += p.ConsumeIntegralInRange<int64_t>(-kEmbargoMs, 0) +
               p.ConsumeIntegralInRange<int64_t>(0, 2000);
        if (p.ConsumeBool()) {
          guard.Shown(now);
          shown = now;
          break;
        }
        const bool counts = guard.Press(now);
        const bool after_last =
            !last_press || (now >= *last_press &&
                            now - *last_press >= kPromptInputProtectionMs);
        const bool after_shown =
            shown && now >= *shown && now - *shown >= kPromptInputProtectionMs;
        CHECK(counts == (after_last && after_shown));
        last_press = now;
        break;
      }
      case 0: {
        const auto answer =
            static_cast<SiteConsent>(p.ConsumeIntegralInRange<int>(0, 2));
        const bool ok = table.SetAnswer(key, answer, now);
        CHECK(ok == IsPlausibleSiteKey(key));
        if (ok && answer != SiteConsent::kAsk) {
          CHECK(table.Get(key, now) == answer);
        }
        break;
      }
      case 1: {
        const SiteRecord* before = table.Find(key);
        const SiteConsent answer =
            before ? before->answer : SiteConsent::kAsk;
        const bool paused = table.RecordDismissal(key, now);
        if (answer != SiteConsent::kAsk) {
          // No prompt for a site with an answer: nothing changes.
          CHECK(!paused && table.Get(key, now) == answer);
        } else if (paused) {
          CHECK(table.Get(key, now) == SiteConsent::kBlocked);
        }
        break;
      }
      case 2:
        table.Remove(key);
        CHECK(!table.Find(key));
        break;
      case 3: {
        const int64_t hi = std::max<int64_t>(now, 0) + 10;
        const int64_t begin = p.ConsumeIntegralInRange<int64_t>(-10, hi);
        const int64_t end = p.ConsumeIntegralInRange<int64_t>(begin, hi);
        table.RemoveAnswersBetween(begin, end, {key, SiteKey(&p)});
        break;
      }
      case 4:
        table.RemovePromptHistory({key});
        if (const SiteRecord* r = table.Find(key)) {
          CHECK(r->answer != SiteConsent::kAsk);
        }
        break;
      default:
        if (p.ConsumeIntegralInRange<int>(0, 15) == 0) table.Clear();
        break;
    }
    CHECK(table.size() <= SitePermissionTable::kMaxSites);
  }

  // Everything in the table is clean and listed in order.
  for (const auto& [site, record] : table.records()) {
    CHECK(IsPlausibleSiteKey(site));
    if (record.answer != SiteConsent::kAsk) {
      CHECK(record.answered_ms >= 0 && record.answered_ms <= kMaxStoredTimeMs);
    } else {
      CHECK(record.dismissals >= 1 && record.dismissals <= kMaxStoredDismissals);
    }
  }
  const std::vector<SiteListEntry> rows = table.List(now);
  for (size_t i = 1; i < rows.size(); ++i) {
    CHECK(rows[i - 1].kind < rows[i].kind ||
          (rows[i - 1].kind == rows[i].kind && rows[i - 1].site < rows[i].site));
  }
  for (const SiteListEntry& row : rows) {
    if (row.kind == SiteListEntry::Kind::kPaused) {
      CHECK(row.until_ms == row.since_ms + kEmbargoMs);
      CHECK(table.Get(row.site, now) == SiteConsent::kBlocked);
    }
  }
  CHECK(table.Sites().size() == table.size());
  return 0;
}
