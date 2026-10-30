// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Confirming a routine's sites by voice (docs/design/TOUCHLESS_BROWSING.md
// §6.8, §7.3): a routine with steps the on-device planner chose keeps an
// agent draft's rules (voice_routines.h): it runs nowhere until the user has
// confirmed the sites it may use. Today that needs a real click in the Agent
// studio (AutomationSecurity::ConfirmDraftSites(), behind
// ConsumeUserClick()). The voice gesture (§7.3) will let voice mode take
// that confirmation by voice; this is the seam it plugs into.
//
// The voice loop (voice_learning_loop.h) asks the confirmer when the user
// saves such a routine and when they run one whose sites aren't confirmed
// yet. A confirmer that can confirm by voice speaks and shows the sites,
// takes the gesture, and answers |done| with true; the loop then saves the
// routine with AutomationSecurity::ConfirmDraftSites() on exactly |sites|.
// CreateRoutineSiteConfirmer() gives the stub, which can't: it answers
// false with the words to say ("Confirm it in the Agent studio for now.").

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

// What voice mode confirms sites with: the stub until the voice gesture
// joins (§7.3).
std::unique_ptr<RoutineSiteConfirmer> CreateRoutineSiteConfirmer();

}  // namespace voice
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_VOICE_VOICE_ROUTINE_SITES_H_
