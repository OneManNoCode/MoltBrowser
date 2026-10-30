// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Confirming a routine's sites by voice (docs/design/TOUCHLESS_BROWSING.md
// §6.8, §7.3): a routine with steps the on-device planner chose keeps an
// agent draft's rules (voice_routines.h): it runs nowhere until the user has
// confirmed the sites it may use. Today that needs a real click in the Agent
// studio (AutomationSecurity::ConfirmDraftSites(), behind
// ConsumeUserClick()), or, while the window listens, the voice gesture
// (§7.3, VoiceController::ConfirmDraftSitesByVoice()).
//
// The voice loop (voice_learning_loop.h) runs one whose sites aren't
// confirmed yet only after that: by voice first (the sites said and shown,
// the gesture taken for exactly that workflow and those sites), and the
// loop then saves the routine with AutomationSecurity::ConfirmDraftSites()
// on exactly |sites|. Where the window doesn't listen (typed commands
// only) it asks the confirmer, which says what to do instead:
// CreateRoutineSiteConfirmer() answers false with the words to say
// ("Confirm it in the Agent studio for now.").

#ifndef CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_SITES_H_
#define CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_SITES_H_

#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"

namespace content {
class WebContents;
}  // namespace content

namespace molt_ai {
namespace voice {

class RoutineSiteConfirmer {
 public:
  // |confirmed|: the user confirmed exactly the sites asked about, by a
  // gesture only the browser can mint. Otherwise |said| is what the browser
  // says instead (why not, and what to do).
  using Done = base::OnceCallback<void(bool confirmed, std::string said)>;

  virtual ~RoutineSiteConfirmer() = default;

  // Asks to confirm |sites| (AutomationSecurity::HostsReferencedBy() of
  // the routine's steps) for the routine |routine_name| in |contents| (the
  // tab voice mode is on). |done| runs once, maybe at once; not after the
  // confirmer is destroyed.
  virtual void ConfirmSites(content::WebContents* contents,
                            const std::string& routine_name,
                            const std::vector<std::string>& sites,
                            Done done) = 0;
};

// What voice mode confirms sites with when it can't by voice.
std::unique_ptr<RoutineSiteConfirmer> CreateRoutineSiteConfirmer();

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_SITES_H_
