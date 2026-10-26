// Copyright 2026 GenEye AI Labs Inc.
// Licensed under GPLv3. See LICENSE file.
//
// MOLT_RAW_PTR_EXCLUSION: base/memory/raw_ptr_exclusion.h's RAW_PTR_EXCLUSION
// for the std-only inference code (the engine, the grammar sampler, the
// translation table, the shader-pack test; they also build without //base,
// see CLAUDE.md). It marks a pointer field that the raw_ptr plugin's field
// checks (a non-official build, such as Windows' configuration, runs them)
// leave alone: these point to llama.cpp's C objects, which raw_ptr can't
// own, or to static tables. Unlike //base's, it doesn't depend on the build
// being official (it can't read //base's buildflags): the annotation is on
// a handful of fields.

#ifndef CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_RAW_PTR_EXCLUSION_H_
#define CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_RAW_PTR_EXCLUSION_H_

#if defined(__clang__)
#define MOLT_RAW_PTR_EXCLUSION __attribute__((annotate("raw_ptr_exclusion")))
#else
#define MOLT_RAW_PTR_EXCLUSION
#endif

#endif  // CHROME_SERVICES_MOLT_AI_INFERENCE_ENGINE_RAW_PTR_EXCLUSION_H_
