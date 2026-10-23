// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The built-in AI APIs' policy (web_ai/web_ai_policy.h) on what web pages
// control: language tags, text sizes, sampling parameters (any float, NaN
// included), image sizes, and the order and number of their requests
// (GenerationScheduler, checked against a reference model), and the device
// facts kept per site (SiteDeviceFacts, against a reference model).
//
// Checks: no memory or UB error; token estimates saturate rather than wrap
// and agree with the size shortcut; sampling stays within the documented
// range; IsValidLanguageTag() accepts exactly 2-8 letters then 1-8
// alphanumerics per subtag, at most 35 bytes; the scheduler never admits
// more than its caps, runs one request at a time, and agrees with the model
// on what is queued and running; held images never exceed the caps; a site
// is only ever answered from its own facts or told to wait for its own
// read; a session serves only where the context, the switch and the site's
// answer all allow it.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai::web_ai {
namespace {

bool ReferenceTag(const std::string& tag) {
  if (tag.empty() || tag.size() > 35) return false;
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    const size_t dash = tag.find('-', start);
    parts.push_back(tag.substr(start, dash - start));
    if (dash == std::string::npos) break;
    start = dash + 1;
  }
  for (size_t i = 0; i < parts.size(); ++i) {
    const std::string& s = parts[i];
    const size_t min = i == 0 ? 2 : 1;
    if (s.size() < min || s.size() > 8) return false;
    for (char c : s) {
      const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
      const bool digit = c >= '0' && c <= '9';
      if (!alpha && !(i > 0 && digit)) return false;
    }
  }
  return true;
}

