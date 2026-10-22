// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"

#include <algorithm>

#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"

namespace molt_ai::web_ai {

Turn MakeTurn(Role role, std::string text) {
  Turn turn;
  turn.role = role;
  turn.tokens = EstimateMessageTokens(text);
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
  std::vector<std::pair<Role, std::string>> merged;
  auto push = [&merged](const Turn& t) {
    if (!merged.empty() && merged.back().first == t.role) {
      merged.back().second += "\n\n";
      merged.back().second += t.text;
    } else {
      merged.emplace_back(t.role, t.text);
    }
  };
  for (size_t i = std::min(plan.evict, turns_.size()); i < turns_.size();
       ++i) {
    push(turns_[i]);
  }
  for (const Turn& t : added) {
    push(t);
  }
  Shape shape;
  if (!merged.empty() && merged.back().first == Role::kUser) {
    shape.user_message = std::move(merged.back().second);
    merged.pop_back();
  }
  shape.history = std::move(merged);
  return shape;
}

}  // namespace molt_ai::web_ai
