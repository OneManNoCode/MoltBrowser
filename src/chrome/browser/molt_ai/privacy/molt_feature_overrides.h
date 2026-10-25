// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Features MoltBrowser turns off by default because their only effect in a
// build without Google API keys is a request to a Google host that cannot
// help the user, or a component download for a Google-only feature (see
// docs/NETWORK_TRAFFIC.md for the full inventory), and
// the few it turns on: Global Privacy Control, and the Writer and Rewriter
// web APIs (served by the on-device model, docs/design/WEB_AI_APIS.md).
//
// The list is applied as base::FeatureList "extra overrides" from
// ChromeFeatureListCreator (chrome/browser/metrics/), which places it:
//   * AFTER --enable-features / --disable-features, which chrome://flags also
//     writes, so a user or a test can still turn any of these back on;
//   * BEFORE testing/variations/fieldtrial_testing_config.json. Non-Chrome-
//     branded builds apply that config by default (FIELDTRIAL_TESTING_ENABLED
//     is true unless disable_fieldtrial_testing_config is set in GN), and a
//     study in it that touches a feature listed here is skipped as a whole
//     (ShouldSkipExperiment() in components/variations/field_trial_config/).
// Flipping a BASE_FEATURE default in an upstream file does not survive the
// testing config, which is why the list lives here.

#ifndef CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_FEATURE_OVERRIDES_H_
#define CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_FEATURE_OVERRIDES_H_

#include <vector>

#include "base/feature_list.h"

namespace molt_ai {
namespace privacy {

// Appends MoltBrowser's default feature overrides to `overrides`. Call while
// the FeatureList is being built, before field trials are associated.
void AppendDefaultFeatureOverrides(
    std::vector<base::FeatureList::FeatureOverrideInfo>& overrides);

}  // namespace privacy
}  // namespace molt_ai

#endif  // CHROME_BROWSER_MOLT_AI_PRIVACY_MOLT_FEATURE_OVERRIDES_H_
