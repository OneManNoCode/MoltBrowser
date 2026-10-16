// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_feature_overrides.h"

#include <functional>

#include "base/containers/span.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/omnibox/common/omnibox_features.h"
#include "components/translate/core/browser/translate_ranker_impl.h"

namespace molt_ai {
namespace privacy {

namespace {

void AppendOverrides(
    base::span<const base::Feature* const> features,
    base::FeatureList::OverrideState state,
    std::vector<base::FeatureList::FeatureOverrideInfo>& overrides) {
  for (const base::Feature* feature : features) {
    overrides.emplace_back(std::cref(*feature), state);
  }
}

}  // namespace

void AppendDefaultFeatureOverrides(
    std::vector<base::FeatureList::FeatureOverrideInfo>& overrides) {
  static const base::Feature* const kDisabledByDefault[] = {
      // Autofill crowdsourcing. On every http(s) page with a form that has a
      // password field or enough fillable fields, the browser asks
      // content-autofill.googleapis.com for field-type predictions (hashed
      // form and field signatures: action host, form and field names), and on
      // submission it uploads "votes". Nothing gates this on an API key, so a
      // keyless build sends it with the placeholder key, which Google is
      // expected to reject. Autofill keeps working on its local heuristics.
      &autofill::features::debug::kAutofillServerCommunication,

      // Translate Ranker. The ranker downloads its model from www.gstatic.com
      // when it is first created (the first time translate evaluates a page)
      // and again when the cached copy expires; the model only tunes how
      // often the translate prompt is offered. With both features off the
      // ranker answers "offer", exactly as upstream does before the model
      // has downloaded.
      &translate::kTranslateRankerQuery,
      &translate::kTranslateRankerEnforcement,

      // Google AI Mode entry points in the omnibox. 152 dropped
      // kAiModeOmniboxEntryPoint; OmniboxFieldTrial's
      // IsAimOmniboxEntrypointEnabled() now shows the AI Mode button
      //  * for a Google default search engine when AimEligibilityService
      //    says the user is eligible, which needs kAimEnabled (the AIM kill
      //    switch, which also gates every other AIM surface), and
      //  * for any other engine when kAim3pEntrypoint is on and kAimEnabled
      //    plus policy allow it.
      // MoltBrowser defaults both features to disabled in its overlay
      // (aim_eligibility_service_features.cc, omnibox_features.cc), but the
      // "Aim3pEntrypoint" study in the field trial testing config turns
      // kAim3pEntrypoint back on for desktop; listing both here makes any
      // study that touches them skip.
      &omnibox::kAimEnabled,
      &omnibox::kAim3pEntrypoint,
  };
  AppendOverrides(kDisabledByDefault,
                  base::FeatureList::OVERRIDE_DISABLE_FEATURE, overrides);

  // Features MoltBrowser forces on go in a list like the one above, appended
  // with base::FeatureList::OVERRIDE_ENABLE_FEATURE.
}

}  // namespace privacy
}  // namespace molt_ai
