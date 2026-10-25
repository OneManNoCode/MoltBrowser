// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Entry point for MoltBrowser's engine-level fingerprinting protection. The
// embedder (chrome/renderer/molt_ai/fingerprinting/) calls, on the main
// thread:
//   * ConfigureBeforeBlinkInitialization() once, before blink::Initialize()
//     (SetRuntimeFeaturesDefaultsBeforeBlinkInitialization), with the same
//     mode, for what has to be set before V8 starts;
//   * Configure() once at renderer start (RenderThreadStarted) with the
//     profile's mode, which the browser put on the command line;
//   * SetSessionSeed() with the profile's session seed, which the browser
//     sends over a channel-associated mojo message right after launch (the
//     seed is a secret and stays off the command line);
//   * SetDocumentShieldsDecision() / InheritDocumentShieldsDecision() for
//     every new document (RenderFrameObserver::DidCreateNewDocument), with
//     the Shields decision the browser sent for it, if any.
// A renderer process only ever hosts one BrowserContext, so a per-process
// mode and seed are exact. Shields decisions are per document (and per
// worker): no renderer is told the profile's list of Shields-off sites.

#ifndef THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_
#define THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/containers/span.h"
#include "third_party/blink/public/platform/web_common.h"

namespace base {
class File;
}  // namespace base

namespace blink {

class WebLocalFrame;

class BLINK_EXPORT WebMoltFingerprinting {
 public:
  enum class Mode {
    // No protection (feature disabled, or not a web-content profile).
    kOff,
    // Normal and Incognito windows: per-site, per-session farbling.
    kFarble,
    // MoltNet (Tor) windows: farbling plus uniform values (UTC, en-US,
    // letterboxed screen metrics, fixed hardwareConcurrency, no unmasked
    // WebGL renderer strings).
    kUniform,
  };

  // Must be called before blink::Initialize(), which initialises V8, with
  // the mode Configure() will get. Only kUniform does anything: V8 (flags,
  // every isolate's heap) and image colour transforms run as on one fixed
  // machine (core/molt_fingerprinting/moltnet_cpu.h). Needs no Blink object
  // or thread; only the first kUniform call has an effect.
  static void ConfigureBeforeBlinkInitialization(Mode mode);
  // Must be called on the main thread after blink::Initialize() and before
  // any script runs. Only the first call has an effect.
  static void Configure(Mode mode);
  // The profile's 32-byte session seed. Only the first call with exactly 32
  // bytes has an effect, and none while the mode is kOff. Until it arrives,
  // a context that needs a key gets a random one for its lifetime.
  static void SetSessionSeed(base::span<const uint8_t> session_seed);
  // The Shields decision for the document `frame` has just created (call
  // from RenderFrameObserver::DidCreateNewDocument, before any of its script
  // runs), as the browser sent it for the commit: whether the user turned
  // MoltShield off (`shields_down`) for `top_level_site`, the commit's
  // top-level site as net::SchemefulSite serializes it. In kFarble mode the
  // document, and the dedicated workers it creates, are then not farbled,
  // provided `top_level_site` is the document's own top-level site; anything
  // else farbles them. kUniform ignores it.
  static void SetDocumentShieldsDecision(WebLocalFrame* frame,
                                         bool shields_down,
                                         const std::string& top_level_site);
  // MoltNet renderers whose browser has MoltNet's bundled fonts (it puts
  // --molt-net-fonts on their command line): call on the main thread right
  // after Configure(kUniform), before any frame or worker exists. From then
  // on every font of the process comes from the bundled set, on every OS
  // (core/molt_fingerprinting/moltnet_fonts.h). No effect in other modes.
  static void ExpectBundledFonts();
  // The bundled set's files, read-only, in the order of its table, as the
  // browser sends them right after launch. Main thread; accepted once.
  static void SetBundledFontFiles(std::vector<base::File> files);
  // For a document of `frame` the browser didn't commit (the initial empty
  // document, and later ones in a frame that never got a decision): takes
  // the decision of the parent frame's document, or else of the opener's,
  // when that frame is in this process. Otherwise the document is farbled.
  static void InheritDocumentShieldsDecision(WebLocalFrame* frame);
};

}  // namespace blink

#endif  // THIRD_PARTY_BLINK_PUBLIC_WEB_WEB_MOLT_FINGERPRINTING_H_
