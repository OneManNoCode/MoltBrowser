// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.

#include "third_party/blink/renderer/core/molt_fingerprinting/molt_fingerprinting.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/compiler_specific.h"
#include "base/no_destructor.h"
#include "base/numerics/checked_math.h"
#include "base/numerics/safe_conversions.h"
#include "crypto/random.h"
#include "gpu/command_buffer/client/gles2_interface.h"
#include "third_party/blink/public/common/storage_key/storage_key.h"
#include "third_party/blink/public/web/web_molt_fingerprinting.h"
#include "third_party/blink/renderer/bindings/core/v8/v8_binding_for_core.h"
#include "third_party/blink/renderer/core/execution_context/execution_context.h"
#include "third_party/blink/renderer/core/frame/local_dom_window.h"
#include "third_party/blink/renderer/core/frame/local_frame.h"
#include "third_party/blink/renderer/core/inspector/locale_controller.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/letterbox.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_locale.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_text.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/moltnet_uniformity.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/shared_worker_key.h"
#include "third_party/blink/renderer/core/molt_fingerprinting/shields_decision.h"
#include "third_party/blink/renderer/core/timezone/timezone_controller.h"
#include "third_party/blink/renderer/core/workers/worker_clients.h"
#include "third_party/blink/renderer/core/workers/worker_global_scope.h"
#include "third_party/blink/renderer/core/workers/worklet_global_scope.h"
#include "third_party/blink/renderer/platform/graphics/image.h"
#include "third_party/blink/renderer/platform/graphics/static_bitmap_image.h"
#include "third_party/blink/renderer/platform/graphics/unaccelerated_static_bitmap_image.h"
#include "third_party/blink/renderer/platform/heap/garbage_collected.h"
#include "third_party/blink/renderer/platform/network/blink_schemeful_site.h"
#include "third_party/blink/renderer/platform/storage/blink_storage_key.h"
#include "third_party/blink/renderer/platform/supplementable.h"
#include "third_party/blink/renderer/platform/weborigin/security_origin.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/vector.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"
#include "third_party/skia/include/core/SkData.h"
#include "third_party/skia/include/core/SkImage.h"
#include "third_party/skia/include/core/SkPixmap.h"
#include "ui/gfx/skia_span_util.h"
#include "v8/include/v8-isolate.h"

