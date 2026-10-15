// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/no_destructor.h"
#include "base/numerics/safe_conversions.h"
#include "crypto/random.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/inspector/locale_controller.h"
#include "third_party/blink/renderer/core/page/chrome_client.h"
#include "third_party/blink/renderer/core/timezone/timezone_controller.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/network/blink_schemeful_site.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/display/screen_info.h"
#include "ui/gfx/skia_span_util.h"
#include "v8/include/v8-isolate.h"

namespace blink {

// static
void WebMoltFingerprinting::Configure(Mode mode,
                                      base::span<const uint8_t> session_seed) {
  molt_fingerprinting::Mode internal = molt_fingerprinting::Mode::kOff;
  switch (mode) {
    case Mode::kOff:
      internal = molt_fingerprinting::Mode::kOff;
      break;
    case Mode::kFarble:
      internal = molt_fingerprinting::Mode::kFarble;
      break;
    case Mode::kUniform:
      internal = molt_fingerprinting::Mode::kUniform;
      break;
  }
  molt_fingerprinting::Configure(internal, session_seed);
}

namespace molt_fingerprinting {

namespace {

// Written once on the main thread (Configure) before any worker exists and
// published with release/acquire through g_mode.
std::array<uint8_t, kSessionSeedSize> g_seed = {};
std::atomic<Mode> g_mode{Mode::kOff};
std::atomic<bool> g_configured{false};

// Per-execution-context cache of the derived key.
class MoltFarblingState final : public GarbageCollected<MoltFarblingState>,
                                public Supplement<ExecutionContext> {
 public:
  static const char kSupplementName[];

  static MoltFarblingState& From(ExecutionContext& context) {
    MoltFarblingState* state =
        Supplement<ExecutionContext>::From<MoltFarblingState>(context);
    if (!state) {
      state = MakeGarbageCollected<MoltFarblingState>(context);
      Supplement<ExecutionContext>::ProvideTo(context, state);
    }
    return *state;
  }

  explicit MoltFarblingState(ExecutionContext& context)
      : Supplement<ExecutionContext>(context) {}

  void Trace(Visitor* visitor) const override {
    Supplement<ExecutionContext>::Trace(visitor);
  }

  // `site_and_partition` identifies what `key` was derived for; a null
  // string means `key` is a random per-context key.
  bool has_key = false;
  String site_and_partition;
  SiteKey key = {};
};

const char MoltFarblingState::kSupplementName[] = "MoltFarblingState";

enum class SiteKind { kNone, kWeb, kOpaque };

struct ResolvedSite {
  SiteKind kind = SiteKind::kNone;
  std::string site;
  std::string partition;
};

ResolvedSite FromSchemefulSite(const BlinkSchemefulSite& site,
                               std::string partition) {
  if (site.IsOpaque()) {
    return {SiteKind::kOpaque, std::string(), std::string()};
  }
  const String serialized = site.Serialize();
  if (!serialized.StartsWith("https://") && !serialized.StartsWith("http://")) {
    // chrome://, chrome-extension://, file://, ...: internal or local pages.
    return {};
  }
  return {SiteKind::kWeb, serialized.Utf8(), std::move(partition)};
}

ResolvedSite ResolveSite(ExecutionContext& context) {
  if (auto* window = DynamicTo<LocalDOMWindow>(context)) {
    const BlinkStorageKey& storage_key = window->GetStorageKey();
    std::string partition;
    if (const auto& nonce = storage_key.GetNonce()) {
      partition = nonce->ToString();
    }
    return FromSchemefulSite(storage_key.GetTopLevelSite(),
                             std::move(partition));
  }
  if (auto* worker = DynamicTo<WorkerGlobalScope>(context)) {
    // Set for dedicated workers (and workers nested in them) only.
    if (const SecurityOrigin* top = worker->top_level_frame_security_origin()) {
      return FromSchemefulSite(
          BlinkSchemefulSite(scoped_refptr<const SecurityOrigin>(top)),
          std::string());
    }
    // Shared and service workers: no top-level site known in Blink.
    return {SiteKind::kOpaque, std::string(), std::string()};
  }
  // Worklets etc. expose none of the farbled APIs.
  return {};
}

SkColorType ReadbackColorType(SkColorType source) {
  switch (source) {
    case kRGBA_F16_SkColorType:
    case kRGBA_F16Norm_SkColorType:
      return kRGBA_F16_SkColorType;
    case kRGBA_F32_SkColorType:
      return kRGBA_F32_SkColorType;
    default:
      // 8-bit (and the rare 10-bit) canvases are read back as native 8-bit.
      return kN32_SkColorType;
  }
}

}  // namespace

void Configure(Mode mode, base::span<const uint8_t> session_seed) {
  DCHECK(IsMainThread());
  if (g_configured.exchange(true)) {
    return;
  }
  if (mode == Mode::kOff || session_seed.size() != kSessionSeedSize) {
    return;
  }
  for (size_t i = 0; i < kSessionSeedSize; ++i) {
    g_seed[i] = session_seed[i];
  }
  g_mode.store(mode, std::memory_order_release);

  if (mode == Mode::kUniform) {
    // Pinned for the life of the process. The handle is never released, so
    // DevTools emulation can't replace it either.
    TimeZoneController::TimeZoneOverrideResult result =
        TimeZoneController::SetTimeZoneOverride(String(kUniformTimeZone));
    if (result.handle) {
      static base::NoDestructor<
          std::unique_ptr<TimeZoneController::TimeZoneOverride>>
          holder;
      *holder = std::move(result.handle);
    }
    const String error = LocaleController::instance().SetLocaleOverride(
        String(kUniformLocale), /*is_claiming_override=*/true);
    DCHECK(error.empty()) << error;
  }
}

Mode GetMode() {
  return g_mode.load(std::memory_order_acquire);
}

bool IsUniform() {
  return GetMode() == Mode::kUniform;
}

void ResetForTesting() {
  g_mode.store(Mode::kOff, std::memory_order_release);
  g_configured.store(false);
}

std::optional<Farbler> FarblerFor(ExecutionContext* context) {
  if (!context || GetMode() == Mode::kOff) {
    return std::nullopt;
  }
  const ResolvedSite resolved = ResolveSite(*context);
  if (resolved.kind == SiteKind::kNone) {
    return std::nullopt;
  }
  MoltFarblingState& state = MoltFarblingState::From(*context);
  if (resolved.kind == SiteKind::kOpaque) {
    if (!state.has_key || !state.site_and_partition.IsNull()) {
      crypto::RandBytes(state.key);
      state.site_and_partition = String();
      state.has_key = true;
    }
    return Farbler(state.key);
  }
  // The site of a context can change when Blink reuses the initial empty
  // document's window for the first navigation, so compare every time.
  const String id = String::FromUTF8(resolved.site + '\n' + resolved.partition);
  if (!state.has_key || state.site_and_partition != id) {
    state.key = DeriveSiteKey(g_seed, resolved.site, resolved.partition);
    state.site_and_partition = id;
    state.has_key = true;
  }
  return Farbler(state.key);
}

void FarblePixmapWith(const Farbler& farbler, const SkPixmap& pixmap) {
  if (!pixmap.addr() || pixmap.width() <= 0 || pixmap.height() <= 0 ||
      pixmap.alphaType() == kPremul_SkAlphaType) {
    // Callers pass unpremultiplied (or opaque) data; moving a premultiplied
    // channel could push it above alpha.
    return;
  }
  size_t bytes_per_pixel = 0;
  switch (pixmap.colorType()) {
    case kRGBA_8888_SkColorType:
    case kBGRA_8888_SkColorType:
      bytes_per_pixel = 4;
      break;
    case kRGBA_F16_SkColorType:
      bytes_per_pixel = 8;
      break;
    case kRGBA_F32_SkColorType:
      bytes_per_pixel = 16;
      break;
    default:
      return;
  }
  const base::span<uint8_t> all = gfx::SkPixmapToWritableSpan(pixmap);
  const size_t row_bytes = pixmap.rowBytes();
  const size_t row_size = static_cast<size_t>(pixmap.width()) * bytes_per_pixel;
  for (int y = 0; y < pixmap.height(); ++y) {
    const size_t start = static_cast<size_t>(y) * row_bytes;
    if (start > all.size() || all.size() - start < row_size) {
      return;
    }
    const base::span<uint8_t> row = all.subspan(start, row_size);
    switch (pixmap.colorType()) {
      case kRGBA_8888_SkColorType:
        farbler.FarbleRGBA8(row, ChannelOrder::kRGBA);
        break;
      case kBGRA_8888_SkColorType:
        farbler.FarbleRGBA8(row, ChannelOrder::kBGRA);
        break;
      case kRGBA_F16_SkColorType:
        farbler.FarbleRGBAF16(row);
        break;
      default:
        farbler.FarbleRGBAF32(row);
        break;
    }
  }
}

void FarblePixmap(ExecutionContext* context, const SkPixmap& pixmap) {
  if (const std::optional<Farbler> farbler = FarblerFor(context)) {
    FarblePixmapWith(*farbler, pixmap);
  }
}

scoped_refptr<StaticBitmapImage> FarbleSnapshot(
    ExecutionContext* context,
    scoped_refptr<StaticBitmapImage> image) {
  if (!image) {
    return image;
  }
  const std::optional<Farbler> farbler = FarblerFor(context);
  if (!farbler) {
    return image;
  }
  PaintImage paint_image = image->PaintImageForCurrentFrame();
  if (!paint_image || paint_image.IsPaintWorklet()) {
    return image;
  }
  const SkImageInfo source_info = paint_image.GetSkImageInfo();
  if (source_info.isEmpty()) {
    return image;
  }
  // Read back unpremultiplied, the way ImageDataBuffer feeds the encoders.
  const SkImageInfo info =
      SkImageInfo::Make(source_info.width(), source_info.height(),
                        ReadbackColorType(source_info.colorType()),
                        kUnpremul_SkAlphaType, source_info.refColorSpace());
  const size_t row_bytes = info.minRowBytes();
  const size_t size = info.computeByteSize(row_bytes);
  if (SkImageInfo::ByteSizeOverflowed(size)) {
    return image;
  }
  sk_sp<SkData> data = SkData::MakeUninitialized(size);
  if (!data ||
      !paint_image.readPixels(info, data->writable_data(), row_bytes, 0, 0)) {
    return image;
  }
  FarblePixmapWith(*farbler, SkPixmap(info, data->writable_data(), row_bytes));
  sk_sp<SkImage> farbled =
      SkImages::RasterFromData(info, std::move(data), row_bytes);
  if (!farbled) {
    return image;
  }
  return UnacceleratedStaticBitmapImage::Create(std::move(farbled),
                                                image->Orientation());
}

void FarbleReadPixels(ExecutionContext* context,
                      bool is_rgba8,
                      int width,
                      int height,
                      const WebGLImageConversion::PixelStoreParams& pack,
                      base::span<uint8_t> destination) {
  if (!is_rgba8 || width <= 0 || height <= 0) {
    return;
  }
  const std::optional<Farbler> farbler = FarblerFor(context);
  if (!farbler) {
    return;
  }
  // Row layout per the pack parameters (WebGL has already checked that the
  // destination is large enough; we re-check every row anyway).
  const size_t row_size = static_cast<size_t>(width) * 4;
  const size_t row_pixels = pack.row_length > 0
                                ? static_cast<size_t>(pack.row_length)
                                : static_cast<size_t>(width);
  const size_t alignment =
      pack.alignment > 0 ? static_cast<size_t>(pack.alignment) : 1;
  const size_t unpadded = row_pixels * 4;
  const size_t stride = (unpadded + alignment - 1) / alignment * alignment;
  const size_t first =
      static_cast<size_t>(pack.skip_rows > 0 ? pack.skip_rows : 0) * stride +
      static_cast<size_t>(pack.skip_pixels > 0 ? pack.skip_pixels : 0) * 4;
  for (int y = 0; y < height; ++y) {
    const size_t start = first + static_cast<size_t>(y) * stride;
    if (start > destination.size() || destination.size() - start < row_size) {
      return;
    }
    farbler->FarbleRGBA8(destination.subspan(start, row_size),
                         ChannelOrder::kRGBA);
  }
}

void FarbleAudio(ExecutionContext* context, base::span<float> samples) {
  if (samples.empty()) {
    return;
  }
  if (const std::optional<Farbler> farbler = FarblerFor(context)) {
    farbler->FarbleAudio(samples);
  }
}

void FarbleAnalyserBytes(ExecutionContext* context,
                         base::span<uint8_t> bytes,
                         uint8_t neutral) {
  if (bytes.empty()) {
    return;
  }
  if (const std::optional<Farbler> farbler = FarblerFor(context)) {
    farbler->FarbleAudioBytes(bytes, neutral);
  }
}

ExecutionContext* ContextOfCurrentCall(v8::Isolate* isolate) {
  if (!isolate || !isolate->InContext()) {
    return nullptr;
  }
  return CurrentExecutionContext(isolate);
}

unsigned HardwareConcurrency(ExecutionContext* context, unsigned real) {
  if (IsUniform()) {
    return kUniformHardwareConcurrency;
  }
  if (const std::optional<Farbler> farbler = FarblerFor(context)) {
    return farbler->HardwareConcurrency(real);
  }
  return real;
}

std::optional<gfx::Rect> UniformScreenRect(LocalFrame* frame) {
  if (!IsUniform() || !frame) {
    return std::nullopt;
  }
  gfx::Size viewport;
  const LocalFrame& root = frame->LocalFrameRoot();
  // GetOutermostMainFrameSize() reads the main frame's view, which doesn't
  // exist yet very early in a main frame's life.
  if (!root.IsOutermostMainFrame() || root.View()) {
    viewport = frame->GetOutermostMainFrameSize();
  }
  const float scale =
      frame->GetChromeClient().GetScreenInfo(*frame).device_scale_factor;
  const float dsf = scale > 0 ? scale : 1.0f;
  const LetterboxSize size =
      LetterboxBucket(base::ClampFloor(viewport.width() / dsf),
                      base::ClampFloor(viewport.height() / dsf));
  return gfx::Rect(0, 0, size.width, size.height);
}

std::optional<gfx::PointF> UniformScreenPoint(
    const LocalDOMWindow* window,
    const gfx::PointF& position_in_widget) {
  if (!IsUniform()) {
    return std::nullopt;
  }
  float dsf = 1.0f;
  if (window && window->GetFrame()) {
    LocalFrame& frame = *window->GetFrame();
    const float scale =
        frame.GetChromeClient().GetScreenInfo(frame).device_scale_factor;
    if (scale > 0) {
      dsf = scale;
    }
  }
  return gfx::ScalePoint(position_in_widget, 1.0f / dsf);
}

}  // namespace molt_fingerprinting
}  // namespace blink
