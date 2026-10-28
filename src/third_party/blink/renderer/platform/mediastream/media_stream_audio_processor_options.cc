// Copyright 2013 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "third_party/blink/renderer/platform/mediastream/media_stream_audio_processor_options.h"

#include "base/strings/stringprintf.h"
#include "base/strings/to_string.h"
#include "media/base/audio_parameters.h"
#include "media/base/media_switches.h"
#include "media/base/molt_portable_media.h"  // MoltBrowser

namespace blink {

namespace {

// MoltBrowser: the echo canceller of a MoltNet renderer (the portable media
// process, media/base/molt_portable_media.h) for every mode that asks for
// one: WebRTC's, in the audio service with the Chromium-wide reference where
// Chromium runs that (Windows, macOS and Linux), in the renderer otherwise.
// Never the OS's echo canceller, nor the OS's loopback as the reference:
// their samples differ between OSes and machines. A page that asks for "all"
// gets this one too, on every OS.
EchoCanceller::Type MoltNetEchoCanceller() {
  return media::IsChromeWideEchoCancellationEnabled()
             ? EchoCanceller::Type::kChromeWide
             : EchoCanceller::Type::kPeerConnection;
}

}  // namespace

const char kEchoCancellationModeAll[] = "all";
const char kEchoCancellationModeRemoteOnly[] = "remote-only";

const char* EchoCancellationModeToString(EchoCancellationMode ec_mode) {
  switch (ec_mode) {
    case EchoCancellationMode::kDisabled:
      return "disabled";
    case EchoCancellationMode::kBrowserDecides:
      return "browser-decides";
    case EchoCancellationMode::kAll:
      return kEchoCancellationModeAll;
    case EchoCancellationMode::kRemoteOnly:
      return kEchoCancellationModeRemoteOnly;
  }
}

// static
const AudioProcessingProperties& AudioProcessingProperties::Disabled() {
  static constexpr AudioProcessingProperties kDisabledProperties{
      .echo_cancellation_mode = EchoCancellationMode::kDisabled,
      .auto_gain_control = false,
      .noise_suppression = false,
      .voice_isolation = kVoiceIsolationDefaultValue};

  return kDisabledProperties;
}

bool AudioProcessingProperties::HasSameSessionIdentityProperties(
    const AudioProcessingProperties& other) const {
  return echo_cancellation_mode == other.echo_cancellation_mode
#if BUILDFLAG(CHROME_WIDE_ECHO_CANCELLATION)
         && voice_isolation == other.voice_isolation
#endif
      ;
}

bool AudioProcessingProperties::HasSameInterlockingProperties(
    const AudioProcessingProperties& other) const {
  return auto_gain_control == other.auto_gain_control &&
         noise_suppression == other.noise_suppression
#if !BUILDFLAG(CHROME_WIDE_ECHO_CANCELLATION)
         && voice_isolation == other.voice_isolation
#endif
      ;
}

std::string AudioProcessingProperties::ToString() const {
  auto str = base::StringPrintf(
      "echo_cancellation_mode: %s, "
      "auto_gain_control: %s, "
      "noise_suppression: %s, ",
      EchoCancellationModeToString(echo_cancellation_mode),
      base::ToString(auto_gain_control).c_str(),
      base::ToString(noise_suppression).c_str());
  return str;
}

// static
bool EchoCanceller::IsSystemWideAecAvailable(int available_platform_effects) {
  // MoltBrowser: "all" on every OS in MoltNet (MoltNetEchoCanceller()).
  if (media::IsPortableMediaProcess()) {
    return true;
  }
  return IsPlatformAecAvailable(available_platform_effects) ||
         media::IsSystemLoopbackAsAecReferenceEnabled();
}

// static
EchoCanceller EchoCanceller::From(const AudioProcessingProperties& properties,
                                  int available_platform_effects) {
  return From(properties.echo_cancellation_mode, available_platform_effects);
}

// static
EchoCanceller EchoCanceller::From(EchoCancellationMode mode,
                                  int available_platform_effects) {
  auto to_echo_canceller_type = [](EchoCancellationMode mode,
                                   int available_platform_effects) {
    switch (mode) {
      case EchoCancellationMode::kDisabled:
        return Type::kNone;
      case EchoCancellationMode::kBrowserDecides:
        return GetPreferredAec(available_platform_effects);
      case EchoCancellationMode::kRemoteOnly:
        return Type::kPeerConnection;
      case EchoCancellationMode::kAll:
        return GetSystemWideAec(available_platform_effects);
    }
  };

  return EchoCanceller(
      to_echo_canceller_type(mode, available_platform_effects));
}

// static
EchoCanceller EchoCanceller::MakeForTesting(EchoCanceller::Type type) {
  return EchoCanceller(type);
}

EchoCanceller::ApmLocation EchoCanceller::GetApmLocation() const {
  if (type_ == Type::kPeerConnection || type_ == Type::kPlatformProvided) {
    return ApmLocation::kRenderer;
  }
  if (type_ == Type::kChromeWide || type_ == Type::kLoopbackBased) {
    return ApmLocation::kAudioService;
  }
  if (media::IsChromeWideEchoCancellationEnabled()) {
    return ApmLocation::kAudioService;
  }
  return ApmLocation::kRenderer;
}

const char* EchoCanceller::ToString() const {
  switch (type_) {
    case Type::kNone:
      return "None";
    case Type::kPlatformProvided:
      return "PlatformProvided";
    case Type::kChromeWide:
      return "ChromeWide";
    case Type::kLoopbackBased:
      return "LoopbackBased";
    case Type::kPeerConnection:
      return "PeerConnection";
  };
}

// static
EchoCanceller::Type EchoCanceller::GetPreferredAec(
    int available_platform_effects) {
  // MoltBrowser: WebRTC's in MoltNet (MoltNetEchoCanceller()).
  if (media::IsPortableMediaProcess()) {
    return MoltNetEchoCanceller();
  }
  if (media::IsSystemLoopbackAsAecReferenceForcedOn()) {
    return Type::kLoopbackBased;
  }
  if (IsPlatformAecAvailable(available_platform_effects)) {
    // Platform AEC effect is only exposed on the platforms where platform echo
    // cancellation is either a default behavior or enforced via a feature flag,
    // see media::IsSystemEchoCancellationEnforced().
    return Type::kPlatformProvided;
  }
  if (media::IsChromeWideEchoCancellationEnabled()) {
    return Type::kChromeWide;
  }
  return Type::kPeerConnection;
}

// static
EchoCanceller::Type EchoCanceller::GetSystemWideAec(
    int available_platform_effects) {
  // MoltBrowser: WebRTC's in MoltNet (MoltNetEchoCanceller()).
  if (media::IsPortableMediaProcess()) {
    return MoltNetEchoCanceller();
  }
  if (media::IsSystemLoopbackAsAecReferenceEnabled()) {
    return Type::kLoopbackBased;
  }
  // See IsSystemWideAecAvailable().
  CHECK(IsPlatformAecAvailable(available_platform_effects));
  return Type::kPlatformProvided;
}

// static
bool EchoCanceller::IsPlatformAecAvailable(int available_platform_effects) {
  return available_platform_effects & media::AudioParameters::ECHO_CANCELLER;
}

}  // namespace blink
