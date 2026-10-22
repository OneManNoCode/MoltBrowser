// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A LanguageModel session's context and its quota accounting (Prompt API:
// inputQuota / inputUsage, contextoverflow, QuotaExceededError). Counts come
// from EstimateTokens(), never the model's tokenizer. Standard library only,
// tested in molt_ai_unittests.
//
// Semantics:
//  * The page's system prompt counts against the quota and is never evicted.
//  * Adding turns that, with the system prompt, exceed the quota fails
//    (QuotaExceededError with the requested and available counts).
//  * Otherwise the oldest turns are evicted until everything fits (whole
//    exchanges: the kept history starts with a user turn), and the page
//    gets a contextoverflow event.
//  * A prompt and its reply join the context only when the reply completes;
//    a failed, aborted or cancelled prompt leaves the context unchanged.
//  * Images (image input) go with user turns and count kImageInputTokens
//    each. The model sees at most kMaxContextImages per request, the
//    newest, so the context keeps the pixels of the newest ones only; an
//    older image still counts against the quota (it was part of the turn)
//    until its turn is evicted.

#ifndef CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONVERSATION_H_
#define CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONVERSATION_H_

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "chrome/browser/molt_ai/runtime/model_image.h"

namespace molt_ai::web_ai {

enum class Role { kUser, kAssistant };

// The most images a request shows the model (the inference service's
// kMaxImagesPerRequest), and so the most a context keeps the pixels of.
inline constexpr size_t kMaxContextImages = 4;

struct Turn {
  Turn();
  Turn(Turn&&);
  Turn& operator=(Turn&&);
  Turn(const Turn&);
  Turn& operator=(const Turn&);
  ~Turn();

  Role role = Role::kUser;
  std::string text;
  // A user turn's images, in the page's order (shown to the model before
  // the text). May be emptied when newer images replace them (see above).
  std::vector<ModelImage> images;
  // EstimateMessageTokens(text) plus kImageInputTokens per image the turn
  // was made with.
  uint32_t tokens = 0;
};

// A turn of |text| that will hold |image_count| images (counted now; the
// pixels are added to Turn::images once they are ready).
Turn MakeTurn(Role role, std::string text, size_t image_count = 0);

class Conversation {
 public:
  explicit Conversation(uint32_t input_quota);
  Conversation(const Conversation&);
  Conversation& operator=(const Conversation&);
  ~Conversation();

  // The page's system prompt. False (and nothing set) if it alone exceeds
  // the quota.
  bool SetSystemPrompt(std::string text);
  const std::string& system_prompt() const { return system_prompt_; }
  uint32_t system_tokens() const { return system_tokens_; }

  uint32_t input_quota() const { return input_quota_; }
  uint32_t input_usage() const;
  const std::deque<Turn>& turns() const { return turns_; }
  // The images whose pixels the context keeps (at most kMaxContextImages).
  size_t image_count() const;

  struct Plan {
    // False: the system prompt and the new turns alone exceed the quota.
    bool fits = false;
    // System prompt + new turns (QuotaErrorInfo.requested).
    uint32_t requested = 0;
    // Oldest turns to evict first.
    size_t evict = 0;
  };
  Plan PlanAdd(const std::vector<Turn>& added) const;

  // Commits |added|, evicting |plan.evict| turns first, then lets go of the
  // pixels of all but the newest kMaxContextImages images. |plan| must come
  // from PlanAdd(added) on the current state and fit.
  void Add(const Plan& plan, std::vector<Turn> added);

  // Adds the model's reply, evicting the oldest turns (never the reply) if
  // the context would exceed the quota. True if anything was evicted.
  bool AddReply(std::string text);

  // What the runtime gets for the context as it would be after
  // Add(plan, added): runs of turns with the same role merged (chat
  // templates expect alternating roles; their images joined in order), and
  // the trailing user turn as the new message. Only the newest
  // kMaxContextImages images are in it (copies).
  struct Shape {
    Shape();
    Shape(Shape&&);
    Shape& operator=(Shape&&);
    ~Shape();

    std::vector<std::pair<Role, std::string>> history;
    // Each history entry's images: empty, or one entry per history entry.
    std::vector<std::vector<ModelImage>> history_images;
    std::string user_message;
    std::vector<ModelImage> user_images;
  };
  Shape ShapeAfter(const Plan& plan, const std::vector<Turn>& added) const;

 private:
  // Lets go of the pixels of all but the newest kMaxContextImages images.
  void KeepNewestImages();

  uint32_t input_quota_;
  std::string system_prompt_;
  uint32_t system_tokens_ = 0;
  std::deque<Turn> turns_;
  uint32_t turns_tokens_ = 0;
};

}  // namespace molt_ai::web_ai

#endif  // CHROME_BROWSER_MOLT_AI_WEB_AI_WEB_AI_CONVERSATION_H_
