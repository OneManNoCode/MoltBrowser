// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/audio/pulse/molt_capture_source.h"

namespace media::pulse::molt {

namespace {

// How many filters deep a chain is followed.
constexpr int kMaxFilterDepth = 4;

const SourceInfo* FindByName(std::span<const SourceInfo> sources,
                             std::string_view name) {
  for (const SourceInfo& source : sources) {
    if (source.name == name) {
      return &source;
    }
  }
  return nullptr;
}

const SourceInfo* FindByIndex(std::span<const SourceInfo> sources,
                              uint32_t index) {
  for (const SourceInfo& source : sources) {
    if (source.index == index) {
      return &source;
    }
  }
  return nullptr;
}

// The source `filter` processes, if the server tells: its master device
// (PulseAudio), or the source a recording stream of its link group reads
// (PipeWire). nullptr otherwise.
const SourceInfo* SourceUnder(const SourceInfo& filter,
                              std::span<const SourceInfo> sources,
                              std::span<const SourceOutputInfo> outputs) {
  if (!filter.master_device.empty()) {
    if (const SourceInfo* master = FindByName(sources, filter.master_device)) {
      return master;
    }
  }
  if (!filter.link_group.empty()) {
    for (const SourceOutputInfo& output : outputs) {
      if (output.link_group != filter.link_group) {
        continue;
      }
      const SourceInfo* read = FindByIndex(sources, output.source_index);
      if (read && read != &filter) {
        return read;
      }
    }
  }
  return nullptr;
}

}  // namespace

bool IsFilterSource(const SourceInfo& source) {
  return source.device_class == "filter" || !source.master_device.empty() ||
         !source.link_group.empty();
}

std::string CaptureSourceFor(std::string_view requested,
                             std::span<const SourceInfo> sources,
                             std::span<const SourceOutputInfo> outputs) {
  const SourceInfo* source = FindByName(sources, requested);
  if (!source || !IsFilterSource(*source)) {
    return std::string(requested);
  }
  for (int depth = 0; depth < kMaxFilterDepth; ++depth) {
    const SourceInfo* under = SourceUnder(*source, sources, outputs);
    if (!under) {
      break;
    }
    if (!IsFilterSource(*under) && !under->monitor) {
      return under->name;
    }
    source = under;
  }
  for (const SourceInfo& candidate : sources) {
    if (candidate.hardware && !candidate.monitor &&
        !IsFilterSource(candidate)) {
      return candidate.name;
    }
  }
  return std::string(requested);
}

}  // namespace media::pulse::molt
