// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace molt_ai::web_ai {

namespace {

bool IsAsciiAlpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAsciiDigit(char c) {
  return c >= '0' && c <= '9';
}

char ToLowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

uint32_t Saturate(uint64_t v) {
  return v > std::numeric_limits<uint32_t>::max()
             ? std::numeric_limits<uint32_t>::max()
             : static_cast<uint32_t>(v);
}

struct LanguageName {
  const char* code;
  const char* name;
};

// Primary subtags with an English name for the model's instructions. Others
// are named by their tag.
constexpr LanguageName kLanguageNames[] = {
    {"ar", "Arabic"},     {"bg", "Bulgarian"},  {"bn", "Bengali"},
    {"ca", "Catalan"},    {"cs", "Czech"},      {"da", "Danish"},
    {"de", "German"},     {"el", "Greek"},      {"en", "English"},
    {"es", "Spanish"},    {"et", "Estonian"},   {"fa", "Persian"},
    {"fi", "Finnish"},    {"fil", "Filipino"},  {"fr", "French"},
    {"he", "Hebrew"},     {"hi", "Hindi"},      {"hr", "Croatian"},
    {"hu", "Hungarian"},  {"id", "Indonesian"}, {"it", "Italian"},
    {"ja", "Japanese"},   {"ko", "Korean"},     {"lt", "Lithuanian"},
    {"lv", "Latvian"},    {"ms", "Malay"},      {"nb", "Norwegian"},
    {"nl", "Dutch"},      {"no", "Norwegian"},  {"pl", "Polish"},
    {"pt", "Portuguese"}, {"ro", "Romanian"},   {"ru", "Russian"},
    {"sk", "Slovak"},     {"sl", "Slovenian"},  {"sr", "Serbian"},
    {"sv", "Swedish"},    {"sw", "Swahili"},    {"ta", "Tamil"},
    {"te", "Telugu"},     {"th", "Thai"},       {"tr", "Turkish"},
    {"uk", "Ukrainian"},  {"ur", "Urdu"},       {"vi", "Vietnamese"},
    {"zh", "Chinese"},
};

}  // namespace

// ---------------------------------------------------------------------------
// Availability and create()
// ---------------------------------------------------------------------------

bool IsUsableImageSource(int64_t width, int64_t height) {
  if (width <= 0 || height <= 0 ||
      width > static_cast<int64_t>(kMaxImageSourcePixels) ||
      height > static_cast<int64_t>(kMaxImageSourcePixels)) {
    return false;
  }
  // Both sides are at most 2^26 here, so the product fits.
  return static_cast<uint64_t>(width) * static_cast<uint64_t>(height) <=
         kMaxImageSourcePixels;
}

bool IsContextEligible(const ContextFacts& c) {
  return c.feature_enabled && c.enterprise_allowed && c.user_enabled &&
         !c.moltnet && c.is_document && c.http_top_level;
}

bool MaySessionServe(const ContextFacts& context,
                     bool web_apis_enabled,
                     SiteConsent consent) {
  return IsContextEligible(context) && web_apis_enabled &&
         consent == SiteConsent::kAllowed;
}

