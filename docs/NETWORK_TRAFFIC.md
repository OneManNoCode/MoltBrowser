# What MoltBrowser contacts without being asked

This page lists every host that MoltBrowser connects to on its own. It does
not list the sites you visit or requests you start yourself. The goal is to
be honest. Where we have only read the code and not yet watched the traffic,
the page says so.

**Scope.** Desktop builds (macOS arm64, Linux x64, Windows x64) at Chromium
156.0.8078.48 (`d0c02bfa`, see `chromium_version.txt`), on a new profile that
is not signed in to anything, with default settings. That covers startup,
idle time, the first navigation and every navigation after it. Android and
iOS are **not covered**: they use different services (for example Google
Play Services for Safe Browsing on Android) and have not been audited. The
page was first written at Chromium 148, and re-audited against Chromium
156's source after the rebase (PR #64): new components, model downloads,
Safe Browsing, Privacy Sandbox, variations, metrics, sign-in, DNS and the
Gemini and Lens entry points.

**How sure we are.** Each row says one of:

- **Source**: confirmed by reading Chromium's source at the pin. The file and
  line are given, relative to the Chromium tree; MoltBrowser's own files start
  with `src/`.
- **Likely**: the code clearly points this way, but we did not follow every
  call.
- **Capture**: has to be confirmed by watching real traffic. The leak kit's
  traffic capture does this on a real build ([How we'll
  verify](#how-well-verify)).

No row has been confirmed by a traffic capture yet.

## Summary

| Host | Why | In MoltBrowser |
|---|---|---|
| `update.googleapis.com` and Google's download hosts | Component updates: certificate revocation (CRLSet), the Chrome Root Store, Safety Tips, download file-type rules and more | **On**, for security |
| `clients2.google.com/time` | Network time, used to check certificate dates when your clock is wrong | **On**, for security |
| `safebrowsing.googleapis.com` | Safe Browsing list updates | **On, but it doesn't work** (no API key). See [S17 memo](SAFE_BROWSING_DECISION.md) |
| `sb-ssl.google.com` | Safe Browsing check of downloaded programs | **On, but it doesn't work** (no API key). Sends the download URL. See S17 memo |
| `redirector.gvt1.com` | Spell-check dictionary download (Linux, some Windows languages) | **On**, so spell-check works |
| `translate.googleapis.com` | Translate's language list, and Google's page translation | **Off**: pages are translated on your device |
| `content-autofill.googleapis.com` | Autofill form-type hints, on every page with a form | **Off** |
| `www.gstatic.com` (translate model) | Model that decides when to offer Translate | **Off** |
| `8.8.8.8`, `8.8.4.4` (Google Public DNS) | DNS check after a "site can't be reached" error; Secure DNS's fallback | **Off** |
| `connectivitycheck.gstatic.com` | Wi-Fi login-page (captive portal) check after certificate errors and timeouts | **Off** |
| `passwordsleakcheck-pa.googleapis.com` | Checks whether a password you submit has leaked | **Off** |
| Components for Google-only features (Indigo, Dictation, history search strings, on-device head suggest, the geolocation permission model, actor safety lists, CAPTCHA providers, amount-extraction regexes, commerce heuristics) | Downloaded through the component updater for features MoltBrowser doesn't use | **Off** (changed in the Chromium 156 audit) |
| The on-device model manifest (through `update.googleapis.com`) | The list of Chromium's own on-device model files | **Only when a Chromium feature asks for an on-device model**; none does in a default profile (changed in the Chromium 156 audit) |
| `duckduckgo.com` | Default search engine; suggestions while you type in the address bar (never for `@memory`, `@memory <words>` or `@ai <prompt>`, which stay on the device: `docs/design/SEMANTIC_SEARCH.md` §7; text starting with `?` is a web search and is sent) | **On** (only while you type) |
| `api.github.com`, `github.com` | MoltBrowser update check | **On** |
| `easylist.to`, `ublockorigin.github.io`, `raw.githubusercontent.com` | MoltShield ad- and tracker-blocking lists | **On** |

Every other Google service we found is **off** in this build. That includes
variations/Finch, usage statistics, crash reports, sign-in and sync, Google
push messaging, Domain Reliability, surveys, optimization hints, real-time
Safe Browsing lookups, password affiliations, Google's New Tab Page, AI Mode,
What's New, Gemini, Lens, SCT auditing, Private AI and the outdated-version
check. The reasons are in the detailed tables below. [Section 6](#6-machine-readable-host-list)
lists every host of this page in a form the leak kit reads.

## The build this applies to

`configs/*.gn` and `scripts/configure.sh` set these values:

| Setting | macOS (all), Linux, Windows | Android | Effect |
|---|---|---|---|
| `is_chrome_branded` | `false` | `false` | Turns off every `GOOGLE_CHROME_BRANDING` code path (several listed below) and the GN arguments that default to it, such as `report_activity` (`components/activity_reporter/buildflags.gni:11`) |
| `google_api_key`, client id, client secret | `""` | `""` | The API key becomes the placeholder `"dummytoken"` (`google_apis/default_api_keys.h`), so `HasAPIKeyConfigured()` and `IsGoogleChromeAPIKeyUsed()` are false. Features that check this stay off. Features that don't check it **still send their request, with `key=dummytoken`**. |
| `safe_browsing_mode` | `1` | `0` | Mode 1 builds the full desktop Safe Browsing service; mode 0 leaves it out. The Android configs are scaffolding (no real Android build yet) |
| `enable_reporting` | `false` | `false` | Leaves out Network Error Logging and the Reporting API |
| `enable_widevine` | not set → `false` | Android: `true` | `third_party/widevine/cdm/widevine.gni`: Widevine is Chrome-branded on desktop. No Widevine component is downloaded, and DRM video (Netflix and similar) does not play |
| `is_official_build` | `true` (Windows: `false`) | `true` | Doesn't change any network behaviour described here |
| `disable_fieldtrial_testing_config` | not set → `false` | not set | **Applies Chromium's field-trial testing config at runtime.** See below |

**The field-trial testing config.** Builds without Chrome branding compile in
`testing/variations/fieldtrial_testing_config.json` and apply it at startup,
unless `--disable-field-trial-config` is passed
(`components/variations/service/BUILD.gn`,
`variations_field_trial_creator.cc`). At 156 it turns on 633 (Linux) to 670
(Windows) features and turns off 46 or 47 on each desktop platform. They
include Gemini in Chrome (`Glic`), Lens features, the Indigo and Dictation
features, history embeddings, Safe Browsing's V5 list protocol and survey
triggers; at 156 it turns the Privacy Sandbox ad APIs (Topics, Protected
Audience) *off*. So the features MoltBrowser actually runs with are **not**
simply the defaults in the code.

This also explains why some of MoltBrowser's earlier changes don't take
effect. At 148, `omnibox_features.cc` turned `AiModeOmniboxEntryPoint` off in
the code, but the `AIMHintText` study turned it back on; at 152 the omnibox
AI Mode button for non-Google search engines is `Aim3pEntrypoint`, which the
`Aim3pEntrypoint` study turns on. The overrides MoltBrowser adds are
registered *before* the testing config. A study that touches one of those
features is then skipped entirely
(`components/variations/field_trial_config/field_trial_util.cc`). See
[Changes made in this audit](#changes-made-in-this-audit).

**Re-checked at 156.** Of the feature defaults MoltBrowser flips in its
upstream files, only `Aim3pEntrypoint` is touched by a study, and it is
also an override, so that study is skipped (`AimEnabled` and
`PartitionConnectionsByNetworkIsolationKey` are touched by none, or only
turned on). No study touches a feature MoltBrowser overrides other than
the ones the overrides are there for (`Aim3pEntrypoint`, `Indigo`,
`Dictation`, `HistoryEmbeddingsV2Images`,
`PredictionModelComponentDelivery`, `OnDeviceModelBackgroundDownload`,
`EnableOnDeviceWebSpeechSmallExpertModel`), and the testing config cannot change
a pref, so the pref defaults below (DNS probes, captive portal, leak
check) stand. The studies that do apply and make traffic are listed where
their traffic is: Safe Browsing's V5 lists (section 1).

## 1. Google hosts contacted with no user action (startup and idle)

| Endpoint | Purpose | What's sent | When | MoltBrowser default | How to turn off | Confidence |
|---|---|---|---|---|---|---|
| `https://update.googleapis.com/service/update2/json` (falls back to `http://` on the same host) | Component updater: one combined update check for every registered component | JSON request containing OS, OS version, CPU architecture, CPU features (SSE/AVX), RAM in GB, browser version and channel, UI language, each component's ID and version, coarse day counters (`rd`/`ad`/`ping_freshness`) and random per-request IDs. No cookies and no account | 1 min after start, then every 5 h; an on-demand check for one component can come sooner | **On (needed for security).** Registered on every desktop start: CRLSet (certificate revocation), PKI Metadata (key-pinning list, Chrome Root Store updates; its Certificate Transparency list is unused, as CT is off without Chrome branding), Safety Tips, File Type Policies (download danger levels), SSL Error Assistant, Origin Trials keys, Crowd Deny, Trust Token keys, Related Website Sets, Privacy Sandbox attestations, Optimization Hints, Subresource Filter rules, MEI preload, hyphenation, IWA keys, zxcvbn password-strength data and the WASM TTS engine. **The on-device model manifest** (the list of Chromium's own on-device model files) is asked for only when a Chromium feature first asks for an on-device model, never at start (changed in the 156 audit, section 3); MoltBrowser's web AI APIs never ask (they run on MoltBrowser's own model, `docs/design/WEB_AI_APIS.md` §10.8). Components only Google features use are off (section 3). On-demand components (Screen AI, Live Caption, TranslateKit) come only when a feature asks for them; a TranslateKit or language pack installed before (by an earlier version, or by hand in `chrome://on-device-translation-internals`) is still registered for updates at startup. The exact list is **Capture** (it appears in the request) | `--disable-component-update`. **Not recommended:** revocation and root-store updates stop | **Source**: `chrome/browser/chrome_browser_main.cc:1961`; `components/component_updater/configurator_impl.cc:54-65`; `component_updater_url_constants.cc:18-21`; `components/update_client/protocol_serializer_json.cc`; `src/chrome/browser/component_updater/registration.cc` (`RegisterComponentsForUpdate()`); manifest: `optimization_guide_on_device_model_installer.cc:453-476`, `src/components/optimization_guide/core/model_execution/manifest_broker/manifest_monitor.cc` (`RequestManifest()`) |
| Component download hosts named in the update response, usually `edgedl.me.gvt1.com`, `redirector.gvt1.com` and `dl.google.com` | Downloads a component's new version | A file request (the component's CRX or diff). The CRX is signed and checked | Only when a component has an update | On | As above | **Capture** for the exact hosts |
| `http://clients2.google.com/time/1/current` | Network time. Lets certificate checks still work when the system clock is wrong. Also used to tell "your clock is wrong" apart from a real certificate error | Only a signed nonce (`cup2key`/`cup2hreq`). The response is signed, so it cannot be faked even over HTTP | At startup when no valid saved time exists (first run, after a reboot or clock change). Otherwise ~1.2% of checks made every 6 min, about once per 8 h of use. Also on certificate date errors | **On (needed for security).** Network time feeds certificate verification | `--disable-features=NetworkTimeServiceQuerying`, or the `BrowserNetworkTimeQueriesEnabled` policy | **Source**: `components/network_time/network_time_tracker.cc:62-96,155`; `chrome/browser/net/cert_verifier_service_time_updater.cc` |
| `https://safebrowsing.googleapis.com/v5/hashLists:batchGet?...&key=dummytoken` (V4: `/v4/threatListUpdates:fetch`) | Safe Browsing list updates | Client name `chromium`, version, the list names with their (empty) states | 1–5 min after start. Each failure backs off 15–30 min, doubling up to 24 h. **At 156 the testing config's `SafeBrowsingLocalListsUseSBv5` study switches the lists to the V5 protocol** (same host) | **On, but no protection.** Without a valid key the lists never download, so Safe Browsing blocks nothing. See the [S17 memo](SAFE_BROWSING_DECISION.md) | Settings → Privacy and security → Security → *No protection* | **Source**: `components/safe_browsing/core/browser/db/v5_update_protocol_manager.cc:36-54,187-199`; `sb_protocol_manager_util.cc:42-43,180-193`; `core/common/features.cc:392-394`. **Capture**: confirm Google rejects the placeholder key |
| `https://redirector.gvt1.com/edgedl/chrome/dict/<lang>-<ver>.bdic` | Hunspell spell-check dictionary | The dictionary language and version (in the URL) | Linux, and on Windows only for languages the system checker lacks. When spell-check starts and the dictionary isn't on disk yet (normally once per language). macOS uses the system checker and never downloads | **On**: without it there is no spell-check on Linux | Settings → Languages → Spell check off | **Source** for the URL and platforms: `chrome/browser/spellchecker/spellcheck_hunspell_dictionary.cc:281`; `components/spellcheck/spellcheck_build_features.gni`. **Capture** for timing |

## 2. Google hosts contacted as a side effect of browsing

| Endpoint | Purpose | What's sent | When | MoltBrowser default | How to turn off / on | Confidence |
|---|---|---|---|---|---|---|
| `https://content-autofill.googleapis.com/v1/pages/...` | Autofill crowdsourcing: asks for field-type predictions, and uploads "votes" on submit | Hashed form and field signatures (form action host, form and field names). Votes add what the field turned out to be. Sent with `X-Goog-Api-Key: dummytoken`. Nothing in the code checks for a key | Every http(s) page with a form that has a password field or enough fillable fields | **Off** (an override). Autofill uses local heuristics | Back on with `--enable-features=AutofillServerCommunication` | **Source**: `components/autofill/core/browser/crowdsourcing/autofill_crowdsourcing_manager.cc:128`; `autofill_debug_features.cc` |
| `https://chromium-i18n.appspot.com/ssl-aggregate-address/...` | Address formats (libaddressinput) | The country code of the address | When Autofill fills a saved address into a region `<select>` it can't match directly, and when the payment sheet edits an address. Never from MoltNet windows (MOLTNET_COMPLIANCE.md §2) | On: it is what makes those address fields fill. Only after you use a saved address | Don't save addresses (Settings → Autofill) | **Likely** (the hooks MoltNet added are in `src/chrome/browser/ui/autofill/chrome_autofill_client.cc` and `chrome_payment_request_delegate.cc`) |
| `https://www.gstatic.com/chrome/intelligence/assist/ranker/models/translate/...` | Translate Ranker model, which tunes how often Translate is offered | A file request | When the ranker is first created (the first page Translate evaluates), then whenever the cached model expires | **Off** (an override). The ranker then always answers "offer", which is upstream's behaviour before the model arrives | Back on with `--enable-features=TranslateRankerQuery,TranslateRankerEnforcement` | **Source**: `components/translate/core/browser/translate_ranker_impl.cc:80-92,148,177-190` |
| `https://translate.googleapis.com/translate_a/l?client=chrome&hl=..&key=dummytoken` | The list of languages Translate supports | UI language, placeholder key | Was: when Translate UI or the language settings asked for the supported languages | **Off**. Translate's list is the on-device translator's 20 languages, set at startup, and the list has no fetcher left, so it is never requested (`docs/design/PAGE_TRANSLATION.md` §4.5) | Not available | **Source**: `translate_language_list.cc` (`SetLocalLanguages()`, which drops the fetcher), `chrome/browser/translate/translate_service.cc` (`Initialize()`). **Capture** to confirm |
| `https://translate.googleapis.com/translate_a/element.js` and the translation calls; `translate-pa.googleapis.com` | Translating a page, or a selection | Was: the page text, sent to Google | Was: when you clicked Translate, and only with your own `GOOGLE_API_KEY` (without a key Translate refused to run) | **Off**. "Translate" runs on your on-device model: the page's text never leaves the device, and the script is never requested, in any window, with or without a key. Without a model, Translate says so and offers the model picker; nothing is downloaded (`docs/design/PAGE_TRANSLATION.md`). "Translate selection" (partial translate, which sent the selection to Google) is off | Settings → Languages → offer to translate off, or don't click Translate | **Source**: `translate_manager.cc` (`TranslatePage()` skips `TranslateScript` for `TranslatesOnDevice()`; `CanPartiallyTranslateTargetLanguage()`), `chrome_translate_client.cc` (`MoltTranslateDriver`). **Capture** to confirm |
| DNS query for `google.com` sent straight to `8.8.8.8` / `8.8.4.4` | Error-page DNS probe: is it your DNS or the whole internet? | The query, from your IP, to Google Public DNS | After a "site can't be reached" (DNS) error | **Off** (`alternate_error_pages.enabled` defaults to false) | Pref `alternate_error_pages.enabled` or policy `AlternateErrorPagesEnabled` | **Source**: `chrome/browser/net/dns_probe_service_factory.cc:47-48,68-70`; `dns_probe_runner.cc:21`; `net_error_tab_helper.cc:252-265`; `profile_network_context_service.cc` |
| `http://connectivitycheck.gstatic.com/generate_204` | Captive-portal (hotel Wi-Fi login) detection | An empty request | After certificate errors and slow loads, on desktop | **Off**: the same pref as above. Your operating system's own captive-portal detection still works. In MoltNet windows it is off whatever the regular profile's setting, and no sign-in page ever opens there (`docs/MOLTNET_COMPLIANCE.md`). Safety Check's copy of this URL (`components/safety_check/url_constants.cc`) is used on Android and iOS only | Same pref or policy (a policy that turns it on also reaches MoltNet, but still opens no sign-in page there) | **Source**: `components/captive_portal/core/captive_portal_detector.cc:24-26`; `content/captive_portal_service.cc`; `core/features.gni` |
| `http://clients2.google.com/time/1/current` | Network time (row in section 1) | Nonce | On certificate date errors, also a MoltNet window's (outside Tor, MOLTNET_COMPLIANCE.md §5.7) | On | As in section 1 | **Source** (section 1) |
| `https://sb-ssl.google.com/safebrowsing/clientreport/download?key=dummytoken` | Safe Browsing download protection | **The download URL, the chain of pages that led to it (referrers), file name, size, SHA-256 and signature details** | When you download a file type Safe Browsing checks (executables, archives, disk images, ...) | **On while Safe Browsing is on**; never from MoltNet windows, where it is off (MOLTNET_COMPLIANCE.md §2). Without a key it can't return a verdict. See the S17 memo | Safe Browsing → *No protection* | **Source**: `chrome/browser/safe_browsing/download_protection/download_protection_delegate_desktop.cc:27`; `check_client_download_request_base.cc`. **Capture**: confirm the request goes out and is rejected |
| `https://sb-ssl.google.com/safebrowsing/clientreport/login` | Safe Browsing password-reuse ("phishing") report | The reuse event and the page URL | Only after you type a saved password on a different site while Safe Browsing is on | On while Safe Browsing is on; never from MoltNet windows | Safe Browsing → *No protection* | **Likely**: `components/safe_browsing/core/browser/password_protection/password_protection_service_base.cc:44` |
| `https://passwordsleakcheck-pa.googleapis.com/v1/leaks:lookupSingle` | Password leak check | A short hash prefix of the username and a blinded hash of username+password. For signed-out users the only credential is the API key (placeholder) | On every password-form submission | **Off** (`profile.password_manager_leak_detection` defaults to false) | Password Manager settings → "Warn you if passwords are exposed in a data breach" | **Source**: `components/password_manager/core/browser/leak_detection/leak_detection_request.h:40`; `leak_detection_delegate.cc`; `leak_detection_check_impl.cc` |
| `https://cloudflare-dns.com/dns-query?type=TXT&...` | A DNS TXT lookup for a page's email verification request (FedCM's Email Verification Protocol, on by default at 156) | The email address's domain, from your IP | Only when a page asks the browser to verify an email address | On. Chromium sends it to Google Public DNS (`dns.google`); MoltBrowser sends it to Cloudflare instead (`src/chrome/browser/chrome_content_browser_client.cc`, `GetDnsTxtResolverUrlPrefix()`) | Not available | **Source**: `content/browser/webid/delegation/dns_request.cc:90`; `content/public/common/content_features.cc:431` |

