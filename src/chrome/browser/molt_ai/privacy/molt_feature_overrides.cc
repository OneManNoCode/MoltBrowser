// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_feature_overrides.h"

#include <functional>

#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/omnibox/common/omnibox_features.h"
#include "components/translate/core/browser/translate_ranker_impl.h"

namespace molt_ai {
namespace privacy {

void AppendDefaultFeatureOverrides(
    std::vector<base::FeatureList::FeatureOverrideInfo>& overrides) {
  const base::Feature* const kDisabledByDefault[] = {
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

      // Google AI Mode entry point in the omnibox. omnibox_features.cc
      // already defaults it to disabled in MoltBrowser, but the AIMHintText
      // study in the field trial testing config turns it back on; listing it
      // here makes that study skip.
      &omnibox::kAiModeOmniboxEntryPoint,
  };

  for (const base::Feature* feature : kDisabledByDefault) {
    overrides.emplace_back(std::cref(*feature),
                           base::FeatureList::OVERRIDE_DISABLE_FEATURE);
  }
}

}  // namespace privacy
}  // namespace molt_ai
