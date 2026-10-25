// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "chrome/browser/molt_ai/fingerprinting/molt_net_fonts.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "chrome/browser/molt_ai/fingerprinting/molt_net_fonts_table.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"

namespace molt_ai::fingerprinting {
namespace {

constexpr std::array<uint8_t, 4> kTrueType = {0x00, 0x01, 0x00, 0x00};

// A file of `size` bytes that starts with `head` (sparse: quick at any size).
void WriteFile(const base::FilePath& path,
               base::span<const uint8_t> head,
               int64_t size) {
  base::File file(path, base::File::FLAG_CREATE_ALWAYS |
                            base::File::FLAG_WRITE | base::File::FLAG_READ);
  ASSERT_TRUE(file.IsValid());
  ASSERT_TRUE(file.WriteAndCheck(0, head));
  ASSERT_TRUE(file.SetLength(size));
}

// A folder with every file of the set, each of the right size.
void WriteSet(const base::FilePath& dir) {
  for (const MoltNetFontFileSpec& spec : kMoltNetFontFiles) {
    WriteFile(dir.AppendASCII(spec.name), kTrueType,
              static_cast<int64_t>(spec.size));
  }
}

TEST(MoltNetFontsTest, BrowserTableIsBlinksTable) {
  // The browser sends the files in its table's order and Blink reads them in
  // its own: both are generated from tools/fonts/moltnet_fonts.json.
  const auto& blink =
      blink::molt_fingerprinting::moltnet::GetBundledFontTable();
  ASSERT_EQ(std::size(kMoltNetFontFiles), blink.files.size());
  uint64_t total = 0;
  for (size_t i = 0; i < blink.files.size(); ++i) {
    EXPECT_EQ(kMoltNetFontFiles[i].name, blink.files[i].install_name);
    EXPECT_EQ(kMoltNetFontFiles[i].size, blink.files[i].size);
    EXPECT_EQ(kMoltNetFontFiles[i].sha256.size(), 64u);
    total += kMoltNetFontFiles[i].size;
  }
  EXPECT_EQ(total, kMoltNetFontSetBytes);
  EXPECT_EQ(kMoltNetFontSetVersion, blink.version);
}

TEST(MoltNetFontsTest, ExpectedFile) {
  const int64_t size = static_cast<int64_t>(kMoltNetFontFiles[0].size);
  EXPECT_TRUE(IsExpectedMoltNetFontFile(0, size, kTrueType));
  for (const std::array<uint8_t, 4> head :
       {std::array<uint8_t, 4>{'O', 'T', 'T', 'O'},
        std::array<uint8_t, 4>{'t', 't', 'c', 'f'},
        std::array<uint8_t, 4>{'t', 'r', 'u', 'e'}}) {
    EXPECT_TRUE(IsExpectedMoltNetFontFile(0, size, head));
  }
  EXPECT_FALSE(IsExpectedMoltNetFontFile(0, size - 1, kTrueType));
  EXPECT_FALSE(IsExpectedMoltNetFontFile(0, -1, kTrueType));
  EXPECT_FALSE(
      IsExpectedMoltNetFontFile(std::size(kMoltNetFontFiles), size, kTrueType));
  const std::array<uint8_t, 4> woff = {'w', 'O', 'F', 'F'};
  EXPECT_FALSE(IsExpectedMoltNetFontFile(0, size, woff));
  EXPECT_FALSE(IsExpectedMoltNetFontFile(
      0, size, base::span<const uint8_t>(kTrueType).first(3u)));
}

TEST(MoltNetFontsTest, OpensTheWholeSetOrNothing) {
  base::ScopedTempDir dir;
  ASSERT_TRUE(dir.CreateUniqueTempDir());
  WriteSet(dir.GetPath());
  std::optional<std::vector<base::File>> files =
      OpenMoltNetFonts(dir.GetPath());
  ASSERT_TRUE(files.has_value());
  ASSERT_EQ(files->size(), std::size(kMoltNetFontFiles));
  for (size_t i = 0; i < files->size(); ++i) {
    EXPECT_EQ((*files)[i].GetLength(),
              static_cast<int64_t>(kMoltNetFontFiles[i].size));
  }

  // One file missing.
  const base::FilePath last =
      dir.GetPath().AppendASCII(std::string(kMoltNetFontFiles[2].name));
  ASSERT_TRUE(base::DeleteFile(last));
  EXPECT_FALSE(OpenMoltNetFonts(dir.GetPath()).has_value());
  // ... of the wrong size.
  WriteFile(last, kTrueType,
            static_cast<int64_t>(kMoltNetFontFiles[2].size) + 1);
  EXPECT_FALSE(OpenMoltNetFonts(dir.GetPath()).has_value());
  // ... not a font.
  const std::array<uint8_t, 4> html = {'<', 'h', 't', 'm'};
  WriteFile(last, html, static_cast<int64_t>(kMoltNetFontFiles[2].size));
  EXPECT_FALSE(OpenMoltNetFonts(dir.GetPath()).has_value());
  // Fixed again.
  WriteFile(last, kTrueType, static_cast<int64_t>(kMoltNetFontFiles[2].size));
  EXPECT_TRUE(OpenMoltNetFonts(dir.GetPath()).has_value());

  EXPECT_FALSE(OpenMoltNetFonts(base::FilePath()).has_value());
  EXPECT_FALSE(
      OpenMoltNetFonts(dir.GetPath().AppendASCII("nonexistent")).has_value());
}

TEST(MoltNetFontsTest, Directory) {
  const base::FilePath dir = GetMoltNetFontsDirectory();
  ASSERT_FALSE(dir.empty());
  EXPECT_EQ(dir.BaseName().value(), kMoltNetFontsFolder);
}

}  // namespace
}  // namespace molt_ai::fingerprinting
