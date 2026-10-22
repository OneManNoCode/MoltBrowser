# What MoltBrowser contacts without being asked

This page lists every host that MoltBrowser connects to on its own. It does
not list the sites you visit or requests you start yourself. The goal is to
be honest. Where we have only read the code and not yet watched the traffic,
the page says so.

**Scope.** Desktop builds (macOS arm64, Linux x64, Windows x64) at Chromium
148.0.7729.0 (`51a413be`, see `chromium_version.txt`), on a new profile that
is not signed in to anything, with default settings. That covers startup,
idle time, the first navigation and every navigation after it. Android and
iOS are **not covered**: they use different services (for example Google
Play Services for Safe Browsing on Android) and have not been audited.

**How sure we are.** Each row says one of:

- **Source**: confirmed by reading Chromium's source at the pin. The file and
  line are given, relative to the Chromium tree; MoltBrowser's own files start
  with `src/`.
- **Likely**: the code clearly points this way, but we did not follow every
  call.
- **Capture**: has to be confirmed by watching real traffic. The plan is in
  [How we'll verify](#how-well-verify).

No row has been confirmed by a traffic capture yet.

## Summary

| Host | Why | In MoltBrowser |
|---|---|---|
| `update.googleapis.com` and Google's download hosts | Component updates: certificate revocation (CRLSet), Certificate Transparency log list, Chrome Root Store, Safety Tips, download file-type rules and more | **On**, for security |
| `clients2.google.com/time` | Network time, used to check certificate dates when your clock is wrong | **On**, for security |
| `safebrowsing.googleapis.com` | Safe Browsing list updates | **On, but it doesn't work** (no API key). See [S17 memo](SAFE_BROWSING_DECISION.md) |
| `sb-ssl.google.com` | Safe Browsing check of downloaded programs | **On, but it doesn't work** (no API key). Sends the download URL. See S17 memo |
| `redirector.gvt1.com` | Spell-check dictionary download (Linux, some Windows languages) | **On**, so spell-check works |
| `translate.googleapis.com` | List of languages Translate supports | **On** when Translate prompts |
| `content-autofill.googleapis.com` | Autofill form-type hints, on every page with a form | **Off** (changed in this audit) |
| `www.gstatic.com` (translate model) | Model that decides when to offer Translate | **Off** (changed in this audit) |
| `8.8.8.8`, `8.8.4.4` (Google Public DNS) | DNS check after a "site can't be reached" error | **Off** (changed in this audit) |
| `connectivitycheck.gstatic.com` | Wi-Fi login-page (captive portal) check after certificate errors and timeouts | **Off** (changed in this audit) |
| `passwordsleakcheck-pa.googleapis.com` | Checks whether a password you submit has leaked | **Off** (changed in this audit) |
| `duckduckgo.com` | Default search engine; suggestions while you type in the address bar | **On** (only while you type) |
| `api.github.com`, `github.com` | MoltBrowser update check | **On** |
| `easylist.to`, `ublockorigin.github.io`, `raw.githubusercontent.com` | MoltShield ad- and tracker-blocking lists | **On** |

Every other Google service we found is **off** in this build. That includes
variations/Finch, usage statistics, crash reports, sign-in and sync, Google
push messaging, Domain Reliability, surveys, optimization hints, real-time
Safe Browsing lookups, password affiliations, Google's New Tab Page, AI Mode
and What's New. The reasons are in the detailed tables below.

## The build this applies to

`configs/*.gn` and `scripts/configure.sh` set these values:

| Setting | macOS (all), Linux, Windows | Android | Effect |
|---|---|---|---|
| `is_chrome_branded` | `false` | `false` | Turns off every `GOOGLE_CHROME_BRANDING` code path (several listed below) |
| `google_api_key`, client id, client secret | `""` | `""` | The API key becomes the placeholder `"dummytoken"` (`google_apis/default_api_keys.h:18`, `default_api_keys-inc.cc:18-20`), so `HasAPIKeyConfigured()` is false (`api_key_cache.cc:323-325`). Features that check this stay off. Features that don't check it **still send their request, with `key=dummytoken`**. |
| `safe_browsing_mode` | `1` | `0` | Mode 1 builds the full desktop Safe Browsing service; mode 0 leaves it out. The Android configs are scaffolding (no real Android build yet) |
| `enable_reporting` | `false` | `false` | Leaves out Network Error Logging and the Reporting API |
| `enable_widevine` | not set → `false` | Android: `true` | `third_party/widevine/cdm/widevine.gni:15-16`: Widevine is Chrome-branded on desktop. No Widevine component is downloaded, and DRM video (Netflix and similar) does not play |
| `is_official_build` | `true` (Windows: `false`) | `true` | Doesn't change any network behaviour described here |
| `disable_fieldtrial_testing_config` | not set → `false` | not set | **Applies Chromium's field-trial testing config at runtime.** See below |

**The field-trial testing config.** Builds without Chrome branding compile in
`testing/variations/fieldtrial_testing_config.json` and apply it at startup,
unless `--disable-field-trial-config` is passed. The relevant code is
`components/variations/service/BUILD.gn:14,25-27` and
`variations_field_trial_creator.cc:135-147,288-293`. At the pin, this turns
on 760–820 features and turns off about 50 on each desktop platform. They
include the Privacy Sandbox ad APIs, the k-anonymity service, Gemini in
Chrome (`Glic`), Lens features, preconnect-to-search and survey triggers. So
the features MoltBrowser actually runs with are **not** simply the defaults in
the code.

This also explains why some of MoltBrowser's earlier changes don't take
effect. At 148, `omnibox_features.cc` turned `AiModeOmniboxEntryPoint` off in
the code, but the `AIMHintText` study turned it back on; at 152 the omnibox
AI Mode button for non-Google search engines is `Aim3pEntrypoint`, which the
`Aim3pEntrypoint` study turns on. The overrides added in this
audit are registered *before* the testing config. A study that touches one of
those features is then skipped entirely
(`components/variations/field_trial_config/field_trial_util.cc:86-99`). See
[Changes made in this audit](#changes-made-in-this-audit).

## 1. Google hosts contacted with no user action (startup and idle)

| Endpoint | Purpose | What's sent | When | MoltBrowser default | How to turn off | Confidence |
|---|---|---|---|---|---|---|
| `https://update.googleapis.com/service/update2/json` (falls back to `http://` on the same host) | Component updater: one combined update check for every registered component | JSON request containing OS, OS version, CPU architecture, CPU features (SSE/AVX), RAM in GB, browser version and channel, UI language, each component's ID and version, coarse day counters (`rd`/`ad`/`ping_freshness`) and random per-request IDs. No cookies and no account | 1 min after start, then every 5 h | **On (needed for security).** Registered on every desktop start: CRLSet (certificate revocation), PKI Metadata (CT log list, key-pinning list, Chrome Root Store updates), Safety Tips, File Type Policies (download danger levels), SSL Error Assistant, Origin Trials keys, Crowd Deny, Trust Token keys, zxcvbn password-strength data, actor safety lists, CAPTCHA providers, amount-extraction regexes and the WASM TTS engine. Registered when their feature or platform says so: Subresource Filter rules, First-Party/Related Website Sets, Privacy Sandbox attestations and Optimization Hints (all on by default), plus MEI preload, on-device head suggest, hyphenation, IWA keys and commerce heuristics. On-demand components (Screen AI, Live Caption, TranslateKit, on-device model) come only when a feature asks for them. Web pages can no longer ask for either of the last two: the Translator web API is off by default, and the other built-in AI APIs run on MoltBrowser's own model (`docs/design/WEB_AI_APIS.md`). The exact list is **Capture** (it appears in the request) | `--disable-component-update`. **Not recommended:** revocation, CT and root-store updates stop | **Source**: `chrome/browser/chrome_browser_main.cc:1867-1869`; `components/component_updater/configurator_impl.cc:55-91`; `component_updater_url_constants.cc:17-21`; `components/update_client/protocol_serializer_json.cc:30-243`; `chrome/browser/component_updater/registration.cc:134-231` |
| Component download hosts named in the update response, usually `*.gvt1.com`, `dl.google.com` and `edgedl.*` | Downloads a component's new version | A file request (the component's CRX or diff). The CRX is signed and checked | Only when a component has an update | On | As above | **Capture** for the exact hosts |
| `http://clients2.google.com/time/1/current` | Network time. Lets certificate checks still work when the system clock is wrong. Also used to tell "your clock is wrong" apart from a real certificate error | Only a signed nonce (`cup2key`/`cup2hreq`). The response is signed, so it cannot be faked even over HTTP | At startup when no valid saved time exists (first run, after a reboot or clock change). Otherwise ~1.2% of checks made every 6 min, about once per 8 h of use. Also on certificate date errors | **On (needed for security).** Network time feeds certificate verification | `--disable-features=NetworkTimeServiceQuerying`, or the `BrowserNetworkTimeQueriesEnabled` policy | **Source**: `components/network_time/network_time_tracker.cc:62-83,134,269,474,617-643`; `chrome/browser/net/cert_verifier_service_time_updater.cc:13-30`; `net/cert/cert_verify_proc_builtin.cc:1596-1606` |
| `https://safebrowsing.googleapis.com/v4/threatListUpdates:fetch?...&key=dummytoken` | Safe Browsing V4 list updates | Client name `chromium`, version, the list names with their (empty) states, extended-reporting level | 1–5 min after start. Each failure backs off 15–30 min, doubling up to 24 h | **On, but no protection.** Without a valid key the lists never download, so Safe Browsing blocks nothing. See the [S17 memo](SAFE_BROWSING_DECISION.md) | Settings → Privacy and security → Security → *No protection* | **Source**: `components/safe_browsing/core/browser/db/v4_protocol_manager_util.cc:36,239-251`; `v4_protocol_config.cc:27-33`; `chrome/browser/safe_browsing/chrome_v4_protocol_config_provider.cc:20-49`; `v4_update_protocol_manager.cc:76-79,114-116,139-150`; `v4_local_database_manager.cc:578-589,671-684`. **Capture**: confirm Google rejects the placeholder key |
| `https://redirector.gvt1.com/edgedl/chrome/dict/<lang>-<ver>.bdic` | Hunspell spell-check dictionary | The dictionary language and version (in the URL) | Linux, and on Windows only for languages the system checker lacks. When spell-check starts and the dictionary isn't on disk yet (normally once per language). macOS uses the system checker and never downloads | **On**: without it there is no spell-check on Linux | Settings → Languages → Spell check off | **Source** for the URL and platforms: `chrome/browser/spellchecker/spellcheck_hunspell_dictionary.cc:280-283`; `components/spellcheck/spellcheck_build_features.gni:13`. **Capture** for timing |

## 2. Google hosts contacted as a side effect of browsing

| Endpoint | Purpose | What's sent | When | MoltBrowser default | How to turn off / on | Confidence |
|---|---|---|---|---|---|---|
| `https://content-autofill.googleapis.com/v1/pages/...` | Autofill crowdsourcing: asks for field-type predictions, and uploads "votes" on submit | Hashed form and field signatures (form action host, form and field names). Votes add what the field turned out to be. Sent with `X-Goog-Api-Key: dummytoken`. Nothing in the code checks for a key | Every http(s) page with a form that has a password field or enough fillable fields | **Off** (changed in this audit). Autofill uses local heuristics | Back on with `--enable-features=AutofillServerCommunication` | **Source**: `components/autofill/core/browser/crowdsourcing/autofill_crowdsourcing_manager.cc:110-111,139-171,563-573,701-705,969-971`; `autofill_debug_features.cc:113`; `foundations/autofill_manager.cc:300-323,326-370`; `form_qualifiers.cc:180-185` |
| `https://www.gstatic.com/chrome/intelligence/assist/ranker/models/translate/...` | Translate Ranker model, which tunes how often Translate is offered | A file request | When the ranker is first created (the first page Translate evaluates), then whenever the cached model expires | **Off** (changed in this audit). The ranker then always answers "offer", which is upstream's behaviour before the model arrives | Back on with `--enable-features=TranslateRankerQuery,TranslateRankerEnforcement` | **Source**: `components/translate/core/browser/translate_ranker_impl.cc:83-96,149-170,222-243` |
| `https://translate.googleapis.com/translate_a/l?client=chrome&hl=..&key=dummytoken` | The list of languages Translate supports | UI language, placeholder key | When Translate UI or the language settings ask for the supported languages | **On.** Whether it works without a key is unknown | Settings → Languages → Google Translate off | **Likely**: `translate_language_list.cc:308,344-355,388-420`; `translate_prefs.cc:502-504`. **Capture** for timing |
| `https://translate.googleapis.com/translate_a/element.js` and the translation calls | Translating a page | The page text, sent to Google | **Only when you click Translate** | On (your action) | Don't click Translate, or turn it off as above | **Source** for the URL: `translate_script.cc:36,121-122`. **Capture**: does it work without a key? |
| DNS query for `google.com` sent straight to `8.8.8.8` / `8.8.4.4` | Error-page DNS probe: is it your DNS or the whole internet? | The query, from your IP, to Google Public DNS | After a "site can't be reached" (DNS) error | **Off** (changed in this audit: `alternate_error_pages.enabled` now defaults to false) | Pref `alternate_error_pages.enabled` or policy `AlternateErrorPagesEnabled` | **Source**: `chrome/browser/net/dns_probe_service_factory.cc:47-48,63-74`; `dns_probe_runner.cc:21,105`; `net_error_tab_helper.cc:194-203,254-259`; `profile_network_context_service.cc:615-616` |
| `http://connectivitycheck.gstatic.com/generate_204` | Captive-portal (hotel Wi-Fi login) detection | An empty request | After certificate errors and slow loads, on desktop | **Off**: the same pref as above. Your operating system's own captive-portal detection still works | Same pref or policy | **Source**: `components/captive_portal/core/captive_portal_detector.cc:24-26,40-41`; `core/features.cc:9`; `content/captive_portal_service.cc:86-87,240-242`; `core/features.gni:9` |
| `http://clients2.google.com/time/1/current` | Network time (row in section 1) | Nonce | On certificate date errors, also a MoltNet window's (outside Tor, MOLTNET_COMPLIANCE.md §5.7) | On | As in section 1 | **Source** (section 1) |
| `https://sb-ssl.google.com/safebrowsing/clientreport/download?key=dummytoken` | Safe Browsing download protection | **The download URL, the chain of pages that led to it (referrers), file name, size, SHA-256 and signature details** | When you download a file type Safe Browsing checks (executables, archives, disk images, ...) | **On while Safe Browsing is on**; never from MoltNet windows, where it is off (MOLTNET_COMPLIANCE.md §2). Without a key it can't return a verdict. See the S17 memo | Safe Browsing → *No protection* | **Source**: `chrome/browser/safe_browsing/download_protection/download_protection_delegate_desktop.cc:26-51,64-81`; `check_client_download_request_base.cc:501`. **Capture**: confirm the request goes out and is rejected |
| `https://sb-ssl.google.com/safebrowsing/clientreport/login` | Safe Browsing password-reuse ("phishing") report | The reuse event and the page URL | Only after you type a saved password on a different site while Safe Browsing is on | On while Safe Browsing is on; never from MoltNet windows | Safe Browsing → *No protection* | **Likely** (not traced) |
| `https://passwordsleakcheck-pa.googleapis.com/v1/leaks:lookupSingle` | Password leak check | A short hash prefix of the username and a blinded hash of username+password. For signed-out users the only credential is the API key (placeholder) | On every password-form submission | **Off** (changed in this audit: `profile.password_manager_leak_detection` now defaults to false) | Password Manager settings → "Warn you if passwords are exposed in a data breach" | **Source**: `components/password_manager/core/browser/leak_detection/leak_detection_check_factory_impl.cc:27-51`; `leak_detection_request.h:39-40`; `leak_detection_delegate.cc:59-77`; `leak_detection_check_impl.cc:226-238` |

## 3. Google services that are off in this build

| Service (host) | Why it is off | Confidence |
|---|---|---|
| Variations / Finch seed (`clientservices.googleapis.com/chrome-variations/seed`) | Builds without Chrome branding fetch it only with `--variations-server-url` | **Source**: `components/variations/service/variations_service.cc:226-243,393-397`; `variations_url_constants.cc:10-14` |
| Usage statistics: UMA/UKM (Google's metrics servers; host not traced) | Metrics reporting is hard-off without Chrome branding | **Source**: `components/metrics/metrics_service_accessor.cc:30-43` |
| Crash reports (Google's crash server; host not traced) | Upload consent is false without Chrome branding | **Source**: `chrome/app/chrome_crash_reporter_client.cc:171-201` |
| Domain Reliability beacons | Needs metrics consent | **Source**: `chrome/browser/domain_reliability/service_factory.cc:26-42` |
| Surveys / HaTS | Need metrics consent. The testing config turns survey triggers on, but they never pass this check | **Source**: `chrome/browser/ui/hats/hats_service_desktop.cc:550-557`. That the consent check reaches `metrics_service_accessor.cc:30-43` is **Likely** |
| Optimization Guide hints and model downloads (`optimizationguide-pa.googleapis.com`) | Need a configured API key | **Source**: `components/optimization_guide/core/optimization_guide_permissions_util.cc:29-46`; `delivery/prediction_model_download_manager.cc:200-209`. The Optimization Hints *component* still comes through the component updater (section 1) |
| Safe Browsing hash-prefix real-time lookups (relay `google-ohttp-relay-safebrowsing.fastly-edge.com`, key config `safebrowsingohttpgateway.googleapis.com`) | Chrome-branded only. The testing config turns the feature on, but the branding check still wins | **Source**: `components/safe_browsing/core/common/hashprefix_realtime/hash_realtime_utils.cc:26-32,79-82`; `core/common/features.cc:283-295` |
| Safe Browsing real-time URL checks and Enhanced protection pings | Enhanced protection is not the default | **Likely** |
| Password affiliation lookups (`www.googleapis.com/affiliation/v1/`) | Need a configured API key | **Source**: `components/affiliations/core/browser/hash_affiliation_fetcher.cc:189-197` |
| Google sign-in, account cookie checks (`accounts.google.com`), Sync | No OAuth client is configured, so desktop sign-in cannot turn on. MoltBrowser also defaults sign-in to disallowed and Sync to managed-off | **Source**: `chrome/browser/signin/account_consistency_mode_manager.cc:66-81,96-104,124-134`; MoltBrowser `src/components/signin/internal/identity_manager/primary_account_manager.cc`, `src/components/sync/service/sync_prefs.cc`. **Capture**: confirm `accounts.google.com` never appears |
| Push messaging / GCM (Google's push servers, e.g. `mtalk.google.com`; hosts not traced) | Delayed start: nothing connects until a site or extension subscribes to push | **Source**: `components/gcm_driver/gcm_client_impl.cc:262,387` |
| Google New Tab Page (OneGoogleBar, doodle, promos) | The new tab page is MoltBrowser's local `molt://home`. Google's widgets are forced off | **Source**: `src/chrome/browser/search/search.cc:176,385`; `src/chrome/browser/ui/webui/new_tab_page/new_tab_page_ui.cc:254-258` |
| AI Mode eligibility (`www.google.com/async/folae`) | `AimEnabled` is off in MoltBrowser, and the request needs Google as the search engine | **Source**: `components/omnibox/browser/aim_eligibility_service.cc:384-386,938-940` |
| Suggestions when you focus the address bar (zero-suggest) | Only sent when Google is the search engine | **Source**: `components/omnibox/browser/base_search_provider.cc:441-446` |
| Preconnect to the search engine | Network prediction is off by MoltBrowser default (and the engine is DuckDuckGo) | **Source**: `chrome/browser/navigation_predictor/search_engine_preconnector.cc:239,418-421`; `chrome/browser/predictors/loading_predictor_config.cc:20-25` |
| What's New page | Chrome-branded only | **Source**: `chrome/browser/ui/webui/whats_new/whats_new_util.cc:35-39` |
| Preinstalled Google web apps | Chrome-branded only | **Source**: `chrome/browser/web_applications/preinstalled_web_apps/preinstalled_web_apps.cc:21-49` |
| Hangout Services and Network Speech Synthesis component extensions | Need Google-internal sources / Chrome branding | **Source**: `chrome/common/features.gni:57`; `chrome/browser/extensions/component_loader.cc:606-608` |
| Recovery component, Widevine component | Chrome-branded only / not built (see build table) | **Source**: `chrome/browser/component_updater/recovery_improved_component_installer.cc:12,214-240`; `widevine.gni:15-16,44-46` |
| Gemini in Chrome (`Glic`) | Needs a signed-in Google account. The testing config turns its master switch on | **Source**: `chrome/browser/glic/public/glic_enabling.cc:278-296`. **Capture**: confirm no `gemini.google.com` |
| Lens | Needs Google as the search engine | **Likely** |
| Privacy Sandbox ad APIs (Topics, Protected Audience, k-anonymity) | The testing config turns the features on, but the APIs still wait for the consent prefs (`privacy_sandbox_settings_impl.cc:249-253`), which nothing in MoltBrowser turns on. We believe the k-anonymity service also needs sign-in (not checked) | **Likely**. **Capture** |
| Profile avatar images (`www.gstatic.com/chrome/profile_avatars/`) | New profiles use the built-in placeholder. A download happens only if you pick another avatar | **Source**: `chrome/browser/profiles/profile_manager.cc:1256`; `profile_attributes_storage.cc:962-976`; `profile_avatar_downloader.cc:22-23` |
| Chrome Web Store updates (`clients2.google.com/service/update2/crx`) | Only once a Web Store extension is installed | **Likely**: `extensions/common/extension_urls.cc:46`; `chrome/browser/extensions/updater/extension_updater.cc:436,573-614`. **Capture** |
| Location (`www.googleapis.com/geolocation/v1/geolocate`), speech recognition (`www.google.com/speech-api/`) | Only after a site uses them and you grant permission. Expected to fail without a key. macOS uses the system location service | **Likely** |
| DevTools remote front-end (`chrome-devtools-frontend.appspot.com`), console insights | Only when you use those DevTools features | **Likely** |

**DNS over HTTPS.** The default is "automatic". The browser upgrades to DoH
only if your current DNS provider offers it, and uses that same provider. Its
test queries look up `www.gstatic.com`, but they go to *your* DNS provider,
not to Google (`chrome/browser/net/default_dns_over_https_config_source.cc:25-28`;
`net/dns/dns_transaction.h:30`). Falling back to a different DoH provider
needs Enhanced protection or a policy
(`chrome/browser/net/stub_resolver_config_reader.cc:143-175`).
**Source**.

## 4. Non-Google hosts that are part of browsing defaults

| Host | Purpose | When | Confidence |
|---|---|---|---|
| `duckduckgo.com` (`/?q=`, `/ac/?q=`) | Default search engine and its suggestions | Only while you type in the address bar or search | **Source**: `src/third_party/search_engines_data/resources/definitions/prepopulated_engines.json:173-184` |

MoltBrowser puts DuckDuckGo in Chromium's default slot (prepopulated id 1, the
entry still called `"google"`). Chromium still recognises it as DuckDuckGo,
not Google. It finds an engine by its URL and checks the sorted engine list,
where `duckduckgo` comes before `google`, so every "is the search engine
Google?" feature stays off. **Source**:
`components/search_engines/template_url.cc:1973-1981`;
`search_engine_utils.cc:44-70`; `tools/json_to_struct/aggregation.py:25-27`.

Chromium's per-country engine list
(`third_party/search_engines_data/resources/definitions/regional_settings.json`,
not in MoltBrowser's overlay) may put a different engine first in some
countries. We have not checked this.

## 5. MoltBrowser's own traffic

| Host | Purpose | What's sent | When | Default | How to turn off | Confidence |
|---|---|---|---|---|---|---|
| `api.github.com/repos/OneManNoCode/MoltBrowser/releases/latest`; if that fails, `github.com/OneManNoCode/MoltBrowser/releases/latest` (redirect only) | Update check | User-Agent `MoltBrowser/<version>`, no cookies | 30 s after launch, then every 6 h | On | No switch yet. The auto-update toggle in `molt://ai-update` stops automatic *downloads*, not the check | **Source**: `src/chrome/browser/molt_ai/update/update_manager.cc:49-69,154-215,326-345` |
| `github.com/OneManNoCode/MoltBrowser/releases/download/...`, which redirects to GitHub's asset host (`*.githubusercontent.com`) | Downloading an update, its signed manifest and signature | Same User-Agent | When an update exists and auto-update is on, or you click Update | On | Auto-update toggle | **Source** (URLs); **Capture** for the CDN host |
| `easylist.to`, `ublockorigin.github.io`; fallback `raw.githubusercontent.com`; optional lists from `secure.fanboy.co.nz` | MoltShield filter lists | Plain GET with `If-None-Match` / `If-Modified-Since`, no cookies. Fetched through the profile's network (through Tor in MoltNet) | 10 s after the first run, then about once a day with random jitter | On | Turn lists off in `molt://ai-settings`, or `--disable-molt-shield` | **Source**: `src/chrome/browser/molt_ai/shield/filter_list_catalog.cc:16-52`; `filter_list_manager.cc:33-60,96-113,250-253` |
| `huggingface.co` and its download CDN | On-device model download (chat models, and the memory search model from molt://memory) | File request | Only when you download a model | Off until you ask | — | **Source**: `src/chrome/browser/molt_ai/models/model_downloader.cc`, `src/chrome/browser/molt_ai/memory/embedding_model_manager.cc` |
| `api.openai.com`, `api.anthropic.com`, `generativelanguage.googleapis.com`, `api.mistral.ai`, `api.groq.com`, `api.deepseek.com`, `api.x.ai`, `api.perplexity.ai`, `openrouter.ai`, or a custom endpoint | Cloud AI chat | Your message and any page context you attach | Only with your own key, per message | Off until you add a key | Remove the key | **Source**: `src/chrome/browser/molt_ai/cloud/cloud_provider.cc` |
| Tor directory authorities and relays; `check.torproject.org/api/ip` | MoltNet (Tor) windows | Tor traffic | Only after you open a MoltNet window | Off until you ask | — | **Source**: `src/chrome/browser/molt_ai/tor/`, `src/chrome/browser/ui/webui/molt_ai/molt_net_handler.cc` |

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
   - `Aim3pEntrypoint` off (it was `AiModeOmniboxEntryPoint` before
     Chromium 152): keeps the omnibox AI Mode button off, which the
     `Aim3pEntrypoint` testing-config study would otherwise turn on.
2. **Default pref values**
   (`src/chrome/browser/molt_ai/privacy/molt_privacy_prefs.cc`):
   - `alternate_error_pages.enabled = false`: no DNS probe to Google Public
     DNS and no captive-portal check against gstatic.
   - `profile.password_manager_leak_detection = false`: no leak checks that
     only a Google API key could authenticate.

**Left on deliberately:**
- **Component updater and network time.** They carry security data
  (revocation, CT, root store, safe file types, certificate times) and reveal
  little: your IP, OS and version, and component versions.
- **Spell-check dictionary downloads.** These are what make spell-check work
  on Linux.

Safe Browsing is a product decision; see the
[S17 memo](SAFE_BROWSING_DECISION.md).

## How we'll verify

Run this on the build machine for every release candidate, on macOS arm64
first, then Linux. It turns every **Likely** and **Capture** above into a
measured result.

**Partly automated on Linux.** The MoltNet leak kit
([LEAK_TESTING.md](LEAK_TESTING.md)) logs every name the browser looks up
outside Tor during its run and checks it against this page:
`tools/leak_test/leakkit/background.py` mirrors the Summary table and the
pass criteria below. A host that must not appear fails, and a host on no
list has to be triaged (added here and there, with the reason, or treated as
a leak). Because the kit lets nothing but Tor out, it only sees the first
lookup of each host, not URLs, request bodies, timing over hours, or what a
successful request would lead to. The capture below remains the full
procedure.

### 1. Setup

```bash
APP="/Applications/MoltBrowser.app/Contents/MacOS/MoltBrowser"   # Linux: out/MoltBrowser/chrome
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
| 0–10 min | Launch and leave idle on `molt://home` | Startup and idle: component updater (~1 min), Safe Browsing (1–5 min), network time, spell-check (Linux), MoltShield lists (10 s), update check (30 s) |
| 10 min | Open `https://example.com` | First navigation |
| +1 min | `https://github.com/login`, then submit fake credentials on a local test page with a password form (`python3 -m http.server`) | Autofill query (must be absent), leak check (must be absent) |
| +1 min | `https://www.lemonde.fr` | Translate ranker (must be absent), translate language list |
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

- **Must not appear** in a default run: `content-autofill.googleapis.com`;
  `www.gstatic.com/chrome/intelligence/assist/ranker`; DNS to
  `8.8.8.8`/`8.8.4.4`; `connectivitycheck.gstatic.com`/`www.gstatic.com/generate_204`;
  `passwordsleakcheck-pa.googleapis.com`;
  `clientservices.googleapis.com`; `accounts.google.com`;
  `optimizationguide-pa.googleapis.com`; `www.googleapis.com/affiliation`;
  `mtalk.google.com`; `gemini.google.com`; `www.google.com/async/*`; any host
  not in this document.
- **Expected**: `update.googleapis.com` and the component download hosts;
  `clients2.google.com/time`; `safebrowsing.googleapis.com` and
  `sb-ssl.google.com` (record the HTTP status; we expect 400/403 for
  `key=dummytoken`); `redirector.gvt1.com` (Linux); `api.github.com`,
  `github.com`; the MoltShield list hosts; `duckduckgo.com` only after typing.
- **Overrides still yield to the user.** Relaunch with
  `--enable-features=AutofillServerCommunication`: the autofill query should
  reappear.

Record the date and build next to each row above, and change its confidence
to **Captured** with the date.

## Keeping this page true

- Re-run the capture after every Chromium rebase (`docs/REBASING.md`). The
  testing config, components and defaults change with every version.
- A new feature that talks to a Google host without a key check belongs in
  `molt_feature_overrides.cc`, not in a `BASE_FEATURE` flip in an upstream
  file. The testing config can undo a flip; it cannot undo an override.
