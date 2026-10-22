// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// The decisions behind the built-in AI APIs MoltBrowser gives web pages
// (LanguageModel, Summarizer, Writer, Rewriter, Translator), backed by the
// user's own on-device model: what availability() reports, what create()
// does, how big a session may be, how page text is counted, which model
// serves pages, and the per-site limits. Standard library only, so molt_ai_unittests tests it
// without a browser. Design: docs/design/WEB_AI_APIS.md.
//
// Fingerprinting: everything a page can read back from here is a small,
// uniform set of values that doesn't depend on which model the user has.
//  * availability(): only "unavailable", "downloadable" or "available";
//    never "downloading". A site the user hasn't allowed sees
//    "downloadable" (or "unavailable" on a device that can't run any
//    model), whether or not a model is installed.
//  * inputQuota: one of two buckets (kSmallInputQuota, kLargeInputQuota).
//  * Token counts: EstimateTokens(), from the text alone, never the model's
//    tokenizer (whose counts would name the model); an image counts
//    kImageInputTokens, whatever its size.
//  * Image input: availability() answers the same with or without it; only
//    create(), on a site the user allowed, can fail because the model in
//    use doesn't see images.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_POLICY_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace molt_ai::web_ai {

// ---------------------------------------------------------------------------
// Availability and create()
// ---------------------------------------------------------------------------

// What availability() reports. "downloading" is never reported: it would
// tell a site when the user is downloading a model.
enum class Availability {
  kUnavailable,
  kDownloadable,
  kAvailable,
};

// The user's answer for a site (the top-level origin), kept per profile
// (web_ai_site_permissions.h). kAsk: not asked yet, or the prompt was
// closed without an answer.
enum class SiteConsent {
  kAsk,
  kAllowed,
  kBlocked,
};

// Why an API is unavailable. Only shown to developers (the renderer turns it
// into a console message, which page script can't read).
enum class UnavailableReason {
  kNone,
  // The API's feature is off (chrome://flags), the context can't use it
  // (MoltNet window, worker, non-http(s) page) or the user turned it off.
  kDisabled,
  // BuiltInAIAPIsEnabled or GenAILocalFoundationalModelSettings policy.
  kEnterprisePolicy,
  // Audio input, tools, or other options the on-device models can't serve
  // (image input with the multimodal-input feature off).
  kUnsupportedOptions,
  // A language tag that isn't a well-formed BCP 47 tag.
  kUnsupportedLanguage,
  // The user blocked the site.
  kBlockedByUser,
  // This device can't run any on-device model (CPU, platform, RAM).
  kDeviceNotEligible,
};

// Facts about the document or worker asking.
struct ContextFacts {
  // The API's base::Feature (kAIPromptAPI, kAISummarizationAPI, ...).
  bool feature_enabled = false;
  // BuiltInAIAPIsEnabled and GenAILocalFoundationalModelSettings.
  bool enterprise_allowed = false;
  // chrome://settings "On-device AI" (Chromium's toggle).
  bool user_enabled = false;
  // A MoltNet (Tor) window: never. Tor Browser has no such API, and a local
  // model's answers and speed would fingerprint the machine.
  bool moltnet = false;
  // Documents only. Workers have no frame to ask the user in, no user
  // activation and no visibility, so they report "unavailable".
  bool is_document = false;
  // The top-level document is http(s) (not an extension, file:// or an
  // internal page).
  bool http_top_level = false;
  // LanguageModel image input: Blink's kAIPromptAPIMultimodalInput (as
  // upstream's AIManager checks it). The same for every user with the same
  // flags; whether a model can see images is never part of availability().
  bool image_input_enabled = false;
};

// Facts about this device, from the on-device runtime and settings.json.
struct DeviceFacts {
  // The CPU and platform can run local inference at all.
  bool inference_supported = false;
  // At least one model is downloaded.
  bool model_installed = false;
  // At least one catalogue model fits this device's RAM.
  bool some_model_fits = false;
  // settings.json "web_ai_apis" (default true): the user's switch for
  // these APIs.
  bool web_apis_enabled = false;
};

// The context may use the APIs at all (device and consent aside).
// LanguageModel.params() answers only then.
bool IsContextEligible(const ContextFacts& context);

struct AvailabilityInput {
  ContextFacts context;
  DeviceFacts device;
  // The request's options are something a text model can serve.
  bool options_supported = true;
  // Every language tag in the options is well formed.
  bool languages_supported = true;
  SiteConsent consent = SiteConsent::kAsk;
};

struct AvailabilityDecision {
  Availability availability = Availability::kUnavailable;
  UnavailableReason reason = UnavailableReason::kDisabled;
};

AvailabilityDecision DecideAvailability(const AvailabilityInput& input);

// What create() does.
enum class CreateAction {
  // Unavailable: fail.
  kReject,
  // Ask the user (an infobar), then decide again with the answer.
  kAskUser,
  // Allowed, but no model is downloaded: offer the model picker and fail.
  kNeedsModel,
  // Load the model and create the session.
  kCreate,
};

CreateAction DecideCreateAction(const AvailabilityInput& input);

