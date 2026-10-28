// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_fonts.h"

#include <unicode/uscript.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/files/memory_mapped_file.h"
#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/no_destructor.h"
#include "base/synchronization/lock.h"
#include "base/task/single_thread_task_runner.h"
#include "base/thread_annotations.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/fonts.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_font_set.h"
#include "third_party/blink/renderer/platform/allow_discouraged_type.h"
#include "third_party/blink/renderer/platform/font_family_names.h"
#include "third_party/blink/renderer/platform/fonts/font_cache.h"
#include "third_party/blink/renderer/platform/fonts/font_description.h"
#include "third_party/blink/renderer/platform/fonts/font_fallback_priority.h"
#include "third_party/blink/renderer/platform/fonts/font_global_context.h"
#include "third_party/blink/renderer/platform/fonts/font_platform_data.h"
#include "third_party/blink/renderer/platform/fonts/font_selection_types.h"
#include "third_party/blink/renderer/platform/text/layout_locale.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkFontArguments.h"
#include "third_party/skia/include/core/SkFourByteTag.h"
#include "third_party/skia/include/core/SkString.h"
#include "third_party/skia/include/core/SkTypeface.h"
#include "third_party/skia/include/ports/SkTypeface_fontations.h"

namespace blink::molt_fingerprinting {

namespace {

using moltnet::BundledFontFace;
using moltnet::BundledFontSet;
using moltnet::BundledFontTable;
using moltnet::Generic;
using moltnet::HanRegion;

// The script codes tools/fonts/generate_tables.py writes into the table are
// ICU's.
static_assert(USCRIPT_COMMON == 0 && USCRIPT_INHERITED == 1 &&
              USCRIPT_ARABIC == 2 && USCRIPT_BOPOMOFO == 5 &&
              USCRIPT_CYRILLIC == 8 && USCRIPT_DEVANAGARI == 10 &&
              USCRIPT_HAN == 17 && USCRIPT_HANGUL == 18 &&
              USCRIPT_HEBREW == 19 && USCRIPT_HIRAGANA == 20 &&
              USCRIPT_KATAKANA == 22 && USCRIPT_LATIN == 25 &&
              USCRIPT_THAI == 38 && USCRIPT_KATAKANA_OR_HIRAGANA == 54 &&
              USCRIPT_SIMPLIFIED_HAN == 73 && USCRIPT_TRADITIONAL_HAN == 74 &&
              USCRIPT_JAPANESE == 105 && USCRIPT_KOREAN == 119 &&
              USCRIPT_ADLAM == 167);

// Typefaces made, per face and weight. Bounded: a page that asks for every
// weight of a variable family only makes the cache start over (fonts in use
// keep their typeface).
constexpr size_t kMaxTypefaces = 256;

// Threads (the main thread, workers) whose font caches were used before the
// files came, to be invalidated when they come. Bounded far above the
// threads a renderer runs.
using WaitingThreads = std::vector<scoped_refptr<base::SingleThreadTaskRunner>>;
constexpr size_t kMaxWaitingThreads = 1024;

base::Lock& TestingHookLock() {
  static base::NoDestructor<base::Lock> lock;
  return *lock;
}

base::RepeatingClosure& FontsInvalidatedForTesting() {
  static base::NoDestructor<base::RepeatingClosure> callback;
  return *callback;
}

// Runs on each thread that looked fonts up before the files came: its font
// cache (and through its clients, the font selectors of its documents or
// worker) looks again.
void InvalidateThisThreadsFonts() {
  if (FontGlobalContext::TryGet()) {
    FontCache::Get().Invalidate();
  }
  base::RepeatingClosure hook;
  {
    base::AutoLock lock(TestingHookLock());
    hook = FontsInvalidatedForTesting();
  }
  if (hook) {
    hook.Run();
  }
}

FontName ToFontName(const AtomicString& name) {
  if (name.empty()) {
    return FontName::Utf8(std::string_view());
  }
  return name.Is8Bit() ? FontName::Latin1(name.Span8())
                       : FontName::Utf16(name.Span16());
}

bool LooksLikeFont(base::span<const uint8_t> bytes) {
  if (bytes.size() < 12) {
    return false;
  }
  const uint32_t tag = (uint32_t{bytes[0]} << 24) | (uint32_t{bytes[1]} << 16) |
                       (uint32_t{bytes[2]} << 8) | uint32_t{bytes[3]};
  return tag == 0x00010000u || tag == 0x4f54544fu /* OTTO */ ||
         tag == 0x74727565u /* true */ || tag == 0x74746366u /* ttcf */;
}

int WeightOf(const FontDescription& description) {
  const float weight = static_cast<float>(description.Weight());
  if (!std::isfinite(weight)) {
    return 400;
  }
  return static_cast<int>(std::lround(weight));
}

class BundledFonts final : public FontPlatformData::UniformFontSet {
 public:
  BundledFonts() : table_(&moltnet::GetBundledFontTable()) {}
  BundledFonts(const BundledFonts&) = delete;
  BundledFonts& operator=(const BundledFonts&) = delete;
  ~BundledFonts() override = default;

