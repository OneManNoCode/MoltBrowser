// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// Runs Blink's metric code (Chromium 156 + MoltBrowser's hooks, extracted
// verbatim from src/ into extracted.inc by run.py) for one OS
// (-DMOLT_OS_LINUX / _WIN / _MAC) over what Skia's Fontations backend
// reports for the bundled fonts (cases.txt from skrifa_dump), and prints
// every value it computes: SimpleFontData::PlatformInit()'s FontMetrics,
// average and maximum character widths, and per sample glyph its bounds,
// width and the extents HarfBuzz gets.
//
//   harness_<os> cases.txt uniform|system
//
// uniform: MoltNet's bundled fonts are active (the hooks apply); system:
// they aren't (upstream's per-OS behaviour), with the same SkFont input.
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "standins.h"

namespace blink {
bool g_uniform = false;
#include "extracted.inc"
}  // namespace blink

using blink::FontMetricsOverride;
using blink::FontPlatformData;
using blink::SimpleFontData;

namespace {

std::string Hex(float v) {
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%a", static_cast<double>(v));
  return buffer;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: harness cases.txt uniform|system\n");
    return 2;
  }
  blink::g_uniform = std::strcmp(argv[2], "uniform") == 0;
  std::ifstream in(argv[1]);
  std::string line;
  size_t cases = 0;
  while (std::getline(in, line)) {
    std::istringstream fields(line);
    std::string word, name, fvar, trak, sbix, m;
    int index = 0;
    float weight = 0, size = 0;
    fields >> word >> name >> index >> weight >> size >> fvar >> trak >> sbix >>
        m;
    SkFont font;
    SkFontMetrics& sm = font.metrics;
    int under_valid = 0, thick_valid = 0;
    fields >> sm.fTop >> sm.fAscent >> sm.fDescent >> sm.fBottom >>
        sm.fLeading >> sm.fAvgCharWidth >> sm.fMaxCharWidth >> sm.fXMin >>
        sm.fXMax >> sm.fXHeight >> sm.fCapHeight >> sm.fUnderlinePosition >>
        under_valid >> sm.fUnderlineThickness >> thick_valid;
    sm.fFlags = (under_valid ? SkFontMetrics::kUnderlinePositionIsValid_Flag
                             : 0) |
                (thick_valid ? SkFontMetrics::kUnderlineThicknessIsValid_Flag
                             : 0);
    std::map<UChar32, Glyph> cmap;
    std::vector<Glyph> sample;
    while (fields >> word && word == "g") {
      int cp = 0, gid = 0, has_path = 0;
      GlyphInfo info;
      float l, t, r, b;
      fields >> cp >> gid >> info.advance >> has_path >> l >> t >> r >> b;
      info.has_path = has_path;
      info.path = SkRect::MakeLTRB(l, t, r, b);
      if (gid) {
        cmap[cp] = static_cast<Glyph>(gid);
        font.glyphs[static_cast<uint16_t>(gid)] = info;
      }
      sample.push_back(static_cast<Glyph>(gid));
    }
    FontPlatformData platform_data;
    platform_data.text_size = size;
    platform_data.family = name.substr(0, name.find_first_of("-."));
    SimpleFontData data(&platform_data, font, cmap);
    data.PlatformInit(false, FontMetricsOverride());
    const blink::FontMetrics& fm = data.font_metrics_;
    std::printf("%s %d %g %g asc %s desc %s cap %s xh %s/%d gap %s spacing %s "
                "ul %s %s avg %s max %s",
                name.c_str(), index, static_cast<double>(weight),
                static_cast<double>(size), Hex(fm.ascent).c_str(),
                Hex(fm.descent).c_str(), Hex(fm.cap_height).c_str(),
                Hex(fm.x_height).c_str(), fm.has_x_height ? 1 : 0,
                Hex(fm.line_gap).c_str(), Hex(fm.line_spacing).c_str(),
                Hex(fm.underline_position).c_str(),
                Hex(fm.underline_thickness).c_str(),
                Hex(data.avg_char_width_).c_str(),
                Hex(data.max_char_width_).c_str());
    for (Glyph glyph : sample) {
      const blink::gfx::RectF bounds = data.BoundsForGlyph(glyph);
      blink::hb_glyph_extents_t extents{};
      blink::SkFontGetGlyphExtentsForHarfBuzz(data.font_, glyph, &extents);
      std::printf(" | %u b %s %s %s %s w %s hb %d %d %d %d", glyph,
                  Hex(bounds.x()).c_str(), Hex(bounds.y()).c_str(),
                  Hex(bounds.width()).c_str(), Hex(bounds.height()).c_str(),
                  Hex(data.WidthForGlyph(glyph)).c_str(), extents.x_bearing,
                  extents.y_bearing, extents.width, extents.height);
    }
    std::printf("\n");
    ++cases;
  }
  std::fprintf(stderr, "%zu cases\n", cases);
  return cases ? 0 : 1;
}