## 3. Google services that are off in this build

| Service (host) | Why it is off | Confidence |
|---|---|---|
| Variations / Finch seed (`clientservices.googleapis.com/chrome-variations/seed`) | Builds without Chrome branding fetch it only with `--variations-server-url` | **Source**: `components/variations/service/variations_service.cc:275-292`; `variations_url_constants.cc:11` |
| Usage statistics: UMA/UKM (Google's metrics servers; host not traced) | Metrics reporting is hard-off without Chrome branding | **Source**: `components/metrics/metrics_service_accessor.cc:34-47` |
| Crash reports (Google's crash server; host not traced) | Upload consent is false without Chrome branding | **Source**: `chrome/app/chrome_crash_reporter_client.cc:182-215` |
| Domain Reliability beacons (`beacons*.gvt2.com`) | Need metrics consent | **Source**: `chrome/browser/domain_reliability/service_factory.cc:26-28`; `components/domain_reliability/google_configs.cc` |
| Surveys / HaTS | Need metrics consent. The testing config turns survey triggers on, but they never pass this check | **Source**: `chrome/browser/ui/hats/hats_service_desktop.cc:460-467,559`. That the consent check reaches `metrics_service_accessor.cc` is **Likely** |
| Private Insights (federated analytics uploads) | The testing config turns `PrivateInsightsFeature` on at 156; the service uploads only with metrics reporting on, and has no server URL by default | **Source**: `components/private_insights/private_insights_service.cc:168`; `private_insights_features.cc` |
| Activity reporting to the updater (`update.googleapis.com` "active" pings) | Built only when `report_activity` is set, which defaults to Chrome branding | **Source**: `components/activity_reporter/buildflags.gni:11`, `BUILD.gn` |
| Optimization Guide hints and model downloads (`optimizationguide-pa.googleapis.com`) | Need a configured API key | **Source**: `components/optimization_guide/core/optimization_guide_permissions_util.cc:50`; `delivery/prediction_model_download_manager.cc:216`. The Optimization Hints *component* still comes through the component updater (section 1) |
| Prediction models delivered as components (the geolocation permission prediction model) | The testing config's `PredictionModelComponentDelivery` study (target 20) would deliver it through the component updater, around the API key check. **Off** (an override, changed in the 156 audit): the model stays on the key-gated path, as before | **Source**: `components/optimization_guide/core/delivery/prediction_model_component_configs.cc:21,83`; `chrome/browser/permissions/prediction_service/prediction_model_handler_provider.cc:89-96` |
| Indigo (an AI image feature: a content script as a component, an API client, a Gemini entry point) | The testing config turns `Indigo` and `IndigoComponent` on. **Off** (overrides, changed in the 156 audit) | **Source**: `chrome/browser/component_updater/indigo_component_installer.cc:127-136`; `chrome/common/chrome_features.cc` |
| Dictation (Google's dictation; its connector is a component) | The testing config turns `Dictation` on. **Off** (an override, changed in the 156 audit); MoltBrowser's own voice input runs on whisper, on the device | **Source**: `chrome/browser/component_updater/dictation_connector_component_installer.cc:181-190` |
| History embeddings (the history search strings component) | The testing config turns `HistoryEmbeddings` on, which registers the component at every start. **Off** (an override, changed in the 156 audit); MoltBrowser's `@memory` search is its own | **Source**: `chrome/browser/component_updater/registration.cc:164-166`; `chrome/browser/history_embeddings/history_embeddings_utils.cc:173-187` |
| On-device head suggest (a model of popular Google queries, per locale) | Registered at every start whatever the search engine, but used only with Google as the default engine. **Off** (overrides, changed in the 156 audit) | **Source**: `components/component_updater/installer_policies/on_device_head_suggest_component_installer.cc:138-147`; `components/omnibox/browser/on_device_head_provider.cc:140-142` |
| AI Mode eligibility component, Private Verification Tokens component | Their features are off by default and no study turns them on. The AI Mode one (`AimEligibilityComponentExtension`) is also an override since the 156 audit, so no study or new default can turn it on | **Source**: `chrome/browser/component_updater/aim_eligibility_component_installer.cc:167-182`; `private_verification_tokens_installer.cc:26-31` |
| Actor safety lists, CAPTCHA providers, amount-extraction regexes, commerce heuristics (components) | Upstream registers them at every start, but only Google features read them: Gemini in Chrome's browser actor (the safety lists), usage statistics (the CAPTCHA list, read only by `CaptchaMetricsObserver`, and MoltBrowser sends none), Google Payments' buy-now-pay-later offers in Autofill (the regexes) and the cart module of Google's New Tab Page (commerce heuristics, registered only in the US; MoltBrowser's new tab page is `molt://home`). MoltBrowser's agent mode is its own and doesn't use the actor's lists. **Not registered** (changed in the 156 audit; `--enable-features=MoltGoogleOnlyComponents` registers them again) | **Source**: `src/chrome/browser/component_updater/registration.cc` (the `MoltBrowser:` hooks), `src/chrome/browser/molt_ai/privacy/molt_component_policy.h`. Readers: `chrome/browser/actor/execution_engine.cc:345`; `chrome/browser/page_load_metrics/observers/captcha_metrics_observer.cc`; `components/autofill/core/browser/payments/amount_extraction_manager.cc:133-200`; `chrome/browser/component_updater/commerce_heuristics_component_installer.cc:189-201`, `chrome/browser/new_tab_page/new_tab_page_util.cc:75-81` |
| The on-device model manifest at start | Upstream asks the component updater for it (a registration, then an on-demand foreground check) as soon as the model broker is created after startup, whether or not anything will use an on-device model. **Now asked for only when a Chromium feature first asks the broker for a model** (changed in the 156 audit). What can still ask in MoltBrowser: Enhanced protection's on-device scam check, but only while `ClientSideDetectionServerModelForScamDetectionDesktop` is off, and the testing config turns it on, so in MoltBrowser's builds Enhanced protection uses Google's server model instead (section 3, above) and doesn't ask; Live Caption's expert speech model, only with its flag (`LiveCaptionSpeechRecognitionSmallExpertModel`, off by default); on-device Web Speech for conversations (`OnDeviceWebSpeechGeminiNano`, a flag, off); `chrome://on-device-internals` when you request a model there; and model validation (command-line switches only). Chromium's built-in AI APIs never reach it: MoltBrowser serves them from its own manager. A feature that asks gets the manifest as before, then the model files it needs | **Source**: `src/components/optimization_guide/core/model_execution/manifest_broker/manifest_monitor.cc` (`RequestManifest()`) and `manifest_broker_state.cc` (the `MoltBrowser:` hooks); upstream: `chrome/browser/optimization_guide/chrome_browser_main_extra_parts_optimization_guide.cc:12`, `chrome/browser/component_updater/optimization_guide_on_device_model_installer.cc:364-372,453-476`. Askers: `chrome/browser/safe_browsing/client_side_detection_intelligent_scan_delegate_desktop.cc:358-372,430-445`; `chrome/browser/speech/speech_recognition_small_expert_model_installer.cc:95-140` |
| On-device model downloads ahead of use; Web Speech on a small expert model | The testing config turns on `OnDeviceModelBackgroundDownload` (models for the Prompt, Writing Assistance and Summarize APIs downloaded ahead of use, on AC power with 50 GB free; at 156 nothing marks a desktop model for it yet) and `OnDeviceWebSpeechSmallExpertModel` (on-device Web Speech on a model from the broker, so a page's availability check alone would ask for the manifest). Both are off by default upstream. **Off** (overrides, changed in the 156 audit). On-device Web Speech uses Chromium's default speech engine, downloaded only when a page you have interacted with installs it | **Source**: `components/optimization_guide/core/optimization_guide_features.cc:141-160,465-470`; `manifest_broker/manifest_asset_manager.cc:319-331,593`; `chrome/browser/speech/on_device_speech_recognition_util.cc:27-42`; `third_party/blink/renderer/modules/speech/speech_recognition.cc:370-390` |
| Safe Browsing hash-prefix real-time lookups (relay `ohttp-relay-safebrowsing-chrome.google.fastly-edge.com`, key `www.gstatic.com/ohttp_gateway/hpke_public_keys/v1/sbc_prod`) | Chrome-branded only | **Source**: `components/safe_browsing/core/common/hashprefix_realtime/hash_realtime_utils.cc:20-31,106-109`; `core/common/features.cc:360-375` |
| Safe Browsing real-time URL checks, Enhanced protection pings, client-side scam detection with a server model (`safebrowsing.google.com`) | Enhanced protection is not the default | **Source** for scam detection: `chrome/browser/safe_browsing/client_side_detection_intelligent_scan_delegate_desktop.cc:299-305`; the rest **Likely** |
| SCT auditing (`sctauditing-pa.googleapis.com`) | Certificate Transparency is enforced only in Chrome-branded builds, and auditing needs it | **Source**: `chrome/browser/ssl/sct_reporting_service.cc:217-235`; `chrome/browser/net/system_network_context_manager.cc:1156-1166`; `chrome/browser/browser_features.cc:44-49` |
| Permission predictions from Google's server (`webpermissionpredictions.googleapis.com`) | Need "Make searches and browsing better", which is off for a new profile | **Source**: `chrome/browser/permissions/prediction_service/permissions_ai_ui_selector.cc:659-700` |
| Outdated-version check (`versionhistory.googleapis.com`) | Only with the `RelaunchFastIfOutdated` policy | **Source**: `chrome/browser/upgrade_detector/upgrade_detector.cc:114-121,377-381` |
| Private AI (`phosphor-pa.googleapis.com`, proxy `proxy.g2.fastly-masque.net`) | Needs Chrome's own API key | **Source**: `components/private_ai/private_ai_service.cc:29-41` |
| Web Store information on installed extensions (`chromewebstore.googleapis.com`) | Needs Chrome's own API key | **Source**: `extensions/browser/cws_info_service.cc:308-318` |
| Skills (`chromeskills.pa.googleapis.com`) | Needs a Google account's OAuth token | **Source**: `components/skills/features.cc:26-33`; `chrome/browser/skills/skills_service_factory.cc` |
| Password affiliation lookups (`www.googleapis.com/affiliation/v1/`) | Need a configured API key | **Source**: `components/affiliations/core/browser/hash_affiliation_fetcher.cc:200` |
| Google sign-in, account cookie checks (`accounts.google.com`), Sync (`clients4.google.com`) | No OAuth client is configured, so desktop sign-in cannot turn on. MoltBrowser also defaults sign-in to disallowed and Sync to managed-off | **Source**: `chrome/browser/signin/account_consistency_mode_manager.cc:68-104,156`; MoltBrowser `src/components/signin/internal/identity_manager/primary_account_manager.cc`, `src/components/sync/service/sync_prefs.cc`. **Capture**: confirm `accounts.google.com` never appears |
| Push messaging / GCM (`mtalk.google.com`, `android.clients.google.com`, `fcm.googleapis.com`) | Delayed start: nothing connects until a site or extension subscribes to push | **Source**: `components/gcm_driver/gcm_client_impl.cc:262,387`; `google_apis/gcm/engine/gservices_settings.cc` |
| Google New Tab Page (OneGoogleBar, doodle, promos) | The new tab page is MoltBrowser's local `molt://home`. Google's widgets are forced off | **Source**: `src/chrome/browser/search/search.cc`; `src/chrome/browser/ui/webui/new_tab_page/new_tab_page_ui.cc` |
| AI Mode eligibility (`www.google.com/async/folae`) | `AimEnabled` is off in MoltBrowser, and the request needs Google as the search engine | **Source**: `components/omnibox/browser/aim_eligibility_service.cc:326-348,915-918` |
| Suggestions when you focus the address bar (zero-suggest) | Only sent when Google is the search engine | **Source**: `components/omnibox/browser/base_search_provider.cc` |
| Preconnect to the search engine | Network prediction is off by MoltBrowser default (and the engine is DuckDuckGo) | **Source**: `chrome/browser/navigation_predictor/search_engine_preconnector.cc:257,340`; `chrome/browser/predictors/loading_predictor_config.cc` |
| What's New page | Chrome-branded only | **Source**: `chrome/browser/ui/webui/whats_new/whats_new_util.cc:36` |
| Preinstalled Google web apps | Chrome-branded only | **Source**: `chrome/browser/web_applications/preinstalled_web_apps/preinstalled_web_apps.cc:21-53` |
| Hangout Services and Network Speech Synthesis component extensions | Need Google-internal sources / Chrome branding | **Source**: `chrome/common/features.gni`; `chrome/browser/extensions/component_loader.cc` |
| Recovery component, Widevine component, Platform Runtime component, Windows installer downloader (`dl.google.com/tag/s/...`) | Chrome-branded only / not built (see build table) | **Source**: `chrome/browser/component_updater/recovery_improved_component_installer.cc`; `platform_runtime_component_installer.cc:554-556`; `widevine.gni`; `chrome/browser/BUILD.gn:1508-1513` (the installer downloader is linked only when `is_chrome_branded`) |
| Gemini in Chrome (`Glic`, `gemini.google.com`) | Needs a signed-in Google account. The testing config turns its master switch on | **Source**: `chrome/browser/glic/public/glic_enabling.cc:687-696,714`. **Capture**: confirm no `gemini.google.com` |
| Lens | Needs Google as the search engine | **Source**: `chrome/browser/ui/lens/lens_overlay_entry_point_controller.cc:115` |
| Privacy Sandbox ad APIs (Topics, Protected Audience, k-anonymity) | At 156 the testing config turns Topics and Protected Audience *off*; the APIs also wait for the consent prefs, which nothing in MoltBrowser turns on | **Likely**. **Capture** |
| Profile avatar images (`www.gstatic.com/chrome/profile_avatars/`) | New profiles use the built-in placeholder. A download happens only if you pick another avatar | **Source**: `chrome/browser/profiles/profile_avatar_downloader.cc:23`; `profile_attributes_storage.cc` |
| Chrome Web Store updates (`clients2.google.com/service/update2/crx`) | Only once a Web Store extension is installed | **Likely**: `extensions/common/extension_urls.cc:47`; `chrome/browser/extensions/updater/extension_updater.cc`. **Capture** |
| Location (`www.googleapis.com/geolocation/v1/geolocate`), speech recognition (`www.google.com/speech-api/`) | Only after a site uses them and you grant permission. Expected to fail without a key. macOS uses the system location service | **Likely** |
| DevTools remote front-end (`chrome-devtools-frontend.appspot.com`), AI assistance (`aida.googleapis.com`) | Only when you use those DevTools features | **Likely** |

**DNS over HTTPS.** The default is "automatic". The browser upgrades to DoH
only if your current DNS provider offers it, and uses that same provider. Its
test queries look up `www.gstatic.com`, but they go to *your* DNS provider,
not to Google (`net/dns/dns_transaction.h:32`). At 156, automatic mode can
also fall back to Google Public DNS (`8.8.8.8`) over DoH, but only with the
Bundled Security Settings' Secure DNS option, whose feature
(`BundledSecuritySettingsSecureDnsV2`) is off by default and turned on by no
study. Since the 156 audit it is also one of MoltBrowser's overrides, so
neither a study nor a future default can turn the fallback on; automatic
mode only ever upgrades to your own provider
(`chrome/browser/net/stub_resolver_config_reader.cc:141-158,313-318`;
`components/safe_browsing/core/common/features.cc:67-68`;
`src/chrome/browser/molt_ai/privacy/molt_feature_overrides.cc`). **Source**.

## 4. Non-Google hosts that are part of browsing defaults

| Host | Purpose | When | Confidence |
|---|---|---|---|
| `duckduckgo.com` (`/?q=`, `/ac/?q=`) | Default search engine and its suggestions | Only while you type in the address bar or search | **Source**: `src/third_party/search_engines_data/resources/definitions/prepopulated_engines.json` |

MoltBrowser puts DuckDuckGo in Chromium's default slot (prepopulated id 1, the
entry still called `"google"`). Chromium still recognises it as DuckDuckGo,
not Google. It finds an engine by its URL and checks the sorted engine list,
where `duckduckgo` comes before `google`, so every "is the search engine
Google?" feature stays off. **Source**:
`components/search_engines/template_url.cc`;
`search_engine_utils.cc`; `tools/json_to_struct/aggregation.py`.

Chromium's per-country engine list
(`third_party/search_engines_data/resources/definitions/regional_settings.json`,
not in MoltBrowser's overlay) may put a different engine first in some
countries. We have not checked this.

## 5. MoltBrowser's own traffic

| Host | Purpose | What's sent | When | Default | How to turn off | Confidence |
|---|---|---|---|---|---|---|
| `api.github.com/repos/OneManNoCode/MoltBrowser/releases/latest`; if that fails, `github.com/OneManNoCode/MoltBrowser/releases/latest` (redirect only) | Update check | User-Agent `MoltBrowser/<version>`, no cookies | 30 s after launch, then every 6 h; 10 min after a failed check; when you open `molt://ai-update` | On | No switch yet. The auto-update toggle in `molt://ai-update` stops automatic *downloads*, not the check | **Source**: `src/chrome/browser/molt_ai/update/update_manager.cc` (`kInitialCheckDelay`, `kCheckInterval`, `kFailureRetryDelay`); `src/chrome/browser/ui/webui/molt_ai/molt_ai_update_ui.cc` |
| `github.com/OneManNoCode/MoltBrowser/releases/download/...`, which redirects to GitHub's asset hosts (`objects.githubusercontent.com`, `release-assets.githubusercontent.com`) | Downloading an update, its signed manifest and signature | Same User-Agent | When an update exists and auto-update is on, or you click Update | On | Auto-update toggle | **Source** (URLs); **Capture** for the asset host |
| `easylist.to`, `ublockorigin.github.io`; fallback `raw.githubusercontent.com`; optional lists from `secure.fanboy.co.nz` | MoltShield filter lists | Plain GET with `If-None-Match` / `If-Modified-Since`, no cookies. Fetched through the profile's network (through Tor in MoltNet) | 10 s after the first run, then about once a day with random jitter | On (the Fanboy lists only once you turn them on) | Turn lists off in `molt://ai-settings`, or `--disable-molt-shield` | **Source**: `src/chrome/browser/molt_ai/shield/filter_list_catalog.cc`; `filter_list_manager.cc` |
| `huggingface.co` and its download hosts (`*.huggingface.co`, `*.hf.co`) | On-device model download (chat models, and the memory search model from molt://memory) | File request | Only when you download a model | Off until you ask | — | **Source**: `src/chrome/browser/molt_ai/models/download_policy.cc`, `src/chrome/browser/molt_ai/runtime/browser_ai_runtime.cc`, `src/chrome/browser/molt_ai/memory/embedding_model_manager.cc` |
| `api.openai.com`, `api.anthropic.com`, `generativelanguage.googleapis.com`, `api.mistral.ai`, `api.groq.com`, `api.deepseek.com`, `api.x.ai`, `api.perplexity.ai`, `openrouter.ai`, or a custom endpoint | Cloud AI chat | Your message and any page context you attach | Only with your own key, per message | Off until you add a key | Remove the key | **Source**: `src/chrome/browser/molt_ai/cloud/cloud_provider.cc` |
| `search.brave.com` | The agent's web search | The search terms of the task you gave it | Only when an agent you started searches the web | Off until you ask | — | **Source**: `src/chrome/browser/molt_ai/agents/agent_actions.h` (`kSearchURLPrefix`) |
| Tor directory authorities and relays; `check.torproject.org/api/ip` | MoltNet (Tor) windows | Tor traffic | Only after you open a MoltNet window | Off until you ask | — | **Source**: `src/chrome/browser/molt_ai/tor/`, `src/chrome/browser/ui/webui/molt_ai/molt_net_handler.cc` |

## 6. Machine-readable host list

The leak kit's traffic capture (`tools/leak_test/leaktest.sh traffic`,
[LEAK_TESTING.md](LEAK_TESTING.md#regular-window-traffic-capture)) compares
what a regular window contacts with this table, through
`docs/network_traffic_hosts.json`, which is generated from it
(`tools/leak_test/leaktest.sh hosts --write`). The kit's tests fail when
the two differ, and also when a host named in sections 1 to 5 is missing
here. Change a host above, change it here too.

- **Host**: an exact name, `*.suffix` for every name under it, or an IP
  address.
- **Ports**: TCP unless marked `/udp`.
- **Expect**: `allowed` (any time); `step: name` (only during that step of
  the capture, for example the download check); `user` (only after
  something a user does that the capture never does); `forbidden` (off in
  MoltBrowser: must never appear).
- **Why**: what sends it. The capture's report quotes it as the likely
  source.

<!-- hosts:begin -->
| Host | Ports | Expect | Why | § |
|---|---|---|---|---|
| `update.googleapis.com` | 443, 80 | allowed | Component updater: one combined update check for every registered component, about a minute after start and every 5 h (the components it must not name are in the table below) | 1 |
| `*.gvt1.com` | 443, 80 | allowed | Component downloads named in an update response (`edgedl.me.gvt1.com`, `redirector.gvt1.com`) and spell-check dictionaries (`redirector.gvt1.com`, Linux) | 1 |
| `dl.google.com` | 443, 80 | allowed | Component downloads named in an update response | 1 |
| `clients2.google.com` | 80, 443 | allowed | Network time (`http://clients2.google.com/time/1/current`): at start without a saved time, now and then, and on certificate date errors; Chrome Web Store extension updates once one is installed | 1 |
| `safebrowsing.googleapis.com` | 443 | allowed | Safe Browsing list updates (V5 `hashLists:batchGet`, which the field-trial testing config turns on; else V4 `threatListUpdates:fetch`), with the placeholder key | 1 |
| `sb-ssl.google.com` | 443 | step: download | Safe Browsing download check of a program or archive: the download URL, the referrer chain and the file's hash | 2 |
| `chromium-i18n.appspot.com` | 443 | user | Address formats (libaddressinput), when Autofill fills a saved address or the payment sheet needs them | 2 |
| `cloudflare-dns.com` | 443 | user | DNS TXT lookup for a page's email verification request (MoltBrowser's resolver in place of `dns.google`) | 2 |
| `dns.google` | 443 | forbidden | Chromium's DNS TXT resolver for email verification: MoltBrowser uses `cloudflare-dns.com` instead | 2 |
| `content-autofill.googleapis.com` | 443 | forbidden | Autofill crowdsourcing: off (AutofillServerCommunication) | 2 |
| `passwordsleakcheck-pa.googleapis.com` | 443 | forbidden | Password leak check: off (pref) | 2 |
| `translate.googleapis.com`, `translate-pa.googleapis.com` | 443 | forbidden | Google Translate (language list, page and partial translation): off, Translate runs on the device | 2 |
| `www.gstatic.com` | 443 | user | Profile avatar images, when you pick one; the Translate ranker model (off) and Safe Browsing's real-time lookup key (Chrome-branded only) live here too | 2 |
| `connectivitycheck.gstatic.com` | 80, 443 | forbidden | Captive-portal check: off (pref) | 2 |
| `8.8.8.8`, `8.8.4.4`, `2001:4860:4860::8888`, `2001:4860:4860::8844` | 53/udp, 53, 443 | forbidden | Google Public DNS: the DNS probe after a "site can't be reached" error (off, pref), and Secure DNS's fallback (off, the BundledSecuritySettingsSecureDnsV2 override) | 2 |
| `google.com` | 443, 80 | forbidden | The DNS probe's query name (sent to Google Public DNS, off); nothing else looks it up | 2 |
| `duckduckgo.com` | 443 | user | Default search engine, and its suggestions while you type in the address bar | 4 |
| `clientservices.googleapis.com` | 443, 80 | forbidden | Variations seed (Chrome-branded only) | 3 |
| `accounts.google.com` | 443 | forbidden | Google sign-in and account cookies (sign-in is off) | 3 |
| `optimizationguide-pa.googleapis.com` | 443 | forbidden | Optimization Guide hints and models (need a Google API key) | 3 |
| `gemini.google.com` | 443 | forbidden | Gemini in Chrome (needs a Google account) | 3 |
| `www.google.com` | 443 | forbidden | AI Mode eligibility and other Google Search features (need Google as the search engine) | 3 |
| `*.gvt2.com` | 443 | forbidden | Domain Reliability beacons (need metrics consent) | 3 |
| `sctauditing-pa.googleapis.com` | 443 | forbidden | SCT auditing (Certificate Transparency is off without Chrome branding) | 3 |
| `ohttp-relay-safebrowsing-chrome.google.fastly-edge.com` | 443 | forbidden | Relay of Safe Browsing's hash-prefix real-time lookups (Chrome-branded only) | 3 |
| `safebrowsing.google.com` | 443 | user | Safe Browsing real-time URL checks and reports (Enhanced protection, extended reporting) | 3 |
| `mtalk.google.com`, `android.clients.google.com`, `fcm.googleapis.com` | 5228, 443 | user | Push messaging (GCM), once a site or an extension subscribes | 3 |
| `webpermissionpredictions.googleapis.com` | 443 | forbidden | Permission predictions from Google's server (need "Make searches and browsing better") | 3 |
| `versionhistory.googleapis.com` | 443 | forbidden | Outdated-version check (only with the RelaunchFastIfOutdated policy) | 3 |
| `chromewebstore.googleapis.com` | 443 | forbidden | Web Store information on installed extensions (needs Chrome's API key) | 3 |
| `phosphor-pa.googleapis.com`, `proxy.g2.fastly-masque.net` | 443, 2498 | forbidden | Private AI (needs Chrome's API key) | 3 |
| `www.googleapis.com` | 443 | user | Location for a site you allowed (Linux, Windows); password affiliation lookups (need a key) | 3 |
| `clients4.google.com` | 443 | forbidden | Sync (no sign-in) | 3 |
| `chromeskills.pa.googleapis.com` | 443 | forbidden | Skills (needs a Google account) | 3 |
| `chrome-devtools-frontend.appspot.com`, `aida.googleapis.com` | 443 | user | DevTools' remote front-end and AI assistance | 3 |
| `api.github.com`, `github.com` | 443 | allowed | MoltBrowser update check, 30 s after launch and every 6 h (`github.com`: its fallback, and the update's download URL) | 5 |
| `objects.githubusercontent.com`, `release-assets.githubusercontent.com` | 443 | allowed | MoltBrowser update download and its signed manifest, when an update exists | 5 |
| `easylist.to`, `ublockorigin.github.io`, `raw.githubusercontent.com` | 443 | allowed | MoltShield filter lists, 10 s after the first run, then about once a day | 5 |
| `secure.fanboy.co.nz` | 443 | user | MoltShield's optional Fanboy lists, once you turn them on | 5 |
| `huggingface.co`, `*.huggingface.co`, `*.hf.co` | 443 | user | On-device model downloads you start | 5 |
| `api.openai.com`, `api.anthropic.com`, `generativelanguage.googleapis.com`, `api.mistral.ai`, `api.groq.com`, `api.deepseek.com`, `api.x.ai`, `api.perplexity.ai`, `openrouter.ai` | 443 | user | Cloud AI chat with your own key | 5 |
| `search.brave.com` | 443 | user | The agent's web search, when you give an agent a task | 5 |
| `check.torproject.org` | 443 | user | MoltNet's Verify (through Tor) | 5 |
<!-- hosts:end -->

**Components the update check must not name.** The component updater's
check to `update.googleapis.com` names every component it asks about by
its ID (the `appid` of each entry in the request). The capture's sinkhole
reads those IDs, and the report fails if one of these is among them. Each
is off in MoltBrowser (section 3); the on-device model manifest and the
base model only come when a feature asks for an on-device model, which
nothing in the capture does. The same build rules apply: change a row,
then `leaktest.sh hosts --write`.

<!-- components:begin -->
| Component | ID | Why | § |
|---|---|---|---|
| On-device model manifest | `ceofaddefefcbblgcgnibnonglccbfja` | Asked for only when a Chromium feature asks for an on-device model | 3 |
| On-device base model | `fklghjjljmnfjoepjmlobpekiapffcja` | Chromium's on-device model, named by the manifest; only for a feature that asks | 3 |
| Actor safety lists | `ninodabcejpeglfjbkhdplaoglpcbffj` | Gemini in Chrome's browser actor: not registered | 3 |
| CAPTCHA providers | `pficcddpglkpaaihklmahepgjmefdnom` | Read only for usage statistics: not registered | 3 |
| Amount extraction heuristic regexes | `hajigopbbjhghbfimgkfmpenfkclmohk` | Google Payments' buy-now-pay-later offers: not registered | 3 |
| Commerce heuristics | `cocncanleafgejenidihemfflagifjic` | The cart module of Google's New Tab Page: not registered | 3 |
| Indigo | `kgejniifjbbpoocfeblcejfmmjffbfdb` | Off (IndigoComponent override) | 3 |
| Dictation connector | `kbglekiebdohdafflpmiejhbfdmjdbbe` | Off (Dictation override) | 3 |
| History search strings | `pkomkdjpmjfbkgjjmmaioegaojgdahkm` | Off (HistoryEmbeddings override) | 3 |
| On-device head suggest | `obedbbhbpmojnkanicioggnmelmoomoc` | Off (OmniboxOnDeviceHeadProvider overrides) | 3 |
| AI Mode eligibility | `kabpfonokkamggpgbldambnmkliehlbh` | Off (AimEligibilityComponentExtension override) | 3 |
| Geolocation permission predictions | `kpfeefaldebcckpkaeelpiecbjfpbmle` | Off (PredictionModelComponentDelivery override) | 3 |
| Notification permission predictions | `fmkegdflifakmolfcmpbohjhgpicfbol` | Off (PredictionModelComponentDelivery override) | 3 |
<!-- components:end -->

## Changes made in this audit

All of these change *defaults* only. A user (or `--enable-features` /
chrome://flags) can still turn each one back on.

1. **Default feature overrides**
   (`src/chrome/browser/molt_ai/privacy/molt_feature_overrides.cc`). These
   are hooked into `chrome/browser/metrics/chrome_feature_list_creator.cc`,
   after the command line and before the field-trial testing config:
   - `AutofillServerCommunication` off: no form-signature queries or votes to
     `content-autofill.googleapis.com`.
   - `TranslateRankerQuery` and `TranslateRankerEnforcement` off: no
     translate-model download from `www.gstatic.com`.
   - `Aim3pEntrypoint` and `AimEnabled` off (it was `AiModeOmniboxEntryPoint`
     before Chromium 152): keeps the omnibox AI Mode button off, which the
     `Aim3pEntrypoint` testing-config study would otherwise turn on.
   - Added in the Chromium 156 audit, each a component (or model) only a
     Google feature uses, downloaded through the component updater:
     `Indigo` and `IndigoComponent`; `Dictation`; `HistoryEmbeddings` (the
     history search strings component); `PredictionModelComponentDelivery`
     (the geolocation permission prediction model as a component);
     `OmniboxOnDeviceHeadProviderNonIncognito` and
     `OmniboxOnDeviceHeadProviderIncognito` (on-device head suggest, used
     only with Google as the search engine);
     `AimEligibilityComponentExtension` (the AI Mode eligibility component,
     already off; listed so nothing can turn it on).
   - Also added in the 156 audit, on-device model downloads nothing
     MoltBrowser ships asks for: `OnDeviceModelBackgroundDownload` (models
     downloaded ahead of use) and `OnDeviceWebSpeechSmallExpertModel` (Web
     Speech on a model from Google's broker, which a page's availability
     check alone would ask for). Both are off upstream by default and on in
     the testing config.
   - `BundledSecuritySettingsSecureDnsV2` off (156 audit): Secure DNS's
     automatic mode never falls back to Google Public DNS (`8.8.8.8`),
     whatever a study or a future default says. It was already off; now
     nothing can turn it on.
2. **Default pref values**
   (`src/chrome/browser/molt_ai/privacy/molt_privacy_prefs.cc`):
   - `alternate_error_pages.enabled = false`: no DNS probe to Google Public
     DNS and no captive-portal check against gstatic.
   - `profile.password_manager_leak_detection = false`: no leak checks that
     only a Google API key could authenticate.
3. **Components not registered** (156 audit; hooks in
   `src/chrome/browser/component_updater/registration.cc`, the switch in
   `src/chrome/browser/molt_ai/privacy/molt_component_policy.h`). Upstream
   registers these at every start, and only Google features read them, so
   they are no longer in the update check: the actor safety lists (Gemini
   in Chrome's browser actor), CAPTCHA providers (usage statistics only),
   the amount extraction regexes (Google Payments' buy-now-pay-later
   offers) and commerce heuristics (the cart module of Google's New Tab
   Page). The rest of `RegisterComponentsForUpdate()` was checked the same
   way and stays: the security components (CRLSet, PKI Metadata, Safety
   Tips, SSL Error Assistant, File Type Policies, Crowd Deny, Subresource
   Filter rules), the web platform's data (Origin Trials keys, Related
   Website Sets, Trust Token keys, Privacy Sandbox attestations, the list
   of sites allowed to use those APIs, MEI preload, hyphenation, IWA
   keys), zxcvbn, the WASM TTS engine, Optimization Hints
   (also read by Chromium features such as the loading predictor and
   compile hints), and the components that come only on demand or when a
   flag turns them on. `--enable-features=MoltGoogleOnlyComponents`
   registers the four again.
4. **The on-device model manifest on demand** (156 audit; hooks in
   `src/components/optimization_guide/core/model_execution/manifest_broker/`).
   Upstream asks for it as soon as the model broker exists, shortly after
   start. Now the broker asks for it when a feature first asks for an
   on-device model: binding the broker, an eligibility check or a session,
   an availability observer, or requesting a model in
   `chrome://on-device-internals`. In a default profile nothing does, and
   with the testing config's studies nothing MoltBrowser ships does either.
   What can still ask is in section 3 (Live Caption's expert model with
   its flag; Enhanced protection's on-device scam check if its server-model
   study is ever off); those features get the manifest and their model as
   before, a moment later. A model downloaded earlier stays on disk until a
   feature asks again or on-device AI is turned off. Turning the request
   off altogether would have been one line, but it would also take the
   model away from those features for good; on demand keeps them working.

**Left on deliberately:**
- **Component updater and network time.** They carry security data
  (revocation, root store, safe file types, certificate times) and reveal
  little: your IP, OS and version, and component versions.
- **Spell-check dictionary downloads.** These are what make spell-check work
  on Linux.
- **Safe Browsing's V5 list protocol.** The testing config's study switches
  the list updates from V4 to V5; the host, the placeholder key and what is
  sent are the same. Whether Safe Browsing stays on is the owner's
  decision.

Safe Browsing is a product decision; see the
[S17 memo](SAFE_BROWSING_DECISION.md).

## How we'll verify

Run this on the build machine for every release candidate.

**On Linux: the leak kit's traffic capture.**
`sudo tools/leak_test/leaktest.sh traffic --browser <the build>`
([LEAK_TESTING.md](LEAK_TESTING.md#regular-window-traffic-capture),
`docs/BUILD_MACHINE_CHECKLIST.md` §8). It starts the browser with a fresh
profile in the kit's network namespace and takes it through a first run,
10 minutes idle, a scripted browse of the kit's local pages (a form, a
French page, misspelt text, a program download, a name that doesn't
resolve, an expired certificate), a restart and MoltBrowser's update check.
Every name it looks up gets an address in a sinkhole on the namespace's
loopback, which records each connection (server name, port, HTTP request
line) and answers 503, so nothing reaches Google or anyone else. Its report
compares what happened with [section 6](#6-machine-readable-host-list) and
fails on anything else, with the time, the step and the likely source.
Because nothing answers, it sees the first request of each service, not
what a successful answer would lead to (no component downloads, no update
download), and not request bodies.

**The full capture** below stays the procedure for macOS, for request
bodies, and for what follows a real answer. The MoltNet leak run
(`leaktest.sh run`) also lists the regular profile's background lookups it
sees (`tools/leak_test/leakkit/background.py`, kept in line with section 6
by the kit's tests).

### 1. Setup

```bash
APP="/Applications/MoltBrowser.app/Contents/MacOS/MoltBrowser"   # Linux: chromium/src/out/MoltBrowser-linux/chrome
RUN=$(date +%Y%m%d-%H%M); OUT=~/molt-traffic/$RUN; mkdir -p "$OUT"
PROFILE=$(mktemp -d)                                              # fresh, never signed in
```

Do **not** pass `--disable-field-trial-config`. The point is to measure what
users actually run.

### 2. Record three views at once

1. **Packet capture**, which sees everything, including traffic that skips any
   proxy: DNS probes to 8.8.8.8, plain-HTTP time queries, DoH, Tor.
   ```bash
   sudo tcpdump -i any -w "$OUT/all.pcap" &       # macOS: -i en0 (or pktap)
   ```
2. **Chromium NetLog**: every request the browser makes, with full URLs.
   No root and no TLS interception needed.
   ```bash
   "$APP" --user-data-dir="$PROFILE" --log-net-log="$OUT/netlog.json" \
          --net-log-capture-mode=Everything --remote-debugging-port=9222 &
   ```
3. **Request bodies (optional second run)**: mitmproxy in regular mode,
   with its CA trusted by the OS (macOS Keychain, or the NSS db on Linux:
   `certutil -d sql:$HOME/.pki/nssdb -A -t C,, -n mitm -i ~/.mitmproxy/mitmproxy-ca-cert.pem`).
   Add `--proxy-server=http://127.0.0.1:8080` to the command above. Use this
   to read what the component-updater, Safe Browsing, autofill and leak-check
   requests contain. DNS probes and DoH don't go through the proxy; the pcap
   catches them.

### 3. Script

| Time | Action | What it exercises |
|---|---|---|
| 0–10 min | Launch and leave idle on `molt://home` | Startup and idle: component updater (~1 min; none of the components section 6 lists, and no on-device model manifest), Safe Browsing (1–5 min), network time, spell-check (Linux), MoltShield lists (10 s), update check (30 s) |
| 10 min | Open `https://example.com` | First navigation |
| +1 min | `https://github.com/login`, then submit fake credentials on a local test page with a password form (`python3 -m http.server`) | Autofill query (must be absent), leak check (must be absent) |
| +1 min | `https://www.lemonde.fr`, then click Translate (with an on-device model) | Translate ranker, language list, `translate_a/element.js` and any `translate.googleapis.com` / `translate-pa.googleapis.com` request (all must be absent) |
| +1 min | `https://does-not-exist.invalid` | DNS probe to 8.8.8.8 (must be absent) |
| +1 min | `https://expired.badssl.com` | Network time on demand, captive portal (must be absent) |
| +1 min | Download a small signed `.dmg`/`.exe`/`.deb` from a known site | Safe Browsing download ping (expected while SB is on) |
| +1 min | Type a few letters in the address bar | DuckDuckGo suggestions (expected) |
| then 10 min | Idle | Re-check timers, Safe Browsing back-off |

Drive it by hand or with CDP on port 9222 (e.g. a short Puppeteer script that
`page.goto`s the list). Record wall-clock times so pcap and NetLog line up.

### 4. Reduce

```bash
# Hosts from the NetLog (requests the browser made)
python3 -c 'import json,sys,urllib.parse as u;d=json.load(open(sys.argv[1]))
hosts={}
for e in d["events"]:
  url=e.get("params",{}).get("url")
  if url: hosts.setdefault(u.urlsplit(url).hostname,url)
[print(h,"\t",hosts[h][:120]) for h in sorted(filter(None,hosts))]' "$OUT/netlog.json"

# DNS names, TLS SNI, plain-HTTP hosts, raw IPs from the pcap
tshark -r "$OUT/all.pcap" -Y 'dns.flags.response==0 or tls.handshake.type==1 or http.request' \
  -T fields -e frame.time_relative -e ip.dst -e dns.qry.name \
  -e tls.handshake.extensions_server_name -e http.host -e http.request.uri | sort -u
```

Also save `molt://version/?show-variations-cmd` (or `chrome://version` if the
`molt://` alias isn't there) to record which testing-config studies were
active.

### 5. Pass criteria

- **Must not appear** in a default run: every host section 6 marks
  `forbidden` or `user` (the second only after the user action named
  there), and any host not on this page. The component updater's request
  must not name a component section 6 lists.
- **Expected**: the hosts section 6 marks `allowed`, and `sb-ssl.google.com`
  during the download (record the HTTP status of the Google ones; we
  expect 400/403 for `key=dummytoken`); `duckduckgo.com` only after typing.
- **Overrides still yield to the user.** Relaunch with
  `--enable-features=AutofillServerCommunication`: the autofill query should
  reappear; with `--enable-features=MoltGoogleOnlyComponents` the four
  Google-only components are back in the update request.

Record the date and build next to each row above, and change its confidence
to **Captured** with the date.

## Keeping this page true

- Re-run the capture after every Chromium rebase (`docs/REBASING.md`). The
  testing config, components and defaults change with every version.
- A new host goes into the table above **and** section 6; then run
  `tools/leak_test/leaktest.sh hosts --write` and commit
  `docs/network_traffic_hosts.json` with it. The kit's tests catch a table
  and a JSON that disagree.
- A new feature that talks to a Google host without a key check belongs in
  `molt_feature_overrides.cc`, not in a `BASE_FEATURE` flip in an upstream
  file. The testing config can undo a flip; it cannot undo an override.
- A component upstream registers unconditionally for a Google-only
  feature is skipped in `registration.cc` behind
  `ShouldRegisterGoogleOnlyComponents()`
  (`src/chrome/browser/molt_ai/privacy/molt_component_policy.h`), and its
  ID goes into section 6's component table so the capture checks for it.
  Compute the ID from the installer's public key hash (the first 16 bytes,
  each half-byte as a letter from `a` to `p`).
