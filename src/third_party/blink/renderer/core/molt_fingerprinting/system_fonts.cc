// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/system_fonts.h"

#include <array>
#include <cstdint>
#include <optional>

#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "base/synchronization/lock.h"
#include "base/thread_annotations.h"
#include "third_party/blink/renderer/core/dom/document.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_family.h"
#include "third_party/blink/renderer/platform/fonts/font_platform_data.h"
#include "third_party/blink/renderer/platform/fonts/font_selection_types.h"
#include "third_party/blink/renderer/platform/fonts/simple_font_data.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/heap/member.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/skia/include/core/SkFontStyle.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"

namespace blink::molt_fingerprinting {

namespace {

// SystemFontVisibility() of a context, so a font lookup costs no site
// resolution (string building) after the first one.
class MoltFontVisibilityState final
    : public GarbageCollected<MoltFontVisibilityState>,
      public Supplement<ExecutionContext> {
 public:
  static const char kSupplementName[];

  explicit MoltFontVisibilityState(ExecutionContext& context)
      : Supplement<ExecutionContext>(context) {}

  void Trace(Visitor* visitor) const override {
    visitor->Trace(document);
    Supplement<ExecutionContext>::Trace(visitor);
  }

  bool computed = false;
  // Windows: the document `visibility` was computed for. Blink reuses the
  // initial empty document's window for the first navigation.
  WeakMember<Document> document;
  FontVisibility visibility = FontVisibility::All();
};

const char MoltFontVisibilityState::kSupplementName[] =
    "MoltFontVisibilityState";

FontVisibility ComputeVisibility(ExecutionContext* context) {
  const Mode mode = GetMode();
  if (mode == Mode::kOff) {
    return FontVisibility::All();
  }
  const FontPlatform platform = CurrentFontPlatform();
  if (!context) {
    // A selector whose document or worker is gone: fail closed.
    return FontVisibility::AllowList(platform);
  }
  bool site_key = false;
  const std::optional<Farbler> farbler = SiteFarblerFor(context, &site_key);
  if (!farbler) {
    // Not a web context, or Shields are off for its site (kFarble).
    return FontVisibility::All();
  }
  if (mode == Mode::kUniform || !site_key) {
    return FontVisibility::AllowList(platform);
  }
  return FontVisibility::AllowListAndSample(platform, *farbler);
}

// The family name of a typeface, as the allow-list sees it. A typeface's
// family never changes and SkTypefaceIDs are unique in the process, so the
// result is cached (a small direct-mapped table; a collision just evicts).
struct TypefaceFamily {
  SkTypefaceID id = 0;
  uint64_t hash = 0;
  bool allow_listed = false;
  bool named = false;
};

class TypefaceFamilyCache {
 public:
  std::optional<TypefaceFamily> Get(SkTypefaceID id) {
    base::AutoLock auto_lock(lock_);
    const std::optional<TypefaceFamily>& entry = entries_[id % kSize];
    if (entry && entry->id == id) {
      return entry;
    }
    return std::nullopt;
  }

  void Put(const TypefaceFamily& family) {
    base::AutoLock auto_lock(lock_);
    entries_[family.id % kSize] = family;
  }

