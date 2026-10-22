// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"

#include <algorithm>
#include <limits>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/services/molt_ai/inference/engine/image_input.h"

namespace molt_ai::web_ai {

static_assert(kMaxContextImages == kMaxImagesPerRequest,
              "the context keeps what one request can show the model");

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

Turn MakeTurn(Role role, std::string text, size_t image_count) {
  Turn turn;
  turn.role = role;
  constexpr uint32_t kMax = std::numeric_limits<uint32_t>::max();
  const uint32_t image_tokens =
      image_count > kMax / kImageInputTokens
          ? kMax
          : static_cast<uint32_t>(image_count) * kImageInputTokens;
  turn.tokens = AddTokens(EstimateMessageTokens(text), image_tokens);
  turn.text = std::move(text);
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

void Conversation::KeepNewestImages() {
  size_t kept = 0;
  for (size_t i = turns_.size(); i-- > 0;) {
    std::vector<ModelImage>& images = turns_[i].images;
    if (kept + images.size() <= kMaxContextImages) {
      kept += images.size();
      continue;
    }
    // A turn's newest images are its last ones.
    const size_t keep = kMaxContextImages - kept;
    images.erase(images.begin(),
                 images.end() - static_cast<std::ptrdiff_t>(keep));
    kept = kMaxContextImages;
  }
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
  KeepNewestImages();
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
  // The turns as they would be, newest last; then which images go: the
  // newest kMaxContextImages, counted from the end.
  std::vector<const Turn*> all;
  for (size_t i = std::min(plan.evict, turns_.size()); i < turns_.size();
       ++i) {
    all.push_back(&turns_[i]);
  }
  for (const Turn& t : added) {
    all.push_back(&t);
  }
  std::vector<size_t> sent(all.size(), 0);
  size_t room = kMaxContextImages;
  for (size_t i = all.size(); i-- > 0 && room > 0;) {
    sent[i] = std::min(room, all[i]->images.size());
    room -= sent[i];
  }

  std::vector<std::pair<Role, std::string>> merged;
  std::vector<std::vector<ModelImage>> merged_images;
  for (size_t i = 0; i < all.size(); ++i) {
    const Turn& t = *all[i];
    if (merged.empty() || merged.back().first != t.role) {
      merged.emplace_back(t.role, t.text);
      merged_images.emplace_back();
    } else {
      merged.back().second += "\n\n";
      merged.back().second += t.text;
    }
    // A turn's newest images are its last ones.
    merged_images.back().insert(
        merged_images.back().end(),
        t.images.end() - static_cast<std::ptrdiff_t>(sent[i]),
        t.images.end());
  }
  Shape shape;
  if (!merged.empty() && merged.back().first == Role::kUser) {
    shape.user_message = std::move(merged.back().second);
    shape.user_images = std::move(merged_images.back());
    merged.pop_back();
    merged_images.pop_back();
  }
  shape.history = std::move(merged);
  bool any_history_images = false;
  for (const std::vector<ModelImage>& images : merged_images) {
    any_history_images = any_history_images || !images.empty();
  }
  if (any_history_images) {
    shape.history_images = std::move(merged_images);
  }
  return shape;
}

}  // namespace molt_ai::web_ai