void CheckStrings(FuzzedDataProvider* p) {
  const std::string tag = p->ConsumeRandomLengthString(48);
  CHECK(IsValidLanguageTag(tag) == ReferenceTag(tag));
  const std::string name = LanguageNameForPrompt(tag);
  CHECK(name.empty() == !IsValidLanguageTag(tag));
  const std::string base = BaseLanguage(tag);
  CHECK(base.size() <= tag.size());

  const std::string text = p->ConsumeRandomLengthString(1 << 16);
  const uint32_t tokens = EstimateTokens(text);
  // At most 4/3 of a token per byte (a lone lead byte).
  CHECK(uint64_t{tokens} * 3 <= uint64_t{text.size()} * 4 + 2);
  CHECK(EstimateMessageTokens(text) == AddTokens(tokens, kPerMessageTokens));
  const uint32_t quota = p->ConsumeIntegral<uint32_t>();
  // The size shortcut never refuses what the estimate would accept, for
  // valid UTF-8 (a lone continuation byte counts for nothing).
  bool ascii = true;
  for (char c : text) ascii = ascii && static_cast<unsigned char>(c) < 0x80;
  if (ascii && ExceedsQuotaBySize(text.size(), quota)) CHECK(tokens > quota);
  CHECK(TokensForSize(text.size()) >= (text.size() + 2) / 3 ||
        TokensForSize(text.size()) == UINT32_MAX);
  CHECK(AddTokens(UINT32_MAX, p->ConsumeIntegral<uint32_t>()) == UINT32_MAX);

  float temperature;
  switch (p->ConsumeIntegralInRange<int>(0, 3)) {
    case 0:
      temperature = NAN;
      break;
    case 1:
      temperature = p->ConsumeBool() ? INFINITY : -INFINITY;
      break;
    default:
      temperature = p->ConsumeFloatingPoint<float>();
      break;
  }
  const Sampling s = ClampSampling(p->ConsumeIntegral<uint32_t>(), temperature);
  CHECK(s.top_k >= 1 && s.top_k <= kMaxSampling.top_k);
  CHECK(s.temperature >= 0.0f && s.temperature <= kMaxSampling.temperature);
  const Sampling mode = SamplingForMode(
      static_cast<SamplingMode>(p->ConsumeIntegralInRange<int>(0, 6)));
  CHECK(mode.top_k >= 1 && mode.top_k <= kMaxSampling.top_k);

  const int64_t w = p->ConsumeIntegral<int64_t>();
  const int64_t h = p->ConsumeIntegral<int64_t>();
  if (IsUsableImageSource(w, h)) {
    CHECK(w > 0 && h > 0);
    CHECK(static_cast<uint64_t>(w) * static_cast<uint64_t>(h) <=
          kMaxImageSourcePixels);
  }
  const LanguageModelLimits limits =
      LanguageModelLimitsFor(p->ConsumeIntegral<uint32_t>());
  CHECK(limits.input_quota == kSmallInputQuota ||
        limits.input_quota == kLargeInputQuota);

  std::vector<ModelCandidate> models(p->ConsumeIntegralInRange<size_t>(0, 6));
  for (ModelCandidate& m : models) {
    m.model_id = p->ConsumeRandomLengthString(8);
    m.downloaded = p->ConsumeBool();
    m.ram_required_bytes = p->ConsumeIntegral<uint64_t>();
    m.sees_images = p->ConsumeBool();
  }
  const std::string loaded = p->ConsumeRandomLengthString(8);
  const std::string preferred = p->ConsumeRandomLengthString(8);
  const bool wants_images = p->ConsumeBool();
  const std::string pick = PickWebModel(loaded, preferred, models, wants_images);
  if (!loaded.empty()) {
    CHECK(pick == loaded);
  } else if (!pick.empty()) {
    bool found = false;
    for (const ModelCandidate& m : models) {
      found = found || (m.model_id == pick && m.downloaded);
    }
    CHECK(found) << "picked a model that isn't downloaded";
  } else {
    for (const ModelCandidate& m : models) {
      CHECK(!m.downloaded || m.model_id.empty());
    }
  }
  AnyModelFits(models, p->ConsumeIntegral<uint64_t>());
  AnyModelInstalled(models);

  AvailabilityInput in;
  in.context.feature_enabled = p->ConsumeBool();
  in.context.enterprise_allowed = p->ConsumeBool();
  in.context.user_enabled = p->ConsumeBool();
  in.context.moltnet = p->ConsumeBool();
  in.context.is_document = p->ConsumeBool();
  in.context.http_top_level = p->ConsumeBool();
  in.device.inference_supported = p->ConsumeBool();
  in.device.model_installed = p->ConsumeBool();
  in.device.some_model_fits = p->ConsumeBool();
  in.device.web_apis_enabled = p->ConsumeBool();
  in.options_supported = p->ConsumeBool();
  in.languages_supported = p->ConsumeBool();
  in.consent = static_cast<SiteConsent>(p->ConsumeIntegralInRange<int>(0, 2));
  const AvailabilityDecision decision = DecideAvailability(in);
  const CreateAction action = DecideCreateAction(in);
  // A MoltNet window, a blocked site or a disabled API never gets a model.
  if (in.context.moltnet || in.consent == SiteConsent::kBlocked ||
      !IsContextEligible(in.context)) {
    CHECK(decision.availability == Availability::kUnavailable);
    CHECK(action == CreateAction::kReject);
  }
  if (action == CreateAction::kCreate) {
    CHECK(in.consent == SiteConsent::kAllowed && in.device.model_installed);
  }
  if (MaySessionServe(in.context, in.device.web_apis_enabled, in.consent)) {
    CHECK(IsContextEligible(in.context) && in.device.web_apis_enabled &&
          in.consent == SiteConsent::kAllowed);
    CHECK(decision.availability != Availability::kUnavailable ||
          !in.options_supported || !in.languages_supported ||
          !in.device.inference_supported ||
          (!in.device.model_installed && !in.device.some_model_fits));
  }
}

