// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/audio/pulse/molt_pulse_capture.h"

#include <utility>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "media/audio/audio_device_description.h"
#include "media/audio/pulse/molt_capture_source.h"
#include "media/audio/pulse/pulse_util.h"
#include "media/base/audio_parameters.h"
#include "media/base/molt_portable_media.h"

#if defined(DLOPEN_PULSEAUDIO)
#include "media/audio/pulse/pulse_stubs.h"
#endif

namespace media::pulse {

namespace {

// PipeWire's link group (PW_KEY_NODE_LINK_GROUP), which pipewire-pulse passes
// on with a node's other properties.
constexpr char kLinkGroup[] = "node.link-group";

struct ServerLists {
  explicit ServerLists(pa_threaded_mainloop* mainloop) : loop(mainloop) {}

  const raw_ptr<pa_threaded_mainloop> loop;
  std::vector<molt::SourceInfo> sources;
  std::vector<molt::SourceOutputInfo> outputs;
};

std::string Property(const pa_proplist* properties, const char* key) {
  if (!properties || pa_proplist_contains(properties, key) != 1) {
    return std::string();
  }
  const char* value = pa_proplist_gets(properties, key);
  return value ? std::string(value) : std::string();
}

void SourceCallback(pa_context* context,
                    const pa_source_info* info,
                    int eol,
                    void* user_data) {
  ServerLists* lists = static_cast<ServerLists*>(user_data);
  if (eol || !info) {
    pa_threaded_mainloop_signal(lists->loop, 0);
    return;
  }
  molt::SourceInfo source;
  source.name = info->name ? info->name : "";
  source.index = info->index;
  source.hardware = (info->flags & PA_SOURCE_HARDWARE) != 0;
  source.monitor = info->monitor_of_sink != PA_INVALID_INDEX;
  source.device_class = Property(info->proplist, PA_PROP_DEVICE_CLASS);
  source.master_device =
      Property(info->proplist, PA_PROP_DEVICE_MASTER_DEVICE);
  source.link_group = Property(info->proplist, kLinkGroup);
  lists->sources.push_back(std::move(source));
}

void SourceOutputCallback(pa_context* context,
                          const pa_source_output_info* info,
                          int eol,
                          void* user_data) {
  ServerLists* lists = static_cast<ServerLists*>(user_data);
  if (eol || !info) {
    pa_threaded_mainloop_signal(lists->loop, 0);
    return;
  }
  molt::SourceOutputInfo output;
  output.source_index = info->source;
  output.link_group = Property(info->proplist, kLinkGroup);
  if (!output.link_group.empty()) {
    lists->outputs.push_back(std::move(output));
  }
}

}  // namespace

std::string MoltCaptureSource(pa_threaded_mainloop* mainloop,
                              pa_context* context,
                              const std::string& device_id,
                              const AudioParameters& params) {
  if (!(params.effects() & kPortableCaptureEffect)) {
    return device_id;
  }
  // Before the lock: GetRealDefaultDeviceId() takes it, and waiting for the
  // server with the (recursive) lock held twice would never let the
  // mainloop's thread answer.
  std::string requested = device_id;
  if (device_id == AudioDeviceDescription::kDefaultDeviceId) {
    requested = GetRealDefaultDeviceId(mainloop, context, RequestType::INPUT);
    if (requested.empty()) {
      return device_id;
    }
  }
  AutoPulseLock auto_lock(mainloop);
  ServerLists lists(mainloop);
  if (!WaitForOperationCompletion(
          mainloop,
          pa_context_get_source_info_list(context, &SourceCallback, &lists),
          context)) {
    return device_id;
  }
  if (!WaitForOperationCompletion(
          mainloop,
          pa_context_get_source_output_info_list(
              context, &SourceOutputCallback, &lists),
          context)) {
    lists.outputs.clear();
  }
  const std::string chosen =
      molt::CaptureSourceFor(requested, lists.sources, lists.outputs);
  // Not a filter: as asked (the default device keeps following the
  // server's default).
  return chosen == requested ? device_id : chosen;
}

}  // namespace media::pulse
