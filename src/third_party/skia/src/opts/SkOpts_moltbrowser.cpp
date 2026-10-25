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
// Normal windows never install these stages.

#include "src/core/SkOpts.h"

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
    void Init_MoltBrowserPortable() {}
}  // namespace SkOpts

#endif  // SK_ENABLE_OPTIMIZE_SIZE