AvailabilityDecision DecideAvailability(const AvailabilityInput& input) {
  const ContextFacts& c = input.context;
  const DeviceFacts& d = input.device;
  auto unavailable = [](UnavailableReason reason) {
    return AvailabilityDecision{Availability::kUnavailable, reason};
  };
  if (!c.feature_enabled) {
    return unavailable(UnavailableReason::kDisabled);
  }
  if (!c.enterprise_allowed) {
    return unavailable(UnavailableReason::kEnterprisePolicy);
  }
  if (c.moltnet || !c.is_document || !c.http_top_level || !c.user_enabled ||
      !d.web_apis_enabled) {
    return unavailable(UnavailableReason::kDisabled);
  }
  if (!input.options_supported) {
    return unavailable(UnavailableReason::kUnsupportedOptions);
  }
  if (!input.languages_supported) {
    return unavailable(UnavailableReason::kUnsupportedLanguage);
  }
  if (input.consent == SiteConsent::kBlocked) {
    return unavailable(UnavailableReason::kBlockedByUser);
  }
  if (!d.inference_supported || (!d.model_installed && !d.some_model_fits)) {
    return unavailable(UnavailableReason::kDeviceNotEligible);
  }
  // Audio input: the same answer for every user of this build when its
  // catalogue has no model that hears audio; then, only to a site the user
  // allowed, whether one is downloaded.
  if (input.wants_audio &&
      (!d.audio_model_offered ||
       (input.consent == SiteConsent::kAllowed && !d.audio_model_installed))) {
    return unavailable(UnavailableReason::kNoAudioModel);
  }
  // From here on the device can run a model. A site the user hasn't allowed
  // learns nothing more: "downloadable" whether or not a model is
  // installed. Blink asks for a user gesture before create() in that state,
  // so a page can't raise the prompt by itself.
  if (input.consent != SiteConsent::kAllowed || !d.model_installed) {
    return {Availability::kDownloadable, UnavailableReason::kNone};
  }
  return {Availability::kAvailable, UnavailableReason::kNone};
}

SiteDeviceFacts::SiteDeviceFacts(int64_t lifetime_us)
    : lifetime_us_(lifetime_us) {}

SiteDeviceFacts::~SiteDeviceFacts() = default;

SiteDeviceFacts::Lookup SiteDeviceFacts::Find(const std::string& site,
                                              int64_t now_us) {
  Prune(now_us);
  Entry& entry = sites_[site];
  Lookup lookup;
  if (entry.pending != 0) {
    lookup.kind = Lookup::Kind::kJoin;
    lookup.ticket = entry.pending;
    return lookup;
  }
  if (entry.facts && now_us - entry.read_at_us < lifetime_us_) {
    lookup.kind = Lookup::Kind::kCached;
    lookup.facts = *entry.facts;
    return lookup;
  }
  entry.pending = next_ticket_++;
  lookup.kind = Lookup::Kind::kRead;
  lookup.ticket = entry.pending;
  return lookup;
}

bool SiteDeviceFacts::Finish(const std::string& site,
                             uint64_t ticket,
                             const DeviceFacts& facts,
                             int64_t now_us) {
  auto it = sites_.find(site);
  if (ticket == 0 || it == sites_.end() || it->second.pending != ticket) {
    return false;
  }
  it->second.pending = 0;
  it->second.facts = facts;
  it->second.read_at_us = now_us;
  return true;
}

void SiteDeviceFacts::Clear() {
  sites_.clear();
}

void SiteDeviceFacts::Prune(int64_t now_us) {
  for (auto it = sites_.begin(); it != sites_.end();) {
    const Entry& entry = it->second;
    const bool in_use =
        entry.pending != 0 ||
        (entry.facts && now_us - entry.read_at_us < lifetime_us_);
    if (in_use) {
      ++it;
    } else {
      it = sites_.erase(it);
    }
  }
}

CreateAction DecideCreateAction(const AvailabilityInput& input) {
  if (DecideAvailability(input).availability == Availability::kUnavailable) {
    return CreateAction::kReject;
  }
  if (input.consent != SiteConsent::kAllowed) {
    return CreateAction::kAskUser;
  }
  if (!input.device.model_installed) {
    return CreateAction::kNeedsModel;
  }
  return CreateAction::kCreate;
}

// ---------------------------------------------------------------------------
// Sizes and counting
// ---------------------------------------------------------------------------

uint32_t AudioInputTokens(uint32_t seconds) {
  // At most kMaxAudioSeconds in practice; saturating anyway.
  const uint64_t tokens = uint64_t{seconds} * kAudioInputTokensPerSecond;
  return std::max(kAudioInputMinTokens, Saturate(tokens));
}

