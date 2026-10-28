// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "media/audio/pulse/molt_capture_source.h"

#include <string>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace media::pulse::molt {
namespace {

SourceInfo Make(std::string name,
                uint32_t index,
                bool hardware,
                bool monitor = false,
                std::string device_class = "",
                std::string master_device = "",
                std::string link_group = "") {
  SourceInfo info;
  info.name = std::move(name);
  info.index = index;
  info.hardware = hardware;
  info.monitor = monitor;
  info.device_class = std::move(device_class);
  info.master_device = std::move(master_device);
  info.link_group = std::move(link_group);
  return info;
}

SourceOutputInfo Output(uint32_t source_index, std::string link_group) {
  SourceOutputInfo output;
  output.source_index = source_index;
  output.link_group = std::move(link_group);
  return output;
}

// The sources of a laptop with a USB headset, as PulseAudio lists them with
// module-echo-cancel loaded on the built-in microphone.
std::vector<SourceInfo> PulseAudioSources() {
  return {
      Make("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor", 0, false,
           true, "monitor"),
      Make("alsa_input.pci-0000_00_1f.3.analog-stereo", 1, true, false,
           "sound"),
      Make("alsa_input.usb-Headset-00.mono-fallback", 2, true, false, "sound"),
      Make("echo-cancel-source", 3, false, false, "filter",
           "alsa_input.pci-0000_00_1f.3.analog-stereo"),
      // A remapped source on top of the echo canceller.
      Make("remapped-mono", 4, false, false, "filter", "echo-cancel-source")};
}

// The same machine under pipewire-pulse with PipeWire's echo canceller: the
// source carries the canceller's link group and no hardware flag, and the
// canceller's capture stream (same link group) reads the microphone.
std::vector<SourceInfo> PipeWireSources() {
  return {Make("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor", 40, false,
               true),
          Make("alsa_input.pci-0000_00_1f.3.analog-stereo", 41, true),
          Make("alsa_input.usb-Headset-00.mono-fallback", 42, true),
          Make("echo-cancel-source", 57, false, false, "", "",
               "echo-cancel-1234-21")};
}

TEST(MoltPulseCaptureSourceTest, TellsFilters) {
  const std::vector<SourceInfo> pulse = PulseAudioSources();
  EXPECT_FALSE(IsFilterSource(pulse[0]));
  EXPECT_FALSE(IsFilterSource(pulse[1]));
  EXPECT_TRUE(IsFilterSource(pulse[3]));
  EXPECT_TRUE(IsFilterSource(pulse[4]));
  const std::vector<SourceInfo> pipewire = PipeWireSources();
  EXPECT_FALSE(IsFilterSource(pipewire[1]));
  EXPECT_TRUE(IsFilterSource(pipewire[3]));
  // Not hardware, but nothing it filters: a null source, a tunnel.
  EXPECT_FALSE(IsFilterSource(Make("null-source", 9, false)));
}

TEST(MoltPulseCaptureSourceTest, RecordsUnderPulseAudiosFilters) {
  const std::vector<SourceInfo> sources = PulseAudioSources();
  // A device: itself.
  EXPECT_EQ(CaptureSourceFor("alsa_input.usb-Headset-00.mono-fallback",
                             sources, {}),
            "alsa_input.usb-Headset-00.mono-fallback");
  // The echo canceller: its master device.
  EXPECT_EQ(CaptureSourceFor("echo-cancel-source", sources, {}),
            "alsa_input.pci-0000_00_1f.3.analog-stereo");
  // A filter on a filter: followed down.
  EXPECT_EQ(CaptureSourceFor("remapped-mono", sources, {}),
            "alsa_input.pci-0000_00_1f.3.analog-stereo");
  // Not listed (gone meanwhile): as asked.
  EXPECT_EQ(CaptureSourceFor("unknown", sources, {}), "unknown");
}

TEST(MoltPulseCaptureSourceTest, RecordsUnderPipeWiresFilters) {
  const std::vector<SourceInfo> sources = PipeWireSources();
  const std::vector<SourceOutputInfo> outputs = {
      Output(42, ""),  // another application
      Output(41, "echo-cancel-1234-21")};
  EXPECT_EQ(CaptureSourceFor("echo-cancel-source", sources, outputs),
            "alsa_input.pci-0000_00_1f.3.analog-stereo");
  EXPECT_EQ(CaptureSourceFor("alsa_input.usb-Headset-00.mono-fallback",
                             sources, outputs),
            "alsa_input.usb-Headset-00.mono-fallback");
  // The canceller on the headset: the headset, though it is listed second.
  const std::vector<SourceOutputInfo> on_headset = {
      Output(42, "echo-cancel-1234-21")};
  EXPECT_EQ(CaptureSourceFor("echo-cancel-source", sources, on_headset),
            "alsa_input.usb-Headset-00.mono-fallback");
}

// A filter under which the server names no microphone is recorded as asked:
// never the first microphone the server lists, which may not be the one the
// filter processes, or there may be none under it.
TEST(MoltPulseCaptureSourceTest, NeverGuessesAnotherDevice) {
  // The canceller's capture stream isn't there (not linked yet).
  EXPECT_EQ(CaptureSourceFor("echo-cancel-source", PipeWireSources(), {}),
            "echo-cancel-source");
  // A master that isn't listed.
  std::vector<SourceInfo> sources = PulseAudioSources();
  sources[3].master_device = "gone";
  EXPECT_EQ(CaptureSourceFor("echo-cancel-source", sources, {}),
            "echo-cancel-source");
  // No microphone at all besides the filter.
  const std::vector<SourceInfo> only_filter = {
      Make("echo-cancel-source", 3, false, false, "filter")};
  EXPECT_EQ(CaptureSourceFor("echo-cancel-source", only_filter, {}),
            "echo-cancel-source");
}

// A virtual microphone that plays applications' output: what the user picked,
// never a real microphone in its place.
TEST(MoltPulseCaptureSourceTest, RecordsVirtualMicrophonesAsAsked) {
  // PulseAudio: module-null-sink and a module-remap-source of its monitor.
  std::vector<SourceInfo> pulse = PulseAudioSources();
  pulse.push_back(Make("virtual_speaker.monitor", 5, false, true, "monitor"));
  pulse.push_back(Make("virtual_mic", 6, false, false, "filter",
                       "virtual_speaker.monitor"));
  EXPECT_EQ(CaptureSourceFor("virtual_mic", pulse, {}), "virtual_mic");
  // A filter on top of it.
  pulse.push_back(
      Make("virtual_mic_mono", 7, false, false, "filter", "virtual_mic"));
  EXPECT_EQ(CaptureSourceFor("virtual_mic_mono", pulse, {}),
            "virtual_mic_mono");
  // pipewire-pulse: a loopback whose capture side is a virtual sink, so no
  // recording stream shares its link group.
  std::vector<SourceInfo> pipewire = PipeWireSources();
  pipewire.push_back(
      Make("virtual-mic", 58, false, false, "", "", "loopback-1234-30"));
  const std::vector<SourceOutputInfo> outputs = {
      Output(41, "echo-cancel-1234-21")};
  EXPECT_EQ(CaptureSourceFor("virtual-mic", pipewire, outputs), "virtual-mic");
  // A loopback that records a sink's monitor (index 40).
  const std::vector<SourceOutputInfo> from_monitor = {
      Output(40, "loopback-1234-30")};
  EXPECT_EQ(CaptureSourceFor("virtual-mic", pipewire, from_monitor),
            "virtual-mic");
}

TEST(MoltPulseCaptureSourceTest, LoopsEnd) {
  // Two filters naming each other: as asked.
  const std::vector<SourceInfo> sources = {
      Make("a", 1, false, false, "", "b"), Make("b", 2, false, false, "", "a"),
      Make("mic", 3, true)};
  EXPECT_EQ(CaptureSourceFor("a", sources, {}), "a");
  // A link group whose capture stream reads the filter itself.
  const std::vector<SourceInfo> self = {
      Make("f", 7, false, false, "", "", "g"), Make("mic", 8, true)};
  const std::vector<SourceOutputInfo> outputs = {Output(7, "g")};
  EXPECT_EQ(CaptureSourceFor("f", self, outputs), "f");
}

}  // namespace
}  // namespace media::pulse::molt