// SiteDeviceFacts against a reference: each site's own facts, read time
// and read in flight, over sites, times, stale tickets and clears.
void CheckSiteDeviceFacts(FuzzedDataProvider* p) {
  const int64_t lifetime = p->ConsumeIntegralInRange<int64_t>(1, 1 << 20);
  SiteDeviceFacts cache(lifetime);
  struct Entry {
    std::optional<DeviceFacts> facts;
    int64_t read_at = 0;
    uint64_t pending = 0;
  };
  std::map<std::string, Entry> ref;
  std::map<uint64_t, std::string> ticket_site;
  std::set<uint64_t> tickets;
  int64_t now = p->ConsumeIntegralInRange<int64_t>(0, int64_t{1} << 40);
  auto fresh = [&](const Entry& e) {
    return e.facts && now - e.read_at < lifetime;
  };
  auto site_of = [](FuzzedDataProvider* fp) {
    return std::string(
        1, static_cast<char>('a' + fp->ConsumeIntegralInRange<int>(0, 5)));
  };
  for (int step = 0; step < 2048 && p->remaining_bytes() > 0; ++step) {
    now += p->ConsumeIntegralInRange<int64_t>(0, 2 * lifetime);
    switch (p->ConsumeIntegralInRange<int>(0, 3)) {
      case 0:
      case 1: {
        const std::string site = site_of(p);
        const SiteDeviceFacts::Lookup lookup = cache.Find(site, now);
        Entry& e = ref[site];
        if (e.pending != 0) {
          CHECK(lookup.kind == SiteDeviceFacts::Lookup::Kind::kJoin);
          CHECK(lookup.ticket == e.pending);
        } else if (fresh(e)) {
          CHECK(lookup.kind == SiteDeviceFacts::Lookup::Kind::kCached);
          CHECK(lookup.facts.web_apis_enabled == e.facts->web_apis_enabled &&
                lookup.facts.model_installed == e.facts->model_installed);
        } else {
          CHECK(lookup.kind == SiteDeviceFacts::Lookup::Kind::kRead);
          CHECK(lookup.ticket != 0 && tickets.insert(lookup.ticket).second);
          e.pending = lookup.ticket;
          ticket_site[lookup.ticket] = site;
        }
        // A join is always for the site's own read.
        if (lookup.kind != SiteDeviceFacts::Lookup::Kind::kCached) {
          CHECK(ticket_site[lookup.ticket] == site);
        }
        // Find() forgets the sites no call would use.
        size_t in_use = 0;
        for (const auto& [s, entry] : ref) {
          in_use += entry.pending != 0 || fresh(entry);
        }
        CHECK(cache.site_count() == in_use);
        break;
      }
      case 2: {
        // A read ends: a site's own, or any ticket (stale, unknown, 0).
        const std::string site = site_of(p);
        uint64_t ticket = p->ConsumeIntegralInRange<uint64_t>(0, 4);
        if (!tickets.empty() && p->ConsumeBool()) {
          auto it = tickets.begin();
          std::advance(
              it, p->ConsumeIntegralInRange<size_t>(0, tickets.size() - 1));
          ticket = *it;
        }
        DeviceFacts facts;
        facts.web_apis_enabled = p->ConsumeBool();
        facts.model_installed = p->ConsumeBool();
        const bool kept = cache.Finish(site, ticket, facts, now);
        auto it = ref.find(site);
        const bool expected =
            ticket != 0 && it != ref.end() && it->second.pending == ticket;
        CHECK(kept == expected);
        if (kept) {
          it->second.pending = 0;
          it->second.facts = facts;
          it->second.read_at = now;
        }
        break;
      }
      default:
        if (p->ConsumeIntegralInRange<int>(0, 7) == 0) {
          cache.Clear();
          ref.clear();
        }
        break;
    }
    // Drop the reference's entries the cache forgot (not in use).
    for (auto it = ref.begin(); it != ref.end();) {
      it = it->second.pending == 0 && !fresh(it->second) ? ref.erase(it)
                                                         : std::next(it);
    }
  }
}