  BundledFontSet rules() const {
    return BundledFontSet(*table_.load(std::memory_order_acquire));
  }
  void SetTableForTesting(const BundledFontTable& table) {
    table_.store(&table, std::memory_order_release);
  }

  // Accepts the files; `waiting` gets the threads that looked fonts up
  // before they came (their font caches hold what they found: nothing).
  bool SetFiles(std::vector<base::File> files, WaitingThreads* waiting) {
    const BundledFontTable& table = rules().table();
    if (ready_.load(std::memory_order_acquire)) {
      return false;
    }
    if (files.size() != table.files.size()) {
      LOG(ERROR) << "MoltNet fonts: " << files.size() << " files, expected "
                 << table.files.size();
      return false;
    }
    std::vector<std::unique_ptr<base::MemoryMappedFile>> mapped;
    std::vector<sk_sp<SkData>> data;
    for (size_t i = 0; i < files.size(); ++i) {
      auto file = std::make_unique<base::MemoryMappedFile>();
      if (!files[i].IsValid() || !file->Initialize(std::move(files[i])) ||
          file->length() != table.files[i].size ||
          !LooksLikeFont(file->bytes())) {
        LOG(ERROR) << "MoltNet fonts: refusing " << table.files[i].install_name;
        return false;
      }
      // The mapping lives as long as the process (this object is never
      // destroyed), so the typefaces may read it without a copy.
      data.push_back(SkData::MakeWithoutCopy(file->data(), file->length()));
      mapped.push_back(std::move(file));
    }
    // Under the lock, so that every lookup that found nothing has recorded
    // its thread before the list is taken.
    base::AutoLock lock(lock_);
    files_ = std::move(mapped);
    data_ = std::move(data);
    ready_.store(true, std::memory_order_release);
    waiting->swap(waiting_threads_);
    waiting_threads_.clear();
    return true;
  }

  void ResetForTesting() {
    base::AutoLock lock(lock_);
    typefaces_.clear();
    data_.clear();
    files_.clear();
    waiting_threads_.clear();
    ready_.store(false, std::memory_order_release);
  }

  // FontPlatformData::UniformFontSet:
  sk_sp<SkTypeface> MatchFamily(
      const AtomicString& family,
      const FontDescription& description) const override {
    const std::optional<uint8_t> index =
        rules().FamilyForName(ToFontName(family));
    if (!index) {
      return nullptr;
    }
    return TypefaceFor(*index, description);
  }

  sk_sp<SkTypeface> MatchUniqueName(const AtomicString& name) const override {
    const std::optional<uint8_t> face =
        rules().FaceForUniqueName(ToFontName(name));
    if (!face) {
      return nullptr;
    }
    return Typeface(*face, rules().table().faces[*face].default_weight);
  }

  AtomicString GenericFamily(
      const FontDescription& description,
      const AtomicString& generic_family_name) const override {
    Generic generic = Generic::kStandard;
    if (description.GenericFamily() == FontDescription::kStandardFamily ||
        description.GenericFamily() == FontDescription::kWebkitBodyFamily ||
        generic_family_name == font_family_names::kWebkitStandard) {
      generic = Generic::kStandard;
    } else if (generic_family_name == font_family_names::kSerif) {
      generic = Generic::kSerif;
    } else if (generic_family_name == font_family_names::kSansSerif) {
      generic = Generic::kSansSerif;
    } else if (generic_family_name == font_family_names::kCursive) {
      generic = Generic::kCursive;
    } else if (generic_family_name == font_family_names::kFantasy) {
      generic = Generic::kFantasy;
    } else if (generic_family_name == font_family_names::kMonospace) {
      generic = Generic::kMonospace;
    } else if (generic_family_name == font_family_names::kMath) {
      generic = Generic::kMath;
    } else {
      // system-ui (and anything else): looked up by name, which the set
      // also maps.
      return g_empty_atom;
    }
    return FamilyName(GenericIndex(generic, description));
  }

