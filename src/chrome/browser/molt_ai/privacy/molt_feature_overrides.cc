// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/privacy/molt_feature_overrides.h"

#include <functional>

#include "base/containers/span.h"
#include "chrome/browser/dictation/features.h"
#include "chrome/common/chrome_features.h"
#include "components/autofill/core/common/autofill_debug_features.h"
#include "components/history_embeddings/core/history_embeddings_features.h"
#include "components/omnibox/browser/aim_eligibility_service_features.h"
#include "components/omnibox/common/omnibox_features.h"
#include "components/optimization_guide/core/delivery/prediction_model_component_configs.h"
#include "components/translate/core/browser/translate_ranker_impl.h"
#include "third_party/blink/public/common/features.h"

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

      // Components and model downloads that only serve Google features
      // (Chromium 156 audit, docs/NETWORK_TRAFFIC.md "Changes made in this
      // audit"). Each one adds a component to the component updater's check
      // of update.googleapis.com and downloads it from Google's servers.
      //
      // Indigo: an AI image feature whose content script arrives as a
      // component ("Indigo" study in the testing config, which also turns
      // on its API client and Gemini entry point). Listing both makes the
      // whole study skip.
      &features::kIndigo,
      &features::kIndigoComponent,
      // Dictation: Google's dictation, whose connector arrives as a
      // component ("Dictation" study). MoltBrowser's own voice input runs
      // on whisper, on the device.
      &dictation::kDictation,
      // History embeddings ("HistoryEmbeddingsV2Images" study): registers
      // the history search strings component at every start. MoltBrowser's
      // @memory search is its own (docs/design/SEMANTIC_SEARCH.md).
      &history_embeddings::kHistoryEmbeddings,
      // Prediction models through the component updater
      // ("PredictionModelComponentDelivery" study, target 20: the geolocation
      // permission prediction model). Without it these models would come
      // from optimizationguide-pa.googleapis.com, which needs a Google API
      // key, so none is downloaded, as before Chromium 156.
      &optimization_guide::kPredictionModelComponentDelivery,
      // On-device head suggest: a model of popular Google queries, one per
      // locale, registered as a component at every start whatever the
      // search engine, but used only when Google is the default search
      // engine (OnDeviceHeadProvider::IsOnDeviceHeadProviderAllowed()).
      // MoltBrowser's default engine is DuckDuckGo.
      &omnibox::kOnDeviceHeadProviderNonIncognito,
      &omnibox::kOnDeviceHeadProviderIncognito,
  };
  AppendOverrides(kDisabledByDefault,
                  base::FeatureList::OVERRIDE_DISABLE_FEATURE, overrides);

  static const base::Feature* const kEnabledByDefault[] = {
      // Global Privacy Control: the `Sec-GPC: 1` request header (browser-
      // and renderer-initiated requests) and navigator.globalPrivacyControl.
      // Chromium 152 implements both behind this flag, off until Chrome adds
      // a setting; MoltBrowser sends the signal by default, as Brave does.
      // chrome://flags (#enable-global-privacy-control) can still turn it off.
      &blink::features::kGlobalPrivacyControlForce,

      // The Writer and Rewriter web APIs. At 152 Blink ships them only
      // behind an origin trial or the experimental web platform features
      // flag (LanguageModel and Summarizer are stable). MoltBrowser serves
      // all four from the user's own on-device model, and exposes the
      // interfaces to every page so their presence says nothing about the
      // user: availability() reports what a site may use
      // (docs/design/WEB_AI_APIS.md). Overriding the base::Feature turns the
      // Blink feature on ("copied_from_base_feature_if: overridden" in
      // runtime_enabled_features.json5); chrome://flags or
      // --disable-features can still turn them off.
      &blink::features::kAIWriterAPI,
      &blink::features::kAIRewriterAPI,
  };
  AppendOverrides(kEnabledByDefault, base::FeatureList::OVERRIDE_ENABLE_FEATURE,
                  overrides);
}

}  // namespace privacy
}  // namespace molt_ai
