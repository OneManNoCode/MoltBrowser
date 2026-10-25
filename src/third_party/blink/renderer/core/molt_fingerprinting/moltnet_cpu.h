// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MoltBrowser fingerprinting protection: what a MoltNet page computes doesn't
// show the machine's CPU features or memory. These settings have to be made
// before Blink initialises V8 and before the first image is decoded, so the
// embedder calls WebMoltFingerprinting::ConfigureBeforeBlinkInitialization()
// (ContentRendererClient::SetRuntimeFeaturesDefaultsBeforeBlinkInitialization),
// for the life of the process:
//
//   WebAssembly relaxed SIMD   off (V8's --no-wasm-relaxed-simd). Its results
//                                depend on the CPU: fused or separate
//                                multiply-add (FMA3), x86 or Arm lane
//                                semantics (swizzle, truncation, lane select,
//                                min/max, Q15 multiply), dot products with or
//                                without VNNI. A module that uses it fails
//                                validation, the same way on every machine;
//                                feature detection (WebAssembly.validate)
//                                reports it unsupported, and pages fall back
//                                to the plain SIMD build they ship for
//                                browsers without it.
//   V8's heap                  every isolate (windows, workers, worklets) is
//                                configured for one machine,
//                                moltnet::kV8HeapPhysicalMemory
//                                (gin::IsolateHolder), whose limit
//                                performance.memory reports (moltnet_memory.h)
//   skcms colour transforms    (images with colour profiles, WebCodecs
//                                ImageDecoder) the baseline code on every
//                                x86-64 CPU, not the AVX2 or AVX-512
//                                variants, converting half floats exactly as
//                                those and NEON do, so x86-64 and Arm give the
//                                same bytes (skcms_MoltBrowserEnablePortableMode)
//   Skia's raster pipeline     (canvas, image and video drawing, colour
//                                conversion of read-back pixels) stages that
//                                compute the same pixels on every CPU, x86-64
//                                and Arm alike: no fused multiply-add, no
//                                reciprocal estimates, exact half floats
//                                (SkGraphics::
//                                MoltBrowserUsePortableRasterPipeline)
//
// Done elsewhere for the same processes: Skia's other CPU-specific code
// (blitters, bitmap sampling, swizzles) is never installed, nor its raster
// pipeline's (the browser adds --disable-skia-runtime-opts to MoltNet's
// renderers, chrome/browser/molt_ai/fingerprinting/
// molt_fingerprinting_browser.cc), media decodes and encodes with portable
// kernels (moltnet_pixels.h), and on Linux the process's data limit is the
// same on every machine (sandbox/policy/linux/molt_data_limit.h).
//
// Normal and Incognito windows are unchanged; the kill switch turns it off
// (no mode switch, no call).

#ifndef THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CPU_H_
#define THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CPU_H_

#include <string_view>

#include "third_party/blink/renderer/core/core_export.h"

namespace blink::molt_fingerprinting {

// The V8 flags of a MoltNet renderer.
inline constexpr std::string_view kMoltNetV8Flags = "--no-wasm-relaxed-simd";

// For a MoltNet renderer, before V8 is initialised and before any isolate,
// image decoder or colour transform exists (no Blink object or thread is
// needed). Once per process.
CORE_EXPORT void PinUniformMachineBeforeV8();

}  // namespace blink::molt_fingerprinting

#endif  // THIRD_PARTY_BLINK_RENDERER_CORE_MOLT_FINGERPRINTING_MOLTNET_CPU_H_