LanguageModelLimits LanguageModelLimitsFor(uint32_t context_tokens) {
  const uint64_t large_needs = static_cast<uint64_t>(kLargeInputQuota) +
                               kLargeMaxReplyTokens + kTemplateOverheadTokens;
  if (context_tokens >= large_needs) {
    return {kLargeInputQuota, kLargeMaxReplyTokens};
  }
  return {kSmallInputQuota, kSmallMaxReplyTokens};
}

uint32_t EstimateTokens(std::string_view text) {
  // Thirds of a token: 1 per ASCII byte, 4 per non-ASCII character (its lead
  // byte; continuation bytes 10xxxxxx add nothing).
  uint64_t thirds = 0;
  for (char ch : text) {
    const unsigned char b = static_cast<unsigned char>(ch);
    if (b < 0x80) {
      thirds += 1;
    } else if ((b & 0xC0) != 0x80) {
      thirds += 4;
    }
  }
  return Saturate((thirds + 2) / 3);
}

uint32_t EstimateMessageTokens(std::string_view text) {
  return AddTokens(EstimateTokens(text), kPerMessageTokens);
}

uint32_t AddTokens(uint32_t a, uint32_t b) {
  return Saturate(static_cast<uint64_t>(a) + b);
}

bool ExceedsQuotaBySize(size_t bytes, uint32_t quota) {
  // EstimateTokens() >= ceil(bytes / 3) for valid UTF-8 (an ASCII byte is a
  // third of a token; a 2-, 3- or 4-byte character 4/3, i.e. at least a
  // third per byte).
  return static_cast<uint64_t>(bytes) > 3 * static_cast<uint64_t>(quota);
}

uint32_t TokensForSize(size_t bytes) {
  const uint64_t b = bytes;
  return Saturate(b / 3 + (b % 3 != 0 ? 1 : 0));
}

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

Sampling SamplingForMode(SamplingMode mode) {
  // The same points upstream uses for these modes (chrome/browser/ai/
  // ai_manager.cc at 156).
  switch (mode) {
    case SamplingMode::kMostPredictable:
      return {1, 0.0f};
    case SamplingMode::kPredictable:
      return {32, 0.3f};
    case SamplingMode::kSlightlyPredictable:
      return {64, 0.7f};
    case SamplingMode::kBalanced:
      return {64, 1.0f};
    case SamplingMode::kSlightlyCreative:
      return {72, 1.1f};
    case SamplingMode::kCreative:
      return {80, 1.15f};
    case SamplingMode::kMostCreative:
      return {100, 1.2f};
  }
  return kDefaultSampling;
}

Sampling ClampSampling(uint32_t top_k, float temperature) {
  Sampling s;
  s.top_k = std::clamp<uint32_t>(top_k, 1u, kMaxSampling.top_k);
  if (std::isnan(temperature)) {
    s.temperature = kDefaultSampling.temperature;
  } else {
    s.temperature =
        std::clamp(temperature, 0.0f, kMaxSampling.temperature);
  }
  return s;
}

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

bool IsValidLanguageTag(std::string_view tag) {
  if (tag.empty() || tag.size() > 35) {
    return false;
  }
  size_t i = 0;
  // Primary subtag: 2-8 letters.
  size_t start = i;
  while (i < tag.size() && tag[i] != '-') {
    if (!IsAsciiAlpha(tag[i])) {
      return false;
    }
    ++i;
  }
  if (i - start < 2 || i - start > 8) {
    return false;
  }
  // Further subtags: 1-8 letters or digits.
  while (i < tag.size()) {
    ++i;  // the '-'
    start = i;
    while (i < tag.size() && tag[i] != '-') {
      if (!IsAsciiAlpha(tag[i]) && !IsAsciiDigit(tag[i])) {
        return false;
      }
      ++i;
    }
    if (i - start < 1 || i - start > 8) {
      return false;
    }
  }
  return true;
}

std::string BaseLanguage(std::string_view tag) {
  std::string base;
  for (char c : tag) {
    if (c == '-') {
      break;
    }
    base.push_back(ToLowerAscii(c));
  }
  return base;
}

