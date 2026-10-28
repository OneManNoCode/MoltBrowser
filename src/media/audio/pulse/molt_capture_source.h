// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser: which PulseAudio source a microphone stream of a MoltNet page
// records from (media/base/molt_portable_media.h, kPortableCaptureEffect).
// Pure and std-only (tested standalone); the PulseAudio side is
// molt_pulse_capture.h.
//
// A filter source processes another source's audio before the browser gets
// it: PulseAudio's and PipeWire's echo cancellers, PipeWire's filter chains
// (noise suppression plugins), remapped or virtual sources. Picked as the
// microphone, or set as the default source, it makes the OS's processing
// part of what a page receives, which differs between machines. A MoltNet
// stream records from the source under the filter instead, and its own
// (CPU-independent) processing does the rest.
//
// What tells a filter source, as the PulseAudio server reports a source
// (pa_source_info):
//   PulseAudio      device.class "filter" and device.master_device (the source
//                   it filters) on module-echo-cancel's, module-remap-source's
//                   and module-virtual-source's sources; no PA_SOURCE_HARDWARE
//                   flag.
//   pipewire-pulse  no device.class or device.master_device; the driver is
//                   "PipeWire" for every source. node.link-group, which the
//                   echo canceller and filter-chain set on their source and
//                   on the capture stream that reads the source below
//                   ("echo-cancel-<pid>-<id>"), and node.virtual (no
//                   PA_SOURCE_HARDWARE flag).
// A source without PA_SOURCE_HARDWARE but with neither sign (a null source,
// a network tunnel, an application's virtual microphone) isn't processing a
// microphone, and is recorded as it is.

#ifndef MEDIA_AUDIO_PULSE_MOLT_CAPTURE_SOURCE_H_
#define MEDIA_AUDIO_PULSE_MOLT_CAPTURE_SOURCE_H_

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace media::pulse::molt {

// A source as the server lists it.
struct SourceInfo {
  std::string name;
  uint32_t index = 0;
  bool hardware = false;      // PA_SOURCE_HARDWARE
  bool monitor = false;       // a sink's monitor (monitor_of_sink is set)
  std::string device_class;   // device.class
  std::string master_device;  // device.master_device
  std::string link_group;     // node.link-group
};

// A recording stream (a source output) as the server lists it: the source it
// reads, and its node.link-group.
struct SourceOutputInfo {
  uint32_t source_index = 0;
  std::string link_group;
};

// Whether `source` processes another source's audio (see above).
bool IsFilterSource(const SourceInfo& source);

// The source to record from for `requested` (a source name, already the
// server's default source when the page asked for the default device):
// `requested` itself unless it is a filter source; then the source it filters
// (device.master_device, or the source that a recording stream in its link
// group reads), followed through up to four filters; and when that can't be
// told, the first hardware source that isn't a monitor or a filter. Returns
// `requested` when nothing better is found (also when it isn't listed).
std::string CaptureSourceFor(std::string_view requested,
                             std::span<const SourceInfo> sources,
                             std::span<const SourceOutputInfo> outputs);

}  // namespace media::pulse::molt

#endif  // MEDIA_AUDIO_PULSE_MOLT_CAPTURE_SOURCE_H_