// ---------------------------------------------------------------------------
// Sizes
// ---------------------------------------------------------------------------

// LanguageModel inputQuota buckets. A session gets the large one when the
// loaded model's context window holds it plus the largest reply and the chat
// template's overhead; otherwise the small one. The exact context window is
// never exposed: it depends on the model and the RAM tier.
inline constexpr uint32_t kSmallInputQuota = 1024;
inline constexpr uint32_t kLargeInputQuota = 4096;
inline constexpr uint32_t kSmallMaxReplyTokens = 512;
inline constexpr uint32_t kLargeMaxReplyTokens = 1024;
inline constexpr uint32_t kTemplateOverheadTokens = 256;

// Summarizer, Writer and Rewriter: Blink reports a fixed inputQuota of
// blink::mojom::kWritingAssistanceMaxInputTokenSize to every page; the
// browser enforces the same number with EstimateTokens().
inline constexpr uint32_t kWritingAssistanceInputQuota = 9216;

struct LanguageModelLimits {
  uint32_t input_quota = kSmallInputQuota;
  uint32_t max_reply_tokens = kSmallMaxReplyTokens;
};

// |context_tokens|: the loaded model's context window (0 if unknown).
LanguageModelLimits LanguageModelLimitsFor(uint32_t context_tokens);

// ---------------------------------------------------------------------------
// Image input (LanguageModel expectedInputs "image")
// ---------------------------------------------------------------------------

// What one image counts against inputQuota, whatever its size and whatever
// the model: a page's images are scaled to at most 768 pixels a side
// (runtime/model_image.h kWebImageMaxSide), which a vision encoder with a
// 32-pixel token grid turns into at most 576 tokens.
inline constexpr uint32_t kImageInputTokens = 576;

// The largest bitmap a page may send (64 megapixels, beyond any camera
// photo): scaling work grows with it, so bigger ones are refused unread.
inline constexpr uint64_t kMaxImageSourcePixels = uint64_t{1} << 26;

// A page's bitmap of |width| x |height| may be scaled for the model: both
// sides positive and at most kMaxImageSourcePixels pixels.
bool IsUsableImageSource(int64_t width, int64_t height);

// Model images held in the browser for pages (those of requests not
// answered yet, and those kept in sessions' contexts), per site and in
// total: at most 1.7 MB each (768 x 768 RGB), so about 82 MB for every page
// together. A request over either cap fails.
inline constexpr size_t kMaxHeldImagesPerSite = 16;
inline constexpr size_t kMaxHeldImagesTotal = 48;

// ---------------------------------------------------------------------------
// Counting page text
// ---------------------------------------------------------------------------

// A token count for |text| that depends only on the text: a third of a token
// per ASCII byte and 4/3 per non-ASCII character, rounded up. Real
// tokenizers average about four ASCII characters per token, so this
// overestimates English (the quota is safe) and is near one token per
// character for CJK. Linear time. For valid UTF-8 it is never below
// text.size() / 3 (see ExceedsQuotaBySize()).
uint32_t EstimateTokens(std::string_view text);

// Per message (role marker, separators), added by EstimateMessageTokens().
inline constexpr uint32_t kPerMessageTokens = 4;
uint32_t EstimateMessageTokens(std::string_view text);

// Saturating addition for token counts.
uint32_t AddTokens(uint32_t a, uint32_t b);

// True when |bytes| of text certainly estimate to more than |quota| tokens,
// without reading it (EstimateTokens() >= bytes / 3 for valid UTF-8). Lets
// the browser refuse a huge string in constant time.
bool ExceedsQuotaBySize(size_t bytes, uint32_t quota);

// The token count reported for text refused by ExceedsQuotaBySize().
uint32_t TokensForSize(size_t bytes);

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------

struct Sampling {
  uint32_t top_k = 40;
  float temperature = 0.7f;
};

// What LanguageModel.params() reports: the same for every user and model.
inline constexpr Sampling kDefaultSampling{40, 0.7f};
inline constexpr Sampling kMaxSampling{128, 2.0f};

// LanguageModel samplingMode (blink::mojom::AILanguageModelSamplingMode).
enum class SamplingMode {
  kMostPredictable,
  kPredictable,
  kBalanced,
  kCreative,
  kMostCreative,
};

Sampling SamplingForMode(SamplingMode mode);

// Clamps page-chosen values into [1, kMaxSampling.top_k] and
// [0, kMaxSampling.temperature]; NaN becomes the default.
Sampling ClampSampling(uint32_t top_k, float temperature);

// ---------------------------------------------------------------------------
// Languages
// ---------------------------------------------------------------------------

// A well-formed BCP 47 tag as Blink sends it (canonicalized): 2-8 letters,
// then "-"-separated subtags of 1-8 letters or digits, at most 35 bytes.
// Checked with a linear scanner (no std::regex on page input). Any
// well-formed tag is accepted whatever the model knows: the answer must not
// depend on the model.
bool IsValidLanguageTag(std::string_view tag);

// An English name for |tag| for the model's instructions ("Spanish"), or a
// description naming the tag when it isn't in the table. |tag| must pass
// IsValidLanguageTag(): the result goes into a browser-authored prompt.
std::string LanguageNameForPrompt(std::string_view tag);