 private:
  static constexpr SkTypefaceID kSize = 256;
  base::Lock lock_;
  std::array<std::optional<TypefaceFamily>, kSize> entries_ GUARDED_BY(lock_);
};

TypefaceFamily FamilyOf(const SkTypeface& typeface) {
  static base::NoDestructor<TypefaceFamilyCache> cache;
  const SkTypefaceID id = typeface.uniqueID();
  if (const std::optional<TypefaceFamily> cached = cache->Get(id)) {
    return *cached;
  }
  // Skia's family name: CTFontCopyFamilyName on macOS, the en-us (else
  // first) DirectWrite family name on Windows, fontconfig's family on Linux.
  SkString name;
  typeface.getFamilyName(&name);
  const FontName family = FontName::Utf8(std::string_view(name.c_str()));
  TypefaceFamily result;
  result.id = id;
  result.hash = family.Hash();
  result.allow_listed =
      IsAllowListedFamily(CurrentFontPlatform(), family, result.hash);
  result.named = !family.IsEmpty();
  cache->Put(result);
  return result;
}

bool IsVisible(const FontVisibility& visibility,
               const SimpleFontData& font_data) {
  if (!visibility.restricts()) {
    return true;
  }
  const SkTypeface* typeface = font_data.PlatformData().Typeface();
  if (!typeface) {
    return false;
  }
  const TypefaceFamily family = FamilyOf(*typeface);
  return family.named &&
         visibility.IsFamilyVisible(family.hash, family.allow_listed);
}

FontName ToFontName(const AtomicString& name) {
  if (name.empty()) {
    return FontName::Utf8(std::string_view());
  }
  return name.Is8Bit() ? FontName::Latin1(name.Span8())
                       : FontName::Utf16(name.Span16());
}

// The family lookup FontFallbackList would do, under `visibility`.
const SimpleFontData* LookUpVisible(const FontVisibility& visibility,
                                    const FontDescription& font_description,
                                    const AtomicString& family) {
  using Kind = FontVisibility::Request::Kind;
  FontCache& font_cache = FontCache::Get();
  const FontVisibility::Request request =
      visibility.ForFamilyRequest(ToFontName(family));
  const SimpleFontData* font_data = nullptr;
  switch (request.kind) {
    case Kind::kBlocked:
      return nullptr;
    case Kind::kUnrestricted:
      return font_cache.GetFontData(font_description, family);
    case Kind::kLookUp:
      font_data = font_cache.GetFontData(font_description, family);
      break;
    case Kind::kLookUpInstead:
      font_data = font_cache.GetFontData(
          font_description, AtomicString::FromUtf8(request.instead));
      break;
  }
  return font_data && IsVisible(visibility, *font_data) ? font_data : nullptr;
}

}  // namespace

FontVisibility SystemFontVisibility(ExecutionContext* context) {
  if (GetMode() == Mode::kOff) {
    return FontVisibility::All();
  }
  if (!context || !context->IsContextThread()) {
    return ComputeVisibility(context);
  }
  MoltFontVisibilityState* state =
      Supplement<ExecutionContext>::From<MoltFontVisibilityState>(*context);
  if (!state) {
    state = MakeGarbageCollected<MoltFontVisibilityState>(*context);
    Supplement<ExecutionContext>::ProvideTo(*context, state);
  }
  Document* document = nullptr;
  if (auto* window = DynamicTo<LocalDOMWindow>(*context)) {
    document = window->document();
  }
  if (!state->computed || state->document != document) {
    state->visibility = ComputeVisibility(context);
    state->document = document;
    state->computed = true;
  }
  return state->visibility;
}

const SimpleFontData* GetSystemFontData(ExecutionContext* context,
                                        const FontDescription& font_description,
                                        const FontFamily& family) {
  if (!family.FamilyIsGeneric()) {
    const FontVisibility visibility = SystemFontVisibility(context);
    if (visibility.restricts()) {
      return LookUpVisible(visibility, font_description, family.FamilyName());
    }
  }
  return FontCache::Get().GetFontData(font_description, family.FamilyName());
}

std::optional<bool> IsSystemFontFamilyAvailable(
    ExecutionContext* context,
    const FontDescription& font_description,
    const FontFamily& family) {
  if (family.FamilyIsGeneric()) {
    return std::nullopt;
  }
  const FontVisibility visibility = SystemFontVisibility(context);
  if (!visibility.restricts()) {
    return std::nullopt;
  }
  // The same answer layout gets from GetSystemFontData().
  return LookUpVisible(visibility, font_description, family.FamilyName()) !=
         nullptr;
}

bool RestrictsLocalFontFaces(ExecutionContext* context) {
  return SystemFontVisibility(context).restricts();
}

const SimpleFontData* GetLocalFontFaceData(
    ExecutionContext* context,
    const FontDescription& font_description,
    const AtomicString& unique_name) {
  const FontVisibility visibility = SystemFontVisibility(context);
  if (!visibility.restricts()) {
    return nullptr;  // Callers look the unique name up themselves.
  }
  const FontVisibility::LocalFaceRequest request =
      visibility.ForLocalFaceRequest(ToFontName(unique_name));
  FontCache& font_cache = FontCache::Get();
  for (const FontVisibility::LocalFace& face : request.candidates()) {
    // The family lookup CSS would do for this family and style. No
    // alternate names: Blink's Arial-for-Helvetica substitutes are not this
    // face's family.
    FontDescription description(font_description);
    if (face.styled) {
      description.SetWeight(FontSelectionValue(face.weight));
      description.SetStyle(face.italic ? kItalicSlopeValue : kNormalSlopeValue);
      description.SetStretch(kNormalWidthValue);
    }
    const AtomicString family =
        face.sampled()
            ? AtomicString(unique_name.GetString().Left(
                  base::checked_cast<wtf_size_t>(face.prefix_length)))
            : AtomicString::FromUtf8(face.listed);
    const SimpleFontData* font_data = font_cache.GetFontData(
        description, family, AlternateFontName::kNoAlternate);
    if (!font_data) {
      continue;
    }
    const SkTypeface* typeface = font_data->PlatformData().Typeface();
    if (!typeface) {
      continue;
    }
    // The family it resolved to must be visible (as for any family lookup)
    // and, for a name in the sample, the one asked for.
    const TypefaceFamily resolved = FamilyOf(*typeface);
    if (!resolved.named ||
        !visibility.IsFamilyVisible(resolved.hash, resolved.allow_listed) ||
        (face.sampled() && resolved.hash != face.family_hash)) {
      continue;
    }
    // And a face of the style the name gives, not a neighbour the platform
    // matched instead (or one Blink would make bold or italic itself).
    const SkFontStyle style = typeface->fontStyle();
    if (!face.StyleMatches(style.weight(),
                           style.slant() != SkFontStyle::kUpright_Slant)) {
      continue;
    }
    return font_data;
  }
  return nullptr;
}

bool AllowEnumeratedFont(ExecutionContext* context, std::string_view family) {
  return SystemFontVisibility(context).IsFamilyVisible(FontName::Utf8(family));
}

}  // namespace blink::molt_fingerprinting
