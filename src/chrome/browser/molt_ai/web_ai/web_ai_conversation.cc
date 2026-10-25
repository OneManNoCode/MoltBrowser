// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"

#include <algorithm>
#include <limits>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/services/molt_ai/inference/engine/audio_input.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"

namespace molt_ai::web_ai {

static_assert(kMaxContextImages == kMaxImagesPerRequest,
              "the context keeps what one request can show the model");
static_assert(kMaxContextAudio == kMaxAudioPerRequest,
              "the context keeps what one request can give the model");

namespace {

// Lets go of all but the newest |max| items of |media| over |turns|; a
// turn's newest are its last ones.
template <typename T>
void KeepNewest(std::deque<Turn>& turns,
                std::vector<T> Turn::*media,
                size_t max) {
  size_t kept = 0;
  for (size_t i = turns.size(); i-- > 0;) {
    std::vector<T>& items = turns[i].*media;
    if (kept + items.size() <= max) {
      kept += items.size();
      continue;
    }
    const size_t keep = max - kept;
    items.erase(items.begin(), items.end() - static_cast<std::ptrdiff_t>(keep));
    kept = max;
  }
}

// How many of each turn's |media| a request sends: the newest |max|,
// counted from the end.
template <typename T>
std::vector<size_t> NewestCounts(const std::vector<const Turn*>& turns,
                                 std::vector<T> Turn::*media,
                                 size_t max) {
  std::vector<size_t> sent(turns.size(), 0);
  size_t room = max;
  for (size_t i = turns.size(); i-- > 0 && room > 0;) {
    sent[i] = std::min(room, (turns[i]->*media).size());
    room -= sent[i];
  }
  return sent;
}

}  // namespace

Turn::Turn() = default;
Turn::Turn(Turn&&) = default;
Turn& Turn::operator=(Turn&&) = default;
Turn::Turn(const Turn&) = default;
Turn& Turn::operator=(const Turn&) = default;
Turn::~Turn() = default;

Conversation::Shape::Shape() = default;
Conversation::Shape::Shape(Shape&&) = default;
Conversation::Shape& Conversation::Shape::operator=(Shape&&) = default;
Conversation::Shape::~Shape() = default;

Turn MakeTurn(Role role,
              std::string text,
              size_t image_count,
              std::vector<ModelAudio> audio) {
  Turn turn;
  turn.role = role;
  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  const uint32_t image_tokens =
      image_count > kMax / kImageInputTokens
          ? kMax
          : static_cast<uint32_t>(image_count) * kImageInputTokens;
  turn.tokens = AddTokens(EstimateMessageTokens(text), image_tokens);
  for (const ModelAudio& clip : audio) {
    turn.tokens = AddTokens(turn.tokens, AudioInputTokens(clip.Seconds()));
  }
  turn.text = std::move(text);
  turn.audio = std::move(audio);
  return turn;
}

Conversation::Conversation(uint32_t input_quota) : input_quota_(input_quota) {}
Conversation::Conversation(const Conversation&) = default;
Conversation& Conversation::operator=(const Conversation&) = default;
Conversation::~Conversation() = default;

bool Conversation::SetSystemPrompt(std::string text) {
  const uint32_t tokens = EstimateMessageTokens(text);
  if (AddTokens(tokens, turns_tokens_) > input_quota_) {
    return false;
  }
  system_tokens_ = tokens;
  system_prompt_ = std::move(text);
  return true;
}

uint32_t Conversation::input_usage() const {
  return AddTokens(system_tokens_, turns_tokens_);
}

size_t Conversation::image_count() const {
  size_t count = 0;
  for (const Turn& t : turns_) {
    count += t.images.size();
  }
  return count;
}

size_t Conversation::audio_count() const {
  size_t count = 0;
  for (const Turn& t : turns_) {
    count += t.audio.size();
  }
  return count;
}

void Conversation::KeepNewestMedia() {
  KeepNewest(turns_, &Turn::images, kMaxContextImages);
  KeepNewest(turns_, &Turn::audio, kMaxContextAudio);
}

Conversation::Plan Conversation::PlanAdd(
    const std::vector<Turn>& added) const {
  Plan plan;
  uint32_t added_tokens = 0;
  for (const Turn& t : added) {
    added_tokens = AddTokens(added_tokens, t.tokens);
  }
  plan.requested = AddTokens(system_tokens_, added_tokens);
  if (plan.requested > input_quota_) {
    return plan;
  }
  plan.fits = true;
  uint32_t kept = turns_tokens_;
  while (AddTokens(plan.requested, kept) > input_quota_ &&
         plan.evict < turns_.size()) {
    kept -= turns_[plan.evict].tokens;
    ++plan.evict;
  }
  // Once anything goes, whole exchanges go: the history the model sees
  // starts with a user turn (chat templates expect that).
  while (plan.evict > 0 && plan.evict < turns_.size() &&
         turns_[plan.evict].role == Role::kAssistant) {
    ++plan.evict;
  }
  return plan;
}

void Conversation::Add(const Plan& plan, std::vector<Turn> added) {
  const size_t evict = std::min(plan.evict, turns_.size());
  for (size_t i = 0; i < evict; ++i) {
    turns_tokens_ -= turns_.front().tokens;
    turns_.pop_front();
  }
  for (Turn& t : added) {
    turns_tokens_ = AddTokens(turns_tokens_, t.tokens);
    turns_.push_back(std::move(t));
  }
  KeepNewestMedia();
}

bool Conversation::AddReply(std::string text) {
  Turn reply = MakeTurn(Role::kAssistant, std::move(text));
  bool evicted = false;
  while (!turns_.empty() &&
         AddTokens(AddTokens(system_tokens_, turns_tokens_), reply.tokens) >
             input_quota_) {
    turns_tokens_ -= turns_.front().tokens;
    turns_.pop_front();
    evicted = true;
  }
  // As in PlanAdd(): the history keeps starting with a user turn.
  while (evicted && !turns_.empty() &&
         turns_.front().role == Role::kAssistant) {
    turns_tokens_ -= turns_.front().tokens;
    turns_.pop_front();
  }
  turns_tokens_ = AddTokens(turns_tokens_, reply.tokens);
  turns_.push_back(std::move(reply));
  return evicted;
}

Conversation::Shape Conversation::ShapeAfter(
    const Plan& plan,
    const std::vector<Turn>& added) const {
  // The turns as they would be, newest last; then which images and clips
  // go: the newest kMaxContextImages and kMaxContextAudio, counted from the
  // end.
  std::vector<const Turn*> all;
  for (size_t i = std::min(plan.evict, turns_.size()); i < turns_.size();
       ++i) {
    all.push_back(&turns_[i]);
  }
  for (const Turn& t : added) {
    all.push_back(&t);
  }
  const std::vector<size_t> sent =
      NewestCounts(all, &Turn::images, kMaxContextImages);
  const std::vector<size_t> sent_audio =
      NewestCounts(all, &Turn::audio, kMaxContextAudio);

  std::vector<std::pair<Role, std::string>> merged;
  std::vector<std::vector<ModelImage>> merged_images;
  std::vector<std::vector<ModelAudio>> merged_audio;
  for (size_t i = 0; i < all.size(); ++i) {
    const Turn& t = *all[i];
    if (merged.empty() || merged.back().first != t.role) {
      merged.emplace_back(t.role, t.text);
      merged_images.emplace_back();
      merged_audio.emplace_back();
    } else {
      merged.back().second += "\n\n";
      merged.back().second += t.text;
    }
    // A turn's newest images and clips are its last ones.
    merged_images.back().insert(
        merged_images.back().end(),
        t.images.end() - static_cast<std::ptrdiff_t>(sent[i]),
        t.images.end());
    merged_audio.back().insert(
        merged_audio.back().end(),
        t.audio.end() - static_cast<std::ptrdiff_t>(sent_audio[i]),
        t.audio.end());
  }
  Shape shape;
  if (!merged.empty() && merged.back().first == Role::kUser) {
    shape.user_message = std::move(merged.back().second);
    shape.user_images = std::move(merged_images.back());
    shape.user_audio = std::move(merged_audio.back());
    merged.pop_back();
    merged_images.pop_back();
    merged_audio.pop_back();
  }
  shape.history = std::move(merged);
  bool any_history_images = false;
  for (const std::vector<ModelImage>& images : merged_images) {
    any_history_images = any_history_images || !images.empty();
  }
  if (any_history_images) {
    shape.history_images = std::move(merged_images);
  }
  bool any_history_audio = false;
  for (const std::vector<ModelAudio>& clips : merged_audio) {
    any_history_audio = any_history_audio || !clips.empty();
  }
  if (any_history_audio) {
    shape.history_audio = std::move(merged_audio);
  }
  return shape;
}

}  // namespace molt_ai::web_ai