  AtomicString FallbackFamily(
      const FontDescription& description,
      UChar32 character,
      FontFallbackPriority fallback_priority) const override {
    UErrorCode error = U_ZERO_ERROR;
    int script = uscript_getScript(character, &error);
    if (U_FAILURE(error)) {
      script = USCRIPT_COMMON;
    }
    BundledFontSet::Presentation presentation =
        BundledFontSet::Presentation::kText;
    if (IsEmojiPresentationEmoji(fallback_priority)) {
      presentation = BundledFontSet::Presentation::kEmoji;
    } else if (IsTextPresentationEmoji(fallback_priority)) {
      presentation = BundledFontSet::Presentation::kTextEmoji;
    }
    std::array<uint8_t, moltnet::kMaxFallbackFamilies> order{};
    const size_t count = rules().FallbackFamilies(
        script, HanRegionFor(description), presentation, order);
    for (size_t i = 0; i < count; ++i) {
      const sk_sp<SkTypeface> typeface = TypefaceFor(order[i], description);
      if (typeface && typeface->unicharToGlyph(character)) {
        return FamilyName(order[i]);
      }
    }
    return AtomicString();
  }

  AtomicString LastResortFamily(
      const FontDescription& description) const override {
    Generic generic = Generic::kStandard;
    switch (description.GenericFamily()) {
      case FontDescription::kSerifFamily:
        generic = Generic::kSerif;
        break;
      case FontDescription::kSansSerifFamily:
        generic = Generic::kSansSerif;
        break;
      case FontDescription::kMonospaceFamily:
        generic = Generic::kMonospace;
        break;
      case FontDescription::kCursiveFamily:
        generic = Generic::kCursive;
        break;
      case FontDescription::kFantasyFamily:
        generic = Generic::kFantasy;
        break;
      default:
        break;
    }
    return FamilyName(GenericIndex(generic, description));
  }

 private:
  struct CachedTypeface {
    uint8_t face = 0;
    uint16_t weight = 0;
    sk_sp<SkTypeface> typeface;
  };

  AtomicString FamilyName(uint8_t family) const {
    return AtomicString::FromUtf8(rules().table().families[family].name);
  }

  uint8_t GenericIndex(Generic generic,
                       const FontDescription& description) const {
    const std::string locale = description.LocaleOrDefault().Ascii();
    return rules().FamilyForGeneric(generic, description.GetScript(), locale);
  }

  // The region of Han characters for this content: its locale's, as Blink
  // decides it for the system fallback (LayoutLocale::LocaleForHan).
  HanRegion HanRegionFor(const FontDescription& description) const {
    if (const LayoutLocale* han_locale =
            LayoutLocale::LocaleForHan(description.Locale())) {
      if (const char* locale = han_locale->LocaleForHanForSkFontMgr()) {
        return rules().RegionForLocale(locale);
      }
    }
    return rules().RegionForLocale(std::string_view());
  }

  sk_sp<SkTypeface> TypefaceFor(uint8_t family,
                                const FontDescription& description) const {
    const BundledFontSet::FaceMatch match =
        rules().MatchFace(family, WeightOf(description),
                          description.Style() != kNormalSlopeValue);
    return Typeface(match.face, match.weight);
  }

  sk_sp<SkTypeface> Typeface(uint8_t face_index, uint16_t weight) const {
    const BundledFontTable& table = rules().table();
    const BundledFontFace& face = table.faces[face_index];
    if (!face.variable_weight) {
      weight = face.default_weight;
    }
    base::AutoLock lock(lock_);
    if (!ready_.load(std::memory_order_acquire)) {
      // Nothing yet: this thread's font caches must look again once the
      // files come (a worker's as well as the main thread's).
      RememberThisThreadLocked();
      return nullptr;
    }
    for (const CachedTypeface& cached : typefaces_) {
      if (cached.face == face_index && cached.weight == weight) {
        return cached.typeface;
      }
    }
    SkFontArguments arguments;
    arguments.setCollectionIndex(face.collection_index);
    // Always the weight asked for: the file's default instance may be
    // another (Noto Sans CJK opens as Thin).
    const SkFontArguments::VariationPosition::Coordinate coordinate = {
        SkSetFourByteTag('w', 'g', 'h', 't'), static_cast<float>(weight)};
    if (face.variable_weight) {
      arguments.setVariationDesignPosition({&coordinate, 1});
    }
    sk_sp<SkTypeface> typeface =
        SkTypeface_Make_Fontations(data_[face.file], arguments);
    if (typeface) {
      // The file must hold the face the table describes.
      SkString family;
      typeface->getFamilyName(&family);
      if (!FontName::Utf8(std::string_view(family.c_str(), family.size()))
               .Equals(FontName::Utf8(table.families[face.family].name))) {
        LOG(ERROR) << "MoltNet fonts: " << table.files[face.file].install_name
                   << " face " << int{face.collection_index} << " is "
                   << family.c_str();
        typeface = nullptr;
      }
    }
    if (typefaces_.size() >= kMaxTypefaces) {
      typefaces_.clear();
    }
    typefaces_.push_back({face_index, weight, typeface});
    return typeface;
  }