namespace blink {

// static
void WebMoltFingerprinting::Configure(Mode mode) {
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
  molt_fingerprinting::Configure(internal);
}

// static
void WebMoltFingerprinting::SetSessionSeed(
    base::span<const uint8_t> session_seed) {
  molt_fingerprinting::SetSessionSeed(session_seed);
}

namespace molt_fingerprinting {

namespace {

// g_mode is written once on the main thread before any worker exists
// (Configure); g_seed is written once, at any time (SetSessionSeed). Both
// are published with release/acquire (through g_mode and g_have_seed) and
// never change afterwards.
std::array<uint8_t, kSessionSeedSize> g_seed = {};
std::atomic<Mode> g_mode{Mode::kOff};
std::atomic<bool> g_configured{false};
std::atomic<bool> g_have_seed{false};

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
  // The key was needed before the session seed arrived: the random key is
  // kept for the context's whole life, so its values stay consistent.
  bool pinned_random = false;
  String site_and_partition;
  SiteKey key = {};
};

const char MoltFarblingState::kSupplementName[] = "MoltFarblingState";

// The key a worklet farbles with, decided by its document at creation. Made
// on the main thread, then only read on the worklet's thread (the usual
// WorkerClients pattern).
class MoltFarblingWorkletKey final
    : public GarbageCollected<MoltFarblingWorkletKey>,
      public Supplement<WorkerClients> {
 public:
  static const char kSupplementName[];

  MoltFarblingWorkletKey(WorkerClients& clients, const SiteKey& key)
      : Supplement<WorkerClients>(clients), key_(key) {}

  const SiteKey& key() const { return key_; }

  void Trace(Visitor* visitor) const override {
    Supplement<WorkerClients>::Trace(visitor);
  }

 private:
  const SiteKey key_;
};

const char MoltFarblingWorkletKey::kSupplementName[] = "MoltFarblingWorkletKey";

enum class SiteKind { kNone, kWeb, kOpaque, kHandedOver };

struct ResolvedSite {
  SiteKind kind = SiteKind::kNone;
  std::string site;
  std::string partition;
  SiteKey handed_over_key = {};
};

ResolvedSite FromSchemefulSite(const BlinkSchemefulSite& site,
                               std::string partition) {
  ResolvedSite resolved;
  if (site.IsOpaque()) {
    resolved.kind = SiteKind::kOpaque;
    return resolved;
  }
  const String serialized = site.Serialize();
  if (!serialized.starts_with("https://") &&
      !serialized.starts_with("http://")) {
    // chrome://, chrome-extension://, file://, ...: internal or local pages.
    return resolved;
  }
  resolved.kind = SiteKind::kWeb;
  resolved.site = serialized.Utf8();
  resolved.partition = std::move(partition);
  return resolved;
}

// The top-level site of a storage key, plus its nonce (fenced frames,
// credentialless iframes) as the partition.
ResolvedSite FromStorageKey(const BlinkStorageKey& storage_key) {
  std::string partition;
  if (const auto& nonce = storage_key.GetNonce()) {
    partition = nonce->ToString();
  }
  return FromSchemefulSite(storage_key.GetTopLevelSite(), std::move(partition));
}

// A service worker's site, from its registration's storage key, set before
// its script runs (SetServiceWorkerStorageKey). Worker thread only.
class MoltServiceWorkerSite final
    : public GarbageCollected<MoltServiceWorkerSite>,
      public Supplement<ExecutionContext> {
 public:
  static const char kSupplementName[];

  MoltServiceWorkerSite(ExecutionContext& context, ResolvedSite resolved)
      : Supplement<ExecutionContext>(context), resolved_(std::move(resolved)) {}

  const ResolvedSite& resolved() const { return resolved_; }

  void Trace(Visitor* visitor) const override {
    Supplement<ExecutionContext>::Trace(visitor);
  }

 private:
  const ResolvedSite resolved_;
};

const char MoltServiceWorkerSite::kSupplementName[] = "MoltServiceWorkerSite";

ResolvedSite ResolveSite(ExecutionContext& context) {
  if (auto* window = DynamicTo<LocalDOMWindow>(context)) {
    return FromStorageKey(window->GetStorageKey());
  }
  if (auto* worker = DynamicTo<WorkerGlobalScope>(context)) {
    // Service workers: their registration's storage key.
    if (const MoltServiceWorkerSite* service_worker_site =
            Supplement<ExecutionContext>::From<MoltServiceWorkerSite>(
                context)) {
      return service_worker_site->resolved();
    }
    // Shared workers: the storage key they were constructed with, handed
    // over by the browser when they started (shared_worker_key.h).
    if (const StorageKey* shared_worker_key =
            SharedWorkerStorageKey(worker->Clients())) {
      return FromStorageKey(BlinkStorageKey(*shared_worker_key));
    }
    // Set for dedicated workers (and workers nested in them) only.
    if (const SecurityOrigin* top = worker->top_level_frame_security_origin()) {
      return FromSchemefulSite(
          BlinkSchemefulSite(scoped_refptr<const SecurityOrigin>(top)),
          std::string());
    }
    // Workers without a handed-over storage key (not expected: shared and
    // service workers get theirs before any script runs) farble with a
    // random per-context key, as an opaque site does. (Their fetch
    // context's site for cookies is out of reach at 152:
    // web_worker_fetch_context() is protected.)
    ResolvedSite unknown;
    unknown.kind = SiteKind::kOpaque;
    return unknown;
  }
  if (auto* worklet = DynamicTo<WorkletGlobalScope>(context)) {
    WorkerClients* clients = worklet->Clients();
    const MoltFarblingWorkletKey* handed_over =
        clients
            ? Supplement<WorkerClients>::From<MoltFarblingWorkletKey>(*clients)
            : nullptr;
    ResolvedSite resolved;
    if (handed_over) {
      resolved.kind = SiteKind::kHandedOver;
      resolved.handed_over_key = handed_over->key();
    }
    return resolved;
  }
  return {};
}

std::optional<SiteKey> SiteKeyFor(ExecutionContext* context) {
  if (!context || GetMode() == Mode::kOff) {
    return std::nullopt;
  }
  const ResolvedSite resolved = ResolveSite(*context);
  switch (resolved.kind) {
    case SiteKind::kNone:
      return std::nullopt;
    case SiteKind::kHandedOver:
      return resolved.handed_over_key;
    case SiteKind::kWeb:
    case SiteKind::kOpaque:
      // The browser's Shields decision for this document or worker; none
      // means farbled. MoltNet ignores it (AreShieldsDown is false there):
      // the point is to look like every other MoltNet user.
      if (AreShieldsDown(*context)) {
        return std::nullopt;
      }
      break;
  }
  MoltFarblingState& state = MoltFarblingState::From(*context);
  if (state.pinned_random) {
    return state.key;
  }
  const bool have_seed = g_have_seed.load(std::memory_order_acquire);
  if (resolved.kind == SiteKind::kOpaque || !have_seed) {
    if (!state.has_key || !state.site_and_partition.IsNull()) {
      crypto::RandBytes(state.key);
      state.site_and_partition = String();
      state.has_key = true;
    }
    state.pinned_random = !have_seed;
    return state.key;
  }
  // The site of a context can change when Blink reuses the initial empty
  // document's window for the first navigation, so compare every time.
  const String id = String::FromUtf8(resolved.site + '\n' + resolved.partition);
  if (!state.has_key || state.site_and_partition != id) {
    state.key = DeriveSiteKey(g_seed, resolved.site, resolved.partition);
    state.site_and_partition = id;
    state.has_key = true;
  }
  return state.key;
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

// 8-bit premultiplied rows, farbled as stored.
void FarblePremulPixmapWith(const Farbler& farbler, const SkPixmap& pixmap) {
  if (!pixmap.addr() || pixmap.width() <= 0 || pixmap.height() <= 0) {
    return;
  }
  ChannelOrder order = ChannelOrder::kRGBA;
  switch (pixmap.colorType()) {
    case kRGBA_8888_SkColorType:
      break;
    case kBGRA_8888_SkColorType:
      order = ChannelOrder::kBGRA;
      break;
    default:
      return;
  }
  const base::span<uint8_t> all = gfx::SkPixmapToWritableSpan(pixmap);
  const size_t row_bytes = pixmap.rowBytes();
  const size_t row_size = static_cast<size_t>(pixmap.width()) * 4;
  for (int y = 0; y < pixmap.height(); ++y) {
    const size_t start = static_cast<size_t>(y) * row_bytes;
    if (start > all.size() || all.size() - start < row_size) {
      return;
    }
    farbler.FarbleRGBA8Premul(all.subspan(start, row_size), order);
  }
}

}  // namespace

scoped_refptr<StaticBitmapImage> FarbleForExportWith(const Farbler& farbler,
                                                     Image& image) {
  PaintImage paint_image = image.PaintImageForCurrentFrame();
  if (!paint_image || paint_image.IsPaintWorklet()) {
    return nullptr;
  }
  const SkImageInfo source_info = paint_image.GetSkImageInfo();
  if (source_info.isEmpty()) {
    return nullptr;
  }
  const SkColorType color_type = ReadbackColorType(source_info.colorType());
  SkAlphaType alpha_type = source_info.alphaType();
  if (alpha_type == kUnknown_SkAlphaType) {
    alpha_type = kPremul_SkAlphaType;
  }
  // 8-bit premultiplied data is farbled as stored; float data is farbled
  // unpremultiplied and premultiplied again below.
  const bool premul_8bit =
      alpha_type == kPremul_SkAlphaType && color_type == kN32_SkColorType;
  const SkAlphaType read_alpha_type =
      alpha_type == kPremul_SkAlphaType && !premul_8bit ? kUnpremul_SkAlphaType
                                                        : alpha_type;
  const SkImageInfo info =
      SkImageInfo::Make(source_info.width(), source_info.height(), color_type,
                        read_alpha_type, source_info.refColorSpace());
  const size_t row_bytes = info.minRowBytes();
  const size_t size = info.computeByteSize(row_bytes);
  if (SkImageInfo::ByteSizeOverflowed(size)) {
    return nullptr;
  }
  sk_sp<SkData> data = SkData::MakeUninitialized(size);
  if (!data ||
      !paint_image.readPixels(info, data->writable_data(), row_bytes, 0, 0)) {
    return nullptr;
  }
  const SkPixmap pixmap(info, data->writable_data(), row_bytes);
  if (premul_8bit) {
    FarblePremulPixmapWith(farbler, pixmap);
  } else {
    FarblePixmapWith(farbler, pixmap);
  }
  SkImageInfo out_info = info;
  if (read_alpha_type != alpha_type) {
    out_info = info.makeAlphaType(alpha_type);
    sk_sp<SkData> premultiplied = SkData::MakeUninitialized(size);
    if (!premultiplied ||
        !pixmap.readPixels(out_info, premultiplied->writable_data(),
                           row_bytes)) {
      return nullptr;
    }
    data = std::move(premultiplied);
  }
  sk_sp<SkImage> farbled =
      SkImages::RasterFromData(out_info, std::move(data), row_bytes);
  if (!farbled) {
    return nullptr;
  }
  return UnacceleratedStaticBitmapImage::Create(
      std::move(farbled), image.Orientation(), paint_image.GetHDRMetadata());
}

namespace {

// The noise and bytes per pixel for a WebGL readPixels format / type, or
// nullopt for formats that aren't farbled.
struct ReadLayout {
  enum class Kind { kUnorm8, kFloat16, kFloat32 };
  Kind kind;
  size_t channels;
  size_t bytes_per_pixel;
};

std::optional<ReadLayout> ReadLayoutFor(GLenum format, GLenum type) {
  size_t channels = 0;
  switch (format) {
    case GL_RED:
      channels = 1;
      break;
    case GL_RG:
      channels = 2;
      break;
    case GL_RGB:
      channels = 3;
      break;
    case GL_RGBA:
      channels = 4;
      break;
    default:
      return std::nullopt;
  }
  switch (type) {
    case GL_UNSIGNED_BYTE:
      return ReadLayout{ReadLayout::Kind::kUnorm8, channels, channels};
    case GL_HALF_FLOAT:
    case GL_HALF_FLOAT_OES:
      return ReadLayout{ReadLayout::Kind::kFloat16, channels, 2 * channels};
    case GL_FLOAT:
      return ReadLayout{ReadLayout::Kind::kFloat32, channels, 4 * channels};
    default:
      return std::nullopt;
  }
}

void FarbleReadRow(const Farbler& farbler,
                   const ReadLayout& layout,
                   base::span<uint8_t> row) {
  switch (layout.kind) {
    case ReadLayout::Kind::kUnorm8:
      farbler.FarbleUnorm8(row, layout.channels);
      break;
    case ReadLayout::Kind::kFloat16:
      farbler.FarbleFloat16(row);
      break;
    case ReadLayout::Kind::kFloat32:
      farbler.FarbleFloat32(row);
      break;
  }
}

// Where the rows of a packed readPixels result are, relative to the start of
// the destination (GL ES 3.0 section 4.3.2 with the pack parameters).
struct PackedRows {
  size_t first = 0;     // Offset of the first pixel of the first row.
  size_t stride = 0;    // Distance between rows.
  size_t row_size = 0;  // Bytes of pixels in a row.
};

std::optional<PackedRows> PackedRowsFor(
    const ReadLayout& layout,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack) {
  if (width <= 0 || height <= 0) {
    return std::nullopt;
  }
  const size_t row_pixels = pack.row_length > 0
                                ? static_cast<size_t>(pack.row_length)
                                : static_cast<size_t>(width);
  const size_t alignment =
      pack.alignment > 0 ? static_cast<size_t>(pack.alignment) : 1;
  // Rows are padded to `alignment`; for the power-of-two alignments and
  // component sizes WebGL allows this is exactly the spec's rule.
  base::CheckedNumeric<size_t> stride = row_pixels;
  stride *= layout.bytes_per_pixel;
  stride += alignment - 1;
  stride /= alignment;
  stride *= alignment;
  base::CheckedNumeric<size_t> first =
      static_cast<size_t>(pack.skip_rows > 0 ? pack.skip_rows : 0);
  first *= stride;
  first += base::CheckedNumeric<size_t>(static_cast<size_t>(
               pack.skip_pixels > 0 ? pack.skip_pixels : 0)) *
           layout.bytes_per_pixel;
  base::CheckedNumeric<size_t> row_size = static_cast<size_t>(width);
  row_size *= layout.bytes_per_pixel;
  PackedRows rows;
  if (!stride.AssignIfValid(&rows.stride) ||
      !first.AssignIfValid(&rows.first) ||
      !row_size.AssignIfValid(&rows.row_size) || rows.stride == 0) {
    return std::nullopt;
  }
  return rows;
}

// Largest PIXEL_PACK buffer range mapped at once (whole rows; one row can be
// larger).
constexpr size_t kMaxPackBufferMapBytes = 4 * 1024 * 1024;

// Reads `chunk.size()` bytes of the bound PIXEL_PACK buffer at `offset`.
// GetBufferSubDataCHROMIUM() leaves `chunk` untouched when the client gets
// no shared memory for the read (GL_OUT_OF_MEMORY) or the context is lost,
// which a buffer holding exactly the fill pattern looks like; so a chunk
// that reads back as all zeros is read again over 0xff, and the read failed
// only if neither read wrote it. False: write nothing back.
bool ReadPackBufferChunk(gpu::gles2::GLES2Interface* gl,
                         GLintptr offset,
                         GLsizeiptr size,
                         base::span<uint8_t> chunk) {
  std::ranges::fill(chunk, 0);
  gl->GetBufferSubDataCHROMIUM(GL_PIXEL_PACK_BUFFER, offset, size,
                               chunk.data());
  if (!std::ranges::all_of(chunk, [](uint8_t b) { return b == 0; })) {
    return true;
  }
  std::ranges::fill(chunk, 0xff);
  gl->GetBufferSubDataCHROMIUM(GL_PIXEL_PACK_BUFFER, offset, size,
                               chunk.data());
  return !std::ranges::all_of(chunk, [](uint8_t b) { return b == 0xff; });
}

}  // namespace

void Configure(Mode mode) {
  DCHECK(IsMainThread());
  if (g_configured.exchange(true)) {
    return;
  }
  if (mode == Mode::kOff) {
    return;
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
    // Form controls don't use ICU's default locale (moltnet_uniformity.h).
    PinUniformFormLocale();
    // Nor does text without a lang attribute (moltnet_locale.h).
    PinUniformTextLocale();
    // Text is measured the same way on every machine of an OS
    // (moltnet_text.h).
    PinUniformTextRendering();
  }
}

void SetSessionSeed(base::span<const uint8_t> session_seed) {
  DCHECK(IsMainThread());
  if (GetMode() == Mode::kOff || session_seed.size() != kSessionSeedSize ||
      g_have_seed.load(std::memory_order_acquire)) {
    return;
  }
  for (size_t i = 0; i < kSessionSeedSize; ++i) {
    g_seed[i] = session_seed[i];
  }
  g_have_seed.store(true, std::memory_order_release);
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
  g_have_seed.store(false, std::memory_order_release);
  g_seed = {};
}

std::optional<Farbler> FarblerFor(ExecutionContext* context) {
  if (const std::optional<SiteKey> key = SiteKeyFor(context)) {
    return Farbler(*key);
  }
  return std::nullopt;
}

bool IsFarbled(ExecutionContext* context) {
  return SiteKeyFor(context).has_value();
}

bool FarblesVideoExports(ExecutionContext* context) {
  return IsUniform() && IsFarbled(context);
}

void SetServiceWorkerStorageKey(ExecutionContext& worker,
                                const BlinkStorageKey& storage_key,
                                bool shields_down) {
  if (Supplement<ExecutionContext>::From<MoltServiceWorkerSite>(worker)) {
    return;  // Set once, before any script ran; never changes afterwards.
  }
  Supplement<ExecutionContext>::ProvideTo(
      worker, MakeGarbageCollected<MoltServiceWorkerSite>(
                  worker, FromStorageKey(storage_key)));
  SetWorkerShieldsDecision(worker, shields_down);
}

void ProvideToWorklet(LocalDOMWindow& window, WorkerClients* worker_clients) {
  if (!worker_clients) {
    return;
  }
  if (const std::optional<SiteKey> key = SiteKeyFor(&window)) {
    Supplement<WorkerClients>::ProvideTo(
        *worker_clients,
        MakeGarbageCollected<MoltFarblingWorkletKey>(*worker_clients, *key));
  }
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
  return UnacceleratedStaticBitmapImage::Create(
      std::move(farbled), image->Orientation(), paint_image.GetHDRMetadata());
}

scoped_refptr<StaticBitmapImage> FarbleForExport(
    ExecutionContext* context,
    scoped_refptr<StaticBitmapImage> image) {
  if (!image) {
    return image;
  }
  const std::optional<Farbler> farbler = FarblerFor(context);
  if (!farbler) {
    return image;
  }
  scoped_refptr<StaticBitmapImage> farbled =
      FarbleForExportWith(*farbler, *image);
  return farbled ? farbled : image;
}

scoped_refptr<Image> FarbleForExport(ExecutionContext* context,
                                     scoped_refptr<Image> image) {
  if (!image) {
    return image;
  }
  const std::optional<Farbler> farbler = FarblerFor(context);
  if (!farbler) {
    return image;
  }
  scoped_refptr<StaticBitmapImage> farbled =
      FarbleForExportWith(*farbler, *image);
  if (!farbled) {
    return image;
  }
  return farbled;
}

void FarbleReadPixels(ExecutionContext* context,
                      GLenum format,
                      GLenum type,
                      int width,
                      int height,
                      const WebGLImageConversion::PixelStoreParams& pack,
                      base::span<uint8_t> destination) {
  if (!ReadLayoutFor(format, type) || width <= 0 || height <= 0) {
    return;
  }
  if (const std::optional<Farbler> farbler = FarblerFor(context)) {
    FarbleReadPixelsWith(*farbler, format, type, width, height, pack,
                         destination);
  }
}

void FarbleReadPixelsWith(const Farbler& farbler,
                          GLenum format,
                          GLenum type,
                          int width,
                          int height,
                          const WebGLImageConversion::PixelStoreParams& pack,
                          base::span<uint8_t> destination) {
  const std::optional<ReadLayout> layout = ReadLayoutFor(format, type);
  if (!layout) {
    return;
  }
  const std::optional<PackedRows> rows =
      PackedRowsFor(*layout, width, height, pack);
  if (!rows) {
    return;
  }
  // WebGL has already checked that the destination is large enough; we
  // re-check every row anyway.
  size_t start = rows->first;
  for (int y = 0; y < height; ++y) {
    if (start > destination.size() ||
        destination.size() - start < rows->row_size) {
      return;
    }
    FarbleReadRow(farbler, *layout, destination.subspan(start, rows->row_size));
    if (!base::CheckAdd(start, rows->stride).AssignIfValid(&start)) {
      return;
    }
  }
}

void FarblePixelPackBuffer(ExecutionContext* context,
                           gpu::gles2::GLES2Interface* gl,
                           GLenum format,
                           GLenum type,
                           int width,
                           int height,
                           const WebGLImageConversion::PixelStoreParams& pack,
                           int64_t offset) {
  if (!gl || !ReadLayoutFor(format, type) || width <= 0 || height <= 0) {
    return;
  }
  if (const std::optional<Farbler> farbler = FarblerFor(context)) {
    FarblePixelPackBufferWith(*farbler, gl, format, type, width, height, pack,
                              offset);
  }
}

void FarblePixelPackBufferWith(
    const Farbler& farbler,
    gpu::gles2::GLES2Interface* gl,
    GLenum format,
    GLenum type,
    int width,
    int height,
    const WebGLImageConversion::PixelStoreParams& pack,
    int64_t offset) {
  if (!gl || offset < 0) {
    return;
  }
  const std::optional<ReadLayout> layout = ReadLayoutFor(format, type);
  if (!layout) {
    return;
  }
  const std::optional<PackedRows> rows =
      PackedRowsFor(*layout, width, height, pack);
  if (!rows) {
    return;
  }
  // Whole rows, a few MiB at a time, so one huge read never needs one huge
  // shared-memory mapping. The padding between rows is mapped too and
  // written back unchanged.
  const size_t rows_per_map =
      std::max<size_t>(1, kMaxPackBufferMapBytes / rows->stride);
  const size_t total_rows = static_cast<size_t>(height);
  for (size_t y0 = 0; y0 < total_rows; y0 += rows_per_map) {
    const size_t count = std::min(rows_per_map, total_rows - y0);
    base::CheckedNumeric<size_t> start = y0;
    start *= rows->stride;
    start += rows->first;
    base::CheckedNumeric<GLintptr> map_offset = offset;
    map_offset += start;
    base::CheckedNumeric<size_t> map_size = count - 1;
    map_size *= rows->stride;
    map_size += rows->row_size;
    GLintptr gl_offset = 0;
    size_t size = 0;
    GLsizeiptr gl_size = 0;
    if (!map_offset.AssignIfValid(&gl_offset) ||
        !map_size.AssignIfValid(&size) ||
        !base::CheckedNumeric<GLsizeiptr>(size).AssignIfValid(&gl_size)) {
      return;
    }
    // The GLES2 client has no MapBufferRange() at 152: read the rows back,
    // farble them and write them over the originals.
    Vector<uint8_t> chunk(base::checked_cast<wtf_size_t>(size));
    if (!ReadPackBufferChunk(gl, gl_offset, gl_size, base::span(chunk))) {
      return;
    }
    for (size_t r = 0; r < count; ++r) {
      FarbleReadRow(farbler, *layout,
                    base::span(chunk).subspan(r * rows->stride,
                                              rows->row_size));
    }
    gl->BufferSubData(GL_PIXEL_PACK_BUFFER, gl_offset, gl_size, chunk.data());
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

std::optional<Farbler> SiteFarblerFor(ExecutionContext* context,
                                      bool* is_site_key) {
  *is_site_key = false;
  const std::optional<SiteKey> key = SiteKeyFor(context);
  if (!key) {
    return std::nullopt;
  }
  // SiteKeyFor() keeps every key it made in the context's state; only a key
  // derived from a site has a non-null `site_and_partition`. Handed-over
  // worklet keys have no state.
  if (const MoltFarblingState* state =
          Supplement<ExecutionContext>::From<MoltFarblingState>(*context)) {
    *is_site_key = state->has_key && !state->pinned_random &&
                   !state->site_and_partition.IsNull();
  }
  return Farbler(*key);
}

std::optional<gfx::Rect> UniformScreenRect(LocalFrame* frame) {
  if (!IsUniform() || !frame) {
    return std::nullopt;
  }
  // Scrollbars included, rounded like innerWidth / innerHeight (letterbox.h).
  return gfx::Rect(UniformScreenSizeOf(*frame));
}

std::optional<gfx::PointF> UniformScreenPoint(
    const LocalDOMWindow* window,
    const gfx::PointF& position_in_widget) {
  if (!IsUniform()) {
    return std::nullopt;
  }
  // In the page's CSS pixels, like the uniform screen, so page zoom shows in
  // neither (letterbox.h).
  const float scale = window && window->GetFrame()
                          ? UniformScreenScale(*window->GetFrame())
                          : 1.0f;
  return gfx::ScalePoint(position_in_widget, 1.0f / scale);
}

}  // namespace molt_fingerprinting
}  // namespace blink
