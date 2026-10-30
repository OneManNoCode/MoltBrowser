// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Who asks the user about what a page tried with the user activation an
// agent's or voice mode's action gave it (activation_policy.h): voice mode
// on the tab (voice_controller.h) and web agent runs on it
// (agents/web_agent.h). They register with the tab's ActivationGuard
// (activation_guard.h), which calls them. Std-only, so their headers stay
// light.

#ifndef CHROME_BROWSER_MOLT_AI_SECURITY_PAGE_REQUEST_ASKER_H_
#define CHROME_BROWSER_MOLT_AI_SECURITY_PAGE_REQUEST_ASKER_H_

#include <string>

#include "chrome/browser/molt_ai/security/activation_policy.h"

namespace molt_ai {
namespace activation {

class PageRequestAsker {
 public:
  virtual ~PageRequestAsker() = default;

  // Ask |question|; the guard's Answer(|id|, ...) takes the answer. False
  // if it can't now: the request is refused then.
  virtual bool AskAboutPageRequest(int id, const Question& question) = 0;
  // Say what the guard refused (the browser's words, the site cleaned).
  virtual void TellAboutPageRequest(const std::string& text) = 0;
  // Question |id| no longer waits for an answer: it timed out, or the page
  // it was about is gone (what it asked about stays refused).
  virtual void PageRequestQuestionWithdrawn(int id) {}
  // Whether this asker reads permission prompts aloud and takes a spoken
  // answer (voice mode listening, the voice gesture): the guard then leaves
  // a prompt raised while acting to it instead of dismissing it.
  virtual bool TakesPromptsByVoice() const { return false; }
};

}  // namespace activation
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_SECURITY_PAGE_REQUEST_ASKER_H_