std::string LanguageNameForPrompt(std::string_view tag) {
  if (!IsValidLanguageTag(tag)) {
    return std::string();
  }
  const std::string base = BaseLanguage(tag);
  for (const LanguageName& entry : kLanguageNames) {
    if (base == entry.code) {
      return entry.name;
    }
  }
  return "the language with the BCP 47 tag " + std::string(tag);
}

// ---------------------------------------------------------------------------
// Which model serves pages
// ---------------------------------------------------------------------------

namespace {

// The default if it is downloaded, else the smallest downloaded model
// (by RAM, then id); among those that see images only if |need_images|, and
// that hear audio only if |need_audio|.
const ModelCandidate* PickAmong(std::string_view default_model_id,
                                const std::vector<ModelCandidate>& models,
                                bool need_images,
                                bool need_audio) {
  const ModelCandidate* smallest = nullptr;
  for (const ModelCandidate& m : models) {
    if (!m.downloaded || m.model_id.empty() ||
        (need_images && !m.sees_images) || (need_audio && !m.hears_audio)) {
      continue;
    }
    if (m.model_id == default_model_id) {
      return &m;
    }
    if (!smallest || m.ram_required_bytes < smallest->ram_required_bytes ||
        (m.ram_required_bytes == smallest->ram_required_bytes &&
         m.model_id < smallest->model_id)) {
      smallest = &m;
    }
  }
  return smallest;
}

}  // namespace

std::string PickWebModel(std::string_view loaded_model_id,
                         std::string_view default_model_id,
                         const std::vector<ModelCandidate>& models,
                         bool wants_images,
                         bool wants_audio) {
  if (!loaded_model_id.empty()) {
    return std::string(loaded_model_id);
  }
  const ModelCandidate* pick = nullptr;
  if (wants_images || wants_audio) {
    pick = PickAmong(default_model_id, models, wants_images, wants_audio);
  }
  if (!pick) {
    pick = PickAmong(default_model_id, models, /*need_images=*/false,
                     /*need_audio=*/false);
  }
  return pick ? pick->model_id : std::string();
}

bool AnyModelInstalled(const std::vector<ModelCandidate>& models) {
  return std::any_of(models.begin(), models.end(),
                     [](const ModelCandidate& m) { return m.downloaded; });
}

bool AnyModelHearsAudio(const std::vector<ModelCandidate>& models) {
  return std::any_of(models.begin(), models.end(),
                     [](const ModelCandidate& m) { return m.hears_audio; });
}

bool AnyInstalledModelHearsAudio(const std::vector<ModelCandidate>& models) {
  return std::any_of(models.begin(), models.end(), [](const ModelCandidate& m) {
    return m.downloaded && m.hears_audio;
  });
}

bool AnyModelFits(const std::vector<ModelCandidate>& models,
                  uint64_t total_ram_bytes) {
  return std::any_of(models.begin(), models.end(),
                     [total_ram_bytes](const ModelCandidate& m) {
                       return m.ram_required_bytes > 0 &&
                              m.ram_required_bytes <= total_ram_bytes;
                     });
}

// ---------------------------------------------------------------------------
// GenerationScheduler
// ---------------------------------------------------------------------------

GenerationScheduler::GenerationScheduler() = default;
GenerationScheduler::~GenerationScheduler() = default;

GenerationScheduler::Admission GenerationScheduler::Enqueue(
    uint64_t id,
    const std::string& site,
    bool foreground) {
  if (pending_total() >= kMaxPendingTotal) {
    return Admission::kQueueFull;
  }
  if (PendingForSite(site) >= kMaxPendingPerSite) {
    return Admission::kSiteBusy;
  }
  (foreground ? foreground_ : background_).push_back({id, site});
  ++pending_by_site_[site];
  return Admission::kAdmitted;
}

uint64_t GenerationScheduler::LastStart(const std::string& site) const {
  auto it = last_start_by_site_.find(site);
  return it == last_start_by_site_.end() ? 0 : it->second;
}

