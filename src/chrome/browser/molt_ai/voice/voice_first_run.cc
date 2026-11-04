// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/voice/voice_first_run.h"

#include <cstdint>
#include <string>

#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/location.h"
#include "base/task/sequenced_task_runner.h"
#include "base/time/time.h"
#include "chrome/browser/first_run/first_run.h"
#include "chrome/browser/molt_ai/tor/molt_net_profile_id.h"
#include "chrome/browser/molt_ai/voice/spoken_answers.h"
#include "chrome/browser/molt_ai/voice/voice_controller.h"
#include "chrome/browser/molt_ai/voice/voice_controller_features.h"
#include "chrome/browser/molt_ai/voice/voice_session.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"

namespace molt_ai {
namespace voice {

namespace {

// The offer is said once the listening session has said it is on.
constexpr base::TimeDelta kOfferDelay = base::Seconds(3);

constexpr std::string_view kOffer = SaidLater(
    "This is MoltBrowser's voice mode, on because this is the browser's "
    "first run. Say Hey Molt, then what you want, or what can I say. To "
    "turn it off, say turn off voice mode.");

void SayTheOffer(uintptr_t window) {
  for (BrowserWindowInterface* open : GetAllBrowserWindowInterfaces()) {
    if (reinterpret_cast<uintptr_t>(open) != window) {
      continue;
    }
    VoiceSession* session = VoiceSession::FromBrowser(open);
    VoiceController* voice = VoiceController::FromBrowser(open);
    if (session && session->listening() && voice) {
      voice->SayNotice(Localized(voice->language(), kOffer));
    }
    return;
  }
}

}  // namespace

bool VoiceModeOfferedAtFirstRun() {
  return VoiceModeEnabled() &&
         base::FeatureList::IsEnabled(kMoltVoiceModeAtFirstRun) &&
         first_run::IsChromeFirstRun();
}

void MaybeOfferVoiceModeAtFirstRun(BrowserWindowInterface* browser) {
  static bool offered = false;
  if (offered || !browser ||
      browser->GetType() != BrowserWindowInterface::TYPE_NORMAL ||
      !browser->GetProfile() || tor::IsMoltNetProfile(browser->GetProfile()) ||
      !VoiceModeOfferedAtFirstRun()) {
    return;
  }
  offered = true;
  VoiceSession* session = VoiceSession::GetOrCreateForBrowser(browser);
  VoiceController* controller = VoiceController::GetOrCreateForBrowser(browser);
  if (!session || !controller || !session->Start()) {
    return;
  }
  // The window is found again by its address among the open ones (never
  // used if it closed meanwhile).
  const uintptr_t window = reinterpret_cast<uintptr_t>(browser);
  base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
      FROM_HERE, base::BindOnce(&SayTheOffer, window), kOfferDelay);
}

}  // namespace voice
}  // namespace molt_ai
