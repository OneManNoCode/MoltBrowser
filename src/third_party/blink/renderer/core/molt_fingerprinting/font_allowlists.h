// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: the per-platform lists of installed
// font families web content may see (data only; fonts.h has the matcher and
// docs/FINGERPRINTING.md, "Fonts", the provenance).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FONT_ALLOWLISTS_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FONT_ALLOWLISTS_H_

#include <cstdint>
#include <string_view>

#include "base/containers/span.h"

namespace blink::molt_fingerprinting {

enum class FontPlatform : uint8_t { kWindows, kMac, kLinux, kAndroid };

struct FontListEntry {
  // UTF-8, as the OS spells it; compared in FontName's normalized form.
  std::string_view name;
  // Empty: a family (or another name of a family) the OS installs by
  // default; it may be requested, may be the family a lookup resolves to,
  // and is reported by queryLocalFonts(). Otherwise: a name that the
  // platform resolves differently from machine to machine (fontconfig's
  // metric aliases, Blink's own Arial/Helvetica-style alternates). It is
  // only a request name: protected contexts look up `alias_of`, a listed
  // family, instead.
  std::string_view alias_of = {};
};

// The list for `platform`, in no particular order.
base::span<const FontListEntry> FontListFor(FontPlatform platform);

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_FONT_ALLOWLISTS_H_
