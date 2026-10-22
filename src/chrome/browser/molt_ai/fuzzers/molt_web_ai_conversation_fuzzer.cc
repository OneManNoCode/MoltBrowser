// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// A LanguageModel session's context (web_ai/web_ai_conversation.h) driven
// the way a web page drives it: system prompts, prompt() calls with text
// and images, replies, in any order and size; plus the writing-assistance
// prompts (web_ai/web_ai_prompts.h) on page options and text, and how a
// request's images are picked (runtime/model_image.h).
//
// Checks: input_usage() is the system prompt plus the turns' tokens and an
// accepted Add() stays within the quota; after an eviction the history
// starts with a user turn; the context keeps at most kMaxContextImages
// images and a request shows at most that many; ShapeAfter() alternates
// roles and ends with the user's message; the browser-authored parts of a
// prompt (system prompt, context preamble) never depend on page text.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <fuzzer/FuzzedDataProvider.h>

#include "base/check.h"
#include "chrome/browser/molt_ai/runtime/model_image.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_conversation.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_policy.h"
#include "chrome/browser/molt_ai/web_ai/web_ai_prompts.h"

namespace molt_ai::web_ai {
namespace {

// Pixels don't matter here; a small stand-in image per slot.
ModelImage TinyImage(uint8_t tag) {
  return ModelImage(32, 32, std::vector<uint8_t>(1, tag));
}

uint32_t SumTokens(const Conversation& c) {
  uint32_t sum = c.system_tokens();
  for (const Turn& t : c.turns()) sum = AddTokens(sum, t.tokens);
  return sum;
}

void CheckShape(const Conversation::Shape& shape,
                const Conversation& c,
                const Conversation::Plan& plan,
                const std::vector<Turn>& added) {
  for (size_t i = 1; i < shape.history.size(); ++i) {
    CHECK(shape.history[i].first != shape.history[i - 1].first)
        << "history roles don't alternate";
  }
  if (!shape.history.empty() && !shape.user_message.empty()) {
    CHECK(shape.history.back().first == Role::kAssistant);
  }
  CHECK(shape.history_images.empty() ||
        shape.history_images.size() == shape.history.size());
  size_t images = shape.user_images.size();
  for (const auto& list : shape.history_images) images += list.size();
  CHECK(images <= kMaxContextImages);
  // The images per turn PlanRequestImages() would pick agree.
  std::vector<size_t> turn_images;
  for (size_t i = std::min(plan.evict, c.turns().size()); i < c.turns().size();
       ++i) {
    turn_images.push_back(c.turns()[i].images.size());
  }
  size_t new_images = 0;
  for (const Turn& t : added) new_images += t.images.size();
  if (new_images <= kMaxContextImages) {
    std::optional<std::vector<size_t>> sent =
        PlanRequestImages(new_images, turn_images);
    CHECK(sent && sent->size() == turn_images.size());
    size_t total = new_images;
    for (size_t i = 0; i < sent->size(); ++i) {
      CHECK((*sent)[i] <= turn_images[i]);
      total += (*sent)[i];
    }
    CHECK(total <= kMaxContextImages);
  } else {
    CHECK(!PlanRequestImages(new_images, turn_images));
  }
}

void DriveConversation(FuzzedDataProvider* p) {
  Conversation c(p->ConsumeBool() ? kSmallInputQuota
                                  : p->ConsumeIntegralInRange<uint32_t>(0, 9000));
  for (int step = 0; step < 256 && p->remaining_bytes() > 0; ++step) {
    switch (p->ConsumeIntegralInRange<int>(0, 3)) {
      case 0: {
        const std::string text = p->ConsumeRandomLengthString(20000);
        const uint32_t before = c.input_usage();
        if (c.SetSystemPrompt(text)) {
          CHECK(c.system_prompt() == text);
          CHECK(c.input_usage() <= c.input_quota());
        } else {
          CHECK(c.input_usage() == before);
        }
        break;
      }
      case 1:
      case 2: {
        std::vector<Turn> added;
        const size_t n = p->ConsumeIntegralInRange<size_t>(1, 3);
        for (size_t i = 0; i < n; ++i) {
          const Role role = (i + 1 == n || p->ConsumeBool()) ? Role::kUser
                                                             : Role::kAssistant;
          const size_t images =
              role == Role::kUser ? p->ConsumeIntegralInRange<size_t>(0, 6) : 0;
          Turn t = MakeTurn(role, p->ConsumeRandomLengthString(8000), images);
          for (size_t k = 0; k < images; ++k) {
            t.images.push_back(TinyImage(static_cast<uint8_t>(k)));
          }
          added.push_back(std::move(t));
        }
        const Conversation::Plan plan = c.PlanAdd(added);
        if (!plan.fits) {
          CHECK(plan.requested > c.input_quota());
          break;
        }
        CHECK(plan.evict <= c.turns().size());
        const Conversation::Shape shape = c.ShapeAfter(plan, added);
        CheckShape(shape, c, plan, added);
        // A prompt that fails or is aborted leaves the context unchanged;
        // one that completes adds its turns, then the reply.
        if (p->ConsumeBool()) break;
        // Old turns go in whole exchanges: what is left of them starts with
        // a user turn. (The page orders the turns it adds itself.)
        const bool keeps_old = plan.evict > 0 && plan.evict < c.turns().size();
        c.Add(plan, std::move(added));
        CHECK(c.input_usage() <= c.input_quota())
            << "the context is over the quota after Add";
        if (keeps_old) CHECK(c.turns().front().role == Role::kUser);
        if (p->ConsumeBool()) {
          const bool reply_evicted =
              c.AddReply(p->ConsumeRandomLengthString(4000));
          CHECK(!c.turns().empty() &&
                c.turns().back().role == Role::kAssistant);
          if (reply_evicted && c.turns().size() > 1) {
            CHECK(c.turns().front().role == Role::kUser);
          }
        }
        break;
      }
      default: {
        const bool evicted = c.AddReply(p->ConsumeRandomLengthString(4000));
        if (evicted && c.turns().size() > 1) {
          CHECK(c.turns().front().role == Role::kUser);
        }
        break;
      }
    }
    CHECK(c.input_usage() == SumTokens(c));
    CHECK(c.image_count() <= kMaxContextImages);
  }
}

void CheckPrompts(FuzzedDataProvider* p) {
  const std::string tag = p->ConsumeRandomLengthString(40);
  const std::string language = IsValidLanguageTag(tag) ? tag : std::string();
  const std::string shared = p->ConsumeRandomLengthString(4000);
  const std::string context = p->ConsumeRandomLengthString(4000);
  const std::string input = p->ConsumeRandomLengthString(8000);

  SummarizerOptions so;
  so.type = static_cast<SummarizerType>(p->ConsumeIntegralInRange<int>(0, 3));
  so.format = static_cast<TextFormat>(p->ConsumeIntegralInRange<int>(0, 1));
  so.length = static_cast<SummarizerLength>(p->ConsumeIntegralInRange<int>(0, 2));
  so.output_language = language;
  WriterOptions wo;
  wo.tone = static_cast<WriterTone>(p->ConsumeIntegralInRange<int>(0, 2));
  wo.format = so.format;
  wo.length = static_cast<WriterLength>(p->ConsumeIntegralInRange<int>(0, 2));
  wo.output_language = language;
  RewriterOptions ro;
  ro.tone = static_cast<RewriterTone>(p->ConsumeIntegralInRange<int>(0, 2));
  ro.format = static_cast<RewriterFormat>(p->ConsumeIntegralInRange<int>(0, 2));
  ro.length = static_cast<RewriterLength>(p->ConsumeIntegralInRange<int>(0, 2));
  ro.output_language = language;

  const TaskPrompt prompts[] = {
      BuildSummarizerPrompt(so, shared, context, input),
      BuildWriterPrompt(wo, shared, context, input),
      BuildRewriterPrompt(ro, shared, context, input),
  };
  // Other page text, empty where the page's was (the preamble is there only
  // when there is context).
  const std::string s2 = shared.empty() ? "" : "s";
  const std::string c2 = context.empty() ? "" : "c";
  const std::string i2 = input.empty() ? "" : "i";
  const TaskPrompt others[] = {
      BuildSummarizerPrompt(so, s2, c2, i2),
      BuildWriterPrompt(wo, s2, c2, i2),
      BuildRewriterPrompt(ro, s2, c2, i2),
  };
  for (size_t i = 0; i < 3; ++i) {
    // Browser-authored text never carries page text.
    CHECK(prompts[i].system_prompt == others[i].system_prompt);
    CHECK(prompts[i].context_preamble == others[i].context_preamble);
    CHECK(prompts[i].user_message == input);
    CHECK(prompts[i].context == CombineContext(shared, context));
    CHECK(prompts[i].max_output_tokens > 0);
  }
  CHECK(BuildLanguageModelSystemPrompt(language) ==
        BuildLanguageModelSystemPrompt(language));
  RewriterMaxOutputTokens(ro.length, p->ConsumeIntegral<uint32_t>());
}

}  // namespace
}  // namespace molt_ai::web_ai

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  FuzzedDataProvider p(data, size);
  if (p.ConsumeBool()) {
    molt_ai::web_ai::DriveConversation(&p);
  } else {
    molt_ai::web_ai::CheckPrompts(&p);
  }
  return 0;
}
