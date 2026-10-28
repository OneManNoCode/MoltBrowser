// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: a CPU-independent microphone stream's down-mix of a device of
// another layout (media/base/molt_portable_media.h, "Microphones of other
// layouts").

#include <memory>

#include "base/check.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/base/channel_mixer.h"
#include "media/base/molt_portable_media.h"

namespace media {

bool PortableCaptureNeedsDownmix(ChannelLayout layout) {
  switch (layout) {
    case CHANNEL_LAYOUT_NONE:
    case CHANNEL_LAYOUT_UNSUPPORTED:
    case CHANNEL_LAYOUT_MONO:
    case CHANNEL_LAYOUT_STEREO:
    case CHANNEL_LAYOUT_DISCRETE:
    case CHANNEL_LAYOUT_BITSTREAM:
      return false;
    default:
      return ChannelLayoutToChannelCount(layout) > 0;
  }
}

PortableCaptureDownmix::PortableCaptureDownmix(const AudioParameters& device,
                                               int channels) {
  CHECK(PortableCaptureNeedsDownmix(device.channel_layout()));
  CHECK(channels == 1 || channels == 2);
  const ChannelLayoutConfig output_layout = channels == 1
                                                ? ChannelLayoutConfig::Mono()
                                                : ChannelLayoutConfig::Stereo();
  mixer_ = std::make_unique<ChannelMixer>(device.channel_layout_config(),
                                          output_layout);
  output_ = AudioBus::Create(channels, device.frames_per_buffer());
}

PortableCaptureDownmix::~PortableCaptureDownmix() = default;

int PortableCaptureDownmix::channels() const {
  return output_->channels();
}

const AudioBus& PortableCaptureDownmix::Downmix(const AudioBus& source) {
  if (source.frames() != output_->frames()) {
    output_ = AudioBus::Create(output_->channels(), source.frames());
  }
  // The mixer's multiply-adds (vector_math::FMAC) in their plain C version,
  // and IEEE subnormals, on whatever thread the stream runs.
  ScopedPortableAudioThread portable_code(true);
  ScopedPortableSubnormals portable_subnormals;
  mixer_->Transform(&source, output_.get());
  return *output_;
}

}  // namespace media
