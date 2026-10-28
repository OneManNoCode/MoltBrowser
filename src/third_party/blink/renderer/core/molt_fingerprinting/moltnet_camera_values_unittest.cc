// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_camera_values.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <tuple>
#include <utility>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace blink::molt_fingerprinting::moltnet {
namespace {

// The sizes of the entries a camera with `modes` shows.
std::vector<std::pair<int, int>> Shown(const std::vector<CameraMode>& modes) {
  std::vector<std::pair<int, int>> sizes;
  for (const CameraMode& entry : ReachableUniformCameraModes(modes)) {
    EXPECT_EQ(entry.frame_rate, kUniformCameraFrameRate);
    sizes.emplace_back(entry.width, entry.height);
  }
  return sizes;
}

const std::vector<std::pair<int, int>> kBoth = {{640, 480}, {1280, 720}};
const std::vector<std::pair<int, int>> kVga = {{640, 480}};

// The mode the camera opens in, as width, height and rate.
std::optional<CameraMode> Opened(const std::vector<CameraMode>& modes) {
  const std::optional<size_t> index = UniformCameraOpenMode(modes);
  if (!index) {
    return std::nullopt;
  }
  EXPECT_LT(*index, modes.size());
  return modes[*index];
}

void ExpectOpens(const std::vector<CameraMode>& modes,
                 int width,
                 int height,
                 float frame_rate) {
  const std::optional<CameraMode> opened = Opened(modes);
  ASSERT_TRUE(opened.has_value());
  EXPECT_EQ(opened->width, width);
  EXPECT_EQ(opened->height, height);
  EXPECT_EQ(opened->frame_rate, frame_rate);
}

// The menu: 640x480 and 1280x720 at 30 fps, each entry at least as large as
// the one before.
TEST(MoltNetCameraValuesTest, Menu) {
  ASSERT_EQ(std::size(kUniformCameraMenu), 2u);
  EXPECT_EQ(kUniformCameraMenu[0].width, 640);
  EXPECT_EQ(kUniformCameraMenu[0].height, 480);
  EXPECT_EQ(kUniformCameraMenu[1].width, 1280);
  EXPECT_EQ(kUniformCameraMenu[1].height, 720);
  for (const CameraMode& entry : kUniformCameraMenu) {
    EXPECT_EQ(entry.frame_rate, 30.0f);
  }
  EXPECT_LT(kUniformCameraFastFrameRate, kUniformCameraFrameRate);
}

// An HD camera shows both entries, whatever else it has, and opens in its
// smallest mode that reaches 1280x720.
TEST(MoltNetCameraValuesTest, HdCamera) {
  const std::vector<CameraMode> modes = {{1920, 1080, 30.0f},
                                         {640, 480, 30.0f},
                                         {1280, 720, 30.0f},
                                         {800, 600, 30.0f},
                                         {320, 240, 30.0f}};
  EXPECT_EQ(Shown(modes), kBoth);
  ExpectOpens(modes, 1280, 720, 30.0f);

  // A 1080p-only camera reaches both, by cropping and scaling.
  EXPECT_EQ(Shown({{1920, 1080, 30.0f}}), kBoth);
  ExpectOpens({{1920, 1080, 30.0f}}, 1920, 1080, 30.0f);
  // A 4:3 camera taller than 720 too.
  EXPECT_EQ(Shown({{1600, 1200, 30.0f}, {640, 480, 30.0f}}), kBoth);
  ExpectOpens({{1600, 1200, 30.0f}, {640, 480, 30.0f}}, 1600, 1200, 30.0f);
}

// A camera whose largest mode is 640x480 shows 640x480 alone, never its own
// smaller modes; one between the entries too.
TEST(MoltNetCameraValuesTest, VgaCamera) {
  const std::vector<CameraMode> vga = {
      {160, 120, 30.0f}, {320, 240, 30.0f}, {640, 480, 30.0f}};
  EXPECT_EQ(Shown(vga), kVga);
  ExpectOpens(vga, 640, 480, 30.0f);

  const std::vector<CameraMode> svga = {{800, 600, 30.0f}, {1024, 768, 30.0f}};
  EXPECT_EQ(Shown(svga), kVga);
  ExpectOpens(svga, 800, 600, 30.0f);

  // 1280x800 is wide and tall enough; 1280x700 isn't tall enough for 720.
  EXPECT_EQ(Shown({{1280, 800, 30.0f}}), kBoth);
  EXPECT_EQ(Shown({{1280, 700, 30.0f}}), kVga);
}

// A camera that reaches no entry still shows the smallest, to which its
// frames are scaled up, and opens in its largest mode.
TEST(MoltNetCameraValuesTest, CameraBelowTheMenu) {
  const std::vector<CameraMode> modes = {
      {176, 144, 30.0f}, {352, 288, 30.0f}, {320, 240, 30.0f}};
  EXPECT_EQ(Shown(modes), kVga);
  ExpectOpens(modes, 352, 288, 30.0f);

  // Wide but not tall enough for any entry.
  EXPECT_EQ(Shown({{1920, 400, 30.0f}}), kVga);
  ExpectOpens({{1920, 400, 30.0f}, {640, 360, 30.0f}}, 1920, 400, 30.0f);
}

// The entries count at the menu's rate: a camera's slow modes don't reach an
// entry while it has a fast mode, so an HD size it only has at 10 fps isn't
// shown. A camera without any fast mode goes by all its modes.
TEST(MoltNetCameraValuesTest, FrameRates) {
  const std::vector<CameraMode> slow_hd = {{1280, 720, 10.0f},
                                           {1920, 1080, 5.0f},
                                           {640, 480, 30.0f}};
  EXPECT_EQ(Shown(slow_hd), kVga);
  ExpectOpens(slow_hd, 640, 480, 30.0f);

  const std::vector<CameraMode> all_slow = {{640, 480, 15.0f},
                                            {1280, 720, 15.0f}};
  EXPECT_EQ(Shown(all_slow), kBoth);
  ExpectOpens(all_slow, 1280, 720, 15.0f);
  EXPECT_EQ(UniformCameraOpenFrameRate({1280, 720, 15.0f}), 15.0f);

  // 29.97 fps is the menu's rate.
  EXPECT_EQ(Shown({{1280, 720, 29.97f}, {640, 480, 15.0f}}), kBoth);
  EXPECT_EQ(UniformCameraOpenFrameRate({1280, 720, 29.97f}), 29.97f);

  // Of one size, the rate closest to 30 from above; a faster mode asks for
  // the menu's rate.
  ExpectOpens({{1280, 720, 60.0f}, {1280, 720, 30.0f}, {1280, 720, 25.0f}},
              1280, 720, 30.0f);
  ExpectOpens({{1280, 720, 60.0f}, {1280, 720, 15.0f}}, 1280, 720, 60.0f);
  EXPECT_EQ(UniformCameraOpenFrameRate({1280, 720, 60.0f}), 30.0f);
  ExpectOpens({{1280, 720, 10.0f}, {1280, 720, 15.0f}}, 1280, 720, 15.0f);

  // Chromium's fake capture device: six sizes, 96x96 to 3840x2160, at 20
  // fps.
  const std::vector<CameraMode> fake = {
      {96, 96, 20.0f},     {320, 240, 20.0f},   {640, 480, 20.0f},
      {1280, 720, 20.0f},  {1920, 1080, 20.0f}, {3840, 2160, 20.0f}};
  EXPECT_EQ(Shown(fake), kBoth);
  ExpectOpens(fake, 1280, 720, 20.0f);
}

// A rotated (portrait) mode reaches what its width and height reach.
TEST(MoltNetCameraValuesTest, PortraitModes) {
  EXPECT_EQ(Shown({{720, 1280, 30.0f}}), kVga);
  ExpectOpens({{720, 1280, 30.0f}, {480, 640, 30.0f}}, 720, 1280, 30.0f);
}

// What the camera shows and opens in depends on its set of modes, not on the
// order the device lists them in.
TEST(MoltNetCameraValuesTest, OrderOfModes) {
  std::vector<CameraMode> modes = {{1920, 1080, 30.0f}, {640, 480, 30.0f},
                                   {1280, 720, 60.0f},  {1280, 720, 30.0f},
                                   {320, 240, 30.0f},   {1280, 720, 5.0f}};
  std::sort(modes.begin(), modes.end(),
            [](const CameraMode& a, const CameraMode& b) {
              return std::tie(a.width, a.height, a.frame_rate) <
                     std::tie(b.width, b.height, b.frame_rate);
            });
  do {
    EXPECT_EQ(Shown(modes), kBoth);
    ExpectOpens(modes, 1280, 720, 30.0f);
  } while (std::next_permutation(
      modes.begin(), modes.end(),
      [](const CameraMode& a, const CameraMode& b) {
        return std::tie(a.width, a.height, a.frame_rate) <
               std::tie(b.width, b.height, b.frame_rate);
      }));
}

// No mode of a positive size: nothing to show or open. Modes without a size
// or with an unusable rate don't count, or count as slow.
TEST(MoltNetCameraValuesTest, UnusableModes) {
  EXPECT_TRUE(Shown({}).empty());
  EXPECT_FALSE(Opened({}).has_value());
  EXPECT_TRUE(Shown({{0, 0, 30.0f}, {640, 0, 30.0f}, {-1, 480, 30.0f}})
                  .empty());
  EXPECT_FALSE(Opened({{0, 0, 30.0f}}).has_value());

  const float nan = std::numeric_limits<float>::quiet_NaN();
  // A NaN rate is slow: with a fast mode it doesn't count.
  EXPECT_EQ(Shown({{1920, 1080, nan}, {640, 480, 30.0f}}), kVga);
  ExpectOpens({{1920, 1080, nan}, {640, 480, 30.0f}, {0, 0, 30.0f}}, 640, 480,
              30.0f);
  // Without one, every mode counts.
  EXPECT_EQ(Shown({{1920, 1080, nan}}), kBoth);
  EXPECT_EQ(UniformCameraOpenFrameRate({1920, 1080, nan}), 30.0f);
  EXPECT_EQ(UniformCameraOpenFrameRate({1920, 1080, 0.0f}), 30.0f);
  EXPECT_EQ(UniformCameraOpenFrameRate(
                {1920, 1080, std::numeric_limits<float>::infinity()}),
            30.0f);
}

// The mode a camera opens in reaches every entry it shows, so a camera that
// is already open shows, from that one mode, what it showed before it
// opened (a second getUserMedia(), applyConstraints()).
TEST(MoltNetCameraValuesTest, OpenModeShowsTheSameEntries) {
  const std::vector<std::vector<CameraMode>> cameras = {
      {{1920, 1080, 30.0f}, {1280, 720, 30.0f}, {640, 480, 30.0f}},
      {{1280, 720, 10.0f}, {640, 480, 30.0f}},
      {{640, 480, 15.0f}, {1280, 720, 15.0f}},
      {{320, 240, 30.0f}},
      {{800, 600, 30.0f}},
      {{720, 1280, 30.0f}},
      {{1600, 1200, 60.0f}, {1600, 1200, 30.0f}},
  };
  for (const std::vector<CameraMode>& modes : cameras) {
    const std::optional<CameraMode> opened = Opened(modes);
    ASSERT_TRUE(opened.has_value());
    EXPECT_EQ(Shown({*opened}), Shown(modes));
  }
}

}  // namespace
}  // namespace blink::molt_fingerprinting::moltnet