  void RememberThisThreadLocked() const EXCLUSIVE_LOCKS_REQUIRED(lock_) {
    if (!base::SingleThreadTaskRunner::HasCurrentDefault()) {
      return;
    }
    for (const auto& runner : waiting_threads_) {
      if (runner->RunsTasksInCurrentSequence()) {
        return;
      }
    }
    if (waiting_threads_.size() < kMaxWaitingThreads) {
      waiting_threads_.push_back(
          base::SingleThreadTaskRunner::GetCurrentDefault());
    }
  }

  // The generated table (another one in tests only).
  std::atomic<const BundledFontTable*> table_;
  mutable base::Lock lock_;
  // Written once, with ready_. The table is one for the process, shared by
  // every renderer thread under lock_, and holds //base and Skia types.
  std::vector<std::unique_ptr<base::MemoryMappedFile>> files_ GUARDED_BY(lock_)
      ALLOW_DISCOURAGED_TYPE("process-wide, shared across threads");
  std::vector<sk_sp<SkData>> data_ GUARDED_BY(lock_)
      ALLOW_DISCOURAGED_TYPE("process-wide, shared across threads");
  mutable std::vector<CachedTypeface> typefaces_ GUARDED_BY(lock_)
      ALLOW_DISCOURAGED_TYPE("process-wide, shared across threads");
  // Threads that looked a font up before the files came.
  mutable WaitingThreads waiting_threads_ GUARDED_BY(lock_)
      ALLOW_DISCOURAGED_TYPE("process-wide, shared across threads");
  std::atomic<bool> ready_{false};
};

BundledFonts& Fonts() {
  static base::NoDestructor<BundledFonts> fonts;
  return *fonts;
}

std::atomic<bool> g_active{false};

}  // namespace

void ExpectBundledFonts() {
  if (g_active.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  FontPlatformData::SetUniformFontSet(&Fonts());
}

bool BundledFontsActive() {
  return g_active.load(std::memory_order_acquire);
}

bool SetBundledFontFiles(std::vector<base::File> files) {
  if (!BundledFontsActive()) {
    return false;
  }
  BundledFonts& fonts = Fonts();
  WaitingThreads waiting;
  if (!fonts.SetFiles(std::move(files), &waiting)) {
    return false;
  }
  // Lookups made before the files came found nothing (and text used the
  // empty last-resort font): every thread that made one looks again, this
  // one now, workers in their next task. A thread that has ended is skipped.
  for (const scoped_refptr<base::SingleThreadTaskRunner>& runner : waiting) {
    if (runner->RunsTasksInCurrentSequence()) {
      InvalidateThisThreadsFonts();
    } else {
      runner->PostTask(FROM_HERE, base::BindOnce(&InvalidateThisThreadsFonts));
    }
  }
  return true;
}

void SetFontsInvalidatedForTesting(base::RepeatingClosure callback) {
  base::AutoLock lock(TestingHookLock());
  FontsInvalidatedForTesting() = std::move(callback);
}

std::vector<BundledFontFaceInfo> BundledFontFaces() {
  const BundledFontTable& table = Fonts().rules().table();
  std::vector<BundledFontFaceInfo> faces;
  faces.reserve(table.faces.size());
  for (const BundledFontFace& face : table.faces) {
    faces.push_back({face.postscript_name, face.full_name,
                     table.families[face.family].name, face.style});
  }
  return faces;
}

void ResetBundledFontsForTesting() {
  FontPlatformData::SetUniformFontSet(nullptr);
  Fonts().ResetForTesting();
  Fonts().SetTableForTesting(moltnet::GetBundledFontTable());
  g_active.store(false, std::memory_order_release);
}

void UseBundledFontTableForTesting(const moltnet::BundledFontTable& table) {
  ResetBundledFontsForTesting();
  Fonts().SetTableForTesting(table);
}

}  // namespace blink::molt_fingerprinting
