/*
 * Copyright 2026 GenEye AI Labs Inc.
 * Licensed under GPLv3. See LICENSE file.
 */

// MoltBrowser: a raster pipeline that computes the same pixels on every CPU,
// x86-64 and Arm alike, for MoltNet's renderers
// (SkGraphics::MoltBrowserUsePortableRasterPipeline()).
//
// The same stages as SkOpts.cpp's, built at the build's baseline (SSE2 code
// on x86-64, NEON on Arm) with SKRP_MOLTBROWSER_PORTABLE, which makes the
// operations whose results differ between CPUs exact or alike:
//   * multiply-add is a separate multiply and add (Arm's default fuses them,
//     as x86's AVX2 and AVX-512 stages do);
//   * reciprocals and reciprocal square roots are divisions, not the
//     estimate instructions (rcpps, rsqrtps, frecpe), whose results differ
//     between CPU vendors;
//   * half floats are converted exactly (round to nearest even, denormals),
//     as F16C and NEON convert them, not truncated;
//   * floor and ceil are exact for large values and keep the sign of zero,
//     min and max follow x86's semantics, and lowp's divide-by-255 is exact.
// It also installs one blitter of its own: drawing 32-bit pixels with alpha
// over 32-bit pixels (SkOpts::blit_row_s32a_opaque), whose NEON version
// rounds where the x86 versions and the plain C one truncate.
// Normal windows never install these stages.

#include <cstdint>

#include "include/core/SkColor.h"
#include "src/core/SkBlitRow.h"
#include "src/core/SkColorPriv.h"
#include "src/core/SkOpts.h"
#include "src/core/SkVx.h"

namespace {

// SkBlitRow_opts.h's blit_row_s32a_opaque as x86-64 computes it (its SSE2
// and AVX2 loops, then SkPMSrcOver), on every CPU: each channel of the
// destination times 256 minus the source's alpha, shifted right by 8, added
// to the source's channel with saturation. NEON's loops round
// dst * (255 - alpha) / 255 instead, one more in some channels.
void moltbrowser_blit_row_s32a_opaque(SkPMColor* dst, const SkPMColor* src,
                                      int len, U8CPU alpha) {
    SkASSERT(alpha == 0xFF);
    using U32 = skvx::Vec<4, uint32_t>;
    using U16 = skvx::Vec<8, uint16_t>;
    using U8 = skvx::Vec<16, uint8_t>;
    while (len >= 4) {
        const U32 s = U32::Load(src);
        const U32 d = U32::Load(dst);
        const U32 scale = 256u - (s >> 24);
        // The scale in both 16-bit halves of each pixel.
        const U16 scale_x2 = sk_bit_cast<U16>((scale << 16) | scale);
        // Blue and red, then green and alpha (in the high byte), as SSE2's
        // 16-bit multiplies give them.
        const U16 rb = (sk_bit_cast<U16>(d & 0x00ff00ffu) * scale_x2) >> 8;
        const U16 ga = ((sk_bit_cast<U16>(d) >> 8) * scale_x2) & uint16_t{0xff00};
        skvx::saturated_add(sk_bit_cast<U8>(s), sk_bit_cast<U8>(rb | ga))
            .store(dst);
        src += 4;
        dst += 4;
        len -= 4;
    }
    while (len-- > 0) {
        *dst = SkPMSrcOver(*src, *dst);
        src++;
        dst++;
    }
}

void install_portable_blitters() {
    SkOpts::blit_row_s32a_opaque = moltbrowser_blit_row_s32a_opaque;
}

}  // namespace

#if !defined(SK_ENABLE_OPTIMIZE_SIZE)

// Never contract a*b+c into a fused multiply-add here, whatever the flags.
#if defined(__clang__)
    #pragma clang fp contract(off)
#endif

#define SK_OPTS_NS moltbrowser_portable
#define SKRP_MOLTBROWSER_PORTABLE
#include "src/opts/SkRasterPipeline_opts.h"

namespace SkOpts {
    void Init_MoltBrowserPortable() {
        install_portable_blitters();

        raster_pipeline_lowp_stride  = SK_OPTS_NS::raster_pipeline_lowp_stride();
        raster_pipeline_highp_stride = SK_OPTS_NS::raster_pipeline_highp_stride();

    #define M(st) ops_highp[(int)SkRasterPipelineOp::st] = (StageFn)SK_OPTS_NS::st;
        SK_RASTER_PIPELINE_OPS_ALL(M)
        just_return_highp = (StageFn)SK_OPTS_NS::just_return;
        start_pipeline_highp = SK_OPTS_NS::start_pipeline;
    #undef M

    #define M(st) ops_lowp[(int)SkRasterPipelineOp::st] = (StageFn)SK_OPTS_NS::lowp::st;
        SK_RASTER_PIPELINE_OPS_LOWP(M)
        just_return_lowp = (StageFn)SK_OPTS_NS::lowp::just_return;
        start_pipeline_lowp = SK_OPTS_NS::lowp::start_pipeline;
    #undef M
    }
}  // namespace SkOpts

#else

namespace SkOpts {
    void Init_MoltBrowserPortable() {
        install_portable_blitters();
    }
}  // namespace SkOpts

#endif  // SK_ENABLE_OPTIMIZE_SIZE