uint64_t GenerationScheduler::StartNext() {
  if (running_ != 0) {
    return 0;
  }
  std::deque<Entry>* queue = !foreground_.empty()   ? &foreground_
                             : !background_.empty() ? &background_
                                                    : nullptr;
  if (!queue) {
    return 0;
  }
  // The oldest request of the site that started a generation least
  // recently (strictly less: FIFO among equals).
  auto next = queue->begin();
  for (auto it = queue->begin(); it != queue->end(); ++it) {
    if (LastStart(it->site) < LastStart(next->site)) {
      next = it;
    }
  }
  running_ = next->id;
  running_site_ = std::move(next->site);
  queue->erase(next);
  last_start_by_site_[running_site_] = ++starts_;
  return running_;
}

void GenerationScheduler::Finish(uint64_t id) {
  if (id == 0) {
    return;
  }
  if (running_ == id) {
    running_ = 0;
    std::string site = std::move(running_site_);
    running_site_.clear();
    Release(site);
    return;
  }
  for (std::deque<Entry>* queue : {&foreground_, &background_}) {
    for (auto it = queue->begin(); it != queue->end(); ++it) {
      if (it->id == id) {
        std::string site = std::move(it->site);
        queue->erase(it);
        Release(site);
        return;
      }
    }
  }
}

bool GenerationScheduler::IsQueued(uint64_t id) const {
  for (const std::deque<Entry>* queue : {&foreground_, &background_}) {
    for (const Entry& e : *queue) {
      if (e.id == id) {
        return true;
      }
    }
  }
  return false;
}

size_t GenerationScheduler::PendingForSite(const std::string& site) const {
  auto it = pending_by_site_.find(site);
  return it == pending_by_site_.end() ? 0 : it->second;
}

size_t GenerationScheduler::pending_total() const {
  return foreground_.size() + background_.size() + (running_ != 0 ? 1 : 0);
}

void GenerationScheduler::Release(const std::string& site) {
  auto it = pending_by_site_.find(site);
  if (it == pending_by_site_.end()) {
    return;
  }
  if (--it->second == 0) {
    pending_by_site_.erase(it);
    // Nothing left for this site: it doesn't need its turn order any more.
    last_start_by_site_.erase(site);
  }
}

bool GenerationScheduler::TryAddSession(const std::string& site) {
  size_t& count = sessions_by_site_[site];
  if (count >= kMaxSessionsPerSite) {
    return false;
  }
  ++count;
  return true;
}

void GenerationScheduler::RemoveSession(const std::string& site) {
  auto it = sessions_by_site_.find(site);
  if (it == sessions_by_site_.end()) {
    return;
  }
  if (it->second <= 1) {
    sessions_by_site_.erase(it);
  } else {
    --it->second;
  }
}

size_t GenerationScheduler::SessionsForSite(const std::string& site) const {
  auto it = sessions_by_site_.find(site);
  return it == sessions_by_site_.end() ? 0 : it->second;
}

bool GenerationScheduler::TryHoldImages(const std::string& site,
                                        size_t count) {
  if (count == 0) {
    return true;
  }
  const size_t held = ImagesHeldForSite(site);
  // Written so that no sum can wrap.
  if (count > kMaxHeldImagesPerSite - held ||
      count > kMaxHeldImagesTotal - images_held_total_) {
    return false;
  }
  images_by_site_[site] = held + count;
  images_held_total_ += count;
  return true;
}

void GenerationScheduler::ReleaseImages(const std::string& site,
                                        size_t count) {
  auto it = images_by_site_.find(site);
  if (it == images_by_site_.end() || count == 0) {
    return;
  }
  const size_t released = std::min(count, it->second);
  images_held_total_ -= released;
  if (released == it->second) {
    images_by_site_.erase(it);
  } else {
    it->second -= released;
  }
}

size_t GenerationScheduler::ImagesHeldForSite(const std::string& site) const {
  auto it = images_by_site_.find(site);
  return it == images_by_site_.end() ? 0 : it->second;
}

}  // namespace molt_ai::web_ai