// GenerationScheduler against a reference: which ids are queued (per
// queue, in order), which one runs, and the per-site counts.
void CheckScheduler(FuzzedDataProvider* p) {
  GenerationScheduler scheduler;
  struct Ref {
    std::deque<std::pair<uint64_t, std::string>> fg, bg;
    uint64_t running = 0;
    std::string running_site;
    std::map<std::string, size_t> sessions;
    std::map<std::string, size_t> images;
    size_t images_total = 0;
    std::map<std::string, uint64_t> last_start;
    uint64_t starts = 0;
  } ref;
  auto pending_for = [&](const std::string& site) {
    size_t n = ref.running != 0 && ref.running_site == site ? 1 : 0;
    for (const auto* q : {&ref.fg, &ref.bg}) {
      for (const auto& e : *q) n += e.second == site;
    }
    return n;
  };
  auto site_of = [&](FuzzedDataProvider* fp) {
    return std::string(1, static_cast<char>('a' + fp->ConsumeIntegralInRange<int>(0, 5)));
  };
  uint64_t next_id = 1;
  for (int step = 0; step < 2048 && p->remaining_bytes() > 0; ++step) {
    switch (p->ConsumeIntegralInRange<int>(0, 8)) {
      case 0:
      case 1: {
        const std::string site = site_of(p);
        const bool fg = p->ConsumeBool();
        const uint64_t id = next_id++;
        const size_t total =
            ref.fg.size() + ref.bg.size() + (ref.running != 0 ? 1 : 0);
        const auto admission = scheduler.Enqueue(id, site, fg);
        if (total >= kMaxPendingTotal) {
          CHECK(admission == GenerationScheduler::Admission::kQueueFull);
        } else if (pending_for(site) >= kMaxPendingPerSite) {
          CHECK(admission == GenerationScheduler::Admission::kSiteBusy);
        } else {
          CHECK(admission == GenerationScheduler::Admission::kAdmitted);
          (fg ? ref.fg : ref.bg).push_back({id, site});
        }
        break;
      }
      case 2: {
        const uint64_t id = scheduler.StartNext();
        if (ref.running != 0 || (ref.fg.empty() && ref.bg.empty())) {
          CHECK(id == 0);
          break;
        }
        // Foreground first; in the queue, the oldest request of the site
        // that started a generation least recently.
        auto& q = ref.fg.empty() ? ref.bg : ref.fg;
        auto last = [&](const std::string& site) {
          auto it = ref.last_start.find(site);
          return it == ref.last_start.end() ? 0 : it->second;
        };
        auto want = q.begin();
        for (auto it = q.begin(); it != q.end(); ++it) {
          if (last(it->second) < last(want->second)) want = it;
        }
        CHECK(id == want->first) << "started the wrong request";
        ref.running = id;
        ref.running_site = want->second;
        q.erase(want);
        ref.last_start[ref.running_site] = ++ref.starts;
        break;
      }
      case 3: {
        // Finish the running one, a queued one, or an unknown id.
        uint64_t id = p->ConsumeIntegralInRange<uint64_t>(0, next_id);
        if (p->ConsumeBool() && ref.running) id = ref.running;
        scheduler.Finish(id);
        if (id == 0) break;
        std::string site;
        if (id == ref.running) {
          site = ref.running_site;
          ref.running = 0;
          ref.running_site.clear();
        } else {
          for (auto* q : {&ref.fg, &ref.bg}) {
            auto it = std::find_if(q->begin(), q->end(), [&](const auto& e) {
              return e.first == id;
            });
            if (it != q->end()) {
              site = it->second;
              q->erase(it);
              break;
            }
          }
        }
        // A site with nothing left loses its place in the turn order.
        if (!site.empty() && pending_for(site) == 0) ref.last_start.erase(site);
        break;
      }
      case 4: {
        const std::string site = site_of(p);
        const bool added = scheduler.TryAddSession(site);
        CHECK(added == (ref.sessions[site] < kMaxSessionsPerSite));
        if (added) ++ref.sessions[site];
        break;
      }
      case 5: {
        const std::string site = site_of(p);
        scheduler.RemoveSession(site);
        if (ref.sessions[site] > 0) --ref.sessions[site];
        break;
      }
      case 6: {
        const std::string site = site_of(p);
        const size_t count = p->ConsumeBool()
                                 ? p->ConsumeIntegral<size_t>()
                                 : p->ConsumeIntegralInRange<size_t>(0, 20);
        const bool held = scheduler.TryHoldImages(site, count);
        const bool fits = count <= kMaxHeldImagesPerSite - ref.images[site] &&
                          count <= kMaxHeldImagesTotal - ref.images_total;
        CHECK(held == fits);
        if (held) {
          ref.images[site] += count;
          ref.images_total += count;
        }
        break;
      }
      case 7: {
        const std::string site = site_of(p);
        const size_t count = p->ConsumeIntegral<size_t>() % 24;
        scheduler.ReleaseImages(site, count);
        const size_t released = std::min(count, ref.images[site]);
        ref.images[site] -= released;
        ref.images_total -= released;
        break;
      }
      default:
        break;
    }
    // Agreement with the reference.
    CHECK(scheduler.pending_total() ==
          ref.fg.size() + ref.bg.size() + (ref.running != 0 ? 1 : 0));
    CHECK(scheduler.pending_total() <= kMaxPendingTotal);
    if (ref.running) CHECK(scheduler.IsRunning(ref.running));
    for (const char c : std::string("abcdef")) {
      const std::string site(1, c);
      CHECK(scheduler.PendingForSite(site) == pending_for(site));
      CHECK(scheduler.PendingForSite(site) <= kMaxPendingPerSite);
      CHECK(scheduler.SessionsForSite(site) == ref.sessions[site]);
      CHECK(scheduler.ImagesHeldForSite(site) == ref.images[site]);
      CHECK(ref.images[site] <= kMaxHeldImagesPerSite);
    }
    CHECK(scheduler.images_held_total() == ref.images_total);
    CHECK(ref.images_total <= kMaxHeldImagesTotal);
    for (const auto* q : {&ref.fg, &ref.bg}) {
      for (const auto& e : *q) CHECK(scheduler.IsQueued(e.first));
    }
  }
}

}  // namespace
}  // namespace molt_ai::web_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider p(data, size);
  switch (p.ConsumeIntegralInRange<int>(0, 2)) {
    case 0:
      molt_ai::web_ai::CheckStrings(&p);
      break;
    case 1:
      molt_ai::web_ai::CheckScheduler(&p);
      break;
    default:
      molt_ai::web_ai::CheckSiteDeviceFacts(&p);
      break;
  }
  return 0;
}