// The primary language subtag, lower-cased ("pt-BR" -> "pt").
std::string BaseLanguage(std::string_view tag);

// ---------------------------------------------------------------------------
// Which model serves pages
// ---------------------------------------------------------------------------

struct ModelCandidate {
  std::string model_id;
  bool downloaded = false;
  uint64_t ram_required_bytes = 0;
  // It has a vision projector (image input).
  bool sees_images = false;
};

// The model a page's request runs on:
//  1. the model already loaded (the chat's), so a page never swaps it, even
//     for images it can't see;
//  2. else settings.json's default_model, if downloaded (and, when
//     |wants_images|, if it sees images);
//  3. else, when |wants_images|, the smallest downloaded model that sees
//     images;
//  4. else the default, then the smallest downloaded model (by RAM, then
//     id).
// Empty if nothing is downloaded.
std::string PickWebModel(std::string_view loaded_model_id,
                         std::string_view default_model_id,
                         const std::vector<ModelCandidate>& models,
                         bool wants_images = false);

bool AnyModelInstalled(const std::vector<ModelCandidate>& models);

// Some catalogue model needs no more than this device's RAM.
bool AnyModelFits(const std::vector<ModelCandidate>& models,
                  uint64_t total_ram_bytes);

// ---------------------------------------------------------------------------
// Limits
// ---------------------------------------------------------------------------

// Live sessions (all four APIs) per site and profile.
inline constexpr size_t kMaxSessionsPerSite = 16;
// Generations queued or running per site and profile (a session has at most
// one in the backend at a time, so this matches the session cap).
inline constexpr size_t kMaxPendingPerSite = kMaxSessionsPerSite;
// Generations queued or running for all pages together.
inline constexpr size_t kMaxPendingTotal = 64;
// Requests waiting behind the running one, per session (e.g. a page
// summarizing a list with Promise.all).
inline constexpr size_t kMaxQueuedPerSession = 32;
// create() calls of one document (or worker) not answered yet: waiting for
// device facts, the user's answer or the model. More than a site could
// have sessions can't all succeed; the rest fail at once.
inline constexpr size_t kMaxPendingCreates = kMaxSessionsPerSite;
// Hidden reasoning a page's request may use (reasoning is asked off).
inline constexpr uint32_t kMaxReasoningTokens = 256;
// Streamed text reaches the page at most this often (milliseconds): fewer,
// coarser timing samples of the model's speed.
inline constexpr int kStreamIntervalMs = 200;

// The per-site and global admission rules, and which request runs next. One
// web generation runs at a time, so the user's own chat (which talks to the
// runtime directly) waits behind at most one page request. Requests from
// visible pages go first; within that, sites take turns (the site that
// started a generation least recently goes next), FIFO within a site. Not
// thread-safe (UI thread).
class GenerationScheduler {
 public:
  enum class Admission {
    kAdmitted,
    kSiteBusy,
    kQueueFull,
  };

  GenerationScheduler();
  ~GenerationScheduler();
  GenerationScheduler(const GenerationScheduler&) = delete;
  GenerationScheduler& operator=(const GenerationScheduler&) = delete;

  // |id| must be new and non-zero.
  Admission Enqueue(uint64_t id, const std::string& site, bool foreground);
  // The next id to run, or 0 when one is running or none is queued. The
  // returned id counts as running until Finish().
  uint64_t StartNext();
  // A running or queued request ended (done or cancelled). Unknown ids are
  // ignored.
  void Finish(uint64_t id);

  bool IsRunning(uint64_t id) const { return id != 0 && running_ == id; }
  bool IsQueued(uint64_t id) const;
  size_t PendingForSite(const std::string& site) const;
  size_t pending_total() const;

  // Session slots per site.
  bool TryAddSession(const std::string& site);
  void RemoveSession(const std::string& site);
  size_t SessionsForSite(const std::string& site) const;

  // Model images held for pages: false (and nothing held) if |count| more
  // would exceed kMaxHeldImagesPerSite or kMaxHeldImagesTotal. Releasing
  // more than |site| holds releases what it holds.
  bool TryHoldImages(const std::string& site, size_t count);
  void ReleaseImages(const std::string& site, size_t count);
  size_t ImagesHeldForSite(const std::string& site) const;
  size_t images_held_total() const { return images_held_total_; }

 private:
  struct Entry {
    uint64_t id;
    std::string site;
  };
  void Release(const std::string& site);

  // When |site| last started a generation (0: not while it had requests).
  uint64_t LastStart(const std::string& site) const;

  std::deque<Entry> foreground_;
  std::deque<Entry> background_;
  uint64_t running_ = 0;
  std::string running_site_;
  std::map<std::string, size_t> pending_by_site_;
  std::map<std::string, uint64_t> last_start_by_site_;
  uint64_t starts_ = 0;
  std::map<std::string, size_t> sessions_by_site_;
  std::map<std::string, size_t> images_by_site_;
  size_t images_held_total_ = 0;
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_POLICY_H_
